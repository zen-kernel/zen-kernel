/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

#include "../../debug.h"
#include "../../dformat.h"
#include "../../txnmgr.h"
#include "../../jnode.h"
#include "../../block_alloc.h"
#include "../../tree.h"
#include "../../super.h"
#include "../plugin.h"
#include "space_allocator.h"
#include "bitmap.h"

#include <linux/types.h>
#include <linux/fs.h>		/* for struct super_block  */
#include <linux/mutex.h>
#include <asm/div64.h>

/* Proposed (but discarded) optimization: dynamic loading/unloading of bitmap
 * blocks

   A useful optimization of reiser4 bitmap handling would be dynamic bitmap
   blocks loading/unloading which is different from v3.x where all bitmap
   blocks are loaded at mount time.

   To implement bitmap blocks unloading we need to count bitmap block usage
   and detect currently unused blocks allowing them to be unloaded. It is not
   a simple task since we allow several threads to modify one bitmap block
   simultaneously.

   Briefly speaking, the following schema is proposed: we count in special
   variable associated with each bitmap block. That is for counting of block
   alloc/dealloc operations on that bitmap block. With a deferred block
   deallocation feature of reiser4 all those operation will be represented in
   atom dirty/deleted lists as jnodes for freshly allocated or deleted
   nodes.

   So, we increment usage counter for each new node allocated or deleted, and
   decrement it at atom commit one time for each node from the dirty/deleted
   atom's list.  Of course, freshly allocated node deletion and node reusing
   from atom deleted (if we do so) list should decrement bitmap usage counter
   also.

   This schema seems to be working but that reference counting is
   not easy to debug. I think we should agree with Hans and do not implement
   it in v4.0. Current code implements "on-demand" bitmap blocks loading only.

   For simplicity all bitmap nodes (both commit and working bitmap blocks) are
   loaded into memory on fs mount time or each bitmap nodes are loaded at the
   first access to it, the "dont_load_bitmap" mount option controls whether
   bimtap nodes should be loaded at mount time. Dynamic unloading of bitmap
   nodes currently is not supported. */

#define CHECKSUM_SIZE    4

#define BYTES_PER_LONG   (sizeof(long))

#if BITS_PER_LONG == 64
#  define LONG_INT_SHIFT (6)
#else
#  define LONG_INT_SHIFT (5)
#endif

#define LONG_INT_MASK (BITS_PER_LONG - 1UL)

typedef unsigned long ulong_t;

#define bmap_size(blocksize)	    ((blocksize) - CHECKSUM_SIZE)
#define bmap_bit_count(blocksize)   (bmap_size(blocksize) << 3)

/* Block allocation/deallocation are done through special bitmap objects which
   are allocated in an array at fs mount. */
struct bitmap_node {
	struct mutex mutex;	/* long term lock object */

	jnode *wjnode;		/* j-nodes for WORKING ... */
	jnode *cjnode;		/* ... and COMMIT bitmap blocks */

	bmap_off_t first_zero_bit;	/* for skip_busy option implementation */

	atomic_t loaded;	/* a flag which shows that bnode is loaded
				 * already */
};

static inline char *bnode_working_data(struct bitmap_node *bnode)
{
	char *data;

	data = jdata(bnode->wjnode);
	assert("zam-429", data != NULL);

	return data + CHECKSUM_SIZE;
}

static inline char *bnode_commit_data(const struct bitmap_node *bnode)
{
	char *data;

	data = jdata(bnode->cjnode);
	assert("zam-430", data != NULL);

	return data + CHECKSUM_SIZE;
}

static inline __u32 bnode_commit_crc(const struct bitmap_node *bnode)
{
	char *data;

	data = jdata(bnode->cjnode);
	assert("vpf-261", data != NULL);

	return le32_to_cpu(get_unaligned((d32 *)data));
}

static inline void bnode_set_commit_crc(struct bitmap_node *bnode, __u32 crc)
{
	char *data;

	data = jdata(bnode->cjnode);
	assert("vpf-261", data != NULL);

	put_unaligned(cpu_to_le32(crc), (d32 *)data);
}

/* ZAM-FIXME-HANS: is the idea that this might be a union someday? having
 * written the code, does this added abstraction still have */
/* ANSWER(Zam): No, the abstractions is in the level above (exact place is the
 * reiser4_space_allocator structure) */
/* ZAM-FIXME-HANS: I don't understand your english in comment above. */
/* FIXME-HANS(Zam): I don't understand the questions like "might be a union
 * someday?". What they about?  If there is a reason to have a union, it should
 * be a union, if not, it should not be a union.  "..might be someday" means no
 * reason. */
struct bitmap_allocator_data {
	/* an array for bitmap blocks direct access */
	struct bitmap_node *bitmap;
};

#define get_barray(super) \
(((struct bitmap_allocator_data *)(get_super_private(super)->space_allocator.u.generic)) -> bitmap)

#define get_bnode(super, i) (get_barray(super) + i)

/* allocate and initialize jnode with JNODE_BITMAP type */
static jnode *bnew(void)
{
	jnode *jal = jalloc();

	if (jal)
		jnode_init(jal, current_tree, JNODE_BITMAP);

	return jal;
}

/* this file contains:
   - bitmap based implementation of space allocation plugin
   - all the helper functions like set bit, find_first_zero_bit, etc */

/* Audited by: green(2002.06.12) */
static int find_next_zero_bit_in_word(ulong_t word, int start_bit)
{
	ulong_t mask = 1UL << start_bit;
	int i = start_bit;

	while ((word & mask) != 0) {
		mask <<= 1;
		if (++i >= BITS_PER_LONG)
			break;
	}

	return i;
}

#include <linux/bitops.h>

#if BITS_PER_LONG == 64

#define OFF(addr)  (((ulong_t)(addr) & (BYTES_PER_LONG - 1)) << 3)
#define BASE(addr) ((ulong_t*) ((ulong_t)(addr) & ~(BYTES_PER_LONG - 1)))

static inline void reiser4_set_bit(int nr, void *addr)
{
	__test_and_set_bit_le(nr + OFF(addr), BASE(addr));
}

static inline void reiser4_clear_bit(int nr, void *addr)
{
	__test_and_clear_bit_le(nr + OFF(addr), BASE(addr));
}

