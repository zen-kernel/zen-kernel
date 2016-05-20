/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Hash functions */

#include "../debug.h"
#include "plugin_header.h"
#include "plugin.h"
#include "../super.h"
#include "../inode.h"

#include <linux/types.h>

/* old rupasov (yura) hash */
static __u64 hash_rupasov(const unsigned char *name /* name to hash */ ,
			  int len/* @name's length */)
{
	int i;
	int j;
	int pow;
	__u64 a;
	__u64 c;

	assert("nikita-672", name != NULL);
	assert("nikita-673", len >= 0);

	for (pow = 1, i = 1; i < len; ++i)
		pow = pow * 10;

	if (len == 1)
		a = name[0] - 48;
	else
		a = (name[0] - 48) * pow;

	for (i = 1; i < len; ++i) {
		c = name[i] - 48;
		for (pow = 1, j = i; j < len - 1; ++j)
			pow = pow * 10;
		a = a + c * pow;
	}
	for (; i < 40; ++i) {
		c = '0' - 48;
		for (pow = 1, j = i; j < len - 1; ++j)
			pow = pow * 10;
		a = a + c * pow;
	}

	for (; i < 256; ++i) {
		c = i;
		for (pow = 1, j = i; j < len - 1; ++j)
			pow = pow * 10;
		a = a + c * pow;
	}

	a = a << 7;
	return a;
}

/* r5 hash */
static __u64 hash_r5(const unsigned char *name /* name to hash */ ,
		     int len UNUSED_ARG/* @name's length */)
{
	__u64 a = 0;

	assert("nikita-674", name != NULL);
	assert("nikita-675", len >= 0);

	while (*name) {
		a += *name << 4;
		a += *name >> 4;
		a *= 11;
		name++;
	}
	return a;
}

/* Keyed 32-bit hash function using TEA in a Davis-Meyer function
     H0 = Key
     Hi = E Mi(Hi-1) + Hi-1

   (see Applied Cryptography, 2nd edition, p448).

   Jeremy Fitzhardinge <jeremy@zip.com.au> 1998

   Jeremy has agreed to the contents of reiserfs/README. -Hans

   This code was blindly upgraded to __u64 by s/__u32/__u64/g.
*/
static __u64 hash_tea(const unsigned char *name /* name to hash */ ,
		      int len/* @name's length */)
{
	__u64 k[] = { 0x9464a485u, 0x542e1a94u, 0x3e846bffu, 0xb75bcfc3u };

	__u64 h0 = k[0], h1 = k[1];
	__u64 a, b, c, d;
	__u64 pad;
	int i;

	assert("nikita-676", name != NULL);
	assert("nikita-677", len >= 0);

#define DELTA 0x9E3779B9u
#define FULLROUNDS 10		/* 32 is overkill, 16 is strong crypto */
#define PARTROUNDS 6		/* 6 gets complete mixing */

/* a, b, c, d - data; h0, h1 - accumulated hash */
#define TEACORE(rounds)							\
	do {								\
		__u64 sum = 0;						\
		int n = rounds;						\
		__u64 b0, b1;						\
									\
		b0 = h0;						\
		b1 = h1;						\
									\
		do {							\
			sum += DELTA;					\
			b0 += ((b1 << 4)+a) ^ (b1+sum) ^ ((b1 >> 5)+b);	\
			b1 += ((b0 << 4)+c) ^ (b0+sum) ^ ((b0 >> 5)+d);	\
		} while (--n);						\
									\
		h0 += b0;						\
		h1 += b1;						\
	} while (0)

	pad = (__u64) len | ((__u64) len << 8);
	pad |= pad << 16;

	while (len >= 16) {
		a = (__u64) name[0] | (__u64) name[1] << 8 | (__u64) name[2] <<
		    16 | (__u64) name[3] << 24;
		b = (__u64) name[4] | (__u64) name[5] << 8 | (__u64) name[6] <<
		    16 | (__u64) name[7] << 24;
		c = (__u64) name[8] | (__u64) name[9] << 8 | (__u64) name[10] <<
		    16 | (__u64) name[11] << 24;
		d = (__u64) name[12] | (__u64) name[13] << 8 | (__u64) name[14]
		    << 16 | (__u64) name[15] << 24;

		TEACORE(PARTROUNDS);

		len -= 16;
		name += 16;
	}

	if (len >= 12) {
		/* assert(len < 16); */
		if (len >= 16)
			*(int *)0 = 0;

		a = (__u64) name[0] | (__u64) name[1] << 8 | (__u64) name[2] <<
		    16 | (__u64) name[3] << 24;
		b = (__u64) name[4] | (__u64) name[5] << 8 | (__u64) name[6] <<
		    16 | (__u64) name[7] << 24;
		c = (__u64) name[8] | (__u64) name[9] << 8 | (__u64) name[10] <<
		    16 | (__u64) name[11] << 24;

		d = pad;
		for (i = 12; i < len; i++) {
			d <<= 8;
			d |= name[i];
		}
	} else if (len >= 8) {
		/* assert(len < 12); */
		if (len >= 12)
			*(int *)0 = 0;
		a = (__u64) name[0] | (__u64) name[1] << 8 | (__u64) name[2] <<
		    16 | (__u64) name[3] << 24;
		b = (__u64) name[4] | (__u64) name[5] << 8 | (__u64) name[6] <<
		    16 | (__u64) name[7] << 24;

		c = d = pad;
		for (i = 8; i < len; i++) {
			c <<= 8;
			c |= name[i];
		}
	} else if (len >= 4) {
		/* assert(len < 8); */
		if (len >= 8)
			*(int *)0 = 0;
		a = (__u64) name[0] | (__u64) name[1] << 8 | (__u64) name[2] <<
		    16 | (__u64) name[3] << 24;

		b = c = d = pad;
		for (i = 4; i < len; i++) {
			b <<= 8;
			b |= name[i];
		}
	} else {
		/* assert(len < 4); */
		if (len >= 4)
			*(int *)0 = 0;
		a = b = c = d = pad;
		for (i = 0; i < len; i++) {
			a <<= 8;
			a |= name[i];
		}
	}

	TEACORE(FULLROUNDS);

/*	return 0;*/
	return h0 ^ h1;

}

