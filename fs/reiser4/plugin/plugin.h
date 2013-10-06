/* Copyright 2001, 2002, 2003 by Hans Reiser, licensing governed by
 * reiser4/README */

/* Basic plugin data-types.
   see fs/reiser4/plugin/plugin.c for details */

#if !defined(__FS_REISER4_PLUGIN_TYPES_H__)
#define __FS_REISER4_PLUGIN_TYPES_H__

#include "../forward.h"
#include "../debug.h"
#include "../dformat.h"
#include "../key.h"
#include "compress/compress.h"
#include "crypto/cipher.h"
#include "plugin_header.h"
#include "item/static_stat.h"
#include "item/internal.h"
#include "item/sde.h"
#include "item/cde.h"
#include "item/item.h"
#include "node/node.h"
#include "node/node40.h"
#include "security/perm.h"
#include "fibration.h"

#include "space/bitmap.h"
#include "space/space_allocator.h"

#include "disk_format/disk_format40.h"
#include "disk_format/disk_format.h"

#include <linux/fs.h>		/* for struct super_block, address_space  */
#include <linux/mm.h>		/* for struct page */
#include <linux/buffer_head.h>	/* for struct buffer_head */
#include <linux/dcache.h>	/* for struct dentry */
#include <linux/types.h>
#include <linux/crypto.h>

typedef struct reiser4_object_on_wire reiser4_object_on_wire;

/*
 * File plugin.  Defines the set of methods that file plugins implement, some
 * of which are optional.
 *
 * A file plugin offers to the caller an interface for IO ( writing to and/or
 * reading from) to what the caller sees as one sequence of bytes.  An IO to it
 * may affect more than one physical sequence of bytes, or no physical sequence
 * of bytes, it may affect sequences of bytes offered by other file plugins to
 * the semantic layer, and the file plugin may invoke other plugins and
 * delegate work to them, but its interface is structured for offering the
 * caller the ability to read and/or write what the caller sees as being a
 * single sequence of bytes.
 *
 * The file plugin must present a sequence of bytes to the caller, but it does
 * not necessarily have to store a sequence of bytes, it does not necessarily
 * have to support efficient tree traversal to any offset in the sequence of
 * bytes (tail and extent items, whose keys contain offsets, do however provide
 * efficient non-sequential lookup of any offset in the sequence of bytes).
 *
 * Directory plugins provide methods for selecting file plugins by resolving a
 * name for them.
 *
 * The functionality other filesystems call an attribute, and rigidly tie
 * together, we decompose into orthogonal selectable features of files.  Using
 * the terminology we will define next, an attribute is a perhaps constrained,
 * perhaps static length, file whose parent has a uni-count-intra-link to it,
 * which might be grandparent-major-packed, and whose parent has a deletion
 * method that deletes it.
 *
 * File plugins can implement constraints.
 *
 * Files can be of variable length (e.g. regular unix files), or of static
 * length (e.g. static sized attributes).
 *
 * An object may have many sequences of bytes, and many file plugins, but, it
 * has exactly one objectid.  It is usually desirable that an object has a
 * deletion method which deletes every item with that objectid.  Items cannot
 * in general be found by just their objectids.  This means that an object must
 * have either a method built into its deletion plugin method for knowing what
 * items need to be deleted, or links stored with the object that provide the
 * plugin with a method for finding those items.  Deleting a file within an
 * object may or may not have the effect of deleting the entire object,
 * depending on the file plugin's deletion method.
 *
 * LINK TAXONOMY:
 *
 * Many objects have a reference count, and when the reference count reaches 0
 * the object's deletion method is invoked.  Some links embody a reference
 * count increase ("countlinks"), and others do not ("nocountlinks").
 *
 * Some links are bi-directional links ("bilinks"), and some are
 * uni-directional("unilinks").
 *
 * Some links are between parts of the same object ("intralinks"), and some are
 * between different objects ("interlinks").
 *
 * PACKING TAXONOMY:
 *
 * Some items of an object are stored with a major packing locality based on
 * their object's objectid (e.g. unix directory items in plan A), and these are
 * called "self-major-packed".
 *
 * Some items of an object are stored with a major packing locality based on
 * their semantic parent object's objectid (e.g. unix file bodies in plan A),
 * and these are called "parent-major-packed".
 *
 * Some items of an object are stored with a major packing locality based on
 * their semantic grandparent, and these are called "grandparent-major-packed".
 * Now carefully notice that we run into trouble with key length if we have to
 * store a 8 byte major+minor grandparent based packing locality, an 8 byte
 * parent objectid, an 8 byte attribute objectid, and an 8 byte offset, all in
 * a 24 byte key.  One of these fields must be sacrificed if an item is to be
 * grandparent-major-packed, and which to sacrifice is left to the item author
 * choosing to make the item grandparent-major-packed.  You cannot make tail
 * items and extent items grandparent-major-packed, though you could make them
 * self-major-packed (usually they are parent-major-packed).
 *
 * In the case of ACLs (which are composed of fixed length ACEs which consist
 * of {subject-type, subject, and permission bitmask} triples), it makes sense
 * to not have an offset field in the ACE item key, and to allow duplicate keys
 * for ACEs.  Thus, the set of ACES for a given file is found by looking for a
 * key consisting of the objectid of the grandparent (thus grouping all ACLs in
 * a directory together), the minor packing locality of ACE, the objectid of
 * the file, and 0.
 *
 * IO involves moving data from one location to another, which means that two
 * locations must be specified, source and destination.
 *
 * This source and destination can be in the filesystem, or they can be a
 * pointer in the user process address space plus a byte count.
 *
 * If both source and destination are in the filesystem, then at least one of
 * them must be representable as a pure stream of bytes (which we call a flow,
 * and define as a struct containing a key, a data pointer, and a length).
 * This may mean converting one of them into a flow.  We provide a generic
 * cast_into_flow() method, which will work for any plugin supporting
 * read_flow(), though it is inefficiently implemented in that it temporarily
 * stores the flow in a buffer (Question: what to do with huge flows that
 * cannot fit into memory?  Answer: we must not convert them all at once. )
 *
 * Performing a write requires resolving the write request into a flow defining
 * the source, and a method that performs the write, and a key that defines
 * where in the tree the write is to go.
 *
 * Performing a read requires resolving the read request into a flow defining
 * the target, and a method that performs the read, and a key that defines
 * where in the tree the read is to come from.
 *
 * There will exist file plugins which have no pluginid stored on the disk for
 * them, and which are only invoked by other plugins.
 */

