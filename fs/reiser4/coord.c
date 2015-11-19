/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
   reiser4/README */

#include "forward.h"
#include "debug.h"
#include "dformat.h"
#include "tree.h"
#include "plugin/item/item.h"
#include "znode.h"
#include "coord.h"

/* Internal constructor. */
static inline void
coord_init_values(coord_t *coord, const znode * node, pos_in_node_t item_pos,
		  pos_in_node_t unit_pos, between_enum between)
{
	coord->node = (znode *) node;
	coord_set_item_pos(coord, item_pos);
	coord->unit_pos = unit_pos;
	coord->between = between;
	ON_DEBUG(coord->plug_v = 0);
	ON_DEBUG(coord->body_v = 0);

	/*ON_TRACE (TRACE_COORDS, "init coord %p node %p: %u %u %s\n", coord,
 node, item_pos, unit_pos, coord_tween_tostring (between)); */
}

/* after shifting of node content, coord previously set properly may become
   invalid, try to "normalize" it. */
void coord_normalize(coord_t *coord)
{
	znode *node;

	node = coord->node;
	assert("vs-683", node);

	coord_clear_iplug(coord);

	if (node_is_empty(node)) {
		coord_init_first_unit(coord, node);
	} else if ((coord->between == AFTER_ITEM)
		   || (coord->between == AFTER_UNIT)) {
		return;
	} else if (coord->item_pos == coord_num_items(coord)
		   && coord->between == BEFORE_ITEM) {
		coord_dec_item_pos(coord);
		coord->between = AFTER_ITEM;
	} else if (coord->unit_pos == coord_num_units(coord)
		   && coord->between == BEFORE_UNIT) {
		coord->unit_pos--;
		coord->between = AFTER_UNIT;
	} else if (coord->item_pos == coord_num_items(coord)
		   && coord->unit_pos == 0 && coord->between == BEFORE_UNIT) {
		coord_dec_item_pos(coord);
		coord->unit_pos = 0;
		coord->between = AFTER_ITEM;
	}
}

/* Copy a coordinate. */
void coord_dup(coord_t *coord, const coord_t *old_coord)
{
	assert("jmacd-9800", coord_check(old_coord));
	coord_dup_nocheck(coord, old_coord);
}

/* Copy a coordinate without check. Useful when old_coord->node is not
   loaded. As in cbk_tree_lookup -> connect_znode -> connect_one_side */
void coord_dup_nocheck(coord_t *coord, const coord_t *old_coord)
{
	coord->node = old_coord->node;
	coord_set_item_pos(coord, old_coord->item_pos);
	coord->unit_pos = old_coord->unit_pos;
	coord->between = old_coord->between;
	coord->iplugid = old_coord->iplugid;
	ON_DEBUG(coord->plug_v = old_coord->plug_v);
	ON_DEBUG(coord->body_v = old_coord->body_v);
}

/* Initialize an invalid coordinate. */
void coord_init_invalid(coord_t *coord, const znode * node)
{
	coord_init_values(coord, node, 0, 0, INVALID_COORD);
}

void coord_init_first_unit_nocheck(coord_t *coord, const znode * node)
{
	coord_init_values(coord, node, 0, 0, AT_UNIT);
}

/* Initialize a coordinate to point at the first unit of the first item. If the
   node is empty, it is positioned at the EMPTY_NODE. */
void coord_init_first_unit(coord_t *coord, const znode * node)
{
	int is_empty = node_is_empty(node);

	coord_init_values(coord, node, 0, 0, (is_empty ? EMPTY_NODE : AT_UNIT));

	assert("jmacd-9801", coord_check(coord));
}

/* Initialize a coordinate to point at the last unit of the last item.  If the
   node is empty, it is positioned at the EMPTY_NODE. */
void coord_init_last_unit(coord_t *coord, const znode * node)
{
	int is_empty = node_is_empty(node);

	coord_init_values(coord, node,
			  (is_empty ? 0 : node_num_items(node) - 1), 0,
			  (is_empty ? EMPTY_NODE : AT_UNIT));
	if (!is_empty)
		coord->unit_pos = coord_last_unit_pos(coord);
	assert("jmacd-9802", coord_check(coord));
}

