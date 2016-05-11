/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#ifndef __REISER4_EXTENT_H__
#define __REISER4_EXTENT_H__

/* on disk extent */
typedef struct {
	reiser4_dblock_nr start;
	reiser4_dblock_nr width;
} reiser4_extent;

struct extent_stat {
	int unallocated_units;
	int unallocated_blocks;
	int allocated_units;
	int allocated_blocks;
	int hole_units;
	int hole_blocks;
};

/* extents in an extent item can be either holes, or unallocated or allocated
   extents */
typedef enum {
	HOLE_EXTENT,
	UNALLOCATED_EXTENT,
	ALLOCATED_EXTENT
} extent_state;

#define HOLE_EXTENT_START 0
#define UNALLOCATED_EXTENT_START 1
#define UNALLOCATED_EXTENT_START2 2

struct extent_coord_extension {
	reiser4_block_nr pos_in_unit;
	reiser4_block_nr width;	/* width of current unit */
	pos_in_node_t nr_units;	/* number of units */
	int ext_offset;		/* offset from the beginning of zdata() */
	unsigned long expected_page;
#if REISER4_DEBUG
	reiser4_extent extent;
#endif
};

/* macros to set/get fields of on-disk extent */
static inline reiser4_block_nr extent_get_start(const reiser4_extent * ext)
{
	return le64_to_cpu(ext->start);
}

static inline reiser4_block_nr extent_get_width(const reiser4_extent * ext)
{
	return le64_to_cpu(ext->width);
}

extern __u64 reiser4_current_block_count(void);

static inline void
extent_set_start(reiser4_extent * ext, reiser4_block_nr start)
{
	cassert(sizeof(ext->start) == 8);
	assert("nikita-2510",
	       ergo(start > 1, start < reiser4_current_block_count()));
	put_unaligned(cpu_to_le64(start), &ext->start);
}

static inline void
extent_set_width(reiser4_extent * ext, reiser4_block_nr width)
{
	cassert(sizeof(ext->width) == 8);
	assert("", width > 0);
	put_unaligned(cpu_to_le64(width), &ext->width);
	assert("nikita-2511",
	       ergo(extent_get_start(ext) > 1,
		    extent_get_start(ext) + width <=
		    reiser4_current_block_count()));
}

#define extent_item(coord) 					\
({								\
	assert("nikita-3143", item_is_extent(coord));		\
	((reiser4_extent *)item_body_by_coord (coord));		\
})

#define extent_by_coord(coord)					\
({								\
	assert("nikita-3144", item_is_extent(coord));		\
	(extent_item (coord) + (coord)->unit_pos);		\
})

#define width_by_coord(coord) 					\
({								\
	assert("nikita-3145", item_is_extent(coord));		\
	extent_get_width (extent_by_coord(coord));		\
})

struct carry_cut_data;
struct carry_kill_data;

/* plugin->u.item.b.* */
reiser4_key *max_key_inside_extent(const coord_t *, reiser4_key *);
int can_contain_key_extent(const coord_t * coord, const reiser4_key * key,
			   const reiser4_item_data *);
int mergeable_extent(const coord_t * p1, const coord_t * p2);
pos_in_node_t nr_units_extent(const coord_t *);
lookup_result lookup_extent(const reiser4_key *, lookup_bias, coord_t *);
void init_coord_extent(coord_t *);
int init_extent(coord_t *, reiser4_item_data *);
int paste_extent(coord_t *, reiser4_item_data *, carry_plugin_info *);
int can_shift_extent(unsigned free_space,
		     coord_t * source, znode * target, shift_direction,
		     unsigned *size, unsigned want);
void copy_units_extent(coord_t * target, coord_t * source, unsigned from,
		       unsigned count, shift_direction where_is_free_space,
		       unsigned free_space);
int kill_hook_extent(const coord_t *, pos_in_node_t from, pos_in_node_t count,
		     struct carry_kill_data *);
int create_hook_extent(const coord_t * coord, void *arg);
int cut_units_extent(coord_t * coord, pos_in_node_t from, pos_in_node_t to,
		     struct carry_cut_data *, reiser4_key * smallest_removed,
		     reiser4_key * new_first);
