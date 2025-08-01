// SPDX-License-Identifier: GPL-2.0-or-later
/* A network driver using virtio.
 *
 * Copyright 2007 Rusty Russell <rusty@rustcorp.com.au> IBM Corporation
 */
//#define DEBUG
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <linux/module.h>
#include <linux/virtio.h>
#include <linux/virtio_net.h>
#include <linux/bpf.h>
#include <linux/bpf_trace.h>
#include <linux/scatterlist.h>
#include <linux/if_vlan.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/average.h>
#include <linux/filter.h>
#include <linux/kernel.h>
#include <linux/dim.h>
#include <net/route.h>
#include <net/xdp.h>
#include <net/net_failover.h>
#include <net/netdev_rx_queue.h>
#include <net/netdev_queues.h>
#include <net/xdp_sock_drv.h>

static int napi_weight = NAPI_POLL_WEIGHT;
module_param(napi_weight, int, 0444);

static bool csum = true, gso = true, napi_tx = true;
module_param(csum, bool, 0444);
module_param(gso, bool, 0444);
module_param(napi_tx, bool, 0644);

/* FIXME: MTU in config. */
#define GOOD_PACKET_LEN (ETH_HLEN + VLAN_HLEN + ETH_DATA_LEN)
#define GOOD_COPY_LEN	128

#define VIRTNET_RX_PAD (NET_IP_ALIGN + NET_SKB_PAD)

/* Separating two types of XDP xmit */
#define VIRTIO_XDP_TX		BIT(0)
#define VIRTIO_XDP_REDIR	BIT(1)

/* RX packet size EWMA. The average packet size is used to determine the packet
 * buffer size when refilling RX rings. As the entire RX ring may be refilled
 * at once, the weight is chosen so that the EWMA will be insensitive to short-
 * term, transient changes in packet size.
 */
DECLARE_EWMA(pkt_len, 0, 64)

#define VIRTNET_DRIVER_VERSION "1.0.0"

static const unsigned long guest_offloads[] = {
	VIRTIO_NET_F_GUEST_TSO4,
	VIRTIO_NET_F_GUEST_TSO6,
	VIRTIO_NET_F_GUEST_ECN,
	VIRTIO_NET_F_GUEST_UFO,
	VIRTIO_NET_F_GUEST_CSUM,
	VIRTIO_NET_F_GUEST_USO4,
	VIRTIO_NET_F_GUEST_USO6,
	VIRTIO_NET_F_GUEST_HDRLEN
};

#define GUEST_OFFLOAD_GRO_HW_MASK ((1ULL << VIRTIO_NET_F_GUEST_TSO4) | \
				(1ULL << VIRTIO_NET_F_GUEST_TSO6) | \
				(1ULL << VIRTIO_NET_F_GUEST_ECN)  | \
				(1ULL << VIRTIO_NET_F_GUEST_UFO)  | \
				(1ULL << VIRTIO_NET_F_GUEST_USO4) | \
				(1ULL << VIRTIO_NET_F_GUEST_USO6))

struct virtnet_stat_desc {
	char desc[ETH_GSTRING_LEN];
	size_t offset;
	size_t qstat_offset;
};

struct virtnet_sq_free_stats {
	u64 packets;
	u64 bytes;
	u64 napi_packets;
	u64 napi_bytes;
	u64 xsk;
};

struct virtnet_sq_stats {
	struct u64_stats_sync syncp;
	u64_stats_t packets;
	u64_stats_t bytes;
	u64_stats_t xdp_tx;
	u64_stats_t xdp_tx_drops;
	u64_stats_t kicks;
	u64_stats_t tx_timeouts;
	u64_stats_t stop;
	u64_stats_t wake;
};

struct virtnet_rq_stats {
	struct u64_stats_sync syncp;
	u64_stats_t packets;
	u64_stats_t bytes;
	u64_stats_t drops;
	u64_stats_t xdp_packets;
	u64_stats_t xdp_tx;
	u64_stats_t xdp_redirects;
	u64_stats_t xdp_drops;
	u64_stats_t kicks;
};

#define VIRTNET_SQ_STAT(name, m) {name, offsetof(struct virtnet_sq_stats, m), -1}
#define VIRTNET_RQ_STAT(name, m) {name, offsetof(struct virtnet_rq_stats, m), -1}

#define VIRTNET_SQ_STAT_QSTAT(name, m)				\
	{							\
		name,						\
		offsetof(struct virtnet_sq_stats, m),		\
		offsetof(struct netdev_queue_stats_tx, m),	\
	}

#define VIRTNET_RQ_STAT_QSTAT(name, m)				\
	{							\
		name,						\
		offsetof(struct virtnet_rq_stats, m),		\
		offsetof(struct netdev_queue_stats_rx, m),	\
	}

static const struct virtnet_stat_desc virtnet_sq_stats_desc[] = {
	VIRTNET_SQ_STAT("xdp_tx",       xdp_tx),
	VIRTNET_SQ_STAT("xdp_tx_drops", xdp_tx_drops),
	VIRTNET_SQ_STAT("kicks",        kicks),
	VIRTNET_SQ_STAT("tx_timeouts",  tx_timeouts),
};

static const struct virtnet_stat_desc virtnet_rq_stats_desc[] = {
	VIRTNET_RQ_STAT("drops",         drops),
	VIRTNET_RQ_STAT("xdp_packets",   xdp_packets),
	VIRTNET_RQ_STAT("xdp_tx",        xdp_tx),
	VIRTNET_RQ_STAT("xdp_redirects", xdp_redirects),
	VIRTNET_RQ_STAT("xdp_drops",     xdp_drops),
	VIRTNET_RQ_STAT("kicks",         kicks),
};

static const struct virtnet_stat_desc virtnet_sq_stats_desc_qstat[] = {
	VIRTNET_SQ_STAT_QSTAT("packets", packets),
	VIRTNET_SQ_STAT_QSTAT("bytes",   bytes),
	VIRTNET_SQ_STAT_QSTAT("stop",	 stop),
	VIRTNET_SQ_STAT_QSTAT("wake",	 wake),
};

static const struct virtnet_stat_desc virtnet_rq_stats_desc_qstat[] = {
	VIRTNET_RQ_STAT_QSTAT("packets", packets),
	VIRTNET_RQ_STAT_QSTAT("bytes",   bytes),
};

