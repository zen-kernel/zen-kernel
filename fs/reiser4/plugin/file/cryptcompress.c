/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
   reiser4/README */
/*
 * Written by Edward Shishkin.
 *
 * Implementations of inode/file/address_space operations
 * specific for cryptcompress file plugin which manages
 * regular files built of compressed and(or) encrypted bodies.
 * See http://dev.namesys.com/CryptcompressPlugin for details.
 */

#include "../../inode.h"
#include "../cluster.h"
#include "../object.h"
#include "../../tree_walk.h"
#include "cryptcompress.h"

#include <linux/pagevec.h>
#include <asm/uaccess.h>
#include <linux/swap.h>
#include <linux/writeback.h>
#include <linux/random.h>
#include <linux/scatterlist.h>

/*
               Managing primary and secondary caches by Reiser4
               cryptcompress file plugin. Synchronization scheme.


                                             +------------------+
                        +------------------->|    tfm stream    |
                        |                    | (compressed data)|
                  flush |                    +------------------+
                        +-----------------+           |
                        |(->)longterm lock|           V
--+        writepages() |                 |        +-***-+  reiser4        +---+
  |                     |                 +--+     | *** |  storage tree   |   |
  |                     |                    |     +-***-+  (primary cache)|   |
u | write()   (secondary| cache)             V    /   |   \                |   |
s | ---->  +----+ +----+ +----+ +----+     +-***** ******* **----+  ---->  | d |
e |        |    | |page cluster |    |     | **disk cluster**    |         | i |
r | <----  +----+ +----+ +----+ +----+     +-***** **********----+  <----  | s |
  | read()              ^                      ^      |                    | k |
  |                     |     (->)longterm lock|      |           page_io()|   |
  |                     |                      +------+                    |   |
--+         readpages() |                             |                    +---+
                        |                             V
                        |                    +------------------+
                        +--------------------|    tfm stream    |
                                             |   (plain text)   |
                                             +------------------+
*/

/* get cryptcompress specific portion of inode */
struct cryptcompress_info *cryptcompress_inode_data(const struct inode *inode)
{
	return &reiser4_inode_data(inode)->file_plugin_data.cryptcompress_info;
}

/* plugin->u.file.init_inode_data */
void init_inode_data_cryptcompress(struct inode *inode,
				   reiser4_object_create_data * crd,
				   int create)
{
	struct cryptcompress_info *data;

	data = cryptcompress_inode_data(inode);
	assert("edward-685", data != NULL);

	memset(data, 0, sizeof(*data));

	mutex_init(&data->checkin_mutex);
	data->trunc_index = ULONG_MAX;
	turn_on_compression(data);
	set_lattice_factor(data, MIN_LATTICE_FACTOR);
	init_inode_ordering(inode, crd, create);
}

/* The following is a part of reiser4 cipher key manager
   which is called when opening/creating a cryptcompress file */

/* get/set cipher key info */
struct reiser4_crypto_info * inode_crypto_info (struct inode * inode)
{
	assert("edward-90", inode != NULL);
	assert("edward-91", reiser4_inode_data(inode) != NULL);
	return cryptcompress_inode_data(inode)->crypt;
}

static void set_inode_crypto_info (struct inode * inode,
				   struct reiser4_crypto_info * info)
{
	cryptcompress_inode_data(inode)->crypt = info;
}

/* allocate a cipher key info */
struct reiser4_crypto_info * reiser4_alloc_crypto_info (struct inode * inode)
{
	struct reiser4_crypto_info *info;
	int fipsize;

	info = kzalloc(sizeof(*info), reiser4_ctx_gfp_mask_get());
	if (!info)
		return ERR_PTR(-ENOMEM);

	fipsize = inode_digest_plugin(inode)->fipsize;
	info->keyid = kmalloc(fipsize, reiser4_ctx_gfp_mask_get());
	if (!info->keyid) {
		kfree(info);
		return ERR_PTR(-ENOMEM);
	}
	info->host = inode;
	return info;
}

#if 0
/* allocate/free low-level info for cipher and digest
   transforms */
static int alloc_crypto_tfms(struct reiser4_crypto_info * info)
{
	struct crypto_blkcipher * ctfm = NULL;
	struct crypto_hash      * dtfm = NULL;
	cipher_plugin * cplug = inode_cipher_plugin(info->host);
	digest_plugin * dplug = inode_digest_plugin(info->host);

	if (cplug->alloc) {
		ctfm = cplug->alloc();
		if (IS_ERR(ctfm)) {
			warning("edward-1364",
				"Can not allocate info for %s\n",
				cplug->h.desc);
			return RETERR(PTR_ERR(ctfm));
		}
	}
	info_set_cipher(info, ctfm);
	if (dplug->alloc) {
		dtfm = dplug->alloc();
		if (IS_ERR(dtfm)) {
			warning("edward-1365",
				"Can not allocate info for %s\n",
				dplug->h.desc);
			goto unhappy_with_digest;
		}
	}
	info_set_digest(info, dtfm);
	return 0;
 unhappy_with_digest:
	if (cplug->free) {
		cplug->free(ctfm);
		info_set_cipher(info, NULL);
	}
	return RETERR(PTR_ERR(dtfm));
}
#endif

static void
free_crypto_tfms(struct reiser4_crypto_info * info)
{
	assert("edward-1366", info != NULL);
	if (!info_get_cipher(info)) {
		assert("edward-1601", !info_get_digest(info));
		return;
	}
	inode_cipher_plugin(info->host)->free(info_get_cipher(info));
	info_set_cipher(info, NULL);
	inode_digest_plugin(info->host)->free(info_get_digest(info));
	info_set_digest(info, NULL);
	return;
}

#if 0
/* create a key fingerprint for disk stat-data */
static int create_keyid (struct reiser4_crypto_info * info,
			 struct reiser4_crypto_data * data)
{
	int ret = -ENOMEM;
	size_t blk, pad;
	__u8 * dmem;
	__u8 * cmem;
	struct hash_desc      ddesc;
	struct blkcipher_desc cdesc;
	struct scatterlist sg;

	assert("edward-1367", info != NULL);
	assert("edward-1368", info->keyid != NULL);

	ddesc.tfm = info_get_digest(info);
	ddesc.flags = 0;
	cdesc.tfm = info_get_cipher(info);
	cdesc.flags = 0;

	dmem = kmalloc((size_t)crypto_hash_digestsize(ddesc.tfm),
		       reiser4_ctx_gfp_mask_get());
	if (!dmem)
		goto exit1;

	blk = crypto_blkcipher_blocksize(cdesc.tfm);

	pad = data->keyid_size % blk;
	pad = (pad ? blk - pad : 0);

	cmem = kmalloc((size_t)data->keyid_size + pad,
		       reiser4_ctx_gfp_mask_get());
	if (!cmem)
		goto exit2;
	memcpy(cmem, data->keyid, data->keyid_size);
	memset(cmem + data->keyid_size, 0, pad);

	sg_init_one(&sg, cmem, data->keyid_size + pad);

	ret = crypto_blkcipher_encrypt(&cdesc, &sg, &sg,
				       data->keyid_size + pad);
	if (ret) {
		warning("edward-1369",
			"encryption failed flags=%x\n", cdesc.flags);
		goto exit3;
	}
	ret = crypto_hash_digest(&ddesc, &sg, sg.length, dmem);
	if (ret) {
		warning("edward-1602",
			"digest failed flags=%x\n", ddesc.flags);
		goto exit3;
	}
	memcpy(info->keyid, dmem, inode_digest_plugin(info->host)->fipsize);
 exit3:
	kfree(cmem);
 exit2:
	kfree(dmem);
 exit1:
	return ret;
}
#endif

static void destroy_keyid(struct reiser4_crypto_info * info)
{
	assert("edward-1370", info != NULL);
	assert("edward-1371", info->keyid != NULL);
	kfree(info->keyid);
	return;
}

static void __free_crypto_info (struct inode * inode)
{
	struct reiser4_crypto_info * info = inode_crypto_info(inode);
	assert("edward-1372", info != NULL);

	free_crypto_tfms(info);
	destroy_keyid(info);
	kfree(info);
}

#if 0
static void instantiate_crypto_info(struct reiser4_crypto_info * info)
{
	assert("edward-1373", info != NULL);
	assert("edward-1374", info->inst == 0);
	info->inst = 1;
}
#endif

static void uninstantiate_crypto_info(struct reiser4_crypto_info * info)
{
	assert("edward-1375", info != NULL);
	info->inst = 0;
}

#if 0
static int is_crypto_info_instantiated(struct reiser4_crypto_info * info)
{
	return info->inst;
}

static int inode_has_cipher_key(struct inode * inode)
{
	assert("edward-1376", inode != NULL);
	return inode_crypto_info(inode) &&
		is_crypto_info_instantiated(inode_crypto_info(inode));
}
#endif

static void free_crypto_info (struct inode * inode)
{
	uninstantiate_crypto_info(inode_crypto_info(inode));
	__free_crypto_info(inode);
}

static int need_cipher(struct inode * inode)
{
	return inode_cipher_plugin(inode) !=
		cipher_plugin_by_id(NONE_CIPHER_ID);
}

/* Parse @data which contains a (uninstantiated) cipher key imported
   from user space, create a low-level cipher info and attach it to
   the @object. If success, then info contains an instantiated key */
#if 0
struct reiser4_crypto_info * create_crypto_info(struct inode * object,
				  struct reiser4_crypto_data * data)
{
	int ret;
	struct reiser4_crypto_info * info;

	assert("edward-1377", data != NULL);
	assert("edward-1378", need_cipher(object));

	if (inode_file_plugin(object) !=
	    file_plugin_by_id(DIRECTORY_FILE_PLUGIN_ID))
		return ERR_PTR(-EINVAL);

	info = reiser4_alloc_crypto_info(object);
	if (IS_ERR(info))
		return info;
	ret = alloc_crypto_tfms(info);
	if (ret)
		goto err;
	/* instantiating a key */
	ret = crypto_blkcipher_setkey(info_get_cipher(info),
				      data->key,
				      data->keysize);
	if (ret) {
		warning("edward-1379",
			"setkey failed flags=%x",
			crypto_blkcipher_get_flags(info_get_cipher(info)));
		goto err;
	}
	info->keysize = data->keysize;
	ret = create_keyid(info, data);
	if (ret)
		goto err;
	instantiate_crypto_info(info);
	return info;
 err:
	__free_crypto_info(object);
 	return ERR_PTR(ret);
}
#endif

/* increment/decrement a load counter when
   attaching/detaching the crypto-stat to any object */
static void load_crypto_info(struct reiser4_crypto_info * info)
{
	assert("edward-1380", info != NULL);
	inc_keyload_count(info);
}

static void unload_crypto_info(struct inode * inode)
{
	struct reiser4_crypto_info * info = inode_crypto_info(inode);
	assert("edward-1381", info->keyload_count > 0);

	dec_keyload_count(inode_crypto_info(inode));
	if (info->keyload_count == 0)
		/* final release */
		free_crypto_info(inode);
}

/* attach/detach an existing crypto-stat */
void reiser4_attach_crypto_info(struct inode * inode,
				struct reiser4_crypto_info * info)
{
	assert("edward-1382", inode != NULL);
	assert("edward-1383", info != NULL);
	assert("edward-1384", inode_crypto_info(inode) == NULL);

	set_inode_crypto_info(inode, info);
	load_crypto_info(info);
}

/* returns true, if crypto stat can be attached to the @host */
#if REISER4_DEBUG
static int host_allows_crypto_info(struct inode * host)
{
	int ret;
	file_plugin * fplug = inode_file_plugin(host);

	switch (fplug->h.id) {
	case CRYPTCOMPRESS_FILE_PLUGIN_ID:
		ret = 1;
		break;
	default:
		ret = 0;
	}
	return ret;
}
#endif  /*  REISER4_DEBUG  */

static void reiser4_detach_crypto_info(struct inode * inode)
{
	assert("edward-1385", inode != NULL);
	assert("edward-1386", host_allows_crypto_info(inode));

	if (inode_crypto_info(inode))
		unload_crypto_info(inode);
	set_inode_crypto_info(inode, NULL);
}

#if 0

/* compare fingerprints of @child and @parent */
static int keyid_eq(struct reiser4_crypto_info * child,
		    struct reiser4_crypto_info * parent)
{
	return !memcmp(child->keyid,
		       parent->keyid,
		       info_digest_plugin(parent)->fipsize);
}

/* check if a crypto-stat (which is bound to @parent) can be inherited */
int can_inherit_crypto_cryptcompress(struct inode *child, struct inode *parent)
{
	if (!need_cipher(child))
		return 0;
	/* the child is created */
	if (!inode_crypto_info(child))
		return 1;
	/* the child is looked up */
	if (!inode_crypto_info(parent))
		return 0;
	return (inode_cipher_plugin(child) == inode_cipher_plugin(parent) &&
		inode_digest_plugin(child) == inode_digest_plugin(parent) &&
		inode_crypto_info(child)->keysize ==
		inode_crypto_info(parent)->keysize &&
		keyid_eq(inode_crypto_info(child), inode_crypto_info(parent)));
}
#endif

/* helper functions for ->create() method of the cryptcompress plugin */
static int inode_set_crypto(struct inode * object)
{
	reiser4_inode * info;
	if (!inode_crypto_info(object)) {
		if (need_cipher(object))
			return RETERR(-EINVAL);
		/* the file is not to be encrypted */
		return 0;
	}
	info = reiser4_inode_data(object);
	info->extmask |= (1 << CRYPTO_STAT);
 	return 0;
}

static int inode_init_compression(struct inode * object)
{
	int result = 0;
	assert("edward-1461", object != NULL);
	if (inode_compression_plugin(object)->init)
		result = inode_compression_plugin(object)->init();
	return result;
}

static int inode_check_cluster(struct inode * object)
{
	assert("edward-696", object != NULL);

	if (unlikely(inode_cluster_size(object) < PAGE_CACHE_SIZE)) {
		warning("edward-1320", "Can not support '%s' "
			"logical clusters (less then page size)",
			inode_cluster_plugin(object)->h.label);
		return RETERR(-EINVAL);
	}
	if (unlikely(inode_cluster_shift(object)) >= BITS_PER_BYTE*sizeof(int)){
		warning("edward-1463", "Can not support '%s' "
			"logical clusters (too big for transform)",
			inode_cluster_plugin(object)->h.label);
		return RETERR(-EINVAL);
	}
	return 0;
}

/* plugin->destroy_inode() */
void destroy_inode_cryptcompress(struct inode * inode)
{
	assert("edward-1464", INODE_PGCOUNT(inode) == 0);
	reiser4_detach_crypto_info(inode);
	return;
}

/* plugin->create_object():
. install plugins
. attach crypto info if specified
. attach compression info if specified
. attach cluster info
*/
int create_object_cryptcompress(struct inode *object, struct inode *parent,
				reiser4_object_create_data * data)
{
	int result;
	reiser4_inode *info;

	assert("edward-23", object != NULL);
	assert("edward-24", parent != NULL);
	assert("edward-30", data != NULL);
	assert("edward-26", reiser4_inode_get_flag(object, REISER4_NO_SD));
	assert("edward-27", data->id == CRYPTCOMPRESS_FILE_PLUGIN_ID);

	info = reiser4_inode_data(object);

	assert("edward-29", info != NULL);

	/* set file bit */
	info->plugin_mask |= (1 << PSET_FILE);

	/* set crypto */
	result = inode_set_crypto(object);
	if (result)
		goto error;
	/* set compression */
	result = inode_init_compression(object);
	if (result)
		goto error;
	/* set cluster */
	result = inode_check_cluster(object);
	if (result)
		goto error;

	/* save everything in disk stat-data */
	result = write_sd_by_inode_common(object);
	if (!result)
		return 0;
 error:
	reiser4_detach_crypto_info(object);
	return result;
}

