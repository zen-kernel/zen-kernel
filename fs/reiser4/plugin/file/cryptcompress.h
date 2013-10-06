/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */
/* See http://www.namesys.com/cryptcompress_design.html */

#if !defined( __FS_REISER4_CRYPTCOMPRESS_H__ )
#define __FS_REISER4_CRYPTCOMPRESS_H__

#include "../../page_cache.h"
#include "../compress/compress.h"
#include "../crypto/cipher.h"

#include <linux/pagemap.h>

#define MIN_CLUSTER_SHIFT PAGE_CACHE_SHIFT
#define MAX_CLUSTER_SHIFT 16
#define MAX_CLUSTER_NRPAGES (1U << MAX_CLUSTER_SHIFT >> PAGE_CACHE_SHIFT)
#define DC_CHECKSUM_SIZE 4

#define MIN_LATTICE_FACTOR 1
#define MAX_LATTICE_FACTOR 32

/* this mask contains all non-standard plugins that might
   be present in reiser4-specific part of inode managed by
   cryptcompress file plugin */
#define cryptcompress_mask				\
	((1 << PSET_FILE) |				\
	 (1 << PSET_CLUSTER) |				\
	 (1 << PSET_CIPHER) |				\
	 (1 << PSET_DIGEST) |				\
	 (1 << PSET_COMPRESSION) |			\
	 (1 << PSET_COMPRESSION_MODE))

#if REISER4_DEBUG
static inline int cluster_shift_ok(int shift)
{
	return (shift >= MIN_CLUSTER_SHIFT) && (shift <= MAX_CLUSTER_SHIFT);
}
#endif

#if REISER4_DEBUG
#define INODE_PGCOUNT(inode)						\
({								        \
	assert("edward-1530", inode_file_plugin(inode) ==		\
	       file_plugin_by_id(CRYPTCOMPRESS_FILE_PLUGIN_ID));	\
	atomic_read(&cryptcompress_inode_data(inode)->pgcount);		\
 })
#define INODE_PGCOUNT_INC(inode)					\
do {								        \
	assert("edward-1531", inode_file_plugin(inode) ==		\
	       file_plugin_by_id(CRYPTCOMPRESS_FILE_PLUGIN_ID));	\
	atomic_inc(&cryptcompress_inode_data(inode)->pgcount);		\
} while (0)
#define INODE_PGCOUNT_DEC(inode)					\
do {								        \
	if (inode_file_plugin(inode) ==					\
	    file_plugin_by_id(CRYPTCOMPRESS_FILE_PLUGIN_ID))		\
		atomic_dec(&cryptcompress_inode_data(inode)->pgcount);	\
} while (0)
#else
#define INODE_PGCOUNT(inode) (0)
#define INODE_PGCOUNT_INC(inode)
#define INODE_PGCOUNT_DEC(inode)
#endif /* REISER4_DEBUG */

struct tfm_stream {
	__u8 *data;
	size_t size;
};

typedef enum {
	INPUT_STREAM,
	OUTPUT_STREAM,
	LAST_STREAM
} tfm_stream_id;

typedef struct tfm_stream * tfm_unit[LAST_STREAM];

static inline __u8 *ts_data(struct tfm_stream * stm)
{
	assert("edward-928", stm != NULL);
	return stm->data;
}

static inline size_t ts_size(struct tfm_stream * stm)
{
	assert("edward-929", stm != NULL);
	return stm->size;
}

static inline void set_ts_size(struct tfm_stream * stm, size_t size)
{
	assert("edward-930", stm != NULL);

	stm->size = size;
}

static inline int alloc_ts(struct tfm_stream ** stm)
{
	assert("edward-931", stm);
	assert("edward-932", *stm == NULL);

	*stm = kzalloc(sizeof(**stm), reiser4_ctx_gfp_mask_get());
	if (!*stm)
		return -ENOMEM;
	return 0;
}

static inline void free_ts(struct tfm_stream * stm)
{
	assert("edward-933", !ts_data(stm));
	assert("edward-934", !ts_size(stm));

	kfree(stm);
}

