/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#include "item.h"
#include "../../inode.h"
#include "../../tree_walk.h"	/* check_sibling_list() */
#include "../../page_cache.h"
#include "../../carry.h"

/* item_plugin->b.max_key_inside */
reiser4_key *max_key_inside_extent(const coord_t * coord, reiser4_key * key)
{
	item_key_by_coord(coord, key);
	set_key_offset(key, get_key_offset(reiser4_max_key()));
	return key;
}

/* item_plugin->b.can_contain_key
   this checks whether @key of @data is matching to position set by @coord */
int
can_contain_key_extent(const coord_t * coord, const reiser4_key * key,
		       const reiser4_item_data * data)
{
	reiser4_key item_key;

	if (item_plugin_by_coord(coord) != data->iplug)
		return 0;

	item_key_by_coord(coord, &item_key);
	if (get_key_locality(key) != get_key_locality(&item_key) ||
	    get_key_objectid(key) != get_key_objectid(&item_key) ||
	    get_key_ordering(key) != get_key_ordering(&item_key))
		return 0;

	return 1;
}

/* item_plugin->b.mergeable
   first item is of extent type */
/* Audited by: green(2002.06.13) */
int mergeable_extent(const coord_t * p1, const coord_t * p2)
{
	reiser4_key key1, key2;

	assert("vs-299", item_id_by_coord(p1) == EXTENT_POINTER_ID);
	/* FIXME-VS: Which is it? Assert or return 0 */
	if (item_id_by_coord(p2) != EXTENT_POINTER_ID) {
		return 0;
	}

	item_key_by_coord(p1, &key1);
	item_key_by_coord(p2, &key2);
	if (get_key_locality(&key1) != get_key_locality(&key2) ||
	    get_key_objectid(&key1) != get_key_objectid(&key2) ||
	    get_key_ordering(&key1) != get_key_ordering(&key2) ||
	    get_key_type(&key1) != get_key_type(&key2))
		return 0;
	if (get_key_offset(&key1) +
	    reiser4_extent_size(p1, nr_units_extent(p1)) !=
	    get_key_offset(&key2))
		return 0;
	return 1;
}

/* item_plugin->b.nr_units */
pos_in_node_t nr_units_extent(const coord_t * coord)
{
	/* length of extent item has to be multiple of extent size */
	assert("vs-1424",
	       (item_length_by_coord(coord) % sizeof(reiser4_extent)) == 0);
	return item_length_by_coord(coord) / sizeof(reiser4_extent);
}

/* item_plugin->b.lookup */
lookup_result
lookup_extent(const reiser4_key * key, lookup_bias bias UNUSED_ARG,
	      coord_t * coord)
{				/* znode and item_pos are
				   set to an extent item to
				   look through */
	reiser4_key item_key;
	reiser4_block_nr lookuped, offset;
	unsigned i, nr_units;
	reiser4_extent *ext;
	unsigned blocksize;
	unsigned char blocksize_bits;

	item_key_by_coord(coord, &item_key);
	offset = get_key_offset(&item_key);

	/* key we are looking for must be greater than key of item @coord */
	assert("vs-414", keygt(key, &item_key));

	assert("umka-99945",
	       !keygt(key, max_key_inside_extent(coord, &item_key)));

	ext = extent_item(coord);
	assert("vs-1350", (char *)ext == (zdata(coord->node) + coord->offset));

	blocksize = current_blocksize;
	blocksize_bits = current_blocksize_bits;

	/* offset we are looking for */
	lookuped = get_key_offset(key);

	nr_units = nr_units_extent(coord);
	/* go through all extents until the one which address given offset */
	for (i = 0; i < nr_units; i++, ext++) {
		offset += (extent_get_width(ext) << blocksize_bits);
		if (offset > lookuped) {
			/* desired byte is somewhere in this extent */
			coord->unit_pos = i;
			coord->between = AT_UNIT;
			return CBK_COORD_FOUND;
		}
	}

	/* set coord after last unit */
	coord->unit_pos = nr_units - 1;
	coord->between = AFTER_UNIT;
	return CBK_COORD_FOUND;
}

