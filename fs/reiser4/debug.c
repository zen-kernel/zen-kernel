/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Debugging facilities. */

/*
 * This file contains generic debugging functions used by reiser4. Roughly
 * following:
 *
 *     panicking: reiser4_do_panic(), reiser4_print_prefix().
 *
 *     locking:
 *     reiser4_schedulable(), reiser4_lock_counters(), print_lock_counters(),
 *     reiser4_no_counters_are_held(), reiser4_commit_check_locks()
 *
 *     error code monitoring (see comment before RETERR macro):
 *     reiser4_return_err(), reiser4_report_err().
 *
 *     stack back-tracing: fill_backtrace()
 *
 *     miscellaneous: reiser4_preempt_point(), call_on_each_assert(),
 *     reiser4_debugtrap().
 *
 */

#include "reiser4.h"
#include "context.h"
#include "super.h"
#include "txnmgr.h"
#include "znode.h"

#include <linux/sysfs.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/fs.h>
#include <linux/spinlock.h>
#include <linux/kallsyms.h>
#include <linux/vmalloc.h>
#include <linux/ctype.h>
#include <linux/sysctl.h>
#include <linux/hardirq.h>

#if 0
#if REISER4_DEBUG
static void reiser4_report_err(void);
#else
#define reiser4_report_err() noop
#endif
#endif  /*  0  */

/*
 * global buffer where message given to reiser4_panic is formatted.
 */
static char panic_buf[REISER4_PANIC_MSG_BUFFER_SIZE];

/*
 * lock protecting consistency of panic_buf under concurrent panics
 */
static DEFINE_SPINLOCK(panic_guard);

/* Your best friend. Call it on each occasion.  This is called by
    fs/reiser4/debug.h:reiser4_panic(). */
void reiser4_do_panic(const char *format/* format string */ , ... /* rest */)
{
	static int in_panic = 0;
	va_list args;

	/*
	 * check for recursive panic.
	 */
	if (in_panic == 0) {
		in_panic = 1;

		spin_lock(&panic_guard);
		va_start(args, format);
		vsnprintf(panic_buf, sizeof(panic_buf), format, args);
		va_end(args);
		printk(KERN_EMERG "reiser4 panicked cowardly: %s", panic_buf);
		spin_unlock(&panic_guard);

		/*
		 * if kernel debugger is configured---drop in. Early dropping
		 * into kgdb is not always convenient, because panic message
		 * is not yet printed most of the times. But:
		 *
		 *     (1) message can be extracted from printk_buf[]
		 *     (declared static inside of printk()), and
		 *
		 *     (2) sometimes serial/kgdb combo dies while printing
		 *     long panic message, so it's more prudent to break into
		 *     debugger earlier.
		 *
		 */
		DEBUGON(1);
	}
	/* to make gcc happy about noreturn attribute */
	panic("%s", panic_buf);
}

#if 0
void
reiser4_print_prefix(const char *level, int reperr, const char *mid,
		     const char *function, const char *file, int lineno)
{
	const char *comm;
	int pid;

	if (unlikely(in_interrupt() || in_irq())) {
		comm = "interrupt";
		pid = 0;
	} else {
		comm = current->comm;
		pid = current->pid;
	}
	printk("%sreiser4[%.16s(%i)]: %s (%s:%i)[%s]:\n",
	       level, comm, pid, function, file, lineno, mid);
	if (reperr)
		reiser4_report_err();
}
#endif  /*  0  */

/* Preemption point: this should be called periodically during long running
   operations (carry, allocate, and squeeze are best examples) */
int reiser4_preempt_point(void)
{
	assert("nikita-3008", reiser4_schedulable());
	cond_resched();
	return signal_pending(current);
}

#if REISER4_DEBUG
/* Debugging aid: return struct where information about locks taken by current
   thread is accumulated. This can be used to formulate lock ordering
   constraints and various assertions.

*/
reiser4_lock_cnt_info *reiser4_lock_counters(void)
{
	reiser4_context *ctx = get_current_context();
	assert("jmacd-1123", ctx != NULL);
	return &ctx->locks;
}

/*
 * print human readable information about locks held by the reiser4 context.
 */
