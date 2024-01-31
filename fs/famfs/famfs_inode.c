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
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/time.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/backing-dev.h>
#include <linux/sched.h>
#include <linux/parser.h>
#include <linux/magic.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/fs_context.h>
#include <linux/fs_parser.h>
#include <linux/seq_file.h>
#include <linux/dax.h>
#include <linux/hugetlb.h>
#include <linux/uio.h>
#include <linux/iomap.h>
#include <linux/path.h>
#include <linux/namei.h>
#include <linux/pfn_t.h>
#include <linux/blkdev.h>

#include "famfs_internal.h"

#define FAMFS_DEFAULT_MODE	0755

static const struct super_operations famfs_ops;
static const struct inode_operations famfs_file_inode_operations;
static const struct inode_operations famfs_dir_inode_operations;

static struct inode *famfs_get_inode(
	struct super_block *sb,
	const struct inode *dir,
	umode_t             mode,
	dev_t               dev)
{
	struct inode *inode = new_inode(sb);

	if (inode) {
		struct timespec64       tv;

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

			/* Directory inodes start off with i_nlink == 2 (for "." entry) */
			inc_nlink(inode);
			break;
		case S_IFLNK:
			inode->i_op = &page_symlink_inode_operations;
			inode_nohighmem(inode);
			break;
		}
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
/* SMP-safe */
static int
famfs_mknod(
	struct mnt_idmap *idmap,
	struct inode     *dir,
	struct dentry    *dentry,
	umode_t           mode,
	dev_t             dev)
{
	struct inode *inode = famfs_get_inode(dir->i_sb, dir, mode, dev);
	int error           = -ENOSPC;

	if (inode) {
		struct timespec64       tv;

		d_instantiate(dentry, inode);
		dget(dentry);	/* Extra count - pin the dentry in core */
		error = 0;
		tv = inode_set_ctime_current(inode);
		inode_set_mtime_to_ts(inode, tv);
		inode_set_atime_to_ts(inode, tv);
	}
	return error;
}

static int famfs_mkdir(
	struct mnt_idmap *idmap,
	struct inode     *dir,
	struct dentry    *dentry,
	umode_t           mode)
{
	int retval = famfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFDIR, 0);

	if (!retval)
		inc_nlink(dir);

	return retval;
}

static int famfs_create(
	struct mnt_idmap *idmap,
	struct inode     *dir,
	struct dentry    *dentry,
	umode_t           mode,
	bool              excl)
{
	return famfs_mknod(&nop_mnt_idmap, dir, dentry, mode | S_IFREG, 0);
}

static int famfs_symlink(
	struct mnt_idmap *idmap,
	struct inode     *dir,
	struct dentry    *dentry,
	const char       *symname)
{
	struct inode *inode;
	int error = -ENOSPC;

	inode = famfs_get_inode(dir->i_sb, dir, S_IFLNK | 0777, 0);
	if (inode) {
		int l = strlen(symname)+1;

		error = page_symlink(inode, symname, l);
		if (!error) {
			struct timespec64       tv;

			d_instantiate(dentry, inode);
			dget(dentry);
			tv = inode_set_ctime_current(inode);
			inode_set_mtime_to_ts(inode, tv);
			inode_set_atime_to_ts(inode, tv);
		} else
			iput(inode);
	}
	return error;
}

static const struct inode_operations famfs_dir_inode_operations = {
	.create		= famfs_create,
	.lookup		= simple_lookup,
	.link		= simple_link,
	.unlink		= simple_unlink,
	.symlink	= famfs_symlink,
	.mkdir		= famfs_mkdir,
	.rmdir		= simple_rmdir,
	.mknod		= famfs_mknod,
	.rename		= simple_rename,
};

/**********************************************************************************
 * famfs super_operations
 *
 * TODO: implement a famfs_statfs() that shows size, free and available space, etc.
 */

/**
 * famfs_show_options() - Display the mount options in /proc/mounts.
 */
static int famfs_show_options(
	struct seq_file *m,
	struct dentry   *root)
{
	struct famfs_fs_info *fsi = root->d_sb->s_fs_info;

	if (fsi->mount_opts.mode != FAMFS_DEFAULT_MODE)
		seq_printf(m, ",mode=%o", fsi->mount_opts.mode);

	return 0;
}

static const struct super_operations famfs_ops = {
	.statfs		= simple_statfs,
	.drop_inode	= generic_delete_inode,
	.show_options	= famfs_show_options,
};

/***************************************************************************************
 * dax_holder_operations for block dax
 */

static int
famfs_blk_dax_notify_failure(
	struct dax_device	*dax_devp,
	u64			offset,
	u64			len,
	int			mf_flags)
{

	pr_err("%s: dax_devp %llx offset %llx len %lld mf_flags %x\n",
	       __func__, (u64)dax_devp, (u64)offset, (u64)len, mf_flags);
	return -EOPNOTSUPP;
}

const struct dax_holder_operations famfs_blk_dax_holder_ops = {
	.notify_failure		= famfs_blk_dax_notify_failure,
};

static int
famfs_open_char_device(
	struct super_block *sb,
	struct fs_context  *fc)
{
	pr_err("%s: Root device is %s, but your kernel does not support famfs on /dev/dax\n",
	       __func__, fc->source);
	return -ENODEV;
}

/**
 * famfs_open_device()
 *
 * Open the memory device. If it looks like /dev/dax, call famfs_open_char_device().
 * Otherwise try to open it as a block/pmem device.
 */