/* item_plugin->b.paste
   item @coord is set to has been appended with @data->length of free
   space. data->data contains data to be pasted into the item in position
   @coord->in_item.unit_pos. It must fit into that free space.
   @coord must be set between units.
*/
int
paste_extent(coord_t * coord, reiser4_item_data * data,
	     carry_plugin_info * info UNUSED_ARG)
{
	unsigned old_nr_units;
	reiser4_extent *ext;
	int item_length;

	ext = extent_item(coord);
	item_length = item_length_by_coord(coord);
	old_nr_units = (item_length - data->length) / sizeof(reiser4_extent);

	/* this is also used to copy extent into newly created item, so
	   old_nr_units could be 0 */
	assert("vs-260", item_length >= data->length);

	/* make sure that coord is set properly */
	assert("vs-35",
	       ((!coord_is_existing_unit(coord))
		|| (!old_nr_units && !coord->unit_pos)));

	/* first unit to be moved */
	switch (coord->between) {
	case AFTER_UNIT:
		coord->unit_pos++;
	case BEFORE_UNIT:
		coord->between = AT_UNIT;
		break;
	case AT_UNIT:
		assert("vs-331", !old_nr_units && !coord->unit_pos);
		break;
	default:
		impossible("vs-330", "coord is set improperly");
	}

	/* prepare space for new units */
	memmove(ext + coord->unit_pos + data->length / sizeof(reiser4_extent),
		ext + coord->unit_pos,
		(old_nr_units - coord->unit_pos) * sizeof(reiser4_extent));

	/* copy new data from kernel space */
	assert("vs-556", data->user == 0);
	memcpy(ext + coord->unit_pos, data->data, (unsigned)data->length);

	/* after paste @coord is set to first of pasted units */
	assert("vs-332", coord_is_existing_unit(coord));
	assert("vs-333",
	       !memcmp(data->data, extent_by_coord(coord),
		       (unsigned)data->length));
	return 0;
}

/* item_plugin->b.can_shift */
int
can_shift_extent(unsigned free_space, coord_t * source,
		 znode * target UNUSED_ARG, shift_direction pend UNUSED_ARG,
		 unsigned *size, unsigned want)
{
	*size = item_length_by_coord(source);
	if (*size > free_space)
		/* never split a unit of extent item */
		*size = free_space - free_space % sizeof(reiser4_extent);

	/* we can shift *size bytes, calculate how many do we want to shift */
	if (*size > want * sizeof(reiser4_extent))
		*size = want * sizeof(reiser4_extent);

	if (*size % sizeof(reiser4_extent) != 0)
		impossible("vs-119", "Wrong extent size: %i %zd", *size,
			   sizeof(reiser4_extent));
	return *size / sizeof(reiser4_extent);

}

/* item_plugin->b.copy_units */
void
copy_units_extent(coord_t * target, coord_t * source,
		  unsigned from, unsigned count,
		  shift_direction where_is_free_space, unsigned free_space)
{
	char *from_ext, *to_ext;

	assert("vs-217", free_space == count * sizeof(reiser4_extent));

	from_ext = item_body_by_coord(source);
	to_ext = item_body_by_coord(target);

	if (where_is_free_space == SHIFT_LEFT) {
		assert("vs-215", from == 0);

		/* At this moment, item length was already updated in the item
		   header by shifting code, hence nr_units_extent() will
		   return "new" number of units---one we obtain after copying
		   units.
		 */
		to_ext +=
		    (nr_units_extent(target) - count) * sizeof(reiser4_extent);
	} else {
		reiser4_key key;
		coord_t coord;

		assert("vs-216",
		       from + count == coord_last_unit_pos(source) + 1);

		from_ext += item_length_by_coord(source) - free_space;

		/* new units are inserted before first unit in an item,
		   therefore, we have to update item key */
		coord = *source;
		coord.unit_pos = from;
		unit_key_extent(&coord, &key);

		node_plugin_by_node(target->node)->update_item_key(target, &key,
								   NULL /*info */);
	}

	memcpy(to_ext, from_ext, free_space);
}

