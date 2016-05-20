/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Functions to add/delete new nodes to/from the tree. See tree_mod.c for
 * comments. */

#if !defined( __REISER4_TREE_MOD_H__ )
#define __REISER4_TREE_MOD_H__

#include "forward.h"

znode *reiser4_new_node(znode * brother, tree_level level);
znode *reiser4_add_tree_root(znode * old_root, znode * fake);
int reiser4_kill_tree_root(znode * old_root);
void build_child_ptr_data(znode * child, reiser4_item_data * data);

/* __REISER4_TREE_MOD_H__ */
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
