/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Tree operations. See fs/reiser4/tree.c for comments */

#if !defined( __REISER4_TREE_H__ )
#define __REISER4_TREE_H__

#include "forward.h"
#include "debug.h"
#include "dformat.h"
#include "plugin/node/node.h"
#include "plugin/plugin.h"
#include "znode.h"
#include "tap.h"

#include <linux/types.h>	/* for __u??  */
#include <linux/fs.h>		/* for struct super_block  */
#include <linux/spinlock.h>
#include <linux/sched.h>	/* for struct task_struct */

/* fictive block number never actually used */
extern const reiser4_block_nr UBER_TREE_ADDR;

/* &cbk_cache_slot - entry in a coord cache.

   This is entry in a coord_by_key (cbk) cache, represented by
   &cbk_cache.

*/
typedef struct cbk_cache_slot {
	/* cached node */
	znode *node;
	/* linkage to the next cbk cache slot in a LRU order */
	struct list_head lru;
} cbk_cache_slot;

/* &cbk_cache - coord cache. This is part of reiser4_tree.

   cbk_cache is supposed to speed up tree lookups by caching results of recent
   successful lookups (we don't cache negative results as dentry cache
   does). Cache consists of relatively small number of entries kept in a LRU
   order. Each entry (&cbk_cache_slot) contains a pointer to znode, from
   which we can obtain a range of keys that covered by this znode. Before
   embarking into real tree traversal we scan cbk_cache slot by slot and for
   each slot check whether key we are looking for is between minimal and
   maximal keys for node pointed to by this slot. If no match is found, real
   tree traversal is performed and if result is successful, appropriate entry
   is inserted into cache, possibly pulling least recently used entry out of
   it.

   Tree spin lock is used to protect coord cache. If contention for this
   lock proves to be too high, more finer grained locking can be added.

   Invariants involving parts of this data-type:

      [cbk-cache-invariant]
*/
typedef struct cbk_cache {
	/* serializator */
	rwlock_t guard;
	int nr_slots;
	/* head of LRU list of cache slots */
	struct list_head lru;
	/* actual array of slots */
	cbk_cache_slot *slot;
} cbk_cache;

/* level_lookup_result - possible outcome of looking up key at some level.
   This is used by coord_by_key when traversing tree downward. */
typedef enum {
	/* continue to the next level */
	LOOKUP_CONT,
	/* done. Either required item was found, or we can prove it
	   doesn't exist, or some error occurred. */
	LOOKUP_DONE,
	/* restart traversal from the root. Infamous "repetition". */
	LOOKUP_REST
} level_lookup_result;

/*    This is representation of internal reiser4 tree where all file-system
   data and meta-data are stored. This structure is passed to all tree
   manipulation functions. It's different from the super block because:
   we don't want to limit ourselves to strictly one to one mapping
   between super blocks and trees, and, because they are logically
   different: there are things in a super block that have no relation to
   the tree (bitmaps, journalling area, mount options, etc.) and there
   are things in a tree that bear no relation to the super block, like
   tree of znodes.

   At this time, there is only one tree
   per filesystem, and this struct is part of the super block.  We only
   call the super block the super block for historical reasons (most
   other filesystems call the per filesystem metadata the super block).
*/

struct reiser4_tree {
	/* block_nr == 0 is fake znode. Write lock it, while changing
	   tree height. */
	/* disk address of root node of a tree */
	reiser4_block_nr root_block;

	/* level of the root node. If this is 1, tree consists of root
	   node only */
	tree_level height;

	/*
	 * this is cached here avoid calling plugins through function
	 * dereference all the time.
	 */
	__u64 estimate_one_insert;

	/* cache of recent tree lookup results */
	cbk_cache cbk_cache;

	/* hash table to look up znodes by block number. */
	z_hash_table zhash_table;
	z_hash_table zfake_table;
	/* hash table to look up jnodes by inode and offset. */
	j_hash_table jhash_table;

	/* lock protecting:
	   - parent pointers,
	   - sibling pointers,
	   - znode hash table
	   - coord cache
	 */
	/* NOTE: The "giant" tree lock can be replaced by more spin locks,
	   hoping they will be less contented. We can use one spin lock per one
	   znode hash bucket.  With adding of some code complexity, sibling
	   pointers can be protected by both znode spin locks.  However it looks
	   more SMP scalable we should test this locking change on n-ways (n >
	   4) SMP machines.  Current 4-ways machine test does not show that tree
	   lock is contented and it is a bottleneck (2003.07.25). */