/* plugin->open() */
int open_cryptcompress(struct inode * inode, struct file * file)
{
	return 0;
}

/* returns a blocksize, the attribute of a cipher algorithm */
static unsigned int
cipher_blocksize(struct inode * inode)
{
	assert("edward-758", need_cipher(inode));
	assert("edward-1400", inode_crypto_info(inode) != NULL);
	return crypto_blkcipher_blocksize
		(info_get_cipher(inode_crypto_info(inode)));
}

/* returns offset translated by scale factor of the crypto-algorithm */
static loff_t inode_scaled_offset (struct inode * inode,
				   const loff_t src_off /* input offset */)
{
	assert("edward-97", inode != NULL);

	if (!need_cipher(inode) ||
	    src_off == get_key_offset(reiser4_min_key()) ||
	    src_off == get_key_offset(reiser4_max_key()))
		return src_off;

	return inode_cipher_plugin(inode)->scale(inode,
						 cipher_blocksize(inode),
						 src_off);
}

/* returns disk cluster size */
size_t inode_scaled_cluster_size(struct inode * inode)
{
	assert("edward-110", inode != NULL);

	return inode_scaled_offset(inode, inode_cluster_size(inode));
}

/* set number of cluster pages */
static void set_cluster_nrpages(struct cluster_handle * clust,
				struct inode *inode)
{
	struct reiser4_slide * win;

	assert("edward-180", clust != NULL);
	assert("edward-1040", inode != NULL);

	clust->old_nrpages = size_in_pages(lbytes(clust->index, inode));
	win = clust->win;
	if (!win) {
		clust->nr_pages = size_in_pages(lbytes(clust->index, inode));
		return;
	}
	assert("edward-1176", clust->op != LC_INVAL);
	assert("edward-1064", win->off + win->count + win->delta != 0);

	if (win->stat == HOLE_WINDOW &&
	    win->off == 0 && win->count == inode_cluster_size(inode)) {
		/* special case: writing a "fake" logical cluster */
		clust->nr_pages = 0;
		return;
	}
	clust->nr_pages = size_in_pages(max(win->off + win->count + win->delta,
					    lbytes(clust->index, inode)));
	return;
}

/* plugin->key_by_inode()
   build key of a disk cluster */
int key_by_inode_cryptcompress(struct inode *inode, loff_t off,
			       reiser4_key * key)
{
	assert("edward-64", inode != 0);

	if (likely(off != get_key_offset(reiser4_max_key())))
		off = off_to_clust_to_off(off, inode);
	if (inode_crypto_info(inode))
		off = inode_scaled_offset(inode, off);

	key_by_inode_and_offset_common(inode, 0, key);
	set_key_offset(key, (__u64)off);
	return 0;
}

/* plugin->flow_by_inode() */
/* flow is used to read/write disk clusters */
int flow_by_inode_cryptcompress(struct inode *inode, const char __user * buf,
				int user,       /* 1: @buf is of user space,
					           0: kernel space */
				loff_t size,    /* @buf size */
				loff_t off,     /* offset to start io from */
				rw_op op,       /* READ or WRITE */
				flow_t * f      /* resulting flow */)
{
	assert("edward-436", f != NULL);
	assert("edward-149", inode != NULL);
	assert("edward-150", inode_file_plugin(inode) != NULL);
	assert("edward-1465", user == 0); /* we use flow to read/write
					    disk clusters located in
					    kernel space */
	f->length = size;
	memcpy(&f->data, &buf, sizeof(buf));
	f->user = user;
	f->op = op;

	return key_by_inode_cryptcompress(inode, off, &f->key);
}

static int
cryptcompress_hint_validate(hint_t * hint, const reiser4_key * key,
			    znode_lock_mode lock_mode)
{
	coord_t *coord;

	assert("edward-704", hint != NULL);
	assert("edward-1089", !hint_is_valid(hint));
	assert("edward-706", hint->lh.owner == NULL);

	coord = &hint->ext_coord.coord;

	if (!hint || !hint_is_set(hint) || hint->mode != lock_mode)
		/* hint either not set or set by different operation */
		return RETERR(-E_REPEAT);

	if (get_key_offset(key) != hint->offset)
		/* hint is set for different key */
		return RETERR(-E_REPEAT);

	assert("edward-707", reiser4_schedulable());

	return reiser4_seal_validate(&hint->seal, &hint->ext_coord.coord,
				     key, &hint->lh, lock_mode,
				     ZNODE_LOCK_LOPRI);
}

/* reserve disk space when writing a logical cluster */
static int reserve4cluster(struct inode *inode, struct cluster_handle *clust)
{
	int result = 0;

	assert("edward-965", reiser4_schedulable());
	assert("edward-439", inode != NULL);
	assert("edward-440", clust != NULL);
	assert("edward-441", clust->pages != NULL);

	if (clust->nr_pages == 0) {
		assert("edward-1152", clust->win != NULL);
		assert("edward-1153", clust->win->stat == HOLE_WINDOW);
		/* don't reserve disk space for fake logical cluster */
		return 0;
	}
	assert("edward-442", jprivate(clust->pages[0]) != NULL);

	result = reiser4_grab_space_force(estimate_insert_cluster(inode) +
					  estimate_update_cluster(inode),
 					  BA_CAN_COMMIT);
	if (result)
		return result;
	clust->reserved = 1;
	grabbed2cluster_reserved(estimate_insert_cluster(inode) +
				 estimate_update_cluster(inode));
#if REISER4_DEBUG
	clust->reserved_prepped = estimate_update_cluster(inode);
	clust->reserved_unprepped = estimate_insert_cluster(inode);
#endif
	/* there can be space grabbed by txnmgr_force_commit_all */
	return 0;
}

/* free reserved disk space if writing a logical cluster fails */
static void free_reserved4cluster(struct inode *inode,
				  struct cluster_handle *ch, int count)
{
	assert("edward-967", ch->reserved == 1);

	cluster_reserved2free(count);
	ch->reserved = 0;
}

/*
 * The core search procedure of the cryptcompress plugin.
 * If returned value is not cbk_errored, then current position
 * is locked.
 */
static int find_cluster_item(hint_t * hint,
			     const reiser4_key * key, /* key of the item we are
							 looking for */
			     znode_lock_mode lock_mode /* which lock */ ,
			     ra_info_t * ra_info, lookup_bias bias, __u32 flags)
{
	int result;
	reiser4_key ikey;
	coord_t *coord = &hint->ext_coord.coord;
	coord_t orig = *coord;

	assert("edward-152", hint != NULL);

	if (!hint_is_valid(hint)) {
		result = cryptcompress_hint_validate(hint, key, lock_mode);
		if (result == -E_REPEAT)
			goto traverse_tree;
		else if (result) {
			assert("edward-1216", 0);
			return result;
		}
		hint_set_valid(hint);
	}
	assert("edward-709", znode_is_any_locked(coord->node));
	/*
	 * Hint is valid, so we perform in-place lookup.
	 * It means we just need to check if the next item in
	 * the tree (relative to the current position @coord)
	 * has key @key.
	 *
	 * Valid hint means in particular, that node is not
	 * empty and at least one its item has been processed
	 */
	if (equal_to_rdk(coord->node, key)) {
		/*
		 * Look for the item in the right neighbor
		 */
		lock_handle lh_right;

		init_lh(&lh_right);
		result = reiser4_get_right_neighbor(&lh_right, coord->node,
				    znode_is_wlocked(coord->node) ?
				    ZNODE_WRITE_LOCK : ZNODE_READ_LOCK,
				    GN_CAN_USE_UPPER_LEVELS);
		if (result) {
			done_lh(&lh_right);
			reiser4_unset_hint(hint);
			if (result == -E_NO_NEIGHBOR)
				return RETERR(-EIO);
			return result;
		}
		assert("edward-1218",
		       equal_to_ldk(lh_right.node, key));
		result = zload(lh_right.node);
		if (result) {
			done_lh(&lh_right);
			reiser4_unset_hint(hint);
			return result;
		}
		coord_init_first_unit_nocheck(coord, lh_right.node);

		if (!coord_is_existing_item(coord)) {
			zrelse(lh_right.node);
			done_lh(&lh_right);
			goto traverse_tree;
		}
		item_key_by_coord(coord, &ikey);
		zrelse(coord->node);
		if (unlikely(!keyeq(key, &ikey))) {
			warning("edward-1608",
				"Expected item not found. Fsck?");
			done_lh(&lh_right);
			goto not_found;
		}
		/*
		 * item has been found in the right neighbor;
		 * move lock to the right
		 */
		done_lh(&hint->lh);
		move_lh(&hint->lh, &lh_right);

		dclust_inc_extension_ncount(hint);

		return CBK_COORD_FOUND;
	} else {
		/*
		 *  Look for the item in the current node
		 */
		coord->item_pos++;
		coord->unit_pos = 0;
		coord->between = AT_UNIT;

		result = zload(coord->node);
		if (result) {
			done_lh(&hint->lh);
			return result;
		}
		if (!coord_is_existing_item(coord)) {
			zrelse(coord->node);
			goto not_found;
		}
		item_key_by_coord(coord, &ikey);
		zrelse(coord->node);
		if (!keyeq(key, &ikey))
			goto not_found;
		/*
		 * item has been found in the current node
		 */
		dclust_inc_extension_ncount(hint);

		return CBK_COORD_FOUND;
	}
 not_found:
	/*
	 * The tree doesn't contain an item with @key;
	 * roll back the coord
	 */
	*coord = orig;
	ON_DEBUG(coord_update_v(coord));
	return CBK_COORD_NOTFOUND;

 traverse_tree:

	reiser4_unset_hint(hint);
	dclust_init_extension(hint);
	coord_init_zero(coord);

	assert("edward-713", hint->lh.owner == NULL);
	assert("edward-714", reiser4_schedulable());

	result = coord_by_key(current_tree, key, coord, &hint->lh,
			      lock_mode, bias, LEAF_LEVEL, LEAF_LEVEL,
			      CBK_UNIQUE | flags, ra_info);
	if (cbk_errored(result))
		return result;
	if(result == CBK_COORD_FOUND)
		dclust_inc_extension_ncount(hint);
	hint_set_valid(hint);
	return result;
}

/* This function is called by deflate[inflate] manager when
   creating a transformed/plain stream to check if we should
   create/cut some overhead. If this returns true, then @oh
   contains the size of this overhead.
 */
static int need_cut_or_align(struct inode * inode,
			     struct cluster_handle * ch, rw_op rw, int * oh)
{
	struct tfm_cluster * tc = &ch->tc;
	switch (rw) {
	case WRITE_OP: /* estimate align */
		*oh = tc->len % cipher_blocksize(inode);
		if (*oh != 0)
			return 1;
		break;
	case READ_OP:  /* estimate cut */
		*oh = *(tfm_output_data(ch) + tc->len - 1);
		break;
	default:
		impossible("edward-1401", "bad option");
	}
	return (tc->len != tc->lsize);
}

/* create/cut an overhead of transformed/plain stream */
static void align_or_cut_overhead(struct inode * inode,
				  struct cluster_handle * ch, rw_op rw)
{
	unsigned int oh;
	cipher_plugin * cplug = inode_cipher_plugin(inode);

	assert("edward-1402", need_cipher(inode));

	if (!need_cut_or_align(inode, ch, rw, &oh))
		return;
	switch (rw) {
	case WRITE_OP: /* do align */
		ch->tc.len +=
			cplug->align_stream(tfm_input_data(ch) +
					    ch->tc.len, ch->tc.len,
					    cipher_blocksize(inode));
		*(tfm_input_data(ch) + ch->tc.len - 1) =
			cipher_blocksize(inode) - oh;
		break;
	case READ_OP: /* do cut */
		assert("edward-1403", oh <= cipher_blocksize(inode));
		ch->tc.len -= oh;
		break;
	default:
		impossible("edward-1404", "bad option");
	}
	return;
}

static unsigned max_cipher_overhead(struct inode * inode)
{
	if (!need_cipher(inode) || !inode_cipher_plugin(inode)->align_stream)
		return 0;
	return cipher_blocksize(inode);
}

static int deflate_overhead(struct inode *inode)
{
	return (inode_compression_plugin(inode)->
		checksum ? DC_CHECKSUM_SIZE : 0);
}

static unsigned deflate_overrun(struct inode * inode, int ilen)
{
	return coa_overrun(inode_compression_plugin(inode), ilen);
}

/* Estimating compressibility of a logical cluster by various
   policies represented by compression mode plugin.
   If this returns false, then compressor won't be called for
   the cluster of index @index.
*/
static int should_compress(struct tfm_cluster * tc, cloff_t index,
			   struct inode *inode)
{
	compression_plugin *cplug = inode_compression_plugin(inode);
	compression_mode_plugin *mplug = inode_compression_mode_plugin(inode);

	assert("edward-1321", tc->len != 0);
	assert("edward-1322", cplug != NULL);
	assert("edward-1323", mplug != NULL);

	return /* estimate by size */
		(cplug->min_size_deflate ?
		 tc->len >= cplug->min_size_deflate() :
		 1) &&
		/* estimate by compression mode plugin */
		(mplug->should_deflate ?
		 mplug->should_deflate(inode, index) :
		 1);
}

/* Evaluating results of compression transform.
   Returns true, if we need to accept this results */
static int save_compressed(int size_before, int size_after, struct inode *inode)
{
	return (size_after + deflate_overhead(inode) +
		max_cipher_overhead(inode) < size_before);
}

/* Guess result of the evaluation above */
static int need_inflate(struct cluster_handle * ch, struct inode * inode,
			int encrypted /* is cluster encrypted */ )
{
	struct tfm_cluster * tc = &ch->tc;

	assert("edward-142", tc != 0);
	assert("edward-143", inode != NULL);

	return tc->len <
	    (encrypted ?
	     inode_scaled_offset(inode, tc->lsize) :
	     tc->lsize);
}

/* If results of compression were accepted, then we add
   a checksum to catch possible disk cluster corruption.
   The following is a format of the data stored in disk clusters:

		   data                   This is (transformed) logical cluster.
		   cipher_overhead        This is created by ->align() method
                                          of cipher plugin. May be absent.
		   checksum          (4)  This is created by ->checksum method
                                          of compression plugin to check
                                          integrity. May be absent.

		   Crypto overhead format:

		   data
		   control_byte      (1)   contains aligned overhead size:
		                           1 <= overhead <= cipher_blksize
*/
/* Append a checksum at the end of a transformed stream */
static void dc_set_checksum(compression_plugin * cplug, struct tfm_cluster * tc)
{
	__u32 checksum;

	assert("edward-1309", tc != NULL);
	assert("edward-1310", tc->len > 0);
	assert("edward-1311", cplug->checksum != NULL);

	checksum = cplug->checksum(tfm_stream_data(tc, OUTPUT_STREAM), tc->len);
	put_unaligned(cpu_to_le32(checksum),
		 (d32 *)(tfm_stream_data(tc, OUTPUT_STREAM) + tc->len));
	tc->len += (int)DC_CHECKSUM_SIZE;
}

/* Check a disk cluster checksum.
   Returns 0 if checksum is correct, otherwise returns 1 */
