/* Copyright 2001, 2002, 2003, 2004 by Hans Reiser, licensing governed by
 * reiser4/README */

/*
 * this file contains implementations of inode/file/address_space/file plugin
 * operations specific for "unix file plugin" (plugin id is
 * UNIX_FILE_PLUGIN_ID). "Unix file" is either built of tail items only
 * (FORMATTING_ID) or of extent items only (EXTENT_POINTER_ID) or empty (have
 * no items but stat data)
 */

#include "../../inode.h"
#include "../../super.h"
#include "../../tree_walk.h"
#include "../../carry.h"
#include "../../page_cache.h"
#include "../../ioctl.h"
#include "../object.h"
#include "../cluster.h"
#include "../../safe_link.h"

#include <linux/writeback.h>
#include <linux/pagevec.h>
#include <linux/syscalls.h>


static int unpack(struct file *file, struct inode *inode, int forever);
static void drop_access(struct unix_file_info *);
static int hint_validate(hint_t * hint, const reiser4_key * key, int check_key,
			 znode_lock_mode lock_mode);

/* Get exclusive access and make sure that file is not partially
 * converted (It may happen that another process is doing tail
 * conversion. If so, wait until it completes)
 */
static inline void get_exclusive_access_careful(struct unix_file_info * uf_info,
						struct inode *inode)
{
        do {
		get_exclusive_access(uf_info);
		if (!reiser4_inode_get_flag(inode, REISER4_PART_IN_CONV))
			break;
		drop_exclusive_access(uf_info);
		schedule();
	} while (1);
}

/* get unix file plugin specific portion of inode */
struct unix_file_info *unix_file_inode_data(const struct inode *inode)
{
	return &reiser4_inode_data(inode)->file_plugin_data.unix_file_info;
}

/**
 * equal_to_rdk - compare key and znode's right delimiting key
 * @node: node whose right delimiting key to compare with @key
 * @key: key to compare with @node's right delimiting key
 *
 * Returns true if @key is equal to right delimiting key of @node.
 */
int equal_to_rdk(znode *node, const reiser4_key *key)
{
	int result;

	read_lock_dk(znode_get_tree(node));
	result = keyeq(key, znode_get_rd_key(node));
	read_unlock_dk(znode_get_tree(node));
	return result;
}

#if REISER4_DEBUG

/**
 * equal_to_ldk - compare key and znode's left delimiting key
 * @node: node whose left delimiting key to compare with @key
 * @key: key to compare with @node's left delimiting key
 *
 * Returns true if @key is equal to left delimiting key of @node.
 */
int equal_to_ldk(znode *node, const reiser4_key *key)
{
	int result;

	read_lock_dk(znode_get_tree(node));
	result = keyeq(key, znode_get_ld_key(node));
	read_unlock_dk(znode_get_tree(node));
	return result;
}

/**
 * check_coord - check whether coord corresponds to key
 * @coord: coord to check
 * @key: key @coord has to correspond to
 *
 * Returns true if @coord is set as if it was set as result of lookup with @key
 * in coord->node.
 */
static int check_coord(const coord_t *coord, const reiser4_key *key)
{
	coord_t twin;

	node_plugin_by_node(coord->node)->lookup(coord->node, key,
						 FIND_MAX_NOT_MORE_THAN, &twin);
	return coords_equal(coord, &twin);
}

#endif /* REISER4_DEBUG */

/**
 * init_uf_coord - initialize extended coord
 * @uf_coord:
 * @lh:
 *
 *
 */
void init_uf_coord(uf_coord_t *uf_coord, lock_handle *lh)
{
	coord_init_zero(&uf_coord->coord);
	coord_clear_iplug(&uf_coord->coord);
	uf_coord->lh = lh;
	init_lh(lh);
	memset(&uf_coord->extension, 0, sizeof(uf_coord->extension));
	uf_coord->valid = 0;
}

static void validate_extended_coord(uf_coord_t *uf_coord, loff_t offset)
{
	assert("vs-1333", uf_coord->valid == 0);

	if (coord_is_between_items(&uf_coord->coord))
		return;

	assert("vs-1348",
	       item_plugin_by_coord(&uf_coord->coord)->s.file.
	       init_coord_extension);

	item_body_by_coord(&uf_coord->coord);
	item_plugin_by_coord(&uf_coord->coord)->s.file.
	    init_coord_extension(uf_coord, offset);
}

/**
 * goto_right_neighbor - lock right neighbor, drop current node lock
 * @coord:
 * @lh:
 *
 * Obtain lock on right neighbor and drop lock on current node.
 */
int goto_right_neighbor(coord_t *coord, lock_handle *lh)
{
	int result;
	lock_handle lh_right;

	assert("vs-1100", znode_is_locked(coord->node));

	init_lh(&lh_right);
	result = reiser4_get_right_neighbor(&lh_right, coord->node,
					    znode_is_wlocked(coord->node) ?
					    ZNODE_WRITE_LOCK : ZNODE_READ_LOCK,
					    GN_CAN_USE_UPPER_LEVELS);
	if (result) {
		done_lh(&lh_right);
		return result;
	}

	/*
	 * we hold two longterm locks on neighboring nodes. Unlock left of
	 * them
	 */
	done_lh(lh);

	coord_init_first_unit_nocheck(coord, lh_right.node);
	move_lh(lh, &lh_right);

	return 0;

}

/**
 * set_file_state
 * @uf_info:
 * @cbk_result:
 * @level:
 *
 * This is to be used by find_file_item and in find_file_state to
 * determine real state of file
 */
static void set_file_state(struct unix_file_info *uf_info, int cbk_result,
			   tree_level level)
{
	if (cbk_errored(cbk_result))
		/* error happened in find_file_item */
		return;

	assert("vs-1164", level == LEAF_LEVEL || level == TWIG_LEVEL);

	if (uf_info->container == UF_CONTAINER_UNKNOWN) {
		if (cbk_result == CBK_COORD_NOTFOUND)
			uf_info->container = UF_CONTAINER_EMPTY;
		else if (level == LEAF_LEVEL)
			uf_info->container = UF_CONTAINER_TAILS;
		else
			uf_info->container = UF_CONTAINER_EXTENTS;
	} else {
		/*
		 * file state is known, check whether it is set correctly if
		 * file is not being tail converted
		 */
		if (!reiser4_inode_get_flag(unix_file_info_to_inode(uf_info),
					    REISER4_PART_IN_CONV)) {
			assert("vs-1162",
			       ergo(level == LEAF_LEVEL &&
				    cbk_result == CBK_COORD_FOUND,
				    uf_info->container == UF_CONTAINER_TAILS));
			assert("vs-1165",
			       ergo(level == TWIG_LEVEL &&
				    cbk_result == CBK_COORD_FOUND,
				    uf_info->container == UF_CONTAINER_EXTENTS));
		}
	}
}

int find_file_item_nohint(coord_t *coord, lock_handle *lh,
			  const reiser4_key *key, znode_lock_mode lock_mode,
			  struct inode *inode)
{
	return reiser4_object_lookup(inode, key, coord, lh, lock_mode,
				     FIND_MAX_NOT_MORE_THAN,
				     TWIG_LEVEL, LEAF_LEVEL,
				     (lock_mode == ZNODE_READ_LOCK) ? CBK_UNIQUE :
				     (CBK_UNIQUE | CBK_FOR_INSERT),
				     NULL /* ra_info */ );
}

/**
 * find_file_item - look for file item in the tree
 * @hint: provides coordinate, lock handle, seal
 * @key: key for search
 * @mode: mode of lock to put on returned node
 * @ra_info:
 * @inode:
 *
 * This finds position in the tree corresponding to @key. It first tries to use
 * @hint's seal if it is set.
 */
int find_file_item(hint_t *hint, const reiser4_key *key,
		   znode_lock_mode lock_mode,
		   struct inode *inode)
{
	int result;
	coord_t *coord;
	lock_handle *lh;

	assert("nikita-3030", reiser4_schedulable());
	assert("vs-1707", hint != NULL);
	assert("vs-47", inode != NULL);

	coord = &hint->ext_coord.coord;
	lh = hint->ext_coord.lh;
	init_lh(lh);

	result = hint_validate(hint, key, 1 /* check key */, lock_mode);
	if (!result) {
		if (coord->between == AFTER_UNIT &&
		    equal_to_rdk(coord->node, key)) {
			result = goto_right_neighbor(coord, lh);
			if (result == -E_NO_NEIGHBOR)
				return RETERR(-EIO);
			if (result)
				return result;
			assert("vs-1152", equal_to_ldk(coord->node, key));
			/*
			 * we moved to different node. Invalidate coord
			 * extension, zload is necessary to init it again
			 */
			hint->ext_coord.valid = 0;
		}

		set_file_state(unix_file_inode_data(inode), CBK_COORD_FOUND,
			       znode_get_level(coord->node));

		return CBK_COORD_FOUND;
	}

	coord_init_zero(coord);
	result = find_file_item_nohint(coord, lh, key, lock_mode, inode);
	set_file_state(unix_file_inode_data(inode), result,
		       znode_get_level(coord->node));

	/* FIXME: we might already have coord extension initialized */
	hint->ext_coord.valid = 0;
	return result;
}

void hint_init_zero(hint_t * hint)
{
	memset(hint, 0, sizeof(*hint));
	init_lh(&hint->lh);
	hint->ext_coord.lh = &hint->lh;
}

static int find_file_state(struct inode *inode, struct unix_file_info *uf_info)
{
	int result;
	reiser4_key key;
	coord_t coord;
	lock_handle lh;

	assert("vs-1628", ea_obtained(uf_info));

	if (uf_info->container == UF_CONTAINER_UNKNOWN) {
		key_by_inode_and_offset_common(inode, 0, &key);
		init_lh(&lh);
		result = find_file_item_nohint(&coord, &lh, &key,
					       ZNODE_READ_LOCK, inode);
		set_file_state(uf_info, result, znode_get_level(coord.node));
		done_lh(&lh);
		if (!cbk_errored(result))
			result = 0;
	} else
		result = 0;
	assert("vs-1074",
	       ergo(result == 0, uf_info->container != UF_CONTAINER_UNKNOWN));
	reiser4_txn_restart_current();
	return result;
}

/**
 * Estimate and reserve space needed to truncate page
 * which gets partially truncated: one block for page
 * itself, stat-data update (estimate_one_insert_into_item)
 * and one item insertion (estimate_one_insert_into_item)
 * which may happen if page corresponds to hole extent and
 * unallocated one will have to be created
 */