	rwlock_t tree_lock;

	/* lock protecting delimiting keys */
	rwlock_t dk_lock;

	/* spin lock protecting znode_epoch */
	spinlock_t epoch_lock;
	/* version stamp used to mark znode updates. See seal.[ch] for more
	 * information. */
	__u64 znode_epoch;

	znode *uber;
	node_plugin *nplug;
	struct super_block *super;
	struct {
		/* carry flags used for insertion of new nodes */
		__u32 new_node_flags;
		/* carry flags used for insertion of new extents */
		__u32 new_extent_flags;
		/* carry flags used for paste operations */
		__u32 paste_flags;
		/* carry flags used for insert operations */
		__u32 insert_flags;
	} carry;
};

extern int reiser4_init_tree(reiser4_tree * tree,
			     const reiser4_block_nr * root_block,
			     tree_level height, node_plugin * default_plugin);
extern void reiser4_done_tree(reiser4_tree * tree);

/* cbk flags: options for coord_by_key() */
typedef enum {
	/* coord_by_key() is called for insertion. This is necessary because
	   of extents being located at the twig level. For explanation, see
	   comment just above is_next_item_internal().
	 */
	CBK_FOR_INSERT = (1 << 0),
	/* coord_by_key() is called with key that is known to be unique */
	CBK_UNIQUE = (1 << 1),
	/* coord_by_key() can trust delimiting keys. This options is not user
	   accessible. coord_by_key() will set it automatically. It will be
	   only cleared by special-case in extents-on-the-twig-level handling
	   where it is necessary to insert item with a key smaller than
	   leftmost key in a node. This is necessary because of extents being
	   located at the twig level. For explanation, see comment just above
	   is_next_item_internal().
	 */
	CBK_TRUST_DK = (1 << 2),
	CBK_READA = (1 << 3),	/* original: readahead leaves which contain items of certain file */
	CBK_READDIR_RA = (1 << 4),	/* readdir: readahead whole directory and all its stat datas */
	CBK_DKSET = (1 << 5),
	CBK_EXTENDED_COORD = (1 << 6),	/* coord_t is actually */
	CBK_IN_CACHE = (1 << 7),	/* node is already in cache */
	CBK_USE_CRABLOCK = (1 << 8)	/* use crab_lock in stead of long term
					 * lock */
} cbk_flags;

/* insertion outcome. IBK = insert by key */
typedef enum {
	IBK_INSERT_OK = 0,
	IBK_ALREADY_EXISTS = -EEXIST,
	IBK_IO_ERROR = -EIO,
	IBK_NO_SPACE = -E_NODE_FULL,
	IBK_OOM = -ENOMEM
} insert_result;

#define IS_CBKERR(err) ((err) != CBK_COORD_FOUND && (err) != CBK_COORD_NOTFOUND)

typedef int (*tree_iterate_actor_t) (reiser4_tree * tree, coord_t * coord,
				     lock_handle * lh, void *arg);
extern int reiser4_iterate_tree(reiser4_tree * tree, coord_t * coord,
				lock_handle * lh,
				tree_iterate_actor_t actor, void *arg,
				znode_lock_mode mode, int through_units_p);
extern int get_uber_znode(reiser4_tree * tree, znode_lock_mode mode,
			  znode_lock_request pri, lock_handle * lh);

/* return node plugin of @node */
static inline node_plugin *node_plugin_by_node(const znode *
					       node /* node to query */ )
{
	assert("vs-213", node != NULL);
	assert("vs-214", znode_is_loaded(node));

	return node->nplug;
}

/* number of items in @node */
static inline pos_in_node_t node_num_items(const znode * node)
{
	assert("nikita-2754", znode_is_loaded(node));
	assert("nikita-2468",
	       node_plugin_by_node(node)->num_of_items(node) == node->nr_items);

	return node->nr_items;
}

/* Return the number of items at the present node.  Asserts coord->node !=
   NULL. */
static inline unsigned coord_num_items(const coord_t * coord)
{
	assert("jmacd-9805", coord->node != NULL);

	return node_num_items(coord->node);
}

/* true if @node is empty */
static inline int node_is_empty(const znode * node)
{
	return node_num_items(node) == 0;
}

typedef enum {
	SHIFTED_SOMETHING = 0,
	SHIFT_NO_SPACE = -E_NODE_FULL,
	SHIFT_IO_ERROR = -EIO,
	SHIFT_OOM = -ENOMEM,
} shift_result;

