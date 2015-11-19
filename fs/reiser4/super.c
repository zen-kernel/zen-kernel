/* Copyright 2001, 2002, 2003, 2004 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Super-block manipulations. */

#include "debug.h"
#include "dformat.h"
#include "key.h"
#include "plugin/security/perm.h"
#include "plugin/space/space_allocator.h"
#include "plugin/plugin.h"
#include "tree.h"
#include "vfs_ops.h"
#include "super.h"
#include "reiser4.h"

#include <linux/types.h>	/* for __u??  */
#include <linux/fs.h>		/* for struct super_block  */

static __u64 reserved_for_gid(const struct super_block *super, gid_t gid);
static __u64 reserved_for_uid(const struct super_block *super, uid_t uid);
static __u64 reserved_for_root(const struct super_block *super);

/* Return reiser4-specific part of super block */
reiser4_super_info_data *get_super_private_nocheck(const struct super_block *super)
{
	return (reiser4_super_info_data *) super->s_fs_info;
}

/* Return reiser4 fstype: value that is returned in ->f_type field by statfs()
 */
long reiser4_statfs_type(const struct super_block *super UNUSED_ARG)
{
	assert("nikita-448", super != NULL);
	assert("nikita-449", is_reiser4_super(super));
	return (long)REISER4_SUPER_MAGIC;
}

/* functions to read/modify fields of reiser4_super_info_data */

/* get number of blocks in file system */
__u64 reiser4_block_count(const struct super_block *super	/* super block
								   queried */ )
{
	assert("vs-494", super != NULL);
	assert("vs-495", is_reiser4_super(super));
	return get_super_private(super)->block_count;
}

#if REISER4_DEBUG
/*
 * number of blocks in the current file system
 */
__u64 reiser4_current_block_count(void)
{
	return get_current_super_private()->block_count;
}
#endif  /*  REISER4_DEBUG  */

/* set number of block in filesystem */
void reiser4_set_block_count(const struct super_block *super, __u64 nr)
{
	assert("vs-501", super != NULL);
	assert("vs-502", is_reiser4_super(super));
	get_super_private(super)->block_count = nr;
	/*
	 * The proper calculation of the reserved space counter (%5 of device
	 * block counter) we need a 64 bit division which is missing in Linux
	 * on i386 platform. Because we do not need a precise calculation here
	 * we can replace a div64 operation by this combination of
	 * multiplication and shift: 51. / (2^10) == .0498 .
	 * FIXME: this is a bug. It comes up only for very small filesystems
	 * which probably are never used. Nevertheless, it is a bug. Number of
	 * reserved blocks must be not less than maximal number of blocks which
	 * get grabbed with BA_RESERVED.
	 */
	get_super_private(super)->blocks_reserved = ((nr * 51) >> 10);
}

/* amount of blocks used (allocated for data) in file system */
__u64 reiser4_data_blocks(const struct super_block *super	/* super block
								   queried */ )
{
	assert("nikita-452", super != NULL);
	assert("nikita-453", is_reiser4_super(super));
	return get_super_private(super)->blocks_used;
}

/* set number of block used in filesystem */
void reiser4_set_data_blocks(const struct super_block *super, __u64 nr)
{
	assert("vs-503", super != NULL);
	assert("vs-504", is_reiser4_super(super));
	get_super_private(super)->blocks_used = nr;
}

/* amount of free blocks in file system */
__u64 reiser4_free_blocks(const struct super_block *super	/* super block
								   queried */ )
{
	assert("nikita-454", super != NULL);
	assert("nikita-455", is_reiser4_super(super));
	return get_super_private(super)->blocks_free;
}

/* set number of blocks free in filesystem */
void reiser4_set_free_blocks(const struct super_block *super, __u64 nr)
{
	assert("vs-505", super != NULL);
	assert("vs-506", is_reiser4_super(super));
	get_super_private(super)->blocks_free = nr;
}

/* get mkfs unique identifier */
__u32 reiser4_mkfs_id(const struct super_block *super	/* super block
							   queried */ )
{
	assert("vpf-221", super != NULL);
	assert("vpf-222", is_reiser4_super(super));
	return get_super_private(super)->mkfs_id;
}

/* amount of free blocks in file system */
__u64 reiser4_free_committed_blocks(const struct super_block *super)
{
	assert("vs-497", super != NULL);
	assert("vs-498", is_reiser4_super(super));
	return get_super_private(super)->blocks_free_committed;
}

/* amount of blocks in the file system reserved for @uid and @gid */
long reiser4_reserved_blocks(const struct super_block *super	/* super block
								   queried */ ,
			     uid_t uid /* user id */ ,
			     gid_t gid/* group id */)
{
	long reserved;

	assert("nikita-456", super != NULL);
	assert("nikita-457", is_reiser4_super(super));

	reserved = 0;
	if (REISER4_SUPPORT_GID_SPACE_RESERVATION)
		reserved += reserved_for_gid(super, gid);
	if (REISER4_SUPPORT_UID_SPACE_RESERVATION)
		reserved += reserved_for_uid(super, uid);
	if (REISER4_SUPPORT_ROOT_SPACE_RESERVATION && (uid == 0))
		reserved += reserved_for_root(super);
	return reserved;
}

