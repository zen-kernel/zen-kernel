/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#include "item.h"
#include "../../inode.h"
#include "../../page_cache.h"
#include "../../carry.h"
#include "../../vfs_ops.h"

#include <asm/uaccess.h>
#include <linux/swap.h>
#include <linux/writeback.h>

/* plugin->u.item.b.max_key_inside */
reiser4_key *max_key_inside_tail(const coord_t *coord, reiser4_key *key)
{
	item_key_by_coord(coord, key);
	set_key_offset(key, get_key_offset(reiser4_max_key()));
	return key;
}

/* plugin->u.item.b.can_contain_key */
int can_contain_key_tail(const coord_t *coord, const reiser4_key *key,
			 const reiser4_item_data *data)
{
	reiser4_key item_key;

	if (item_plugin_by_coord(coord) != data->iplug)
		return 0;

	item_key_by_coord(coord, &item_key);
	if (get_key_locality(key) != get_key_locality(&item_key) ||
	    get_key_objectid(key) != get_key_objectid(&item_key))
		return 0;

	return 1;
}

/* plugin->u.item.b.mergeable
   first item is of tail type */
/* Audited by: green(2002.06.14) */
int mergeable_tail(const coord_t *p1, const coord_t *p2)
{
	reiser4_key key1, key2;

	assert("vs-535", plugin_of_group(item_plugin_by_coord(p1),
					 UNIX_FILE_METADATA_ITEM_TYPE));
	assert("vs-365", item_id_by_coord(p1) == FORMATTING_ID);

	if (item_id_by_coord(p2) != FORMATTING_ID) {
		/* second item is of another type */
		return 0;
	}

	item_key_by_coord(p1, &key1);
	item_key_by_coord(p2, &key2);
	if (get_key_locality(&key1) != get_key_locality(&key2) ||
	    get_key_objectid(&key1) != get_key_objectid(&key2)
	    || get_key_type(&key1) != get_key_type(&key2)) {
		/* items of different objects */
		return 0;
	}
	if (get_key_offset(&key1) + nr_units_tail(p1) != get_key_offset(&key2)) {
		/* not adjacent items */
		return 0;
	}
	return 1;
}

/* plugin->u.item.b.print
   plugin->u.item.b.check */

/* plugin->u.item.b.nr_units */
pos_in_node_t nr_units_tail(const coord_t * coord)
{
	return item_length_by_coord(coord);
}

/* plugin->u.item.b.lookup */
lookup_result
lookup_tail(const reiser4_key * key, lookup_bias bias, coord_t * coord)
{
	reiser4_key item_key;
	__u64 lookuped, offset;
	unsigned nr_units;

	item_key_by_coord(coord, &item_key);
	offset = get_key_offset(item_key_by_coord(coord, &item_key));
	nr_units = nr_units_tail(coord);

	/* key we are looking for must be greater than key of item @coord */
	assert("vs-416", keygt(key, &item_key));

	/* offset we are looking for */
	lookuped = get_key_offset(key);

	if (lookuped >= offset && lookuped < offset + nr_units) {
		/* byte we are looking for is in this item */
		coord->unit_pos = lookuped - offset;
		coord->between = AT_UNIT;
		return CBK_COORD_FOUND;
	}

	/* set coord after last unit */
	coord->unit_pos = nr_units - 1;
	coord->between = AFTER_UNIT;
	return bias ==
	    FIND_MAX_NOT_MORE_THAN ? CBK_COORD_FOUND : CBK_COORD_NOTFOUND;
}

