// SPDX-License-Identifier: GPL-2.0
/*
 * xHCI host controller driver
 *
 * Copyright (C) 2008 Intel Corp.
 *
 * Author: Sarah Sharp
 * Some code borrowed from the Linux EHCI driver.
 */

/*
 * Ring initialization rules:
 * 1. Each segment is initialized to zero, except for link TRBs.
 * 2. Ring cycle state = 0.  This represents Producer Cycle State (PCS) or
 *    Consumer Cycle State (CCS), depending on ring function.
 * 3. Enqueue pointer = dequeue pointer = address of first TRB in the segment.
 *
 * Ring behavior rules:
 * 1. A ring is empty if enqueue == dequeue.  This means there will always be at
 *    least one free TRB in the ring.  This is useful if you want to turn that
 *    into a link TRB and expand the ring.
 * 2. When incrementing an enqueue or dequeue pointer, if the next TRB is a
 *    link TRB, then load the pointer with the address in the link TRB.  If the
 *    link TRB had its toggle bit set, you may need to update the ring cycle
 *    state (see cycle bit rules).  You may have to do this multiple times
 *    until you reach a non-link TRB.
 * 3. A ring is full if enqueue++ (for the definition of increment above)
 *    equals the dequeue pointer.
 *
 * Cycle bit rules:
 * 1. When a consumer increments a dequeue pointer and encounters a toggle bit
 *    in a link TRB, it must toggle the ring cycle state.
 * 2. When a producer increments an enqueue pointer and encounters a toggle bit
 *    in a link TRB, it must toggle the ring cycle state.
 *
 * Producer rules:
 * 1. Check if ring is full before you enqueue.
 * 2. Write the ring cycle state to the cycle bit in the TRB you're enqueuing.
 *    Update enqueue pointer between each write (which may update the ring
 *    cycle state).
 * 3. Notify consumer.  If SW is producer, it rings the doorbell for command
 *    and endpoint rings.  If HC is the producer for the event ring,
 *    and it generates an interrupt according to interrupt modulation rules.
 *
 * Consumer rules:
 * 1. Check if TRB belongs to you.  If the cycle bit == your ring cycle state,
 *    the TRB is owned by the consumer.
 * 2. Update dequeue pointer (which may update the ring cycle state) and
 *    continue processing TRBs until you reach a TRB which is not owned by you.
 * 3. Notify the producer.  SW is the consumer for the event ring, and it
 *   updates event ring dequeue pointer.  HC is the consumer for the command and
 *   endpoint rings; it generates events on the event ring for these.
 */

#include <linux/jiffies.h>
#include <linux/scatterlist.h>
#include <linux/slab.h>
#include <linux/string_choices.h>
#include <linux/dma-mapping.h>
#include "xhci.h"
#include "xhci-trace.h"

static int queue_command(struct xhci_hcd *xhci, struct xhci_command *cmd,
			 u32 field1, u32 field2,
			 u32 field3, u32 field4, bool command_must_succeed);

/*
 * Returns zero if the TRB isn't in this segment, otherwise it returns the DMA
 * address of the TRB.
 */
dma_addr_t xhci_trb_virt_to_dma(struct xhci_segment *seg,
		union xhci_trb *trb)
{
	unsigned long segment_offset;

	if (!seg || !trb || trb < seg->trbs)
		return 0;
	/* offset in TRBs */
	segment_offset = trb - seg->trbs;
	if (segment_offset >= TRBS_PER_SEGMENT)
		return 0;
	return seg->dma + (segment_offset * sizeof(*trb));
}

static bool trb_is_noop(union xhci_trb *trb)
{
	return TRB_TYPE_NOOP_LE32(trb->generic.field[3]);
}

static bool trb_is_link(union xhci_trb *trb)
{
	return TRB_TYPE_LINK_LE32(trb->link.control);
}

static bool last_trb_on_seg(struct xhci_segment *seg, union xhci_trb *trb)
{
	return trb == &seg->trbs[TRBS_PER_SEGMENT - 1];
}

static bool last_trb_on_ring(struct xhci_ring *ring,
			struct xhci_segment *seg, union xhci_trb *trb)
{
	return last_trb_on_seg(seg, trb) && (seg->next == ring->first_seg);
}

static bool link_trb_toggles_cycle(union xhci_trb *trb)
{
	return le32_to_cpu(trb->link.control) & LINK_TOGGLE;
}

static bool last_td_in_urb(struct xhci_td *td)
{
	struct urb_priv *urb_priv = td->urb->hcpriv;

	return urb_priv->num_tds_done == urb_priv->num_tds;
}

static bool unhandled_event_trb(struct xhci_ring *ring)
{
	return ((le32_to_cpu(ring->dequeue->event_cmd.flags) & TRB_CYCLE) ==
		ring->cycle_state);
}

static void inc_td_cnt(struct urb *urb)
{
	struct urb_priv *urb_priv = urb->hcpriv;

	urb_priv->num_tds_done++;
}

static void trb_to_noop(union xhci_trb *trb, u32 noop_type)
{
	if (trb_is_link(trb)) {
		/* unchain chained link TRBs */
		trb->link.control &= cpu_to_le32(~TRB_CHAIN);
	} else {
		trb->generic.field[0] = 0;
		trb->generic.field[1] = 0;
		trb->generic.field[2] = 0;
		/* Preserve only the cycle bit of this TRB */
		trb->generic.field[3] &= cpu_to_le32(TRB_CYCLE);
		trb->generic.field[3] |= cpu_to_le32(TRB_TYPE(noop_type));
	}
}

/* Updates trb to point to the next TRB in the ring, and updates seg if the next
 * TRB is in a new segment.  This does not skip over link TRBs, and it does not
 * effect the ring dequeue or enqueue pointers.
 */
static void next_trb(struct xhci_segment **seg,
			union xhci_trb **trb)
{
	if (trb_is_link(*trb) || last_trb_on_seg(*seg, *trb)) {
		*seg = (*seg)->next;
		*trb = ((*seg)->trbs);
	} else {
		(*trb)++;
	}
}

/*
 * See Cycle bit rules. SW is the consumer for the event ring only.
 */
void inc_deq(struct xhci_hcd *xhci, struct xhci_ring *ring)
{
	unsigned int link_trb_count = 0;

	/* event ring doesn't have link trbs, check for last trb */
	if (ring->type == TYPE_EVENT) {
		if (!last_trb_on_seg(ring->deq_seg, ring->dequeue)) {
			ring->dequeue++;
			return;
		}
		if (last_trb_on_ring(ring, ring->deq_seg, ring->dequeue))
			ring->cycle_state ^= 1;
		ring->deq_seg = ring->deq_seg->next;
		ring->dequeue = ring->deq_seg->trbs;

		trace_xhci_inc_deq(ring);

		return;
	}

	/* All other rings have link trbs */
	if (!trb_is_link(ring->dequeue)) {
		if (last_trb_on_seg(ring->deq_seg, ring->dequeue))
			xhci_warn(xhci, "Missing link TRB at end of segment\n");
		else
			ring->dequeue++;
	}

	while (trb_is_link(ring->dequeue)) {
		ring->deq_seg = ring->deq_seg->next;
		ring->dequeue = ring->deq_seg->trbs;

		trace_xhci_inc_deq(ring);

		if (link_trb_count++ > ring->num_segs) {
			xhci_warn(xhci, "Ring is an endless link TRB loop\n");
			break;
		}
	}
	return;
}

/*
 * If enqueue points at a link TRB, follow links until an ordinary TRB is reached.
 * Toggle the cycle bit of passed link TRBs and optionally chain them.
 */
static void inc_enq_past_link(struct xhci_hcd *xhci, struct xhci_ring *ring, u32 chain)
{
	unsigned int link_trb_count = 0;

	while (trb_is_link(ring->enqueue)) {

		/*
		 * Section 6.4.4.1 of the 0.95 spec says link TRBs cannot have the chain bit
		 * set, but other sections talk about dealing with the chain bit set. This was
		 * fixed in the 0.96 specification errata, but we have to assume that all 0.95
		 * xHCI hardware can't handle the chain bit being cleared on a link TRB.
		 *
		 * On 0.95 and some 0.96 HCs the chain bit is set once at segment initalization
		 * and never changed here. On all others, modify it as requested by the caller.
		 */
		if (!xhci_link_chain_quirk(xhci, ring->type)) {
			ring->enqueue->link.control &= cpu_to_le32(~TRB_CHAIN);
			ring->enqueue->link.control |= cpu_to_le32(chain);
		}

		/* Give this link TRB to the hardware */
		wmb();
		ring->enqueue->link.control ^= cpu_to_le32(TRB_CYCLE);

		/* Toggle the cycle bit after the last ring segment. */
		if (link_trb_toggles_cycle(ring->enqueue))
			ring->cycle_state ^= 1;

		ring->enq_seg = ring->enq_seg->next;
		ring->enqueue = ring->enq_seg->trbs;

		trace_xhci_inc_enq(ring);

		if (link_trb_count++ > ring->num_segs) {
			xhci_warn(xhci, "Link TRB loop at enqueue\n");
			break;
		}
	}
}

/*
 * See Cycle bit rules. SW is the consumer for the event ring only.
 *
 * If we've just enqueued a TRB that is in the middle of a TD (meaning the
 * chain bit is set), then set the chain bit in all the following link TRBs.
 * If we've enqueued the last TRB in a TD, make sure the following link TRBs
 * have their chain bit cleared (so that each Link TRB is a separate TD).
 *
 * @more_trbs_coming:	Will you enqueue more TRBs before calling
 *			prepare_transfer()?
 */
static void inc_enq(struct xhci_hcd *xhci, struct xhci_ring *ring,
			bool more_trbs_coming)
{
	u32 chain;

	chain = le32_to_cpu(ring->enqueue->generic.field[3]) & TRB_CHAIN;

	if (last_trb_on_seg(ring->enq_seg, ring->enqueue)) {
		xhci_err(xhci, "Tried to move enqueue past ring segment\n");
		return;
	}

	ring->enqueue++;

	/*
	 * If we are in the middle of a TD or the caller plans to enqueue more
	 * TDs as one transfer (eg. control), traverse any link TRBs right now.
	 * Otherwise, enqueue can stay on a link until the next prepare_ring().
	 * This avoids enqueue entering deq_seg and simplifies ring expansion.
	 */
	if (trb_is_link(ring->enqueue) && (chain || more_trbs_coming))
		inc_enq_past_link(xhci, ring, chain);
}

/*
 * If the suspect DMA address is a TRB in this TD, this function returns that
 * TRB's segment. Otherwise it returns 0.
 */
static struct xhci_segment *trb_in_td(struct xhci_td *td, dma_addr_t suspect_dma)
{
	dma_addr_t start_dma;
	dma_addr_t end_seg_dma;
	dma_addr_t end_trb_dma;
	struct xhci_segment *cur_seg;

	start_dma = xhci_trb_virt_to_dma(td->start_seg, td->start_trb);
	cur_seg = td->start_seg;

	do {
		if (start_dma == 0)
			return NULL;
		/* We may get an event for a Link TRB in the middle of a TD */
		end_seg_dma = xhci_trb_virt_to_dma(cur_seg,
				&cur_seg->trbs[TRBS_PER_SEGMENT - 1]);
		/* If the end TRB isn't in this segment, this is set to 0 */
		end_trb_dma = xhci_trb_virt_to_dma(cur_seg, td->end_trb);

		if (end_trb_dma > 0) {
			/* The end TRB is in this segment, so suspect should be here */
			if (start_dma <= end_trb_dma) {
				if (suspect_dma >= start_dma && suspect_dma <= end_trb_dma)
					return cur_seg;
			} else {
				/* Case for one segment with
				 * a TD wrapped around to the top
				 */
				if ((suspect_dma >= start_dma &&
							suspect_dma <= end_seg_dma) ||
						(suspect_dma >= cur_seg->dma &&
						 suspect_dma <= end_trb_dma))
					return cur_seg;
			}
			return NULL;
		}
		/* Might still be somewhere in this segment */
		if (suspect_dma >= start_dma && suspect_dma <= end_seg_dma)
			return cur_seg;

		cur_seg = cur_seg->next;
		start_dma = xhci_trb_virt_to_dma(cur_seg, &cur_seg->trbs[0]);
	} while (cur_seg != td->start_seg);

	return NULL;
}

/*
 * Return number of free normal TRBs from enqueue to dequeue pointer on ring.
 * Not counting an assumed link TRB at end of each TRBS_PER_SEGMENT sized segment.
 * Only for transfer and command rings where driver is the producer, not for
 * event rings.
 */
static unsigned int xhci_num_trbs_free(struct xhci_ring *ring)
{
	struct xhci_segment *enq_seg = ring->enq_seg;
	union xhci_trb *enq = ring->enqueue;
	union xhci_trb *last_on_seg;
	unsigned int free = 0;
	int i = 0;

	/* Ring might be empty even if enq != deq if enq is left on a link trb */
	if (trb_is_link(enq)) {
		enq_seg = enq_seg->next;
		enq = enq_seg->trbs;
	}

	/* Empty ring, common case, don't walk the segments */
	if (enq == ring->dequeue)
		return ring->num_segs * (TRBS_PER_SEGMENT - 1);

	do {
		if (ring->deq_seg == enq_seg && ring->dequeue >= enq)
			return free + (ring->dequeue - enq);
		last_on_seg = &enq_seg->trbs[TRBS_PER_SEGMENT - 1];
		free += last_on_seg - enq;
		enq_seg = enq_seg->next;
		enq = enq_seg->trbs;
	} while (i++ < ring->num_segs);

	return free;
}

/*
 * Check to see if there's room to enqueue num_trbs on the ring and make sure
 * enqueue pointer will not advance into dequeue segment. See rules above.
 * return number of new segments needed to ensure this.
 */

static unsigned int xhci_ring_expansion_needed(struct xhci_hcd *xhci, struct xhci_ring *ring,
					       unsigned int num_trbs)
{
	struct xhci_segment *seg;
	int trbs_past_seg;
	int enq_used;
	int new_segs;

	enq_used = ring->enqueue - ring->enq_seg->trbs;

	/* how many trbs will be queued past the enqueue segment? */
	trbs_past_seg = enq_used + num_trbs - (TRBS_PER_SEGMENT - 1);

	/*
	 * Consider expanding the ring already if num_trbs fills the current
	 * segment (i.e. trbs_past_seg == 0), not only when num_trbs goes into
	 * the next segment. Avoids confusing full ring with special empty ring
	 * case below
	 */
	if (trbs_past_seg < 0)
		return 0;

	/* Empty ring special case, enqueue stuck on link trb while dequeue advanced */
	if (trb_is_link(ring->enqueue) && ring->enq_seg->next->trbs == ring->dequeue)
		return 0;

	new_segs = 1 + (trbs_past_seg / (TRBS_PER_SEGMENT - 1));
	seg = ring->enq_seg;

	while (new_segs > 0) {
		seg = seg->next;
		if (seg == ring->deq_seg) {
			xhci_dbg(xhci, "Adding %d trbs requires expanding ring by %d segments\n",
				 num_trbs, new_segs);
			return new_segs;
		}
		new_segs--;
	}

	return 0;
}

/* Ring the host controller doorbell after placing a command on the ring */
void xhci_ring_cmd_db(struct xhci_hcd *xhci)
{
	if (!(xhci->cmd_ring_state & CMD_RING_STATE_RUNNING))
		return;

	xhci_dbg(xhci, "// Ding dong!\n");

	trace_xhci_ring_host_doorbell(0, DB_VALUE_HOST);

	writel(DB_VALUE_HOST, &xhci->dba->doorbell[0]);
	/* Flush PCI posted writes */
	readl(&xhci->dba->doorbell[0]);
}

static bool xhci_mod_cmd_timer(struct xhci_hcd *xhci)
{
	return mod_delayed_work(system_wq, &xhci->cmd_timer,
			msecs_to_jiffies(xhci->current_cmd->timeout_ms));
}

static struct xhci_command *xhci_next_queued_cmd(struct xhci_hcd *xhci)
{
	return list_first_entry_or_null(&xhci->cmd_list, struct xhci_command,
					cmd_list);
}

/*
 * Turn all commands on command ring with status set to "aborted" to no-op trbs.
 * If there are other commands waiting then restart the ring and kick the timer.
 * This must be called with command ring stopped and xhci->lock held.
 */
static void xhci_handle_stopped_cmd_ring(struct xhci_hcd *xhci,
					 struct xhci_command *cur_cmd)
{
	struct xhci_command *i_cmd;

	/* Turn all aborted commands in list to no-ops, then restart */
	list_for_each_entry(i_cmd, &xhci->cmd_list, cmd_list) {

		if (i_cmd->status != COMP_COMMAND_ABORTED)
			continue;

		i_cmd->status = COMP_COMMAND_RING_STOPPED;

		xhci_dbg(xhci, "Turn aborted command %p to no-op\n",
			 i_cmd->command_trb);

		trb_to_noop(i_cmd->command_trb, TRB_CMD_NOOP);

		/*
		 * caller waiting for completion is called when command
		 *  completion event is received for these no-op commands
		 */
	}

	xhci->cmd_ring_state = CMD_RING_STATE_RUNNING;

	/* ring command ring doorbell to restart the command ring */
	if ((xhci->cmd_ring->dequeue != xhci->cmd_ring->enqueue) &&
	    !(xhci->xhc_state & XHCI_STATE_DYING)) {
		xhci->current_cmd = cur_cmd;
		if (cur_cmd)
			xhci_mod_cmd_timer(xhci);
		xhci_ring_cmd_db(xhci);
	}
}

/* Must be called with xhci->lock held, releases and acquires lock back */
static int xhci_abort_cmd_ring(struct xhci_hcd *xhci, unsigned long flags)
{
	struct xhci_segment *new_seg	= xhci->cmd_ring->deq_seg;
	union xhci_trb *new_deq		= xhci->cmd_ring->dequeue;
	u64 crcr;
	int ret;

	xhci_dbg(xhci, "Abort command ring\n");

	reinit_completion(&xhci->cmd_ring_stop_completion);

	/*
	 * The control bits like command stop, abort are located in lower
	 * dword of the command ring control register.
	 * Some controllers require all 64 bits to be written to abort the ring.
	 * Make sure the upper dword is valid, pointing to the next command,
	 * avoiding corrupting the command ring pointer in case the command ring
	 * is stopped by the time the upper dword is written.
	 */
	next_trb(&new_seg, &new_deq);
	if (trb_is_link(new_deq))
		next_trb(&new_seg, &new_deq);

	crcr = xhci_trb_virt_to_dma(new_seg, new_deq);
	xhci_write_64(xhci, crcr | CMD_RING_ABORT, &xhci->op_regs->cmd_ring);

	/* Section 4.6.1.2 of xHCI 1.0 spec says software should also time the
	 * completion of the Command Abort operation. If CRR is not negated in 5
	 * seconds then driver handles it as if host died (-ENODEV).
	 * In the future we should distinguish between -ENODEV and -ETIMEDOUT
	 * and try to recover a -ETIMEDOUT with a host controller reset.
	 */
	ret = xhci_handshake(&xhci->op_regs->cmd_ring,
			CMD_RING_RUNNING, 0, 5 * 1000 * 1000);
	if (ret < 0) {
		xhci_err(xhci, "Abort failed to stop command ring: %d\n", ret);
		xhci_halt(xhci);
		xhci_hc_died(xhci);
		return ret;
	}
	/*
	 * Writing the CMD_RING_ABORT bit should cause a cmd completion event,
	 * however on some host hw the CMD_RING_RUNNING bit is correctly cleared
	 * but the completion event in never sent. Wait 2 secs (arbitrary
	 * number) to handle those cases after negation of CMD_RING_RUNNING.
	 */
	spin_unlock_irqrestore(&xhci->lock, flags);
	ret = wait_for_completion_timeout(&xhci->cmd_ring_stop_completion,
					  msecs_to_jiffies(2000));
	spin_lock_irqsave(&xhci->lock, flags);
	if (!ret) {
		xhci_dbg(xhci, "No stop event for abort, ring start fail?\n");
		xhci_cleanup_command_queue(xhci);
	} else {
		xhci_handle_stopped_cmd_ring(xhci, xhci_next_queued_cmd(xhci));
	}
	return 0;
}

void xhci_ring_ep_doorbell(struct xhci_hcd *xhci,
		unsigned int slot_id,
		unsigned int ep_index,
		unsigned int stream_id)
{
	__le32 __iomem *db_addr = &xhci->dba->doorbell[slot_id];
	struct xhci_virt_ep *ep = &xhci->devs[slot_id]->eps[ep_index];
	unsigned int ep_state = ep->ep_state;

	/* Don't ring the doorbell for this endpoint if there are pending
	 * cancellations because we don't want to interrupt processing.
	 * We don't want to restart any stream rings if there's a set dequeue
	 * pointer command pending because the device can choose to start any
	 * stream once the endpoint is on the HW schedule.
	 */
	if ((ep_state & EP_STOP_CMD_PENDING) || (ep_state & SET_DEQ_PENDING) ||
	    (ep_state & EP_HALTED) || (ep_state & EP_CLEARING_TT))
		return;

	trace_xhci_ring_ep_doorbell(slot_id, DB_VALUE(ep_index, stream_id));

	writel(DB_VALUE(ep_index, stream_id), db_addr);
	/* flush the write */
	readl(db_addr);
}