static void print_lock_counters(const char *prefix,
				const reiser4_lock_cnt_info * info)
{
	printk("%s: jnode: %i, tree: %i (r:%i,w:%i), dk: %i (r:%i,w:%i)\n"
	       "jload: %i, "
	       "txnh: %i, atom: %i, stack: %i, txnmgr: %i, "
	       "ktxnmgrd: %i, fq: %i\n"
	       "inode: %i, "
	       "cbk_cache: %i (r:%i,w%i), "
	       "eflush: %i, "
	       "zlock: %i,\n"
	       "spin: %i, long: %i inode_sem: (r:%i,w:%i)\n"
	       "d: %i, x: %i, t: %i\n", prefix,
	       info->spin_locked_jnode,
	       info->rw_locked_tree, info->read_locked_tree,
	       info->write_locked_tree,
	       info->rw_locked_dk, info->read_locked_dk, info->write_locked_dk,
	       info->spin_locked_jload,
	       info->spin_locked_txnh,
	       info->spin_locked_atom, info->spin_locked_stack,
	       info->spin_locked_txnmgr, info->spin_locked_ktxnmgrd,
	       info->spin_locked_fq,
	       info->spin_locked_inode,
	       info->rw_locked_cbk_cache,
	       info->read_locked_cbk_cache,
	       info->write_locked_cbk_cache,
	       info->spin_locked_super_eflush,
	       info->spin_locked_zlock,
	       info->spin_locked,
	       info->long_term_locked_znode,
	       info->inode_sem_r, info->inode_sem_w,
	       info->d_refs, info->x_refs, info->t_refs);
}

/* check that no spinlocks are held */
int reiser4_schedulable(void)
{
	if (get_current_context_check() != NULL) {
		if (!LOCK_CNT_NIL(spin_locked)) {
			print_lock_counters("in atomic", reiser4_lock_counters());
			return 0;
		}
	}
	might_sleep();
	return 1;
}
/*
 * return true, iff no locks are held.
 */
int reiser4_no_counters_are_held(void)
{
	reiser4_lock_cnt_info *counters;

	counters = reiser4_lock_counters();
	return
	    (counters->spin_locked_zlock == 0) &&
	    (counters->spin_locked_jnode == 0) &&
	    (counters->rw_locked_tree == 0) &&
	    (counters->read_locked_tree == 0) &&
	    (counters->write_locked_tree == 0) &&
	    (counters->rw_locked_dk == 0) &&
	    (counters->read_locked_dk == 0) &&
	    (counters->write_locked_dk == 0) &&
	    (counters->spin_locked_txnh == 0) &&
	    (counters->spin_locked_atom == 0) &&
	    (counters->spin_locked_stack == 0) &&
	    (counters->spin_locked_txnmgr == 0) &&
	    (counters->spin_locked_inode == 0) &&
	    (counters->spin_locked == 0) &&
	    (counters->long_term_locked_znode == 0) &&
	    (counters->inode_sem_r == 0) &&
	    (counters->inode_sem_w == 0) && (counters->d_refs == 0);
}

/*
 * return true, iff transaction commit can be done under locks held by the
 * current thread.
 */
int reiser4_commit_check_locks(void)
{
	reiser4_lock_cnt_info *counters;
	int inode_sem_r;
	int inode_sem_w;
	int result;

	/*
	 * inode's read/write semaphore is the only reiser4 lock that can be
	 * held during commit.
	 */

	counters = reiser4_lock_counters();
	inode_sem_r = counters->inode_sem_r;
	inode_sem_w = counters->inode_sem_w;

	counters->inode_sem_r = counters->inode_sem_w = 0;
	result = reiser4_no_counters_are_held();
	counters->inode_sem_r = inode_sem_r;
	counters->inode_sem_w = inode_sem_w;
	return result;
}

/*
 * fill "error site" in the current reiser4 context. See comment before RETERR
 * macro for more details.
 */
void reiser4_return_err(int code, const char *file, int line)
{
	if (code < 0 && is_in_reiser4_context()) {
		reiser4_context *ctx = get_current_context();

		if (ctx != NULL) {
			ctx->err.code = code;
			ctx->err.file = file;
			ctx->err.line = line;
		}
	}
}

#if 0
/*
 * report error information recorder by reiser4_return_err().
 */
static void reiser4_report_err(void)
{
	reiser4_context *ctx = get_current_context_check();

	if (ctx != NULL) {
		if (ctx->err.code != 0) {
			printk("code: %i at %s:%i\n",
			       ctx->err.code, ctx->err.file, ctx->err.line);
		}
	}
}
#endif  /*  0  */

#endif				/* REISER4_DEBUG */

#if KERNEL_DEBUGGER

/*
 * this functions just drops into kernel debugger. It is a convenient place to
 * put breakpoint in.
 */
void reiser4_debugtrap(void)
{
	/* do nothing. Put break point here. */
#if defined(CONFIG_KGDB) && !defined(CONFIG_REISER4_FS_MODULE)
	extern void kgdb_breakpoint(void);
	kgdb_breakpoint();
#endif
}
#endif

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
