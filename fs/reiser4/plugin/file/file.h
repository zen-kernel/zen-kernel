/* Copyright 2001, 2002, 2003, 2004 by Hans Reiser, licensing governed by
 * reiser4/README */

/* this file contains declarations of methods implementing
   file plugins (UNIX_FILE_PLUGIN_ID, CRYPTCOMPRESS_FILE_PLUGIN_ID
   and SYMLINK_FILE_PLUGIN_ID) */

#if !defined( __REISER4_FILE_H__ )
#define __REISER4_FILE_H__

/* possible states in dispatching process */
typedef enum {
	DISPATCH_INVAL_STATE,  /* invalid state */
	DISPATCH_POINT,        /* dispatching point has been achieved */
	DISPATCH_REMAINS_OLD,  /* made a decision to manage by old plugin */
	DISPATCH_ASSIGNED_NEW  /* a new plugin has been assigned */
} dispatch_state;

struct dispatch_context {
	int nr_pages;
	struct page **pages;
	dispatch_state state;
};

/*
 * Declarations of methods provided for VFS.
 */

/* inode operations */
int reiser4_setattr_dispatch(struct dentry *, struct iattr *);

/* file operations */
ssize_t reiser4_read_dispatch(struct file *, char __user *buf,
			      size_t count, loff_t *off);
ssize_t reiser4_write_dispatch(struct file *, const char __user *buf,
			       size_t count, loff_t * off);
long reiser4_ioctl_dispatch(struct file *filp, unsigned int cmd,
			    unsigned long arg);
int reiser4_mmap_dispatch(struct file *, struct vm_area_struct *);
int reiser4_open_dispatch(struct inode *inode, struct file *file);
int reiser4_release_dispatch(struct inode *, struct file *);
int reiser4_sync_file_common(struct file *, loff_t, loff_t, int datasync);

/* address space operations */
int reiser4_readpage_dispatch(struct file *, struct page *);
int reiser4_readpages_dispatch(struct file *, struct address_space *,
			       struct list_head *, unsigned);
int reiser4_writepages_dispatch(struct address_space *,
				struct writeback_control *);
int reiser4_write_begin_dispatch(struct file *file,
				 struct address_space *mapping,
				 loff_t pos, unsigned len, unsigned flags,
				 struct page **pagep, void **fsdata);
int reiser4_write_end_dispatch(struct file *file,
			       struct address_space *mapping,
			       loff_t pos, unsigned len, unsigned copied,
			       struct page *page, void *fsdata);
sector_t reiser4_bmap_dispatch(struct address_space *, sector_t lblock);

/*
 * Private methods of unix-file plugin
 * (UNIX_FILE_PLUGIN_ID)
 */

/* private inode operations */
int setattr_unix_file(struct dentry *, struct iattr *);

/* private file operations */

ssize_t read_unix_file(struct file *, char __user *buf, size_t read_amount,
		       loff_t *off);
ssize_t write_unix_file(struct file *, const char __user *buf, size_t write_amount,
			loff_t * off, struct dispatch_context * cont);
int ioctl_unix_file(struct file *, unsigned int cmd, unsigned long arg);
int mmap_unix_file(struct file *, struct vm_area_struct *);
int open_unix_file(struct inode *, struct file *);
int release_unix_file(struct inode *, struct file *);

/* private address space operations */
int readpage_unix_file(struct file *, struct page *);
int readpages_unix_file(struct file*, struct address_space*, struct list_head*,
			unsigned);
int writepages_unix_file(struct address_space *, struct writeback_control *);
int write_begin_unix_file(struct file *file, struct page *page,
			  loff_t pos, unsigned len, void **fsdata);
int write_end_unix_file(struct file *file, struct page *page,
			loff_t pos, unsigned copied, void *fsdata);
sector_t bmap_unix_file(struct address_space *, sector_t lblock);