/* item_plugin->b.create_hook
   @arg is znode of leaf node for which we need to update right delimiting key */
int create_hook_extent(const coord_t * coord, void *arg)
{
	coord_t *child_coord;
	znode *node;
	reiser4_key key;
	reiser4_tree *tree;

	if (!arg)
		return 0;

	child_coord = arg;
	tree = znode_get_tree(coord->node);

	assert("nikita-3246", znode_get_level(child_coord->node) == LEAF_LEVEL);

	write_lock_tree(tree);
	write_lock_dk(tree);
	/* find a node on the left level for which right delimiting key has to
	   be updated */
	if (coord_wrt(child_coord) == COORD_ON_THE_LEFT) {
		assert("vs-411", znode_is_left_connected(child_coord->node));
		node = child_coord->node->left;
	} else {
		assert("vs-412", coord_wrt(child_coord) == COORD_ON_THE_RIGHT);
		node = child_coord->node;
		assert("nikita-3314", node != NULL);
	}

	if (node != NULL) {
		znode_set_rd_key(node, item_key_by_coord(coord, &key));

		assert("nikita-3282", check_sibling_list(node));
		/* break sibling links */
		if (ZF_ISSET(node, JNODE_RIGHT_CONNECTED) && node->right) {
			ON_DEBUG(node->right->left_version =
				 atomic_inc_return(&delim_key_version);
				 node->right_version =
				 atomic_inc_return(&delim_key_version););

			node->right->left = NULL;
			node->right = NULL;
		}
	}
	write_unlock_dk(tree);
	write_unlock_tree(tree);
	return 0;
}

#define ITEM_TAIL_KILLED 0
#define ITEM_HEAD_KILLED 1
#define ITEM_KILLED 2

/* item_plugin->b.kill_hook
   this is called when @count units starting from @from-th one are going to be removed
   */
int
kill_hook_extent(const coord_t * coord, pos_in_node_t from, pos_in_node_t count,
		 struct carry_kill_data *kdata)
{
	reiser4_extent *ext;
	reiser4_block_nr start, length;
	const reiser4_key *pfrom_key, *pto_key;
	struct inode *inode;
	reiser4_tree *tree;
	pgoff_t from_off, to_off, offset, skip;
	int retval;

	/* these are located in memory kmalloc-ed by kill_node_content */
	reiser4_key *min_item_key, *max_item_key, *from_key, *to_key, *key;
	coord_t *dup, *next;

	assert("zam-811", znode_is_write_locked(coord->node));
	assert("nikita-3315", kdata != NULL);
	assert("vs-34", kdata->buf != NULL);

	/* map structures to kdata->buf */
	min_item_key = (reiser4_key *) (kdata->buf);
	max_item_key = min_item_key + 1;
	from_key = max_item_key + 1;
	to_key = from_key + 1;
	key = to_key + 1;
	dup = (coord_t *) (key + 1);
	next = dup + 1;

	item_key_by_coord(coord, min_item_key);
	max_item_key_by_coord(coord, max_item_key);

	if (kdata->params.from_key) {
		pfrom_key = kdata->params.from_key;
		pto_key = kdata->params.to_key;
	} else {
		assert("vs-1549", from == coord->unit_pos);
		unit_key_by_coord(coord, from_key);
		pfrom_key = from_key;

		coord_dup(dup, coord);
		dup->unit_pos = from + count - 1;
		max_unit_key_by_coord(dup, to_key);
		pto_key = to_key;
	}

