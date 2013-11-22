/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
   reiser4/README */

/* Declarations of debug macros. */

#if !defined(__FS_REISER4_DEBUG_H__)
#define __FS_REISER4_DEBUG_H__

#include "forward.h"
#include "reiser4.h"

/* generic function to produce formatted output, decorating it with
   whatever standard prefixes/postfixes we want. "Fun" is a function
   that will be actually called, can be printk, panic etc.
   This is for use by other debugging macros, not by users. */
#define DCALL(lev, fun, reperr, label, format, ...)			\
({									\
	fun(lev "reiser4[%.16s(%i)]: %s (%s:%i)[%s]:\n" format "\n" ,	\
	    current->comm, current->pid, __FUNCTION__,			\
	    __FILE__, __LINE__, label, ## __VA_ARGS__);			\
})

/*
 * cause kernel to crash
 */
#define reiser4_panic(mid, format, ...)				\
	DCALL("", reiser4_do_panic, 1, mid, format , ## __VA_ARGS__)

/* print message with indication of current process, file, line and
   function */
#define reiser4_log(label, format, ...) 				\
	DCALL(KERN_DEBUG, printk, 0, label, format , ## __VA_ARGS__)

/* Assertion checked during compilation.
    If "cond" is false (0) we get duplicate case label in switch.
    Use this to check something like famous
       cassert (sizeof(struct reiserfs_journal_commit) == 4096) ;
    in 3.x journal.c. If cassertion fails you get compiler error,
    so no "maintainer-id".
*/
#define cassert(cond) ({ switch (-1) { case (cond): case 0: break; } })

#define noop   do {; } while (0)

#if REISER4_DEBUG
/* version of info that only actually prints anything when _d_ebugging
    is on */
#define dinfo(format, ...) printk(format , ## __VA_ARGS__)
/* macro to catch logical errors. Put it into `default' clause of
    switch() statement. */
#define impossible(label, format, ...) 			\
	reiser4_panic(label, "impossible: " format , ## __VA_ARGS__)
/* assert assures that @cond is true. If it is not, reiser4_panic() is
   called. Use this for checking logical consistency and _never_ call
   this to check correctness of external data: disk blocks and user-input . */
#define assert(label, cond)						\
({									\
	/* call_on_each_assert(); */					\
	if (cond) {							\
		/* put negated check to avoid using !(cond) that would lose \
		 * warnings for things like assert(a = b); */		\
		;							\
	} else {							\
		DEBUGON(1);						\
		reiser4_panic(label, "assertion failed: %s", #cond);	\
	}								\
})

/* like assertion, but @expr is evaluated even if REISER4_DEBUG is off. */
#define check_me(label, expr)	assert(label, (expr))

#define ON_DEBUG(exp) exp

extern int reiser4_schedulable(void);
extern void call_on_each_assert(void);

#else

#define dinfo(format, args...) noop
#define impossible(label, format, args...) noop
#define assert(label, cond) noop
#define check_me(label, expr)	((void) (expr))
#define ON_DEBUG(exp)
#define reiser4_schedulable() might_sleep()

/* REISER4_DEBUG */
#endif

#if REISER4_DEBUG
/* per-thread information about lock acquired by this thread. Used by lock
 * ordering checking in spin_macros.h */
typedef struct reiser4_lock_cnt_info {
	int rw_locked_tree;
	int read_locked_tree;
	int write_locked_tree;

	int rw_locked_dk;
	int read_locked_dk;
	int write_locked_dk;

	int rw_locked_cbk_cache;
	int read_locked_cbk_cache;
	int write_locked_cbk_cache;

	int spin_locked_zlock;
	int spin_locked_jnode;
	int spin_locked_jload;
	int spin_locked_txnh;
	int spin_locked_atom;
	int spin_locked_stack;
	int spin_locked_txnmgr;
	int spin_locked_ktxnmgrd;
	int spin_locked_fq;
	int spin_locked_inode;
	int spin_locked_super_eflush;
	int spin_locked;
	int long_term_locked_znode;

	int inode_sem_r;
	int inode_sem_w;

	int d_refs;
	int x_refs;
	int t_refs;
} reiser4_lock_cnt_info;

extern struct reiser4_lock_cnt_info *reiser4_lock_counters(void);
#define IN_CONTEXT(a, b) (is_in_reiser4_context() ? (a) : (b))

/* increment lock-counter @counter, if present */
#define LOCK_CNT_INC(counter)					\
	IN_CONTEXT(++(reiser4_lock_counters()->counter), 0)

/* decrement lock-counter @counter, if present */
#define LOCK_CNT_DEC(counter)					\
	IN_CONTEXT(--(reiser4_lock_counters()->counter), 0)

/* check that lock-counter is zero. This is for use in assertions */
#define LOCK_CNT_NIL(counter)					\
	IN_CONTEXT(reiser4_lock_counters()->counter == 0, 1)

/* check that lock-counter is greater than zero. This is for use in
 * assertions */
#define LOCK_CNT_GTZ(counter)					\
	IN_CONTEXT(reiser4_lock_counters()->counter > 0, 1)
#define LOCK_CNT_LT(counter,n)					\
	IN_CONTEXT(reiser4_lock_counters()->counter < n, 1)

#else				/* REISER4_DEBUG */

/* no-op versions on the above */

typedef struct reiser4_lock_cnt_info {
} reiser4_lock_cnt_info;

#define reiser4_lock_counters() ((reiser4_lock_cnt_info *)NULL)
#define LOCK_CNT_INC(counter) noop
#define LOCK_CNT_DEC(counter) noop
#define LOCK_CNT_NIL(counter) (1)
#define LOCK_CNT_GTZ(counter) (1)
#define LOCK_CNT_LT(counter, n) (1)

#endif				/* REISER4_DEBUG */

#define assert_spin_not_locked(lock) BUG_ON(0)
#define assert_rw_write_locked(lock) BUG_ON(0)
#define assert_rw_read_locked(lock) BUG_ON(0)
#define assert_rw_locked(lock) BUG_ON(0)
#define assert_rw_not_write_locked(lock) BUG_ON(0)
#define assert_rw_not_read_locked(lock) BUG_ON(0)
#define assert_rw_not_locked(lock) BUG_ON(0)

/* flags controlling debugging behavior. Are set through debug_flags=N mount
   option. */
typedef enum {
	/* print a lot of information during panic. When this is on all jnodes
	 * are listed. This can be *very* large output. Usually you don't want
	 * this. Especially over serial line. */
	REISER4_VERBOSE_PANIC = 0x00000001,
	/* print a lot of information during umount */
	REISER4_VERBOSE_UMOUNT = 0x00000002,
	/* print gathered statistics on umount */
	REISER4_STATS_ON_UMOUNT = 0x00000004,
	/* check node consistency */
	REISER4_CHECK_NODE = 0x00000008
} reiser4_debug_flags;

extern int is_in_reiser4_context(void);

/*
 * evaluate expression @e only if with reiser4 context
 */
#define ON_CONTEXT(e)	do {			\
	if (is_in_reiser4_context()) {		\
		e;				\
	} } while (0)

/*
 * evaluate expression @e only when within reiser4_context and debugging is
 * on.
 */
#define ON_DEBUG_CONTEXT(e) ON_DEBUG(ON_CONTEXT(e))

/*
 * complain about unexpected function result and crash. Used in "default"
 * branches of switch statements and alike to assert that invalid results are
 * not silently ignored.
 */
#define wrong_return_value(label, function)				\
	impossible(label, "wrong return value from " function)

/* Issue different types of reiser4 messages to the console */
#define warning(label, format, ...)					\
	DCALL(KERN_WARNING, 						\
	       printk, 1, label, "WARNING: " format , ## __VA_ARGS__)
#define notice(label, format, ...)					\
	DCALL(KERN_NOTICE, 						\
	       printk, 1, label, "NOTICE: " format , ## __VA_ARGS__)

/* mark not yet implemented functionality */
#define not_yet(label, format, ...)				\
	reiser4_panic(label, "NOT YET IMPLEMENTED: " format , ## __VA_ARGS__)

extern void reiser4_do_panic(const char *format, ...)
    __attribute__ ((noreturn, format(printf, 1, 2)));

extern int reiser4_preempt_point(void);
extern void reiser4_print_stats(void);

#if REISER4_DEBUG
extern int reiser4_no_counters_are_held(void);
extern int reiser4_commit_check_locks(void);
#else
#define reiser4_no_counters_are_held() (1)
#define reiser4_commit_check_locks() (1)
#endif

/* true if @i is power-of-two. Useful for rate-limited warnings, etc. */
#define IS_POW(i) 				\
({						\
	typeof(i) __i;				\
						\
	__i = (i);				\
	!(__i & (__i - 1));			\
})

#define KERNEL_DEBUGGER (1)

#if KERNEL_DEBUGGER

extern void reiser4_debugtrap(void);

/*
 * Check condition @cond and drop into kernel debugger (kgdb) if it's true. If
 * kgdb is not compiled in, do nothing.
 */
#define DEBUGON(cond)					\
({	               					\
	if (unlikely(cond))				\
		reiser4_debugtrap();			\
})
#else
#define DEBUGON(cond) noop
#endif

/*
 * Error code tracing facility. (Idea is borrowed from XFS code.)
 *
 * Suppose some strange and/or unexpected code is returned from some function
 * (for example, write(2) returns -EEXIST). It is possible to place a
 * breakpoint in the reiser4_write(), but it is too late here. How to find out
 * in what particular place -EEXIST was generated first?
 *
 * In reiser4 all places where actual error codes are produced (that is,
 * statements of the form
 *
 *     return -EFOO;        // (1), or
 *
 *     result = -EFOO;      // (2)
 *
 * are replaced with
 *
 *     return RETERR(-EFOO);        // (1a), and
 *
 *     result = RETERR(-EFOO);      // (2a) respectively
 *
 * RETERR() macro fills a backtrace in reiser4_context. This back-trace is
 * printed in error and warning messages. Moreover, it's possible to put a
 * conditional breakpoint in reiser4_return_err (low-level function called
 * by RETERR() to do the actual work) to break into debugger immediately
 * when particular error happens.
 *
 */

#if REISER4_DEBUG

/*
 * data-type to store information about where error happened ("error site").
 */
typedef struct err_site {
	int code;		/* error code */
	const char *file;	/* source file, filled by __FILE__ */
	int line;		/* source file line, filled by __LINE__ */
} err_site;

extern void reiser4_return_err(int code, const char *file, int line);

/*
 * fill &get_current_context()->err_site with error information.
 */
#define RETERR(code)					\
({						        \
	typeof(code) __code;				\
							\
	__code = (code);				\
	reiser4_return_err(__code, __FILE__, __LINE__);	\
	__code;						\
})

#else

/*
 * no-op versions of the above
 */

typedef struct err_site {
} err_site;
#define RETERR(code) code
#endif

#if REISER4_LARGE_KEY
/*
 * conditionally compile arguments only if REISER4_LARGE_KEY is on.
 */
#define ON_LARGE_KEY(...) __VA_ARGS__
#else
#define ON_LARGE_KEY(...)
#endif

/* __FS_REISER4_DEBUG_H__ */
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