/* Ring the doorbell for any rings with pending URBs */
static void ring_doorbell_for_active_rings(struct xhci_hcd *xhci,
		unsigned int slot_id,
		unsigned int ep_index)
{
	unsigned int stream_id;
	struct xhci_virt_ep *ep;

	ep = &xhci->devs[slot_id]->eps[ep_index];

	/* A ring has pending URBs if its TD list is not empty */
	if (!(ep->ep_state & EP_HAS_STREAMS)) {
		if (ep->ring && !(list_empty(&ep->ring->td_list)))
			xhci_ring_ep_doorbell(xhci, slot_id, ep_index, 0);
		return;
	}

	for (stream_id = 1; stream_id < ep->stream_info->num_streams;
			stream_id++) {
		struct xhci_stream_info *stream_info = ep->stream_info;
		if (!list_empty(&stream_info->stream_rings[stream_id]->td_list))
			xhci_ring_ep_doorbell(xhci, slot_id, ep_index,
						stream_id);
	}
}

void xhci_ring_doorbell_for_active_rings(struct xhci_hcd *xhci,
		unsigned int slot_id,
		unsigned int ep_index)
{
	ring_doorbell_for_active_rings(xhci, slot_id, ep_index);
}

static struct xhci_virt_ep *xhci_get_virt_ep(struct xhci_hcd *xhci,
					     unsigned int slot_id,
					     unsigned int ep_index)
{
	if (slot_id == 0 || slot_id >= MAX_HC_SLOTS) {
		xhci_warn(xhci, "Invalid slot_id %u\n", slot_id);
		return NULL;
	}
	if (ep_index >= EP_CTX_PER_DEV) {
		xhci_warn(xhci, "Invalid endpoint index %u\n", ep_index);
		return NULL;
	}
	if (!xhci->devs[slot_id]) {
		xhci_warn(xhci, "No xhci virt device for slot_id %u\n", slot_id);
		return NULL;
	}

	return &xhci->devs[slot_id]->eps[ep_index];
}

static struct xhci_ring *xhci_virt_ep_to_ring(struct xhci_hcd *xhci,
					      struct xhci_virt_ep *ep,
					      unsigned int stream_id)
{
	/* common case, no streams */
	if (!(ep->ep_state & EP_HAS_STREAMS))
		return ep->ring;

	if (!ep->stream_info)
		return NULL;

	if (stream_id == 0 || stream_id >= ep->stream_info->num_streams) {
		xhci_warn(xhci, "Invalid stream_id %u request for slot_id %u ep_index %u\n",
			  stream_id, ep->vdev->slot_id, ep->ep_index);
		return NULL;
	}

	return ep->stream_info->stream_rings[stream_id];
}

/* Get the right ring for the given slot_id, ep_index and stream_id.
 * If the endpoint supports streams, boundary check the URB's stream ID.
 * If the endpoint doesn't support streams, return the singular endpoint ring.
 */
struct xhci_ring *xhci_triad_to_transfer_ring(struct xhci_hcd *xhci,
		unsigned int slot_id, unsigned int ep_index,
		unsigned int stream_id)
{
	struct xhci_virt_ep *ep;

	ep = xhci_get_virt_ep(xhci, slot_id, ep_index);
	if (!ep)
		return NULL;

	return xhci_virt_ep_to_ring(xhci, ep, stream_id);
}


/*
 * Get the hw dequeue pointer xHC stopped on, either directly from the
 * endpoint context, or if streams are in use from the stream context.
 * The returned hw_dequeue contains the lowest four bits with cycle state
 * and possbile stream context type.
 */
static u64 xhci_get_hw_deq(struct xhci_hcd *xhci, struct xhci_virt_device *vdev,
			   unsigned int ep_index, unsigned int stream_id)
{
	struct xhci_ep_ctx *ep_ctx;
	struct xhci_stream_ctx *st_ctx;
	struct xhci_virt_ep *ep;

	ep = &vdev->eps[ep_index];

	if (ep->ep_state & EP_HAS_STREAMS) {
		st_ctx = &ep->stream_info->stream_ctx_array[stream_id];
		return le64_to_cpu(st_ctx->stream_ring);
	}
	ep_ctx = xhci_get_ep_ctx(xhci, vdev->out_ctx, ep_index);
	return le64_to_cpu(ep_ctx->deq);
}

static int xhci_move_dequeue_past_td(struct xhci_hcd *xhci,
				unsigned int slot_id, unsigned int ep_index,
				unsigned int stream_id, struct xhci_td *td)
{
	struct xhci_virt_device *dev = xhci->devs[slot_id];
	struct xhci_virt_ep *ep = &dev->eps[ep_index];
	struct xhci_ring *ep_ring;
	struct xhci_command *cmd;
	struct xhci_segment *new_seg;
	union xhci_trb *new_deq;
	int new_cycle;
	dma_addr_t addr;
	u64 hw_dequeue;
	bool hw_dequeue_found = false;
	bool td_last_trb_found = false;
	u32 trb_sct = 0;
	int ret;

	ep_ring = xhci_triad_to_transfer_ring(xhci, slot_id,
			ep_index, stream_id);
	if (!ep_ring) {
		xhci_warn(xhci, "WARN can't find new dequeue, invalid stream ID %u\n",
			  stream_id);
		return -ENODEV;
	}

	hw_dequeue = xhci_get_hw_deq(xhci, dev, ep_index, stream_id);
	new_seg = ep_ring->deq_seg;
	new_deq = ep_ring->dequeue;
	new_cycle = le32_to_cpu(td->end_trb->generic.field[3]) & TRB_CYCLE;

	/*
	 * Walk the ring until both the next TRB and hw_dequeue are found (don't
	 * move hw_dequeue back if it went forward due to a HW bug). Cycle state
	 * is loaded from a known good TRB, track later toggles to maintain it.
	 */
	do {
		if (!hw_dequeue_found && xhci_trb_virt_to_dma(new_seg, new_deq)
		    == (dma_addr_t)(hw_dequeue & ~0xf)) {
			hw_dequeue_found = true;
			if (td_last_trb_found)
				break;
		}
		if (new_deq == td->end_trb)
			td_last_trb_found = true;

		if (td_last_trb_found && trb_is_link(new_deq) &&
		    link_trb_toggles_cycle(new_deq))
			new_cycle ^= 0x1;

		next_trb(&new_seg, &new_deq);

		/* Search wrapped around, bail out */
		if (new_deq == ep->ring->dequeue) {
			xhci_err(xhci, "Error: Failed finding new dequeue state\n");
			return -EINVAL;
		}

	} while (!hw_dequeue_found || !td_last_trb_found);

	/* Don't update the ring cycle state for the producer (us). */
	addr = xhci_trb_virt_to_dma(new_seg, new_deq);
	if (addr == 0) {
		xhci_warn(xhci, "Can't find dma of new dequeue ptr\n");
		xhci_warn(xhci, "deq seg = %p, deq ptr = %p\n", new_seg, new_deq);
		return -EINVAL;
	}

	if ((ep->ep_state & SET_DEQ_PENDING)) {
		xhci_warn(xhci, "Set TR Deq already pending, don't submit for 0x%pad\n",
			  &addr);
		return -EBUSY;
	}

	/* This function gets called from contexts where it cannot sleep */
	cmd = xhci_alloc_command(xhci, false, GFP_ATOMIC);
	if (!cmd) {
		xhci_warn(xhci, "Can't alloc Set TR Deq cmd 0x%pad\n", &addr);
		return -ENOMEM;
	}

	if (stream_id)
		trb_sct = SCT_FOR_TRB(SCT_PRI_TR);
	ret = queue_command(xhci, cmd,
		lower_32_bits(addr) | trb_sct | new_cycle,
		upper_32_bits(addr),
		STREAM_ID_FOR_TRB(stream_id), SLOT_ID_FOR_TRB(slot_id) |
		EP_INDEX_FOR_TRB(ep_index) | TRB_TYPE(TRB_SET_DEQ), false);
	if (ret < 0) {
		xhci_free_command(xhci, cmd);
		return ret;
	}
	ep->queued_deq_seg = new_seg;
	ep->queued_deq_ptr = new_deq;

	xhci_dbg_trace(xhci, trace_xhci_dbg_cancel_urb,
		       "Set TR Deq ptr 0x%llx, cycle %u\n", addr, new_cycle);

	/* Stop the TD queueing code from ringing the doorbell until
	 * this command completes.  The HC won't set the dequeue pointer
	 * if the ring is running, and ringing the doorbell starts the
	 * ring running.
	 */
	ep->ep_state |= SET_DEQ_PENDING;
	xhci_ring_cmd_db(xhci);
	return 0;
}

/* flip_cycle means flip the cycle bit of all but the first and last TRB.
 * (The last TRB actually points to the ring enqueue pointer, which is not part
 * of this TD.)  This is used to remove partially enqueued isoc TDs from a ring.
 */
static void td_to_noop(struct xhci_td *td, bool flip_cycle)
{
	struct xhci_segment *seg	= td->start_seg;
	union xhci_trb *trb		= td->start_trb;

	while (1) {
		trb_to_noop(trb, TRB_TR_NOOP);

		/* flip cycle if asked to */
		if (flip_cycle && trb != td->start_trb && trb != td->end_trb)
			trb->generic.field[3] ^= cpu_to_le32(TRB_CYCLE);

		if (trb == td->end_trb)
			break;

		next_trb(&seg, &trb);
	}
}

static void xhci_giveback_urb_in_irq(struct xhci_hcd *xhci,
				     struct xhci_td *cur_td, int status)
{
	struct urb	*urb		= cur_td->urb;
	struct urb_priv	*urb_priv	= urb->hcpriv;
	struct usb_hcd	*hcd		= bus_to_hcd(urb->dev->bus);

	if (usb_pipetype(urb->pipe) == PIPE_ISOCHRONOUS) {
		xhci_to_hcd(xhci)->self.bandwidth_isoc_reqs--;
		if (xhci_to_hcd(xhci)->self.bandwidth_isoc_reqs	== 0) {
			if (xhci->quirks & XHCI_AMD_PLL_FIX)
				usb_amd_quirk_pll_enable();
		}
	}
	xhci_urb_free_priv(urb_priv);
	usb_hcd_unlink_urb_from_ep(hcd, urb);
	trace_xhci_urb_giveback(urb);
	usb_hcd_giveback_urb(hcd, urb, status);
}

static void xhci_unmap_td_bounce_buffer(struct xhci_hcd *xhci,
		struct xhci_ring *ring, struct xhci_td *td)
{
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;
	struct xhci_segment *seg = td->bounce_seg;
	struct urb *urb = td->urb;
	size_t len;

	if (!ring || !seg || !urb)
		return;

	if (usb_urb_dir_out(urb)) {
		dma_unmap_single(dev, seg->bounce_dma, ring->bounce_buf_len,
				 DMA_TO_DEVICE);
		return;
	}

	dma_unmap_single(dev, seg->bounce_dma, ring->bounce_buf_len,
			 DMA_FROM_DEVICE);
	/* for in transfers we need to copy the data from bounce to sg */
	if (urb->num_sgs) {
		len = sg_pcopy_from_buffer(urb->sg, urb->num_sgs, seg->bounce_buf,
					   seg->bounce_len, seg->bounce_offs);
		if (len != seg->bounce_len)
			xhci_warn(xhci, "WARN Wrong bounce buffer read length: %zu != %d\n",
				  len, seg->bounce_len);
	} else {
		memcpy(urb->transfer_buffer + seg->bounce_offs, seg->bounce_buf,
		       seg->bounce_len);
	}
	seg->bounce_len = 0;
	seg->bounce_offs = 0;
}

static void xhci_td_cleanup(struct xhci_hcd *xhci, struct xhci_td *td,
			    struct xhci_ring *ep_ring, int status)
{
	struct urb *urb = NULL;

	/* Clean up the endpoint's TD list */
	urb = td->urb;

	/* if a bounce buffer was used to align this td then unmap it */
	xhci_unmap_td_bounce_buffer(xhci, ep_ring, td);

	/* Do one last check of the actual transfer length.
	 * If the host controller said we transferred more data than the buffer
	 * length, urb->actual_length will be a very big number (since it's
	 * unsigned).  Play it safe and say we didn't transfer anything.
	 */
	if (urb->actual_length > urb->transfer_buffer_length) {
		xhci_warn(xhci, "URB req %u and actual %u transfer length mismatch\n",
			  urb->transfer_buffer_length, urb->actual_length);
		urb->actual_length = 0;
		status = 0;
	}
	/* TD might be removed from td_list if we are giving back a cancelled URB */
	if (!list_empty(&td->td_list))
		list_del_init(&td->td_list);
	/* Giving back a cancelled URB, or if a slated TD completed anyway */
	if (!list_empty(&td->cancelled_td_list))
		list_del_init(&td->cancelled_td_list);

	inc_td_cnt(urb);
	/* Giveback the urb when all the tds are completed */
	if (last_td_in_urb(td)) {
		if ((urb->actual_length != urb->transfer_buffer_length &&
		     (urb->transfer_flags & URB_SHORT_NOT_OK)) ||
		    (status != 0 && !usb_endpoint_xfer_isoc(&urb->ep->desc)))
			xhci_dbg(xhci, "Giveback URB %p, len = %d, expected = %d, status = %d\n",
				 urb, urb->actual_length,
				 urb->transfer_buffer_length, status);

		/* set isoc urb status to 0 just as EHCI, UHCI, and OHCI */
		if (usb_pipetype(urb->pipe) == PIPE_ISOCHRONOUS)
			status = 0;
		xhci_giveback_urb_in_irq(xhci, td, status);
	}
}

/* Give back previous TD and move on to the next TD. */
static void xhci_dequeue_td(struct xhci_hcd *xhci, struct xhci_td *td, struct xhci_ring *ring,
			    u32 status)
{
	ring->dequeue = td->end_trb;
	ring->deq_seg = td->end_seg;
	inc_deq(xhci, ring);

	xhci_td_cleanup(xhci, td, ring, status);
}

/* Complete the cancelled URBs we unlinked from td_list. */
static void xhci_giveback_invalidated_tds(struct xhci_virt_ep *ep)
{
	struct xhci_ring *ring;
	struct xhci_td *td, *tmp_td;

	list_for_each_entry_safe(td, tmp_td, &ep->cancelled_td_list,
				 cancelled_td_list) {

		ring = xhci_urb_to_transfer_ring(ep->xhci, td->urb);

		if (td->cancel_status == TD_CLEARED) {
			xhci_dbg(ep->xhci, "%s: Giveback cancelled URB %p TD\n",
				 __func__, td->urb);
			xhci_td_cleanup(ep->xhci, td, ring, td->status);
		} else {
			xhci_dbg(ep->xhci, "%s: Keep cancelled URB %p TD as cancel_status is %d\n",
				 __func__, td->urb, td->cancel_status);
		}
		if (ep->xhci->xhc_state & XHCI_STATE_DYING)
			return;
	}
}

static int xhci_reset_halted_ep(struct xhci_hcd *xhci, unsigned int slot_id,
				unsigned int ep_index, enum xhci_ep_reset_type reset_type)
{
	struct xhci_command *command;
	int ret = 0;

	command = xhci_alloc_command(xhci, false, GFP_ATOMIC);
	if (!command) {
		ret = -ENOMEM;
		goto done;
	}

	xhci_dbg(xhci, "%s-reset ep %u, slot %u\n",
		 (reset_type == EP_HARD_RESET) ? "Hard" : "Soft",
		 ep_index, slot_id);

	ret = xhci_queue_reset_ep(xhci, command, slot_id, ep_index, reset_type);
done:
	if (ret)
		xhci_err(xhci, "ERROR queuing reset endpoint for slot %d ep_index %d, %d\n",
			 slot_id, ep_index, ret);
	return ret;
}

static int xhci_handle_halted_endpoint(struct xhci_hcd *xhci,
				struct xhci_virt_ep *ep,
				struct xhci_td *td,
				enum xhci_ep_reset_type reset_type)
{
	unsigned int slot_id = ep->vdev->slot_id;
	int err;

	/*
	 * Avoid resetting endpoint if link is inactive. Can cause host hang.
	 * Device will be reset soon to recover the link so don't do anything
	 */
	if (ep->vdev->flags & VDEV_PORT_ERROR)
		return -ENODEV;

	/* add td to cancelled list and let reset ep handler take care of it */
	if (reset_type == EP_HARD_RESET) {
		ep->ep_state |= EP_HARD_CLEAR_TOGGLE;
		if (td && list_empty(&td->cancelled_td_list)) {
			list_add_tail(&td->cancelled_td_list, &ep->cancelled_td_list);
			td->cancel_status = TD_HALTED;
		}
	}

	if (ep->ep_state & EP_HALTED) {
		xhci_dbg(xhci, "Reset ep command for ep_index %d already pending\n",
			 ep->ep_index);
		return 0;
	}

	err = xhci_reset_halted_ep(xhci, slot_id, ep->ep_index, reset_type);
	if (err)
		return err;

	ep->ep_state |= EP_HALTED;

	xhci_ring_cmd_db(xhci);

	return 0;
}

/*
 * Fix up the ep ring first, so HW stops executing cancelled TDs.
 * We have the xHCI lock, so nothing can modify this list until we drop it.
 * We're also in the event handler, so we can't get re-interrupted if another
 * Stop Endpoint command completes.
 *
 * only call this when ring is not in a running state
 */

static int xhci_invalidate_cancelled_tds(struct xhci_virt_ep *ep)
{
	struct xhci_hcd		*xhci;
	struct xhci_td		*td = NULL;
	struct xhci_td		*tmp_td = NULL;
	struct xhci_td		*cached_td = NULL;
	struct xhci_ring	*ring;
	u64			hw_deq;
	unsigned int		slot_id = ep->vdev->slot_id;
	int			err;

	/*
	 * This is not going to work if the hardware is changing its dequeue
	 * pointers as we look at them. Completion handler will call us later.
	 */
	if (ep->ep_state & SET_DEQ_PENDING)
		return 0;

	xhci = ep->xhci;

	list_for_each_entry_safe(td, tmp_td, &ep->cancelled_td_list, cancelled_td_list) {
		xhci_dbg_trace(xhci, trace_xhci_dbg_cancel_urb,
			       "Removing canceled TD starting at 0x%llx (dma) in stream %u URB %p",
			       (unsigned long long)xhci_trb_virt_to_dma(
				       td->start_seg, td->start_trb),
			       td->urb->stream_id, td->urb);
		list_del_init(&td->td_list);
		ring = xhci_urb_to_transfer_ring(xhci, td->urb);
		if (!ring) {
			xhci_warn(xhci, "WARN Cancelled URB %p has invalid stream ID %u.\n",
				  td->urb, td->urb->stream_id);
			continue;
		}
		/*
		 * If a ring stopped on the TD we need to cancel then we have to
		 * move the xHC endpoint ring dequeue pointer past this TD.
		 * Rings halted due to STALL may show hw_deq is past the stalled
		 * TD, but still require a set TR Deq command to flush xHC cache.
		 */
		hw_deq = xhci_get_hw_deq(xhci, ep->vdev, ep->ep_index,
					 td->urb->stream_id);
		hw_deq &= ~0xf;

		if (td->cancel_status == TD_HALTED || trb_in_td(td, hw_deq)) {
			switch (td->cancel_status) {
			case TD_CLEARED: /* TD is already no-op */
			case TD_CLEARING_CACHE: /* set TR deq command already queued */
				break;
			case TD_DIRTY: /* TD is cached, clear it */
			case TD_HALTED:
			case TD_CLEARING_CACHE_DEFERRED:
				if (cached_td) {
					if (cached_td->urb->stream_id != td->urb->stream_id) {
						/* Multiple streams case, defer move dq */
						xhci_dbg(xhci,
							 "Move dq deferred: stream %u URB %p\n",
							 td->urb->stream_id, td->urb);
						td->cancel_status = TD_CLEARING_CACHE_DEFERRED;
						break;
					}

					/* Should never happen, but clear the TD if it does */
					xhci_warn(xhci,
						  "Found multiple active URBs %p and %p in stream %u?\n",
						  td->urb, cached_td->urb,
						  td->urb->stream_id);
					td_to_noop(cached_td, false);
					cached_td->cancel_status = TD_CLEARED;
				}
				td_to_noop(td, false);
				td->cancel_status = TD_CLEARING_CACHE;
				cached_td = td;
				break;
			}
		} else {
			td_to_noop(td, false);
			td->cancel_status = TD_CLEARED;
		}
	}

	/* If there's no need to move the dequeue pointer then we're done */
	if (!cached_td)
		return 0;

	err = xhci_move_dequeue_past_td(xhci, slot_id, ep->ep_index,
					cached_td->urb->stream_id,
					cached_td);
	if (err) {
		/* Failed to move past cached td, just set cached TDs to no-op */
		list_for_each_entry_safe(td, tmp_td, &ep->cancelled_td_list, cancelled_td_list) {
			/*
			 * Deferred TDs need to have the deq pointer set after the above command
			 * completes, so if that failed we just give up on all of them (and
			 * complain loudly since this could cause issues due to caching).
			 */
			if (td->cancel_status != TD_CLEARING_CACHE &&
			    td->cancel_status != TD_CLEARING_CACHE_DEFERRED)
				continue;
			xhci_warn(xhci, "Failed to clear cancelled cached URB %p, mark clear anyway\n",
				  td->urb);
			td_to_noop(td, false);
			td->cancel_status = TD_CLEARED;
		}
	}
	return 0;
}

