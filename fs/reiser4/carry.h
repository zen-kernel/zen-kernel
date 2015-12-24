/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
   reiser4/README */

/* Functions and data types to "carry" tree modification(s) upward.
   See fs/reiser4/carry.c for details. */

#if !defined(__FS_REISER4_CARRY_H__)
#define __FS_REISER4_CARRY_H__

#include "forward.h"
#include "debug.h"
#include "pool.h"
#include "znode.h"

#include <linux/types.h>

/* &carry_node - "location" of carry node.

   "location" of node that is involved or going to be involved into
   carry process. Node where operation will be carried to on the
   parent level cannot be recorded explicitly. Operation will be carried
   usually to the parent of some node (where changes are performed at
   the current level) or, to the left neighbor of its parent. But while
   modifications are performed at the current level, parent may
   change. So, we have to allow some indirection (or, positevly,
   flexibility) in locating carry nodes.

*/
typedef struct carry_node {
	/* pool linkage */
	struct reiser4_pool_header header;

	/* base node from which real_node is calculated. See
	   fs/reiser4/carry.c:lock_carry_node(). */
	znode *node;

	/* how to get ->real_node */
	/* to get ->real_node obtain parent of ->node */
	__u32 parent:1;
	/* to get ->real_node obtain left neighbor of parent of
	   ->node */
	__u32 left:1;
	__u32 left_before:1;

	/* locking */

	/* this node was locked by carry process and should be
	   unlocked when carry leaves a level */
	__u32 unlock:1;

	/* disk block for this node was allocated by carry process and
	   should be deallocated when carry leaves a level */
	__u32 deallocate:1;
	/* this carry node was allocated by carry process and should be
	   freed when carry leaves a level */
	__u32 free:1;

	/* type of lock we want to take on this node */
	lock_handle lock_handle;
} carry_node;

/* &carry_opcode - elementary operations that can be carried upward

   Operations that carry() can handle. This list is supposed to be
   expanded.

   Each carry operation (cop) is handled by appropriate function defined
   in fs/reiser4/carry.c. For example COP_INSERT is handled by
   fs/reiser4/carry.c:carry_insert() etc. These functions in turn
   call plugins of nodes affected by operation to modify nodes' content
   and to gather operations to be performed on the next level.

*/
typedef enum {
	/* insert new item into node. */
	COP_INSERT,
	/* delete pointer from parent node */
	COP_DELETE,
	/* remove part of or whole node. */
	COP_CUT,
	/* increase size of item. */
	COP_PASTE,
	/* insert extent (that is sequence of unformatted nodes). */
	COP_EXTENT,
	/* update delimiting key in least common ancestor of two
	   nodes. This is performed when items are moved between two
	   nodes.
	 */
	COP_UPDATE,
	/* insert flow */
	COP_INSERT_FLOW,
	COP_LAST_OP,
} carry_opcode;

#define CARRY_FLOW_NEW_NODES_LIMIT 20

/* mode (or subtype) of COP_{INSERT|PASTE} operation. Specifies how target
   item is determined. */
typedef enum {
	/* target item is one containing pointer to the ->child node */
	COPT_CHILD,
	/* target item is given explicitly by @coord */
	COPT_ITEM_DATA,
	/* target item is given by key */
	COPT_KEY,
	/* see insert_paste_common() for more comments on this. */
	COPT_PASTE_RESTARTED,
} cop_insert_pos_type;

/* flags to cut and delete */
typedef enum {
	/* don't kill node even if it became completely empty as results of
	 * cut. This is needed for eottl handling. See carry_extent() for
	 * details. */
	DELETE_RETAIN_EMPTY = (1 << 0)
} cop_delete_flag;

/*
 * carry() implements "lock handle tracking" feature.
 *
 * Callers supply carry with node where to perform initial operation and lock
 * handle on this node. Trying to optimize node utilization carry may actually
 * move insertion point to different node. Callers expect that lock handle
 * will rebe transferred to the new node also.
 *
 */
typedef enum {
	/* transfer lock handle along with insertion point */
	CARRY_TRACK_CHANGE = 1,
	/* acquire new lock handle to the node where insertion point is. This
	 * is used when carry() client doesn't initially possess lock handle
	 * on the insertion point node, for example, by extent insertion
	 * code. See carry_extent(). */
	CARRY_TRACK_NODE = 2
} carry_track_type;

/* data supplied to COP_{INSERT|PASTE} by callers */
typedef struct carry_insert_data {
	/* position where new item is to be inserted */
	coord_t *coord;
	/* new item description */
	reiser4_item_data * data;
	/* key of new item */
	const reiser4_key * key;
} carry_insert_data;

/* cut and kill are similar, so carry_cut_data and carry_kill_data share the
   below structure of parameters */
