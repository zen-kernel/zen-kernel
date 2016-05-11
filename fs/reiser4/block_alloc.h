/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#if !defined(__FS_REISER4_BLOCK_ALLOC_H__)
#define __FS_REISER4_BLOCK_ALLOC_H__

#include "dformat.h"
#include "forward.h"

#include <linux/types.h>	/* for __u??  */
#include <linux/fs.h>

/* Mask when is applied to given block number shows is that block number is a
   fake one */
#define REISER4_FAKE_BLOCKNR_BIT_MASK   0x8000000000000000ULL
/* Mask which isolates a type of object this fake block number was assigned
   to */
#define REISER4_BLOCKNR_STATUS_BIT_MASK 0xC000000000000000ULL

/*result after applying the REISER4_BLOCKNR_STATUS_BIT_MASK should be compared
   against these two values to understand is the object unallocated or bitmap
   shadow object (WORKING BITMAP block, look at the plugin/space/bitmap.c) */
#define REISER4_UNALLOCATED_STATUS_VALUE    0xC000000000000000ULL
#define REISER4_BITMAP_BLOCKS_STATUS_VALUE  0x8000000000000000ULL

/* specification how block allocation was counted in sb block counters */
typedef enum {
	BLOCK_NOT_COUNTED = 0,	/* reiser4 has no info about this block yet */
	BLOCK_GRABBED = 1,	/* free space grabbed for further allocation
				   of this block */
	BLOCK_FLUSH_RESERVED = 2,	/* block is reserved for flush needs. */
	BLOCK_UNALLOCATED = 3,	/* block is used for existing in-memory object
				   ( unallocated formatted or unformatted
				   node) */
	BLOCK_ALLOCATED = 4	/* block is mapped to disk, real on-disk block
				   number assigned */
} block_stage_t;

/* a hint for block allocator */
struct reiser4_blocknr_hint {
	/* FIXME: I think we want to add a longterm lock on the bitmap block
	   here. This is to prevent jnode_flush() calls from interleaving
	   allocations on the same bitmap, once a hint is established. */

	/* search start hint */
	reiser4_block_nr blk;
	/* if not zero, it is a region size we search for free blocks in */
	reiser4_block_nr max_dist;
	/* level for allocation, may be useful have branch-level and higher
	   write-optimized. */
	tree_level level;
	/* block allocator assumes that blocks, which will be mapped to disk,
	   are in this specified block_stage */
	block_stage_t block_stage;
	/* If direction = 1 allocate blocks in backward direction from the end
	 * of disk to the beginning of disk.  */
	unsigned int backward:1;

};

/* These flags control block allocation/deallocation behavior */
enum reiser4_ba_flags {
	/* do allocatations from reserved (5%) area */
	BA_RESERVED = (1 << 0),

	/* block allocator can do commit trying to recover free space */
	BA_CAN_COMMIT = (1 << 1),

	/* if operation will be applied to formatted block */
	BA_FORMATTED = (1 << 2),

	/* defer actual block freeing until transaction commit */
	BA_DEFER = (1 << 3),

	/* allocate blocks for permanent fs objects (formatted or unformatted),
	    not wandered of log blocks */
	BA_PERMANENT = (1 << 4),

	/* grab space even it was disabled */
	BA_FORCE = (1 << 5),

	/* use default start value for free blocks search. */
	BA_USE_DEFAULT_SEARCH_START = (1 << 6)
};

typedef enum reiser4_ba_flags reiser4_ba_flags_t;

extern void reiser4_blocknr_hint_init(reiser4_blocknr_hint * hint);
extern void reiser4_blocknr_hint_done(reiser4_blocknr_hint * hint);
extern void update_blocknr_hint_default(const struct super_block *,
					const reiser4_block_nr *);
extern void get_blocknr_hint_default(reiser4_block_nr *);

extern reiser4_block_nr reiser4_fs_reserved_space(struct super_block *super);

int assign_fake_blocknr_formatted(reiser4_block_nr *);
reiser4_block_nr fake_blocknr_unformatted(int);

/* free -> grabbed -> fake_allocated -> used */

int reiser4_grab_space(__u64 count, reiser4_ba_flags_t flags);
void all_grabbed2free(void);
void grabbed2free(reiser4_context * , reiser4_super_info_data * , __u64 count);
void fake_allocated2free(__u64 count, reiser4_ba_flags_t flags);
void grabbed2flush_reserved_nolock(txn_atom * atom, __u64 count);
void grabbed2flush_reserved(__u64 count);
int reiser4_alloc_blocks(reiser4_blocknr_hint * hint,
			 reiser4_block_nr * start,
			 reiser4_block_nr * len, reiser4_ba_flags_t flags);
int reiser4_dealloc_blocks(const reiser4_block_nr *,
			   const reiser4_block_nr *,
			   block_stage_t, reiser4_ba_flags_t flags);

static inline int reiser4_alloc_block(reiser4_blocknr_hint * hint,
				      reiser4_block_nr * start,
				      reiser4_ba_flags_t flags)
{
	reiser4_block_nr one = 1;
	return reiser4_alloc_blocks(hint, start, &one, flags);
}

static inline int reiser4_dealloc_block(const reiser4_block_nr * block,
					block_stage_t stage,
					reiser4_ba_flags_t flags)
{
	const reiser4_block_nr one = 1;
	return reiser4_dealloc_blocks(block, &one, stage, flags);
}

#define reiser4_grab_space_force(count, flags)		\
	reiser4_grab_space(count, flags | BA_FORCE)

extern void grabbed2free_mark(__u64 mark);
extern int reiser4_grab_reserved(struct super_block *,
				 __u64, reiser4_ba_flags_t);
extern void reiser4_release_reserved(struct super_block *super);

/* grabbed -> fake_allocated */

/* fake_allocated -> used */

/* used -> fake_allocated -> grabbed -> free */

extern void flush_reserved2grabbed(txn_atom * atom, __u64 count);

extern int reiser4_blocknr_is_fake(const reiser4_block_nr * da);

extern void grabbed2cluster_reserved(int count);
extern void cluster_reserved2grabbed(int count);
extern void cluster_reserved2free(int count);

extern int reiser4_check_block_counters(const struct super_block *);


extern int reiser4_check_blocks(const reiser4_block_nr *start,
                                const reiser4_block_nr *len, int desired);

static inline int reiser4_check_block(const reiser4_block_nr *start,
                                      int desired)
{
	return reiser4_check_blocks(start, NULL, desired);
}

extern int reiser4_pre_commit_hook(void);
extern void reiser4_post_commit_hook(void);
extern void reiser4_post_write_back_hook(void);

#endif				/* __FS_REISER4_BLOCK_ALLOC_H__ */

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