/*
 * Erase queued TDs from transfer ring(s) and give back those the xHC didn't
 * stop on. If necessary, queue commands to move the xHC off cancelled TDs it
 * stopped on. Those will be given back later when the commands complete.
 *
 * Call under xhci->lock on a stopped endpoint.
 */
void xhci_process_cancelled_tds(struct xhci_virt_ep *ep)
{
	xhci_invalidate_cancelled_tds(ep);
	xhci_giveback_invalidated_tds(ep);
}

/*
 * Returns the TD the endpoint ring halted on.
 * Only call for non-running rings without streams.
 */
static struct xhci_td *find_halted_td(struct xhci_virt_ep *ep)
{
	struct xhci_td	*td;
	u64		hw_deq;

	if (!list_empty(&ep->ring->td_list)) { /* Not streams compatible */
		hw_deq = xhci_get_hw_deq(ep->xhci, ep->vdev, ep->ep_index, 0);
		hw_deq &= ~0xf;
		td = list_first_entry(&ep->ring->td_list, struct xhci_td, td_list);
		if (trb_in_td(td, hw_deq))
			return td;
	}
	return NULL;
}

/*
 * When we get a command completion for a Stop Endpoint Command, we need to
 * unlink any cancelled TDs from the ring.  There are two ways to do that:
 *
 *  1. If the HW was in the middle of processing the TD that needs to be
 *     cancelled, then we must move the ring's dequeue pointer past the last TRB
 *     in the TD with a Set Dequeue Pointer Command.
 *  2. Otherwise, we turn all the TRBs in the TD into No-op TRBs (with the chain
 *     bit cleared) so that the HW will skip over them.
 */
static void xhci_handle_cmd_stop_ep(struct xhci_hcd *xhci, int slot_id,
				    union xhci_trb *trb, u32 comp_code)
{
	unsigned int ep_index;
	struct xhci_virt_ep *ep;
	struct xhci_ep_ctx *ep_ctx;
	struct xhci_td *td = NULL;
	enum xhci_ep_reset_type reset_type;
	struct xhci_command *command;
	int err;

	if (unlikely(TRB_TO_SUSPEND_PORT(le32_to_cpu(trb->generic.field[3])))) {
		if (!xhci->devs[slot_id])
			xhci_warn(xhci, "Stop endpoint command completion for disabled slot %u\n",
				  slot_id);
		return;
	}

	ep_index = TRB_TO_EP_INDEX(le32_to_cpu(trb->generic.field[3]));
	ep = xhci_get_virt_ep(xhci, slot_id, ep_index);
	if (!ep)
		return;

	ep_ctx = xhci_get_ep_ctx(xhci, ep->vdev->out_ctx, ep_index);

	trace_xhci_handle_cmd_stop_ep(ep_ctx);

	if (comp_code == COMP_CONTEXT_STATE_ERROR) {
	/*
	 * If stop endpoint command raced with a halting endpoint we need to
	 * reset the host side endpoint first.
	 * If the TD we halted on isn't cancelled the TD should be given back
	 * with a proper error code, and the ring dequeue moved past the TD.
	 * If streams case we can't find hw_deq, or the TD we halted on so do a
	 * soft reset.
	 *
	 * Proper error code is unknown here, it would be -EPIPE if device side
	 * of enadpoit halted (aka STALL), and -EPROTO if not (transaction error)
	 * We use -EPROTO, if device is stalled it should return a stall error on
	 * next transfer, which then will return -EPIPE, and device side stall is
	 * noted and cleared by class driver.
	 */
		switch (GET_EP_CTX_STATE(ep_ctx)) {
		case EP_STATE_HALTED:
			xhci_dbg(xhci, "Stop ep completion raced with stall\n");
			/*
			 * If the halt happened before Stop Endpoint failed, its transfer event
			 * should have already been handled and Reset Endpoint should be pending.
			 */
			if (ep->ep_state & EP_HALTED)
				goto reset_done;

			if (ep->ep_state & EP_HAS_STREAMS) {
				reset_type = EP_SOFT_RESET;
			} else {
				reset_type = EP_HARD_RESET;
				td = find_halted_td(ep);
				if (td)
					td->status = -EPROTO;
			}
			/* reset ep, reset handler cleans up cancelled tds */
			err = xhci_handle_halted_endpoint(xhci, ep, td, reset_type);
			xhci_dbg(xhci, "Stop ep completion resetting ep, status %d\n", err);
			if (err)
				break;
reset_done:
			/* Reset EP handler will clean up cancelled TDs */
			ep->ep_state &= ~EP_STOP_CMD_PENDING;
			return;
		case EP_STATE_STOPPED:
			/*
			 * Per xHCI 4.6.9, Stop Endpoint command on a Stopped
			 * EP is a Context State Error, and EP stays Stopped.
			 *
			 * But maybe it failed on Halted, and somebody ran Reset
			 * Endpoint later. EP state is now Stopped and EP_HALTED
			 * still set because Reset EP handler will run after us.
			 */
			if (ep->ep_state & EP_HALTED)
				break;
			/*
			 * On some HCs EP state remains Stopped for some tens of
			 * us to a few ms or more after a doorbell ring, and any
			 * new Stop Endpoint fails without aborting the restart.
			 * This handler may run quickly enough to still see this
			 * Stopped state, but it will soon change to Running.
			 *
			 * Assume this bug on unexpected Stop Endpoint failures.
			 * Keep retrying until the EP starts and stops again.
			 */
			fallthrough;
		case EP_STATE_RUNNING:
			/* Race, HW handled stop ep cmd before ep was running */
			xhci_dbg(xhci, "Stop ep completion ctx error, ctx_state %d\n",
					GET_EP_CTX_STATE(ep_ctx));
			/*
			 * Don't retry forever if we guessed wrong or a defective HC never starts
			 * the EP or says 'Running' but fails the command. We must give back TDs.
			 */
			if (time_is_before_jiffies(ep->stop_time + msecs_to_jiffies(100)))
				break;

			command = xhci_alloc_command(xhci, false, GFP_ATOMIC);
			if (!command) {
				ep->ep_state &= ~EP_STOP_CMD_PENDING;
				return;
			}
			xhci_queue_stop_endpoint(xhci, command, slot_id, ep_index, 0);
			xhci_ring_cmd_db(xhci);

			return;
		default:
			break;
		}
	}

	/* will queue a set TR deq if stopped on a cancelled, uncleared TD */
	xhci_invalidate_cancelled_tds(ep);
	ep->ep_state &= ~EP_STOP_CMD_PENDING;

	/* Otherwise ring the doorbell(s) to restart queued transfers */
	xhci_giveback_invalidated_tds(ep);
	ring_doorbell_for_active_rings(xhci, slot_id, ep_index);
}

static void xhci_kill_ring_urbs(struct xhci_hcd *xhci, struct xhci_ring *ring)
{
	struct xhci_td *cur_td;
	struct xhci_td *tmp;

	list_for_each_entry_safe(cur_td, tmp, &ring->td_list, td_list) {
		list_del_init(&cur_td->td_list);

		if (!list_empty(&cur_td->cancelled_td_list))
			list_del_init(&cur_td->cancelled_td_list);

		xhci_unmap_td_bounce_buffer(xhci, ring, cur_td);

		inc_td_cnt(cur_td->urb);
		if (last_td_in_urb(cur_td))
			xhci_giveback_urb_in_irq(xhci, cur_td, -ESHUTDOWN);
	}
}

static void xhci_kill_endpoint_urbs(struct xhci_hcd *xhci,
		int slot_id, int ep_index)
{
	struct xhci_td *cur_td;
	struct xhci_td *tmp;
	struct xhci_virt_ep *ep;
	struct xhci_ring *ring;

	ep = xhci_get_virt_ep(xhci, slot_id, ep_index);
	if (!ep)
		return;

	if ((ep->ep_state & EP_HAS_STREAMS) ||
			(ep->ep_state & EP_GETTING_NO_STREAMS)) {
		int stream_id;

		for (stream_id = 1; stream_id < ep->stream_info->num_streams;
				stream_id++) {
			ring = ep->stream_info->stream_rings[stream_id];
			if (!ring)
				continue;

			xhci_dbg_trace(xhci, trace_xhci_dbg_cancel_urb,
					"Killing URBs for slot ID %u, ep index %u, stream %u",
					slot_id, ep_index, stream_id);
			xhci_kill_ring_urbs(xhci, ring);
		}
	} else {
		ring = ep->ring;
		if (!ring)
			return;
		xhci_dbg_trace(xhci, trace_xhci_dbg_cancel_urb,
				"Killing URBs for slot ID %u, ep index %u",
				slot_id, ep_index);
		xhci_kill_ring_urbs(xhci, ring);
	}

	list_for_each_entry_safe(cur_td, tmp, &ep->cancelled_td_list,
			cancelled_td_list) {
		list_del_init(&cur_td->cancelled_td_list);
		inc_td_cnt(cur_td->urb);

		if (last_td_in_urb(cur_td))
			xhci_giveback_urb_in_irq(xhci, cur_td, -ESHUTDOWN);
	}
}

/*
 * host controller died, register read returns 0xffffffff
 * Complete pending commands, mark them ABORTED.
 * URBs need to be given back as usb core might be waiting with device locks
 * held for the URBs to finish during device disconnect, blocking host remove.
 *
 * Call with xhci->lock held.
 * lock is relased and re-acquired while giving back urb.
 */
void xhci_hc_died(struct xhci_hcd *xhci)
{
	int i, j;

	if (xhci->xhc_state & XHCI_STATE_DYING)
		return;

	xhci_err(xhci, "xHCI host controller not responding, assume dead\n");
	xhci->xhc_state |= XHCI_STATE_DYING;

	xhci_cleanup_command_queue(xhci);

	/* return any pending urbs, remove may be waiting for them */
	for (i = 0; i <= HCS_MAX_SLOTS(xhci->hcs_params1); i++) {
		if (!xhci->devs[i])
			continue;
		for (j = 0; j < 31; j++)
			xhci_kill_endpoint_urbs(xhci, i, j);
	}

	/* inform usb core hc died if PCI remove isn't already handling it */
	if (!(xhci->xhc_state & XHCI_STATE_REMOVING))
		usb_hc_died(xhci_to_hcd(xhci));
}

/*
 * When we get a completion for a Set Transfer Ring Dequeue Pointer command,
 * we need to clear the set deq pending flag in the endpoint ring state, so that
 * the TD queueing code can ring the doorbell again.  We also need to ring the
 * endpoint doorbell to restart the ring, but only if there aren't more
 * cancellations pending.
 */
static void xhci_handle_cmd_set_deq(struct xhci_hcd *xhci, int slot_id,
		union xhci_trb *trb, u32 cmd_comp_code)
{
	unsigned int ep_index;
	unsigned int stream_id;
	struct xhci_ring *ep_ring;
	struct xhci_virt_ep *ep;
	struct xhci_ep_ctx *ep_ctx;
	struct xhci_slot_ctx *slot_ctx;
	struct xhci_stream_ctx *stream_ctx;
	struct xhci_td *td, *tmp_td;

	ep_index = TRB_TO_EP_INDEX(le32_to_cpu(trb->generic.field[3]));
	stream_id = TRB_TO_STREAM_ID(le32_to_cpu(trb->generic.field[2]));
	ep = xhci_get_virt_ep(xhci, slot_id, ep_index);
	if (!ep)
		return;

	ep_ring = xhci_virt_ep_to_ring(xhci, ep, stream_id);
	if (!ep_ring) {
		xhci_warn(xhci, "WARN Set TR deq ptr command for freed stream ID %u\n",
				stream_id);
		/* XXX: Harmless??? */
		goto cleanup;
	}

	ep_ctx = xhci_get_ep_ctx(xhci, ep->vdev->out_ctx, ep_index);
	slot_ctx = xhci_get_slot_ctx(xhci, ep->vdev->out_ctx);
	trace_xhci_handle_cmd_set_deq(slot_ctx);
	trace_xhci_handle_cmd_set_deq_ep(ep_ctx);

	if (ep->ep_state & EP_HAS_STREAMS) {
		stream_ctx = &ep->stream_info->stream_ctx_array[stream_id];
		trace_xhci_handle_cmd_set_deq_stream(ep->stream_info, stream_id);
	}

	if (cmd_comp_code != COMP_SUCCESS) {
		unsigned int ep_state;
		unsigned int slot_state;

		switch (cmd_comp_code) {
		case COMP_TRB_ERROR:
			xhci_warn(xhci, "WARN Set TR Deq Ptr cmd invalid because of stream ID configuration\n");
			break;
		case COMP_CONTEXT_STATE_ERROR:
			xhci_warn(xhci, "WARN Set TR Deq Ptr cmd failed due to incorrect slot or ep state.\n");
			ep_state = GET_EP_CTX_STATE(ep_ctx);
			slot_state = le32_to_cpu(slot_ctx->dev_state);
			slot_state = GET_SLOT_STATE(slot_state);
			xhci_dbg_trace(xhci, trace_xhci_dbg_cancel_urb,
					"Slot state = %u, EP state = %u",
					slot_state, ep_state);
			break;
		case COMP_SLOT_NOT_ENABLED_ERROR:
			xhci_warn(xhci, "WARN Set TR Deq Ptr cmd failed because slot %u was not enabled.\n",
					slot_id);
			break;
		default:
			xhci_warn(xhci, "WARN Set TR Deq Ptr cmd with unknown completion code of %u.\n",
					cmd_comp_code);
			break;
		}
		/* OK what do we do now?  The endpoint state is hosed, and we
		 * should never get to this point if the synchronization between
		 * queueing, and endpoint state are correct.  This might happen
		 * if the device gets disconnected after we've finished
		 * cancelling URBs, which might not be an error...
		 */
	} else {
		u64 deq;
		/* 4.6.10 deq ptr is written to the stream ctx for streams */
		if (ep->ep_state & EP_HAS_STREAMS) {
			deq = le64_to_cpu(stream_ctx->stream_ring) & SCTX_DEQ_MASK;

			/*
			 * Cadence xHCI controllers store some endpoint state
			 * information within Rsvd0 fields of Stream Endpoint
			 * context. This field is not cleared during Set TR
			 * Dequeue Pointer command which causes XDMA to skip
			 * over transfer ring and leads to data loss on stream
			 * pipe.
			 * To fix this issue driver must clear Rsvd0 field.
			 */
			if (xhci->quirks & XHCI_CDNS_SCTX_QUIRK) {
				stream_ctx->reserved[0] = 0;
				stream_ctx->reserved[1] = 0;
			}
		} else {
			deq = le64_to_cpu(ep_ctx->deq) & ~EP_CTX_CYCLE_MASK;
		}
		xhci_dbg_trace(xhci, trace_xhci_dbg_cancel_urb,
			"Successful Set TR Deq Ptr cmd, deq = @%08llx", deq);
		if (xhci_trb_virt_to_dma(ep->queued_deq_seg,
					 ep->queued_deq_ptr) == deq) {
			/* Update the ring's dequeue segment and dequeue pointer
			 * to reflect the new position.
			 */
			ep_ring->deq_seg = ep->queued_deq_seg;
			ep_ring->dequeue = ep->queued_deq_ptr;
		} else {
			xhci_warn(xhci, "Mismatch between completed Set TR Deq Ptr command & xHCI internal state.\n");
			xhci_warn(xhci, "ep deq seg = %p, deq ptr = %p\n",
				  ep->queued_deq_seg, ep->queued_deq_ptr);
		}
	}
	/* HW cached TDs cleared from cache, give them back */
	list_for_each_entry_safe(td, tmp_td, &ep->cancelled_td_list,
				 cancelled_td_list) {
		ep_ring = xhci_urb_to_transfer_ring(ep->xhci, td->urb);
		if (td->cancel_status == TD_CLEARING_CACHE) {
			td->cancel_status = TD_CLEARED;
			xhci_dbg(ep->xhci, "%s: Giveback cancelled URB %p TD\n",
				 __func__, td->urb);
			xhci_td_cleanup(ep->xhci, td, ep_ring, td->status);
		} else {
			xhci_dbg(ep->xhci, "%s: Keep cancelled URB %p TD as cancel_status is %d\n",
				 __func__, td->urb, td->cancel_status);
		}
	}
cleanup:
	ep->ep_state &= ~SET_DEQ_PENDING;
	ep->queued_deq_seg = NULL;
	ep->queued_deq_ptr = NULL;

	/* Check for deferred or newly cancelled TDs */
	if (!list_empty(&ep->cancelled_td_list)) {
		xhci_dbg(ep->xhci, "%s: Pending TDs to clear, continuing with invalidation\n",
			 __func__);
		xhci_invalidate_cancelled_tds(ep);
		/* Try to restart the endpoint if all is done */
		ring_doorbell_for_active_rings(xhci, slot_id, ep_index);
		/* Start giving back any TDs invalidated above */
		xhci_giveback_invalidated_tds(ep);
	} else {
		/* Restart any rings with pending URBs */
		xhci_dbg(ep->xhci, "%s: All TDs cleared, ring doorbell\n", __func__);
		ring_doorbell_for_active_rings(xhci, slot_id, ep_index);
	}
}

static void xhci_handle_cmd_reset_ep(struct xhci_hcd *xhci, int slot_id,
		union xhci_trb *trb, u32 cmd_comp_code)
{
	struct xhci_virt_ep *ep;
	struct xhci_ep_ctx *ep_ctx;
	unsigned int ep_index;

	ep_index = TRB_TO_EP_INDEX(le32_to_cpu(trb->generic.field[3]));
	ep = xhci_get_virt_ep(xhci, slot_id, ep_index);
	if (!ep)
		return;

	ep_ctx = xhci_get_ep_ctx(xhci, ep->vdev->out_ctx, ep_index);
	trace_xhci_handle_cmd_reset_ep(ep_ctx);

	/* This command will only fail if the endpoint wasn't halted,
	 * but we don't care.
	 */
	xhci_dbg_trace(xhci, trace_xhci_dbg_reset_ep,
		"Ignoring reset ep completion code of %u", cmd_comp_code);

	/* Cleanup cancelled TDs as ep is stopped. May queue a Set TR Deq cmd */
	xhci_invalidate_cancelled_tds(ep);

	/* Clear our internal halted state */
	ep->ep_state &= ~EP_HALTED;

	xhci_giveback_invalidated_tds(ep);

	/* if this was a soft reset, then restart */
	if ((le32_to_cpu(trb->generic.field[3])) & TRB_TSP)
		ring_doorbell_for_active_rings(xhci, slot_id, ep_index);
}

static void xhci_handle_cmd_enable_slot(int slot_id, struct xhci_command *command,
					u32 cmd_comp_code)
{
	if (cmd_comp_code == COMP_SUCCESS)
		command->slot_id = slot_id;
	else
		command->slot_id = 0;
}

static void xhci_handle_cmd_disable_slot(struct xhci_hcd *xhci, int slot_id)
{
	struct xhci_virt_device *virt_dev;
	struct xhci_slot_ctx *slot_ctx;

	virt_dev = xhci->devs[slot_id];
	if (!virt_dev)
		return;

	slot_ctx = xhci_get_slot_ctx(xhci, virt_dev->out_ctx);
	trace_xhci_handle_cmd_disable_slot(slot_ctx);

	if (xhci->quirks & XHCI_EP_LIMIT_QUIRK)
		/* Delete default control endpoint resources */
		xhci_free_device_endpoint_resources(xhci, virt_dev, true);
}

static void xhci_handle_cmd_config_ep(struct xhci_hcd *xhci, int slot_id)
{
	struct xhci_virt_device *virt_dev;
	struct xhci_input_control_ctx *ctrl_ctx;
	struct xhci_ep_ctx *ep_ctx;
	unsigned int ep_index;
	u32 add_flags;

	/*
	 * Configure endpoint commands can come from the USB core configuration
	 * or alt setting changes, or when streams were being configured.
	 */

	virt_dev = xhci->devs[slot_id];
	if (!virt_dev)
		return;
	ctrl_ctx = xhci_get_input_control_ctx(virt_dev->in_ctx);
	if (!ctrl_ctx) {
		xhci_warn(xhci, "Could not get input context, bad type.\n");
		return;
	}

	add_flags = le32_to_cpu(ctrl_ctx->add_flags);

	/* Input ctx add_flags are the endpoint index plus one */
	ep_index = xhci_last_valid_endpoint(add_flags) - 1;

	ep_ctx = xhci_get_ep_ctx(xhci, virt_dev->out_ctx, ep_index);
	trace_xhci_handle_cmd_config_ep(ep_ctx);

	return;
}

static void xhci_handle_cmd_addr_dev(struct xhci_hcd *xhci, int slot_id)
{
	struct xhci_virt_device *vdev;
	struct xhci_slot_ctx *slot_ctx;

	vdev = xhci->devs[slot_id];
	if (!vdev)
		return;
	slot_ctx = xhci_get_slot_ctx(xhci, vdev->out_ctx);
	trace_xhci_handle_cmd_addr_dev(slot_ctx);
}

static void xhci_handle_cmd_reset_dev(struct xhci_hcd *xhci, int slot_id)
{
	struct xhci_virt_device *vdev;
	struct xhci_slot_ctx *slot_ctx;

	vdev = xhci->devs[slot_id];
	if (!vdev) {
		xhci_warn(xhci, "Reset device command completion for disabled slot %u\n",
			  slot_id);
		return;
	}
	slot_ctx = xhci_get_slot_ctx(xhci, vdev->out_ctx);
	trace_xhci_handle_cmd_reset_dev(slot_ctx);

	xhci_dbg(xhci, "Completed reset device command.\n");
}