static inline int reiser4_test_bit(int nr, void *addr)
{
	return test_bit_le(nr + OFF(addr), BASE(addr));
}
static inline int reiser4_find_next_zero_bit(void *addr, int maxoffset,
					     int offset)
{
	int off = OFF(addr);

	return find_next_zero_bit_le(BASE(addr), maxoffset + off,
				     offset + off) - off;
}

#else

#define reiser4_set_bit(nr, addr)    __test_and_set_bit_le(nr, addr)
#define reiser4_clear_bit(nr, addr)  __test_and_clear_bit_le(nr, addr)
#define reiser4_test_bit(nr, addr)  test_bit_le(nr, addr)

#define reiser4_find_next_zero_bit(addr, maxoffset, offset) \
find_next_zero_bit_le(addr, maxoffset, offset)
#endif

/* Search for a set bit in the bit array [@start_offset, @max_offset[, offsets
 * are counted from @addr, return the offset of the first bit if it is found,
 * @maxoffset otherwise. */
static bmap_off_t __reiser4_find_next_set_bit(void *addr, bmap_off_t max_offset,
					      bmap_off_t start_offset)
{
	ulong_t *base = addr;
	/* start_offset is in bits, convert it to byte offset within bitmap. */
	int word_nr = start_offset >> LONG_INT_SHIFT;
	/* bit number within the byte. */
	int bit_nr = start_offset & LONG_INT_MASK;
	int max_word_nr = (max_offset - 1) >> LONG_INT_SHIFT;

	assert("zam-387", max_offset != 0);

	/* Unaligned @start_offset case.  */
	if (bit_nr != 0) {
		bmap_nr_t nr;

		nr = find_next_zero_bit_in_word(~(base[word_nr]), bit_nr);

		if (nr < BITS_PER_LONG)
			return (word_nr << LONG_INT_SHIFT) + nr;

		++word_nr;
	}

	/* Fast scan trough aligned words. */
	while (word_nr <= max_word_nr) {
		if (base[word_nr] != 0) {
			return (word_nr << LONG_INT_SHIFT)
			    + find_next_zero_bit_in_word(~(base[word_nr]), 0);
		}

		++word_nr;
	}

	return max_offset;
}

#if BITS_PER_LONG == 64

static bmap_off_t reiser4_find_next_set_bit(void *addr, bmap_off_t max_offset,
					    bmap_off_t start_offset)
{
	bmap_off_t off = OFF(addr);

	return __reiser4_find_next_set_bit(BASE(addr), max_offset + off,
					   start_offset + off) - off;
}

#else
#define reiser4_find_next_set_bit(addr, max_offset, start_offset) \
  __reiser4_find_next_set_bit(addr, max_offset, start_offset)
#endif

/* search for the first set bit in single word. */
static int find_last_set_bit_in_word(ulong_t word, int start_bit)
{
	ulong_t bit_mask;
	int nr = start_bit;

	assert("zam-965", start_bit < BITS_PER_LONG);
	assert("zam-966", start_bit >= 0);

	bit_mask = (1UL << nr);

	while (bit_mask != 0) {
		if (bit_mask & word)
			return nr;
		bit_mask >>= 1;
		nr--;
	}
	return BITS_PER_LONG;
}

/* Search bitmap for a set bit in backward direction from the end to the
 * beginning of given region
 *
 * @result: result offset of the last set bit
 * @addr:   base memory address,
 * @low_off:  low end of the search region, edge bit included into the region,
 * @high_off: high end of the search region, edge bit included into the region,
 *
 * @return: 0 - set bit was found, -1 otherwise.
 */
static int
reiser4_find_last_set_bit(bmap_off_t * result, void *addr, bmap_off_t low_off,
			  bmap_off_t high_off)
{
	ulong_t *base = addr;
	int last_word;
	int first_word;
	int last_bit;
	int nr;

	assert("zam-962", high_off >= low_off);

	last_word = high_off >> LONG_INT_SHIFT;
	last_bit = high_off & LONG_INT_MASK;
	first_word = low_off >> LONG_INT_SHIFT;

	if (last_bit < BITS_PER_LONG) {
		nr = find_last_set_bit_in_word(base[last_word], last_bit);
		if (nr < BITS_PER_LONG) {
			*result = (last_word << LONG_INT_SHIFT) + nr;
			return 0;
		}
		--last_word;
	}
	while (last_word >= first_word) {
		if (base[last_word] != 0x0) {
			last_bit =
			    find_last_set_bit_in_word(base[last_word],
						      BITS_PER_LONG - 1);
			assert("zam-972", last_bit < BITS_PER_LONG);
			*result = (last_word << LONG_INT_SHIFT) + last_bit;
			return 0;
		}
		--last_word;
	}

	return -1;		/* set bit not found */
}

/* Search bitmap for a clear bit in backward direction from the end to the
 * beginning of given region */
static int
reiser4_find_last_zero_bit(bmap_off_t * result, void *addr, bmap_off_t low_off,
			   bmap_off_t high_off)
{
	ulong_t *base = addr;
	int last_word;
	int first_word;
	int last_bit;
	int nr;

	last_word = high_off >> LONG_INT_SHIFT;
	last_bit = high_off & LONG_INT_MASK;
	first_word = low_off >> LONG_INT_SHIFT;

	if (last_bit < BITS_PER_LONG) {
		nr = find_last_set_bit_in_word(~base[last_word], last_bit);
		if (nr < BITS_PER_LONG) {
			*result = (last_word << LONG_INT_SHIFT) + nr;
			return 0;
		}
		--last_word;
	}
	while (last_word >= first_word) {
		if (base[last_word] != (ulong_t) (-1)) {
			*result = (last_word << LONG_INT_SHIFT) +
			    find_last_set_bit_in_word(~base[last_word],
						      BITS_PER_LONG - 1);
			return 0;
		}
		--last_word;
	}

	return -1;		/* zero bit not found */
}

/* Audited by: green(2002.06.12) */
static void reiser4_clear_bits(char *addr, bmap_off_t start, bmap_off_t end)
{
	int first_byte;
	int last_byte;

	unsigned char first_byte_mask = 0xFF;
	unsigned char last_byte_mask = 0xFF;

	assert("zam-410", start < end);

	first_byte = start >> 3;
	last_byte = (end - 1) >> 3;

	if (last_byte > first_byte + 1)
		memset(addr + first_byte + 1, 0,
		       (size_t) (last_byte - first_byte - 1));

	first_byte_mask >>= 8 - (start & 0x7);
	last_byte_mask <<= ((end - 1) & 0x7) + 1;

	if (first_byte == last_byte) {
		addr[first_byte] &= (first_byte_mask | last_byte_mask);
	} else {
		addr[first_byte] &= first_byte_mask;
		addr[last_byte] &= last_byte_mask;
	}
}