static int dc_check_checksum(compression_plugin * cplug, struct tfm_cluster * tc)
{
	assert("edward-1312", tc != NULL);
	assert("edward-1313", tc->len > (int)DC_CHECKSUM_SIZE);
	assert("edward-1314", cplug->checksum != NULL);

	if (cplug->checksum(tfm_stream_data(tc, INPUT_STREAM),
			    tc->len - (int)DC_CHECKSUM_SIZE) !=
	    le32_to_cpu(get_unaligned((d32 *)
				      (tfm_stream_data(tc, INPUT_STREAM)
				       + tc->len - (int)DC_CHECKSUM_SIZE)))) {
		warning("edward-156",
			"Bad disk cluster checksum %d, (should be %d) Fsck?\n",
			(int)le32_to_cpu
			(get_unaligned((d32 *)
				       (tfm_stream_data(tc, INPUT_STREAM) +
					tc->len - (int)DC_CHECKSUM_SIZE))),
			(int)cplug->checksum
			(tfm_stream_data(tc, INPUT_STREAM),
			 tc->len - (int)DC_CHECKSUM_SIZE));
		return 1;
	}
	tc->len -= (int)DC_CHECKSUM_SIZE;
	return 0;
}

/* get input/output stream for some transform action */
int grab_tfm_stream(struct inode * inode, struct tfm_cluster * tc,
		    tfm_stream_id id)
{
	size_t size = inode_scaled_cluster_size(inode);

	assert("edward-901", tc != NULL);
	assert("edward-1027", inode_compression_plugin(inode) != NULL);

	if (cluster_get_tfm_act(tc) == TFMA_WRITE)
		size += deflate_overrun(inode, inode_cluster_size(inode));

	if (!get_tfm_stream(tc, id) && id == INPUT_STREAM)
		alternate_streams(tc);
	if (!get_tfm_stream(tc, id))
		return alloc_tfm_stream(tc, size, id);

	assert("edward-902", tfm_stream_is_set(tc, id));

	if (tfm_stream_size(tc, id) < size)
		return realloc_tfm_stream(tc, size, id);
	return 0;
}

/* Common deflate manager */
int reiser4_deflate_cluster(struct cluster_handle * clust, struct inode * inode)
{
	int result = 0;
	int compressed = 0;
	int encrypted = 0;
	struct tfm_cluster * tc = &clust->tc;
	compression_plugin * coplug;

	assert("edward-401", inode != NULL);
	assert("edward-903", tfm_stream_is_set(tc, INPUT_STREAM));
	assert("edward-1348", cluster_get_tfm_act(tc) == TFMA_WRITE);
	assert("edward-498", !tfm_cluster_is_uptodate(tc));

	coplug = inode_compression_plugin(inode);
	if (should_compress(tc, clust->index, inode)) {
		/* try to compress, discard bad results */
		size_t dst_len;
		compression_mode_plugin * mplug =
			inode_compression_mode_plugin(inode);
		assert("edward-602", coplug != NULL);
		assert("edward-1423", coplug->compress != NULL);

		result = grab_coa(tc, coplug);
		if (result) {
		    warning("edward-1424",
			    "alloc_coa failed with ret=%d, skipped compression",
			    result);
		    goto cipher;
		}
		result = grab_tfm_stream(inode, tc, OUTPUT_STREAM);
		if (result) {
		    warning("edward-1425",
			 "alloc stream failed with ret=%d, skipped compression",
			    result);
		    goto cipher;
		}
		dst_len = tfm_stream_size(tc, OUTPUT_STREAM);
		coplug->compress(get_coa(tc, coplug->h.id, tc->act),
				 tfm_input_data(clust), tc->len,
				 tfm_output_data(clust), &dst_len);
		/* make sure we didn't overwrite extra bytes */
		assert("edward-603",
		       dst_len <= tfm_stream_size(tc, OUTPUT_STREAM));

		/* evaluate results of compression transform */
		if (save_compressed(tc->len, dst_len, inode)) {
			/* good result, accept */
			tc->len = dst_len;
			if (mplug->accept_hook != NULL) {
			       result = mplug->accept_hook(inode, clust->index);
			       if (result)
				       warning("edward-1426",
					       "accept_hook failed with ret=%d",
					       result);
			}
			compressed = 1;
		}
		else {
			/* bad result, discard */
#if 0
			if (cluster_is_complete(clust, inode))
			      warning("edward-1496",
				      "incompressible cluster %lu (inode %llu)",
				      clust->index,
				      (unsigned long long)get_inode_oid(inode));
#endif
			if (mplug->discard_hook != NULL &&
			    cluster_is_complete(clust, inode)) {
				result = mplug->discard_hook(inode,
							     clust->index);
				if (result)
				      warning("edward-1427",
					      "discard_hook failed with ret=%d",
					      result);
			}
		}
	}
 cipher:
	if (need_cipher(inode)) {
		cipher_plugin * ciplug;
		struct blkcipher_desc desc;
		struct scatterlist src;
		struct scatterlist dst;

		ciplug = inode_cipher_plugin(inode);
		desc.tfm = info_get_cipher(inode_crypto_info(inode));
		desc.flags = 0;
		if (compressed)
			alternate_streams(tc);
		result = grab_tfm_stream(inode, tc, OUTPUT_STREAM);
		if (result)
			return result;

		align_or_cut_overhead(inode, clust, WRITE_OP);
		sg_init_one(&src, tfm_input_data(clust), tc->len);
		sg_init_one(&dst, tfm_output_data(clust), tc->len);

		result = crypto_blkcipher_encrypt(&desc, &dst, &src, tc->len);
		if (result) {
			warning("edward-1405",
				"encryption failed flags=%x\n", desc.flags);
			return result;
		}
		encrypted = 1;
	}
	if (compressed && coplug->checksum != NULL)
		dc_set_checksum(coplug, tc);
	if (!compressed && !encrypted)
		alternate_streams(tc);
	return result;
}

/* Common inflate manager. */
int reiser4_inflate_cluster(struct cluster_handle * clust, struct inode * inode)
{
	int result = 0;
	int transformed = 0;
	struct tfm_cluster * tc = &clust->tc;
	compression_plugin * coplug;

	assert("edward-905", inode != NULL);
	assert("edward-1178", clust->dstat == PREP_DISK_CLUSTER);
	assert("edward-906", tfm_stream_is_set(&clust->tc, INPUT_STREAM));
	assert("edward-1349", tc->act == TFMA_READ);
	assert("edward-907", !tfm_cluster_is_uptodate(tc));

	/* Handle a checksum (if any) */
	coplug = inode_compression_plugin(inode);
	if (need_inflate(clust, inode, need_cipher(inode)) &&
	    coplug->checksum != NULL) {
		result = dc_check_checksum(coplug, tc);
		if (unlikely(result)) {
			warning("edward-1460",
				"Inode %llu: disk cluster %lu looks corrupted",
				(unsigned long long)get_inode_oid(inode),
				clust->index);
			return RETERR(-EIO);
		}
	}
	if (need_cipher(inode)) {
		cipher_plugin * ciplug;
		struct blkcipher_desc desc;
		struct scatterlist src;
		struct scatterlist dst;

		ciplug = inode_cipher_plugin(inode);
		desc.tfm = info_get_cipher(inode_crypto_info(inode));
		desc.flags = 0;
		result = grab_tfm_stream(inode, tc, OUTPUT_STREAM);
		if (result)
			return result;
		assert("edward-909", tfm_cluster_is_set(tc));

		sg_init_one(&src, tfm_input_data(clust), tc->len);
		sg_init_one(&dst, tfm_output_data(clust), tc->len);

		result = crypto_blkcipher_decrypt(&desc, &dst, &src, tc->len);
		if (result) {
			warning("edward-1600", "decrypt failed flags=%x\n",
				desc.flags);
			return result;
		}
		align_or_cut_overhead(inode, clust, READ_OP);
		transformed = 1;
	}
	if (need_inflate(clust, inode, 0)) {
		size_t dst_len = inode_cluster_size(inode);
		if(transformed)
			alternate_streams(tc);

		result = grab_tfm_stream(inode, tc, OUTPUT_STREAM);
		if (result)
			return result;
		assert("edward-1305", coplug->decompress != NULL);
		assert("edward-910", tfm_cluster_is_set(tc));

		coplug->decompress(get_coa(tc, coplug->h.id, tc->act),
				   tfm_input_data(clust), tc->len,
				   tfm_output_data(clust), &dst_len);
		/* check length */
		tc->len = dst_len;
		assert("edward-157", dst_len == tc->lsize);
		transformed = 1;
	}
	if (!transformed)
		alternate_streams(tc);
	return result;
}

/* This is implementation of readpage method of struct
   address_space_operations for cryptcompress plugin. */
int readpage_cryptcompress(struct file *file, struct page *page)
{
	reiser4_context *ctx;
	struct cluster_handle clust;
	item_plugin *iplug;
	int result;

	assert("edward-88", PageLocked(page));
	assert("vs-976", !PageUptodate(page));
	assert("edward-89", page->mapping && page->mapping->host);

	ctx = reiser4_init_context(page->mapping->host->i_sb);
	if (IS_ERR(ctx)) {
		unlock_page(page);
		return PTR_ERR(ctx);
	}
	assert("edward-113",
	       ergo(file != NULL,
		    page->mapping == file->f_dentry->d_inode->i_mapping));

	if (PageUptodate(page)) {
		warning("edward-1338", "page is already uptodate\n");
		unlock_page(page);
		reiser4_exit_context(ctx);
		return 0;
	}
	cluster_init_read(&clust, NULL);
	clust.file = file;
	iplug = item_plugin_by_id(CTAIL_ID);
	if (!iplug->s.file.readpage) {
		unlock_page(page);
		put_cluster_handle(&clust);
		reiser4_exit_context(ctx);
		return -EINVAL;
	}
	result = iplug->s.file.readpage(&clust, page);

	put_cluster_handle(&clust);
	reiser4_txn_restart(ctx);
	reiser4_exit_context(ctx);
	return result;
}

/* number of pages to check in */
static int get_new_nrpages(struct cluster_handle * clust)
{
	switch (clust->op) {
	case LC_APPOV:
		return clust->nr_pages;
	case LC_TRUNC:
		assert("edward-1179", clust->win != NULL);
		return size_in_pages(clust->win->off + clust->win->count);
	default:
		impossible("edward-1180", "bad page cluster option");
		return 0;
	}
}

static void set_cluster_pages_dirty(struct cluster_handle * clust,
				    struct inode * inode)
{
	int i;
	struct page *pg;
	int nrpages = get_new_nrpages(clust);

	for (i = 0; i < nrpages; i++) {

		pg = clust->pages[i];
		assert("edward-968", pg != NULL);
		lock_page(pg);
		assert("edward-1065", PageUptodate(pg));
		set_page_dirty_notag(pg);
		unlock_page(pg);
		mark_page_accessed(pg);
	}
}

/* Grab a page cluster for read/write operations.
   Attach a jnode for write operations (when preparing for modifications, which
   are supposed to be committed).

   We allocate only one jnode per page cluster; this jnode is binded to the
   first page of this cluster, so we have an extra-reference that will be put
   as soon as jnode is evicted from memory), other references will be cleaned
   up in flush time (assume that check in page cluster was successful).
*/
int grab_page_cluster(struct inode * inode,
		      struct cluster_handle * clust, rw_op rw)
{
	int i;
	int result = 0;
	jnode *node = NULL;

	assert("edward-182", clust != NULL);
	assert("edward-183", clust->pages != NULL);
	assert("edward-1466", clust->node == NULL);
	assert("edward-1428", inode != NULL);
	assert("edward-1429", inode->i_mapping != NULL);
	assert("edward-184", clust->nr_pages <= cluster_nrpages(inode));

	if (clust->nr_pages == 0)
		return 0;

	for (i = 0; i < clust->nr_pages; i++) {

		assert("edward-1044", clust->pages[i] == NULL);

		clust->pages[i] =
		       find_or_create_page(inode->i_mapping,
					   clust_to_pg(clust->index, inode) + i,
					   reiser4_ctx_gfp_mask_get());
		if (!clust->pages[i]) {
			result = RETERR(-ENOMEM);
			break;
		}
		if (i == 0 && rw == WRITE_OP) {
			node = jnode_of_page(clust->pages[i]);
			if (IS_ERR(node)) {
				result = PTR_ERR(node);
				unlock_page(clust->pages[i]);
				break;
			}
			JF_SET(node, JNODE_CLUSTER_PAGE);
			assert("edward-920", jprivate(clust->pages[0]));
		}
		INODE_PGCOUNT_INC(inode);
		unlock_page(clust->pages[i]);
	}
	if (unlikely(result)) {
		while (i) {
			put_cluster_page(clust->pages[--i]);
			INODE_PGCOUNT_DEC(inode);
		}
		if (node && !IS_ERR(node))
			jput(node);
		return result;
	}
	clust->node = node;
	return 0;
}

static void truncate_page_cluster_range(struct inode * inode,
					struct page ** pages,
					cloff_t index,
					int from, int count,
					int even_cows)
{
	assert("edward-1467", count > 0);
	reiser4_invalidate_pages(inode->i_mapping,
				 clust_to_pg(index, inode) + from,
				 count, even_cows);
}

/* Put @count pages starting from @from offset */
void __put_page_cluster(int from, int count,
			struct page ** pages, struct inode  * inode)
{
	int i;
	assert("edward-1468", pages != NULL);
	assert("edward-1469", inode != NULL);
	assert("edward-1470", from >= 0 && count >= 0);

	for (i = 0; i < count; i++) {
		assert("edward-1471", pages[from + i] != NULL);
		assert("edward-1472",
		       pages[from + i]->index == pages[from]->index + i);

		put_cluster_page(pages[from + i]);
		INODE_PGCOUNT_DEC(inode);
	}
}

/*
 * This is dual to grab_page_cluster,
 * however if @rw == WRITE_OP, then we call this function
 * only if something is failed before checkin page cluster.
 */
void put_page_cluster(struct cluster_handle * clust,
		      struct inode * inode, rw_op rw)
{
	assert("edward-445", clust != NULL);
	assert("edward-922", clust->pages != NULL);
	assert("edward-446",
	       ergo(clust->nr_pages != 0, clust->pages[0] != NULL));

	__put_page_cluster(0, clust->nr_pages, clust->pages, inode);
	if (rw == WRITE_OP) {
		if (unlikely(clust->node)) {
			assert("edward-447",
			       clust->node == jprivate(clust->pages[0]));
			jput(clust->node);
			clust->node = NULL;
		}
	}
}

#if REISER4_DEBUG
int cryptcompress_inode_ok(struct inode *inode)
{
	if (!(reiser4_inode_data(inode)->plugin_mask & (1 << PSET_FILE)))
		return 0;
	if (!cluster_shift_ok(inode_cluster_shift(inode)))
		return 0;
	return 1;
}

static int window_ok(struct reiser4_slide * win, struct inode *inode)
{
	assert("edward-1115", win != NULL);
	assert("edward-1116", ergo(win->delta, win->stat == HOLE_WINDOW));

	return (win->off != inode_cluster_size(inode)) &&
	    (win->off + win->count + win->delta <= inode_cluster_size(inode));
}