static void xhci_handle_cmd_nec_get_fw(struct xhci_hcd *xhci,
		struct xhci_event_cmd *event)
{
	if (!(xhci->quirks & XHCI_NEC_HOST)) {
		xhci_warn(xhci, "WARN NEC_GET_FW command on non-NEC host\n");
		return;
	}
	xhci_dbg_trace(xhci, trace_xhci_dbg_quirks,
			"NEC firmware version %2x.%02x",
			NEC_FW_MAJOR(le32_to_cpu(event->status)),
			NEC_FW_MINOR(le32_to_cpu(event->status)));
}

static void xhci_complete_del_and_free_cmd(struct xhci_command *cmd, u32 comp_code, u32 comp_param)
{
	list_del(&cmd->cmd_list);

	if (cmd->completion) {
		cmd->status = comp_code;
		cmd->comp_param = comp_param;
		complete(cmd->completion);
	} else {
		kfree(cmd);
	}
}

void xhci_cleanup_command_queue(struct xhci_hcd *xhci)
{
	struct xhci_command *cur_cmd, *tmp_cmd;
	xhci->current_cmd = NULL;
	list_for_each_entry_safe(cur_cmd, tmp_cmd, &xhci->cmd_list, cmd_list)
		xhci_complete_del_and_free_cmd(cur_cmd, COMP_COMMAND_ABORTED, 0);
}

void xhci_handle_command_timeout(struct work_struct *work)
{
	struct xhci_hcd	*xhci;
	unsigned long	flags;
	char		str[XHCI_MSG_MAX];
	u64		hw_ring_state;
	u32		cmd_field3;
	u32		usbsts;

	xhci = container_of(to_delayed_work(work), struct xhci_hcd, cmd_timer);

	spin_lock_irqsave(&xhci->lock, flags);

	/*
	 * If timeout work is pending, or current_cmd is NULL, it means we
	 * raced with command completion. Command is handled so just return.
	 */
	if (!xhci->current_cmd || delayed_work_pending(&xhci->cmd_timer)) {
		spin_unlock_irqrestore(&xhci->lock, flags);
		return;
	}

	cmd_field3 = le32_to_cpu(xhci->current_cmd->command_trb->generic.field[3]);
	usbsts = readl(&xhci->op_regs->status);
	xhci_dbg(xhci, "Command timeout, USBSTS:%s\n", xhci_decode_usbsts(str, usbsts));

	/* Bail out and tear down xhci if a stop endpoint command failed */
	if (TRB_FIELD_TO_TYPE(cmd_field3) == TRB_STOP_RING) {
		struct xhci_virt_ep	*ep;

		xhci_warn(xhci, "xHCI host not responding to stop endpoint command\n");

		ep = xhci_get_virt_ep(xhci, TRB_TO_SLOT_ID(cmd_field3),
				      TRB_TO_EP_INDEX(cmd_field3));
		if (ep)
			ep->ep_state &= ~EP_STOP_CMD_PENDING;

		xhci_halt(xhci);
		xhci_hc_died(xhci);
		goto time_out_completed;
	}

	/* mark this command to be cancelled */
	xhci->current_cmd->status = COMP_COMMAND_ABORTED;

	/* Make sure command ring is running before aborting it */
	hw_ring_state = xhci_read_64(xhci, &xhci->op_regs->cmd_ring);
	if (hw_ring_state == ~(u64)0) {
		xhci_hc_died(xhci);
		goto time_out_completed;
	}

	if ((xhci->cmd_ring_state & CMD_RING_STATE_RUNNING) &&
	    (hw_ring_state & CMD_RING_RUNNING))  {
		/* Prevent new doorbell, and start command abort */
		xhci->cmd_ring_state = CMD_RING_STATE_ABORTED;
		xhci_dbg(xhci, "Command timeout\n");
		xhci_abort_cmd_ring(xhci, flags);
		goto time_out_completed;
	}

	/* host removed. Bail out */
	if (xhci->xhc_state & XHCI_STATE_REMOVING) {
		xhci_dbg(xhci, "host removed, ring start fail?\n");
		xhci_cleanup_command_queue(xhci);

		goto time_out_completed;
	}

	/* command timeout on stopped ring, ring can't be aborted */
	xhci_dbg(xhci, "Command timeout on stopped ring\n");
	xhci_handle_stopped_cmd_ring(xhci, xhci->current_cmd);

time_out_completed:
	spin_unlock_irqrestore(&xhci->lock, flags);
	return;
}

static void handle_cmd_completion(struct xhci_hcd *xhci,
		struct xhci_event_cmd *event)
{
	unsigned int slot_id = TRB_TO_SLOT_ID(le32_to_cpu(event->flags));
	u32 status = le32_to_cpu(event->status);
	u64 cmd_dma;
	dma_addr_t cmd_dequeue_dma;
	u32 cmd_comp_code;
	union xhci_trb *cmd_trb;
	struct xhci_command *cmd;
	u32 cmd_type;

	if (slot_id >= MAX_HC_SLOTS) {
		xhci_warn(xhci, "Invalid slot_id %u\n", slot_id);
		return;
	}

	cmd_dma = le64_to_cpu(event->cmd_trb);
	cmd_trb = xhci->cmd_ring->dequeue;

	trace_xhci_handle_command(xhci->cmd_ring, &cmd_trb->generic, cmd_dma);

	cmd_comp_code = GET_COMP_CODE(le32_to_cpu(event->status));

	/* If CMD ring stopped we own the trbs between enqueue and dequeue */
	if (cmd_comp_code == COMP_COMMAND_RING_STOPPED) {
		complete_all(&xhci->cmd_ring_stop_completion);
		return;
	}

	cmd_dequeue_dma = xhci_trb_virt_to_dma(xhci->cmd_ring->deq_seg,
			cmd_trb);
	/*
	 * Check whether the completion event is for our internal kept
	 * command.
	 */
	if (!cmd_dequeue_dma || cmd_dma != (u64)cmd_dequeue_dma) {
		xhci_warn(xhci,
			  "ERROR mismatched command completion event\n");
		return;
	}

	cmd = list_first_entry(&xhci->cmd_list, struct xhci_command, cmd_list);

	cancel_delayed_work(&xhci->cmd_timer);

	if (cmd->command_trb != xhci->cmd_ring->dequeue) {
		xhci_err(xhci,
			 "Command completion event does not match command\n");
		return;
	}

	/*
	 * Host aborted the command ring, check if the current command was
	 * supposed to be aborted, otherwise continue normally.
	 * The command ring is stopped now, but the xHC will issue a Command
	 * Ring Stopped event which will cause us to restart it.
	 */
	if (cmd_comp_code == COMP_COMMAND_ABORTED) {
		xhci->cmd_ring_state = CMD_RING_STATE_STOPPED;
		if (cmd->status == COMP_COMMAND_ABORTED) {
			if (xhci->current_cmd == cmd)
				xhci->current_cmd = NULL;
			goto event_handled;
		}
	}

	cmd_type = TRB_FIELD_TO_TYPE(le32_to_cpu(cmd_trb->generic.field[3]));
	switch (cmd_type) {
	case TRB_ENABLE_SLOT:
		xhci_handle_cmd_enable_slot(slot_id, cmd, cmd_comp_code);
		break;
	case TRB_DISABLE_SLOT:
		xhci_handle_cmd_disable_slot(xhci, slot_id);
		break;
	case TRB_CONFIG_EP:
		if (!cmd->completion)
			xhci_handle_cmd_config_ep(xhci, slot_id);
		break;
	case TRB_EVAL_CONTEXT:
		break;
	case TRB_ADDR_DEV:
		xhci_handle_cmd_addr_dev(xhci, slot_id);
		break;
	case TRB_STOP_RING:
		WARN_ON(slot_id != TRB_TO_SLOT_ID(
				le32_to_cpu(cmd_trb->generic.field[3])));
		if (!cmd->completion)
			xhci_handle_cmd_stop_ep(xhci, slot_id, cmd_trb,
						cmd_comp_code);
		break;
	case TRB_SET_DEQ:
		WARN_ON(slot_id != TRB_TO_SLOT_ID(
				le32_to_cpu(cmd_trb->generic.field[3])));
		xhci_handle_cmd_set_deq(xhci, slot_id, cmd_trb, cmd_comp_code);
		break;
	case TRB_CMD_NOOP:
		/* Is this an aborted command turned to NO-OP? */
		if (cmd->status == COMP_COMMAND_RING_STOPPED)
			cmd_comp_code = COMP_COMMAND_RING_STOPPED;
		break;
	case TRB_RESET_EP:
		WARN_ON(slot_id != TRB_TO_SLOT_ID(
				le32_to_cpu(cmd_trb->generic.field[3])));
		xhci_handle_cmd_reset_ep(xhci, slot_id, cmd_trb, cmd_comp_code);
		break;
	case TRB_RESET_DEV:
		/* SLOT_ID field in reset device cmd completion event TRB is 0.
		 * Use the SLOT_ID from the command TRB instead (xhci 4.6.11)
		 */
		slot_id = TRB_TO_SLOT_ID(
				le32_to_cpu(cmd_trb->generic.field[3]));
		xhci_handle_cmd_reset_dev(xhci, slot_id);
		break;
	case TRB_NEC_GET_FW:
		xhci_handle_cmd_nec_get_fw(xhci, event);
		break;
	default:
		/* Skip over unknown commands on the event ring */
		xhci_info(xhci, "INFO unknown command type %d\n", cmd_type);
		break;
	}

	/* restart timer if this wasn't the last command */
	if (!list_is_singular(&xhci->cmd_list)) {
		xhci->current_cmd = list_first_entry(&cmd->cmd_list,
						struct xhci_command, cmd_list);
		xhci_mod_cmd_timer(xhci);
	} else if (xhci->current_cmd == cmd) {
		xhci->current_cmd = NULL;
	}

event_handled:
	xhci_complete_del_and_free_cmd(cmd, cmd_comp_code, COMP_PARAM(status));

	inc_deq(xhci, xhci->cmd_ring);
}

static void handle_vendor_event(struct xhci_hcd *xhci,
				union xhci_trb *event, u32 trb_type)
{
	xhci_dbg(xhci, "Vendor specific event TRB type = %u\n", trb_type);
	if (trb_type == TRB_NEC_CMD_COMP && (xhci->quirks & XHCI_NEC_HOST))
		handle_cmd_completion(xhci, &event->event_cmd);
}

static void handle_device_notification(struct xhci_hcd *xhci,
		union xhci_trb *event)
{
	u32 slot_id;
	struct usb_device *udev;

	slot_id = TRB_TO_SLOT_ID(le32_to_cpu(event->generic.field[3]));
	if (!xhci->devs[slot_id]) {
		xhci_warn(xhci, "Device Notification event for "
				"unused slot %u\n", slot_id);
		return;
	}

	xhci_dbg(xhci, "Device Wake Notification event for slot ID %u\n",
			slot_id);
	udev = xhci->devs[slot_id]->udev;
	if (udev && udev->parent)
		usb_wakeup_notification(udev->parent, udev->portnum);
}

/*
 * Quirk hanlder for errata seen on Cavium ThunderX2 processor XHCI
 * Controller.
 * As per ThunderX2errata-129 USB 2 device may come up as USB 1
 * If a connection to a USB 1 device is followed by another connection
 * to a USB 2 device.
 *
 * Reset the PHY after the USB device is disconnected if device speed
 * is less than HCD_USB3.
 * Retry the reset sequence max of 4 times checking the PLL lock status.
 *
 */
static void xhci_cavium_reset_phy_quirk(struct xhci_hcd *xhci)
{
	struct usb_hcd *hcd = xhci_to_hcd(xhci);
	u32 pll_lock_check;
	u32 retry_count = 4;

	do {
		/* Assert PHY reset */
		writel(0x6F, hcd->regs + 0x1048);
		udelay(10);
		/* De-assert the PHY reset */
		writel(0x7F, hcd->regs + 0x1048);
		udelay(200);
		pll_lock_check = readl(hcd->regs + 0x1070);
	} while (!(pll_lock_check & 0x1) && --retry_count);
}

static void handle_port_status(struct xhci_hcd *xhci, union xhci_trb *event)
{
	struct usb_hcd *hcd;
	u32 port_id;
	u32 portsc, cmd_reg;
	int max_ports;
	unsigned int hcd_portnum;
	struct xhci_bus_state *bus_state;
	bool bogus_port_status = false;
	struct xhci_port *port;

	/* Port status change events always have a successful completion code */
	if (GET_COMP_CODE(le32_to_cpu(event->generic.field[2])) != COMP_SUCCESS)
		xhci_warn(xhci,
			  "WARN: xHC returned failed port status event\n");

	port_id = GET_PORT_ID(le32_to_cpu(event->generic.field[0]));
	max_ports = HCS_MAX_PORTS(xhci->hcs_params1);

	if ((port_id <= 0) || (port_id > max_ports)) {
		xhci_warn(xhci, "Port change event with invalid port ID %d\n",
			  port_id);
		return;
	}

	port = &xhci->hw_ports[port_id - 1];
	if (!port || !port->rhub || port->hcd_portnum == DUPLICATE_ENTRY) {
		xhci_warn(xhci, "Port change event, no port for port ID %u\n",
			  port_id);
		bogus_port_status = true;
		goto cleanup;
	}

	/* We might get interrupts after shared_hcd is removed */
	if (port->rhub == &xhci->usb3_rhub && xhci->shared_hcd == NULL) {
		xhci_dbg(xhci, "ignore port event for removed USB3 hcd\n");
		bogus_port_status = true;
		goto cleanup;
	}

	hcd = port->rhub->hcd;
	bus_state = &port->rhub->bus_state;
	hcd_portnum = port->hcd_portnum;
	portsc = readl(port->addr);

	xhci_dbg(xhci, "Port change event, %d-%d, id %d, portsc: 0x%x\n",
		 hcd->self.busnum, hcd_portnum + 1, port_id, portsc);

	trace_xhci_handle_port_status(port, portsc);

	if (hcd->state == HC_STATE_SUSPENDED) {
		xhci_dbg(xhci, "resume root hub\n");
		usb_hcd_resume_root_hub(hcd);
	}

	if (hcd->speed >= HCD_USB3 &&
	    (portsc & PORT_PLS_MASK) == XDEV_INACTIVE) {
		if (port->slot_id && xhci->devs[port->slot_id])
			xhci->devs[port->slot_id]->flags |= VDEV_PORT_ERROR;
	}

	if ((portsc & PORT_PLC) && (portsc & PORT_PLS_MASK) == XDEV_RESUME) {
		xhci_dbg(xhci, "port resume event for port %d\n", port_id);

		cmd_reg = readl(&xhci->op_regs->command);
		if (!(cmd_reg & CMD_RUN)) {
			xhci_warn(xhci, "xHC is not running.\n");
			goto cleanup;
		}

		if (DEV_SUPERSPEED_ANY(portsc)) {
			xhci_dbg(xhci, "remote wake SS port %d\n", port_id);
			/* Set a flag to say the port signaled remote wakeup,
			 * so we can tell the difference between the end of
			 * device and host initiated resume.
			 */
			bus_state->port_remote_wakeup |= 1 << hcd_portnum;
			xhci_test_and_clear_bit(xhci, port, PORT_PLC);
			usb_hcd_start_port_resume(&hcd->self, hcd_portnum);
			xhci_set_link_state(xhci, port, XDEV_U0);
			/* Need to wait until the next link state change
			 * indicates the device is actually in U0.
			 */
			bogus_port_status = true;
			goto cleanup;
		} else if (!test_bit(hcd_portnum, &bus_state->resuming_ports)) {
			xhci_dbg(xhci, "resume HS port %d\n", port_id);
			port->resume_timestamp = jiffies +
				msecs_to_jiffies(USB_RESUME_TIMEOUT);
			set_bit(hcd_portnum, &bus_state->resuming_ports);
			/* Do the rest in GetPortStatus after resume time delay.
			 * Avoid polling roothub status before that so that a
			 * usb device auto-resume latency around ~40ms.
			 */
			set_bit(HCD_FLAG_POLL_RH, &hcd->flags);
			mod_timer(&hcd->rh_timer,
				  port->resume_timestamp);
			usb_hcd_start_port_resume(&hcd->self, hcd_portnum);
			bogus_port_status = true;
		}
	}

	if ((portsc & PORT_PLC) &&
	    DEV_SUPERSPEED_ANY(portsc) &&
	    ((portsc & PORT_PLS_MASK) == XDEV_U0 ||
	     (portsc & PORT_PLS_MASK) == XDEV_U1 ||
	     (portsc & PORT_PLS_MASK) == XDEV_U2)) {
		xhci_dbg(xhci, "resume SS port %d finished\n", port_id);
		complete(&port->u3exit_done);
		/* We've just brought the device into U0/1/2 through either the
		 * Resume state after a device remote wakeup, or through the
		 * U3Exit state after a host-initiated resume.  If it's a device
		 * initiated remote wake, don't pass up the link state change,
		 * so the roothub behavior is consistent with external
		 * USB 3.0 hub behavior.
		 */
		if (port->slot_id && xhci->devs[port->slot_id])
			xhci_ring_device(xhci, port->slot_id);
		if (bus_state->port_remote_wakeup & (1 << hcd_portnum)) {
			xhci_test_and_clear_bit(xhci, port, PORT_PLC);
			usb_wakeup_notification(hcd->self.root_hub,
					hcd_portnum + 1);
			bogus_port_status = true;
			goto cleanup;
		}
	}

	/*
	 * Check to see if xhci-hub.c is waiting on RExit to U0 transition (or
	 * RExit to a disconnect state).  If so, let the driver know it's
	 * out of the RExit state.
	 */
	if (hcd->speed < HCD_USB3 && port->rexit_active) {
		complete(&port->rexit_done);
		port->rexit_active = false;
		bogus_port_status = true;
		goto cleanup;
	}

	if (hcd->speed < HCD_USB3) {
		xhci_test_and_clear_bit(xhci, port, PORT_PLC);
		if ((xhci->quirks & XHCI_RESET_PLL_ON_DISCONNECT) &&
		    (portsc & PORT_CSC) && !(portsc & PORT_CONNECT))
			xhci_cavium_reset_phy_quirk(xhci);
	}

cleanup:

	/* Don't make the USB core poll the roothub if we got a bad port status
	 * change event.  Besides, at that point we can't tell which roothub
	 * (USB 2.0 or USB 3.0) to kick.
	 */
	if (bogus_port_status)
		return;

	/*
	 * xHCI port-status-change events occur when the "or" of all the
	 * status-change bits in the portsc register changes from 0 to 1.
	 * New status changes won't cause an event if any other change
	 * bits are still set.  When an event occurs, switch over to
	 * polling to avoid losing status changes.
	 */
	xhci_dbg(xhci, "%s: starting usb%d port polling.\n",
		 __func__, hcd->self.busnum);
	set_bit(HCD_FLAG_POLL_RH, &hcd->flags);
	spin_unlock(&xhci->lock);
	/* Pass this up to the core */
	usb_hcd_poll_rh_status(hcd);
	spin_lock(&xhci->lock);
}

static void xhci_clear_hub_tt_buffer(struct xhci_hcd *xhci, struct xhci_td *td,
		struct xhci_virt_ep *ep)
{
	/*
	 * As part of low/full-speed endpoint-halt processing
	 * we must clear the TT buffer (USB 2.0 specification 11.17.5).
	 */
	if (td->urb->dev->tt && !usb_pipeint(td->urb->pipe) &&
	    (td->urb->dev->tt->hub != xhci_to_hcd(xhci)->self.root_hub) &&
	    !(ep->ep_state & EP_CLEARING_TT)) {
		ep->ep_state |= EP_CLEARING_TT;
		td->urb->ep->hcpriv = td->urb->dev;
		if (usb_hub_clear_tt_buffer(td->urb))
			ep->ep_state &= ~EP_CLEARING_TT;
	}
}

/*
 * Check if xhci internal endpoint state has gone to a "halt" state due to an
 * error or stall, including default control pipe protocol stall.
 * The internal halt needs to be cleared with a reset endpoint command.
 *
 * External device side is also halted in functional stall cases. Class driver
 * will clear the device halt with a CLEAR_FEATURE(ENDPOINT_HALT) request later.
 */
static bool xhci_halted_host_endpoint(struct xhci_ep_ctx *ep_ctx, unsigned int comp_code)
{
	/* Stall halts both internal and device side endpoint */
	if (comp_code == COMP_STALL_ERROR)
		return true;

	/* TRB completion codes that may require internal halt cleanup */
	if (comp_code == COMP_USB_TRANSACTION_ERROR ||
	    comp_code == COMP_BABBLE_DETECTED_ERROR ||
	    comp_code == COMP_SPLIT_TRANSACTION_ERROR)
		/*
		 * The 0.95 spec says a babbling control endpoint is not halted.
		 * The 0.96 spec says it is. Some HW claims to be 0.95
		 * compliant, but it halts the control endpoint anyway.
		 * Check endpoint context if endpoint is halted.
		 */
		if (GET_EP_CTX_STATE(ep_ctx) == EP_STATE_HALTED)
			return true;

	return false;
}

