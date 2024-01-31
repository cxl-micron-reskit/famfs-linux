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



MODULE_LICENSE("GPL");
