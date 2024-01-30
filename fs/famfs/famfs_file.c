// SPDX-License-Identifier: GPL-2.0
/*
 * famfs - dax file system for shared fabric-attached memory
 *
 * Copyright 2023-2024 Micron Technology, Inc.
 *
 * This file system, originally based on ramfs the dax support from xfs,
 * is intended to allow multiple host systems to mount a common file system
 * view of dax files that map to shared memory.
 */

#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/dax.h>
#include <linux/iomap.h>

#include "famfs_internal.h"

/*********************************************************************
 * vm_operations
 */
static vm_fault_t
__famfs_filemap_fault(struct vm_fault *vmf, unsigned int pe_size,
		      bool write_fault)
{
	struct inode *inode = file_inode(vmf->vma->vm_file);
	struct super_block *sb = inode->i_sb;
	struct famfs_fs_info *fsi = sb->s_fs_info;
	vm_fault_t ret;
	pfn_t pfn;

	if (fsi->deverror)
		return VM_FAULT_SIGBUS;

	if (!IS_DAX(file_inode(vmf->vma->vm_file))) {
		pr_err("%s: file not marked IS_DAX!!\n", __func__);
		return VM_FAULT_SIGBUS;
	}

	if (write_fault) {
		sb_start_pagefault(inode->i_sb);
		file_update_time(vmf->vma->vm_file);
	}

	ret = dax_iomap_fault(vmf, pe_size, &pfn, NULL, NULL /*&famfs_iomap_ops */);
	if (ret & VM_FAULT_NEEDDSYNC)
		ret = dax_finish_sync_fault(vmf, pe_size, pfn);

	if (write_fault)
		sb_end_pagefault(inode->i_sb);

	return ret;
}

static inline bool
famfs_is_write_fault(struct vm_fault *vmf)
{
	return (vmf->flags & FAULT_FLAG_WRITE) &&
	       (vmf->vma->vm_flags & VM_SHARED);
}

static vm_fault_t
famfs_filemap_fault(struct vm_fault *vmf)
{
	return __famfs_filemap_fault(vmf, 0, famfs_is_write_fault(vmf));
}

static vm_fault_t
famfs_filemap_huge_fault(struct vm_fault *vmf, unsigned int pe_size)
{
	return __famfs_filemap_fault(vmf, pe_size, famfs_is_write_fault(vmf));
}

static vm_fault_t
famfs_filemap_page_mkwrite(struct vm_fault *vmf)
{
	return __famfs_filemap_fault(vmf, 0, true);
}

static vm_fault_t
famfs_filemap_pfn_mkwrite(struct vm_fault *vmf)
{
	return __famfs_filemap_fault(vmf, 0, true);
}

static vm_fault_t
famfs_filemap_map_pages(struct vm_fault	*vmf, pgoff_t start_pgoff,
			pgoff_t	end_pgoff)
{
	return filemap_map_pages(vmf, start_pgoff, end_pgoff);
}

const struct vm_operations_struct famfs_file_vm_ops = {
	.fault		= famfs_filemap_fault,
	.huge_fault	= famfs_filemap_huge_fault,
	.map_pages	= famfs_filemap_map_pages,
	.page_mkwrite	= famfs_filemap_page_mkwrite,
	.pfn_mkwrite	= famfs_filemap_pfn_mkwrite,
};

/*********************************************************************
 * file_operations
 */

/* Reject I/O to files that aren't in a valid state */
static ssize_t
famfs_file_invalid(struct inode *inode)
{
	if (!IS_DAX(inode)) {
		pr_debug("%s: inode %llx IS_DAX is false\n",
			 __func__, (u64)inode);
		return -ENXIO;
	}
	return 0;
}

static ssize_t
famfs_rw_prep(struct kiocb *iocb, struct iov_iter *ubuf)
{
	struct inode *inode = iocb->ki_filp->f_mapping->host;
	struct super_block *sb = inode->i_sb;
	struct famfs_fs_info *fsi = sb->s_fs_info;
	size_t i_size = i_size_read(inode);
	size_t count = iov_iter_count(ubuf);
	size_t max_count;
	ssize_t rc;

	if (fsi->deverror)
		return -ENODEV;

	rc = famfs_file_invalid(inode);
	if (rc)
		return rc;

	max_count = max_t(size_t, 0, i_size - iocb->ki_pos);

	if (count > max_count)
		iov_iter_truncate(ubuf, max_count);

	if (!iov_iter_count(ubuf))
		return 0;

	return rc;
}

static ssize_t
famfs_dax_read_iter(struct kiocb *iocb, struct iov_iter	*to)
{
	ssize_t rc;

	rc = famfs_rw_prep(iocb, to);
	if (rc)
		return rc;

	if (!iov_iter_count(to))
		return 0;

	rc = dax_iomap_rw(iocb, to, NULL /*&famfs_iomap_ops */);

	file_accessed(iocb->ki_filp);
	return rc;
}

/**
 * famfs_dax_write_iter()
 *
 * We need our own write-iter in order to prevent append
 *
 * @iocb:
 * @from: iterator describing the user memory source for the write
 */
static ssize_t
famfs_dax_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	ssize_t rc;

	rc = famfs_rw_prep(iocb, from);
	if (rc)
		return rc;

	if (!iov_iter_count(from))
		return 0;

	return dax_iomap_rw(iocb, from, NULL /*&famfs_iomap_ops*/);
}

static int
famfs_file_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct inode *inode = file_inode(file);
	struct super_block *sb = inode->i_sb;
	struct famfs_fs_info *fsi = sb->s_fs_info;
	ssize_t rc;

	if (fsi->deverror)
		return -ENODEV;

	rc = famfs_file_invalid(inode);
	if (rc)
		return (int)rc;

	file_accessed(file);
	vma->vm_ops = &famfs_file_vm_ops;
	vm_flags_set(vma, VM_HUGEPAGE);
	return 0;
}

const struct file_operations famfs_file_operations = {
	.owner             = THIS_MODULE,

	/* Custom famfs operations */
	.write_iter	   = famfs_dax_write_iter,
	.read_iter	   = famfs_dax_read_iter,
	.unlocked_ioctl    = NULL /*famfs_file_ioctl*/,
	.mmap		   = famfs_file_mmap,

	/* Force PMD alignment for mmap */
	.get_unmapped_area = thp_get_unmapped_area,

	/* Generic Operations */
	.fsync		   = noop_fsync,
	.splice_read	   = filemap_splice_read,
	.splice_write	   = iter_file_splice_write,
	.llseek		   = generic_file_llseek,
};