int kill_units_extent(coord_t * coord, pos_in_node_t from, pos_in_node_t to,
		      struct carry_kill_data *, reiser4_key * smallest_removed,
		      reiser4_key * new_first);
reiser4_key *unit_key_extent(const coord_t *, reiser4_key *);
reiser4_key *max_unit_key_extent(const coord_t *, reiser4_key *);
void print_extent(const char *, coord_t *);
int utmost_child_extent(const coord_t * coord, sideof side, jnode ** child);
int utmost_child_real_block_extent(const coord_t * coord, sideof side,
				   reiser4_block_nr * block);
void item_stat_extent(const coord_t * coord, void *vp);
int reiser4_check_extent(const coord_t * coord, const char **error);

/* plugin->u.item.s.file.* */
ssize_t reiser4_write_extent(struct file *, struct inode * inode,
			     const char __user *, size_t, loff_t *);
int reiser4_read_extent(struct file *, flow_t *, hint_t *);
int reiser4_readpage_extent(void *, struct page *);
int reiser4_do_readpage_extent(reiser4_extent*, reiser4_block_nr, struct page*);
reiser4_key *append_key_extent(const coord_t *, reiser4_key *);
void init_coord_extension_extent(uf_coord_t *, loff_t offset);
int get_block_address_extent(const coord_t *, sector_t block,
			     sector_t * result);

/* these are used in flush.c
   FIXME-VS: should they be somewhere in item_plugin? */
int allocate_extent_item_in_place(coord_t *, lock_handle *, flush_pos_t * pos);
int allocate_and_copy_extent(znode * left, coord_t * right, flush_pos_t * pos,
			     reiser4_key * stop_key);

int extent_is_unallocated(const coord_t * item);	/* True if this extent is unallocated (i.e., not a hole, not allocated). */
__u64 extent_unit_index(const coord_t * item);	/* Block offset of this unit. */
__u64 extent_unit_width(const coord_t * item);	/* Number of blocks in this unit. */

/* plugin->u.item.f. */
int reiser4_scan_extent(flush_scan * scan);
extern int key_by_offset_extent(struct inode *, loff_t, reiser4_key *);

reiser4_item_data *init_new_extent(reiser4_item_data * data, void *ext_unit,
				   int nr_extents);
reiser4_block_nr reiser4_extent_size(const coord_t * coord, pos_in_node_t nr);
extent_state state_of_extent(reiser4_extent * ext);
void reiser4_set_extent(reiser4_extent *, reiser4_block_nr start,
			reiser4_block_nr width);
int reiser4_update_extent(struct inode *, jnode *, loff_t pos,
			  int *plugged_hole);

#include "../../coord.h"
#include "../../lock.h"
#include "../../tap.h"

struct replace_handle {
	/* these are to be set before calling reiser4_replace_extent */
	coord_t *coord;
	lock_handle *lh;
	reiser4_key key;
	reiser4_key *pkey;
	reiser4_extent overwrite;
	reiser4_extent new_extents[2];
	int nr_new_extents;
	unsigned flags;

	/* these are used by reiser4_replace_extent */
	reiser4_item_data item;
	coord_t coord_after;
	lock_handle lh_after;
	tap_t watch;
	reiser4_key paste_key;
#if REISER4_DEBUG
	reiser4_extent orig_ext;
	reiser4_key tmp;
#endif
};

/* this structure is kmalloced before calling make_extent to avoid excessive
   stack consumption on plug_hole->reiser4_replace_extent */
struct make_extent_handle {
	uf_coord_t *uf_coord;
	reiser4_block_nr blocknr;
	int created;
	struct inode *inode;
	union {
		struct {
		} append;
		struct replace_handle replace;
	} u;
};

int reiser4_replace_extent(struct replace_handle *,
			   int return_inserted_position);
lock_handle *znode_lh(znode *);

/* the reiser4 repacker support */
struct repacker_cursor;
extern int process_extent_backward_for_repacking(tap_t *,
						 struct repacker_cursor *);
extern int mark_extent_for_repacking(tap_t *, int);

#define coord_by_uf_coord(uf_coord) (&((uf_coord)->coord))
#define ext_coord_by_uf_coord(uf_coord) (&((uf_coord)->extension.extent))

/* __REISER4_EXTENT_H__ */
#endif
/*
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