/* This should be incremented with each new contributed
   pair (plugin type, plugin id).
   NOTE: Make sure there is a release of reiser4progs
   with the corresponding version number */
#define PLUGIN_LIBRARY_VERSION 0

 /* enumeration of fields within plugin_set */
typedef enum {
	PSET_FILE,
	PSET_DIR,		/* PSET_FILE and PSET_DIR should be first
				 * elements: inode.c:read_inode() depends on
				 * this. */
	PSET_PERM,
	PSET_FORMATTING,
	PSET_HASH,
	PSET_FIBRATION,
	PSET_SD,
	PSET_DIR_ITEM,
	PSET_CIPHER,
	PSET_DIGEST,
	PSET_COMPRESSION,
	PSET_COMPRESSION_MODE,
	PSET_CLUSTER,
	PSET_CREATE,
	PSET_LAST
} pset_member;

/* builtin file-plugins */
typedef enum {
	/* regular file */
	UNIX_FILE_PLUGIN_ID,
	/* directory */
	DIRECTORY_FILE_PLUGIN_ID,
	/* symlink */
	SYMLINK_FILE_PLUGIN_ID,
	/* for objects completely handled by the VFS: fifos, devices,
	   sockets  */
	SPECIAL_FILE_PLUGIN_ID,
	/* regular cryptcompress file */
	CRYPTCOMPRESS_FILE_PLUGIN_ID,
	/* number of file plugins. Used as size of arrays to hold
	   file plugins. */
	LAST_FILE_PLUGIN_ID
} reiser4_file_id;

