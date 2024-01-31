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

/*
 * filemap_fault counters
 */
extern struct attribute_group famfs_attr_group;

enum famfs_fault {
	FAMFS_PTE = 0,
	FAMFS_PMD,
	FAMFS_PUD,
	FAMFS_NUM_FAULT_TYPES,
};

static inline int valid_fault_type(int type)
{
	if (unlikely(type < 0 || type > FAMFS_PUD))
		return 0;
	return 1;
}

struct famfs_fault_counters {
	atomic64_t fault_ct[FAMFS_NUM_FAULT_TYPES];
};

extern struct famfs_fault_counters ffc;

static inline void famfs_clear_fault_counters(struct famfs_fault_counters *fc)
{
	int i;

	for (i = 0; i < FAMFS_NUM_FAULT_TYPES; i++)
		atomic64_set(&fc->fault_ct[i], 0);
}

static inline void famfs_inc_fault_counter(struct famfs_fault_counters *fc,
					   enum famfs_fault type)
{
	if (valid_fault_type(type))
		atomic64_inc(&fc->fault_ct[type]);
}

static inline void famfs_inc_fault_counter_by_order(struct famfs_fault_counters *fc, int order)
{
	int pgf = -1;

	switch (order) {
	case 0:
		pgf = FAMFS_PTE;
		break;
	case PMD_ORDER:
		pgf = FAMFS_PMD;
		break;
	case PUD_ORDER:
		pgf = FAMFS_PUD;
		break;
	}
	famfs_inc_fault_counter(fc, pgf);
}

static inline u64 famfs_pte_fault_ct(struct famfs_fault_counters *fc)
{
	return atomic64_read(&fc->fault_ct[FAMFS_PTE]);
}

static inline u64 famfs_pmd_fault_ct(struct famfs_fault_counters *fc)
{
	return atomic64_read(&fc->fault_ct[FAMFS_PMD]);
}

static inline u64 famfs_pud_fault_ct(struct famfs_fault_counters *fc)
{
	return atomic64_read(&fc->fault_ct[FAMFS_PUD]);
}

#endif /* FAMFS_INTERNAL_H */
