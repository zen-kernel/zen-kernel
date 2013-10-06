/* Copyright 2001, 2002, 2003, 2004 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Super-block functions. See super.c for details. */

#if !defined(__REISER4_SUPER_H__)
#define __REISER4_SUPER_H__

#include <linux/exportfs.h>

#include "tree.h"
#include "entd.h"
#include "wander.h"
#include "fsdata.h"
#include "plugin/object.h"
#include "plugin/space/space_allocator.h"

/*
 * Flush algorithms parameters.
 */
struct flush_params {
	unsigned relocate_threshold;
	unsigned relocate_distance;
	unsigned written_threshold;
	unsigned scan_maxnodes;
};

typedef enum {
	/*
	 * True if this file system doesn't support hard-links (multiple names)
	 * for directories: this is default UNIX behavior.
	 *
	 * If hard-links on directoires are not allowed, file system is Acyclic
	 * Directed Graph (modulo dot, and dotdot, of course).
	 *
	 * This is used by reiser4_link().
	 */
	REISER4_ADG = 0,
	/*
	 * set if all nodes in internal tree have the same node layout plugin.
	 * If so, znode_guess_plugin() will return tree->node_plugin in stead
	 * of guessing plugin by plugin id stored in the node.
	 */
	REISER4_ONE_NODE_PLUGIN = 1,
	/* if set, bsd gid assignment is supported. */
	REISER4_BSD_GID = 2,
	/* [mac]_time are 32 bit in inode */
	REISER4_32_BIT_TIMES = 3,
	/* load all bitmap blocks at mount time */
	REISER4_DONT_LOAD_BITMAP = 5,
	/* enforce atomicity during write(2) */
	REISER4_ATOMIC_WRITE = 6,
	/* don't use write barriers in the log writer code. */
	REISER4_NO_WRITE_BARRIER = 7
} reiser4_fs_flag;

/*
 * VFS related operation vectors.
 */
struct object_ops {
	struct super_operations super;
	struct dentry_operations dentry;
	struct export_operations export;
};

/* reiser4-specific part of super block

   Locking

   Fields immutable after mount:

    ->oid*
    ->space*
    ->default_[ug]id
    ->mkfs_id
    ->trace_flags
    ->debug_flags
    ->fs_flags
    ->df_plug
    ->optimal_io_size
    ->plug
    ->flush
    ->u (bad name)
    ->txnmgr
    ->ra_params
    ->fsuid
    ->journal_header
    ->journal_footer

   Fields protected by ->lnode_guard

    ->lnode_htable

   Fields protected by per-super block spin lock

    ->block_count
    ->blocks_used
    ->blocks_free
    ->blocks_free_committed
    ->blocks_grabbed
    ->blocks_fake_allocated_unformatted
    ->blocks_fake_allocated
    ->blocks_flush_reserved
    ->eflushed
    ->blocknr_hint_default

   After journal replaying during mount,

    ->last_committed_tx

   is protected by ->tmgr.commit_mutex

   Invariants involving this data-type:

      [sb-block-counts]
      [sb-grabbed]
      [sb-fake-allocated]
*/
struct reiser4_super_info_data {
	/*
	 * guard spinlock which protects reiser4 super block fields (currently
	 * blocks_free, blocks_free_committed)
	 */
	spinlock_t guard;

	/* next oid that will be returned by oid_allocate() */
	oid_t next_to_use;
	/* total number of used oids */
	oid_t oids_in_use;

	/* space manager plugin */
	reiser4_space_allocator space_allocator;

	/* reiser4 internal tree */
	reiser4_tree tree;

	/*
	 * default user id used for light-weight files without their own
	 * stat-data.
	 */
	uid_t default_uid;

	/*
	 * default group id used for light-weight files without their own
	 * stat-data.
	 */
	gid_t default_gid;

	/* mkfs identifier generated at mkfs time. */
	__u32 mkfs_id;
	/* amount of blocks in a file system */
	__u64 block_count;

	/* inviolable reserve */
	__u64 blocks_reserved;

	/* amount of blocks used by file system data and meta-data. */
	__u64 blocks_used;

	/*
	 * amount of free blocks. This is "working" free blocks counter. It is
	 * like "working" bitmap, please see block_alloc.c for description.
	 */
	__u64 blocks_free;

	/*
	 * free block count for fs committed state. This is "commit" version of
	 * free block counter.
	 */
	__u64 blocks_free_committed;

	/*
	 * number of blocks reserved for further allocation, for all
	 * threads.
	 */
	__u64 blocks_grabbed;