typedef struct file_plugin {

	/* generic fields */
	plugin_header h;

	/* VFS methods */
	struct inode_operations * inode_ops;
	struct file_operations * file_ops;
	struct address_space_operations * as_ops;
	/**
	 * Private methods. These are optional. If used they will allow you
	 * to minimize the amount of code needed to implement a deviation
	 * from some other method that also uses them.
	 */
	/*
	 * private inode_ops
	 */
	int (*setattr)(struct dentry *, struct iattr *);
	/*
	 * private file_ops
	 */
	/* do whatever is necessary to do when object is opened */
	int (*open) (struct inode *inode, struct file *file);
	ssize_t (*read) (struct file *, char __user *buf, size_t read_amount,
			loff_t *off);
	/* write as much as possible bytes from nominated @write_amount
	 * before plugin scheduling is occurred. Save scheduling state
	 * in @cont */
	ssize_t (*write) (struct file *, const char __user *buf,
			  size_t write_amount, loff_t * off,
			  struct dispatch_context * cont);
	int (*ioctl) (struct file *filp, unsigned int cmd, unsigned long arg);
	int (*mmap) (struct file *, struct vm_area_struct *);
	int (*release) (struct inode *, struct file *);
	/*
	 * private a_ops
	 */
	int (*readpage) (struct file *file, struct page *page);
	int (*readpages)(struct file *file, struct address_space *mapping,
			  struct list_head *pages, unsigned nr_pages);
	int (*writepages)(struct address_space *mapping,
			  struct writeback_control *wbc);
	int (*write_begin)(struct file *file, struct page *page,
			   loff_t pos, unsigned len, void **fsdata);
	int (*write_end)(struct file *file, struct page *page,
			 loff_t pos, unsigned copied, void *fsdata);
	sector_t (*bmap) (struct address_space * mapping, sector_t lblock);
	/* other private methods */
	/* save inode cached stat-data onto disk. It was called
	   reiserfs_update_sd() in 3.x */
	int (*write_sd_by_inode) (struct inode *);
	/*
	 * Construct flow into @flow according to user-supplied data.
	 *
	 * This is used by read/write methods to construct a flow to
	 * write/read. ->flow_by_inode() is plugin method, rather than single
	 * global implementation, because key in a flow used by plugin may
	 * depend on data in a @buf.
	 *
	 * NIKITA-FIXME-HANS: please create statistics on what functions are
	 * dereferenced how often for the mongo benchmark.  You can supervise
	 * Elena doing this for you if that helps.  Email me the list of the
	 * top 10, with their counts, and an estimate of the total number of
	 * CPU cycles spent dereferencing as a percentage of CPU cycles spent
	 * processing (non-idle processing).  If the total percent is, say,
	 * less than 1%, it will make our coding discussions much easier, and
	 * keep me from questioning whether functions like the below are too
	 * frequently called to be dereferenced.  If the total percent is more
	 * than 1%, perhaps private methods should be listed in a "required"
	 * comment at the top of each plugin (with stern language about how if
	 * the comment is missing it will not be accepted by the maintainer),
	 * and implemented using macros not dereferenced functions.  How about
	 * replacing this whole private methods part of the struct with a
	 * thorough documentation of what the standard helper functions are for
	 * use in constructing plugins?  I think users have been asking for
	 * that, though not in so many words.
	 */
	int (*flow_by_inode) (struct inode *, const char __user *buf,
			      int user, loff_t size,
			      loff_t off, rw_op op, flow_t *);
	/*
	 * Return the key used to retrieve an offset of a file. It is used by
	 * default implementation of ->flow_by_inode() method
	 * (common_build_flow()) and, among other things, to get to the extent
	 * from jnode of unformatted node.
	 */
	int (*key_by_inode) (struct inode *, loff_t off, reiser4_key *);

	/* NIKITA-FIXME-HANS: this comment is not as clear to others as you
	 * think.... */
	/*
	 * set the plugin for a file.  Called during file creation in creat()
	 * but not reiser4() unless an inode already exists for the file.
	 */
	int (*set_plug_in_inode) (struct inode *inode, struct inode *parent,
				  reiser4_object_create_data *);

	/* NIKITA-FIXME-HANS: comment and name seem to say different things,
	 * are you setting up the object itself also or just adjusting the
	 * parent?.... */
	/* set up plugins for new @object created in @parent. @root is root
	   directory. */
	int (*adjust_to_parent) (struct inode *object, struct inode *parent,
				 struct inode *root);
	/*
	 * this does whatever is necessary to do when object is created. For
	 * instance, for unix files stat data is inserted. It is supposed to be
	 * called by create of struct inode_operations.
	 */
	int (*create_object) (struct inode *object, struct inode *parent,
			      reiser4_object_create_data *);
	/*
	 * this method should check REISER4_NO_SD and set REISER4_NO_SD on
	 * success. Deletion of an object usually includes removal of items
	 * building file body (for directories this is removal of "." and "..")
	 * and removal of stat-data item.
	 */
	int (*delete_object) (struct inode *);

	/* add link from @parent to @object */
	int (*add_link) (struct inode *object, struct inode *parent);

	/* remove link from @parent to @object */
	int (*rem_link) (struct inode *object, struct inode *parent);

	/*
	 * return true if item addressed by @coord belongs to @inode.  This is
	 * used by read/write to properly slice flow into items in presence of
	 * multiple key assignment policies, because items of a file are not
	 * necessarily contiguous in a key space, for example, in a plan-b.
	 */
	int (*owns_item) (const struct inode *, const coord_t *);

	/* checks whether yet another hard links to this object can be
	   added  */
	int (*can_add_link) (const struct inode *);

	/* checks whether hard links to this object can be removed */
	int (*can_rem_link) (const struct inode *);

	/* not empty for DIRECTORY_FILE_PLUGIN_ID only currently. It calls
	   detach of directory plugin to remove ".." */
	int (*detach) (struct inode *child, struct inode *parent);

	/* called when @child was just looked up in the @parent. It is not
	   empty for DIRECTORY_FILE_PLUGIN_ID only where it calls attach of
	   directory plugin */
	int (*bind) (struct inode *child, struct inode *parent);

	/* process safe-link during mount */
	int (*safelink) (struct inode *object, reiser4_safe_link_t link,
			 __u64 value);

	/* The couple of estimate methods for all file operations */
	struct {
		reiser4_block_nr(*create) (const struct inode *);
		reiser4_block_nr(*update) (const struct inode *);
		reiser4_block_nr(*unlink) (const struct inode *,
					   const struct inode *);
	} estimate;

	/*
	 * reiser4 specific part of inode has a union of structures which are
	 * specific to a plugin. This method is called when inode is read
	 * (read_inode) and when file is created (common_create_child) so that
	 * file plugin could initialize its inode data
	 */
	void (*init_inode_data) (struct inode *, reiser4_object_create_data * ,
				 int);

	/*
	 * This method performs progressive deletion of items and whole nodes
	 * from right to left.
	 *
	 * @tap: the point deletion process begins from,
	 * @from_key: the beginning of the deleted key range,
	 * @to_key: the end of the deleted key range,
	 * @smallest_removed: the smallest removed key,
	 *
	 * @return: 0 if success, error code otherwise, -E_REPEAT means that
	 * long cut_tree operation was interrupted for allowing atom commit .
	 */
	int (*cut_tree_worker) (tap_t *, const reiser4_key * from_key,
				const reiser4_key * to_key,
				reiser4_key * smallest_removed, struct inode *,
				int, int *);

	/* called from ->destroy_inode() */
	void (*destroy_inode) (struct inode *);

	/*
	 * methods to serialize object identify. This is used, for example, by
	 * reiser4_{en,de}code_fh().
	 */
	struct {
		/* store object's identity at @area */
		char *(*write) (struct inode *inode, char *area);
		/* parse object from wire to the @obj */
		char *(*read) (char *area, reiser4_object_on_wire * obj);
		/* given object identity in @obj, find or create its dentry */
		struct dentry *(*get) (struct super_block *s,
				       reiser4_object_on_wire * obj);
		/* how many bytes ->wire.write() consumes */
		int (*size) (struct inode *inode);
		/* finish with object identify */
		void (*done) (reiser4_object_on_wire * obj);
	} wire;
} file_plugin;