/* Initialize a coordinate to before the first item. If the node is empty, it is
   positioned at the EMPTY_NODE. */
void coord_init_before_first_item(coord_t *coord, const znode * node)
{
	int is_empty = node_is_empty(node);

	coord_init_values(coord, node, 0, 0,
			  (is_empty ? EMPTY_NODE : BEFORE_UNIT));

	assert("jmacd-9803", coord_check(coord));
}

/* Initialize a coordinate to after the last item. If the node is empty, it is
   positioned at the EMPTY_NODE. */
void coord_init_after_last_item(coord_t *coord, const znode * node)
{
	int is_empty = node_is_empty(node);

	coord_init_values(coord, node,
			  (is_empty ? 0 : node_num_items(node) - 1), 0,
			  (is_empty ? EMPTY_NODE : AFTER_ITEM));

	assert("jmacd-9804", coord_check(coord));
}

/* Initialize a coordinate to after last unit in the item. Coord must be set
   already to existing item */
void coord_init_after_item_end(coord_t *coord)
{
	coord->between = AFTER_UNIT;
	coord->unit_pos = coord_last_unit_pos(coord);
}

/* Initialize a coordinate to before the item. Coord must be set already to
   existing item */
void coord_init_before_item(coord_t *coord)
{
	coord->unit_pos = 0;
	coord->between = BEFORE_ITEM;
}

/* Initialize a coordinate to after the item. Coord must be set already to
   existing item */
void coord_init_after_item(coord_t *coord)
{
	coord->unit_pos = 0;
	coord->between = AFTER_ITEM;
}

/* Initialize a coordinate by 0s. Used in places where init_coord was used and
   it was not clear how actually */
void coord_init_zero(coord_t *coord)
{
	memset(coord, 0, sizeof(*coord));
}

/* Return the number of units at the present item.
   Asserts coord_is_existing_item(). */
unsigned coord_num_units(const coord_t *coord)
{
	assert("jmacd-9806", coord_is_existing_item(coord));

	return item_plugin_by_coord(coord)->b.nr_units(coord);
}

/* Returns true if the coord was initializewd by coord_init_invalid (). */
/* Audited by: green(2002.06.15) */
int coord_is_invalid(const coord_t *coord)
{
	return coord->between == INVALID_COORD;
}

/* Returns true if the coordinate is positioned at an existing item, not before
   or after an item.  It may be placed at, before, or after any unit within the
   item, whether existing or not. */
int coord_is_existing_item(const coord_t *coord)
{
	switch (coord->between) {
	case EMPTY_NODE:
	case BEFORE_ITEM:
	case AFTER_ITEM:
	case INVALID_COORD:
		return 0;

	case BEFORE_UNIT:
	case AT_UNIT:
	case AFTER_UNIT:
		return coord->item_pos < coord_num_items(coord);
	}

	impossible("jmacd-9900", "unreachable coord: %p", coord);
	return 0;
}

/* Returns true if the coordinate is positioned at an existing unit, not before
   or after a unit. */
/* Audited by: green(2002.06.15) */
int coord_is_existing_unit(const coord_t *coord)
{
	switch (coord->between) {
	case EMPTY_NODE:
	case BEFORE_UNIT:
	case AFTER_UNIT:
	case BEFORE_ITEM:
	case AFTER_ITEM:
	case INVALID_COORD:
		return 0;

	case AT_UNIT:
		return (coord->item_pos < coord_num_items(coord)
			&& coord->unit_pos < coord_num_units(coord));
	}

	impossible("jmacd-9902", "unreachable");
	return 0;
}

/* Returns true if the coordinate is positioned at the first unit of the first
   item. Not true for empty nodes nor coordinates positioned before the first
   item. */
