// SPDX-License-Identifier: GPL-2.0
/*
 * famfs - dax file system for shared fabric-attached memory
 *
 * Copyright 2023-2024 Micron Technology, inc
 *
 * This file system, originally based on ramfs the dax support from xfs,
 * is intended to allow multiple host systems to mount a common file system
 * view of dax files that map to shared memory.
 */

#include <linux/fs.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/dax.h>
#include <linux/hugetlb.h>
#include <linux/iomap.h>
#include <linux/path.h>
#include <linux/namei.h>

#include "famfs_internal.h"

#define FAMFS_DEFAULT_MODE	0755

static const struct inode_operations famfs_file_inode_operations;
static const struct inode_operations famfs_dir_inode_operations;

static struct inode *famfs_get_inode(struct super_block *sb,
				     const struct inode *dir,
				     umode_t mode, dev_t dev)
{
	struct inode *inode = new_inode(sb);
	struct timespec64 tv;

	if (!inode)
		return NULL;

	inode->i_ino = get_next_ino();
	inode_init_owner(&nop_mnt_idmap, inode, dir, mode);
	inode->i_mapping->a_ops = &ram_aops;
	mapping_set_gfp_mask(inode->i_mapping, GFP_HIGHUSER);
	mapping_set_unevictable(inode->i_mapping);
	tv = inode_set_ctime_current(inode);
	inode_set_mtime_to_ts(inode, tv);
	inode_set_atime_to_ts(inode, tv);

	switch (mode & S_IFMT) {
	default:
		init_special_inode(inode, mode, dev);
		break;
	case S_IFREG:
		inode->i_op = &famfs_file_inode_operations;
		inode->i_fop = &famfs_file_operations;
		break;
	case S_IFDIR:
		inode->i_op = &famfs_dir_inode_operations;
		inode->i_fop = &simple_dir_operations;

		/* Directory inodes start off with i_nlink == 2 (for ".") */
		inc_nlink(inode);
		break;
	case S_IFLNK:
		inode->i_op = &page_symlink_inode_operations;
		inode_nohighmem(inode);
		break;
	}
	return inode;
}

/***************************************************************************
 * famfs inode_operations: these are currently pretty much boilerplate
 */

static const struct inode_operations famfs_file_inode_operations = {
	/* All generic */
	.setattr	   = simple_setattr,
	.getattr	   = simple_getattr,
};

/*
 * File creation. Allocate an inode, and we're done..
 */
static int
famfs_mknod(struct mnt_idmap *idmap, struct inode *dir, struct dentry *dentry,
	    umode_t mode, dev_t dev)
{
	struct famfs_fs_info *fsi = dir->i_sb->s_fs_info;
	struct timespec64 tv;
	struct inode *inode;

	if (fsi->deverror)
		return -ENODEV;

	inode = famfs_get_inode(dir->i_sb, dir, mode, dev);
	if (!inode)
		return -ENOSPC;

	d_instantiate(dentry, inode);
	dget(dentry);	/* Extra count - pin the dentry in core */
	tv = inode_set_ctime_current(inode);
	inode_set_mtime_to_ts(inode, tv);
	inode_set_atime_to_ts(inode, tv);

	return 0;
}

static int famfs_mkdir(struct mnt_idmap *idmap, struct inode *dir,
		       struct dentry *dentry, umode_t mode)
{
	struct famfs_fs_info *fsi = dir->i_sb->s_fs_info;
	int rc;

	if (fsi->deverror)
		return -ENODEV;

	rc = famfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFDIR, 0);
	if (rc)
		return rc;

	inc_nlink(dir);

	return 0;
}

static int famfs_create(struct mnt_idmap *idmap, struct inode *dir,
			struct dentry *dentry, umode_t mode, bool excl)
{
	struct famfs_fs_info *fsi = dir->i_sb->s_fs_info;

	if (fsi->deverror)
		return -ENODEV;

	return famfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFREG, 0);
}

static const struct inode_operations famfs_dir_inode_operations = {
	.create		= famfs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink		= simple_unlink,
	.mkdir		= famfs_mkdir,
	.rmdir		= simple_rmdir,
	.rename		= simple_rename,
};