/* plugin->u.item.b.paste */
int
paste_tail(coord_t *coord, reiser4_item_data *data,
	   carry_plugin_info *info UNUSED_ARG)
{
	unsigned old_item_length;
	char *item;

	/* length the item had before resizing has been performed */
	old_item_length = item_length_by_coord(coord) - data->length;

	/* tail items never get pasted in the middle */
	assert("vs-363",
	       (coord->unit_pos == 0 && coord->between == BEFORE_UNIT) ||
	       (coord->unit_pos == old_item_length - 1 &&
		coord->between == AFTER_UNIT) ||
	       (coord->unit_pos == 0 && old_item_length == 0
		&& coord->between == AT_UNIT));

	item = item_body_by_coord(coord);
	if (coord->unit_pos == 0)
		/* make space for pasted data when pasting at the beginning of
		   the item */
		memmove(item + data->length, item, old_item_length);

	if (coord->between == AFTER_UNIT)
		coord->unit_pos++;

	if (data->data) {
		assert("vs-554", data->user == 0 || data->user == 1);
		if (data->user) {
			assert("nikita-3035", reiser4_schedulable());
			/* copy from user space */
			if (__copy_from_user(item + coord->unit_pos,
					     (const char __user *)data->data,
					     (unsigned)data->length))
				return RETERR(-EFAULT);
		} else
			/* copy from kernel space */
			memcpy(item + coord->unit_pos, data->data,
			       (unsigned)data->length);
	} else {
		memset(item + coord->unit_pos, 0, (unsigned)data->length);
	}
	return 0;
}

/* plugin->u.item.b.fast_paste */

/* plugin->u.item.b.can_shift
   number of units is returned via return value, number of bytes via @size. For
   tail items they coincide */
int
can_shift_tail(unsigned free_space, coord_t * source UNUSED_ARG,
	       znode * target UNUSED_ARG, shift_direction direction UNUSED_ARG,
	       unsigned *size, unsigned want)
{
	/* make sure that that we do not want to shift more than we have */
	assert("vs-364", want > 0
	       && want <= (unsigned)item_length_by_coord(source));

	*size = min(want, free_space);
	return *size;
}

/* plugin->u.item.b.copy_units */
void
copy_units_tail(coord_t * target, coord_t * source,
		unsigned from, unsigned count,
		shift_direction where_is_free_space,
		unsigned free_space UNUSED_ARG)
{
	/* make sure that item @target is expanded already */
	assert("vs-366", (unsigned)item_length_by_coord(target) >= count);
	assert("vs-370", free_space >= count);

	if (where_is_free_space == SHIFT_LEFT) {
		/* append item @target with @count first bytes of @source */
		assert("vs-365", from == 0);

		memcpy((char *)item_body_by_coord(target) +
		       item_length_by_coord(target) - count,
		       (char *)item_body_by_coord(source), count);
	} else {
		/* target item is moved to right already */
		reiser4_key key;

		assert("vs-367",
		       (unsigned)item_length_by_coord(source) == from + count);

		memcpy((char *)item_body_by_coord(target),
		       (char *)item_body_by_coord(source) + from, count);

		/* new units are inserted before first unit in an item,
		   therefore, we have to update item key */
		item_key_by_coord(source, &key);
		set_key_offset(&key, get_key_offset(&key) + from);

		node_plugin_by_node(target->node)->update_item_key(target, &key,
								   NULL /*info */);
	}
}

/* plugin->u.item.b.create_hook */

/* item_plugin->b.kill_hook
   this is called when @count units starting from @from-th one are going to be removed
   */
int
kill_hook_tail(const coord_t * coord, pos_in_node_t from,
	       pos_in_node_t count, struct carry_kill_data *kdata)
{
	reiser4_key key;
	loff_t start, end;

	assert("vs-1577", kdata);
	assert("vs-1579", kdata->inode);

	item_key_by_coord(coord, &key);
	start = get_key_offset(&key) + from;
	end = start + count;
	fake_kill_hook_tail(kdata->inode, start, end, kdata->params.truncate);
	return 0;
}

/* plugin->u.item.b.shift_hook */

/* helper for kill_units_tail and cut_units_tail */
static int
do_cut_or_kill(coord_t * coord, pos_in_node_t from, pos_in_node_t to,
	       reiser4_key * smallest_removed, reiser4_key * new_first)
{
	pos_in_node_t count;

	/* this method is only called to remove part of item */
	assert("vs-374", (to - from + 1) < item_length_by_coord(coord));
	/* tails items are never cut from the middle of an item */
	assert("vs-396", ergo(from != 0, to == coord_last_unit_pos(coord)));
	assert("vs-1558", ergo(from == 0, to < coord_last_unit_pos(coord)));

	count = to - from + 1;

	if (smallest_removed) {
		/* store smallest key removed */
		item_key_by_coord(coord, smallest_removed);
		set_key_offset(smallest_removed,
			       get_key_offset(smallest_removed) + from);
	}
	if (new_first) {
		/* head of item is cut */
		assert("vs-1529", from == 0);

		item_key_by_coord(coord, new_first);
		set_key_offset(new_first,
			       get_key_offset(new_first) + from + count);
	}

	if (REISER4_DEBUG)
		memset((char *)item_body_by_coord(coord) + from, 0, count);
	return count;
}