	if (!keylt(pto_key, max_item_key)) {
		if (!keygt(pfrom_key, min_item_key)) {
			znode *left, *right;

			/* item is to be removed completely */
			assert("nikita-3316", kdata->left != NULL
			       && kdata->right != NULL);

			left = kdata->left->node;
			right = kdata->right->node;

			tree = current_tree;
			/* we have to do two things:
			 *
			 *     1. link left and right formatted neighbors of
			 *        extent being removed, and
			 *
			 *     2. update their delimiting keys.
			 *
			 * atomicity of these operations is protected by
			 * taking dk-lock and tree-lock.
			 */
			/* if neighbors of item being removed are znodes -
			 * link them */
			write_lock_tree(tree);
			write_lock_dk(tree);
			link_left_and_right(left, right);
			if (left) {
				/* update right delimiting key of left
				 * neighbor of extent item */
				/*coord_t next;
				   reiser4_key key; */

				coord_dup(next, coord);

				if (coord_next_item(next))
					*key = *znode_get_rd_key(coord->node);
				else
					item_key_by_coord(next, key);
				znode_set_rd_key(left, key);
			}
			write_unlock_dk(tree);
			write_unlock_tree(tree);

			from_off =
			    get_key_offset(min_item_key) >> PAGE_CACHE_SHIFT;
			to_off =
			    (get_key_offset(max_item_key) +
			     1) >> PAGE_CACHE_SHIFT;
			retval = ITEM_KILLED;
		} else {
			/* tail of item is to be removed */
			from_off =
			    (get_key_offset(pfrom_key) + PAGE_CACHE_SIZE -
			     1) >> PAGE_CACHE_SHIFT;
			to_off =
			    (get_key_offset(max_item_key) +
			     1) >> PAGE_CACHE_SHIFT;
			retval = ITEM_TAIL_KILLED;
		}
	} else {
		/* head of item is to be removed */
		assert("vs-1571", keyeq(pfrom_key, min_item_key));
		assert("vs-1572",
		       (get_key_offset(pfrom_key) & (PAGE_CACHE_SIZE - 1)) ==
		       0);
		assert("vs-1573",
		       ((get_key_offset(pto_key) + 1) & (PAGE_CACHE_SIZE -
							 1)) == 0);

		if (kdata->left->node) {
			/* update right delimiting key of left neighbor of extent item */
			/*reiser4_key key; */

			*key = *pto_key;
			set_key_offset(key, get_key_offset(pto_key) + 1);

			write_lock_dk(current_tree);
			znode_set_rd_key(kdata->left->node, key);
			write_unlock_dk(current_tree);
		}

		from_off = get_key_offset(pfrom_key) >> PAGE_CACHE_SHIFT;
		to_off = (get_key_offset(pto_key) + 1) >> PAGE_CACHE_SHIFT;
		retval = ITEM_HEAD_KILLED;
	}

	inode = kdata->inode;
	assert("vs-1545", inode != NULL);
	if (inode != NULL)
		/* take care of pages and jnodes corresponding to part of item being killed */
		reiser4_invalidate_pages(inode->i_mapping, from_off,
					 to_off - from_off,
					 kdata->params.truncate);

	ext = extent_item(coord) + from;
	offset =
	    (get_key_offset(min_item_key) +
	     reiser4_extent_size(coord, from)) >> PAGE_CACHE_SHIFT;

	assert("vs-1551", from_off >= offset);
	assert("vs-1552", from_off - offset <= extent_get_width(ext));
	skip = from_off - offset;
	offset = from_off;

	while (offset < to_off) {
		length = extent_get_width(ext) - skip;
		if (state_of_extent(ext) == HOLE_EXTENT) {
			skip = 0;
			offset += length;
			ext++;
			continue;
		}

		if (offset + length > to_off) {
			length = to_off - offset;
		}

		inode_sub_blocks(inode, length);

		if (state_of_extent(ext) == UNALLOCATED_EXTENT) {
			/* some jnodes corresponding to this unallocated extent */
			fake_allocated2free(length, 0 /* unformatted */ );

			skip = 0;
			offset += length;
			ext++;
			continue;
		}

		assert("vs-1218", state_of_extent(ext) == ALLOCATED_EXTENT);

		if (length != 0) {
			start = extent_get_start(ext) + skip;

			/* BA_DEFER bit parameter is turned on because blocks which get freed are not safe to be freed
			   immediately */
			reiser4_dealloc_blocks(&start, &length,
					       0 /* not used */ ,
					       BA_DEFER
					       /* unformatted with defer */ );
		}
		skip = 0;
		offset += length;
		ext++;
	}
	return retval;
}