static int cluster_ok(struct cluster_handle * clust, struct inode *inode)
{
	assert("edward-279", clust != NULL);

	if (!clust->pages)
		return 0;
	return (clust->win ? window_ok(clust->win, inode) : 1);
}
#if 0
static int pages_truncate_ok(struct inode *inode, pgoff_t start)
{
	int found;
	struct page * page;

	found = find_get_pages(inode->i_mapping, start, 1, &page);
	if (found)
		put_cluster_page(page);
	return !found;
}
#else
#define pages_truncate_ok(inode, start) 1
#endif

static int jnode_truncate_ok(struct inode *inode, cloff_t index)
{
	jnode *node;
	node = jlookup(current_tree, get_inode_oid(inode),
		       clust_to_pg(index, inode));
	if (likely(!node))
		return 1;
	jput(node);
	return 0;
}

static int find_fake_appended(struct inode *inode, cloff_t * index);

static int body_truncate_ok(struct inode *inode, cloff_t aidx)
{
	int result;
	cloff_t raidx;

	result = find_fake_appended(inode, &raidx);
	return !result && (aidx == raidx);
}
#endif

/* guess next window stat */
static inline window_stat next_window_stat(struct reiser4_slide * win)
{
	assert("edward-1130", win != NULL);
	return ((win->stat == HOLE_WINDOW && win->delta == 0) ?
		HOLE_WINDOW : DATA_WINDOW);
}

/* guess and set next cluster index and window params */
static void move_update_window(struct inode * inode,
			       struct cluster_handle * clust,
			       loff_t file_off, loff_t to_file)
{
	struct reiser4_slide * win;

	assert("edward-185", clust != NULL);
	assert("edward-438", clust->pages != NULL);
	assert("edward-281", cluster_ok(clust, inode));

	win = clust->win;
	if (!win)
		return;

	switch (win->stat) {
	case DATA_WINDOW:
		/* increment */
		clust->index++;
		win->stat = DATA_WINDOW;
		win->off = 0;
		win->count = min((loff_t)inode_cluster_size(inode), to_file);
		break;
	case HOLE_WINDOW:
		switch (next_window_stat(win)) {
		case HOLE_WINDOW:
			/* skip */
			clust->index = off_to_clust(file_off, inode);
			win->stat = HOLE_WINDOW;
			win->off = 0;
			win->count = off_to_cloff(file_off, inode);
			win->delta = min((loff_t)(inode_cluster_size(inode) -
						  win->count), to_file);
			break;
		case DATA_WINDOW:
			/* stay */
			win->stat = DATA_WINDOW;
			/* off+count+delta=inv */
			win->off = win->off + win->count;
			win->count = win->delta;
			win->delta = 0;
			break;
		default:
			impossible("edward-282", "wrong next window state");
		}
		break;
	default:
		impossible("edward-283", "wrong current window state");
	}
	assert("edward-1068", cluster_ok(clust, inode));
}

static int update_sd_cryptcompress(struct inode *inode)
{
	int result = 0;

	assert("edward-978", reiser4_schedulable());

	result = reiser4_grab_space_force(/* one for stat data update */
					  estimate_update_common(inode),
					  BA_CAN_COMMIT);
	if (result)
		return result;
	if (!IS_NOCMTIME(inode))
		inode->i_ctime = inode->i_mtime = CURRENT_TIME;

	result = reiser4_update_sd(inode);

	if (unlikely(result != 0))
		warning("edward-1573",
			"Can not update stat-data: %i. FSCK?",
			result);
	return result;
}

static void uncapture_cluster_jnode(jnode * node)
{
	txn_atom *atom;

	assert_spin_locked(&(node->guard));

	atom = jnode_get_atom(node);
	if (atom == NULL) {
		assert("jmacd-7111", !JF_ISSET(node, JNODE_DIRTY));
		spin_unlock_jnode(node);
		return;
	}
	reiser4_uncapture_block(node);
	spin_unlock_atom(atom);
	jput(node);
}

static void put_found_pages(struct page **pages, int nr)
{
	int i;
	for (i = 0; i < nr; i++) {
		assert("edward-1045", pages[i] != NULL);
		put_cluster_page(pages[i]);
	}
}

/*             Lifecycle of a logical cluster in the system.
 *
 *
 * Logical cluster of a cryptcompress file is represented in the system by
 * . page cluster (in memory, primary cache, contains plain text);
 * . disk cluster (in memory, secondary cache, contains transformed text).
 * Primary cache is to reduce number of transform operations (compression,
 * encryption), i.e. to implement transform-caching strategy.
 * Secondary cache is to reduce number of I/O operations, i.e. for usual
 * write-caching strategy. Page cluster is a set of pages, i.e. mapping of
 * a logical cluster to the primary cache. Disk cluster is a set of items
 * of the same type defined by some reiser4 item plugin id.
 *
 *              1. Performing modifications
 *
 * Every modification of a cryptcompress file is considered as a set of
 * operations performed on file's logical clusters. Every such "atomic"
 * modification is truncate, append and(or) overwrite some bytes of a
 * logical cluster performed in the primary cache with the following
 * synchronization with the secondary cache (in flush time). Disk clusters,
 * which live in the secondary cache, are supposed to be synchronized with
 * disk. The mechanism of synchronization of primary and secondary caches
 * includes so-called checkin/checkout technique described below.
 *
 *              2. Submitting modifications
 *
 * Each page cluster has associated jnode (a special in-memory header to
 * keep a track of transactions in reiser4), which is attached to its first
 * page when grabbing page cluster for modifications (see grab_page_cluster).
 * Submitting modifications (see checkin_logical_cluster) is going per logical
 * cluster and includes:
 * . checkin_cluster_size;
 * . checkin_page_cluster.
 * checkin_cluster_size() is resolved to file size update (which completely
 * defines new size of logical cluster (number of file's bytes in a logical
 * cluster).
 * checkin_page_cluster() captures jnode of a page cluster and installs
 * jnode's dirty flag (if needed) to indicate that modifications are
 * successfully checked in.
 *
 *              3. Checking out modifications
 *
 * Is going per logical cluster in flush time (see checkout_logical_cluster).
 * This is the time of synchronizing primary and secondary caches.
 * checkout_logical_cluster() includes:
 * . checkout_page_cluster (retrieving checked in pages).
 * . uncapture jnode (including clear dirty flag and unlock)
 *
 *              4. Committing modifications
 *
 * Proceeding a synchronization of primary and secondary caches. When checking
 * out page cluster (the phase above) pages are locked/flushed/unlocked
 * one-by-one in ascending order of their indexes to contiguous stream, which
 * is supposed to be transformed (compressed, encrypted), chopped up into items
 * and committed to disk as a disk cluster.
 *
 *              5. Managing page references
 *
 * Every checked in page have a special additional "control" reference,
 * which is dropped at checkout. We need this to avoid unexpected evicting
 * pages from memory before checkout. Control references are managed so
 * they are not accumulated with every checkin:
 *
 *            0
 * checkin -> 1
 *            0 -> checkout
 * checkin -> 1
 * checkin -> 1
 * checkin -> 1
 *            0 -> checkout
 *           ...
 *
 * Every page cluster has its own unique "cluster lock". Update/drop
 * references are serialized via this lock. Number of checked in cluster
 * pages is calculated by i_size under cluster lock. File size is updated
 * at every checkin action also under cluster lock (except cases of
 * appending/truncating fake logical clusters).
 *
 * Proof of correctness:
 *
 * Since we update file size under cluster lock, in the case of non-fake
 * logical cluster with its lock held we do have expected number of checked
 * in pages. On the other hand, append/truncate of fake logical clusters
 * doesn't change number of checked in pages of any cluster.
 *
 * NOTE-EDWARD: As cluster lock we use guard (spinlock_t) of its jnode.
 * Currently, I don't see any reason to create a special lock for those
 * needs.
 */

static inline void lock_cluster(jnode * node)
{
	spin_lock_jnode(node);
}

static inline void unlock_cluster(jnode * node)
{
	spin_unlock_jnode(node);
}

static inline void unlock_cluster_uncapture(jnode * node)
{
	uncapture_cluster_jnode(node);
}

/* Set new file size by window. Cluster lock is required. */
static void checkin_file_size(struct cluster_handle * clust,
			      struct inode * inode)
{
	loff_t new_size;
	struct reiser4_slide * win;

	assert("edward-1181", clust != NULL);
	assert("edward-1182", inode != NULL);
	assert("edward-1473", clust->pages != NULL);
	assert("edward-1474", clust->pages[0] != NULL);
	assert("edward-1475", jprivate(clust->pages[0]) != NULL);
	assert_spin_locked(&(jprivate(clust->pages[0])->guard));


	win = clust->win;
	assert("edward-1183", win != NULL);

	new_size = clust_to_off(clust->index, inode) + win->off;

	switch (clust->op) {
	case LC_APPOV:
		if (new_size + win->count <= i_size_read(inode))
			/* overwrite only */
			return;
		new_size += win->count;
		break;
	case LC_TRUNC:
		break;
	default:
		impossible("edward-1184", "bad page cluster option");
		break;
	}
	inode_check_scale_nolock(inode, i_size_read(inode), new_size);
	i_size_write(inode, new_size);
	return;
}

static inline void checkin_cluster_size(struct cluster_handle * clust,
					struct inode * inode)
{
	if (clust->win)
		checkin_file_size(clust, inode);
}

static int checkin_page_cluster(struct cluster_handle * clust,
				struct inode * inode)
{
	int result;
	jnode * node;
	int old_nrpages = clust->old_nrpages;
	int new_nrpages = get_new_nrpages(clust);

	node = clust->node;

	assert("edward-221", node != NULL);
	assert("edward-971", clust->reserved == 1);
	assert("edward-1263",
	       clust->reserved_prepped == estimate_update_cluster(inode));
	assert("edward-1264", clust->reserved_unprepped == 0);

	if (JF_ISSET(node, JNODE_DIRTY)) {
		/*
		 * page cluster was checked in, but not yet
		 * checked out, so release related resources
		 */
		free_reserved4cluster(inode, clust,
				      estimate_update_cluster(inode));
		__put_page_cluster(0, clust->old_nrpages,
				   clust->pages, inode);
	} else {
		result = capture_cluster_jnode(node);
		if (unlikely(result)) {
			unlock_cluster(node);
			return result;
		}
		jnode_make_dirty_locked(node);
		clust->reserved = 0;
	}
	unlock_cluster(node);

	if (new_nrpages < old_nrpages) {
		/* truncate >= 1 complete pages */
		__put_page_cluster(new_nrpages,
				   old_nrpages - new_nrpages,
				   clust->pages, inode);
		truncate_page_cluster_range(inode,
					    clust->pages, clust->index,
					    new_nrpages,
					    old_nrpages - new_nrpages,
					    0);
	}
#if REISER4_DEBUG
	clust->reserved_prepped -= estimate_update_cluster(inode);
#endif
	return 0;
}

/* Submit modifications of a logical cluster */
static int checkin_logical_cluster(struct cluster_handle * clust,
				   struct inode *inode)
{
	int result = 0;
	jnode * node;

	node = clust->node;

	assert("edward-1035", node != NULL);
	assert("edward-1029", clust != NULL);
	assert("edward-1030", clust->reserved == 1);
	assert("edward-1031", clust->nr_pages != 0);
	assert("edward-1032", clust->pages != NULL);
	assert("edward-1033", clust->pages[0] != NULL);
	assert("edward-1446", jnode_is_cluster_page(node));
	assert("edward-1476", node == jprivate(clust->pages[0]));

	lock_cluster(node);
	checkin_cluster_size(clust, inode);
	/* this will unlock cluster */
	result = checkin_page_cluster(clust, inode);
	jput(node);
	clust->node = NULL;
	return result;
}

/*
 * Retrieve size of logical cluster that was checked in at
 * the latest modifying session (cluster lock is required)
 */
static inline void checkout_cluster_size(struct cluster_handle * clust,
					 struct inode * inode)
{
	struct tfm_cluster *tc = &clust->tc;

	tc->len = lbytes(clust->index, inode);
	assert("edward-1478", tc->len != 0);
}

/*
 * Retrieve a page cluster with the latest submitted modifications
 * and flush its pages to previously allocated contiguous stream.
 */
static void checkout_page_cluster(struct cluster_handle * clust,
				  jnode * node, struct inode * inode)
{
	int i;
	int found;
	int to_put;
	struct tfm_cluster *tc = &clust->tc;

	/* find and put checked in pages: cluster is locked,
	 * so we must get expected number (to_put) of pages
	 */
	to_put = size_in_pages(lbytes(clust->index, inode));
	found = find_get_pages(inode->i_mapping,
			       clust_to_pg(clust->index, inode),
			       to_put, clust->pages);
	BUG_ON(found != to_put);

	__put_page_cluster(0, to_put, clust->pages, inode);
	unlock_cluster_uncapture(node);

	/* Flush found pages.
	 *
	 * Note, that we don't disable modifications while flushing,
	 * moreover, some found pages can be truncated, as we have
	 * released cluster lock.
	 */
	for (i = 0; i < found; i++) {
		int in_page;
		char * data;
		assert("edward-1479",
		       clust->pages[i]->index == clust->pages[0]->index + i);

		lock_page(clust->pages[i]);
		if (!PageUptodate(clust->pages[i])) {
			/* page was truncated */
			assert("edward-1480",
			       i_size_read(inode) <= page_offset(clust->pages[i]));
			assert("edward-1481",
			       clust->pages[i]->mapping != inode->i_mapping);
			unlock_page(clust->pages[i]);
			break;
		}
		/* Update the number of bytes in the logical cluster,
		 * as it could be partially truncated. Note, that only
		 * partial truncate is possible (complete truncate can
		 * not go here, as it is performed via ->kill_hook()
                 * called by cut_file_items(), and the last one must
                 * wait for znode locked with parent coord).
		 */
		checkout_cluster_size(clust, inode);

		/* this can be zero, as new file size is
		   checked in before truncating pages */
		in_page = __mbp(tc->len, i);

		data = kmap_atomic(clust->pages[i]);
		memcpy(tfm_stream_data(tc, INPUT_STREAM) + pg_to_off(i),
		       data, in_page);
		kunmap_atomic(data);

		if (PageDirty(clust->pages[i]))
			cancel_dirty_page(clust->pages[i], PAGE_CACHE_SIZE);

		unlock_page(clust->pages[i]);

		if (in_page < PAGE_CACHE_SIZE)
			/* end of the file */
			break;
	}
	put_found_pages(clust->pages, found); /* find_get_pages */
	tc->lsize = tc->len;
	return;
}

/* Check out modifications of a logical cluster */
int checkout_logical_cluster(struct cluster_handle * clust,
			     jnode * node, struct inode *inode)
{
	int result;
	struct tfm_cluster *tc = &clust->tc;

	assert("edward-980", node != NULL);
	assert("edward-236", inode != NULL);
	assert("edward-237", clust != NULL);
	assert("edward-240", !clust->win);
	assert("edward-241", reiser4_schedulable());
	assert("edward-718", cryptcompress_inode_ok(inode));

	result = grab_tfm_stream(inode, tc, INPUT_STREAM);
	if (result) {
		warning("edward-1430", "alloc stream failed with ret=%d",
			result);
		return RETERR(-E_REPEAT);
	}
	lock_cluster(node);

 	if (unlikely(!JF_ISSET(node, JNODE_DIRTY))) {
		/* race with another flush */
 		warning("edward-982",
			"checking out logical cluster %lu of inode %llu: "
			"jnode is not dirty", clust->index,
			(unsigned long long)get_inode_oid(inode));
 		unlock_cluster(node);
 		return RETERR(-E_REPEAT);
 	}
	cluster_reserved2grabbed(estimate_update_cluster(inode));

	/* this will unlock cluster */
	checkout_page_cluster(clust, node, inode);
	return 0;
}