/* plugin->u.item.b.cut_units */
int
cut_units_tail(coord_t * coord, pos_in_node_t from, pos_in_node_t to,
	       struct carry_cut_data *cdata UNUSED_ARG,
	       reiser4_key * smallest_removed, reiser4_key * new_first)
{
	return do_cut_or_kill(coord, from, to, smallest_removed, new_first);
}

/* plugin->u.item.b.kill_units */
int
kill_units_tail(coord_t * coord, pos_in_node_t from, pos_in_node_t to,
		struct carry_kill_data *kdata, reiser4_key * smallest_removed,
		reiser4_key * new_first)
{
	kill_hook_tail(coord, from, to - from + 1, kdata);
	return do_cut_or_kill(coord, from, to, smallest_removed, new_first);
}

/* plugin->u.item.b.unit_key */
reiser4_key *unit_key_tail(const coord_t * coord, reiser4_key * key)
{
	assert("vs-375", coord_is_existing_unit(coord));

	item_key_by_coord(coord, key);
	set_key_offset(key, (get_key_offset(key) + coord->unit_pos));

	return key;
}

/* plugin->u.item.b.estimate
   plugin->u.item.b.item_data_by_flow */

/* tail redpage function. It is called from readpage_tail(). */
static int do_readpage_tail(uf_coord_t *uf_coord, struct page *page)
{
	tap_t tap;
	int result;
	coord_t coord;
	lock_handle lh;
	int count, mapped;
	struct inode *inode;
	char *pagedata;

	/* saving passed coord in order to do not move it by tap. */
	init_lh(&lh);
	copy_lh(&lh, uf_coord->lh);
	inode = page->mapping->host;
	coord_dup(&coord, &uf_coord->coord);

	reiser4_tap_init(&tap, &coord, &lh, ZNODE_READ_LOCK);

	if ((result = reiser4_tap_load(&tap)))
		goto out_tap_done;

	/* lookup until page is filled up. */
	for (mapped = 0; mapped < PAGE_CACHE_SIZE; ) {
		/* number of bytes to be copied to page */
		count = item_length_by_coord(&coord) - coord.unit_pos;
		if (count > PAGE_CACHE_SIZE - mapped)
			count = PAGE_CACHE_SIZE - mapped;

		/* attach @page to address space and get data address */
		pagedata = kmap_atomic(page);

		/* copy tail item to page */
		memcpy(pagedata + mapped,
		       ((char *)item_body_by_coord(&coord) + coord.unit_pos),
		       count);
		mapped += count;

		flush_dcache_page(page);

		/* dettach page from address space */
		kunmap_atomic(pagedata);

		/* Getting next tail item. */
		if (mapped < PAGE_CACHE_SIZE) {
			/*
			 * unlock page in order to avoid keep it locked
			 * during tree lookup, which takes long term locks
			 */
			unlock_page(page);

			/* getting right neighbour. */
			result = go_dir_el(&tap, RIGHT_SIDE, 0);

			/* lock page back */
			lock_page(page);
			if (PageUptodate(page)) {
				/*
				 * another thread read the page, we have
				 * nothing to do
				 */
				result = 0;
				goto out_unlock_page;
			}

			if (result) {
				if (result == -E_NO_NEIGHBOR) {
					/*
					 * rigth neighbor is not a formatted
					 * node
					 */
					result = 0;
					goto done;
				} else {
					goto out_tap_relse;
				}
			} else {
				if (!inode_file_plugin(inode)->
				    owns_item(inode, &coord)) {
					/* item of another file is found */
					result = 0;
					goto done;
				}
			}
		}
	}

 done:
	if (mapped != PAGE_CACHE_SIZE)
		zero_user_segment(page, mapped, PAGE_CACHE_SIZE);
	SetPageUptodate(page);
 out_unlock_page:
	unlock_page(page);
 out_tap_relse:
	reiser4_tap_relse(&tap);
 out_tap_done:
	reiser4_tap_done(&tap);
	return result;
}