extern file_plugin file_plugins[LAST_FILE_PLUGIN_ID];

struct reiser4_object_on_wire {
	file_plugin *plugin;
	union {
		struct {
			obj_key_id key_id;
		} std;
		void *generic;
	} u;
};

/* builtin dir-plugins */
typedef enum {
	HASHED_DIR_PLUGIN_ID,
	SEEKABLE_HASHED_DIR_PLUGIN_ID,
	LAST_DIR_ID
} reiser4_dir_id;

typedef struct dir_plugin {
	/* generic fields */
	plugin_header h;

	struct inode_operations * inode_ops;
	struct file_operations * file_ops;
	struct address_space_operations * as_ops;

	/*
	 * private methods: These are optional.  If used they will allow you to
	 * minimize the amount of code needed to implement a deviation from
	 * some other method that uses them.  You could logically argue that
	 * they should be a separate type of plugin.
	 */

	struct dentry *(*get_parent) (struct inode *childdir);

	/*
	 * check whether "name" is acceptable name to be inserted into this
	 * object. Optionally implemented by directory-like objects.  Can check
	 * for maximal length, reserved symbols etc
	 */
	int (*is_name_acceptable) (const struct inode *inode, const char *name,
				   int len);

	void (*build_entry_key) (const struct inode *dir /* directory where
							  * entry is (or will
							  * be) in.*/ ,
				 const struct qstr *name /* name of file
							  * referenced by this
							  * entry */ ,
				 reiser4_key * result	/* resulting key of
							 * directory entry */ );
	int (*build_readdir_key) (struct file *dir, reiser4_key * result);
	int (*add_entry) (struct inode *object, struct dentry *where,
			  reiser4_object_create_data * data,
			  reiser4_dir_entry_desc * entry);
	int (*rem_entry) (struct inode *object, struct dentry *where,
			  reiser4_dir_entry_desc * entry);

	/*
	 * initialize directory structure for newly created object. For normal
	 * unix directories, insert dot and dotdot.
	 */
	int (*init) (struct inode *object, struct inode *parent,
		     reiser4_object_create_data * data);

	/* destroy directory */
	int (*done) (struct inode *child);

	/* called when @subdir was just looked up in the @dir */
	int (*attach) (struct inode *subdir, struct inode *dir);
	int (*detach) (struct inode *subdir, struct inode *dir);

	struct {
		reiser4_block_nr(*add_entry) (const struct inode *);
		reiser4_block_nr(*rem_entry) (const struct inode *);
		reiser4_block_nr(*unlink) (const struct inode *,
					   const struct inode *);
	} estimate;
} dir_plugin;