extern node_plugin *node_plugin_by_coord(const coord_t * coord);
extern int is_coord_in_node(const coord_t * coord);
extern int key_in_node(const reiser4_key *, const coord_t *);
extern void coord_item_move_to(coord_t * coord, int items);
extern void coord_unit_move_to(coord_t * coord, int units);

/* there are two types of repetitive accesses (ra): intra-syscall
   (local) and inter-syscall (global). Local ra is used when
   during single syscall we add/delete several items and units in the
   same place in a tree. Note that plan-A fragments local ra by
   separating stat-data and file body in key-space. Global ra is
   used when user does repetitive modifications in the same place in a
   tree.

   Our ra implementation serves following purposes:
    1 it affects balancing decisions so that next operation in a row
      can be performed faster;
    2 it affects lower-level read-ahead in page-cache;
    3 it allows to avoid unnecessary lookups by maintaining some state
      across several operations (this is only for local ra);
    4 it leaves room for lazy-micro-balancing: when we start a sequence of
      operations they are performed without actually doing any intra-node
      shifts, until we finish sequence or scope of sequence leaves
      current node, only then we really pack node (local ra only).
*/

/* another thing that can be useful is to keep per-tree and/or
   per-process cache of recent lookups. This cache can be organised as a
   list of block numbers of formatted nodes sorted by starting key in
   this node. Balancings should invalidate appropriate parts of this
   cache.
*/

lookup_result coord_by_key(reiser4_tree * tree, const reiser4_key * key,
			   coord_t * coord, lock_handle * handle,
			   znode_lock_mode lock, lookup_bias bias,
			   tree_level lock_level, tree_level stop_level,
			   __u32 flags, ra_info_t *);

lookup_result reiser4_object_lookup(struct inode *object,
				    const reiser4_key * key,
				    coord_t * coord,
				    lock_handle * lh,
				    znode_lock_mode lock_mode,
				    lookup_bias bias,
				    tree_level lock_level,
				    tree_level stop_level,
				    __u32 flags, ra_info_t * info);

insert_result insert_by_key(reiser4_tree * tree, const reiser4_key * key,
			    reiser4_item_data * data, coord_t * coord,
			    lock_handle * lh,
			    tree_level stop_level, __u32 flags);
insert_result insert_by_coord(coord_t * coord,
			      reiser4_item_data * data, const reiser4_key * key,
			      lock_handle * lh, __u32);
insert_result insert_extent_by_coord(coord_t * coord,
				     reiser4_item_data * data,
				     const reiser4_key * key, lock_handle * lh);
int cut_node_content(coord_t * from, coord_t * to, const reiser4_key * from_key,
		     const reiser4_key * to_key,
		     reiser4_key * smallest_removed);
int kill_node_content(coord_t * from, coord_t * to,
		      const reiser4_key * from_key, const reiser4_key * to_key,
		      reiser4_key * smallest_removed,
		      znode * locked_left_neighbor, struct inode *inode,
		      int truncate);

int reiser4_resize_item(coord_t * coord, reiser4_item_data * data,
			reiser4_key * key, lock_handle * lh, cop_insert_flag);
int insert_into_item(coord_t * coord, lock_handle * lh, const reiser4_key * key,
		     reiser4_item_data * data, unsigned);
int reiser4_insert_flow(coord_t * coord, lock_handle * lh, flow_t * f);
int find_new_child_ptr(znode * parent, znode * child, znode * left,
		       coord_t * result);

int shift_right_of_but_excluding_insert_coord(coord_t * insert_coord);
int shift_left_of_and_including_insert_coord(coord_t * insert_coord);

void fake_kill_hook_tail(struct inode *, loff_t start, loff_t end, int);

extern int cut_tree_worker_common(tap_t *, const reiser4_key *,
				  const reiser4_key *, reiser4_key *,
				  struct inode *, int, int *);
extern int reiser4_cut_tree_object(reiser4_tree *, const reiser4_key *,
				   const reiser4_key *, reiser4_key *,
				   struct inode *, int, int *);
extern int reiser4_cut_tree(reiser4_tree * tree, const reiser4_key * from,
			    const reiser4_key * to, struct inode *, int);

extern int reiser4_delete_node(znode *, reiser4_key *, struct inode *, int);
extern int check_tree_pointer(const coord_t * pointer, const znode * child);
extern int find_new_child_ptr(znode * parent, znode * child UNUSED_ARG,
			      znode * left, coord_t * result);