/* other private methods */
int delete_object_unix_file(struct inode *);
int flow_by_inode_unix_file(struct inode *, const char __user *buf,
			    int user, loff_t, loff_t, rw_op, flow_t *);
int owns_item_unix_file(const struct inode *, const coord_t *);
void init_inode_data_unix_file(struct inode *, reiser4_object_create_data *,
			       int create);

/*
 * Private methods of cryptcompress file plugin
 * (CRYPTCOMPRESS_FILE_PLUGIN_ID)
 */

/* private inode operations */
int setattr_cryptcompress(struct dentry *, struct iattr *);

/* private file operations */
ssize_t read_cryptcompress(struct file *, char __user *buf,
			   size_t count, loff_t *off);
ssize_t write_cryptcompress(struct file *, const char __user *buf,
			    size_t count, loff_t * off,
			    struct dispatch_context *cont);
int ioctl_cryptcompress(struct file *, unsigned int cmd, unsigned long arg);
int mmap_cryptcompress(struct file *, struct vm_area_struct *);
int open_cryptcompress(struct inode *, struct file *);
int release_cryptcompress(struct inode *, struct file *);

/* private address space operations */
int readpage_cryptcompress(struct file *, struct page *);
int readpages_cryptcompress(struct file*, struct address_space*,
			    struct list_head*, unsigned);
int writepages_cryptcompress(struct address_space *,
			     struct writeback_control *);
int write_begin_cryptcompress(struct file *file, struct page *page,
			      loff_t pos, unsigned len, void **fsdata);
int write_end_cryptcompress(struct file *file, struct page *page,
			    loff_t pos, unsigned copied, void *fsdata);
sector_t bmap_cryptcompress(struct address_space *, sector_t lblock);

/* other private methods */
int flow_by_inode_cryptcompress(struct inode *, const char __user *buf,
				int user, loff_t, loff_t, rw_op, flow_t *);
int key_by_inode_cryptcompress(struct inode *, loff_t off, reiser4_key *);
int create_object_cryptcompress(struct inode *, struct inode *,
				reiser4_object_create_data *);
int delete_object_cryptcompress(struct inode *);
void init_inode_data_cryptcompress(struct inode *, reiser4_object_create_data *,
				   int create);
int cut_tree_worker_cryptcompress(tap_t *, const reiser4_key * from_key,
				  const reiser4_key * to_key,
				  reiser4_key * smallest_removed,
				  struct inode *object, int truncate,
				  int *progress);
void destroy_inode_cryptcompress(struct inode *);

/*
 * Private methods of symlink file plugin
 * (SYMLINK_FILE_PLUGIN_ID)
 */
int reiser4_create_symlink(struct inode *symlink, struct inode *dir,
			   reiser4_object_create_data *);
void destroy_inode_symlink(struct inode *);

/*
 * all the write into unix file is performed by item write method. Write method
 * of unix file plugin only decides which item plugin (extent or tail) and in
 * which mode (one from the enum below) to call
 */
typedef enum {
	FIRST_ITEM = 1,
	APPEND_ITEM = 2,
	OVERWRITE_ITEM = 3
} write_mode_t;

/* unix file may be in one the following states */
typedef enum {
	UF_CONTAINER_UNKNOWN = 0,
	UF_CONTAINER_TAILS = 1,
	UF_CONTAINER_EXTENTS = 2,
	UF_CONTAINER_EMPTY = 3
} file_container_t;

struct formatting_plugin;
struct inode;

/* unix file plugin specific part of reiser4 inode */
struct unix_file_info {
	/*
	 * this read-write lock protects file containerization change. Accesses
	 * which do not change file containerization (see file_container_t)
	 * (read, readpage, writepage, write (until tail conversion is
	 * involved)) take read-lock. Accesses which modify file
	 * containerization (truncate, conversion from tail to extent and back)
	 * take write-lock.
	 */
	struct rw_semaphore latch;
	/* this enum specifies which items are used to build the file */
	file_container_t container;
	/*
	 * plugin which controls when file is to be converted to extents and
	 * back to tail
	 */
	struct formatting_plugin *tplug;
	/* if this is set, file is in exclusive use */
	int exclusive_use;
#if REISER4_DEBUG
	/* pointer to task struct of thread owning exclusive access to file */
	void *ea_owner;
	atomic_t nr_neas;
	void *last_reader;
#endif
};