extern dir_plugin dir_plugins[LAST_DIR_ID];

typedef struct formatting_plugin {
	/* generic fields */
	plugin_header h;
	/* returns non-zero iff file's tail has to be stored
	   in a direct item. */
	int (*have_tail) (const struct inode *inode, loff_t size);
} formatting_plugin;

typedef struct hash_plugin {
	/* generic fields */
	plugin_header h;
	/* computes hash of the given name */
	 __u64(*hash) (const unsigned char *name, int len);
} hash_plugin;

typedef struct cipher_plugin {
	/* generic fields */
	plugin_header h;
	struct crypto_blkcipher * (*alloc) (void);
	void (*free) (struct crypto_blkcipher *tfm);
	/* Offset translator. For each offset this returns (k * offset), where
	   k (k >= 1) is an expansion factor of the cipher algorithm.
	   For all symmetric algorithms k == 1. For asymmetric algorithms (which
	   inflate data) offset translation guarantees that all disk cluster's
	   units will have keys smaller then next cluster's one.
	 */
	 loff_t(*scale) (struct inode *inode, size_t blocksize, loff_t src);
	/* Cipher algorithms can accept data only by chunks of cipher block
	   size. This method is to align any flow up to cipher block size when
	   we pass it to cipher algorithm. To align means to append padding of
	   special format specific to the cipher algorithm */
	int (*align_stream) (__u8 *tail, int clust_size, int blocksize);
	/* low-level key manager (check, install, etc..) */
	int (*setkey) (struct crypto_tfm *tfm, const __u8 *key,
		       unsigned int keylen);
	/* main text processing procedures */
	void (*encrypt) (__u32 *expkey, __u8 *dst, const __u8 *src);
	void (*decrypt) (__u32 *expkey, __u8 *dst, const __u8 *src);
} cipher_plugin;

typedef struct digest_plugin {
	/* generic fields */
	plugin_header h;
	/* fingerprint size in bytes */
	int fipsize;
	struct crypto_hash * (*alloc) (void);
	void (*free) (struct crypto_hash *tfm);
} digest_plugin;

typedef struct compression_plugin {
	/* generic fields */
	plugin_header h;
	int (*init) (void);
	/* the maximum number of bytes the size of the "compressed" data can
	 * exceed the uncompressed data. */
	int (*overrun) (unsigned src_len);
	 coa_t(*alloc) (tfm_action act);
	void (*free) (coa_t coa, tfm_action act);
	/* minimal size of the flow we still try to compress */
	int (*min_size_deflate) (void);
	 __u32(*checksum) (char *data, __u32 length);
	/* main transform procedures */
	void (*compress) (coa_t coa, __u8 *src_first, size_t src_len,
			  __u8 *dst_first, size_t *dst_len);
	void (*decompress) (coa_t coa, __u8 *src_first, size_t src_len,
			    __u8 *dst_first, size_t *dst_len);
} compression_plugin;

typedef struct compression_mode_plugin {
	/* generic fields */
	plugin_header h;
	/* this is called when estimating compressibility
	   of a logical cluster by its content */
	int (*should_deflate) (struct inode *inode, cloff_t index);
	/* this is called when results of compression should be saved */
	int (*accept_hook) (struct inode *inode, cloff_t index);
	/* this is called when results of compression should be discarded */
	int (*discard_hook) (struct inode *inode, cloff_t index);
} compression_mode_plugin;

typedef struct cluster_plugin {
	/* generic fields */
	plugin_header h;
	int shift;
} cluster_plugin;

typedef struct sd_ext_plugin {
	/* generic fields */
	plugin_header h;
	int (*present) (struct inode *inode, char **area, int *len);
	int (*absent) (struct inode *inode);
	int (*save_len) (struct inode *inode);
	int (*save) (struct inode *inode, char **area);
	/* alignment requirement for this stat-data part */
	int alignment;
} sd_ext_plugin;

/* this plugin contains methods to allocate objectid for newly created files,
   to deallocate objectid when file gets removed, to report number of used and
   free objectids */