/* Audited by: green(2002.06.15) */
int coord_is_leftmost_unit(const coord_t *coord)
{
	return (coord->between == AT_UNIT && coord->item_pos == 0
		&& coord->unit_pos == 0);
}

#if REISER4_DEBUG
/* For assertions only, checks for a valid coordinate. */
int coord_check(const coord_t *coord)
{
	if (coord->node == NULL)
		return 0;
	if (znode_above_root(coord->node))
		return 1;

	switch (coord->between) {
	default:
	case INVALID_COORD:
		return 0;
	case EMPTY_NODE:
		if (!node_is_empty(coord->node))
			return 0;
		return coord->item_pos == 0 && coord->unit_pos == 0;

	case BEFORE_UNIT:
	case AFTER_UNIT:
		if (node_is_empty(coord->node) && (coord->item_pos == 0)
		    && (coord->unit_pos == 0))
			return 1;
	case AT_UNIT:
		break;
	case AFTER_ITEM:
	case BEFORE_ITEM:
		/* before/after item should not set unit_pos. */
		if (coord->unit_pos != 0)
			return 0;
		break;
	}

	if (coord->item_pos >= node_num_items(coord->node))
		return 0;

	/* FIXME-VS: we are going to check unit_pos. This makes no sense when
	   between is set either AFTER_ITEM or BEFORE_ITEM */
	if (coord->between == AFTER_ITEM || coord->between == BEFORE_ITEM)
		return 1;

	if (coord_is_iplug_set(coord) &&
	    coord->unit_pos >
	    item_plugin_by_coord(coord)->b.nr_units(coord) - 1)
		return 0;
	return 1;
}
#endif

/* Adjust coordinate boundaries based on the number of items prior to
   coord_next/prev. Returns 1 if the new position is does not exist. */
static int coord_adjust_items(coord_t *coord, unsigned items, int is_next)
{
	/* If the node is invalid, leave it. */
	if (coord->between == INVALID_COORD)
		return 1;

	/* If the node is empty, set it appropriately. */
	if (items == 0) {
		coord->between = EMPTY_NODE;
		coord_set_item_pos(coord, 0);
		coord->unit_pos = 0;
		return 1;
	}

	/* If it was empty and it no longer is, set to BEFORE/AFTER_ITEM. */
	if (coord->between == EMPTY_NODE) {
		coord->between = (is_next ? BEFORE_ITEM : AFTER_ITEM);
		coord_set_item_pos(coord, 0);
		coord->unit_pos = 0;
		return 0;
	}

	/* If the item_pos is out-of-range, set it appropriatly. */
	if (coord->item_pos >= items) {
		coord->between = AFTER_ITEM;
		coord_set_item_pos(coord, items - 1);
		coord->unit_pos = 0;
		/* If is_next, return 1 (can't go any further). */
		return is_next;
	}

	return 0;
}

/* Advances the coordinate by one unit to the right.  If empty, no change.  If
   coord_is_rightmost_unit, advances to AFTER THE LAST ITEM.  Returns 0 if new
   position is an existing unit. */
int coord_next_unit(coord_t *coord)
{
	unsigned items = coord_num_items(coord);

	if (coord_adjust_items(coord, items, 1) == 1)
		return 1;

	switch (coord->between) {
	case BEFORE_UNIT:
		/* Now it is positioned at the same unit. */
		coord->between = AT_UNIT;
		return 0;

	case AFTER_UNIT:
	case AT_UNIT:
		/* If it was at or after a unit and there are more units in this
		   item, advance to the next one. */
		if (coord->unit_pos < coord_last_unit_pos(coord)) {
			coord->unit_pos += 1;
			coord->between = AT_UNIT;
			return 0;
		}

		/* Otherwise, it is crossing an item boundary and treated as if
		   it was after the current item. */
		coord->between = AFTER_ITEM;
		coord->unit_pos = 0;
		/* FALLTHROUGH */

	case AFTER_ITEM:
		/* Check for end-of-node. */
		if (coord->item_pos == items - 1)
			return 1;

		coord_inc_item_pos(coord);
		coord->unit_pos = 0;
		coord->between = AT_UNIT;
		return 0;

	case BEFORE_ITEM:
		/* The adjust_items checks ensure that we are valid here. */
		coord->unit_pos = 0;
		coord->between = AT_UNIT;
		return 0;

	case INVALID_COORD:
	case EMPTY_NODE:
		/* Handled in coord_adjust_items(). */
		break;
	}

	impossible("jmacd-9902", "unreachable");
	return 0;
}