static inline int alloc_ts_data(struct tfm_stream * stm, size_t size)
{
	assert("edward-935", !ts_data(stm));
	assert("edward-936", !ts_size(stm));
	assert("edward-937", size != 0);

	stm->data = reiser4_vmalloc(size);
	if (!stm->data)
		return -ENOMEM;
	set_ts_size(stm, size);
	return 0;
}

static inline void free_ts_data(struct tfm_stream * stm)
{
	assert("edward-938", equi(ts_data(stm), ts_size(stm)));

	if (ts_data(stm))
		vfree(ts_data(stm));
	memset(stm, 0, sizeof *stm);
}

/* Write modes for item conversion in flush convert phase */
typedef enum {
	CRC_APPEND_ITEM = 1,
	CRC_OVERWRITE_ITEM = 2,
	CRC_CUT_ITEM = 3
} cryptcompress_write_mode_t;

typedef enum {
	LC_INVAL  = 0,   /* invalid value */
	LC_APPOV = 1,    /* append and/or overwrite */
	LC_TRUNC = 2	 /* truncate */
} logical_cluster_op;

/* Transform cluster.
 * Intermediate state between page cluster and disk cluster
 * Is used for data transform (compression/encryption)
 */
struct tfm_cluster {
	coa_set coa;      /* compression algorithms info */
	tfm_unit tun;     /* plain and transformed streams */
	tfm_action act;
	int uptodate;
	int lsize;        /* number of bytes in logical cluster */
	int len;          /* length of the transform stream */
};

static inline coa_t get_coa(struct tfm_cluster * tc, reiser4_compression_id id,
			    tfm_action act)
{
	return tc->coa[id][act];
}

static inline void set_coa(struct tfm_cluster * tc, reiser4_compression_id id,
			   tfm_action act, coa_t coa)
{
	tc->coa[id][act] = coa;
}

static inline int alloc_coa(struct tfm_cluster * tc, compression_plugin * cplug)
{
	coa_t coa;

	coa = cplug->alloc(tc->act);
	if (IS_ERR(coa))
		return PTR_ERR(coa);
	set_coa(tc, cplug->h.id, tc->act, coa);
	return 0;
}

static inline int
grab_coa(struct tfm_cluster * tc, compression_plugin * cplug)
{
	return (cplug->alloc && !get_coa(tc, cplug->h.id, tc->act) ?
		alloc_coa(tc, cplug) : 0);
}

static inline void free_coa_set(struct tfm_cluster * tc)
{
	tfm_action j;
	reiser4_compression_id i;
	compression_plugin *cplug;

	assert("edward-810", tc != NULL);

	for (j = 0; j < TFMA_LAST; j++)
		for (i = 0; i < LAST_COMPRESSION_ID; i++) {
			if (!get_coa(tc, i, j))
				continue;
			cplug = compression_plugin_by_id(i);
			assert("edward-812", cplug->free != NULL);
			cplug->free(get_coa(tc, i, j), j);
			set_coa(tc, i, j, 0);
		}
	return;
}

static inline struct tfm_stream * get_tfm_stream(struct tfm_cluster * tc,
						 tfm_stream_id id)
{
	return tc->tun[id];
}

static inline void set_tfm_stream(struct tfm_cluster * tc,
				  tfm_stream_id id, struct tfm_stream * ts)
{
	tc->tun[id] = ts;
}

static inline __u8 *tfm_stream_data(struct tfm_cluster * tc, tfm_stream_id id)
{
	return ts_data(get_tfm_stream(tc, id));
}

static inline void set_tfm_stream_data(struct tfm_cluster * tc,
				       tfm_stream_id id, __u8 * data)
{
	get_tfm_stream(tc, id)->data = data;
}

static inline size_t tfm_stream_size(struct tfm_cluster * tc, tfm_stream_id id)
{
	return ts_size(get_tfm_stream(tc, id));
}

static inline void
set_tfm_stream_size(struct tfm_cluster * tc, tfm_stream_id id, size_t size)
{
	get_tfm_stream(tc, id)->size = size;
}