#define VIRTNET_STATS_DESC_CQ(name) \
	{#name, offsetof(struct virtio_net_stats_cvq, name), -1}

#define VIRTNET_STATS_DESC_RX(class, name) \
	{#name, offsetof(struct virtio_net_stats_rx_ ## class, rx_ ## name), -1}

#define VIRTNET_STATS_DESC_TX(class, name) \
	{#name, offsetof(struct virtio_net_stats_tx_ ## class, tx_ ## name), -1}


static const struct virtnet_stat_desc virtnet_stats_cvq_desc[] = {
	VIRTNET_STATS_DESC_CQ(command_num),
	VIRTNET_STATS_DESC_CQ(ok_num),
};

static const struct virtnet_stat_desc virtnet_stats_rx_basic_desc[] = {
	VIRTNET_STATS_DESC_RX(basic, packets),
	VIRTNET_STATS_DESC_RX(basic, bytes),

	VIRTNET_STATS_DESC_RX(basic, notifications),
	VIRTNET_STATS_DESC_RX(basic, interrupts),
};

static const struct virtnet_stat_desc virtnet_stats_tx_basic_desc[] = {
	VIRTNET_STATS_DESC_TX(basic, packets),
	VIRTNET_STATS_DESC_TX(basic, bytes),

	VIRTNET_STATS_DESC_TX(basic, notifications),
	VIRTNET_STATS_DESC_TX(basic, interrupts),
};

static const struct virtnet_stat_desc virtnet_stats_rx_csum_desc[] = {
	VIRTNET_STATS_DESC_RX(csum, needs_csum),
};

static const struct virtnet_stat_desc virtnet_stats_tx_gso_desc[] = {
	VIRTNET_STATS_DESC_TX(gso, gso_packets_noseg),
	VIRTNET_STATS_DESC_TX(gso, gso_bytes_noseg),
};

static const struct virtnet_stat_desc virtnet_stats_rx_speed_desc[] = {
	VIRTNET_STATS_DESC_RX(speed, ratelimit_bytes),
};

static const struct virtnet_stat_desc virtnet_stats_tx_speed_desc[] = {
	VIRTNET_STATS_DESC_TX(speed, ratelimit_bytes),
};

#define VIRTNET_STATS_DESC_RX_QSTAT(class, name, qstat_field)			\
	{									\
		#name,								\
		offsetof(struct virtio_net_stats_rx_ ## class, rx_ ## name),	\
		offsetof(struct netdev_queue_stats_rx, qstat_field),		\
	}

#define VIRTNET_STATS_DESC_TX_QSTAT(class, name, qstat_field)			\
	{									\
		#name,								\
		offsetof(struct virtio_net_stats_tx_ ## class, tx_ ## name),	\
		offsetof(struct netdev_queue_stats_tx, qstat_field),		\
	}

static const struct virtnet_stat_desc virtnet_stats_rx_basic_desc_qstat[] = {
	VIRTNET_STATS_DESC_RX_QSTAT(basic, drops,         hw_drops),
	VIRTNET_STATS_DESC_RX_QSTAT(basic, drop_overruns, hw_drop_overruns),
};

static const struct virtnet_stat_desc virtnet_stats_tx_basic_desc_qstat[] = {
	VIRTNET_STATS_DESC_TX_QSTAT(basic, drops,          hw_drops),
	VIRTNET_STATS_DESC_TX_QSTAT(basic, drop_malformed, hw_drop_errors),
};

static const struct virtnet_stat_desc virtnet_stats_rx_csum_desc_qstat[] = {
	VIRTNET_STATS_DESC_RX_QSTAT(csum, csum_valid, csum_unnecessary),
	VIRTNET_STATS_DESC_RX_QSTAT(csum, csum_none,  csum_none),
	VIRTNET_STATS_DESC_RX_QSTAT(csum, csum_bad,   csum_bad),
};

static const struct virtnet_stat_desc virtnet_stats_tx_csum_desc_qstat[] = {
	VIRTNET_STATS_DESC_TX_QSTAT(csum, csum_none,  csum_none),
	VIRTNET_STATS_DESC_TX_QSTAT(csum, needs_csum, needs_csum),
};

static const struct virtnet_stat_desc virtnet_stats_rx_gso_desc_qstat[] = {
	VIRTNET_STATS_DESC_RX_QSTAT(gso, gso_packets,           hw_gro_packets),
	VIRTNET_STATS_DESC_RX_QSTAT(gso, gso_bytes,             hw_gro_bytes),
	VIRTNET_STATS_DESC_RX_QSTAT(gso, gso_packets_coalesced, hw_gro_wire_packets),
	VIRTNET_STATS_DESC_RX_QSTAT(gso, gso_bytes_coalesced,   hw_gro_wire_bytes),
};

static const struct virtnet_stat_desc virtnet_stats_tx_gso_desc_qstat[] = {
	VIRTNET_STATS_DESC_TX_QSTAT(gso, gso_packets,        hw_gso_packets),
	VIRTNET_STATS_DESC_TX_QSTAT(gso, gso_bytes,          hw_gso_bytes),
	VIRTNET_STATS_DESC_TX_QSTAT(gso, gso_segments,       hw_gso_wire_packets),
	VIRTNET_STATS_DESC_TX_QSTAT(gso, gso_segments_bytes, hw_gso_wire_bytes),
};

static const struct virtnet_stat_desc virtnet_stats_rx_speed_desc_qstat[] = {
	VIRTNET_STATS_DESC_RX_QSTAT(speed, ratelimit_packets, hw_drop_ratelimits),
};

static const struct virtnet_stat_desc virtnet_stats_tx_speed_desc_qstat[] = {
	VIRTNET_STATS_DESC_TX_QSTAT(speed, ratelimit_packets, hw_drop_ratelimits),
};

#define VIRTNET_Q_TYPE_RX 0
#define VIRTNET_Q_TYPE_TX 1
#define VIRTNET_Q_TYPE_CQ 2

struct virtnet_interrupt_coalesce {
	u32 max_packets;
	u32 max_usecs;
};

/* The dma information of pages allocated at a time. */
struct virtnet_rq_dma {
	dma_addr_t addr;
	u32 ref;
	u16 len;
	u16 need_sync;
};

/* Internal representation of a send virtqueue */
struct send_queue {
	/* Virtqueue associated with this send _queue */
	struct virtqueue *vq;

	/* TX: fragments + linear part + virtio header */
	struct scatterlist sg[MAX_SKB_FRAGS + 2];

	/* Name of the send queue: output.$index */
	char name[16];

	struct virtnet_sq_stats stats;

	struct virtnet_interrupt_coalesce intr_coal;

	struct napi_struct napi;

	/* Record whether sq is in reset state. */
	bool reset;

	struct xsk_buff_pool *xsk_pool;

	dma_addr_t xsk_hdr_dma_addr;
};

/* Internal representation of a receive virtqueue */
struct receive_queue {
	/* Virtqueue associated with this receive_queue */
	struct virtqueue *vq;

	struct napi_struct napi;

	struct bpf_prog __rcu *xdp_prog;

	struct virtnet_rq_stats stats;

	/* The number of rx notifications */
	u16 calls;

	/* Is dynamic interrupt moderation enabled? */
	bool dim_enabled;

	/* Used to protect dim_enabled and inter_coal */
	struct mutex dim_lock;

	/* Dynamic Interrupt Moderation */
	struct dim dim;

	u32 packets_in_napi;

	struct virtnet_interrupt_coalesce intr_coal;

	/* Chain pages by the private ptr. */
	struct page *pages;

	/* Average packet length for mergeable receive buffers. */
	struct ewma_pkt_len mrg_avg_pkt_len;

	/* Page frag for packet buffer allocation. */
	struct page_frag alloc_frag;

	/* RX: fragments + linear part + virtio header */
	struct scatterlist sg[MAX_SKB_FRAGS + 2];

	/* Min single buffer size for mergeable buffers case. */
	unsigned int min_buf_len;

	/* Name of this receive queue: input.$index */
	char name[16];

	struct xdp_rxq_info xdp_rxq;

	/* Record the last dma info to free after new pages is allocated. */
	struct virtnet_rq_dma *last_dma;

	struct xsk_buff_pool *xsk_pool;

	/* xdp rxq used by xsk */
	struct xdp_rxq_info xsk_rxq_info;

	struct xdp_buff **xsk_buffs;
};

#define VIRTIO_NET_RSS_MAX_KEY_SIZE     40

/* Control VQ buffers: protected by the rtnl lock */
struct control_buf {
	struct virtio_net_ctrl_hdr hdr;
	virtio_net_ctrl_ack status;
};

struct virtnet_info {
	struct virtio_device *vdev;
	struct virtqueue *cvq;
	struct net_device *dev;
	struct send_queue *sq;
	struct receive_queue *rq;
	unsigned int status;

	/* Max # of queue pairs supported by the device */
	u16 max_queue_pairs;

	/* # of queue pairs currently used by the driver */
	u16 curr_queue_pairs;

	/* # of XDP queue pairs currently used by the driver */
	u16 xdp_queue_pairs;

	/* xdp_queue_pairs may be 0, when xdp is already loaded. So add this. */
	bool xdp_enabled;

	/* I like... big packets and I cannot lie! */
	bool big_packets;

	/* number of sg entries allocated for big packets */
	unsigned int big_packets_num_skbfrags;

	/* Host will merge rx buffers for big packets (shake it! shake it!) */
	bool mergeable_rx_bufs;

	/* Host supports rss and/or hash report */
	bool has_rss;
	bool has_rss_hash_report;
	u8 rss_key_size;
	u16 rss_indir_table_size;
	u32 rss_hash_types_supported;
	u32 rss_hash_types_saved;
	struct virtio_net_rss_config_hdr *rss_hdr;
	struct virtio_net_rss_config_trailer rss_trailer;
	u8 rss_hash_key_data[VIRTIO_NET_RSS_MAX_KEY_SIZE];

	/* Has control virtqueue */
	bool has_cvq;

	/* Lock to protect the control VQ */
	struct mutex cvq_lock;

	/* Host can handle any s/g split between our header and packet data */
	bool any_header_sg;

	/* Packet virtio header size */
	u8 hdr_len;

	/* Work struct for delayed refilling if we run low on memory. */
	struct delayed_work refill;

	/* Is delayed refill enabled? */
	bool refill_enabled;

	/* The lock to synchronize the access to refill_enabled */
	spinlock_t refill_lock;

	/* Work struct for config space updates */
	struct work_struct config_work;

	/* Work struct for setting rx mode */
	struct work_struct rx_mode_work;

	/* OK to queue work setting RX mode? */
	bool rx_mode_work_enabled;

	/* Does the affinity hint is set for virtqueues? */
	bool affinity_hint_set;

	/* CPU hotplug instances for online & dead */
	struct hlist_node node;
	struct hlist_node node_dead;

	struct control_buf *ctrl;

	/* Ethtool settings */
	u8 duplex;
	u32 speed;

	/* Is rx dynamic interrupt moderation enabled? */
	bool rx_dim_enabled;

	/* Interrupt coalescing settings */
	struct virtnet_interrupt_coalesce intr_coal_tx;
	struct virtnet_interrupt_coalesce intr_coal_rx;

	unsigned long guest_offloads;
	unsigned long guest_offloads_capable;

	/* failover when STANDBY feature enabled */
	struct failover *failover;

	u64 device_stats_cap;
};

struct padded_vnet_hdr {
	struct virtio_net_hdr_v1_hash hdr;
	/*
	 * hdr is in a separate sg buffer, and data sg buffer shares same page
	 * with this header sg. This padding makes next sg 16 byte aligned
	 * after the header.
	 */
	char padding[12];
};

struct virtio_net_common_hdr {
	union {
		struct virtio_net_hdr hdr;
		struct virtio_net_hdr_mrg_rxbuf	mrg_hdr;
		struct virtio_net_hdr_v1_hash hash_v1_hdr;
	};
};

static struct virtio_net_common_hdr xsk_hdr;

static void virtnet_sq_free_unused_buf(struct virtqueue *vq, void *buf);
static void virtnet_sq_free_unused_buf_done(struct virtqueue *vq);
static int virtnet_xdp_handler(struct bpf_prog *xdp_prog, struct xdp_buff *xdp,
			       struct net_device *dev,
			       unsigned int *xdp_xmit,
			       struct virtnet_rq_stats *stats);
static void virtnet_receive_done(struct virtnet_info *vi, struct receive_queue *rq,
				 struct sk_buff *skb, u8 flags);
static struct sk_buff *virtnet_skb_append_frag(struct sk_buff *head_skb,
					       struct sk_buff *curr_skb,
					       struct page *page, void *buf,
					       int len, int truesize);
static void virtnet_xsk_completed(struct send_queue *sq, int num);

enum virtnet_xmit_type {
	VIRTNET_XMIT_TYPE_SKB,
	VIRTNET_XMIT_TYPE_SKB_ORPHAN,
	VIRTNET_XMIT_TYPE_XDP,
	VIRTNET_XMIT_TYPE_XSK,
};

static size_t virtnet_rss_hdr_size(const struct virtnet_info *vi)
{
	u16 indir_table_size = vi->has_rss ? vi->rss_indir_table_size : 1;

	return struct_size(vi->rss_hdr, indirection_table, indir_table_size);
}

static size_t virtnet_rss_trailer_size(const struct virtnet_info *vi)
{
	return struct_size(&vi->rss_trailer, hash_key_data, vi->rss_key_size);
}

/* We use the last two bits of the pointer to distinguish the xmit type. */
#define VIRTNET_XMIT_TYPE_MASK (BIT(0) | BIT(1))

#define VIRTIO_XSK_FLAG_OFFSET 2

static enum virtnet_xmit_type virtnet_xmit_ptr_unpack(void **ptr)
{
	unsigned long p = (unsigned long)*ptr;

	*ptr = (void *)(p & ~VIRTNET_XMIT_TYPE_MASK);

	return p & VIRTNET_XMIT_TYPE_MASK;
}

static void *virtnet_xmit_ptr_pack(void *ptr, enum virtnet_xmit_type type)
{
	return (void *)((unsigned long)ptr | type);
}

static int virtnet_add_outbuf(struct send_queue *sq, int num, void *data,
			      enum virtnet_xmit_type type)
{
	return virtqueue_add_outbuf(sq->vq, sq->sg, num,
				    virtnet_xmit_ptr_pack(data, type),
				    GFP_ATOMIC);
}

static u32 virtnet_ptr_to_xsk_buff_len(void *ptr)
{
	return ((unsigned long)ptr) >> VIRTIO_XSK_FLAG_OFFSET;
}

static void sg_fill_dma(struct scatterlist *sg, dma_addr_t addr, u32 len)
{
	sg_dma_address(sg) = addr;
	sg_dma_len(sg) = len;
}

static void __free_old_xmit(struct send_queue *sq, struct netdev_queue *txq,
			    bool in_napi, struct virtnet_sq_free_stats *stats)
{
	struct xdp_frame *frame;
	struct sk_buff *skb;
	unsigned int len;
	void *ptr;

	while ((ptr = virtqueue_get_buf(sq->vq, &len)) != NULL) {
		switch (virtnet_xmit_ptr_unpack(&ptr)) {
		case VIRTNET_XMIT_TYPE_SKB:
			skb = ptr;

			pr_debug("Sent skb %p\n", skb);
			stats->napi_packets++;
			stats->napi_bytes += skb->len;
			napi_consume_skb(skb, in_napi);
			break;

		case VIRTNET_XMIT_TYPE_SKB_ORPHAN:
			skb = ptr;

			stats->packets++;
			stats->bytes += skb->len;
			napi_consume_skb(skb, in_napi);
			break;

		case VIRTNET_XMIT_TYPE_XDP:
			frame = ptr;

			stats->packets++;
			stats->bytes += xdp_get_frame_len(frame);
			xdp_return_frame(frame);
			break;

		case VIRTNET_XMIT_TYPE_XSK:
			stats->bytes += virtnet_ptr_to_xsk_buff_len(ptr);
			stats->xsk++;
			break;
		}
	}
	netdev_tx_completed_queue(txq, stats->napi_packets, stats->napi_bytes);
}

static void virtnet_free_old_xmit(struct send_queue *sq,
				  struct netdev_queue *txq,
				  bool in_napi,
				  struct virtnet_sq_free_stats *stats)
{
	__free_old_xmit(sq, txq, in_napi, stats);

	if (stats->xsk)
		virtnet_xsk_completed(sq, stats->xsk);
}

/* Converting between virtqueue no. and kernel tx/rx queue no.
 * 0:rx0 1:tx0 2:rx1 3:tx1 ... 2N:rxN 2N+1:txN 2N+2:cvq
 */
static int vq2txq(struct virtqueue *vq)
{
	return (vq->index - 1) / 2;
}

static int txq2vq(int txq)
{
	return txq * 2 + 1;
}

static int vq2rxq(struct virtqueue *vq)
{
	return vq->index / 2;
}

static int rxq2vq(int rxq)
{
	return rxq * 2;
}

static int vq_type(struct virtnet_info *vi, int qid)
{
	if (qid == vi->max_queue_pairs * 2)
		return VIRTNET_Q_TYPE_CQ;

	if (qid % 2)
		return VIRTNET_Q_TYPE_TX;

	return VIRTNET_Q_TYPE_RX;
}

static inline struct virtio_net_common_hdr *
skb_vnet_common_hdr(struct sk_buff *skb)
{
	return (struct virtio_net_common_hdr *)skb->cb;
}

/*
 * private is used to chain pages for big packets, put the whole
 * most recent used list in the beginning for reuse
 */
static void give_pages(struct receive_queue *rq, struct page *page)
{
	struct page *end;

	/* Find end of list, sew whole thing into vi->rq.pages. */
	for (end = page; end->private; end = (struct page *)end->private);
	end->private = (unsigned long)rq->pages;
	rq->pages = page;
}

static struct page *get_a_page(struct receive_queue *rq, gfp_t gfp_mask)
{
	struct page *p = rq->pages;

	if (p) {
		rq->pages = (struct page *)p->private;
		/* clear private here, it is used to chain pages */
		p->private = 0;
	} else
		p = alloc_page(gfp_mask);
	return p;
}

static void virtnet_rq_free_buf(struct virtnet_info *vi,
				struct receive_queue *rq, void *buf)
{
	if (vi->mergeable_rx_bufs)
		put_page(virt_to_head_page(buf));
	else if (vi->big_packets)
		give_pages(rq, buf);
	else
		put_page(virt_to_head_page(buf));
}

static void enable_delayed_refill(struct virtnet_info *vi)
{
	spin_lock_bh(&vi->refill_lock);
	vi->refill_enabled = true;
	spin_unlock_bh(&vi->refill_lock);
}

static void disable_delayed_refill(struct virtnet_info *vi)
{
	spin_lock_bh(&vi->refill_lock);
	vi->refill_enabled = false;
	spin_unlock_bh(&vi->refill_lock);
}

static void enable_rx_mode_work(struct virtnet_info *vi)
{
	rtnl_lock();
	vi->rx_mode_work_enabled = true;
	rtnl_unlock();
}

static void disable_rx_mode_work(struct virtnet_info *vi)
{
	rtnl_lock();
	vi->rx_mode_work_enabled = false;
	rtnl_unlock();
}

static void virtqueue_napi_schedule(struct napi_struct *napi,
				    struct virtqueue *vq)
{
	if (napi_schedule_prep(napi)) {
		virtqueue_disable_cb(vq);
		__napi_schedule(napi);
	}
}

static bool virtqueue_napi_complete(struct napi_struct *napi,
				    struct virtqueue *vq, int processed)
{
	int opaque;

	opaque = virtqueue_enable_cb_prepare(vq);
	if (napi_complete_done(napi, processed)) {
		if (unlikely(virtqueue_poll(vq, opaque)))
			virtqueue_napi_schedule(napi, vq);
		else
			return true;
	} else {
		virtqueue_disable_cb(vq);
	}

	return false;
}

static void skb_xmit_done(struct virtqueue *vq)
{
	struct virtnet_info *vi = vq->vdev->priv;
	struct napi_struct *napi = &vi->sq[vq2txq(vq)].napi;

	/* Suppress further interrupts. */
	virtqueue_disable_cb(vq);

	if (napi->weight)
		virtqueue_napi_schedule(napi, vq);
	else
		/* We were probably waiting for more output buffers. */
		netif_wake_subqueue(vi->dev, vq2txq(vq));
}

#define MRG_CTX_HEADER_SHIFT 22
static void *mergeable_len_to_ctx(unsigned int truesize,
				  unsigned int headroom)
{
	return (void *)(unsigned long)((headroom << MRG_CTX_HEADER_SHIFT) | truesize);
}

static unsigned int mergeable_ctx_to_headroom(void *mrg_ctx)
{
	return (unsigned long)mrg_ctx >> MRG_CTX_HEADER_SHIFT;
}

static unsigned int mergeable_ctx_to_truesize(void *mrg_ctx)
{
	return (unsigned long)mrg_ctx & ((1 << MRG_CTX_HEADER_SHIFT) - 1);
}

static int check_mergeable_len(struct net_device *dev, void *mrg_ctx,
			       unsigned int len)
{
	unsigned int headroom, tailroom, room, truesize;

	truesize = mergeable_ctx_to_truesize(mrg_ctx);
	headroom = mergeable_ctx_to_headroom(mrg_ctx);
	tailroom = headroom ? sizeof(struct skb_shared_info) : 0;
	room = SKB_DATA_ALIGN(headroom + tailroom);

	if (len > truesize - room) {
		pr_debug("%s: rx error: len %u exceeds truesize %lu\n",
			 dev->name, len, (unsigned long)(truesize - room));
		DEV_STATS_INC(dev, rx_length_errors);
		return -1;
	}

	return 0;
}

static struct sk_buff *virtnet_build_skb(void *buf, unsigned int buflen,
					 unsigned int headroom,
					 unsigned int len)
{
	struct sk_buff *skb;

	skb = build_skb(buf, buflen);
	if (unlikely(!skb))
		return NULL;

	skb_reserve(skb, headroom);
	skb_put(skb, len);

	return skb;
}

/* Called from bottom half context */
static struct sk_buff *page_to_skb(struct virtnet_info *vi,
				   struct receive_queue *rq,
				   struct page *page, unsigned int offset,
				   unsigned int len, unsigned int truesize,
				   unsigned int headroom)
{
	struct sk_buff *skb;
	struct virtio_net_common_hdr *hdr;
	unsigned int copy, hdr_len, hdr_padded_len;
	struct page *page_to_free = NULL;
	int tailroom, shinfo_size;
	char *p, *hdr_p, *buf;

	p = page_address(page) + offset;
	hdr_p = p;

	hdr_len = vi->hdr_len;
	if (vi->mergeable_rx_bufs)
		hdr_padded_len = hdr_len;
	else
		hdr_padded_len = sizeof(struct padded_vnet_hdr);

	buf = p - headroom;
	len -= hdr_len;
	offset += hdr_padded_len;
	p += hdr_padded_len;
	tailroom = truesize - headroom  - hdr_padded_len - len;

	shinfo_size = SKB_DATA_ALIGN(sizeof(struct skb_shared_info));

	if (!NET_IP_ALIGN && len > GOOD_COPY_LEN && tailroom >= shinfo_size) {
		skb = virtnet_build_skb(buf, truesize, p - buf, len);
		if (unlikely(!skb))
			return NULL;

		page = (struct page *)page->private;
		if (page)
			give_pages(rq, page);
		goto ok;
	}

	/* copy small packet so we can reuse these pages for small data */
	skb = napi_alloc_skb(&rq->napi, GOOD_COPY_LEN);
	if (unlikely(!skb))
		return NULL;

	/* Copy all frame if it fits skb->head, otherwise
	 * we let virtio_net_hdr_to_skb() and GRO pull headers as needed.
	 */
	if (len <= skb_tailroom(skb))
		copy = len;
	else
		copy = ETH_HLEN;
	skb_put_data(skb, p, copy);

	len -= copy;
	offset += copy;

	if (vi->mergeable_rx_bufs) {
		if (len)
			skb_add_rx_frag(skb, 0, page, offset, len, truesize);
		else
			page_to_free = page;
		goto ok;
	}

	/*
	 * Verify that we can indeed put this data into a skb.
	 * This is here to handle cases when the device erroneously
	 * tries to receive more than is possible. This is usually
	 * the case of a broken device.
	 */
	if (unlikely(len > MAX_SKB_FRAGS * PAGE_SIZE)) {
		net_dbg_ratelimited("%s: too much data\n", skb->dev->name);
		dev_kfree_skb(skb);
		return NULL;
	}
	BUG_ON(offset >= PAGE_SIZE);
	while (len) {
		unsigned int frag_size = min((unsigned)PAGE_SIZE - offset, len);
		skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags, page, offset,
				frag_size, truesize);
		len -= frag_size;
		page = (struct page *)page->private;
		offset = 0;
	}

	if (page)
		give_pages(rq, page);

ok:
	hdr = skb_vnet_common_hdr(skb);
	memcpy(hdr, hdr_p, hdr_len);
	if (page_to_free)
		put_page(page_to_free);

	return skb;
}

static void virtnet_rq_unmap(struct receive_queue *rq, void *buf, u32 len)
{
	struct virtnet_info *vi = rq->vq->vdev->priv;
	struct page *page = virt_to_head_page(buf);
	struct virtnet_rq_dma *dma;
	void *head;
	int offset;

	BUG_ON(vi->big_packets && !vi->mergeable_rx_bufs);

	head = page_address(page);

	dma = head;

	--dma->ref;

	if (dma->need_sync && len) {
		offset = buf - (head + sizeof(*dma));

		virtqueue_dma_sync_single_range_for_cpu(rq->vq, dma->addr,
							offset, len,
							DMA_FROM_DEVICE);
	}

	if (dma->ref)
		return;

	virtqueue_dma_unmap_single_attrs(rq->vq, dma->addr, dma->len,
					 DMA_FROM_DEVICE, DMA_ATTR_SKIP_CPU_SYNC);
	put_page(page);
}

static void *virtnet_rq_get_buf(struct receive_queue *rq, u32 *len, void **ctx)
{
	struct virtnet_info *vi = rq->vq->vdev->priv;
	void *buf;

	BUG_ON(vi->big_packets && !vi->mergeable_rx_bufs);

	buf = virtqueue_get_buf_ctx(rq->vq, len, ctx);
	if (buf)
		virtnet_rq_unmap(rq, buf, *len);

	return buf;
}

static void virtnet_rq_init_one_sg(struct receive_queue *rq, void *buf, u32 len)
{
	struct virtnet_info *vi = rq->vq->vdev->priv;
	struct virtnet_rq_dma *dma;
	dma_addr_t addr;
	u32 offset;
	void *head;

	BUG_ON(vi->big_packets && !vi->mergeable_rx_bufs);

	head = page_address(rq->alloc_frag.page);

	offset = buf - head;

	dma = head;

	addr = dma->addr - sizeof(*dma) + offset;

	sg_init_table(rq->sg, 1);
	sg_fill_dma(rq->sg, addr, len);
}

static void *virtnet_rq_alloc(struct receive_queue *rq, u32 size, gfp_t gfp)
{
	struct page_frag *alloc_frag = &rq->alloc_frag;
	struct virtnet_info *vi = rq->vq->vdev->priv;
	struct virtnet_rq_dma *dma;
	void *buf, *head;
	dma_addr_t addr;

	BUG_ON(vi->big_packets && !vi->mergeable_rx_bufs);

	head = page_address(alloc_frag->page);

	dma = head;

	/* new pages */
	if (!alloc_frag->offset) {
		if (rq->last_dma) {
			/* Now, the new page is allocated, the last dma
			 * will not be used. So the dma can be unmapped
			 * if the ref is 0.
			 */
			virtnet_rq_unmap(rq, rq->last_dma, 0);
			rq->last_dma = NULL;
		}

		dma->len = alloc_frag->size - sizeof(*dma);

		addr = virtqueue_dma_map_single_attrs(rq->vq, dma + 1,
						      dma->len, DMA_FROM_DEVICE, 0);
		if (virtqueue_dma_mapping_error(rq->vq, addr))
			return NULL;

		dma->addr = addr;
		dma->need_sync = virtqueue_dma_need_sync(rq->vq, addr);

		/* Add a reference to dma to prevent the entire dma from
		 * being released during error handling. This reference
		 * will be freed after the pages are no longer used.
		 */
		get_page(alloc_frag->page);
		dma->ref = 1;
		alloc_frag->offset = sizeof(*dma);

		rq->last_dma = dma;
	}

	++dma->ref;

	buf = head + alloc_frag->offset;

	get_page(alloc_frag->page);
	alloc_frag->offset += size;

	return buf;
}

static void virtnet_rq_unmap_free_buf(struct virtqueue *vq, void *buf)
{
	struct virtnet_info *vi = vq->vdev->priv;
	struct receive_queue *rq;
	int i = vq2rxq(vq);

	rq = &vi->rq[i];

	if (rq->xsk_pool) {
		xsk_buff_free((struct xdp_buff *)buf);
		return;
	}

	if (!vi->big_packets || vi->mergeable_rx_bufs)
		virtnet_rq_unmap(rq, buf, 0);

	virtnet_rq_free_buf(vi, rq, buf);
}

static void free_old_xmit(struct send_queue *sq, struct netdev_queue *txq,
			  bool in_napi)
{
	struct virtnet_sq_free_stats stats = {0};

	virtnet_free_old_xmit(sq, txq, in_napi, &stats);

	/* Avoid overhead when no packets have been processed
	 * happens when called speculatively from start_xmit.
	 */
	if (!stats.packets && !stats.napi_packets)
		return;

	u64_stats_update_begin(&sq->stats.syncp);
	u64_stats_add(&sq->stats.bytes, stats.bytes + stats.napi_bytes);
	u64_stats_add(&sq->stats.packets, stats.packets + stats.napi_packets);
	u64_stats_update_end(&sq->stats.syncp);
}

static bool is_xdp_raw_buffer_queue(struct virtnet_info *vi, int q)
{
	if (q < (vi->curr_queue_pairs - vi->xdp_queue_pairs))
		return false;
	else if (q < vi->curr_queue_pairs)
		return true;
	else
		return false;
}

static bool tx_may_stop(struct virtnet_info *vi,
			struct net_device *dev,
			struct send_queue *sq)
{
	int qnum;

	qnum = sq - vi->sq;

	/* If running out of space, stop queue to avoid getting packets that we
	 * are then unable to transmit.
	 * An alternative would be to force queuing layer to requeue the skb by
	 * returning NETDEV_TX_BUSY. However, NETDEV_TX_BUSY should not be
	 * returned in a normal path of operation: it means that driver is not
	 * maintaining the TX queue stop/start state properly, and causes
	 * the stack to do a non-trivial amount of useless work.
	 * Since most packets only take 1 or 2 ring slots, stopping the queue
	 * early means 16 slots are typically wasted.
	 */
	if (sq->vq->num_free < 2+MAX_SKB_FRAGS) {
		struct netdev_queue *txq = netdev_get_tx_queue(dev, qnum);

		netif_tx_stop_queue(txq);
		u64_stats_update_begin(&sq->stats.syncp);
		u64_stats_inc(&sq->stats.stop);
		u64_stats_update_end(&sq->stats.syncp);

		return true;
	}

	return false;
}

static void check_sq_full_and_disable(struct virtnet_info *vi,
				      struct net_device *dev,
				      struct send_queue *sq)
{
	bool use_napi = sq->napi.weight;
	int qnum;

	qnum = sq - vi->sq;

	if (tx_may_stop(vi, dev, sq)) {
		struct netdev_queue *txq = netdev_get_tx_queue(dev, qnum);

		if (use_napi) {
			if (unlikely(!virtqueue_enable_cb_delayed(sq->vq)))
				virtqueue_napi_schedule(&sq->napi, sq->vq);
		} else if (unlikely(!virtqueue_enable_cb_delayed(sq->vq))) {
			/* More just got used, free them then recheck. */
			free_old_xmit(sq, txq, false);
			if (sq->vq->num_free >= 2+MAX_SKB_FRAGS) {
				netif_start_subqueue(dev, qnum);
				u64_stats_update_begin(&sq->stats.syncp);
				u64_stats_inc(&sq->stats.wake);
				u64_stats_update_end(&sq->stats.syncp);
				virtqueue_disable_cb(sq->vq);
			}
		}
	}
}

/* Note that @len is the length of received data without virtio header */
static struct xdp_buff *buf_to_xdp(struct virtnet_info *vi,
				   struct receive_queue *rq, void *buf,
				   u32 len, bool first_buf)
{
	struct xdp_buff *xdp;
	u32 bufsize;

	xdp = (struct xdp_buff *)buf;

	/* In virtnet_add_recvbuf_xsk, we use part of XDP_PACKET_HEADROOM for
	 * virtio header and ask the vhost to fill data from
	 *         hard_start + XDP_PACKET_HEADROOM - vi->hdr_len
	 * The first buffer has virtio header so the remaining region for frame
	 * data is
	 *         xsk_pool_get_rx_frame_size()
	 * While other buffers than the first one do not have virtio header, so
	 * the maximum frame data's length can be
	 *         xsk_pool_get_rx_frame_size() + vi->hdr_len
	 */
	bufsize = xsk_pool_get_rx_frame_size(rq->xsk_pool);
	if (!first_buf)
		bufsize += vi->hdr_len;

	if (unlikely(len > bufsize)) {
		pr_debug("%s: rx error: len %u exceeds truesize %u\n",
			 vi->dev->name, len, bufsize);
		DEV_STATS_INC(vi->dev, rx_length_errors);
		xsk_buff_free(xdp);
		return NULL;
	}

	xsk_buff_set_size(xdp, len);
	xsk_buff_dma_sync_for_cpu(xdp);

	return xdp;
}

static struct sk_buff *xsk_construct_skb(struct receive_queue *rq,
					 struct xdp_buff *xdp)
{
	unsigned int metasize = xdp->data - xdp->data_meta;
	struct sk_buff *skb;
	unsigned int size;

	size = xdp->data_end - xdp->data_hard_start;
	skb = napi_alloc_skb(&rq->napi, size);
	if (unlikely(!skb)) {
		xsk_buff_free(xdp);
		return NULL;
	}

	skb_reserve(skb, xdp->data_meta - xdp->data_hard_start);

	size = xdp->data_end - xdp->data_meta;
	memcpy(__skb_put(skb, size), xdp->data_meta, size);

	if (metasize) {
		__skb_pull(skb, metasize);
		skb_metadata_set(skb, metasize);
	}

	xsk_buff_free(xdp);

	return skb;
}

static struct sk_buff *virtnet_receive_xsk_small(struct net_device *dev, struct virtnet_info *vi,
						 struct receive_queue *rq, struct xdp_buff *xdp,
						 unsigned int *xdp_xmit,
						 struct virtnet_rq_stats *stats)
{
	struct bpf_prog *prog;
	u32 ret;

	ret = XDP_PASS;
	rcu_read_lock();
	prog = rcu_dereference(rq->xdp_prog);
	if (prog)
		ret = virtnet_xdp_handler(prog, xdp, dev, xdp_xmit, stats);
	rcu_read_unlock();

	switch (ret) {
	case XDP_PASS:
		return xsk_construct_skb(rq, xdp);

	case XDP_TX:
	case XDP_REDIRECT:
		return NULL;

	default:
		/* drop packet */
		xsk_buff_free(xdp);
		u64_stats_inc(&stats->drops);
		return NULL;
	}
}

static void xsk_drop_follow_bufs(struct net_device *dev,
				 struct receive_queue *rq,
				 u32 num_buf,
				 struct virtnet_rq_stats *stats)
{
	struct xdp_buff *xdp;
	u32 len;

	while (num_buf-- > 1) {
		xdp = virtqueue_get_buf(rq->vq, &len);
		if (unlikely(!xdp)) {
			pr_debug("%s: rx error: %d buffers missing\n",
				 dev->name, num_buf);
			DEV_STATS_INC(dev, rx_length_errors);
			break;
		}
		u64_stats_add(&stats->bytes, len);
		xsk_buff_free(xdp);
	}
}

static int xsk_append_merge_buffer(struct virtnet_info *vi,
				   struct receive_queue *rq,
				   struct sk_buff *head_skb,
				   u32 num_buf,
				   struct virtio_net_hdr_mrg_rxbuf *hdr,
				   struct virtnet_rq_stats *stats)
{
	struct sk_buff *curr_skb;
	struct xdp_buff *xdp;
	u32 len, truesize;
	struct page *page;
	void *buf;

	curr_skb = head_skb;

	while (--num_buf) {
		buf = virtqueue_get_buf(rq->vq, &len);
		if (unlikely(!buf)) {
			pr_debug("%s: rx error: %d buffers out of %d missing\n",
				 vi->dev->name, num_buf,
				 virtio16_to_cpu(vi->vdev,
						 hdr->num_buffers));
			DEV_STATS_INC(vi->dev, rx_length_errors);
			return -EINVAL;
		}

		u64_stats_add(&stats->bytes, len);

		xdp = buf_to_xdp(vi, rq, buf, len, false);
		if (!xdp)
			goto err;

		buf = napi_alloc_frag(len);
		if (!buf) {
			xsk_buff_free(xdp);
			goto err;
		}

		memcpy(buf, xdp->data - vi->hdr_len, len);

		xsk_buff_free(xdp);

		page = virt_to_page(buf);

		truesize = len;

		curr_skb  = virtnet_skb_append_frag(head_skb, curr_skb, page,
						    buf, len, truesize);
		if (!curr_skb) {
			put_page(page);
			goto err;
		}
	}

	return 0;

err:
	xsk_drop_follow_bufs(vi->dev, rq, num_buf, stats);
	return -EINVAL;
}

static struct sk_buff *virtnet_receive_xsk_merge(struct net_device *dev, struct virtnet_info *vi,
						 struct receive_queue *rq, struct xdp_buff *xdp,
						 unsigned int *xdp_xmit,
						 struct virtnet_rq_stats *stats)
{
	struct virtio_net_hdr_mrg_rxbuf *hdr;
	struct bpf_prog *prog;
	struct sk_buff *skb;
	u32 ret, num_buf;

	hdr = xdp->data - vi->hdr_len;
	num_buf = virtio16_to_cpu(vi->vdev, hdr->num_buffers);

	ret = XDP_PASS;
	rcu_read_lock();
	prog = rcu_dereference(rq->xdp_prog);
	/* TODO: support multi buffer. */
	if (prog && num_buf == 1)
		ret = virtnet_xdp_handler(prog, xdp, dev, xdp_xmit, stats);
	rcu_read_unlock();

	switch (ret) {
	case XDP_PASS:
		skb = xsk_construct_skb(rq, xdp);
		if (!skb)
			goto drop_bufs;

		if (xsk_append_merge_buffer(vi, rq, skb, num_buf, hdr, stats)) {
			dev_kfree_skb(skb);
			goto drop;
		}

		return skb;

	case XDP_TX:
	case XDP_REDIRECT:
		return NULL;

	default:
		/* drop packet */
		xsk_buff_free(xdp);
	}

drop_bufs:
	xsk_drop_follow_bufs(dev, rq, num_buf, stats);

drop:
	u64_stats_inc(&stats->drops);
	return NULL;
}

static void virtnet_receive_xsk_buf(struct virtnet_info *vi, struct receive_queue *rq,
				    void *buf, u32 len,
				    unsigned int *xdp_xmit,
				    struct virtnet_rq_stats *stats)
{
	struct net_device *dev = vi->dev;
	struct sk_buff *skb = NULL;
	struct xdp_buff *xdp;
	u8 flags;

	len -= vi->hdr_len;

	u64_stats_add(&stats->bytes, len);

	xdp = buf_to_xdp(vi, rq, buf, len, true);
	if (!xdp)
		return;

	if (unlikely(len < ETH_HLEN)) {
		pr_debug("%s: short packet %i\n", dev->name, len);
		DEV_STATS_INC(dev, rx_length_errors);
		xsk_buff_free(xdp);
		return;
	}

	flags = ((struct virtio_net_common_hdr *)(xdp->data - vi->hdr_len))->hdr.flags;

	if (!vi->mergeable_rx_bufs)
		skb = virtnet_receive_xsk_small(dev, vi, rq, xdp, xdp_xmit, stats);
	else
		skb = virtnet_receive_xsk_merge(dev, vi, rq, xdp, xdp_xmit, stats);

	if (skb)
		virtnet_receive_done(vi, rq, skb, flags);
}

static int virtnet_add_recvbuf_xsk(struct virtnet_info *vi, struct receive_queue *rq,
				   struct xsk_buff_pool *pool, gfp_t gfp)
{
	struct xdp_buff **xsk_buffs;
	dma_addr_t addr;
	int err = 0;
	u32 len, i;
	int num;

	xsk_buffs = rq->xsk_buffs;

	num = xsk_buff_alloc_batch(pool, xsk_buffs, rq->vq->num_free);
	if (!num)
		return -ENOMEM;

	len = xsk_pool_get_rx_frame_size(pool) + vi->hdr_len;

	for (i = 0; i < num; ++i) {
		/* Use the part of XDP_PACKET_HEADROOM as the virtnet hdr space.
		 * We assume XDP_PACKET_HEADROOM is larger than hdr->len.
		 * (see function virtnet_xsk_pool_enable)
		 */
		addr = xsk_buff_xdp_get_dma(xsk_buffs[i]) - vi->hdr_len;

		sg_init_table(rq->sg, 1);
		sg_fill_dma(rq->sg, addr, len);

		err = virtqueue_add_inbuf_premapped(rq->vq, rq->sg, 1,
						    xsk_buffs[i], NULL, gfp);
		if (err)
			goto err;
	}

	return num;

err:
	for (; i < num; ++i)
		xsk_buff_free(xsk_buffs[i]);

	return err;
}

static void *virtnet_xsk_to_ptr(u32 len)
{
	unsigned long p;

	p = len << VIRTIO_XSK_FLAG_OFFSET;

	return virtnet_xmit_ptr_pack((void *)p, VIRTNET_XMIT_TYPE_XSK);
}

static int virtnet_xsk_xmit_one(struct send_queue *sq,
				struct xsk_buff_pool *pool,
				struct xdp_desc *desc)
{
	struct virtnet_info *vi;
	dma_addr_t addr;

	vi = sq->vq->vdev->priv;

	addr = xsk_buff_raw_get_dma(pool, desc->addr);
	xsk_buff_raw_dma_sync_for_device(pool, addr, desc->len);

	sg_init_table(sq->sg, 2);
	sg_fill_dma(sq->sg, sq->xsk_hdr_dma_addr, vi->hdr_len);
	sg_fill_dma(sq->sg + 1, addr, desc->len);

	return virtqueue_add_outbuf_premapped(sq->vq, sq->sg, 2,
					      virtnet_xsk_to_ptr(desc->len),
					      GFP_ATOMIC);
}

static int virtnet_xsk_xmit_batch(struct send_queue *sq,
				  struct xsk_buff_pool *pool,
				  unsigned int budget,
				  u64 *kicks)
{
	struct xdp_desc *descs = pool->tx_descs;
	bool kick = false;
	u32 nb_pkts, i;
	int err;

	budget = min_t(u32, budget, sq->vq->num_free);

	nb_pkts = xsk_tx_peek_release_desc_batch(pool, budget);
	if (!nb_pkts)
		return 0;

	for (i = 0; i < nb_pkts; i++) {
		err = virtnet_xsk_xmit_one(sq, pool, &descs[i]);
		if (unlikely(err)) {
			xsk_tx_completed(sq->xsk_pool, nb_pkts - i);
			break;
		}

		kick = true;
	}

	if (kick && virtqueue_kick_prepare(sq->vq) && virtqueue_notify(sq->vq))
		(*kicks)++;

	return i;
}

static bool virtnet_xsk_xmit(struct send_queue *sq, struct xsk_buff_pool *pool,
			     int budget)
{
	struct virtnet_info *vi = sq->vq->vdev->priv;
	struct virtnet_sq_free_stats stats = {};
	struct net_device *dev = vi->dev;
	u64 kicks = 0;
	int sent;

	/* Avoid to wakeup napi meanless, so call __free_old_xmit instead of
	 * free_old_xmit().
	 */
	__free_old_xmit(sq, netdev_get_tx_queue(dev, sq - vi->sq), true, &stats);

	if (stats.xsk)
		xsk_tx_completed(sq->xsk_pool, stats.xsk);

	sent = virtnet_xsk_xmit_batch(sq, pool, budget, &kicks);

	if (!is_xdp_raw_buffer_queue(vi, sq - vi->sq))
		check_sq_full_and_disable(vi, vi->dev, sq);

	if (sent) {
		struct netdev_queue *txq;

		txq = netdev_get_tx_queue(vi->dev, sq - vi->sq);
		txq_trans_cond_update(txq);
	}

	u64_stats_update_begin(&sq->stats.syncp);
	u64_stats_add(&sq->stats.packets, stats.packets);
	u64_stats_add(&sq->stats.bytes,   stats.bytes);
	u64_stats_add(&sq->stats.kicks,   kicks);
	u64_stats_add(&sq->stats.xdp_tx,  sent);
	u64_stats_update_end(&sq->stats.syncp);

	if (xsk_uses_need_wakeup(pool))
		xsk_set_tx_need_wakeup(pool);

	return sent;
}

static void xsk_wakeup(struct send_queue *sq)
{
	if (napi_if_scheduled_mark_missed(&sq->napi))
		return;

	local_bh_disable();
	virtqueue_napi_schedule(&sq->napi, sq->vq);
	local_bh_enable();
}

static int virtnet_xsk_wakeup(struct net_device *dev, u32 qid, u32 flag)
{
	struct virtnet_info *vi = netdev_priv(dev);
	struct send_queue *sq;

	if (!netif_running(dev))
		return -ENETDOWN;

	if (qid >= vi->curr_queue_pairs)
		return -EINVAL;

	sq = &vi->sq[qid];

	xsk_wakeup(sq);
	return 0;
}

static void virtnet_xsk_completed(struct send_queue *sq, int num)
{
	xsk_tx_completed(sq->xsk_pool, num);

	/* If this is called by rx poll, start_xmit and xdp xmit we should
	 * wakeup the tx napi to consume the xsk tx queue, because the tx
	 * interrupt may not be triggered.
	 */
	xsk_wakeup(sq);
}

static int __virtnet_xdp_xmit_one(struct virtnet_info *vi,
				   struct send_queue *sq,
				   struct xdp_frame *xdpf)
{
	struct virtio_net_hdr_mrg_rxbuf *hdr;
	struct skb_shared_info *shinfo;
	u8 nr_frags = 0;
	int err, i;

	if (unlikely(xdpf->headroom < vi->hdr_len))
		return -EOVERFLOW;

	if (unlikely(xdp_frame_has_frags(xdpf))) {
		shinfo = xdp_get_shared_info_from_frame(xdpf);
		nr_frags = shinfo->nr_frags;
	}

	/* In wrapping function virtnet_xdp_xmit(), we need to free
	 * up the pending old buffers, where we need to calculate the
	 * position of skb_shared_info in xdp_get_frame_len() and
	 * xdp_return_frame(), which will involve to xdpf->data and
	 * xdpf->headroom. Therefore, we need to update the value of
	 * headroom synchronously here.
	 */
	xdpf->headroom -= vi->hdr_len;
	xdpf->data -= vi->hdr_len;
	/* Zero header and leave csum up to XDP layers */
	hdr = xdpf->data;
	memset(hdr, 0, vi->hdr_len);
	xdpf->len   += vi->hdr_len;

	sg_init_table(sq->sg, nr_frags + 1);
	sg_set_buf(sq->sg, xdpf->data, xdpf->len);
	for (i = 0; i < nr_frags; i++) {
		skb_frag_t *frag = &shinfo->frags[i];

		sg_set_page(&sq->sg[i + 1], skb_frag_page(frag),
			    skb_frag_size(frag), skb_frag_off(frag));
	}

	err = virtnet_add_outbuf(sq, nr_frags + 1, xdpf, VIRTNET_XMIT_TYPE_XDP);
	if (unlikely(err))
		return -ENOSPC; /* Caller handle free/refcnt */

	return 0;
}

/* when vi->curr_queue_pairs > nr_cpu_ids, the txq/sq is only used for xdp tx on
 * the current cpu, so it does not need to be locked.
 *
 * Here we use marco instead of inline functions because we have to deal with
 * three issues at the same time: 1. the choice of sq. 2. judge and execute the
 * lock/unlock of txq 3. make sparse happy. It is difficult for two inline
 * functions to perfectly solve these three problems at the same time.
 */
#define virtnet_xdp_get_sq(vi) ({                                       \
	int cpu = smp_processor_id();                                   \
	struct netdev_queue *txq;                                       \
	typeof(vi) v = (vi);                                            \
	unsigned int qp;                                                \
									\
	if (v->curr_queue_pairs > nr_cpu_ids) {                         \
		qp = v->curr_queue_pairs - v->xdp_queue_pairs;          \
		qp += cpu;                                              \
		txq = netdev_get_tx_queue(v->dev, qp);                  \
		__netif_tx_acquire(txq);                                \
	} else {                                                        \
		qp = cpu % v->curr_queue_pairs;                         \
		txq = netdev_get_tx_queue(v->dev, qp);                  \
		__netif_tx_lock(txq, cpu);                              \
	}                                                               \
	v->sq + qp;                                                     \
})

#define virtnet_xdp_put_sq(vi, q) {                                     \
	struct netdev_queue *txq;                                       \
	typeof(vi) v = (vi);                                            \
									\
	txq = netdev_get_tx_queue(v->dev, (q) - v->sq);                 \
	if (v->curr_queue_pairs > nr_cpu_ids)                           \
		__netif_tx_release(txq);                                \
	else                                                            \
		__netif_tx_unlock(txq);                                 \
}

static int virtnet_xdp_xmit(struct net_device *dev,
			    int n, struct xdp_frame **frames, u32 flags)
{
	struct virtnet_info *vi = netdev_priv(dev);
	struct virtnet_sq_free_stats stats = {0};
	struct receive_queue *rq = vi->rq;
	struct bpf_prog *xdp_prog;
	struct send_queue *sq;
	int nxmit = 0;
	int kicks = 0;
	int ret;
	int i;

	/* Only allow ndo_xdp_xmit if XDP is loaded on dev, as this
	 * indicate XDP resources have been successfully allocated.
	 */
	xdp_prog = rcu_access_pointer(rq->xdp_prog);
	if (!xdp_prog)
		return -ENXIO;

	sq = virtnet_xdp_get_sq(vi);

	if (unlikely(flags & ~XDP_XMIT_FLAGS_MASK)) {
		ret = -EINVAL;
		goto out;
	}

	/* Free up any pending old buffers before queueing new ones. */
	virtnet_free_old_xmit(sq, netdev_get_tx_queue(dev, sq - vi->sq),
			      false, &stats);

	for (i = 0; i < n; i++) {
		struct xdp_frame *xdpf = frames[i];

		if (__virtnet_xdp_xmit_one(vi, sq, xdpf))
			break;
		nxmit++;
	}
	ret = nxmit;

	if (!is_xdp_raw_buffer_queue(vi, sq - vi->sq))
		check_sq_full_and_disable(vi, dev, sq);

	if (flags & XDP_XMIT_FLUSH) {
		if (virtqueue_kick_prepare(sq->vq) && virtqueue_notify(sq->vq))
			kicks = 1;
	}
out:
	u64_stats_update_begin(&sq->stats.syncp);
	u64_stats_add(&sq->stats.bytes, stats.bytes);
	u64_stats_add(&sq->stats.packets, stats.packets);
	u64_stats_add(&sq->stats.xdp_tx, n);
	u64_stats_add(&sq->stats.xdp_tx_drops, n - nxmit);
	u64_stats_add(&sq->stats.kicks, kicks);
	u64_stats_update_end(&sq->stats.syncp);

	virtnet_xdp_put_sq(vi, sq);
	return ret;
}

static void put_xdp_frags(struct xdp_buff *xdp)
{
	struct skb_shared_info *shinfo;
	struct page *xdp_page;
	int i;

	if (xdp_buff_has_frags(xdp)) {
		shinfo = xdp_get_shared_info_from_buff(xdp);
		for (i = 0; i < shinfo->nr_frags; i++) {
			xdp_page = skb_frag_page(&shinfo->frags[i]);
			put_page(xdp_page);
		}
	}
}

static int virtnet_xdp_handler(struct bpf_prog *xdp_prog, struct xdp_buff *xdp,
			       struct net_device *dev,
			       unsigned int *xdp_xmit,
			       struct virtnet_rq_stats *stats)
{
	struct xdp_frame *xdpf;
	int err;
	u32 act;

	act = bpf_prog_run_xdp(xdp_prog, xdp);
	u64_stats_inc(&stats->xdp_packets);

	switch (act) {
	case XDP_PASS:
		return act;

	case XDP_TX:
		u64_stats_inc(&stats->xdp_tx);
		xdpf = xdp_convert_buff_to_frame(xdp);
		if (unlikely(!xdpf)) {
			netdev_dbg(dev, "convert buff to frame failed for xdp\n");
			return XDP_DROP;
		}

		err = virtnet_xdp_xmit(dev, 1, &xdpf, 0);
		if (unlikely(!err)) {
			xdp_return_frame_rx_napi(xdpf);
		} else if (unlikely(err < 0)) {
			trace_xdp_exception(dev, xdp_prog, act);
			return XDP_DROP;
		}
		*xdp_xmit |= VIRTIO_XDP_TX;
		return act;

	case XDP_REDIRECT:
		u64_stats_inc(&stats->xdp_redirects);
		err = xdp_do_redirect(dev, xdp, xdp_prog);
		if (err)
			return XDP_DROP;

		*xdp_xmit |= VIRTIO_XDP_REDIR;
		return act;

	default:
		bpf_warn_invalid_xdp_action(dev, xdp_prog, act);
		fallthrough;
	case XDP_ABORTED:
		trace_xdp_exception(dev, xdp_prog, act);
		fallthrough;
	case XDP_DROP:
		return XDP_DROP;
	}
}

static unsigned int virtnet_get_headroom(struct virtnet_info *vi)
{
	return vi->xdp_enabled ? XDP_PACKET_HEADROOM : 0;
}

/* We copy the packet for XDP in the following cases:
 *
 * 1) Packet is scattered across multiple rx buffers.
 * 2) Headroom space is insufficient.
 *
 * This is inefficient but it's a temporary condition that
 * we hit right after XDP is enabled and until queue is refilled
 * with large buffers with sufficient headroom - so it should affect
 * at most queue size packets.
 * Afterwards, the conditions to enable
 * XDP should preclude the underlying device from sending packets
 * across multiple buffers (num_buf > 1), and we make sure buffers
 * have enough headroom.
 */
static struct page *xdp_linearize_page(struct net_device *dev,
				       struct receive_queue *rq,
				       int *num_buf,
				       struct page *p,
				       int offset,
				       int page_off,
				       unsigned int *len)
{
	int tailroom = SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
	struct page *page;

	if (page_off + *len + tailroom > PAGE_SIZE)
		return NULL;

	page = alloc_page(GFP_ATOMIC);
	if (!page)
		return NULL;

	memcpy(page_address(page) + page_off, page_address(p) + offset, *len);
	page_off += *len;

	/* Only mergeable mode can go inside this while loop. In small mode,
	 * *num_buf == 1, so it cannot go inside.
	 */
	while (--*num_buf) {
		unsigned int buflen;
		void *buf;
		void *ctx;
		int off;

		buf = virtnet_rq_get_buf(rq, &buflen, &ctx);
		if (unlikely(!buf))
			goto err_buf;

		p = virt_to_head_page(buf);
		off = buf - page_address(p);

		if (check_mergeable_len(dev, ctx, buflen)) {
			put_page(p);
			goto err_buf;
		}

		/* guard against a misconfigured or uncooperative backend that
		 * is sending packet larger than the MTU.
		 */
		if ((page_off + buflen + tailroom) > PAGE_SIZE) {
			put_page(p);
			goto err_buf;
		}

		memcpy(page_address(page) + page_off,
		       page_address(p) + off, buflen);
		page_off += buflen;
		put_page(p);
	}

	/* Headroom does not contribute to packet length */
	*len = page_off - XDP_PACKET_HEADROOM;
	return page;
err_buf:
	__free_pages(page, 0);
	return NULL;
}

static struct sk_buff *receive_small_build_skb(struct virtnet_info *vi,
					       unsigned int xdp_headroom,
					       void *buf,
					       unsigned int len)
{
	unsigned int header_offset;
	unsigned int headroom;
	unsigned int buflen;
	struct sk_buff *skb;

	header_offset = VIRTNET_RX_PAD + xdp_headroom;
	headroom = vi->hdr_len + header_offset;
	buflen = SKB_DATA_ALIGN(GOOD_PACKET_LEN + headroom) +
		SKB_DATA_ALIGN(sizeof(struct skb_shared_info));

	skb = virtnet_build_skb(buf, buflen, headroom, len);
	if (unlikely(!skb))
		return NULL;

	buf += header_offset;
	memcpy(skb_vnet_common_hdr(skb), buf, vi->hdr_len);

	return skb;
}

static struct sk_buff *receive_small_xdp(struct net_device *dev,
					 struct virtnet_info *vi,
					 struct receive_queue *rq,
					 struct bpf_prog *xdp_prog,
					 void *buf,
					 unsigned int xdp_headroom,
					 unsigned int len,
					 unsigned int *xdp_xmit,
					 struct virtnet_rq_stats *stats)
{
	unsigned int header_offset = VIRTNET_RX_PAD + xdp_headroom;
	unsigned int headroom = vi->hdr_len + header_offset;
	struct virtio_net_hdr_mrg_rxbuf *hdr = buf + header_offset;
	struct page *page = virt_to_head_page(buf);
	struct page *xdp_page;
	unsigned int buflen;
	struct xdp_buff xdp;
	struct sk_buff *skb;
	unsigned int metasize = 0;
	u32 act;

	if (unlikely(hdr->hdr.gso_type))
		goto err_xdp;

	/* Partially checksummed packets must be dropped. */
	if (unlikely(hdr->hdr.flags & VIRTIO_NET_HDR_F_NEEDS_CSUM))
		goto err_xdp;

	buflen = SKB_DATA_ALIGN(GOOD_PACKET_LEN + headroom) +
		SKB_DATA_ALIGN(sizeof(struct skb_shared_info));

	if (unlikely(xdp_headroom < virtnet_get_headroom(vi))) {
		int offset = buf - page_address(page) + header_offset;
		unsigned int tlen = len + vi->hdr_len;
		int num_buf = 1;

		xdp_headroom = virtnet_get_headroom(vi);
		header_offset = VIRTNET_RX_PAD + xdp_headroom;
		headroom = vi->hdr_len + header_offset;
		buflen = SKB_DATA_ALIGN(GOOD_PACKET_LEN + headroom) +
			SKB_DATA_ALIGN(sizeof(struct skb_shared_info));
		xdp_page = xdp_linearize_page(dev, rq, &num_buf, page,
					      offset, header_offset,
					      &tlen);
		if (!xdp_page)
			goto err_xdp;

		buf = page_address(xdp_page);
		put_page(page);
		page = xdp_page;
	}

	xdp_init_buff(&xdp, buflen, &rq->xdp_rxq);
	xdp_prepare_buff(&xdp, buf + VIRTNET_RX_PAD + vi->hdr_len,
			 xdp_headroom, len, true);

	act = virtnet_xdp_handler(xdp_prog, &xdp, dev, xdp_xmit, stats);

	switch (act) {
	case XDP_PASS:
		/* Recalculate length in case bpf program changed it */
		len = xdp.data_end - xdp.data;
		metasize = xdp.data - xdp.data_meta;
		break;

	case XDP_TX:
	case XDP_REDIRECT:
		goto xdp_xmit;

	default:
		goto err_xdp;
	}

	skb = virtnet_build_skb(buf, buflen, xdp.data - buf, len);
	if (unlikely(!skb))
		goto err;

	if (metasize)
		skb_metadata_set(skb, metasize);

	return skb;

err_xdp:
	u64_stats_inc(&stats->xdp_drops);
err:
	u64_stats_inc(&stats->drops);
	put_page(page);
xdp_xmit:
	return NULL;
}

static struct sk_buff *receive_small(struct net_device *dev,
				     struct virtnet_info *vi,
				     struct receive_queue *rq,
				     void *buf, void *ctx,
				     unsigned int len,
				     unsigned int *xdp_xmit,
				     struct virtnet_rq_stats *stats)
{
	unsigned int xdp_headroom = (unsigned long)ctx;
	struct page *page = virt_to_head_page(buf);
	struct sk_buff *skb;

	/* We passed the address of virtnet header to virtio-core,
	 * so truncate the padding.
	 */
	buf -= VIRTNET_RX_PAD + xdp_headroom;

	len -= vi->hdr_len;
	u64_stats_add(&stats->bytes, len);

	if (unlikely(len > GOOD_PACKET_LEN)) {
		pr_debug("%s: rx error: len %u exceeds max size %d\n",
			 dev->name, len, GOOD_PACKET_LEN);
		DEV_STATS_INC(dev, rx_length_errors);
		goto err;
	}

	if (unlikely(vi->xdp_enabled)) {
		struct bpf_prog *xdp_prog;

		rcu_read_lock();
		xdp_prog = rcu_dereference(rq->xdp_prog);
		if (xdp_prog) {
			skb = receive_small_xdp(dev, vi, rq, xdp_prog, buf,
						xdp_headroom, len, xdp_xmit,
						stats);
			rcu_read_unlock();
			return skb;
		}
		rcu_read_unlock();
	}

	skb = receive_small_build_skb(vi, xdp_headroom, buf, len);
	if (likely(skb))
		return skb;

err:
	u64_stats_inc(&stats->drops);
	put_page(page);
	return NULL;
}

static struct sk_buff *receive_big(struct net_device *dev,
				   struct virtnet_info *vi,
				   struct receive_queue *rq,
				   void *buf,
				   unsigned int len,
				   struct virtnet_rq_stats *stats)
{
	struct page *page = buf;
	struct sk_buff *skb =
		page_to_skb(vi, rq, page, 0, len, PAGE_SIZE, 0);

	u64_stats_add(&stats->bytes, len - vi->hdr_len);
	if (unlikely(!skb))
		goto err;

	return skb;

err:
	u64_stats_inc(&stats->drops);
	give_pages(rq, page);
	return NULL;
}

static void mergeable_buf_free(struct receive_queue *rq, int num_buf,
			       struct net_device *dev,
			       struct virtnet_rq_stats *stats)
{
	struct page *page;
	void *buf;
	int len;

	while (num_buf-- > 1) {
		buf = virtnet_rq_get_buf(rq, &len, NULL);
		if (unlikely(!buf)) {
			pr_debug("%s: rx error: %d buffers missing\n",
				 dev->name, num_buf);
			DEV_STATS_INC(dev, rx_length_errors);
			break;
		}
		u64_stats_add(&stats->bytes, len);
		page = virt_to_head_page(buf);
		put_page(page);
	}
}

/* Why not use xdp_build_skb_from_frame() ?
 * XDP core assumes that xdp frags are PAGE_SIZE in length, while in
 * virtio-net there are 2 points that do not match its requirements:
 *  1. The size of the prefilled buffer is not fixed before xdp is set.
 *  2. xdp_build_skb_from_frame() does more checks that we don't need,
 *     like eth_type_trans() (which virtio-net does in receive_buf()).
 */
static struct sk_buff *build_skb_from_xdp_buff(struct net_device *dev,
					       struct virtnet_info *vi,
					       struct xdp_buff *xdp,
					       unsigned int xdp_frags_truesz)
{
	struct skb_shared_info *sinfo = xdp_get_shared_info_from_buff(xdp);
	unsigned int headroom, data_len;
	struct sk_buff *skb;
	int metasize;
	u8 nr_frags;

	if (unlikely(xdp->data_end > xdp_data_hard_end(xdp))) {
		pr_debug("Error building skb as missing reserved tailroom for xdp");
		return NULL;
	}

	if (unlikely(xdp_buff_has_frags(xdp)))
		nr_frags = sinfo->nr_frags;

	skb = build_skb(xdp->data_hard_start, xdp->frame_sz);
	if (unlikely(!skb))
		return NULL;

	headroom = xdp->data - xdp->data_hard_start;
	data_len = xdp->data_end - xdp->data;
	skb_reserve(skb, headroom);
	__skb_put(skb, data_len);

	metasize = xdp->data - xdp->data_meta;
	metasize = metasize > 0 ? metasize : 0;
	if (metasize)
		skb_metadata_set(skb, metasize);

	if (unlikely(xdp_buff_has_frags(xdp)))
		xdp_update_skb_shared_info(skb, nr_frags,
					   sinfo->xdp_frags_size,
					   xdp_frags_truesz,
					   xdp_buff_is_frag_pfmemalloc(xdp));

	return skb;
}

/* TODO: build xdp in big mode */
static int virtnet_build_xdp_buff_mrg(struct net_device *dev,
				      struct virtnet_info *vi,
				      struct receive_queue *rq,
				      struct xdp_buff *xdp,
				      void *buf,
				      unsigned int len,
				      unsigned int frame_sz,
				      int *num_buf,
				      unsigned int *xdp_frags_truesize,
				      struct virtnet_rq_stats *stats)
{
	struct virtio_net_hdr_mrg_rxbuf *hdr = buf;
	unsigned int headroom, tailroom, room;
	unsigned int truesize, cur_frag_size;
	struct skb_shared_info *shinfo;
	unsigned int xdp_frags_truesz = 0;
	struct page *page;
	skb_frag_t *frag;
	int offset;
	void *ctx;

	xdp_init_buff(xdp, frame_sz, &rq->xdp_rxq);
	xdp_prepare_buff(xdp, buf - XDP_PACKET_HEADROOM,
			 XDP_PACKET_HEADROOM + vi->hdr_len, len - vi->hdr_len, true);

	if (!*num_buf)
		return 0;

	if (*num_buf > 1) {
		/* If we want to build multi-buffer xdp, we need
		 * to specify that the flags of xdp_buff have the
		 * XDP_FLAGS_HAS_FRAG bit.
		 */
		if (!xdp_buff_has_frags(xdp))
			xdp_buff_set_frags_flag(xdp);

		shinfo = xdp_get_shared_info_from_buff(xdp);
		shinfo->nr_frags = 0;
		shinfo->xdp_frags_size = 0;
	}

	if (*num_buf > MAX_SKB_FRAGS + 1)
		return -EINVAL;

	while (--*num_buf > 0) {
		buf = virtnet_rq_get_buf(rq, &len, &ctx);
		if (unlikely(!buf)) {
			pr_debug("%s: rx error: %d buffers out of %d missing\n",
				 dev->name, *num_buf,
				 virtio16_to_cpu(vi->vdev, hdr->num_buffers));
			DEV_STATS_INC(dev, rx_length_errors);
			goto err;
		}

		u64_stats_add(&stats->bytes, len);
		page = virt_to_head_page(buf);
		offset = buf - page_address(page);

		truesize = mergeable_ctx_to_truesize(ctx);
		headroom = mergeable_ctx_to_headroom(ctx);
		tailroom = headroom ? sizeof(struct skb_shared_info) : 0;
		room = SKB_DATA_ALIGN(headroom + tailroom);

		cur_frag_size = truesize;
		xdp_frags_truesz += cur_frag_size;
		if (unlikely(len > truesize - room || cur_frag_size > PAGE_SIZE)) {
			put_page(page);
			pr_debug("%s: rx error: len %u exceeds truesize %lu\n",
				 dev->name, len, (unsigned long)(truesize - room));
			DEV_STATS_INC(dev, rx_length_errors);
			goto err;
		}

		frag = &shinfo->frags[shinfo->nr_frags++];
		skb_frag_fill_page_desc(frag, page, offset, len);
		if (page_is_pfmemalloc(page))
			xdp_buff_set_frag_pfmemalloc(xdp);

		shinfo->xdp_frags_size += len;
	}

	*xdp_frags_truesize = xdp_frags_truesz;
	return 0;

err:
	put_xdp_frags(xdp);
	return -EINVAL;
}

static void *mergeable_xdp_get_buf(struct virtnet_info *vi,
				   struct receive_queue *rq,
				   struct bpf_prog *xdp_prog,
				   void *ctx,
				   unsigned int *frame_sz,
				   int *num_buf,
				   struct page **page,
				   int offset,
				   unsigned int *len,
				   struct virtio_net_hdr_mrg_rxbuf *hdr)
{
	unsigned int truesize = mergeable_ctx_to_truesize(ctx);
	unsigned int headroom = mergeable_ctx_to_headroom(ctx);
	struct page *xdp_page;
	unsigned int xdp_room;

	/* Transient failure which in theory could occur if
	 * in-flight packets from before XDP was enabled reach
	 * the receive path after XDP is loaded.
	 */
	if (unlikely(hdr->hdr.gso_type))
		return NULL;

	/* Partially checksummed packets must be dropped. */
	if (unlikely(hdr->hdr.flags & VIRTIO_NET_HDR_F_NEEDS_CSUM))
		return NULL;

	/* Now XDP core assumes frag size is PAGE_SIZE, but buffers
	 * with headroom may add hole in truesize, which
	 * make their length exceed PAGE_SIZE. So we disabled the
	 * hole mechanism for xdp. See add_recvbuf_mergeable().
	 */
	*frame_sz = truesize;

	if (likely(headroom >= virtnet_get_headroom(vi) &&
		   (*num_buf == 1 || xdp_prog->aux->xdp_has_frags))) {
		return page_address(*page) + offset;
	}

	/* This happens when headroom is not enough because
	 * of the buffer was prefilled before XDP is set.
	 * This should only happen for the first several packets.
	 * In fact, vq reset can be used here to help us clean up
	 * the prefilled buffers, but many existing devices do not
	 * support it, and we don't want to bother users who are
	 * using xdp normally.
	 */
	if (!xdp_prog->aux->xdp_has_frags) {
		/* linearize data for XDP */
		xdp_page = xdp_linearize_page(vi->dev, rq, num_buf,
					      *page, offset,
					      XDP_PACKET_HEADROOM,
					      len);
		if (!xdp_page)
			return NULL;
	} else {
		xdp_room = SKB_DATA_ALIGN(XDP_PACKET_HEADROOM +
					  sizeof(struct skb_shared_info));
		if (*len + xdp_room > PAGE_SIZE)
			return NULL;

		xdp_page = alloc_page(GFP_ATOMIC);
		if (!xdp_page)
			return NULL;

		memcpy(page_address(xdp_page) + XDP_PACKET_HEADROOM,
		       page_address(*page) + offset, *len);
	}

	*frame_sz = PAGE_SIZE;

	put_page(*page);

	*page = xdp_page;

	return page_address(*page) + XDP_PACKET_HEADROOM;
}

static struct sk_buff *receive_mergeable_xdp(struct net_device *dev,
					     struct virtnet_info *vi,
					     struct receive_queue *rq,
					     struct bpf_prog *xdp_prog,
					     void *buf,
					     void *ctx,
					     unsigned int len,
					     unsigned int *xdp_xmit,
					     struct virtnet_rq_stats *stats)
{
	struct virtio_net_hdr_mrg_rxbuf *hdr = buf;
	int num_buf = virtio16_to_cpu(vi->vdev, hdr->num_buffers);
	struct page *page = virt_to_head_page(buf);
	int offset = buf - page_address(page);
	unsigned int xdp_frags_truesz = 0;
	struct sk_buff *head_skb;
	unsigned int frame_sz;
	struct xdp_buff xdp;
	void *data;
	u32 act;
	int err;

	data = mergeable_xdp_get_buf(vi, rq, xdp_prog, ctx, &frame_sz, &num_buf, &page,
				     offset, &len, hdr);
	if (unlikely(!data))
		goto err_xdp;

	err = virtnet_build_xdp_buff_mrg(dev, vi, rq, &xdp, data, len, frame_sz,
					 &num_buf, &xdp_frags_truesz, stats);
	if (unlikely(err))
		goto err_xdp;

	act = virtnet_xdp_handler(xdp_prog, &xdp, dev, xdp_xmit, stats);

	switch (act) {
	case XDP_PASS:
		head_skb = build_skb_from_xdp_buff(dev, vi, &xdp, xdp_frags_truesz);
		if (unlikely(!head_skb))
			break;
		return head_skb;

	case XDP_TX:
	case XDP_REDIRECT:
		return NULL;

	default:
		break;
	}

	put_xdp_frags(&xdp);

err_xdp:
	put_page(page);
	mergeable_buf_free(rq, num_buf, dev, stats);

	u64_stats_inc(&stats->xdp_drops);
	u64_stats_inc(&stats->drops);
	return NULL;
}

static struct sk_buff *virtnet_skb_append_frag(struct sk_buff *head_skb,
					       struct sk_buff *curr_skb,
					       struct page *page, void *buf,
					       int len, int truesize)
{
	int num_skb_frags;
	int offset;

	num_skb_frags = skb_shinfo(curr_skb)->nr_frags;
	if (unlikely(num_skb_frags == MAX_SKB_FRAGS)) {
		struct sk_buff *nskb = alloc_skb(0, GFP_ATOMIC);

		if (unlikely(!nskb))
			return NULL;

		if (curr_skb == head_skb)
			skb_shinfo(curr_skb)->frag_list = nskb;
		else
			curr_skb->next = nskb;
		curr_skb = nskb;
		head_skb->truesize += nskb->truesize;
		num_skb_frags = 0;
	}

	if (curr_skb != head_skb) {
		head_skb->data_len += len;
		head_skb->len += len;
		head_skb->truesize += truesize;
	}

	offset = buf - page_address(page);
	if (skb_can_coalesce(curr_skb, num_skb_frags, page, offset)) {
		put_page(page);
		skb_coalesce_rx_frag(curr_skb, num_skb_frags - 1,
				     len, truesize);
	} else {
		skb_add_rx_frag(curr_skb, num_skb_frags, page,
				offset, len, truesize);
	}

	return curr_skb;
}

static struct sk_buff *receive_mergeable(struct net_device *dev,
					 struct virtnet_info *vi,
					 struct receive_queue *rq,
					 void *buf,
					 void *ctx,
					 unsigned int len,
					 unsigned int *xdp_xmit,
					 struct virtnet_rq_stats *stats)
{
	struct virtio_net_hdr_mrg_rxbuf *hdr = buf;
	int num_buf = virtio16_to_cpu(vi->vdev, hdr->num_buffers);
	struct page *page = virt_to_head_page(buf);
	int offset = buf - page_address(page);
	struct sk_buff *head_skb, *curr_skb;
	unsigned int truesize = mergeable_ctx_to_truesize(ctx);
	unsigned int headroom = mergeable_ctx_to_headroom(ctx);
	unsigned int tailroom = headroom ? sizeof(struct skb_shared_info) : 0;
	unsigned int room = SKB_DATA_ALIGN(headroom + tailroom);

	head_skb = NULL;
	u64_stats_add(&stats->bytes, len - vi->hdr_len);

	if (unlikely(len > truesize - room)) {
		pr_debug("%s: rx error: len %u exceeds truesize %lu\n",
			 dev->name, len, (unsigned long)(truesize - room));
		DEV_STATS_INC(dev, rx_length_errors);
		goto err_skb;
	}

	if (unlikely(vi->xdp_enabled)) {
		struct bpf_prog *xdp_prog;

		rcu_read_lock();
		xdp_prog = rcu_dereference(rq->xdp_prog);
		if (xdp_prog) {
			head_skb = receive_mergeable_xdp(dev, vi, rq, xdp_prog, buf, ctx,
							 len, xdp_xmit, stats);
			rcu_read_unlock();
			return head_skb;
		}
		rcu_read_unlock();
	}

	head_skb = page_to_skb(vi, rq, page, offset, len, truesize, headroom);
	curr_skb = head_skb;

	if (unlikely(!curr_skb))
		goto err_skb;
	while (--num_buf) {
		buf = virtnet_rq_get_buf(rq, &len, &ctx);
		if (unlikely(!buf)) {
			pr_debug("%s: rx error: %d buffers out of %d missing\n",
				 dev->name, num_buf,
				 virtio16_to_cpu(vi->vdev,
						 hdr->num_buffers));
			DEV_STATS_INC(dev, rx_length_errors);
			goto err_buf;
		}

		u64_stats_add(&stats->bytes, len);
		page = virt_to_head_page(buf);

		truesize = mergeable_ctx_to_truesize(ctx);
		headroom = mergeable_ctx_to_headroom(ctx);
		tailroom = headroom ? sizeof(struct skb_shared_info) : 0;
		room = SKB_DATA_ALIGN(headroom + tailroom);
		if (unlikely(len > truesize - room)) {
			pr_debug("%s: rx error: len %u exceeds truesize %lu\n",
				 dev->name, len, (unsigned long)(truesize - room));
			DEV_STATS_INC(dev, rx_length_errors);
			goto err_skb;
		}

		curr_skb  = virtnet_skb_append_frag(head_skb, curr_skb, page,
						    buf, len, truesize);
		if (!curr_skb)
			goto err_skb;
	}

	ewma_pkt_len_add(&rq->mrg_avg_pkt_len, head_skb->len);
	return head_skb;

err_skb:
	put_page(page);
	mergeable_buf_free(rq, num_buf, dev, stats);

err_buf:
	u64_stats_inc(&stats->drops);
	dev_kfree_skb(head_skb);
	return NULL;
}

static void virtio_skb_set_hash(const struct virtio_net_hdr_v1_hash *hdr_hash,
				struct sk_buff *skb)
{
	enum pkt_hash_types rss_hash_type;

	if (!hdr_hash || !skb)
		return;

	switch (__le16_to_cpu(hdr_hash->hash_report)) {
	case VIRTIO_NET_HASH_REPORT_TCPv4:
	case VIRTIO_NET_HASH_REPORT_UDPv4:
	case VIRTIO_NET_HASH_REPORT_TCPv6:
	case VIRTIO_NET_HASH_REPORT_UDPv6:
	case VIRTIO_NET_HASH_REPORT_TCPv6_EX:
	case VIRTIO_NET_HASH_REPORT_UDPv6_EX:
		rss_hash_type = PKT_HASH_TYPE_L4;
		break;
	case VIRTIO_NET_HASH_REPORT_IPv4:
	case VIRTIO_NET_HASH_REPORT_IPv6:
	case VIRTIO_NET_HASH_REPORT_IPv6_EX:
		rss_hash_type = PKT_HASH_TYPE_L3;
		break;
	case VIRTIO_NET_HASH_REPORT_NONE:
	default:
		rss_hash_type = PKT_HASH_TYPE_NONE;
	}
	skb_set_hash(skb, __le32_to_cpu(hdr_hash->hash_value), rss_hash_type);
}

static void virtnet_receive_done(struct virtnet_info *vi, struct receive_queue *rq,
				 struct sk_buff *skb, u8 flags)
{
	struct virtio_net_common_hdr *hdr;
	struct net_device *dev = vi->dev;

	hdr = skb_vnet_common_hdr(skb);
	if (dev->features & NETIF_F_RXHASH && vi->has_rss_hash_report)
		virtio_skb_set_hash(&hdr->hash_v1_hdr, skb);

	if (flags & VIRTIO_NET_HDR_F_DATA_VALID)
		skb->ip_summed = CHECKSUM_UNNECESSARY;

	if (virtio_net_hdr_to_skb(skb, &hdr->hdr,
				  virtio_is_little_endian(vi->vdev))) {
		net_warn_ratelimited("%s: bad gso: type: %u, size: %u\n",
				     dev->name, hdr->hdr.gso_type,
				     hdr->hdr.gso_size);
		goto frame_err;
	}

	skb_record_rx_queue(skb, vq2rxq(rq->vq));
	skb->protocol = eth_type_trans(skb, dev);
	pr_debug("Receiving skb proto 0x%04x len %i type %i\n",
		 ntohs(skb->protocol), skb->len, skb->pkt_type);

	napi_gro_receive(&rq->napi, skb);
	return;

frame_err:
	DEV_STATS_INC(dev, rx_frame_errors);
	dev_kfree_skb(skb);
}

static void receive_buf(struct virtnet_info *vi, struct receive_queue *rq,
			void *buf, unsigned int len, void **ctx,
			unsigned int *xdp_xmit,
			struct virtnet_rq_stats *stats)
{
	struct net_device *dev = vi->dev;
	struct sk_buff *skb;
	u8 flags;

	if (unlikely(len < vi->hdr_len + ETH_HLEN)) {
		pr_debug("%s: short packet %i\n", dev->name, len);
		DEV_STATS_INC(dev, rx_length_errors);
		virtnet_rq_free_buf(vi, rq, buf);
		return;
	}

	/* 1. Save the flags early, as the XDP program might overwrite them.
	 * These flags ensure packets marked as VIRTIO_NET_HDR_F_DATA_VALID
	 * stay valid after XDP processing.
	 * 2. XDP doesn't work with partially checksummed packets (refer to
	 * virtnet_xdp_set()), so packets marked as
	 * VIRTIO_NET_HDR_F_NEEDS_CSUM get dropped during XDP processing.
	 */
	flags = ((struct virtio_net_common_hdr *)buf)->hdr.flags;

	if (vi->mergeable_rx_bufs)
		skb = receive_mergeable(dev, vi, rq, buf, ctx, len, xdp_xmit,
					stats);
	else if (vi->big_packets)
		skb = receive_big(dev, vi, rq, buf, len, stats);
	else
		skb = receive_small(dev, vi, rq, buf, ctx, len, xdp_xmit, stats);

	if (unlikely(!skb))
		return;

	virtnet_receive_done(vi, rq, skb, flags);
}

/* Unlike mergeable buffers, all buffers are allocated to the
 * same size, except for the headroom. For this reason we do
 * not need to use  mergeable_len_to_ctx here - it is enough
 * to store the headroom as the context ignoring the truesize.
 */
static int add_recvbuf_small(struct virtnet_info *vi, struct receive_queue *rq,
			     gfp_t gfp)
{
	char *buf;
	unsigned int xdp_headroom = virtnet_get_headroom(vi);
	void *ctx = (void *)(unsigned long)xdp_headroom;
	int len = vi->hdr_len + VIRTNET_RX_PAD + GOOD_PACKET_LEN + xdp_headroom;
	int err;

	len = SKB_DATA_ALIGN(len) +
	      SKB_DATA_ALIGN(sizeof(struct skb_shared_info));

	if (unlikely(!skb_page_frag_refill(len, &rq->alloc_frag, gfp)))
		return -ENOMEM;

	buf = virtnet_rq_alloc(rq, len, gfp);
	if (unlikely(!buf))
		return -ENOMEM;

	buf += VIRTNET_RX_PAD + xdp_headroom;

	virtnet_rq_init_one_sg(rq, buf, vi->hdr_len + GOOD_PACKET_LEN);

	err = virtqueue_add_inbuf_premapped(rq->vq, rq->sg, 1, buf, ctx, gfp);
	if (err < 0) {
		virtnet_rq_unmap(rq, buf, 0);
		put_page(virt_to_head_page(buf));
	}

	return err;
}

static int add_recvbuf_big(struct virtnet_info *vi, struct receive_queue *rq,
			   gfp_t gfp)
{
	struct page *first, *list = NULL;
	char *p;
	int i, err, offset;

	sg_init_table(rq->sg, vi->big_packets_num_skbfrags + 2);

	/* page in rq->sg[vi->big_packets_num_skbfrags + 1] is list tail */
	for (i = vi->big_packets_num_skbfrags + 1; i > 1; --i) {
		first = get_a_page(rq, gfp);
		if (!first) {
			if (list)
				give_pages(rq, list);
			return -ENOMEM;
		}
		sg_set_buf(&rq->sg[i], page_address(first), PAGE_SIZE);

		/* chain new page in list head to match sg */
		first->private = (unsigned long)list;
		list = first;
	}

	first = get_a_page(rq, gfp);
	if (!first) {
		give_pages(rq, list);
		return -ENOMEM;
	}
	p = page_address(first);

	/* rq->sg[0], rq->sg[1] share the same page */
	/* a separated rq->sg[0] for header - required in case !any_header_sg */
	sg_set_buf(&rq->sg[0], p, vi->hdr_len);

	/* rq->sg[1] for data packet, from offset */
	offset = sizeof(struct padded_vnet_hdr);
	sg_set_buf(&rq->sg[1], p + offset, PAGE_SIZE - offset);

	/* chain first in list head */
	first->private = (unsigned long)list;
	err = virtqueue_add_inbuf(rq->vq, rq->sg, vi->big_packets_num_skbfrags + 2,
				  first, gfp);
	if (err < 0)
		give_pages(rq, first);

	return err;
}

static unsigned int get_mergeable_buf_len(struct receive_queue *rq,
					  struct ewma_pkt_len *avg_pkt_len,
					  unsigned int room)
{
	struct virtnet_info *vi = rq->vq->vdev->priv;
	const size_t hdr_len = vi->hdr_len;
	unsigned int len;

	if (room)
		return PAGE_SIZE - room;

	len = hdr_len +	clamp_t(unsigned int, ewma_pkt_len_read(avg_pkt_len),
				rq->min_buf_len, PAGE_SIZE - hdr_len);

	return ALIGN(len, L1_CACHE_BYTES);
}

static int add_recvbuf_mergeable(struct virtnet_info *vi,
				 struct receive_queue *rq, gfp_t gfp)
{
	struct page_frag *alloc_frag = &rq->alloc_frag;
	unsigned int headroom = virtnet_get_headroom(vi);
	unsigned int tailroom = headroom ? sizeof(struct skb_shared_info) : 0;
	unsigned int room = SKB_DATA_ALIGN(headroom + tailroom);
	unsigned int len, hole;
	void *ctx;
	char *buf;
	int err;

	/* Extra tailroom is needed to satisfy XDP's assumption. This
	 * means rx frags coalescing won't work, but consider we've
	 * disabled GSO for XDP, it won't be a big issue.
	 */
	len = get_mergeable_buf_len(rq, &rq->mrg_avg_pkt_len, room);

	if (unlikely(!skb_page_frag_refill(len + room, alloc_frag, gfp)))
		return -ENOMEM;

	if (!alloc_frag->offset && len + room + sizeof(struct virtnet_rq_dma) > alloc_frag->size)
		len -= sizeof(struct virtnet_rq_dma);

	buf = virtnet_rq_alloc(rq, len + room, gfp);
	if (unlikely(!buf))
		return -ENOMEM;

	buf += headroom; /* advance address leaving hole at front of pkt */
	hole = alloc_frag->size - alloc_frag->offset;
	if (hole < len + room) {
		/* To avoid internal fragmentation, if there is very likely not
		 * enough space for another buffer, add the remaining space to
		 * the current buffer.
		 * XDP core assumes that frame_size of xdp_buff and the length
		 * of the frag are PAGE_SIZE, so we disable the hole mechanism.
		 */
		if (!headroom)
			len += hole;
		alloc_frag->offset += hole;
	}

	virtnet_rq_init_one_sg(rq, buf, len);

	ctx = mergeable_len_to_ctx(len + room, headroom);
	err = virtqueue_add_inbuf_premapped(rq->vq, rq->sg, 1, buf, ctx, gfp);
	if (err < 0) {
		virtnet_rq_unmap(rq, buf, 0);
		put_page(virt_to_head_page(buf));
	}

	return err;
}

/*
 * Returns false if we couldn't fill entirely (OOM).
 *
 * Normally run in the receive path, but can also be run from ndo_open
 * before we're receiving packets, or from refill_work which is
 * careful to disable receiving (using napi_disable).
 */
static bool try_fill_recv(struct virtnet_info *vi, struct receive_queue *rq,
			  gfp_t gfp)
{
	int err;

	if (rq->xsk_pool) {
		err = virtnet_add_recvbuf_xsk(vi, rq, rq->xsk_pool, gfp);
		goto kick;
	}

	do {
		if (vi->mergeable_rx_bufs)
			err = add_recvbuf_mergeable(vi, rq, gfp);
		else if (vi->big_packets)
			err = add_recvbuf_big(vi, rq, gfp);
		else
			err = add_recvbuf_small(vi, rq, gfp);

		if (err)
			break;
	} while (rq->vq->num_free);

kick:
	if (virtqueue_kick_prepare(rq->vq) && virtqueue_notify(rq->vq)) {
		unsigned long flags;

		flags = u64_stats_update_begin_irqsave(&rq->stats.syncp);
		u64_stats_inc(&rq->stats.kicks);
		u64_stats_update_end_irqrestore(&rq->stats.syncp, flags);
	}

	return err != -ENOMEM;
}

static void skb_recv_done(struct virtqueue *rvq)
{
	struct virtnet_info *vi = rvq->vdev->priv;
	struct receive_queue *rq = &vi->rq[vq2rxq(rvq)];

	rq->calls++;
	virtqueue_napi_schedule(&rq->napi, rvq);
}

static void virtnet_napi_do_enable(struct virtqueue *vq,
				   struct napi_struct *napi)
{
	napi_enable(napi);

	/* If all buffers were filled by other side before we napi_enabled, we
	 * won't get another interrupt, so process any outstanding packets now.
	 * Call local_bh_enable after to trigger softIRQ processing.
	 */
	local_bh_disable();
	virtqueue_napi_schedule(napi, vq);
	local_bh_enable();
}

static void virtnet_napi_enable(struct receive_queue *rq)
{
	struct virtnet_info *vi = rq->vq->vdev->priv;
	int qidx = vq2rxq(rq->vq);

	virtnet_napi_do_enable(rq->vq, &rq->napi);
	netif_queue_set_napi(vi->dev, qidx, NETDEV_QUEUE_TYPE_RX, &rq->napi);
}

static void virtnet_napi_tx_enable(struct send_queue *sq)
{
	struct virtnet_info *vi = sq->vq->vdev->priv;
	struct napi_struct *napi = &sq->napi;
	int qidx = vq2txq(sq->vq);

	if (!napi->weight)
		return;

	/* Tx napi touches cachelines on the cpu handling tx interrupts. Only
	 * enable the feature if this is likely affine with the transmit path.
	 */
	if (!vi->affinity_hint_set) {
		napi->weight = 0;
		return;
	}

	virtnet_napi_do_enable(sq->vq, napi);
	netif_queue_set_napi(vi->dev, qidx, NETDEV_QUEUE_TYPE_TX, napi);
}

static void virtnet_napi_tx_disable(struct send_queue *sq)
{
	struct virtnet_info *vi = sq->vq->vdev->priv;
	struct napi_struct *napi = &sq->napi;
	int qidx = vq2txq(sq->vq);

	if (napi->weight) {
		netif_queue_set_napi(vi->dev, qidx, NETDEV_QUEUE_TYPE_TX, NULL);
		napi_disable(napi);
	}
}

static void virtnet_napi_disable(struct receive_queue *rq)
{
	struct virtnet_info *vi = rq->vq->vdev->priv;
	struct napi_struct *napi = &rq->napi;
	int qidx = vq2rxq(rq->vq);

	netif_queue_set_napi(vi->dev, qidx, NETDEV_QUEUE_TYPE_RX, NULL);
	napi_disable(napi);
}

static void refill_work(struct work_struct *work)
{
	struct virtnet_info *vi =
		container_of(work, struct virtnet_info, refill.work);
	bool still_empty;
	int i;

	for (i = 0; i < vi->curr_queue_pairs; i++) {
		struct receive_queue *rq = &vi->rq[i];

		/*
		 * When queue API support is added in the future and the call
		 * below becomes napi_disable_locked, this driver will need to
		 * be refactored.
		 *
		 * One possible solution would be to:
		 *   - cancel refill_work with cancel_delayed_work (note:
		 *     non-sync)
		 *   - cancel refill_work with cancel_delayed_work_sync in
		 *     virtnet_remove after the netdev is unregistered
		 *   - wrap all of the work in a lock (perhaps the netdev
		 *     instance lock)
		 *   - check netif_running() and return early to avoid a race
		 */
		napi_disable(&rq->napi);
		still_empty = !try_fill_recv(vi, rq, GFP_KERNEL);
		virtnet_napi_do_enable(rq->vq, &rq->napi);

		/* In theory, this can happen: if we don't get any buffers in
		 * we will *never* try to fill again.
		 */
		if (still_empty)
			schedule_delayed_work(&vi->refill, HZ/2);
	}
}

static int virtnet_receive_xsk_bufs(struct virtnet_info *vi,
				    struct receive_queue *rq,
				    int budget,
				    unsigned int *xdp_xmit,
				    struct virtnet_rq_stats *stats)
{
	unsigned int len;
	int packets = 0;
	void *buf;

	while (packets < budget) {
		buf = virtqueue_get_buf(rq->vq, &len);
		if (!buf)
			break;

		virtnet_receive_xsk_buf(vi, rq, buf, len, xdp_xmit, stats);
		packets++;
	}

	return packets;
}

static int virtnet_receive_packets(struct virtnet_info *vi,
				   struct receive_queue *rq,
				   int budget,
				   unsigned int *xdp_xmit,
				   struct virtnet_rq_stats *stats)
{
	unsigned int len;
	int packets = 0;
	void *buf;

	if (!vi->big_packets || vi->mergeable_rx_bufs) {
		void *ctx;
		while (packets < budget &&
		       (buf = virtnet_rq_get_buf(rq, &len, &ctx))) {
			receive_buf(vi, rq, buf, len, ctx, xdp_xmit, stats);
			packets++;
		}
	} else {
		while (packets < budget &&
		       (buf = virtqueue_get_buf(rq->vq, &len)) != NULL) {
			receive_buf(vi, rq, buf, len, NULL, xdp_xmit, stats);
			packets++;
		}
	}

	return packets;
}

static int virtnet_receive(struct receive_queue *rq, int budget,
			   unsigned int *xdp_xmit)
{
	struct virtnet_info *vi = rq->vq->vdev->priv;
	struct virtnet_rq_stats stats = {};
	int i, packets;

	if (rq->xsk_pool)
		packets = virtnet_receive_xsk_bufs(vi, rq, budget, xdp_xmit, &stats);
	else
		packets = virtnet_receive_packets(vi, rq, budget, xdp_xmit, &stats);

	if (rq->vq->num_free > min((unsigned int)budget, virtqueue_get_vring_size(rq->vq)) / 2) {
		if (!try_fill_recv(vi, rq, GFP_ATOMIC)) {
			spin_lock(&vi->refill_lock);
			if (vi->refill_enabled)
				schedule_delayed_work(&vi->refill, 0);
			spin_unlock(&vi->refill_lock);
		}
	}

	u64_stats_set(&stats.packets, packets);
	u64_stats_update_begin(&rq->stats.syncp);
	for (i = 0; i < ARRAY_SIZE(virtnet_rq_stats_desc); i++) {
		size_t offset = virtnet_rq_stats_desc[i].offset;
		u64_stats_t *item, *src;

		item = (u64_stats_t *)((u8 *)&rq->stats + offset);
		src = (u64_stats_t *)((u8 *)&stats + offset);
		u64_stats_add(item, u64_stats_read(src));
	}

	u64_stats_add(&rq->stats.packets, u64_stats_read(&stats.packets));
	u64_stats_add(&rq->stats.bytes, u64_stats_read(&stats.bytes));

	u64_stats_update_end(&rq->stats.syncp);

	return packets;
}

static void virtnet_poll_cleantx(struct receive_queue *rq, int budget)
{
	struct virtnet_info *vi = rq->vq->vdev->priv;
	unsigned int index = vq2rxq(rq->vq);
	struct send_queue *sq = &vi->sq[index];
	struct netdev_queue *txq = netdev_get_tx_queue(vi->dev, index);

	if (!sq->napi.weight || is_xdp_raw_buffer_queue(vi, index))
		return;

	if (__netif_tx_trylock(txq)) {
		if (sq->reset) {
			__netif_tx_unlock(txq);
			return;
		}

		do {
			virtqueue_disable_cb(sq->vq);
			free_old_xmit(sq, txq, !!budget);
		} while (unlikely(!virtqueue_enable_cb_delayed(sq->vq)));

		if (sq->vq->num_free >= 2 + MAX_SKB_FRAGS) {
			if (netif_tx_queue_stopped(txq)) {
				u64_stats_update_begin(&sq->stats.syncp);
				u64_stats_inc(&sq->stats.wake);
				u64_stats_update_end(&sq->stats.syncp);
			}
			netif_tx_wake_queue(txq);
		}

		__netif_tx_unlock(txq);
	}
}

static void virtnet_rx_dim_update(struct virtnet_info *vi, struct receive_queue *rq)
{
	struct dim_sample cur_sample = {};

	if (!rq->packets_in_napi)
		return;

	/* Don't need protection when fetching stats, since fetcher and
	 * updater of the stats are in same context
	 */
	dim_update_sample(rq->calls,
			  u64_stats_read(&rq->stats.packets),
			  u64_stats_read(&rq->stats.bytes),
			  &cur_sample);

	net_dim(&rq->dim, &cur_sample);
	rq->packets_in_napi = 0;
}

static int virtnet_poll(struct napi_struct *napi, int budget)
{
	struct receive_queue *rq =
		container_of(napi, struct receive_queue, napi);
	struct virtnet_info *vi = rq->vq->vdev->priv;
	struct send_queue *sq;
	unsigned int received;
	unsigned int xdp_xmit = 0;
	bool napi_complete;

	virtnet_poll_cleantx(rq, budget);

	received = virtnet_receive(rq, budget, &xdp_xmit);
	rq->packets_in_napi += received;

	if (xdp_xmit & VIRTIO_XDP_REDIR)
		xdp_do_flush();

	/* Out of packets? */
	if (received < budget) {
		napi_complete = virtqueue_napi_complete(napi, rq->vq, received);
		/* Intentionally not taking dim_lock here. This may result in a
		 * spurious net_dim call. But if that happens virtnet_rx_dim_work
		 * will not act on the scheduled work.
		 */
		if (napi_complete && rq->dim_enabled)
			virtnet_rx_dim_update(vi, rq);
	}

	if (xdp_xmit & VIRTIO_XDP_TX) {
		sq = virtnet_xdp_get_sq(vi);
		if (virtqueue_kick_prepare(sq->vq) && virtqueue_notify(sq->vq)) {
			u64_stats_update_begin(&sq->stats.syncp);
			u64_stats_inc(&sq->stats.kicks);
			u64_stats_update_end(&sq->stats.syncp);
		}
		virtnet_xdp_put_sq(vi, sq);
	}

	return received;
}

static void virtnet_disable_queue_pair(struct virtnet_info *vi, int qp_index)
{
	virtnet_napi_tx_disable(&vi->sq[qp_index]);
	virtnet_napi_disable(&vi->rq[qp_index]);
	xdp_rxq_info_unreg(&vi->rq[qp_index].xdp_rxq);
}

static int virtnet_enable_queue_pair(struct virtnet_info *vi, int qp_index)
{
	struct net_device *dev = vi->dev;
	int err;

	err = xdp_rxq_info_reg(&vi->rq[qp_index].xdp_rxq, dev, qp_index,
			       vi->rq[qp_index].napi.napi_id);
	if (err < 0)
		return err;

	err = xdp_rxq_info_reg_mem_model(&vi->rq[qp_index].xdp_rxq,
					 MEM_TYPE_PAGE_SHARED, NULL);
	if (err < 0)
		goto err_xdp_reg_mem_model;

	virtnet_napi_enable(&vi->rq[qp_index]);
	virtnet_napi_tx_enable(&vi->sq[qp_index]);

	return 0;

err_xdp_reg_mem_model:
	xdp_rxq_info_unreg(&vi->rq[qp_index].xdp_rxq);
	return err;
}

static void virtnet_cancel_dim(struct virtnet_info *vi, struct dim *dim)
{
	if (!virtio_has_feature(vi->vdev, VIRTIO_NET_F_VQ_NOTF_COAL))
		return;
	net_dim_work_cancel(dim);
}

static void virtnet_update_settings(struct virtnet_info *vi)
{
	u32 speed;
	u8 duplex;

	if (!virtio_has_feature(vi->vdev, VIRTIO_NET_F_SPEED_DUPLEX))
		return;

	virtio_cread_le(vi->vdev, struct virtio_net_config, speed, &speed);

	if (ethtool_validate_speed(speed))
		vi->speed = speed;

	virtio_cread_le(vi->vdev, struct virtio_net_config, duplex, &duplex);

	if (ethtool_validate_duplex(duplex))
		vi->duplex = duplex;
}

static int virtnet_open(struct net_device *dev)
{
	struct virtnet_info *vi = netdev_priv(dev);
	int i, err;

	enable_delayed_refill(vi);

	for (i = 0; i < vi->max_queue_pairs; i++) {
		if (i < vi->curr_queue_pairs)
			/* Make sure we have some buffers: if oom use wq. */
			if (!try_fill_recv(vi, &vi->rq[i], GFP_KERNEL))
				schedule_delayed_work(&vi->refill, 0);

		err = virtnet_enable_queue_pair(vi, i);
		if (err < 0)
			goto err_enable_qp;
	}

	if (virtio_has_feature(vi->vdev, VIRTIO_NET_F_STATUS)) {
		if (vi->status & VIRTIO_NET_S_LINK_UP)
			netif_carrier_on(vi->dev);
		virtio_config_driver_enable(vi->vdev);
	} else {
		vi->status = VIRTIO_NET_S_LINK_UP;
		netif_carrier_on(dev);
	}

	return 0;

err_enable_qp:
	disable_delayed_refill(vi);
	cancel_delayed_work_sync(&vi->refill);

	for (i--; i >= 0; i--) {
		virtnet_disable_queue_pair(vi, i);
		virtnet_cancel_dim(vi, &vi->rq[i].dim);
	}

	return err;
}

static int virtnet_poll_tx(struct napi_struct *napi, int budget)
{
	struct send_queue *sq = container_of(napi, struct send_queue, napi);
	struct virtnet_info *vi = sq->vq->vdev->priv;
	unsigned int index = vq2txq(sq->vq);
	struct netdev_queue *txq;
	int opaque, xsk_done = 0;
	bool done;

	if (unlikely(is_xdp_raw_buffer_queue(vi, index))) {
		/* We don't need to enable cb for XDP */
		napi_complete_done(napi, 0);
		return 0;
	}

	txq = netdev_get_tx_queue(vi->dev, index);
	__netif_tx_lock(txq, raw_smp_processor_id());
	virtqueue_disable_cb(sq->vq);

	if (sq->xsk_pool)
		xsk_done = virtnet_xsk_xmit(sq, sq->xsk_pool, budget);
	else
		free_old_xmit(sq, txq, !!budget);

	if (sq->vq->num_free >= 2 + MAX_SKB_FRAGS) {
		if (netif_tx_queue_stopped(txq)) {
			u64_stats_update_begin(&sq->stats.syncp);
			u64_stats_inc(&sq->stats.wake);
			u64_stats_update_end(&sq->stats.syncp);
		}
		netif_tx_wake_queue(txq);
	}

	if (xsk_done >= budget) {
		__netif_tx_unlock(txq);
		return budget;
	}

	opaque = virtqueue_enable_cb_prepare(sq->vq);

	done = napi_complete_done(napi, 0);

	if (!done)
		virtqueue_disable_cb(sq->vq);

	__netif_tx_unlock(txq);

	if (done) {
		if (unlikely(virtqueue_poll(sq->vq, opaque))) {
			if (napi_schedule_prep(napi)) {
				__netif_tx_lock(txq, raw_smp_processor_id());
				virtqueue_disable_cb(sq->vq);
				__netif_tx_unlock(txq);
				__napi_schedule(napi);
			}
		}
	}

	return 0;
}

static int xmit_skb(struct send_queue *sq, struct sk_buff *skb, bool orphan)
{
	struct virtio_net_hdr_mrg_rxbuf *hdr;
	const unsigned char *dest = ((struct ethhdr *)skb->data)->h_dest;
	struct virtnet_info *vi = sq->vq->vdev->priv;
	int num_sg;
	unsigned hdr_len = vi->hdr_len;
	bool can_push;

	pr_debug("%s: xmit %p %pM\n", vi->dev->name, skb, dest);

	can_push = vi->any_header_sg &&
		!((unsigned long)skb->data & (__alignof__(*hdr) - 1)) &&
		!skb_header_cloned(skb) && skb_headroom(skb) >= hdr_len;
	/* Even if we can, don't push here yet as this would skew
	 * csum_start offset below. */
	if (can_push)
		hdr = (struct virtio_net_hdr_mrg_rxbuf *)(skb->data - hdr_len);
	else
		hdr = &skb_vnet_common_hdr(skb)->mrg_hdr;

	if (virtio_net_hdr_from_skb(skb, &hdr->hdr,
				    virtio_is_little_endian(vi->vdev), false,
				    0))
		return -EPROTO;

	if (vi->mergeable_rx_bufs)
		hdr->num_buffers = 0;

	sg_init_table(sq->sg, skb_shinfo(skb)->nr_frags + (can_push ? 1 : 2));
	if (can_push) {
		__skb_push(skb, hdr_len);
		num_sg = skb_to_sgvec(skb, sq->sg, 0, skb->len);
		if (unlikely(num_sg < 0))
			return num_sg;
		/* Pull header back to avoid skew in tx bytes calculations. */
		__skb_pull(skb, hdr_len);
	} else {
		sg_set_buf(sq->sg, hdr, hdr_len);
		num_sg = skb_to_sgvec(skb, sq->sg + 1, 0, skb->len);
		if (unlikely(num_sg < 0))
			return num_sg;
		num_sg++;
	}

	return virtnet_add_outbuf(sq, num_sg, skb,
				  orphan ? VIRTNET_XMIT_TYPE_SKB_ORPHAN : VIRTNET_XMIT_TYPE_SKB);
}

static netdev_tx_t start_xmit(struct sk_buff *skb, struct net_device *dev)
{
	struct virtnet_info *vi = netdev_priv(dev);
	int qnum = skb_get_queue_mapping(skb);
	struct send_queue *sq = &vi->sq[qnum];
	int err;
	struct netdev_queue *txq = netdev_get_tx_queue(dev, qnum);
	bool xmit_more = netdev_xmit_more();
	bool use_napi = sq->napi.weight;
	bool kick;

	if (!use_napi)
		free_old_xmit(sq, txq, false);
	else
		virtqueue_disable_cb(sq->vq);

	/* timestamp packet in software */
	skb_tx_timestamp(skb);

	/* Try to transmit */
	err = xmit_skb(sq, skb, !use_napi);

	/* This should not happen! */
	if (unlikely(err)) {
		DEV_STATS_INC(dev, tx_fifo_errors);
		if (net_ratelimit())
			dev_warn(&dev->dev,
				 "Unexpected TXQ (%d) queue failure: %d\n",
				 qnum, err);
		DEV_STATS_INC(dev, tx_dropped);
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/* Don't wait up for transmitted skbs to be freed. */
	if (!use_napi) {
		skb_orphan(skb);
		nf_reset_ct(skb);
	}

	if (use_napi)
		tx_may_stop(vi, dev, sq);
	else
		check_sq_full_and_disable(vi, dev,sq);

	kick = use_napi ? __netdev_tx_sent_queue(txq, skb->len, xmit_more) :
			  !xmit_more || netif_xmit_stopped(txq);
	if (kick) {
		if (virtqueue_kick_prepare(sq->vq) && virtqueue_notify(sq->vq)) {
			u64_stats_update_begin(&sq->stats.syncp);
			u64_stats_inc(&sq->stats.kicks);
			u64_stats_update_end(&sq->stats.syncp);
		}
	}

	if (use_napi && kick && unlikely(!virtqueue_enable_cb_delayed(sq->vq)))
		virtqueue_napi_schedule(&sq->napi, sq->vq);

	return NETDEV_TX_OK;
}

static void __virtnet_rx_pause(struct virtnet_info *vi,
			       struct receive_queue *rq)
{
	bool running = netif_running(vi->dev);

	if (running) {
		virtnet_napi_disable(rq);
		virtnet_cancel_dim(vi, &rq->dim);
	}
}

static void virtnet_rx_pause_all(struct virtnet_info *vi)
{
	int i;

	/*
	 * Make sure refill_work does not run concurrently to
	 * avoid napi_disable race which leads to deadlock.
	 */
	disable_delayed_refill(vi);
	cancel_delayed_work_sync(&vi->refill);
	for (i = 0; i < vi->max_queue_pairs; i++)
		__virtnet_rx_pause(vi, &vi->rq[i]);
}

static void virtnet_rx_pause(struct virtnet_info *vi, struct receive_queue *rq)
{
	/*
	 * Make sure refill_work does not run concurrently to
	 * avoid napi_disable race which leads to deadlock.
	 */
	disable_delayed_refill(vi);
	cancel_delayed_work_sync(&vi->refill);
	__virtnet_rx_pause(vi, rq);
}

static void __virtnet_rx_resume(struct virtnet_info *vi,
				struct receive_queue *rq,
				bool refill)
{
	bool running = netif_running(vi->dev);
	bool schedule_refill = false;

	if (refill && !try_fill_recv(vi, rq, GFP_KERNEL))
		schedule_refill = true;
	if (running)
		virtnet_napi_enable(rq);

	if (schedule_refill)
		schedule_delayed_work(&vi->refill, 0);
}

static void virtnet_rx_resume_all(struct virtnet_info *vi)
{
	int i;

	enable_delayed_refill(vi);
	for (i = 0; i < vi->max_queue_pairs; i++) {
		if (i < vi->curr_queue_pairs)
			__virtnet_rx_resume(vi, &vi->rq[i], true);
		else
			__virtnet_rx_resume(vi, &vi->rq[i], false);
	}
}

static void virtnet_rx_resume(struct virtnet_info *vi, struct receive_queue *rq)
{
	enable_delayed_refill(vi);
	__virtnet_rx_resume(vi, rq, true);
}

static int virtnet_rx_resize(struct virtnet_info *vi,
			     struct receive_queue *rq, u32 ring_num)
{
	int err, qindex;

	qindex = rq - vi->rq;

	virtnet_rx_pause(vi, rq);

	err = virtqueue_resize(rq->vq, ring_num, virtnet_rq_unmap_free_buf, NULL);
	if (err)
		netdev_err(vi->dev, "resize rx fail: rx queue index: %d err: %d\n", qindex, err);

	virtnet_rx_resume(vi, rq);
	return err;
}

static void virtnet_tx_pause(struct virtnet_info *vi, struct send_queue *sq)
{
	bool running = netif_running(vi->dev);
	struct netdev_queue *txq;
	int qindex;

	qindex = sq - vi->sq;

	if (running)
		virtnet_napi_tx_disable(sq);

	txq = netdev_get_tx_queue(vi->dev, qindex);

	/* 1. wait all ximt complete
	 * 2. fix the race of netif_stop_subqueue() vs netif_start_subqueue()
	 */
	__netif_tx_lock_bh(txq);

	/* Prevent rx poll from accessing sq. */
	sq->reset = true;

	/* Prevent the upper layer from trying to send packets. */
	netif_stop_subqueue(vi->dev, qindex);

	__netif_tx_unlock_bh(txq);
}

static void virtnet_tx_resume(struct virtnet_info *vi, struct send_queue *sq)
{
	bool running = netif_running(vi->dev);
	struct netdev_queue *txq;
	int qindex;

	qindex = sq - vi->sq;

	txq = netdev_get_tx_queue(vi->dev, qindex);

	__netif_tx_lock_bh(txq);
	sq->reset = false;
	netif_tx_wake_queue(txq);
	__netif_tx_unlock_bh(txq);

	if (running)
		virtnet_napi_tx_enable(sq);
}

static int virtnet_tx_resize(struct virtnet_info *vi, struct send_queue *sq,
			     u32 ring_num)
{
	int qindex, err;

	if (ring_num <= MAX_SKB_FRAGS + 2) {
		netdev_err(vi->dev, "tx size (%d) cannot be smaller than %d\n",
			   ring_num, MAX_SKB_FRAGS + 2);
		return -EINVAL;
	}

	qindex = sq - vi->sq;

	virtnet_tx_pause(vi, sq);

	err = virtqueue_resize(sq->vq, ring_num, virtnet_sq_free_unused_buf,
			       virtnet_sq_free_unused_buf_done);
	if (err)
		netdev_err(vi->dev, "resize tx fail: tx queue index: %d err: %d\n", qindex, err);

	virtnet_tx_resume(vi, sq);

	return err;
}

/*
 * Send command via the control virtqueue and check status.  Commands
 * supported by the hypervisor, as indicated by feature bits, should
 * never fail unless improperly formatted.
 */
static bool virtnet_send_command_reply(struct virtnet_info *vi, u8 class, u8 cmd,
				       struct scatterlist *out,
				       struct scatterlist *in)
{
	struct scatterlist *sgs[5], hdr, stat;
	u32 out_num = 0, tmp, in_num = 0;
	bool ok;
	int ret;

	/* Caller should know better */
	BUG_ON(!virtio_has_feature(vi->vdev, VIRTIO_NET_F_CTRL_VQ));

	mutex_lock(&vi->cvq_lock);
	vi->ctrl->status = ~0;
	vi->ctrl->hdr.class = class;
	vi->ctrl->hdr.cmd = cmd;
	/* Add header */
	sg_init_one(&hdr, &vi->ctrl->hdr, sizeof(vi->ctrl->hdr));
	sgs[out_num++] = &hdr;

	if (out)
		sgs[out_num++] = out;

	/* Add return status. */
	sg_init_one(&stat, &vi->ctrl->status, sizeof(vi->ctrl->status));
	sgs[out_num + in_num++] = &stat;

	if (in)
		sgs[out_num + in_num++] = in;

	BUG_ON(out_num + in_num > ARRAY_SIZE(sgs));
	ret = virtqueue_add_sgs(vi->cvq, sgs, out_num, in_num, vi, GFP_ATOMIC);
	if (ret < 0) {
		dev_warn(&vi->vdev->dev,
			 "Failed to add sgs for command vq: %d\n.", ret);
		mutex_unlock(&vi->cvq_lock);
		return false;
	}

	if (unlikely(!virtqueue_kick(vi->cvq)))
		goto unlock;

	/* Spin for a response, the kick causes an ioport write, trapping
	 * into the hypervisor, so the request should be handled immediately.
	 */
	while (!virtqueue_get_buf(vi->cvq, &tmp) &&
	       !virtqueue_is_broken(vi->cvq)) {
		cond_resched();
		cpu_relax();
	}

unlock:
	ok = vi->ctrl->status == VIRTIO_NET_OK;
	mutex_unlock(&vi->cvq_lock);
	return ok;
}

static bool virtnet_send_command(struct virtnet_info *vi, u8 class, u8 cmd,
				 struct scatterlist *out)
{
	return virtnet_send_command_reply(vi, class, cmd, out, NULL);
}

static int virtnet_set_mac_address(struct net_device *dev, void *p)
{
	struct virtnet_info *vi = netdev_priv(dev);
	struct virtio_device *vdev = vi->vdev;
	int ret;
	struct sockaddr *addr;
	struct scatterlist sg;

	if (virtio_has_feature(vi->vdev, VIRTIO_NET_F_STANDBY))
		return -EOPNOTSUPP;

	addr = kmemdup(p, sizeof(*addr), GFP_KERNEL);
	if (!addr)
		return -ENOMEM;

	ret = eth_prepare_mac_addr_change(dev, addr);
	if (ret)
		goto out;

	if (virtio_has_feature(vdev, VIRTIO_NET_F_CTRL_MAC_ADDR)) {
		sg_init_one(&sg, addr->sa_data, dev->addr_len);
		if (!virtnet_send_command(vi, VIRTIO_NET_CTRL_MAC,
					  VIRTIO_NET_CTRL_MAC_ADDR_SET, &sg)) {
			dev_warn(&vdev->dev,
				 "Failed to set mac address by vq command.\n");
			ret = -EINVAL;
			goto out;
		}
	} else if (virtio_has_feature(vdev, VIRTIO_NET_F_MAC) &&
		   !virtio_has_feature(vdev, VIRTIO_F_VERSION_1)) {
		unsigned int i;

		/* Naturally, this has an atomicity problem. */
		for (i = 0; i < dev->addr_len; i++)
			virtio_cwrite8(vdev,
				       offsetof(struct virtio_net_config, mac) +
				       i, addr->sa_data[i]);
	}

	eth_commit_mac_addr_change(dev, p);
	ret = 0;

out:
	kfree(addr);
	return ret;
}

static void virtnet_stats(struct net_device *dev,
			  struct rtnl_link_stats64 *tot)
{
	struct virtnet_info *vi = netdev_priv(dev);
	unsigned int start;
	int i;

	for (i = 0; i < vi->max_queue_pairs; i++) {
		u64 tpackets, tbytes, terrors, rpackets, rbytes, rdrops;
		struct receive_queue *rq = &vi->rq[i];
		struct send_queue *sq = &vi->sq[i];

		do {
			start = u64_stats_fetch_begin(&sq->stats.syncp);
			tpackets = u64_stats_read(&sq->stats.packets);
			tbytes   = u64_stats_read(&sq->stats.bytes);
			terrors  = u64_stats_read(&sq->stats.tx_timeouts);
		} while (u64_stats_fetch_retry(&sq->stats.syncp, start));

		do {
			start = u64_stats_fetch_begin(&rq->stats.syncp);
			rpackets = u64_stats_read(&rq->stats.packets);
			rbytes   = u64_stats_read(&rq->stats.bytes);
			rdrops   = u64_stats_read(&rq->stats.drops);
		} while (u64_stats_fetch_retry(&rq->stats.syncp, start));

		tot->rx_packets += rpackets;
		tot->tx_packets += tpackets;
		tot->rx_bytes   += rbytes;
		tot->tx_bytes   += tbytes;
		tot->rx_dropped += rdrops;
		tot->tx_errors  += terrors;
	}

	tot->tx_dropped = DEV_STATS_READ(dev, tx_dropped);
	tot->tx_fifo_errors = DEV_STATS_READ(dev, tx_fifo_errors);
	tot->rx_length_errors = DEV_STATS_READ(dev, rx_length_errors);
	tot->rx_frame_errors = DEV_STATS_READ(dev, rx_frame_errors);
}

static void virtnet_ack_link_announce(struct virtnet_info *vi)
{
	if (!virtnet_send_command(vi, VIRTIO_NET_CTRL_ANNOUNCE,
				  VIRTIO_NET_CTRL_ANNOUNCE_ACK, NULL))
		dev_warn(&vi->dev->dev, "Failed to ack link announce.\n");
}

static bool virtnet_commit_rss_command(struct virtnet_info *vi);

static void virtnet_rss_update_by_qpairs(struct virtnet_info *vi, u16 queue_pairs)
{
	u32 indir_val = 0;
	int i = 0;

	for (; i < vi->rss_indir_table_size; ++i) {
		indir_val = ethtool_rxfh_indir_default(i, queue_pairs);
		vi->rss_hdr->indirection_table[i] = cpu_to_le16(indir_val);
	}
	vi->rss_trailer.max_tx_vq = cpu_to_le16(queue_pairs);
}

static int virtnet_set_queues(struct virtnet_info *vi, u16 queue_pairs)
{
	struct virtio_net_ctrl_mq *mq __free(kfree) = NULL;
	struct virtio_net_rss_config_hdr *old_rss_hdr;
	struct virtio_net_rss_config_trailer old_rss_trailer;
	struct net_device *dev = vi->dev;
	struct scatterlist sg;

	if (!vi->has_cvq || !virtio_has_feature(vi->vdev, VIRTIO_NET_F_MQ))
		return 0;

	/* Firstly check if we need update rss. Do updating if both (1) rss enabled and
	 * (2) no user configuration.
	 *
	 * During rss command processing, device updates queue_pairs using rss.max_tx_vq. That is,
	 * the device updates queue_pairs together with rss, so we can skip the sperate queue_pairs
	 * update (VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET below) and return directly.
	 */
	if (vi->has_rss && !netif_is_rxfh_configured(dev)) {
		old_rss_hdr = vi->rss_hdr;
		old_rss_trailer = vi->rss_trailer;
		vi->rss_hdr = devm_kzalloc(&dev->dev, virtnet_rss_hdr_size(vi), GFP_KERNEL);
		if (!vi->rss_hdr) {
			vi->rss_hdr = old_rss_hdr;
			return -ENOMEM;
		}

		*vi->rss_hdr = *old_rss_hdr;
		virtnet_rss_update_by_qpairs(vi, queue_pairs);

		if (!virtnet_commit_rss_command(vi)) {
			/* restore ctrl_rss if commit_rss_command failed */
			devm_kfree(&dev->dev, vi->rss_hdr);
			vi->rss_hdr = old_rss_hdr;
			vi->rss_trailer = old_rss_trailer;

			dev_warn(&dev->dev, "Fail to set num of queue pairs to %d, because committing RSS failed\n",
				 queue_pairs);
			return -EINVAL;
		}
		devm_kfree(&dev->dev, old_rss_hdr);
		goto succ;
	}

	mq = kzalloc(sizeof(*mq), GFP_KERNEL);
	if (!mq)
		return -ENOMEM;

	mq->virtqueue_pairs = cpu_to_virtio16(vi->vdev, queue_pairs);
	sg_init_one(&sg, mq, sizeof(*mq));

	if (!virtnet_send_command(vi, VIRTIO_NET_CTRL_MQ,
				  VIRTIO_NET_CTRL_MQ_VQ_PAIRS_SET, &sg)) {
		dev_warn(&dev->dev, "Fail to set num of queue pairs to %d\n",
			 queue_pairs);
		return -EINVAL;
	}
succ:
	vi->curr_queue_pairs = queue_pairs;
	/* virtnet_open() will refill when device is going to up. */
	spin_lock_bh(&vi->refill_lock);
	if (dev->flags & IFF_UP && vi->refill_enabled)
		schedule_delayed_work(&vi->refill, 0);
	spin_unlock_bh(&vi->refill_lock);

	return 0;
}

static int virtnet_close(struct net_device *dev)
{
	struct virtnet_info *vi = netdev_priv(dev);
	int i;

	/* Make sure NAPI doesn't schedule refill work */
	disable_delayed_refill(vi);
	/* Make sure refill_work doesn't re-enable napi! */
	cancel_delayed_work_sync(&vi->refill);
	/* Prevent the config change callback from changing carrier
	 * after close
	 */
	virtio_config_driver_disable(vi->vdev);
	/* Stop getting status/speed updates: we don't care until next
	 * open
	 */
	cancel_work_sync(&vi->config_work);

	for (i = 0; i < vi->max_queue_pairs; i++) {
		virtnet_disable_queue_pair(vi, i);
		virtnet_cancel_dim(vi, &vi->rq[i].dim);
	}

	netif_carrier_off(dev);

	return 0;
}

static void virtnet_rx_mode_work(struct work_struct *work)
{
	struct virtnet_info *vi =
		container_of(work, struct virtnet_info, rx_mode_work);
	u8 *promisc_allmulti  __free(kfree) = NULL;
	struct net_device *dev = vi->dev;
	struct scatterlist sg[2];
	struct virtio_net_ctrl_mac *mac_data;
	struct netdev_hw_addr *ha;
	int uc_count;
	int mc_count;
	void *buf;
	int i;

	/* We can't dynamically set ndo_set_rx_mode, so return gracefully */
	if (!virtio_has_feature(vi->vdev, VIRTIO_NET_F_CTRL_RX))
		return;

	promisc_allmulti = kzalloc(sizeof(*promisc_allmulti), GFP_KERNEL);
	if (!promisc_allmulti) {
		dev_warn(&dev->dev, "Failed to set RX mode, no memory.\n");
		return;
	}

	rtnl_lock();

	*promisc_allmulti = !!(dev->flags & IFF_PROMISC);
	sg_init_one(sg, promisc_allmulti, sizeof(*promisc_allmulti));

	if (!virtnet_send_command(vi, VIRTIO_NET_CTRL_RX,
				  VIRTIO_NET_CTRL_RX_PROMISC, sg))
		dev_warn(&dev->dev, "Failed to %sable promisc mode.\n",
			 *promisc_allmulti ? "en" : "dis");

	*promisc_allmulti = !!(dev->flags & IFF_ALLMULTI);
	sg_init_one(sg, promisc_allmulti, sizeof(*promisc_allmulti));

	if (!virtnet_send_command(vi, VIRTIO_NET_CTRL_RX,
				  VIRTIO_NET_CTRL_RX_ALLMULTI, sg))
		dev_warn(&dev->dev, "Failed to %sable allmulti mode.\n",
			 *promisc_allmulti ? "en" : "dis");

	netif_addr_lock_bh(dev);

	uc_count = netdev_uc_count(dev);
	mc_count = netdev_mc_count(dev);
	/* MAC filter - use one buffer for both lists */
	buf = kzalloc(((uc_count + mc_count) * ETH_ALEN) +
		      (2 * sizeof(mac_data->entries)), GFP_ATOMIC);
	mac_data = buf;
	if (!buf) {
		netif_addr_unlock_bh(dev);
		rtnl_unlock();
		return;
	}

	sg_init_table(sg, 2);

	/* Store the unicast list and count in the front of the buffer */
	mac_data->entries = cpu_to_virtio32(vi->vdev, uc_count);
	i = 0;
	netdev_for_each_uc_addr(ha, dev)
		memcpy(&mac_data->macs[i++][0], ha->addr, ETH_ALEN);

	sg_set_buf(&sg[0], mac_data,
		   sizeof(mac_data->entries) + (uc_count * ETH_ALEN));

	/* multicast list and count fill the end */
	mac_data = (void *)&mac_data->macs[uc_count][0];

	mac_data->entries = cpu_to_virtio32(vi->vdev, mc_count);
	i = 0;
	netdev_for_each_mc_addr(ha, dev)
		memcpy(&mac_data->macs[i++][0], ha->addr, ETH_ALEN);

	netif_addr_unlock_bh(dev);

	sg_set_buf(&sg[1], mac_data,
		   sizeof(mac_data->entries) + (mc_count * ETH_ALEN));

	if (!virtnet_send_command(vi, VIRTIO_NET_CTRL_MAC,
				  VIRTIO_NET_CTRL_MAC_TABLE_SET, sg))
		dev_warn(&dev->dev, "Failed to set MAC filter table.\n");

	rtnl_unlock();

	kfree(buf);
}

static void virtnet_set_rx_mode(struct net_device *dev)
{
	struct virtnet_info *vi = netdev_priv(dev);

	if (vi->rx_mode_work_enabled)
		schedule_work(&vi->rx_mode_work);
}

static int virtnet_vlan_rx_add_vid(struct net_device *dev,
				   __be16 proto, u16 vid)
{
	struct virtnet_info *vi = netdev_priv(dev);
	__virtio16 *_vid __free(kfree) = NULL;
	struct scatterlist sg;

	_vid = kzalloc(sizeof(*_vid), GFP_KERNEL);
	if (!_vid)
		return -ENOMEM;

	*_vid = cpu_to_virtio16(vi->vdev, vid);
	sg_init_one(&sg, _vid, sizeof(*_vid));

	if (!virtnet_send_command(vi, VIRTIO_NET_CTRL_VLAN,
				  VIRTIO_NET_CTRL_VLAN_ADD, &sg))
		dev_warn(&dev->dev, "Failed to add VLAN ID %d.\n", vid);
	return 0;
}

static int virtnet_vlan_rx_kill_vid(struct net_device *dev,
				    __be16 proto, u16 vid)
{
	struct virtnet_info *vi = netdev_priv(dev);
	__virtio16 *_vid __free(kfree) = NULL;
	struct scatterlist sg;

	_vid = kzalloc(sizeof(*_vid), GFP_KERNEL);
	if (!_vid)
		return -ENOMEM;

	*_vid = cpu_to_virtio16(vi->vdev, vid);
	sg_init_one(&sg, _vid, sizeof(*_vid));

	if (!virtnet_send_command(vi, VIRTIO_NET_CTRL_VLAN,
				  VIRTIO_NET_CTRL_VLAN_DEL, &sg))
		dev_warn(&dev->dev, "Failed to kill VLAN ID %d.\n", vid);
	return 0;
}

static void virtnet_clean_affinity(struct virtnet_info *vi)
{
	int i;

	if (vi->affinity_hint_set) {
		for (i = 0; i < vi->max_queue_pairs; i++) {
			virtqueue_set_affinity(vi->rq[i].vq, NULL);
			virtqueue_set_affinity(vi->sq[i].vq, NULL);
		}

		vi->affinity_hint_set = false;
	}
}

static void virtnet_set_affinity(struct virtnet_info *vi)
{
	cpumask_var_t mask;
	int stragglers;
	int group_size;
	int i, start = 0, cpu;
	int num_cpu;
	int stride;

	if (!zalloc_cpumask_var(&mask, GFP_KERNEL)) {
		virtnet_clean_affinity(vi);
		return;
	}

	num_cpu = num_online_cpus();
	stride = max_t(int, num_cpu / vi->curr_queue_pairs, 1);
	stragglers = num_cpu >= vi->curr_queue_pairs ?
			num_cpu % vi->curr_queue_pairs :
			0;

	for (i = 0; i < vi->curr_queue_pairs; i++) {
		group_size = stride + (i < stragglers ? 1 : 0);

		for_each_online_cpu_wrap(cpu, start) {
			if (!group_size--) {
				start = cpu;
				break;
			}
			cpumask_set_cpu(cpu, mask);
		}

		virtqueue_set_affinity(vi->rq[i].vq, mask);
		virtqueue_set_affinity(vi->sq[i].vq, mask);
		__netif_set_xps_queue(vi->dev, cpumask_bits(mask), i, XPS_CPUS);
		cpumask_clear(mask);
	}

	vi->affinity_hint_set = true;
	free_cpumask_var(mask);
}

static int virtnet_cpu_online(unsigned int cpu, struct hlist_node *node)
{
	struct virtnet_info *vi = hlist_entry_safe(node, struct virtnet_info,
						   node);
	virtnet_set_affinity(vi);
	return 0;
}

static int virtnet_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
	struct virtnet_info *vi = hlist_entry_safe(node, struct virtnet_info,
						   node_dead);
	virtnet_set_affinity(vi);
	return 0;
}

static int virtnet_cpu_down_prep(unsigned int cpu, struct hlist_node *node)
{
	struct virtnet_info *vi = hlist_entry_safe(node, struct virtnet_info,
						   node);

	virtnet_clean_affinity(vi);
	return 0;
}

static enum cpuhp_state virtionet_online;

static int virtnet_cpu_notif_add(struct virtnet_info *vi)
{
	int ret;

	ret = cpuhp_state_add_instance_nocalls(virtionet_online, &vi->node);
	if (ret)
		return ret;
	ret = cpuhp_state_add_instance_nocalls(CPUHP_VIRT_NET_DEAD,
					       &vi->node_dead);
	if (!ret)
		return ret;
	cpuhp_state_remove_instance_nocalls(virtionet_online, &vi->node);
	return ret;
}

static void virtnet_cpu_notif_remove(struct virtnet_info *vi)
{
	cpuhp_state_remove_instance_nocalls(virtionet_online, &vi->node);
	cpuhp_state_remove_instance_nocalls(CPUHP_VIRT_NET_DEAD,
					    &vi->node_dead);
}

static int virtnet_send_ctrl_coal_vq_cmd(struct virtnet_info *vi,
					 u16 vqn, u32 max_usecs, u32 max_packets)
{
	struct virtio_net_ctrl_coal_vq *coal_vq __free(kfree) = NULL;
	struct scatterlist sgs;

	coal_vq = kzalloc(sizeof(*coal_vq), GFP_KERNEL);
	if (!coal_vq)
		return -ENOMEM;

	coal_vq->vqn = cpu_to_le16(vqn);
	coal_vq->coal.max_usecs = cpu_to_le32(max_usecs);
	coal_vq->coal.max_packets = cpu_to_le32(max_packets);
	sg_init_one(&sgs, coal_vq, sizeof(*coal_vq));

	if (!virtnet_send_command(vi, VIRTIO_NET_CTRL_NOTF_COAL,
				  VIRTIO_NET_CTRL_NOTF_COAL_VQ_SET,
				  &sgs))
		return -EINVAL;

	return 0;
}

static int virtnet_send_rx_ctrl_coal_vq_cmd(struct virtnet_info *vi,
					    u16 queue, u32 max_usecs,
					    u32 max_packets)
{
	int err;

	if (!virtio_has_feature(vi->vdev, VIRTIO_NET_F_VQ_NOTF_COAL))
		return -EOPNOTSUPP;

	err = virtnet_send_ctrl_coal_vq_cmd(vi, rxq2vq(queue),
					    max_usecs, max_packets);
	if (err)
		return err;

	vi->rq[queue].intr_coal.max_usecs = max_usecs;
	vi->rq[queue].intr_coal.max_packets = max_packets;

	return 0;
}

static int virtnet_send_tx_ctrl_coal_vq_cmd(struct virtnet_info *vi,
					    u16 queue, u32 max_usecs,
					    u32 max_packets)
{
	int err;

	if (!virtio_has_feature(vi->vdev, VIRTIO_NET_F_VQ_NOTF_COAL))
		return -EOPNOTSUPP;

	err = virtnet_send_ctrl_coal_vq_cmd(vi, txq2vq(queue),
					    max_usecs, max_packets);
	if (err)
		return err;

	vi->sq[queue].intr_coal.max_usecs = max_usecs;
	vi->sq[queue].intr_coal.max_packets = max_packets;

	return 0;
}

static void virtnet_get_ringparam(struct net_device *dev,
				  struct ethtool_ringparam *ring,
				  struct kernel_ethtool_ringparam *kernel_ring,
				  struct netlink_ext_ack *extack)
{
	struct virtnet_info *vi = netdev_priv(dev);

	ring->rx_max_pending = vi->rq[0].vq->num_max;
	ring->tx_max_pending = vi->sq[0].vq->num_max;
	ring->rx_pending = virtqueue_get_vring_size(vi->rq[0].vq);
	ring->tx_pending = virtqueue_get_vring_size(vi->sq[0].vq);
}

static int virtnet_set_ringparam(struct net_device *dev,
				 struct ethtool_ringparam *ring,
				 struct kernel_ethtool_ringparam *kernel_ring,
				 struct netlink_ext_ack *extack)
{
	struct virtnet_info *vi = netdev_priv(dev);
	u32 rx_pending, tx_pending;
	struct receive_queue *rq;
	struct send_queue *sq;
	int i, err;

	if (ring->rx_mini_pending || ring->rx_jumbo_pending)
		return -EINVAL;

	rx_pending = virtqueue_get_vring_size(vi->rq[0].vq);
	tx_pending = virtqueue_get_vring_size(vi->sq[0].vq);

	if (ring->rx_pending == rx_pending &&
	    ring->tx_pending == tx_pending)
		return 0;

	if (ring->rx_pending > vi->rq[0].vq->num_max)
		return -EINVAL;

	if (ring->tx_pending > vi->sq[0].vq->num_max)
		return -EINVAL;

	for (i = 0; i < vi->max_queue_pairs; i++) {
		rq = vi->rq + i;
		sq = vi->sq + i;

		if (ring->tx_pending != tx_pending) {
			err = virtnet_tx_resize(vi, sq, ring->tx_pending);
			if (err)
				return err;

			/* Upon disabling and re-enabling a transmit virtqueue, the device must
			 * set the coalescing parameters of the virtqueue to those configured
			 * through the VIRTIO_NET_CTRL_NOTF_COAL_TX_SET command, or, if the driver
			 * did not set any TX coalescing parameters, to 0.
			 */
			err = virtnet_send_tx_ctrl_coal_vq_cmd(vi, i,
							       vi->intr_coal_tx.max_usecs,
							       vi->intr_coal_tx.max_packets);

			/* Don't break the tx resize action if the vq coalescing is not
			 * supported. The same is true for rx resize below.
			 */
			if (err && err != -EOPNOTSUPP)
				return err;
		}

		if (ring->rx_pending != rx_pending) {
			err = virtnet_rx_resize(vi, rq, ring->rx_pending);
			if (err)
				return err;

			/* The reason is same as the transmit virtqueue reset */
			mutex_lock(&vi->rq[i].dim_lock);
			err = virtnet_send_rx_ctrl_coal_vq_cmd(vi, i,
							       vi->intr_coal_rx.max_usecs,
							       vi->intr_coal_rx.max_packets);
			mutex_unlock(&vi->rq[i].dim_lock);
			if (err && err != -EOPNOTSUPP)
				return err;
		}
	}

	return 0;
}

static bool virtnet_commit_rss_command(struct virtnet_info *vi)
{
	struct net_device *dev = vi->dev;
	struct scatterlist sgs[2];

	/* prepare sgs */
	sg_init_table(sgs, 2);
	sg_set_buf(&sgs[0], vi->rss_hdr, virtnet_rss_hdr_size(vi));
	sg_set_buf(&sgs[1], &vi->rss_trailer, virtnet_rss_trailer_size(vi));

	if (!virtnet_send_command(vi, VIRTIO_NET_CTRL_MQ,
				  vi->has_rss ? VIRTIO_NET_CTRL_MQ_RSS_CONFIG
				  : VIRTIO_NET_CTRL_MQ_HASH_CONFIG, sgs))
		goto err;

	return true;

err:
	dev_warn(&dev->dev, "VIRTIONET issue with committing RSS sgs\n");
	return false;

}

static void virtnet_init_default_rss(struct virtnet_info *vi)
{
	vi->rss_hdr->hash_types = cpu_to_le32(vi->rss_hash_types_supported);
	vi->rss_hash_types_saved = vi->rss_hash_types_supported;
	vi->rss_hdr->indirection_table_mask = vi->rss_indir_table_size
						? cpu_to_le16(vi->rss_indir_table_size - 1) : 0;
	vi->rss_hdr->unclassified_queue = 0;

	virtnet_rss_update_by_qpairs(vi, vi->curr_queue_pairs);

	vi->rss_trailer.hash_key_length = vi->rss_key_size;

	netdev_rss_key_fill(vi->rss_hash_key_data, vi->rss_key_size);
}

static void virtnet_get_hashflow(const struct virtnet_info *vi, struct ethtool_rxnfc *info)
{
	info->data = 0;
	switch (info->flow_type) {
	case TCP_V4_FLOW:
		if (vi->rss_hash_types_saved & VIRTIO_NET_RSS_HASH_TYPE_TCPv4) {
			info->data = RXH_IP_SRC | RXH_IP_DST |
						 RXH_L4_B_0_1 | RXH_L4_B_2_3;
		} else if (vi->rss_hash_types_saved & VIRTIO_NET_RSS_HASH_TYPE_IPv4) {
			info->data = RXH_IP_SRC | RXH_IP_DST;
		}
		break;
	case TCP_V6_FLOW:
		if (vi->rss_hash_types_saved & VIRTIO_NET_RSS_HASH_TYPE_TCPv6) {
			info->data = RXH_IP_SRC | RXH_IP_DST |
						 RXH_L4_B_0_1 | RXH_L4_B_2_3;
		} else if (vi->rss_hash_types_saved & VIRTIO_NET_RSS_HASH_TYPE_IPv6) {
			info->data = RXH_IP_SRC | RXH_IP_DST;
		}
		break;
	case UDP_V4_FLOW:
		if (vi->rss_hash_types_saved & VIRTIO_NET_RSS_HASH_TYPE_UDPv4) {
			info->data = RXH_IP_SRC | RXH_IP_DST |
						 RXH_L4_B_0_1 | RXH_L4_B_2_3;
		} else if (vi->rss_hash_types_saved & VIRTIO_NET_RSS_HASH_TYPE_IPv4) {
			info->data = RXH_IP_SRC | RXH_IP_DST;
		}
		break;
	case UDP_V6_FLOW:
		if (vi->rss_hash_types_saved & VIRTIO_NET_RSS_HASH_TYPE_UDPv6) {
			info->data = RXH_IP_SRC | RXH_IP_DST |
						 RXH_L4_B_0_1 | RXH_L4_B_2_3;
		} else if (vi->rss_hash_types_saved & VIRTIO_NET_RSS_HASH_TYPE_IPv6) {
			info->data = RXH_IP_SRC | RXH_IP_DST;
		}
		break;
	case IPV4_FLOW:
		if (vi->rss_hash_types_saved & VIRTIO_NET_RSS_HASH_TYPE_IPv4)
			info->data = RXH_IP_SRC | RXH_IP_DST;

		break;
	case IPV6_FLOW:
		if (vi->rss_hash_types_saved & VIRTIO_NET_RSS_HASH_TYPE_IPv6)
			info->data = RXH_IP_SRC | RXH_IP_DST;

		break;
	default:
		info->data = 0;
		break;
	}
}

static bool virtnet_set_hashflow(struct virtnet_info *vi, struct ethtool_rxnfc *info)
{
	u32 new_hashtypes = vi->rss_hash_types_saved;
	bool is_disable = info->data & RXH_DISCARD;
	bool is_l4 = info->data == (RXH_IP_SRC | RXH_IP_DST | RXH_L4_B_0_1 | RXH_L4_B_2_3);

	/* supports only 'sd', 'sdfn' and 'r' */
	if (!((info->data == (RXH_IP_SRC | RXH_IP_DST)) | is_l4 | is_disable))
		return false;

	switch (info->flow_type) {
	case TCP_V4_FLOW:
		new_hashtypes &= ~(VIRTIO_NET_RSS_HASH_TYPE_IPv4 | VIRTIO_NET_RSS_HASH_TYPE_TCPv4);
		if (!is_disable)
			new_hashtypes |= VIRTIO_NET_RSS_HASH_TYPE_IPv4
				| (is_l4 ? VIRTIO_NET_RSS_HASH_TYPE_TCPv4 : 0);
		break;
	case UDP_V4_FLOW:
		new_hashtypes &= ~(VIRTIO_NET_RSS_HASH_TYPE_IPv4 | VIRTIO_NET_RSS_HASH_TYPE_UDPv4);
		if (!is_disable)
			new_hashtypes |= VIRTIO_NET_RSS_HASH_TYPE_IPv4
				| (is_l4 ? VIRTIO_NET_RSS_HASH_TYPE_UDPv4 : 0);
		break;
	case IPV4_FLOW:
		new_hashtypes &= ~VIRTIO_NET_RSS_HASH_TYPE_IPv4;
		if (!is_disable)
			new_hashtypes = VIRTIO_NET_RSS_HASH_TYPE_IPv4;
		break;
	case TCP_V6_FLOW:
		new_hashtypes &= ~(VIRTIO_NET_RSS_HASH_TYPE_IPv6 | VIRTIO_NET_RSS_HASH_TYPE_TCPv6);
		if (!is_disable)
			new_hashtypes |= VIRTIO_NET_RSS_HASH_TYPE_IPv6
				| (is_l4 ? VIRTIO_NET_RSS_HASH_TYPE_TCPv6 : 0);
		break;
	case UDP_V6_FLOW:
		new_hashtypes &= ~(VIRTIO_NET_RSS_HASH_TYPE_IPv6 | VIRTIO_NET_RSS_HASH_TYPE_UDPv6);
		if (!is_disable)
			new_hashtypes |= VIRTIO_NET_RSS_HASH_TYPE_IPv6
				| (is_l4 ? VIRTIO_NET_RSS_HASH_TYPE_UDPv6 : 0);
		break;
	case IPV6_FLOW:
		new_hashtypes &= ~VIRTIO_NET_RSS_HASH_TYPE_IPv6;
		if (!is_disable)
			new_hashtypes = VIRTIO_NET_RSS_HASH_TYPE_IPv6;
		break;
	default:
		/* unsupported flow */
		return false;
	}

	/* if unsupported hashtype was set */
	if (new_hashtypes != (new_hashtypes & vi->rss_hash_types_supported))
		return false;

	if (new_hashtypes != vi->rss_hash_types_saved) {
		vi->rss_hash_types_saved = new_hashtypes;
		vi->rss_hdr->hash_types = cpu_to_le32(vi->rss_hash_types_saved);
		if (vi->dev->features & NETIF_F_RXHASH)
			return virtnet_commit_rss_command(vi);
	}

	return true;
}

static void virtnet_get_drvinfo(struct net_device *dev,
				struct ethtool_drvinfo *info)
{
	struct virtnet_info *vi = netdev_priv(dev);
	struct virtio_device *vdev = vi->vdev;

	strscpy(info->driver, KBUILD_MODNAME, sizeof(info->driver));
	strscpy(info->version, VIRTNET_DRIVER_VERSION, sizeof(info->version));
	strscpy(info->bus_info, virtio_bus_name(vdev), sizeof(info->bus_info));

}

/* TODO: Eliminate OOO packets during switching */
static int virtnet_set_channels(struct net_device *dev,
				struct ethtool_channels *channels)
{
	struct virtnet_info *vi = netdev_priv(dev);
	u16 queue_pairs = channels->combined_count;
	int err;

	/* We don't support separate rx/tx channels.
	 * We don't allow setting 'other' channels.
	 */
	if (channels->rx_count || channels->tx_count || channels->other_count)
		return -EINVAL;

	if (queue_pairs > vi->max_queue_pairs || queue_pairs == 0)
		return -EINVAL;

	/* For now we don't support modifying channels while XDP is loaded
	 * also when XDP is loaded all RX queues have XDP programs so we only
	 * need to check a single RX queue.
	 */
	if (vi->rq[0].xdp_prog)
		return -EINVAL;

	cpus_read_lock();
	err = virtnet_set_queues(vi, queue_pairs);
	if (err) {
		cpus_read_unlock();
		goto err;
	}
	virtnet_set_affinity(vi);
	cpus_read_unlock();

	netif_set_real_num_tx_queues(dev, queue_pairs);
	netif_set_real_num_rx_queues(dev, queue_pairs);
 err:
	return err;
}

static void virtnet_stats_sprintf(u8 **p, const char *fmt, const char *noq_fmt,
				  int num, int qid, const struct virtnet_stat_desc *desc)
{
	int i;

	if (qid < 0) {
		for (i = 0; i < num; ++i)
			ethtool_sprintf(p, noq_fmt, desc[i].desc);
	} else {
		for (i = 0; i < num; ++i)
			ethtool_sprintf(p, fmt, qid, desc[i].desc);
	}
}

/* qid == -1: for rx/tx queue total field */
static void virtnet_get_stats_string(struct virtnet_info *vi, int type, int qid, u8 **data)
{
	const struct virtnet_stat_desc *desc;
	const char *fmt, *noq_fmt;
	u8 *p = *data;
	u32 num;

	if (type == VIRTNET_Q_TYPE_CQ && qid >= 0) {
		noq_fmt = "cq_hw_%s";

		if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_CVQ) {
			desc = &virtnet_stats_cvq_desc[0];
			num = ARRAY_SIZE(virtnet_stats_cvq_desc);

			virtnet_stats_sprintf(&p, NULL, noq_fmt, num, -1, desc);
		}
	}

	if (type == VIRTNET_Q_TYPE_RX) {
		fmt = "rx%u_%s";
		noq_fmt = "rx_%s";

		desc = &virtnet_rq_stats_desc[0];
		num = ARRAY_SIZE(virtnet_rq_stats_desc);

		virtnet_stats_sprintf(&p, fmt, noq_fmt, num, qid, desc);

		fmt = "rx%u_hw_%s";
		noq_fmt = "rx_hw_%s";

		if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_RX_BASIC) {
			desc = &virtnet_stats_rx_basic_desc[0];
			num = ARRAY_SIZE(virtnet_stats_rx_basic_desc);

			virtnet_stats_sprintf(&p, fmt, noq_fmt, num, qid, desc);
		}

		if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_RX_CSUM) {
			desc = &virtnet_stats_rx_csum_desc[0];
			num = ARRAY_SIZE(virtnet_stats_rx_csum_desc);

			virtnet_stats_sprintf(&p, fmt, noq_fmt, num, qid, desc);
		}

		if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_RX_SPEED) {
			desc = &virtnet_stats_rx_speed_desc[0];
			num = ARRAY_SIZE(virtnet_stats_rx_speed_desc);

			virtnet_stats_sprintf(&p, fmt, noq_fmt, num, qid, desc);
		}
	}

	if (type == VIRTNET_Q_TYPE_TX) {
		fmt = "tx%u_%s";
		noq_fmt = "tx_%s";

		desc = &virtnet_sq_stats_desc[0];
		num = ARRAY_SIZE(virtnet_sq_stats_desc);

		virtnet_stats_sprintf(&p, fmt, noq_fmt, num, qid, desc);

		fmt = "tx%u_hw_%s";
		noq_fmt = "tx_hw_%s";

		if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_TX_BASIC) {
			desc = &virtnet_stats_tx_basic_desc[0];
			num = ARRAY_SIZE(virtnet_stats_tx_basic_desc);

			virtnet_stats_sprintf(&p, fmt, noq_fmt, num, qid, desc);
		}

		if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_TX_GSO) {
			desc = &virtnet_stats_tx_gso_desc[0];
			num = ARRAY_SIZE(virtnet_stats_tx_gso_desc);

			virtnet_stats_sprintf(&p, fmt, noq_fmt, num, qid, desc);
		}

		if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_TX_SPEED) {
			desc = &virtnet_stats_tx_speed_desc[0];
			num = ARRAY_SIZE(virtnet_stats_tx_speed_desc);

			virtnet_stats_sprintf(&p, fmt, noq_fmt, num, qid, desc);
		}
	}

	*data = p;
}

