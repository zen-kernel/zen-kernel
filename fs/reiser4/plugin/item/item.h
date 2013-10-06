/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* first read balance.c comments before reading this */

/* An item_plugin implements all of the operations required for
   balancing that are item specific. */

/* an item plugin also implements other operations that are specific to that
   item.  These go into the item specific operations portion of the item
   handler, and all of the item specific portions of the item handler are put
   into a union. */

#if !defined( __REISER4_ITEM_H__ )
#define __REISER4_ITEM_H__

#include "../../forward.h"
#include "../plugin_header.h"
#include "../../dformat.h"
#include "../../seal.h"
#include "../../plugin/file/file.h"

#include <linux/fs.h>		/* for struct file, struct inode  */
#include <linux/mm.h>		/* for struct page */
#include <linux/dcache.h>	/* for struct dentry */

typedef enum {
	STAT_DATA_ITEM_TYPE,
	DIR_ENTRY_ITEM_TYPE,
	INTERNAL_ITEM_TYPE,
	UNIX_FILE_METADATA_ITEM_TYPE,
	OTHER_ITEM_TYPE
} item_type_id;

/* this is the part of each item plugin that all items are expected to
   support or at least explicitly fail to support by setting the
   pointer to null. */
struct balance_ops {
	/* operations called by balancing

	   It is interesting to consider that some of these item
	   operations could be given sources or targets that are not
	   really items in nodes.  This could be ok/useful.

	 */
	/* maximal key that can _possibly_ be occupied by this item

	   When inserting, and node ->lookup() method (called by
	   coord_by_key()) reaches an item after binary search,
	   the  ->max_key_inside() item plugin method is used to determine
	   whether new item should pasted into existing item
	   (new_key<=max_key_inside()) or new item has to be created
	   (new_key>max_key_inside()).

	   For items that occupy exactly one key (like stat-data)
	   this method should return this key. For items that can
	   grow indefinitely (extent, directory item) this should
	   return reiser4_max_key().

	   For example extent with the key

	   (LOCALITY,4,OBJID,STARTING-OFFSET), and length BLK blocks,

	   ->max_key_inside is (LOCALITY,4,OBJID,0xffffffffffffffff), and
	 */
	reiser4_key *(*max_key_inside) (const coord_t *, reiser4_key *);

	/* true if item @coord can merge data at @key. */
	int (*can_contain_key) (const coord_t *, const reiser4_key *,
				const reiser4_item_data *);
	/* mergeable() - check items for mergeability

	   Optional method. Returns true if two items can be merged.

	 */
	int (*mergeable) (const coord_t *, const coord_t *);

	/* number of atomic things in an item.
	   NOTE FOR CONTRIBUTORS: use a generic method
	   nr_units_single_unit() for solid (atomic) items, as
	   tree operations use it as a criterion of solidness
	   (see is_solid_item macro) */
	pos_in_node_t(*nr_units) (const coord_t *);

	/* search within item for a unit within the item, and return a
	   pointer to it.  This can be used to calculate how many
	   bytes to shrink an item if you use pointer arithmetic and
	   compare to the start of the item body if the item's data
	   are continuous in the node, if the item's data are not
	   continuous in the node, all sorts of other things are maybe
	   going to break as well. */
	 lookup_result(*lookup) (const reiser4_key *, lookup_bias, coord_t *);
	/* method called by ode_plugin->create_item() to initialise new
	   item */
	int (*init) (coord_t * target, coord_t * from,
		     reiser4_item_data * data);
	/* method called (e.g., by reiser4_resize_item()) to place new data
	   into item when it grows */
	int (*paste) (coord_t *, reiser4_item_data *, carry_plugin_info *);
	/* return true if paste into @coord is allowed to skip
	   carry. That is, if such paste would require any changes
	   at the parent level
	 */
	int (*fast_paste) (const coord_t *);
	/* how many but not more than @want units of @source can be
	   shifted into @target node. If pend == append - we try to
	   append last item of @target by first units of @source. If
	   pend == prepend - we try to "prepend" first item in @target
	   by last units of @source. @target node has @free_space
	   bytes of free space. Total size of those units are returned
	   via @size.

	   @target is not NULL if shifting to the mergeable item and
	   NULL is new item will be created during shifting.
	 */
	int (*can_shift) (unsigned free_space, coord_t *,
			  znode *, shift_direction, unsigned *size,
			  unsigned want);

	/* starting off @from-th unit of item @source append or
	   prepend @count units to @target. @target has been already
	   expanded by @free_space bytes. That must be exactly what is
	   needed for those items in @target. If @where_is_free_space
	   == SHIFT_LEFT - free space is at the end of @target item,
	   othersize - it is in the beginning of it. */
	void (*copy_units) (coord_t *, coord_t *,
			    unsigned from, unsigned count,
			    shift_direction where_is_free_space,
			    unsigned free_space);

