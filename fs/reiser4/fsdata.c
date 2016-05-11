/* Copyright 2001, 2002, 2003, 2004, 2005 by Hans Reiser, licensing governed by
 * reiser4/README */

#include "fsdata.h"
#include "inode.h"

#include <linux/shrinker.h>

/* cache or dir_cursors */
static struct kmem_cache *d_cursor_cache;

/* list of unused cursors */
static LIST_HEAD(cursor_cache);

/* number of cursors in list of ununsed cursors */
static unsigned long d_cursor_unused = 0;

/* spinlock protecting manipulations with dir_cursor's hash table and lists */
DEFINE_SPINLOCK(d_c_lock);

static reiser4_file_fsdata *create_fsdata(struct file *file);
static int file_is_stateless(struct file *file);
static void free_fsdata(reiser4_file_fsdata *fsdata);
static void kill_cursor(dir_cursor *);

static unsigned long d_cursor_shrink_scan(struct shrinker *shrink,
					  struct shrink_control *sc)
{
	dir_cursor *scan;
	unsigned long freed = 0;

	spin_lock(&d_c_lock);
	while (!list_empty(&cursor_cache) && sc->nr_to_scan) {
		scan = list_entry(cursor_cache.next, dir_cursor, alist);
		assert("nikita-3567", scan->ref == 0);
		kill_cursor(scan);
		freed++;
		sc->nr_to_scan--;
	}
	spin_unlock(&d_c_lock);
	return freed;
}

static unsigned long d_cursor_shrink_count (struct shrinker *shrink,
					    struct shrink_control *sc)
{
	return d_cursor_unused;
}

/*
 * actually, d_cursors are "priceless", because there is no way to
 * recover information stored in them. On the other hand, we don't
 * want to consume all kernel memory by them. As a compromise, just
 * assign higher "seeks" value to d_cursor cache, so that it will be
 * shrunk only if system is really tight on memory.
 */
static struct shrinker d_cursor_shrinker = {
	.count_objects = d_cursor_shrink_count,
	.scan_objects = d_cursor_shrink_scan,
	.seeks = DEFAULT_SEEKS << 3
};

/**
 * reiser4_init_d_cursor - create d_cursor cache
 *
 * Initializes slab cache of d_cursors. It is part of reiser4 module
 * initialization.
 */
int reiser4_init_d_cursor(void)
{
	d_cursor_cache = kmem_cache_create("d_cursor", sizeof(dir_cursor), 0,
					   SLAB_HWCACHE_ALIGN, NULL);
	if (d_cursor_cache == NULL)
		return RETERR(-ENOMEM);

	register_shrinker(&d_cursor_shrinker);
	return 0;
}

/**
 * reiser4_done_d_cursor - delete d_cursor cache and d_cursor shrinker
 *
 * This is called on reiser4 module unloading or system shutdown.
 */
void reiser4_done_d_cursor(void)
{
	unregister_shrinker(&d_cursor_shrinker);

	destroy_reiser4_cache(&d_cursor_cache);
}

#define D_CURSOR_TABLE_SIZE (256)

static inline unsigned long
d_cursor_hash(d_cursor_hash_table * table, const struct d_cursor_key *key)
{
	assert("nikita-3555", IS_POW(D_CURSOR_TABLE_SIZE));
	return (key->oid + key->cid) & (D_CURSOR_TABLE_SIZE - 1);
}

static inline int d_cursor_eq(const struct d_cursor_key *k1,
			      const struct d_cursor_key *k2)
{
	return k1->cid == k2->cid && k1->oid == k2->oid;
}

/*
 * define functions to manipulate reiser4 super block's hash table of
 * dir_cursors
 */
#define KMALLOC(size) kmalloc((size), reiser4_ctx_gfp_mask_get())
#define KFREE(ptr, size) kfree(ptr)
TYPE_SAFE_HASH_DEFINE(d_cursor,
		      dir_cursor,
		      struct d_cursor_key,
		      key, hash, d_cursor_hash, d_cursor_eq);
#undef KFREE
#undef KMALLOC