struct virtnet_stats_ctx {
	/* The stats are write to qstats or ethtool -S */
	bool to_qstat;

	/* Used to calculate the offset inside the output buffer. */
	u32 desc_num[3];

	/* The actual supported stat types. */
	u64 bitmap[3];

	/* Used to calculate the reply buffer size. */
	u32 size[3];

	/* Record the output buffer. */
	u64 *data;
};

static void virtnet_stats_ctx_init(struct virtnet_info *vi,
				   struct virtnet_stats_ctx *ctx,
				   u64 *data, bool to_qstat)
{
	u32 queue_type;

	ctx->data = data;
	ctx->to_qstat = to_qstat;

	if (to_qstat) {
		ctx->desc_num[VIRTNET_Q_TYPE_RX] = ARRAY_SIZE(virtnet_rq_stats_desc_qstat);
		ctx->desc_num[VIRTNET_Q_TYPE_TX] = ARRAY_SIZE(virtnet_sq_stats_desc_qstat);

		queue_type = VIRTNET_Q_TYPE_RX;

		if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_RX_BASIC) {
			ctx->bitmap[queue_type]   |= VIRTIO_NET_STATS_TYPE_RX_BASIC;
			ctx->desc_num[queue_type] += ARRAY_SIZE(virtnet_stats_rx_basic_desc_qstat);
			ctx->size[queue_type]     += sizeof(struct virtio_net_stats_rx_basic);
		}

		if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_RX_CSUM) {
			ctx->bitmap[queue_type]   |= VIRTIO_NET_STATS_TYPE_RX_CSUM;
			ctx->desc_num[queue_type] += ARRAY_SIZE(virtnet_stats_rx_csum_desc_qstat);
			ctx->size[queue_type]     += sizeof(struct virtio_net_stats_rx_csum);
		}

		if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_RX_GSO) {
			ctx->bitmap[queue_type]   |= VIRTIO_NET_STATS_TYPE_RX_GSO;
			ctx->desc_num[queue_type] += ARRAY_SIZE(virtnet_stats_rx_gso_desc_qstat);
			ctx->size[queue_type]     += sizeof(struct virtio_net_stats_rx_gso);
		}

		if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_RX_SPEED) {
			ctx->bitmap[queue_type]   |= VIRTIO_NET_STATS_TYPE_RX_SPEED;
			ctx->desc_num[queue_type] += ARRAY_SIZE(virtnet_stats_rx_speed_desc_qstat);
			ctx->size[queue_type]     += sizeof(struct virtio_net_stats_rx_speed);
		}

		queue_type = VIRTNET_Q_TYPE_TX;

		if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_TX_BASIC) {
			ctx->bitmap[queue_type]   |= VIRTIO_NET_STATS_TYPE_TX_BASIC;
			ctx->desc_num[queue_type] += ARRAY_SIZE(virtnet_stats_tx_basic_desc_qstat);
			ctx->size[queue_type]     += sizeof(struct virtio_net_stats_tx_basic);
		}

		if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_TX_CSUM) {
			ctx->bitmap[queue_type]   |= VIRTIO_NET_STATS_TYPE_TX_CSUM;
			ctx->desc_num[queue_type] += ARRAY_SIZE(virtnet_stats_tx_csum_desc_qstat);
			ctx->size[queue_type]     += sizeof(struct virtio_net_stats_tx_csum);
		}

		if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_TX_GSO) {
			ctx->bitmap[queue_type]   |= VIRTIO_NET_STATS_TYPE_TX_GSO;
			ctx->desc_num[queue_type] += ARRAY_SIZE(virtnet_stats_tx_gso_desc_qstat);
			ctx->size[queue_type]     += sizeof(struct virtio_net_stats_tx_gso);
		}

		if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_TX_SPEED) {
			ctx->bitmap[queue_type]   |= VIRTIO_NET_STATS_TYPE_TX_SPEED;
			ctx->desc_num[queue_type] += ARRAY_SIZE(virtnet_stats_tx_speed_desc_qstat);
			ctx->size[queue_type]     += sizeof(struct virtio_net_stats_tx_speed);
		}

		return;
	}

	ctx->desc_num[VIRTNET_Q_TYPE_RX] = ARRAY_SIZE(virtnet_rq_stats_desc);
	ctx->desc_num[VIRTNET_Q_TYPE_TX] = ARRAY_SIZE(virtnet_sq_stats_desc);

	if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_CVQ) {
		queue_type = VIRTNET_Q_TYPE_CQ;

		ctx->bitmap[queue_type]   |= VIRTIO_NET_STATS_TYPE_CVQ;
		ctx->desc_num[queue_type] += ARRAY_SIZE(virtnet_stats_cvq_desc);
		ctx->size[queue_type]     += sizeof(struct virtio_net_stats_cvq);
	}

	queue_type = VIRTNET_Q_TYPE_RX;

	if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_RX_BASIC) {
		ctx->bitmap[queue_type]   |= VIRTIO_NET_STATS_TYPE_RX_BASIC;
		ctx->desc_num[queue_type] += ARRAY_SIZE(virtnet_stats_rx_basic_desc);
		ctx->size[queue_type]     += sizeof(struct virtio_net_stats_rx_basic);
	}

	if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_RX_CSUM) {
		ctx->bitmap[queue_type]   |= VIRTIO_NET_STATS_TYPE_RX_CSUM;
		ctx->desc_num[queue_type] += ARRAY_SIZE(virtnet_stats_rx_csum_desc);
		ctx->size[queue_type]     += sizeof(struct virtio_net_stats_rx_csum);
	}

	if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_RX_SPEED) {
		ctx->bitmap[queue_type]   |= VIRTIO_NET_STATS_TYPE_RX_SPEED;
		ctx->desc_num[queue_type] += ARRAY_SIZE(virtnet_stats_rx_speed_desc);
		ctx->size[queue_type]     += sizeof(struct virtio_net_stats_rx_speed);
	}

	queue_type = VIRTNET_Q_TYPE_TX;

	if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_TX_BASIC) {
		ctx->bitmap[queue_type]   |= VIRTIO_NET_STATS_TYPE_TX_BASIC;
		ctx->desc_num[queue_type] += ARRAY_SIZE(virtnet_stats_tx_basic_desc);
		ctx->size[queue_type]     += sizeof(struct virtio_net_stats_tx_basic);
	}

	if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_TX_GSO) {
		ctx->bitmap[queue_type]   |= VIRTIO_NET_STATS_TYPE_TX_GSO;
		ctx->desc_num[queue_type] += ARRAY_SIZE(virtnet_stats_tx_gso_desc);
		ctx->size[queue_type]     += sizeof(struct virtio_net_stats_tx_gso);
	}

	if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_TX_SPEED) {
		ctx->bitmap[queue_type]   |= VIRTIO_NET_STATS_TYPE_TX_SPEED;
		ctx->desc_num[queue_type] += ARRAY_SIZE(virtnet_stats_tx_speed_desc);
		ctx->size[queue_type]     += sizeof(struct virtio_net_stats_tx_speed);
	}
}

