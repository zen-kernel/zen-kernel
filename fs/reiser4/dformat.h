/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
   reiser4/README */

/* Formats of on-disk data and conversion functions. */

/* put all item formats in the files describing the particular items,
   our model is, everything you need to do to add an item to reiser4,
   (excepting the changes to the plugin that uses the item which go
   into the file defining that plugin), you put into one file. */
/* Data on disk are stored in little-endian format.
   To declare fields of on-disk structures, use d8, d16, d32 and d64.
   d??tocpu() and cputod??() to convert. */

#if !defined(__FS_REISER4_DFORMAT_H__)
#define __FS_REISER4_DFORMAT_H__

#include "debug.h"

#include <asm/byteorder.h>
#include <asm/unaligned.h>
#include <linux/types.h>

typedef __u8 d8;
typedef __le16 d16;
typedef __le32 d32;
typedef __le64 d64;

#define PACKED __attribute__((packed))

/* data-type for block number */
typedef __u64 reiser4_block_nr;

/* data-type for block number on disk, disk format */
typedef __le64 reiser4_dblock_nr;

/**
 * disk_addr_eq - compare disk addresses
 * @b1: pointer to block number ot compare
 * @b2: pointer to block number ot compare
 *
 * Returns true if if disk addresses are the same
 */
static inline int disk_addr_eq(const reiser4_block_nr * b1,
			       const reiser4_block_nr * b2)
{
	assert("nikita-1033", b1 != NULL);
	assert("nikita-1266", b2 != NULL);

	return !memcmp(b1, b2, sizeof *b1);
}

/* structure of master reiser4 super block */
typedef struct reiser4_master_sb {
	char magic[16];		/* "ReIsEr4" */
	__le16 disk_plugin_id;	/* id of disk layout plugin */
	__le16 blocksize;
	char uuid[16];		/* unique id */
	char label[16];		/* filesystem label */
	__le64 diskmap;		/* location of the diskmap. 0 if not present */
} reiser4_master_sb;

/* __FS_REISER4_DFORMAT_H__ */
#endif

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 79
 * End:
 */