	/* number of fake allocated unformatted blocks in tree. */
	__u64 blocks_fake_allocated_unformatted;

	/* number of fake allocated formatted blocks in tree. */
	__u64 blocks_fake_allocated;

	/* number of blocks reserved for flush operations. */
	__u64 blocks_flush_reserved;

	/* number of blocks reserved for cluster operations. */
	__u64 blocks_clustered;

	/* unique file-system identifier */
	__u32 fsuid;

	/* On-disk format version. If does not equal to the disk_format
	   plugin version, some format updates (e.g. enlarging plugin
	   set, etc) may have place on mount. */
	int version;

	/* file-system wide flags. See reiser4_fs_flag enum */
	unsigned long fs_flags;

	/* transaction manager */
	txn_mgr tmgr;

	/* ent thread */
	entd_context entd;

	/* fake inode used to bind formatted nodes */
	struct inode *fake;
	/* inode used to bind bitmaps (and journal heads) */
	struct inode *bitmap;
	/* inode used to bind copied on capture nodes */
	struct inode *cc;

	/* disk layout plugin */
	disk_format_plugin *df_plug;

	/* disk layout specific part of reiser4 super info data */
	union {
		format40_super_info format40;
	} u;

	/* value we return in st_blksize on stat(2) */
	unsigned long optimal_io_size;

	/* parameters for the flush algorithm */
	struct flush_params flush;

	/* pointers to jnodes for journal header and footer */
	jnode *journal_header;
	jnode *journal_footer;

	journal_location jloc;

	/* head block number of last committed transaction */
	__u64 last_committed_tx;

	/*
	 * we remember last written location for using as a hint for new block
	 * allocation
	 */
	__u64 blocknr_hint_default;

	/* committed number of files (oid allocator state variable ) */
	__u64 nr_files_committed;

	struct formatted_ra_params ra_params;

	/*
	 * A mutex for serializing cut tree operation if out-of-free-space:
	 * the only one cut_tree thread is allowed to grab space from reserved
	 * area (it is 5% of disk space)
	 */
	struct mutex delete_mutex;
	/* task owning ->delete_mutex */
	struct task_struct *delete_mutex_owner;

	/* Diskmap's blocknumber */
	__u64 diskmap_block;

	/* What to do in case of error */
	int onerror;

	/* operations for objects on this file system */
	struct object_ops ops;

	/*
	 * structure to maintain d_cursors. See plugin/file_ops_readdir.c for
	 * more details
	 */
	struct d_cursor_info d_info;

#ifdef CONFIG_REISER4_BADBLOCKS
	/* Alternative master superblock offset (in bytes) */
	unsigned long altsuper;
#endif
	struct repacker *repacker;
	struct page *status_page;
	struct bio *status_bio;

#if REISER4_DEBUG
	/*
	 * minimum used blocks value (includes super blocks, bitmap blocks and
	 * other fs reserved areas), depends on fs format and fs size.
	 */
	__u64 min_blocks_used;

	/*
	 * when debugging is on, all jnodes (including znodes, bitmaps, etc.)
	 * are kept on a list anchored at sbinfo->all_jnodes. This list is
	 * protected by sbinfo->all_guard spin lock. This lock should be taken
	 * with _irq modifier, because it is also modified from interrupt
	 * contexts (by RCU).
	 */
	spinlock_t all_guard;
	/* list of all jnodes */
	struct list_head all_jnodes;
#endif
	struct dentry *debugfs_root;
};

extern reiser4_super_info_data *get_super_private_nocheck(const struct
							  super_block * super);

/* Return reiser4-specific part of super block */
static inline reiser4_super_info_data *get_super_private(const struct
							 super_block * super)
{
	assert("nikita-447", super != NULL);

	return (reiser4_super_info_data *) super->s_fs_info;
}

/* get ent context for the @super */
static inline entd_context *get_entd_context(struct super_block *super)
{
	return &get_super_private(super)->entd;
}

/* "Current" super-block: main super block used during current system
   call. Reference to this super block is stored in reiser4_context. */
static inline struct super_block *reiser4_get_current_sb(void)
{
	return get_current_context()->super;
}

/* Reiser4-specific part of "current" super-block: main super block used
   during current system call. Reference to this super block is stored in
   reiser4_context. */
static inline reiser4_super_info_data *get_current_super_private(void)
{
	return get_super_private(reiser4_get_current_sb());
}

static inline struct formatted_ra_params *get_current_super_ra_params(void)
{
	return &(get_current_super_private()->ra_params);
}

/*
 * true, if file system on @super is read-only
 */
