/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */
/* reiser4 compression transform plugins */

#include "../../debug.h"
#include "../../inode.h"
#include "../plugin.h"

#include <linux/lzo.h>
#include <linux/zlib.h>
#include <linux/types.h>
#include <linux/hardirq.h>

static int change_compression(struct inode *inode,
			      reiser4_plugin * plugin,
			      pset_member memb)
{
	assert("edward-1316", inode != NULL);
	assert("edward-1317", plugin != NULL);
	assert("edward-1318", is_reiser4_inode(inode));
	assert("edward-1319",
	       plugin->h.type_id == REISER4_COMPRESSION_PLUGIN_TYPE);

	/* cannot change compression plugin of already existing regular object */
	if (!plugin_of_group(inode_file_plugin(inode), REISER4_DIRECTORY_FILE))
		return RETERR(-EINVAL);

	/* If matches, nothing to change. */
	if (inode_hash_plugin(inode) != NULL &&
	    inode_hash_plugin(inode)->h.id == plugin->h.id)
		return 0;

	return aset_set_unsafe(&reiser4_inode_data(inode)->pset,
			       PSET_COMPRESSION, plugin);
}

static reiser4_plugin_ops compression_plugin_ops = {
	.init = NULL,
	.load = NULL,
	.save_len = NULL,
	.save = NULL,
	.change = &change_compression
};

/******************************************************************************/
/*                         gzip1 compression                                  */
/******************************************************************************/

#define GZIP1_DEF_LEVEL		        Z_BEST_SPEED
#define GZIP1_DEF_WINBITS		15
#define GZIP1_DEF_MEMLEVEL		MAX_MEM_LEVEL

static int gzip1_init(void)
{
	return 0;
}

static int gzip1_overrun(unsigned src_len UNUSED_ARG)
{
	return 0;
}

static coa_t gzip1_alloc(tfm_action act)
{
	coa_t coa = NULL;
	int ret = 0;
	switch (act) {
	case TFMA_WRITE:	/* compress */
		coa = reiser4_vmalloc(zlib_deflate_workspacesize(MAX_WBITS,
							MAX_MEM_LEVEL));
		if (!coa) {
			ret = -ENOMEM;
			break;
		}
		break;
	case TFMA_READ:	/* decompress */
		coa = reiser4_vmalloc(zlib_inflate_workspacesize());
		if (!coa) {
			ret = -ENOMEM;
			break;
		}
		break;
	default:
		impossible("edward-767",
			   "trying to alloc workspace for unknown tfm action");
	}
	if (ret) {
		warning("edward-768",
			"alloc workspace for gzip1 (tfm action = %d) failed\n",
			act);
		return ERR_PTR(ret);
	}
	return coa;
}

static void gzip1_free(coa_t coa, tfm_action act)
{
	assert("edward-769", coa != NULL);

	switch (act) {
	case TFMA_WRITE:	/* compress */
		vfree(coa);
		break;
	case TFMA_READ:		/* decompress */
		vfree(coa);
		break;
	default:
		impossible("edward-770", "unknown tfm action");
	}
	return;
}

static int gzip1_min_size_deflate(void)
{
	return 64;
}

static void
gzip1_compress(coa_t coa, __u8 * src_first, size_t src_len,
	       __u8 * dst_first, size_t *dst_len)
{
	int ret = 0;
	struct z_stream_s stream;

	assert("edward-842", coa != NULL);
	assert("edward-875", src_len != 0);

	stream.workspace = coa;
	ret = zlib_deflateInit2(&stream, GZIP1_DEF_LEVEL, Z_DEFLATED,
				-GZIP1_DEF_WINBITS, GZIP1_DEF_MEMLEVEL,
				Z_DEFAULT_STRATEGY);
	if (ret != Z_OK) {
		warning("edward-771", "zlib_deflateInit2 returned %d\n", ret);
		goto rollback;
	}
	ret = zlib_deflateReset(&stream);
	if (ret != Z_OK) {
		warning("edward-772", "zlib_deflateReset returned %d\n", ret);
		goto rollback;
	}
	stream.next_in = src_first;
	stream.avail_in = src_len;
	stream.next_out = dst_first;
	stream.avail_out = *dst_len;

	ret = zlib_deflate(&stream, Z_FINISH);
	if (ret != Z_STREAM_END) {
		if (ret != Z_OK)
			warning("edward-773",
				"zlib_deflate returned %d\n", ret);
		goto rollback;
	}
	*dst_len = stream.total_out;
	return;
      rollback:
	*dst_len = src_len;
	return;
}