/* stats_sum_queue - Calculate the sum of the same fields in sq or rq.
 * @sum: the position to store the sum values
 * @num: field num
 * @q_value: the first queue fields
 * @q_num: number of the queues
 */
static void stats_sum_queue(u64 *sum, u32 num, u64 *q_value, u32 q_num)
{
	u32 step = num;
	int i, j;
	u64 *p;

	for (i = 0; i < num; ++i) {
		p = sum + i;
		*p = 0;

		for (j = 0; j < q_num; ++j)
			*p += *(q_value + i + j * step);
	}
}

static void virtnet_fill_total_fields(struct virtnet_info *vi,
				      struct virtnet_stats_ctx *ctx)
{
	u64 *data, *first_rx_q, *first_tx_q;
	u32 num_cq, num_rx, num_tx;

	num_cq = ctx->desc_num[VIRTNET_Q_TYPE_CQ];
	num_rx = ctx->desc_num[VIRTNET_Q_TYPE_RX];
	num_tx = ctx->desc_num[VIRTNET_Q_TYPE_TX];

	first_rx_q = ctx->data + num_rx + num_tx + num_cq;
	first_tx_q = first_rx_q + vi->curr_queue_pairs * num_rx;

	data = ctx->data;

	stats_sum_queue(data, num_rx, first_rx_q, vi->curr_queue_pairs);

