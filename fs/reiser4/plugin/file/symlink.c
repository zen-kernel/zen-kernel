/* Copyright 2002, 2003, 2005 by Hans Reiser, licensing governed by reiser4/README */

#include "../../inode.h"

#include <linux/types.h>
#include <linux/fs.h>

/* file plugin methods specific for symlink files
   (SYMLINK_FILE_PLUGIN_ID) */

/* this is implementation of create_object method of file plugin for
   SYMLINK_FILE_PLUGIN_ID
 */

/**
 * reiser4_create_symlink - create_object of file plugin for SYMLINK_FILE_PLUGIN_ID
 * @symlink: inode of symlink object
 * @dir: inode of parent directory
 * @info:  parameters of new object
 *
 * Inserts stat data with symlink extension where into the tree.
 */
int reiser4_create_symlink(struct inode *symlink,
			   struct inode *dir UNUSED_ARG,
			   reiser4_object_create_data *data /* info passed to us
							     * this is filled by
							     * reiser4() syscall
							     * in particular */)
{
	int result;

	assert("nikita-680", symlink != NULL);
	assert("nikita-681", S_ISLNK(symlink->i_mode));
	assert("nikita-685", reiser4_inode_get_flag(symlink, REISER4_NO_SD));
	assert("nikita-682", dir != NULL);
	assert("nikita-684", data != NULL);
	assert("nikita-686", data->id == SYMLINK_FILE_PLUGIN_ID);

	/*
	 * stat data of symlink has symlink extension in which we store
	 * symlink content, that is, path symlink is pointing to.
	 */
	reiser4_inode_data(symlink)->extmask |= (1 << SYMLINK_STAT);

	assert("vs-838", symlink->i_private == NULL);
	symlink->i_private = (void *)data->name;

	assert("vs-843", symlink->i_size == 0);
	INODE_SET_FIELD(symlink, i_size, strlen(data->name));

	/* insert stat data appended with data->name */
	result = inode_file_plugin(symlink)->write_sd_by_inode(symlink);
	if (result) {
		/* FIXME-VS: Make sure that symlink->i_private is not attached
		   to kmalloced data */
		INODE_SET_FIELD(symlink, i_size, 0);
	} else {
		assert("vs-849", symlink->i_private
		       && reiser4_inode_get_flag(symlink,
						 REISER4_GENERIC_PTR_USED));
		assert("vs-850",
		       !memcmp((char *)symlink->i_private, data->name,
			       (size_t) symlink->i_size + 1));
	}
	return result;
}

/* this is implementation of destroy_inode method of file plugin for
   SYMLINK_FILE_PLUGIN_ID
 */
void destroy_inode_symlink(struct inode *inode)
{
	assert("edward-799",
	       inode_file_plugin(inode) ==
	       file_plugin_by_id(SYMLINK_FILE_PLUGIN_ID));
	assert("edward-800", !is_bad_inode(inode) && is_inode_loaded(inode));
	assert("edward-801", reiser4_inode_get_flag(inode,
						    REISER4_GENERIC_PTR_USED));
	assert("vs-839", S_ISLNK(inode->i_mode));

	kfree(inode->i_private);
	inode->i_private = NULL;
	reiser4_inode_clr_flag(inode, REISER4_GENERIC_PTR_USED);
}

/*
  Local variables:
  c-indentation-style: "K&R"
  mode-name: "LC"
  c-basic-offset: 8
  tab-width: 8
  fill-column: 80
  scroll-step: 1
  End:
*/