/*
 * plugin->s.file.readpage
 *
 * reiser4_read_dispatch->read_unix_file->page_cache_readahead->
 * ->reiser4_readpage_dispatch->readpage_unix_file->readpage_tail
 * or
 * filemap_fault->reiser4_readpage_dispatch->readpage_unix_file->readpage_tail
 *
 * At the beginning: coord->node is read locked, zloaded, page is locked,
 * coord is set to existing unit inside of tail item.
 */
int readpage_tail(void *vp, struct page *page)
{
	uf_coord_t *uf_coord = vp;
	ON_DEBUG(coord_t * coord = &uf_coord->coord);
	ON_DEBUG(reiser4_key key);

	assert("umka-2515", PageLocked(page));
	assert("umka-2516", !PageUptodate(page));
	assert("umka-2517", !jprivate(page) && !PagePrivate(page));
	assert("umka-2518", page->mapping && page->mapping->host);

	assert("umka-2519", znode_is_loaded(coord->node));
	assert("umka-2520", item_is_tail(coord));
	assert("umka-2521", coord_is_existing_unit(coord));
	assert("umka-2522", znode_is_rlocked(coord->node));
	assert("umka-2523",
	       page->mapping->host->i_ino ==
	       get_key_objectid(item_key_by_coord(coord, &key)));

	return do_readpage_tail(uf_coord, page);
}

/**
 * overwrite_tail
 * @flow:
 * @coord:
 *
 * Overwrites tail item or its part by user data. Returns number of bytes
 * written or error code.
 */
static int overwrite_tail(flow_t *flow, coord_t *coord)
{
	unsigned count;

	assert("vs-570", flow->user == 1);
	assert("vs-946", flow->data);
	assert("vs-947", coord_is_existing_unit(coord));
	assert("vs-948", znode_is_write_locked(coord->node));
	assert("nikita-3036", reiser4_schedulable());

	count = item_length_by_coord(coord) - coord->unit_pos;
	if (count > flow->length)
		count = flow->length;

	if (__copy_from_user((char *)item_body_by_coord(coord) + coord->unit_pos,
			     (const char __user *)flow->data, count))
		return RETERR(-EFAULT);

	znode_make_dirty(coord->node);
	return count;
}

/**
 * insert_first_tail
 * @inode:
 * @flow:
 * @coord:
 * @lh:
 *
 * Returns number of bytes written or error code.
 */
static ssize_t insert_first_tail(struct inode *inode, flow_t *flow,
				 coord_t *coord, lock_handle *lh)
{
	int result;
	loff_t to_write;
	struct unix_file_info *uf_info;

	if (get_key_offset(&flow->key) != 0) {
		/*
		 * file is empty and we have to write not to the beginning of
		 * file. Create a hole at the beginning of file. On success
		 * insert_flow returns 0 as number of written bytes which is
		 * what we have to return on padding a file with holes
		 */
		flow->data = NULL;
		flow->length = get_key_offset(&flow->key);
		set_key_offset(&flow->key, 0);
		/*
		 * holes in files built of tails are stored just like if there
		 * were real data which are all zeros.
		 */
		inode_add_bytes(inode, flow->length);
		result = reiser4_insert_flow(coord, lh, flow);
		if (flow->length)
			inode_sub_bytes(inode, flow->length);

		uf_info = unix_file_inode_data(inode);

		/*
		 * first item insertion is only possible when writing to empty
		 * file or performing tail conversion
		 */
		assert("", (uf_info->container == UF_CONTAINER_EMPTY ||
			    (reiser4_inode_get_flag(inode,
						    REISER4_PART_MIXED) &&
			     reiser4_inode_get_flag(inode,
						    REISER4_PART_IN_CONV))));
		/* if file was empty - update its state */
		if (result == 0 && uf_info->container == UF_CONTAINER_EMPTY)
			uf_info->container = UF_CONTAINER_TAILS;
		return result;
	}

	inode_add_bytes(inode, flow->length);

	to_write = flow->length;
	result = reiser4_insert_flow(coord, lh, flow);
	if (flow->length)
		inode_sub_bytes(inode, flow->length);
	return (to_write - flow->length) ? (to_write - flow->length) : result;
}