/* Audited by: green(2002.06.12) */
/* ZAM-FIXME-HANS: comment this */
static void reiser4_set_bits(char *addr, bmap_off_t start, bmap_off_t end)
{
	int first_byte;
	int last_byte;

	unsigned char first_byte_mask = 0xFF;
	unsigned char last_byte_mask = 0xFF;

	assert("zam-386", start < end);

	first_byte = start >> 3;
	last_byte = (end - 1) >> 3;

	if (last_byte > first_byte + 1)
		memset(addr + first_byte + 1, 0xFF,
		       (size_t) (last_byte - first_byte - 1));

	first_byte_mask <<= start & 0x7;
	last_byte_mask >>= 7 - ((end - 1) & 0x7);

	if (first_byte == last_byte) {
		addr[first_byte] |= (first_byte_mask & last_byte_mask);
	} else {
		addr[first_byte] |= first_byte_mask;
		addr[last_byte] |= last_byte_mask;
	}
}

#define ADLER_BASE    65521
#define ADLER_NMAX    5552

/* Calculates the adler32 checksum for the data pointed by `data` of the
    length `len`. This function was originally taken from zlib, version 1.1.3,
    July 9th, 1998.

    Copyright (C) 1995-1998 Jean-loup Gailly and Mark Adler

    This software is provided 'as-is', without any express or implied
    warranty.  In no event will the authors be held liable for any damages
    arising from the use of this software.

    Permission is granted to anyone to use this software for any purpose,
    including commercial applications, and to alter it and redistribute it
    freely, subject to the following restrictions:

    1. The origin of this software must not be misrepresented; you must not
	claim that you wrote the original software. If you use this software
	in a product, an acknowledgment in the product documentation would be
	appreciated but is not required.
    2. Altered source versions must be plainly marked as such, and must not be
	misrepresented as being the original software.
    3. This notice may not be removed or altered from any source distribution.

    Jean-loup Gailly        Mark Adler
    jloup@gzip.org          madler@alumni.caltech.edu

    The above comment applies only to the reiser4_adler32 function.
*/

__u32 reiser4_adler32(char *data, __u32 len)
{
	unsigned char *t = data;
	__u32 s1 = 1;
	__u32 s2 = 0;
	int k;

	while (len > 0) {
		k = len < ADLER_NMAX ? len : ADLER_NMAX;
		len -= k;

		while (k--) {
			s1 += *t++;
			s2 += s1;
		}

		s1 %= ADLER_BASE;
		s2 %= ADLER_BASE;
	}
	return (s2 << 16) | s1;
}

#define sb_by_bnode(bnode) \
	((struct super_block *)jnode_get_tree(bnode->wjnode)->super)

static __u32 bnode_calc_crc(const struct bitmap_node *bnode, unsigned long size)
{
	return reiser4_adler32(bnode_commit_data(bnode), bmap_size(size));
}

static int
bnode_check_adler32(const struct bitmap_node *bnode, unsigned long size)
{
	if (bnode_calc_crc(bnode, size) != bnode_commit_crc(bnode)) {
		bmap_nr_t bmap;

		bmap = bnode - get_bnode(sb_by_bnode(bnode), 0);

		warning("vpf-263",
			"Checksum for the bitmap block %llu is incorrect",
			bmap);

		return RETERR(-EIO);
	}

	return 0;
}

#define REISER4_CHECK_BMAP_CRC (0)

#if REISER4_CHECK_BMAP_CRC
static int bnode_check_crc(const struct bitmap_node *bnode)
{
	return bnode_check_adler32(bnode,
				   bmap_size(sb_by_bnode(bnode)->s_blocksize));
}

/* REISER4_CHECK_BMAP_CRC */
#else

#define bnode_check_crc(bnode) (0)

/* REISER4_CHECK_BMAP_CRC */
#endif

/* Recalculates the adler32 checksum for only 1 byte change.
    adler - previous adler checksum
    old_data, data - old, new byte values.
    tail == (chunk - offset) : length, checksum was calculated for, - offset of
    the changed byte within this chunk.
    This function can be used for checksum calculation optimisation.
*/

static __u32
adler32_recalc(__u32 adler, unsigned char old_data, unsigned char data,
	       __u32 tail)
{
	__u32 delta = data - old_data + 2 * ADLER_BASE;
	__u32 s1 = adler & 0xffff;
	__u32 s2 = (adler >> 16) & 0xffff;

	s1 = (delta + s1) % ADLER_BASE;
	s2 = (delta * tail + s2) % ADLER_BASE;

	return (s2 << 16) | s1;
}

#define LIMIT(val, boundary) ((val) > (boundary) ? (boundary) : (val))

/**
 * get_nr_bitmap - calculate number of bitmap blocks
 * @super: super block with initialized blocksize and block count
 *
 * Calculates number of bitmap blocks of a filesystem which uses bitmaps to
 * maintain free disk space. It assumes that each bitmap addresses the same
 * number of blocks which is calculated by bmap_block_count macro defined in
 * above. Number of blocks in the filesystem has to be initialized in reiser4
 * private data of super block already so that it can be obtained via
 * reiser4_block_count(). Unfortunately, number of blocks addressed by a bitmap
 * is not power of 2 because 4 bytes are used for checksum. Therefore, we have
 * to use special function to divide and modulo 64bits filesystem block
 * counters.
 *
 * Example: suppose filesystem have 32768 blocks. Blocksize is 4096. Each bitmap
 * block addresses (4096 - 4) * 8 = 32736 blocks. Number of bitmaps to address
 * all 32768 blocks is calculated as (32768 - 1) / 32736 + 1 = 2.
 */
static bmap_nr_t get_nr_bmap(const struct super_block *super)
{
	u64 quotient;

	assert("zam-393", reiser4_block_count(super) != 0);

	quotient = reiser4_block_count(super) - 1;
	do_div(quotient, bmap_bit_count(super->s_blocksize));
	return quotient + 1;
}

/**
 * parse_blocknr - calculate bitmap number and offset in it by block number
 * @block: pointer to block number to calculate location in bitmap of
 * @bmap: pointer where to store bitmap block number
 * @offset: pointer where to store offset within bitmap block
 *
 * Calculates location of bit which is responsible for allocation/freeing of
 * block @*block. That location is represented by bitmap block number and offset
 * within that bitmap block.
 */
