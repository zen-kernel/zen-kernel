/* Copyright 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Safe-links. See safe_link.c for details. */

#if !defined(__FS_SAFE_LINK_H__)
#define __FS_SAFE_LINK_H__

#include "tree.h"

int safe_link_grab(reiser4_tree * tree, reiser4_ba_flags_t flags);
void safe_link_release(reiser4_tree * tree);
int safe_link_add(struct inode *inode, reiser4_safe_link_t link);
int safe_link_del(reiser4_tree *, oid_t oid, reiser4_safe_link_t link);

int process_safelinks(struct super_block *super);

/* __FS_SAFE_LINK_H__ */
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
