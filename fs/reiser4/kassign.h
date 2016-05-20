/* Copyright 2001, 2002, 2003, 2004 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Key assignment policy interface. See kassign.c for details. */

#if !defined(__KASSIGN_H__)
#define __KASSIGN_H__

#include "forward.h"
#include "key.h"
#include "dformat.h"

#include <linux/types.h>	/* for __u??  */
#include <linux/fs.h>		/* for struct super_block, etc  */
#include <linux/dcache.h>	/* for struct qstr */

/* key assignment functions */

/* Information from which key of file stat-data can be uniquely
   restored. This depends on key assignment policy for
   stat-data. Currently it's enough to store object id and locality id
   (60+60==120) bits, because minor packing locality and offset of
   stat-data key are always known constants: KEY_SD_MINOR and 0
   respectively. For simplicity 4 bits are wasted in each id, and just
   two 64 bit integers are stored.

   This field has to be byte-aligned, because we don't want to waste
   space in directory entries. There is another side of a coin of
   course: we waste CPU and bus bandwidth in stead, by copying data back
   and forth.

   Next optimization: &obj_key_id is mainly used to address stat data from
   directory entries. Under the assumption that majority of files only have
   only name (one hard link) from *the* parent directory it seems reasonable
   to only store objectid of stat data and take its locality from key of
   directory item.

   This requires some flag to be added to the &obj_key_id to distinguish
   between these two cases. Remaining bits in flag byte are then asking to be
   used to store file type.

   This optimization requires changes in directory item handling code.

*/
typedef struct obj_key_id {
	d8 locality[sizeof(__u64)];
	 ON_LARGE_KEY(d8 ordering[sizeof(__u64)];
	    )
	d8 objectid[sizeof(__u64)];
}
obj_key_id;

/* Information sufficient to uniquely identify directory entry within
   compressed directory item.

   For alignment issues see &obj_key_id above.
*/
typedef struct de_id {
	ON_LARGE_KEY(d8 ordering[sizeof(__u64)];)
	d8 objectid[sizeof(__u64)];
	d8 offset[sizeof(__u64)];
}
de_id;

extern int inode_onwire_size(const struct inode *obj);
extern char *build_inode_onwire(const struct inode *obj, char *area);
extern char *locate_obj_key_id_onwire(char *area);
extern char *extract_obj_key_id_from_onwire(char *area, obj_key_id * key_id);

extern int build_inode_key_id(const struct inode *obj, obj_key_id * id);
extern int extract_key_from_id(const obj_key_id * id, reiser4_key * key);
extern int build_obj_key_id(const reiser4_key * key, obj_key_id * id);
extern oid_t extract_dir_id_from_key(const reiser4_key * de_key);
extern int build_de_id(const struct inode *dir, const struct qstr *name,
		       de_id * id);
extern int build_de_id_by_key(const reiser4_key * entry_key, de_id * id);
extern int extract_key_from_de_id(const oid_t locality, const de_id * id,
				  reiser4_key * key);
extern cmp_t de_id_cmp(const de_id * id1, const de_id * id2);
extern cmp_t de_id_key_cmp(const de_id * id, const reiser4_key * key);

extern int build_readdir_key_common(struct file *dir, reiser4_key * result);
extern void build_entry_key_common(const struct inode *dir,
				   const struct qstr *name,
				   reiser4_key * result);
extern void build_entry_key_stable_entry(const struct inode *dir,
					 const struct qstr *name,
					 reiser4_key * result);
extern int is_dot_key(const reiser4_key * key);
extern reiser4_key *build_sd_key(const struct inode *target,
				 reiser4_key * result);

extern int is_longname_key(const reiser4_key * key);
extern int is_longname(const char *name, int len);
extern char *extract_name_from_key(const reiser4_key * key, char *buf);
extern char *reiser4_unpack_string(__u64 value, char *buf);
extern void complete_entry_key(const struct inode *dir, const char *name,
			       int len, reiser4_key *result);

/* __KASSIGN_H__ */
#endif

/* Make Linus happy.
   Local variables:
   c-indentation-style: "K&R"
   mode-name: "LC"
   c-basic-offset: 8
   tab-width: 8
   fill-column: 120
   End:
*/
