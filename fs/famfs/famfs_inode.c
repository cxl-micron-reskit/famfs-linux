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


MODULE_LICENSE("GPL");