static int reserve_partial_page(reiser4_tree * tree)
{
	grab_space_enable();
	return reiser4_grab_reserved(reiser4_get_current_sb(),
				     1 +
				     2 * estimate_one_insert_into_item(tree),
				     BA_CAN_COMMIT);
}

/* estimate and reserve space needed to cut one item and update one stat data */
static int reserve_cut_iteration(reiser4_tree * tree)
{
	__u64 estimate = estimate_one_item_removal(tree)
	    + estimate_one_insert_into_item(tree);

	assert("nikita-3172", lock_stack_isclean(get_current_lock_stack()));

	grab_space_enable();
	/* We need to double our estimate now that we can delete more than one
	   node. */
	return reiser4_grab_reserved(reiser4_get_current_sb(), estimate * 2,
				     BA_CAN_COMMIT);
}

int reiser4_update_file_size(struct inode *inode, loff_t new_size,
			     int update_sd)
{
	int result = 0;

	INODE_SET_SIZE(inode, new_size);
	if (update_sd) {
		inode->i_ctime = inode->i_mtime = CURRENT_TIME;
		result = reiser4_update_sd(inode);
	}
	return result;
}

/**
 * Cut file items one by one starting from the last one until
 * new file size (inode->i_size) is reached. Reserve space
 * and update file stat data on every single cut from the tree
 */
int cut_file_items(struct inode *inode, loff_t new_size,
		   int update_sd, loff_t cur_size,
		   int (*update_actor) (struct inode *, loff_t, int))
{
	reiser4_key from_key, to_key;
	reiser4_key smallest_removed;
	file_plugin *fplug = inode_file_plugin(inode);
	int result;
	int progress = 0;

	assert("vs-1248",
	       fplug == file_plugin_by_id(UNIX_FILE_PLUGIN_ID) ||
	       fplug == file_plugin_by_id(CRYPTCOMPRESS_FILE_PLUGIN_ID));

	fplug->key_by_inode(inode, new_size, &from_key);
	to_key = from_key;
	set_key_offset(&to_key, cur_size - 1 /*get_key_offset(reiser4_max_key()) */ );
	/* this loop normally runs just once */
	while (1) {
		result = reserve_cut_iteration(reiser4_tree_by_inode(inode));
		if (result)
			break;

		result = reiser4_cut_tree_object(current_tree, &from_key, &to_key,
						 &smallest_removed, inode, 1,
						 &progress);
		if (result == -E_REPEAT) {
			/**
			 * -E_REPEAT is a signal to interrupt a long
			 * file truncation process
			 */
			if (progress) {
				result = update_actor(inode,
					      get_key_offset(&smallest_removed),
					      update_sd);
				if (result)
					break;
			}
			/* the below does up(sbinfo->delete_mutex).
			 * Do not get folled */
			reiser4_release_reserved(inode->i_sb);
			/**
			 * reiser4_cut_tree_object() was interrupted probably
			 * because current atom requires commit, we have to
			 * release transaction handle to allow atom commit.
			 */
			reiser4_txn_restart_current();
			continue;
		}
		if (result
		    && !(result == CBK_COORD_NOTFOUND && new_size == 0
			 && inode->i_size == 0))
			break;

		set_key_offset(&smallest_removed, new_size);
		/* Final sd update after the file gets its correct size */
		result = update_actor(inode, get_key_offset(&smallest_removed),
				      update_sd);
		break;
	}

	/* the below does up(sbinfo->delete_mutex). Do not get folled */
	reiser4_release_reserved(inode->i_sb);

	return result;
}

int find_or_create_extent(struct page *page);

/* part of truncate_file_body: it is called when truncate is used to make file
   shorter */
static int shorten_file(struct inode *inode, loff_t new_size)
{
	int result;
	struct page *page;
	int padd_from;
	unsigned long index;
	struct unix_file_info *uf_info;

	/*
	 * all items of ordinary reiser4 file are grouped together. That is why
	 * we can use reiser4_cut_tree. Plan B files (for instance) can not be
	 * truncated that simply
	 */
	result = cut_file_items(inode, new_size, 1 /*update_sd */ ,
				get_key_offset(reiser4_max_key()),
				reiser4_update_file_size);
	if (result)
		return result;

	uf_info = unix_file_inode_data(inode);
	assert("vs-1105", new_size == inode->i_size);
	if (new_size == 0) {
		uf_info->container = UF_CONTAINER_EMPTY;
		return 0;
	}

	result = find_file_state(inode, uf_info);
	if (result)
		return result;
	if (uf_info->container == UF_CONTAINER_TAILS)
		/*
		 * No need to worry about zeroing last page after new file
		 * end
		 */
		return 0;

	padd_from = inode->i_size & (PAGE_CACHE_SIZE - 1);
	if (!padd_from)
		/* file is truncated to page boundary */
		return 0;

	result = reserve_partial_page(reiser4_tree_by_inode(inode));
	if (result) {
		reiser4_release_reserved(inode->i_sb);
		return result;
	}

	/* last page is partially truncated - zero its content */
	index = (inode->i_size >> PAGE_CACHE_SHIFT);
	page = read_mapping_page(inode->i_mapping, index, NULL);
	if (IS_ERR(page)) {
		/*
		 * the below does up(sbinfo->delete_mutex). Do not get
		 * confused
		 */
		reiser4_release_reserved(inode->i_sb);
		if (likely(PTR_ERR(page) == -EINVAL)) {
			/* looks like file is built of tail items */
			return 0;
		}
		return PTR_ERR(page);
	}
	wait_on_page_locked(page);
	if (!PageUptodate(page)) {
		page_cache_release(page);
		/*
		 * the below does up(sbinfo->delete_mutex). Do not get
		 * confused
		 */
		reiser4_release_reserved(inode->i_sb);
		return RETERR(-EIO);
	}

	/*
	 * if page correspons to hole extent unit - unallocated one will be
	 * created here. This is not necessary
	 */
	result = find_or_create_extent(page);

	/*
	 * FIXME: cut_file_items has already updated inode. Probably it would
	 * be better to update it here when file is really truncated
	 */
	if (result) {
		page_cache_release(page);
		/*
		 * the below does up(sbinfo->delete_mutex). Do not get
		 * confused
		 */
		reiser4_release_reserved(inode->i_sb);
		return result;
	}

	lock_page(page);
	assert("vs-1066", PageLocked(page));
	zero_user_segment(page, padd_from, PAGE_CACHE_SIZE);
	unlock_page(page);
	page_cache_release(page);
	/* the below does up(sbinfo->delete_mutex). Do not get confused */
	reiser4_release_reserved(inode->i_sb);
	return 0;
}

/**
 * should_have_notail
 * @uf_info:
 * @new_size:
 *
 * Calls formatting plugin to see whether file of size @new_size has to be
 * stored in unformatted nodes or in tail items. 0 is returned for later case.
 */
static int should_have_notail(const struct unix_file_info *uf_info, loff_t new_size)
{
	if (!uf_info->tplug)
		return 1;
	return !uf_info->tplug->have_tail(unix_file_info_to_inode(uf_info),
					  new_size);

}

/**
 * truncate_file_body - change length of file
 * @inode: inode of file
 * @new_size: new file length
 *
 * Adjusts items file @inode is built of to match @new_size. It may either cut
 * items or add them to represent a hole at the end of file. The caller has to
 * obtain exclusive access to the file.
 */
static int truncate_file_body(struct inode *inode, struct iattr *attr)
{
	int result;
	loff_t new_size = attr->ia_size;

	if (inode->i_size < new_size) {
		/* expanding truncate */
		struct unix_file_info *uf_info = unix_file_inode_data(inode);

		result = find_file_state(inode, uf_info);
		if (result)
			return result;

		if (should_have_notail(uf_info, new_size)) {
			/*
			 * file of size @new_size has to be built of
			 * extents. If it is built of tails - convert to
			 * extents
			 */
			if (uf_info->container ==  UF_CONTAINER_TAILS) {
				/*
				 * if file is being convered by another process
				 * - wait until it completes
				 */
				while (1) {
					if (reiser4_inode_get_flag(inode,
								   REISER4_PART_IN_CONV)) {
						drop_exclusive_access(uf_info);
						schedule();
						get_exclusive_access(uf_info);
						continue;
					}
					break;
				}

				if (uf_info->container ==  UF_CONTAINER_TAILS) {
					result = tail2extent(uf_info);
					if (result)
						return result;
				}
			}
			result = reiser4_write_extent(NULL, inode, NULL,
						      0, &new_size);
			if (result)
				return result;
			uf_info->container = UF_CONTAINER_EXTENTS;
		} else {
			if (uf_info->container ==  UF_CONTAINER_EXTENTS) {
				result = reiser4_write_extent(NULL, inode, NULL,
							      0, &new_size);
				if (result)
					return result;
			} else {
				result = reiser4_write_tail(NULL, inode, NULL,
							    0, &new_size);
				if (result)
					return result;
				uf_info->container = UF_CONTAINER_TAILS;
			}
		}
		BUG_ON(result > 0);
		result = reiser4_update_file_size(inode, new_size, 1);
		BUG_ON(result != 0);
	} else
		result = shorten_file(inode, new_size);
	return result;
}

/**
 * load_file_hint - copy hint from struct file to local variable
 * @file: file to get hint from
 * @hint: structure to fill
 *
 * Reiser4 specific portion of struct file may contain information (hint)
 * stored on exiting from previous read or write. That information includes
 * seal of znode and coord within that znode where previous read or write
 * stopped. This function copies that information to @hint if it was stored or
 * initializes @hint by 0s otherwise.
 */
int load_file_hint(struct file *file, hint_t *hint)
{
	reiser4_file_fsdata *fsdata;

	if (file) {
		fsdata = reiser4_get_file_fsdata(file);
		if (IS_ERR(fsdata))
			return PTR_ERR(fsdata);

		spin_lock_inode(file->f_dentry->d_inode);
		if (reiser4_seal_is_set(&fsdata->reg.hint.seal)) {
			memcpy(hint, &fsdata->reg.hint, sizeof(*hint));
			init_lh(&hint->lh);
			hint->ext_coord.lh = &hint->lh;
			spin_unlock_inode(file->f_dentry->d_inode);
			/*
			 * force re-validation of the coord on the first
			 * iteration of the read/write loop.
			 */
			hint->ext_coord.valid = 0;
			assert("nikita-19892",
			       coords_equal(&hint->seal.coord1,
					    &hint->ext_coord.coord));
			return 0;
		}
		memset(&fsdata->reg.hint, 0, sizeof(hint_t));
		spin_unlock_inode(file->f_dentry->d_inode);
	}
	hint_init_zero(hint);
	return 0;
}