static inline int
alloc_tfm_stream(struct tfm_cluster * tc, size_t size, tfm_stream_id id)
{
	assert("edward-939", tc != NULL);
	assert("edward-940", !get_tfm_stream(tc, id));

	tc->tun[id] = kzalloc(sizeof(struct tfm_stream),
			      reiser4_ctx_gfp_mask_get());
	if (!tc->tun[id])
		return -ENOMEM;
	return alloc_ts_data(get_tfm_stream(tc, id), size);
}

static inline int
realloc_tfm_stream(struct tfm_cluster * tc, size_t size, tfm_stream_id id)
{
	assert("edward-941", tfm_stream_size(tc, id) < size);
	free_ts_data(get_tfm_stream(tc, id));
	return alloc_ts_data(get_tfm_stream(tc, id), size);
}

static inline void free_tfm_stream(struct tfm_cluster * tc, tfm_stream_id id)
{
	free_ts_data(get_tfm_stream(tc, id));
	free_ts(get_tfm_stream(tc, id));
	set_tfm_stream(tc, id, 0);
}

static inline unsigned coa_overrun(compression_plugin * cplug, int ilen)
{
	return (cplug->overrun != NULL ? cplug->overrun(ilen) : 0);
}

static inline void free_tfm_unit(struct tfm_cluster * tc)
{
	tfm_stream_id id;
	for (id = 0; id < LAST_STREAM; id++) {
		if (!get_tfm_stream(tc, id))
			continue;
		free_tfm_stream(tc, id);
	}
}

static inline void put_tfm_cluster(struct tfm_cluster * tc)
{
	assert("edward-942", tc != NULL);
	free_coa_set(tc);
	free_tfm_unit(tc);
}

static inline int tfm_cluster_is_uptodate(struct tfm_cluster * tc)
{
	assert("edward-943", tc != NULL);
	assert("edward-944", tc->uptodate == 0 || tc->uptodate == 1);
	return (tc->uptodate == 1);
}

static inline void tfm_cluster_set_uptodate(struct tfm_cluster * tc)
{
	assert("edward-945", tc != NULL);
	assert("edward-946", tc->uptodate == 0 || tc->uptodate == 1);
	tc->uptodate = 1;
	return;
}

static inline void tfm_cluster_clr_uptodate(struct tfm_cluster * tc)
{
	assert("edward-947", tc != NULL);
	assert("edward-948", tc->uptodate == 0 || tc->uptodate == 1);
	tc->uptodate = 0;
	return;
}

static inline int tfm_stream_is_set(struct tfm_cluster * tc, tfm_stream_id id)
{
	return (get_tfm_stream(tc, id) &&
		tfm_stream_data(tc, id) && tfm_stream_size(tc, id));
}

static inline int tfm_cluster_is_set(struct tfm_cluster * tc)
{
	int i;
	for (i = 0; i < LAST_STREAM; i++)
		if (!tfm_stream_is_set(tc, i))
			return 0;
	return 1;
}

static inline void alternate_streams(struct tfm_cluster * tc)
{
	struct tfm_stream *tmp = get_tfm_stream(tc, INPUT_STREAM);

	set_tfm_stream(tc, INPUT_STREAM, get_tfm_stream(tc, OUTPUT_STREAM));
	set_tfm_stream(tc, OUTPUT_STREAM, tmp);
}

/* Set of states to indicate a kind of data
 * that will be written to the window */
typedef enum {
	DATA_WINDOW,		/* user's data */
	HOLE_WINDOW		/* zeroes (such kind of data can be written
				 * if we start to write from offset > i_size) */
} window_stat;

/* Window (of logical cluster size) discretely sliding along a file.
 * Is used to locate hole region in a logical cluster to be properly
 * represented on disk.
 * We split a write to cryptcompress file into writes to its logical
 * clusters. Before writing to a logical cluster we set a window, i.e.
 * calculate values of the following fields:
 */
struct reiser4_slide {
	unsigned off;		/* offset to write from */
	unsigned count;		/* number of bytes to write */
	unsigned delta;		/* number of bytes to append to the hole */
	window_stat stat;	/* what kind of data will be written starting
				   from @off */
};

