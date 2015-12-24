/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* definition of item plugins. */

#include "../../forward.h"
#include "../../debug.h"
#include "../../key.h"
#include "../../coord.h"
#include "../plugin_header.h"
#include "sde.h"
#include "internal.h"
#include "item.h"
#include "static_stat.h"
#include "../plugin.h"
#include "../../znode.h"
#include "../../tree.h"
#include "../../context.h"
#include "ctail.h"

/* return pointer to item body */
void item_body_by_coord_hard(coord_t * coord /* coord to query */ )
{
	assert("nikita-324", coord != NULL);
	assert("nikita-325", coord->node != NULL);
	assert("nikita-326", znode_is_loaded(coord->node));
	assert("nikita-3200", coord->offset == INVALID_OFFSET);

	coord->offset =
	    node_plugin_by_node(coord->node)->item_by_coord(coord) -
	    zdata(coord->node);
	ON_DEBUG(coord->body_v = coord->node->times_locked);
}

void *item_body_by_coord_easy(const coord_t * coord /* coord to query */ )
{
	return zdata(coord->node) + coord->offset;
}

#if REISER4_DEBUG

int item_body_is_valid(const coord_t * coord)
{
	return
	    coord->offset ==
	    node_plugin_by_node(coord->node)->item_by_coord(coord) -
	    zdata(coord->node);
}

#endif

/* return length of item at @coord */
pos_in_node_t item_length_by_coord(const coord_t * coord /* coord to query */ )
{
	int len;

	assert("nikita-327", coord != NULL);
	assert("nikita-328", coord->node != NULL);
	assert("nikita-329", znode_is_loaded(coord->node));

	len = node_plugin_by_node(coord->node)->length_by_coord(coord);
	return len;
}

void obtain_item_plugin(const coord_t * coord)
{
	assert("nikita-330", coord != NULL);
	assert("nikita-331", coord->node != NULL);
	assert("nikita-332", znode_is_loaded(coord->node));

	coord_set_iplug((coord_t *) coord,
			node_plugin_by_node(coord->node)->
			plugin_by_coord(coord));
	assert("nikita-2479",
	       coord_iplug(coord) ==
	       node_plugin_by_node(coord->node)->plugin_by_coord(coord));
}

/* return id of item */
/* Audited by: green(2002.06.15) */
item_id item_id_by_coord(const coord_t * coord /* coord to query */ )
{
	assert("vs-539", coord != NULL);
	assert("vs-538", coord->node != NULL);
	assert("vs-537", znode_is_loaded(coord->node));
	assert("vs-536", item_plugin_by_coord(coord) != NULL);
	assert("vs-540",
	       item_id_by_plugin(item_plugin_by_coord(coord)) < LAST_ITEM_ID);

	return item_id_by_plugin(item_plugin_by_coord(coord));
}

/* return key of item at @coord */
/* Audited by: green(2002.06.15) */
reiser4_key *item_key_by_coord(const coord_t * coord /* coord to query */ ,
			       reiser4_key * key /* result */ )
{
	assert("nikita-338", coord != NULL);
	assert("nikita-339", coord->node != NULL);
	assert("nikita-340", znode_is_loaded(coord->node));

	return node_plugin_by_node(coord->node)->key_at(coord, key);
}

/* this returns max key in the item */
reiser4_key *max_item_key_by_coord(const coord_t * coord /* coord to query */ ,
				   reiser4_key * key /* result */ )
{
	coord_t last;

	assert("nikita-338", coord != NULL);
	assert("nikita-339", coord->node != NULL);
	assert("nikita-340", znode_is_loaded(coord->node));

	/* make coord pointing to last item's unit */
	coord_dup(&last, coord);
	last.unit_pos = coord_num_units(&last) - 1;
	assert("vs-1560", coord_is_existing_unit(&last));

	max_unit_key_by_coord(&last, key);
	return key;
}

/* return key of unit at @coord */
reiser4_key *unit_key_by_coord(const coord_t * coord /* coord to query */ ,
			       reiser4_key * key /* result */ )
{
	assert("nikita-772", coord != NULL);
	assert("nikita-774", coord->node != NULL);
	assert("nikita-775", znode_is_loaded(coord->node));

	if (item_plugin_by_coord(coord)->b.unit_key != NULL)
		return item_plugin_by_coord(coord)->b.unit_key(coord, key);
	else
		return item_key_by_coord(coord, key);
}

