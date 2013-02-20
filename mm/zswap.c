/*
 * zswap.c - zswap driver file
 *
 * zswap is a backend for frontswap that takes pages that are in the
 * process of being swapped out and attempts to compress them and store
 * them in a RAM-based memory pool.  This results in a significant I/O
 * reduction on the real swap device and, in the case of a slow swap
 * device, can also improve workload performance.
 *
 * Copyright (C) 2012  Seth Jennings <sjenning@linux.vnet.ibm.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
*/

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt

#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/atomic.h>
#include <linux/frontswap.h>
#include <linux/rbtree.h>
#include <linux/swap.h>
#include <linux/crypto.h>
#include <linux/mempool.h>
#include <linux/zsmalloc.h>

/*********************************
* statistics
**********************************/
/* Number of memory pages used by the compressed pool */
static atomic_t zswap_pool_pages = ATOMIC_INIT(0);
/* The number of compressed pages currently stored in zswap */
static atomic_t zswap_stored_pages = ATOMIC_INIT(0);

/*
 * The statistics below are not protected from concurrent access for
 * performance reasons so they may not be a 100% accurate.  However,
 * they do provide useful information on roughly how many times a
 * certain event is occurring.
*/
static u64 zswap_pool_limit_hit;
static u64 zswap_reject_compress_poor;
static u64 zswap_reject_zsmalloc_fail;
static u64 zswap_reject_kmemcache_fail;
static u64 zswap_duplicate_entry;

/*********************************
* tunables
**********************************/
/* Enable/disable zswap (disabled by default, fixed at boot for now) */
static bool zswap_enabled;
module_param_named(enabled, zswap_enabled, bool, 0);

/* Compressor to be used by zswap (fixed at boot for now) */
#define ZSWAP_COMPRESSOR_DEFAULT "lzo"
static char *zswap_compressor = ZSWAP_COMPRESSOR_DEFAULT;
module_param_named(compressor, zswap_compressor, charp, 0);

/* The maximum percentage of memory that the compressed pool can occupy */
static unsigned int zswap_max_pool_percent = 20;
module_param_named(max_pool_percent,
			zswap_max_pool_percent, uint, 0644);

/*
 * Maximum compression ratio, as as percentage, for an acceptable
 * compressed page. Any pages that do not compress by at least
 * this ratio will be rejected.
*/
static unsigned int zswap_max_compression_ratio = 80;
module_param_named(max_compression_ratio,
			zswap_max_compression_ratio, uint, 0644);

/*********************************
* compression functions
**********************************/
/* per-cpu compression transforms */
static struct crypto_comp * __percpu *zswap_comp_pcpu_tfms;

enum comp_op {
	ZSWAP_COMPOP_COMPRESS,
	ZSWAP_COMPOP_DECOMPRESS
};

static int zswap_comp_op(enum comp_op op, const u8 *src, unsigned int slen,
				u8 *dst, unsigned int *dlen)
{
	struct crypto_comp *tfm;
	int ret;

	tfm = *per_cpu_ptr(zswap_comp_pcpu_tfms, get_cpu());
	switch (op) {
	case ZSWAP_COMPOP_COMPRESS:
		ret = crypto_comp_compress(tfm, src, slen, dst, dlen);
		break;
	case ZSWAP_COMPOP_DECOMPRESS:
		ret = crypto_comp_decompress(tfm, src, slen, dst, dlen);
		break;
	default:
		ret = -EINVAL;
	}

	put_cpu();
	return ret;
}

static int __init zswap_comp_init(void)
{
	if (!crypto_has_comp(zswap_compressor, 0, 0)) {
		pr_info("%s compressor not available\n", zswap_compressor);
		/* fall back to default compressor */
		zswap_compressor = ZSWAP_COMPRESSOR_DEFAULT;
		if (!crypto_has_comp(zswap_compressor, 0, 0))
			/* can't even load the default compressor */
			return -ENODEV;
	}
	pr_info("using %s compressor\n", zswap_compressor);

	/* alloc percpu transforms */
	zswap_comp_pcpu_tfms = alloc_percpu(struct crypto_comp *);
	if (!zswap_comp_pcpu_tfms)
		return -ENOMEM;
	return 0;
}

