// SPDX-License-Identifier: GPL-2.0-or-later

#define pr_fmt(fmt) "zram: lz4hc: " fmt

#include <linux/kernel.h>
#include <linux/lz4.h>
#include <linux/vmalloc.h>

#include "backend_lz4hc.h"

static void lz4hc_release_params(struct zcomp_params *params)
{
	LZ4_streamHC_t *dict_stream = params->drv_data;

	params->drv_data = NULL;
	if (!dict_stream)
		return;

	vfree(dict_stream);
}

static int lz4hc_setup_params(struct zcomp_params *params)
{
	LZ4_streamHC_t *dict_stream;
	int ret;

	if (params->level == ZCOMP_PARAM_NOT_SET)
		params->level = LZ4HC_DEFAULT_CLEVEL;

	if (!params->dict || !params->dict_sz)
		return 0;

	dict_stream = vzalloc(sizeof(*dict_stream));
	if (!dict_stream)
		return -ENOMEM;

	LZ4_resetStreamHC(dict_stream, params->level);
	ret = LZ4_loadDictHC(dict_stream, params->dict, params->dict_sz);
	if (ret != params->dict_sz) {
		vfree(dict_stream);
		return -EINVAL;
	}
	params->drv_data = dict_stream;

	return 0;
}

static void lz4hc_destroy(struct zcomp_ctx *ctx)
{
	vfree(ctx->context);
}

static int lz4hc_create(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	BUILD_BUG_ON(sizeof(LZ4_streamHC_t) != LZ4HC_MEM_COMPRESS);

	ctx->context = vmalloc(LZ4HC_MEM_COMPRESS);
	if (!ctx->context)
		return -ENOMEM;
	return 0;
}

static int lz4hc_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			  struct zcomp_req *req)
{
	int ret;

	if (!params->drv_data) {
		ret = LZ4_compress_HC(req->src, req->dst, req->src_len,
				      req->dst_len, params->level,
				      ctx->context);
	} else {
		/* Cstrm needs to be reset */
		memcpy(ctx->context, params->drv_data, sizeof(LZ4_streamHC_t));
		ret = LZ4_compress_HC_continue(ctx->context, req->src, req->dst,
					       req->src_len, req->dst_len);
	}
	if (!ret)
		return -EINVAL;
	req->dst_len = ret;
	return 0;
}

static int lz4hc_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			    struct zcomp_req *req)
{
	int ret;

	ret = LZ4_decompress_safe_usingDict(req->src, req->dst, req->src_len,
					    req->dst_len, params->dict,
					    params->dict_sz);
	if (ret < 0)
		return -EINVAL;
	return 0;
}

const struct zcomp_ops backend_lz4hc = {
	.compress	= lz4hc_compress,
	.decompress	= lz4hc_decompress,
	.create_ctx	= lz4hc_create,
	.destroy_ctx	= lz4hc_destroy,
	.setup_params	= lz4hc_setup_params,
	.release_params	= lz4hc_release_params,
	.name		= "lz4hc",
};