/* Advances the coordinate by one item to the right.  If empty, no change.  If
   coord_is_rightmost_unit, advances to AFTER THE LAST ITEM.  Returns 0 if new
   position is an existing item. */
int coord_next_item(coord_t *coord)
{
	unsigned items = coord_num_items(coord);

	if (coord_adjust_items(coord, items, 1) == 1)
		return 1;

	switch (coord->between) {
	case AFTER_UNIT:
	case AT_UNIT:
	case BEFORE_UNIT:
	case AFTER_ITEM:
		/* Check for end-of-node. */
		if (coord->item_pos == items - 1) {
			coord->between = AFTER_ITEM;
			coord->unit_pos = 0;
			coord_clear_iplug(coord);
			return 1;
		}

		/* Anywhere in an item, go to the next one. */
		coord->between = AT_UNIT;
		coord_inc_item_pos(coord);
		coord->unit_pos = 0;
		return 0;

	case BEFORE_ITEM:
		/* The out-of-range check ensures that we are valid here. */
		coord->unit_pos = 0;
		coord->between = AT_UNIT;
		return 0;
	case INVALID_COORD:
	case EMPTY_NODE:
		/* Handled in coord_adjust_items(). */
		break;
	}

	impossible("jmacd-9903", "unreachable");
	return 0;
}

/* Advances the coordinate by one unit to the left.  If empty, no change.  If
   coord_is_leftmost_unit, advances to BEFORE THE FIRST ITEM.  Returns 0 if new
   position is an existing unit. */
int coord_prev_unit(coord_t *coord)
{
	unsigned items = coord_num_items(coord);

	if (coord_adjust_items(coord, items, 0) == 1)
		return 1;

	switch (coord->between) {
	case AT_UNIT:
	case BEFORE_UNIT:
		if (coord->unit_pos > 0) {
			coord->unit_pos -= 1;
			coord->between = AT_UNIT;
			return 0;
		}

		if (coord->item_pos == 0) {
			coord->between = BEFORE_ITEM;
			return 1;
		}

		coord_dec_item_pos(coord);
		coord->unit_pos = coord_last_unit_pos(coord);
		coord->between = AT_UNIT;
		return 0;

	case AFTER_UNIT:
		/* What if unit_pos is out-of-range? */
		assert("jmacd-5442",
		       coord->unit_pos <= coord_last_unit_pos(coord));
		coord->between = AT_UNIT;
		return 0;

	case BEFORE_ITEM:
		if (coord->item_pos == 0)
			return 1;

		coord_dec_item_pos(coord);
		/* FALLTHROUGH */

	case AFTER_ITEM:
		coord->between = AT_UNIT;
		coord->unit_pos = coord_last_unit_pos(coord);
		return 0;

	case INVALID_COORD:
	case EMPTY_NODE:
		break;
	}

	impossible("jmacd-9904", "unreachable");
	return 0;
}

/* Advances the coordinate by one item to the left.  If empty, no change.  If
   coord_is_leftmost_unit, advances to BEFORE THE FIRST ITEM.  Returns 0 if new
   position is an existing item. */