	data = ctx->data + num_rx;

	stats_sum_queue(data, num_tx, first_tx_q, vi->curr_queue_pairs);
}

static void virtnet_fill_stats_qstat(struct virtnet_info *vi, u32 qid,
				     struct virtnet_stats_ctx *ctx,
				     const u8 *base, bool drv_stats, u8 reply_type)
{
	const struct virtnet_stat_desc *desc;
	const u64_stats_t *v_stat;
	u64 offset, bitmap;
	const __le64 *v;
	u32 queue_type;
	int i, num;

	queue_type = vq_type(vi, qid);
	bitmap = ctx->bitmap[queue_type];

	if (drv_stats) {
		if (queue_type == VIRTNET_Q_TYPE_RX) {
			desc = &virtnet_rq_stats_desc_qstat[0];
			num = ARRAY_SIZE(virtnet_rq_stats_desc_qstat);
		} else {
			desc = &virtnet_sq_stats_desc_qstat[0];
			num = ARRAY_SIZE(virtnet_sq_stats_desc_qstat);
		}

		for (i = 0; i < num; ++i) {
			offset = desc[i].qstat_offset / sizeof(*ctx->data);
			v_stat = (const u64_stats_t *)(base + desc[i].offset);
			ctx->data[offset] = u64_stats_read(v_stat);
		}
		return;
	}

	if (bitmap & VIRTIO_NET_STATS_TYPE_RX_BASIC) {
		desc = &virtnet_stats_rx_basic_desc_qstat[0];
		num = ARRAY_SIZE(virtnet_stats_rx_basic_desc_qstat);
		if (reply_type == VIRTIO_NET_STATS_TYPE_REPLY_RX_BASIC)
			goto found;
	}

	if (bitmap & VIRTIO_NET_STATS_TYPE_RX_CSUM) {
		desc = &virtnet_stats_rx_csum_desc_qstat[0];
		num = ARRAY_SIZE(virtnet_stats_rx_csum_desc_qstat);
		if (reply_type == VIRTIO_NET_STATS_TYPE_REPLY_RX_CSUM)
			goto found;
	}

	if (bitmap & VIRTIO_NET_STATS_TYPE_RX_GSO) {
		desc = &virtnet_stats_rx_gso_desc_qstat[0];
		num = ARRAY_SIZE(virtnet_stats_rx_gso_desc_qstat);
		if (reply_type == VIRTIO_NET_STATS_TYPE_REPLY_RX_GSO)
			goto found;
	}

	if (bitmap & VIRTIO_NET_STATS_TYPE_RX_SPEED) {
		desc = &virtnet_stats_rx_speed_desc_qstat[0];
		num = ARRAY_SIZE(virtnet_stats_rx_speed_desc_qstat);
		if (reply_type == VIRTIO_NET_STATS_TYPE_REPLY_RX_SPEED)
			goto found;
	}

	if (bitmap & VIRTIO_NET_STATS_TYPE_TX_BASIC) {
		desc = &virtnet_stats_tx_basic_desc_qstat[0];
		num = ARRAY_SIZE(virtnet_stats_tx_basic_desc_qstat);
		if (reply_type == VIRTIO_NET_STATS_TYPE_REPLY_TX_BASIC)
			goto found;
	}

	if (bitmap & VIRTIO_NET_STATS_TYPE_TX_CSUM) {
		desc = &virtnet_stats_tx_csum_desc_qstat[0];
		num = ARRAY_SIZE(virtnet_stats_tx_csum_desc_qstat);
		if (reply_type == VIRTIO_NET_STATS_TYPE_REPLY_TX_CSUM)
			goto found;
	}

	if (bitmap & VIRTIO_NET_STATS_TYPE_TX_GSO) {
		desc = &virtnet_stats_tx_gso_desc_qstat[0];
		num = ARRAY_SIZE(virtnet_stats_tx_gso_desc_qstat);
		if (reply_type == VIRTIO_NET_STATS_TYPE_REPLY_TX_GSO)
			goto found;
	}

	if (bitmap & VIRTIO_NET_STATS_TYPE_TX_SPEED) {
		desc = &virtnet_stats_tx_speed_desc_qstat[0];
		num = ARRAY_SIZE(virtnet_stats_tx_speed_desc_qstat);
		if (reply_type == VIRTIO_NET_STATS_TYPE_REPLY_TX_SPEED)
			goto found;
	}

	return;

found:
	for (i = 0; i < num; ++i) {
		offset = desc[i].qstat_offset / sizeof(*ctx->data);
		v = (const __le64 *)(base + desc[i].offset);
		ctx->data[offset] = le64_to_cpu(*v);
	}
}