static inline int rofs_super(struct super_block *super)
{
	return super->s_flags & MS_RDONLY;
}

/*
 * true, if @tree represents read-only file system
 */
static inline int rofs_tree(reiser4_tree * tree)
{
	return rofs_super(tree->super);
}

/*
 * true, if file system where @inode lives on, is read-only
 */
static inline int rofs_inode(struct inode *inode)
{
	return rofs_super(inode->i_sb);
}

/*
 * true, if file system where @node lives on, is read-only
 */
static inline int rofs_jnode(jnode * node)
{
	return rofs_tree(jnode_get_tree(node));
}

extern __u64 reiser4_current_block_count(void);

extern void build_object_ops(struct super_block *super, struct object_ops *ops);

#define REISER4_SUPER_MAGIC 0x52345362	/* (*(__u32 *)"R4Sb"); */

static inline void spin_lock_reiser4_super(reiser4_super_info_data *sbinfo)
{
	spin_lock(&(sbinfo->guard));
}

static inline void spin_unlock_reiser4_super(reiser4_super_info_data *sbinfo)
{
	assert_spin_locked(&(sbinfo->guard));
	spin_unlock(&(sbinfo->guard));
}

extern __u64 reiser4_flush_reserved(const struct super_block *);
extern int reiser4_is_set(const struct super_block *super, reiser4_fs_flag f);
extern long reiser4_statfs_type(const struct super_block *super);
extern __u64 reiser4_block_count(const struct super_block *super);
extern void reiser4_set_block_count(const struct super_block *super, __u64 nr);
extern __u64 reiser4_data_blocks(const struct super_block *super);
extern void reiser4_set_data_blocks(const struct super_block *super, __u64 nr);
extern __u64 reiser4_free_blocks(const struct super_block *super);
extern void reiser4_set_free_blocks(const struct super_block *super, __u64 nr);
extern __u32 reiser4_mkfs_id(const struct super_block *super);

extern __u64 reiser4_free_committed_blocks(const struct super_block *super);

extern __u64 reiser4_grabbed_blocks(const struct super_block *);
extern __u64 reiser4_fake_allocated(const struct super_block *);
extern __u64 reiser4_fake_allocated_unformatted(const struct super_block *);
extern __u64 reiser4_clustered_blocks(const struct super_block *);

extern long reiser4_reserved_blocks(const struct super_block *super, uid_t uid,
				    gid_t gid);

extern reiser4_space_allocator *
reiser4_get_space_allocator(const struct super_block *super);
extern reiser4_oid_allocator *
reiser4_get_oid_allocator(const struct super_block *super);
extern struct inode *reiser4_get_super_fake(const struct super_block *super);
extern struct inode *reiser4_get_cc_fake(const struct super_block *super);
extern struct inode *reiser4_get_bitmap_fake(const struct super_block *super);
extern reiser4_tree *reiser4_get_tree(const struct super_block *super);
extern int is_reiser4_super(const struct super_block *super);

extern int reiser4_blocknr_is_sane(const reiser4_block_nr * blk);
extern int reiser4_blocknr_is_sane_for(const struct super_block *super,
				       const reiser4_block_nr * blk);
extern int reiser4_fill_super(struct super_block *s, void *data, int silent);
extern int reiser4_done_super(struct super_block *s);

/* step of fill super */
extern int reiser4_init_fs_info(struct super_block *);
extern void reiser4_done_fs_info(struct super_block *);
extern int reiser4_init_super_data(struct super_block *, char *opt_string);
extern int reiser4_init_read_super(struct super_block *, int silent);
extern int reiser4_init_root_inode(struct super_block *);
extern reiser4_plugin *get_default_plugin(pset_member memb);

/* Maximal possible object id. */
#define  ABSOLUTE_MAX_OID ((oid_t)~0)

#define OIDS_RESERVED  (1 << 16)
int oid_init_allocator(struct super_block *, oid_t nr_files, oid_t next);
oid_t oid_allocate(struct super_block *);
int oid_release(struct super_block *, oid_t);
oid_t oid_next(const struct super_block *);
void oid_count_allocated(void);
void oid_count_released(void);
long oids_used(const struct super_block *);

#if REISER4_DEBUG
void print_fs_info(const char *prefix, const struct super_block *);
#endif

extern void destroy_reiser4_cache(struct kmem_cache **);

extern struct super_operations reiser4_super_operations;
extern struct export_operations reiser4_export_operations;
extern struct dentry_operations reiser4_dentry_operations;

/* __REISER4_SUPER_H__ */
#endif

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 120
 * End:
 */
