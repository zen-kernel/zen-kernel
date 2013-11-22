/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
   reiser4/README */

#include "debug.h"
#include "super.h"
#include "txnmgr.h"
#include "jnode.h"
#include "znode.h"
#include "page_cache.h"
#include "wander.h"
#include "vfs_ops.h"
#include "writeout.h"
#include "flush.h"

#include <linux/bio.h>
#include <linux/mm.h>
#include <linux/pagemap.h>
#include <linux/blkdev.h>
#include <linux/writeback.h>

/* A flush queue object is an accumulator for keeping jnodes prepared
   by the jnode_flush() function for writing to disk. Those "queued" jnodes are
   kept on the flush queue until memory pressure or atom commit asks
   flush queues to write some or all from their jnodes. */

/*
   LOCKING:

   fq->guard spin lock protects fq->atom pointer and nothing else.  fq->prepped
   list protected by atom spin lock.  fq->prepped list uses the following
   locking:

   two ways to protect fq->prepped list for read-only list traversal:

   1. atom spin-lock atom.
   2. fq is IN_USE, atom->nr_running_queues increased.

   and one for list modification:

   1. atom is spin-locked and one condition is true: fq is IN_USE or
      atom->nr_running_queues == 0.

   The deadlock-safe order for flush queues and atoms is: first lock atom, then
   lock flush queue, then lock jnode.
*/

#define fq_in_use(fq)          ((fq)->state & FQ_IN_USE)
#define fq_ready(fq)           (!fq_in_use(fq))

#define mark_fq_in_use(fq)     do { (fq)->state |= FQ_IN_USE;    } while (0)
#define mark_fq_ready(fq)      do { (fq)->state &= ~FQ_IN_USE;   } while (0)

/* get lock on atom from locked flush queue object */
static txn_atom *atom_locked_by_fq_nolock(flush_queue_t *fq)
{
	/* This code is similar to jnode_get_atom(), look at it for the
	 * explanation. */
	txn_atom *atom;

	assert_spin_locked(&(fq->guard));

	while (1) {
		atom = fq->atom;
		if (atom == NULL)
			break;

		if (spin_trylock_atom(atom))
			break;

		atomic_inc(&atom->refcount);
		spin_unlock(&(fq->guard));
		spin_lock_atom(atom);
		spin_lock(&(fq->guard));

		if (fq->atom == atom) {
			atomic_dec(&atom->refcount);
			break;
		}

		spin_unlock(&(fq->guard));
		atom_dec_and_unlock(atom);
		spin_lock(&(fq->guard));
	}

	return atom;
}

txn_atom *atom_locked_by_fq(flush_queue_t *fq)
{
	txn_atom *atom;

	spin_lock(&(fq->guard));
	atom = atom_locked_by_fq_nolock(fq);
	spin_unlock(&(fq->guard));
	return atom;
}

static void init_fq(flush_queue_t *fq)
{
	memset(fq, 0, sizeof *fq);

	atomic_set(&fq->nr_submitted, 0);

	INIT_LIST_HEAD(ATOM_FQ_LIST(fq));

	init_waitqueue_head(&fq->wait);
	spin_lock_init(&fq->guard);
}

/* slab for flush queues */
static struct kmem_cache *fq_slab;

/**
 * reiser4_init_fqs - create flush queue cache
 *
 * Initializes slab cache of flush queues. It is part of reiser4 module
 * initialization.
 */
int reiser4_init_fqs(void)
{
	fq_slab = kmem_cache_create("fq",
				    sizeof(flush_queue_t),
				    0, SLAB_HWCACHE_ALIGN, NULL);
	if (fq_slab == NULL)
		return RETERR(-ENOMEM);
	return 0;
}

/**
 * reiser4_done_fqs - delete flush queue cache
 *
 * This is called on reiser4 module unloading or system shutdown.
 */
void reiser4_done_fqs(void)
{
	destroy_reiser4_cache(&fq_slab);
}

