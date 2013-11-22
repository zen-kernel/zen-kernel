/* Copyright 2001, 2002, 2003, 2004 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Reiser4 context. See context.c for details. */

#if !defined( __REISER4_CONTEXT_H__ )
#define __REISER4_CONTEXT_H__

#include "forward.h"
#include "debug.h"
#include "dformat.h"
#include "tap.h"
#include "lock.h"

#include <linux/types.h>	/* for __u??  */
#include <linux/fs.h>		/* for struct super_block  */
#include <linux/spinlock.h>
#include <linux/sched.h>	/* for struct task_struct */

/* reiser4 per-thread context */
struct reiser4_context {
	/* magic constant. For identification of reiser4 contexts. */
	__u32 magic;

	/* current lock stack. See lock.[ch]. This is where list of all
	   locks taken by current thread is kept. This is also used in
	   deadlock detection. */
	lock_stack stack;

	/* current transcrash. */
	txn_handle *trans;
	/* transaction handle embedded into reiser4_context. ->trans points
	 * here by default. */
	txn_handle trans_in_ctx;

	/* super block we are working with.  To get the current tree
	   use &get_super_private (reiser4_get_current_sb ())->tree. */
	struct super_block *super;

	/* parent fs activation */
	struct fs_activation *outer;

	/* per-thread grabbed (for further allocation) blocks counter */
	reiser4_block_nr grabbed_blocks;

	/* list of taps currently monitored. See tap.c */
	struct list_head taps;

	/* grabbing space is enabled */
	unsigned int grab_enabled:1;
	/* should be set when we are write dirty nodes to disk in jnode_flush or
	 * reiser4_write_logs() */
	unsigned int writeout_mode:1;
	/* true, if current thread is an ent thread */
	unsigned int entd:1;
	/* true, if balance_dirty_pages() should not be run when leaving this
	 * context. This is used to avoid lengthly balance_dirty_pages()
	 * operation when holding some important resource, like directory
	 * ->i_mutex */
	unsigned int nobalance:1;

	/* this bit is used on reiser4_done_context to decide whether context is
	   kmalloc-ed and has to be kfree-ed */
	unsigned int on_stack:1;

	/* count non-trivial jnode_set_dirty() calls */
	unsigned long nr_marked_dirty;
	/*
	 * reiser4_writeback_inodes calls (via generic_writeback_sb_inodes)
	 * reiser4_writepages_dispatch for each of dirty inodes.
	 * Reiser4_writepages_dispatch captures pages. When number of pages
	 * captured in one reiser4_writeback_inodes reaches some threshold -
	 * some atoms get flushed
	 */
	int nr_captured;
	int nr_children;	/* number of child contexts */
	struct page *locked_page; /* page that should be unlocked in
				   * reiser4_dirty_inode() before taking
				   * a longterm lock (to not violate
				   * reiser4 lock ordering) */
#if REISER4_DEBUG
	/* debugging information about reiser4 locks held by the current
	 * thread */
	reiser4_lock_cnt_info locks;
	struct task_struct *task;	/* so we can easily find owner of the stack */

	/*
	 * disk space grabbing debugging support
	 */
	/* how many disk blocks were grabbed by the first call to
	 * reiser4_grab_space() in this context */
	reiser4_block_nr grabbed_initially;

	/* list of all threads doing flush currently */
	struct list_head flushers_link;
	/* information about last error encountered by reiser4 */
	err_site err;
#endif
	void *vp;
	gfp_t gfp_mask;
};

extern reiser4_context *get_context_by_lock_stack(lock_stack *);

/* Debugging helps. */
#if REISER4_DEBUG
extern void print_contexts(void);
#endif

#define current_tree (&(get_super_private(reiser4_get_current_sb())->tree))
#define current_blocksize reiser4_get_current_sb()->s_blocksize
#define current_blocksize_bits reiser4_get_current_sb()->s_blocksize_bits

extern reiser4_context *reiser4_init_context(struct super_block *);
extern void init_stack_context(reiser4_context *, struct super_block *);
extern void reiser4_exit_context(reiser4_context *);

/* magic constant we store in reiser4_context allocated at the stack. Used to
   catch accesses to staled or uninitialized contexts. */
#define context_magic ((__u32) 0x4b1b5d0b)

extern int is_in_reiser4_context(void);

/*
 * return reiser4_context for the thread @tsk
 */
static inline reiser4_context *get_context(const struct task_struct *tsk)
{
	assert("vs-1682",
	       ((reiser4_context *) tsk->journal_info)->magic == context_magic);
	return (reiser4_context *) tsk->journal_info;
}

/*
 * return reiser4 context of the current thread, or NULL if there is none.
 */
static inline reiser4_context *get_current_context_check(void)
{
	if (is_in_reiser4_context())
		return get_context(current);
	else
		return NULL;
}

static inline reiser4_context *get_current_context(void);	/* __attribute__((const)); */

/* return context associated with current thread */
static inline reiser4_context *get_current_context(void)
{
	return get_context(current);
}

static inline gfp_t reiser4_ctx_gfp_mask_get(void)
{
	reiser4_context *ctx;

	ctx = get_current_context_check();
	return (ctx == NULL) ? GFP_KERNEL : ctx->gfp_mask;
}

void reiser4_ctx_gfp_mask_set(void);
void reiser4_ctx_gfp_mask_force (gfp_t mask);

/*
 * true if current thread is in the write-out mode. Thread enters write-out
 * mode during jnode_flush and reiser4_write_logs().
 */
static inline int is_writeout_mode(void)
{
	return get_current_context()->writeout_mode;
}

/*
 * enter write-out mode
 */
static inline void writeout_mode_enable(void)
{
	assert("zam-941", !get_current_context()->writeout_mode);
	get_current_context()->writeout_mode = 1;
}

/*
 * leave write-out mode
 */
static inline void writeout_mode_disable(void)
{
	assert("zam-942", get_current_context()->writeout_mode);
	get_current_context()->writeout_mode = 0;
}

static inline void grab_space_enable(void)
{
	get_current_context()->grab_enabled = 1;
}

static inline void grab_space_disable(void)
{
	get_current_context()->grab_enabled = 0;
}

static inline void grab_space_set_enabled(int enabled)
{
	get_current_context()->grab_enabled = enabled;
}

static inline int is_grab_enabled(reiser4_context * ctx)
{
	return ctx->grab_enabled;
}

/* mark transaction handle in @ctx as TXNH_DONT_COMMIT, so that no commit or
 * flush would be performed when it is closed. This is necessary when handle
 * has to be closed under some coarse semaphore, like i_mutex of
 * directory. Commit will be performed by ktxnmgrd. */
static inline void context_set_commit_async(reiser4_context * context)
{
	context->nobalance = 1;
	context->trans->flags |= TXNH_DONT_COMMIT;
}

/* __REISER4_CONTEXT_H__ */
#endif

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   scroll-step: 1
   End:
*/