/**
 * reiser4_init_super_d_info - initialize per-super-block d_cursor resources
 * @super: super block to initialize
 *
 * Initializes per-super-block d_cursor's hash table and radix tree. It is part
 * of mount.
 */
int reiser4_init_super_d_info(struct super_block *super)
{
	struct d_cursor_info *p;

	p = &get_super_private(super)->d_info;

	INIT_RADIX_TREE(&p->tree, reiser4_ctx_gfp_mask_get());
	return d_cursor_hash_init(&p->table, D_CURSOR_TABLE_SIZE);
}

/**
 * reiser4_done_super_d_info - release per-super-block d_cursor resources
 * @super: super block being umounted
 *
 * It is called on umount. Kills all directory cursors attached to suoer block.
 */
void reiser4_done_super_d_info(struct super_block *super)
{
	struct d_cursor_info *d_info;
	dir_cursor *cursor, *next;

	d_info = &get_super_private(super)->d_info;
	for_all_in_htable(&d_info->table, d_cursor, cursor, next)
		kill_cursor(cursor);

	BUG_ON(d_info->tree.rnode != NULL);
	d_cursor_hash_done(&d_info->table);
}

/**
 * kill_cursor - free dir_cursor and reiser4_file_fsdata attached to it
 * @cursor: cursor to free
 *
 * Removes reiser4_file_fsdata attached to @cursor from readdir list of
 * reiser4_inode, frees that reiser4_file_fsdata. Removes @cursor from from
 * indices, hash table, list of unused cursors and frees it.
 */
static void kill_cursor(dir_cursor *cursor)
{
	unsigned long index;

	assert("nikita-3566", cursor->ref == 0);
	assert("nikita-3572", cursor->fsdata != NULL);

	index = (unsigned long)cursor->key.oid;
	list_del_init(&cursor->fsdata->dir.linkage);
	free_fsdata(cursor->fsdata);
	cursor->fsdata = NULL;

	if (list_empty_careful(&cursor->list))
		/* this is last cursor for a file. Kill radix-tree entry */
		radix_tree_delete(&cursor->info->tree, index);
	else {
		void **slot;

		/*
		 * there are other cursors for the same oid.
		 */

		/*
		 * if radix tree point to the cursor being removed, re-target
		 * radix tree slot to the next cursor in the (non-empty as was
		 * checked above) element of the circular list of all cursors
		 * for this oid.
		 */
		slot = radix_tree_lookup_slot(&cursor->info->tree, index);
		assert("nikita-3571", *slot != NULL);
		if (*slot == cursor)
			*slot = list_entry(cursor->list.next, dir_cursor, list);
		/* remove cursor from circular list */
		list_del_init(&cursor->list);
	}
	/* remove cursor from the list of unused cursors */
	list_del_init(&cursor->alist);
	/* remove cursor from the hash table */
	d_cursor_hash_remove(&cursor->info->table, cursor);
	/* and free it */
	kmem_cache_free(d_cursor_cache, cursor);
	--d_cursor_unused;
}

/* possible actions that can be performed on all cursors for the given file */
enum cursor_action {
	/*
	 * load all detached state: this is called when stat-data is loaded
	 * from the disk to recover information about all pending readdirs
	 */
	CURSOR_LOAD,
	/*
	 * detach all state from inode, leaving it in the cache. This is called
	 * when inode is removed form the memory by memory pressure
	 */
	CURSOR_DISPOSE,
	/*
	 * detach cursors from the inode, and free them. This is called when
	 * inode is destroyed
	 */
	CURSOR_KILL
};

/*
 * return d_cursor data for the file system @inode is in.
 */
static inline struct d_cursor_info *d_info(struct inode *inode)
{
	return &get_super_private(inode->i_sb)->d_info;
}

/*
 * lookup d_cursor in the per-super-block radix tree.
 */
static inline dir_cursor *lookup(struct d_cursor_info *info,
				 unsigned long index)
{
	return (dir_cursor *) radix_tree_lookup(&info->tree, index);
}

/*
 * attach @cursor to the radix tree. There may be multiple cursors for the
 * same oid, they are chained into circular list.
 */