static void
parse_blocknr(const reiser4_block_nr *block, bmap_nr_t *bmap,
	      bmap_off_t *offset)
{
	struct super_block *super = get_current_context()->super;
	u64 quotient = *block;

	*offset = do_div(quotient, bmap_bit_count(super->s_blocksize));
	*bmap = quotient;

	assert("zam-433", *bmap < get_nr_bmap(super));
	assert("", *offset < bmap_bit_count(super->s_blocksize));
}

#if REISER4_DEBUG
/* Audited by: green(2002.06.12) */
static void
check_block_range(const reiser4_block_nr * start, const reiser4_block_nr * len)
{
	struct super_block *sb = reiser4_get_current_sb();

	assert("zam-436", sb != NULL);

	assert("zam-455", start != NULL);
	assert("zam-437", *start != 0);
	assert("zam-541", !reiser4_blocknr_is_fake(start));
	assert("zam-441", *start < reiser4_block_count(sb));

	if (len != NULL) {
		assert("zam-438", *len != 0);
		assert("zam-442", *start + *len <= reiser4_block_count(sb));
	}
}

static void check_bnode_loaded(const struct bitmap_node *bnode)
{
	assert("zam-485", bnode != NULL);
	assert("zam-483", jnode_page(bnode->wjnode) != NULL);
	assert("zam-484", jnode_page(bnode->cjnode) != NULL);
	assert("nikita-2820", jnode_is_loaded(bnode->wjnode));
	assert("nikita-2821", jnode_is_loaded(bnode->cjnode));
}

#else

#  define check_block_range(start, len) do { /* nothing */} while(0)
#  define check_bnode_loaded(bnode)     do { /* nothing */} while(0)

#endif

/* modify bnode->first_zero_bit (if we free bits before); bnode should be
   spin-locked */
static inline void
adjust_first_zero_bit(struct bitmap_node *bnode, bmap_off_t offset)
{
	if (offset < bnode->first_zero_bit)
		bnode->first_zero_bit = offset;
}

/* return a physical disk address for logical bitmap number @bmap */
/* FIXME-VS: this is somehow related to disk layout? */
/* ZAM-FIXME-HANS: your answer is? Use not more than one function dereference
 * per block allocation so that performance is not affected.  Probably this
 * whole file should be considered part of the disk layout plugin, and other
 * disk layouts can use other defines and efficiency will not be significantly
 * affected.  */

#define REISER4_FIRST_BITMAP_BLOCK \
	((REISER4_MASTER_OFFSET / PAGE_CACHE_SIZE) + 2)

/* Audited by: green(2002.06.12) */
static void
get_bitmap_blocknr(struct super_block *super, bmap_nr_t bmap,
		   reiser4_block_nr * bnr)
{

	assert("zam-390", bmap < get_nr_bmap(super));

#ifdef CONFIG_REISER4_BADBLOCKS
#define BITMAP_PLUGIN_DISKMAP_ID ((0xc0e1<<16) | (0xe0ff))
	/* Check if the diskmap have this already, first. */
	if (reiser4_get_diskmap_value(BITMAP_PLUGIN_DISKMAP_ID, bmap, bnr) == 0)
		return;		/* Found it in diskmap */
#endif
	/* FIXME_ZAM: before discussing of disk layouts and disk format
	   plugins I implement bitmap location scheme which is close to scheme
	   used in reiser 3.6 */
	if (bmap == 0) {
		*bnr = REISER4_FIRST_BITMAP_BLOCK;
	} else {
		*bnr = bmap * bmap_bit_count(super->s_blocksize);
	}
}

/* construct a fake block number for shadow bitmap (WORKING BITMAP) block */
/* Audited by: green(2002.06.12) */
static void get_working_bitmap_blocknr(bmap_nr_t bmap, reiser4_block_nr * bnr)
{
	*bnr =
	    (reiser4_block_nr) ((bmap & ~REISER4_BLOCKNR_STATUS_BIT_MASK) |
				REISER4_BITMAP_BLOCKS_STATUS_VALUE);
}

/* bnode structure initialization */
static void
init_bnode(struct bitmap_node *bnode,
	   struct super_block *super UNUSED_ARG, bmap_nr_t bmap UNUSED_ARG)
{
	memset(bnode, 0, sizeof(struct bitmap_node));

	mutex_init(&bnode->mutex);
	atomic_set(&bnode->loaded, 0);
}

static void release(jnode * node)
{
	jrelse(node);
	JF_SET(node, JNODE_HEARD_BANSHEE);
	jput(node);
}

/* This function is for internal bitmap.c use because it assumes that jnode is
   in under full control of this thread */
static void done_bnode(struct bitmap_node *bnode)
{
	if (bnode) {
		atomic_set(&bnode->loaded, 0);
		if (bnode->wjnode != NULL)
			release(bnode->wjnode);
		if (bnode->cjnode != NULL)
			release(bnode->cjnode);
		bnode->wjnode = bnode->cjnode = NULL;
	}
}

/* ZAM-FIXME-HANS: comment this.  Called only by load_and_lock_bnode()*/
static int prepare_bnode(struct bitmap_node *bnode, jnode **cjnode_ret,
			 jnode **wjnode_ret)
{
	struct super_block *super;
	jnode *cjnode;
	jnode *wjnode;
	bmap_nr_t bmap;
	int ret;

	super = reiser4_get_current_sb();

	*wjnode_ret = wjnode = bnew();
	if (wjnode == NULL) {
		*cjnode_ret = NULL;
		return RETERR(-ENOMEM);
	}

	*cjnode_ret = cjnode = bnew();
	if (cjnode == NULL)
		return RETERR(-ENOMEM);

	bmap = bnode - get_bnode(super, 0);

	get_working_bitmap_blocknr(bmap, &wjnode->blocknr);
	get_bitmap_blocknr(super, bmap, &cjnode->blocknr);

	jref(cjnode);
	jref(wjnode);

	/* load commit bitmap */
	ret = jload_gfp(cjnode, GFP_NOFS, 1);

	if (ret)
		goto error;

	/* allocate memory for working bitmap block. Note that for
	 * bitmaps jinit_new() doesn't actually modifies node content,
	 * so parallel calls to this are ok. */
	ret = jinit_new(wjnode, GFP_NOFS);

	if (ret != 0) {
		jrelse(cjnode);
		goto error;
	}

	return 0;

      error:
	jput(cjnode);
	jput(wjnode);
	*wjnode_ret = *cjnode_ret = NULL;
	return ret;

}