/**
 * append_tail
 * @inode:
 * @flow:
 * @coord:
 * @lh:
 *
 * Returns number of bytes written or error code.
 */
static ssize_t append_tail(struct inode *inode,
			   flow_t *flow, coord_t *coord, lock_handle *lh)
{
	int result;
	reiser4_key append_key;
	loff_t to_write;

	if (!keyeq(&flow->key, append_key_tail(coord, &append_key))) {
		flow->data = NULL;
		flow->length = get_key_offset(&flow->key) - get_key_offset(&append_key);
		set_key_offset(&flow->key, get_key_offset(&append_key));
		/*
		 * holes in files built of tails are stored just like if there
		 * were real data which are all zeros.
		 */
		inode_add_bytes(inode, flow->length);
		result = reiser4_insert_flow(coord, lh, flow);
		if (flow->length)
			inode_sub_bytes(inode, flow->length);
		return result;
	}

	inode_add_bytes(inode, flow->length);

	to_write = flow->length;
	result = reiser4_insert_flow(coord, lh, flow);
	if (flow->length)
		inode_sub_bytes(inode, flow->length);
	return (to_write - flow->length) ? (to_write - flow->length) : result;
}

/**
 * write_tail_reserve_space - reserve space for tail write operation
 * @inode:
 *
 * Estimates and reserves space which may be required for writing one flow to a
 * file
 */
static int write_extent_reserve_space(struct inode *inode)
{
	__u64 count;
	reiser4_tree *tree;

	/*
	 * to write one flow to a file by tails we have to reserve disk space for:

	 * 1. find_file_item may have to insert empty node to the tree (empty
	 * leaf node between two extent items). This requires 1 block and
	 * number of blocks which are necessary to perform insertion of an
	 * internal item into twig level.
	 *
	 * 2. flow insertion
	 *
	 * 3. stat data update
	 */
	tree = reiser4_tree_by_inode(inode);
	count = estimate_one_insert_item(tree) +
		estimate_insert_flow(tree->height) +
		estimate_one_insert_item(tree);
	grab_space_enable();
	return reiser4_grab_space(count, 0 /* flags */);
}

#define PAGE_PER_FLOW 4

static loff_t faultin_user_pages(const char __user *buf, size_t count)
{
	loff_t faulted;
	int to_fault;

	if (count > PAGE_PER_FLOW * PAGE_CACHE_SIZE)
		count = PAGE_PER_FLOW * PAGE_CACHE_SIZE;
	faulted = 0;
	while (count > 0) {
		to_fault = PAGE_CACHE_SIZE;
		if (count < to_fault)
			to_fault = count;
		fault_in_pages_readable(buf + faulted, to_fault);
		count -= to_fault;
		faulted += to_fault;
	}
	return faulted;
}

ssize_t reiser4_write_tail_noreserve(struct file *file,
				     struct inode * inode,
				     const char __user *buf,
				     size_t count, loff_t *pos)
{
	struct hint hint;
	int result;
	flow_t flow;
	coord_t *coord;
	lock_handle *lh;
	znode *loaded;

	assert("edward-1548", inode != NULL);

	result = load_file_hint(file, &hint);
	BUG_ON(result != 0);

	flow.length = faultin_user_pages(buf, count);
	flow.user = 1;
	memcpy(&flow.data, &buf, sizeof(buf));
	flow.op = WRITE_OP;
	key_by_inode_and_offset_common(inode, *pos, &flow.key);

	result = find_file_item(&hint, &flow.key, ZNODE_WRITE_LOCK, inode);
	if (IS_CBKERR(result))
		return result;

	coord = &hint.ext_coord.coord;
	lh = hint.ext_coord.lh;

	result = zload(coord->node);
	BUG_ON(result != 0);
	loaded = coord->node;

	if (coord->between == AFTER_UNIT) {
		/* append with data or hole */
		result = append_tail(inode, &flow, coord, lh);
	} else if (coord->between == AT_UNIT) {
		/* overwrite */
		result = overwrite_tail(&flow, coord);
	} else {
		/* no items of this file yet. insert data or hole */
		result = insert_first_tail(inode, &flow, coord, lh);
	}
	zrelse(loaded);
	if (result < 0) {
		done_lh(lh);
		return result;
	}

	/* seal and unlock znode */
	hint.ext_coord.valid = 0;
	if (hint.ext_coord.valid)
		reiser4_set_hint(&hint, &flow.key, ZNODE_WRITE_LOCK);
	else
		reiser4_unset_hint(&hint);

	save_file_hint(file, &hint);
	return result;
}