static void bind_cursor(dir_cursor * cursor, unsigned long index)
{
	dir_cursor *head;

	head = lookup(cursor->info, index);
	if (head == NULL) {
		/* this is the first cursor for this index */
		INIT_LIST_HEAD(&cursor->list);
		radix_tree_insert(&cursor->info->tree, index, cursor);
	} else {
		/* some cursor already exists. Chain ours */
		list_add(&cursor->list, &head->list);
	}
}

/*
 * detach fsdata (if detachable) from file descriptor, and put cursor on the
 * "unused" list. Called when file descriptor is not longer in active use.
 */
static void clean_fsdata(struct file *file)
{
	dir_cursor *cursor;
	reiser4_file_fsdata *fsdata;

	assert("nikita-3570", file_is_stateless(file));

	fsdata = (reiser4_file_fsdata *) file->private_data;
	if (fsdata != NULL) {
		cursor = fsdata->cursor;
		if (cursor != NULL) {
			spin_lock(&d_c_lock);
			--cursor->ref;
			if (cursor->ref == 0) {
				list_add_tail(&cursor->alist, &cursor_cache);
				++d_cursor_unused;
			}
			spin_unlock(&d_c_lock);
			file->private_data = NULL;
		}
	}
}

/*
 * global counter used to generate "client ids". These ids are encoded into
 * high bits of fpos.
 */
static __u32 cid_counter = 0;
#define CID_SHIFT (20)
#define CID_MASK  (0xfffffull)

static void free_file_fsdata_nolock(struct file *);

/**
 * insert_cursor - allocate file_fsdata, insert cursor to tree and hash table
 * @cursor:
 * @file:
 * @inode:
 *
 * Allocates reiser4_file_fsdata, attaches it to @cursor, inserts cursor to
 * reiser4 super block's hash table and radix tree.
 add detachable readdir
 * state to the @f
 */
static int insert_cursor(dir_cursor *cursor, struct file *file, loff_t *fpos,
			 struct inode *inode)
{
	int result;
	reiser4_file_fsdata *fsdata;

	memset(cursor, 0, sizeof *cursor);

	/* this is either first call to readdir, or rewind. Anyway, create new
	 * cursor. */
	fsdata = create_fsdata(NULL);
	if (fsdata != NULL) {
		result = radix_tree_preload(reiser4_ctx_gfp_mask_get());
		if (result == 0) {
			struct d_cursor_info *info;
			oid_t oid;

			info = d_info(inode);
			oid = get_inode_oid(inode);
			/* cid occupies higher 12 bits of f->f_pos. Don't
			 * allow it to become negative: this confuses
			 * nfsd_readdir() */
			cursor->key.cid = (++cid_counter) & 0x7ff;
			cursor->key.oid = oid;
			cursor->fsdata = fsdata;
			cursor->info = info;
			cursor->ref = 1;

			spin_lock_inode(inode);
			/* install cursor as @f's private_data, discarding old
			 * one if necessary */
#if REISER4_DEBUG
			if (file->private_data)
				warning("", "file has fsdata already");
#endif
			clean_fsdata(file);
			free_file_fsdata_nolock(file);
			file->private_data = fsdata;
			fsdata->cursor = cursor;
			spin_unlock_inode(inode);
			spin_lock(&d_c_lock);
			/* insert cursor into hash table */
			d_cursor_hash_insert(&info->table, cursor);
			/* and chain it into radix-tree */
			bind_cursor(cursor, (unsigned long)oid);
			spin_unlock(&d_c_lock);
			radix_tree_preload_end();
			*fpos = ((__u64) cursor->key.cid) << CID_SHIFT;
		}
	} else
		result = RETERR(-ENOMEM);
	return result;
}

/**
 * process_cursors - do action on each cursor attached to inode
 * @inode:
 * @act: action to do
 *
 * Finds all cursors of @inode in reiser4's super block radix tree of cursors
 * and performs action specified by @act on each of cursors.
 */
