/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
   reiser4/README */

/* Inode functions. */

#if !defined(__REISER4_INODE_H__)
#define __REISER4_INODE_H__

#include "forward.h"
#include "debug.h"
#include "key.h"
#include "seal.h"
#include "plugin/plugin.h"
#include "plugin/file/cryptcompress.h"
#include "plugin/file/file.h"
#include "plugin/dir/dir.h"
#include "plugin/plugin_set.h"
#include "plugin/security/perm.h"
#include "vfs_ops.h"
#include "jnode.h"
#include "fsdata.h"

#include <linux/types.h>	/* for __u?? , ino_t */
#include <linux/fs.h>		/* for struct super_block, struct
				 * rw_semaphore, etc  */
#include <linux/spinlock.h>
#include <asm/types.h>

/* reiser4-specific inode flags. They are "transient" and are not
   supposed to be stored on disk. Used to trace "state" of
   inode
*/
typedef enum {
	/* this is light-weight inode, inheriting some state from its
	   parent  */
	REISER4_LIGHT_WEIGHT = 0,
	/* stat data wasn't yet created */
	REISER4_NO_SD = 1,
	/* internal immutable flag. Currently is only used
	   to avoid race condition during file creation.
	   See comment in create_object(). */
	REISER4_IMMUTABLE = 2,
	/* inode was read from storage */
	REISER4_LOADED = 3,
	/* this bit is set for symlinks. inode->i_private points to target
	   name of symlink. */
	REISER4_GENERIC_PTR_USED = 4,
	/* set if size of stat-data item for this inode is known. If this is
	 * set we can avoid recalculating size of stat-data on each update. */
	REISER4_SDLEN_KNOWN = 5,
	/* reiser4_inode->crypt points to the crypto stat */
	REISER4_CRYPTO_STAT_LOADED = 6,
	/* cryptcompress_inode_data points to the secret key */
	REISER4_SECRET_KEY_INSTALLED = 7,
	/* File (possibly) has pages corresponding to the tail items, that
	 * were created by ->readpage. It is set by mmap_unix_file() and
	 * sendfile_unix_file(). This bit is inspected by write_unix_file and
	 * kill-hook of tail items. It is never cleared once set. This bit is
	 * modified and inspected under i_mutex. */
	REISER4_HAS_MMAP = 8,
	REISER4_PART_MIXED = 9,
	REISER4_PART_IN_CONV = 10,
	/* This flag indicates that file plugin conversion is in progress */
	REISER4_FILE_CONV_IN_PROGRESS = 11
} reiser4_file_plugin_flags;

/* state associated with each inode.
   reiser4 inode.

   NOTE-NIKITA In 2.5 kernels it is not necessary that all file-system inodes
   be of the same size. File-system allocates inodes by itself through
   s_op->allocate_inode() method. So, it is possible to adjust size of inode
   at the time of its creation.

   Invariants involving parts of this data-type:

      [inode->eflushed]

*/

typedef struct reiser4_inode reiser4_inode;
/* return pointer to reiser4-specific part of inode */
static inline reiser4_inode *reiser4_inode_data(const struct inode *inode
						/* inode queried */ );

#if BITS_PER_LONG == 64

#define REISER4_INO_IS_OID (1)
typedef struct {;
} oid_hi_t;

/* BITS_PER_LONG == 64 */
#else

#define REISER4_INO_IS_OID (0)
typedef __u32 oid_hi_t;

/* BITS_PER_LONG == 64 */
#endif

struct reiser4_inode {
	/* spin lock protecting fields of this structure. */
	spinlock_t guard;
	/* main plugin set that control the file
	   (see comments in plugin/plugin_set.c) */
	plugin_set *pset;
	/* plugin set for inheritance
	   (see comments in plugin/plugin_set.c) */
	plugin_set *hset;
	/* high 32 bits of object id */
	oid_hi_t oid_hi;
	/* seal for stat-data */
	seal_t sd_seal;
	/* locality id for this file */
	oid_t locality_id;
#if REISER4_LARGE_KEY
	__u64 ordering;
#endif
	/* coord of stat-data in sealed node */
	coord_t sd_coord;
	/* bit-mask of stat-data extentions used by this file */
	__u64 extmask;
	/* bitmask of non-default plugins for this inode */
	__u16 plugin_mask;
	/* bitmask of set heir plugins for this inode. */
	__u16 heir_mask;
	union {
		struct list_head readdir_list;
		struct list_head not_used;
	} lists;
	/* per-inode flags. Filled by values of reiser4_file_plugin_flags */
	unsigned long flags;
	union {
		/* fields specific to unix_file plugin */
		struct unix_file_info unix_file_info;
		/* fields specific to cryptcompress file plugin */
		struct cryptcompress_info cryptcompress_info;
	} file_plugin_data;

