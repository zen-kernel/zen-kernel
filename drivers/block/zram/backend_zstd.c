// SPDX-License-Identifier: GPL-2.0-or-later

#define pr_fmt(fmt) "zram: zstd: " fmt

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/zstd.h>

#include "backend_zstd.h"

struct zstd_ctx {
	void *ctx;
	void *mem;
};

struct zstd_params {
	zstd_custom_mem custom_mem;
	zstd_cdict *cdict;
	zstd_ddict *ddict;
	zstd_parameters cprm;
};

/*
 * For C/D dictionaries we need to provide zstd with zstd_custom_mem,
 * which zstd uses internally to allocate/free memory when needed.
 *
 * zstd allocations (C/D dictionaries and contexts) are large and
 * long-lived.  Don't use kvzalloc() for them: when enough physically
 * contiguous memory is available it serves them from kmalloc(), which
 * rounds them up to a power-of-two number of pages.
 */
static void *zstd_custom_alloc(void *opaque, size_t size)
{
	if (size <= PAGE_SIZE)
		return kzalloc(size, GFP_NOIO | __GFP_NOWARN);
	return __vmalloc(size, GFP_NOIO | __GFP_NOWARN | __GFP_ZERO);
}

static void zstd_custom_free(void *opaque, void *address)
{
	kvfree(address);
}

static void zstd_release_params(struct zcomp_params *params)
{
	struct zstd_params *zp = params->drv_data;

	params->drv_data = NULL;
	if (!zp)
		return;

	zstd_free_cdict(zp->cdict);
	zstd_free_ddict(zp->ddict);
	kfree(zp);
}

static int zstd_setup_params(struct zcomp_params *params)
{
	zstd_compression_parameters prm;
	struct zstd_params *zp;

	zp = kzalloc_obj(*zp);
	if (!zp)
		return -ENOMEM;

	params->drv_data = zp;
	if (params->level == ZCOMP_PARAM_NOT_SET)
		params->level = zstd_default_clevel();

	zp->cprm = zstd_get_params(params->level, PAGE_SIZE);

	zp->custom_mem.customAlloc = zstd_custom_alloc;
	zp->custom_mem.customFree = zstd_custom_free;

	prm = zstd_get_cparams(params->level, PAGE_SIZE,
			       params->dict_sz);

	if (!params->dict || !params->dict_sz)
		return 0;

	zp->cdict = zstd_create_cdict_byreference(params->dict,
						  params->dict_sz,
						  prm,
						  zp->custom_mem);
	if (!zp->cdict)
		goto error;

	zp->ddict = zstd_create_ddict_byreference(params->dict,
						  params->dict_sz,
						  zp->custom_mem);
	if (!zp->ddict)
		goto error;

	return 0;

error:
	return -EINVAL;
}

static void zstd_destroy_cctx(struct zcomp_ctx *ctx)
{
	struct zstd_ctx *zctx = ctx->context;

	if (!zctx)
		return;

	/*
	 * If ->mem was allocated then we didn't use C/D dictionary and
	 * ->ctx was "embedded" into this buffer.
	 *
	 * If otherwise then we need to explicitly release ->ctx.
	 */
	if (zctx->mem)
		vfree(zctx->mem);
	else
		zstd_free_cctx(zctx->ctx);

	kfree(zctx);
}

static int zstd_create_cctx(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct zstd_params *zp = params->drv_data;
	struct zstd_ctx *zctx;

	zctx = kzalloc_obj(*zctx);
	if (!zctx)
		return -ENOMEM;

	ctx->context = zctx;
	if (params->dict_sz == 0) {
		zstd_parameters prm = zp->cprm;
		size_t sz = zstd_cctx_workspace_bound(&prm.cParams);

		zctx->mem = vzalloc(sz);
		if (!zctx->mem)
			goto error;

		zctx->ctx = zstd_init_cctx(zctx->mem, sz);
		if (!zctx->ctx)
			goto error;
	} else {
		zctx->ctx = zstd_create_cctx_advanced(zp->custom_mem);
		if (!zctx->ctx)
			goto error;
	}

	return 0;

error:
	zstd_destroy_cctx(ctx);
	return -EINVAL;
}

static void zstd_destroy_dctx(struct zcomp_ctx *ctx)
{
	struct zstd_ctx *zctx = ctx->context;

	if (!zctx)
		return;

	if (zctx->mem)
		vfree(zctx->mem);
	else
		zstd_free_dctx(zctx->ctx);

	kfree(zctx);
}

static int zstd_create_dctx(struct zcomp_params *params, struct zcomp_ctx *ctx)
{
	struct zstd_params *zp = params->drv_data;
	struct zstd_ctx *zctx;

	zctx = kzalloc_obj(*zctx);
	if (!zctx)
		return -ENOMEM;

	ctx->context = zctx;
	if (params->dict_sz == 0) {
		size_t sz = zstd_dctx_workspace_bound();

		zctx->mem = vzalloc(sz);
		if (!zctx->mem)
			goto error;

		zctx->ctx = zstd_init_dctx(zctx->mem, sz);
		if (!zctx->ctx)
			goto error;
	} else {
		zctx->ctx = zstd_create_dctx_advanced(zp->custom_mem);
		if (!zctx->ctx)
			goto error;
	}

	return 0;

error:
	zstd_destroy_dctx(ctx);
	return -EINVAL;
}

static int zstd_compress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			 struct zcomp_req *req)
{
	struct zstd_params *zp = params->drv_data;
	struct zstd_ctx *zctx = ctx->context;
	size_t ret;

	if (params->dict_sz == 0)
		ret = zstd_compress_cctx(zctx->ctx, req->dst, req->dst_len,
					 req->src, req->src_len, &zp->cprm);
	else
		ret = zstd_compress_using_cdict(zctx->ctx, req->dst,
						req->dst_len, req->src,
						req->src_len,
						zp->cdict);
	if (zstd_is_error(ret))
		return -EINVAL;
	req->dst_len = ret;
	return 0;
}

static int zstd_decompress(struct zcomp_params *params, struct zcomp_ctx *ctx,
			   struct zcomp_req *req)
{
	struct zstd_params *zp = params->drv_data;
	struct zstd_ctx *zctx = ctx->context;
	size_t ret;

	if (params->dict_sz == 0)
		ret = zstd_decompress_dctx(zctx->ctx, req->dst, req->dst_len,
					   req->src, req->src_len);
	else
		ret = zstd_decompress_using_ddict(zctx->ctx, req->dst,
						  req->dst_len, req->src,
						  req->src_len, zp->ddict);
	if (zstd_is_error(ret))
		return -EINVAL;
	return 0;
}

const struct zcomp_ops backend_zstd = {
	.compress	= zstd_compress,
	.decompress	= zstd_decompress,
	.create_cctx	= zstd_create_cctx,
	.destroy_cctx	= zstd_destroy_cctx,
	.create_dctx	= zstd_create_dctx,
	.destroy_dctx	= zstd_destroy_dctx,
	.setup_params	= zstd_setup_params,
	.release_params	= zstd_release_params,
	.name		= "zstd",
};