static void process_cursors(struct inode *inode, enum cursor_action act)
{
	oid_t oid;
	dir_cursor *start;
	struct list_head *head;
	reiser4_context *ctx;
	struct d_cursor_info *info;

	/* this can be called by
	 *
	 * kswapd->...->prune_icache->..reiser4_destroy_inode
	 *
	 * without reiser4_context
	 */
	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx)) {
		warning("vs-23", "failed to init context");
		return;
	}

	assert("nikita-3558", inode != NULL);

	info = d_info(inode);
	oid = get_inode_oid(inode);
	spin_lock_inode(inode);
	head = get_readdir_list(inode);
	spin_lock(&d_c_lock);
	/* find any cursor for this oid: reference to it is hanging of radix
	 * tree */
	start = lookup(info, (unsigned long)oid);
	if (start != NULL) {
		dir_cursor *scan;
		reiser4_file_fsdata *fsdata;

		/* process circular list of cursors for this oid */
		scan = start;
		do {
			dir_cursor *next;

			next = list_entry(scan->list.next, dir_cursor, list);
			fsdata = scan->fsdata;
			assert("nikita-3557", fsdata != NULL);
			if (scan->key.oid == oid) {
				switch (act) {
				case CURSOR_DISPOSE:
					list_del_init(&fsdata->dir.linkage);
					break;
				case CURSOR_LOAD:
					list_add(&fsdata->dir.linkage, head);
					break;
				case CURSOR_KILL:
					kill_cursor(scan);
					break;
				}
			}
			if (scan == next)
				/* last cursor was just killed */
				break;
			scan = next;
		} while (scan != start);
	}
	spin_unlock(&d_c_lock);
	/* check that we killed 'em all */
	assert("nikita-3568",
	       ergo(act == CURSOR_KILL,
		    list_empty_careful(get_readdir_list(inode))));
	assert("nikita-3569",
	       ergo(act == CURSOR_KILL, lookup(info, oid) == NULL));
	spin_unlock_inode(inode);
	reiser4_exit_context(ctx);
}

/**
 * reiser4_dispose_cursors - removes cursors from inode's list
 * @inode: inode to dispose cursors of
 *
 * For each of cursors corresponding to @inode - removes reiser4_file_fsdata
 * attached to cursor from inode's readdir list. This is called when inode is
 * removed from the memory by memory pressure.
 */
void reiser4_dispose_cursors(struct inode *inode)
{
	process_cursors(inode, CURSOR_DISPOSE);
}

/**
 * reiser4_load_cursors - attach cursors to inode
 * @inode: inode to load cursors to
 *
 * For each of cursors corresponding to @inode - attaches reiser4_file_fsdata
 * attached to cursor to inode's readdir list. This is done when inode is
 * loaded into memory.
 */
void reiser4_load_cursors(struct inode *inode)
{
	process_cursors(inode, CURSOR_LOAD);
}

/**
 * reiser4_kill_cursors - kill all inode cursors
 * @inode: inode to kill cursors of
 *
 * Frees all cursors for this inode. This is called when inode is destroyed.
 */
void reiser4_kill_cursors(struct inode *inode)
{
	process_cursors(inode, CURSOR_KILL);
}

/**
 * file_is_stateless -
 * @file:
 *
 * true, if file descriptor @f is created by NFS server by "demand" to serve
 * one file system operation. This means that there may be "detached state"
 * for underlying inode.
 */
static int file_is_stateless(struct file *file)
{
	return reiser4_get_dentry_fsdata(file->f_path.dentry)->stateless;
}

/**
 * reiser4_get_dir_fpos -
 * @dir:
 * @fpos: effective value of dir->f_pos
 *
 * Calculates ->fpos from user-supplied cookie. Normally it is dir->f_pos, but
 * in the case of stateless directory operation (readdir-over-nfs), client id
 * was encoded in the high bits of cookie and should me masked off.
 */
loff_t reiser4_get_dir_fpos(struct file *dir, loff_t fpos)
{
	if (file_is_stateless(dir))
		return fpos & CID_MASK;
	else
		return fpos;
}

/**
 * reiser4_attach_fsdata - try to attach fsdata
 * @file:
 * @fpos: effective value of @file->f_pos
 * @inode:
 *
 * Finds or creates cursor for readdir-over-nfs.
 */