/* Check the bnode data on read. */
static int check_struct_bnode(struct bitmap_node *bnode, __u32 blksize)
{
	void *data;
	int ret;

	/* Check CRC */
	ret = bnode_check_adler32(bnode, blksize);

	if (ret) {
		return ret;
	}

	data = jdata(bnode->cjnode) + CHECKSUM_SIZE;

	/* Check the very first bit -- it must be busy. */
	if (!reiser4_test_bit(0, data)) {
		warning("vpf-1362", "The allocator block %llu is not marked "
			"as used.", (unsigned long long)bnode->cjnode->blocknr);

		return -EINVAL;
	}

	return 0;
}

/* load bitmap blocks "on-demand" */
static int load_and_lock_bnode(struct bitmap_node *bnode)
{
	int ret;

	jnode *cjnode;
	jnode *wjnode;

	assert("nikita-3040", reiser4_schedulable());

/* ZAM-FIXME-HANS: since bitmaps are never unloaded, this does not
 * need to be atomic, right? Just leave a comment that if bitmaps were
 * unloadable, this would need to be atomic.  */
	if (atomic_read(&bnode->loaded)) {
		/* bitmap is already loaded, nothing to do */
		check_bnode_loaded(bnode);
		mutex_lock(&bnode->mutex);
		assert("nikita-2827", atomic_read(&bnode->loaded));
		return 0;
	}

	ret = prepare_bnode(bnode, &cjnode, &wjnode);
	if (ret)
		return ret;

	mutex_lock(&bnode->mutex);

	if (!atomic_read(&bnode->loaded)) {
		assert("nikita-2822", cjnode != NULL);
		assert("nikita-2823", wjnode != NULL);
		assert("nikita-2824", jnode_is_loaded(cjnode));
		assert("nikita-2825", jnode_is_loaded(wjnode));

		bnode->wjnode = wjnode;
		bnode->cjnode = cjnode;

		ret = check_struct_bnode(bnode, current_blocksize);
		if (unlikely(ret != 0))
			goto error;

		atomic_set(&bnode->loaded, 1);
		/* working bitmap is initialized by on-disk
		 * commit bitmap. This should be performed
		 * under mutex. */
		memcpy(bnode_working_data(bnode),
		       bnode_commit_data(bnode),
		       bmap_size(current_blocksize));
	} else
		/* race: someone already loaded bitmap
		 * while we were busy initializing data. */
		check_bnode_loaded(bnode);
	return 0;

 error:
	release(wjnode);
	release(cjnode);
	bnode->wjnode = NULL;
	bnode->cjnode = NULL;
	mutex_unlock(&bnode->mutex);
	return ret;
}

static void release_and_unlock_bnode(struct bitmap_node *bnode)
{
	check_bnode_loaded(bnode);
	mutex_unlock(&bnode->mutex);
}

/* This function does all block allocation work but only for one bitmap
   block.*/
/* FIXME_ZAM: It does not allow us to allocate block ranges across bitmap
   block responsibility zone boundaries. This had no sense in v3.6 but may
   have it in v4.x */
/* ZAM-FIXME-HANS: do you mean search one bitmap block forward? */
static int
search_one_bitmap_forward(bmap_nr_t bmap, bmap_off_t * offset,
			  bmap_off_t max_offset, int min_len, int max_len)
{
	struct super_block *super = get_current_context()->super;
	struct bitmap_node *bnode = get_bnode(super, bmap);

	char *data;

	bmap_off_t search_end;
	bmap_off_t start;
	bmap_off_t end;

	int set_first_zero_bit = 0;

	int ret;

	assert("zam-364", min_len > 0);
	assert("zam-365", max_len >= min_len);
	assert("zam-366", *offset <= max_offset);

	ret = load_and_lock_bnode(bnode);

	if (ret)
		return ret;

	data = bnode_working_data(bnode);

	start = *offset;

	if (bnode->first_zero_bit >= start) {
		start = bnode->first_zero_bit;
		set_first_zero_bit = 1;
	}

	while (start + min_len < max_offset) {

		start =
		    reiser4_find_next_zero_bit((long *)data, max_offset, start);
		if (set_first_zero_bit) {
			bnode->first_zero_bit = start;
			set_first_zero_bit = 0;
		}
		if (start >= max_offset)
			break;

		search_end = LIMIT(start + max_len, max_offset);
		end =
		    reiser4_find_next_set_bit((long *)data, search_end, start);
		if (end >= start + min_len) {
			/* we can't trust find_next_set_bit result if set bit
			   was not fount, result may be bigger than
			   max_offset */
			if (end > search_end)
				end = search_end;

			ret = end - start;
			*offset = start;

			reiser4_set_bits(data, start, end);

			/* FIXME: we may advance first_zero_bit if [start,
			   end] region overlaps the first_zero_bit point */

			break;
		}

		start = end + 1;
	}

	release_and_unlock_bnode(bnode);

	return ret;
}

static int
search_one_bitmap_backward(bmap_nr_t bmap, bmap_off_t * start_offset,
			   bmap_off_t end_offset, int min_len, int max_len)
{
	struct super_block *super = get_current_context()->super;
	struct bitmap_node *bnode = get_bnode(super, bmap);
	char *data;
	bmap_off_t start;
	int ret;

	assert("zam-958", min_len > 0);
	assert("zam-959", max_len >= min_len);
	assert("zam-960", *start_offset >= end_offset);

	ret = load_and_lock_bnode(bnode);
	if (ret)
		return ret;

	data = bnode_working_data(bnode);
	start = *start_offset;

	while (1) {
		bmap_off_t end, search_end;

		/* Find the beginning of the zero filled region */
		if (reiser4_find_last_zero_bit(&start, data, end_offset, start))
			break;
		/* Is there more than `min_len' bits from `start' to
		 * `end_offset'?  */
		if (start < end_offset + min_len - 1)
			break;

		/* Do not search to `end_offset' if we need to find less than
		 * `max_len' zero bits. */
		if (end_offset + max_len - 1 < start)
			search_end = start - max_len + 1;
		else
			search_end = end_offset;

		if (reiser4_find_last_set_bit(&end, data, search_end, start))
			end = search_end;
		else
			end++;

		if (end + min_len <= start + 1) {
			if (end < search_end)
				end = search_end;
			ret = start - end + 1;
			*start_offset = end;	/* `end' is lowest offset */
			assert("zam-987",
			       reiser4_find_next_set_bit(data, start + 1,
							 end) >= start + 1);
			reiser4_set_bits(data, end, start + 1);
			break;
		}

		if (end <= end_offset)
			/* left search boundary reached. */
			break;
		start = end - 1;
	}

	release_and_unlock_bnode(bnode);
	return ret;
}

