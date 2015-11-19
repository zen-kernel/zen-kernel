/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#include "item.h"
#include "../../key.h"
#include "../../super.h"
#include "../../carry.h"
#include "../../inode.h"
#include "../../page_cache.h"
#include "../../flush.h"
#include "../object.h"

/* prepare structure reiser4_item_data. It is used to put one extent unit into tree */
/* Audited by: green(2002.06.13) */
reiser4_item_data *init_new_extent(reiser4_item_data * data, void *ext_unit,
				   int nr_extents)
{
	data->data = ext_unit;
	/* data->data is kernel space */
	data->user = 0;
	data->length = sizeof(reiser4_extent) * nr_extents;
	data->arg = NULL;
	data->iplug = item_plugin_by_id(EXTENT_POINTER_ID);
	return data;
}

/* how many bytes are addressed by @nr first extents of the extent item */
reiser4_block_nr reiser4_extent_size(const coord_t * coord, pos_in_node_t nr)
{
	pos_in_node_t i;
	reiser4_block_nr blocks;
	reiser4_extent *ext;

	ext = item_body_by_coord(coord);
	assert("vs-263", nr <= nr_units_extent(coord));

	blocks = 0;
	for (i = 0; i < nr; i++, ext++) {
		blocks += extent_get_width(ext);
	}

	return blocks * current_blocksize;
}

extent_state state_of_extent(reiser4_extent * ext)
{
	switch ((int)extent_get_start(ext)) {
	case 0:
		return HOLE_EXTENT;
	case 1:
		return UNALLOCATED_EXTENT;
	default:
		break;
	}
	return ALLOCATED_EXTENT;
}

int extent_is_unallocated(const coord_t * item)
{
	assert("jmacd-5133", item_is_extent(item));

	return state_of_extent(extent_by_coord(item)) == UNALLOCATED_EXTENT;
}

/* set extent's start and width */
void reiser4_set_extent(reiser4_extent * ext, reiser4_block_nr start,
			reiser4_block_nr width)
{
	extent_set_start(ext, start);
	extent_set_width(ext, width);
}

/**
 * reiser4_replace_extent - replace extent and paste 1 or 2 after it
 * @un_extent: coordinate of extent to be overwritten
 * @lh: need better comment
 * @key: need better comment
 * @exts_to_add: data prepared for insertion into tree
 * @replace: need better comment
 * @flags: need better comment
 * @return_insert_position: need better comment
 *
 * Overwrites one extent, pastes 1 or 2 more ones after overwritten one.  If
 * @return_inserted_position is 1 - @un_extent and @lh are returned set to
 * first of newly inserted units, if it is 0 - @un_extent and @lh are returned
 * set to extent which was overwritten.
 */
int reiser4_replace_extent(struct replace_handle *h,
			   int return_inserted_position)
{
	int result;
	znode *orig_znode;
	/*ON_DEBUG(reiser4_extent orig_ext);*/	/* this is for debugging */

	assert("vs-990", coord_is_existing_unit(h->coord));
	assert("vs-1375", znode_is_write_locked(h->coord->node));
	assert("vs-1426", extent_get_width(&h->overwrite) != 0);
	assert("vs-1427", extent_get_width(&h->new_extents[0]) != 0);
	assert("vs-1427", ergo(h->nr_new_extents == 2,
			       extent_get_width(&h->new_extents[1]) != 0));

	/* compose structure for paste */
	init_new_extent(&h->item, &h->new_extents[0], h->nr_new_extents);

	coord_dup(&h->coord_after, h->coord);
	init_lh(&h->lh_after);
	copy_lh(&h->lh_after, h->lh);
	reiser4_tap_init(&h->watch, &h->coord_after, &h->lh_after, ZNODE_WRITE_LOCK);
	reiser4_tap_monitor(&h->watch);

	ON_DEBUG(h->orig_ext = *extent_by_coord(h->coord));
	orig_znode = h->coord->node;

#if REISER4_DEBUG
	/* make sure that key is set properly */
	unit_key_by_coord(h->coord, &h->tmp);
	set_key_offset(&h->tmp,
		       get_key_offset(&h->tmp) +
		       extent_get_width(&h->overwrite) * current_blocksize);
	assert("vs-1080", keyeq(&h->tmp, &h->paste_key));
#endif

	/* set insert point after unit to be replaced */
	h->coord->between = AFTER_UNIT;

	result = insert_into_item(h->coord, return_inserted_position ? h->lh : NULL,
				  &h->paste_key, &h->item, h->flags);
	if (!result) {
		/* now we have to replace the unit after which new units were
		   inserted. Its position is tracked by @watch */
		reiser4_extent *ext;
		znode *node;

		node = h->coord_after.node;
		if (node != orig_znode) {
			coord_clear_iplug(&h->coord_after);
			result = zload(node);
		}

		if (likely(!result)) {
			ext = extent_by_coord(&h->coord_after);

			assert("vs-987", znode_is_loaded(node));
			assert("vs-988", !memcmp(ext, &h->orig_ext, sizeof(*ext)));

			/* overwrite extent unit */
			memcpy(ext, &h->overwrite, sizeof(reiser4_extent));
			znode_make_dirty(node);

			if (node != orig_znode)
				zrelse(node);

			if (return_inserted_position == 0) {
				/* coord and lh are to be set to overwritten
				   extent */
				assert("vs-1662",
				       WITH_DATA(node, !memcmp(&h->overwrite,
							       extent_by_coord(
								       &h->coord_after),
							       sizeof(reiser4_extent))));

				*h->coord = h->coord_after;
				done_lh(h->lh);
				copy_lh(h->lh, &h->lh_after);
			} else {
				/* h->coord and h->lh are to be set to first of
				   inserted units */
				assert("vs-1663",
				       WITH_DATA(h->coord->node,
						 !memcmp(&h->new_extents[0],
							 extent_by_coord(h->coord),
							 sizeof(reiser4_extent))));
				assert("vs-1664", h->lh->node == h->coord->node);
			}
		}
	}
	reiser4_tap_done(&h->watch);

	return result;
}

lock_handle *znode_lh(znode *node)
{
	assert("vs-1371", znode_is_write_locked(node));
	assert("vs-1372", znode_is_wlocked_once(node));
	return list_entry(node->lock.owners.next, lock_handle, owners_link);
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