struct cut_kill_params {
	/* coord where cut starts (inclusive) */
	coord_t *from;
	/* coord where cut stops (inclusive, this item/unit will also be
	 * cut) */
	coord_t *to;
	/* starting key. This is necessary when item and unit pos don't
	 * uniquely identify what portion or tree to remove. For example, this
	 * indicates what portion of extent unit will be affected. */
	const reiser4_key * from_key;
	/* exclusive stop key */
	const reiser4_key * to_key;
	/* if this is not NULL, smallest actually removed key is stored
	 * here. */
	reiser4_key *smallest_removed;
	/* kill_node_content()  is called for file truncate */
	int truncate;
};

struct carry_cut_data {
	struct cut_kill_params params;
};

struct carry_kill_data {
	struct cut_kill_params params;
	/* parameter to be passed to the ->kill_hook() method of item
	 * plugin */
	/*void *iplug_params; *//* FIXME: unused currently */
	/* if not NULL---inode whose items are being removed. This is needed
	 * for ->kill_hook() of extent item to update VM structures when
	 * removing pages. */
	struct inode *inode;
	/* sibling list maintenance is complicated by existence of eottl. When
	 * eottl whose left and right neighbors are formatted leaves is
	 * removed, one has to connect said leaves in the sibling list. This
	 * cannot be done when extent removal is just started as locking rules
	 * require sibling list update to happen atomically with removal of
	 * extent item. Therefore: 1. pointers to left and right neighbors
	 * have to be passed down to the ->kill_hook() of extent item, and
	 * 2. said neighbors have to be locked. */
	lock_handle *left;
	lock_handle *right;
	/* flags modifying behavior of kill. Currently, it may have
	   DELETE_RETAIN_EMPTY set. */
	unsigned flags;
	char *buf;
};

/* &carry_tree_op - operation to "carry" upward.

   Description of an operation we want to "carry" to the upper level of
   a tree: e.g, when we insert something and there is not enough space
   we allocate a new node and "carry" the operation of inserting a
   pointer to the new node to the upper level, on removal of empty node,
   we carry up operation of removing appropriate entry from parent.

   There are two types of carry ops: when adding or deleting node we
   node at the parent level where appropriate modification has to be
   performed is known in advance. When shifting items between nodes
   (split, merge), delimiting key should be changed in the least common
   parent of the nodes involved that is not known in advance.

   For the operations of the first type we store in &carry_op pointer to
   the &carry_node at the parent level. For the operation of the second
   type we store &carry_node or parents of the left and right nodes
   modified and keep track of them upward until they coincide.

*/
typedef struct carry_op {
	/* pool linkage */
	struct reiser4_pool_header header;
	carry_opcode op;
	/* node on which operation is to be performed:

	   for insert, paste: node where new item is to be inserted

	   for delete: node where pointer is to be deleted

	   for cut: node to cut from

	   for update: node where delimiting key is to be modified

	   for modify: parent of modified node

	 */
	carry_node *node;
	union {
		struct {
			/* (sub-)type of insertion/paste. Taken from
			   cop_insert_pos_type. */
			__u8 type;
			/* various operation flags. Taken from
			   cop_insert_flag. */
			__u8 flags;
			carry_insert_data *d;
			carry_node *child;
			znode *brother;
		} insert, paste, extent;

		struct {
			int is_cut;
			union {
				carry_kill_data *kill;
				carry_cut_data *cut;
			} u;
		} cut_or_kill;

		struct {
			carry_node *left;
		} update;
		struct {
			/* changed child */
			carry_node *child;
			/* bitmask of changes. See &cop_modify_flag */
			__u32 flag;
		} modify;
		struct {
			/* flags to deletion operation. Are taken from
			   cop_delete_flag */
			__u32 flags;
			/* child to delete from parent. If this is
			   NULL, delete op->node.  */
			carry_node *child;
		} delete;
		struct {
			/* various operation flags. Taken from
			   cop_insert_flag. */
			__u32 flags;
			flow_t *flow;
			coord_t *insert_point;
			reiser4_item_data *data;
			/* flow insertion is limited by number of new blocks
			   added in that operation which do not get any data
			   but part of flow. This limit is set by macro
			   CARRY_FLOW_NEW_NODES_LIMIT. This field stores number
			   of nodes added already during one carry_flow */
			int new_nodes;
		} insert_flow;
	} u;
} carry_op;

/* &carry_op_pool - preallocated pool of carry operations, and nodes */
typedef struct carry_pool {
	carry_op op[CARRIES_POOL_SIZE];
	struct reiser4_pool op_pool;
	carry_node node[NODES_LOCKED_POOL_SIZE];
	struct reiser4_pool node_pool;
} carry_pool;

/* &carry_tree_level - carry process on given level

   Description of balancing process on the given level.

   No need for locking here, as carry_tree_level is essentially per
   thread thing (for now).

*/
struct carry_level {
	/* this level may be restarted */
	__u32 restartable:1;
	/* list of carry nodes on this level, ordered by key order */
	struct list_head nodes;
	struct list_head ops;
	/* pool where new objects are allocated from */
	carry_pool *pool;
	int ops_num;
	int nodes_num;
	/* new root created on this level, if any */
	znode *new_root;
	/* This is set by caller (insert_by_key(), rreiser4_esize_item(), etc.)
	   when they want ->tracked to automagically wander to the node where
	   insertion point moved after insert or paste.
	 */
	carry_track_type track_type;
	/* lock handle supplied by user that we are tracking. See
	   above. */
	lock_handle *tracked;
};