/* item_plugin->b.kill_units */
int
kill_units_extent(coord_t * coord, pos_in_node_t from, pos_in_node_t to,
		  struct carry_kill_data *kdata, reiser4_key * smallest_removed,
		  reiser4_key * new_first)
{
	reiser4_extent *ext;
	reiser4_key item_key;
	pos_in_node_t count;
	reiser4_key from_key, to_key;
	const reiser4_key *pfrom_key, *pto_key;
	loff_t off;
	int result;

	assert("vs-1541",
	       ((kdata->params.from_key == NULL && kdata->params.to_key == NULL)
		|| (kdata->params.from_key != NULL
		    && kdata->params.to_key != NULL)));

	if (kdata->params.from_key) {
		pfrom_key = kdata->params.from_key;
		pto_key = kdata->params.to_key;
	} else {
		coord_t dup;

		/* calculate key range of kill */
		assert("vs-1549", from == coord->unit_pos);
		unit_key_by_coord(coord, &from_key);
		pfrom_key = &from_key;

		coord_dup(&dup, coord);
		dup.unit_pos = to;
		max_unit_key_by_coord(&dup, &to_key);
		pto_key = &to_key;
	}

	item_key_by_coord(coord, &item_key);

#if REISER4_DEBUG
	{
		reiser4_key max_item_key;

		max_item_key_by_coord(coord, &max_item_key);

		if (new_first) {
			/* head of item is to be cut */
			assert("vs-1542", keyeq(pfrom_key, &item_key));
			assert("vs-1538", keylt(pto_key, &max_item_key));
		} else {
			/* tail of item is to be cut */
			assert("vs-1540", keygt(pfrom_key, &item_key));
			assert("vs-1543", !keylt(pto_key, &max_item_key));
		}
	}
#endif

	if (smallest_removed)
		*smallest_removed = *pfrom_key;

	if (new_first) {
		/* item head is cut. Item key will change. This new key is calculated here */
		assert("vs-1556",
		       (get_key_offset(pto_key) & (PAGE_CACHE_SIZE - 1)) ==
		       (PAGE_CACHE_SIZE - 1));
		*new_first = *pto_key;
		set_key_offset(new_first, get_key_offset(new_first) + 1);
	}

	count = to - from + 1;
	result = kill_hook_extent(coord, from, count, kdata);
	if (result == ITEM_TAIL_KILLED) {
		assert("vs-1553",
		       get_key_offset(pfrom_key) >=
		       get_key_offset(&item_key) +
		       reiser4_extent_size(coord, from));
		off =
		    get_key_offset(pfrom_key) -
			(get_key_offset(&item_key) +
			 reiser4_extent_size(coord, from));
		if (off) {
			/* unit @from is to be cut partially. Its width decreases */
			ext = extent_item(coord) + from;
			extent_set_width(ext,
					 (off + PAGE_CACHE_SIZE -
					  1) >> PAGE_CACHE_SHIFT);
			count--;
		}
	} else {
		__u64 max_to_offset;
		__u64 rest;

		assert("vs-1575", result == ITEM_HEAD_KILLED);
		assert("", from == 0);
		assert("",
		       ((get_key_offset(pto_key) + 1) & (PAGE_CACHE_SIZE -
							 1)) == 0);
		assert("",
		       get_key_offset(pto_key) + 1 >
		       get_key_offset(&item_key) +
		       reiser4_extent_size(coord, to));
		max_to_offset =
		    get_key_offset(&item_key) +
			reiser4_extent_size(coord, to + 1) - 1;
		assert("", get_key_offset(pto_key) <= max_to_offset);

		rest =
		    (max_to_offset -
		     get_key_offset(pto_key)) >> PAGE_CACHE_SHIFT;
		if (rest) {
			/* unit @to is to be cut partially */
			ext = extent_item(coord) + to;

			assert("", extent_get_width(ext) > rest);

			if (state_of_extent(ext) == ALLOCATED_EXTENT)
				extent_set_start(ext,
						 extent_get_start(ext) +
						 (extent_get_width(ext) -
						  rest));

			extent_set_width(ext, rest);
			count--;
		}
	}
	return count * sizeof(reiser4_extent);
}