/**
 * save_file_hint - copy hint to reiser4 private struct file's part
 * @file: file to save hint in
 * @hint: hint to save
 *
 * This copies @hint to reiser4 private part of struct file. It can help
 * speedup future accesses to the file.
 */
void save_file_hint(struct file *file, const hint_t *hint)
{
	reiser4_file_fsdata *fsdata;

	assert("edward-1337", hint != NULL);

	if (!file || !reiser4_seal_is_set(&hint->seal))
		return;
	fsdata = reiser4_get_file_fsdata(file);
	assert("vs-965", !IS_ERR(fsdata));
	assert("nikita-19891",
	       coords_equal(&hint->seal.coord1, &hint->ext_coord.coord));
	assert("vs-30", hint->lh.owner == NULL);
	spin_lock_inode(file->f_dentry->d_inode);
	fsdata->reg.hint = *hint;
	spin_unlock_inode(file->f_dentry->d_inode);
	return;
}

void reiser4_unset_hint(hint_t * hint)
{
	assert("vs-1315", hint);
	hint->ext_coord.valid = 0;
	reiser4_seal_done(&hint->seal);
	done_lh(&hint->lh);
}

/* coord must be set properly. So, that reiser4_set_hint
   has nothing to do */
void reiser4_set_hint(hint_t * hint, const reiser4_key * key,
		      znode_lock_mode mode)
{
	ON_DEBUG(coord_t * coord = &hint->ext_coord.coord);
	assert("vs-1207", WITH_DATA(coord->node, check_coord(coord, key)));

	reiser4_seal_init(&hint->seal, &hint->ext_coord.coord, key);
	hint->offset = get_key_offset(key);
	hint->mode = mode;
	done_lh(&hint->lh);
}

int hint_is_set(const hint_t * hint)
{
	return reiser4_seal_is_set(&hint->seal);
}

#if REISER4_DEBUG
static int all_but_offset_key_eq(const reiser4_key * k1, const reiser4_key * k2)
{
	return (get_key_locality(k1) == get_key_locality(k2) &&
		get_key_type(k1) == get_key_type(k2) &&
		get_key_band(k1) == get_key_band(k2) &&
		get_key_ordering(k1) == get_key_ordering(k2) &&
		get_key_objectid(k1) == get_key_objectid(k2));
}
#endif

static int
hint_validate(hint_t * hint, const reiser4_key * key, int check_key,
	      znode_lock_mode lock_mode)
{
	if (!hint || !hint_is_set(hint) || hint->mode != lock_mode)
		/* hint either not set or set by different operation */
		return RETERR(-E_REPEAT);

	assert("vs-1277", all_but_offset_key_eq(key, &hint->seal.key));

	if (check_key && get_key_offset(key) != hint->offset)
		/* hint is set for different key */
		return RETERR(-E_REPEAT);

	assert("vs-31", hint->ext_coord.lh == &hint->lh);
	return reiser4_seal_validate(&hint->seal, &hint->ext_coord.coord, key,
				     hint->ext_coord.lh, lock_mode,
				     ZNODE_LOCK_LOPRI);
}

/**
 * Look for place at twig level for extent corresponding to page,
 * call extent's writepage method to create unallocated extent if
 * it does not exist yet, initialize jnode, capture page
 */
int find_or_create_extent(struct page *page)
{
	int result;
	struct inode *inode;
	int plugged_hole;

	jnode *node;

	assert("vs-1065", page->mapping && page->mapping->host);
	inode = page->mapping->host;

	lock_page(page);
	node = jnode_of_page(page);
	if (IS_ERR(node)) {
		unlock_page(page);
		return PTR_ERR(node);
	}
	JF_SET(node, JNODE_WRITE_PREPARED);
	unlock_page(page);

	if (node->blocknr == 0) {
		plugged_hole = 0;
		result = reiser4_update_extent(inode, node, page_offset(page),
					       &plugged_hole);
		if (result) {
 			JF_CLR(node, JNODE_WRITE_PREPARED);
			jput(node);
			warning("edward-1549",
				"reiser4_update_extent failed: %d", result);
			return result;
		}
		if (plugged_hole)
			reiser4_update_sd(inode);
	} else {
		spin_lock_jnode(node);
		result = reiser4_try_capture(node, ZNODE_WRITE_LOCK, 0);
		BUG_ON(result != 0);
		jnode_make_dirty_locked(node);
		spin_unlock_jnode(node);
	}

	BUG_ON(node->atom == NULL);
	JF_CLR(node, JNODE_WRITE_PREPARED);

	if (get_current_context()->entd) {
		entd_context *ent = get_entd_context(node->tree->super);

		if (ent->cur_request->page == page)
			/* the following reference will be
			   dropped in reiser4_writeout */
			ent->cur_request->node = jref(node);
	}
	jput(node);
	return 0;
}

/**
 * has_anonymous_pages - check whether inode has pages dirtied via mmap
 * @inode: inode to check
 *
 * Returns true if inode's mapping has dirty pages which do not belong to any
 * atom. Those are either tagged PAGECACHE_TAG_REISER4_MOVED in mapping's page
 * tree or were eflushed and can be found via jnodes tagged
 * EFLUSH_TAG_ANONYMOUS in radix tree of jnodes.
 */
static int has_anonymous_pages(struct inode *inode)
{
	int result;

	spin_lock_irq(&inode->i_mapping->tree_lock);
	result = radix_tree_tagged(&inode->i_mapping->page_tree, PAGECACHE_TAG_REISER4_MOVED);
	spin_unlock_irq(&inode->i_mapping->tree_lock);
	return result;
}

/**
 * capture_page_and_create_extent -
 * @page: page to be captured
 *
 * Grabs space for extent creation and stat data update and calls function to
 * do actual work.
 * Exclusive, or non-exclusive lock must be held.
 */
static int capture_page_and_create_extent(struct page *page)
{
	int result;
	struct inode *inode;

	assert("vs-1084", page->mapping && page->mapping->host);
	inode = page->mapping->host;
	assert("vs-1139",
	       unix_file_inode_data(inode)->container == UF_CONTAINER_EXTENTS);
	/* page belongs to file */
	assert("vs-1393",
	       inode->i_size > page_offset(page));

	/* page capture may require extent creation (if it does not exist yet)
	   and stat data's update (number of blocks changes on extent
	   creation) */
	grab_space_enable();
	result = reiser4_grab_space(2 * estimate_one_insert_into_item
				    (reiser4_tree_by_inode(inode)),
				    BA_CAN_COMMIT);
	if (likely(!result))
		result = find_or_create_extent(page);

	if (result != 0)
		SetPageError(page);
	return result;
}

/*
 * Support for "anonymous" pages and jnodes.
 *
 * When file is write-accessed through mmap pages can be dirtied from the user
 * level. In this case kernel is not notified until one of following happens:
 *
 *     (1) msync()
 *
 *     (2) truncate() (either explicit or through unlink)
 *
 *     (3) VM scanner starts reclaiming mapped pages, dirtying them before
 *     starting write-back.
 *
 * As a result of (3) ->writepage may be called on a dirty page without
 * jnode. Such page is called "anonymous" in reiser4. Certain work-loads
 * (iozone) generate huge number of anonymous pages.
 *
 * reiser4_sync_sb() method tries to insert anonymous pages into
 * tree. This is done by capture_anonymous_*() functions below.
 */

/**
 * capture_anonymous_page - involve page into transaction
 * @pg: page to deal with
 *
 * Takes care that @page has corresponding metadata in the tree, creates jnode
 * for @page and captures it. On success 1 is returned.
 */
static int capture_anonymous_page(struct page *page)
{
	int result;

	if (PageWriteback(page))
		/* FIXME: do nothing? */
		return 0;

	result = capture_page_and_create_extent(page);
	if (result == 0) {
		result = 1;
	} else
		warning("nikita-3329",
				"Cannot capture anon page: %i", result);

	return result;
}

/**
 * capture_anonymous_pages - find and capture pages dirtied via mmap
 * @mapping: address space where to look for pages
 * @index: start index
 * @to_capture: maximum number of pages to capture
 *
 * Looks for pages tagged REISER4_MOVED starting from the *@index-th page,
 * captures (involves into atom) them, returns number of captured pages,
 * updates @index to next page after the last captured one.
 */
static int
capture_anonymous_pages(struct address_space *mapping, pgoff_t *index,
			unsigned int to_capture)
{
	int result;
	struct pagevec pvec;
	unsigned int i, count;
	int nr;

	pagevec_init(&pvec, 0);
	count = min(pagevec_space(&pvec), to_capture);
	nr = 0;

	/* find pages tagged MOVED */
	spin_lock_irq(&mapping->tree_lock);
	pvec.nr = radix_tree_gang_lookup_tag(&mapping->page_tree,
					     (void **)pvec.pages, *index, count,
					     PAGECACHE_TAG_REISER4_MOVED);
	if (pagevec_count(&pvec) == 0) {
		/*
		 * there are no pages tagged MOVED in mapping->page_tree
		 * starting from *index
		 */
		spin_unlock_irq(&mapping->tree_lock);
		*index = (pgoff_t)-1;
		return 0;
	}

	/* clear MOVED tag for all found pages */
	for (i = 0; i < pagevec_count(&pvec); i++) {
		page_cache_get(pvec.pages[i]);
		radix_tree_tag_clear(&mapping->page_tree, pvec.pages[i]->index,
				     PAGECACHE_TAG_REISER4_MOVED);
	}
	spin_unlock_irq(&mapping->tree_lock);


	*index = pvec.pages[i - 1]->index + 1;

	for (i = 0; i < pagevec_count(&pvec); i++) {
		result = capture_anonymous_page(pvec.pages[i]);
		if (result == 1)
			nr++;
		else {
			if (result < 0) {
				warning("vs-1454",
					"failed to capture page: "
					"result=%d, captured=%d)\n",
					result, i);

				/*
				 * set MOVED tag to all pages which left not
				 * captured
				 */
				spin_lock_irq(&mapping->tree_lock);
				for (; i < pagevec_count(&pvec); i ++) {
					radix_tree_tag_set(&mapping->page_tree,
							   pvec.pages[i]->index,
							   PAGECACHE_TAG_REISER4_MOVED);
				}
				spin_unlock_irq(&mapping->tree_lock);

				pagevec_release(&pvec);
				return result;
			} else {
				/*
				 * result == 0. capture_anonymous_page returns
				 * 0 for Writeback-ed page. Set MOVED tag on
				 * that page
				 */
				spin_lock_irq(&mapping->tree_lock);
				radix_tree_tag_set(&mapping->page_tree,
						   pvec.pages[i]->index,
						   PAGECACHE_TAG_REISER4_MOVED);
				spin_unlock_irq(&mapping->tree_lock);
				if (i == 0)
					*index = pvec.pages[0]->index;
				else
					*index = pvec.pages[i - 1]->index + 1;
			}
		}
	}
	pagevec_release(&pvec);
	return nr;
}