/*****************************************************************************
 * famfs super_operations
 *
 * TODO: implement a famfs_statfs() that shows size, free and available space,
 * etc.
 */

/*
 * famfs_show_options() - Display the mount options in /proc/mounts.
 */
static int famfs_show_options(struct seq_file *m, struct dentry *root)
{
	struct famfs_fs_info *fsi = root->d_sb->s_fs_info;

	if (fsi->mount_opts.mode != FAMFS_DEFAULT_MODE)
		seq_printf(m, ",mode=%o", fsi->mount_opts.mode);

	return 0;
}

static const struct super_operations famfs_super_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= generic_delete_inode,
	.show_options	= famfs_show_options,
};

/*****************************************************************************/

/*
 * famfs dax_operations  (for char dax)
 */
static int
famfs_dax_notify_failure(struct dax_device *dax_dev, u64 offset,
			u64 len, int mf_flags)
{
	struct super_block *sb = dax_holder(dax_dev);
	struct famfs_fs_info *fsi = sb->s_fs_info;

	pr_err("%s: rootdev=%s offset=%lld len=%llu flags=%x\n", __func__,
	       fsi->rootdev, offset, len, mf_flags);

	return 0;
}

static const struct dax_holder_operations famfs_dax_holder_ops = {
	.notify_failure		= famfs_dax_notify_failure,
};

/*****************************************************************************
 * fs_context_operations
 */

static int
famfs_fill_super(struct super_block *sb, struct fs_context *fc)
{
	int rc = 0;

	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_blocksize		= PAGE_SIZE;
	sb->s_blocksize_bits	= PAGE_SHIFT;
	sb->s_magic		= FAMFS_SUPER_MAGIC;
	sb->s_op		= &famfs_super_ops;
	sb->s_time_gran		= 1;

	return rc;
}

static int
lookup_daxdev(const char *pathname, dev_t *devno)
{
	struct inode *inode;
	struct path path;
	int err;

	if (!pathname || !*pathname)
		return -EINVAL;

	err = kern_path(pathname, LOOKUP_FOLLOW, &path);
	if (err)
		return err;

	inode = d_backing_inode(path.dentry);
	if (!S_ISCHR(inode->i_mode)) {
		err = -EINVAL;
		goto out_path_put;
	}

	if (!may_open_dev(&path)) { /* had to export this */
		err = -EACCES;
		goto out_path_put;
	}

	 /* if it's dax, i_rdev is struct dax_device */
	*devno = inode->i_rdev;

out_path_put:
	path_put(&path);
	return err;
}

static int
famfs_get_tree(struct fs_context *fc)
{
	struct famfs_fs_info *fsi = fc->s_fs_info;
	struct dax_device *dax_devp;
	struct super_block *sb;
	struct inode *inode;
	dev_t daxdevno;
	int err;

	/* TODO: clean up chatty messages */

	err = lookup_daxdev(fc->source, &daxdevno);
	if (err)
		return err;

	fsi->daxdevno = daxdevno;

	/* This will set sb->s_dev=daxdevno */
	sb = sget_dev(fc, daxdevno);
	if (IS_ERR(sb)) {
		pr_err("%s: sget_dev error\n", __func__);
		return PTR_ERR(sb);
	}

	if (sb->s_root) {
		pr_info("%s: found a matching suerblock for %s\n",
			__func__, fc->source);

		/* We don't expect to find a match by dev_t; if we do, it must
		 * already be mounted, so we bail
		 */
		err = -EBUSY;
		goto deactivate_out;
	} else {
		pr_info("%s: initializing new superblock for %s\n",
			__func__, fc->source);
		err = famfs_fill_super(sb, fc);
		if (err)
			goto deactivate_out;
	}

	/* This will fail if it's not a dax device */
	dax_devp = dax_dev_get(daxdevno);
	if (!dax_devp) {
		pr_warn("%s: device %s not found or not dax\n",
		       __func__, fc->source);
		err = -ENODEV;
		goto deactivate_out;
	}

	err = fs_dax_get(dax_devp, sb, &famfs_dax_holder_ops);
	if (err) {
		pr_err("%s: fs_dax_get(%lld) failed\n", __func__, (u64)daxdevno);
		err = -EBUSY;
		goto deactivate_out;
	}
	fsi->dax_devp = dax_devp;

	inode = famfs_get_inode(sb, NULL, S_IFDIR | fsi->mount_opts.mode, 0);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root) {
		pr_err("%s: d_make_root() failed\n", __func__);
		err = -ENOMEM;
		fs_put_dax(fsi->dax_devp, sb);
		goto deactivate_out;
	}

	sb->s_flags |= SB_ACTIVE;

	WARN_ON(fc->root);
	fc->root = dget(sb->s_root);
	return err;