/* create new flush queue object */
static flush_queue_t *create_fq(gfp_t gfp)
{
	flush_queue_t *fq;

	fq = kmem_cache_alloc(fq_slab, gfp);
	if (fq)
		init_fq(fq);

	return fq;
}

/* adjust atom's and flush queue's counters of queued nodes */
static void count_enqueued_node(flush_queue_t *fq)
{
	ON_DEBUG(fq->atom->num_queued++);
}

static void count_dequeued_node(flush_queue_t *fq)
{
	assert("zam-993", fq->atom->num_queued > 0);
	ON_DEBUG(fq->atom->num_queued--);
}

/* attach flush queue object to the atom */
static void attach_fq(txn_atom *atom, flush_queue_t *fq)
{
	assert_spin_locked(&(atom->alock));
	list_add(&fq->alink, &atom->flush_queues);
	fq->atom = atom;
	ON_DEBUG(atom->nr_flush_queues++);
}

static void detach_fq(flush_queue_t *fq)
{
	assert_spin_locked(&(fq->atom->alock));

	spin_lock(&(fq->guard));
	list_del_init(&fq->alink);
	assert("vs-1456", fq->atom->nr_flush_queues > 0);
	ON_DEBUG(fq->atom->nr_flush_queues--);
	fq->atom = NULL;
	spin_unlock(&(fq->guard));
}

/* destroy flush queue object */
static void done_fq(flush_queue_t *fq)
{
	assert("zam-763", list_empty_careful(ATOM_FQ_LIST(fq)));
	assert("zam-766", atomic_read(&fq->nr_submitted) == 0);

	kmem_cache_free(fq_slab, fq);
}

/* */
static void mark_jnode_queued(flush_queue_t *fq, jnode * node)
{
	JF_SET(node, JNODE_FLUSH_QUEUED);
	count_enqueued_node(fq);
}

/* Putting jnode into the flush queue. Both atom and jnode should be
   spin-locked. */
void queue_jnode(flush_queue_t *fq, jnode * node)
{
	assert_spin_locked(&(node->guard));
	assert("zam-713", node->atom != NULL);
	assert_spin_locked(&(node->atom->alock));
	assert("zam-716", fq->atom != NULL);
	assert("zam-717", fq->atom == node->atom);
	assert("zam-907", fq_in_use(fq));

	assert("zam-714", JF_ISSET(node, JNODE_DIRTY));
	assert("zam-826", JF_ISSET(node, JNODE_RELOC));
	assert("vs-1481", !JF_ISSET(node, JNODE_FLUSH_QUEUED));
	assert("vs-1481", NODE_LIST(node) != FQ_LIST);

	mark_jnode_queued(fq, node);
	list_move_tail(&node->capture_link, ATOM_FQ_LIST(fq));

	ON_DEBUG(count_jnode(node->atom, node, NODE_LIST(node),
			     FQ_LIST, 1));
}

/* repeatable process for waiting io completion on a flush queue object */
static int wait_io(flush_queue_t *fq, int *nr_io_errors)
{
	assert("zam-738", fq->atom != NULL);
	assert_spin_locked(&(fq->atom->alock));
	assert("zam-736", fq_in_use(fq));
	assert("zam-911", list_empty_careful(ATOM_FQ_LIST(fq)));

	if (atomic_read(&fq->nr_submitted) != 0) {
		struct super_block *super;

		spin_unlock_atom(fq->atom);

		assert("nikita-3013", reiser4_schedulable());

		super = reiser4_get_current_sb();

		/* FIXME: this is instead of blk_run_queues() */
		//blk_flush_plug(current);

		if (!(super->s_flags & MS_RDONLY))
			wait_event(fq->wait,
				   atomic_read(&fq->nr_submitted) == 0);

		/* Ask the caller to re-acquire the locks and call this
		   function again. Note: this technique is commonly used in
		   the txnmgr code. */
		return -E_REPEAT;
	}

	*nr_io_errors += atomic_read(&fq->nr_errors);
	return 0;
}

