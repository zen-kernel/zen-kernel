/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */
/* This file contains definitions for the objects operated
   by reiser4 key manager, which is something like keyring
   wrapped by appropriate reiser4 plugin */

#if !defined( __FS_REISER4_CRYPT_H__ )
#define __FS_REISER4_CRYPT_H__

#include <linux/crypto.h>

/* key info imported from user space */
struct reiser4_crypto_data {
	int keysize;    /* uninstantiated key size */
	__u8 * key;     /* uninstantiated key */
	int keyid_size; /* size of passphrase */
	__u8 * keyid;   /* passphrase */
};

/* This object contains all needed infrastructure to implement
   cipher transform. This is operated (allocating, inheriting,
   validating, binding to host inode, etc..) by reiser4 key manager.

   This info can be allocated in two cases:
   1. importing a key from user space.
   2. reading inode from disk */
struct reiser4_crypto_info {
	struct inode * host;
	struct crypto_hash      * digest;
	struct crypto_blkcipher * cipher;
#if 0
	cipher_key_plugin * kplug; /* key manager */
#endif
	__u8 * keyid;              /* key fingerprint, created by digest plugin,
				      using uninstantiated key and passphrase.
				      supposed to be stored in disk stat-data */
	int inst;                  /* this indicates if the cipher key is
				      instantiated (case 1 above) */
	int keysize;               /* uninstantiated key size (bytes), supposed
				      to be stored in disk stat-data */
	int keyload_count;         /* number of the objects which has this
				      crypto-stat attached */
};

#endif /* __FS_REISER4_CRYPT_H__ */

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