/* return the biggest key contained the unit @coord */
reiser4_key *max_unit_key_by_coord(const coord_t * coord /* coord to query */ ,
				   reiser4_key * key /* result */ )
{
	assert("nikita-772", coord != NULL);
	assert("nikita-774", coord->node != NULL);
	assert("nikita-775", znode_is_loaded(coord->node));

	if (item_plugin_by_coord(coord)->b.max_unit_key != NULL)
		return item_plugin_by_coord(coord)->b.max_unit_key(coord, key);
	else
		return unit_key_by_coord(coord, key);
}

/* ->max_key_inside() method for items consisting of exactly one key (like
    stat-data) */
static reiser4_key *max_key_inside_single_key(const coord_t *
					      coord /* coord of item */ ,
					      reiser4_key *
					      result /* resulting key */ )
{
	assert("nikita-604", coord != NULL);

	/* coord -> key is starting key of this item and it has to be already
	   filled in */
	return unit_key_by_coord(coord, result);
}

/* ->nr_units() method for items consisting of exactly one unit always */
pos_in_node_t
nr_units_single_unit(const coord_t * coord UNUSED_ARG /* coord of item */ )
{
	return 1;
}

static int
paste_no_paste(coord_t * coord UNUSED_ARG,
	       reiser4_item_data * data UNUSED_ARG,
	       carry_plugin_info * info UNUSED_ARG)
{
	return 0;
}

/* default ->fast_paste() method */
static int
agree_to_fast_op(const coord_t * coord UNUSED_ARG /* coord of item */ )
{
	return 1;
}

int item_can_contain_key(const coord_t * item /* coord of item */ ,
			 const reiser4_key * key /* key to check */ ,
			 const reiser4_item_data * data	/* parameters of item
							 * being created */ )
{
	item_plugin *iplug;
	reiser4_key min_key_in_item;
	reiser4_key max_key_in_item;

	assert("nikita-1658", item != NULL);
	assert("nikita-1659", key != NULL);

	iplug = item_plugin_by_coord(item);
	if (iplug->b.can_contain_key != NULL)
		return iplug->b.can_contain_key(item, key, data);
	else {
		assert("nikita-1681", iplug->b.max_key_inside != NULL);
		item_key_by_coord(item, &min_key_in_item);
		iplug->b.max_key_inside(item, &max_key_in_item);

		/* can contain key if
		   min_key_in_item <= key &&
		   key <= max_key_in_item
		 */
		return keyle(&min_key_in_item, key)
		    && keyle(key, &max_key_in_item);
	}
}

/* mergeable method for non mergeable items */
static int
not_mergeable(const coord_t * i1 UNUSED_ARG, const coord_t * i2 UNUSED_ARG)
{
	return 0;
}

/* return 0 if @item1 and @item2 are not mergeable, !0 - otherwise */
int are_items_mergeable(const coord_t * i1 /* coord of first item */ ,
			const coord_t * i2 /* coord of second item */ )
{
	item_plugin *iplug;
	reiser4_key k1;
	reiser4_key k2;

	assert("nikita-1336", i1 != NULL);
	assert("nikita-1337", i2 != NULL);

	iplug = item_plugin_by_coord(i1);
	assert("nikita-1338", iplug != NULL);

	/* NOTE-NIKITA are_items_mergeable() is also called by assertions in
	   shifting code when nodes are in "suspended" state. */
	assert("nikita-1663",
	       keyle(item_key_by_coord(i1, &k1), item_key_by_coord(i2, &k2)));

	if (iplug->b.mergeable != NULL) {
		return iplug->b.mergeable(i1, i2);
	} else if (iplug->b.max_key_inside != NULL) {
		iplug->b.max_key_inside(i1, &k1);
		item_key_by_coord(i2, &k2);

		/* mergeable if ->max_key_inside() >= key of i2; */
		return keyge(iplug->b.max_key_inside(i1, &k1),
			     item_key_by_coord(i2, &k2));
	} else {
		item_key_by_coord(i1, &k1);
		item_key_by_coord(i2, &k2);

		return
		    (get_key_locality(&k1) == get_key_locality(&k2)) &&
		    (get_key_objectid(&k1) == get_key_objectid(&k2))
		    && (iplug == item_plugin_by_coord(i2));
	}
}

int item_is_extent(const coord_t * item)
{
	assert("vs-482", coord_is_existing_item(item));
	return item_id_by_coord(item) == EXTENT_POINTER_ID;
}

int item_is_tail(const coord_t * item)
{
	assert("vs-482", coord_is_existing_item(item));
	return item_id_by_coord(item) == FORMATTING_ID;
}

#if REISER4_DEBUG