/**
 * capture_anonymous_jnodes - find and capture anonymous jnodes
 * @mapping: address space where to look for jnodes
 * @from: start index
 * @to: end index
 * @to_capture: maximum number of jnodes to capture
 *
 * Looks for jnodes tagged EFLUSH_TAG_ANONYMOUS in inode's tree of jnodes in
 * the range of indexes @from-@to and captures them, returns number of captured
 * jnodes, updates @from to next jnode after the last captured one.
 */
static int
capture_anonymous_jnodes(struct address_space *mapping,
			 pgoff_t *from, pgoff_t to, int to_capture)
{
	*from = to;
	return 0;
}

/*
 * Commit atom of the jnode of a page.
 */
static int sync_page(struct page *page)
{
	int result;
	do {
		jnode *node;
		txn_atom *atom;

		lock_page(page);
		node = jprivate(page);
		if (node != NULL) {
			spin_lock_jnode(node);
			atom = jnode_get_atom(node);
			spin_unlock_jnode(node);
		} else
			atom = NULL;
		unlock_page(page);
		result = reiser4_sync_atom(atom);
	} while (result == -E_REPEAT);
	/*
	 * ZAM-FIXME-HANS: document the logic of this loop, is it just to
	 * handle the case where more pages get added to the atom while we are
	 * syncing it?
	 */
	assert("nikita-3485", ergo(result == 0,
				   get_current_context()->trans->atom == NULL));
	return result;
}

/*
 * Commit atoms of pages on @pages list.
 * call sync_page for each page from mapping's page tree
 */
static int sync_page_list(struct inode *inode)
{
	int result;
	struct address_space *mapping;
	unsigned long from;	/* start index for radix_tree_gang_lookup */
	unsigned int found;	/* return value for radix_tree_gang_lookup */

	mapping = inode->i_mapping;
	from = 0;
	result = 0;
	spin_lock_irq(&mapping->tree_lock);
	while (result == 0) {
		struct page *page;

		found =
		    radix_tree_gang_lookup(&mapping->page_tree, (void **)&page,
					   from, 1);
		assert("edward-1550", found < 2);
		if (found == 0)
			break;
		/**
		 * page may not leave radix tree because it is protected from
		 * truncating by inode->i_mutex locked by sys_fsync
		 */
		page_cache_get(page);
		spin_unlock_irq(&mapping->tree_lock);

		from = page->index + 1;

		result = sync_page(page);

		page_cache_release(page);
		spin_lock_irq(&mapping->tree_lock);
	}

	spin_unlock_irq(&mapping->tree_lock);
	return result;
}

static int commit_file_atoms(struct inode *inode)
{
	int result;
	struct unix_file_info *uf_info;

	uf_info = unix_file_inode_data(inode);

	get_exclusive_access(uf_info);
	/*
	 * find what items file is made from
	 */
	result = find_file_state(inode, uf_info);
	drop_exclusive_access(uf_info);
	if (result != 0)
		return result;

	/*
	 * file state cannot change because we are under ->i_mutex
	 */
	switch (uf_info->container) {
	case UF_CONTAINER_EXTENTS:
		/* find_file_state might open join an atom */
		reiser4_txn_restart_current();
		result =
		    /*
		     * when we are called by
		     * filemap_fdatawrite->
		     *    do_writepages()->
		     *       reiser4_writepages_dispatch()
		     *
		     * inode->i_mapping->dirty_pages are spices into
		     * ->io_pages, leaving ->dirty_pages dirty.
		     *
		     * When we are called from
		     * reiser4_fsync()->sync_unix_file(), we have to
		     * commit atoms of all pages on the ->dirty_list.
		     *
		     * So for simplicity we just commit ->io_pages and
		     * ->dirty_pages.
		     */
		    sync_page_list(inode);
		break;
	case UF_CONTAINER_TAILS:
		/*
		 * NOTE-NIKITA probably we can be smarter for tails. For now
		 * just commit all existing atoms.
		 */
		result = txnmgr_force_commit_all(inode->i_sb, 0);
		break;
	case UF_CONTAINER_EMPTY:
		result = 0;
		break;
	case UF_CONTAINER_UNKNOWN:
	default:
		result = -EIO;
		break;
	}

	/*
	 * commit current transaction: there can be captured nodes from
	 * find_file_state() and finish_conversion().
	 */
	reiser4_txn_restart_current();
	return result;
}

/**
 * writepages_unix_file - writepages of struct address_space_operations
 * @mapping:
 * @wbc:
 *
 * This captures anonymous pages and anonymous jnodes. Anonymous pages are
 * pages which are dirtied via mmapping. Anonymous jnodes are ones which were
 * created by reiser4_writepage.
 */
int writepages_unix_file(struct address_space *mapping,
		     struct writeback_control *wbc)
{
	int result;
	struct unix_file_info *uf_info;
	pgoff_t pindex, jindex, nr_pages;
	long to_capture;
	struct inode *inode;

	inode = mapping->host;
	if (!has_anonymous_pages(inode)) {
		result = 0;
		goto end;
	}
	jindex = pindex = wbc->range_start >> PAGE_CACHE_SHIFT;
	result = 0;
	nr_pages = size_in_pages(i_size_read(inode));

	uf_info = unix_file_inode_data(inode);

	do {
		reiser4_context *ctx;

		if (wbc->sync_mode != WB_SYNC_ALL)
			to_capture = min(wbc->nr_to_write, CAPTURE_APAGE_BURST);
		else
			to_capture = CAPTURE_APAGE_BURST;

		ctx = reiser4_init_context(inode->i_sb);
		if (IS_ERR(ctx)) {
			result = PTR_ERR(ctx);
			break;
		}
		/* avoid recursive calls to ->sync_inodes */
		ctx->nobalance = 1;
		assert("zam-760", lock_stack_isclean(get_current_lock_stack()));
		assert("edward-1551", LOCK_CNT_NIL(inode_sem_w));
		assert("edward-1552", LOCK_CNT_NIL(inode_sem_r));

		reiser4_txn_restart_current();

		/* we have to get nonexclusive access to the file */
		if (get_current_context()->entd) {
			/*
			 * use nonblocking version of nonexclusive_access to
			 * avoid deadlock which might look like the following:
			 * process P1 holds NEA on file F1 and called entd to
			 * reclaim some memory. Entd works for P1 and is going
			 * to capture pages of file F2. To do that entd has to
			 * get NEA to F2. F2 is held by process P2 which also
			 * called entd. But entd is serving P1 at the moment
			 * and P2 has to wait. Process P3 trying to get EA to
			 * file F2. Existence of pending EA request to file F2
			 * makes impossible for entd to get NEA to file
			 * F2. Neither of these process can continue. Using
			 * nonblocking version of gettign NEA is supposed to
			 * avoid this deadlock.
			 */
			if (try_to_get_nonexclusive_access(uf_info) == 0) {
				result = RETERR(-EBUSY);
				reiser4_exit_context(ctx);
				break;
			}
		} else
			get_nonexclusive_access(uf_info);

		while (to_capture > 0) {
			pgoff_t start;

			assert("vs-1727", jindex <= pindex);
			if (pindex == jindex) {
				start = pindex;
				result =
				    capture_anonymous_pages(inode->i_mapping,
							    &pindex,
							    to_capture);
				if (result <= 0)
					break;
				to_capture -= result;
				wbc->nr_to_write -= result;
				if (start + result == pindex) {
					jindex = pindex;
					continue;
				}
				if (to_capture <= 0)
					break;
			}
			/* deal with anonymous jnodes between jindex and pindex */
			result =
			    capture_anonymous_jnodes(inode->i_mapping, &jindex,
						     pindex, to_capture);
			if (result < 0)
				break;
			to_capture -= result;
			get_current_context()->nr_captured += result;

			if (jindex == (pgoff_t) - 1) {
				assert("vs-1728", pindex == (pgoff_t) - 1);
				break;
			}
		}
		if (to_capture <= 0)
			/* there may be left more pages */
			__mark_inode_dirty(inode, I_DIRTY_PAGES);

		drop_nonexclusive_access(uf_info);
		if (result < 0) {
			/* error happened */
			reiser4_exit_context(ctx);
			return result;
		}
		if (wbc->sync_mode != WB_SYNC_ALL) {
			reiser4_exit_context(ctx);
			return 0;
		}
		result = commit_file_atoms(inode);
		reiser4_exit_context(ctx);
		if (pindex >= nr_pages && jindex == pindex)
			break;
	} while (1);

      end:
	if (is_in_reiser4_context()) {
		if (get_current_context()->nr_captured >= CAPTURE_APAGE_BURST) {
			/*
			 * there are already pages to flush, flush them out, do
			 * not delay until end of reiser4_sync_inodes
			 */
			reiser4_writeout(inode->i_sb, wbc);
			get_current_context()->nr_captured = 0;
		}
	}
	return result;
}

/**
 * readpage_unix_file_nolock - readpage of struct address_space_operations
 * @file:
 * @page:
 *
 * Compose a key and search for item containing information about @page
 * data. If item is found - its readpage method is called.
 */
