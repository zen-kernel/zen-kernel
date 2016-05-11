/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Transaction manager daemon. See ktxnmgrd.c for comments. */

#ifndef __KTXNMGRD_H__
#define __KTXNMGRD_H__

#include "txnmgr.h"

#include <linux/fs.h>
#include <linux/wait.h>
#include <linux/completion.h>
#include <linux/spinlock.h>
#include <asm/atomic.h>
#include <linux/sched.h>	/* for struct task_struct */

/* in this structure all data necessary to start up, shut down and communicate
 * with ktxnmgrd are kept. */
struct ktxnmgrd_context {
	/* wait queue head on which ktxnmgrd sleeps */
	wait_queue_head_t wait;
	/* spin lock protecting all fields of this structure */
	spinlock_t guard;
	/* timeout of sleeping on ->wait */
	signed long timeout;
	/* kernel thread running ktxnmgrd */
	struct task_struct *tsk;
	/* list of all file systems served by this ktxnmgrd */
	struct list_head queue;
	/* should ktxnmgrd repeat scanning of atoms? */
	unsigned int rescan:1;
};

extern int reiser4_init_ktxnmgrd(struct super_block *);
extern void reiser4_done_ktxnmgrd(struct super_block *);

extern void ktxnmgrd_kick(txn_mgr * mgr);
extern int is_current_ktxnmgrd(void);

/* __KTXNMGRD_H__ */
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
