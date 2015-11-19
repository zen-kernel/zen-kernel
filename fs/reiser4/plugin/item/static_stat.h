/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* This describes the static_stat item, used to hold all information needed by the stat() syscall.

In the case where each file has not less than the fields needed by the
stat() syscall, it is more compact to store those fields in this
struct.

If this item does not exist, then all stats are dynamically resolved.
At the moment, we either resolve all stats dynamically or all of them
statically.  If you think this is not fully optimal, and the rest of
reiser4 is working, then fix it...:-)

*/

#if !defined( __FS_REISER4_PLUGIN_ITEM_STATIC_STAT_H__ )
#define __FS_REISER4_PLUGIN_ITEM_STATIC_STAT_H__

#include "../../forward.h"
#include "../../dformat.h"

#include <linux/fs.h>		/* for struct inode */

/* Stat data layout: goals and implementation.

   We want to be able to have lightweight files which have complete flexibility in what semantic metadata is attached to
   them, including not having semantic metadata attached to them.

   There is one problem with doing that, which is that if in fact you have exactly the same metadata for most files you
   want to store, then it takes more space to store that metadata in a dynamically sized structure than in a statically
   sized structure because the statically sized structure knows without recording it what the names and lengths of the
   attributes are.

   This leads to a natural compromise, which is to special case those files which have simply the standard unix file
   attributes, and only employ the full dynamic stat data mechanism for those files that differ from the standard unix
   file in their use of file attributes.

   Yet this compromise deserves to be compromised a little.

   We accommodate the case where you have no more than the standard unix file attributes by using an "extension
   bitmask": each bit in it indicates presence or absence of or particular stat data extension (see sd_ext_bits enum).

   If the first bit of the extension bitmask bit is 0, we have light-weight file whose attributes are either inherited
   from parent directory (as uid, gid) or initialised to some sane values.

   To capitalize on existing code infrastructure, extensions are
   implemented as plugins of type REISER4_SD_EXT_PLUGIN_TYPE.
   Each stat-data extension plugin implements four methods:

    ->present() called by sd_load() when this extension is found in stat-data
    ->absent() called by sd_load() when this extension is not found in stat-data
    ->save_len() called by sd_len() to calculate total length of stat-data
    ->save() called by sd_save() to store extension data into stat-data

    Implementation is in fs/reiser4/plugin/item/static_stat.c
*/

/* stat-data extension. Please order this by presumed frequency of use */
typedef enum {
	/* support for light-weight files */
	LIGHT_WEIGHT_STAT,
	/* data required to implement unix stat(2) call. Layout is in
	   reiser4_unix_stat. If this is not present, file is light-weight */
	UNIX_STAT,
	/* this contains additional set of 32bit [anc]time fields to implement
	   nanosecond resolution. Layout is in reiser4_large_times_stat. Usage
	   if this extension is governed by 32bittimes mount option. */
	LARGE_TIMES_STAT,
	/* stat data has link name included */
	SYMLINK_STAT,
	/* on-disk slots of non-standard plugins for main plugin table
	   (@reiser4_inode->pset), that is, plugins that cannot be deduced
	   from file mode bits), for example, aggregation, interpolation etc. */
	PLUGIN_STAT,
	/* this extension contains persistent inode flags. These flags are
	   single bits: immutable, append, only, etc. Layout is in
	   reiser4_flags_stat. */
	FLAGS_STAT,
	/* this extension contains capabilities sets, associated with this
	   file. Layout is in reiser4_capabilities_stat */
	CAPABILITIES_STAT,
	/* this extension contains size and public id of the secret key.
	   Layout is in reiser4_crypto_stat */
	CRYPTO_STAT,
	/* on-disk slots of non-default plugins for inheritance, which
	   are extracted to special plugin table (@reiser4_inode->hset).
	   By default, children of the object will inherit plugins from
	   its main plugin table (pset). */
	HEIR_STAT,
	LAST_SD_EXTENSION,
	/*
	 * init_inode_static_sd() iterates over extension mask until all
	 * non-zero bits are processed. This means, that neither ->present(),
	 * nor ->absent() methods will be called for stat-data extensions that
	 * go after last present extension. But some basic extensions, we want
	 * either ->absent() or ->present() method to be called, because these
	 * extensions set up something in inode even when they are not
	 * present. This is what LAST_IMPORTANT_SD_EXTENSION is for: for all
	 * extensions before and including LAST_IMPORTANT_SD_EXTENSION either
	 * ->present(), or ->absent() method will be called, independently of
	 * what other extensions are present.
	 */
	LAST_IMPORTANT_SD_EXTENSION = PLUGIN_STAT
} sd_ext_bits;

