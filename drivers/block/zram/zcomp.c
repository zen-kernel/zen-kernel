// SPDX-License-Identifier: GPL-2.0-or-later

#include <linux/kernel.h>
#include <linux/string.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/wait.h>
#include <linux/sched.h>
#include <linux/cpuhotplug.h>
#include <linux/highmem.h>
#include <linux/scatterlist.h>
#include <linux/vmalloc.h>
#include <linux/sysfs.h>

#include "zcomp.h"

#include "backend_lzo.h"
#include "backend_lzorle.h"
#include "backend_lz4.h"
#include "backend_lz4hc.h"
#include "backend_zstd.h"
#include "backend_deflate.h"
#include "backend_842.h"

static const struct zcomp_ops *backends[] = {
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_LZO)
	&backend_lzorle,
	&backend_lzo,
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_LZ4)
	&backend_lz4,
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_LZ4HC)
	&backend_lz4hc,
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_ZSTD)
	&backend_zstd,
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_DEFLATE)
	&backend_deflate,
#endif
#if IS_ENABLED(CONFIG_ZRAM_BACKEND_842)
	&backend_842,
#endif
	NULL
};

static void zcomp_cstrm_free(struct zcomp *comp, struct zcomp_cstrm *zstrm)
{
	if (comp->ops->destroy_cctx)
		comp->ops->destroy_cctx(&zstrm->ctx);
	vfree(zstrm->buffer);
	zstrm->buffer = NULL;
	zstrm->ctx.context = NULL;
}

static int zcomp_cstrm_init(struct zcomp *comp, struct zcomp_cstrm *zstrm)
{
	int ret;

	if (comp->ops->create_cctx) {
		ret = comp->ops->create_cctx(comp->params, &zstrm->ctx);
		if (ret) {
			/* backend has released partially created context */
			zstrm->ctx.context = NULL;
			return ret;
		}
	}

	/*
	 * allocate 2 pages. 1 for compressed data, plus 1 extra for the
	 * case when compressed size is larger than the original one
	 */
	zstrm->buffer = vzalloc(2 * PAGE_SIZE);
	if (!zstrm->buffer) {
		zcomp_cstrm_free(comp, zstrm);
		return -ENOMEM;
	}
	return 0;
}

static void zcomp_dstrm_free(struct zcomp *comp, struct zcomp_dstrm *zstrm)
{
	if (comp->ops->destroy_dctx)
		comp->ops->destroy_dctx(&zstrm->ctx);
	vfree(zstrm->local_copy);
	zstrm->local_copy = NULL;
	zstrm->ctx.context = NULL;
}

static int zcomp_dstrm_init(struct zcomp *comp, struct zcomp_dstrm *zstrm)
{
	int ret;

	if (comp->ops->create_dctx) {
		ret = comp->ops->create_dctx(comp->params, &zstrm->ctx);
		if (ret) {
			/* backend has released partially created context */
			zstrm->ctx.context = NULL;
			return ret;
		}
	}

	zstrm->local_copy = vzalloc(PAGE_SIZE);
	if (!zstrm->local_copy) {
		zcomp_dstrm_free(comp, zstrm);
		return -ENOMEM;
	}
	return 0;
}

static const struct zcomp_ops *lookup_backend_ops(const char *comp)
{
	int i = 0;

	while (backends[i]) {
		if (sysfs_streq(comp, backends[i]->name))
			break;
		i++;
	}
	return backends[i];
}

const char *zcomp_lookup_backend_name(const char *comp)
{
	const struct zcomp_ops *backend = lookup_backend_ops(comp);

	if (backend)
		return backend->name;

	return NULL;
}

/* show available compressors */
ssize_t zcomp_available_show(const char *comp, char *buf, ssize_t at)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(backends) - 1; i++) {
		if (!strcmp(comp, backends[i]->name)) {
			at += sysfs_emit_at(buf, at, "[%s] ",
					    backends[i]->name);
		} else {
			at += sysfs_emit_at(buf, at, "%s ", backends[i]->name);
		}
	}

	at += sysfs_emit_at(buf, at, "\n");
	return at;
}

struct zcomp_cstrm *zcomp_cstrm_get(struct zcomp *comp)
{
	if (comp->recomp_cstream) {
		mutex_lock(&comp->recomp_cstream->lock);
		return comp->recomp_cstream;
	}

	for (;;) {
		struct zcomp_cstrm *zstrm = raw_cpu_ptr(comp->cstream);

		/*
		 * Inspired by zswap
		 *
		 * stream is returned with ->mutex locked which prevents
		 * cpu_dead() from releasing this stream under us, however
		 * there is still a race window between raw_cpu_ptr() and
		 * mutex_lock(), during which we could have been migrated
		 * from a CPU that has already destroyed its stream.  If
		 * so then unlock and re-try on the current CPU.
		 */
		mutex_lock(&zstrm->lock);
		if (zstrm->buffer)
			return zstrm;
		mutex_unlock(&zstrm->lock);
	}
}

void zcomp_cstrm_put(struct zcomp_cstrm *zstrm)
{
	mutex_unlock(&zstrm->lock);
}