typedef struct oid_allocator_plugin {
	/* generic fields */
	plugin_header h;
	int (*init_oid_allocator) (reiser4_oid_allocator * map, __u64 nr_files,
				   __u64 oids);
	/* used to report statfs->f_files */
	 __u64(*oids_used) (reiser4_oid_allocator * map);
	/* get next oid to use */
	 __u64(*next_oid) (reiser4_oid_allocator * map);
	/* used to report statfs->f_ffree */
	 __u64(*oids_free) (reiser4_oid_allocator * map);
	/* allocate new objectid */
	int (*allocate_oid) (reiser4_oid_allocator * map, oid_t *);
	/* release objectid */
	int (*release_oid) (reiser4_oid_allocator * map, oid_t);
	/* how many pages to reserve in transaction for allocation of new
	   objectid */
	int (*oid_reserve_allocate) (reiser4_oid_allocator * map);
	/* how many pages to reserve in transaction for freeing of an
	   objectid */
	int (*oid_reserve_release) (reiser4_oid_allocator * map);
	void (*print_info) (const char *, reiser4_oid_allocator *);
} oid_allocator_plugin;

/* disk layout plugin: this specifies super block, journal, bitmap (if there
   are any) locations, etc */
typedef struct disk_format_plugin {
	/* generic fields */
	plugin_header h;
	/* replay journal, initialize super_info_data, etc */
	int (*init_format) (struct super_block *, void *data);

	/* key of root directory stat data */
	const reiser4_key * (*root_dir_key) (const struct super_block *);

	int (*release) (struct super_block *);
	jnode * (*log_super) (struct super_block *);
	int (*check_open) (const struct inode *object);
	int (*version_update) (struct super_block *);
} disk_format_plugin;

struct jnode_plugin {
	/* generic fields */
	plugin_header h;
	int (*init) (jnode * node);
	int (*parse) (jnode * node);
	struct address_space *(*mapping) (const jnode * node);
	unsigned long (*index) (const jnode * node);
	jnode * (*clone) (jnode * node);
};

/* plugin instance.                                                         */
/*                                                                          */
/* This is "wrapper" union for all types of plugins. Most of the code uses  */
/* plugins of particular type (file_plugin, dir_plugin, etc.)  rather than  */
/* operates with pointers to reiser4_plugin. This union is only used in     */
/* some generic code in plugin/plugin.c that operates on all                */
/* plugins. Technically speaking purpose of this union is to add type       */
/* safety to said generic code: each plugin type (file_plugin, for          */
/* example), contains plugin_header as its first memeber. This first member */
/* is located at the same place in memory as .h member of                   */
/* reiser4_plugin. Generic code, obtains pointer to reiser4_plugin and      */
/* looks in the .h which is header of plugin type located in union. This    */
/* allows to avoid type-casts.                                              */
union reiser4_plugin {
	/* generic fields */
	plugin_header h;
	/* file plugin */
	file_plugin file;
	/* directory plugin */
	dir_plugin dir;
	/* hash plugin, used by directory plugin */
	hash_plugin hash;
	/* fibration plugin used by directory plugin */
	fibration_plugin fibration;
	/* cipher transform plugin, used by file plugin */
	cipher_plugin cipher;
	/* digest transform plugin, used by file plugin */
	digest_plugin digest;
	/* compression transform plugin, used by file plugin */
	compression_plugin compression;
	/* tail plugin, used by file plugin */
	formatting_plugin formatting;
	/* permission plugin */
	perm_plugin perm;
	/* node plugin */
	node_plugin node;
	/* item plugin */
	item_plugin item;
	/* stat-data extension plugin */
	sd_ext_plugin sd_ext;
	/* disk layout plugin */
	disk_format_plugin format;
	/* object id allocator plugin */
	oid_allocator_plugin oid_allocator;
	/* plugin for different jnode types */
	jnode_plugin jnode;
	/* compression mode plugin, used by object plugin */
	compression_mode_plugin compression_mode;
	/* cluster plugin, used by object plugin */
	cluster_plugin clust;
	/* place-holder for new plugin types that can be registered
	   dynamically, and used by other dynamically loaded plugins.  */
	void *generic;
};

struct reiser4_plugin_ops {
	/* called when plugin is initialized */
	int (*init) (reiser4_plugin * plugin);
	/* called when plugin is unloaded */
	int (*done) (reiser4_plugin * plugin);
	/* load given plugin from disk */
	int (*load) (struct inode *inode,
		     reiser4_plugin * plugin, char **area, int *len);
	/* how many space is required to store this plugin's state
	   in stat-data */
	int (*save_len) (struct inode *inode, reiser4_plugin * plugin);
	/* save persistent plugin-data to disk */
	int (*save) (struct inode *inode, reiser4_plugin * plugin,
		     char **area);
	/* alignment requirement for on-disk state of this plugin
	   in number of bytes */
	int alignment;
	/* install itself into given inode. This can return error
	   (e.g., you cannot change hash of non-empty directory). */
	int (*change) (struct inode *inode, reiser4_plugin * plugin,
		       pset_member memb);
	/* install itself into given inode. This can return error
	   (e.g., you cannot change hash of non-empty directory). */
	int (*inherit) (struct inode *inode, struct inode *parent,
			reiser4_plugin * plugin);
};