/* allocate contiguous range of blocks in bitmap */
static int bitmap_alloc_forward(reiser4_block_nr * start,
				const reiser4_block_nr * end, int min_len,
				int max_len)
{
	bmap_nr_t bmap, end_bmap;
	bmap_off_t offset, end_offset;
	int len;

	reiser4_block_nr tmp;

	struct super_block *super = get_current_context()->super;
	const bmap_off_t max_offset = bmap_bit_count(super->s_blocksize);

	parse_blocknr(start, &bmap, &offset);

	tmp = *end - 1;
	parse_blocknr(&tmp, &end_bmap, &end_offset);
	++end_offset;

	assert("zam-358", end_bmap >= bmap);
	assert("zam-359", ergo(end_bmap == bmap, end_offset >= offset));

	for (; bmap < end_bmap; bmap++, offset = 0) {
		len =
		    search_one_bitmap_forward(bmap, &offset, max_offset,
					      min_len, max_len);
		if (len != 0)
			goto out;
	}

	len =
	    search_one_bitmap_forward(bmap, &offset, end_offset, min_len,
				      max_len);
      out:
	*start = bmap * max_offset + offset;
	return len;
}

/* allocate contiguous range of blocks in bitmap (from @start to @end in
 * backward direction) */
static int bitmap_alloc_backward(reiser4_block_nr * start,
				 const reiser4_block_nr * end, int min_len,
				 int max_len)
{
	bmap_nr_t bmap, end_bmap;
	bmap_off_t offset, end_offset;
	int len;
	struct super_block *super = get_current_context()->super;
	const bmap_off_t max_offset = bmap_bit_count(super->s_blocksize);

	parse_blocknr(start, &bmap, &offset);
	parse_blocknr(end, &end_bmap, &end_offset);

	assert("zam-961", end_bmap <= bmap);
	assert("zam-962", ergo(end_bmap == bmap, end_offset <= offset));

	for (; bmap > end_bmap; bmap--, offset = max_offset - 1) {
		len =
		    search_one_bitmap_backward(bmap, &offset, 0, min_len,
					       max_len);
		if (len != 0)
			goto out;
	}

	len =
	    search_one_bitmap_backward(bmap, &offset, end_offset, min_len,
				       max_len);
      out:
	*start = bmap * max_offset + offset;
	return len;
}

/* plugin->u.space_allocator.alloc_blocks() */
static int alloc_blocks_forward(reiser4_blocknr_hint *hint, int needed,
				reiser4_block_nr *start, reiser4_block_nr *len)
{
	struct super_block *super = get_current_context()->super;
	int actual_len;

	reiser4_block_nr search_start;
	reiser4_block_nr search_end;

	assert("zam-398", super != NULL);
	assert("zam-412", hint != NULL);
	assert("zam-397", hint->blk <= reiser4_block_count(super));

	if (hint->max_dist == 0)
		search_end = reiser4_block_count(super);
	else
		search_end =
		    LIMIT(hint->blk + hint->max_dist,
			  reiser4_block_count(super));

	/* We use @hint -> blk as a search start and search from it to the end
	   of the disk or in given region if @hint -> max_dist is not zero */
	search_start = hint->blk;

	actual_len =
	    bitmap_alloc_forward(&search_start, &search_end, 1, needed);

	/* There is only one bitmap search if max_dist was specified or first
	   pass was from the beginning of the bitmap. We also do one pass for
	   scanning bitmap in backward direction. */
	if (!(actual_len != 0 || hint->max_dist != 0 || search_start == 0)) {
		/* next step is a scanning from 0 to search_start */
		search_end = search_start;
		search_start = 0;
		actual_len =
		    bitmap_alloc_forward(&search_start, &search_end, 1, needed);
	}
	if (actual_len == 0)
		return RETERR(-ENOSPC);
	if (actual_len < 0)
		return RETERR(actual_len);
	*len = actual_len;
	*start = search_start;
	return 0;
}

static int alloc_blocks_backward(reiser4_blocknr_hint * hint, int needed,
				 reiser4_block_nr * start,
				 reiser4_block_nr * len)
{
	reiser4_block_nr search_start;
	reiser4_block_nr search_end;
	int actual_len;

	ON_DEBUG(struct super_block *super = reiser4_get_current_sb());

	assert("zam-969", super != NULL);
	assert("zam-970", hint != NULL);
	assert("zam-971", hint->blk <= reiser4_block_count(super));

	search_start = hint->blk;
	if (hint->max_dist == 0 || search_start <= hint->max_dist)
		search_end = 0;
	else
		search_end = search_start - hint->max_dist;

	actual_len =
	    bitmap_alloc_backward(&search_start, &search_end, 1, needed);
	if (actual_len == 0)
		return RETERR(-ENOSPC);
	if (actual_len < 0)
		return RETERR(actual_len);
	*len = actual_len;
	*start = search_start;
	return 0;
}

/* plugin->u.space_allocator.alloc_blocks() */
int reiser4_alloc_blocks_bitmap(reiser4_space_allocator * allocator,
				reiser4_blocknr_hint * hint, int needed,
				reiser4_block_nr * start, reiser4_block_nr * len)
{
	if (hint->backward)
		return alloc_blocks_backward(hint, needed, start, len);
	return alloc_blocks_forward(hint, needed, start, len);
}

/* plugin->u.space_allocator.dealloc_blocks(). */
/* It just frees blocks in WORKING BITMAP. Usually formatted an unformatted
   nodes deletion is deferred until transaction commit.  However, deallocation
   of temporary objects like wandered blocks and transaction commit records
   requires immediate node deletion from WORKING BITMAP.*/