bool zcomp_shared_decomp(struct zcomp *comp)
{
	/*
	 * Shared decompression is only safe when the backend has no
	 * mutable per-stream decompression context.
	 */
	return !comp->ops->create_dctx;
}

static bool zcomp_dstrm_excl_trylock(struct zcomp_dstrm *zstrm)
{
	/* Cheap pre-check: don't touch a busy stream's cachelines */
	if (mutex_is_locked(&zstrm->excl_lock))
		return false;
	/*
	 * Pin the stream.  The write side of ->lock is only taken by
	 * zcomp_cpu_dead(), which also sets ->local_copy to NULL.
	 */
	if (!down_read_trylock(&zstrm->lock))
		return false;
	if (zstrm->local_copy && mutex_trylock(&zstrm->excl_lock))
		return true;
	up_read(&zstrm->lock);
	return false;
}

static struct zcomp_dstrm *zcomp_dstrm_excl_tryget(struct zcomp *comp)
{
	struct zcomp_dstrm *zstrm;
	unsigned int cpu, start = raw_smp_processor_id();

	/* Best-effort start CPU hint; migration during the scan is fine */
	for_each_online_cpu_wrap(cpu, start) {
		zstrm = per_cpu_ptr(comp->dstream, cpu);
		if (zcomp_dstrm_excl_trylock(zstrm))
			return zstrm;
	}
	return NULL;
}

struct zcomp_dstrm *zcomp_dstrm_get(struct zcomp *comp,
				    enum zcomp_dstrm_mode mode)
{
	struct zcomp_dstrm *zstrm;

	if (mode == ZCOMP_DSTRM_SHARED) {
		for (;;) {
			zstrm = raw_cpu_ptr(comp->dstream);
			down_read(&zstrm->lock);
			if (zstrm->local_copy)
				return zstrm;
			up_read(&zstrm->lock);
		}
	}

	/* Checks zcomp_dstrm_excl_tryget() before touching the waitq */
	wait_event(comp->dstrm_waitq, (zstrm = zcomp_dstrm_excl_tryget(comp)));
	return zstrm;
}

void zcomp_dstrm_put(struct zcomp *comp, struct zcomp_dstrm *zstrm,
		     enum zcomp_dstrm_mode mode)
{
	if (mode == ZCOMP_DSTRM_EXCLUSIVE) {
		mutex_unlock(&zstrm->excl_lock);
		up_read(&zstrm->lock);
		if (wq_has_sleeper(&comp->dstrm_waitq))
			wake_up(&comp->dstrm_waitq);
		return;
	}
	up_read(&zstrm->lock);
}

int zcomp_compress(struct zcomp *comp, struct zcomp_cstrm *zstrm,
		   const void *src, unsigned int *dst_len)
{
	struct zcomp_req req = {
		.src = src,
		.dst = zstrm->buffer,
		.src_len = PAGE_SIZE,
		.dst_len = 2 * PAGE_SIZE,
	};
	int ret;

	might_sleep();
	ret = comp->ops->compress(comp->params, &zstrm->ctx, &req);
	if (!ret)
		*dst_len = req.dst_len;
	return ret;
}

static int zcomp_decompress_buf(struct zcomp *comp, struct zcomp_dstrm *zstrm,
				struct scatterlist *sg, void *buf,
				struct zcomp_req *req)
{
	sg_copy_to_buffer(sg, 2, buf, req->src_len);
	req->src = buf;
	return comp->ops->decompress(comp->params, &zstrm->ctx, req);
}

int zcomp_decompress(struct zcomp *comp, struct zcomp_dstrm *zstrm,
		     struct scatterlist *sg, unsigned int src_len, void *dst)
{
	struct zcomp_req req = {
		.dst = dst,
		.src_len = src_len,
		.dst_len = PAGE_SIZE,
	};
	void *src;
	int ret;

	might_sleep();
	lockdep_assert_held(&zstrm->lock);
	if (!zcomp_shared_decomp(comp))
		lockdep_assert_held(&zstrm->excl_lock);

	if (sg_is_last(sg)) {
		/* the object is contained within one page, read it in-place */
		src = kmap_local_page(sg_page(sg));
		req.src = src + sg->offset;
		ret = comp->ops->decompress(comp->params, &zstrm->ctx, &req);
		kunmap_local(src);
		return ret;
	}

	/* exclusive mode already holds ->excl_lock */
	if (!zcomp_shared_decomp(comp))
		return zcomp_decompress_buf(comp, zstrm, sg, zstrm->local_copy,
					    &req);

	/* shared mode: try a per-request bounce buffer first */
	src = kmalloc(src_len, GFP_NOWAIT | __GFP_NOWARN);
	if (src) {
		ret = zcomp_decompress_buf(comp, zstrm, sg, src, &req);
		kfree(src);
		return ret;
	}

	/* fall back to ->local_copy serialized by ->excl_lock */
	mutex_lock(&zstrm->excl_lock);
	ret = zcomp_decompress_buf(comp, zstrm, sg, zstrm->local_copy, &req);
	mutex_unlock(&zstrm->excl_lock);
	if (wq_has_sleeper(&comp->dstrm_waitq))
		wake_up(&comp->dstrm_waitq);
	return ret;
}