int coord_prev_item(coord_t *coord)
{
	unsigned items = coord_num_items(coord);

	if (coord_adjust_items(coord, items, 0) == 1)
		return 1;

	switch (coord->between) {
	case AT_UNIT:
	case AFTER_UNIT:
	case BEFORE_UNIT:
	case BEFORE_ITEM:

		if (coord->item_pos == 0) {
			coord->between = BEFORE_ITEM;
			coord->unit_pos = 0;
			return 1;
		}

		coord_dec_item_pos(coord);
		coord->unit_pos = 0;
		coord->between = AT_UNIT;
		return 0;

	case AFTER_ITEM:
		coord->between = AT_UNIT;
		coord->unit_pos = 0;
		return 0;

	case INVALID_COORD:
	case EMPTY_NODE:
		break;
	}

	impossible("jmacd-9905", "unreachable");
	return 0;
}

/* Calls either coord_init_first_unit or coord_init_last_unit depending on
   sideof argument. */
void coord_init_sideof_unit(coord_t *coord, const znode * node, sideof dir)
{
	assert("jmacd-9821", dir == LEFT_SIDE || dir == RIGHT_SIDE);
	if (dir == LEFT_SIDE) {
		coord_init_first_unit(coord, node);
	} else {
		coord_init_last_unit(coord, node);
	}
}

/* Calls either coord_is_before_leftmost or coord_is_after_rightmost depending
   on sideof argument. */
/* Audited by: green(2002.06.15) */
int coord_is_after_sideof_unit(coord_t *coord, sideof dir)
{
	assert("jmacd-9822", dir == LEFT_SIDE || dir == RIGHT_SIDE);
	if (dir == LEFT_SIDE) {
		return coord_is_before_leftmost(coord);
	} else {
		return coord_is_after_rightmost(coord);
	}
}

/* Calls either coord_next_unit or coord_prev_unit depending on sideof argument.
 */
/* Audited by: green(2002.06.15) */
int coord_sideof_unit(coord_t *coord, sideof dir)
{
	assert("jmacd-9823", dir == LEFT_SIDE || dir == RIGHT_SIDE);
	if (dir == LEFT_SIDE) {
		return coord_prev_unit(coord);
	} else {
		return coord_next_unit(coord);
	}
}

#if REISER4_DEBUG
int coords_equal(const coord_t *c1, const coord_t *c2)
{
	assert("nikita-2840", c1 != NULL);
	assert("nikita-2841", c2 != NULL);

	return
	    c1->node == c2->node &&
	    c1->item_pos == c2->item_pos &&
	    c1->unit_pos == c2->unit_pos && c1->between == c2->between;
}
#endif  /*  REISER4_DEBUG  */

/* If coord_is_after_rightmost return NCOORD_ON_THE_RIGHT, if
   coord_is_after_leftmost return NCOORD_ON_THE_LEFT, otherwise return
   NCOORD_INSIDE. */
/* Audited by: green(2002.06.15) */
coord_wrt_node coord_wrt(const coord_t *coord)
{
	if (coord_is_before_leftmost(coord))
		return COORD_ON_THE_LEFT;

	if (coord_is_after_rightmost(coord))
		return COORD_ON_THE_RIGHT;

	return COORD_INSIDE;
}

/* Returns true if the coordinate is positioned after the last item or after the
   last unit of the last item or it is an empty node. */
/* Audited by: green(2002.06.15) */
int coord_is_after_rightmost(const coord_t *coord)
{
	assert("jmacd-7313", coord_check(coord));

	switch (coord->between) {
	case INVALID_COORD:
	case AT_UNIT:
	case BEFORE_UNIT:
	case BEFORE_ITEM:
		return 0;

	case EMPTY_NODE:
		return 1;

	case AFTER_ITEM:
		return (coord->item_pos == node_num_items(coord->node) - 1);

	case AFTER_UNIT:
		return ((coord->item_pos == node_num_items(coord->node) - 1) &&
			coord->unit_pos == coord_last_unit_pos(coord));
	}

	impossible("jmacd-9908", "unreachable");
	return 0;
}

/* Returns true if the coordinate is positioned before the first item or it is
   an empty node. */
