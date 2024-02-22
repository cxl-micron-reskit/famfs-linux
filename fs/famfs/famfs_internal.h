/* SPDX-License-Identifier: GPL-2.0 */
/*
 * famfs - dax file system for shared fabric-attached memory
 *
 * Copyright 2023-2024 Micron Technology, Inc.
 *
 * This file system, originally based on ramfs the dax support from xfs,
 * is intended to allow multiple host systems to mount a common file system
 * view of dax files that map to shared memory.
 */
#ifndef FAMFS_INTERNAL_H
#define FAMFS_INTERNAL_H

#include <linux/atomic.h>
#include <linux/famfs_ioctl.h>

#define FAMFS_MAGIC 0x87b282ff

#define FAMFS_BLKDEV_MODE (FMODE_READ|FMODE_WRITE)

extern const struct file_operations      famfs_file_operations;

/*
 * Each famfs dax file has this hanging from its inode->i_private.
 */
struct famfs_file_meta {
	int                   error;
	enum famfs_file_type  file_type;
	size_t                file_size;
	enum extent_type      tfs_extent_type;
	size_t                tfs_extent_ct;
	struct famfs_extent   tfs_extents[];  /* flexible array */
};

struct famfs_mount_opts {
	umode_t mode;
};

extern const struct iomap_ops             famfs_iomap_ops;
extern const struct vm_operations_struct  famfs_file_vm_ops;

#define ROOTDEV_STRLEN 80

struct famfs_fs_info {
	struct mutex             fsi_mutex;
	struct famfs_mount_opts  mount_opts;
	int                      num_dax_devs;
	struct file             *dax_filp;
	struct dax_device       *dax_devp;
	struct bdev_handle      *bdev_handle;
	struct list_head         fsi_list;
	char                    *rootdev;
};

#endif /* FAMFS_INTERNAL_H */