int readpage_unix_file(struct file *file, struct page *page)
{
	reiser4_context *ctx;
	int result;
	struct inode *inode;
	reiser4_key key;
	item_plugin *iplug;
	hint_t *hint;
	lock_handle *lh;
	coord_t *coord;

	assert("vs-1062", PageLocked(page));
	assert("vs-976", !PageUptodate(page));
	assert("vs-1061", page->mapping && page->mapping->host);

	if (page->mapping->host->i_size <= page_offset(page)) {
		/* page is out of file */
		zero_user(page, 0, PAGE_CACHE_SIZE);
		SetPageUptodate(page);
		unlock_page(page);
		return 0;
	}

	inode = page->mapping->host;
	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx)) {
		unlock_page(page);
		return PTR_ERR(ctx);
	}

	hint = kmalloc(sizeof(*hint), reiser4_ctx_gfp_mask_get());
	if (hint == NULL) {
		unlock_page(page);
		reiser4_exit_context(ctx);
		return RETERR(-ENOMEM);
	}

	result = load_file_hint(file, hint);
	if (result) {
		kfree(hint);
		unlock_page(page);
		reiser4_exit_context(ctx);
		return result;
	}
	lh = &hint->lh;

	/* get key of first byte of the page */
	key_by_inode_and_offset_common(inode, page_offset(page), &key);

	/* look for file metadata corresponding to first byte of page */
	page_cache_get(page);
	unlock_page(page);
	result = find_file_item(hint, &key, ZNODE_READ_LOCK, inode);
	lock_page(page);
	page_cache_release(page);

	if (page->mapping == NULL) {
		/*
		 * readpage allows truncate to run concurrently. Page was
		 * truncated while it was not locked
		 */
		done_lh(lh);
		kfree(hint);
		unlock_page(page);
		reiser4_txn_restart(ctx);
		reiser4_exit_context(ctx);
		return -EINVAL;
	}

	if (result != CBK_COORD_FOUND || hint->ext_coord.coord.between != AT_UNIT) {
		if (result == CBK_COORD_FOUND &&
		    hint->ext_coord.coord.between != AT_UNIT)
			/* file is truncated */
			result = -EINVAL;
		done_lh(lh);
		kfree(hint);
		unlock_page(page);
		reiser4_txn_restart(ctx);
		reiser4_exit_context(ctx);
		return result;
	}

	/*
	 * item corresponding to page is found. It can not be removed because
	 * znode lock is held
	 */
	if (PageUptodate(page)) {
		done_lh(lh);
		kfree(hint);
		unlock_page(page);
		reiser4_txn_restart(ctx);
		reiser4_exit_context(ctx);
		return 0;
	}

	coord = &hint->ext_coord.coord;
	result = zload(coord->node);
	if (result) {
		done_lh(lh);
		kfree(hint);
		unlock_page(page);
		reiser4_txn_restart(ctx);
		reiser4_exit_context(ctx);
		return result;
	}

	validate_extended_coord(&hint->ext_coord, page_offset(page));

	if (!coord_is_existing_unit(coord)) {
		/* this indicates corruption */
		warning("vs-280",
			"Looking for page %lu of file %llu (size %lli). "
			"No file items found (%d). File is corrupted?\n",
			page->index, (unsigned long long)get_inode_oid(inode),
			inode->i_size, result);
		zrelse(coord->node);
		done_lh(lh);
		kfree(hint);
		unlock_page(page);
		reiser4_txn_restart(ctx);
		reiser4_exit_context(ctx);
		return RETERR(-EIO);
	}

	/*
	 * get plugin of found item or use plugin if extent if there are no
	 * one
	 */
	iplug = item_plugin_by_coord(coord);
	if (iplug->s.file.readpage)
		result = iplug->s.file.readpage(coord, page);
	else
		result = RETERR(-EINVAL);

	if (!result) {
		set_key_offset(&key,
			       (loff_t) (page->index + 1) << PAGE_CACHE_SHIFT);
		/* FIXME should call reiser4_set_hint() */
		reiser4_unset_hint(hint);
	} else {
		unlock_page(page);
		reiser4_unset_hint(hint);
	}
	assert("vs-979",
	       ergo(result == 0, (PageLocked(page) || PageUptodate(page))));
	assert("vs-9791", ergo(result != 0, !PageLocked(page)));

	zrelse(coord->node);
	done_lh(lh);

	save_file_hint(file, hint);
	kfree(hint);

	/*
	 * FIXME: explain why it is needed. HINT: page allocation in write can
	 * not be done when atom is not NULL because reiser4_writepage can not
	 * kick entd and have to eflush
	 */
	reiser4_txn_restart(ctx);
	reiser4_exit_context(ctx);
	return result;
}

struct uf_readpages_context {
	lock_handle lh;
	coord_t coord;
};

/*
 * A callback function for readpages_unix_file/read_cache_pages.
 * We don't take non-exclusive access. If an item different from
 * extent pointer is found in some iteration, then return error
 * (-EINVAL).
 *
 * @data -- a pointer to reiser4_readpages_context object,
 *            to save the twig lock and the coord between
 *            read_cache_page iterations.
 * @page -- page to start read.
 */
static int readpages_filler(void * data, struct page * page)
{
	struct uf_readpages_context *rc = data;
	jnode * node;
	int ret = 0;
	reiser4_extent *ext;
	__u64 ext_index;
	int cbk_done = 0;
	struct address_space *mapping = page->mapping;

	if (PageUptodate(page)) {
		unlock_page(page);
		return 0;
	}
	page_cache_get(page);

	if (rc->lh.node == 0) {
		/* no twig lock  - have to do tree search. */
		reiser4_key key;
	repeat:
		unlock_page(page);
		key_by_inode_and_offset_common(
			mapping->host, page_offset(page), &key);
		ret = coord_by_key(
			&get_super_private(mapping->host->i_sb)->tree,
			&key, &rc->coord, &rc->lh,
			ZNODE_READ_LOCK, FIND_EXACT,
			TWIG_LEVEL, TWIG_LEVEL, CBK_UNIQUE, NULL);
		if (unlikely(ret))
			goto exit;
		lock_page(page);
		if (PageUptodate(page))
			goto unlock;
		cbk_done = 1;
	}
	ret = zload(rc->coord.node);
	if (unlikely(ret))
		goto unlock;
	if (!coord_is_existing_item(&rc->coord)) {
		zrelse(rc->coord.node);
		ret = RETERR(-ENOENT);
		goto unlock;
	}
	if (!item_is_extent(&rc->coord)) {
		/*
		 * ->readpages() is not
		 * defined for tail items
		 */
		zrelse(rc->coord.node);
		ret = RETERR(-EINVAL);
		goto unlock;
	}
	ext = extent_by_coord(&rc->coord);
	ext_index = extent_unit_index(&rc->coord);
	if (page->index < ext_index ||
	    page->index >= ext_index + extent_get_width(ext)) {
		/* the page index doesn't belong to the extent unit
		   which the coord points to - release the lock and
		   repeat with tree search. */
		zrelse(rc->coord.node);
		done_lh(&rc->lh);
		/* we can be here after a CBK call only in case of
		   corruption of the tree or the tree lookup algorithm bug. */
		if (unlikely(cbk_done)) {
			ret = RETERR(-EIO);
			goto unlock;
		}
		goto repeat;
	}
	node = jnode_of_page(page);
	if (unlikely(IS_ERR(node))) {
		zrelse(rc->coord.node);
		ret = PTR_ERR(node);
		goto unlock;
	}
	ret = reiser4_do_readpage_extent(ext, page->index - ext_index, page);
	jput(node);
	zrelse(rc->coord.node);
	if (likely(!ret))
		goto exit;
 unlock:
	unlock_page(page);
 exit:
	page_cache_release(page);
	return ret;
}

/**
 * readpages_unix_file - called by the readahead code, starts reading for each
 * page of given list of pages
 */
int readpages_unix_file(struct file *file, struct address_space *mapping,
			struct list_head *pages, unsigned nr_pages)
{
	reiser4_context *ctx;
	struct uf_readpages_context rc;
	int ret;

	ctx = reiser4_init_context(mapping->host->i_sb);
	if (IS_ERR(ctx)) {
		put_pages_list(pages);
		return PTR_ERR(ctx);
	}
	init_lh(&rc.lh);
	ret = read_cache_pages(mapping, pages,  readpages_filler, &rc);
	done_lh(&rc.lh);

	context_set_commit_async(ctx);
	/* close the transaction to protect further page allocation from deadlocks */
	reiser4_txn_restart(ctx);
	reiser4_exit_context(ctx);
	return ret;
}

static reiser4_block_nr unix_file_estimate_read(struct inode *inode,
						loff_t count UNUSED_ARG)
{
	/* We should reserve one block, because of updating of the stat data
	   item */
	assert("vs-1249",
	       inode_file_plugin(inode)->estimate.update ==
	       estimate_update_common);
	return estimate_update_common(inode);
}

/* this is called with nonexclusive access obtained,
   file's container can not change */
static ssize_t do_read_compound_file(hint_t *hint, struct file *file,
				     char __user *buf, size_t count,
				     loff_t *off)
{
	int result;
	struct inode *inode;
	flow_t flow;
	coord_t *coord;
	znode *loaded;

	inode = file->f_dentry->d_inode;

	/* build flow */
	assert("vs-1250",
	       inode_file_plugin(inode)->flow_by_inode ==
	       flow_by_inode_unix_file);
	result = flow_by_inode_unix_file(inode, buf, 1 /* user space */,
					 count, *off, READ_OP, &flow);
	if (unlikely(result))
		return result;

	/* get seal and coord sealed with it from reiser4 private data
	   of struct file.  The coord will tell us where our last read
	   of this file finished, and the seal will help to determine
	   if that location is still valid.
	 */
	coord = &hint->ext_coord.coord;
	while (flow.length && result == 0) {
		result = find_file_item(hint, &flow.key,
					ZNODE_READ_LOCK, inode);
		if (cbk_errored(result))
			/* error happened */
			break;

		if (coord->between != AT_UNIT) {
			/* there were no items corresponding to given offset */
			done_lh(hint->ext_coord.lh);
			break;
		}

		loaded = coord->node;
		result = zload(loaded);
		if (unlikely(result)) {
			done_lh(hint->ext_coord.lh);
			break;
		}

		if (hint->ext_coord.valid == 0)
			validate_extended_coord(&hint->ext_coord,
						get_key_offset(&flow.key));

		assert("vs-4", hint->ext_coord.valid == 1);
		assert("vs-33", hint->ext_coord.lh == &hint->lh);
		/* call item's read method */
		result = item_plugin_by_coord(coord)->s.file.read(file,
								  &flow,
								  hint);
		zrelse(loaded);
		done_lh(hint->ext_coord.lh);
	}
	return (count - flow.length) ? (count - flow.length) : result;
}

static ssize_t read_compound_file(struct file*, char __user*, size_t, loff_t*);

/**
 * unix-file specific ->read() method
 * of struct file_operations.
 */
