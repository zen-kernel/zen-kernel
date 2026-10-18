// SPDX-License-Identifier: GPL-2.0-or-later

#define pr_fmt(fmt) "zram: deflate: " fmt

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/zlib.h>

#include "backend_deflate.h"

/* Use the same value as crypto API */
#define DEFLATE_DEF_WINBITS		(-11)
#define DEFLATE_DEF_MEMLEVEL		MAX_MEM_LEVEL

static void deflate_release_params(struct zcomp_params *params)
{
}

static int deflate_setup_params(struct zcomp_params *params)
{
	if (params->dict_sz) {
		pr_err("dictionary is not supported\n");
		return -EOPNOTSUPP;
	}

	if (params->level == ZCOMP_PARAM_NOT_SET) {
		params->level = Z_DEFAULT_COMPRESSION;
	} else if (params->level < Z_DEFAULT_COMPRESSION ||
		   params->level > Z_BEST_COMPRESSION) {
		pr_err("invalid compression level %d\n", params->level);
		return -EINVAL;
	}

	if (params->deflate.winbits == ZCOMP_PARAM_NOT_SET) {
		params->deflate.winbits = DEFLATE_DEF_WINBITS;
	} else {
		s32 wb = params->deflate.winbits;

		if ((wb < -15 || wb > -9) && (wb < 9 || wb > 15)) {
			pr_err("invalid winbits %d\n", wb);
			return -EINVAL;
		}
	}

	return 0;
}

static void deflate_destroy_cctx(struct zcomp_ctx *ctx)
{
	struct z_stream_s *cctx = ctx->context;

	if (!cctx)
		return;

	if (cctx->workspace) {
		zlib_deflateEnd(cctx);
		vfree(cctx->workspace);
	}
	kfree(cctx);
}

static int deflate_create_cctx(struct zcomp_params *params,
			       struct zcomp_ctx *ctx)
{
	struct z_stream_s *cctx;
	size_t sz;
	int ret;

	cctx = kzalloc_obj(*cctx);
	if (!cctx)
		return -ENOMEM;

	ctx->context = cctx;
	sz = zlib_deflate_workspacesize(params->deflate.winbits, MAX_MEM_LEVEL);
	cctx->workspace = vzalloc(sz);
	if (!cctx->workspace)
		goto error;

	ret = zlib_deflateInit2(cctx, params->level, Z_DEFLATED,
				params->deflate.winbits, DEFLATE_DEF_MEMLEVEL,
				Z_DEFAULT_STRATEGY);
	if (ret != Z_OK)
		goto error;

	return 0;

error:
	deflate_destroy_cctx(ctx);
	return -EINVAL;
}

static void deflate_destroy_dctx(struct zcomp_ctx *ctx)
{
	struct z_stream_s *dctx = ctx->context;

	if (!dctx)
		return;

	if (dctx->workspace) {
		zlib_inflateEnd(dctx);
		vfree(dctx->workspace);
	}
	kfree(dctx);
}

static int deflate_create_dctx(struct zcomp_params *params,
			       struct zcomp_ctx *ctx)
{
	struct z_stream_s *dctx;
	size_t sz;
	int ret;

	dctx = kzalloc_obj(*dctx);
	if (!dctx)
		return -ENOMEM;

	ctx->context = dctx;
	sz = zlib_inflate_workspacesize();
	dctx->workspace = vzalloc(sz);
	if (!dctx->workspace)
		goto error;

	ret = zlib_inflateInit2(dctx, params->deflate.winbits);
	if (ret != Z_OK)
		goto error;

	return 0;

error:
	deflate_destroy_dctx(ctx);
	return -EINVAL;
}

static int deflate_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			    struct zcomp_req *req)
{
	struct z_stream_s *deflate = ctx->context;
	int ret;

	ret = zlib_deflateReset(deflate);
	if (ret != Z_OK)
		return -EINVAL;

	deflate->next_in = (u8 *)req->src;
	deflate->avail_in = req->src_len;
	deflate->next_out = (u8 *)req->dst;
	deflate->avail_out = req->dst_len;

	ret = zlib_deflate(deflate, Z_FINISH);
	if (ret != Z_STREAM_END)
		return -EINVAL;

	req->dst_len = deflate->total_out;
	return 0;
}

static int deflate_decompress(struct zcomp_params *params,
			      struct zcomp_ctx *ctx,
			      struct zcomp_req *req)
{
	struct z_stream_s *inflate = ctx->context;
	int ret;

	ret = zlib_inflateReset(inflate);
	if (ret != Z_OK)
		return -EINVAL;

	inflate->next_in = (u8 *)req->src;
	inflate->avail_in = req->src_len;
	inflate->next_out = (u8 *)req->dst;
	inflate->avail_out = req->dst_len;

	ret = zlib_inflate(inflate, Z_SYNC_FLUSH);
	if (ret != Z_STREAM_END)
		return -EINVAL;

	return 0;
}

const struct zcomp_ops backend_deflate = {
	.compress	= deflate_compress,
	.decompress	= deflate_decompress,
	.create_cctx	= deflate_create_cctx,
	.destroy_cctx	= deflate_destroy_cctx,
	.create_dctx	= deflate_create_dctx,
	.destroy_dctx	= deflate_destroy_dctx,
	.setup_params	= deflate_setup_params,
	.release_params	= deflate_release_params,
	.name		= "deflate",
};