int coord_is_before_leftmost(const coord_t *coord)
{
	/* FIXME-VS: coord_check requires node to be loaded whereas it is not
	   necessary to check if coord is set before leftmost
	   assert ("jmacd-7313", coord_check (coord)); */
	switch (coord->between) {
	case INVALID_COORD:
	case AT_UNIT:
	case AFTER_ITEM:
	case AFTER_UNIT:
		return 0;

	case EMPTY_NODE:
		return 1;

	case BEFORE_ITEM:
	case BEFORE_UNIT:
		return (coord->item_pos == 0) && (coord->unit_pos == 0);
	}

	impossible("jmacd-9908", "unreachable");
	return 0;
}

/* Returns true if the coordinate is positioned after a item, before a item,
   after the last unit of an item, before the first unit of an item, or at an
   empty node. */
/* Audited by: green(2002.06.15) */
int coord_is_between_items(const coord_t *coord)
{
	assert("jmacd-7313", coord_check(coord));

	switch (coord->between) {
	case INVALID_COORD:
	case AT_UNIT:
		return 0;

	case AFTER_ITEM:
	case BEFORE_ITEM:
	case EMPTY_NODE:
		return 1;

	case BEFORE_UNIT:
		return coord->unit_pos == 0;

	case AFTER_UNIT:
		return coord->unit_pos == coord_last_unit_pos(coord);
	}

	impossible("jmacd-9908", "unreachable");
	return 0;
}

#if REISER4_DEBUG
/* Returns true if the coordinates are positioned at adjacent units, regardless
   of before-after or item boundaries. */
int coord_are_neighbors(coord_t *c1, coord_t *c2)
{
	coord_t *left;
	coord_t *right;

	assert("nikita-1241", c1 != NULL);
	assert("nikita-1242", c2 != NULL);
	assert("nikita-1243", c1->node == c2->node);
	assert("nikita-1244", coord_is_existing_unit(c1));
	assert("nikita-1245", coord_is_existing_unit(c2));

	left = right = NULL;
	switch (coord_compare(c1, c2)) {
	case COORD_CMP_ON_LEFT:
		left = c1;
		right = c2;
		break;
	case COORD_CMP_ON_RIGHT:
		left = c2;
		right = c1;
		break;
	case COORD_CMP_SAME:
		return 0;
	default:
		wrong_return_value("nikita-1246", "compare_coords()");
	}
	assert("vs-731", left && right);
	if (left->item_pos == right->item_pos) {
		return left->unit_pos + 1 == right->unit_pos;
	} else if (left->item_pos + 1 == right->item_pos) {
		return (left->unit_pos == coord_last_unit_pos(left))
		    && (right->unit_pos == 0);
	} else {
		return 0;
	}
}
#endif  /*  REISER4_DEBUG  */

/* Assuming two coordinates are positioned in the same node, return
   COORD_CMP_ON_RIGHT, COORD_CMP_ON_LEFT, or COORD_CMP_SAME depending on c1's
   position relative to c2.  */
/* Audited by: green(2002.06.15) */
coord_cmp coord_compare(coord_t *c1, coord_t *c2)
{
	assert("vs-209", c1->node == c2->node);
	assert("vs-194", coord_is_existing_unit(c1)
	       && coord_is_existing_unit(c2));

	if (c1->item_pos > c2->item_pos)
		return COORD_CMP_ON_RIGHT;
	if (c1->item_pos < c2->item_pos)
		return COORD_CMP_ON_LEFT;
	if (c1->unit_pos > c2->unit_pos)
		return COORD_CMP_ON_RIGHT;
	if (c1->unit_pos < c2->unit_pos)
		return COORD_CMP_ON_LEFT;
	return COORD_CMP_SAME;
}

/* If the coordinate is between items, shifts it to the right.  Returns 0 on
   success and non-zero if there is no position to the right. */