ssize_t read_unix_file(struct file *file, char __user *buf,
		       size_t read_amount, loff_t *off)
{
	reiser4_context *ctx;
	ssize_t result;
	struct inode *inode;
	struct unix_file_info *uf_info;

	if (unlikely(read_amount == 0))
		return 0;

	inode = file->f_dentry->d_inode;
	assert("vs-972", !reiser4_inode_get_flag(inode, REISER4_NO_SD));

	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	result = reiser4_grab_space_force(unix_file_estimate_read(inode,
					  read_amount), BA_CAN_COMMIT);
	if (unlikely(result != 0))
		goto out2;

	uf_info = unix_file_inode_data(inode);

	if (uf_info->container == UF_CONTAINER_UNKNOWN) {
		get_exclusive_access(uf_info);
		result = find_file_state(inode, uf_info);
		if (unlikely(result != 0))
			goto out;
	}
	else
		get_nonexclusive_access(uf_info);

	switch (uf_info->container) {
	case UF_CONTAINER_EXTENTS:
		if (!reiser4_inode_get_flag(inode, REISER4_PART_MIXED)) {
			result = do_sync_read(file, buf, read_amount, off);
			break;
		}
	case UF_CONTAINER_TAILS:
	case UF_CONTAINER_UNKNOWN:
		result = read_compound_file(file, buf, read_amount, off);
		break;
	case UF_CONTAINER_EMPTY:
		result = 0;
	}
 out:
	drop_access(uf_info);
 out2:
	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);
	return result;
}

/*
 * Read a file, which contains tails and, maybe,
 * extents.
 *
 * Sometimes file can consist of items of both types
 * (extents and tails). It can happen, e.g. because
 * of failed tail conversion. Also the conversion code
 * may release exclusive lock before calling
 * balance_dirty_pages().
 *
 * In this case applying a generic VFS library function
 * would be suboptimal. We use our own "light-weigth"
 * version below.
 */
static ssize_t read_compound_file(struct file *file, char __user *buf,
				  size_t count, loff_t *off)
{
	ssize_t result = 0;
	struct inode *inode;
	hint_t *hint;
	struct unix_file_info *uf_info;
	size_t to_read;
	size_t was_read = 0;
	loff_t i_size;

	inode = file->f_dentry->d_inode;
	assert("vs-972", !reiser4_inode_get_flag(inode, REISER4_NO_SD));

	i_size = i_size_read(inode);
	if (*off >= i_size)
		/* position to read from is past the end of file */
		goto exit;
	if (*off + count > i_size)
		count = i_size - *off;

	hint = kmalloc(sizeof(*hint), reiser4_ctx_gfp_mask_get());
	if (hint == NULL)
		return RETERR(-ENOMEM);

	result = load_file_hint(file, hint);
	if (result) {
		kfree(hint);
		return result;
	}
	uf_info = unix_file_inode_data(inode);

	/* read by page-aligned chunks */
	to_read = PAGE_CACHE_SIZE - (*off & (loff_t)(PAGE_CACHE_SIZE - 1));
	if (to_read > count)
		to_read = count;
	while (count > 0) {
		reiser4_txn_restart_current();
		/*
		 * faultin user page
		 */
		result = fault_in_pages_writeable(buf, to_read);
		if (result)
			return RETERR(-EFAULT);

		result = do_read_compound_file(hint, file, buf, to_read, off);
		if (result < 0)
			break;
		count -= result;
		buf += result;

		/* update position in a file */
		*off += result;
		/* total number of read bytes */
		was_read += result;
		to_read = count;
		if (to_read > PAGE_CACHE_SIZE)
			to_read = PAGE_CACHE_SIZE;
	}
	done_lh(&hint->lh);
	save_file_hint(file, hint);
	kfree(hint);
	if (was_read)
		file_accessed(file);
 exit:
	return was_read ? was_read : result;
}

/* This function takes care about @file's pages. First of all it checks if
   filesystems readonly and if so gets out. Otherwise, it throws out all
   pages of file if it was mapped for read and going to be mapped for write
   and consists of tails. This is done in order to not manage few copies
   of the data (first in page cache and second one in tails them selves)
   for the case of mapping files consisting tails.

   Here also tail2extent conversion is performed if it is allowed and file
   is going to be written or mapped for write. This functions may be called
   from write_unix_file() or mmap_unix_file(). */
static int check_pages_unix_file(struct file *file, struct inode *inode)
{
	reiser4_invalidate_pages(inode->i_mapping, 0,
				 (inode->i_size + PAGE_CACHE_SIZE -
				  1) >> PAGE_CACHE_SHIFT, 0);
	return unpack(file, inode, 0 /* not forever */ );
}

/**
 * mmap_unix_file - mmap of struct file_operations
 * @file: file to mmap
 * @vma:
 *
 * This is implementation of vfs's mmap method of struct file_operations for
 * unix file plugin. It converts file to extent if necessary. Sets
 * reiser4_inode's flag - REISER4_HAS_MMAP.
 */
int mmap_unix_file(struct file *file, struct vm_area_struct *vma)
{
	reiser4_context *ctx;
	int result;
	struct inode *inode;
	struct unix_file_info *uf_info;
	reiser4_block_nr needed;

	inode = file->f_dentry->d_inode;
	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	uf_info = unix_file_inode_data(inode);

	get_exclusive_access_careful(uf_info, inode);

	if (!IS_RDONLY(inode) && (vma->vm_flags & (VM_MAYWRITE | VM_SHARED))) {
		/*
		 * we need file built of extent items. If it is still built of
		 * tail items we have to convert it. Find what items the file
		 * is built of
		 */
		result = find_file_state(inode, uf_info);
		if (result != 0) {
			drop_exclusive_access(uf_info);
			reiser4_exit_context(ctx);
			return result;
		}

		assert("vs-1648", (uf_info->container == UF_CONTAINER_TAILS ||
				   uf_info->container == UF_CONTAINER_EXTENTS ||
				   uf_info->container == UF_CONTAINER_EMPTY));
		if (uf_info->container == UF_CONTAINER_TAILS) {
			/*
			 * invalidate all pages and convert file from tails to
			 * extents
			 */
			result = check_pages_unix_file(file, inode);
			if (result) {
				drop_exclusive_access(uf_info);
				reiser4_exit_context(ctx);
				return result;
			}
		}
	}

	/*
	 * generic_file_mmap will do update_atime. Grab space for stat data
	 * update.
	 */
	needed = inode_file_plugin(inode)->estimate.update(inode);
	result = reiser4_grab_space_force(needed, BA_CAN_COMMIT);
	if (result) {
		drop_exclusive_access(uf_info);
		reiser4_exit_context(ctx);
		return result;
	}

	result = generic_file_mmap(file, vma);
	if (result == 0) {
		/* mark file as having mapping. */
		reiser4_inode_set_flag(inode, REISER4_HAS_MMAP);
	}

	drop_exclusive_access(uf_info);
	reiser4_exit_context(ctx);
	return result;
}

/**
 * find_first_item
 * @inode:
 *
 * Finds file item which is responsible for first byte in the file.
 */
static int find_first_item(struct inode *inode)
{
	coord_t coord;
	lock_handle lh;
	reiser4_key key;
	int result;

	coord_init_zero(&coord);
	init_lh(&lh);
	inode_file_plugin(inode)->key_by_inode(inode, 0, &key);
	result = find_file_item_nohint(&coord, &lh, &key, ZNODE_READ_LOCK,
				       inode);
	if (result == CBK_COORD_FOUND) {
		if (coord.between == AT_UNIT) {
			result = zload(coord.node);
			if (result == 0) {
				result = item_id_by_coord(&coord);
				zrelse(coord.node);
				if (result != EXTENT_POINTER_ID &&
				    result != FORMATTING_ID)
					result = RETERR(-EIO);
			}
		} else
			result = RETERR(-EIO);
	}
	done_lh(&lh);
	return result;
}

/**
 * open_unix_file
 * @inode:
 * @file:
 *
 * If filesystem is not readonly - complete uncompleted tail conversion if
 * there was one
 */
int open_unix_file(struct inode *inode, struct file *file)
{
	int result;
	reiser4_context *ctx;
	struct unix_file_info *uf_info;

	if (IS_RDONLY(inode))
		return 0;

	if (!reiser4_inode_get_flag(inode, REISER4_PART_MIXED))
		return 0;

	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	uf_info = unix_file_inode_data(inode);

	get_exclusive_access_careful(uf_info, inode);

	if (!reiser4_inode_get_flag(inode, REISER4_PART_MIXED)) {
		/*
		 * other process completed the conversion
		 */
		drop_exclusive_access(uf_info);
		reiser4_exit_context(ctx);
		return 0;
	}

	/*
	 * file left in semi converted state after unclean shutdown or another
	 * thread is doing conversion and dropped exclusive access which doing
	 * balance dirty pages. Complete the conversion
	 */
	result = find_first_item(inode);
	if (result == EXTENT_POINTER_ID)
		/*
		 * first item is extent, therefore there was incomplete
		 * tail2extent conversion. Complete it
		 */
		result = tail2extent(unix_file_inode_data(inode));
	else if (result == FORMATTING_ID)
		/*
		 * first item is formatting item, therefore there was
		 * incomplete extent2tail conversion. Complete it
		 */
		result = extent2tail(file, unix_file_inode_data(inode));
	else
		result = -EIO;

	assert("vs-1712",
	       ergo(result == 0,
		    (!reiser4_inode_get_flag(inode, REISER4_PART_MIXED) &&
		     !reiser4_inode_get_flag(inode, REISER4_PART_IN_CONV))));
	drop_exclusive_access(uf_info);
	reiser4_exit_context(ctx);
	return result;
}

#define NEITHER_OBTAINED 0
#define EA_OBTAINED 1
#define NEA_OBTAINED 2

static void drop_access(struct unix_file_info *uf_info)
{
	if (uf_info->exclusive_use)
		drop_exclusive_access(uf_info);
	else
		drop_nonexclusive_access(uf_info);
}

#define debug_wuf(format, ...) printk("%s: %d: %s: " format "\n", \
			      __FILE__, __LINE__, __FUNCTION__, ## __VA_ARGS__)

/**
 * write_unix_file - private ->write() method of unix_file plugin.
 *
 * @file: file to write to
 * @buf: address of user-space buffer
 * @count: number of bytes to write
 * @pos: position in file to write to
 * @cont: unused argument, as we don't perform plugin conversion when being
 * managed by unix_file plugin.
 */
ssize_t write_unix_file(struct file *file,
			const char __user *buf,
			size_t count, loff_t *pos,
			struct dispatch_context *cont)
{
	int result;
	reiser4_context *ctx;
	struct inode *inode;
	struct unix_file_info *uf_info;
	ssize_t written;
	int to_write = PAGE_CACHE_SIZE * WRITE_GRANULARITY;
	size_t left;
	ssize_t (*write_op)(struct file *, struct inode *,
			    const char __user *, size_t,
			    loff_t *pos);
	int ea;
	int enospc = 0; /* item plugin ->write() returned ENOSPC */
	loff_t new_size;