/**
 * reiser4_write_tail - write method of tail item plugin
 * @file: file to write to
 * @buf: address of user-space buffer
 * @count: number of bytes to write
 * @pos: position in file to write to
 *
 * Returns number of written bytes or error code.
 */
ssize_t reiser4_write_tail(struct file *file,
			   struct inode * inode,
			   const char __user *buf,
			   size_t count, loff_t *pos)
{
	if (write_extent_reserve_space(inode))
		return RETERR(-ENOSPC);
	return reiser4_write_tail_noreserve(file, inode, buf, count, pos);
}

#if REISER4_DEBUG

static int
coord_matches_key_tail(const coord_t * coord, const reiser4_key * key)
{
	reiser4_key item_key;

	assert("vs-1356", coord_is_existing_unit(coord));
	assert("vs-1354", keylt(key, append_key_tail(coord, &item_key)));
	assert("vs-1355", keyge(key, item_key_by_coord(coord, &item_key)));
	return get_key_offset(key) ==
	    get_key_offset(&item_key) + coord->unit_pos;

}

#endif

/* plugin->u.item.s.file.read */
int reiser4_read_tail(struct file *file UNUSED_ARG, flow_t *f, hint_t *hint)
{
	unsigned count;
	int item_length;
	coord_t *coord;
	uf_coord_t *uf_coord;

	uf_coord = &hint->ext_coord;
	coord = &uf_coord->coord;

	assert("vs-571", f->user == 1);
	assert("vs-571", f->data);
	assert("vs-967", coord && coord->node);
	assert("vs-1117", znode_is_rlocked(coord->node));
	assert("vs-1118", znode_is_loaded(coord->node));

	assert("nikita-3037", reiser4_schedulable());
	assert("vs-1357", coord_matches_key_tail(coord, &f->key));

	/* calculate number of bytes to read off the item */
	item_length = item_length_by_coord(coord);
	count = item_length_by_coord(coord) - coord->unit_pos;
	if (count > f->length)
		count = f->length;

	/* user page has to be brought in so that major page fault does not
	 * occur here when longtem lock is held */
	if (__copy_to_user((char __user *)f->data,
			   ((char *)item_body_by_coord(coord) + coord->unit_pos),
			   count))
		return RETERR(-EFAULT);

	/* probably mark_page_accessed() should only be called if
	 * coord->unit_pos is zero. */
	mark_page_accessed(znode_page(coord->node));
	move_flow_forward(f, count);

	coord->unit_pos += count;
	if (item_length == coord->unit_pos) {
		coord->unit_pos--;
		coord->between = AFTER_UNIT;
	}
	reiser4_set_hint(hint, &f->key, ZNODE_READ_LOCK);
	return 0;
}

/*
   plugin->u.item.s.file.append_key
   key of first byte which is the next to last byte by addressed by this item
*/
reiser4_key *append_key_tail(const coord_t * coord, reiser4_key * key)
{
	item_key_by_coord(coord, key);
	set_key_offset(key, get_key_offset(key) + item_length_by_coord(coord));
	return key;
}

/* plugin->u.item.s.file.init_coord_extension */
void init_coord_extension_tail(uf_coord_t * uf_coord, loff_t lookuped)
{
	uf_coord->valid = 1;
}

/*
  plugin->u.item.s.file.get_block
*/
int
get_block_address_tail(const coord_t * coord, sector_t lblock, sector_t * block)
{
	assert("nikita-3252", znode_get_level(coord->node) == LEAF_LEVEL);

	if (reiser4_blocknr_is_fake(znode_get_block(coord->node)))
		/* if node has'nt obtainet its block number yet, return 0.
		 * Lets avoid upsetting users with some cosmic numbers beyond
		 * the device capacity.*/
		*block = 0;
	else
		*block = *znode_get_block(coord->node);
	return 0;
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