static void zswap_comp_exit(void)
{
	/* free percpu transforms */
	if (zswap_comp_pcpu_tfms)
		free_percpu(zswap_comp_pcpu_tfms);
}

/*********************************
* data structures
**********************************/
struct zswap_entry {
	struct rb_node rbnode;
	unsigned type;
	pgoff_t offset;
	unsigned long handle;
	unsigned int length;
};

struct zswap_tree {
	struct rb_root rbroot;
	spinlock_t lock;
	struct zs_pool *pool;
};

static struct zswap_tree *zswap_trees[MAX_SWAPFILES];

/*********************************
* zswap entry functions
**********************************/
#define ZSWAP_KMEM_CACHE_NAME "zswap_entry_cache"
static struct kmem_cache *zswap_entry_cache;

static inline int zswap_entry_cache_create(void)
{
	zswap_entry_cache =
		kmem_cache_create(ZSWAP_KMEM_CACHE_NAME,
			sizeof(struct zswap_entry), 0, 0, NULL);
	return (zswap_entry_cache == NULL);
}

static inline void zswap_entry_cache_destory(void)
{
	kmem_cache_destroy(zswap_entry_cache);
}

static inline struct zswap_entry *zswap_entry_cache_alloc(gfp_t gfp)
{
	struct zswap_entry *entry;
	entry = kmem_cache_alloc(zswap_entry_cache, gfp);
	if (!entry)
		return NULL;
	return entry;
}

static inline void zswap_entry_cache_free(struct zswap_entry *entry)
{
	kmem_cache_free(zswap_entry_cache, entry);
}

/*********************************
* rbtree functions
**********************************/
static struct zswap_entry *zswap_rb_search(struct rb_root *root, pgoff_t offset)
{
	struct rb_node *node = root->rb_node;
	struct zswap_entry *entry;

	while (node) {
		entry = rb_entry(node, struct zswap_entry, rbnode);
		if (entry->offset > offset)
			node = node->rb_left;
		else if (entry->offset < offset)
			node = node->rb_right;
		else
			return entry;
	}
	return NULL;
}

/*
 * In the case that a entry with the same offset is found, it a pointer to
 * the existing entry is stored in dupentry and the function returns -EEXIST
*/
static int zswap_rb_insert(struct rb_root *root, struct zswap_entry *entry,
			struct zswap_entry **dupentry)
{
	struct rb_node **link = &root->rb_node, *parent = NULL;
	struct zswap_entry *myentry;

	while (*link) {
		parent = *link;
		myentry = rb_entry(parent, struct zswap_entry, rbnode);
		if (myentry->offset > entry->offset)
			link = &(*link)->rb_left;
		else if (myentry->offset < entry->offset)
			link = &(*link)->rb_right;
		else {
			*dupentry = myentry;
			return -EEXIST;
		}
	}
	rb_link_node(&entry->rbnode, parent, link);
	rb_insert_color(&entry->rbnode, root);
	return 0;
}

/*********************************
* per-cpu code
**********************************/
static DEFINE_PER_CPU(u8 *, zswap_dstmem);