/* item_plugin->b.cut_units
   this is too similar to kill_units_extent */
int
cut_units_extent(coord_t * coord, pos_in_node_t from, pos_in_node_t to,
		 struct carry_cut_data *cdata, reiser4_key * smallest_removed,
		 reiser4_key * new_first)
{
	reiser4_extent *ext;
	reiser4_key item_key;
	pos_in_node_t count;
	reiser4_key from_key, to_key;
	const reiser4_key *pfrom_key, *pto_key;
	loff_t off;

	assert("vs-1541",
	       ((cdata->params.from_key == NULL && cdata->params.to_key == NULL)
		|| (cdata->params.from_key != NULL
		    && cdata->params.to_key != NULL)));

	if (cdata->params.from_key) {
		pfrom_key = cdata->params.from_key;
		pto_key = cdata->params.to_key;
	} else {
		coord_t dup;

		/* calculate key range of kill */
		coord_dup(&dup, coord);
		dup.unit_pos = from;
		unit_key_by_coord(&dup, &from_key);

		dup.unit_pos = to;
		max_unit_key_by_coord(&dup, &to_key);

		pfrom_key = &from_key;
		pto_key = &to_key;
	}

	assert("vs-1555",
	       (get_key_offset(pfrom_key) & (PAGE_CACHE_SIZE - 1)) == 0);
	assert("vs-1556",
	       (get_key_offset(pto_key) & (PAGE_CACHE_SIZE - 1)) ==
	       (PAGE_CACHE_SIZE - 1));

	item_key_by_coord(coord, &item_key);

#if REISER4_DEBUG
	{
		reiser4_key max_item_key;

		assert("vs-1584",
		       get_key_locality(pfrom_key) ==
		       get_key_locality(&item_key));
		assert("vs-1585",
		       get_key_type(pfrom_key) == get_key_type(&item_key));
		assert("vs-1586",
		       get_key_objectid(pfrom_key) ==
		       get_key_objectid(&item_key));
		assert("vs-1587",
		       get_key_ordering(pfrom_key) ==
		       get_key_ordering(&item_key));

		max_item_key_by_coord(coord, &max_item_key);

		if (new_first != NULL) {
			/* head of item is to be cut */
			assert("vs-1542", keyeq(pfrom_key, &item_key));
			assert("vs-1538", keylt(pto_key, &max_item_key));
		} else {
			/* tail of item is to be cut */
			assert("vs-1540", keygt(pfrom_key, &item_key));
			assert("vs-1543", keyeq(pto_key, &max_item_key));
		}
	}
#endif

	if (smallest_removed)
		*smallest_removed = *pfrom_key;

	if (new_first) {
		/* item head is cut. Item key will change. This new key is calculated here */
		*new_first = *pto_key;
		set_key_offset(new_first, get_key_offset(new_first) + 1);
	}

	count = to - from + 1;

	assert("vs-1553",
	       get_key_offset(pfrom_key) >=
	       get_key_offset(&item_key) + reiser4_extent_size(coord, from));
	off =
	    get_key_offset(pfrom_key) - (get_key_offset(&item_key) +
					 reiser4_extent_size(coord, from));
	if (off) {
		/* tail of unit @from is to be cut partially. Its width decreases */
		assert("vs-1582", new_first == NULL);
		ext = extent_item(coord) + from;
		extent_set_width(ext, off >> PAGE_CACHE_SHIFT);
		count--;
	}

	assert("vs-1554",
	       get_key_offset(pto_key) <=
	       get_key_offset(&item_key) +
	       reiser4_extent_size(coord, to + 1) - 1);
	off =
		(get_key_offset(&item_key) +
		 reiser4_extent_size(coord, to + 1) - 1) -
		get_key_offset(pto_key);
	if (off) {
		/* @to_key is smaller than max key of unit @to. Unit @to will not be removed. It gets start increased
		   and width decreased. */
		assert("vs-1583", (off & (PAGE_CACHE_SIZE - 1)) == 0);
		ext = extent_item(coord) + to;
		if (state_of_extent(ext) == ALLOCATED_EXTENT)
			extent_set_start(ext,
					 extent_get_start(ext) +
					 (extent_get_width(ext) -
					  (off >> PAGE_CACHE_SHIFT)));

		extent_set_width(ext, (off >> PAGE_CACHE_SHIFT));
		count--;
	}
	return count * sizeof(reiser4_extent);
}

