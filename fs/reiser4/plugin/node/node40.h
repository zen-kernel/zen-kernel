/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#if !defined( __REISER4_NODE40_H__ )
#define __REISER4_NODE40_H__

#include "../../forward.h"
#include "../../dformat.h"
#include "node.h"

#include <linux/types.h>

/* format of node header for 40 node layouts. Keep bloat out of this struct.  */
typedef struct node40_header {
	/* identifier of node plugin. Must be located at the very beginning
	   of a node. */
	common_node_header common_header;	/* this is 16 bits */
	/* number of items. Should be first element in the node header,
	   because we haven't yet finally decided whether it shouldn't go into
	   common_header.
	 */
/* NIKITA-FIXME-HANS: Create a macro such that if there is only one
 * node format at compile time, and it is this one, accesses do not function dereference when
 * accessing these fields (and otherwise they do).  Probably 80% of users will only have one node format at a time throughout the life of reiser4.  */
	d16 nr_items;
	/* free space in node measured in bytes */
	d16 free_space;
	/* offset to start of free space in node */
	d16 free_space_start;
	/* for reiser4_fsck.  When information about what is a free
	   block is corrupted, and we try to recover everything even
	   if marked as freed, then old versions of data may
	   duplicate newer versions, and this field allows us to
	   restore the newer version.  Also useful for when users
	   who don't have the new trashcan installed on their linux distro
	   delete the wrong files and send us desperate emails
	   offering $25 for them back.  */

	/* magic field we need to tell formatted nodes NIKITA-FIXME-HANS: improve this comment */
	d32 magic;
	/* flushstamp is made of mk_id and write_counter. mk_id is an
	   id generated randomly at mkreiserfs time. So we can just
	   skip all nodes with different mk_id. write_counter is d64
	   incrementing counter of writes on disk. It is used for
	   choosing the newest data at fsck time. NIKITA-FIXME-HANS: why was field name changed but not comment? */

	d32 mkfs_id;
	d64 flush_id;
	/* node flags to be used by fsck (reiser4ck or reiser4fsck?)
	   and repacker NIKITA-FIXME-HANS: say more or reference elsewhere that says more */
	d16 flags;

	/* 1 is leaf level, 2 is twig level, root is the numerically
	   largest level */
	d8 level;

	d8 pad;
} PACKED node40_header;

/* item headers are not standard across all node layouts, pass
   pos_in_node to functions instead */
typedef struct item_header40 {
	/* key of item */
	/*  0 */ reiser4_key key;
	/* offset from start of a node measured in 8-byte chunks */
	/* 24 */ d16 offset;
	/* 26 */ d16 flags;
	/* 28 */ d16 plugin_id;
} PACKED item_header40;

size_t item_overhead_node40(const znode * node, flow_t * aflow);
size_t free_space_node40(znode * node);
node_search_result lookup_node40(znode * node, const reiser4_key * key,
				 lookup_bias bias, coord_t * coord);
int num_of_items_node40(const znode * node);
char *item_by_coord_node40(const coord_t * coord);
int length_by_coord_node40(const coord_t * coord);
item_plugin *plugin_by_coord_node40(const coord_t * coord);
reiser4_key *key_at_node40(const coord_t * coord, reiser4_key * key);
size_t estimate_node40(znode * node);
int check_node40(const znode * node, __u32 flags, const char **error);
int parse_node40(znode * node);
int init_node40(znode * node);
#ifdef GUESS_EXISTS
int guess_node40(const znode * node);
#endif
void change_item_size_node40(coord_t * coord, int by);
int create_item_node40(coord_t * target, const reiser4_key * key,
		       reiser4_item_data * data, carry_plugin_info * info);
void update_item_key_node40(coord_t * target, const reiser4_key * key,
			    carry_plugin_info * info);
int kill_node40(struct carry_kill_data *, carry_plugin_info *);
int cut_node40(struct carry_cut_data *, carry_plugin_info *);
int shift_node40(coord_t * from, znode * to, shift_direction pend,
		 /* if @from->node becomes
		    empty - it will be deleted from
		    the tree if this is set to 1
		  */
		 int delete_child, int including_stop_coord,
		 carry_plugin_info * info);

int fast_insert_node40(const coord_t * coord);
int fast_paste_node40(const coord_t * coord);
int fast_cut_node40(const coord_t * coord);
int max_item_size_node40(void);
int prepare_removal_node40(znode * empty, carry_plugin_info * info);
int set_item_plugin_node40(coord_t * coord, item_id id);
int shrink_item_node40(coord_t * coord, int delta);

#if REISER4_DEBUG
void *shift_check_prepare(const znode *left, const znode *right);
void shift_check(void *vp, const znode *left, const znode *right);
#endif

/* __REISER4_NODE40_H__ */
#endif
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
