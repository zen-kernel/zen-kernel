/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */
/* Internal item contains down-link to the child of the internal/twig
   node in a tree. It is internal items that are actually used during
   tree traversal. */

#if !defined( __FS_REISER4_PLUGIN_ITEM_INTERNAL_H__ )
#define __FS_REISER4_PLUGIN_ITEM_INTERNAL_H__

#include "../../forward.h"
#include "../../dformat.h"

/* on-disk layout of internal item */
typedef struct internal_item_layout {
	/*  0 */ reiser4_dblock_nr pointer;
	/*  4 */
} internal_item_layout;

struct cut_list;

int mergeable_internal(const coord_t * p1, const coord_t * p2);
lookup_result lookup_internal(const reiser4_key * key, lookup_bias bias,
			      coord_t * coord);
/* store pointer from internal item into "block". Implementation of
    ->down_link() method */
extern void down_link_internal(const coord_t * coord, const reiser4_key * key,
			       reiser4_block_nr * block);
extern int has_pointer_to_internal(const coord_t * coord,
				   const reiser4_block_nr * block);
extern int create_hook_internal(const coord_t * item, void *arg);
extern int kill_hook_internal(const coord_t * item, pos_in_node_t from,
			      pos_in_node_t count, struct carry_kill_data *);
extern int shift_hook_internal(const coord_t * item, unsigned from,
			       unsigned count, znode * old_node);
extern void reiser4_print_internal(const char *prefix, coord_t * coord);

extern int utmost_child_internal(const coord_t * coord, sideof side,
				 jnode ** child);
int utmost_child_real_block_internal(const coord_t * coord, sideof side,
				     reiser4_block_nr * block);

extern void reiser4_update_internal(const coord_t * coord,
				    const reiser4_block_nr * blocknr);
/* FIXME: reiserfs has check_internal */
extern int check__internal(const coord_t * coord, const char **error);

/* __FS_REISER4_PLUGIN_ITEM_INTERNAL_H__ */
#endif

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