/* functions implemented in fs/reiser4/plugin/plugin.c */

/* stores plugin reference in reiser4-specific part of inode */
extern int set_object_plugin(struct inode *inode, reiser4_plugin_id id);
extern int init_plugins(void);

/* builtin plugins */

/* builtin hash-plugins */

typedef enum {
	RUPASOV_HASH_ID,
	R5_HASH_ID,
	TEA_HASH_ID,
	FNV1_HASH_ID,
	DEGENERATE_HASH_ID,
	LAST_HASH_ID
} reiser4_hash_id;

/* builtin cipher plugins */

typedef enum {
	NONE_CIPHER_ID,
	LAST_CIPHER_ID
} reiser4_cipher_id;

/* builtin digest plugins */

typedef enum {
	SHA256_32_DIGEST_ID,
	LAST_DIGEST_ID
} reiser4_digest_id;

/* builtin compression mode plugins */
typedef enum {
	NONE_COMPRESSION_MODE_ID,
	LATTD_COMPRESSION_MODE_ID,
	ULTIM_COMPRESSION_MODE_ID,
	FORCE_COMPRESSION_MODE_ID,
	CONVX_COMPRESSION_MODE_ID,
	LAST_COMPRESSION_MODE_ID
} reiser4_compression_mode_id;

/* builtin cluster plugins */
typedef enum {
	CLUSTER_64K_ID,
	CLUSTER_32K_ID,
	CLUSTER_16K_ID,
	CLUSTER_8K_ID,
	CLUSTER_4K_ID,
	LAST_CLUSTER_ID
} reiser4_cluster_id;

/* builtin tail-plugins */

typedef enum {
	NEVER_TAILS_FORMATTING_ID,
	ALWAYS_TAILS_FORMATTING_ID,
	SMALL_FILE_FORMATTING_ID,
	LAST_TAIL_FORMATTING_ID
} reiser4_formatting_id;

/* data type used to pack parameters that we pass to vfs object creation
   function create_object() */
struct reiser4_object_create_data {
	/* plugin to control created object */
	reiser4_file_id id;
	/* mode of regular file, directory or special file */
/* what happens if some other sort of perm plugin is in use? */
	umode_t mode;
	/* rdev of special file */
	dev_t rdev;
	/* symlink target */
	const char *name;
	/* add here something for non-standard objects you invent, like
	   query for interpolation file etc. */

	struct reiser4_crypto_info *crypto;

	struct inode *parent;
	struct dentry *dentry;
};

/* description of directory entry being created/destroyed/sought for

   It is passed down to the directory plugin and farther to the
   directory item plugin methods. Creation of new directory is done in
   several stages: first we search for an entry with the same name, then
   create new one. reiser4_dir_entry_desc is used to store some information
   collected at some stage of this process and required later: key of
   item that we want to insert/delete and pointer to an object that will
   be bound by the new directory entry. Probably some more fields will
   be added there.

*/
struct reiser4_dir_entry_desc {
	/* key of directory entry */
	reiser4_key key;
	/* object bound by this entry. */
	struct inode *obj;
};

#define MAX_PLUGIN_TYPE_LABEL_LEN  32
#define MAX_PLUGIN_PLUG_LABEL_LEN  32

#define PLUGIN_BY_ID(TYPE, ID, FIELD)					\
static inline TYPE *TYPE ## _by_id(reiser4_plugin_id id)		\
{									\
	reiser4_plugin *plugin = plugin_by_id(ID, id);			\
	return plugin ? &plugin->FIELD : NULL;				\
}									\
static inline TYPE *TYPE ## _by_disk_id(reiser4_tree * tree, d16 *id)	\
{									\
	reiser4_plugin *plugin = plugin_by_disk_id(tree, ID, id);	\
	return plugin ? &plugin->FIELD : NULL;				\
}									\
static inline TYPE *TYPE ## _by_unsafe_id(reiser4_plugin_id id)	        \
{									\
	reiser4_plugin *plugin = plugin_by_unsafe_id(ID, id);		\
	return plugin ? &plugin->FIELD : NULL;				\
}									\
static inline reiser4_plugin* TYPE ## _to_plugin(TYPE* plugin)	        \
{									\
	return (reiser4_plugin *) plugin;				\
}									\
static inline reiser4_plugin_id TYPE ## _id(TYPE* plugin)		\
{									\
	return TYPE ## _to_plugin(plugin)->h.id;			\
}									\
typedef struct { int foo; } TYPE ## _plugin_dummy