	/* this semaphore is to serialize readers and writers of @pset->file
	 * when file plugin conversion is enabled
	 */
	struct rw_semaphore conv_sem;

	/* tree of jnodes. Phantom jnodes (ones not attched to any atom) are
	   tagged in that tree by EFLUSH_TAG_ANONYMOUS */
	struct radix_tree_root jnodes_tree;
#if REISER4_DEBUG
	/* number of unformatted node jnodes of this file in jnode hash table */
	unsigned long nr_jnodes;
#endif

	/* block number of virtual root for this object. See comment above
	 * fs/reiser4/search.c:handle_vroot() */
	reiser4_block_nr vroot;
	struct mutex loading;
};

void loading_init_once(reiser4_inode *);
void loading_alloc(reiser4_inode *);
void loading_destroy(reiser4_inode *);

struct reiser4_inode_object {
	/* private part */
	reiser4_inode p;
	/* generic fields not specific to reiser4, but used by VFS */
	struct inode vfs_inode;
};

/* return pointer to the reiser4 specific portion of @inode */
static inline reiser4_inode *reiser4_inode_data(const struct inode *inode
						/* inode queried */ )
{
	assert("nikita-254", inode != NULL);
	return &container_of(inode, struct reiser4_inode_object, vfs_inode)->p;
}

static inline struct inode *inode_by_reiser4_inode(const reiser4_inode *
						   r4_inode /* inode queried */
						   )
{
	return &container_of(r4_inode, struct reiser4_inode_object,
			     p)->vfs_inode;
}

/*
 * reiser4 inodes are identified by 64bit object-id (oid_t), but in struct
 * inode ->i_ino field is of type ino_t (long) that can be either 32 or 64
 * bits.
 *
 * If ->i_ino is 32 bits we store remaining 32 bits in reiser4 specific part
 * of inode, otherwise whole oid is stored in i_ino.
 *
 * Wrappers below ([sg]et_inode_oid()) are used to hide this difference.
 */

#define OID_HI_SHIFT (sizeof(ino_t) * 8)

#if REISER4_INO_IS_OID

static inline oid_t get_inode_oid(const struct inode *inode)
{
	return inode->i_ino;
}

static inline void set_inode_oid(struct inode *inode, oid_t oid)
{
	inode->i_ino = oid;
}

/* REISER4_INO_IS_OID */
#else

static inline oid_t get_inode_oid(const struct inode *inode)
{
	return
	    ((__u64) reiser4_inode_data(inode)->oid_hi << OID_HI_SHIFT) |
	    inode->i_ino;
}

static inline void set_inode_oid(struct inode *inode, oid_t oid)
{
	assert("nikita-2519", inode != NULL);
	inode->i_ino = (ino_t) (oid);
	reiser4_inode_data(inode)->oid_hi = (oid) >> OID_HI_SHIFT;
	assert("nikita-2521", get_inode_oid(inode) == (oid));
}

/* REISER4_INO_IS_OID */
#endif

static inline oid_t get_inode_locality(const struct inode *inode)
{
	return reiser4_inode_data(inode)->locality_id;
}

#if REISER4_LARGE_KEY
static inline __u64 get_inode_ordering(const struct inode *inode)
{
	return reiser4_inode_data(inode)->ordering;
}

static inline void set_inode_ordering(const struct inode *inode, __u64 ordering)
{
	reiser4_inode_data(inode)->ordering = ordering;
}

#else

#define get_inode_ordering(inode) (0)
#define set_inode_ordering(inode, val) noop

#endif