	ctx = get_current_context();
	inode = file->f_dentry->d_inode;

	assert("vs-947", !reiser4_inode_get_flag(inode, REISER4_NO_SD));
	assert("vs-9471", (!reiser4_inode_get_flag(inode, REISER4_PART_MIXED)));

	/* check amount of bytes to write and writing position */
	result = generic_write_checks(file, pos, &count, 0);
	if (result) {
		context_set_commit_async(ctx);
		return result;
	}

	result = file_remove_suid(file);
	if (result) {
		context_set_commit_async(ctx);
		return result;
	}
	/* remove_suid might create a transaction */
	reiser4_txn_restart(ctx);

	uf_info = unix_file_inode_data(inode);

	written = 0;
	left = count;
	ea = NEITHER_OBTAINED;
	enospc = 0;

	new_size = i_size_read(inode);
	if (*pos + count > new_size)
		new_size = *pos + count;

	while (left) {
		int update_sd = 0;
		if (left < to_write)
			to_write = left;

		if (uf_info->container == UF_CONTAINER_EMPTY) {
			get_exclusive_access(uf_info);
			ea = EA_OBTAINED;
			if (uf_info->container != UF_CONTAINER_EMPTY) {
				/* file is made not empty by another process */
				drop_exclusive_access(uf_info);
				ea = NEITHER_OBTAINED;
				continue;
			}
		} else if (uf_info->container == UF_CONTAINER_UNKNOWN) {
			/*
			 * get exclusive access directly just to not have to
			 * re-obtain it if file will appear empty
			 */
			get_exclusive_access(uf_info);
			ea = EA_OBTAINED;
			result = find_file_state(inode, uf_info);
			if (result) {
				drop_exclusive_access(uf_info);
				ea = NEITHER_OBTAINED;
				break;
			}
		} else {
			get_nonexclusive_access(uf_info);
			ea = NEA_OBTAINED;
		}

		/* either EA or NEA is obtained. Choose item write method */
		if (uf_info->container == UF_CONTAINER_EXTENTS) {
			/* file is built of extent items */
			write_op = reiser4_write_extent;
		} else if (uf_info->container == UF_CONTAINER_EMPTY) {
			/* file is empty */
			if (should_have_notail(uf_info, new_size))
				write_op = reiser4_write_extent;
			else
				write_op = reiser4_write_tail;
		} else {
			/* file is built of tail items */
			if (should_have_notail(uf_info, new_size)) {
				if (ea == NEA_OBTAINED) {
					drop_nonexclusive_access(uf_info);
					get_exclusive_access(uf_info);
					ea = EA_OBTAINED;
				}
				if (uf_info->container == UF_CONTAINER_TAILS) {
					/*
					 * if file is being convered by another
					 * process - wait until it completes
					 */
					while (1) {
						if (reiser4_inode_get_flag(inode,
									   REISER4_PART_IN_CONV)) {
							drop_exclusive_access(uf_info);
							schedule();
							get_exclusive_access(uf_info);
							continue;
						}
						break;
					}
					if (uf_info->container ==  UF_CONTAINER_TAILS) {
						result = tail2extent(uf_info);
						if (result) {
							drop_exclusive_access(uf_info);
							context_set_commit_async(ctx);
							break;
						}
					}
				}
				drop_exclusive_access(uf_info);
				ea = NEITHER_OBTAINED;
				continue;
			}
			write_op = reiser4_write_tail;
		}

		written = write_op(file, inode, buf, to_write, pos);
		if (written == -ENOSPC && !enospc) {
			drop_access(uf_info);
			txnmgr_force_commit_all(inode->i_sb, 0);
			enospc = 1;
			continue;
		}
		if (written < 0) {
			/*
			 * If this is -ENOSPC, then it happened
			 * second time, so don't try to free space
			 * once again.
			 */
			drop_access(uf_info);
			result = written;
			break;
		}
		/* something is written. */
		if (enospc)
			enospc = 0;
		if (uf_info->container == UF_CONTAINER_EMPTY) {
			assert("edward-1553", ea == EA_OBTAINED);
			uf_info->container =
				(write_op == reiser4_write_extent) ?
				UF_CONTAINER_EXTENTS : UF_CONTAINER_TAILS;
		}
		assert("edward-1554",
		       ergo(uf_info->container == UF_CONTAINER_EXTENTS,
			    write_op == reiser4_write_extent));
		assert("edward-1555",
		       ergo(uf_info->container == UF_CONTAINER_TAILS,
			    write_op == reiser4_write_tail));
		if (*pos + written > inode->i_size) {
			INODE_SET_FIELD(inode, i_size, *pos + written);
			update_sd = 1;
		}
		if (!IS_NOCMTIME(inode)) {
			inode->i_ctime = inode->i_mtime = CURRENT_TIME;
			update_sd = 1;
		}
		if (update_sd) {
			/*
			 * space for update_sd was reserved in write_op
			 */
			result = reiser4_update_sd(inode);
			if (result) {
				warning("edward-1574",
					"Can not update stat-data: %i. FSCK?",
					result);
				drop_access(uf_info);
				context_set_commit_async(ctx);
				break;
			}
		}
		drop_access(uf_info);
		ea = NEITHER_OBTAINED;

		/*
		 * tell VM how many pages were dirtied. Maybe number of pages
		 * which were dirty already should not be counted
		 */
		reiser4_throttle_write(inode);
		left -= written;
		buf += written;
		*pos += written;
	}
	if (result == 0 && ((file->f_flags & O_SYNC) || IS_SYNC(inode))) {
		reiser4_txn_restart_current();
		grab_space_enable();
		result = reiser4_sync_file_common(file, 0, LONG_MAX,
						  0 /* data and stat data */);
		if (result)
			warning("reiser4-7", "failed to sync file %llu",
				(unsigned long long)get_inode_oid(inode));
	}
	/*
	 * return number of written bytes or error code if nothing is
	 * written. Note, that it does not work correctly in case when
	 * sync_unix_file returns error
	 */
	return (count - left) ? (count - left) : result;
}

/**
 * release_unix_file - release of struct file_operations
 * @inode: inode of released file
 * @file: file to release
 *
 * Implementation of release method of struct file_operations for unix file
 * plugin. If last reference to indode is released - convert all extent items
 * into tail items if necessary. Frees reiser4 specific file data.
 */
int release_unix_file(struct inode *inode, struct file *file)
{
	reiser4_context *ctx;
	struct unix_file_info *uf_info;
	int result;
	int in_reiser4;

	in_reiser4 = is_in_reiser4_context();

	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	result = 0;
	if (in_reiser4 == 0) {
		uf_info = unix_file_inode_data(inode);

		get_exclusive_access_careful(uf_info, inode);
		if (file->f_dentry->d_lockref.count == 1 &&
		    uf_info->container == UF_CONTAINER_EXTENTS &&
		    !should_have_notail(uf_info, inode->i_size) &&
		    !rofs_inode(inode)) {
			result = extent2tail(file, uf_info);
			if (result != 0) {
				context_set_commit_async(ctx);
				warning("nikita-3233",
					"Failed (%d) to convert in %s (%llu)",
					result, __FUNCTION__,
					(unsigned long long)
					get_inode_oid(inode));
			}
		}
		drop_exclusive_access(uf_info);
	} else {
		/*
		   we are within reiser4 context already. How latter is
		   possible? Simple:

		   (gdb) bt
		   #0  get_exclusive_access ()
		   #2  0xc01e56d3 in release_unix_file ()
		   #3  0xc01c3643 in reiser4_release ()
		   #4  0xc014cae0 in __fput ()
		   #5  0xc013ffc3 in remove_vm_struct ()
		   #6  0xc0141786 in exit_mmap ()
		   #7  0xc0118480 in mmput ()
		   #8  0xc0133205 in oom_kill ()
		   #9  0xc01332d1 in out_of_memory ()
		   #10 0xc013bc1d in try_to_free_pages ()
		   #11 0xc013427b in __alloc_pages ()
		   #12 0xc013f058 in do_anonymous_page ()
		   #13 0xc013f19d in do_no_page ()
		   #14 0xc013f60e in handle_mm_fault ()
		   #15 0xc01131e5 in do_page_fault ()
		   #16 0xc0104935 in error_code ()
		   #17 0xc025c0c6 in __copy_to_user_ll ()
		   #18 0xc01d496f in reiser4_read_tail ()
		   #19 0xc01e4def in read_unix_file ()
		   #20 0xc01c3504 in reiser4_read ()
		   #21 0xc014bd4f in vfs_read ()
		   #22 0xc014bf66 in sys_read ()
		 */
		warning("vs-44", "out of memory?");
	}

	reiser4_free_file_fsdata(file);

	reiser4_exit_context(ctx);
	return result;
}

static void set_file_notail(struct inode *inode)
{
	reiser4_inode *state;
	formatting_plugin *tplug;

	state = reiser4_inode_data(inode);
	tplug = formatting_plugin_by_id(NEVER_TAILS_FORMATTING_ID);
	force_plugin_pset(inode, PSET_FORMATTING, (reiser4_plugin *)tplug);
}

/* if file is built of tails - convert it to extents */
static int unpack(struct file *filp, struct inode *inode, int forever)
{
	int result = 0;
	struct unix_file_info *uf_info;

	uf_info = unix_file_inode_data(inode);
	assert("vs-1628", ea_obtained(uf_info));

	result = find_file_state(inode, uf_info);
	if (result)
		return result;
	assert("vs-1074", uf_info->container != UF_CONTAINER_UNKNOWN);

	if (uf_info->container == UF_CONTAINER_TAILS) {
		/*
		 * if file is being convered by another process - wait until it
		 * completes
		 */
		while (1) {
			if (reiser4_inode_get_flag(inode,
						   REISER4_PART_IN_CONV)) {
				drop_exclusive_access(uf_info);
				schedule();
				get_exclusive_access(uf_info);
				continue;
			}
			break;
		}
		if (uf_info->container == UF_CONTAINER_TAILS) {
			result = tail2extent(uf_info);
			if (result)
				return result;
		}
	}
	if (forever) {
		/* safe new formatting plugin in stat data */
		__u64 tograb;

		set_file_notail(inode);

		grab_space_enable();
		tograb = inode_file_plugin(inode)->estimate.update(inode);
		result = reiser4_grab_space(tograb, BA_CAN_COMMIT);
		result = reiser4_update_sd(inode);
	}

	return result;
}