/* classical 64 bit Fowler/Noll/Vo-1 (FNV-1) hash.

   See http://www.isthe.com/chongo/tech/comp/fnv/ for details.

   Excerpts:

     FNV hashes are designed to be fast while maintaining a low collision
     rate.

     [This version also seems to preserve lexicographical order locally.]

     FNV hash algorithms and source code have been released into the public
     domain.

*/
static __u64 hash_fnv1(const unsigned char *name /* name to hash */ ,
		       int len UNUSED_ARG/* @name's length */)
{
	unsigned long long a = 0xcbf29ce484222325ull;
	const unsigned long long fnv_64_prime = 0x100000001b3ull;

	assert("nikita-678", name != NULL);
	assert("nikita-679", len >= 0);

	/* FNV-1 hash each octet in the buffer */
	for (; *name; ++name) {
		/* multiply by the 32 bit FNV magic prime mod 2^64 */
		a *= fnv_64_prime;
		/* xor the bottom with the current octet */
		a ^= (unsigned long long)(*name);
	}
	/* return our new hash value */
	return a;
}

/* degenerate hash function used to simplify testing of non-unique key
   handling */
static __u64 hash_deg(const unsigned char *name UNUSED_ARG /* name to hash */ ,
		      int len UNUSED_ARG/* @name's length */)
{
	return 0xc0c0c0c010101010ull;
}

static int change_hash(struct inode *inode,
		       reiser4_plugin * plugin,
		       pset_member memb)
{
	int result;

	assert("nikita-3503", inode != NULL);
	assert("nikita-3504", plugin != NULL);

	assert("nikita-3505", is_reiser4_inode(inode));
	assert("nikita-3507", plugin->h.type_id == REISER4_HASH_PLUGIN_TYPE);

	if (!plugin_of_group(inode_file_plugin(inode), REISER4_DIRECTORY_FILE))
		return RETERR(-EINVAL);

	result = 0;
	if (inode_hash_plugin(inode) == NULL ||
	    inode_hash_plugin(inode)->h.id != plugin->h.id) {
		if (is_dir_empty(inode) == 0)
			result = aset_set_unsafe(&reiser4_inode_data(inode)->pset,
						 PSET_HASH, plugin);
		else
			result = RETERR(-ENOTEMPTY);

	}
	return result;
}

static reiser4_plugin_ops hash_plugin_ops = {
	.init = NULL,
	.load = NULL,
	.save_len = NULL,
	.save = NULL,
	.change = change_hash
};

/* hash plugins */
hash_plugin hash_plugins[LAST_HASH_ID] = {
	[RUPASOV_HASH_ID] = {
		.h = {
			.type_id = REISER4_HASH_PLUGIN_TYPE,
			.id = RUPASOV_HASH_ID,
			.pops = &hash_plugin_ops,
			.label = "rupasov",
			.desc = "Original Yura's hash",
			.linkage = {NULL, NULL}
		},
		.hash = hash_rupasov
	},
	[R5_HASH_ID] = {
		.h = {
			.type_id = REISER4_HASH_PLUGIN_TYPE,
			.id = R5_HASH_ID,
			.pops = &hash_plugin_ops,
			.label = "r5",
			.desc = "r5 hash",
			.linkage = {NULL, NULL}
		},
		.hash = hash_r5
	},
	[TEA_HASH_ID] = {
		.h = {
			.type_id = REISER4_HASH_PLUGIN_TYPE,
			.id = TEA_HASH_ID,
			.pops = &hash_plugin_ops,
			.label = "tea",
			.desc = "tea hash",
			.linkage = {NULL, NULL}
		},
		.hash = hash_tea
	},
	[FNV1_HASH_ID] = {
		.h = {
			.type_id = REISER4_HASH_PLUGIN_TYPE,
			.id = FNV1_HASH_ID,
			.pops = &hash_plugin_ops,
			.label = "fnv1",
			.desc = "fnv1 hash",
			.linkage = {NULL, NULL}
		},
		.hash = hash_fnv1
	},
	[DEGENERATE_HASH_ID] = {
		.h = {
			.type_id = REISER4_HASH_PLUGIN_TYPE,
			.id = DEGENERATE_HASH_ID,
			.pops = &hash_plugin_ops,
			.label = "degenerate hash",
			.desc = "Degenerate hash: only for testing",
			.linkage = {NULL, NULL}
		},
		.hash = hash_deg
	}
};

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