/* set hint for the cluster of the index @index */
static void set_hint_cluster(struct inode *inode, hint_t * hint,
			     cloff_t index, znode_lock_mode mode)
{
	reiser4_key key;
	assert("edward-722", cryptcompress_inode_ok(inode));
	assert("edward-723",
	       inode_file_plugin(inode) ==
	       file_plugin_by_id(CRYPTCOMPRESS_FILE_PLUGIN_ID));

	inode_file_plugin(inode)->key_by_inode(inode,
					       clust_to_off(index, inode),
					       &key);

	reiser4_seal_init(&hint->seal, &hint->ext_coord.coord, &key);
	hint->offset = get_key_offset(&key);
	hint->mode = mode;
}

void invalidate_hint_cluster(struct cluster_handle * clust)
{
	assert("edward-1291", clust != NULL);
	assert("edward-1292", clust->hint != NULL);

	done_lh(&clust->hint->lh);
	hint_clr_valid(clust->hint);
}

static void put_hint_cluster(struct cluster_handle * clust,
			     struct inode *inode, znode_lock_mode mode)
{
	assert("edward-1286", clust != NULL);
	assert("edward-1287", clust->hint != NULL);

	set_hint_cluster(inode, clust->hint, clust->index + 1, mode);
	invalidate_hint_cluster(clust);
}

static int balance_dirty_page_cluster(struct cluster_handle * clust,
				      struct inode *inode, loff_t off,
				      loff_t to_file,
				      int nr_dirtied)
{
	int result;
	struct cryptcompress_info * info;

	assert("edward-724", inode != NULL);
	assert("edward-725", cryptcompress_inode_ok(inode));
	assert("edward-1547", nr_dirtied <= cluster_nrpages(inode));

	/* set next window params */
	move_update_window(inode, clust, off, to_file);

	result = update_sd_cryptcompress(inode);
	if (result)
		return result;
	assert("edward-726", clust->hint->lh.owner == NULL);
	info = cryptcompress_inode_data(inode);

	if (nr_dirtied == 0)
		return 0;
	mutex_unlock(&info->checkin_mutex);
	reiser4_throttle_write(inode);
	mutex_lock(&info->checkin_mutex);
	return 0;
}

/* set zeroes to the page cluster, proceed it, and maybe, try to capture
   its pages */
static int write_hole(struct inode *inode, struct cluster_handle * clust,
		      loff_t file_off, loff_t to_file)
{
	int result = 0;
	unsigned cl_off, cl_count = 0;
	unsigned to_pg, pg_off;
	struct reiser4_slide * win;

	assert("edward-190", clust != NULL);
	assert("edward-1069", clust->win != NULL);
	assert("edward-191", inode != NULL);
	assert("edward-727", cryptcompress_inode_ok(inode));
	assert("edward-1171", clust->dstat != INVAL_DISK_CLUSTER);
	assert("edward-1154",
	       ergo(clust->dstat != FAKE_DISK_CLUSTER, clust->reserved == 1));

	win = clust->win;

	assert("edward-1070", win != NULL);
	assert("edward-201", win->stat == HOLE_WINDOW);
	assert("edward-192", cluster_ok(clust, inode));

	if (win->off == 0 && win->count == inode_cluster_size(inode)) {
		/* This part of the hole will be represented by "fake"
		 * logical cluster, i.e. which doesn't have appropriate
		 * disk cluster until someone modify this logical cluster
		 * and make it dirty.
		 * So go forward here..
		 */
		move_update_window(inode, clust, file_off, to_file);
		return 0;
	}
	cl_count = win->count;	/* number of zeroes to write */
	cl_off = win->off;
	pg_off = off_to_pgoff(win->off);

	while (cl_count) {
		struct page *page;
		page = clust->pages[off_to_pg(cl_off)];

		assert("edward-284", page != NULL);

		to_pg = min((typeof(pg_off))PAGE_CACHE_SIZE - pg_off, cl_count);
		lock_page(page);
		zero_user(page, pg_off, to_pg);
		SetPageUptodate(page);
		set_page_dirty_notag(page);
		mark_page_accessed(page);
		unlock_page(page);

		cl_off += to_pg;
		cl_count -= to_pg;
		pg_off = 0;
	}
	if (!win->delta) {
		/* only zeroes in this window, try to capture
		 */
		result = checkin_logical_cluster(clust, inode);
		if (result)
			return result;
		put_hint_cluster(clust, inode, ZNODE_WRITE_LOCK);
		result = balance_dirty_page_cluster(clust,
						    inode, file_off, to_file,
						    win_count_to_nrpages(win));
	} else
		move_update_window(inode, clust, file_off, to_file);
	return result;
}

/*
  The main disk search procedure for cryptcompress plugin, which
  . scans all items of disk cluster with the lock mode @mode
  . maybe reads each one (if @read)
  . maybe makes its znode dirty (if write lock mode was specified)

  NOTE-EDWARD: Callers should handle the case when disk cluster
  is incomplete (-EIO)
*/
int find_disk_cluster(struct cluster_handle * clust,
		      struct inode *inode, int read, znode_lock_mode mode)
{
	flow_t f;
	hint_t *hint;
	int result = 0;
	int was_grabbed;
	ra_info_t ra_info;
	file_plugin *fplug;
	item_plugin *iplug;
	struct tfm_cluster *tc;
	struct cryptcompress_info * info;

	assert("edward-138", clust != NULL);
	assert("edward-728", clust->hint != NULL);
	assert("edward-226", reiser4_schedulable());
	assert("edward-137", inode != NULL);
	assert("edward-729", cryptcompress_inode_ok(inode));

	hint = clust->hint;
	fplug = inode_file_plugin(inode);
	was_grabbed = get_current_context()->grabbed_blocks;
	info = cryptcompress_inode_data(inode);
	tc = &clust->tc;

	assert("edward-462", !tfm_cluster_is_uptodate(tc));
	assert("edward-461", ergo(read, tfm_stream_is_set(tc, INPUT_STREAM)));

	dclust_init_extension(hint);

	/* set key of the first disk cluster item */
	fplug->flow_by_inode(inode,
			     (read ? (char __user *)tfm_stream_data(tc, INPUT_STREAM) : NULL),
			     0 /* kernel space */ ,
			     inode_scaled_cluster_size(inode),
			     clust_to_off(clust->index, inode), READ_OP, &f);
	if (mode == ZNODE_WRITE_LOCK) {
		/* reserve for flush to make dirty all the leaf nodes
		   which contain disk cluster */
		result =
		    reiser4_grab_space_force(estimate_dirty_cluster(inode),
					     BA_CAN_COMMIT);
		if (result)
			goto out;
	}

	ra_info.key_to_stop = f.key;
	set_key_offset(&ra_info.key_to_stop, get_key_offset(reiser4_max_key()));

	while (f.length) {
		result = find_cluster_item(hint, &f.key, mode,
					   NULL, FIND_EXACT,
					   (mode == ZNODE_WRITE_LOCK ?
					    CBK_FOR_INSERT : 0));
		switch (result) {
		case CBK_COORD_NOTFOUND:
			result = 0;
			if (inode_scaled_offset
			    (inode, clust_to_off(clust->index, inode)) ==
			    get_key_offset(&f.key)) {
				/* first item not found, this is treated
				   as disk cluster is absent */
				clust->dstat = FAKE_DISK_CLUSTER;
				goto out;
			}
			/* we are outside the cluster, stop search here */
			assert("edward-146",
			       f.length != inode_scaled_cluster_size(inode));
			goto ok;
		case CBK_COORD_FOUND:
			assert("edward-148",
			       hint->ext_coord.coord.between == AT_UNIT);
			assert("edward-460",
			       hint->ext_coord.coord.unit_pos == 0);

			coord_clear_iplug(&hint->ext_coord.coord);
			result = zload_ra(hint->ext_coord.coord.node, &ra_info);
			if (unlikely(result))
				goto out;
			iplug = item_plugin_by_coord(&hint->ext_coord.coord);
			assert("edward-147",
			       item_id_by_coord(&hint->ext_coord.coord) ==
			       CTAIL_ID);

			result = iplug->s.file.read(NULL, &f, hint);
			if (result) {
				zrelse(hint->ext_coord.coord.node);
				goto out;
			}
			if (mode == ZNODE_WRITE_LOCK) {
				/* Don't make dirty more nodes then it was
				   estimated (see comments before
				   estimate_dirty_cluster). Missed nodes will be
				   read up in flush time if they are evicted from
				   memory */
				if (dclust_get_extension_ncount(hint) <=
				    estimate_dirty_cluster(inode))
				   znode_make_dirty(hint->ext_coord.coord.node);

				znode_set_convertible(hint->ext_coord.coord.
						      node);
			}
			zrelse(hint->ext_coord.coord.node);
			break;
		default:
			goto out;
		}
	}
 ok:
	/* at least one item was found  */
	/* NOTE-EDWARD: Callers should handle the case
	   when disk cluster is incomplete (-EIO) */
	tc->len = inode_scaled_cluster_size(inode) - f.length;
	tc->lsize = lbytes(clust->index, inode);
	assert("edward-1196", tc->len > 0);
	assert("edward-1406", tc->lsize > 0);

	if (hint_is_unprepped_dclust(clust->hint)) {
		clust->dstat = UNPR_DISK_CLUSTER;
	} else if (clust->index == info->trunc_index) {
		clust->dstat = TRNC_DISK_CLUSTER;
	} else {
		clust->dstat = PREP_DISK_CLUSTER;
		dclust_set_extension_dsize(clust->hint, tc->len);
	}
 out:
	assert("edward-1339",
	       get_current_context()->grabbed_blocks >= was_grabbed);
	grabbed2free(get_current_context(),
		     get_current_super_private(),
		     get_current_context()->grabbed_blocks - was_grabbed);
	return result;
}

int get_disk_cluster_locked(struct cluster_handle * clust, struct inode *inode,
			    znode_lock_mode lock_mode)
{
	reiser4_key key;
	ra_info_t ra_info;

	assert("edward-730", reiser4_schedulable());
	assert("edward-731", clust != NULL);
	assert("edward-732", inode != NULL);

	if (hint_is_valid(clust->hint)) {
		assert("edward-1293", clust->dstat != INVAL_DISK_CLUSTER);
		assert("edward-1294",
		       znode_is_write_locked(clust->hint->lh.node));
		/* already have a valid locked position */
		return (clust->dstat ==
			FAKE_DISK_CLUSTER ? CBK_COORD_NOTFOUND :
			CBK_COORD_FOUND);
	}
	key_by_inode_cryptcompress(inode, clust_to_off(clust->index, inode),
				   &key);
	ra_info.key_to_stop = key;
	set_key_offset(&ra_info.key_to_stop, get_key_offset(reiser4_max_key()));

	return find_cluster_item(clust->hint, &key, lock_mode, NULL, FIND_EXACT,
				 CBK_FOR_INSERT);
}

/* Read needed cluster pages before modifying.
   If success, @clust->hint contains locked position in the tree.
   Also:
   . find and set disk cluster state
   . make disk cluster dirty if its state is not FAKE_DISK_CLUSTER.
*/
static int read_some_cluster_pages(struct inode * inode,
				   struct cluster_handle * clust)
{
	int i;
	int result = 0;
	item_plugin *iplug;
	struct reiser4_slide * win = clust->win;
	znode_lock_mode mode = ZNODE_WRITE_LOCK;

	iplug = item_plugin_by_id(CTAIL_ID);

	assert("edward-924", !tfm_cluster_is_uptodate(&clust->tc));

#if REISER4_DEBUG
	if (clust->nr_pages == 0) {
		/* start write hole from fake disk cluster */
		assert("edward-1117", win != NULL);
		assert("edward-1118", win->stat == HOLE_WINDOW);
		assert("edward-1119", new_logical_cluster(clust, inode));
	}
#endif
	if (new_logical_cluster(clust, inode)) {
		/*
		   new page cluster is about to be written, nothing to read,
		 */
		assert("edward-734", reiser4_schedulable());
		assert("edward-735", clust->hint->lh.owner == NULL);

		if (clust->nr_pages) {
			int off;
			struct page * pg;
			assert("edward-1419", clust->pages != NULL);
			pg = clust->pages[clust->nr_pages - 1];
			assert("edward-1420", pg != NULL);
			off = off_to_pgoff(win->off+win->count+win->delta);
			if (off) {
				lock_page(pg);
				zero_user_segment(pg, off, PAGE_CACHE_SIZE);
				unlock_page(pg);
			}
		}
		clust->dstat = FAKE_DISK_CLUSTER;
		return 0;
	}
	/*
	   Here we should search for disk cluster to figure out its real state.
	   Also there is one more important reason to do disk search: we need
	   to make disk cluster _dirty_ if it exists
	 */

	/* if windows is specified, read the only pages
	   that will be modified partially */

	for (i = 0; i < clust->nr_pages; i++) {
		struct page *pg = clust->pages[i];

		lock_page(pg);
		if (PageUptodate(pg)) {
			unlock_page(pg);
			continue;
		}
		unlock_page(pg);

		if (win &&
		    i >= size_in_pages(win->off) &&
		    i < off_to_pg(win->off + win->count + win->delta))
			/* page will be completely overwritten */
			continue;

		if (win && (i == clust->nr_pages - 1) &&
		    /* the last page is
		       partially modified,
		       not uptodate .. */
		    (size_in_pages(i_size_read(inode)) <= pg->index)) {
			/* .. and appended,
			   so set zeroes to the rest */
			int offset;
			lock_page(pg);
			assert("edward-1260",
			       size_in_pages(win->off + win->count +
					     win->delta) - 1 == i);

			offset =
			    off_to_pgoff(win->off + win->count + win->delta);
			zero_user_segment(pg, offset, PAGE_CACHE_SIZE);
			unlock_page(pg);
			/* still not uptodate */
			break;
		}
		lock_page(pg);
		result = do_readpage_ctail(inode, clust, pg, mode);

		assert("edward-1526", ergo(!result, PageUptodate(pg)));
		unlock_page(pg);
		if (result) {
			warning("edward-219", "do_readpage_ctail failed");
			goto out;
		}
	}
	if (!tfm_cluster_is_uptodate(&clust->tc)) {
		/* disk cluster unclaimed, but we need to make its znodes dirty
		 * to make flush update convert its content
		 */
		result = find_disk_cluster(clust, inode,
					   0 /* do not read items */,
					   mode);
	}
 out:
	tfm_cluster_clr_uptodate(&clust->tc);
	return result;
}

static int should_create_unprepped_cluster(struct cluster_handle * clust,
					   struct inode * inode)
{
	assert("edward-737", clust != NULL);

	switch (clust->dstat) {
	case PREP_DISK_CLUSTER:
	case UNPR_DISK_CLUSTER:
		return 0;
	case FAKE_DISK_CLUSTER:
		if (clust->win &&
		    clust->win->stat == HOLE_WINDOW && clust->nr_pages == 0) {
			assert("edward-1172",
			       new_logical_cluster(clust, inode));
			return 0;
		}
		return 1;
	default:
		impossible("edward-1173", "bad disk cluster state");
		return 0;
	}
}

static int cryptcompress_make_unprepped_cluster(struct cluster_handle * clust,
						struct inode *inode)
{
	int result;