/* item_plugin->b.unit_key */
reiser4_key *unit_key_extent(const coord_t * coord, reiser4_key * key)
{
	assert("vs-300", coord_is_existing_unit(coord));

	item_key_by_coord(coord, key);
	set_key_offset(key,
		       (get_key_offset(key) +
			reiser4_extent_size(coord, coord->unit_pos)));

	return key;
}

/* item_plugin->b.max_unit_key */
reiser4_key *max_unit_key_extent(const coord_t * coord, reiser4_key * key)
{
	assert("vs-300", coord_is_existing_unit(coord));

	item_key_by_coord(coord, key);
	set_key_offset(key,
		       (get_key_offset(key) +
			reiser4_extent_size(coord, coord->unit_pos + 1) - 1));
	return key;
}

/* item_plugin->b.estimate
   item_plugin->b.item_data_by_flow */

#if REISER4_DEBUG

/* item_plugin->b.check
   used for debugging, every item should have here the most complete
   possible check of the consistency of the item that the inventor can
   construct
*/
int reiser4_check_extent(const coord_t * coord /* coord of item to check */,
			 const char **error /* where to store error message */)
{
	reiser4_extent *ext, *first;
	unsigned i, j;
	reiser4_block_nr start, width, blk_cnt;
	unsigned num_units;
	reiser4_tree *tree;
	oid_t oid;
	reiser4_key key;
	coord_t scan;

	assert("vs-933", REISER4_DEBUG);

	if (znode_get_level(coord->node) != TWIG_LEVEL) {
		*error = "Extent on the wrong level";
		return -1;
	}
	if (item_length_by_coord(coord) % sizeof(reiser4_extent) != 0) {
		*error = "Wrong item size";
		return -1;
	}
	ext = first = extent_item(coord);
	blk_cnt = reiser4_block_count(reiser4_get_current_sb());
	num_units = coord_num_units(coord);
	tree = znode_get_tree(coord->node);
	item_key_by_coord(coord, &key);
	oid = get_key_objectid(&key);
	coord_dup(&scan, coord);

	for (i = 0; i < num_units; ++i, ++ext) {
		__u64 index;

		scan.unit_pos = i;
		index = extent_unit_index(&scan);

#if 0
		/* check that all jnodes are present for the unallocated
		 * extent */
		if (state_of_extent(ext) == UNALLOCATED_EXTENT) {
			for (j = 0; j < extent_get_width(ext); j++) {
				jnode *node;

				node = jlookup(tree, oid, index + j);
				if (node == NULL) {
					print_coord("scan", &scan, 0);
					*error = "Jnode missing";
					return -1;
				}
				jput(node);
			}
		}
#endif

		start = extent_get_start(ext);
		if (start < 2)
			continue;
		/* extent is allocated one */
		width = extent_get_width(ext);
		if (start >= blk_cnt) {
			*error = "Start too large";
			return -1;
		}
		if (start + width > blk_cnt) {
			*error = "End too large";
			return -1;
		}
		/* make sure that this extent does not overlap with other
		   allocated extents extents */
		for (j = 0; j < i; j++) {
			if (state_of_extent(first + j) != ALLOCATED_EXTENT)
				continue;
			if (!
			    ((extent_get_start(ext) >=
			      extent_get_start(first + j) +
			      extent_get_width(first + j))
			     || (extent_get_start(ext) +
				 extent_get_width(ext) <=
				 extent_get_start(first + j)))) {
				*error = "Extent overlaps with others";
				return -1;
			}
		}

	}

	return 0;
}

#endif				/* REISER4_DEBUG */

/*
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   scroll-step: 1
   End:
*/