deactivate_out:
	pr_debug("%s: deactivating sb=%llx\n", __func__, (u64)sb);
	deactivate_locked_super(sb);
	return err;
}

/*****************************************************************************/

enum famfs_param {
	Opt_mode,
	Opt_dax,
};

const struct fs_parameter_spec famfs_fs_parameters[] = {
	fsparam_u32oct("mode",	  Opt_mode),
	fsparam_string("dax",     Opt_dax),
	{}
};

static int famfs_parse_param(struct fs_context *fc, struct fs_parameter *param)
{
	struct famfs_fs_info *fsi = fc->s_fs_info;
	struct fs_parse_result result;
	int opt;

	opt = fs_parse(fc, famfs_fs_parameters, param, &result);
	if (opt == -ENOPARAM) {
		opt = vfs_parse_fs_param_source(fc, param);
		if (opt != -ENOPARAM)
			return opt;

		return 0;
	}
	if (opt < 0)
		return opt;

	switch (opt) {
	case Opt_mode:
		fsi->mount_opts.mode = result.uint_32 & S_IALLUGO;
		break;
	case Opt_dax:
		if (strcmp(param->string, "always"))
			pr_notice("%s: invalid dax mode %s\n",
				  __func__, param->string);
		break;
	}

	return 0;
}

static void famfs_free_fc(struct fs_context *fc)
{
	struct famfs_fs_info *fsi = fc->s_fs_info;

	if (fsi && fsi->rootdev)
		kfree(fsi->rootdev);

	kfree(fsi);
}

static const struct fs_context_operations famfs_context_ops = {
	.free		= famfs_free_fc,
	.parse_param	= famfs_parse_param,
	.get_tree	= famfs_get_tree,
};

static int famfs_init_fs_context(struct fs_context *fc)
{
	struct famfs_fs_info *fsi;

	fsi = kzalloc(sizeof(*fsi), GFP_KERNEL);
	if (!fsi)
		return -ENOMEM;

	fsi->mount_opts.mode = FAMFS_DEFAULT_MODE;
	fc->s_fs_info        = fsi;
	fc->ops              = &famfs_context_ops;
	return 0;
}

static void famfs_kill_sb(struct super_block *sb)
{
	struct famfs_fs_info *fsi = sb->s_fs_info;

	if (fsi->dax_devp)
		fs_put_dax(fsi->dax_devp, sb);
	if (fsi && fsi->rootdev)
		kfree(fsi->rootdev);
	kfree(fsi);
	sb->s_fs_info = NULL;

	kill_char_super(sb); /* new */
}

#define MODULE_NAME "famfs"
static struct file_system_type famfs_fs_type = {
	.name		  = MODULE_NAME,
	.init_fs_context  = famfs_init_fs_context,
	.parameters	  = famfs_fs_parameters,
	.kill_sb	  = famfs_kill_sb,
	.fs_flags	  = FS_USERNS_MOUNT,
};

/******************************************************************************
 * Module stuff
 */
static int __init init_famfs_fs(void)
{
	int rc;

	rc = register_filesystem(&famfs_fs_type);

	return rc;
}

static void
__exit famfs_exit(void)
{
	unregister_filesystem(&famfs_fs_type);
	pr_info("%s: unregistered\n", __func__);
}

fs_initcall(init_famfs_fs);
module_exit(famfs_exit);

MODULE_AUTHOR("John Groves, Micron Technology");
MODULE_LICENSE("GPL");
