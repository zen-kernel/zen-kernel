#include <linux/err.h>
#include "debug.h"
#include "checksum.h"

int reiser4_init_csum_tfm(struct crypto_shash **tfm)
{
	struct crypto_shash *new_tfm;

	new_tfm = crypto_alloc_shash("crc32c", 0, 0);
	if (IS_ERR(new_tfm)) {
		warning("intelfx-81", "Could not load crc32c driver");
		return PTR_ERR(new_tfm);
	}

	*tfm = new_tfm;
	return 0;
}

void reiser4_done_csum_tfm(struct crypto_shash *tfm)
{
	crypto_free_shash(tfm);
}

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
