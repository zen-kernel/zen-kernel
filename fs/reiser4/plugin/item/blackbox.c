/* Copyright 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Black box item implementation */

#include "../../forward.h"
#include "../../debug.h"
#include "../../dformat.h"
#include "../../kassign.h"
#include "../../coord.h"
#include "../../tree.h"
#include "../../lock.h"

#include "blackbox.h"
#include "item.h"
#include "../plugin.h"

int
store_black_box(reiser4_tree * tree,
		const reiser4_key * key, void *data, int length)
{
	int result;
	reiser4_item_data idata;
	coord_t coord;
	lock_handle lh;

	memset(&idata, 0, sizeof idata);

	idata.data = data;
	idata.user = 0;
	idata.length = length;
	idata.iplug = item_plugin_by_id(BLACK_BOX_ID);

	init_lh(&lh);
	result = insert_by_key(tree, key,
			       &idata, &coord, &lh, LEAF_LEVEL, CBK_UNIQUE);

	assert("nikita-3413",
	       ergo(result == 0,
		    WITH_COORD(&coord,
			       item_length_by_coord(&coord) == length)));

	done_lh(&lh);
	return result;
}

int
load_black_box(reiser4_tree * tree,
	       reiser4_key * key, void *data, int length, int exact)
{
	int result;
	coord_t coord;
	lock_handle lh;

	init_lh(&lh);
	result = coord_by_key(tree, key,
			      &coord, &lh, ZNODE_READ_LOCK,
			      exact ? FIND_EXACT : FIND_MAX_NOT_MORE_THAN,
			      LEAF_LEVEL, LEAF_LEVEL, CBK_UNIQUE, NULL);

	if (result == 0) {
		int ilen;

		result = zload(coord.node);
		if (result == 0) {
			ilen = item_length_by_coord(&coord);
			if (ilen <= length) {
				memcpy(data, item_body_by_coord(&coord), ilen);
				unit_key_by_coord(&coord, key);
			} else if (exact) {
				/*
				 * item is larger than buffer provided by the
				 * user. Only issue a warning if @exact is
				 * set. If @exact is false, we are iterating
				 * over all safe-links and here we are reaching
				 * the end of the iteration.
				 */
				warning("nikita-3415",
					"Wrong black box length: %i > %i",
					ilen, length);
				result = RETERR(-EIO);
			}
			zrelse(coord.node);
		}
	}

	done_lh(&lh);
	return result;

}

int
update_black_box(reiser4_tree * tree,
		 const reiser4_key * key, void *data, int length)
{
	int result;
	coord_t coord;
	lock_handle lh;

	init_lh(&lh);
	result = coord_by_key(tree, key,
			      &coord, &lh, ZNODE_READ_LOCK,
			      FIND_EXACT,
			      LEAF_LEVEL, LEAF_LEVEL, CBK_UNIQUE, NULL);
	if (result == 0) {
		int ilen;

		result = zload(coord.node);
		if (result == 0) {
			ilen = item_length_by_coord(&coord);
			if (length <= ilen) {
				memcpy(item_body_by_coord(&coord), data,
				       length);
			} else {
				warning("nikita-3437",
					"Wrong black box length: %i < %i",
					ilen, length);
				result = RETERR(-EIO);
			}
			zrelse(coord.node);
		}
	}

	done_lh(&lh);
	return result;

}

int kill_black_box(reiser4_tree * tree, const reiser4_key * key)
{
	return reiser4_cut_tree(tree, key, key, NULL, 1);
}

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