/* minimal stat-data. This allows to support light-weight files. */
typedef struct reiser4_stat_data_base {
	/*  0 */ __le16 extmask;
	/*  2 */
} PACKED reiser4_stat_data_base;

typedef struct reiser4_light_weight_stat {
	/*  0 */ __le16 mode;
	/*  2 */ __le32 nlink;
	/*  6 */ __le64 size;
	/* size in bytes */
	/* 14 */
} PACKED reiser4_light_weight_stat;

typedef struct reiser4_unix_stat {
	/* owner id */
	/*  0 */ __le32 uid;
	/* group id */
	/*  4 */ __le32 gid;
	/* access time */
	/*  8 */ __le32 atime;
	/* modification time */
	/* 12 */ __le32 mtime;
	/* change time */
	/* 16 */ __le32 ctime;
	union {
		/* minor:major for device files */
		/* 20 */ __le64 rdev;
		/* bytes used by file */
		/* 20 */ __le64 bytes;
	} u;
	/* 28 */
} PACKED reiser4_unix_stat;

/* symlink stored as part of inode */
typedef struct reiser4_symlink_stat {
	char body[0];
} PACKED reiser4_symlink_stat;

typedef struct reiser4_plugin_slot {
	/*  0 */ __le16 pset_memb;
	/*  2 */ __le16 id;
	/*  4 *//* here plugin stores its persistent state */
} PACKED reiser4_plugin_slot;

/* stat-data extension for files with non-standard plugin. */
typedef struct reiser4_plugin_stat {
	/* number of additional plugins, associated with this object */
	/*  0 */ __le16 plugins_no;
	/*  2 */ reiser4_plugin_slot slot[0];
	/*  2 */
} PACKED reiser4_plugin_stat;

/* stat-data extension for inode flags. Currently it is just fixed-width 32
 * bit mask. If need arise, this can be replaced with variable width
 * bitmask. */
typedef struct reiser4_flags_stat {
	/*  0 */ __le32 flags;
	/*  4 */
} PACKED reiser4_flags_stat;

typedef struct reiser4_capabilities_stat {
	/*  0 */ __le32 effective;
	/*  8 */ __le32 permitted;
	/* 16 */
} PACKED reiser4_capabilities_stat;

typedef struct reiser4_cluster_stat {
/* this defines cluster size (an attribute of cryptcompress objects) as PAGE_SIZE << cluster shift */
	/* 0 */ d8 cluster_shift;
	/* 1 */
} PACKED reiser4_cluster_stat;

typedef struct reiser4_crypto_stat {
	/* secret key size, bits */
	/*  0 */ d16 keysize;
	/* secret key id */
	/*  2 */ d8 keyid[0];
	/* 2 */
} PACKED reiser4_crypto_stat;

typedef struct reiser4_large_times_stat {
	/* access time */
	/* 0 */ d32 atime;
	/* modification time */
	/* 4 */ d32 mtime;
	/* change time */
	/* 8 */ d32 ctime;
	/* 12 */
} PACKED reiser4_large_times_stat;

/* this structure is filled by sd_item_stat */
typedef struct sd_stat {
	int dirs;
	int files;
	int others;
} sd_stat;

/* plugin->item.common.* */
extern void print_sd(const char *prefix, coord_t * coord);
extern void item_stat_static_sd(const coord_t * coord, void *vp);

/* plugin->item.s.sd.* */
extern int init_inode_static_sd(struct inode *inode, char *sd, int len);
extern int save_len_static_sd(struct inode *inode);
extern int save_static_sd(struct inode *inode, char **area);

/* __FS_REISER4_PLUGIN_ITEM_STATIC_STAT_H__ */
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