	assert("edward-1123", reiser4_schedulable());
	assert("edward-737", clust != NULL);
	assert("edward-738", inode != NULL);
	assert("edward-739", cryptcompress_inode_ok(inode));
	assert("edward-1053", clust->hint != NULL);

 	if (!should_create_unprepped_cluster(clust, inode)) {
 		if (clust->reserved) {
 			cluster_reserved2free(estimate_insert_cluster(inode));
#if REISER4_DEBUG
 			assert("edward-1267",
 			       clust->reserved_unprepped ==
 			       estimate_insert_cluster(inode));
 			clust->reserved_unprepped -=
 				estimate_insert_cluster(inode);
#endif
		}
		return 0;
	}
 	assert("edward-1268", clust->reserved);
 	cluster_reserved2grabbed(estimate_insert_cluster(inode));
#if REISER4_DEBUG
 	assert("edward-1441",
 	       clust->reserved_unprepped == estimate_insert_cluster(inode));
 	clust->reserved_unprepped -= estimate_insert_cluster(inode);
#endif
	result = ctail_insert_unprepped_cluster(clust, inode);
	if (result)
		return result;

	inode_add_bytes(inode, inode_cluster_size(inode));

	assert("edward-743", cryptcompress_inode_ok(inode));
	assert("edward-744", znode_is_write_locked(clust->hint->lh.node));

	clust->dstat = UNPR_DISK_CLUSTER;
	return 0;
}

/* . Grab page cluster for read, write, setattr, etc. operations;
 * . Truncate its complete pages, if needed;
 */
int prepare_page_cluster(struct inode * inode, struct cluster_handle * clust,
			 rw_op rw)
{
	assert("edward-177", inode != NULL);
	assert("edward-741", cryptcompress_inode_ok(inode));
	assert("edward-740", clust->pages != NULL);

	set_cluster_nrpages(clust, inode);
	reset_cluster_pgset(clust, cluster_nrpages(inode));
	return grab_page_cluster(inode, clust, rw);
}

/* Truncate complete page cluster of index @index.
 * This is called by ->kill_hook() method of item
 * plugin when deleting a disk cluster of such index.
 */
void truncate_complete_page_cluster(struct inode *inode, cloff_t index,
				    int even_cows)
{
	int found;
	int nr_pages;
	jnode *node;
	struct page *pages[MAX_CLUSTER_NRPAGES];

	node = jlookup(current_tree, get_inode_oid(inode),
		       clust_to_pg(index, inode));
	nr_pages = size_in_pages(lbytes(index, inode));
	assert("edward-1483", nr_pages != 0);
	if (!node)
		goto truncate;
	found = find_get_pages(inode->i_mapping,
			       clust_to_pg(index, inode),
			       cluster_nrpages(inode), pages);
	if (!found) {
		assert("edward-1484", jnode_truncate_ok(inode, index));
		return;
	}
	lock_cluster(node);

	if (reiser4_inode_get_flag(inode, REISER4_FILE_CONV_IN_PROGRESS)
	    && index == 0)
		/* converting to unix_file is in progress */
		JF_CLR(node, JNODE_CLUSTER_PAGE);
	if (JF_ISSET(node, JNODE_DIRTY)) {
		/*
		 * @nr_pages were checked in, but not yet checked out -
		 * we need to release them. (also there can be pages
		 * attached to page cache by read(), etc. - don't take
		 * them into account).
		 */
		assert("edward-1198", found >= nr_pages);

		/* free disk space grabbed for disk cluster converting */
		cluster_reserved2grabbed(estimate_update_cluster(inode));
		grabbed2free(get_current_context(),
			     get_current_super_private(),
			     estimate_update_cluster(inode));
		__put_page_cluster(0, nr_pages, pages, inode);

		/* This will clear dirty bit, uncapture and unlock jnode */
		unlock_cluster_uncapture(node);
	} else
		unlock_cluster(node);
	jput(node);                         /* jlookup */
	put_found_pages(pages, found); /* find_get_pages */
 truncate:
	if (reiser4_inode_get_flag(inode, REISER4_FILE_CONV_IN_PROGRESS) &&
	    index == 0)
		return;
	truncate_page_cluster_range(inode, pages, index, 0,
				    cluster_nrpages(inode),
				    even_cows);
	assert("edward-1201",
	       ergo(!reiser4_inode_get_flag(inode,
					    REISER4_FILE_CONV_IN_PROGRESS),
		    jnode_truncate_ok(inode, index)));
	return;
}

/*
 * Set cluster handle @clust of a logical cluster before
 * modifications which are supposed to be committed.
 *
 * . grab cluster pages;
 * . reserve disk space;
 * . maybe read pages from disk and set the disk cluster dirty;
 * . maybe write hole and check in (partially zeroed) logical cluster;
 * . create 'unprepped' disk cluster for new or fake logical one.
 */
static int prepare_logical_cluster(struct inode *inode,
				   loff_t file_off, /* write position
						       in the file */
				   loff_t to_file, /* bytes of users data
						      to write to the file */
				   struct cluster_handle * clust,
				   logical_cluster_op op)
{
	int result = 0;
	struct reiser4_slide * win = clust->win;

	reset_cluster_params(clust);
	cluster_set_tfm_act(&clust->tc, TFMA_READ);
#if REISER4_DEBUG
	clust->ctx = get_current_context();
#endif
	assert("edward-1190", op != LC_INVAL);

	clust->op = op;

	result = prepare_page_cluster(inode, clust, WRITE_OP);
	if (result)
		return result;
	assert("edward-1447",
	       ergo(clust->nr_pages != 0, jprivate(clust->pages[0])));
	assert("edward-1448",
	       ergo(clust->nr_pages != 0,
		    jnode_is_cluster_page(jprivate(clust->pages[0]))));

	result = reserve4cluster(inode, clust);
	if (result)
		goto err1;
	result = read_some_cluster_pages(inode, clust);
	if (result) {
		free_reserved4cluster(inode,
				      clust,
				      estimate_update_cluster(inode) +
				      estimate_insert_cluster(inode));
		goto err1;
	}
	assert("edward-1124", clust->dstat != INVAL_DISK_CLUSTER);

	result = cryptcompress_make_unprepped_cluster(clust, inode);
	if (result)
		goto err2;
	if (win && win->stat == HOLE_WINDOW) {
		result = write_hole(inode, clust, file_off, to_file);
		if (result)
			goto err2;
	}
	return 0;
 err2:
	free_reserved4cluster(inode, clust,
			      estimate_update_cluster(inode));
 err1:
	put_page_cluster(clust, inode, WRITE_OP);
	assert("edward-1125", result == -ENOSPC);
	return result;
}

/* set window by two offsets */
static void set_window(struct cluster_handle * clust,
		       struct reiser4_slide * win, struct inode *inode,
		       loff_t o1, loff_t o2)
{
	assert("edward-295", clust != NULL);
	assert("edward-296", inode != NULL);
	assert("edward-1071", win != NULL);
	assert("edward-297", o1 <= o2);

	clust->index = off_to_clust(o1, inode);

	win->off = off_to_cloff(o1, inode);
	win->count = min((loff_t)(inode_cluster_size(inode) - win->off),
			 o2 - o1);
	win->delta = 0;

	clust->win = win;
}

static int set_window_and_cluster(struct inode *inode,
				  struct cluster_handle * clust,
				  struct reiser4_slide * win, size_t length,
				  loff_t file_off)
{
	int result;

	assert("edward-197", clust != NULL);
	assert("edward-1072", win != NULL);
	assert("edward-198", inode != NULL);

	result = alloc_cluster_pgset(clust, cluster_nrpages(inode));
	if (result)
		return result;

	if (file_off > i_size_read(inode)) {
		/* Uhmm, hole in cryptcompress file... */
		loff_t hole_size;
		hole_size = file_off - inode->i_size;

		set_window(clust, win, inode, inode->i_size, file_off);
		win->stat = HOLE_WINDOW;
		if (win->off + hole_size < inode_cluster_size(inode))
			/* there is also user's data to append to the hole */
			win->delta = min(inode_cluster_size(inode) -
					 (win->off + win->count), length);
		return 0;
	}
	set_window(clust, win, inode, file_off, file_off + length);
	win->stat = DATA_WINDOW;
	return 0;
}

int set_cluster_by_page(struct cluster_handle * clust, struct page * page,
			int count)
{
	int result = 0;
	int (*setting_actor)(struct cluster_handle * clust, int count);

	assert("edward-1358", clust != NULL);
	assert("edward-1359", page != NULL);
	assert("edward-1360", page->mapping != NULL);
	assert("edward-1361", page->mapping->host != NULL);

	setting_actor =
		(clust->pages ? reset_cluster_pgset : alloc_cluster_pgset);
	result = setting_actor(clust, count);
	clust->index = pg_to_clust(page->index, page->mapping->host);
	return result;
}

/* reset all the params that not get updated */
void reset_cluster_params(struct cluster_handle * clust)
{
	assert("edward-197", clust != NULL);

	clust->dstat = INVAL_DISK_CLUSTER;
	clust->tc.uptodate = 0;
	clust->tc.len = 0;
}

/* the heart of write_cryptcompress */
static loff_t do_write_cryptcompress(struct file *file, struct inode *inode,
				     const char __user *buf, size_t to_write,
				     loff_t pos, struct dispatch_context *cont)
{
	int i;
	hint_t *hint;
	int result = 0;
	size_t count;
	struct reiser4_slide win;
	struct cluster_handle clust;
	struct cryptcompress_info * info;

	assert("edward-154", buf != NULL);
	assert("edward-161", reiser4_schedulable());
	assert("edward-748", cryptcompress_inode_ok(inode));
	assert("edward-159", current_blocksize == PAGE_CACHE_SIZE);
	assert("edward-1274", get_current_context()->grabbed_blocks == 0);

	hint = kmalloc(sizeof(*hint), reiser4_ctx_gfp_mask_get());
	if (hint == NULL)
		return RETERR(-ENOMEM);

	result = load_file_hint(file, hint);
	if (result) {
		kfree(hint);
		return result;
	}
	count = to_write;

	reiser4_slide_init(&win);
	cluster_init_read(&clust, &win);
	clust.hint = hint;
	info = cryptcompress_inode_data(inode);

	mutex_lock(&info->checkin_mutex);

	result = set_window_and_cluster(inode, &clust, &win, to_write, pos);
	if (result)
		goto out;

	if (next_window_stat(&win) == HOLE_WINDOW) {
		/* write hole in this iteration
		   separated from the loop below */
		result = write_dispatch_hook(file, inode,
					     pos, &clust, cont);
		if (result)
			goto out;
		result = prepare_logical_cluster(inode, pos, count, &clust,
						 LC_APPOV);
		if (result)
			goto out;
	}
	do {
		const char __user * src;
		unsigned page_off, to_page;

		assert("edward-750", reiser4_schedulable());

		result = write_dispatch_hook(file, inode,
					     pos + to_write - count,
					     &clust, cont);
		if (result)
			goto out;
		if (cont->state == DISPATCH_ASSIGNED_NEW)
			/* done_lh was called in write_dispatch_hook */
			goto out_no_longterm_lock;

		result = prepare_logical_cluster(inode, pos, count, &clust,
						 LC_APPOV);
		if (result)
			goto out;

		assert("edward-751", cryptcompress_inode_ok(inode));
		assert("edward-204", win.stat == DATA_WINDOW);
		assert("edward-1288", hint_is_valid(clust.hint));
		assert("edward-752",
		       znode_is_write_locked(hint->ext_coord.coord.node));
		put_hint_cluster(&clust, inode, ZNODE_WRITE_LOCK);

		/* set write position in page */
		page_off = off_to_pgoff(win.off);

		/* copy user's data to cluster pages */
		for (i = off_to_pg(win.off), src = buf;
		     i < size_in_pages(win.off + win.count);
		     i++, src += to_page) {
			to_page = __mbp(win.off + win.count, i) - page_off;
			assert("edward-1039",
			       page_off + to_page <= PAGE_CACHE_SIZE);
			assert("edward-287", clust.pages[i] != NULL);

			fault_in_pages_readable(src, to_page);

			lock_page(clust.pages[i]);
			result =
			    __copy_from_user((char *)kmap(clust.pages[i]) +
					     page_off, src, to_page);
			kunmap(clust.pages[i]);
			if (unlikely(result)) {
				unlock_page(clust.pages[i]);
				result = -EFAULT;
				goto err2;
			}
			SetPageUptodate(clust.pages[i]);
			set_page_dirty_notag(clust.pages[i]);
			flush_dcache_page(clust.pages[i]);
			mark_page_accessed(clust.pages[i]);
			unlock_page(clust.pages[i]);
			page_off = 0;
		}
		assert("edward-753", cryptcompress_inode_ok(inode));

		result = checkin_logical_cluster(&clust, inode);
		if (result)
			goto err2;

		buf   += win.count;
		count -= win.count;

		result = balance_dirty_page_cluster(&clust, inode, 0, count,
						    win_count_to_nrpages(&win));
		if (result)
			goto err1;
		assert("edward-755", hint->lh.owner == NULL);
		reset_cluster_params(&clust);
		continue;
	err2:
		put_page_cluster(&clust, inode, WRITE_OP);
	err1:
		if (clust.reserved)
			free_reserved4cluster(inode,
					      &clust,
					      estimate_update_cluster(inode));
		break;
	} while (count);
 out:
	done_lh(&hint->lh);
	save_file_hint(file, hint);
 out_no_longterm_lock:
	mutex_unlock(&info->checkin_mutex);
	kfree(hint);
	put_cluster_handle(&clust);
	assert("edward-195",
	       ergo((to_write == count),
		    (result < 0 || cont->state == DISPATCH_ASSIGNED_NEW)));
	return (to_write - count) ? (to_write - count) : result;
}

/**
 * plugin->write()
 * @file: file to write to
 * @buf: address of user-space buffer
 * @read_amount: number of bytes to write
 * @off: position in file to write to
 */
ssize_t write_cryptcompress(struct file *file, const char __user *buf,
			    size_t count, loff_t *off,
			    struct dispatch_context *cont)
{
	ssize_t result;
	struct inode *inode;
	reiser4_context *ctx;
  	loff_t pos = *off;
  	struct cryptcompress_info *info;

  	assert("edward-1449", cont->state == DISPATCH_INVAL_STATE);

	inode = file->f_dentry->d_inode;
	assert("edward-196", cryptcompress_inode_ok(inode));

	info = cryptcompress_inode_data(inode);
	ctx = get_current_context();

	result = generic_write_checks(file, &pos, &count, 0);
  	if (unlikely(result != 0)) {
		context_set_commit_async(ctx);
		return result;
	}
  	if (unlikely(count == 0))
		return 0;
	result = file_remove_suid(file);
	if (unlikely(result != 0)) {
		context_set_commit_async(ctx);
		return result;
	}
	/* remove_suid might create a transaction */
	reiser4_txn_restart(ctx);

	result = do_write_cryptcompress(file, inode, buf, count, pos, cont);

  	if (unlikely(result < 0)) {
		context_set_commit_async(ctx);
		return result;
	}
  	/* update position in a file */
  	*off = pos + result;
	return result;
}

/* plugin->readpages */
int readpages_cryptcompress(struct file *file, struct address_space *mapping,
			    struct list_head *pages, unsigned nr_pages)
{
	reiser4_context * ctx;
	int ret;

	ctx = reiser4_init_context(mapping->host->i_sb);
	if (IS_ERR(ctx)) {
		ret = PTR_ERR(ctx);
		goto err;
	}
	/* cryptcompress file can be built of ctail items only */
	ret = readpages_ctail(file, mapping, pages);
	reiser4_txn_restart(ctx);
	reiser4_exit_context(ctx);
	if (ret) {
err:
		put_pages_list(pages);
	}
	return ret;
}

