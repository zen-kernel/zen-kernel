#if !defined( __FS_REISER4_COMPRESS_H__ )
#define __FS_REISER4_COMPRESS_H__

#include <linux/types.h>
#include <linux/string.h>

/* transform direction */
typedef enum {
	TFMA_READ,   /* decrypt, decompress */
	TFMA_WRITE,  /* encrypt, compress */
	TFMA_LAST
} tfm_action;

/* supported compression algorithms */
typedef enum {
	LZO1_COMPRESSION_ID,
	GZIP1_COMPRESSION_ID,
	LAST_COMPRESSION_ID,
} reiser4_compression_id;

/* the same as pgoff, but units are page clusters */
typedef unsigned long cloff_t;

/* working data of a (de)compression algorithm */
typedef void *coa_t;

/* table for all supported (de)compression algorithms */
typedef coa_t coa_set[LAST_COMPRESSION_ID][TFMA_LAST];

__u32 reiser4_adler32(char *data, __u32 len);

#endif				/* __FS_REISER4_COMPRESS_H__ */

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   scroll-step: 1
   End:
*/
