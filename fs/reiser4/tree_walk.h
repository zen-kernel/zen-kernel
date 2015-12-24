/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* definitions of reiser4 tree walk functions */

#ifndef __FS_REISER4_TREE_WALK_H__
#define __FS_REISER4_TREE_WALK_H__

#include "debug.h"
#include "forward.h"

/* establishes horizontal links between cached znodes */
int connect_znode(coord_t * coord, znode * node);

/* tree traversal functions (reiser4_get_parent(), reiser4_get_neighbor())
  have the following common arguments:

  return codes:

  @return : 0        - OK,

ZAM-FIXME-HANS: wrong return code name.  Change them all.
	    -ENOENT  - neighbor is not in cache, what is detected by sibling
	               link absence.

            -E_NO_NEIGHBOR - we are sure that neighbor (or parent) node cannot be
                       found (because we are left-/right- most node of the
		       tree, for example). Also, this return code is for
		       reiser4_get_parent() when we see no parent link -- it
		       means that our node is root node.

            -E_DEADLOCK - deadlock detected (request from high-priority process
	               received), other error codes are conformed to
		       /usr/include/asm/errno.h .
*/

int
reiser4_get_parent_flags(lock_handle * result, znode * node,
			 znode_lock_mode mode, int flags);

/* bits definition for reiser4_get_neighbor function `flags' arg. */
typedef enum {
	/* If sibling pointer is NULL, this flag allows get_neighbor() to try to
	 * find not allocated not connected neigbor by going though upper
	 * levels */
	GN_CAN_USE_UPPER_LEVELS = 0x1,
	/* locking left neighbor instead of right one */
	GN_GO_LEFT = 0x2,
	/* automatically load neighbor node content */
	GN_LOAD_NEIGHBOR = 0x4,
	/* return -E_REPEAT if can't lock  */
	GN_TRY_LOCK = 0x8,
	/* used internally in tree_walk.c, causes renew_sibling to not
	   allocate neighbor znode, but only search for it in znode cache */
	GN_NO_ALLOC = 0x10,
	/* do not go across atom boundaries */
	GN_SAME_ATOM = 0x20,
	/* allow to lock not connected nodes */
	GN_ALLOW_NOT_CONNECTED = 0x40,
	/*  Avoid synchronous jload, instead, call jstartio() and return -E_REPEAT. */
	GN_ASYNC = 0x80
} znode_get_neigbor_flags;

/* A commonly used wrapper for reiser4_get_parent_flags(). */
static inline int reiser4_get_parent(lock_handle * result, znode * node,
				     znode_lock_mode mode)
{
	return reiser4_get_parent_flags(result, node, mode,
					GN_ALLOW_NOT_CONNECTED);
}

int reiser4_get_neighbor(lock_handle * neighbor, znode * node,
			 znode_lock_mode lock_mode, int flags);

/* there are wrappers for most common usages of reiser4_get_neighbor() */
static inline int
reiser4_get_left_neighbor(lock_handle * result, znode * node, int lock_mode,
			  int flags)
{
	return reiser4_get_neighbor(result, node, lock_mode,
				    flags | GN_GO_LEFT);
}

static inline int
reiser4_get_right_neighbor(lock_handle * result, znode * node, int lock_mode,
			   int flags)
{
	ON_DEBUG(check_lock_node_data(node));
	ON_DEBUG(check_lock_data());
	return reiser4_get_neighbor(result, node, lock_mode,
				    flags & (~GN_GO_LEFT));
}

extern void sibling_list_remove(znode * node);
extern void sibling_list_drop(znode * node);
extern void sibling_list_insert_nolock(znode * new, znode * before);
extern void link_left_and_right(znode * left, znode * right);

/* Functions called by tree_walk() when tree_walk() ...  */
struct tree_walk_actor {
	/* ... meets a formatted node, */
	int (*process_znode) (tap_t *, void *);
	/* ... meets an extent, */
	int (*process_extent) (tap_t *, void *);
	/* ... begins tree traversal or repeats it after -E_REPEAT was returned by
	 * node or extent processing functions. */
	int (*before) (void *);
};

#if REISER4_DEBUG
int check_sibling_list(znode * node);
#else
#define check_sibling_list(n) (1)
#endif

#endif				/* __FS_REISER4_TREE_WALK_H__ */

/*
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