static reiser4_block_nr cryptcompress_estimate_read(struct inode *inode)
{
	/* reserve one block to update stat data item */
	assert("edward-1193",
	       inode_file_plugin(inode)->estimate.update ==
	       estimate_update_common);
	return estimate_update_common(inode);
}

/**
 * plugin->read
 * @file: file to read from
 * @buf: address of user-space buffer
 * @read_amount: number of bytes to read
 * @off: position in file to read from
 */
ssize_t read_cryptcompress(struct file * file, char __user *buf, size_t size,
			   loff_t * off)
{
	ssize_t result;
	struct inode *inode;
	reiser4_context *ctx;
	struct cryptcompress_info *info;
	reiser4_block_nr needed;

	inode = file->f_dentry->d_inode;
	assert("edward-1194", !reiser4_inode_get_flag(inode, REISER4_NO_SD));

	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);

	info = cryptcompress_inode_data(inode);
	needed = cryptcompress_estimate_read(inode);

	result = reiser4_grab_space(needed, BA_CAN_COMMIT);
	if (result != 0) {
		reiser4_exit_context(ctx);
		return result;
	}
	result = do_sync_read(file, buf, size, off);

	context_set_commit_async(ctx);
	reiser4_exit_context(ctx);

	return result;
}

/* Look for a disk cluster and keep lookup result in @found.
 * If @index > 0, then find disk cluster of the index (@index - 1);
 * If @index == 0, then find the rightmost disk cluster.
 * Keep incremented index of the found disk cluster in @found.
 * @found == 0 means that disk cluster was not found (in the last
 * case (@index == 0) it means that file doesn't have disk clusters).
 */
static int lookup_disk_cluster(struct inode *inode, cloff_t * found,
			       cloff_t index)
{
	int result;
	reiser4_key key;
	loff_t offset;
	hint_t *hint;
	lock_handle *lh;
	lookup_bias bias;
	coord_t *coord;
	item_plugin *iplug;

	assert("edward-1131", inode != NULL);
	assert("edward-95", cryptcompress_inode_ok(inode));

	hint = kmalloc(sizeof(*hint), reiser4_ctx_gfp_mask_get());
	if (hint == NULL)
		return RETERR(-ENOMEM);
	hint_init_zero(hint);
	lh = &hint->lh;

	bias = (index ? FIND_EXACT : FIND_MAX_NOT_MORE_THAN);
	offset =
	    (index ? clust_to_off(index, inode) -
	     1 : get_key_offset(reiser4_max_key()));

	key_by_inode_cryptcompress(inode, offset, &key);

	/* find the last item of this object */
	result =
	    find_cluster_item(hint, &key, ZNODE_READ_LOCK, NULL /* ra_info */,
			      bias, 0);
	if (cbk_errored(result)) {
		done_lh(lh);
		kfree(hint);
		return result;
	}
	if (result == CBK_COORD_NOTFOUND) {
		/* no real disk clusters */
		done_lh(lh);
		kfree(hint);
		*found = 0;
		return 0;
	}
	/* disk cluster is found */
	coord = &hint->ext_coord.coord;
	coord_clear_iplug(coord);
	result = zload(coord->node);
	if (unlikely(result)) {
		done_lh(lh);
		kfree(hint);
		return result;
	}
	iplug = item_plugin_by_coord(coord);
	assert("edward-277", iplug == item_plugin_by_id(CTAIL_ID));
	assert("edward-1202", ctail_ok(coord));

	item_key_by_coord(coord, &key);
	*found = off_to_clust(get_key_offset(&key), inode) + 1;

	assert("edward-1132", ergo(index, index == *found));

	zrelse(coord->node);
	done_lh(lh);
	kfree(hint);
	return 0;
}

static int find_fake_appended(struct inode *inode, cloff_t * index)
{
	return lookup_disk_cluster(inode, index,
				   0 /* find last real one */ );
}

/* Set left coord when unit is not found after node_lookup()
   This takes into account that there can be holes in a sequence
   of disk clusters */

static void adjust_left_coord(coord_t * left_coord)
{
	switch (left_coord->between) {
	case AFTER_UNIT:
		left_coord->between = AFTER_ITEM;
	case AFTER_ITEM:
	case BEFORE_UNIT:
		break;
	default:
		impossible("edward-1204", "bad left coord to cut");
	}
	return;
}

#define CRC_CUT_TREE_MIN_ITERATIONS 64

/* plugin->cut_tree_worker */
int cut_tree_worker_cryptcompress(tap_t * tap, const reiser4_key * from_key,
				  const reiser4_key * to_key,
				  reiser4_key * smallest_removed,
				  struct inode *object, int truncate,
				  int *progress)
{
	lock_handle next_node_lock;
	coord_t left_coord;
	int result;

	assert("edward-1158", tap->coord->node != NULL);
	assert("edward-1159", znode_is_write_locked(tap->coord->node));
	assert("edward-1160", znode_get_level(tap->coord->node) == LEAF_LEVEL);

	*progress = 0;
	init_lh(&next_node_lock);

	while (1) {
		znode *node;	/* node from which items are cut */
		node_plugin *nplug;	/* node plugin for @node */

		node = tap->coord->node;

		/* Move next_node_lock to the next node on the left. */
		result =
		    reiser4_get_left_neighbor(&next_node_lock, node,
					      ZNODE_WRITE_LOCK,
					      GN_CAN_USE_UPPER_LEVELS);
		if (result != 0 && result != -E_NO_NEIGHBOR)
			break;
		/* FIXME-EDWARD: Check can we delete the node as a whole. */
		result = reiser4_tap_load(tap);
		if (result)
			return result;

		/* Prepare the second (right) point for cut_node() */
		if (*progress)
			coord_init_last_unit(tap->coord, node);

		else if (item_plugin_by_coord(tap->coord)->b.lookup == NULL)
			/* set rightmost unit for the items without lookup method */
			tap->coord->unit_pos = coord_last_unit_pos(tap->coord);

		nplug = node->nplug;

		assert("edward-1161", nplug);
		assert("edward-1162", nplug->lookup);

		/* left_coord is leftmost unit cut from @node */
		result = nplug->lookup(node, from_key, FIND_EXACT, &left_coord);

		if (IS_CBKERR(result))
			break;

		if (result == CBK_COORD_NOTFOUND)
			adjust_left_coord(&left_coord);

		/* adjust coordinates so that they are set to existing units */
		if (coord_set_to_right(&left_coord)
		    || coord_set_to_left(tap->coord)) {
			result = 0;
			break;
		}

		if (coord_compare(&left_coord, tap->coord) ==
		    COORD_CMP_ON_RIGHT) {
			/* keys from @from_key to @to_key are not in the tree */
			result = 0;
			break;
		}

		/* cut data from one node */
		*smallest_removed = *reiser4_min_key();
		result = kill_node_content(&left_coord,
					   tap->coord,
					   from_key,
					   to_key,
					   smallest_removed,
					   next_node_lock.node,
					   object, truncate);
		reiser4_tap_relse(tap);

		if (result)
			break;

		++(*progress);

		/* Check whether all items with keys >= from_key were removed
		 * from the tree. */
		if (keyle(smallest_removed, from_key))
			/* result = 0; */
			break;

		if (next_node_lock.node == NULL)
			break;

		result = reiser4_tap_move(tap, &next_node_lock);
		done_lh(&next_node_lock);
		if (result)
			break;

		/* Break long cut_tree operation (deletion of a large file) if
		 * atom requires commit. */
		if (*progress > CRC_CUT_TREE_MIN_ITERATIONS
		    && current_atom_should_commit()) {
			result = -E_REPEAT;
			break;
		}
	}
	done_lh(&next_node_lock);
	return result;
}

/* Append or expand hole in two steps:
 * 1) set zeroes to the rightmost page of the rightmost non-fake
 *    logical cluster;
 * 2) expand hole via fake logical clusters (just increase i_size)
 */
static int cryptcompress_append_hole(struct inode *inode /* with old size */,
				     loff_t new_size)
{
	int result = 0;
	hint_t *hint;
	lock_handle *lh;
	loff_t hole_size;
	int nr_zeroes;
	struct reiser4_slide win;
	struct cluster_handle clust;

	assert("edward-1133", inode->i_size < new_size);
	assert("edward-1134", reiser4_schedulable());
	assert("edward-1135", cryptcompress_inode_ok(inode));
	assert("edward-1136", current_blocksize == PAGE_CACHE_SIZE);
	assert("edward-1333", off_to_cloff(inode->i_size, inode) != 0);

	hint = kmalloc(sizeof(*hint), reiser4_ctx_gfp_mask_get());
	if (hint == NULL)
		return RETERR(-ENOMEM);
	hint_init_zero(hint);
	lh = &hint->lh;

	reiser4_slide_init(&win);
	cluster_init_read(&clust, &win);
	clust.hint = hint;

	result = alloc_cluster_pgset(&clust, cluster_nrpages(inode));
	if (result)
		goto out;
	if (off_to_cloff(inode->i_size, inode) == 0)
		goto append_fake;
	hole_size = new_size - inode->i_size;
	nr_zeroes =
		inode_cluster_size(inode) - off_to_cloff(inode->i_size, inode);
	if (hole_size < nr_zeroes)
		nr_zeroes = hole_size;
	set_window(&clust, &win, inode, inode->i_size,
		   inode->i_size + nr_zeroes);
	win.stat = HOLE_WINDOW;

	assert("edward-1137",
	       clust.index == off_to_clust(inode->i_size, inode));

	result = prepare_logical_cluster(inode, 0, 0, &clust, LC_APPOV);

	assert("edward-1271", !result || result == -ENOSPC);
	if (result)
		goto out;
	assert("edward-1139",
	       clust.dstat == PREP_DISK_CLUSTER ||
	       clust.dstat == UNPR_DISK_CLUSTER);

	assert("edward-1431", hole_size >= nr_zeroes);
	if (hole_size == nr_zeroes)
	/* nothing to append anymore */
		goto out;
 append_fake:
	INODE_SET_SIZE(inode, new_size);
 out:
	done_lh(lh);
	kfree(hint);
	put_cluster_handle(&clust);
	return result;
}

static int update_cryptcompress_size(struct inode *inode, loff_t new_size,
				     int update_sd)
{
	return (new_size & ((loff_t) (inode_cluster_size(inode)) - 1)
		? 0 : reiser4_update_file_size(inode, new_size, update_sd));
}

/* Prune cryptcompress file in two steps:
 * 1) cut all nominated logical clusters except the leftmost one which
 *    is to be partially truncated. Note, that there can be "holes"
 *    represented by fake logical clusters.
 * 2) set zeroes and capture leftmost partially truncated logical
 *    cluster, if it is not fake; otherwise prune fake logical cluster
 *    (just decrease i_size).
 */
static int prune_cryptcompress(struct inode *inode, loff_t new_size,
			       int update_sd, cloff_t aidx)
{
	int result = 0;
	unsigned nr_zeroes;
	loff_t to_prune;
	loff_t old_size;
	cloff_t ridx;

	hint_t *hint;
	lock_handle *lh;
	struct reiser4_slide win;
	struct cluster_handle clust;

	assert("edward-1140", inode->i_size >= new_size);
	assert("edward-1141", reiser4_schedulable());
	assert("edward-1142", cryptcompress_inode_ok(inode));
	assert("edward-1143", current_blocksize == PAGE_CACHE_SIZE);

	old_size = inode->i_size;

	hint = kmalloc(sizeof(*hint), reiser4_ctx_gfp_mask_get());
	if (hint == NULL)
		return RETERR(-ENOMEM);
	hint_init_zero(hint);
	lh = &hint->lh;

	reiser4_slide_init(&win);
	cluster_init_read(&clust, &win);
	clust.hint = hint;

	/* calculate index of the rightmost logical cluster
	   that will be completely truncated */
	ridx = size_in_lc(new_size, inode);

	/* truncate all disk clusters starting from @ridx */
	assert("edward-1174", ridx <= aidx);
	old_size = inode->i_size;
	if (ridx != aidx) {
		struct cryptcompress_info * info;
		info = cryptcompress_inode_data(inode);
		result = cut_file_items(inode,
					clust_to_off(ridx, inode),
					update_sd,
					clust_to_off(aidx, inode),
					update_cryptcompress_size);
		info->trunc_index = ULONG_MAX;
		if (result)
			goto out;
	}
	/*
	 * there can be pages of fake logical clusters, truncate them
	 */
	truncate_inode_pages(inode->i_mapping, clust_to_off(ridx, inode));
	assert("edward-1524",
	       pages_truncate_ok(inode, clust_to_pg(ridx, inode)));
	/*
	 * now perform partial truncate of last logical cluster
	 */
	if (!off_to_cloff(new_size, inode)) {
		/* no partial truncate is needed */
		assert("edward-1145", inode->i_size == new_size);
		goto truncate_fake;
	}
	assert("edward-1146", new_size < inode->i_size);

	to_prune = inode->i_size - new_size;

	/* check if the last logical cluster is fake */
	result = lookup_disk_cluster(inode, &aidx, ridx);
	if (result)
		goto out;
	if (!aidx)
		/* yup, this is fake one */
		goto truncate_fake;

	assert("edward-1148", aidx == ridx);

	/* do partial truncate of the last page cluster,
	   and try to capture this one */
	result = alloc_cluster_pgset(&clust, cluster_nrpages(inode));
	if (result)
		goto out;
	nr_zeroes = (off_to_pgoff(new_size) ?
		     PAGE_CACHE_SIZE - off_to_pgoff(new_size) : 0);
	set_window(&clust, &win, inode, new_size, new_size + nr_zeroes);
	win.stat = HOLE_WINDOW;

	assert("edward-1149", clust.index == ridx - 1);

	result = prepare_logical_cluster(inode, 0, 0, &clust, LC_TRUNC);
	if (result)
		goto out;
	assert("edward-1151",
	       clust.dstat == PREP_DISK_CLUSTER ||
	       clust.dstat == UNPR_DISK_CLUSTER);

	assert("edward-1191", inode->i_size == new_size);
	assert("edward-1206", body_truncate_ok(inode, ridx));
 truncate_fake:
	/* drop all the pages that don't have jnodes (i.e. pages
	   which can not be truncated by cut_file_items() because
	   of holes represented by fake disk clusters) including
	   the pages of partially truncated cluster which was
	   released by prepare_logical_cluster() */
	INODE_SET_SIZE(inode, new_size);
	truncate_inode_pages(inode->i_mapping, new_size);
 out:
	assert("edward-1334", !result || result == -ENOSPC);
	assert("edward-1497",
	       pages_truncate_ok(inode, size_in_pages(new_size)));

	done_lh(lh);
	kfree(hint);
	put_cluster_handle(&clust);
	return result;
}

/* Prepare cryptcompress file for truncate:
 * prune or append rightmost fake logical clusters (if any)
 */
