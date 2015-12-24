/* Copyright 2002, 2003 by Hans Reiser, licensing governed by reiser4/README */

/* plugin header. Data structures required by all plugin types. */

#if !defined(__PLUGIN_HEADER_H__)
#define __PLUGIN_HEADER_H__

/* plugin data-types and constants */

#include "../debug.h"
#include "../dformat.h"

/* The list of Reiser4 interfaces */
typedef enum {
	REISER4_FILE_PLUGIN_TYPE,             /* manage VFS objects */
	REISER4_DIR_PLUGIN_TYPE,              /* manage directories */
	REISER4_ITEM_PLUGIN_TYPE,             /* manage items */
	REISER4_NODE_PLUGIN_TYPE,             /* manage formatted nodes */
	REISER4_HASH_PLUGIN_TYPE,             /* hash methods */
	REISER4_FIBRATION_PLUGIN_TYPE,        /* directory fibrations */
	REISER4_FORMATTING_PLUGIN_TYPE,       /* dispatching policy */
	REISER4_PERM_PLUGIN_TYPE,             /* stub (vacancy) */
	REISER4_SD_EXT_PLUGIN_TYPE,           /* manage stat-data extensions */
	REISER4_FORMAT_PLUGIN_TYPE,           /* disk format specifications */
	REISER4_JNODE_PLUGIN_TYPE,            /* manage in-memory headers */
	REISER4_CIPHER_PLUGIN_TYPE,           /* cipher transform methods */
	REISER4_DIGEST_PLUGIN_TYPE,           /* digest transform methods */
	REISER4_COMPRESSION_PLUGIN_TYPE,      /* compression methods */
	REISER4_COMPRESSION_MODE_PLUGIN_TYPE, /* dispatching policies */
	REISER4_CLUSTER_PLUGIN_TYPE,          /* manage logical clusters */
	REISER4_TXMOD_PLUGIN_TYPE,            /* transaction models */
	REISER4_PLUGIN_TYPES
} reiser4_plugin_type;

/* Supported plugin groups */
typedef enum {
	REISER4_DIRECTORY_FILE,
	REISER4_REGULAR_FILE,
	REISER4_SYMLINK_FILE,
	REISER4_SPECIAL_FILE,
} file_plugin_group;

struct reiser4_plugin_ops;
/* generic plugin operations, supported by each
    plugin type. */
typedef struct reiser4_plugin_ops reiser4_plugin_ops;

/* the common part of all plugin instances. */
typedef struct plugin_header {
	/* plugin type */
	reiser4_plugin_type type_id;
	/* id of this plugin */
	reiser4_plugin_id id;
	/* bitmask of groups the plugin belongs to. */
	reiser4_plugin_groups groups;
	/* plugin operations */
	reiser4_plugin_ops *pops;
/* NIKITA-FIXME-HANS: usage of and access to label and desc is not commented and
 * defined. */
	/* short label of this plugin */
	const char *label;
	/* descriptive string.. */
	const char *desc;
	/* list linkage */
	struct list_head linkage;
} plugin_header;

#define plugin_of_group(plug, group) (plug->h.groups & (1 << group))

/* PRIVATE INTERFACES */
/* NIKITA-FIXME-HANS: what is this for and why does it duplicate what is in
 * plugin_header? */
/* plugin type representation. */
struct reiser4_plugin_type_data {
	/* internal plugin type identifier. Should coincide with
	   index of this item in plugins[] array. */
	reiser4_plugin_type type_id;
	/* short symbolic label of this plugin type. Should be no longer
	   than MAX_PLUGIN_TYPE_LABEL_LEN characters including '\0'. */
	const char *label;
	/* plugin type description longer than .label */
	const char *desc;

/* NIKITA-FIXME-HANS: define built-in */
	/* number of built-in plugin instances of this type */
	int builtin_num;
	/* array of built-in plugins */
	void *builtin;
	struct list_head plugins_list;
	size_t size;
};

extern struct reiser4_plugin_type_data plugins[REISER4_PLUGIN_TYPES];

int is_plugin_type_valid(reiser4_plugin_type type);
int is_plugin_id_valid(reiser4_plugin_type type, reiser4_plugin_id id);

static inline reiser4_plugin *plugin_at(struct reiser4_plugin_type_data *ptype,
					int i)
{
	char *builtin;

	builtin = ptype->builtin;
	return (reiser4_plugin *) (builtin + i * ptype->size);
}

/* return plugin by its @type_id and @id */
static inline reiser4_plugin *plugin_by_id(reiser4_plugin_type type,
					   reiser4_plugin_id id)
{
	assert("nikita-1651", is_plugin_type_valid(type));
	assert("nikita-1652", is_plugin_id_valid(type, id));
	return plugin_at(&plugins[type], id);
}

extern reiser4_plugin *plugin_by_unsafe_id(reiser4_plugin_type type_id,
					   reiser4_plugin_id id);

/**
 * plugin_by_disk_id - get reiser4_plugin
 * @type_id: plugin type id
 * @did: plugin id in disk format
 *
 * Returns reiser4_plugin by plugin type id an dplugin_id.
 */
static inline reiser4_plugin *plugin_by_disk_id(reiser4_tree * tree UNUSED_ARG,
						reiser4_plugin_type type_id,
						__le16 *plugin_id)
{
	/*
	 * what we should do properly is to maintain within each file-system a
	 * dictionary that maps on-disk plugin ids to "universal" ids. This
	 * dictionary will be resolved on mount time, so that this function
	 * will perform just one additional array lookup.
	 */
	return plugin_by_unsafe_id(type_id, le16_to_cpu(*plugin_id));
}

/* __PLUGIN_HEADER_H__ */
#endif

/*
 * Local variables:
 * c-indentation-style: "K&R"
 * mode-name: "LC"
 * c-basic-offset: 8
 * tab-width: 8
 * fill-column: 79
 * End:
 */