/* Possible states of a disk cluster */
typedef enum {
	INVAL_DISK_CLUSTER,	/* unknown state */
	PREP_DISK_CLUSTER,	/* disk cluster got converted by flush
				 * at least 1 time */
	UNPR_DISK_CLUSTER,	/* disk cluster just created and should be
				 * converted by flush */
	FAKE_DISK_CLUSTER,	/* disk cluster doesn't exist neither in memory
				 * nor on disk */
	TRNC_DISK_CLUSTER       /* disk cluster is partially truncated */
} disk_cluster_stat;

/* The following structure represents various stages of the same logical
 * cluster of index @index:
 * . fixed slide
 * . page cluster         (stage in primary cache)
 * . transform cluster    (transition stage)
 * . disk cluster         (stage in secondary cache)
 * This structure is used in transition and synchronizing operations, e.g.
 * transform cluster is a transition state when synchronizing page cluster
 * and disk cluster.
 * FIXME: Encapsulate page cluster, disk cluster.
 */
struct cluster_handle {
	cloff_t index;		 /* offset in a file (unit is a cluster size) */
	int index_valid;         /* for validating the index above, if needed */
	struct file *file;       /* host file */

	/* logical cluster */
	struct reiser4_slide *win; /* sliding window to locate holes */
	logical_cluster_op op;	 /* logical cluster operation (truncate or
				    append/overwrite) */
	/* transform cluster */
	struct tfm_cluster tc;	 /* contains all needed info to synchronize
				    page cluster and disk cluster) */
        /* page cluster */
	int nr_pages;		 /* number of pages of current checkin action */
 	int old_nrpages;         /* number of pages of last checkin action */
	struct page **pages;	 /* attached pages */
	jnode * node;            /* jnode for capture */

	/* disk cluster */
	hint_t *hint;		 /* current position in the tree */
	disk_cluster_stat dstat; /* state of the current disk cluster */
	int reserved;		 /* is space for disk cluster reserved */
#if REISER4_DEBUG
	reiser4_context *ctx;
	int reserved_prepped;
	int reserved_unprepped;
#endif

};

static inline __u8 * tfm_input_data (struct cluster_handle * clust)
{
	return tfm_stream_data(&clust->tc, INPUT_STREAM);
}

static inline __u8 * tfm_output_data (struct cluster_handle * clust)
{
	return tfm_stream_data(&clust->tc, OUTPUT_STREAM);
}

static inline int reset_cluster_pgset(struct cluster_handle * clust,
				      int nrpages)
{
	assert("edward-1057", clust->pages != NULL);
	memset(clust->pages, 0, sizeof(*clust->pages) * nrpages);
	return 0;
}

static inline int alloc_cluster_pgset(struct cluster_handle * clust,
				      int nrpages)
{
	assert("edward-949", clust != NULL);
	assert("edward-1362", clust->pages == NULL);
	assert("edward-950", nrpages != 0 && nrpages <= MAX_CLUSTER_NRPAGES);

	clust->pages = kzalloc(sizeof(*clust->pages) * nrpages,
			       reiser4_ctx_gfp_mask_get());
	if (!clust->pages)
		return RETERR(-ENOMEM);
	return 0;
}

static inline void move_cluster_pgset(struct cluster_handle *clust,
				      struct page ***pages, int * nr_pages)
{
	assert("edward-1545", clust != NULL && clust->pages != NULL);
	assert("edward-1546", pages != NULL && *pages == NULL);
	*pages = clust->pages;
	*nr_pages = clust->nr_pages;
	clust->pages = NULL;
}

static inline void free_cluster_pgset(struct cluster_handle * clust)
{
	assert("edward-951", clust->pages != NULL);
	kfree(clust->pages);
	clust->pages = NULL;
}

static inline void put_cluster_handle(struct cluster_handle * clust)
{
	assert("edward-435", clust != NULL);

	put_tfm_cluster(&clust->tc);
	if (clust->pages)
		free_cluster_pgset(clust);
	memset(clust, 0, sizeof *clust);
}

static inline void inc_keyload_count(struct reiser4_crypto_info * data)
{
 	assert("edward-1410", data != NULL);
 	data->keyload_count++;
}

static inline void dec_keyload_count(struct reiser4_crypto_info * data)
{
 	assert("edward-1411", data != NULL);
 	assert("edward-1412", data->keyload_count > 0);
 	data->keyload_count--;
}

