/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */
/* Transaction manager daemon. */

/*
 * ktxnmgrd is a kernel daemon responsible for committing transactions. It is
 * needed/important for the following reasons:
 *
 *     1. in reiser4 atom is not committed immediately when last transaction
 *     handle closes, unless atom is either too old or too large (see
 *     atom_should_commit()). This is done to avoid committing too frequently.
 *     because:
 *
 *     2. sometimes we don't want to commit atom when closing last transaction
 *     handle even if it is old and fat enough. For example, because we are at
 *     this point under directory semaphore, and committing would stall all
 *     accesses to this directory.
 *
 * ktxnmgrd binds its time sleeping on condition variable. When is awakes
 * either due to (tunable) timeout or because it was explicitly woken up by
 * call to ktxnmgrd_kick(), it scans list of all atoms and commits ones
 * eligible.
 *
 */

#include "debug.h"
#include "txnmgr.h"
#include "tree.h"
#include "ktxnmgrd.h"
#include "super.h"
#include "reiser4.h"

#include <linux/sched.h>	/* for struct task_struct */
#include <linux/wait.h>
#include <linux/suspend.h>
#include <linux/kernel.h>
#include <linux/writeback.h>
#include <linux/kthread.h>
#include <linux/freezer.h>

static int scan_mgr(struct super_block *);

/*
 * change current->comm so that ps, top, and friends will see changed
 * state. This serves no useful purpose whatsoever, but also costs nothing. May
 * be it will make lonely system administrator feeling less alone at 3 A.M.
 */
#define set_comm(state) 						\
	snprintf(current->comm, sizeof(current->comm),			\
		  "%s:%s:%s", __FUNCTION__, (super)->s_id, (state))

/**
 * ktxnmgrd - kernel txnmgr daemon
 * @arg: pointer to super block
 *
 * The background transaction manager daemon, started as a kernel thread during
 * reiser4 initialization.
 */
static int ktxnmgrd(void *arg)
{
	struct super_block *super;
	ktxnmgrd_context *ctx;
	txn_mgr *mgr;
	int done = 0;

	super = arg;
	mgr = &get_super_private(super)->tmgr;

	/*
	 * do_fork() just copies task_struct into the new thread. ->fs_context
	 * shouldn't be copied of course. This shouldn't be a problem for the
	 * rest of the code though.
	 */
	current->journal_info = NULL;
	ctx = mgr->daemon;
	while (1) {
		try_to_freeze();
		set_comm("wait");
		{
			DEFINE_WAIT(__wait);

			prepare_to_wait(&ctx->wait, &__wait,
					TASK_INTERRUPTIBLE);
			if (kthread_should_stop())
				done = 1;
			else
				schedule_timeout(ctx->timeout);
			finish_wait(&ctx->wait, &__wait);
		}
		if (done)
			break;
		set_comm("run");
		spin_lock(&ctx->guard);
		/*
		 * wait timed out or ktxnmgrd was woken up by explicit request
		 * to commit something. Scan list of atoms in txnmgr and look
		 * for too old atoms.
		 */
		do {
			ctx->rescan = 0;
			scan_mgr(super);
			spin_lock(&ctx->guard);
			if (ctx->rescan) {
				/*
				 * the list could be modified while ctx
				 * spinlock was released, we have to repeat
				 * scanning from the beginning
				 */
				break;
			}
		} while (ctx->rescan);
		spin_unlock(&ctx->guard);
	}
	return 0;
}

#undef set_comm

/**
 * reiser4_init_ktxnmgrd - initialize ktxnmgrd context and start kernel daemon
 * @super: pointer to super block
 *
 * Allocates and initializes ktxnmgrd_context, attaches it to transaction
 * manager. Starts kernel txnmgr daemon. This is called on mount.
 */
int reiser4_init_ktxnmgrd(struct super_block *super)
{
	txn_mgr *mgr;
	ktxnmgrd_context *ctx;

	mgr = &get_super_private(super)->tmgr;

	assert("zam-1014", mgr->daemon == NULL);

	ctx = kzalloc(sizeof(ktxnmgrd_context), reiser4_ctx_gfp_mask_get());
	if (!ctx)
		return RETERR(-ENOMEM);

	assert("nikita-2442", ctx != NULL);

	init_waitqueue_head(&ctx->wait);

	/*kcond_init(&ctx->startup);*/
	spin_lock_init(&ctx->guard);
	ctx->timeout = REISER4_TXNMGR_TIMEOUT;
	ctx->rescan = 1;
	mgr->daemon = ctx;

	ctx->tsk = kthread_run(ktxnmgrd, super, "ktxnmgrd");
	if (IS_ERR(ctx->tsk)) {
		int ret = PTR_ERR(ctx->tsk);
		mgr->daemon = NULL;
		kfree(ctx);
		return RETERR(ret);
	}
	return 0;
}

void ktxnmgrd_kick(txn_mgr *mgr)
{
	assert("nikita-3234", mgr != NULL);
	assert("nikita-3235", mgr->daemon != NULL);
	wake_up(&mgr->daemon->wait);
}

int is_current_ktxnmgrd(void)
{
	return (get_current_super_private()->tmgr.daemon->tsk == current);
}

/**
 * scan_mgr - commit atoms which are to be committed
 * @super: super block to commit atoms of
 *
 * Commits old atoms.
 */
static int scan_mgr(struct super_block *super)
{
	int ret;
	reiser4_context ctx;

	init_stack_context(&ctx, super);

	ret = commit_some_atoms(&get_super_private(super)->tmgr);

	reiser4_exit_context(&ctx);
	return ret;
}

/**
 * reiser4_done_ktxnmgrd - stop kernel thread and frees ktxnmgrd context
 * @mgr:
 *
 * This is called on umount. Stops ktxnmgrd and free t
 */
void reiser4_done_ktxnmgrd(struct super_block *super)
{
	txn_mgr *mgr;

	mgr = &get_super_private(super)->tmgr;
	assert("zam-1012", mgr->daemon != NULL);

	kthread_stop(mgr->daemon->tsk);
	kfree(mgr->daemon);
	mgr->daemon = NULL;
}

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 120
 * End:
 */
