/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

#if !defined(__REISER4_FSDATA_H__)
#define __REISER4_FSDATA_H__

#include "debug.h"
#include "kassign.h"
#include "seal.h"
#include "type_safe_hash.h"
#include "plugin/file/file.h"
#include "readahead.h"

/*
 * comment about reiser4_dentry_fsdata
 *
 *
 */

/*
 * locking: fields of per file descriptor readdir_pos and ->f_pos are
 * protected by ->i_mutex on inode. Under this lock following invariant
 * holds:
 *
 *     file descriptor is "looking" at the entry_no-th directory entry from
 *     the beginning of directory. This entry has key dir_entry_key and is
 *     pos-th entry with duplicate-key sequence.
 *
 */

/* logical position within directory */
struct dir_pos {
	/* key of directory entry (actually, part of a key sufficient to
	   identify directory entry)  */
	de_id dir_entry_key;
	/* ordinal number of directory entry among all entries with the same
	   key. (Starting from 0.) */
	unsigned pos;
};

struct readdir_pos {
	/* f_pos corresponding to this readdir position */
	__u64 fpos;
	/* logical position within directory */
	struct dir_pos position;
	/* logical number of directory entry within
	   directory  */
	__u64 entry_no;
};

/*
 * this is used to speed up lookups for directory entry: on initial call to
 * ->lookup() seal and coord of directory entry (if found, that is) are stored
 * in struct dentry and reused later to avoid tree traversals.
 */
struct de_location {
	/* seal covering directory entry */
	seal_t entry_seal;
	/* coord of directory entry */
	coord_t entry_coord;
	/* ordinal number of directory entry among all entries with the same
	   key. (Starting from 0.) */
	int pos;
};

/**
 * reiser4_dentry_fsdata - reiser4-specific data attached to dentries
 *
 * This is allocated dynamically and released in d_op->d_release()
 *
 * Currently it only contains cached location (hint) of directory entry, but
 * it is expected that other information will be accumulated here.
 */
struct reiser4_dentry_fsdata {
	/*
	 * here will go fields filled by ->lookup() to speedup next
	 * create/unlink, like blocknr of znode with stat-data, or key of
	 * stat-data.
	 */
	struct de_location dec;
	int stateless;		/* created through reiser4_decode_fh, needs
				 * special treatment in readdir. */
};

extern int reiser4_init_dentry_fsdata(void);
extern void reiser4_done_dentry_fsdata(void);
extern struct reiser4_dentry_fsdata *reiser4_get_dentry_fsdata(struct dentry *);
extern void reiser4_free_dentry_fsdata(struct dentry *dentry);

/**
 * reiser4_file_fsdata - reiser4-specific data attached to file->private_data
 *
 * This is allocated dynamically and released in inode->i_fop->release
 */
typedef struct reiser4_file_fsdata {
	/*
	 * pointer back to the struct file which this reiser4_file_fsdata is
	 * part of
	 */
	struct file *back;
	/* detached cursor for stateless readdir. */
	struct dir_cursor *cursor;
	/*
	 * We need both directory and regular file parts here, because there
	 * are file system objects that are files and directories.
	 */
	struct {
		/*
		 * position in directory. It is updated each time directory is
		 * modified
		 */
		struct readdir_pos readdir;
		/* head of this list is reiser4_inode->lists.readdir_list */
		struct list_head linkage;
	} dir;
	/* hints to speed up operations with regular files: read and write. */
	struct {
		hint_t hint;
	} reg;
} reiser4_file_fsdata;

extern int reiser4_init_file_fsdata(void);
extern void reiser4_done_file_fsdata(void);
extern reiser4_file_fsdata *reiser4_get_file_fsdata(struct file *);
extern void reiser4_free_file_fsdata(struct file *);

/*
 * d_cursor is reiser4_file_fsdata not attached to struct file. d_cursors are
 * used to address problem reiser4 has with readdir accesses via NFS. See
 * plugin/file_ops_readdir.c for more details.
 */
struct d_cursor_key{
	__u16 cid;
	__u64 oid;
};

/*
 * define structures d_cursor_hash_table d_cursor_hash_link which are used to
 * maintain hash table of dir_cursor-s in reiser4's super block
 */
typedef struct dir_cursor dir_cursor;
TYPE_SAFE_HASH_DECLARE(d_cursor, dir_cursor);

struct dir_cursor {
	int ref;
	reiser4_file_fsdata *fsdata;

	/* link to reiser4 super block hash table of cursors */
	d_cursor_hash_link hash;

	/*
	 * this is to link cursors to reiser4 super block's radix tree of
	 * cursors if there are more than one cursor of the same objectid
	 */
	struct list_head list;
	struct d_cursor_key key;
	struct d_cursor_info *info;
	/* list of unused cursors */
	struct list_head alist;
};

extern int reiser4_init_d_cursor(void);
extern void reiser4_done_d_cursor(void);

extern int reiser4_init_super_d_info(struct super_block *);
extern void reiser4_done_super_d_info(struct super_block *);

extern loff_t reiser4_get_dir_fpos(struct file *, loff_t);
extern int reiser4_attach_fsdata(struct file *, loff_t *, struct inode *);
extern void reiser4_detach_fsdata(struct file *);

/* these are needed for "stateless" readdir. See plugin/file_ops_readdir.c for
   more details */
void reiser4_dispose_cursors(struct inode *inode);
void reiser4_load_cursors(struct inode *inode);
void reiser4_kill_cursors(struct inode *inode);
void reiser4_adjust_dir_file(struct inode *dir, const struct dentry *de,
			     int offset, int adj);

/*
 * this structure is embedded to reise4_super_info_data. It maintains d_cursors
 * (detached readdir state). See plugin/file_ops_readdir.c for more details.
 */
struct d_cursor_info {
	d_cursor_hash_table table;
	struct radix_tree_root tree;
};

/* spinlock protecting readdir cursors */
extern spinlock_t d_c_lock;

/* __REISER4_FSDATA_H__ */
#endif

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 120
 * End:
 */