static inline int capture_cluster_jnode(jnode * node)
{
	return reiser4_try_capture(node, ZNODE_WRITE_LOCK, 0);
}

/* cryptcompress specific part of reiser4_inode */
struct cryptcompress_info {
	struct mutex checkin_mutex;  /* This is to serialize
				      * checkin_logical_cluster operations */
	cloff_t trunc_index;         /* Index of the leftmost truncated disk
				      * cluster (to resolve races with read) */
	struct reiser4_crypto_info *crypt;
	/*
	 * the following 2 fields are controlled by compression mode plugin
	 */
	int compress_toggle;          /* Current status of compressibility */
	int lattice_factor;           /* Factor of dynamic lattice. FIXME: Have
				       * a compression_toggle to keep the factor
				       */
#if REISER4_DEBUG
	atomic_t pgcount;             /* number of grabbed pages */
#endif
};

static inline void set_compression_toggle (struct cryptcompress_info * info, int val)
{
	info->compress_toggle = val;
}

static inline int get_compression_toggle (struct cryptcompress_info * info)
{
	return info->compress_toggle;
}

static inline int compression_is_on(struct cryptcompress_info * info)
{
	return get_compression_toggle(info) == 1;
}

static inline void turn_on_compression(struct cryptcompress_info * info)
{
	set_compression_toggle(info, 1);
}

static inline void turn_off_compression(struct cryptcompress_info * info)
{
	set_compression_toggle(info, 0);
}

static inline void set_lattice_factor(struct cryptcompress_info * info, int val)
{
	info->lattice_factor = val;
}

static inline int get_lattice_factor(struct cryptcompress_info * info)
{
	return info->lattice_factor;
}

struct cryptcompress_info *cryptcompress_inode_data(const struct inode *);
int equal_to_rdk(znode *, const reiser4_key *);
int goto_right_neighbor(coord_t *, lock_handle *);
int cryptcompress_inode_ok(struct inode *inode);
int coord_is_unprepped_ctail(const coord_t * coord);
extern int do_readpage_ctail(struct inode *, struct cluster_handle *,
			     struct page * page, znode_lock_mode mode);
extern int ctail_insert_unprepped_cluster(struct cluster_handle * clust,
					  struct inode * inode);
extern int readpages_cryptcompress(struct file*, struct address_space*,
				   struct list_head*, unsigned);
int bind_cryptcompress(struct inode *child, struct inode *parent);
void destroy_inode_cryptcompress(struct inode * inode);
int grab_page_cluster(struct inode *inode, struct cluster_handle * clust,
		      rw_op rw);
int write_dispatch_hook(struct file *file, struct inode * inode,
			loff_t pos, struct cluster_handle * clust,
			struct dispatch_context * cont);
int setattr_dispatch_hook(struct inode * inode);
struct reiser4_crypto_info * inode_crypto_info(struct inode * inode);
void inherit_crypto_info_common(struct inode * parent, struct inode * object,
				int (*can_inherit)(struct inode * child,
						   struct inode * parent));
void reiser4_attach_crypto_info(struct inode * inode,
				struct reiser4_crypto_info * info);
void change_crypto_info(struct inode * inode, struct reiser4_crypto_info * new);
struct reiser4_crypto_info * reiser4_alloc_crypto_info (struct inode * inode);

static inline struct crypto_blkcipher * info_get_cipher(struct reiser4_crypto_info * info)
{
	return info->cipher;
}

static inline void info_set_cipher(struct reiser4_crypto_info * info,
				   struct crypto_blkcipher * tfm)
{
	info->cipher = tfm;
}

static inline struct crypto_hash * info_get_digest(struct reiser4_crypto_info * info)
{
	return info->digest;
}

static inline void info_set_digest(struct reiser4_crypto_info * info,
				   struct crypto_hash * tfm)
{
	info->digest = tfm;
}

static inline void put_cluster_page(struct page * page)
{
	page_cache_release(page);
}

#endif /* __FS_REISER4_CRYPTCOMPRESS_H__ */

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   scroll-step: 1
   End:
*/
