/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
   reiser4/README */

/* Forward declarations. Thank you Kernighan. */

#if !defined(__REISER4_FORWARD_H__)
#define __REISER4_FORWARD_H__

#include <asm/errno.h>
#include <linux/types.h>

typedef struct zlock zlock;
typedef struct lock_stack lock_stack;
typedef struct lock_handle lock_handle;
typedef struct znode znode;
typedef struct flow flow_t;
typedef struct coord coord_t;
typedef struct tree_access_pointer tap_t;
typedef struct reiser4_object_create_data reiser4_object_create_data;
typedef union reiser4_plugin reiser4_plugin;
typedef __u16 reiser4_plugin_id;
typedef __u64 reiser4_plugin_groups;
typedef struct item_plugin item_plugin;
typedef struct jnode_plugin jnode_plugin;
typedef struct reiser4_item_data reiser4_item_data;
typedef union reiser4_key reiser4_key;
typedef struct reiser4_tree reiser4_tree;
typedef struct carry_cut_data carry_cut_data;
typedef struct carry_kill_data carry_kill_data;
typedef struct carry_tree_op carry_tree_op;
typedef struct carry_tree_node carry_tree_node;
typedef struct carry_plugin_info carry_plugin_info;
typedef struct reiser4_journal reiser4_journal;
typedef struct txn_atom txn_atom;
typedef struct txn_handle txn_handle;
typedef struct txn_mgr txn_mgr;
typedef struct reiser4_dir_entry_desc reiser4_dir_entry_desc;
typedef struct reiser4_context reiser4_context;
typedef struct carry_level carry_level;
typedef struct blocknr_set_entry blocknr_set_entry;
typedef struct blocknr_list_entry blocknr_list_entry;
/* super_block->s_fs_info points to this */
typedef struct reiser4_super_info_data reiser4_super_info_data;
/* next two objects are fields of reiser4_super_info_data */
typedef struct reiser4_oid_allocator reiser4_oid_allocator;
typedef struct reiser4_space_allocator reiser4_space_allocator;

typedef struct flush_scan flush_scan;
typedef struct flush_position flush_pos_t;

typedef unsigned short pos_in_node_t;
#define MAX_POS_IN_NODE 65535

typedef struct jnode jnode;
typedef struct reiser4_blocknr_hint reiser4_blocknr_hint;

typedef struct uf_coord uf_coord_t;
typedef struct hint hint_t;

typedef struct ktxnmgrd_context ktxnmgrd_context;

struct inode;
struct page;
struct file;
struct dentry;
struct super_block;

/* return values of coord_by_key(). cbk == coord_by_key */
typedef enum {
	CBK_COORD_FOUND = 0,
	CBK_COORD_NOTFOUND = -ENOENT,
} lookup_result;

/* results of lookup with directory file */
typedef enum {
	FILE_NAME_FOUND = 0,
	FILE_NAME_NOTFOUND = -ENOENT,
	FILE_IO_ERROR = -EIO,	/* FIXME: it seems silly to have special OOM,
				   IO_ERROR return codes for each search. */
	FILE_OOM = -ENOMEM	/* FIXME: it seems silly to have special OOM,
				   IO_ERROR return codes for each search. */
} file_lookup_result;

/* behaviors of lookup. If coord we are looking for is actually in a tree,
    both coincide. */
typedef enum {
	/* search exactly for the coord with key given */
	FIND_EXACT,
	/* search for coord with the maximal key not greater than one
	   given */
	FIND_MAX_NOT_MORE_THAN	/*LEFT_SLANT_BIAS */
} lookup_bias;

typedef enum {
	/* number of leaf level of the tree
	   The fake root has (tree_level=0). */
	LEAF_LEVEL = 1,

	/* number of level one above leaf level of the tree.

	   It is supposed that internal tree used by reiser4 to store file
	   system data and meta data will have height 2 initially (when
	   created by mkfs).
	 */
	TWIG_LEVEL = 2,
} tree_level;

/* The "real" maximum ztree height is the 0-origin size of any per-level
   array, since the zero'th level is not used. */
#define REAL_MAX_ZTREE_HEIGHT     (REISER4_MAX_ZTREE_HEIGHT-LEAF_LEVEL)

/* enumeration of possible mutual position of item and coord.  This enum is
    return type of ->is_in_item() item plugin method which see. */
typedef enum {
	/* coord is on the left of an item */
	IP_ON_THE_LEFT,
	/* coord is inside item */
	IP_INSIDE,
	/* coord is inside item, but to the right of the rightmost unit of
	   this item */
	IP_RIGHT_EDGE,
	/* coord is on the right of an item */
	IP_ON_THE_RIGHT
} interposition;