extern int find_child_ptr(znode * parent, znode * child, coord_t * result);
extern int set_child_delimiting_keys(znode * parent, const coord_t * in_parent,
				     znode * child);
extern znode *child_znode(const coord_t * in_parent, znode * parent,
			  int incore_p, int setup_dkeys_p);

extern int cbk_cache_init(cbk_cache * cache);
extern void cbk_cache_done(cbk_cache * cache);
extern void cbk_cache_invalidate(const znode * node, reiser4_tree * tree);

extern char *sprint_address(const reiser4_block_nr * block);

#if REISER4_DEBUG
extern void print_coord_content(const char *prefix, coord_t * p);
extern void reiser4_print_address(const char *prefix,
			const reiser4_block_nr * block);
extern void print_tree_rec(const char *prefix, reiser4_tree * tree,
			   __u32 flags);
extern void check_dkeys(znode *node);
#else
#define print_coord_content(p, c) noop
#define reiser4_print_address(p, b) noop
#endif

extern void forget_znode(lock_handle * handle);
extern int deallocate_znode(znode * node);

extern int is_disk_addr_unallocated(const reiser4_block_nr * addr);

/* struct used internally to pack all numerous arguments of tree lookup.
    Used to avoid passing a lot of arguments to helper functions. */
typedef struct cbk_handle {
	/* tree we are in */
	reiser4_tree *tree;
	/* key we are going after */
	const reiser4_key *key;
	/* coord we will store result in */
	coord_t *coord;
	/* type of lock to take on target node */
	znode_lock_mode lock_mode;
	/* lookup bias. See comments at the declaration of lookup_bias */
	lookup_bias bias;
	/* lock level: level starting from which tree traversal starts taking
	 * write locks. */
	tree_level lock_level;
	/* level where search will stop. Either item will be found between
	   lock_level and stop_level, or CBK_COORD_NOTFOUND will be
	   returned.
	 */
	tree_level stop_level;
	/* level we are currently at */
	tree_level level;
	/* block number of @active node. Tree traversal operates on two
	   nodes: active and parent.  */
	reiser4_block_nr block;
	/* put here error message to be printed by caller */
	const char *error;
	/* result passed back to caller */
	int result;
	/* lock handles for active and parent */
	lock_handle *parent_lh;
	lock_handle *active_lh;
	reiser4_key ld_key;
	reiser4_key rd_key;
	/* flags, passed to the cbk routine. Bits of this bitmask are defined
	   in tree.h:cbk_flags enum. */
	__u32 flags;
	ra_info_t *ra_info;
	struct inode *object;
} cbk_handle;

extern znode_lock_mode cbk_lock_mode(tree_level level, cbk_handle * h);

/* eottl.c */
extern int handle_eottl(cbk_handle *h, int *outcome);

int lookup_multikey(cbk_handle * handle, int nr_keys);
int lookup_couple(reiser4_tree * tree,
		  const reiser4_key * key1, const reiser4_key * key2,
		  coord_t * coord1, coord_t * coord2,
		  lock_handle * lh1, lock_handle * lh2,
		  znode_lock_mode lock_mode, lookup_bias bias,
		  tree_level lock_level, tree_level stop_level, __u32 flags,
		  int *result1, int *result2);

static inline void read_lock_tree(reiser4_tree *tree)
{
	/* check that tree is not locked */
	assert("", (LOCK_CNT_NIL(rw_locked_tree) &&
		    LOCK_CNT_NIL(read_locked_tree) &&
		    LOCK_CNT_NIL(write_locked_tree)));
	/* check that spinlocks of lower priorities are not held */
	assert("", (LOCK_CNT_NIL(spin_locked_txnh) &&
		    LOCK_CNT_NIL(rw_locked_dk) &&
		    LOCK_CNT_NIL(spin_locked_stack)));

	read_lock(&(tree->tree_lock));

	LOCK_CNT_INC(read_locked_tree);
	LOCK_CNT_INC(rw_locked_tree);
	LOCK_CNT_INC(spin_locked);
}

static inline void read_unlock_tree(reiser4_tree *tree)
{
	assert("nikita-1375", LOCK_CNT_GTZ(read_locked_tree));
	assert("nikita-1376", LOCK_CNT_GTZ(rw_locked_tree));
	assert("nikita-1376", LOCK_CNT_GTZ(spin_locked));

	LOCK_CNT_DEC(read_locked_tree);
	LOCK_CNT_DEC(rw_locked_tree);
	LOCK_CNT_DEC(spin_locked);

	read_unlock(&(tree->tree_lock));
}