int xhci_is_vendor_info_code(struct xhci_hcd *xhci, unsigned int trb_comp_code)
{
	if (trb_comp_code >= 224 && trb_comp_code <= 255) {
		/* Vendor defined "informational" completion code,
		 * treat as not-an-error.
		 */
		xhci_dbg(xhci, "Vendor defined info completion code %u\n",
				trb_comp_code);
		xhci_dbg(xhci, "Treating code as success.\n");
		return 1;
	}
	return 0;
}

static void finish_td(struct xhci_hcd *xhci, struct xhci_virt_ep *ep,
		      struct xhci_ring *ep_ring, struct xhci_td *td,
		      u32 trb_comp_code)
{
	struct xhci_ep_ctx *ep_ctx;

	ep_ctx = xhci_get_ep_ctx(xhci, ep->vdev->out_ctx, ep->ep_index);

	switch (trb_comp_code) {
	case COMP_STOPPED_LENGTH_INVALID:
	case COMP_STOPPED_SHORT_PACKET:
	case COMP_STOPPED:
		/*
		 * The "Stop Endpoint" completion will take care of any
		 * stopped TDs. A stopped TD may be restarted, so don't update
		 * the ring dequeue pointer or take this TD off any lists yet.
		 */
		return;
	case COMP_USB_TRANSACTION_ERROR:
	case COMP_BABBLE_DETECTED_ERROR:
	case COMP_SPLIT_TRANSACTION_ERROR:
		/*
		 * If endpoint context state is not halted we might be
		 * racing with a reset endpoint command issued by a unsuccessful
		 * stop endpoint completion (context error). In that case the
		 * td should be on the cancelled list, and EP_HALTED flag set.
		 *
		 * Or then it's not halted due to the 0.95 spec stating that a
		 * babbling control endpoint should not halt. The 0.96 spec
		 * again says it should.  Some HW claims to be 0.95 compliant,
		 * but it halts the control endpoint anyway.
		 */
		if (GET_EP_CTX_STATE(ep_ctx) != EP_STATE_HALTED) {
			/*
			 * If EP_HALTED is set and TD is on the cancelled list
			 * the TD and dequeue pointer will be handled by reset
			 * ep command completion
			 */
			if ((ep->ep_state & EP_HALTED) &&
			    !list_empty(&td->cancelled_td_list)) {
				xhci_dbg(xhci, "Already resolving halted ep for 0x%llx\n",
					 (unsigned long long)xhci_trb_virt_to_dma(
						 td->start_seg, td->start_trb));
				return;
			}
			/* endpoint not halted, don't reset it */
			break;
		}
		/* Almost same procedure as for STALL_ERROR below */
		xhci_clear_hub_tt_buffer(xhci, td, ep);
		xhci_handle_halted_endpoint(xhci, ep, td, EP_HARD_RESET);
		return;
	case COMP_STALL_ERROR:
		/*
		 * xhci internal endpoint state will go to a "halt" state for
		 * any stall, including default control pipe protocol stall.
		 * To clear the host side halt we need to issue a reset endpoint
		 * command, followed by a set dequeue command to move past the
		 * TD.
		 * Class drivers clear the device side halt from a functional
		 * stall later. Hub TT buffer should only be cleared for FS/LS
		 * devices behind HS hubs for functional stalls.
		 */
		if (ep->ep_index != 0)
			xhci_clear_hub_tt_buffer(xhci, td, ep);

		xhci_handle_halted_endpoint(xhci, ep, td, EP_HARD_RESET);

		return; /* xhci_handle_halted_endpoint marked td cancelled */
	default:
		break;
	}

	xhci_dequeue_td(xhci, td, ep_ring, td->status);
}

/* sum trb lengths from the first trb up to stop_trb, _excluding_ stop_trb */
static u32 sum_trb_lengths(struct xhci_td *td, union xhci_trb *stop_trb)
{
	u32 sum;
	union xhci_trb *trb = td->start_trb;
	struct xhci_segment *seg = td->start_seg;

	for (sum = 0; trb != stop_trb; next_trb(&seg, &trb)) {
		if (!trb_is_noop(trb) && !trb_is_link(trb))
			sum += TRB_LEN(le32_to_cpu(trb->generic.field[2]));
	}
	return sum;
}

/*
 * Process control tds, update urb status and actual_length.
 */
static void process_ctrl_td(struct xhci_hcd *xhci, struct xhci_virt_ep *ep,
			    struct xhci_ring *ep_ring,  struct xhci_td *td,
			    union xhci_trb *ep_trb, struct xhci_transfer_event *event)
{
	struct xhci_ep_ctx *ep_ctx;
	u32 trb_comp_code;
	u32 remaining, requested;
	u32 trb_type;

	trb_type = TRB_FIELD_TO_TYPE(le32_to_cpu(ep_trb->generic.field[3]));
	ep_ctx = xhci_get_ep_ctx(xhci, ep->vdev->out_ctx, ep->ep_index);
	trb_comp_code = GET_COMP_CODE(le32_to_cpu(event->transfer_len));
	requested = td->urb->transfer_buffer_length;
	remaining = EVENT_TRB_LEN(le32_to_cpu(event->transfer_len));

	switch (trb_comp_code) {
	case COMP_SUCCESS:
		if (trb_type != TRB_STATUS) {
			xhci_warn(xhci, "WARN: Success on ctrl %s TRB without IOC set?\n",
				  (trb_type == TRB_DATA) ? "data" : "setup");
			td->status = -ESHUTDOWN;
			break;
		}
		td->status = 0;
		break;
	case COMP_SHORT_PACKET:
		td->status = 0;
		break;
	case COMP_STOPPED_SHORT_PACKET:
		if (trb_type == TRB_DATA || trb_type == TRB_NORMAL)
			td->urb->actual_length = remaining;
		else
			xhci_warn(xhci, "WARN: Stopped Short Packet on ctrl setup or status TRB\n");
		goto finish_td;
	case COMP_STOPPED:
		switch (trb_type) {
		case TRB_SETUP:
			td->urb->actual_length = 0;
			goto finish_td;
		case TRB_DATA:
		case TRB_NORMAL:
			td->urb->actual_length = requested - remaining;
			goto finish_td;
		case TRB_STATUS:
			td->urb->actual_length = requested;
			goto finish_td;
		default:
			xhci_warn(xhci, "WARN: unexpected TRB Type %d\n",
				  trb_type);
			goto finish_td;
		}
	case COMP_STOPPED_LENGTH_INVALID:
		goto finish_td;
	default:
		if (!xhci_halted_host_endpoint(ep_ctx, trb_comp_code))
			break;
		xhci_dbg(xhci, "TRB error %u, halted endpoint index = %u\n",
			 trb_comp_code, ep->ep_index);
		fallthrough;
	case COMP_STALL_ERROR:
		/* Did we transfer part of the data (middle) phase? */
		if (trb_type == TRB_DATA || trb_type == TRB_NORMAL)
			td->urb->actual_length = requested - remaining;
		else if (!td->urb_length_set)
			td->urb->actual_length = 0;
		goto finish_td;
	}

	/* stopped at setup stage, no data transferred */
	if (trb_type == TRB_SETUP)
		goto finish_td;

	/*
	 * if on data stage then update the actual_length of the URB and flag it
	 * as set, so it won't be overwritten in the event for the last TRB.
	 */
	if (trb_type == TRB_DATA ||
		trb_type == TRB_NORMAL) {
		td->urb_length_set = true;
		td->urb->actual_length = requested - remaining;
		xhci_dbg(xhci, "Waiting for status stage event\n");
		return;
	}

	/* at status stage */
	if (!td->urb_length_set)
		td->urb->actual_length = requested;

finish_td:
	finish_td(xhci, ep, ep_ring, td, trb_comp_code);
}

/*
 * Process isochronous tds, update urb packet status and actual_length.
 */
static void process_isoc_td(struct xhci_hcd *xhci, struct xhci_virt_ep *ep,
			    struct xhci_ring *ep_ring, struct xhci_td *td,
			    union xhci_trb *ep_trb, struct xhci_transfer_event *event)
{
	struct urb_priv *urb_priv;
	int idx;
	struct usb_iso_packet_descriptor *frame;
	u32 trb_comp_code;
	bool sum_trbs_for_length = false;
	u32 remaining, requested, ep_trb_len;
	int short_framestatus;

	trb_comp_code = GET_COMP_CODE(le32_to_cpu(event->transfer_len));
	urb_priv = td->urb->hcpriv;
	idx = urb_priv->num_tds_done;
	frame = &td->urb->iso_frame_desc[idx];
	requested = frame->length;
	remaining = EVENT_TRB_LEN(le32_to_cpu(event->transfer_len));
	ep_trb_len = TRB_LEN(le32_to_cpu(ep_trb->generic.field[2]));
	short_framestatus = td->urb->transfer_flags & URB_SHORT_NOT_OK ?
		-EREMOTEIO : 0;

	/* handle completion code */
	switch (trb_comp_code) {
	case COMP_SUCCESS:
		/* Don't overwrite status if TD had an error, see xHCI 4.9.1 */
		if (td->error_mid_td)
			break;
		if (remaining) {
			frame->status = short_framestatus;
			sum_trbs_for_length = true;
			break;
		}
		frame->status = 0;
		break;
	case COMP_SHORT_PACKET:
		frame->status = short_framestatus;
		sum_trbs_for_length = true;
		break;
	case COMP_BANDWIDTH_OVERRUN_ERROR:
		frame->status = -ECOMM;
		break;
	case COMP_BABBLE_DETECTED_ERROR:
		sum_trbs_for_length = true;
		fallthrough;
	case COMP_ISOCH_BUFFER_OVERRUN:
		frame->status = -EOVERFLOW;
		if (ep_trb != td->end_trb)
			td->error_mid_td = true;
		break;
	case COMP_MISSED_SERVICE_ERROR:
		frame->status = -EXDEV;
		sum_trbs_for_length = true;
		if (ep_trb != td->end_trb)
			td->error_mid_td = true;
		break;
	case COMP_INCOMPATIBLE_DEVICE_ERROR:
	case COMP_STALL_ERROR:
		frame->status = -EPROTO;
		break;
	case COMP_USB_TRANSACTION_ERROR:
		frame->status = -EPROTO;
		sum_trbs_for_length = true;
		if (ep_trb != td->end_trb)
			td->error_mid_td = true;
		break;
	case COMP_STOPPED:
		sum_trbs_for_length = true;
		break;
	case COMP_STOPPED_SHORT_PACKET:
		/* field normally containing residue now contains transferred */
		frame->status = short_framestatus;
		requested = remaining;
		break;
	case COMP_STOPPED_LENGTH_INVALID:
		/* exclude stopped trb with invalid length from length sum */
		sum_trbs_for_length = true;
		ep_trb_len = 0;
		remaining = 0;
		break;
	default:
		sum_trbs_for_length = true;
		frame->status = -1;
		break;
	}

	if (td->urb_length_set)
		goto finish_td;

	if (sum_trbs_for_length)
		frame->actual_length = sum_trb_lengths(td, ep_trb) +
			ep_trb_len - remaining;
	else
		frame->actual_length = requested;

	td->urb->actual_length += frame->actual_length;

finish_td:
	/* Don't give back TD yet if we encountered an error mid TD */
	if (td->error_mid_td && ep_trb != td->end_trb) {
		xhci_dbg(xhci, "Error mid isoc TD, wait for final completion event\n");
		td->urb_length_set = true;
		return;
	}
	finish_td(xhci, ep, ep_ring, td, trb_comp_code);
}

static void skip_isoc_td(struct xhci_hcd *xhci, struct xhci_td *td,
			 struct xhci_virt_ep *ep, int status)
{
	struct urb_priv *urb_priv;
	struct usb_iso_packet_descriptor *frame;
	int idx;

	urb_priv = td->urb->hcpriv;
	idx = urb_priv->num_tds_done;
	frame = &td->urb->iso_frame_desc[idx];

	/* The transfer is partly done. */
	frame->status = -EXDEV;

	/* calc actual length */
	frame->actual_length = 0;

	xhci_dequeue_td(xhci, td, ep->ring, status);
}

/*
 * Process bulk and interrupt tds, update urb status and actual_length.
 */
static void process_bulk_intr_td(struct xhci_hcd *xhci, struct xhci_virt_ep *ep,
				 struct xhci_ring *ep_ring, struct xhci_td *td,
				 union xhci_trb *ep_trb, struct xhci_transfer_event *event)
{
	struct xhci_slot_ctx *slot_ctx;
	u32 trb_comp_code;
	u32 remaining, requested, ep_trb_len;

	slot_ctx = xhci_get_slot_ctx(xhci, ep->vdev->out_ctx);
	trb_comp_code = GET_COMP_CODE(le32_to_cpu(event->transfer_len));
	remaining = EVENT_TRB_LEN(le32_to_cpu(event->transfer_len));
	ep_trb_len = TRB_LEN(le32_to_cpu(ep_trb->generic.field[2]));
	requested = td->urb->transfer_buffer_length;

	switch (trb_comp_code) {
	case COMP_SUCCESS:
		ep->err_count = 0;
		/* handle success with untransferred data as short packet */
		if (ep_trb != td->end_trb || remaining) {
			xhci_warn(xhci, "WARN Successful completion on short TX\n");
			xhci_dbg(xhci, "ep %#x - asked for %d bytes, %d bytes untransferred\n",
				 td->urb->ep->desc.bEndpointAddress,
				 requested, remaining);
		}
		td->status = 0;
		break;
	case COMP_SHORT_PACKET:
		td->status = 0;
		break;
	case COMP_STOPPED_SHORT_PACKET:
		td->urb->actual_length = remaining;
		goto finish_td;
	case COMP_STOPPED_LENGTH_INVALID:
		/* stopped on ep trb with invalid length, exclude it */
		td->urb->actual_length = sum_trb_lengths(td, ep_trb);
		goto finish_td;
	case COMP_USB_TRANSACTION_ERROR:
		if (xhci->quirks & XHCI_NO_SOFT_RETRY ||
		    (ep->err_count++ > MAX_SOFT_RETRY) ||
		    le32_to_cpu(slot_ctx->tt_info) & TT_SLOT)
			break;

		td->status = 0;

		xhci_handle_halted_endpoint(xhci, ep, td, EP_SOFT_RESET);
		return;
	default:
		/* do nothing */
		break;
	}

	if (ep_trb == td->end_trb)
		td->urb->actual_length = requested - remaining;
	else
		td->urb->actual_length =
			sum_trb_lengths(td, ep_trb) +
			ep_trb_len - remaining;
finish_td:
	if (remaining > requested) {
		xhci_warn(xhci, "bad transfer trb length %d in event trb\n",
			  remaining);
		td->urb->actual_length = 0;
	}

	finish_td(xhci, ep, ep_ring, td, trb_comp_code);
}

/* Transfer events which don't point to a transfer TRB, see xhci 4.17.4 */
static int handle_transferless_tx_event(struct xhci_hcd *xhci, struct xhci_virt_ep *ep,
					u32 trb_comp_code)
{
	switch (trb_comp_code) {
	case COMP_STALL_ERROR:
	case COMP_USB_TRANSACTION_ERROR:
	case COMP_INVALID_STREAM_TYPE_ERROR:
	case COMP_INVALID_STREAM_ID_ERROR:
		xhci_dbg(xhci, "Stream transaction error ep %u no id\n", ep->ep_index);
		if (ep->err_count++ > MAX_SOFT_RETRY)
			xhci_handle_halted_endpoint(xhci, ep, NULL, EP_HARD_RESET);
		else
			xhci_handle_halted_endpoint(xhci, ep, NULL, EP_SOFT_RESET);
		break;
	case COMP_RING_UNDERRUN:
	case COMP_RING_OVERRUN:
	case COMP_STOPPED_LENGTH_INVALID:
		break;
	default:
		xhci_err(xhci, "Transfer event %u for unknown stream ring slot %u ep %u\n",
			 trb_comp_code, ep->vdev->slot_id, ep->ep_index);
		return -ENODEV;
	}
	return 0;
}

static bool xhci_spurious_success_tx_event(struct xhci_hcd *xhci,
					   struct xhci_ring *ring)
{
	switch (ring->old_trb_comp_code) {
	case COMP_SHORT_PACKET:
		return xhci->quirks & XHCI_SPURIOUS_SUCCESS;
	case COMP_USB_TRANSACTION_ERROR:
	case COMP_BABBLE_DETECTED_ERROR:
	case COMP_ISOCH_BUFFER_OVERRUN:
		return xhci->quirks & XHCI_ETRON_HOST &&
			ring->type == TYPE_ISOC;
	default:
		return false;
	}
}

/*
 * If this function returns an error condition, it means it got a Transfer
 * event with a corrupted Slot ID, Endpoint ID, or TRB DMA address.
 * At this point, the host controller is probably hosed and should be reset.
 */
static int handle_tx_event(struct xhci_hcd *xhci,
			   struct xhci_interrupter *ir,
			   struct xhci_transfer_event *event)
{
	struct xhci_virt_ep *ep;
	struct xhci_ring *ep_ring;
	unsigned int slot_id;
	int ep_index;
	struct xhci_td *td = NULL;
	dma_addr_t ep_trb_dma;
	struct xhci_segment *ep_seg;
	union xhci_trb *ep_trb;
	int status = -EINPROGRESS;
	struct xhci_ep_ctx *ep_ctx;
	u32 trb_comp_code;
	bool ring_xrun_event = false;

	slot_id = TRB_TO_SLOT_ID(le32_to_cpu(event->flags));
	ep_index = TRB_TO_EP_ID(le32_to_cpu(event->flags)) - 1;
	trb_comp_code = GET_COMP_CODE(le32_to_cpu(event->transfer_len));
	ep_trb_dma = le64_to_cpu(event->buffer);

	ep = xhci_get_virt_ep(xhci, slot_id, ep_index);
	if (!ep) {
		xhci_err(xhci, "ERROR Invalid Transfer event\n");
		goto err_out;
	}

	ep_ring = xhci_dma_to_transfer_ring(ep, ep_trb_dma);
	ep_ctx = xhci_get_ep_ctx(xhci, ep->vdev->out_ctx, ep_index);

	if (GET_EP_CTX_STATE(ep_ctx) == EP_STATE_DISABLED) {
		xhci_err(xhci,
			 "ERROR Transfer event for disabled endpoint slot %u ep %u\n",
			  slot_id, ep_index);
		goto err_out;
	}

	if (!ep_ring)
		return handle_transferless_tx_event(xhci, ep, trb_comp_code);

	/* Look for common error cases */
	switch (trb_comp_code) {
	/* Skip codes that require special handling depending on
	 * transfer type
	 */
	case COMP_SUCCESS:
		if (EVENT_TRB_LEN(le32_to_cpu(event->transfer_len)) != 0) {
			trb_comp_code = COMP_SHORT_PACKET;
			xhci_dbg(xhci, "Successful completion on short TX for slot %u ep %u with last td comp code %d\n",
				 slot_id, ep_index, ep_ring->old_trb_comp_code);
		}
		break;
	case COMP_SHORT_PACKET:
		break;
	/* Completion codes for endpoint stopped state */
	case COMP_STOPPED:
		xhci_dbg(xhci, "Stopped on Transfer TRB for slot %u ep %u\n",
			 slot_id, ep_index);
		break;
	case COMP_STOPPED_LENGTH_INVALID:
		xhci_dbg(xhci,
			 "Stopped on No-op or Link TRB for slot %u ep %u\n",
			 slot_id, ep_index);
		break;
	case COMP_STOPPED_SHORT_PACKET:
		xhci_dbg(xhci,
			 "Stopped with short packet transfer detected for slot %u ep %u\n",
			 slot_id, ep_index);
		break;
	/* Completion codes for endpoint halted state */
	case COMP_STALL_ERROR:
		xhci_dbg(xhci, "Stalled endpoint for slot %u ep %u\n", slot_id,
			 ep_index);
		status = -EPIPE;
		break;
	case COMP_SPLIT_TRANSACTION_ERROR:
		xhci_dbg(xhci, "Split transaction error for slot %u ep %u\n",
			 slot_id, ep_index);
		status = -EPROTO;
		break;
	case COMP_USB_TRANSACTION_ERROR:
		xhci_dbg(xhci, "Transfer error for slot %u ep %u on endpoint\n",
			 slot_id, ep_index);
		status = -EPROTO;
		break;
	case COMP_BABBLE_DETECTED_ERROR:
		xhci_dbg(xhci, "Babble error for slot %u ep %u on endpoint\n",
			 slot_id, ep_index);
		status = -EOVERFLOW;
		break;
	/* Completion codes for endpoint error state */
	case COMP_TRB_ERROR:
		xhci_warn(xhci,
			  "WARN: TRB error for slot %u ep %u on endpoint\n",
			  slot_id, ep_index);
		status = -EILSEQ;
		break;
	/* completion codes not indicating endpoint state change */
	case COMP_DATA_BUFFER_ERROR:
		xhci_warn(xhci,
			  "WARN: HC couldn't access mem fast enough for slot %u ep %u\n",
			  slot_id, ep_index);
		status = -ENOSR;
		break;
	case COMP_BANDWIDTH_OVERRUN_ERROR:
		xhci_warn(xhci,
			  "WARN: bandwidth overrun event for slot %u ep %u on endpoint\n",
			  slot_id, ep_index);
		break;
	case COMP_ISOCH_BUFFER_OVERRUN:
		xhci_warn(xhci,
			  "WARN: buffer overrun event for slot %u ep %u on endpoint",
			  slot_id, ep_index);
		break;
	case COMP_RING_UNDERRUN:
		/*
		 * When the Isoch ring is empty, the xHC will generate
		 * a Ring Overrun Event for IN Isoch endpoint or Ring
		 * Underrun Event for OUT Isoch endpoint.
		 */
		xhci_dbg(xhci, "Underrun event on slot %u ep %u\n", slot_id, ep_index);
		ring_xrun_event = true;
		break;
	case COMP_RING_OVERRUN:
		xhci_dbg(xhci, "Overrun event on slot %u ep %u\n", slot_id, ep_index);
		ring_xrun_event = true;
		break;
	case COMP_MISSED_SERVICE_ERROR:
		/*
		 * When encounter missed service error, one or more isoc tds
		 * may be missed by xHC.
		 * Set skip flag of the ep_ring; Complete the missed tds as
		 * short transfer when process the ep_ring next time.
		 */
		ep->skip = true;
		xhci_dbg(xhci,
			 "Miss service interval error for slot %u ep %u, set skip flag%s\n",
			 slot_id, ep_index, ep_trb_dma ? ", skip now" : "");
		break;
	case COMP_NO_PING_RESPONSE_ERROR:
		ep->skip = true;
		xhci_dbg(xhci,
			 "No Ping response error for slot %u ep %u, Skip one Isoc TD\n",
			 slot_id, ep_index);
		return 0;

	case COMP_INCOMPATIBLE_DEVICE_ERROR:
		/* needs disable slot command to recover */
		xhci_warn(xhci,
			  "WARN: detect an incompatible device for slot %u ep %u",
			  slot_id, ep_index);
		status = -EPROTO;
		break;
	default:
		if (xhci_is_vendor_info_code(xhci, trb_comp_code)) {
			status = 0;
			break;
		}
		xhci_warn(xhci,
			  "ERROR Unknown event condition %u for slot %u ep %u , HC probably busted\n",
			  trb_comp_code, slot_id, ep_index);
		if (ep->skip)
			break;
		return 0;
	}

