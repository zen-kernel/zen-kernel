/* SPDX-License-Identifier: GPL-2.0-or-later */

#ifndef _ZCOMP_H_
#define _ZCOMP_H_

#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/wait.h>

struct scatterlist;

#define ZCOMP_PARAM_NOT_SET	INT_MIN

enum zcomp_dstrm_mode {
	ZCOMP_DSTRM_SHARED,
	ZCOMP_DSTRM_EXCLUSIVE,
};

struct deflate_params {
	s32 winbits;
};

/*
 * Immutable driver (backend) parameters. The driver may attach private
 * data to it (e.g. driver representation of the dictionary, etc.).
 *
 * This data is kept per-comp and is shared among execution contexts.
 */
struct zcomp_params {
	void *dict;
	size_t dict_sz;
	s32 level;
	union {
		struct deflate_params deflate;
	};

	void *drv_data;
};

/*
 * Run-time driver context - scratch buffers, etc. It is modified during
 * request execution (compression/decompression), cannot be shared, so
 * it's in per-CPU area.
 */
struct zcomp_ctx {
	void *context;
};

struct zcomp_cstrm {
	struct mutex lock;
	/* compression buffer */
	void *buffer;
	struct zcomp_ctx ctx;
};

struct zcomp_dstrm {
	/*
	 * Pins the stream: held in read mode by every user, regardless
	 * of the decompression mode, and in write mode only by
	 * zcomp_cpu_dead().
	 */
	struct rw_semaphore lock;
	/* Serializes users of the mutable stream state: ->local_copy, ->ctx */
	struct mutex excl_lock;
	/* local copy of handle memory */
	void *local_copy;
	struct zcomp_ctx ctx;
};

struct zcomp_req {
	const unsigned char *src;
	const size_t src_len;

	unsigned char *dst;
	size_t dst_len;
};

struct zcomp_ops {
	int (*compress)(struct zcomp_params *params, struct zcomp_ctx *ctx,
			struct zcomp_req *req);
	int (*decompress)(struct zcomp_params *params, struct zcomp_ctx *ctx,
			  struct zcomp_req *req);

	int (*create_cctx)(struct zcomp_params *params, struct zcomp_ctx *ctx);
	void (*destroy_cctx)(struct zcomp_ctx *ctx);

	int (*create_dctx)(struct zcomp_params *params, struct zcomp_ctx *ctx);
	void (*destroy_dctx)(struct zcomp_ctx *ctx);

	int (*setup_params)(struct zcomp_params *params);
	void (*release_params)(struct zcomp_params *params);

	const char *name;
};

/* dynamic per-device compression frontend */
struct zcomp {
	struct zcomp_cstrm __percpu *cstream;
	struct zcomp_dstrm __percpu *dstream;
	wait_queue_head_t dstrm_waitq;
	const struct zcomp_ops *ops;
	struct zcomp_params *params;
	struct hlist_node node;
};

int zcomp_cpu_up_prepare(unsigned int cpu, struct hlist_node *node);
int zcomp_cpu_dead(unsigned int cpu, struct hlist_node *node);
ssize_t zcomp_available_show(const char *comp, char *buf, ssize_t at);
const char *zcomp_lookup_backend_name(const char *comp);

struct zcomp *zcomp_create(const char *alg, struct zcomp_params *params);
void zcomp_destroy(struct zcomp *comp);

struct zcomp_cstrm *zcomp_cstrm_get(struct zcomp *comp);
void zcomp_cstrm_put(struct zcomp_cstrm *zstrm);

bool zcomp_shared_decomp(struct zcomp *comp);
struct zcomp_dstrm *zcomp_dstrm_get(struct zcomp *comp,
				    enum zcomp_dstrm_mode mode);
void zcomp_dstrm_put(struct zcomp *comp, struct zcomp_dstrm *zstrm,
		     enum zcomp_dstrm_mode mode);

int zcomp_compress(struct zcomp *comp, struct zcomp_cstrm *zstrm,
		   const void *src, unsigned int *dst_len);
int zcomp_decompress(struct zcomp *comp, struct zcomp_dstrm *zstrm,
		     struct scatterlist *sg, unsigned int src_len, void *dst);

#endif /* _ZCOMP_H_ */