	int (*create_hook) (const coord_t *, void *);
	/* do whatever is necessary to do when @count units starting
	   from @from-th one are removed from the tree */
	/* FIXME-VS: this is used to be here for, in particular,
	   extents and items of internal type to free blocks they point
	   to at the same time with removing items from a
	   tree. Problems start, however, when dealloc_block fails due
	   to some reason. Item gets removed, but blocks it pointed to
	   are not freed. It is not clear how to fix this for items of
	   internal type because a need to remove internal item may
	   appear in the middle of balancing, and there is no way to
	   undo changes made. OTOH, if space allocator involves
	   balancing to perform dealloc_block - this will probably
	   break balancing due to deadlock issues
	 */
	int (*kill_hook) (const coord_t *, pos_in_node_t from,
			  pos_in_node_t count, struct carry_kill_data *);
	int (*shift_hook) (const coord_t *, unsigned from, unsigned count,
			   znode * _node);

	/* unit @*from contains @from_key. unit @*to contains @to_key. Cut all keys between @from_key and @to_key
	   including boundaries. When units are cut from item beginning - move space which gets freed to head of
	   item. When units are cut from item end - move freed space to item end. When units are cut from the middle of
	   item - move freed space to item head. Return amount of space which got freed. Save smallest removed key in
	   @smallest_removed if it is not 0. Save new first item key in @new_first_key if it is not 0
	 */
	int (*cut_units) (coord_t *, pos_in_node_t from, pos_in_node_t to,
			  struct carry_cut_data *,
			  reiser4_key * smallest_removed,
			  reiser4_key * new_first_key);

	/* like cut_units, except that these units are removed from the
	   tree, not only from a node */
	int (*kill_units) (coord_t *, pos_in_node_t from, pos_in_node_t to,
			   struct carry_kill_data *,
			   reiser4_key * smallest_removed,
			   reiser4_key * new_first);

	/* if @key_of_coord == 1 - returned key of coord, otherwise -
	   key of unit is returned. If @coord is not set to certain
	   unit - ERR_PTR(-ENOENT) is returned */
	reiser4_key *(*unit_key) (const coord_t *, reiser4_key *);
	reiser4_key *(*max_unit_key) (const coord_t *, reiser4_key *);
	/* estimate how much space is needed for paste @data into item at
	   @coord. if @coord==0 - estimate insertion, otherwise - estimate
	   pasting
	 */
	int (*estimate) (const coord_t *, const reiser4_item_data *);

	/* converts flow @f to item data. @coord == 0 on insert */
	int (*item_data_by_flow) (const coord_t *, const flow_t *,
				  reiser4_item_data *);

	/*void (*show) (struct seq_file *, coord_t *); */

#if REISER4_DEBUG
	/* used for debugging, every item should have here the most
	   complete possible check of the consistency of the item that
	   the inventor can construct */
	int (*check) (const coord_t *, const char **error);
#endif

};

struct flush_ops {
	/* return the right or left child of @coord, only if it is in memory */
	int (*utmost_child) (const coord_t *, sideof side, jnode ** child);

	/* return whether the right or left child of @coord has a non-fake
	   block number. */
	int (*utmost_child_real_block) (const coord_t *, sideof side,
					reiser4_block_nr *);
	/* relocate child at @coord to the @block */
	void (*update) (const coord_t *, const reiser4_block_nr *);
	/* count unformatted nodes per item for leave relocation policy, etc.. */
	int (*scan) (flush_scan * scan);
	/* convert item by flush */
	int (*convert) (flush_pos_t * pos);
	/* backward mapping from jnode offset to a key.  */
	int (*key_by_offset) (struct inode *, loff_t, reiser4_key *);
};

/* operations specific to the directory item */
struct dir_entry_iops {
	/* extract stat-data key from directory entry at @coord and place it
	   into @key. */
	int (*extract_key) (const coord_t *, reiser4_key * key);
	/* update object key in item. */
	int (*update_key) (const coord_t *, const reiser4_key *, lock_handle *);
	/* extract name from directory entry at @coord and return it */
	char *(*extract_name) (const coord_t *, char *buf);
	/* extract file type (DT_* stuff) from directory entry at @coord and
	   return it */
	unsigned (*extract_file_type) (const coord_t *);
	int (*add_entry) (struct inode * dir,
			  coord_t *, lock_handle *,
			  const struct dentry * name,
			  reiser4_dir_entry_desc * entry);
	int (*rem_entry) (struct inode * dir, const struct qstr * name,
			  coord_t *, lock_handle *,
			  reiser4_dir_entry_desc * entry);
	int (*max_name_len) (const struct inode * dir);
};

/* operations specific to items regular (unix) file metadata are built of */
struct file_iops{
	ssize_t (*write) (struct file *, struct inode *,
			  const char __user *, size_t, loff_t *pos);
	int (*read) (struct file *, flow_t *, hint_t *);
	int (*readpage) (void *, struct page *);
	int (*get_block) (const coord_t *, sector_t, sector_t *);
	/*
	 * key of first byte which is not addressed by the item @coord is set
	 * to.
	 * For example, for extent item with the key
	 *
	 * (LOCALITY,4,OBJID,STARTING-OFFSET), and length BLK blocks,
	 *
	 * ->append_key is
	 *
	 * (LOCALITY,4,OBJID,STARTING-OFFSET + BLK * block_size)
	 */
	reiser4_key *(*append_key) (const coord_t *, reiser4_key *);