/* wait on I/O completion, re-submit dirty nodes to write */
static int finish_fq(flush_queue_t *fq, int *nr_io_errors)
{
	int ret;
	txn_atom *atom = fq->atom;

	assert("zam-801", atom != NULL);
	assert_spin_locked(&(atom->alock));
	assert("zam-762", fq_in_use(fq));

	ret = wait_io(fq, nr_io_errors);
	if (ret)
		return ret;

	detach_fq(fq);
	done_fq(fq);

	reiser4_atom_send_event(atom);

	return 0;
}

/* wait for all i/o for given atom to be completed, actually do one iteration
   on that and return -E_REPEAT if there more iterations needed */
static int finish_all_fq(txn_atom * atom, int *nr_io_errors)
{
	flush_queue_t *fq;

	assert_spin_locked(&(atom->alock));

	if (list_empty_careful(&atom->flush_queues))
		return 0;

	list_for_each_entry(fq, &atom->flush_queues, alink) {
		if (fq_ready(fq)) {
			int ret;

			mark_fq_in_use(fq);
			assert("vs-1247", fq->owner == NULL);
			ON_DEBUG(fq->owner = current);
			ret = finish_fq(fq, nr_io_errors);

			if (*nr_io_errors)
				reiser4_handle_error();

			if (ret) {
				reiser4_fq_put(fq);
				return ret;
			}

			spin_unlock_atom(atom);

			return -E_REPEAT;
		}
	}

	/* All flush queues are in use; atom remains locked */
	return -EBUSY;
}

/* wait all i/o for current atom */
int current_atom_finish_all_fq(void)
{
	txn_atom *atom;
	int nr_io_errors = 0;
	int ret = 0;

	do {
		while (1) {
			atom = get_current_atom_locked();
			ret = finish_all_fq(atom, &nr_io_errors);
			if (ret != -EBUSY)
				break;
			reiser4_atom_wait_event(atom);
		}
	} while (ret == -E_REPEAT);

	/* we do not need locked atom after this function finishes, SUCCESS or
	   -EBUSY are two return codes when atom remains locked after
	   finish_all_fq */
	if (!ret)
		spin_unlock_atom(atom);

	assert_spin_not_locked(&(atom->alock));

	if (ret)
		return ret;

	if (nr_io_errors)
		return RETERR(-EIO);

	return 0;
}

/* change node->atom field for all jnode from given list */
static void
scan_fq_and_update_atom_ref(struct list_head *list, txn_atom *atom)
{
	jnode *cur;

	list_for_each_entry(cur, list, capture_link) {
		spin_lock_jnode(cur);
		cur->atom = atom;
		spin_unlock_jnode(cur);
	}
}

/* support for atom fusion operation */
void reiser4_fuse_fq(txn_atom *to, txn_atom *from)
{
	flush_queue_t *fq;

	assert_spin_locked(&(to->alock));
	assert_spin_locked(&(from->alock));

	list_for_each_entry(fq, &from->flush_queues, alink) {
		scan_fq_and_update_atom_ref(ATOM_FQ_LIST(fq), to);
		spin_lock(&(fq->guard));
		fq->atom = to;
		spin_unlock(&(fq->guard));
	}

	list_splice_init(&from->flush_queues, to->flush_queues.prev);

#if REISER4_DEBUG
	to->num_queued += from->num_queued;
	to->nr_flush_queues += from->nr_flush_queues;
	from->nr_flush_queues = 0;
#endif
}