int coord_set_to_right(coord_t *coord)
{
	unsigned items = coord_num_items(coord);

	if (coord_adjust_items(coord, items, 1) == 1)
		return 1;

	switch (coord->between) {
	case AT_UNIT:
		return 0;

	case BEFORE_ITEM:
	case BEFORE_UNIT:
		coord->between = AT_UNIT;
		return 0;

	case AFTER_UNIT:
		if (coord->unit_pos < coord_last_unit_pos(coord)) {
			coord->unit_pos += 1;
			coord->between = AT_UNIT;
			return 0;
		} else {

			coord->unit_pos = 0;

			if (coord->item_pos == items - 1) {
				coord->between = AFTER_ITEM;
				return 1;
			}

			coord_inc_item_pos(coord);
			coord->between = AT_UNIT;
			return 0;
		}

	case AFTER_ITEM:
		if (coord->item_pos == items - 1)
			return 1;

		coord_inc_item_pos(coord);
		coord->unit_pos = 0;
		coord->between = AT_UNIT;
		return 0;

	case EMPTY_NODE:
		return 1;

	case INVALID_COORD:
		break;
	}

	impossible("jmacd-9920", "unreachable");
	return 0;
}

/* If the coordinate is between items, shifts it to the left.  Returns 0 on
   success and non-zero if there is no position to the left. */
int coord_set_to_left(coord_t *coord)
{
	unsigned items = coord_num_items(coord);

	if (coord_adjust_items(coord, items, 0) == 1)
		return 1;

	switch (coord->between) {
	case AT_UNIT:
		return 0;

	case AFTER_UNIT:
		coord->between = AT_UNIT;
		return 0;

	case AFTER_ITEM:
		coord->between = AT_UNIT;
		coord->unit_pos = coord_last_unit_pos(coord);
		return 0;

	case BEFORE_UNIT:
		if (coord->unit_pos > 0) {
			coord->unit_pos -= 1;
			coord->between = AT_UNIT;
			return 0;
		} else {

			if (coord->item_pos == 0) {
				coord->between = BEFORE_ITEM;
				return 1;
			}

			coord->unit_pos = coord_last_unit_pos(coord);
			coord_dec_item_pos(coord);
			coord->between = AT_UNIT;
			return 0;
		}

	case BEFORE_ITEM:
		if (coord->item_pos == 0)
			return 1;

		coord_dec_item_pos(coord);
		coord->unit_pos = coord_last_unit_pos(coord);
		coord->between = AT_UNIT;
		return 0;

	case EMPTY_NODE:
		return 1;

	case INVALID_COORD:
		break;
	}

	impossible("jmacd-9920", "unreachable");
	return 0;
}

static const char *coord_tween_tostring(between_enum n)
{
	switch (n) {
	case BEFORE_UNIT:
		return "before unit";
	case BEFORE_ITEM:
		return "before item";
	case AT_UNIT:
		return "at unit";
	case AFTER_UNIT:
		return "after unit";
	case AFTER_ITEM:
		return "after item";
	case EMPTY_NODE:
		return "empty node";
	case INVALID_COORD:
		return "invalid";
	default:
	{
		static char buf[30];

		sprintf(buf, "unknown: %i", n);
		return buf;
	}
	}
}

void print_coord(const char *mes, const coord_t *coord, int node)
{
	if (coord == NULL) {
		printk("%s: null\n", mes);
		return;
	}
	printk("%s: item_pos = %d, unit_pos %d, tween=%s, iplug=%d\n",
	       mes, coord->item_pos, coord->unit_pos,
	       coord_tween_tostring(coord->between), coord->iplugid);
}

int
item_utmost_child_real_block(const coord_t *coord, sideof side,
			     reiser4_block_nr * blk)
{
	return item_plugin_by_coord(coord)->f.utmost_child_real_block(coord,
								      side,
								      blk);
}

int item_utmost_child(const coord_t *coord, sideof side, jnode ** child)
{
	return item_plugin_by_coord(coord)->f.utmost_child(coord, side, child);
}

/* @count bytes of flow @f got written, update correspondingly f->length,
   f->data and f->key */
void move_flow_forward(flow_t *f, unsigned count)
{
	if (f->data)
		f->data += count;
	f->length -= count;
	set_key_offset(&f->key, get_key_offset(&f->key) + count);
}

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
