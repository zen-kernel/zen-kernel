/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* Node plugin interface.

   Description: The tree provides the abstraction of flows, which it
   internally fragments into items which it stores in nodes.

   A key_atom is a piece of data bound to a single key.

   For reasonable space efficiency to be achieved it is often
   necessary to store key_atoms in the nodes in the form of items, where
   an item is a sequence of key_atoms of the same or similar type. It is
   more space-efficient, because the item can implement (very)
   efficient compression of key_atom's bodies using internal knowledge
   about their semantics, and it can often avoid having a key for each
   key_atom. Each type of item has specific operations implemented by its
   item handler (see balance.c).

   Rationale: the rest of the code (specifically balancing routines)
   accesses leaf level nodes through this interface. This way we can
   implement various block layouts and even combine various layouts
   within the same tree. Balancing/allocating algorithms should not
   care about peculiarities of splitting/merging specific item types,
   but rather should leave that to the item's item handler.

   Items, including those that provide the abstraction of flows, have
   the property that if you move them in part or in whole to another
   node, the balancing code invokes their is_left_mergeable()
   item_operation to determine if they are mergeable with their new
   neighbor in the node you have moved them to.  For some items the
   is_left_mergeable() function always returns null.

   When moving the bodies of items from one node to another:

     if a partial item is shifted to another node the balancing code invokes
     an item handler method to handle the item splitting.

     if the balancing code needs to merge with an item in the node it
     is shifting to, it will invoke an item handler method to handle
     the item merging.

     if it needs to move whole item bodies unchanged, the balancing code uses xmemcpy()
     adjusting the item headers after the move is done using the node handler.
*/

#include "../../forward.h"
#include "../../debug.h"
#include "../../key.h"
#include "../../coord.h"
#include "../plugin_header.h"
#include "../item/item.h"
#include "node.h"
#include "../plugin.h"
#include "../../znode.h"
#include "../../tree.h"
#include "../../super.h"
#include "../../reiser4.h"

/**
 * leftmost_key_in_node - get the smallest key in node
 * @node:
 * @key: store result here
 *
 * Stores the leftmost key of @node in @key.
 */
reiser4_key *leftmost_key_in_node(const znode *node, reiser4_key *key)
{
	assert("nikita-1634", node != NULL);
	assert("nikita-1635", key != NULL);

	if (!node_is_empty(node)) {
		coord_t first_item;

		coord_init_first_unit(&first_item, (znode *) node);
		item_key_by_coord(&first_item, key);
	} else
		*key = *reiser4_max_key();
	return key;
}

node_plugin node_plugins[LAST_NODE_ID] = {
	[NODE40_ID] = {
		.h = {
			.type_id = REISER4_NODE_PLUGIN_TYPE,
			.id = NODE40_ID,
			.pops = NULL,
			.label = "unified",
			.desc = "unified node layout",
			.linkage = {NULL, NULL}
		},
		.item_overhead = item_overhead_node40,
		.free_space = free_space_node40,
		.lookup = lookup_node40,
		.num_of_items = num_of_items_node40,
		.item_by_coord = item_by_coord_node40,
		.length_by_coord = length_by_coord_node40,
		.plugin_by_coord = plugin_by_coord_node40,
		.key_at = key_at_node40,
		.estimate = estimate_node40,
		.check = check_node40,
		.parse = parse_node40,
		.init = init_node40,
#ifdef GUESS_EXISTS
		.guess = guess_node40,
#endif
		.change_item_size = change_item_size_node40,
		.create_item = create_item_node40,
		.update_item_key = update_item_key_node40,
		.cut_and_kill = kill_node40,
		.cut = cut_node40,
		.shift = shift_node40,
		.shrink_item = shrink_item_node40,
		.fast_insert = fast_insert_node40,
		.fast_paste = fast_paste_node40,
		.fast_cut = fast_cut_node40,
		.max_item_size = max_item_size_node40,
		.prepare_removal = prepare_removal_node40,
		.set_item_plugin = set_item_plugin_node40
	}
};

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