/* return inode in which @uf_info is embedded */
static inline struct inode *
unix_file_info_to_inode(const struct unix_file_info *uf_info)
{
	return &container_of(uf_info, struct reiser4_inode_object,
			     p.file_plugin_data.unix_file_info)->vfs_inode;
}

extern ino_t oid_to_ino(oid_t oid) __attribute__ ((const));
extern ino_t oid_to_uino(oid_t oid) __attribute__ ((const));

extern reiser4_tree *reiser4_tree_by_inode(const struct inode *inode);

#if REISER4_DEBUG
extern void reiser4_inode_invariant(const struct inode *inode);
extern int inode_has_no_jnodes(reiser4_inode *);
#else
#define reiser4_inode_invariant(inode) noop
#endif

static inline int spin_inode_is_locked(const struct inode *inode)
{
	assert_spin_locked(&reiser4_inode_data(inode)->guard);
	return 1;
}

/**
 * spin_lock_inode - lock reiser4_inode' embedded spinlock
 * @inode: inode to lock
 *
 * In debug mode it checks that lower priority locks are not held and
 * increments reiser4_context's lock counters on which lock ordering checking
 * is based.
 */
static inline void spin_lock_inode(struct inode *inode)
{
	assert("", LOCK_CNT_NIL(spin_locked));
	/* check lock ordering */
	assert_spin_not_locked(&d_c_lock);

	spin_lock(&reiser4_inode_data(inode)->guard);

	LOCK_CNT_INC(spin_locked_inode);
	LOCK_CNT_INC(spin_locked);

	reiser4_inode_invariant(inode);
}

/**
 * spin_unlock_inode - unlock reiser4_inode' embedded spinlock
 * @inode: inode to unlock
 *
 * In debug mode it checks that spinlock is held and decrements
 * reiser4_context's lock counters on which lock ordering checking is based.
 */
static inline void spin_unlock_inode(struct inode *inode)
{
	assert_spin_locked(&reiser4_inode_data(inode)->guard);
	assert("nikita-1375", LOCK_CNT_GTZ(spin_locked_inode));
	assert("nikita-1376", LOCK_CNT_GTZ(spin_locked));

	reiser4_inode_invariant(inode);

	LOCK_CNT_DEC(spin_locked_inode);
	LOCK_CNT_DEC(spin_locked);

	spin_unlock(&reiser4_inode_data(inode)->guard);
}

extern znode *inode_get_vroot(struct inode *inode);
extern void inode_set_vroot(struct inode *inode, znode * vroot);

extern int reiser4_max_filename_len(const struct inode *inode);
extern int max_hash_collisions(const struct inode *dir);
extern void reiser4_unlock_inode(struct inode *inode);
extern int is_reiser4_inode(const struct inode *inode);
extern int setup_inode_ops(struct inode *inode, reiser4_object_create_data *);
extern struct inode *reiser4_iget(struct super_block *super,
				  const reiser4_key * key, int silent);
extern void reiser4_iget_complete(struct inode *inode);
extern void reiser4_inode_set_flag(struct inode *inode,
				   reiser4_file_plugin_flags f);
extern void reiser4_inode_clr_flag(struct inode *inode,
				   reiser4_file_plugin_flags f);
extern int reiser4_inode_get_flag(const struct inode *inode,
				  reiser4_file_plugin_flags f);

/*  has inode been initialized? */
static inline int
is_inode_loaded(const struct inode *inode/* inode queried */)
{
	assert("nikita-1120", inode != NULL);
	return reiser4_inode_get_flag(inode, REISER4_LOADED);
}

extern file_plugin *inode_file_plugin(const struct inode *inode);
extern dir_plugin *inode_dir_plugin(const struct inode *inode);
extern formatting_plugin *inode_formatting_plugin(const struct inode *inode);
extern hash_plugin *inode_hash_plugin(const struct inode *inode);
extern fibration_plugin *inode_fibration_plugin(const struct inode *inode);
extern cipher_plugin *inode_cipher_plugin(const struct inode *inode);
extern digest_plugin *inode_digest_plugin(const struct inode *inode);
extern compression_plugin *inode_compression_plugin(const struct inode *inode);
extern compression_mode_plugin *inode_compression_mode_plugin(const struct inode
							      *inode);