static inline void write_lock_tree(reiser4_tree *tree)
{
	/* check that tree is not locked */
	assert("", (LOCK_CNT_NIL(rw_locked_tree) &&
		    LOCK_CNT_NIL(read_locked_tree) &&
		    LOCK_CNT_NIL(write_locked_tree)));
	/* check that spinlocks of lower priorities are not held */
	assert("", (LOCK_CNT_NIL(spin_locked_txnh) &&
		    LOCK_CNT_NIL(rw_locked_dk) &&
		    LOCK_CNT_NIL(spin_locked_stack)));

	write_lock(&(tree->tree_lock));

	LOCK_CNT_INC(write_locked_tree);
	LOCK_CNT_INC(rw_locked_tree);
	LOCK_CNT_INC(spin_locked);
}

static inline void write_unlock_tree(reiser4_tree *tree)
{
	assert("nikita-1375", LOCK_CNT_GTZ(write_locked_tree));
	assert("nikita-1376", LOCK_CNT_GTZ(rw_locked_tree));
	assert("nikita-1376", LOCK_CNT_GTZ(spin_locked));

	LOCK_CNT_DEC(write_locked_tree);
	LOCK_CNT_DEC(rw_locked_tree);
	LOCK_CNT_DEC(spin_locked);

	write_unlock(&(tree->tree_lock));
}

static inline void read_lock_dk(reiser4_tree *tree)
{
	/* check that dk is not locked */
	assert("", (LOCK_CNT_NIL(rw_locked_dk) &&
		    LOCK_CNT_NIL(read_locked_dk) &&
		    LOCK_CNT_NIL(write_locked_dk)));
	/* check that spinlocks of lower priorities are not held */
	assert("", LOCK_CNT_NIL(spin_locked_stack));

	read_lock(&((tree)->dk_lock));

	LOCK_CNT_INC(read_locked_dk);
	LOCK_CNT_INC(rw_locked_dk);
	LOCK_CNT_INC(spin_locked);
}

static inline void read_unlock_dk(reiser4_tree *tree)
{
	assert("nikita-1375", LOCK_CNT_GTZ(read_locked_dk));
	assert("nikita-1376", LOCK_CNT_GTZ(rw_locked_dk));
	assert("nikita-1376", LOCK_CNT_GTZ(spin_locked));

	LOCK_CNT_DEC(read_locked_dk);
	LOCK_CNT_DEC(rw_locked_dk);
	LOCK_CNT_DEC(spin_locked);

	read_unlock(&(tree->dk_lock));
}

static inline void write_lock_dk(reiser4_tree *tree)
{
	/* check that dk is not locked */
	assert("", (LOCK_CNT_NIL(rw_locked_dk) &&
		    LOCK_CNT_NIL(read_locked_dk) &&
		    LOCK_CNT_NIL(write_locked_dk)));
	/* check that spinlocks of lower priorities are not held */
	assert("", LOCK_CNT_NIL(spin_locked_stack));

	write_lock(&((tree)->dk_lock));

	LOCK_CNT_INC(write_locked_dk);
	LOCK_CNT_INC(rw_locked_dk);
	LOCK_CNT_INC(spin_locked);
}

static inline void write_unlock_dk(reiser4_tree *tree)
{
	assert("nikita-1375", LOCK_CNT_GTZ(write_locked_dk));
	assert("nikita-1376", LOCK_CNT_GTZ(rw_locked_dk));
	assert("nikita-1376", LOCK_CNT_GTZ(spin_locked));

	LOCK_CNT_DEC(write_locked_dk);
	LOCK_CNT_DEC(rw_locked_dk);
	LOCK_CNT_DEC(spin_locked);

	write_unlock(&(tree->dk_lock));
}

/* estimate api. Implementation is in estimate.c */
reiser4_block_nr estimate_one_insert_item(reiser4_tree *);
reiser4_block_nr estimate_one_insert_into_item(reiser4_tree *);
reiser4_block_nr estimate_insert_flow(tree_level);
reiser4_block_nr estimate_one_item_removal(reiser4_tree *);
reiser4_block_nr calc_estimate_one_insert(tree_level);
reiser4_block_nr estimate_dirty_cluster(struct inode *);
reiser4_block_nr estimate_insert_cluster(struct inode *);
reiser4_block_nr estimate_update_cluster(struct inode *);

/* __REISER4_TREE_H__ */
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
