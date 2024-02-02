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

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/dax.h>
#include <linux/uio.h>
#include <linux/iomap.h>
#include <uapi/linux/famfs_ioctl.h>
#include "famfs_internal.h"

/*********************************************************************
 * iomap_operations
 *
 * This stuff uses the iomap (dax-related) helpers to resolve file offsets to
 * offsets within a dax device.
 */

/**
 * famfs_meta_to_dax_offset()
 *
 * This function is called by famfs_iomap_begin() to resolve an offset in a file to
 * an offset in a dax device. This is upcalled from dax from calls to both
 * dax_iomap_fault() and dax_iomap_rw(). Dax finishes the job resolving a fault to
 * a specific physical page (the fault case) or doing a memcpy variant (the rw case)
 *
 * Pages can be PTE (4k), PMD (2MiB) or (theoretically) PuD (1GiB)
 * (these sizes are for X86; may vary on other cpu architectures
 *
 * @inode  - the file where the fault occurred
 * @iomap  - struct iomap to be filled in to indicate where to find the right memory, relative
 *           to a dax device.
 * @offset - the offset within the file where the fault occurred (will be page boundary)
 * @len    - the length of the faulted mapping (will be a page multiple)
 *           (will be trimmed in *iomap if it's disjoint in the extent list)
 * @flags
 */
static int
famfs_meta_to_dax_offset(
	struct inode *inode,
	struct iomap *iomap,
	loff_t        offset,
	loff_t        len,
	unsigned int  flags)
{
	struct famfs_file_meta *meta = (struct famfs_file_meta *)inode->i_private;
	int i;
	loff_t local_offset = offset;
	struct famfs_fs_info  *fsi = inode->i_sb->s_fs_info;

	iomap->offset = offset; /* file offset */

	for (i = 0; i < meta->tfs_extent_ct; i++) {
		loff_t dax_ext_offset = meta->tfs_extents[i].offset;
		loff_t dax_ext_len    = meta->tfs_extents[i].len;

		if ((dax_ext_offset == 0) && (meta->file_type != FAMFS_SUPERBLOCK))
			pr_err("%s: zero offset on non-superblock file!!\n", __func__);

		/* local_offset is the offset minus the size of extents skipped so far;
		 * If local_offset < dax_ext_len, the data of interest starts in this extent
		 */
		if (local_offset < dax_ext_len) {
			loff_t ext_len_remainder = dax_ext_len - local_offset;

			/*+
			 * OK, we found the file metadata extent where this data begins
			 * @local_offset      - The offset within the current extent
			 * @ext_len_remainder - Remaining length of ext after skipping local_offset
			 *
			 * iomap->addr is the offset within the dax device where that data
			 * starts
			 */
			iomap->addr    = dax_ext_offset + local_offset; /* dax dev offset */
			iomap->offset  = offset; /* file offset */
			iomap->length  = min_t(loff_t, len, ext_len_remainder);
			iomap->dax_dev = fsi->dax_devp;
			iomap->type    = IOMAP_MAPPED;
			iomap->flags   = flags;

			return 0;
		}
		local_offset -= dax_ext_len; /* Get ready for the next extent */
	}

	/* Set iomap to zero length in this case, and return 0
	 * This just means that the r/w is past EOF
	 */
	iomap->addr    = offset;
	iomap->offset  = offset; /* file offset */
	iomap->length  = 0; /* this had better result in no access to dax mem */
	iomap->dax_dev = fsi->dax_devp;
	iomap->type    = IOMAP_MAPPED;
	iomap->flags   = flags;

	return 0;
}

/**
 * famfs_iomap_begin()
 *
 * This function is pretty simple because files are
 * * never partially allocated
 * * never have holes (never sparse)
 * * never "allocate on write"
 */
static int
famfs_iomap_begin(
	struct inode	       *inode,
	loff_t			offset,
	loff_t			length,
	unsigned int		flags,
	struct iomap	       *iomap,
	struct iomap	       *srcmap)
{
	struct famfs_file_meta *meta = inode->i_private;
	size_t size;
	int rc;

	size = i_size_read(inode);

	WARN_ON(size != meta->file_size);

	rc = famfs_meta_to_dax_offset(inode, iomap, offset, length, flags);

	return rc;
}

/* Note: We never need a special set of write_iomap_ops because famfs never
 * performs allocation on write.
 */
const struct iomap_ops famfs_iomap_ops = {
	.iomap_begin		= famfs_iomap_begin,
};

/*********************************************************************
 * vm_operations
 */
static vm_fault_t
__famfs_filemap_fault(
	struct vm_fault		*vmf,
	unsigned int		pe_size,
	bool			write_fault)
{
	struct inode		*inode = file_inode(vmf->vma->vm_file);
	vm_fault_t		ret;

	if (write_fault) {
		sb_start_pagefault(inode->i_sb);
		file_update_time(vmf->vma->vm_file);
	}

	if (IS_DAX(inode)) {
		pfn_t pfn;

		ret = dax_iomap_fault(vmf, pe_size, &pfn, NULL, &famfs_iomap_ops);
		if (ret & VM_FAULT_NEEDDSYNC)
			ret = dax_finish_sync_fault(vmf, pe_size, pfn);
	} else {
		/* All famfs faults will be dax... */
		pr_err("%s: oops, non-dax fault\n", __func__);
		ret = VM_FAULT_SIGBUS;
	}

	if (write_fault)
		sb_end_pagefault(inode->i_sb);

	return ret;
}

static inline bool
famfs_is_write_fault(
	struct vm_fault		*vmf)
{
	return (vmf->flags & FAULT_FLAG_WRITE) &&
	       (vmf->vma->vm_flags & VM_SHARED);
}

static vm_fault_t
famfs_filemap_fault(
	struct vm_fault		*vmf)
{
	/* DAX can shortcut the normal fault path on write faults! */
	return __famfs_filemap_fault(vmf, 0,
			IS_DAX(file_inode(vmf->vma->vm_file)) && famfs_is_write_fault(vmf));
}

static vm_fault_t
famfs_filemap_huge_fault(
	struct vm_fault	*vmf,
	unsigned int	 pe_size)
{
	if (!IS_DAX(file_inode(vmf->vma->vm_file))) {
		pr_err("%s: file not marked IS_DAX!!\n", __func__);
		return VM_FAULT_SIGBUS;
	}

	/* DAX can shortcut the normal fault path on write faults! */
	return __famfs_filemap_fault(vmf, pe_size, famfs_is_write_fault(vmf));
}

static vm_fault_t
famfs_filemap_page_mkwrite(
	struct vm_fault		*vmf)
{
	return __famfs_filemap_fault(vmf, 0, true);
}

static vm_fault_t
famfs_filemap_pfn_mkwrite(
	struct vm_fault		*vmf)
{
	return __famfs_filemap_fault(vmf, 0, true);
}

static vm_fault_t
famfs_filemap_map_pages(
	struct vm_fault	       *vmf,
	pgoff_t			start_pgoff,
	pgoff_t			end_pgoff)
{
	vm_fault_t ret;

	ret = filemap_map_pages(vmf, start_pgoff, end_pgoff);
	return ret;
}

const struct vm_operations_struct famfs_file_vm_ops = {
	.fault		= famfs_filemap_fault,
	.huge_fault	= famfs_filemap_huge_fault,
	.map_pages	= famfs_filemap_map_pages,
	.page_mkwrite	= famfs_filemap_page_mkwrite,
	.pfn_mkwrite	= famfs_filemap_pfn_mkwrite,
};