/* virtnet_fill_stats - copy the stats to qstats or ethtool -S
 * The stats source is the device or the driver.
 *
 * @vi: virtio net info
 * @qid: the vq id
 * @ctx: stats ctx (initiated by virtnet_stats_ctx_init())
 * @base: pointer to the device reply or the driver stats structure.
 * @drv_stats: designate the base type (device reply, driver stats)
 * @type: the type of the device reply (if drv_stats is true, this must be zero)
 */
static void virtnet_fill_stats(struct virtnet_info *vi, u32 qid,
			       struct virtnet_stats_ctx *ctx,
			       const u8 *base, bool drv_stats, u8 reply_type)
{
	u32 queue_type, num_rx, num_tx, num_cq;
	const struct virtnet_stat_desc *desc;
	const u64_stats_t *v_stat;
	u64 offset, bitmap;
	const __le64 *v;
	int i, num;

	if (ctx->to_qstat)
		return virtnet_fill_stats_qstat(vi, qid, ctx, base, drv_stats, reply_type);

	num_cq = ctx->desc_num[VIRTNET_Q_TYPE_CQ];
	num_rx = ctx->desc_num[VIRTNET_Q_TYPE_RX];
	num_tx = ctx->desc_num[VIRTNET_Q_TYPE_TX];

	queue_type = vq_type(vi, qid);
	bitmap = ctx->bitmap[queue_type];

	/* skip the total fields of pairs */
	offset = num_rx + num_tx;

	if (queue_type == VIRTNET_Q_TYPE_TX) {
		offset += num_cq + num_rx * vi->curr_queue_pairs + num_tx * (qid / 2);

		num = ARRAY_SIZE(virtnet_sq_stats_desc);
		if (drv_stats) {
			desc = &virtnet_sq_stats_desc[0];
			goto drv_stats;
		}

		offset += num;

	} else if (queue_type == VIRTNET_Q_TYPE_RX) {
		offset += num_cq + num_rx * (qid / 2);

		num = ARRAY_SIZE(virtnet_rq_stats_desc);
		if (drv_stats) {
			desc = &virtnet_rq_stats_desc[0];
			goto drv_stats;
		}

		offset += num;
	}

	if (bitmap & VIRTIO_NET_STATS_TYPE_CVQ) {
		desc = &virtnet_stats_cvq_desc[0];
		num = ARRAY_SIZE(virtnet_stats_cvq_desc);
		if (reply_type == VIRTIO_NET_STATS_TYPE_REPLY_CVQ)
			goto found;

		offset += num;
	}

	if (bitmap & VIRTIO_NET_STATS_TYPE_RX_BASIC) {
		desc = &virtnet_stats_rx_basic_desc[0];
		num = ARRAY_SIZE(virtnet_stats_rx_basic_desc);
		if (reply_type == VIRTIO_NET_STATS_TYPE_REPLY_RX_BASIC)
			goto found;

		offset += num;
	}

	if (bitmap & VIRTIO_NET_STATS_TYPE_RX_CSUM) {
		desc = &virtnet_stats_rx_csum_desc[0];
		num = ARRAY_SIZE(virtnet_stats_rx_csum_desc);
		if (reply_type == VIRTIO_NET_STATS_TYPE_REPLY_RX_CSUM)
			goto found;

		offset += num;
	}

	if (bitmap & VIRTIO_NET_STATS_TYPE_RX_SPEED) {
		desc = &virtnet_stats_rx_speed_desc[0];
		num = ARRAY_SIZE(virtnet_stats_rx_speed_desc);
		if (reply_type == VIRTIO_NET_STATS_TYPE_REPLY_RX_SPEED)
			goto found;

		offset += num;
	}

	if (bitmap & VIRTIO_NET_STATS_TYPE_TX_BASIC) {
		desc = &virtnet_stats_tx_basic_desc[0];
		num = ARRAY_SIZE(virtnet_stats_tx_basic_desc);
		if (reply_type == VIRTIO_NET_STATS_TYPE_REPLY_TX_BASIC)
			goto found;

		offset += num;
	}

	if (bitmap & VIRTIO_NET_STATS_TYPE_TX_GSO) {
		desc = &virtnet_stats_tx_gso_desc[0];
		num = ARRAY_SIZE(virtnet_stats_tx_gso_desc);
		if (reply_type == VIRTIO_NET_STATS_TYPE_REPLY_TX_GSO)
			goto found;

		offset += num;
	}

	if (bitmap & VIRTIO_NET_STATS_TYPE_TX_SPEED) {
		desc = &virtnet_stats_tx_speed_desc[0];
		num = ARRAY_SIZE(virtnet_stats_tx_speed_desc);
		if (reply_type == VIRTIO_NET_STATS_TYPE_REPLY_TX_SPEED)
			goto found;

		offset += num;
	}

	return;

found:
	for (i = 0; i < num; ++i) {
		v = (const __le64 *)(base + desc[i].offset);
		ctx->data[offset + i] = le64_to_cpu(*v);
	}

	return;

drv_stats:
	for (i = 0; i < num; ++i) {
		v_stat = (const u64_stats_t *)(base + desc[i].offset);
		ctx->data[offset + i] = u64_stats_read(v_stat);
	}
}

static int __virtnet_get_hw_stats(struct virtnet_info *vi,
				  struct virtnet_stats_ctx *ctx,
				  struct virtio_net_ctrl_queue_stats *req,
				  int req_size, void *reply, int res_size)
{
	struct virtio_net_stats_reply_hdr *hdr;
	struct scatterlist sgs_in, sgs_out;
	void *p;
	u32 qid;
	int ok;

	sg_init_one(&sgs_out, req, req_size);
	sg_init_one(&sgs_in, reply, res_size);

	ok = virtnet_send_command_reply(vi, VIRTIO_NET_CTRL_STATS,
					VIRTIO_NET_CTRL_STATS_GET,
					&sgs_out, &sgs_in);

	if (!ok)
		return ok;

	for (p = reply; p - reply < res_size; p += le16_to_cpu(hdr->size)) {
		hdr = p;
		qid = le16_to_cpu(hdr->vq_index);
		virtnet_fill_stats(vi, qid, ctx, p, false, hdr->type);
	}

	return 0;
}

static void virtnet_make_stat_req(struct virtnet_info *vi,
				  struct virtnet_stats_ctx *ctx,
				  struct virtio_net_ctrl_queue_stats *req,
				  int qid, int *idx)
{
	int qtype = vq_type(vi, qid);
	u64 bitmap = ctx->bitmap[qtype];

	if (!bitmap)
		return;

	req->stats[*idx].vq_index = cpu_to_le16(qid);
	req->stats[*idx].types_bitmap[0] = cpu_to_le64(bitmap);
	*idx += 1;
}

/* qid: -1: get stats of all vq.
 *     > 0: get the stats for the special vq. This must not be cvq.
 */
static int virtnet_get_hw_stats(struct virtnet_info *vi,
				struct virtnet_stats_ctx *ctx, int qid)
{
	int qnum, i, j, res_size, qtype, last_vq, first_vq;
	struct virtio_net_ctrl_queue_stats *req;
	bool enable_cvq;
	void *reply;
	int ok;

	if (!virtio_has_feature(vi->vdev, VIRTIO_NET_F_DEVICE_STATS))
		return 0;

	if (qid == -1) {
		last_vq = vi->curr_queue_pairs * 2 - 1;
		first_vq = 0;
		enable_cvq = true;
	} else {
		last_vq = qid;
		first_vq = qid;
		enable_cvq = false;
	}

	qnum = 0;
	res_size = 0;
	for (i = first_vq; i <= last_vq ; ++i) {
		qtype = vq_type(vi, i);
		if (ctx->bitmap[qtype]) {
			++qnum;
			res_size += ctx->size[qtype];
		}
	}

	if (enable_cvq && ctx->bitmap[VIRTNET_Q_TYPE_CQ]) {
		res_size += ctx->size[VIRTNET_Q_TYPE_CQ];
		qnum += 1;
	}

	req = kcalloc(qnum, sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	reply = kmalloc(res_size, GFP_KERNEL);
	if (!reply) {
		kfree(req);
		return -ENOMEM;
	}

	j = 0;
	for (i = first_vq; i <= last_vq ; ++i)
		virtnet_make_stat_req(vi, ctx, req, i, &j);

	if (enable_cvq)
		virtnet_make_stat_req(vi, ctx, req, vi->max_queue_pairs * 2, &j);

	ok = __virtnet_get_hw_stats(vi, ctx, req, sizeof(*req) * j, reply, res_size);

	kfree(req);
	kfree(reply);

	return ok;
}

static void virtnet_get_strings(struct net_device *dev, u32 stringset, u8 *data)
{
	struct virtnet_info *vi = netdev_priv(dev);
	unsigned int i;
	u8 *p = data;

	switch (stringset) {
	case ETH_SS_STATS:
		/* Generate the total field names. */
		virtnet_get_stats_string(vi, VIRTNET_Q_TYPE_RX, -1, &p);
		virtnet_get_stats_string(vi, VIRTNET_Q_TYPE_TX, -1, &p);

		virtnet_get_stats_string(vi, VIRTNET_Q_TYPE_CQ, 0, &p);

		for (i = 0; i < vi->curr_queue_pairs; ++i)
			virtnet_get_stats_string(vi, VIRTNET_Q_TYPE_RX, i, &p);

		for (i = 0; i < vi->curr_queue_pairs; ++i)
			virtnet_get_stats_string(vi, VIRTNET_Q_TYPE_TX, i, &p);
		break;
	}
}

static int virtnet_get_sset_count(struct net_device *dev, int sset)
{
	struct virtnet_info *vi = netdev_priv(dev);
	struct virtnet_stats_ctx ctx = {0};
	u32 pair_count;

	switch (sset) {
	case ETH_SS_STATS:
		virtnet_stats_ctx_init(vi, &ctx, NULL, false);

		pair_count = ctx.desc_num[VIRTNET_Q_TYPE_RX] + ctx.desc_num[VIRTNET_Q_TYPE_TX];

		return pair_count + ctx.desc_num[VIRTNET_Q_TYPE_CQ] +
			vi->curr_queue_pairs * pair_count;
	default:
		return -EOPNOTSUPP;
	}
}

static void virtnet_get_ethtool_stats(struct net_device *dev,
				      struct ethtool_stats *stats, u64 *data)
{
	struct virtnet_info *vi = netdev_priv(dev);
	struct virtnet_stats_ctx ctx = {0};
	unsigned int start, i;
	const u8 *stats_base;

	virtnet_stats_ctx_init(vi, &ctx, data, false);
	if (virtnet_get_hw_stats(vi, &ctx, -1))
		dev_warn(&vi->dev->dev, "Failed to get hw stats.\n");

	for (i = 0; i < vi->curr_queue_pairs; i++) {
		struct receive_queue *rq = &vi->rq[i];
		struct send_queue *sq = &vi->sq[i];

		stats_base = (const u8 *)&rq->stats;
		do {
			start = u64_stats_fetch_begin(&rq->stats.syncp);
			virtnet_fill_stats(vi, i * 2, &ctx, stats_base, true, 0);
		} while (u64_stats_fetch_retry(&rq->stats.syncp, start));

		stats_base = (const u8 *)&sq->stats;
		do {
			start = u64_stats_fetch_begin(&sq->stats.syncp);
			virtnet_fill_stats(vi, i * 2 + 1, &ctx, stats_base, true, 0);
		} while (u64_stats_fetch_retry(&sq->stats.syncp, start));
	}

	virtnet_fill_total_fields(vi, &ctx);
}

static void virtnet_get_channels(struct net_device *dev,
				 struct ethtool_channels *channels)
{
	struct virtnet_info *vi = netdev_priv(dev);

	channels->combined_count = vi->curr_queue_pairs;
	channels->max_combined = vi->max_queue_pairs;
	channels->max_other = 0;
	channels->rx_count = 0;
	channels->tx_count = 0;
	channels->other_count = 0;
}

static int virtnet_set_link_ksettings(struct net_device *dev,
				      const struct ethtool_link_ksettings *cmd)
{
	struct virtnet_info *vi = netdev_priv(dev);

	return ethtool_virtdev_set_link_ksettings(dev, cmd,
						  &vi->speed, &vi->duplex);
}

static int virtnet_get_link_ksettings(struct net_device *dev,
				      struct ethtool_link_ksettings *cmd)
{
	struct virtnet_info *vi = netdev_priv(dev);

	cmd->base.speed = vi->speed;
	cmd->base.duplex = vi->duplex;
	cmd->base.port = PORT_OTHER;

	return 0;
}

static int virtnet_send_tx_notf_coal_cmds(struct virtnet_info *vi,
					  struct ethtool_coalesce *ec)
{
	struct virtio_net_ctrl_coal_tx *coal_tx __free(kfree) = NULL;
	struct scatterlist sgs_tx;
	int i;

	coal_tx = kzalloc(sizeof(*coal_tx), GFP_KERNEL);
	if (!coal_tx)
		return -ENOMEM;

	coal_tx->tx_usecs = cpu_to_le32(ec->tx_coalesce_usecs);
	coal_tx->tx_max_packets = cpu_to_le32(ec->tx_max_coalesced_frames);
	sg_init_one(&sgs_tx, coal_tx, sizeof(*coal_tx));

	if (!virtnet_send_command(vi, VIRTIO_NET_CTRL_NOTF_COAL,
				  VIRTIO_NET_CTRL_NOTF_COAL_TX_SET,
				  &sgs_tx))
		return -EINVAL;

	vi->intr_coal_tx.max_usecs = ec->tx_coalesce_usecs;
	vi->intr_coal_tx.max_packets = ec->tx_max_coalesced_frames;
	for (i = 0; i < vi->max_queue_pairs; i++) {
		vi->sq[i].intr_coal.max_usecs = ec->tx_coalesce_usecs;
		vi->sq[i].intr_coal.max_packets = ec->tx_max_coalesced_frames;
	}

	return 0;
}

static int virtnet_send_rx_notf_coal_cmds(struct virtnet_info *vi,
					  struct ethtool_coalesce *ec)
{
	struct virtio_net_ctrl_coal_rx *coal_rx __free(kfree) = NULL;
	bool rx_ctrl_dim_on = !!ec->use_adaptive_rx_coalesce;
	struct scatterlist sgs_rx;
	int i;

	if (rx_ctrl_dim_on && !virtio_has_feature(vi->vdev, VIRTIO_NET_F_VQ_NOTF_COAL))
		return -EOPNOTSUPP;

	if (rx_ctrl_dim_on && (ec->rx_coalesce_usecs != vi->intr_coal_rx.max_usecs ||
			       ec->rx_max_coalesced_frames != vi->intr_coal_rx.max_packets))
		return -EINVAL;

	if (rx_ctrl_dim_on && !vi->rx_dim_enabled) {
		vi->rx_dim_enabled = true;
		for (i = 0; i < vi->max_queue_pairs; i++) {
			mutex_lock(&vi->rq[i].dim_lock);
			vi->rq[i].dim_enabled = true;
			mutex_unlock(&vi->rq[i].dim_lock);
		}
		return 0;
	}

	coal_rx = kzalloc(sizeof(*coal_rx), GFP_KERNEL);
	if (!coal_rx)
		return -ENOMEM;

	if (!rx_ctrl_dim_on && vi->rx_dim_enabled) {
		vi->rx_dim_enabled = false;
		for (i = 0; i < vi->max_queue_pairs; i++) {
			mutex_lock(&vi->rq[i].dim_lock);
			vi->rq[i].dim_enabled = false;
			mutex_unlock(&vi->rq[i].dim_lock);
		}
	}

	/* Since the per-queue coalescing params can be set,
	 * we need apply the global new params even if they
	 * are not updated.
	 */
	coal_rx->rx_usecs = cpu_to_le32(ec->rx_coalesce_usecs);
	coal_rx->rx_max_packets = cpu_to_le32(ec->rx_max_coalesced_frames);
	sg_init_one(&sgs_rx, coal_rx, sizeof(*coal_rx));

	if (!virtnet_send_command(vi, VIRTIO_NET_CTRL_NOTF_COAL,
				  VIRTIO_NET_CTRL_NOTF_COAL_RX_SET,
				  &sgs_rx))
		return -EINVAL;

	vi->intr_coal_rx.max_usecs = ec->rx_coalesce_usecs;
	vi->intr_coal_rx.max_packets = ec->rx_max_coalesced_frames;
	for (i = 0; i < vi->max_queue_pairs; i++) {
		mutex_lock(&vi->rq[i].dim_lock);
		vi->rq[i].intr_coal.max_usecs = ec->rx_coalesce_usecs;
		vi->rq[i].intr_coal.max_packets = ec->rx_max_coalesced_frames;
		mutex_unlock(&vi->rq[i].dim_lock);
	}

	return 0;
}

static int virtnet_send_notf_coal_cmds(struct virtnet_info *vi,
				       struct ethtool_coalesce *ec)
{
	int err;

	err = virtnet_send_tx_notf_coal_cmds(vi, ec);
	if (err)
		return err;

	err = virtnet_send_rx_notf_coal_cmds(vi, ec);
	if (err)
		return err;

	return 0;
}

static int virtnet_send_rx_notf_coal_vq_cmds(struct virtnet_info *vi,
					     struct ethtool_coalesce *ec,
					     u16 queue)
{
	bool rx_ctrl_dim_on = !!ec->use_adaptive_rx_coalesce;
	u32 max_usecs, max_packets;
	bool cur_rx_dim;
	int err;

	mutex_lock(&vi->rq[queue].dim_lock);
	cur_rx_dim = vi->rq[queue].dim_enabled;
	max_usecs = vi->rq[queue].intr_coal.max_usecs;
	max_packets = vi->rq[queue].intr_coal.max_packets;

	if (rx_ctrl_dim_on && (ec->rx_coalesce_usecs != max_usecs ||
			       ec->rx_max_coalesced_frames != max_packets)) {
		mutex_unlock(&vi->rq[queue].dim_lock);
		return -EINVAL;
	}

	if (rx_ctrl_dim_on && !cur_rx_dim) {
		vi->rq[queue].dim_enabled = true;
		mutex_unlock(&vi->rq[queue].dim_lock);
		return 0;
	}

	if (!rx_ctrl_dim_on && cur_rx_dim)
		vi->rq[queue].dim_enabled = false;

	/* If no params are updated, userspace ethtool will
	 * reject the modification.
	 */
	err = virtnet_send_rx_ctrl_coal_vq_cmd(vi, queue,
					       ec->rx_coalesce_usecs,
					       ec->rx_max_coalesced_frames);
	mutex_unlock(&vi->rq[queue].dim_lock);
	return err;
}

static int virtnet_send_notf_coal_vq_cmds(struct virtnet_info *vi,
					  struct ethtool_coalesce *ec,
					  u16 queue)
{
	int err;

	err = virtnet_send_rx_notf_coal_vq_cmds(vi, ec, queue);
	if (err)
		return err;

	err = virtnet_send_tx_ctrl_coal_vq_cmd(vi, queue,
					       ec->tx_coalesce_usecs,
					       ec->tx_max_coalesced_frames);
	if (err)
		return err;

	return 0;
}

static void virtnet_rx_dim_work(struct work_struct *work)
{
	struct dim *dim = container_of(work, struct dim, work);
	struct receive_queue *rq = container_of(dim,
			struct receive_queue, dim);
	struct virtnet_info *vi = rq->vq->vdev->priv;
	struct net_device *dev = vi->dev;
	struct dim_cq_moder update_moder;
	int qnum, err;

	qnum = rq - vi->rq;

	mutex_lock(&rq->dim_lock);
	if (!rq->dim_enabled)
		goto out;

	update_moder = net_dim_get_rx_irq_moder(dev, dim);
	if (update_moder.usec != rq->intr_coal.max_usecs ||
	    update_moder.pkts != rq->intr_coal.max_packets) {
		err = virtnet_send_rx_ctrl_coal_vq_cmd(vi, qnum,
						       update_moder.usec,
						       update_moder.pkts);
		if (err)
			pr_debug("%s: Failed to send dim parameters on rxq%d\n",
				 dev->name, qnum);
	}
out:
	dim->state = DIM_START_MEASURE;
	mutex_unlock(&rq->dim_lock);
}

static int virtnet_coal_params_supported(struct ethtool_coalesce *ec)
{
	/* usecs coalescing is supported only if VIRTIO_NET_F_NOTF_COAL
	 * or VIRTIO_NET_F_VQ_NOTF_COAL feature is negotiated.
	 */
	if (ec->rx_coalesce_usecs || ec->tx_coalesce_usecs)
		return -EOPNOTSUPP;

	if (ec->tx_max_coalesced_frames > 1 ||
	    ec->rx_max_coalesced_frames != 1)
		return -EINVAL;

	return 0;
}

static int virtnet_should_update_vq_weight(int dev_flags, int weight,
					   int vq_weight, bool *should_update)
{
	if (weight ^ vq_weight) {
		if (dev_flags & IFF_UP)
			return -EBUSY;
		*should_update = true;
	}

	return 0;
}

static int virtnet_set_coalesce(struct net_device *dev,
				struct ethtool_coalesce *ec,
				struct kernel_ethtool_coalesce *kernel_coal,
				struct netlink_ext_ack *extack)
{
	struct virtnet_info *vi = netdev_priv(dev);
	int ret, queue_number, napi_weight, i;
	bool update_napi = false;

	/* Can't change NAPI weight if the link is up */
	napi_weight = ec->tx_max_coalesced_frames ? NAPI_POLL_WEIGHT : 0;
	for (queue_number = 0; queue_number < vi->max_queue_pairs; queue_number++) {
		ret = virtnet_should_update_vq_weight(dev->flags, napi_weight,
						      vi->sq[queue_number].napi.weight,
						      &update_napi);
		if (ret)
			return ret;

		if (update_napi) {
			/* All queues that belong to [queue_number, vi->max_queue_pairs] will be
			 * updated for the sake of simplicity, which might not be necessary
			 */
			break;
		}
	}

	if (virtio_has_feature(vi->vdev, VIRTIO_NET_F_NOTF_COAL))
		ret = virtnet_send_notf_coal_cmds(vi, ec);
	else
		ret = virtnet_coal_params_supported(ec);

	if (ret)
		return ret;

	if (update_napi) {
		/* xsk xmit depends on the tx napi. So if xsk is active,
		 * prevent modifications to tx napi.
		 */
		for (i = queue_number; i < vi->max_queue_pairs; i++) {
			if (vi->sq[i].xsk_pool)
				return -EBUSY;
		}

		for (; queue_number < vi->max_queue_pairs; queue_number++)
			vi->sq[queue_number].napi.weight = napi_weight;
	}

	return ret;
}

static int virtnet_get_coalesce(struct net_device *dev,
				struct ethtool_coalesce *ec,
				struct kernel_ethtool_coalesce *kernel_coal,
				struct netlink_ext_ack *extack)
{
	struct virtnet_info *vi = netdev_priv(dev);

	if (virtio_has_feature(vi->vdev, VIRTIO_NET_F_NOTF_COAL)) {
		ec->rx_coalesce_usecs = vi->intr_coal_rx.max_usecs;
		ec->tx_coalesce_usecs = vi->intr_coal_tx.max_usecs;
		ec->tx_max_coalesced_frames = vi->intr_coal_tx.max_packets;
		ec->rx_max_coalesced_frames = vi->intr_coal_rx.max_packets;
		ec->use_adaptive_rx_coalesce = vi->rx_dim_enabled;
	} else {
		ec->rx_max_coalesced_frames = 1;

		if (vi->sq[0].napi.weight)
			ec->tx_max_coalesced_frames = 1;
	}

	return 0;
}

static int virtnet_set_per_queue_coalesce(struct net_device *dev,
					  u32 queue,
					  struct ethtool_coalesce *ec)
{
	struct virtnet_info *vi = netdev_priv(dev);
	int ret, napi_weight;
	bool update_napi = false;

	if (queue >= vi->max_queue_pairs)
		return -EINVAL;

	/* Can't change NAPI weight if the link is up */
	napi_weight = ec->tx_max_coalesced_frames ? NAPI_POLL_WEIGHT : 0;
	ret = virtnet_should_update_vq_weight(dev->flags, napi_weight,
					      vi->sq[queue].napi.weight,
					      &update_napi);
	if (ret)
		return ret;

	if (virtio_has_feature(vi->vdev, VIRTIO_NET_F_VQ_NOTF_COAL))
		ret = virtnet_send_notf_coal_vq_cmds(vi, ec, queue);
	else
		ret = virtnet_coal_params_supported(ec);

	if (ret)
		return ret;

	if (update_napi)
		vi->sq[queue].napi.weight = napi_weight;

	return 0;
}

static int virtnet_get_per_queue_coalesce(struct net_device *dev,
					  u32 queue,
					  struct ethtool_coalesce *ec)
{
	struct virtnet_info *vi = netdev_priv(dev);

	if (queue >= vi->max_queue_pairs)
		return -EINVAL;

	if (virtio_has_feature(vi->vdev, VIRTIO_NET_F_VQ_NOTF_COAL)) {
		mutex_lock(&vi->rq[queue].dim_lock);
		ec->rx_coalesce_usecs = vi->rq[queue].intr_coal.max_usecs;
		ec->tx_coalesce_usecs = vi->sq[queue].intr_coal.max_usecs;
		ec->tx_max_coalesced_frames = vi->sq[queue].intr_coal.max_packets;
		ec->rx_max_coalesced_frames = vi->rq[queue].intr_coal.max_packets;
		ec->use_adaptive_rx_coalesce = vi->rq[queue].dim_enabled;
		mutex_unlock(&vi->rq[queue].dim_lock);
	} else {
		ec->rx_max_coalesced_frames = 1;

		if (vi->sq[queue].napi.weight)
			ec->tx_max_coalesced_frames = 1;
	}

	return 0;
}

static void virtnet_init_settings(struct net_device *dev)
{
	struct virtnet_info *vi = netdev_priv(dev);

	vi->speed = SPEED_UNKNOWN;
	vi->duplex = DUPLEX_UNKNOWN;
}

static u32 virtnet_get_rxfh_key_size(struct net_device *dev)
{
	return ((struct virtnet_info *)netdev_priv(dev))->rss_key_size;
}

static u32 virtnet_get_rxfh_indir_size(struct net_device *dev)
{
	return ((struct virtnet_info *)netdev_priv(dev))->rss_indir_table_size;
}

static int virtnet_get_rxfh(struct net_device *dev,
			    struct ethtool_rxfh_param *rxfh)
{
	struct virtnet_info *vi = netdev_priv(dev);
	int i;

	if (rxfh->indir) {
		for (i = 0; i < vi->rss_indir_table_size; ++i)
			rxfh->indir[i] = le16_to_cpu(vi->rss_hdr->indirection_table[i]);
	}