	/*
	 * xhci 4.10.2 states isoc endpoints should continue
	 * processing the next TD if there was an error mid TD.
	 * So host like NEC don't generate an event for the last
	 * isoc TRB even if the IOC flag is set.
	 * xhci 4.9.1 states that if there are errors in mult-TRB
	 * TDs xHC should generate an error for that TRB, and if xHC
	 * proceeds to the next TD it should genete an event for
	 * any TRB with IOC flag on the way. Other host follow this.
	 *
	 * We wait for the final IOC event, but if we get an event
	 * anywhere outside this TD, just give it back already.
	 */
	td = list_first_entry_or_null(&ep_ring->td_list, struct xhci_td, td_list);

	if (td && td->error_mid_td && !trb_in_td(td, ep_trb_dma)) {
		xhci_dbg(xhci, "Missing TD completion event after mid TD error\n");
		xhci_dequeue_td(xhci, td, ep_ring, td->status);
	}

	/* If the TRB pointer is NULL, missed TDs will be skipped on the next event */
	if (trb_comp_code == COMP_MISSED_SERVICE_ERROR && !ep_trb_dma)
		return 0;

	if (list_empty(&ep_ring->td_list)) {
		/*
		 * Don't print wanings if ring is empty due to a stopped endpoint generating an
		 * extra completion event if the device was suspended. Or, a event for the last TRB
		 * of a short TD we already got a short event for. The short TD is already removed
		 * from the TD list.
		 */
		if (trb_comp_code != COMP_STOPPED &&
		    trb_comp_code != COMP_STOPPED_LENGTH_INVALID &&
		    !ring_xrun_event &&
		    !xhci_spurious_success_tx_event(xhci, ep_ring)) {
			xhci_warn(xhci, "Event TRB for slot %u ep %u with no TDs queued\n",
				  slot_id, ep_index);
		}

		ep->skip = false;
		goto check_endpoint_halted;
	}

	do {
		td = list_first_entry(&ep_ring->td_list, struct xhci_td,
				      td_list);

		/* Is this a TRB in the currently executing TD? */
		ep_seg = trb_in_td(td, ep_trb_dma);

		if (!ep_seg) {

			if (ep->skip && usb_endpoint_xfer_isoc(&td->urb->ep->desc)) {
				/* this event is unlikely to match any TD, don't skip them all */
				if (trb_comp_code == COMP_STOPPED_LENGTH_INVALID)
					return 0;

				skip_isoc_td(xhci, td, ep, status);

				if (!list_empty(&ep_ring->td_list)) {
					if (ring_xrun_event) {
						/*
						 * If we are here, we are on xHCI 1.0 host with no
						 * idea how many TDs were missed or where the xrun
						 * occurred. New TDs may have been added after the
						 * xrun, so skip only one TD to be safe.
						 */
						xhci_dbg(xhci, "Skipped one TD for slot %u ep %u",
								slot_id, ep_index);
						return 0;
					}
					continue;
				}

				xhci_dbg(xhci, "All TDs skipped for slot %u ep %u. Clear skip flag.\n",
					 slot_id, ep_index);
				ep->skip = false;
				td = NULL;
				goto check_endpoint_halted;
			}

			/* TD was queued after xrun, maybe xrun was on a link, don't panic yet */
			if (ring_xrun_event)
				return 0;

			/*
			 * Skip the Force Stopped Event. The 'ep_trb' of FSE is not in the current
			 * TD pointed by 'ep_ring->dequeue' because that the hardware dequeue
			 * pointer still at the previous TRB of the current TD. The previous TRB
			 * maybe a Link TD or the last TRB of the previous TD. The command
			 * completion handle will take care the rest.
			 */
			if (trb_comp_code == COMP_STOPPED ||
			    trb_comp_code == COMP_STOPPED_LENGTH_INVALID) {
				return 0;
			}

			/*
			 * Some hosts give a spurious success event after a short
			 * transfer or error on last TRB. Ignore it.
			 */
			if (xhci_spurious_success_tx_event(xhci, ep_ring)) {
				xhci_dbg(xhci, "Spurious event dma %pad, comp_code %u after %u\n",
					 &ep_trb_dma, trb_comp_code, ep_ring->old_trb_comp_code);
				ep_ring->old_trb_comp_code = 0;
				return 0;
			}

			/* HC is busted, give up! */
			goto debug_finding_td;
		}

		if (ep->skip) {
			xhci_dbg(xhci,
				 "Found td. Clear skip flag for slot %u ep %u.\n",
				 slot_id, ep_index);
			ep->skip = false;
		}

	/*
	 * If ep->skip is set, it means there are missed tds on the
	 * endpoint ring need to take care of.
	 * Process them as short transfer until reach the td pointed by
	 * the event.
	 */
	} while (ep->skip);

	ep_ring->old_trb_comp_code = trb_comp_code;

	/* Get out if a TD was queued at enqueue after the xrun occurred */
	if (ring_xrun_event)
		return 0;

	ep_trb = &ep_seg->trbs[(ep_trb_dma - ep_seg->dma) / sizeof(*ep_trb)];
	trace_xhci_handle_transfer(ep_ring, (struct xhci_generic_trb *) ep_trb, ep_trb_dma);

	/*
	 * No-op TRB could trigger interrupts in a case where a URB was killed
	 * and a STALL_ERROR happens right after the endpoint ring stopped.
	 * Reset the halted endpoint. Otherwise, the endpoint remains stalled
	 * indefinitely.
	 */

	if (trb_is_noop(ep_trb))
		goto check_endpoint_halted;

	td->status = status;

	/* update the urb's actual_length and give back to the core */
	if (usb_endpoint_xfer_control(&td->urb->ep->desc))
		process_ctrl_td(xhci, ep, ep_ring, td, ep_trb, event);
	else if (usb_endpoint_xfer_isoc(&td->urb->ep->desc))
		process_isoc_td(xhci, ep, ep_ring, td, ep_trb, event);
	else
		process_bulk_intr_td(xhci, ep, ep_ring, td, ep_trb, event);
	return 0;

check_endpoint_halted:
	if (xhci_halted_host_endpoint(ep_ctx, trb_comp_code))
		xhci_handle_halted_endpoint(xhci, ep, td, EP_HARD_RESET);

	return 0;

debug_finding_td:
	xhci_err(xhci, "Event dma %pad for ep %d status %d not part of TD at %016llx - %016llx\n",
		 &ep_trb_dma, ep_index, trb_comp_code,
		 (unsigned long long)xhci_trb_virt_to_dma(td->start_seg, td->start_trb),
		 (unsigned long long)xhci_trb_virt_to_dma(td->end_seg, td->end_trb));

	xhci_for_each_ring_seg(ep_ring->first_seg, ep_seg)
		xhci_warn(xhci, "Ring seg %u dma %pad\n", ep_seg->num, &ep_seg->dma);

	return -ESHUTDOWN;

err_out:
	xhci_err(xhci, "@%016llx %08x %08x %08x %08x\n",
		 (unsigned long long) xhci_trb_virt_to_dma(
			 ir->event_ring->deq_seg,
			 ir->event_ring->dequeue),
		 lower_32_bits(le64_to_cpu(event->buffer)),
		 upper_32_bits(le64_to_cpu(event->buffer)),
		 le32_to_cpu(event->transfer_len),
		 le32_to_cpu(event->flags));
	return -ENODEV;
}

/*
 * This function handles one OS-owned event on the event ring. It may drop
 * xhci->lock between event processing (e.g. to pass up port status changes).
 */
static int xhci_handle_event_trb(struct xhci_hcd *xhci, struct xhci_interrupter *ir,
				 union xhci_trb *event)
{
	u32 trb_type;

	trace_xhci_handle_event(ir->event_ring, &event->generic,
				xhci_trb_virt_to_dma(ir->event_ring->deq_seg,
						     ir->event_ring->dequeue));

	/*
	 * Barrier between reading the TRB_CYCLE (valid) flag before, and any
	 * speculative reads of the event's flags/data below.
	 */
	rmb();
	trb_type = TRB_FIELD_TO_TYPE(le32_to_cpu(event->event_cmd.flags));
	/* FIXME: Handle more event types. */

	switch (trb_type) {
	case TRB_COMPLETION:
		handle_cmd_completion(xhci, &event->event_cmd);
		break;
	case TRB_PORT_STATUS:
		handle_port_status(xhci, event);
		break;
	case TRB_TRANSFER:
		handle_tx_event(xhci, ir, &event->trans_event);
		break;
	case TRB_DEV_NOTE:
		handle_device_notification(xhci, event);
		break;
	default:
		if (trb_type >= TRB_VENDOR_DEFINED_LOW)
			handle_vendor_event(xhci, event, trb_type);
		else
			xhci_warn(xhci, "ERROR unknown event type %d\n", trb_type);
	}
	/* Any of the above functions may drop and re-acquire the lock, so check
	 * to make sure a watchdog timer didn't mark the host as non-responsive.
	 */
	if (xhci->xhc_state & XHCI_STATE_DYING) {
		xhci_dbg(xhci, "xHCI host dying, returning from event handler.\n");
		return -ENODEV;
	}

	return 0;
}

/*
 * Update Event Ring Dequeue Pointer:
 * - When all events have finished
 * - To avoid "Event Ring Full Error" condition
 */
static void xhci_update_erst_dequeue(struct xhci_hcd *xhci,
				     struct xhci_interrupter *ir,
				     bool clear_ehb)
{
	u64 temp_64;
	dma_addr_t deq;

	temp_64 = xhci_read_64(xhci, &ir->ir_set->erst_dequeue);
	deq = xhci_trb_virt_to_dma(ir->event_ring->deq_seg,
				   ir->event_ring->dequeue);
	if (deq == 0)
		xhci_warn(xhci, "WARN something wrong with SW event ring dequeue ptr\n");
	/*
	 * Per 4.9.4, Software writes to the ERDP register shall always advance
	 * the Event Ring Dequeue Pointer value.
	 */
	if ((temp_64 & ERST_PTR_MASK) == (deq & ERST_PTR_MASK) && !clear_ehb)
		return;

	/* Update HC event ring dequeue pointer */
	temp_64 = ir->event_ring->deq_seg->num & ERST_DESI_MASK;
	temp_64 |= deq & ERST_PTR_MASK;

	/* Clear the event handler busy flag (RW1C) */
	if (clear_ehb)
		temp_64 |= ERST_EHB;
	xhci_write_64(xhci, temp_64, &ir->ir_set->erst_dequeue);
}

/* Clear the interrupt pending bit for a specific interrupter. */
static void xhci_clear_interrupt_pending(struct xhci_interrupter *ir)
{
	if (!ir->ip_autoclear) {
		u32 irq_pending;

		irq_pending = readl(&ir->ir_set->irq_pending);
		irq_pending |= IMAN_IP;
		writel(irq_pending, &ir->ir_set->irq_pending);
	}
}

/*
 * Handle all OS-owned events on an interrupter event ring. It may drop
 * and reaquire xhci->lock between event processing.
 */
static int xhci_handle_events(struct xhci_hcd *xhci, struct xhci_interrupter *ir)
{
	int event_loop = 0;
	int err;
	u64 temp;

	xhci_clear_interrupt_pending(ir);

	/* Event ring hasn't been allocated yet. */
	if (!ir->event_ring || !ir->event_ring->dequeue) {
		xhci_err(xhci, "ERROR interrupter event ring not ready\n");
		return -ENOMEM;
	}

	if (xhci->xhc_state & XHCI_STATE_DYING ||
	    xhci->xhc_state & XHCI_STATE_HALTED) {
		xhci_dbg(xhci, "xHCI dying, ignoring interrupt. Shouldn't IRQs be disabled?\n");

		/* Clear the event handler busy flag (RW1C) */
		temp = xhci_read_64(xhci, &ir->ir_set->erst_dequeue);
		xhci_write_64(xhci, temp | ERST_EHB, &ir->ir_set->erst_dequeue);
		return -ENODEV;
	}

	/* Process all OS owned event TRBs on this event ring */
	while (unhandled_event_trb(ir->event_ring)) {
		err = xhci_handle_event_trb(xhci, ir, ir->event_ring->dequeue);

		/*
		 * If half a segment of events have been handled in one go then
		 * update ERDP, and force isoc trbs to interrupt more often
		 */
		if (event_loop++ > TRBS_PER_SEGMENT / 2) {
			xhci_update_erst_dequeue(xhci, ir, false);

			if (ir->isoc_bei_interval > AVOID_BEI_INTERVAL_MIN)
				ir->isoc_bei_interval = ir->isoc_bei_interval / 2;

			event_loop = 0;
		}

		/* Update SW event ring dequeue pointer */
		inc_deq(xhci, ir->event_ring);

		if (err)
			break;
	}

	xhci_update_erst_dequeue(xhci, ir, true);

	return 0;
}

/*
 * xHCI spec says we can get an interrupt, and if the HC has an error condition,
 * we might get bad data out of the event ring.  Section 4.10.2.7 has a list of
 * indicators of an event TRB error, but we check the status *first* to be safe.
 */
irqreturn_t xhci_irq(struct usb_hcd *hcd)
{
	struct xhci_hcd *xhci = hcd_to_xhci(hcd);
	irqreturn_t ret = IRQ_HANDLED;
	u32 status;

	spin_lock(&xhci->lock);
	/* Check if the xHC generated the interrupt, or the irq is shared */
	status = readl(&xhci->op_regs->status);
	if (status == ~(u32)0) {
		xhci_hc_died(xhci);
		goto out;
	}

	if (!(status & STS_EINT)) {
		ret = IRQ_NONE;
		goto out;
	}

	if (status & STS_HCE) {
		xhci_warn(xhci, "WARNING: Host Controller Error\n");
		goto out;
	}

	if (status & STS_FATAL) {
		xhci_warn(xhci, "WARNING: Host System Error\n");
		xhci_halt(xhci);
		goto out;
	}

	/*
	 * Clear the op reg interrupt status first,
	 * so we can receive interrupts from other MSI-X interrupters.
	 * Write 1 to clear the interrupt status.
	 */
	status |= STS_EINT;
	writel(status, &xhci->op_regs->status);

	/* This is the handler of the primary interrupter */
	xhci_handle_events(xhci, xhci->interrupters[0]);
out:
	spin_unlock(&xhci->lock);

	return ret;
}

irqreturn_t xhci_msi_irq(int irq, void *hcd)
{
	return xhci_irq(hcd);
}
EXPORT_SYMBOL_GPL(xhci_msi_irq);

/****		Endpoint Ring Operations	****/

/*
 * Generic function for queueing a TRB on a ring.
 * The caller must have checked to make sure there's room on the ring.
 *
 * @more_trbs_coming:	Will you enqueue more TRBs before calling
 *			prepare_transfer()?
 */
static void queue_trb(struct xhci_hcd *xhci, struct xhci_ring *ring,
		bool more_trbs_coming,
		u32 field1, u32 field2, u32 field3, u32 field4)
{
	struct xhci_generic_trb *trb;

	trb = &ring->enqueue->generic;
	trb->field[0] = cpu_to_le32(field1);
	trb->field[1] = cpu_to_le32(field2);
	trb->field[2] = cpu_to_le32(field3);
	/* make sure TRB is fully written before giving it to the controller */
	wmb();
	trb->field[3] = cpu_to_le32(field4);

	trace_xhci_queue_trb(ring, trb,
			     xhci_trb_virt_to_dma(ring->enq_seg, ring->enqueue));

	inc_enq(xhci, ring, more_trbs_coming);
}

/*
 * Does various checks on the endpoint ring, and makes it ready to queue num_trbs.
 * expand ring if it start to be full.
 */
static int prepare_ring(struct xhci_hcd *xhci, struct xhci_ring *ep_ring,
		u32 ep_state, unsigned int num_trbs, gfp_t mem_flags)
{
	unsigned int new_segs = 0;

	/* Make sure the endpoint has been added to xHC schedule */
	switch (ep_state) {
	case EP_STATE_DISABLED:
		/*
		 * USB core changed config/interfaces without notifying us,
		 * or hardware is reporting the wrong state.
		 */
		xhci_warn(xhci, "WARN urb submitted to disabled ep\n");
		return -ENOENT;
	case EP_STATE_ERROR:
		xhci_warn(xhci, "WARN waiting for error on ep to be cleared\n");
		/* FIXME event handling code for error needs to clear it */
		/* XXX not sure if this should be -ENOENT or not */
		return -EINVAL;
	case EP_STATE_HALTED:
		xhci_dbg(xhci, "WARN halted endpoint, queueing URB anyway.\n");
		break;
	case EP_STATE_STOPPED:
	case EP_STATE_RUNNING:
		break;
	default:
		xhci_err(xhci, "ERROR unknown endpoint state for ep\n");
		/*
		 * FIXME issue Configure Endpoint command to try to get the HC
		 * back into a known state.
		 */
		return -EINVAL;
	}

	if (ep_ring != xhci->cmd_ring) {
		new_segs = xhci_ring_expansion_needed(xhci, ep_ring, num_trbs);
	} else if (xhci_num_trbs_free(ep_ring) <= num_trbs) {
		xhci_err(xhci, "Do not support expand command ring\n");
		return -ENOMEM;
	}

	if (new_segs) {
		xhci_dbg_trace(xhci, trace_xhci_dbg_ring_expansion,
				"ERROR no room on ep ring, try ring expansion");
		if (xhci_ring_expansion(xhci, ep_ring, new_segs, mem_flags)) {
			xhci_err(xhci, "Ring expansion failed\n");
			return -ENOMEM;
		}
	}

	/* Ensure that new TRBs won't overwrite a link */
	if (trb_is_link(ep_ring->enqueue))
		inc_enq_past_link(xhci, ep_ring, 0);

	if (last_trb_on_seg(ep_ring->enq_seg, ep_ring->enqueue)) {
		xhci_warn(xhci, "Missing link TRB at end of ring segment\n");
		return -EINVAL;
	}

	return 0;
}

static int prepare_transfer(struct xhci_hcd *xhci,
		struct xhci_virt_device *xdev,
		unsigned int ep_index,
		unsigned int stream_id,
		unsigned int num_trbs,
		struct urb *urb,
		unsigned int td_index,
		gfp_t mem_flags)
{
	int ret;
	struct urb_priv *urb_priv;
	struct xhci_td	*td;
	struct xhci_ring *ep_ring;
	struct xhci_ep_ctx *ep_ctx = xhci_get_ep_ctx(xhci, xdev->out_ctx, ep_index);

	ep_ring = xhci_triad_to_transfer_ring(xhci, xdev->slot_id, ep_index,
					      stream_id);
	if (!ep_ring) {
		xhci_dbg(xhci, "Can't prepare ring for bad stream ID %u\n",
				stream_id);
		return -EINVAL;
	}

	ret = prepare_ring(xhci, ep_ring, GET_EP_CTX_STATE(ep_ctx),
			   num_trbs, mem_flags);
	if (ret)
		return ret;

	urb_priv = urb->hcpriv;
	td = &urb_priv->td[td_index];

	INIT_LIST_HEAD(&td->td_list);
	INIT_LIST_HEAD(&td->cancelled_td_list);

	if (td_index == 0) {
		ret = usb_hcd_link_urb_to_ep(bus_to_hcd(urb->dev->bus), urb);
		if (unlikely(ret))
			return ret;
	}

	td->urb = urb;
	/* Add this TD to the tail of the endpoint ring's TD list */
	list_add_tail(&td->td_list, &ep_ring->td_list);
	td->start_seg = ep_ring->enq_seg;
	td->start_trb = ep_ring->enqueue;

	return 0;
}