static int
famfs_open_device(
	struct super_block *sb,
	struct fs_context  *fc)
{
	struct famfs_fs_info *fsi = sb->s_fs_info;
	struct dax_device    *dax_devp;
	u64 start_off = 0;
	struct bdev_handle   *handlep;

	if (fsi->dax_devp) {
		pr_err("%s: already mounted\n", __func__);
		return -EALREADY;
	}

	if (strstr(fc->source, "/dev/dax")) /* There is probably a better way to check this */
		return famfs_open_char_device(sb, fc);

	if (!strstr(fc->source, "/dev/pmem")) { /* There is probably a better way to check this */
		pr_err("%s: primary backing dev (%s) is not pmem\n",
		       __func__, fc->source);
		return -EINVAL;
	}

	handlep = bdev_open_by_path(fc->source, FAMFS_BLKDEV_MODE, fsi, &fs_holder_ops);
	if (IS_ERR(handlep->bdev)) {
		pr_err("%s: failed blkdev_get_by_path(%s)\n", __func__, fc->source);
		return PTR_ERR(handlep->bdev);
	}

	dax_devp = fs_dax_get_by_bdev(handlep->bdev, &start_off,
				      fsi  /* holder */,
				      &famfs_blk_dax_holder_ops);
	if (IS_ERR(dax_devp)) {
		pr_err("%s: unable to get daxdev from handlep->bdev\n", __func__);
		bdev_release(handlep);
		return -ENODEV;
	}
	fsi->bdev_handle = handlep;
	fsi->dax_devp    = dax_devp;

	pr_notice("%s: root device is block dax (%s)\n", __func__, fc->source);
	return 0;
}

/*****************************************************************************************
 * fs_context_operations
 */
static int
famfs_fill_super(
	struct super_block *sb,
	struct fs_context  *fc)
{
	struct famfs_fs_info *fsi = sb->s_fs_info;
	struct inode *inode;
	int rc = 0;

	sb->s_maxbytes		= MAX_LFS_FILESIZE;
	sb->s_blocksize		= PAGE_SIZE;
	sb->s_blocksize_bits	= PAGE_SHIFT;
	sb->s_magic		= FAMFS_MAGIC;
	sb->s_op		= &famfs_ops;
	sb->s_time_gran		= 1;

	rc = famfs_open_device(sb, fc);
	if (rc)
		goto out;

	inode = famfs_get_inode(sb, NULL, S_IFDIR | fsi->mount_opts.mode, 0);
	sb->s_root = d_make_root(inode);
	if (!sb->s_root)
		rc = -ENOMEM;

out:
	return rc;
}

enum famfs_param {
	Opt_mode,
	Opt_dax,
};

const struct fs_parameter_spec famfs_fs_parameters[] = {
	fsparam_u32oct("mode",	  Opt_mode),
	fsparam_string("dax",     Opt_dax),
	{}
};

static int famfs_parse_param(
	struct fs_context   *fc,
	struct fs_parameter *param)
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

static DEFINE_MUTEX(famfs_context_mutex);
static LIST_HEAD(famfs_context_list);

static int famfs_get_tree(struct fs_context *fc)
{
	struct famfs_fs_info *fsi_entry;
	struct famfs_fs_info *fsi = fc->s_fs_info;

	fsi->rootdev = kstrdup(fc->source, GFP_KERNEL);
	if (!fsi->rootdev)
		return -ENOMEM;

	/* Fail if famfs is already mounted from the same device */
	mutex_lock(&famfs_context_mutex);
	list_for_each_entry(fsi_entry, &famfs_context_list, fsi_list) {
		if (strcmp(fsi_entry->rootdev, fc->source) == 0) {
			mutex_unlock(&famfs_context_mutex);
			pr_err("%s: already mounted from rootdev %s\n", __func__, fc->source);
			return -EALREADY;
		}
	}

	list_add(&fsi->fsi_list, &famfs_context_list);
	mutex_unlock(&famfs_context_mutex);

	return get_tree_nodev(fc, famfs_fill_super);

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

	mutex_init(&fsi->fsi_mutex);
	fsi->mount_opts.mode = FAMFS_DEFAULT_MODE;
	fc->s_fs_info        = fsi;
	fc->ops              = &famfs_context_ops;
	return 0;
}

static void famfs_kill_sb(struct super_block *sb)
{
	struct famfs_fs_info *fsi = sb->s_fs_info;

	mutex_lock(&famfs_context_mutex);
	list_del(&fsi->fsi_list);
	mutex_unlock(&famfs_context_mutex);

	mutex_destroy(&fsi->fsi_mutex);
	if (fsi->bdev_handle)
		bdev_release(fsi->bdev_handle);
	if (fsi->dax_devp)
		fs_put_dax(fsi->dax_devp, fsi);
	if (fsi->dax_filp) /* This only happens if it's char dax */
		filp_close(fsi->dax_filp, NULL);

	if (fsi && fsi->rootdev)
		kfree(fsi->rootdev);
	kfree(fsi);
	kill_litter_super(sb);
}

#define MODULE_NAME "famfs"
static struct file_system_type famfs_fs_type = {
	.name		  = MODULE_NAME,
	.init_fs_context  = famfs_init_fs_context,
	.parameters	  = famfs_fs_parameters,
	.kill_sb	  = famfs_kill_sb,
	.fs_flags	  = FS_USERNS_MOUNT,
};

MODULE_LICENSE("GPL");