int item_is_statdata(const coord_t * item)
{
	assert("vs-516", coord_is_existing_item(item));
	return plugin_of_group(item_plugin_by_coord(item), STAT_DATA_ITEM_TYPE);
}

int item_is_ctail(const coord_t * item)
{
	assert("edward-xx", coord_is_existing_item(item));
	return item_id_by_coord(item) == CTAIL_ID;
}

#endif  /*  REISER4_DEBUG  */

static int change_item(struct inode *inode,
		       reiser4_plugin * plugin,
		       pset_member memb)
{
	/* cannot change constituent item (sd, or dir_item) */
	return RETERR(-EINVAL);
}

static reiser4_plugin_ops item_plugin_ops = {
	.init = NULL,
	.load = NULL,
	.save_len = NULL,
	.save = NULL,
	.change = change_item
};

item_plugin item_plugins[LAST_ITEM_ID] = {
	[STATIC_STAT_DATA_ID] = {
		.h = {
			.type_id = REISER4_ITEM_PLUGIN_TYPE,
			.id = STATIC_STAT_DATA_ID,
			.groups = (1 << STAT_DATA_ITEM_TYPE),
			.pops = &item_plugin_ops,
			.label = "sd",
			.desc = "stat-data",
			.linkage = {NULL, NULL}
		},
		.b = {
			.max_key_inside = max_key_inside_single_key,
			.can_contain_key = NULL,
			.mergeable = not_mergeable,
			.nr_units = nr_units_single_unit,
			.lookup = NULL,
			.init = NULL,
			.paste = paste_no_paste,
			.fast_paste = NULL,
			.can_shift = NULL,
			.copy_units = NULL,
			.create_hook = NULL,
			.kill_hook = NULL,
			.shift_hook = NULL,
			.cut_units = NULL,
			.kill_units = NULL,
			.unit_key = NULL,
			.max_unit_key = NULL,
			.estimate = NULL,
			.item_data_by_flow = NULL,
#if REISER4_DEBUG
			.check = NULL
#endif
		},
		.f = {
			.utmost_child = NULL,
			.utmost_child_real_block = NULL,
			.update = NULL,
			.scan = NULL,
			.convert = NULL
		},
		.s = {
			.sd = {
				.init_inode = init_inode_static_sd,
				.save_len = save_len_static_sd,
				.save = save_static_sd
			}
		}
	},
	[SIMPLE_DIR_ENTRY_ID] = {
		.h = {
			.type_id = REISER4_ITEM_PLUGIN_TYPE,
			.id = SIMPLE_DIR_ENTRY_ID,
			.groups = (1 << DIR_ENTRY_ITEM_TYPE),
			.pops = &item_plugin_ops,
			.label = "de",
			.desc = "directory entry",
			.linkage = {NULL, NULL}
		},
		.b = {
			.max_key_inside = max_key_inside_single_key,
			.can_contain_key = NULL,
			.mergeable = NULL,
			.nr_units = nr_units_single_unit,
			.lookup = NULL,
			.init = NULL,
			.paste = NULL,
			.fast_paste = NULL,
			.can_shift = NULL,
			.copy_units = NULL,
			.create_hook = NULL,
			.kill_hook = NULL,
			.shift_hook = NULL,
			.cut_units = NULL,
			.kill_units = NULL,
			.unit_key = NULL,
			.max_unit_key = NULL,
			.estimate = NULL,
			.item_data_by_flow = NULL,
#if REISER4_DEBUG
			.check = NULL
#endif
		},
		.f = {
			.utmost_child = NULL,
			.utmost_child_real_block = NULL,
			.update = NULL,
			.scan = NULL,
			.convert = NULL
		},
		.s = {
			.dir = {
				.extract_key = extract_key_de,
				.update_key = update_key_de,
				.extract_name = extract_name_de,
				.extract_file_type = extract_file_type_de,
				.add_entry = add_entry_de,
				.rem_entry = rem_entry_de,
				.max_name_len = max_name_len_de
			}
		}
	},
	[COMPOUND_DIR_ID] = {
		.h = {
			.type_id = REISER4_ITEM_PLUGIN_TYPE,
			.id = COMPOUND_DIR_ID,
			.groups = (1 << DIR_ENTRY_ITEM_TYPE),
			.pops = &item_plugin_ops,
			.label = "cde",
			.desc = "compressed directory entry",
			.linkage = {NULL, NULL}
		},
		.b = {
			.max_key_inside = max_key_inside_cde,
			.can_contain_key = can_contain_key_cde,
			.mergeable = mergeable_cde,
			.nr_units = nr_units_cde,
			.lookup = lookup_cde,
			.init = init_cde,
			.paste = paste_cde,
			.fast_paste = agree_to_fast_op,
			.can_shift = can_shift_cde,
			.copy_units = copy_units_cde,
			.create_hook = NULL,
			.kill_hook = NULL,
			.shift_hook = NULL,
			.cut_units = cut_units_cde,
			.kill_units = kill_units_cde,
			.unit_key = unit_key_cde,
			.max_unit_key = unit_key_cde,
			.estimate = estimate_cde,
			.item_data_by_flow = NULL,
#if REISER4_DEBUG
			.check = reiser4_check_cde
#endif
		},
		.f = {
			.utmost_child = NULL,
			.utmost_child_real_block = NULL,
			.update = NULL,
			.scan = NULL,
			.convert = NULL
		},
		.s = {
			.dir = {
				.extract_key = extract_key_cde,
				.update_key = update_key_cde,
				.extract_name = extract_name_cde,
				.extract_file_type = extract_file_type_de,
				.add_entry = add_entry_cde,
				.rem_entry = rem_entry_cde,
				.max_name_len = max_name_len_cde
			}
		}
	},
	[NODE_POINTER_ID] = {
		.h = {
			.type_id = REISER4_ITEM_PLUGIN_TYPE,
			.id = NODE_POINTER_ID,
			.groups = (1 << INTERNAL_ITEM_TYPE),
			.pops = NULL,
			.label = "internal",
			.desc = "internal item",
			.linkage = {NULL, NULL}
		},
		.b = {
			.max_key_inside = NULL,
			.can_contain_key = NULL,
			.mergeable = mergeable_internal,
			.nr_units = nr_units_single_unit,
			.lookup = lookup_internal,
			.init = NULL,
			.paste = NULL,
			.fast_paste = NULL,
			.can_shift = NULL,
			.copy_units = NULL,
			.create_hook = create_hook_internal,
			.kill_hook = kill_hook_internal,
			.shift_hook = shift_hook_internal,
			.cut_units = NULL,
			.kill_units = NULL,
			.unit_key = NULL,
			.max_unit_key = NULL,
			.estimate = NULL,
			.item_data_by_flow = NULL,
#if REISER4_DEBUG
			.check = check__internal
#endif
		},
		.f = {
			.utmost_child = utmost_child_internal,
			.utmost_child_real_block =
			utmost_child_real_block_internal,
			.update = reiser4_update_internal,
			.scan = NULL,
			.convert = NULL
		},
		.s = {
			.internal = {
				.down_link = down_link_internal,
				.has_pointer_to = has_pointer_to_internal
			}
		}
	},
	[EXTENT_POINTER_ID] = {
		.h = {
			.type_id = REISER4_ITEM_PLUGIN_TYPE,
			.id = EXTENT_POINTER_ID,
			.groups = (1 << UNIX_FILE_METADATA_ITEM_TYPE),
			.pops = NULL,
			.label = "extent",
			.desc = "extent item",
			.linkage = {NULL, NULL}
		},
		.b = {
			.max_key_inside = max_key_inside_extent,
			.can_contain_key = can_contain_key_extent,
			.mergeable = mergeable_extent,
			.nr_units = nr_units_extent,
			.lookup = lookup_extent,
			.init = NULL,
			.paste = paste_extent,
			.fast_paste = agree_to_fast_op,
			.can_shift = can_shift_extent,
			.create_hook = create_hook_extent,
			.copy_units = copy_units_extent,
			.kill_hook = kill_hook_extent,
			.shift_hook = NULL,
			.cut_units = cut_units_extent,
			.kill_units = kill_units_extent,
			.unit_key = unit_key_extent,
			.max_unit_key = max_unit_key_extent,
			.estimate = NULL,
			.item_data_by_flow = NULL,
#if REISER4_DEBUG
			.check = reiser4_check_extent
#endif
		},
		.f = {
			.utmost_child = utmost_child_extent,
			.utmost_child_real_block =
			utmost_child_real_block_extent,
			.update = NULL,
			.scan = reiser4_scan_extent,
			.convert = NULL,
			.key_by_offset = key_by_offset_extent
		},
		.s = {
			.file = {
				.write = reiser4_write_extent,
				.read = reiser4_read_extent,
				.readpage = reiser4_readpage_extent,
				.get_block = get_block_address_extent,
				.append_key = append_key_extent,
				.init_coord_extension =
				init_coord_extension_extent
			}
		}
	},
	[FORMATTING_ID] = {
		.h = {
			.type_id = REISER4_ITEM_PLUGIN_TYPE,
			.id = FORMATTING_ID,
			.groups = (1 << UNIX_FILE_METADATA_ITEM_TYPE),
			.pops = NULL,
			.label = "body",
			.desc = "body (or tail?) item",
			.linkage = {NULL, NULL}
		},
		.b = {
			.max_key_inside = max_key_inside_tail,
			.can_contain_key = can_contain_key_tail,
			.mergeable = mergeable_tail,
			.nr_units = nr_units_tail,
			.lookup = lookup_tail,
			.init = NULL,
			.paste = paste_tail,
			.fast_paste = agree_to_fast_op,
			.can_shift = can_shift_tail,
			.create_hook = NULL,
			.copy_units = copy_units_tail,
			.kill_hook = kill_hook_tail,
			.shift_hook = NULL,
			.cut_units = cut_units_tail,
			.kill_units = kill_units_tail,
			.unit_key = unit_key_tail,
			.max_unit_key = unit_key_tail,
			.estimate = NULL,
			.item_data_by_flow = NULL,
#if REISER4_DEBUG
			.check = NULL
#endif
		},
		.f = {
			.utmost_child = NULL,
			.utmost_child_real_block = NULL,
			.update = NULL,
			.scan = NULL,
			.convert = NULL
		},
		.s = {
			.file = {
				.write = reiser4_write_tail,
				.read = reiser4_read_tail,
				.readpage = readpage_tail,
				.get_block = get_block_address_tail,
				.append_key = append_key_tail,
				.init_coord_extension =
				init_coord_extension_tail
			}
		}
	},
	[CTAIL_ID] = {
		.h = {
			.type_id = REISER4_ITEM_PLUGIN_TYPE,
			.id = CTAIL_ID,
			.groups = (1 << UNIX_FILE_METADATA_ITEM_TYPE),
			.pops = NULL,
			.label = "ctail",
			.desc = "cryptcompress tail item",
			.linkage = {NULL, NULL}
		},
		.b = {
			.max_key_inside = max_key_inside_tail,
			.can_contain_key = can_contain_key_ctail,
			.mergeable = mergeable_ctail,
			.nr_units = nr_units_ctail,
			.lookup = NULL,
			.init = init_ctail,
			.paste = paste_ctail,
			.fast_paste = agree_to_fast_op,
			.can_shift = can_shift_ctail,
			.create_hook = create_hook_ctail,
			.copy_units = copy_units_ctail,
			.kill_hook = kill_hook_ctail,
			.shift_hook = shift_hook_ctail,
			.cut_units = cut_units_ctail,
			.kill_units = kill_units_ctail,
			.unit_key = unit_key_tail,
			.max_unit_key = unit_key_tail,
			.estimate = estimate_ctail,
			.item_data_by_flow = NULL,
#if REISER4_DEBUG
			.check = check_ctail
#endif
		},
		.f = {
			.utmost_child = utmost_child_ctail,
			/* FIXME-EDWARD: write this */
			.utmost_child_real_block = NULL,
			.update = NULL,
			.scan = scan_ctail,
			.convert = convert_ctail
		},
		.s = {
			.file = {
				.write = NULL,
				.read = read_ctail,
				.readpage = readpage_ctail,
				.get_block = get_block_address_tail,
				.append_key = append_key_ctail,
				.init_coord_extension =
				init_coord_extension_tail
			}
		}
	},
	[BLACK_BOX_ID] = {
		.h = {
			.type_id = REISER4_ITEM_PLUGIN_TYPE,
			.id = BLACK_BOX_ID,
			.groups = (1 << OTHER_ITEM_TYPE),
			.pops = NULL,
			.label = "blackbox",
			.desc = "black box item",
			.linkage = {NULL, NULL}
		},
		.b = {
			.max_key_inside = NULL,
			.can_contain_key = NULL,
			.mergeable = not_mergeable,
			.nr_units = nr_units_single_unit,
			/* to need for ->lookup method */
			.lookup = NULL,
			.init = NULL,
			.paste = NULL,
			.fast_paste = NULL,
			.can_shift = NULL,
			.copy_units = NULL,
			.create_hook = NULL,
			.kill_hook = NULL,
			.shift_hook = NULL,
			.cut_units = NULL,
			.kill_units = NULL,
			.unit_key = NULL,
			.max_unit_key = NULL,
			.estimate = NULL,
			.item_data_by_flow = NULL,
#if REISER4_DEBUG
			.check = NULL
#endif
		}
	}
};

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