int reiser4_attach_fsdata(struct file *file, loff_t *fpos, struct inode *inode)
{
	loff_t pos;
	int result;
	dir_cursor *cursor;

	/*
	 * we are serialized by inode->i_mutex
	 */
	if (!file_is_stateless(file))
		return 0;

	pos = *fpos;
	result = 0;
	if (pos == 0) {
		/*
		 * first call to readdir (or rewind to the beginning of
		 * directory)
		 */
		cursor = kmem_cache_alloc(d_cursor_cache,
					  reiser4_ctx_gfp_mask_get());
		if (cursor != NULL)
			result = insert_cursor(cursor, file, fpos, inode);
		else
			result = RETERR(-ENOMEM);
	} else {
		/* try to find existing cursor */
		struct d_cursor_key key;

		key.cid = pos >> CID_SHIFT;
		key.oid = get_inode_oid(inode);
		spin_lock(&d_c_lock);
		cursor = d_cursor_hash_find(&d_info(inode)->table, &key);
		if (cursor != NULL) {
			/* cursor was found */
			if (cursor->ref == 0) {
				/* move it from unused list */
				list_del_init(&cursor->alist);
				--d_cursor_unused;
			}
			++cursor->ref;
		}
		spin_unlock(&d_c_lock);
		if (cursor != NULL) {
			spin_lock_inode(inode);
			assert("nikita-3556", cursor->fsdata->back == NULL);
			clean_fsdata(file);
			free_file_fsdata_nolock(file);
			file->private_data = cursor->fsdata;
			spin_unlock_inode(inode);
		}
	}
	return result;
}

/**
 * reiser4_detach_fsdata - ???
 * @file:
 *
 * detach fsdata, if necessary
 */
void reiser4_detach_fsdata(struct file *file)
{
	struct inode *inode;

	if (!file_is_stateless(file))
		return;

	inode = file_inode(file);
	spin_lock_inode(inode);
	clean_fsdata(file);
	spin_unlock_inode(inode);
}

/* slab for reiser4_dentry_fsdata */
static struct kmem_cache *dentry_fsdata_cache;

/**
 * reiser4_init_dentry_fsdata - create cache of dentry_fsdata
 *
 * Initializes slab cache of structures attached to denty->d_fsdata. It is
 * part of reiser4 module initialization.
 */
int reiser4_init_dentry_fsdata(void)
{
	dentry_fsdata_cache = kmem_cache_create("dentry_fsdata",
					   sizeof(struct reiser4_dentry_fsdata),
					   0,
					   SLAB_HWCACHE_ALIGN |
					   SLAB_RECLAIM_ACCOUNT,
					   NULL);
	if (dentry_fsdata_cache == NULL)
		return RETERR(-ENOMEM);
	return 0;
}

/**
 * reiser4_done_dentry_fsdata - delete cache of dentry_fsdata
 *
 * This is called on reiser4 module unloading or system shutdown.
 */
void reiser4_done_dentry_fsdata(void)
{
	destroy_reiser4_cache(&dentry_fsdata_cache);
}

/**
 * reiser4_get_dentry_fsdata - get fs-specific dentry data
 * @dentry: queried dentry
 *
 * Allocates if necessary and returns per-dentry data that we attach to each
 * dentry.
 */
struct reiser4_dentry_fsdata *reiser4_get_dentry_fsdata(struct dentry *dentry)
{
	assert("nikita-1365", dentry != NULL);

	if (dentry->d_fsdata == NULL) {
		dentry->d_fsdata = kmem_cache_alloc(dentry_fsdata_cache,
						    reiser4_ctx_gfp_mask_get());
		if (dentry->d_fsdata == NULL)
			return ERR_PTR(RETERR(-ENOMEM));
		memset(dentry->d_fsdata, 0,
		       sizeof(struct reiser4_dentry_fsdata));
	}
	return dentry->d_fsdata;
}

/**
 * reiser4_free_dentry_fsdata - detach and free dentry_fsdata
 * @dentry: dentry to free fsdata of
 *
 * Detaches and frees fs-specific dentry data
 */
void reiser4_free_dentry_fsdata(struct dentry *dentry)
{
	if (dentry->d_fsdata != NULL) {
		kmem_cache_free(dentry_fsdata_cache, dentry->d_fsdata);
		dentry->d_fsdata = NULL;
	}
}