static void
gzip1_decompress(coa_t coa, __u8 * src_first, size_t src_len,
		 __u8 * dst_first, size_t *dst_len)
{
	int ret = 0;
	struct z_stream_s stream;

	assert("edward-843", coa != NULL);
	assert("edward-876", src_len != 0);

	stream.workspace = coa;
	ret = zlib_inflateInit2(&stream, -GZIP1_DEF_WINBITS);
	if (ret != Z_OK) {
		warning("edward-774", "zlib_inflateInit2 returned %d\n", ret);
		return;
	}
	ret = zlib_inflateReset(&stream);
	if (ret != Z_OK) {
		warning("edward-775", "zlib_inflateReset returned %d\n", ret);
		return;
	}

	stream.next_in = src_first;
	stream.avail_in = src_len;
	stream.next_out = dst_first;
	stream.avail_out = *dst_len;

	ret = zlib_inflate(&stream, Z_SYNC_FLUSH);
	/*
	 * Work around a bug in zlib, which sometimes wants to taste an extra
	 * byte when being used in the (undocumented) raw deflate mode.
	 * (From USAGI).
	 */
	if (ret == Z_OK && !stream.avail_in && stream.avail_out) {
		u8 zerostuff = 0;
		stream.next_in = &zerostuff;
		stream.avail_in = 1;
		ret = zlib_inflate(&stream, Z_FINISH);
	}
	if (ret != Z_STREAM_END) {
		warning("edward-776", "zlib_inflate returned %d\n", ret);
		return;
	}
	*dst_len = stream.total_out;
	return;
}

/******************************************************************************/
/*                            lzo1 compression                                */
/******************************************************************************/

static int lzo1_init(void)
{
	return 0;
}

static int lzo1_overrun(unsigned in_len)
{
	return in_len / 64 + 16 + 3;
}

static coa_t lzo1_alloc(tfm_action act)
{
	int ret = 0;
	coa_t coa = NULL;

	switch (act) {
	case TFMA_WRITE:	/* compress */
		coa = reiser4_vmalloc(LZO1X_1_MEM_COMPRESS);
		if (!coa) {
			ret = -ENOMEM;
			break;
		}
	case TFMA_READ:		/* decompress */
		break;
	default:
		impossible("edward-877",
			   "trying to alloc workspace for unknown tfm action");
	}
	if (ret) {
		warning("edward-878",
			"alloc workspace for lzo1 (tfm action = %d) failed\n",
			act);
		return ERR_PTR(ret);
	}
	return coa;
}

static void lzo1_free(coa_t coa, tfm_action act)
{
	assert("edward-879", coa != NULL);

	switch (act) {
	case TFMA_WRITE:	/* compress */
		vfree(coa);
		break;
	case TFMA_READ:		/* decompress */
		impossible("edward-1304",
			   "trying to free non-allocated workspace");
	default:
		impossible("edward-880", "unknown tfm action");
	}
	return;
}

static int lzo1_min_size_deflate(void)
{
	return 256;
}

static void
lzo1_compress(coa_t coa, __u8 * src_first, size_t src_len,
	      __u8 * dst_first, size_t *dst_len)
{
	int result;

	assert("edward-846", coa != NULL);
	assert("edward-847", src_len != 0);

	result = lzo1x_1_compress(src_first, src_len, dst_first, dst_len, coa);
	if (unlikely(result != LZO_E_OK)) {
		warning("edward-849", "lzo1x_1_compress failed\n");
		goto out;
	}
	if (*dst_len >= src_len) {
		//warning("edward-850", "lzo1x_1_compress: incompressible data\n");
		goto out;
	}
	return;
      out:
	*dst_len = src_len;
	return;
}

static void
lzo1_decompress(coa_t coa, __u8 * src_first, size_t src_len,
		__u8 * dst_first, size_t *dst_len)
{
	int result;

	assert("edward-851", coa == NULL);
	assert("edward-852", src_len != 0);

	result = lzo1x_decompress_safe(src_first, src_len, dst_first, dst_len);
	if (result != LZO_E_OK)
		warning("edward-853", "lzo1x_1_decompress failed\n");
	return;
}

compression_plugin compression_plugins[LAST_COMPRESSION_ID] = {
	[LZO1_COMPRESSION_ID] = {
		.h = {
			.type_id = REISER4_COMPRESSION_PLUGIN_TYPE,
			.id = LZO1_COMPRESSION_ID,
			.pops = &compression_plugin_ops,
			.label = "lzo1",
			.desc = "lzo1 compression transform",
			.linkage = {NULL, NULL}
		},
		.init = lzo1_init,
		.overrun = lzo1_overrun,
		.alloc = lzo1_alloc,
		.free = lzo1_free,
		.min_size_deflate = lzo1_min_size_deflate,
		.checksum = reiser4_adler32,
		.compress = lzo1_compress,
		.decompress = lzo1_decompress
	},
	[GZIP1_COMPRESSION_ID] = {
		.h = {
			.type_id = REISER4_COMPRESSION_PLUGIN_TYPE,
			.id = GZIP1_COMPRESSION_ID,
			.pops = &compression_plugin_ops,
			.label = "gzip1",
			.desc = "gzip1 compression transform",
			.linkage = {NULL, NULL}
		},
		.init = gzip1_init,
		.overrun = gzip1_overrun,
		.alloc = gzip1_alloc,
		.free = gzip1_free,
		.min_size_deflate = gzip1_min_size_deflate,
		.checksum = reiser4_adler32,
		.compress = gzip1_compress,
		.decompress = gzip1_decompress
	}
};

/*
  Local variables:
  c-indentation-style: "K&R"
  mode-name: "LC"
  c-basic-offset: 8
  tab-width: 8
  fill-column: 120
  scroll-step: 1
  End:
*/
