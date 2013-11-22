/* Copyright 2003 by Hans Reiser, licensing governed by reiser4/README */

/* Ent daemon. */

#ifndef __ENTD_H__
#define __ENTD_H__

#include "context.h"

#include <linux/fs.h>
#include <linux/completion.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/sched.h>	/* for struct task_struct */

#define WBQ_MAGIC 0x7876dc76

/* write-back request. */
struct wbq {
	int magic;
	struct list_head link; /* list head of this list is in entd context */
	struct writeback_control *wbc;
	struct page *page;
	struct address_space *mapping;
	struct completion completion;
	jnode *node; /* set if ent thread captured requested page */
	int written; /* set if ent thread wrote requested page */
};

/* ent-thread context. This is used to synchronize starting/stopping ent
 * threads. */
typedef struct entd_context {
	 /* wait queue that ent thread waits on for more work. It's
	  * signaled by write_page_by_ent(). */
	wait_queue_head_t wait;
	/* spinlock protecting other fields */
	spinlock_t guard;
	/* ent thread */
	struct task_struct *tsk;
	/* set to indicate that ent thread should leave. */
	int done;
	/* counter of active flushers */
	int flushers;
	/*
	 * when reiser4_writepage asks entd to write a page - it adds struct
	 * wbq to this list
	 */
	struct list_head todo_list;
	/* number of elements on the above list */
	int nr_todo_reqs;

	struct wbq *cur_request;
	/*
	 * when entd writes a page it moves write-back request from todo_list
	 * to done_list. This list is used at the end of entd iteration to
	 * wakeup requestors and iput inodes.
	 */
	struct list_head done_list;
	/* number of elements on the above list */
	int nr_done_reqs;

#if REISER4_DEBUG
	/* list of all active flushers */
	struct list_head flushers_list;
#endif
} entd_context;

extern int  reiser4_init_entd(struct super_block *);
extern void reiser4_done_entd(struct super_block *);

extern void reiser4_enter_flush(struct super_block *);
extern void reiser4_leave_flush(struct super_block *);

extern int write_page_by_ent(struct page *, struct writeback_control *);
extern int wbq_available(void);
extern void ent_writes_page(struct super_block *, struct page *);

extern jnode *get_jnode_by_wbq(struct super_block *, struct wbq *);
/* __ENTD_H__ */
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