	if (rxfh->key)
		memcpy(rxfh->key, vi->rss_hash_key_data, vi->rss_key_size);

	rxfh->hfunc = ETH_RSS_HASH_TOP;

	return 0;
}

static int virtnet_set_rxfh(struct net_device *dev,
			    struct ethtool_rxfh_param *rxfh,
			    struct netlink_ext_ack *extack)
{
	struct virtnet_info *vi = netdev_priv(dev);
	bool update = false;
	int i;

	if (rxfh->hfunc != ETH_RSS_HASH_NO_CHANGE &&
	    rxfh->hfunc != ETH_RSS_HASH_TOP)
		return -EOPNOTSUPP;

	if (rxfh->indir) {
		if (!vi->has_rss)
			return -EOPNOTSUPP;

		for (i = 0; i < vi->rss_indir_table_size; ++i)
			vi->rss_hdr->indirection_table[i] = cpu_to_le16(rxfh->indir[i]);
		update = true;
	}

	if (rxfh->key) {
		/* If either _F_HASH_REPORT or _F_RSS are negotiated, the
		 * device provides hash calculation capabilities, that is,
		 * hash_key is configured.
		 */
		if (!vi->has_rss && !vi->has_rss_hash_report)
			return -EOPNOTSUPP;

		memcpy(vi->rss_hash_key_data, rxfh->key, vi->rss_key_size);
		update = true;
	}

	if (update)
		virtnet_commit_rss_command(vi);

	return 0;
}

static int virtnet_get_rxnfc(struct net_device *dev, struct ethtool_rxnfc *info, u32 *rule_locs)
{
	struct virtnet_info *vi = netdev_priv(dev);
	int rc = 0;

	switch (info->cmd) {
	case ETHTOOL_GRXRINGS:
		info->data = vi->curr_queue_pairs;
		break;
	case ETHTOOL_GRXFH:
		virtnet_get_hashflow(vi, info);
		break;
	default:
		rc = -EOPNOTSUPP;
	}

	return rc;
}

static int virtnet_set_rxnfc(struct net_device *dev, struct ethtool_rxnfc *info)
{
	struct virtnet_info *vi = netdev_priv(dev);
	int rc = 0;

	switch (info->cmd) {
	case ETHTOOL_SRXFH:
		if (!virtnet_set_hashflow(vi, info))
			rc = -EINVAL;

		break;
	default:
		rc = -EOPNOTSUPP;
	}

	return rc;
}

static const struct ethtool_ops virtnet_ethtool_ops = {
	.supported_coalesce_params = ETHTOOL_COALESCE_MAX_FRAMES |
		ETHTOOL_COALESCE_USECS | ETHTOOL_COALESCE_USE_ADAPTIVE_RX,
	.get_drvinfo = virtnet_get_drvinfo,
	.get_link = ethtool_op_get_link,
	.get_ringparam = virtnet_get_ringparam,
	.set_ringparam = virtnet_set_ringparam,
	.get_strings = virtnet_get_strings,
	.get_sset_count = virtnet_get_sset_count,
	.get_ethtool_stats = virtnet_get_ethtool_stats,
	.set_channels = virtnet_set_channels,
	.get_channels = virtnet_get_channels,
	.get_ts_info = ethtool_op_get_ts_info,
	.get_link_ksettings = virtnet_get_link_ksettings,
	.set_link_ksettings = virtnet_set_link_ksettings,
	.set_coalesce = virtnet_set_coalesce,
	.get_coalesce = virtnet_get_coalesce,
	.set_per_queue_coalesce = virtnet_set_per_queue_coalesce,
	.get_per_queue_coalesce = virtnet_get_per_queue_coalesce,
	.get_rxfh_key_size = virtnet_get_rxfh_key_size,
	.get_rxfh_indir_size = virtnet_get_rxfh_indir_size,
	.get_rxfh = virtnet_get_rxfh,
	.set_rxfh = virtnet_set_rxfh,
	.get_rxnfc = virtnet_get_rxnfc,
	.set_rxnfc = virtnet_set_rxnfc,
};

static void virtnet_get_queue_stats_rx(struct net_device *dev, int i,
				       struct netdev_queue_stats_rx *stats)
{
	struct virtnet_info *vi = netdev_priv(dev);
	struct receive_queue *rq = &vi->rq[i];
	struct virtnet_stats_ctx ctx = {0};

	virtnet_stats_ctx_init(vi, &ctx, (void *)stats, true);

	virtnet_get_hw_stats(vi, &ctx, i * 2);
	virtnet_fill_stats(vi, i * 2, &ctx, (void *)&rq->stats, true, 0);
}

static void virtnet_get_queue_stats_tx(struct net_device *dev, int i,
				       struct netdev_queue_stats_tx *stats)
{
	struct virtnet_info *vi = netdev_priv(dev);
	struct send_queue *sq = &vi->sq[i];
	struct virtnet_stats_ctx ctx = {0};

	virtnet_stats_ctx_init(vi, &ctx, (void *)stats, true);

	virtnet_get_hw_stats(vi, &ctx, i * 2 + 1);
	virtnet_fill_stats(vi, i * 2 + 1, &ctx, (void *)&sq->stats, true, 0);
}

static void virtnet_get_base_stats(struct net_device *dev,
				   struct netdev_queue_stats_rx *rx,
				   struct netdev_queue_stats_tx *tx)
{
	struct virtnet_info *vi = netdev_priv(dev);

	/* The queue stats of the virtio-net will not be reset. So here we
	 * return 0.
	 */
	rx->bytes = 0;
	rx->packets = 0;

	if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_RX_BASIC) {
		rx->hw_drops = 0;
		rx->hw_drop_overruns = 0;
	}

	if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_RX_CSUM) {
		rx->csum_unnecessary = 0;
		rx->csum_none = 0;
		rx->csum_bad = 0;
	}

	if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_RX_GSO) {
		rx->hw_gro_packets = 0;
		rx->hw_gro_bytes = 0;
		rx->hw_gro_wire_packets = 0;
		rx->hw_gro_wire_bytes = 0;
	}

	if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_RX_SPEED)
		rx->hw_drop_ratelimits = 0;

	tx->bytes = 0;
	tx->packets = 0;
	tx->stop = 0;
	tx->wake = 0;

	if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_TX_BASIC) {
		tx->hw_drops = 0;
		tx->hw_drop_errors = 0;
	}

	if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_TX_CSUM) {
		tx->csum_none = 0;
		tx->needs_csum = 0;
	}

	if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_TX_GSO) {
		tx->hw_gso_packets = 0;
		tx->hw_gso_bytes = 0;
		tx->hw_gso_wire_packets = 0;
		tx->hw_gso_wire_bytes = 0;
	}

	if (vi->device_stats_cap & VIRTIO_NET_STATS_TYPE_TX_SPEED)
		tx->hw_drop_ratelimits = 0;

	netdev_stat_queue_sum(dev,
			      dev->real_num_rx_queues, vi->max_queue_pairs, rx,
			      dev->real_num_tx_queues, vi->max_queue_pairs, tx);
}

static const struct netdev_stat_ops virtnet_stat_ops = {
	.get_queue_stats_rx	= virtnet_get_queue_stats_rx,
	.get_queue_stats_tx	= virtnet_get_queue_stats_tx,
	.get_base_stats		= virtnet_get_base_stats,
};

static void virtnet_freeze_down(struct virtio_device *vdev)
{
	struct virtnet_info *vi = vdev->priv;

	/* Make sure no work handler is accessing the device */
	flush_work(&vi->config_work);
	disable_rx_mode_work(vi);
	flush_work(&vi->rx_mode_work);

	netif_tx_lock_bh(vi->dev);
	netif_device_detach(vi->dev);
	netif_tx_unlock_bh(vi->dev);
	if (netif_running(vi->dev)) {
		rtnl_lock();
		virtnet_close(vi->dev);
		rtnl_unlock();
	}
}

static int init_vqs(struct virtnet_info *vi);

static int virtnet_restore_up(struct virtio_device *vdev)
{
	struct virtnet_info *vi = vdev->priv;
	int err;

	err = init_vqs(vi);
	if (err)
		return err;

	virtio_device_ready(vdev);

	enable_delayed_refill(vi);
	enable_rx_mode_work(vi);

	if (netif_running(vi->dev)) {
		rtnl_lock();
		err = virtnet_open(vi->dev);
		rtnl_unlock();
		if (err)
			return err;
	}

	netif_tx_lock_bh(vi->dev);
	netif_device_attach(vi->dev);
	netif_tx_unlock_bh(vi->dev);
	return err;
}

static int virtnet_set_guest_offloads(struct virtnet_info *vi, u64 offloads)
{
	__virtio64 *_offloads __free(kfree) = NULL;
	struct scatterlist sg;

	_offloads = kzalloc(sizeof(*_offloads), GFP_KERNEL);
	if (!_offloads)
		return -ENOMEM;

	*_offloads = cpu_to_virtio64(vi->vdev, offloads);

	sg_init_one(&sg, _offloads, sizeof(*_offloads));

	if (!virtnet_send_command(vi, VIRTIO_NET_CTRL_GUEST_OFFLOADS,
				  VIRTIO_NET_CTRL_GUEST_OFFLOADS_SET, &sg)) {
		dev_warn(&vi->dev->dev, "Fail to set guest offload.\n");
		return -EINVAL;
	}

	return 0;
}

static int virtnet_clear_guest_offloads(struct virtnet_info *vi)
{
	u64 offloads = 0;

	if (!vi->guest_offloads)
		return 0;

	return virtnet_set_guest_offloads(vi, offloads);
}

static int virtnet_restore_guest_offloads(struct virtnet_info *vi)
{
	u64 offloads = vi->guest_offloads;

	if (!vi->guest_offloads)
		return 0;

	return virtnet_set_guest_offloads(vi, offloads);
}

static int virtnet_rq_bind_xsk_pool(struct virtnet_info *vi, struct receive_queue *rq,
				    struct xsk_buff_pool *pool)
{
	int err, qindex;

	qindex = rq - vi->rq;

	if (pool) {
		err = xdp_rxq_info_reg(&rq->xsk_rxq_info, vi->dev, qindex, rq->napi.napi_id);
		if (err < 0)
			return err;

		err = xdp_rxq_info_reg_mem_model(&rq->xsk_rxq_info,
						 MEM_TYPE_XSK_BUFF_POOL, NULL);
		if (err < 0)
			goto unreg;

		xsk_pool_set_rxq_info(pool, &rq->xsk_rxq_info);
	}

	virtnet_rx_pause(vi, rq);

	err = virtqueue_reset(rq->vq, virtnet_rq_unmap_free_buf, NULL);
	if (err) {
		netdev_err(vi->dev, "reset rx fail: rx queue index: %d err: %d\n", qindex, err);

		pool = NULL;
	}

	rq->xsk_pool = pool;

	virtnet_rx_resume(vi, rq);

	if (pool)
		return 0;

unreg:
	xdp_rxq_info_unreg(&rq->xsk_rxq_info);
	return err;
}

static int virtnet_sq_bind_xsk_pool(struct virtnet_info *vi,
				    struct send_queue *sq,
				    struct xsk_buff_pool *pool)
{
	int err, qindex;

	qindex = sq - vi->sq;

	virtnet_tx_pause(vi, sq);

	err = virtqueue_reset(sq->vq, virtnet_sq_free_unused_buf,
			      virtnet_sq_free_unused_buf_done);
	if (err) {
		netdev_err(vi->dev, "reset tx fail: tx queue index: %d err: %d\n", qindex, err);
		pool = NULL;
	}

	sq->xsk_pool = pool;

	virtnet_tx_resume(vi, sq);

	return err;
}

static int virtnet_xsk_pool_enable(struct net_device *dev,
				   struct xsk_buff_pool *pool,
				   u16 qid)
{
	struct virtnet_info *vi = netdev_priv(dev);
	struct receive_queue *rq;
	struct device *dma_dev;
	struct send_queue *sq;
	dma_addr_t hdr_dma;
	int err, size;

	if (vi->hdr_len > xsk_pool_get_headroom(pool))
		return -EINVAL;

	/* In big_packets mode, xdp cannot work, so there is no need to
	 * initialize xsk of rq.
	 */
	if (vi->big_packets && !vi->mergeable_rx_bufs)
		return -ENOENT;

	if (qid >= vi->curr_queue_pairs)
		return -EINVAL;

	sq = &vi->sq[qid];
	rq = &vi->rq[qid];

	/* xsk assumes that tx and rx must have the same dma device. The af-xdp
	 * may use one buffer to receive from the rx and reuse this buffer to
	 * send by the tx. So the dma dev of sq and rq must be the same one.
	 *
	 * But vq->dma_dev allows every vq has the respective dma dev. So I
	 * check the dma dev of vq and sq is the same dev.
	 */
	if (virtqueue_dma_dev(rq->vq) != virtqueue_dma_dev(sq->vq))
		return -EINVAL;

	dma_dev = virtqueue_dma_dev(rq->vq);
	if (!dma_dev)
		return -EINVAL;

	size = virtqueue_get_vring_size(rq->vq);

	rq->xsk_buffs = kvcalloc(size, sizeof(*rq->xsk_buffs), GFP_KERNEL);
	if (!rq->xsk_buffs)
		return -ENOMEM;

	hdr_dma = virtqueue_dma_map_single_attrs(sq->vq, &xsk_hdr, vi->hdr_len,
						 DMA_TO_DEVICE, 0);
	if (virtqueue_dma_mapping_error(sq->vq, hdr_dma)) {
		err = -ENOMEM;
		goto err_free_buffs;
	}

	err = xsk_pool_dma_map(pool, dma_dev, 0);
	if (err)
		goto err_xsk_map;

	err = virtnet_rq_bind_xsk_pool(vi, rq, pool);
	if (err)
		goto err_rq;

	err = virtnet_sq_bind_xsk_pool(vi, sq, pool);
	if (err)
		goto err_sq;

	/* Now, we do not support tx offload(such as tx csum), so all the tx
	 * virtnet hdr is zero. So all the tx packets can share a single hdr.
	 */
	sq->xsk_hdr_dma_addr = hdr_dma;

	return 0;

err_sq:
	virtnet_rq_bind_xsk_pool(vi, rq, NULL);
err_rq:
	xsk_pool_dma_unmap(pool, 0);
err_xsk_map:
	virtqueue_dma_unmap_single_attrs(rq->vq, hdr_dma, vi->hdr_len,
					 DMA_TO_DEVICE, 0);
err_free_buffs:
	kvfree(rq->xsk_buffs);
	return err;
}

static int virtnet_xsk_pool_disable(struct net_device *dev, u16 qid)
{
	struct virtnet_info *vi = netdev_priv(dev);
	struct xsk_buff_pool *pool;
	struct receive_queue *rq;
	struct send_queue *sq;
	int err;

	if (qid >= vi->curr_queue_pairs)
		return -EINVAL;

	sq = &vi->sq[qid];
	rq = &vi->rq[qid];

	pool = rq->xsk_pool;

	err = virtnet_rq_bind_xsk_pool(vi, rq, NULL);
	err |= virtnet_sq_bind_xsk_pool(vi, sq, NULL);

	xsk_pool_dma_unmap(pool, 0);

	virtqueue_dma_unmap_single_attrs(sq->vq, sq->xsk_hdr_dma_addr,
					 vi->hdr_len, DMA_TO_DEVICE, 0);
	kvfree(rq->xsk_buffs);

	return err;
}

static int virtnet_xsk_pool_setup(struct net_device *dev, struct netdev_bpf *xdp)
{
	if (xdp->xsk.pool)
		return virtnet_xsk_pool_enable(dev, xdp->xsk.pool,
					       xdp->xsk.queue_id);
	else
		return virtnet_xsk_pool_disable(dev, xdp->xsk.queue_id);
}

static int virtnet_xdp_set(struct net_device *dev, struct bpf_prog *prog,
			   struct netlink_ext_ack *extack)
{
	unsigned int room = SKB_DATA_ALIGN(XDP_PACKET_HEADROOM +
					   sizeof(struct skb_shared_info));
	unsigned int max_sz = PAGE_SIZE - room - ETH_HLEN;
	struct virtnet_info *vi = netdev_priv(dev);
	struct bpf_prog *old_prog;
	u16 xdp_qp = 0, curr_qp;
	int i, err;

	if (!virtio_has_feature(vi->vdev, VIRTIO_NET_F_CTRL_GUEST_OFFLOADS)
	    && (virtio_has_feature(vi->vdev, VIRTIO_NET_F_GUEST_TSO4) ||
	        virtio_has_feature(vi->vdev, VIRTIO_NET_F_GUEST_TSO6) ||
	        virtio_has_feature(vi->vdev, VIRTIO_NET_F_GUEST_ECN) ||
		virtio_has_feature(vi->vdev, VIRTIO_NET_F_GUEST_UFO) ||
		virtio_has_feature(vi->vdev, VIRTIO_NET_F_GUEST_CSUM) ||
		virtio_has_feature(vi->vdev, VIRTIO_NET_F_GUEST_USO4) ||
		virtio_has_feature(vi->vdev, VIRTIO_NET_F_GUEST_USO6))) {
		NL_SET_ERR_MSG_MOD(extack, "Can't set XDP while host is implementing GRO_HW/CSUM, disable GRO_HW/CSUM first");
		return -EOPNOTSUPP;
	}

	if (vi->mergeable_rx_bufs && !vi->any_header_sg) {
		NL_SET_ERR_MSG_MOD(extack, "XDP expects header/data in single page, any_header_sg required");
		return -EINVAL;
	}

	if (prog && !prog->aux->xdp_has_frags && dev->mtu > max_sz) {
		NL_SET_ERR_MSG_MOD(extack, "MTU too large to enable XDP without frags");
		netdev_warn(dev, "single-buffer XDP requires MTU less than %u\n", max_sz);
		return -EINVAL;
	}

	curr_qp = vi->curr_queue_pairs - vi->xdp_queue_pairs;
	if (prog)
		xdp_qp = nr_cpu_ids;

	/* XDP requires extra queues for XDP_TX */
	if (curr_qp + xdp_qp > vi->max_queue_pairs) {
		netdev_warn_once(dev, "XDP request %i queues but max is %i. XDP_TX and XDP_REDIRECT will operate in a slower locked tx mode.\n",
				 curr_qp + xdp_qp, vi->max_queue_pairs);
		xdp_qp = 0;
	}

	old_prog = rtnl_dereference(vi->rq[0].xdp_prog);
	if (!prog && !old_prog)
		return 0;

	if (prog)
		bpf_prog_add(prog, vi->max_queue_pairs - 1);

	virtnet_rx_pause_all(vi);

	/* Make sure NAPI is not using any XDP TX queues for RX. */
	if (netif_running(dev)) {
		for (i = 0; i < vi->max_queue_pairs; i++)
			virtnet_napi_tx_disable(&vi->sq[i]);
	}

	if (!prog) {
		for (i = 0; i < vi->max_queue_pairs; i++) {
			rcu_assign_pointer(vi->rq[i].xdp_prog, prog);
			if (i == 0)
				virtnet_restore_guest_offloads(vi);
		}
		synchronize_net();
	}

	err = virtnet_set_queues(vi, curr_qp + xdp_qp);
	if (err)
		goto err;
	netif_set_real_num_rx_queues(dev, curr_qp + xdp_qp);
	vi->xdp_queue_pairs = xdp_qp;

	if (prog) {
		vi->xdp_enabled = true;
		for (i = 0; i < vi->max_queue_pairs; i++) {
			rcu_assign_pointer(vi->rq[i].xdp_prog, prog);
			if (i == 0 && !old_prog)
				virtnet_clear_guest_offloads(vi);
		}
		if (!old_prog)
			xdp_features_set_redirect_target(dev, true);
	} else {
		xdp_features_clear_redirect_target(dev);
		vi->xdp_enabled = false;
	}

	virtnet_rx_resume_all(vi);
	for (i = 0; i < vi->max_queue_pairs; i++) {
		if (old_prog)
			bpf_prog_put(old_prog);
		if (netif_running(dev))
			virtnet_napi_tx_enable(&vi->sq[i]);
	}

	return 0;

err:
	if (!prog) {
		virtnet_clear_guest_offloads(vi);
		for (i = 0; i < vi->max_queue_pairs; i++)
			rcu_assign_pointer(vi->rq[i].xdp_prog, old_prog);
	}

	virtnet_rx_resume_all(vi);
	if (netif_running(dev)) {
		for (i = 0; i < vi->max_queue_pairs; i++)
			virtnet_napi_tx_enable(&vi->sq[i]);
	}
	if (prog)
		bpf_prog_sub(prog, vi->max_queue_pairs - 1);
	return err;
}

static int virtnet_xdp(struct net_device *dev, struct netdev_bpf *xdp)
{
	switch (xdp->command) {
	case XDP_SETUP_PROG:
		return virtnet_xdp_set(dev, xdp->prog, xdp->extack);
	case XDP_SETUP_XSK_POOL:
		return virtnet_xsk_pool_setup(dev, xdp);
	default:
		return -EINVAL;
	}
}

static int virtnet_get_phys_port_name(struct net_device *dev, char *buf,
				      size_t len)
{
	struct virtnet_info *vi = netdev_priv(dev);
	int ret;

	if (!virtio_has_feature(vi->vdev, VIRTIO_NET_F_STANDBY))
		return -EOPNOTSUPP;

	ret = snprintf(buf, len, "sby");
	if (ret >= len)
		return -EOPNOTSUPP;

	return 0;
}

static int virtnet_set_features(struct net_device *dev,
				netdev_features_t features)
{
	struct virtnet_info *vi = netdev_priv(dev);
	u64 offloads;
	int err;

	if ((dev->features ^ features) & NETIF_F_GRO_HW) {
		if (vi->xdp_enabled)
			return -EBUSY;

		if (features & NETIF_F_GRO_HW)
			offloads = vi->guest_offloads_capable;
		else
			offloads = vi->guest_offloads_capable &
				   ~GUEST_OFFLOAD_GRO_HW_MASK;

		err = virtnet_set_guest_offloads(vi, offloads);
		if (err)
			return err;
		vi->guest_offloads = offloads;
	}

	if ((dev->features ^ features) & NETIF_F_RXHASH) {
		if (features & NETIF_F_RXHASH)
			vi->rss_hdr->hash_types = cpu_to_le32(vi->rss_hash_types_saved);
		else
			vi->rss_hdr->hash_types = cpu_to_le32(VIRTIO_NET_HASH_REPORT_NONE);

		if (!virtnet_commit_rss_command(vi))
			return -EINVAL;
	}

	return 0;
}

static void virtnet_tx_timeout(struct net_device *dev, unsigned int txqueue)
{
	struct virtnet_info *priv = netdev_priv(dev);
	struct send_queue *sq = &priv->sq[txqueue];
	struct netdev_queue *txq = netdev_get_tx_queue(dev, txqueue);

	u64_stats_update_begin(&sq->stats.syncp);
	u64_stats_inc(&sq->stats.tx_timeouts);
	u64_stats_update_end(&sq->stats.syncp);

	netdev_err(dev, "TX timeout on queue: %u, sq: %s, vq: 0x%x, name: %s, %u usecs ago\n",
		   txqueue, sq->name, sq->vq->index, sq->vq->name,
		   jiffies_to_usecs(jiffies - READ_ONCE(txq->trans_start)));
}

static int virtnet_init_irq_moder(struct virtnet_info *vi)
{
	u8 profile_flags = 0, coal_flags = 0;
	int ret, i;

	profile_flags |= DIM_PROFILE_RX;
	coal_flags |= DIM_COALESCE_USEC | DIM_COALESCE_PKTS;
	ret = net_dim_init_irq_moder(vi->dev, profile_flags, coal_flags,
				     DIM_CQ_PERIOD_MODE_START_FROM_EQE,
				     0, virtnet_rx_dim_work, NULL);

	if (ret)
		return ret;

	for (i = 0; i < vi->max_queue_pairs; i++)
		net_dim_setting(vi->dev, &vi->rq[i].dim, false);

	return 0;
}

static void virtnet_free_irq_moder(struct virtnet_info *vi)
{
	if (!virtio_has_feature(vi->vdev, VIRTIO_NET_F_VQ_NOTF_COAL))
		return;

	rtnl_lock();
	net_dim_free_irq_moder(vi->dev);
	rtnl_unlock();
}

static const struct net_device_ops virtnet_netdev = {
	.ndo_open            = virtnet_open,
	.ndo_stop   	     = virtnet_close,
	.ndo_start_xmit      = start_xmit,
	.ndo_validate_addr   = eth_validate_addr,
	.ndo_set_mac_address = virtnet_set_mac_address,
	.ndo_set_rx_mode     = virtnet_set_rx_mode,
	.ndo_get_stats64     = virtnet_stats,
	.ndo_vlan_rx_add_vid = virtnet_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid = virtnet_vlan_rx_kill_vid,
	.ndo_bpf		= virtnet_xdp,
	.ndo_xdp_xmit		= virtnet_xdp_xmit,
	.ndo_xsk_wakeup         = virtnet_xsk_wakeup,
	.ndo_features_check	= passthru_features_check,
	.ndo_get_phys_port_name	= virtnet_get_phys_port_name,
	.ndo_set_features	= virtnet_set_features,
	.ndo_tx_timeout		= virtnet_tx_timeout,
};

static void virtnet_config_changed_work(struct work_struct *work)
{
	struct virtnet_info *vi =
		container_of(work, struct virtnet_info, config_work);
	u16 v;

	if (virtio_cread_feature(vi->vdev, VIRTIO_NET_F_STATUS,
				 struct virtio_net_config, status, &v) < 0)
		return;

	if (v & VIRTIO_NET_S_ANNOUNCE) {
		netdev_notify_peers(vi->dev);
		virtnet_ack_link_announce(vi);
	}

	/* Ignore unknown (future) status bits */
	v &= VIRTIO_NET_S_LINK_UP;

	if (vi->status == v)
		return;

	vi->status = v;

	if (vi->status & VIRTIO_NET_S_LINK_UP) {
		virtnet_update_settings(vi);
		netif_carrier_on(vi->dev);
		netif_tx_wake_all_queues(vi->dev);
	} else {
		netif_carrier_off(vi->dev);
		netif_tx_stop_all_queues(vi->dev);
	}
}

static void virtnet_config_changed(struct virtio_device *vdev)
{
	struct virtnet_info *vi = vdev->priv;

	schedule_work(&vi->config_work);
}

static void virtnet_free_queues(struct virtnet_info *vi)
{
	int i;

	for (i = 0; i < vi->max_queue_pairs; i++) {
		__netif_napi_del(&vi->rq[i].napi);
		__netif_napi_del(&vi->sq[i].napi);
	}

	/* We called __netif_napi_del(),
	 * we need to respect an RCU grace period before freeing vi->rq
	 */
	synchronize_net();

	kfree(vi->rq);
	kfree(vi->sq);
	kfree(vi->ctrl);
}

static void _free_receive_bufs(struct virtnet_info *vi)
{
	struct bpf_prog *old_prog;
	int i;

	for (i = 0; i < vi->max_queue_pairs; i++) {
		while (vi->rq[i].pages)
			__free_pages(get_a_page(&vi->rq[i], GFP_KERNEL), 0);

		old_prog = rtnl_dereference(vi->rq[i].xdp_prog);
		RCU_INIT_POINTER(vi->rq[i].xdp_prog, NULL);
		if (old_prog)
			bpf_prog_put(old_prog);
	}
}

static void free_receive_bufs(struct virtnet_info *vi)
{
	rtnl_lock();
	_free_receive_bufs(vi);
	rtnl_unlock();
}

static void free_receive_page_frags(struct virtnet_info *vi)
{
	int i;
	for (i = 0; i < vi->max_queue_pairs; i++)
		if (vi->rq[i].alloc_frag.page) {
			if (vi->rq[i].last_dma)
				virtnet_rq_unmap(&vi->rq[i], vi->rq[i].last_dma, 0);
			put_page(vi->rq[i].alloc_frag.page);
		}
}

static void virtnet_sq_free_unused_buf(struct virtqueue *vq, void *buf)
{
	struct virtnet_info *vi = vq->vdev->priv;
	struct send_queue *sq;
	int i = vq2txq(vq);

	sq = &vi->sq[i];

	switch (virtnet_xmit_ptr_unpack(&buf)) {
	case VIRTNET_XMIT_TYPE_SKB:
	case VIRTNET_XMIT_TYPE_SKB_ORPHAN:
		dev_kfree_skb(buf);
		break;

	case VIRTNET_XMIT_TYPE_XDP:
		xdp_return_frame(buf);
		break;

	case VIRTNET_XMIT_TYPE_XSK:
		xsk_tx_completed(sq->xsk_pool, 1);
		break;
	}
}

static void virtnet_sq_free_unused_buf_done(struct virtqueue *vq)
{
	struct virtnet_info *vi = vq->vdev->priv;
	int i = vq2txq(vq);

	netdev_tx_reset_queue(netdev_get_tx_queue(vi->dev, i));
}

static void free_unused_bufs(struct virtnet_info *vi)
{
	void *buf;
	int i;

	for (i = 0; i < vi->max_queue_pairs; i++) {
		struct virtqueue *vq = vi->sq[i].vq;
		while ((buf = virtqueue_detach_unused_buf(vq)) != NULL)
			virtnet_sq_free_unused_buf(vq, buf);
		cond_resched();
	}

	for (i = 0; i < vi->max_queue_pairs; i++) {
		struct virtqueue *vq = vi->rq[i].vq;

		while ((buf = virtqueue_detach_unused_buf(vq)) != NULL)
			virtnet_rq_unmap_free_buf(vq, buf);
		cond_resched();
	}
}

static void virtnet_del_vqs(struct virtnet_info *vi)
{
	struct virtio_device *vdev = vi->vdev;

	virtnet_clean_affinity(vi);

	vdev->config->del_vqs(vdev);

	virtnet_free_queues(vi);
}

/* How large should a single buffer be so a queue full of these can fit at
 * least one full packet?
 * Logic below assumes the mergeable buffer header is used.
 */
static unsigned int mergeable_min_buf_len(struct virtnet_info *vi, struct virtqueue *vq)
{
	const unsigned int hdr_len = vi->hdr_len;
	unsigned int rq_size = virtqueue_get_vring_size(vq);
	unsigned int packet_len = vi->big_packets ? IP_MAX_MTU : vi->dev->max_mtu;
	unsigned int buf_len = hdr_len + ETH_HLEN + VLAN_HLEN + packet_len;
	unsigned int min_buf_len = DIV_ROUND_UP(buf_len, rq_size);

	return max(max(min_buf_len, hdr_len) - hdr_len,
		   (unsigned int)GOOD_PACKET_LEN);
}

static int virtnet_find_vqs(struct virtnet_info *vi)
{
	struct virtqueue_info *vqs_info;
	struct virtqueue **vqs;
	int ret = -ENOMEM;
	int total_vqs;
	bool *ctx;
	u16 i;

	/* We expect 1 RX virtqueue followed by 1 TX virtqueue, followed by
	 * possible N-1 RX/TX queue pairs used in multiqueue mode, followed by
	 * possible control vq.
	 */
	total_vqs = vi->max_queue_pairs * 2 +
		    virtio_has_feature(vi->vdev, VIRTIO_NET_F_CTRL_VQ);

	/* Allocate space for find_vqs parameters */
	vqs = kcalloc(total_vqs, sizeof(*vqs), GFP_KERNEL);
	if (!vqs)
		goto err_vq;
	vqs_info = kcalloc(total_vqs, sizeof(*vqs_info), GFP_KERNEL);
	if (!vqs_info)
		goto err_vqs_info;
	if (!vi->big_packets || vi->mergeable_rx_bufs) {
		ctx = kcalloc(total_vqs, sizeof(*ctx), GFP_KERNEL);
		if (!ctx)
			goto err_ctx;
	} else {
		ctx = NULL;
	}

	/* Parameters for control virtqueue, if any */
	if (vi->has_cvq) {
		vqs_info[total_vqs - 1].name = "control";
	}

	/* Allocate/initialize parameters for send/receive virtqueues */
	for (i = 0; i < vi->max_queue_pairs; i++) {
		vqs_info[rxq2vq(i)].callback = skb_recv_done;
		vqs_info[txq2vq(i)].callback = skb_xmit_done;
		sprintf(vi->rq[i].name, "input.%u", i);
		sprintf(vi->sq[i].name, "output.%u", i);
		vqs_info[rxq2vq(i)].name = vi->rq[i].name;
		vqs_info[txq2vq(i)].name = vi->sq[i].name;
		if (ctx)
			vqs_info[rxq2vq(i)].ctx = true;
	}

	ret = virtio_find_vqs(vi->vdev, total_vqs, vqs, vqs_info, NULL);
	if (ret)
		goto err_find;

	if (vi->has_cvq) {
		vi->cvq = vqs[total_vqs - 1];
		if (virtio_has_feature(vi->vdev, VIRTIO_NET_F_CTRL_VLAN))
			vi->dev->features |= NETIF_F_HW_VLAN_CTAG_FILTER;
	}

	for (i = 0; i < vi->max_queue_pairs; i++) {
		vi->rq[i].vq = vqs[rxq2vq(i)];
		vi->rq[i].min_buf_len = mergeable_min_buf_len(vi, vi->rq[i].vq);
		vi->sq[i].vq = vqs[txq2vq(i)];
	}

	/* run here: ret == 0. */


err_find:
	kfree(ctx);
err_ctx:
	kfree(vqs_info);
err_vqs_info:
	kfree(vqs);
err_vq:
	return ret;
}

