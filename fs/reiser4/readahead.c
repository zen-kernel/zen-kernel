/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

#include "forward.h"
#include "tree.h"
#include "tree_walk.h"
#include "super.h"
#include "inode.h"
#include "key.h"
#include "znode.h"

#include <linux/swap.h>		/* for totalram_pages */

void reiser4_init_ra_info(ra_info_t *rai)
{
	rai->key_to_stop = *reiser4_min_key();
}

/* global formatted node readahead parameter. It can be set by mount option
 * -o readahead:NUM:1 */
static inline int ra_adjacent_only(int flags)
{
	return flags & RA_ADJACENT_ONLY;
}

/* this is used by formatted_readahead to decide whether read for right neighbor
 * of node is to be issued. It returns 1 if right neighbor's first key is less
 * or equal to readahead's stop key */
static int should_readahead_neighbor(znode * node, ra_info_t *info)
{
	int result;

	read_lock_dk(znode_get_tree(node));
	result = keyle(znode_get_rd_key(node), &info->key_to_stop);
	read_unlock_dk(znode_get_tree(node));
	return result;
}

#define LOW_MEM_PERCENTAGE (5)

static int low_on_memory(void)
{
	unsigned int freepages;

	freepages = nr_free_pages();
	return freepages < (totalram_pages * LOW_MEM_PERCENTAGE / 100);
}

/* start read for @node and for a few of its right neighbors */
void formatted_readahead(znode * node, ra_info_t *info)
{
	struct formatted_ra_params *ra_params;
	znode *cur;
	int i;
	int grn_flags;
	lock_handle next_lh;

	/* do nothing if node block number has not been assigned to node (which
	 * means it is still in cache). */
	if (reiser4_blocknr_is_fake(znode_get_block(node)))
		return;

	ra_params = get_current_super_ra_params();

	if (znode_page(node) == NULL)
		jstartio(ZJNODE(node));

	if (znode_get_level(node) != LEAF_LEVEL)
		return;

	/* don't waste memory for read-ahead when low on memory */
	if (low_on_memory())
		return;

	/* We can have locked nodes on upper tree levels, in this situation lock
	   priorities do not help to resolve deadlocks, we have to use TRY_LOCK
	   here. */
	grn_flags = (GN_CAN_USE_UPPER_LEVELS | GN_TRY_LOCK);

	i = 0;
	cur = zref(node);
	init_lh(&next_lh);
	while (i < ra_params->max) {
		const reiser4_block_nr * nextblk;

		if (!should_readahead_neighbor(cur, info))
			break;

		if (reiser4_get_right_neighbor
		    (&next_lh, cur, ZNODE_READ_LOCK, grn_flags))
			break;

		nextblk = znode_get_block(next_lh.node);
		if (reiser4_blocknr_is_fake(nextblk) ||
		    (ra_adjacent_only(ra_params->flags)
		     && *nextblk != *znode_get_block(cur) + 1))
			break;

		zput(cur);
		cur = zref(next_lh.node);
		done_lh(&next_lh);
		if (znode_page(cur) == NULL)
			jstartio(ZJNODE(cur));
		else
			/* Do not scan read-ahead window if pages already
			 * allocated (and i/o already started). */
			break;

		i++;
	}
	zput(cur);
	done_lh(&next_lh);
}

void reiser4_readdir_readahead_init(struct inode *dir, tap_t *tap)
{
	reiser4_key *stop_key;

	assert("nikita-3542", dir != NULL);
	assert("nikita-3543", tap != NULL);

	stop_key = &tap->ra_info.key_to_stop;
	/* initialize readdir readahead information: include into readahead
	 * stat data of all files of the directory */
	set_key_locality(stop_key, get_inode_oid(dir));
	set_key_type(stop_key, KEY_SD_MINOR);
	set_key_ordering(stop_key, get_key_ordering(reiser4_max_key()));
	set_key_objectid(stop_key, get_key_objectid(reiser4_max_key()));
	set_key_offset(stop_key, get_key_offset(reiser4_max_key()));
}

/*
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 80
   End:
*/