#if REISER4_DEBUG
int atom_fq_parts_are_clean(txn_atom * atom)
{
	assert("zam-915", atom != NULL);
	return list_empty_careful(&atom->flush_queues);
}
#endif
/* Bio i/o completion routine for reiser4 write operations. */
static void
end_io_handler(struct bio *bio, int err)
{
	int i;
	int nr_errors = 0;
	flush_queue_t *fq;

	assert("zam-958", bio->bi_rw & WRITE);

	if (err == -EOPNOTSUPP)
		set_bit(BIO_EOPNOTSUPP, &bio->bi_flags);

	/* we expect that bio->private is set to NULL or fq object which is used
	 * for synchronization and error counting. */
	fq = bio->bi_private;
	/* Check all elements of io_vec for correct write completion. */
	for (i = 0; i < bio->bi_vcnt; i += 1) {
		struct page *pg = bio->bi_io_vec[i].bv_page;

		if (!test_bit(BIO_UPTODATE, &bio->bi_flags)) {
			SetPageError(pg);
			nr_errors++;
		}

		{
			/* jnode WRITEBACK ("write is in progress bit") is
			 * atomically cleared here. */
			jnode *node;

			assert("zam-736", pg != NULL);
			assert("zam-736", PagePrivate(pg));
			node = jprivate(pg);

			JF_CLR(node, JNODE_WRITEBACK);
		}

		end_page_writeback(pg);
		page_cache_release(pg);
	}

	if (fq) {
		/* count i/o error in fq object */
		atomic_add(nr_errors, &fq->nr_errors);

		/* If all write requests registered in this "fq" are done we up
		 * the waiter. */
		if (atomic_sub_and_test(bio->bi_vcnt, &fq->nr_submitted))
			wake_up(&fq->wait);
	}

	bio_put(bio);
}

/* Count I/O requests which will be submitted by @bio in given flush queues
   @fq */
void add_fq_to_bio(flush_queue_t *fq, struct bio *bio)
{
	bio->bi_private = fq;
	bio->bi_end_io = end_io_handler;

	if (fq)
		atomic_add(bio->bi_vcnt, &fq->nr_submitted);
}

/* Move all queued nodes out from @fq->prepped list. */
static void release_prepped_list(flush_queue_t *fq)
{
	txn_atom *atom;

	assert("zam-904", fq_in_use(fq));
	atom = atom_locked_by_fq(fq);

	while (!list_empty(ATOM_FQ_LIST(fq))) {
		jnode *cur;

		cur = list_entry(ATOM_FQ_LIST(fq)->next, jnode, capture_link);
		list_del_init(&cur->capture_link);

		count_dequeued_node(fq);
		spin_lock_jnode(cur);
		assert("nikita-3154", !JF_ISSET(cur, JNODE_OVRWR));
		assert("nikita-3154", JF_ISSET(cur, JNODE_RELOC));
		assert("nikita-3154", JF_ISSET(cur, JNODE_FLUSH_QUEUED));
		JF_CLR(cur, JNODE_FLUSH_QUEUED);

		if (JF_ISSET(cur, JNODE_DIRTY)) {
			list_add_tail(&cur->capture_link,
				      ATOM_DIRTY_LIST(atom,
						      jnode_get_level(cur)));
			ON_DEBUG(count_jnode(atom, cur, FQ_LIST,
					     DIRTY_LIST, 1));
		} else {
			list_add_tail(&cur->capture_link,
				      ATOM_CLEAN_LIST(atom));
			ON_DEBUG(count_jnode(atom, cur, FQ_LIST,
					     CLEAN_LIST, 1));
		}

		spin_unlock_jnode(cur);
	}

	if (--atom->nr_running_queues == 0)
		reiser4_atom_send_event(atom);

	spin_unlock_atom(atom);
}

/* Submit write requests for nodes on the already filled flush queue @fq.

   @fq: flush queue object which contains jnodes we can (and will) write.
   @return: number of submitted blocks (>=0) if success, otherwise -- an error
	    code (<0). */
int reiser4_write_fq(flush_queue_t *fq, long *nr_submitted, int flags)
{
	int ret;
	txn_atom *atom;

	while (1) {
		atom = atom_locked_by_fq(fq);
		assert("zam-924", atom);
		/* do not write fq in parallel. */
		if (atom->nr_running_queues == 0
		    || !(flags & WRITEOUT_SINGLE_STREAM))
			break;
		reiser4_atom_wait_event(atom);
	}

	atom->nr_running_queues++;
	spin_unlock_atom(atom);

	ret = write_jnode_list(ATOM_FQ_LIST(fq), fq, nr_submitted, flags);
	release_prepped_list(fq);

	return ret;
}