/* slab for reiser4_file_fsdata */
static struct kmem_cache *file_fsdata_cache;

/**
 * reiser4_init_file_fsdata - create cache of reiser4_file_fsdata
 *
 * Initializes slab cache of structures attached to file->private_data. It is
 * part of reiser4 module initialization.
 */
int reiser4_init_file_fsdata(void)
{
	file_fsdata_cache = kmem_cache_create("file_fsdata",
					      sizeof(reiser4_file_fsdata),
					      0,
					      SLAB_HWCACHE_ALIGN |
					      SLAB_RECLAIM_ACCOUNT, NULL);
	if (file_fsdata_cache == NULL)
		return RETERR(-ENOMEM);
	return 0;
}

/**
 * reiser4_done_file_fsdata - delete cache of reiser4_file_fsdata
 *
 * This is called on reiser4 module unloading or system shutdown.
 */
void reiser4_done_file_fsdata(void)
{
	destroy_reiser4_cache(&file_fsdata_cache);
}

/**
 * create_fsdata - allocate and initialize reiser4_file_fsdata
 * @file: what to create file_fsdata for, may be NULL
 *
 * Allocates and initializes reiser4_file_fsdata structure.
 */
static reiser4_file_fsdata *create_fsdata(struct file *file)
{
	reiser4_file_fsdata *fsdata;

	fsdata = kmem_cache_alloc(file_fsdata_cache,
				  reiser4_ctx_gfp_mask_get());
	if (fsdata != NULL) {
		memset(fsdata, 0, sizeof *fsdata);
		fsdata->back = file;
		INIT_LIST_HEAD(&fsdata->dir.linkage);
	}
	return fsdata;
}

/**
 * free_fsdata - free reiser4_file_fsdata
 * @fsdata: object to free
 *
 * Dual to create_fsdata(). Free reiser4_file_fsdata.
 */
static void free_fsdata(reiser4_file_fsdata *fsdata)
{
	BUG_ON(fsdata == NULL);
	kmem_cache_free(file_fsdata_cache, fsdata);
}

/**
 * reiser4_get_file_fsdata - get fs-specific file data
 * @file: queried file
 *
 * Returns fs-specific data of @file. If it is NULL, allocates it and attaches
 * to @file.
 */
reiser4_file_fsdata *reiser4_get_file_fsdata(struct file *file)
{
	assert("nikita-1603", file != NULL);

	if (file->private_data == NULL) {
		reiser4_file_fsdata *fsdata;
		struct inode *inode;

		fsdata = create_fsdata(file);
		if (fsdata == NULL)
			return ERR_PTR(RETERR(-ENOMEM));

		inode = file_inode(file);
		spin_lock_inode(inode);
		if (file->private_data == NULL) {
			file->private_data = fsdata;
			fsdata = NULL;
		}
		spin_unlock_inode(inode);
		if (fsdata != NULL)
			/* other thread initialized ->fsdata */
			kmem_cache_free(file_fsdata_cache, fsdata);
	}
	assert("nikita-2665", file->private_data != NULL);
	return file->private_data;
}

/**
 * free_file_fsdata_nolock - detach and free reiser4_file_fsdata
 * @file:
 *
 * Detaches reiser4_file_fsdata from @file, removes reiser4_file_fsdata from
 * readdir list, frees if it is not linked to d_cursor object.
 */
static void free_file_fsdata_nolock(struct file *file)
{
	reiser4_file_fsdata *fsdata;

	assert("", spin_inode_is_locked(file_inode(file)));
	fsdata = file->private_data;
	if (fsdata != NULL) {
		list_del_init(&fsdata->dir.linkage);
		if (fsdata->cursor == NULL)
			free_fsdata(fsdata);
	}
	file->private_data = NULL;
}

/**
 * reiser4_free_file_fsdata - detach from struct file and free reiser4_file_fsdata
 * @file:
 *
 * Spinlocks inode and calls free_file_fsdata_nolock to do the work.
 */
void reiser4_free_file_fsdata(struct file *file)
{
	spin_lock_inode(file_inode(file));
	free_file_fsdata_nolock(file);
	spin_unlock_inode(file_inode(file));
}

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 79
 * End:
 */
