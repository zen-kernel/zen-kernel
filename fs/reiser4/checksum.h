#ifndef __CHECKSUM__
#define __CHECKSUM__

#include <crypto/hash.h>

int reiser4_init_csum_tfm(struct crypto_shash **tfm);
void reiser4_done_csum_tfm(struct crypto_shash *tfm);
u32 static inline reiser4_crc32c(struct crypto_shash *tfm,
				 u32 crc, const void *address,
				 unsigned int length)
{
	struct {
		struct shash_desc shash;
		char ctx[4];
	} desc;
	int err;

	desc.shash.tfm = tfm;
	desc.shash.flags = 0;
	*(u32 *)desc.ctx = crc;

	err = crypto_shash_update(&desc.shash, address, length);
	BUG_ON(err);
	return *(u32 *)desc.ctx;
}

#endif /* __CHECKSUM__ */

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