unsigned int count_trbs(u64 addr, u64 len)
{
	unsigned int num_trbs;

	num_trbs = DIV_ROUND_UP(len + (addr & (TRB_MAX_BUFF_SIZE - 1)),
			TRB_MAX_BUFF_SIZE);
	if (num_trbs == 0)
		num_trbs++;

	return num_trbs;
}

static inline unsigned int count_trbs_needed(struct urb *urb)
{
	return count_trbs(urb->transfer_dma, urb->transfer_buffer_length);
}

static unsigned int count_sg_trbs_needed(struct urb *urb)
{
	struct scatterlist *sg;
	unsigned int i, len, full_len, num_trbs = 0;

	full_len = urb->transfer_buffer_length;

	for_each_sg(urb->sg, sg, urb->num_mapped_sgs, i) {
		len = sg_dma_len(sg);
		num_trbs += count_trbs(sg_dma_address(sg), len);
		len = min_t(unsigned int, len, full_len);
		full_len -= len;
		if (full_len == 0)
			break;
	}

	return num_trbs;
}

static unsigned int count_isoc_trbs_needed(struct urb *urb, int i)
{
	u64 addr, len;

	addr = (u64) (urb->transfer_dma + urb->iso_frame_desc[i].offset);
	len = urb->iso_frame_desc[i].length;

	return count_trbs(addr, len);
}

static void check_trb_math(struct urb *urb, int running_total)
{
	if (unlikely(running_total != urb->transfer_buffer_length))
		dev_err(&urb->dev->dev, "%s - ep %#x - Miscalculated tx length, "
				"queued %#x (%d), asked for %#x (%d)\n",
				__func__,
				urb->ep->desc.bEndpointAddress,
				running_total, running_total,
				urb->transfer_buffer_length,
				urb->transfer_buffer_length);
}

static void giveback_first_trb(struct xhci_hcd *xhci, int slot_id,
		unsigned int ep_index, unsigned int stream_id, int start_cycle,
		struct xhci_generic_trb *start_trb)
{
	/*
	 * Pass all the TRBs to the hardware at once and make sure this write
	 * isn't reordered.
	 */
	wmb();
	if (start_cycle)
		start_trb->field[3] |= cpu_to_le32(start_cycle);
	else
		start_trb->field[3] &= cpu_to_le32(~TRB_CYCLE);
	xhci_ring_ep_doorbell(xhci, slot_id, ep_index, stream_id);
}

static void check_interval(struct urb *urb, struct xhci_ep_ctx *ep_ctx)
{
	int xhci_interval;
	int ep_interval;

	xhci_interval = EP_INTERVAL_TO_UFRAMES(le32_to_cpu(ep_ctx->ep_info));
	ep_interval = urb->interval;

	/* Convert to microframes */
	if (urb->dev->speed == USB_SPEED_LOW ||
			urb->dev->speed == USB_SPEED_FULL)
		ep_interval *= 8;

	/* FIXME change this to a warning and a suggestion to use the new API
	 * to set the polling interval (once the API is added).
	 */
	if (xhci_interval != ep_interval) {
		dev_dbg_ratelimited(&urb->dev->dev,
				"Driver uses different interval (%d microframe%s) than xHCI (%d microframe%s)\n",
				ep_interval, str_plural(ep_interval),
				xhci_interval, str_plural(xhci_interval));
		urb->interval = xhci_interval;
		/* Convert back to frames for LS/FS devices */
		if (urb->dev->speed == USB_SPEED_LOW ||
				urb->dev->speed == USB_SPEED_FULL)
			urb->interval /= 8;
	}
}

/*
 * xHCI uses normal TRBs for both bulk and interrupt.  When the interrupt
 * endpoint is to be serviced, the xHC will consume (at most) one TD.  A TD
 * (comprised of sg list entries) can take several service intervals to
 * transmit.
 */
int xhci_queue_intr_tx(struct xhci_hcd *xhci, gfp_t mem_flags,
		struct urb *urb, int slot_id, unsigned int ep_index)
{
	struct xhci_ep_ctx *ep_ctx;

	ep_ctx = xhci_get_ep_ctx(xhci, xhci->devs[slot_id]->out_ctx, ep_index);
	check_interval(urb, ep_ctx);

	return xhci_queue_bulk_tx(xhci, mem_flags, urb, slot_id, ep_index);
}

/*
 * For xHCI 1.0 host controllers, TD size is the number of max packet sized
 * packets remaining in the TD (*not* including this TRB).
 *
 * Total TD packet count = total_packet_count =
 *     DIV_ROUND_UP(TD size in bytes / wMaxPacketSize)
 *
 * Packets transferred up to and including this TRB = packets_transferred =
 *     rounddown(total bytes transferred including this TRB / wMaxPacketSize)
 *
 * TD size = total_packet_count - packets_transferred
 *
 * For xHCI 0.96 and older, TD size field should be the remaining bytes
 * including this TRB, right shifted by 10
 *
 * For all hosts it must fit in bits 21:17, so it can't be bigger than 31.
 * This is taken care of in the TRB_TD_SIZE() macro
 *
 * The last TRB in a TD must have the TD size set to zero.
 */
static u32 xhci_td_remainder(struct xhci_hcd *xhci, int transferred,
			      int trb_buff_len, unsigned int td_total_len,
			      struct urb *urb, bool more_trbs_coming)
{
	u32 maxp, total_packet_count;

	/* MTK xHCI 0.96 contains some features from 1.0 */
	if (xhci->hci_version < 0x100 && !(xhci->quirks & XHCI_MTK_HOST))
		return ((td_total_len - transferred) >> 10);

	/* One TRB with a zero-length data packet. */
	if (!more_trbs_coming || (transferred == 0 && trb_buff_len == 0) ||
	    trb_buff_len == td_total_len)
		return 0;

	/* for MTK xHCI 0.96, TD size include this TRB, but not in 1.x */
	if ((xhci->quirks & XHCI_MTK_HOST) && (xhci->hci_version < 0x100))
		trb_buff_len = 0;

	maxp = usb_endpoint_maxp(&urb->ep->desc);
	total_packet_count = DIV_ROUND_UP(td_total_len, maxp);

	/* Queueing functions don't count the current TRB into transferred */
	return (total_packet_count - ((transferred + trb_buff_len) / maxp));
}


static int xhci_align_td(struct xhci_hcd *xhci, struct urb *urb, u32 enqd_len,
			 u32 *trb_buff_len, struct xhci_segment *seg)
{
	struct device *dev = xhci_to_hcd(xhci)->self.sysdev;
	unsigned int unalign;
	unsigned int max_pkt;
	u32 new_buff_len;
	size_t len;

	max_pkt = usb_endpoint_maxp(&urb->ep->desc);
	unalign = (enqd_len + *trb_buff_len) % max_pkt;

	/* we got lucky, last normal TRB data on segment is packet aligned */
	if (unalign == 0)
		return 0;

	xhci_dbg(xhci, "Unaligned %d bytes, buff len %d\n",
		 unalign, *trb_buff_len);

	/* is the last nornal TRB alignable by splitting it */
	if (*trb_buff_len > unalign) {
		*trb_buff_len -= unalign;
		xhci_dbg(xhci, "split align, new buff len %d\n", *trb_buff_len);
		return 0;
	}

	/*
	 * We want enqd_len + trb_buff_len to sum up to a number aligned to
	 * number which is divisible by the endpoint's wMaxPacketSize. IOW:
	 * (size of currently enqueued TRBs + remainder) % wMaxPacketSize == 0.
	 */
	new_buff_len = max_pkt - (enqd_len % max_pkt);

	if (new_buff_len > (urb->transfer_buffer_length - enqd_len))
		new_buff_len = (urb->transfer_buffer_length - enqd_len);

	/* create a max max_pkt sized bounce buffer pointed to by last trb */
	if (usb_urb_dir_out(urb)) {
		if (urb->num_sgs) {
			len = sg_pcopy_to_buffer(urb->sg, urb->num_sgs,
						 seg->bounce_buf, new_buff_len, enqd_len);
			if (len != new_buff_len)
				xhci_warn(xhci, "WARN Wrong bounce buffer write length: %zu != %d\n",
					  len, new_buff_len);
		} else {
			memcpy(seg->bounce_buf, urb->transfer_buffer + enqd_len, new_buff_len);
		}

		seg->bounce_dma = dma_map_single(dev, seg->bounce_buf,
						 max_pkt, DMA_TO_DEVICE);
	} else {
		seg->bounce_dma = dma_map_single(dev, seg->bounce_buf,
						 max_pkt, DMA_FROM_DEVICE);
	}

	if (dma_mapping_error(dev, seg->bounce_dma)) {
		/* try without aligning. Some host controllers survive */
		xhci_warn(xhci, "Failed mapping bounce buffer, not aligning\n");
		return 0;
	}
	*trb_buff_len = new_buff_len;
	seg->bounce_len = new_buff_len;
	seg->bounce_offs = enqd_len;

	xhci_dbg(xhci, "Bounce align, new buff len %d\n", *trb_buff_len);

	return 1;
}

/* This is very similar to what ehci-q.c qtd_fill() does */
int xhci_queue_bulk_tx(struct xhci_hcd *xhci, gfp_t mem_flags,
		struct urb *urb, int slot_id, unsigned int ep_index)
{
	struct xhci_ring *ring;
	struct urb_priv *urb_priv;
	struct xhci_td *td;
	struct xhci_generic_trb *start_trb;
	struct scatterlist *sg = NULL;
	bool more_trbs_coming = true;
	bool need_zero_pkt = false;
	bool first_trb = true;
	unsigned int num_trbs;
	unsigned int start_cycle, num_sgs = 0;
	unsigned int enqd_len, block_len, trb_buff_len, full_len;
	int sent_len, ret;
	u32 field, length_field, remainder;
	u64 addr, send_addr;

	ring = xhci_urb_to_transfer_ring(xhci, urb);
	if (!ring)
		return -EINVAL;

	full_len = urb->transfer_buffer_length;
	/* If we have scatter/gather list, we use it. */
	if (urb->num_sgs && !(urb->transfer_flags & URB_DMA_MAP_SINGLE)) {
		num_sgs = urb->num_mapped_sgs;
		sg = urb->sg;
		addr = (u64) sg_dma_address(sg);
		block_len = sg_dma_len(sg);
		num_trbs = count_sg_trbs_needed(urb);
	} else {
		num_trbs = count_trbs_needed(urb);
		addr = (u64) urb->transfer_dma;
		block_len = full_len;
	}
	ret = prepare_transfer(xhci, xhci->devs[slot_id],
			ep_index, urb->stream_id,
			num_trbs, urb, 0, mem_flags);
	if (unlikely(ret < 0))
		return ret;

	urb_priv = urb->hcpriv;

	/* Deal with URB_ZERO_PACKET - need one more td/trb */
	if (urb->transfer_flags & URB_ZERO_PACKET && urb_priv->num_tds > 1)
		need_zero_pkt = true;

	td = &urb_priv->td[0];

	/*
	 * Don't give the first TRB to the hardware (by toggling the cycle bit)
	 * until we've finished creating all the other TRBs.  The ring's cycle
	 * state may change as we enqueue the other TRBs, so save it too.
	 */
	start_trb = &ring->enqueue->generic;
	start_cycle = ring->cycle_state;
	send_addr = addr;

	/* Queue the TRBs, even if they are zero-length */
	for (enqd_len = 0; first_trb || enqd_len < full_len;
			enqd_len += trb_buff_len) {
		field = TRB_TYPE(TRB_NORMAL);

		/* TRB buffer should not cross 64KB boundaries */
		trb_buff_len = TRB_BUFF_LEN_UP_TO_BOUNDARY(addr);
		trb_buff_len = min_t(unsigned int, trb_buff_len, block_len);

		if (enqd_len + trb_buff_len > full_len)
			trb_buff_len = full_len - enqd_len;

		/* Don't change the cycle bit of the first TRB until later */
		if (first_trb) {
			first_trb = false;
			if (start_cycle == 0)
				field |= TRB_CYCLE;
		} else
			field |= ring->cycle_state;

		/* Chain all the TRBs together; clear the chain bit in the last
		 * TRB to indicate it's the last TRB in the chain.
		 */
		if (enqd_len + trb_buff_len < full_len) {
			field |= TRB_CHAIN;
			if (trb_is_link(ring->enqueue + 1)) {
				if (xhci_align_td(xhci, urb, enqd_len,
						  &trb_buff_len,
						  ring->enq_seg)) {
					send_addr = ring->enq_seg->bounce_dma;
					/* assuming TD won't span 2 segs */
					td->bounce_seg = ring->enq_seg;
				}
			}
		}
		if (enqd_len + trb_buff_len >= full_len) {
			field &= ~TRB_CHAIN;
			field |= TRB_IOC;
			more_trbs_coming = false;
			td->end_trb = ring->enqueue;
			td->end_seg = ring->enq_seg;
			if (xhci_urb_suitable_for_idt(urb)) {
				memcpy(&send_addr, urb->transfer_buffer,
				       trb_buff_len);
				le64_to_cpus(&send_addr);
				field |= TRB_IDT;
			}
		}

		/* Only set interrupt on short packet for IN endpoints */
		if (usb_urb_dir_in(urb))
			field |= TRB_ISP;

		/* Set the TRB length, TD size, and interrupter fields. */
		remainder = xhci_td_remainder(xhci, enqd_len, trb_buff_len,
					      full_len, urb, more_trbs_coming);

		length_field = TRB_LEN(trb_buff_len) |
			TRB_TD_SIZE(remainder) |
			TRB_INTR_TARGET(0);

		queue_trb(xhci, ring, more_trbs_coming | need_zero_pkt,
				lower_32_bits(send_addr),
				upper_32_bits(send_addr),
				length_field,
				field);
		addr += trb_buff_len;
		sent_len = trb_buff_len;

		while (sg && sent_len >= block_len) {
			/* New sg entry */
			--num_sgs;
			sent_len -= block_len;
			sg = sg_next(sg);
			if (num_sgs != 0 && sg) {
				block_len = sg_dma_len(sg);
				addr = (u64) sg_dma_address(sg);
				addr += sent_len;
			}
		}
		block_len -= sent_len;
		send_addr = addr;
	}

	if (need_zero_pkt) {
		ret = prepare_transfer(xhci, xhci->devs[slot_id],
				       ep_index, urb->stream_id,
				       1, urb, 1, mem_flags);
		urb_priv->td[1].end_trb = ring->enqueue;
		urb_priv->td[1].end_seg = ring->enq_seg;
		field = TRB_TYPE(TRB_NORMAL) | ring->cycle_state | TRB_IOC;
		queue_trb(xhci, ring, 0, 0, 0, TRB_INTR_TARGET(0), field);
	}

	check_trb_math(urb, enqd_len);
	giveback_first_trb(xhci, slot_id, ep_index, urb->stream_id,
			start_cycle, start_trb);
	return 0;
}

/* Caller must have locked xhci->lock */
int xhci_queue_ctrl_tx(struct xhci_hcd *xhci, gfp_t mem_flags,
		struct urb *urb, int slot_id, unsigned int ep_index)
{
	struct xhci_ring *ep_ring;
	int num_trbs;
	int ret;
	struct usb_ctrlrequest *setup;
	struct xhci_generic_trb *start_trb;
	int start_cycle;
	u32 field;
	struct urb_priv *urb_priv;
	struct xhci_td *td;

	ep_ring = xhci_urb_to_transfer_ring(xhci, urb);
	if (!ep_ring)
		return -EINVAL;

	/*
	 * Need to copy setup packet into setup TRB, so we can't use the setup
	 * DMA address.
	 */
	if (!urb->setup_packet)
		return -EINVAL;

	if ((xhci->quirks & XHCI_ETRON_HOST) &&
	    urb->dev->speed >= USB_SPEED_SUPER) {
		/*
		 * If next available TRB is the Link TRB in the ring segment then
		 * enqueue a No Op TRB, this can prevent the Setup and Data Stage
		 * TRB to be breaked by the Link TRB.
		 */
		if (last_trb_on_seg(ep_ring->enq_seg, ep_ring->enqueue + 1)) {
			field = TRB_TYPE(TRB_TR_NOOP) | ep_ring->cycle_state;
			queue_trb(xhci, ep_ring, false, 0, 0,
					TRB_INTR_TARGET(0), field);
		}
	}

	/* 1 TRB for setup, 1 for status */
	num_trbs = 2;
	/*
	 * Don't need to check if we need additional event data and normal TRBs,
	 * since data in control transfers will never get bigger than 16MB
	 * XXX: can we get a buffer that crosses 64KB boundaries?
	 */
	if (urb->transfer_buffer_length > 0)
		num_trbs++;
	ret = prepare_transfer(xhci, xhci->devs[slot_id],
			ep_index, urb->stream_id,
			num_trbs, urb, 0, mem_flags);
	if (ret < 0)
		return ret;

	urb_priv = urb->hcpriv;
	td = &urb_priv->td[0];

	/*
	 * Don't give the first TRB to the hardware (by toggling the cycle bit)
	 * until we've finished creating all the other TRBs.  The ring's cycle
	 * state may change as we enqueue the other TRBs, so save it too.
	 */
	start_trb = &ep_ring->enqueue->generic;
	start_cycle = ep_ring->cycle_state;

	/* Queue setup TRB - see section 6.4.1.2.1 */
	/* FIXME better way to translate setup_packet into two u32 fields? */
	setup = (struct usb_ctrlrequest *) urb->setup_packet;
	field = 0;
	field |= TRB_IDT | TRB_TYPE(TRB_SETUP);
	if (start_cycle == 0)
		field |= 0x1;

	/* xHCI 1.0/1.1 6.4.1.2.1: Transfer Type field */
	if ((xhci->hci_version >= 0x100) || (xhci->quirks & XHCI_MTK_HOST)) {
		if (urb->transfer_buffer_length > 0) {
			if (setup->bRequestType & USB_DIR_IN)
				field |= TRB_TX_TYPE(TRB_DATA_IN);
			else
				field |= TRB_TX_TYPE(TRB_DATA_OUT);
		}
	}

	queue_trb(xhci, ep_ring, true,
		  setup->bRequestType | setup->bRequest << 8 | le16_to_cpu(setup->wValue) << 16,
		  le16_to_cpu(setup->wIndex) | le16_to_cpu(setup->wLength) << 16,
		  TRB_LEN(8) | TRB_INTR_TARGET(0),
		  /* Immediate data in pointer */
		  field);

	/* If there's data, queue data TRBs */
	/* Only set interrupt on short packet for IN endpoints */
	if (usb_urb_dir_in(urb))
		field = TRB_ISP | TRB_TYPE(TRB_DATA);
	else
		field = TRB_TYPE(TRB_DATA);

	if (urb->transfer_buffer_length > 0) {
		u32 length_field, remainder;
		u64 addr;

		if (xhci_urb_suitable_for_idt(urb)) {
			memcpy(&addr, urb->transfer_buffer,
			       urb->transfer_buffer_length);
			le64_to_cpus(&addr);
			field |= TRB_IDT;
		} else {
			addr = (u64) urb->transfer_dma;
		}

		remainder = xhci_td_remainder(xhci, 0,
				urb->transfer_buffer_length,
				urb->transfer_buffer_length,
				urb, 1);
		length_field = TRB_LEN(urb->transfer_buffer_length) |
				TRB_TD_SIZE(remainder) |
				TRB_INTR_TARGET(0);
		if (setup->bRequestType & USB_DIR_IN)
			field |= TRB_DIR_IN;
		queue_trb(xhci, ep_ring, true,
				lower_32_bits(addr),
				upper_32_bits(addr),
				length_field,
				field | ep_ring->cycle_state);
	}

	/* Save the DMA address of the last TRB in the TD */
	td->end_trb = ep_ring->enqueue;
	td->end_seg = ep_ring->enq_seg;

	/* Queue status TRB - see Table 7 and sections 4.11.2.2 and 6.4.1.2.3 */
	/* If the device sent data, the status stage is an OUT transfer */
	if (urb->transfer_buffer_length > 0 && setup->bRequestType & USB_DIR_IN)
		field = 0;
	else
		field = TRB_DIR_IN;
	queue_trb(xhci, ep_ring, false,
			0,
			0,
			TRB_INTR_TARGET(0),
			/* Event on completion */
			field | TRB_IOC | TRB_TYPE(TRB_STATUS) | ep_ring->cycle_state);

	giveback_first_trb(xhci, slot_id, ep_index, 0,
			start_cycle, start_trb);
	return 0;
}

/*
 * The transfer burst count field of the isochronous TRB defines the number of
 * bursts that are required to move all packets in this TD.  Only SuperSpeed
 * devices can burst up to bMaxBurst number of packets per service interval.
 * This field is zero based, meaning a value of zero in the field means one
 * burst.  Basically, for everything but SuperSpeed devices, this field will be
 * zero.  Only xHCI 1.0 host controllers support this field.
 */
static unsigned int xhci_get_burst_count(struct xhci_hcd *xhci,
		struct urb *urb, unsigned int total_packet_count)
{
	unsigned int max_burst;

	if (xhci->hci_version < 0x100 || urb->dev->speed < USB_SPEED_SUPER)
		return 0;

	max_burst = urb->ep->ss_ep_comp.bMaxBurst;
	return DIV_ROUND_UP(total_packet_count, max_burst + 1) - 1;
}