void reiser4_dealloc_blocks_bitmap(reiser4_space_allocator * allocator,
				   reiser4_block_nr start, reiser4_block_nr len)
{
	struct super_block *super = reiser4_get_current_sb();

	bmap_nr_t bmap;
	bmap_off_t offset;

	struct bitmap_node *bnode;
	int ret;

	assert("zam-468", len != 0);
	check_block_range(&start, &len);

	parse_blocknr(&start, &bmap, &offset);

	assert("zam-469", offset + len <= bmap_bit_count(super->s_blocksize));

	bnode = get_bnode(super, bmap);

	assert("zam-470", bnode != NULL);

	ret = load_and_lock_bnode(bnode);
	assert("zam-481", ret == 0);

	reiser4_clear_bits(bnode_working_data(bnode), offset,
			   (bmap_off_t) (offset + len));

	adjust_first_zero_bit(bnode, offset);

	release_and_unlock_bnode(bnode);
}

static int check_blocks_one_bitmap(bmap_nr_t bmap, bmap_off_t start_offset,
                                    bmap_off_t end_offset, int desired)
{
	struct super_block *super = reiser4_get_current_sb();
	struct bitmap_node *bnode = get_bnode(super, bmap);
	int ret;

	assert("nikita-2215", bnode != NULL);

	ret = load_and_lock_bnode(bnode);
	assert("zam-626", ret == 0);

	assert("nikita-2216", jnode_is_loaded(bnode->wjnode));

	if (desired) {
		ret = reiser4_find_next_zero_bit(bnode_working_data(bnode),
						  end_offset, start_offset)
		      >= end_offset;
	} else {
		ret = reiser4_find_next_set_bit(bnode_working_data(bnode),
						 end_offset, start_offset)
		      >= end_offset;
	}

	release_and_unlock_bnode(bnode);

	return ret;
}

/* plugin->u.space_allocator.check_blocks(). */
int reiser4_check_blocks_bitmap(const reiser4_block_nr * start,
				 const reiser4_block_nr * len, int desired)
{
	struct super_block *super = reiser4_get_current_sb();

	reiser4_block_nr end;
	bmap_nr_t bmap, end_bmap;
	bmap_off_t offset, end_offset;
	const bmap_off_t max_offset = bmap_bit_count(super->s_blocksize);

	assert("intelfx-9", start != NULL);
	assert("intelfx-10", ergo(len != NULL, *len > 0));

	if (len != NULL) {
		check_block_range(start, len);
		end = *start + *len - 1;
	} else {
		/* on next line, end is used as temporary len for check_block_range() */
		end = 1; check_block_range(start, &end);
		end = *start;
	}

	parse_blocknr(start, &bmap, &offset);

	if (end == *start) {
		end_bmap = bmap;
		end_offset = offset;
	} else {
		parse_blocknr(&end, &end_bmap, &end_offset);
	}
	++end_offset;

	assert("intelfx-4", end_bmap >= bmap);
	assert("intelfx-5", ergo(end_bmap == bmap, end_offset >= offset));

	for (; bmap < end_bmap; bmap++, offset = 0) {
		if (!check_blocks_one_bitmap(bmap, offset, max_offset, desired)) {
			return 0;
		}
	}
	return check_blocks_one_bitmap(bmap, offset, end_offset, desired);
}

/* conditional insertion of @node into atom's overwrite set  if it was not there */
static void cond_add_to_overwrite_set(txn_atom * atom, jnode * node)
{
	assert("zam-546", atom != NULL);
	assert("zam-547", atom->stage == ASTAGE_PRE_COMMIT);
	assert("zam-548", node != NULL);

	spin_lock_atom(atom);
	spin_lock_jnode(node);

	if (node->atom == NULL) {
		JF_SET(node, JNODE_OVRWR);
		insert_into_atom_ovrwr_list(atom, node);
	} else {
		assert("zam-549", node->atom == atom);
	}

	spin_unlock_jnode(node);
	spin_unlock_atom(atom);
}

/* an actor which applies delete set to COMMIT bitmap pages and link modified
   pages in a single-linked list */
static int
apply_dset_to_commit_bmap(txn_atom * atom, const reiser4_block_nr * start,
			  const reiser4_block_nr * len, void *data)
{

	bmap_nr_t bmap;
	bmap_off_t offset;
	int ret;

	long long *blocks_freed_p = data;

	struct bitmap_node *bnode;

	struct super_block *sb = reiser4_get_current_sb();

	check_block_range(start, len);

	parse_blocknr(start, &bmap, &offset);

	/* FIXME-ZAM: we assume that all block ranges are allocated by this
	   bitmap-based allocator and each block range can't go over a zone of
	   responsibility of one bitmap block; same assumption is used in
	   other journal hooks in bitmap code. */
	bnode = get_bnode(sb, bmap);
	assert("zam-448", bnode != NULL);

	/* it is safe to unlock atom with is in ASTAGE_PRE_COMMIT */
	assert("zam-767", atom->stage == ASTAGE_PRE_COMMIT);
	ret = load_and_lock_bnode(bnode);
	if (ret)
		return ret;

	/* put bnode into atom's overwrite set */
	cond_add_to_overwrite_set(atom, bnode->cjnode);

	data = bnode_commit_data(bnode);

	ret = bnode_check_crc(bnode);
	if (ret != 0)
		return ret;

	if (len != NULL) {
		/* FIXME-ZAM: a check that all bits are set should be there */
		assert("zam-443",
		       offset + *len <= bmap_bit_count(sb->s_blocksize));
		reiser4_clear_bits(data, offset, (bmap_off_t) (offset + *len));

		(*blocks_freed_p) += *len;
	} else {
		reiser4_clear_bit(offset, data);
		(*blocks_freed_p)++;
	}

	bnode_set_commit_crc(bnode, bnode_calc_crc(bnode, sb->s_blocksize));

	release_and_unlock_bnode(bnode);

	return 0;
}

/* plugin->u.space_allocator.pre_commit_hook(). */
/* It just applies transaction changes to fs-wide COMMIT BITMAP, hoping the
   rest is done by transaction manager (allocate wandered locations for COMMIT
   BITMAP blocks, copy COMMIT BITMAP blocks data). */
/* Only one instance of this function can be running at one given time, because
   only one transaction can be committed a time, therefore it is safe to access
   some global variables without any locking */