/* get/set value of/to grabbed blocks counter */
__u64 reiser4_grabbed_blocks(const struct super_block * super)
{
	assert("zam-512", super != NULL);
	assert("zam-513", is_reiser4_super(super));

	return get_super_private(super)->blocks_grabbed;
}

__u64 reiser4_flush_reserved(const struct super_block *super)
{
	assert("vpf-285", super != NULL);
	assert("vpf-286", is_reiser4_super(super));

	return get_super_private(super)->blocks_flush_reserved;
}

/* get/set value of/to counter of fake allocated formatted blocks */
__u64 reiser4_fake_allocated(const struct super_block *super)
{
	assert("zam-516", super != NULL);
	assert("zam-517", is_reiser4_super(super));

	return get_super_private(super)->blocks_fake_allocated;
}

/* get/set value of/to counter of fake allocated unformatted blocks */
__u64 reiser4_fake_allocated_unformatted(const struct super_block *super)
{
	assert("zam-516", super != NULL);
	assert("zam-517", is_reiser4_super(super));

	return get_super_private(super)->blocks_fake_allocated_unformatted;
}

/* get/set value of/to counter of clustered blocks */
__u64 reiser4_clustered_blocks(const struct super_block *super)
{
	assert("edward-601", super != NULL);
	assert("edward-602", is_reiser4_super(super));

	return get_super_private(super)->blocks_clustered;
}

/* space allocator used by this file system */
reiser4_space_allocator * reiser4_get_space_allocator(const struct super_block
						      *super)
{
	assert("nikita-1965", super != NULL);
	assert("nikita-1966", is_reiser4_super(super));
	return &get_super_private(super)->space_allocator;
}

/* return fake inode used to bind formatted nodes in the page cache */
struct inode *reiser4_get_super_fake(const struct super_block *super)
{
	assert("nikita-1757", super != NULL);
	return get_super_private(super)->fake;
}

/* return fake inode used to bind copied on capture nodes in the page cache */
struct inode *reiser4_get_cc_fake(const struct super_block *super)
{
	assert("nikita-1757", super != NULL);
	return get_super_private(super)->cc;
}

/* return fake inode used to bind bitmaps and journlal heads */
struct inode *reiser4_get_bitmap_fake(const struct super_block *super)
{
	assert("nikita-17571", super != NULL);
	return get_super_private(super)->bitmap;
}

/* tree used by this file system */
reiser4_tree *reiser4_get_tree(const struct super_block *super)
{
	assert("nikita-460", super != NULL);
	assert("nikita-461", is_reiser4_super(super));
	return &get_super_private(super)->tree;
}

/* Check that @super is (looks like) reiser4 super block. This is mainly for
   use in assertions. */
int is_reiser4_super(const struct super_block *super)
{
	return
	    super != NULL &&
	    get_super_private(super) != NULL &&
	    super->s_op == &(get_super_private(super)->ops.super);
}

int reiser4_is_set(const struct super_block *super, reiser4_fs_flag f)
{
	return test_bit((int)f, &get_super_private(super)->fs_flags);
}

/* amount of blocks reserved for given group in file system */
static __u64 reserved_for_gid(const struct super_block *super UNUSED_ARG,
			      gid_t gid UNUSED_ARG/* group id */)
{
	return 0;
}

/* amount of blocks reserved for given user in file system */
static __u64 reserved_for_uid(const struct super_block *super UNUSED_ARG,
			      uid_t uid UNUSED_ARG/* user id */)
{
	return 0;
}

/* amount of blocks reserved for super user in file system */
static __u64 reserved_for_root(const struct super_block *super UNUSED_ARG)
{
	return 0;
}

/*
 * true if block number @blk makes sense for the file system at @super.
 */
int
reiser4_blocknr_is_sane_for(const struct super_block *super,
			    const reiser4_block_nr * blk)
{
	reiser4_super_info_data *sbinfo;

	assert("nikita-2957", super != NULL);
	assert("nikita-2958", blk != NULL);

	if (reiser4_blocknr_is_fake(blk))
		return 1;

	sbinfo = get_super_private(super);
	return *blk < sbinfo->block_count;
}

#if REISER4_DEBUG
/*
 * true, if block number @blk makes sense for the current file system
 */
int reiser4_blocknr_is_sane(const reiser4_block_nr * blk)
{
	return reiser4_blocknr_is_sane_for(reiser4_get_current_sb(), blk);
}
#endif  /*  REISER4_DEBUG  */

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