extern cluster_plugin *inode_cluster_plugin(const struct inode *inode);
extern file_plugin *inode_create_plugin(const struct inode *inode);
extern item_plugin *inode_sd_plugin(const struct inode *inode);
extern item_plugin *inode_dir_item_plugin(const struct inode *inode);
extern file_plugin *child_create_plugin(const struct inode *inode);

extern void reiser4_make_bad_inode(struct inode *inode);

extern void inode_set_extension(struct inode *inode, sd_ext_bits ext);
extern void inode_clr_extension(struct inode *inode, sd_ext_bits ext);
extern void inode_check_scale(struct inode *inode, __u64 old, __u64 new);
extern void inode_check_scale_nolock(struct inode *inode, __u64 old, __u64 new);

#define INODE_SET_SIZE(i, value)			\
({							\
	struct inode *__i;				\
	typeof(value) __v;				\
							\
	__i = (i);					\
	__v = (value);					\
	inode_check_scale(__i, __i->i_size, __v);	\
	i_size_write(__i, __v);				\
})

/*
 * update field @field in inode @i to contain value @value.
 */
#define INODE_SET_FIELD(i, field, value)		\
({							\
	struct inode *__i;				\
	typeof(value) __v;				\
							\
	__i = (i);					\
	__v = (value);					\
	inode_check_scale(__i, __i->field, __v);	\
	__i->field = __v;				\
})

#define INODE_INC_FIELD(i, field)				\
({								\
	struct inode *__i;					\
								\
	__i = (i);						\
	inode_check_scale(__i, __i->field, __i->field + 1);	\
	++ __i->field;						\
})

#define INODE_DEC_FIELD(i, field)				\
({								\
	struct inode *__i;					\
								\
	__i = (i);						\
	inode_check_scale(__i, __i->field, __i->field - 1);	\
	-- __i->field;						\
})

/*
 * Update field i_nlink in inode @i using library function @op.
 */
#define INODE_SET_NLINK(i, value)			\
({							\
	struct inode *__i;				\
	typeof(value) __v;				\
					        	\
	__i = (i);					\
	__v = (value);					\
        inode_check_scale(__i, __i->i_nlink, __v);	\
        set_nlink(__i, __v);				\
})

#define INODE_INC_NLINK(i)					\
	({							\
	struct inode *__i;					\
								\
	__i = (i);						\
	inode_check_scale(__i, __i->i_nlink, __i->i_nlink + 1);	\
	inc_nlink(__i);						\
})

#define INODE_DROP_NLINK(i)					\
	({							\
	struct inode *__i;					\
								\
	__i = (i);						\
	inode_check_scale(__i, __i->i_nlink, __i->i_nlink - 1);	\
	drop_nlink(__i);					\
})

#define INODE_CLEAR_NLINK(i)					\
	({							\
	struct inode *__i;					\
								\
	__i = (i);						\
	inode_check_scale(__i, __i->i_nlink, 0);		\
	clear_nlink(__i);					\
})


static inline void inode_add_blocks(struct inode *inode, __u64 blocks)
{
	inode_add_bytes(inode, blocks << inode->i_blkbits);
}

static inline void inode_sub_blocks(struct inode *inode, __u64 blocks)
{
	inode_sub_bytes(inode, blocks << inode->i_blkbits);
}


/* See comment before reiser4_readdir_common() for description. */
static inline struct list_head *get_readdir_list(const struct inode *inode)
{
	return &reiser4_inode_data(inode)->lists.readdir_list;
}

extern void init_inode_ordering(struct inode *inode,
				reiser4_object_create_data * crd, int create);

static inline struct radix_tree_root *jnode_tree_by_inode(struct inode *inode)
{
	return &reiser4_inode_data(inode)->jnodes_tree;
}

static inline struct radix_tree_root *jnode_tree_by_reiser4_inode(reiser4_inode
								  *r4_inode)
{
	return &r4_inode->jnodes_tree;
}

#if REISER4_DEBUG
extern void print_inode(const char *prefix, const struct inode *i);
#endif

int is_dir_empty(const struct inode *);

/* __REISER4_INODE_H__ */
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