int reiser4_pre_commit_hook_bitmap(void)
{
	struct super_block *super = reiser4_get_current_sb();
	txn_atom *atom;

	long long blocks_freed = 0;

	atom = get_current_atom_locked();
	assert("zam-876", atom->stage == ASTAGE_PRE_COMMIT);
	spin_unlock_atom(atom);

	{			/* scan atom's captured list and find all freshly allocated nodes,
				 * mark corresponded bits in COMMIT BITMAP as used */
		struct list_head *head = ATOM_CLEAN_LIST(atom);
		jnode *node = list_entry(head->next, jnode, capture_link);

		while (head != &node->capture_link) {
			/* we detect freshly allocated jnodes */
			if (JF_ISSET(node, JNODE_RELOC)) {
				int ret;
				bmap_nr_t bmap;

				bmap_off_t offset;
				bmap_off_t index;
				struct bitmap_node *bn;
				__u32 size = bmap_size(super->s_blocksize);
				__u32 crc;
				char byte;

				assert("zam-559", !JF_ISSET(node, JNODE_OVRWR));
				assert("zam-460",
				       !reiser4_blocknr_is_fake(&node->blocknr));

				parse_blocknr(&node->blocknr, &bmap, &offset);
				bn = get_bnode(super, bmap);

				index = offset >> 3;
				assert("vpf-276", index < size);

				ret = bnode_check_crc(bnode);
				if (ret != 0)
					return ret;

				check_bnode_loaded(bn);
				load_and_lock_bnode(bn);

				byte = *(bnode_commit_data(bn) + index);
				reiser4_set_bit(offset, bnode_commit_data(bn));

				crc = adler32_recalc(bnode_commit_crc(bn), byte,
						     *(bnode_commit_data(bn) +
						       index),
						     size - index),
				    bnode_set_commit_crc(bn, crc);

				release_and_unlock_bnode(bn);

				ret = bnode_check_crc(bn);
				if (ret != 0)
					return ret;

				/* working of this depends on how it inserts
				   new j-node into clean list, because we are
				   scanning the same list now. It is OK, if
				   insertion is done to the list front */
				cond_add_to_overwrite_set(atom, bn->cjnode);
			}

			node = list_entry(node->capture_link.next, jnode, capture_link);
		}
	}

	atom_dset_deferred_apply(atom, apply_dset_to_commit_bmap, &blocks_freed, 0);

	blocks_freed -= atom->nr_blocks_allocated;

	{
		reiser4_super_info_data *sbinfo;

		sbinfo = get_super_private(super);

		spin_lock_reiser4_super(sbinfo);
		sbinfo->blocks_free_committed += blocks_freed;
		spin_unlock_reiser4_super(sbinfo);
	}

	return 0;
}

/* plugin->u.space_allocator.init_allocator
    constructor of reiser4_space_allocator object. It is called on fs mount */
int reiser4_init_allocator_bitmap(reiser4_space_allocator * allocator,
				  struct super_block *super, void *arg)
{
	struct bitmap_allocator_data *data = NULL;
	bmap_nr_t bitmap_blocks_nr;
	bmap_nr_t i;

	assert("nikita-3039", reiser4_schedulable());

	/* getting memory for bitmap allocator private data holder */
	data =
		kmalloc(sizeof(struct bitmap_allocator_data),
			reiser4_ctx_gfp_mask_get());

	if (data == NULL)
		return RETERR(-ENOMEM);

	/* allocation and initialization for the array of bnodes */
	bitmap_blocks_nr = get_nr_bmap(super);

	/* FIXME-ZAM: it is not clear what to do with huge number of bitmaps
	   which is bigger than 2^32 (= 8 * 4096 * 4096 * 2^32 bytes = 5.76e+17,
	   may I never meet someone who still uses the ia32 architecture when
	   storage devices of that size enter the market, and wants to use ia32
	   with that storage device, much less reiser4. ;-) -Hans). Kmalloc is not possible and,
	   probably, another dynamic data structure should replace a static
	   array of bnodes. */
	/*data->bitmap = reiser4_kmalloc((size_t) (sizeof (struct bitmap_node) * bitmap_blocks_nr), GFP_KERNEL); */
	data->bitmap = reiser4_vmalloc(sizeof(struct bitmap_node) * bitmap_blocks_nr);
	if (data->bitmap == NULL) {
		kfree(data);
		return RETERR(-ENOMEM);
	}

	for (i = 0; i < bitmap_blocks_nr; i++)
		init_bnode(data->bitmap + i, super, i);

	allocator->u.generic = data;

#if REISER4_DEBUG
	get_super_private(super)->min_blocks_used += bitmap_blocks_nr;
#endif

	/* Load all bitmap blocks at mount time. */
	if (!test_bit
	    (REISER4_DONT_LOAD_BITMAP, &get_super_private(super)->fs_flags)) {
		__u64 start_time, elapsed_time;
		struct bitmap_node *bnode;
		int ret;

		if (REISER4_DEBUG)
			printk(KERN_INFO "loading reiser4 bitmap...");
		start_time = jiffies;

		for (i = 0; i < bitmap_blocks_nr; i++) {
			bnode = data->bitmap + i;
			ret = load_and_lock_bnode(bnode);
			if (ret) {
				reiser4_destroy_allocator_bitmap(allocator,
								 super);
				return ret;
			}
			release_and_unlock_bnode(bnode);
		}

		elapsed_time = jiffies - start_time;
		if (REISER4_DEBUG)
			printk("...done (%llu jiffies)\n",
			       (unsigned long long)elapsed_time);
	}

	return 0;
}

/* plugin->u.space_allocator.destroy_allocator
   destructor. It is called on fs unmount */
int reiser4_destroy_allocator_bitmap(reiser4_space_allocator * allocator,
				     struct super_block *super)
{
	bmap_nr_t bitmap_blocks_nr;
	bmap_nr_t i;

	struct bitmap_allocator_data *data = allocator->u.generic;

	assert("zam-414", data != NULL);
	assert("zam-376", data->bitmap != NULL);

	bitmap_blocks_nr = get_nr_bmap(super);

	for (i = 0; i < bitmap_blocks_nr; i++) {
		struct bitmap_node *bnode = data->bitmap + i;

		mutex_lock(&bnode->mutex);

#if REISER4_DEBUG
		if (atomic_read(&bnode->loaded)) {
			jnode *wj = bnode->wjnode;
			jnode *cj = bnode->cjnode;

			assert("zam-480", jnode_page(cj) != NULL);
			assert("zam-633", jnode_page(wj) != NULL);

			assert("zam-634",
			       memcmp(jdata(wj), jdata(wj),
				      bmap_size(super->s_blocksize)) == 0);

		}
#endif
		done_bnode(bnode);
		mutex_unlock(&bnode->mutex);
	}

	vfree(data->bitmap);
	kfree(data);

	allocator->u.generic = NULL;

	return 0;
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