/* Getting flush queue object for exclusive use by one thread. May require
   several iterations which is indicated by -E_REPEAT return code.

   This function does not contain code for obtaining an atom lock because an
   atom lock is obtained by different ways in different parts of reiser4,
   usually it is current atom, but we need a possibility for getting fq for the
   atom of given jnode. */
static int fq_by_atom_gfp(txn_atom *atom, flush_queue_t **new_fq, gfp_t gfp)
{
	flush_queue_t *fq;

	assert_spin_locked(&(atom->alock));

	fq = list_entry(atom->flush_queues.next, flush_queue_t, alink);
	while (&atom->flush_queues != &fq->alink) {
		spin_lock(&(fq->guard));

		if (fq_ready(fq)) {
			mark_fq_in_use(fq);
			assert("vs-1246", fq->owner == NULL);
			ON_DEBUG(fq->owner = current);
			spin_unlock(&(fq->guard));

			if (*new_fq)
				done_fq(*new_fq);

			*new_fq = fq;

			return 0;
		}

		spin_unlock(&(fq->guard));

		fq = list_entry(fq->alink.next, flush_queue_t, alink);
	}

	/* Use previously allocated fq object */
	if (*new_fq) {
		mark_fq_in_use(*new_fq);
		assert("vs-1248", (*new_fq)->owner == 0);
		ON_DEBUG((*new_fq)->owner = current);
		attach_fq(atom, *new_fq);

		return 0;
	}

	spin_unlock_atom(atom);

	*new_fq = create_fq(gfp);

	if (*new_fq == NULL)
		return RETERR(-ENOMEM);

	return RETERR(-E_REPEAT);
}

int reiser4_fq_by_atom(txn_atom * atom, flush_queue_t **new_fq)
{
	return fq_by_atom_gfp(atom, new_fq, reiser4_ctx_gfp_mask_get());
}

/* A wrapper around reiser4_fq_by_atom for getting a flush queue
   object for current atom, if success fq->atom remains locked. */
flush_queue_t *get_fq_for_current_atom(void)
{
	flush_queue_t *fq = NULL;
	txn_atom *atom;
	int ret;

	do {
		atom = get_current_atom_locked();
		ret = reiser4_fq_by_atom(atom, &fq);
	} while (ret == -E_REPEAT);

	if (ret)
		return ERR_PTR(ret);
	return fq;
}

/* Releasing flush queue object after exclusive use */
void reiser4_fq_put_nolock(flush_queue_t *fq)
{
	assert("zam-747", fq->atom != NULL);
	assert("zam-902", list_empty_careful(ATOM_FQ_LIST(fq)));
	mark_fq_ready(fq);
	assert("vs-1245", fq->owner == current);
	ON_DEBUG(fq->owner = NULL);
}

void reiser4_fq_put(flush_queue_t *fq)
{
	txn_atom *atom;

	spin_lock(&(fq->guard));
	atom = atom_locked_by_fq_nolock(fq);

	assert("zam-746", atom != NULL);

	reiser4_fq_put_nolock(fq);
	reiser4_atom_send_event(atom);

	spin_unlock(&(fq->guard));
	spin_unlock_atom(atom);
}

/* A part of atom object initialization related to the embedded flush queue
   list head */

void init_atom_fq_parts(txn_atom *atom)
{
	INIT_LIST_HEAD(&atom->flush_queues);
}

#if REISER4_DEBUG

void reiser4_check_fq(const txn_atom *atom)
{
	/* check number of nodes on all atom's flush queues */
	flush_queue_t *fq;
	int count;
	struct list_head *pos;

	count = 0;
	list_for_each_entry(fq, &atom->flush_queues, alink) {
		spin_lock(&(fq->guard));
		/* calculate number of jnodes on fq' list of prepped jnodes */
		list_for_each(pos, ATOM_FQ_LIST(fq))
			count++;
		spin_unlock(&(fq->guard));
	}
	if (count != atom->fq)
		warning("", "fq counter %d, real %d\n", atom->fq, count);

}

#endif

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 79
 * scroll-step: 1
 * End:
 */