int zcomp_cpu_up_prepare(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp, node);
	struct zcomp_cstrm *cstrm = NULL;
	struct zcomp_dstrm *dstrm;
	int ret;

	if (comp->cstream) {
		cstrm = per_cpu_ptr(comp->cstream, cpu);
		ret = zcomp_cstrm_init(comp, cstrm);
		if (ret) {
			pr_err("Can't allocate a compression stream\n");
			return ret;
		}
	}

	dstrm = per_cpu_ptr(comp->dstream, cpu);
	ret = zcomp_dstrm_init(comp, dstrm);
	if (ret) {
		pr_err("Can't allocate a decompression stream\n");
		if (cstrm)
			zcomp_cstrm_free(comp, cstrm);
		return ret;
	}

	return 0;
}

int zcomp_cpu_dead(unsigned int cpu, struct hlist_node *node)
{
	struct zcomp *comp = hlist_entry(node, struct zcomp, node);
	struct zcomp_cstrm *cstrm;
	struct zcomp_dstrm *dstrm;

	if (comp->cstream) {
		cstrm = per_cpu_ptr(comp->cstream, cpu);
		mutex_lock(&cstrm->lock);
		zcomp_cstrm_free(comp, cstrm);
		mutex_unlock(&cstrm->lock);
	}

	dstrm = per_cpu_ptr(comp->dstream, cpu);
	down_write(&dstrm->lock);
	zcomp_dstrm_free(comp, dstrm);
	up_write(&dstrm->lock);

	return 0;
}

static int zcomp_init(struct zcomp *comp, struct zcomp_params *params,
		      bool recomp)
{
	int ret, cpu;

	comp->params = params;
	ret = comp->ops->setup_params(comp->params);
	if (ret)
		goto cleanup;

	if (recomp) {
		comp->recomp_cstream = kzalloc_obj(*comp->recomp_cstream);
		if (!comp->recomp_cstream) {
			ret = -ENOMEM;
			goto cleanup;
		}
		mutex_init(&comp->recomp_cstream->lock);
		ret = zcomp_cstrm_init(comp, comp->recomp_cstream);
		if (ret) {
			/* zcomp_cstrm_init() cleans up after itself */
			kfree(comp->recomp_cstream);
			comp->recomp_cstream = NULL;
			goto cleanup;
		}
	} else {
		comp->cstream = alloc_percpu(struct zcomp_cstrm);
		if (!comp->cstream) {
			ret = -ENOMEM;
			goto cleanup;
		}
		for_each_possible_cpu(cpu)
			mutex_init(&per_cpu_ptr(comp->cstream, cpu)->lock);
	}

	comp->dstream = alloc_percpu(struct zcomp_dstrm);
	if (!comp->dstream) {
		ret = -ENOMEM;
		goto cleanup;
	}

	init_waitqueue_head(&comp->dstrm_waitq);

	for_each_possible_cpu(cpu) {
		struct zcomp_dstrm *zstrm = per_cpu_ptr(comp->dstream, cpu);

		init_rwsem(&zstrm->lock);
		mutex_init(&zstrm->excl_lock);
	}

	ret = cpuhp_state_add_instance(CPUHP_ZCOMP_PREPARE, &comp->node);
	if (ret < 0)
		goto cleanup;

	return 0;

cleanup:
	if (comp->recomp_cstream) {
		zcomp_cstrm_free(comp, comp->recomp_cstream);
		kfree(comp->recomp_cstream);
	}
	comp->ops->release_params(comp->params);
	free_percpu(comp->dstream);
	free_percpu(comp->cstream);
	return ret;
}

void zcomp_destroy(struct zcomp *comp)
{
	cpuhp_state_remove_instance(CPUHP_ZCOMP_PREPARE, &comp->node);
	if (comp->recomp_cstream) {
		zcomp_cstrm_free(comp, comp->recomp_cstream);
		kfree(comp->recomp_cstream);
	}
	comp->ops->release_params(comp->params);
	free_percpu(comp->dstream);
	free_percpu(comp->cstream);
	kfree(comp);
}

struct zcomp *zcomp_create(const char *alg, struct zcomp_params *params,
			   bool recomp)
{
	struct zcomp *comp;
	int error;

	/*
	 * The backends array has a sentinel NULL value, so the minimum
	 * size is 1. In order to be valid the array, apart from the
	 * sentinel NULL element, should have at least one compression
	 * backend selected.
	 */
	BUILD_BUG_ON(ARRAY_SIZE(backends) <= 1);

	comp = kzalloc_obj(struct zcomp);
	if (!comp)
		return ERR_PTR(-ENOMEM);

	comp->ops = lookup_backend_ops(alg);
	if (!comp->ops) {
		kfree(comp);
		return ERR_PTR(-EINVAL);
	}

	error = zcomp_init(comp, params, recomp);
	if (error) {
		kfree(comp);
		return ERR_PTR(error);
	}
	return comp;
}