PLUGIN_BY_ID(item_plugin, REISER4_ITEM_PLUGIN_TYPE, item);
PLUGIN_BY_ID(file_plugin, REISER4_FILE_PLUGIN_TYPE, file);
PLUGIN_BY_ID(dir_plugin, REISER4_DIR_PLUGIN_TYPE, dir);
PLUGIN_BY_ID(node_plugin, REISER4_NODE_PLUGIN_TYPE, node);
PLUGIN_BY_ID(sd_ext_plugin, REISER4_SD_EXT_PLUGIN_TYPE, sd_ext);
PLUGIN_BY_ID(perm_plugin, REISER4_PERM_PLUGIN_TYPE, perm);
PLUGIN_BY_ID(hash_plugin, REISER4_HASH_PLUGIN_TYPE, hash);
PLUGIN_BY_ID(fibration_plugin, REISER4_FIBRATION_PLUGIN_TYPE, fibration);
PLUGIN_BY_ID(cipher_plugin, REISER4_CIPHER_PLUGIN_TYPE, cipher);
PLUGIN_BY_ID(digest_plugin, REISER4_DIGEST_PLUGIN_TYPE, digest);
PLUGIN_BY_ID(compression_plugin, REISER4_COMPRESSION_PLUGIN_TYPE, compression);
PLUGIN_BY_ID(formatting_plugin, REISER4_FORMATTING_PLUGIN_TYPE, formatting);
PLUGIN_BY_ID(disk_format_plugin, REISER4_FORMAT_PLUGIN_TYPE, format);
PLUGIN_BY_ID(jnode_plugin, REISER4_JNODE_PLUGIN_TYPE, jnode);
PLUGIN_BY_ID(compression_mode_plugin, REISER4_COMPRESSION_MODE_PLUGIN_TYPE,
	     compression_mode);
PLUGIN_BY_ID(cluster_plugin, REISER4_CLUSTER_PLUGIN_TYPE, clust);

extern int save_plugin_id(reiser4_plugin * plugin, d16 * area);

extern struct list_head *get_plugin_list(reiser4_plugin_type type_id);

#define for_all_plugins(ptype, plugin)							\
for (plugin = list_entry(get_plugin_list(ptype)->next, reiser4_plugin, h.linkage);	\
     get_plugin_list(ptype) != &plugin->h.linkage;					\
     plugin = list_entry(plugin->h.linkage.next, reiser4_plugin, h.linkage))


extern int grab_plugin_pset(struct inode *self, struct inode *ancestor,
			    pset_member memb);
extern int force_plugin_pset(struct inode *self, pset_member memb,
			     reiser4_plugin *plug);
extern int finish_pset(struct inode *inode);

/* defined in fs/reiser4/plugin/object.c */
extern file_plugin file_plugins[LAST_FILE_PLUGIN_ID];
/* defined in fs/reiser4/plugin/object.c */
extern dir_plugin dir_plugins[LAST_DIR_ID];
/* defined in fs/reiser4/plugin/item/static_stat.c */
extern sd_ext_plugin sd_ext_plugins[LAST_SD_EXTENSION];
/* defined in fs/reiser4/plugin/hash.c */
extern hash_plugin hash_plugins[LAST_HASH_ID];
/* defined in fs/reiser4/plugin/fibration.c */
extern fibration_plugin fibration_plugins[LAST_FIBRATION_ID];
/* defined in fs/reiser4/plugin/crypt.c */
extern cipher_plugin cipher_plugins[LAST_CIPHER_ID];
/* defined in fs/reiser4/plugin/digest.c */
extern digest_plugin digest_plugins[LAST_DIGEST_ID];
/* defined in fs/reiser4/plugin/compress/compress.c */
extern compression_plugin compression_plugins[LAST_COMPRESSION_ID];
/* defined in fs/reiser4/plugin/compress/compression_mode.c */
extern compression_mode_plugin
compression_mode_plugins[LAST_COMPRESSION_MODE_ID];
/* defined in fs/reiser4/plugin/cluster.c */
extern cluster_plugin cluster_plugins[LAST_CLUSTER_ID];
/* defined in fs/reiser4/plugin/tail.c */
extern formatting_plugin formatting_plugins[LAST_TAIL_FORMATTING_ID];
/* defined in fs/reiser4/plugin/security/security.c */
extern perm_plugin perm_plugins[LAST_PERM_ID];
/* defined in fs/reiser4/plugin/item/item.c */
extern item_plugin item_plugins[LAST_ITEM_ID];
/* defined in fs/reiser4/plugin/node/node.c */
extern node_plugin node_plugins[LAST_NODE_ID];
/* defined in fs/reiser4/plugin/disk_format/disk_format.c */
extern disk_format_plugin format_plugins[LAST_FORMAT_ID];

/* __FS_REISER4_PLUGIN_TYPES_H__ */
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