struct unix_file_info *unix_file_inode_data(const struct inode *inode);
void get_exclusive_access(struct unix_file_info *);
void drop_exclusive_access(struct unix_file_info *);
void get_nonexclusive_access(struct unix_file_info *);
void drop_nonexclusive_access(struct unix_file_info *);
int try_to_get_nonexclusive_access(struct unix_file_info *);
int find_file_item(hint_t *, const reiser4_key *, znode_lock_mode,
		   struct inode *);
int find_file_item_nohint(coord_t *, lock_handle *,
			  const reiser4_key *, znode_lock_mode,
			  struct inode *);

int load_file_hint(struct file *, hint_t *);
void save_file_hint(struct file *, const hint_t *);

#include "../item/extent.h"
#include "../item/tail.h"
#include "../item/ctail.h"

struct uf_coord {
	coord_t coord;
	lock_handle *lh;
	int valid;
	union {
		struct extent_coord_extension extent;
		struct tail_coord_extension tail;
		struct ctail_coord_extension ctail;
	} extension;
};

#include "../../forward.h"
#include "../../seal.h"
#include "../../lock.h"

/*
 * This structure is used to speed up file operations (reads and writes).  A
 * hint is a suggestion about where a key resolved to last time.  A seal
 * indicates whether a node has been modified since a hint was last recorded.
 * You check the seal, and if the seal is still valid, you can use the hint
 * without traversing the tree again.
 */
struct hint {
	seal_t seal; /* a seal over last file item accessed */
	uf_coord_t ext_coord;
	loff_t offset;
	znode_lock_mode mode;
	lock_handle lh;
};

static inline int hint_is_valid(hint_t * hint)
{
	return hint->ext_coord.valid;
}

static inline void hint_set_valid(hint_t * hint)
{
	hint->ext_coord.valid = 1;
}

static inline void hint_clr_valid(hint_t * hint)
{
	hint->ext_coord.valid = 0;
}

int load_file_hint(struct file *, hint_t *);
void save_file_hint(struct file *, const hint_t *);
void hint_init_zero(hint_t *);
void reiser4_set_hint(hint_t *, const reiser4_key *, znode_lock_mode);
int hint_is_set(const hint_t *);
void reiser4_unset_hint(hint_t *);

int reiser4_update_file_size(struct inode *, loff_t, int update_sd);
int cut_file_items(struct inode *, loff_t new_size,
		   int update_sd, loff_t cur_size,
		   int (*update_actor) (struct inode *, loff_t, int));
#if REISER4_DEBUG

/* return 1 is exclusive access is obtained, 0 - otherwise */
static inline int ea_obtained(struct unix_file_info * uf_info)
{
	int ret;

	ret = down_read_trylock(&uf_info->latch);
	if (ret)
		up_read(&uf_info->latch);
	return !ret;
}

#endif

#define WRITE_GRANULARITY 32

int tail2extent(struct unix_file_info *);
int extent2tail(struct file *, struct unix_file_info *);

int goto_right_neighbor(coord_t *, lock_handle *);
int find_or_create_extent(struct page *);
int equal_to_ldk(znode *, const reiser4_key *);

void init_uf_coord(uf_coord_t *uf_coord, lock_handle *lh);

static inline int cbk_errored(int cbk_result)
{
	return (cbk_result != CBK_COORD_NOTFOUND
		&& cbk_result != CBK_COORD_FOUND);
}

/* __REISER4_FILE_H__ */
#endif

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 79
 * scroll-step: 1
 * End:
*/
