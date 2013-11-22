/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* reiser4 digest transform plugin (is used by cryptcompress object plugin) */
/* EDWARD-FIXME-HANS: and it does what? a digest is a what? */
#include "../../debug.h"
#include "../plugin_header.h"
#include "../plugin.h"
#include "../file/cryptcompress.h"

#include <linux/types.h>

extern digest_plugin digest_plugins[LAST_DIGEST_ID];

static struct crypto_hash * alloc_sha256 (void)
{
#if REISER4_SHA256
	return crypto_alloc_hash ("sha256", 0, CRYPTO_ALG_ASYNC);
#else
	warning("edward-1418", "sha256 unsupported");
	return ERR_PTR(-EINVAL);
#endif
}

static void free_sha256 (struct crypto_hash * tfm)
{
#if REISER4_SHA256
	crypto_free_hash(tfm);
#endif
	return;
}

/* digest plugins */
digest_plugin digest_plugins[LAST_DIGEST_ID] = {
	[SHA256_32_DIGEST_ID] = {
		.h = {
			.type_id = REISER4_DIGEST_PLUGIN_TYPE,
			.id = SHA256_32_DIGEST_ID,
			.pops = NULL,
			.label = "sha256_32",
			.desc = "sha256_32 digest transform",
			.linkage = {NULL, NULL}
		},
		.fipsize = sizeof(__u32),
		.alloc = alloc_sha256,
		.free = free_sha256
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