/* implentation of vfs' ioctl method of struct file_operations for unix file
   plugin
*/
int ioctl_unix_file(struct file *filp, unsigned int cmd,
		    unsigned long arg UNUSED_ARG)
{
	reiser4_context *ctx;
	int result;
	struct inode *inode = filp->f_path.dentry->d_inode;

	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	switch (cmd) {
	case REISER4_IOC_UNPACK:
		get_exclusive_access(unix_file_inode_data(inode));
		result = unpack(filp, inode, 1 /* forever */ );
		drop_exclusive_access(unix_file_inode_data(inode));
		break;

	default:
		result = RETERR(-ENOSYS);
		break;
	}
	reiser4_exit_context(ctx);
	return result;
}

/* implentation of vfs' bmap method of struct address_space_operations for unix
   file plugin
*/
sector_t bmap_unix_file(struct address_space * mapping, sector_t lblock)
{
	reiser4_context *ctx;
	sector_t result;
	reiser4_key key;
	coord_t coord;
	lock_handle lh;
	struct inode *inode;
	item_plugin *iplug;
	sector_t block;

	inode = mapping->host;

	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	key_by_inode_and_offset_common(inode,
				       (loff_t) lblock * current_blocksize,
				       &key);

	init_lh(&lh);
	result =
	    find_file_item_nohint(&coord, &lh, &key, ZNODE_READ_LOCK, inode);
	if (cbk_errored(result)) {
		done_lh(&lh);
		reiser4_exit_context(ctx);
		return result;
	}

	result = zload(coord.node);
	if (result) {
		done_lh(&lh);
		reiser4_exit_context(ctx);
		return result;
	}

	iplug = item_plugin_by_coord(&coord);
	if (iplug->s.file.get_block) {
		result = iplug->s.file.get_block(&coord, lblock, &block);
		if (result == 0)
			result = block;
	} else
		result = RETERR(-EINVAL);

	zrelse(coord.node);
	done_lh(&lh);
	reiser4_exit_context(ctx);
	return result;
}

/**
 * flow_by_inode_unix_file - initizlize structure flow
 * @inode: inode of file for which read or write is abou
 * @buf: buffer to perform read to or write from
 * @user: flag showing whether @buf is user space or kernel space
 * @size: size of buffer @buf
 * @off: start offset fro read or write
 * @op: READ or WRITE
 * @flow:
 *
 * Initializes fields of @flow: key, size of data, i/o mode (read or write).
 */
int flow_by_inode_unix_file(struct inode *inode,
			    const char __user *buf, int user,
			    loff_t size, loff_t off,
			    rw_op op, flow_t *flow)
{
	assert("nikita-1100", inode != NULL);

	flow->length = size;
	memcpy(&flow->data, &buf, sizeof(buf));
	flow->user = user;
	flow->op = op;
	assert("nikita-1931", inode_file_plugin(inode) != NULL);
	assert("nikita-1932",
	       inode_file_plugin(inode)->key_by_inode ==
	       key_by_inode_and_offset_common);
	/* calculate key of write position and insert it into flow->key */
	return key_by_inode_and_offset_common(inode, off, &flow->key);
}

/* plugin->u.file.set_plug_in_sd = NULL
   plugin->u.file.set_plug_in_inode = NULL
   plugin->u.file.create_blank_sd = NULL */
/* plugin->u.file.delete */
/*
   plugin->u.file.add_link = reiser4_add_link_common
   plugin->u.file.rem_link = NULL */

/* plugin->u.file.owns_item
   this is common_file_owns_item with assertion */
/* Audited by: green(2002.06.15) */
int
owns_item_unix_file(const struct inode *inode /* object to check against */ ,
		    const coord_t * coord /* coord to check */ )
{
	int result;

	result = owns_item_common(inode, coord);
	if (!result)
		return 0;
	if (!plugin_of_group(item_plugin_by_coord(coord),
			     UNIX_FILE_METADATA_ITEM_TYPE))
		return 0;
	assert("vs-547",
	       item_id_by_coord(coord) == EXTENT_POINTER_ID ||
	       item_id_by_coord(coord) == FORMATTING_ID);
	return 1;
}

static int setattr_truncate(struct inode *inode, struct iattr *attr)
{
	int result;
	int s_result;
	loff_t old_size;
	reiser4_tree *tree;

	inode_check_scale(inode, inode->i_size, attr->ia_size);

	old_size = inode->i_size;
	tree = reiser4_tree_by_inode(inode);

	result = safe_link_grab(tree, BA_CAN_COMMIT);
	if (result == 0)
		result = safe_link_add(inode, SAFE_TRUNCATE);
	if (result == 0)
		result = truncate_file_body(inode, attr);
	if (result)
		warning("vs-1588", "truncate_file failed: oid %lli, "
			"old size %lld, new size %lld, retval %d",
			(unsigned long long)get_inode_oid(inode),
			old_size, attr->ia_size, result);

	s_result = safe_link_grab(tree, BA_CAN_COMMIT);
	if (s_result == 0)
		s_result =
		    safe_link_del(tree, get_inode_oid(inode), SAFE_TRUNCATE);
	if (s_result != 0) {
		warning("nikita-3417", "Cannot kill safelink %lli: %i",
			(unsigned long long)get_inode_oid(inode), s_result);
	}
	safe_link_release(tree);
	return result;
}

/* plugin->u.file.setattr method */
/* This calls inode_setattr and if truncate is in effect it also takes
   exclusive inode access to avoid races */
int setattr_unix_file(struct dentry *dentry,	/* Object to change attributes */
		      struct iattr *attr /* change description */ )
{
	int result;

	if (attr->ia_valid & ATTR_SIZE) {
		reiser4_context *ctx;
		struct unix_file_info *uf_info;

		/* truncate does reservation itself and requires exclusive
		   access obtained */
		ctx = reiser4_init_context(dentry->d_inode->i_sb);
		if (IS_ERR(ctx))
			return PTR_ERR(ctx);

		uf_info = unix_file_inode_data(dentry->d_inode);
		get_exclusive_access_careful(uf_info, dentry->d_inode);
		result = setattr_truncate(dentry->d_inode, attr);
		drop_exclusive_access(uf_info);
		context_set_commit_async(ctx);
		reiser4_exit_context(ctx);
	} else
		result = reiser4_setattr_common(dentry, attr);

	return result;
}

/* plugin->u.file.init_inode_data */
void
init_inode_data_unix_file(struct inode *inode,
			  reiser4_object_create_data * crd, int create)
{
	struct unix_file_info *data;

	data = unix_file_inode_data(inode);
	data->container = create ? UF_CONTAINER_EMPTY : UF_CONTAINER_UNKNOWN;
	init_rwsem(&data->latch);
	data->tplug = inode_formatting_plugin(inode);
	data->exclusive_use = 0;

#if REISER4_DEBUG
	data->ea_owner = NULL;
	atomic_set(&data->nr_neas, 0);
#endif
	init_inode_ordering(inode, crd, create);
}

/**
 * delete_unix_file - delete_object of file_plugin
 * @inode: inode to be deleted
 *
 * Truncates file to length 0, removes stat data and safe link.
 */
int delete_object_unix_file(struct inode *inode)
{
	struct unix_file_info *uf_info;
	int result;

	if (reiser4_inode_get_flag(inode, REISER4_NO_SD))
		return 0;

	/* truncate file bogy first */
	uf_info = unix_file_inode_data(inode);
	get_exclusive_access(uf_info);
	result = shorten_file(inode, 0 /* size */ );
	drop_exclusive_access(uf_info);

	if (result)
		warning("edward-1556",
			"failed to truncate file (%llu) on removal: %d",
			get_inode_oid(inode), result);

	/* remove stat data and safe link */
	return reiser4_delete_object_common(inode);
}

static int do_write_begin(struct file *file, struct page *page,
			  loff_t pos, unsigned len)
{
	int ret;
	if (len == PAGE_CACHE_SIZE || PageUptodate(page))
		return 0;

	ret = readpage_unix_file(file, page);
	if (ret) {
		SetPageError(page);
		ClearPageUptodate(page);
		/* All reiser4 readpage() implementations should return the
		 * page locked in case of error. */
		assert("nikita-3472", PageLocked(page));
		return ret;
	}
	/*
	 * ->readpage() either:
	 *
	 *     1. starts IO against @page. @page is locked for IO in
	 *     this case.
	 *
	 *     2. doesn't start IO. @page is unlocked.
	 *
	 * In either case, page should be locked.
	 */
	lock_page(page);
	/*
	 * IO (if any) is completed at this point. Check for IO
	 * errors.
	 */
	if (!PageUptodate(page))
		return RETERR(-EIO);
	return ret;
}

/* plugin->write_begin() */
int write_begin_unix_file(struct file *file, struct page *page,
			  loff_t pos, unsigned len, void **fsdata)
{
	int ret;
	struct inode * inode;
	struct unix_file_info *info;

	inode = file->f_dentry->d_inode;
	info = unix_file_inode_data(inode);

	ret = reiser4_grab_space_force(estimate_one_insert_into_item
				       (reiser4_tree_by_inode(inode)),
				       BA_CAN_COMMIT);
	if (ret)
		return ret;
	get_exclusive_access(info);
	ret = find_file_state(file->f_dentry->d_inode, info);
	if (unlikely(ret != 0)) {
		drop_exclusive_access(info);
		return ret;
	}
	if (info->container == UF_CONTAINER_TAILS) {
		ret = tail2extent(info);
		if (ret) {
			warning("edward-1575",
				"tail conversion failed: %d", ret);
			drop_exclusive_access(info);
			return ret;
		}
	}
	ret = do_write_begin(file, page, pos, len);
	if (unlikely(ret != 0))
		drop_exclusive_access(info);
	/* else exclusive access will be dropped in ->write_end() */
	return ret;
}

/* plugin->write_end() */
int write_end_unix_file(struct file *file, struct page *page,
			loff_t pos, unsigned copied, void *fsdata)
{
	int ret;
	struct inode *inode;
	struct unix_file_info *info;

	inode = file->f_dentry->d_inode;
	info = unix_file_inode_data(inode);

	unlock_page(page);
	ret = find_or_create_extent(page);
	if (ret) {
		SetPageError(page);
		goto exit;
	}
	if (pos + copied > inode->i_size) {
		INODE_SET_FIELD(inode, i_size, pos + copied);
		ret = reiser4_update_sd(inode);
		if (unlikely(ret != 0))
			warning("edward-1604",
				"Can not update stat-data: %i. FSCK?",
				ret);
	}
 exit:
	drop_exclusive_access(info);
	return ret;
}

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 79
 * scroll-step: 1
 * End:
 */
