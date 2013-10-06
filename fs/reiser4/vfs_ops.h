/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* vfs_ops.c's exported symbols */

#if !defined( __FS_REISER4_VFS_OPS_H__ )
#define __FS_REISER4_VFS_OPS_H__

#include "forward.h"
#include "coord.h"
#include "seal.h"
#include "plugin/file/file.h"
#include "super.h"
#include "readahead.h"

#include <linux/types.h>	/* for loff_t */
#include <linux/fs.h>		/* for struct address_space */
#include <linux/dcache.h>	/* for struct dentry */
#include <linux/mm.h>
#include <linux/backing-dev.h>

/* address space operations */
int reiser4_writepage(struct page *, struct writeback_control *);
int reiser4_set_page_dirty(struct page *);
void reiser4_invalidatepage(struct page *, unsigned int offset, unsigned int length);
int reiser4_releasepage(struct page *, gfp_t);

#ifdef CONFIG_MIGRATION
int reiser4_migratepage(struct address_space *, struct page *,
			struct page *, enum migrate_mode);
#else
#define reiser4_migratepage NULL
#endif /* CONFIG_MIGRATION */

extern int reiser4_update_sd(struct inode *);
extern int reiser4_add_nlink(struct inode *, struct inode *, int);
extern int reiser4_del_nlink(struct inode *, struct inode *, int);

extern int reiser4_start_up_io(struct page *page);
extern void reiser4_throttle_write(struct inode *);
extern int jnode_is_releasable(jnode *);

#define CAPTURE_APAGE_BURST (1024l)
void reiser4_writeout(struct super_block *, struct writeback_control *);

extern void reiser4_handle_error(void);

/* __FS_REISER4_VFS_OPS_H__ */
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