static int __zswap_cpu_notifier(unsigned long action, unsigned long cpu)
{
	struct crypto_comp *tfm;
	u8 *dst;

	switch (action) {
	case CPU_UP_PREPARE:
		tfm = crypto_alloc_comp(zswap_compressor, 0, 0);
		if (IS_ERR(tfm)) {
			pr_err("can't allocate compressor transform\n");
			return NOTIFY_BAD;
		}
		*per_cpu_ptr(zswap_comp_pcpu_tfms, cpu) = tfm;
		dst = (u8 *)__get_free_pages(GFP_KERNEL, 1);
		if (!dst) {
			pr_err("can't allocate compressor buffer\n");
			crypto_free_comp(tfm);
			*per_cpu_ptr(zswap_comp_pcpu_tfms, cpu) = NULL;
			return NOTIFY_BAD;
		}
		per_cpu(zswap_dstmem, cpu) = dst;
		break;
	case CPU_DEAD:
	case CPU_UP_CANCELED:
		tfm = *per_cpu_ptr(zswap_comp_pcpu_tfms, cpu);
		if (tfm) {
			crypto_free_comp(tfm);
			*per_cpu_ptr(zswap_comp_pcpu_tfms, cpu) = NULL;
		}
		dst = per_cpu(zswap_dstmem, cpu);
		if (dst) {
			free_pages((unsigned long)dst, 1);
			per_cpu(zswap_dstmem, cpu) = NULL;
		}
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static int zswap_cpu_notifier(struct notifier_block *nb,
				unsigned long action, void *pcpu)
{
	unsigned long cpu = (unsigned long)pcpu;
	return __zswap_cpu_notifier(action, cpu);
}

static struct notifier_block zswap_cpu_notifier_block = {
	.notifier_call = zswap_cpu_notifier
};

static int zswap_cpu_init(void)
{
	unsigned long cpu;

	get_online_cpus();
	for_each_online_cpu(cpu)
		if (__zswap_cpu_notifier(CPU_UP_PREPARE, cpu) != NOTIFY_OK)
			goto cleanup;
	register_cpu_notifier(&zswap_cpu_notifier_block);
	put_online_cpus();
	return 0;

cleanup:
	for_each_online_cpu(cpu)
		__zswap_cpu_notifier(CPU_UP_CANCELED, cpu);
	put_online_cpus();
	return -ENOMEM;
}

/*********************************
* zsmalloc callbacks
**********************************/
static mempool_t *zswap_page_pool;

static inline unsigned int zswap_max_pool_pages(void)
{
	return zswap_max_pool_percent * totalram_pages / 100;
}

static inline int zswap_page_pool_create(void)
{
	/* TODO: dynamically size mempool */
	zswap_page_pool = mempool_create_page_pool(256, 0);
	if (!zswap_page_pool)
		return -ENOMEM;
	return 0;
}

static inline void zswap_page_pool_destroy(void)
{
	mempool_destroy(zswap_page_pool);
}

static struct page *zswap_alloc_page(gfp_t flags)
{
	struct page *page;

	if (atomic_read(&zswap_pool_pages) >= zswap_max_pool_pages()) {
		zswap_pool_limit_hit++;
		return NULL;
	}
	page = mempool_alloc(zswap_page_pool, flags);
	if (page)
		atomic_inc(&zswap_pool_pages);
	return page;
}

static void zswap_free_page(struct page *page)
{
	if (!page)
		return;
	mempool_free(page, zswap_page_pool);
	atomic_dec(&zswap_pool_pages);
}

static struct zs_ops zswap_zs_ops = {
	.alloc = zswap_alloc_page,
	.free = zswap_free_page
};

/*********************************
* frontswap hooks
**********************************/
/* attempts to compress and store an single page */
static int zswap_frontswap_store(unsigned type, pgoff_t offset,
				struct page *page)
{
	struct zswap_tree *tree = zswap_trees[type];
	struct zswap_entry *entry, *dupentry;
	int ret;
	unsigned int dlen = PAGE_SIZE;
	unsigned long handle;
	char *buf;
	u8 *src, *dst;

	if (!tree) {
		ret = -ENODEV;
		goto reject;
	}

	/* compress */
	dst = get_cpu_var(zswap_dstmem);
	src = kmap_atomic(page);
	ret = zswap_comp_op(ZSWAP_COMPOP_COMPRESS, src, PAGE_SIZE, dst, &dlen);
	kunmap_atomic(src);
	if (ret) {
		ret = -EINVAL;
		goto putcpu;
	}
	if ((dlen * 100 / PAGE_SIZE) > zswap_max_compression_ratio) {
		zswap_reject_compress_poor++;
		ret = -E2BIG;
		goto putcpu;
	}

	/* store */
	handle = zs_malloc(tree->pool, dlen,
		__GFP_NORETRY | __GFP_HIGHMEM | __GFP_NOMEMALLOC |
			__GFP_NOWARN);
	if (!handle) {
		zswap_reject_zsmalloc_fail++;
		ret = -ENOMEM;
		goto putcpu;
	}

	buf = zs_map_object(tree->pool, handle, ZS_MM_WO);
	memcpy(buf, dst, dlen);
	zs_unmap_object(tree->pool, handle);
	put_cpu_var(zswap_dstmem);

	/* allocate entry */
	entry = zswap_entry_cache_alloc(GFP_KERNEL);
	if (!entry) {
		zs_free(tree->pool, handle);
		zswap_reject_kmemcache_fail++;
		ret = -ENOMEM;
		goto reject;
	}

	/* populate entry */
	entry->type = type;
	entry->offset = offset;
	entry->handle = handle;
	entry->length = dlen;

	/* map */
	spin_lock(&tree->lock);
	do {
		ret = zswap_rb_insert(&tree->rbroot, entry, &dupentry);
		if (ret == -EEXIST) {
			zswap_duplicate_entry++;

			/* remove from rbtree */
			rb_erase(&dupentry->rbnode, &tree->rbroot);
			
			/* free */
			zs_free(tree->pool, dupentry->handle);
			zswap_entry_cache_free(dupentry);
			atomic_dec(&zswap_stored_pages);
		}
	} while (ret == -EEXIST);
	spin_unlock(&tree->lock);

	/* update stats */
	atomic_inc(&zswap_stored_pages);

	return 0;

putcpu:
	put_cpu_var(zswap_dstmem);
reject:
	return ret;
}

/*
 * returns 0 if the page was successfully decompressed
 * return -1 on entry not found or error
*/
static int zswap_frontswap_load(unsigned type, pgoff_t offset,
				struct page *page)
{
	struct zswap_tree *tree = zswap_trees[type];
	struct zswap_entry *entry;
	u8 *src, *dst;
	unsigned int dlen;

	/* find */
	spin_lock(&tree->lock);
	entry = zswap_rb_search(&tree->rbroot, offset);
	spin_unlock(&tree->lock);

	/* decompress */
	dlen = PAGE_SIZE;
	src = zs_map_object(tree->pool, entry->handle, ZS_MM_RO);
	dst = kmap_atomic(page);
	zswap_comp_op(ZSWAP_COMPOP_DECOMPRESS, src, entry->length,
		dst, &dlen);
	kunmap_atomic(dst);
	zs_unmap_object(tree->pool, entry->handle);

	return 0;
}

/* invalidates a single page */
static void zswap_frontswap_invalidate_page(unsigned type, pgoff_t offset)
{
	struct zswap_tree *tree = zswap_trees[type];
	struct zswap_entry *entry;

	/* find */
	spin_lock(&tree->lock);
	entry = zswap_rb_search(&tree->rbroot, offset);

	/* remove from rbtree */
	rb_erase(&entry->rbnode, &tree->rbroot);
	spin_unlock(&tree->lock);

	/* free */
	zs_free(tree->pool, entry->handle);
	zswap_entry_cache_free(entry);
	atomic_dec(&zswap_stored_pages);
}

/* invalidates all pages for the given swap type */
static void zswap_frontswap_invalidate_area(unsigned type)
{
	struct zswap_tree *tree = zswap_trees[type];
	struct rb_node *node;
	struct zswap_entry *entry;

	if (!tree)
		return;

	/* walk the tree and free everything */
	spin_lock(&tree->lock);
	/*
	 * TODO: Even though this code should not be executed because
	 * the try_to_unuse() in swapoff should have emptied the tree,
	 * it is very wasteful to rebalance the tree after every
	 * removal when we are freeing the whole tree.
	 *
	 * If post-order traversal code is ever added to the rbtree
	 * implementation, it should be used here.
	 */
	while ((node = rb_first(&tree->rbroot))) {
		entry = rb_entry(node, struct zswap_entry, rbnode);
		rb_erase(&entry->rbnode, &tree->rbroot);
		zs_free(tree->pool, entry->handle);
		zswap_entry_cache_free(entry);
	}
	tree->rbroot = RB_ROOT;
	spin_unlock(&tree->lock);
}

/* NOTE: this is called in atomic context from swapon and must not sleep */
static void zswap_frontswap_init(unsigned type)
{
	struct zswap_tree *tree;

	tree = kzalloc(sizeof(struct zswap_tree), GFP_NOWAIT);
	if (!tree)
		goto err;
	tree->pool = zs_create_pool(GFP_NOWAIT, &zswap_zs_ops);
	if (!tree->pool)
		goto freetree;
	tree->rbroot = RB_ROOT;
	spin_lock_init(&tree->lock);
	zswap_trees[type] = tree;
	return;

freetree:
	kfree(tree);
err:
	pr_err("alloc failed, zswap disabled for swap type %d\n", type);
}

static struct frontswap_ops zswap_frontswap_ops = {
	.store = zswap_frontswap_store,
	.load = zswap_frontswap_load,
	.invalidate_page = zswap_frontswap_invalidate_page,
	.invalidate_area = zswap_frontswap_invalidate_area,
	.init = zswap_frontswap_init
};

/*********************************
* debugfs functions
**********************************/
#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>

static struct dentry *zswap_debugfs_root;

static int __init zswap_debugfs_init(void)
{
	if (!debugfs_initialized())
		return -ENODEV;

	zswap_debugfs_root = debugfs_create_dir("zswap", NULL);
	if (!zswap_debugfs_root)
		return -ENOMEM;

	debugfs_create_u64("pool_limit_hit", S_IRUGO,
			zswap_debugfs_root, &zswap_pool_limit_hit);
	debugfs_create_u64("reject_zsmalloc_fail", S_IRUGO,
			zswap_debugfs_root, &zswap_reject_zsmalloc_fail);
	debugfs_create_u64("reject_kmemcache_fail", S_IRUGO,
			zswap_debugfs_root, &zswap_reject_kmemcache_fail);
	debugfs_create_u64("reject_compress_poor", S_IRUGO,
			zswap_debugfs_root, &zswap_reject_compress_poor);
	debugfs_create_u64("duplicate_entry", S_IRUGO,
			zswap_debugfs_root, &zswap_duplicate_entry);
	debugfs_create_atomic_t("pool_pages", S_IRUGO,
			zswap_debugfs_root, &zswap_pool_pages);
	debugfs_create_atomic_t("stored_pages", S_IRUGO,
			zswap_debugfs_root, &zswap_stored_pages);

	return 0;
}

static void __exit zswap_debugfs_exit(void)
{
	debugfs_remove_recursive(zswap_debugfs_root);
}
#else
static inline int __init zswap_debugfs_init(void)
{
	return 0;
}

static inline void __exit zswap_debugfs_exit(void) { }
#endif

/*********************************
* module init and exit
**********************************/
static int __init init_zswap(void)
{
	if (!zswap_enabled)
		return 0;

	pr_info("loading zswap\n");
	if (zswap_entry_cache_create()) {
		pr_err("entry cache creation failed\n");
		goto error;
	}
	if (zswap_page_pool_create()) {
		pr_err("page pool initialization failed\n");
		goto pagepoolfail;
	}
	if (zswap_comp_init()) {
		pr_err("compressor initialization failed\n");
		goto compfail;
	}
	if (zswap_cpu_init()) {
		pr_err("per-cpu initialization failed\n");
		goto pcpufail;
	}
	frontswap_register_ops(&zswap_frontswap_ops);
	if (zswap_debugfs_init())
		pr_warn("debugfs initialization failed\n");
	return 0;
pcpufail:
	zswap_comp_exit();
compfail:
	zswap_page_pool_destroy();
pagepoolfail:
	zswap_entry_cache_destory();
error:
	return -ENOMEM;
}
/* must be late so crypto has time to come up */
late_initcall(init_zswap);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Seth Jennings <sjenning@linux.vnet.ibm.com>");
MODULE_DESCRIPTION("Compressed cache for swap pages");