static int virtnet_alloc_queues(struct virtnet_info *vi)
{
	int i;

	if (vi->has_cvq) {
		vi->ctrl = kzalloc(sizeof(*vi->ctrl), GFP_KERNEL);
		if (!vi->ctrl)
			goto err_ctrl;
	} else {
		vi->ctrl = NULL;
	}
	vi->sq = kcalloc(vi->max_queue_pairs, sizeof(*vi->sq), GFP_KERNEL);
	if (!vi->sq)
		goto err_sq;
	vi->rq = kcalloc(vi->max_queue_pairs, sizeof(*vi->rq), GFP_KERNEL);
	if (!vi->rq)
		goto err_rq;

	INIT_DELAYED_WORK(&vi->refill, refill_work);
	for (i = 0; i < vi->max_queue_pairs; i++) {
		vi->rq[i].pages = NULL;
		netif_napi_add_config(vi->dev, &vi->rq[i].napi, virtnet_poll,
				      i);
		vi->rq[i].napi.weight = napi_weight;
		netif_napi_add_tx_weight(vi->dev, &vi->sq[i].napi,
					 virtnet_poll_tx,
					 napi_tx ? napi_weight : 0);

		sg_init_table(vi->rq[i].sg, ARRAY_SIZE(vi->rq[i].sg));
		ewma_pkt_len_init(&vi->rq[i].mrg_avg_pkt_len);
		sg_init_table(vi->sq[i].sg, ARRAY_SIZE(vi->sq[i].sg));

		u64_stats_init(&vi->rq[i].stats.syncp);
		u64_stats_init(&vi->sq[i].stats.syncp);
		mutex_init(&vi->rq[i].dim_lock);
	}

	return 0;

err_rq:
	kfree(vi->sq);
err_sq:
	kfree(vi->ctrl);
err_ctrl:
	return -ENOMEM;
}

static int init_vqs(struct virtnet_info *vi)
{
	int ret;

	/* Allocate send & receive queues */
	ret = virtnet_alloc_queues(vi);
	if (ret)
		goto err;

	ret = virtnet_find_vqs(vi);
	if (ret)
		goto err_free;

	cpus_read_lock();
	virtnet_set_affinity(vi);
	cpus_read_unlock();

	return 0;

err_free:
	virtnet_free_queues(vi);
err:
	return ret;
}

#ifdef CONFIG_SYSFS
static ssize_t mergeable_rx_buffer_size_show(struct netdev_rx_queue *queue,
		char *buf)
{
	struct virtnet_info *vi = netdev_priv(queue->dev);
	unsigned int queue_index = get_netdev_rx_queue_index(queue);
	unsigned int headroom = virtnet_get_headroom(vi);
	unsigned int tailroom = headroom ? sizeof(struct skb_shared_info) : 0;
	struct ewma_pkt_len *avg;

	BUG_ON(queue_index >= vi->max_queue_pairs);
	avg = &vi->rq[queue_index].mrg_avg_pkt_len;
	return sprintf(buf, "%u\n",
		       get_mergeable_buf_len(&vi->rq[queue_index], avg,
				       SKB_DATA_ALIGN(headroom + tailroom)));
}

static struct rx_queue_attribute mergeable_rx_buffer_size_attribute =
	__ATTR_RO(mergeable_rx_buffer_size);

static struct attribute *virtio_net_mrg_rx_attrs[] = {
	&mergeable_rx_buffer_size_attribute.attr,
	NULL
};

static const struct attribute_group virtio_net_mrg_rx_group = {
	.name = "virtio_net",
	.attrs = virtio_net_mrg_rx_attrs
};
#endif

static bool virtnet_fail_on_feature(struct virtio_device *vdev,
				    unsigned int fbit,
				    const char *fname, const char *dname)
{
	if (!virtio_has_feature(vdev, fbit))
		return false;

	dev_err(&vdev->dev, "device advertises feature %s but not %s",
		fname, dname);

	return true;
}

#define VIRTNET_FAIL_ON(vdev, fbit, dbit)			\
	virtnet_fail_on_feature(vdev, fbit, #fbit, dbit)

static bool virtnet_validate_features(struct virtio_device *vdev)
{
	if (!virtio_has_feature(vdev, VIRTIO_NET_F_CTRL_VQ) &&
	    (VIRTNET_FAIL_ON(vdev, VIRTIO_NET_F_CTRL_RX,
			     "VIRTIO_NET_F_CTRL_VQ") ||
	     VIRTNET_FAIL_ON(vdev, VIRTIO_NET_F_CTRL_VLAN,
			     "VIRTIO_NET_F_CTRL_VQ") ||
	     VIRTNET_FAIL_ON(vdev, VIRTIO_NET_F_GUEST_ANNOUNCE,
			     "VIRTIO_NET_F_CTRL_VQ") ||
	     VIRTNET_FAIL_ON(vdev, VIRTIO_NET_F_MQ, "VIRTIO_NET_F_CTRL_VQ") ||
	     VIRTNET_FAIL_ON(vdev, VIRTIO_NET_F_CTRL_MAC_ADDR,
			     "VIRTIO_NET_F_CTRL_VQ") ||
	     VIRTNET_FAIL_ON(vdev, VIRTIO_NET_F_RSS,
			     "VIRTIO_NET_F_CTRL_VQ") ||
	     VIRTNET_FAIL_ON(vdev, VIRTIO_NET_F_HASH_REPORT,
			     "VIRTIO_NET_F_CTRL_VQ") ||
	     VIRTNET_FAIL_ON(vdev, VIRTIO_NET_F_NOTF_COAL,
			     "VIRTIO_NET_F_CTRL_VQ") ||
	     VIRTNET_FAIL_ON(vdev, VIRTIO_NET_F_VQ_NOTF_COAL,
			     "VIRTIO_NET_F_CTRL_VQ"))) {
		return false;
	}

	return true;
}

#define MIN_MTU ETH_MIN_MTU
#define MAX_MTU ETH_MAX_MTU

static int virtnet_validate(struct virtio_device *vdev)
{
	if (!vdev->config->get) {
		dev_err(&vdev->dev, "%s failure: config access disabled\n",
			__func__);
		return -EINVAL;
	}

	if (!virtnet_validate_features(vdev))
		return -EINVAL;

	if (virtio_has_feature(vdev, VIRTIO_NET_F_MTU)) {
		int mtu = virtio_cread16(vdev,
					 offsetof(struct virtio_net_config,
						  mtu));
		if (mtu < MIN_MTU)
			__virtio_clear_bit(vdev, VIRTIO_NET_F_MTU);
	}

	if (virtio_has_feature(vdev, VIRTIO_NET_F_STANDBY) &&
	    !virtio_has_feature(vdev, VIRTIO_NET_F_MAC)) {
		dev_warn(&vdev->dev, "device advertises feature VIRTIO_NET_F_STANDBY but not VIRTIO_NET_F_MAC, disabling standby");
		__virtio_clear_bit(vdev, VIRTIO_NET_F_STANDBY);
	}

	return 0;
}

static bool virtnet_check_guest_gso(const struct virtnet_info *vi)
{
	return virtio_has_feature(vi->vdev, VIRTIO_NET_F_GUEST_TSO4) ||
		virtio_has_feature(vi->vdev, VIRTIO_NET_F_GUEST_TSO6) ||
		virtio_has_feature(vi->vdev, VIRTIO_NET_F_GUEST_ECN) ||
		virtio_has_feature(vi->vdev, VIRTIO_NET_F_GUEST_UFO) ||
		(virtio_has_feature(vi->vdev, VIRTIO_NET_F_GUEST_USO4) &&
		virtio_has_feature(vi->vdev, VIRTIO_NET_F_GUEST_USO6));
}

static void virtnet_set_big_packets(struct virtnet_info *vi, const int mtu)
{
	bool guest_gso = virtnet_check_guest_gso(vi);

	/* If device can receive ANY guest GSO packets, regardless of mtu,
	 * allocate packets of maximum size, otherwise limit it to only
	 * mtu size worth only.
	 */
	if (mtu > ETH_DATA_LEN || guest_gso) {
		vi->big_packets = true;
		vi->big_packets_num_skbfrags = guest_gso ? MAX_SKB_FRAGS : DIV_ROUND_UP(mtu, PAGE_SIZE);
	}
}

#define VIRTIO_NET_HASH_REPORT_MAX_TABLE      10
static enum xdp_rss_hash_type
virtnet_xdp_rss_type[VIRTIO_NET_HASH_REPORT_MAX_TABLE] = {
	[VIRTIO_NET_HASH_REPORT_NONE] = XDP_RSS_TYPE_NONE,
	[VIRTIO_NET_HASH_REPORT_IPv4] = XDP_RSS_TYPE_L3_IPV4,
	[VIRTIO_NET_HASH_REPORT_TCPv4] = XDP_RSS_TYPE_L4_IPV4_TCP,
	[VIRTIO_NET_HASH_REPORT_UDPv4] = XDP_RSS_TYPE_L4_IPV4_UDP,
	[VIRTIO_NET_HASH_REPORT_IPv6] = XDP_RSS_TYPE_L3_IPV6,
	[VIRTIO_NET_HASH_REPORT_TCPv6] = XDP_RSS_TYPE_L4_IPV6_TCP,
	[VIRTIO_NET_HASH_REPORT_UDPv6] = XDP_RSS_TYPE_L4_IPV6_UDP,
	[VIRTIO_NET_HASH_REPORT_IPv6_EX] = XDP_RSS_TYPE_L3_IPV6_EX,
	[VIRTIO_NET_HASH_REPORT_TCPv6_EX] = XDP_RSS_TYPE_L4_IPV6_TCP_EX,
	[VIRTIO_NET_HASH_REPORT_UDPv6_EX] = XDP_RSS_TYPE_L4_IPV6_UDP_EX
};

static int virtnet_xdp_rx_hash(const struct xdp_md *_ctx, u32 *hash,
			       enum xdp_rss_hash_type *rss_type)
{
	const struct xdp_buff *xdp = (void *)_ctx;
	struct virtio_net_hdr_v1_hash *hdr_hash;
	struct virtnet_info *vi;
	u16 hash_report;

	if (!(xdp->rxq->dev->features & NETIF_F_RXHASH))
		return -ENODATA;

	vi = netdev_priv(xdp->rxq->dev);
	hdr_hash = (struct virtio_net_hdr_v1_hash *)(xdp->data - vi->hdr_len);
	hash_report = __le16_to_cpu(hdr_hash->hash_report);

	if (hash_report >= VIRTIO_NET_HASH_REPORT_MAX_TABLE)
		hash_report = VIRTIO_NET_HASH_REPORT_NONE;

	*rss_type = virtnet_xdp_rss_type[hash_report];
	*hash = __le32_to_cpu(hdr_hash->hash_value);
	return 0;
}

static const struct xdp_metadata_ops virtnet_xdp_metadata_ops = {
	.xmo_rx_hash			= virtnet_xdp_rx_hash,
};

static int virtnet_probe(struct virtio_device *vdev)
{
	int i, err = -ENOMEM;
	struct net_device *dev;
	struct virtnet_info *vi;
	u16 max_queue_pairs;
	int mtu = 0;

	/* Find if host supports multiqueue/rss virtio_net device */
	max_queue_pairs = 1;
	if (virtio_has_feature(vdev, VIRTIO_NET_F_MQ) || virtio_has_feature(vdev, VIRTIO_NET_F_RSS))
		max_queue_pairs =
		     virtio_cread16(vdev, offsetof(struct virtio_net_config, max_virtqueue_pairs));

	/* We need at least 2 queue's */
	if (max_queue_pairs < VIRTIO_NET_CTRL_MQ_VQ_PAIRS_MIN ||
	    max_queue_pairs > VIRTIO_NET_CTRL_MQ_VQ_PAIRS_MAX ||
	    !virtio_has_feature(vdev, VIRTIO_NET_F_CTRL_VQ))
		max_queue_pairs = 1;

	/* Allocate ourselves a network device with room for our info */
	dev = alloc_etherdev_mq(sizeof(struct virtnet_info), max_queue_pairs);
	if (!dev)
		return -ENOMEM;

	/* Set up network device as normal. */
	dev->priv_flags |= IFF_UNICAST_FLT | IFF_LIVE_ADDR_CHANGE |
			   IFF_TX_SKB_NO_LINEAR;
	dev->netdev_ops = &virtnet_netdev;
	dev->stat_ops = &virtnet_stat_ops;
	dev->features = NETIF_F_HIGHDMA;

	dev->ethtool_ops = &virtnet_ethtool_ops;
	SET_NETDEV_DEV(dev, &vdev->dev);

	/* Do we support "hardware" checksums? */
	if (virtio_has_feature(vdev, VIRTIO_NET_F_CSUM)) {
		/* This opens up the world of extra features. */
		dev->hw_features |= NETIF_F_HW_CSUM | NETIF_F_SG;
		if (csum)
			dev->features |= NETIF_F_HW_CSUM | NETIF_F_SG;

		if (virtio_has_feature(vdev, VIRTIO_NET_F_GSO)) {
			dev->hw_features |= NETIF_F_TSO
				| NETIF_F_TSO_ECN | NETIF_F_TSO6;
		}
		/* Individual feature bits: what can host handle? */
		if (virtio_has_feature(vdev, VIRTIO_NET_F_HOST_TSO4))
			dev->hw_features |= NETIF_F_TSO;
		if (virtio_has_feature(vdev, VIRTIO_NET_F_HOST_TSO6))
			dev->hw_features |= NETIF_F_TSO6;
		if (virtio_has_feature(vdev, VIRTIO_NET_F_HOST_ECN))
			dev->hw_features |= NETIF_F_TSO_ECN;
		if (virtio_has_feature(vdev, VIRTIO_NET_F_HOST_USO))
			dev->hw_features |= NETIF_F_GSO_UDP_L4;

		dev->features |= NETIF_F_GSO_ROBUST;

		if (gso)
			dev->features |= dev->hw_features & NETIF_F_ALL_TSO;
		/* (!csum && gso) case will be fixed by register_netdev() */
	}

	/* 1. With VIRTIO_NET_F_GUEST_CSUM negotiation, the driver doesn't
	 * need to calculate checksums for partially checksummed packets,
	 * as they're considered valid by the upper layer.
	 * 2. Without VIRTIO_NET_F_GUEST_CSUM negotiation, the driver only
	 * receives fully checksummed packets. The device may assist in
	 * validating these packets' checksums, so the driver won't have to.
	 */
	dev->features |= NETIF_F_RXCSUM;

	if (virtio_has_feature(vdev, VIRTIO_NET_F_GUEST_TSO4) ||
	    virtio_has_feature(vdev, VIRTIO_NET_F_GUEST_TSO6))
		dev->features |= NETIF_F_GRO_HW;
	if (virtio_has_feature(vdev, VIRTIO_NET_F_CTRL_GUEST_OFFLOADS))
		dev->hw_features |= NETIF_F_GRO_HW;

	dev->vlan_features = dev->features;
	dev->xdp_features = NETDEV_XDP_ACT_BASIC | NETDEV_XDP_ACT_REDIRECT |
		NETDEV_XDP_ACT_XSK_ZEROCOPY;

	/* MTU range: 68 - 65535 */
	dev->min_mtu = MIN_MTU;
	dev->max_mtu = MAX_MTU;

	/* Configuration may specify what MAC to use.  Otherwise random. */
	if (virtio_has_feature(vdev, VIRTIO_NET_F_MAC)) {
		u8 addr[ETH_ALEN];

		virtio_cread_bytes(vdev,
				   offsetof(struct virtio_net_config, mac),
				   addr, ETH_ALEN);
		eth_hw_addr_set(dev, addr);
	} else {
		eth_hw_addr_random(dev);
		dev_info(&vdev->dev, "Assigned random MAC address %pM\n",
			 dev->dev_addr);
	}

	/* Set up our device-specific information */
	vi = netdev_priv(dev);
	vi->dev = dev;
	vi->vdev = vdev;
	vdev->priv = vi;

	INIT_WORK(&vi->config_work, virtnet_config_changed_work);
	INIT_WORK(&vi->rx_mode_work, virtnet_rx_mode_work);
	spin_lock_init(&vi->refill_lock);

	if (virtio_has_feature(vdev, VIRTIO_NET_F_MRG_RXBUF)) {
		vi->mergeable_rx_bufs = true;
		dev->xdp_features |= NETDEV_XDP_ACT_RX_SG;
	}

	if (virtio_has_feature(vdev, VIRTIO_NET_F_HASH_REPORT))
		vi->has_rss_hash_report = true;

	if (virtio_has_feature(vdev, VIRTIO_NET_F_RSS)) {
		vi->has_rss = true;

		vi->rss_indir_table_size =
			virtio_cread16(vdev, offsetof(struct virtio_net_config,
				rss_max_indirection_table_length));
	}
	vi->rss_hdr = devm_kzalloc(&vdev->dev, virtnet_rss_hdr_size(vi), GFP_KERNEL);
	if (!vi->rss_hdr) {
		err = -ENOMEM;
		goto free;
	}

	if (vi->has_rss || vi->has_rss_hash_report) {
		vi->rss_key_size =
			virtio_cread8(vdev, offsetof(struct virtio_net_config, rss_max_key_size));
		if (vi->rss_key_size > VIRTIO_NET_RSS_MAX_KEY_SIZE) {
			dev_err(&vdev->dev, "rss_max_key_size=%u exceeds the limit %u.\n",
				vi->rss_key_size, VIRTIO_NET_RSS_MAX_KEY_SIZE);
			err = -EINVAL;
			goto free;
		}

		vi->rss_hash_types_supported =
		    virtio_cread32(vdev, offsetof(struct virtio_net_config, supported_hash_types));
		vi->rss_hash_types_supported &=
				~(VIRTIO_NET_RSS_HASH_TYPE_IP_EX |
				  VIRTIO_NET_RSS_HASH_TYPE_TCP_EX |
				  VIRTIO_NET_RSS_HASH_TYPE_UDP_EX);

		dev->hw_features |= NETIF_F_RXHASH;
		dev->xdp_metadata_ops = &virtnet_xdp_metadata_ops;
	}

	if (vi->has_rss_hash_report)
		vi->hdr_len = sizeof(struct virtio_net_hdr_v1_hash);
	else if (virtio_has_feature(vdev, VIRTIO_NET_F_MRG_RXBUF) ||
		 virtio_has_feature(vdev, VIRTIO_F_VERSION_1))
		vi->hdr_len = sizeof(struct virtio_net_hdr_mrg_rxbuf);
	else
		vi->hdr_len = sizeof(struct virtio_net_hdr);

	if (virtio_has_feature(vdev, VIRTIO_F_ANY_LAYOUT) ||
	    virtio_has_feature(vdev, VIRTIO_F_VERSION_1))
		vi->any_header_sg = true;

	if (virtio_has_feature(vdev, VIRTIO_NET_F_CTRL_VQ))
		vi->has_cvq = true;

	mutex_init(&vi->cvq_lock);

	if (virtio_has_feature(vdev, VIRTIO_NET_F_MTU)) {
		mtu = virtio_cread16(vdev,
				     offsetof(struct virtio_net_config,
					      mtu));
		if (mtu < dev->min_mtu) {
			/* Should never trigger: MTU was previously validated
			 * in virtnet_validate.
			 */
			dev_err(&vdev->dev,
				"device MTU appears to have changed it is now %d < %d",
				mtu, dev->min_mtu);
			err = -EINVAL;
			goto free;
		}

		dev->mtu = mtu;
		dev->max_mtu = mtu;
	}

	virtnet_set_big_packets(vi, mtu);

	if (vi->any_header_sg)
		dev->needed_headroom = vi->hdr_len;

	/* Enable multiqueue by default */
	if (num_online_cpus() >= max_queue_pairs)
		vi->curr_queue_pairs = max_queue_pairs;
	else
		vi->curr_queue_pairs = num_online_cpus();
	vi->max_queue_pairs = max_queue_pairs;

	/* Allocate/initialize the rx/tx queues, and invoke find_vqs */
	err = init_vqs(vi);
	if (err)
		goto free;

	if (virtio_has_feature(vi->vdev, VIRTIO_NET_F_NOTF_COAL)) {
		vi->intr_coal_rx.max_usecs = 0;
		vi->intr_coal_tx.max_usecs = 0;
		vi->intr_coal_rx.max_packets = 0;

		/* Keep the default values of the coalescing parameters
		 * aligned with the default napi_tx state.
		 */
		if (vi->sq[0].napi.weight)
			vi->intr_coal_tx.max_packets = 1;
		else
			vi->intr_coal_tx.max_packets = 0;
	}

	if (virtio_has_feature(vi->vdev, VIRTIO_NET_F_VQ_NOTF_COAL)) {
		/* The reason is the same as VIRTIO_NET_F_NOTF_COAL. */
		for (i = 0; i < vi->max_queue_pairs; i++)
			if (vi->sq[i].napi.weight)
				vi->sq[i].intr_coal.max_packets = 1;

		err = virtnet_init_irq_moder(vi);
		if (err)
			goto free;
	}

#ifdef CONFIG_SYSFS
	if (vi->mergeable_rx_bufs)
		dev->sysfs_rx_queue_group = &virtio_net_mrg_rx_group;
#endif
	netif_set_real_num_tx_queues(dev, vi->curr_queue_pairs);
	netif_set_real_num_rx_queues(dev, vi->curr_queue_pairs);

	virtnet_init_settings(dev);

	if (virtio_has_feature(vdev, VIRTIO_NET_F_STANDBY)) {
		vi->failover = net_failover_create(vi->dev);
		if (IS_ERR(vi->failover)) {
			err = PTR_ERR(vi->failover);
			goto free_vqs;
		}
	}

	if (vi->has_rss || vi->has_rss_hash_report)
		virtnet_init_default_rss(vi);

	enable_rx_mode_work(vi);

	/* serialize netdev register + virtio_device_ready() with ndo_open() */
	rtnl_lock();

	err = register_netdevice(dev);
	if (err) {
		pr_debug("virtio_net: registering device failed\n");
		rtnl_unlock();
		goto free_failover;
	}

	/* Disable config change notification until ndo_open. */
	virtio_config_driver_disable(vi->vdev);

	virtio_device_ready(vdev);

	if (vi->has_rss || vi->has_rss_hash_report) {
		if (!virtnet_commit_rss_command(vi)) {
			dev_warn(&vdev->dev, "RSS disabled because committing failed.\n");
			dev->hw_features &= ~NETIF_F_RXHASH;
			vi->has_rss_hash_report = false;
			vi->has_rss = false;
		}
	}

	virtnet_set_queues(vi, vi->curr_queue_pairs);

	/* a random MAC address has been assigned, notify the device.
	 * We don't fail probe if VIRTIO_NET_F_CTRL_MAC_ADDR is not there
	 * because many devices work fine without getting MAC explicitly
	 */
	if (!virtio_has_feature(vdev, VIRTIO_NET_F_MAC) &&
	    virtio_has_feature(vi->vdev, VIRTIO_NET_F_CTRL_MAC_ADDR)) {
		struct scatterlist sg;

		sg_init_one(&sg, dev->dev_addr, dev->addr_len);
		if (!virtnet_send_command(vi, VIRTIO_NET_CTRL_MAC,
					  VIRTIO_NET_CTRL_MAC_ADDR_SET, &sg)) {
			pr_debug("virtio_net: setting MAC address failed\n");
			rtnl_unlock();
			err = -EINVAL;
			goto free_unregister_netdev;
		}
	}

	if (virtio_has_feature(vi->vdev, VIRTIO_NET_F_DEVICE_STATS)) {
		struct virtio_net_stats_capabilities *stats_cap  __free(kfree) = NULL;
		struct scatterlist sg;
		__le64 v;

		stats_cap = kzalloc(sizeof(*stats_cap), GFP_KERNEL);
		if (!stats_cap) {
			rtnl_unlock();
			err = -ENOMEM;
			goto free_unregister_netdev;
		}

		sg_init_one(&sg, stats_cap, sizeof(*stats_cap));

		if (!virtnet_send_command_reply(vi, VIRTIO_NET_CTRL_STATS,
						VIRTIO_NET_CTRL_STATS_QUERY,
						NULL, &sg)) {
			pr_debug("virtio_net: fail to get stats capability\n");
			rtnl_unlock();
			err = -EINVAL;
			goto free_unregister_netdev;
		}

		v = stats_cap->supported_stats_types[0];
		vi->device_stats_cap = le64_to_cpu(v);
	}

	/* Assume link up if device can't report link status,
	   otherwise get link status from config. */
	netif_carrier_off(dev);
	if (virtio_has_feature(vi->vdev, VIRTIO_NET_F_STATUS)) {
		virtio_config_changed(vi->vdev);
	} else {
		vi->status = VIRTIO_NET_S_LINK_UP;
		virtnet_update_settings(vi);
		netif_carrier_on(dev);
	}

	for (i = 0; i < ARRAY_SIZE(guest_offloads); i++)
		if (virtio_has_feature(vi->vdev, guest_offloads[i]))
			set_bit(guest_offloads[i], &vi->guest_offloads);
	vi->guest_offloads_capable = vi->guest_offloads;

	rtnl_unlock();

	err = virtnet_cpu_notif_add(vi);
	if (err) {
		pr_debug("virtio_net: registering cpu notifier failed\n");
		goto free_unregister_netdev;
	}

	pr_debug("virtnet: registered device %s with %d RX and TX vq's\n",
		 dev->name, max_queue_pairs);

	return 0;

free_unregister_netdev:
	unregister_netdev(dev);
free_failover:
	net_failover_destroy(vi->failover);
free_vqs:
	virtio_reset_device(vdev);
	cancel_delayed_work_sync(&vi->refill);
	free_receive_page_frags(vi);
	virtnet_del_vqs(vi);
free:
	free_netdev(dev);
	return err;
}

static void remove_vq_common(struct virtnet_info *vi)
{
	int i;

	virtio_reset_device(vi->vdev);

	/* Free unused buffers in both send and recv, if any. */
	free_unused_bufs(vi);

	/*
	 * Rule of thumb is netdev_tx_reset_queue() should follow any
	 * skb freeing not followed by netdev_tx_completed_queue()
	 */
	for (i = 0; i < vi->max_queue_pairs; i++)
		netdev_tx_reset_queue(netdev_get_tx_queue(vi->dev, i));

	free_receive_bufs(vi);

	free_receive_page_frags(vi);

	virtnet_del_vqs(vi);
}

static void virtnet_remove(struct virtio_device *vdev)
{
	struct virtnet_info *vi = vdev->priv;

	virtnet_cpu_notif_remove(vi);

	/* Make sure no work handler is accessing the device. */
	flush_work(&vi->config_work);
	disable_rx_mode_work(vi);
	flush_work(&vi->rx_mode_work);

	virtnet_free_irq_moder(vi);

	unregister_netdev(vi->dev);

	net_failover_destroy(vi->failover);

	remove_vq_common(vi);

	free_netdev(vi->dev);
}

static __maybe_unused int virtnet_freeze(struct virtio_device *vdev)
{
	struct virtnet_info *vi = vdev->priv;

	virtnet_cpu_notif_remove(vi);
	virtnet_freeze_down(vdev);
	remove_vq_common(vi);

	return 0;
}

static __maybe_unused int virtnet_restore(struct virtio_device *vdev)
{
	struct virtnet_info *vi = vdev->priv;
	int err;

	err = virtnet_restore_up(vdev);
	if (err)
		return err;
	virtnet_set_queues(vi, vi->curr_queue_pairs);

	err = virtnet_cpu_notif_add(vi);
	if (err) {
		virtnet_freeze_down(vdev);
		remove_vq_common(vi);
		return err;
	}

	return 0;
}

static struct virtio_device_id id_table[] = {
	{ VIRTIO_ID_NET, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

#define VIRTNET_FEATURES \
	VIRTIO_NET_F_CSUM, VIRTIO_NET_F_GUEST_CSUM, \
	VIRTIO_NET_F_MAC, \
	VIRTIO_NET_F_HOST_TSO4, VIRTIO_NET_F_HOST_UFO, VIRTIO_NET_F_HOST_TSO6, \
	VIRTIO_NET_F_HOST_ECN, VIRTIO_NET_F_GUEST_TSO4, VIRTIO_NET_F_GUEST_TSO6, \
	VIRTIO_NET_F_GUEST_ECN, VIRTIO_NET_F_GUEST_UFO, \
	VIRTIO_NET_F_HOST_USO, VIRTIO_NET_F_GUEST_USO4, VIRTIO_NET_F_GUEST_USO6, \
	VIRTIO_NET_F_MRG_RXBUF, VIRTIO_NET_F_STATUS, VIRTIO_NET_F_CTRL_VQ, \
	VIRTIO_NET_F_CTRL_RX, VIRTIO_NET_F_CTRL_VLAN, \
	VIRTIO_NET_F_GUEST_ANNOUNCE, VIRTIO_NET_F_MQ, \
	VIRTIO_NET_F_CTRL_MAC_ADDR, \
	VIRTIO_NET_F_MTU, VIRTIO_NET_F_CTRL_GUEST_OFFLOADS, \
	VIRTIO_NET_F_SPEED_DUPLEX, VIRTIO_NET_F_STANDBY, \
	VIRTIO_NET_F_RSS, VIRTIO_NET_F_HASH_REPORT, VIRTIO_NET_F_NOTF_COAL, \
	VIRTIO_NET_F_VQ_NOTF_COAL, \
	VIRTIO_NET_F_GUEST_HDRLEN, VIRTIO_NET_F_DEVICE_STATS

static unsigned int features[] = {
	VIRTNET_FEATURES,
};

static unsigned int features_legacy[] = {
	VIRTNET_FEATURES,
	VIRTIO_NET_F_GSO,
	VIRTIO_F_ANY_LAYOUT,
};

static struct virtio_driver virtio_net_driver = {
	.feature_table = features,
	.feature_table_size = ARRAY_SIZE(features),
	.feature_table_legacy = features_legacy,
	.feature_table_size_legacy = ARRAY_SIZE(features_legacy),
	.driver.name =	KBUILD_MODNAME,
	.id_table =	id_table,
	.validate =	virtnet_validate,
	.probe =	virtnet_probe,
	.remove =	virtnet_remove,
	.config_changed = virtnet_config_changed,
#ifdef CONFIG_PM_SLEEP
	.freeze =	virtnet_freeze,
	.restore =	virtnet_restore,
#endif
};

static __init int virtio_net_driver_init(void)
{
	int ret;

	ret = cpuhp_setup_state_multi(CPUHP_AP_ONLINE_DYN, "virtio/net:online",
				      virtnet_cpu_online,
				      virtnet_cpu_down_prep);
	if (ret < 0)
		goto out;
	virtionet_online = ret;
	ret = cpuhp_setup_state_multi(CPUHP_VIRT_NET_DEAD, "virtio/net:dead",
				      NULL, virtnet_cpu_dead);
	if (ret)
		goto err_dead;
	ret = register_virtio_driver(&virtio_net_driver);
	if (ret)
		goto err_virtio;
	return 0;
err_virtio:
	cpuhp_remove_multi_state(CPUHP_VIRT_NET_DEAD);
err_dead:
	cpuhp_remove_multi_state(virtionet_online);
out:
	return ret;
}
module_init(virtio_net_driver_init);

static __exit void virtio_net_driver_exit(void)
{
	unregister_virtio_driver(&virtio_net_driver);
	cpuhp_remove_multi_state(CPUHP_VIRT_NET_DEAD);
	cpuhp_remove_multi_state(virtionet_online);
}
module_exit(virtio_net_driver_exit);

MODULE_DEVICE_TABLE(virtio, id_table);
MODULE_DESCRIPTION("Virtio network driver");
MODULE_LICENSE("GPL");