/*
 * Returns the number of packets in the last "burst" of packets.  This field is
 * valid for all speeds of devices.  USB 2.0 devices can only do one "burst", so
 * the last burst packet count is equal to the total number of packets in the
 * TD.  SuperSpeed endpoints can have up to 3 bursts.  All but the last burst
 * must contain (bMaxBurst + 1) number of packets, but the last burst can
 * contain 1 to (bMaxBurst + 1) packets.
 */
static unsigned int xhci_get_last_burst_packet_count(struct xhci_hcd *xhci,
		struct urb *urb, unsigned int total_packet_count)
{
	unsigned int max_burst;
	unsigned int residue;

	if (xhci->hci_version < 0x100)
		return 0;

	if (urb->dev->speed >= USB_SPEED_SUPER) {
		/* bMaxBurst is zero based: 0 means 1 packet per burst */
		max_burst = urb->ep->ss_ep_comp.bMaxBurst;
		residue = total_packet_count % (max_burst + 1);
		/* If residue is zero, the last burst contains (max_burst + 1)
		 * number of packets, but the TLBPC field is zero-based.
		 */
		if (residue == 0)
			return max_burst;
		return residue - 1;
	}
	if (total_packet_count == 0)
		return 0;
	return total_packet_count - 1;
}

/*
 * Calculates Frame ID field of the isochronous TRB identifies the
 * target frame that the Interval associated with this Isochronous
 * Transfer Descriptor will start on. Refer to 4.11.2.5 in 1.1 spec.
 *
 * Returns actual frame id on success, negative value on error.
 */
static int xhci_get_isoc_frame_id(struct xhci_hcd *xhci,
		struct urb *urb, int index)
{
	int start_frame, ist, ret = 0;
	int start_frame_id, end_frame_id, current_frame_id;

	if (urb->dev->speed == USB_SPEED_LOW ||
			urb->dev->speed == USB_SPEED_FULL)
		start_frame = urb->start_frame + index * urb->interval;
	else
		start_frame = (urb->start_frame + index * urb->interval) >> 3;

	/* Isochronous Scheduling Threshold (IST, bits 0~3 in HCSPARAMS2):
	 *
	 * If bit [3] of IST is cleared to '0', software can add a TRB no
	 * later than IST[2:0] Microframes before that TRB is scheduled to
	 * be executed.
	 * If bit [3] of IST is set to '1', software can add a TRB no later
	 * than IST[2:0] Frames before that TRB is scheduled to be executed.
	 */
	ist = HCS_IST(xhci->hcs_params2) & 0x7;
	if (HCS_IST(xhci->hcs_params2) & (1 << 3))
		ist <<= 3;

	/* Software shall not schedule an Isoch TD with a Frame ID value that
	 * is less than the Start Frame ID or greater than the End Frame ID,
	 * where:
	 *
	 * End Frame ID = (Current MFINDEX register value + 895 ms.) MOD 2048
	 * Start Frame ID = (Current MFINDEX register value + IST + 1) MOD 2048
	 *
	 * Both the End Frame ID and Start Frame ID values are calculated
	 * in microframes. When software determines the valid Frame ID value;
	 * The End Frame ID value should be rounded down to the nearest Frame
	 * boundary, and the Start Frame ID value should be rounded up to the
	 * nearest Frame boundary.
	 */
	current_frame_id = readl(&xhci->run_regs->microframe_index);
	start_frame_id = roundup(current_frame_id + ist + 1, 8);
	end_frame_id = rounddown(current_frame_id + 895 * 8, 8);

	start_frame &= 0x7ff;
	start_frame_id = (start_frame_id >> 3) & 0x7ff;
	end_frame_id = (end_frame_id >> 3) & 0x7ff;

	if (start_frame_id < end_frame_id) {
		if (start_frame > end_frame_id ||
				start_frame < start_frame_id)
			ret = -EINVAL;
	} else if (start_frame_id > end_frame_id) {
		if ((start_frame > end_frame_id &&
				start_frame < start_frame_id))
			ret = -EINVAL;
	} else {
			ret = -EINVAL;
	}

	if (index == 0) {
		if (ret == -EINVAL || start_frame == start_frame_id) {
			start_frame = start_frame_id + 1;
			if (urb->dev->speed == USB_SPEED_LOW ||
					urb->dev->speed == USB_SPEED_FULL)
				urb->start_frame = start_frame;
			else
				urb->start_frame = start_frame << 3;
			ret = 0;
		}
	}

	if (ret) {
		xhci_warn(xhci, "Frame ID %d (reg %d, index %d) beyond range (%d, %d)\n",
				start_frame, current_frame_id, index,
				start_frame_id, end_frame_id);
		xhci_warn(xhci, "Ignore frame ID field, use SIA bit instead\n");
		return ret;
	}

	return start_frame;
}

/* Check if we should generate event interrupt for a TD in an isoc URB */
static bool trb_block_event_intr(struct xhci_hcd *xhci, int num_tds, int i,
				 struct xhci_interrupter *ir)
{
	if (xhci->hci_version < 0x100)
		return false;
	/* always generate an event interrupt for the last TD */
	if (i == num_tds - 1)
		return false;
	/*
	 * If AVOID_BEI is set the host handles full event rings poorly,
	 * generate an event at least every 8th TD to clear the event ring
	 */
	if (i && ir->isoc_bei_interval && xhci->quirks & XHCI_AVOID_BEI)
		return !!(i % ir->isoc_bei_interval);

	return true;
}

/* This is for isoc transfer */
static int xhci_queue_isoc_tx(struct xhci_hcd *xhci, gfp_t mem_flags,
		struct urb *urb, int slot_id, unsigned int ep_index)
{
	struct xhci_interrupter *ir;
	struct xhci_ring *ep_ring;
	struct urb_priv *urb_priv;
	struct xhci_td *td;
	int num_tds, trbs_per_td;
	struct xhci_generic_trb *start_trb;
	bool first_trb;
	int start_cycle;
	u32 field, length_field;
	int running_total, trb_buff_len, td_len, td_remain_len, ret;
	u64 start_addr, addr;
	int i, j;
	bool more_trbs_coming;
	struct xhci_virt_ep *xep;
	int frame_id;

	xep = &xhci->devs[slot_id]->eps[ep_index];
	ep_ring = xhci->devs[slot_id]->eps[ep_index].ring;
	ir = xhci->interrupters[0];

	num_tds = urb->number_of_packets;
	if (num_tds < 1) {
		xhci_dbg(xhci, "Isoc URB with zero packets?\n");
		return -EINVAL;
	}
	start_addr = (u64) urb->transfer_dma;
	start_trb = &ep_ring->enqueue->generic;
	start_cycle = ep_ring->cycle_state;

	urb_priv = urb->hcpriv;
	/* Queue the TRBs for each TD, even if they are zero-length */
	for (i = 0; i < num_tds; i++) {
		unsigned int total_pkt_count, max_pkt;
		unsigned int burst_count, last_burst_pkt_count;
		u32 sia_frame_id;

		first_trb = true;
		running_total = 0;
		addr = start_addr + urb->iso_frame_desc[i].offset;
		td_len = urb->iso_frame_desc[i].length;
		td_remain_len = td_len;
		max_pkt = usb_endpoint_maxp(&urb->ep->desc);
		total_pkt_count = DIV_ROUND_UP(td_len, max_pkt);

		/* A zero-length transfer still involves at least one packet. */
		if (total_pkt_count == 0)
			total_pkt_count++;
		burst_count = xhci_get_burst_count(xhci, urb, total_pkt_count);
		last_burst_pkt_count = xhci_get_last_burst_packet_count(xhci,
							urb, total_pkt_count);

		trbs_per_td = count_isoc_trbs_needed(urb, i);

		ret = prepare_transfer(xhci, xhci->devs[slot_id], ep_index,
				urb->stream_id, trbs_per_td, urb, i, mem_flags);
		if (ret < 0) {
			if (i == 0)
				return ret;
			goto cleanup;
		}
		td = &urb_priv->td[i];
		/* use SIA as default, if frame id is used overwrite it */
		sia_frame_id = TRB_SIA;
		if (!(urb->transfer_flags & URB_ISO_ASAP) &&
		    HCC_CFC(xhci->hcc_params)) {
			frame_id = xhci_get_isoc_frame_id(xhci, urb, i);
			if (frame_id >= 0)
				sia_frame_id = TRB_FRAME_ID(frame_id);
		}
		/*
		 * Set isoc specific data for the first TRB in a TD.
		 * Prevent HW from getting the TRBs by keeping the cycle state
		 * inverted in the first TDs isoc TRB.
		 */
		field = TRB_TYPE(TRB_ISOC) |
			TRB_TLBPC(last_burst_pkt_count) |
			sia_frame_id |
			(i ? ep_ring->cycle_state : !start_cycle);

		/* xhci 1.1 with ETE uses TD_Size field for TBC, old is Rsvdz */
		if (!xep->use_extended_tbc)
			field |= TRB_TBC(burst_count);

		/* fill the rest of the TRB fields, and remaining normal TRBs */
		for (j = 0; j < trbs_per_td; j++) {
			u32 remainder = 0;

			/* only first TRB is isoc, overwrite otherwise */
			if (!first_trb)
				field = TRB_TYPE(TRB_NORMAL) |
					ep_ring->cycle_state;

			/* Only set interrupt on short packet for IN EPs */
			if (usb_urb_dir_in(urb))
				field |= TRB_ISP;

			/* Set the chain bit for all except the last TRB  */
			if (j < trbs_per_td - 1) {
				more_trbs_coming = true;
				field |= TRB_CHAIN;
			} else {
				more_trbs_coming = false;
				td->end_trb = ep_ring->enqueue;
				td->end_seg = ep_ring->enq_seg;
				field |= TRB_IOC;
				if (trb_block_event_intr(xhci, num_tds, i, ir))
					field |= TRB_BEI;
			}
			/* Calculate TRB length */
			trb_buff_len = TRB_BUFF_LEN_UP_TO_BOUNDARY(addr);
			if (trb_buff_len > td_remain_len)
				trb_buff_len = td_remain_len;

			/* Set the TRB length, TD size, & interrupter fields. */
			remainder = xhci_td_remainder(xhci, running_total,
						   trb_buff_len, td_len,
						   urb, more_trbs_coming);

			length_field = TRB_LEN(trb_buff_len) |
				TRB_INTR_TARGET(0);

			/* xhci 1.1 with ETE uses TD Size field for TBC */
			if (first_trb && xep->use_extended_tbc)
				length_field |= TRB_TD_SIZE_TBC(burst_count);
			else
				length_field |= TRB_TD_SIZE(remainder);
			first_trb = false;

			queue_trb(xhci, ep_ring, more_trbs_coming,
				lower_32_bits(addr),
				upper_32_bits(addr),
				length_field,
				field);
			running_total += trb_buff_len;

			addr += trb_buff_len;
			td_remain_len -= trb_buff_len;
		}

		/* Check TD length */
		if (running_total != td_len) {
			xhci_err(xhci, "ISOC TD length unmatch\n");
			ret = -EINVAL;
			goto cleanup;
		}
	}

	/* store the next frame id */
	if (HCC_CFC(xhci->hcc_params))
		xep->next_frame_id = urb->start_frame + num_tds * urb->interval;

	if (xhci_to_hcd(xhci)->self.bandwidth_isoc_reqs == 0) {
		if (xhci->quirks & XHCI_AMD_PLL_FIX)
			usb_amd_quirk_pll_disable();
	}
	xhci_to_hcd(xhci)->self.bandwidth_isoc_reqs++;

	giveback_first_trb(xhci, slot_id, ep_index, urb->stream_id,
			start_cycle, start_trb);
	return 0;
cleanup:
	/* Clean up a partially enqueued isoc transfer. */

	for (i--; i >= 0; i--)
		list_del_init(&urb_priv->td[i].td_list);

	/* Use the first TD as a temporary variable to turn the TDs we've queued
	 * into No-ops with a software-owned cycle bit. That way the hardware
	 * won't accidentally start executing bogus TDs when we partially
	 * overwrite them.  td->start_trb and td->start_seg are already set.
	 */
	urb_priv->td[0].end_trb = ep_ring->enqueue;
	/* Every TRB except the first & last will have its cycle bit flipped. */
	td_to_noop(&urb_priv->td[0], true);

	/* Reset the ring enqueue back to the first TRB and its cycle bit. */
	ep_ring->enqueue = urb_priv->td[0].start_trb;
	ep_ring->enq_seg = urb_priv->td[0].start_seg;
	ep_ring->cycle_state = start_cycle;
	usb_hcd_unlink_urb_from_ep(bus_to_hcd(urb->dev->bus), urb);
	return ret;
}

/*
 * Check transfer ring to guarantee there is enough room for the urb.
 * Update ISO URB start_frame and interval.
 * Update interval as xhci_queue_intr_tx does. Use xhci frame_index to
 * update urb->start_frame if URB_ISO_ASAP is set in transfer_flags or
 * Contiguous Frame ID is not supported by HC.
 */
int xhci_queue_isoc_tx_prepare(struct xhci_hcd *xhci, gfp_t mem_flags,
		struct urb *urb, int slot_id, unsigned int ep_index)
{
	struct xhci_virt_device *xdev;
	struct xhci_ring *ep_ring;
	struct xhci_ep_ctx *ep_ctx;
	int start_frame;
	int num_tds, num_trbs, i;
	int ret;
	struct xhci_virt_ep *xep;
	int ist;

	xdev = xhci->devs[slot_id];
	xep = &xhci->devs[slot_id]->eps[ep_index];
	ep_ring = xdev->eps[ep_index].ring;
	ep_ctx = xhci_get_ep_ctx(xhci, xdev->out_ctx, ep_index);

	num_trbs = 0;
	num_tds = urb->number_of_packets;
	for (i = 0; i < num_tds; i++)
		num_trbs += count_isoc_trbs_needed(urb, i);

	/* Check the ring to guarantee there is enough room for the whole urb.
	 * Do not insert any td of the urb to the ring if the check failed.
	 */
	ret = prepare_ring(xhci, ep_ring, GET_EP_CTX_STATE(ep_ctx),
			   num_trbs, mem_flags);
	if (ret)
		return ret;

	/*
	 * Check interval value. This should be done before we start to
	 * calculate the start frame value.
	 */
	check_interval(urb, ep_ctx);

	/* Calculate the start frame and put it in urb->start_frame. */
	if (HCC_CFC(xhci->hcc_params) && !list_empty(&ep_ring->td_list)) {
		if (GET_EP_CTX_STATE(ep_ctx) ==	EP_STATE_RUNNING) {
			urb->start_frame = xep->next_frame_id;
			goto skip_start_over;
		}
	}

	start_frame = readl(&xhci->run_regs->microframe_index);
	start_frame &= 0x3fff;
	/*
	 * Round up to the next frame and consider the time before trb really
	 * gets scheduled by hardare.
	 */
	ist = HCS_IST(xhci->hcs_params2) & 0x7;
	if (HCS_IST(xhci->hcs_params2) & (1 << 3))
		ist <<= 3;
	start_frame += ist + XHCI_CFC_DELAY;
	start_frame = roundup(start_frame, 8);

	/*
	 * Round up to the next ESIT (Endpoint Service Interval Time) if ESIT
	 * is greate than 8 microframes.
	 */
	if (urb->dev->speed == USB_SPEED_LOW ||
			urb->dev->speed == USB_SPEED_FULL) {
		start_frame = roundup(start_frame, urb->interval << 3);
		urb->start_frame = start_frame >> 3;
	} else {
		start_frame = roundup(start_frame, urb->interval);
		urb->start_frame = start_frame;
	}

skip_start_over:

	return xhci_queue_isoc_tx(xhci, mem_flags, urb, slot_id, ep_index);
}

/****		Command Ring Operations		****/

/* Generic function for queueing a command TRB on the command ring.
 * Check to make sure there's room on the command ring for one command TRB.
 * Also check that there's room reserved for commands that must not fail.
 * If this is a command that must not fail, meaning command_must_succeed = TRUE,
 * then only check for the number of reserved spots.
 * Don't decrement xhci->cmd_ring_reserved_trbs after we've queued the TRB
 * because the command event handler may want to resubmit a failed command.
 */
static int queue_command(struct xhci_hcd *xhci, struct xhci_command *cmd,
			 u32 field1, u32 field2,
			 u32 field3, u32 field4, bool command_must_succeed)
{
	int reserved_trbs = xhci->cmd_ring_reserved_trbs;
	int ret;

	if ((xhci->xhc_state & XHCI_STATE_DYING) ||
		(xhci->xhc_state & XHCI_STATE_HALTED)) {
		xhci_dbg(xhci, "xHCI dying or halted, can't queue_command\n");
		return -ESHUTDOWN;
	}

	if (!command_must_succeed)
		reserved_trbs++;

	ret = prepare_ring(xhci, xhci->cmd_ring, EP_STATE_RUNNING,
			reserved_trbs, GFP_ATOMIC);
	if (ret < 0) {
		xhci_err(xhci, "ERR: No room for command on command ring\n");
		if (command_must_succeed)
			xhci_err(xhci, "ERR: Reserved TRB counting for "
					"unfailable commands failed.\n");
		return ret;
	}

	cmd->command_trb = xhci->cmd_ring->enqueue;

	/* if there are no other commands queued we start the timeout timer */
	if (list_empty(&xhci->cmd_list)) {
		xhci->current_cmd = cmd;
		xhci_mod_cmd_timer(xhci);
	}

	list_add_tail(&cmd->cmd_list, &xhci->cmd_list);

	queue_trb(xhci, xhci->cmd_ring, false, field1, field2, field3,
			field4 | xhci->cmd_ring->cycle_state);
	return 0;
}

/* Queue a slot enable or disable request on the command ring */
int xhci_queue_slot_control(struct xhci_hcd *xhci, struct xhci_command *cmd,
		u32 trb_type, u32 slot_id)
{
	return queue_command(xhci, cmd, 0, 0, 0,
			TRB_TYPE(trb_type) | SLOT_ID_FOR_TRB(slot_id), false);
}

/* Queue an address device command TRB */
int xhci_queue_address_device(struct xhci_hcd *xhci, struct xhci_command *cmd,
		dma_addr_t in_ctx_ptr, u32 slot_id, enum xhci_setup_dev setup)
{
	return queue_command(xhci, cmd, lower_32_bits(in_ctx_ptr),
			upper_32_bits(in_ctx_ptr), 0,
			TRB_TYPE(TRB_ADDR_DEV) | SLOT_ID_FOR_TRB(slot_id)
			| (setup == SETUP_CONTEXT_ONLY ? TRB_BSR : 0), false);
}

int xhci_queue_vendor_command(struct xhci_hcd *xhci, struct xhci_command *cmd,
		u32 field1, u32 field2, u32 field3, u32 field4)
{
	return queue_command(xhci, cmd, field1, field2, field3, field4, false);
}

/* Queue a reset device command TRB */
int xhci_queue_reset_device(struct xhci_hcd *xhci, struct xhci_command *cmd,
		u32 slot_id)
{
	return queue_command(xhci, cmd, 0, 0, 0,
			TRB_TYPE(TRB_RESET_DEV) | SLOT_ID_FOR_TRB(slot_id),
			false);
}

/* Queue a configure endpoint command TRB */
int xhci_queue_configure_endpoint(struct xhci_hcd *xhci,
		struct xhci_command *cmd, dma_addr_t in_ctx_ptr,
		u32 slot_id, bool command_must_succeed)
{
	return queue_command(xhci, cmd, lower_32_bits(in_ctx_ptr),
			upper_32_bits(in_ctx_ptr), 0,
			TRB_TYPE(TRB_CONFIG_EP) | SLOT_ID_FOR_TRB(slot_id),
			command_must_succeed);
}

/* Queue an evaluate context command TRB */
int xhci_queue_evaluate_context(struct xhci_hcd *xhci, struct xhci_command *cmd,
		dma_addr_t in_ctx_ptr, u32 slot_id, bool command_must_succeed)
{
	return queue_command(xhci, cmd, lower_32_bits(in_ctx_ptr),
			upper_32_bits(in_ctx_ptr), 0,
			TRB_TYPE(TRB_EVAL_CONTEXT) | SLOT_ID_FOR_TRB(slot_id),
			command_must_succeed);
}

/*
 * Suspend is set to indicate "Stop Endpoint Command" is being issued to stop
 * activity on an endpoint that is about to be suspended.
 */
int xhci_queue_stop_endpoint(struct xhci_hcd *xhci, struct xhci_command *cmd,
			     int slot_id, unsigned int ep_index, int suspend)
{
	u32 trb_slot_id = SLOT_ID_FOR_TRB(slot_id);
	u32 trb_ep_index = EP_INDEX_FOR_TRB(ep_index);
	u32 type = TRB_TYPE(TRB_STOP_RING);
	u32 trb_suspend = SUSPEND_PORT_FOR_TRB(suspend);

	return queue_command(xhci, cmd, 0, 0, 0,
			trb_slot_id | trb_ep_index | type | trb_suspend, false);
}

int xhci_queue_reset_ep(struct xhci_hcd *xhci, struct xhci_command *cmd,
			int slot_id, unsigned int ep_index,
			enum xhci_ep_reset_type reset_type)
{
	u32 trb_slot_id = SLOT_ID_FOR_TRB(slot_id);
	u32 trb_ep_index = EP_INDEX_FOR_TRB(ep_index);
	u32 type = TRB_TYPE(TRB_RESET_EP);

	if (reset_type == EP_SOFT_RESET)
		type |= TRB_TSP;

	return queue_command(xhci, cmd, 0, 0, 0,
			trb_slot_id | trb_ep_index | type, false);
}