static int start_truncate_fake(struct inode *inode, cloff_t aidx,
			       loff_t new_size, int update_sd)
{
	int result = 0;
	int bytes;

	if (new_size > inode->i_size) {
		/* append */
		if (inode->i_size < clust_to_off(aidx, inode))
			/* no fake bytes */
			return 0;
		bytes = new_size - inode->i_size;
		INODE_SET_SIZE(inode, inode->i_size + bytes);
	} else {
		/* prune */
		if (inode->i_size <= clust_to_off(aidx, inode))
			/* no fake bytes */
			return 0;
		bytes = inode->i_size -
			max(new_size, clust_to_off(aidx, inode));
		if (!bytes)
			return 0;
		INODE_SET_SIZE(inode, inode->i_size - bytes);
		/* In the case of fake prune we need to drop page cluster.
		   There are only 2 cases for partially truncated page:
		   1. If is is dirty, therefore it is anonymous
		   (was dirtied via mmap), and will be captured
		   later via ->capture().
		   2. If is clean, therefore it is filled by zeroes.
		   In both cases we don't need to make it dirty and
		   capture here.
		 */
		truncate_inode_pages(inode->i_mapping, inode->i_size);
	}
	if (update_sd)
		result = update_sd_cryptcompress(inode);
	return result;
}

/**
 * This is called in setattr_cryptcompress when it is used to truncate,
 * and in delete_object_cryptcompress
 */
static int cryptcompress_truncate(struct inode *inode,	/* old size */
				  loff_t new_size,	/* new size */
				  int update_sd)
{
	int result;
	cloff_t aidx;

	result = find_fake_appended(inode, &aidx);
	if (result)
		return result;
	assert("edward-1208",
	       ergo(aidx > 0, inode->i_size > clust_to_off(aidx - 1, inode)));

	result = start_truncate_fake(inode, aidx, new_size, update_sd);
	if (result)
		return result;
	if (inode->i_size == new_size)
		/* nothing to truncate anymore */
		return 0;
	result = (inode->i_size < new_size ?
		  cryptcompress_append_hole(inode, new_size) :
		  prune_cryptcompress(inode, new_size, update_sd, aidx));
	if (!result && update_sd)
		result = update_sd_cryptcompress(inode);
	return result;
}

/**
 * Capture a pager cluster.
 * @clust must be set up by a caller.
 */
static int capture_page_cluster(struct cluster_handle * clust,
				struct inode * inode)
{
	int result;

	assert("edward-1073", clust != NULL);
	assert("edward-1074", inode != NULL);
	assert("edward-1075", clust->dstat == INVAL_DISK_CLUSTER);

	result = prepare_logical_cluster(inode, 0, 0, clust, LC_APPOV);
	if (result)
		return result;

	set_cluster_pages_dirty(clust, inode);
	result = checkin_logical_cluster(clust, inode);
	put_hint_cluster(clust, inode, ZNODE_WRITE_LOCK);
	if (unlikely(result))
		put_page_cluster(clust, inode, WRITE_OP);
	return result;
}

/* Starting from @index find tagged pages of the same page cluster.
 * Clear the tag for each of them. Return number of found pages.
 */
static int find_anon_page_cluster(struct address_space * mapping,
				  pgoff_t * index, struct page ** pages)
{
	int i = 0;
	int found;
	spin_lock_irq(&mapping->tree_lock);
	do {
		/* looking for one page */
		found = radix_tree_gang_lookup_tag(&mapping->page_tree,
						   (void **)&pages[i],
						   *index, 1,
						   PAGECACHE_TAG_REISER4_MOVED);
		if (!found)
			break;
		if (!same_page_cluster(pages[0], pages[i]))
			break;

		/* found */
		page_cache_get(pages[i]);
		*index = pages[i]->index + 1;

		radix_tree_tag_clear(&mapping->page_tree,
				     pages[i]->index,
				     PAGECACHE_TAG_REISER4_MOVED);
		if (last_page_in_cluster(pages[i++]))
			break;
	} while (1);
	spin_unlock_irq(&mapping->tree_lock);
	return i;
}

#define MAX_PAGES_TO_CAPTURE  (1024)

/* Capture anonymous page clusters */
static int capture_anon_pages(struct address_space * mapping, pgoff_t * index,
			      int to_capture)
{
	int count = 0;
	int found = 0;
	int result = 0;
	hint_t *hint;
	lock_handle *lh;
	struct inode * inode;
	struct cluster_handle clust;
	struct page * pages[MAX_CLUSTER_NRPAGES];

	assert("edward-1127", mapping != NULL);
	assert("edward-1128", mapping->host != NULL);
	assert("edward-1440", mapping->host->i_mapping == mapping);

	inode = mapping->host;
	hint = kmalloc(sizeof(*hint), reiser4_ctx_gfp_mask_get());
	if (hint == NULL)
		return RETERR(-ENOMEM);
	hint_init_zero(hint);
	lh = &hint->lh;

	cluster_init_read(&clust, NULL);
	clust.hint = hint;

	result = alloc_cluster_pgset(&clust, cluster_nrpages(inode));
	if (result)
		goto out;

	while (to_capture > 0) {
		found = find_anon_page_cluster(mapping, index, pages);
		if (!found) {
			*index = (pgoff_t) - 1;
			break;
		}
		move_cluster_forward(&clust, inode, pages[0]->index);
		result = capture_page_cluster(&clust, inode);

		put_found_pages(pages, found); /* find_anon_page_cluster */
		if (result)
			break;
		to_capture -= clust.nr_pages;
		count += clust.nr_pages;
	}
	if (result) {
		warning("edward-1077",
			"Capture failed (inode %llu, result=%i, captured=%d)\n",
			(unsigned long long)get_inode_oid(inode), result, count);
	} else {
		assert("edward-1078", ergo(found > 0, count > 0));
		if (to_capture <= 0)
			/* there may be left more pages */
			__mark_inode_dirty(inode, I_DIRTY_PAGES);
		result = count;
	}
      out:
	done_lh(lh);
	kfree(hint);
	put_cluster_handle(&clust);
	return result;
}

/* Returns true if inode's mapping has dirty pages
   which do not belong to any atom */
static int cryptcompress_inode_has_anon_pages(struct inode *inode)
{
	int result;
	spin_lock_irq(&inode->i_mapping->tree_lock);
	result = radix_tree_tagged(&inode->i_mapping->page_tree,
				   PAGECACHE_TAG_REISER4_MOVED);
	spin_unlock_irq(&inode->i_mapping->tree_lock);
	return result;
}

/* plugin->writepages */
int writepages_cryptcompress(struct address_space *mapping,
			     struct writeback_control *wbc)
{
	int result = 0;
	long to_capture;
	pgoff_t nrpages;
	pgoff_t index = 0;
	struct inode *inode;
	struct cryptcompress_info *info;

	inode = mapping->host;
	if (!cryptcompress_inode_has_anon_pages(inode))
		goto end;
	info = cryptcompress_inode_data(inode);
	nrpages = size_in_pages(i_size_read(inode));

	if (wbc->sync_mode != WB_SYNC_ALL)
		to_capture = min(wbc->nr_to_write, (long)MAX_PAGES_TO_CAPTURE);
	else
		to_capture = MAX_PAGES_TO_CAPTURE;
	do {
		reiser4_context *ctx;

		ctx = reiser4_init_context(inode->i_sb);
		if (IS_ERR(ctx)) {
			result = PTR_ERR(ctx);
			break;
		}
		/* avoid recursive calls to ->sync_inodes */
		ctx->nobalance = 1;

		assert("edward-1079",
		       lock_stack_isclean(get_current_lock_stack()));

		reiser4_txn_restart_current();

		if (get_current_context()->entd) {
			if (mutex_trylock(&info->checkin_mutex) == 0) {
				/* the mutex might be occupied by
				   entd caller */
				result = RETERR(-EBUSY);
				reiser4_exit_context(ctx);
				break;
			}
		} else
			mutex_lock(&info->checkin_mutex);

		result = capture_anon_pages(inode->i_mapping, &index,
					    to_capture);
		mutex_unlock(&info->checkin_mutex);

		if (result < 0) {
			reiser4_exit_context(ctx);
			break;
		}
		wbc->nr_to_write -= result;
		if (wbc->sync_mode != WB_SYNC_ALL) {
			reiser4_exit_context(ctx);
			break;
		}
		result = txnmgr_force_commit_all(inode->i_sb, 0);
		reiser4_exit_context(ctx);
	} while (result >= 0 && index < nrpages);

 end:
	if (is_in_reiser4_context()) {
		if (get_current_context()->nr_captured >= CAPTURE_APAGE_BURST) {
			/* there are already pages to flush, flush them out,
			   do not delay until end of reiser4_sync_inodes */
			reiser4_writeout(inode->i_sb, wbc);
			get_current_context()->nr_captured = 0;
		}
	}
	return result;
}

/* plugin->ioctl */
int ioctl_cryptcompress(struct file *filp, unsigned int cmd,
			unsigned long arg)
{
	return RETERR(-ENOSYS);
}

/* plugin->mmap */
int mmap_cryptcompress(struct file *file, struct vm_area_struct *vma)
{
	int result;
	struct inode *inode;
	reiser4_context *ctx;

	inode = file->f_dentry->d_inode;
	ctx = reiser4_init_context(inode->i_sb);
	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	/*
	 * generic_file_mmap will do update_atime. Grab space for stat data
	 * update.
	 */
	result = reiser4_grab_space_force
		(inode_file_plugin(inode)->estimate.update(inode),
		 BA_CAN_COMMIT);
	if (result) {
		reiser4_exit_context(ctx);
		return result;
	}
	result = generic_file_mmap(file, vma);
	reiser4_exit_context(ctx);
	return result;
}

/* plugin->delete_object */
int delete_object_cryptcompress(struct inode *inode)
{
	int result;
	struct cryptcompress_info * info;

	assert("edward-429", inode->i_nlink == 0);

	reiser4_txn_restart_current();
	info = cryptcompress_inode_data(inode);

	mutex_lock(&info->checkin_mutex);
	result = cryptcompress_truncate(inode, 0, 0);
	mutex_unlock(&info->checkin_mutex);

	if (result) {
		warning("edward-430",
			"cannot truncate cryptcompress file  %lli: %i",
			(unsigned long long)get_inode_oid(inode),
			result);
	}
	truncate_inode_pages(inode->i_mapping, 0);
	assert("edward-1487", pages_truncate_ok(inode, 0));
	/* and remove stat data */
	return reiser4_delete_object_common(inode);
}

/*
 * plugin->setattr
 * This implements actual truncate (see comments in reiser4/page_cache.c)
 */
int setattr_cryptcompress(struct dentry *dentry, struct iattr *attr)
{
	int result;
	struct inode *inode;
	struct cryptcompress_info * info;

	inode = dentry->d_inode;
	info = cryptcompress_inode_data(inode);

	if (attr->ia_valid & ATTR_SIZE) {
		if (i_size_read(inode) != attr->ia_size) {
			reiser4_context *ctx;
			loff_t old_size;

			ctx = reiser4_init_context(dentry->d_inode->i_sb);
			if (IS_ERR(ctx))
				return PTR_ERR(ctx);
			result = setattr_dispatch_hook(inode);
			if (result) {
				context_set_commit_async(ctx);
				reiser4_exit_context(ctx);
				return result;
			}
			old_size = i_size_read(inode);
			inode_check_scale(inode, old_size, attr->ia_size);

			mutex_lock(&info->checkin_mutex);
			result = cryptcompress_truncate(inode,
							attr->ia_size,
							1/* update sd */);
			mutex_unlock(&info->checkin_mutex);
			if (result) {
			     warning("edward-1192",
				     "truncate_cryptcompress failed: oid %lli, "
				     "old size %lld, new size %lld, retval %d",
				     (unsigned long long)
				     get_inode_oid(inode), old_size,
				     attr->ia_size, result);
			}
			context_set_commit_async(ctx);
			reiser4_exit_context(ctx);
		} else
			result = 0;
	} else
		result = reiser4_setattr_common(dentry, attr);
	return result;
}

/* plugin->release */
int release_cryptcompress(struct inode *inode, struct file *file)
{
	reiser4_context *ctx = reiser4_init_context(inode->i_sb);

	if (IS_ERR(ctx))
		return PTR_ERR(ctx);
	reiser4_free_file_fsdata(file);
	reiser4_exit_context(ctx);
	return 0;
}

/* plugin->write_begin() */
int write_begin_cryptcompress(struct file *file, struct page *page,
			      loff_t pos, unsigned len, void **fsdata)
{
	int ret = -ENOMEM;
	char *buf;
	hint_t *hint;
	struct inode *inode;
	struct reiser4_slide *win;
	struct cluster_handle *clust;
	struct cryptcompress_info *info;
	reiser4_context *ctx;

	ctx = get_current_context();
	inode = page->mapping->host;
	info = cryptcompress_inode_data(inode);

	assert("edward-1564", PageLocked(page));
	buf = kmalloc(sizeof(*clust) +
		      sizeof(*win) +
		      sizeof(*hint),
		      reiser4_ctx_gfp_mask_get());
	if (!buf)
		goto err2;
	clust = (struct cluster_handle *)buf;
	win = (struct reiser4_slide *)(buf + sizeof(*clust));
	hint = (hint_t *)(buf + sizeof(*clust) + sizeof(*win));

	hint_init_zero(hint);
	cluster_init_read(clust, NULL);
	clust->hint = hint;

	mutex_lock(&info->checkin_mutex);

	ret = set_window_and_cluster(inode, clust, win, len, pos);
	if (ret)
		goto err1;
	unlock_page(page);
	ret = prepare_logical_cluster(inode, pos, len, clust, LC_APPOV);
	done_lh(&hint->lh);
	assert("edward-1565", lock_stack_isclean(get_current_lock_stack()));
	lock_page(page);
	if (ret) {
		SetPageError(page);
		ClearPageUptodate(page);
		goto err0;
	}
	/*
	 * Success. All resources (including checkin_mutex)
	 * will be released in ->write_end()
	 */
	ctx->locked_page = page;
	*fsdata = (void *)buf;

	return 0;
 err0:
	put_cluster_handle(clust);
 err1:
	mutex_unlock(&info->checkin_mutex);
	kfree(buf);
 err2:
	assert("edward-1568", !ret);
	return ret;
}

/* plugin->write_end() */
int write_end_cryptcompress(struct file *file, struct page *page,
			    loff_t pos, unsigned copied, void *fsdata)
{
	int ret;
	hint_t *hint;
	struct inode *inode;
	struct cluster_handle *clust;
	struct cryptcompress_info *info;
	reiser4_context *ctx;

	assert("edward-1566",
	       lock_stack_isclean(get_current_lock_stack()));
	ctx = get_current_context();
	inode = page->mapping->host;
	info = cryptcompress_inode_data(inode);
	clust = (struct cluster_handle *)fsdata;
	hint = clust->hint;

	unlock_page(page);
	ctx->locked_page = NULL;
	set_cluster_pages_dirty(clust, inode);
	ret = checkin_logical_cluster(clust, inode);
	if (ret) {
		SetPageError(page);
		goto exit;
	}
 exit:
	mutex_unlock(&info->checkin_mutex);

	put_cluster_handle(clust);

	if (pos + copied > inode->i_size) {
		/*
		 * i_size has been updated in
		 * checkin_logical_cluster
		 */
		ret = reiser4_update_sd(inode);
		if (unlikely(ret != 0))
			warning("edward-1603",
				"Can not update stat-data: %i. FSCK?",
				ret);
	}
	kfree(fsdata);
	return ret;
}

/* plugin->bmap */
sector_t bmap_cryptcompress(struct address_space *mapping, sector_t lblock)
{
	return -EINVAL;
}

/*
  Local variables:
  c-indentation-style: "K&R"
  mode-name: "LC"
  c-basic-offset: 8
  tab-width: 8
  fill-column: 80
  scroll-step: 1
  End:
*/