/* information carry passes to plugin methods that may add new operations to
   the @todo queue  */
struct carry_plugin_info {
	carry_level *doing;
	carry_level *todo;
};

int reiser4_carry(carry_level * doing, carry_level * done);

carry_node *reiser4_add_carry(carry_level * level, pool_ordering order,
			      carry_node * reference);
carry_node *reiser4_add_carry_skip(carry_level * level, pool_ordering order,
				   carry_node * reference);

extern carry_node *insert_carry_node(carry_level * doing,
				     carry_level * todo, const znode * node);

extern carry_pool *init_carry_pool(int);
extern void done_carry_pool(carry_pool * pool);

extern void init_carry_level(carry_level * level, carry_pool * pool);

extern carry_op *reiser4_post_carry(carry_level * level, carry_opcode op,
				    znode * node, int apply_to_parent);
extern carry_op *node_post_carry(carry_plugin_info * info, carry_opcode op,
				 znode * node, int apply_to_parent_p);

carry_node *add_new_znode(znode * brother, carry_node * reference,
			  carry_level * doing, carry_level * todo);

carry_node *find_carry_node(carry_level * level, const znode * node);

extern znode *reiser4_carry_real(const carry_node * node);

/* helper macros to iterate over carry queues */

#define carry_node_next(node)						\
	list_entry((node)->header.level_linkage.next, carry_node,	\
		   header.level_linkage)

#define carry_node_prev(node)						\
	list_entry((node)->header.level_linkage.prev, carry_node,	\
		   header.level_linkage)

#define carry_node_front(level)						\
	list_entry((level)->nodes.next, carry_node, header.level_linkage)

#define carry_node_back(level)						\
	list_entry((level)->nodes.prev, carry_node, header.level_linkage)

#define carry_node_end(level, node)				\
	(&(level)->nodes == &(node)->header.level_linkage)

/* macro to iterate over all operations in a @level */
#define for_all_ops(level /* carry level (of type carry_level *) */,	       \
		    op    /* pointer to carry operation, modified by loop (of  \
			   * type carry_op *) */,			       \
		    tmp   /* pointer to carry operation (of type carry_op *),  \
			   * used to make iterator stable in the face of       \
			   * deletions from the level */ )		       \
for (op = list_entry(level->ops.next, carry_op, header.level_linkage),	       \
     tmp = list_entry(op->header.level_linkage.next, carry_op, header.level_linkage); 	\
     &op->header.level_linkage != &level->ops;				       \
     op = tmp,								       \
     tmp = list_entry(op->header.level_linkage.next, carry_op, header.level_linkage))

#if 0
for (op = (carry_op *) pool_level_list_front(&level->ops),	       \
     tmp = (carry_op *) pool_level_list_next(&op->header) ;	       \
     !pool_level_list_end(&level->ops, &op->header) ;		       \
     op = tmp, tmp = (carry_op *) pool_level_list_next(&op->header))
#endif

/* macro to iterate over all nodes in a @level */			       \
#define for_all_nodes(level /* carry level (of type carry_level *) */,	       \
		      node  /* pointer to carry node, modified by loop (of     \
			      * type carry_node *) */,			       \
		      tmp   /* pointer to carry node (of type carry_node *),   \
			      * used to make iterator stable in the face of *  \
			      * deletions from the level */ )		       \
for (node = list_entry(level->nodes.next, carry_node, header.level_linkage),   \
     tmp = list_entry(node->header.level_linkage.next, carry_node, header.level_linkage); \
     &node->header.level_linkage != &level->nodes;			       \
     node = tmp, 							       \
     tmp = list_entry(node->header.level_linkage.next, carry_node, header.level_linkage))

#if 0
for (node = carry_node_front(level),					\
     tmp = carry_node_next(node) ; !carry_node_end(level, node) ;	\
     node = tmp, tmp = carry_node_next(node))
#endif

/* macro to iterate over all nodes in a @level in reverse order

   This is used, because nodes are unlocked in reversed order of locking */
#define for_all_nodes_back(level /* carry level (of type carry_level *) */,    \
			   node  /* pointer to carry node, modified by loop    \
				   * (of type carry_node *) */,		       \
			   tmp   /* pointer to carry node (of type carry_node  \
				   * *), used to make iterator stable in the   \
				   * face of deletions from the level */ )     \
for (node = carry_node_back(level),		\
     tmp = carry_node_prev(node) ; !carry_node_end(level, node) ;	\
     node = tmp, tmp = carry_node_prev(node))

/* __FS_REISER4_CARRY_H__ */
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