/* type of lock to acquire on znode before returning it to caller */
typedef enum {
	ZNODE_NO_LOCK = 0,
	ZNODE_READ_LOCK = 1,
	ZNODE_WRITE_LOCK = 2,
} znode_lock_mode;

/* type of lock request */
typedef enum {
	ZNODE_LOCK_LOPRI = 0,
	ZNODE_LOCK_HIPRI = (1 << 0),

	/* By setting the ZNODE_LOCK_NONBLOCK flag in a lock request the call to
	   longterm_lock_znode will not sleep waiting for the lock to become
	   available.  If the lock is unavailable, reiser4_znode_lock will
	   immediately return the value -E_REPEAT. */
	ZNODE_LOCK_NONBLOCK = (1 << 1),
	/* An option for longterm_lock_znode which prevents atom fusion */
	ZNODE_LOCK_DONT_FUSE = (1 << 2)
} znode_lock_request;

typedef enum { READ_OP = 0, WRITE_OP = 1 } rw_op;

/* used to specify direction of shift. These must be -1 and 1 */
typedef enum {
	SHIFT_LEFT = 1,
	SHIFT_RIGHT = -1
} shift_direction;

typedef enum {
	LEFT_SIDE,
	RIGHT_SIDE
} sideof;

#define reiser4_round_up(value, order)				\
	((typeof(value))(((long) (value) + (order) - 1U) &	\
			 ~((order) - 1)))

/* values returned by squalloc_right_neighbor and its auxiliary functions */
typedef enum {
	/* unit of internal item is moved */
	SUBTREE_MOVED = 0,
	/* nothing else can be squeezed into left neighbor */
	SQUEEZE_TARGET_FULL = 1,
	/* all content of node is squeezed into its left neighbor */
	SQUEEZE_SOURCE_EMPTY = 2,
	/* one more item is copied (this is only returned by
	   allocate_and_copy_extent to squalloc_twig)) */
	SQUEEZE_CONTINUE = 3
} squeeze_result;

/* Do not change items ids. If you do - there will be format change */
typedef enum {
	STATIC_STAT_DATA_ID = 0x0,
	SIMPLE_DIR_ENTRY_ID = 0x1,
	COMPOUND_DIR_ID = 0x2,
	NODE_POINTER_ID = 0x3,
	EXTENT_POINTER_ID = 0x5,
	FORMATTING_ID = 0x6,
	CTAIL_ID = 0x7,
	BLACK_BOX_ID = 0x8,
	LAST_ITEM_ID = 0x9
} item_id;

/* Flags passed to jnode_flush() to allow it to distinguish default settings
   based on whether commit() was called or VM memory pressure was applied. */
typedef enum {
	/* submit flush queue to disk at jnode_flush completion */
	JNODE_FLUSH_WRITE_BLOCKS = 1,

	/* flush is called for commit */
	JNODE_FLUSH_COMMIT = 2,
	/* not implemented */
	JNODE_FLUSH_MEMORY_FORMATTED = 4,

	/* not implemented */
	JNODE_FLUSH_MEMORY_UNFORMATTED = 8,
} jnode_flush_flags;

/* Flags to insert/paste carry operations. Currently they only used in
   flushing code, but in future, they can be used to optimize for repetitive
   accesses.  */
typedef enum {
	/* carry is not allowed to shift data to the left when trying to find
	   free space  */
	COPI_DONT_SHIFT_LEFT = (1 << 0),
	/* carry is not allowed to shift data to the right when trying to find
	   free space  */
	COPI_DONT_SHIFT_RIGHT = (1 << 1),
	/* carry is not allowed to allocate new node(s) when trying to find
	   free space */
	COPI_DONT_ALLOCATE = (1 << 2),
	/* try to load left neighbor if its not in a cache */
	COPI_LOAD_LEFT = (1 << 3),
	/* try to load right neighbor if its not in a cache */
	COPI_LOAD_RIGHT = (1 << 4),
	/* shift insertion point to the left neighbor */
	COPI_GO_LEFT = (1 << 5),
	/* shift insertion point to the right neighbor */
	COPI_GO_RIGHT = (1 << 6),
	/* try to step back into original node if insertion into new node
	   fails after shifting data there. */
	COPI_STEP_BACK = (1 << 7),
	/* use all possible space in the node */
	COPI_SWEEP = (1 << 8)
} cop_insert_flag;

typedef enum {
	SAFE_UNLINK,		/* safe-link for unlink */
	SAFE_TRUNCATE		/* safe-link for truncate */
} reiser4_safe_link_t;

/* this is to show on which list of atom jnode is */
typedef enum {
	NOT_CAPTURED,
	DIRTY_LIST,
	CLEAN_LIST,
	FQ_LIST,
	WB_LIST,
	OVRWR_LIST
} atom_list;

/* __REISER4_FORWARD_H__ */
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