	void (*init_coord_extension) (uf_coord_t *, loff_t);
};

/* operations specific to items of stat data type */
struct sd_iops {
	int (*init_inode) (struct inode * inode, char *sd, int len);
	int (*save_len) (struct inode * inode);
	int (*save) (struct inode * inode, char **area);
};

/* operations specific to internal item */
struct internal_iops{
	/* all tree traversal want to know from internal item is where
	   to go next. */
	void (*down_link) (const coord_t * coord,
			   const reiser4_key * key, reiser4_block_nr * block);
	/* check that given internal item contains given pointer. */
	int (*has_pointer_to) (const coord_t * coord,
			       const reiser4_block_nr * block);
};

struct item_plugin {
	/* generic fields */
	plugin_header h;
	/* methods common for all item types */
	struct balance_ops b; /* balance operations */
 	struct flush_ops f;   /* flush operates with items via this methods */

	/* methods specific to particular type of item */
	union {
		struct dir_entry_iops dir;
		struct      file_iops file;
		struct        sd_iops sd;
		struct  internal_iops internal;
	} s;
};

#define is_solid_item(iplug) ((iplug)->b.nr_units == nr_units_single_unit)

static inline item_id item_id_by_plugin(item_plugin * plugin)
{
	return plugin->h.id;
}

static inline char get_iplugid(item_plugin * iplug)
{
	assert("nikita-2838", iplug != NULL);
	assert("nikita-2839", iplug->h.id < 0xff);
	return (char)item_id_by_plugin(iplug);
}

extern unsigned long znode_times_locked(const znode * z);

static inline void coord_set_iplug(coord_t * coord, item_plugin * iplug)
{
	assert("nikita-2837", coord != NULL);
	assert("nikita-2838", iplug != NULL);
	coord->iplugid = get_iplugid(iplug);
	ON_DEBUG(coord->plug_v = znode_times_locked(coord->node));
}

static inline item_plugin *coord_iplug(const coord_t * coord)
{
	assert("nikita-2833", coord != NULL);
	assert("nikita-2834", coord->iplugid != INVALID_PLUGID);
	assert("nikita-3549", coord->plug_v == znode_times_locked(coord->node));
	return (item_plugin *) plugin_by_id(REISER4_ITEM_PLUGIN_TYPE,
					    coord->iplugid);
}

extern int item_can_contain_key(const coord_t * item, const reiser4_key * key,
				const reiser4_item_data *);
extern int are_items_mergeable(const coord_t * i1, const coord_t * i2);
extern int item_is_extent(const coord_t *);
extern int item_is_tail(const coord_t *);
extern int item_is_statdata(const coord_t * item);
extern int item_is_ctail(const coord_t *);

extern pos_in_node_t item_length_by_coord(const coord_t * coord);
extern pos_in_node_t nr_units_single_unit(const coord_t * coord);
extern item_id item_id_by_coord(const coord_t * coord /* coord to query */ );
extern reiser4_key *item_key_by_coord(const coord_t * coord, reiser4_key * key);
extern reiser4_key *max_item_key_by_coord(const coord_t *, reiser4_key *);
extern reiser4_key *unit_key_by_coord(const coord_t * coord, reiser4_key * key);
extern reiser4_key *max_unit_key_by_coord(const coord_t * coord,
					  reiser4_key * key);
extern void obtain_item_plugin(const coord_t * coord);

#if defined(REISER4_DEBUG)
extern int znode_is_loaded(const znode * node);
#endif

/* return plugin of item at @coord */
static inline item_plugin *item_plugin_by_coord(const coord_t *
						coord /* coord to query */ )
{
	assert("nikita-330", coord != NULL);
	assert("nikita-331", coord->node != NULL);
	assert("nikita-332", znode_is_loaded(coord->node));

	if (unlikely(!coord_is_iplug_set(coord)))
		obtain_item_plugin(coord);
	return coord_iplug(coord);
}

/* this returns true if item is of internal type */
static inline int item_is_internal(const coord_t * item)
{
	assert("vs-483", coord_is_existing_item(item));
	return plugin_of_group(item_plugin_by_coord(item), INTERNAL_ITEM_TYPE);
}

extern void item_body_by_coord_hard(coord_t * coord);
extern void *item_body_by_coord_easy(const coord_t * coord);
#if REISER4_DEBUG
extern int item_body_is_valid(const coord_t * coord);
#endif

/* return pointer to item body */
static inline void *item_body_by_coord(const coord_t *
				       coord /* coord to query */ )
{
	assert("nikita-324", coord != NULL);
	assert("nikita-325", coord->node != NULL);
	assert("nikita-326", znode_is_loaded(coord->node));

	if (coord->offset == INVALID_OFFSET)
		item_body_by_coord_hard((coord_t *) coord);
	assert("nikita-3201", item_body_is_valid(coord));
	assert("nikita-3550", coord->body_v == znode_times_locked(coord->node));
	return item_body_by_coord_easy(coord);
}

/* __REISER4_ITEM_H__ */
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
