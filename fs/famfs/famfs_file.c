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

#include <linux/famfs_ioctl.h>
#include "famfs_internal.h"

/* Expose famfs kernel abi version as a read-only module parameter */
static int famfs_kabi_version = FAMFS_KABI_VERSION;
module_param(famfs_kabi_version, int, 0444);
MODULE_PARM_DESC(famfs_kabi_version, "famfs kernel abi version");

/**
 * famfs_meta_alloc() - Allocate famfs file metadata
 * @metap:       Pointer to an mcache_map_meta pointer
 * @ext_count:  The number of extents needed
 */
static int
famfs_meta_alloc_v1(struct famfs_file_meta **metap, size_t ext_count)
{
	struct famfs_file_meta *meta;

	meta = kzalloc(sizeof(*meta), GFP_KERNEL);
	if (!meta)
		return -ENOMEM;

	/* v1 alloc only handles simple extents */
	meta->se = kzalloc(sizeof(*(meta->se)), GFP_KERNEL);
	if (!meta->se) {
		kfree(meta);
		return -ENOMEM;
	}

	meta->fm_nextents = ext_count;
	meta->error = false;
	*metap = meta;

	return 0;
}

static void
famfs_meta_free(struct famfs_file_meta *map)
{
	if (map) {
		switch (map->fm_extent_type) {
		case SIMPLE_DAX_EXTENT:
			kfree(map->se);
			break;
		case INTERLEAVED_EXTENT:
			if (map->ie)
				kfree(map->ie->ie_strips);

			kfree(map->ie);
			break;
		default:
			break;
		}
	}
	kfree(map);
}

/**
 * famfs_file_init_dax_v1() - FAMFSIOC_MAP_CREATE ioctl handler
 * @file: the un-initialized file
 * @arg:  ptr to struct mcioc_map in user space
 *
 * Setup the dax mapping for a file. Files are created empty, and then function
 * is called by famfs_file_ioctl() to setup the mapping and set the file size.
 */
static int
famfs_file_init_dax_v1(struct file *file, void __user *arg)
{
	struct famfs_file_meta *meta = NULL;
	struct famfs_ioc_map imap;
	struct famfs_fs_info *fsi;
	size_t extent_total = 0;
	int alignment_errs = 0;
	struct super_block *sb;
	struct inode *inode;
	size_t ext_count;
	int rc;
	int i;

	inode = file_inode(file);
	if (!inode) {
		rc = -EBADF;
		goto errout;
	}

	sb  = inode->i_sb;
	fsi = sb->s_fs_info;
	if (fsi->deverror)
		return -ENODEV;

	rc = copy_from_user(&imap, arg, sizeof(imap));
	if (rc)
		return -EFAULT;

	ext_count = imap.ext_list_count;
	if (ext_count < 1) {
		rc = -ENOSPC;
		goto errout;
	}

	if (ext_count > FAMFS_MAX_EXTENTS) {
		rc = -E2BIG;
		goto errout;
	}

	rc = famfs_meta_alloc_v1(&meta, ext_count);
	if (rc)
		goto errout;

	meta->file_type = imap.file_type;
	meta->file_size = imap.file_size;

	/* Fill in the internal file metadata structure */
	for (i = 0; i < imap.ext_list_count; i++) {
		size_t len;
		off_t  offset;

		offset = imap.ext_list[i].offset;
		len    = imap.ext_list[i].len;

		extent_total += len;

		if (WARN_ON(offset == 0 && meta->file_type != FAMFS_SUPERBLOCK)) {
			rc = -EINVAL;
			goto errout;
		}

		meta->se[i].dev_index  = 0; /* must be zero for now */
		meta->se[i].ext_offset = offset;
		meta->se[i].ext_len    = len;

		/* All extent addresses/offsets must be 2MiB aligned,
		 * and all but the last length must be a 2MiB multiple.
		 */
		if (!IS_ALIGNED(offset, PMD_SIZE)) {
			pr_err("%s: error ext %d hpa %lx not aligned\n",
			       __func__, i, offset);
			alignment_errs++;
		}
		if (i < (imap.ext_list_count - 1) && !IS_ALIGNED(len, PMD_SIZE)) {
			pr_err("%s: error ext %d length %ld not aligned\n",
			       __func__, i, len);
			alignment_errs++;
		}
	}

	/*
	 * File size can be <= ext list size, since extent sizes are constrained
	 * to PMD multiples
	 */
	if (imap.file_size > extent_total) {
		pr_err("%s: file size %lld larger than ext list size %lld\n",
		       __func__, (u64)imap.file_size, (u64)extent_total);
		rc = -EINVAL;
		goto errout;
	}

	if (alignment_errs > 0) {
		pr_err("%s: there were %d alignment errors in the extent list\n",
		       __func__, alignment_errs);
		rc = -EINVAL;
		goto errout;
	}

	/* Publish the famfs metadata on inode->i_private */
	inode_lock(inode);
	if (inode->i_private) {
		rc = -EEXIST; /* file already has famfs metadata */
	} else {
		inode->i_private = meta;
		i_size_write(inode, imap.file_size);
		inode->i_flags |= S_DAX;
	}
	inode_unlock(inode);

 errout:
	if (rc)
		famfs_meta_free(meta);

	return rc;
}

/***************************************************************************/

#if 0
static void
dump_ioc_fmap(struct famfs_ioc_fmap *fmap)
{
	char *ext_type_str;
	char *type_str;

	switch (fmap->fioc_file_type) {
	case FAMFS_REG:
		type_str = "FAMFS_REG";
		break;
	case FAMFS_SUPERBLOCK:
		type_str = "FAMFS_SUPERBLOCK";
		break;
	case FAMFS_LOG:
		type_str = "FAMFS_LOG";
		break;
	default:
		type_str = "FAMFS_INVALID";
		break;
	}
	switch (fmap->fioc_ext_type) {
	case SIMPLE_DAX_EXTENT:
		ext_type_str = "SIMPLE_DAX_EXTENT";
		break;
	case INTERLEAVED_EXTENT:
		ext_type_str = "INTERLRAVED_EXTENT";
		break;
	default:
		ext_type_str = "INVALID_EXTENT_TYPE";
	}
	pr_notice("fmap:  size=%lld type=%s ext_type=%s nextents=%d\n",
		  fmap->fioc_file_size, type_str, ext_type_str,
		  fmap->fioc_nextents);
}

static void
famfs_dump_meta(
	const char *msg,
	const struct famfs_file_meta *meta)
{
	int i, j;

	pr_notice("%s:%s", msg, __func__);
	pr_notice("\ttype=%d size=%ld ext_type=%d\n",
		  meta->file_type, meta->file_size, meta->fm_extent_type);
	switch(meta->fm_extent_type) {
	case SIMPLE_DAX_EXTENT:
		for (j = 0 ; j < meta->fm_nextents; j++) {
			struct famfs_meta_simple_ext *ext = &meta->se[j];

			pr_notice("\t\t%d: devindex=%lld offset=%lld len=%lld\n",
				  j, ext->dev_index, ext->ext_offset, ext->ext_len);
		}
		break;
	case INTERLEAVED_EXTENT:
		for (i = 0; i < meta->fm_niext; i++) {
			pr_notice("\tinterleaved_ext: nstrips=%d\n", i);
			for (j = 0; j < meta->ie[i].ie_nstrips; j++) {
				struct famfs_meta_simple_ext *strip = &meta->ie[i].ie_strips[j];

				pr_notice("\t\t%d: devindex=%lld offset=%lld len=%lld\n",
					  j, strip->dev_index, strip->ext_offset, strip->ext_len);
			}
		}
		break;
	default:
		pr_notice("%s: invalid extent type\n", __func__);
	}
}
#endif

static int
famfs_check_ext_alignment(struct famfs_ioc_simple_extent *se)
{
	int errs = 0;

	if (se->devindex != 0)
		errs++;

	if (!IS_ALIGNED(se->offset, PMD_SIZE))
		errs++;

	if (!IS_ALIGNED(se->len, PMD_SIZE))
		errs++;

	return errs;
}

/**
 * famfs_meta_alloc() - Allocate famfs file metadata
 * @metap:       Pointer to an mcache_map_meta pointer
 * @ext_count:  The number of extents needed
 */
static int
famfs_meta_alloc_v2(
	struct famfs_ioc_fmap *fmap,
	struct famfs_file_meta **metap,
	enum famfs_extent_type ext_type)
{
	struct famfs_file_meta *meta = NULL;
	size_t extent_total = 0;
	int errs = 0;
	int i, j;
	int rc;

	if (fmap->fioc_nextents > FAMFS_MAX_EXTENTS)
		return -EINVAL;

	meta = kzalloc(sizeof(*meta), GFP_KERNEL);
	if (!meta)
		return -ENOMEM;

	meta->error = false;

	meta->file_type = fmap->fioc_file_type;
	meta->file_size = fmap->fioc_file_size;
	meta->fm_extent_type = fmap->fioc_ext_type;

	switch (fmap->fioc_ext_type) {
	case SIMPLE_DAX_EXTENT: {
		struct famfs_ioc_simple_extent tmp_ext_list[FAMFS_MAX_EXTENTS];
		size_t ext_list_size;

		meta->fm_nextents = fmap->fioc_nextents;

		meta->se = kcalloc(fmap->fioc_nextents, sizeof(*fmap->kse),
				   GFP_KERNEL);
		if (!meta->se) {
			rc = -ENOMEM;
			goto errout;
		}

		if (fmap->fioc_nextents > FAMFS_MAX_EXTENTS) {
			rc = -EINVAL;
			goto errout;
		}

		ext_list_size = fmap->fioc_nextents * sizeof(*fmap->kse);
		rc = copy_from_user(tmp_ext_list, fmap->kse, ext_list_size);
		if (rc) {
			pr_err("%s: copy_from_user rc=%d nextents=%d size=%ld\n",
			       __func__, rc, fmap->fioc_nextents, ext_list_size);
			goto errout;
		}

		for (i = 0; i < fmap->fioc_nextents; i++) {
			meta->se[i].dev_index  = tmp_ext_list[i].devindex;
			meta->se[i].ext_offset = tmp_ext_list[i].offset;
			meta->se[i].ext_len    = tmp_ext_list[i].len;

			errs += famfs_check_ext_alignment(&tmp_ext_list[i]);

			extent_total += meta->se[i].ext_len;
		}
		break;
	}

	case INTERLEAVED_EXTENT: {
		struct famfs_ioc_interleaved_ext
			tmp_ie[FAMFS_IOC_MAX_INTERLEAVED_EXTENTS] = { 0 };
		struct famfs_ioc_simple_extent tmp_ext_list[FAMFS_MAX_STRIPS];
		s64 size_remainder = meta->file_size;
		int niext = fmap->fioc_niext;

		if (niext > FAMFS_IOC_MAX_INTERLEAVED_EXTENTS) {
			rc = -EINVAL;
			goto errout;
		}

		meta->fm_niext = niext;

		/* Get the full list of famfs_ioc_interleaved_ext structs */
		rc = copy_from_user(tmp_ie, fmap->kie,
				    niext *
				     sizeof(struct famfs_ioc_interleaved_ext));
		if (rc) {
			pr_err("%s: copy_from_user rc=%d for interleaved extents\n",
				 __func__, rc);
			goto errout;
		}

		/* Allocate interleaved extent */
		meta->ie = kcalloc(niext, sizeof(*(meta->ie)), GFP_KERNEL);
		if (!meta->ie) {
			rc = -ENOMEM;
			goto errout;
		}

		/*
		 * Each interleaved extent has a simple extent list of strips.
		 * Outer loops is over separate interleaved extents
		 */
		for (i = 0; i < niext; i++) {
			//struct famfs_ioc_simple_extent *se;
			u64 nstrips = tmp_ie[i].ie_nstrips;
			size_t strip_list_size;

			if (nstrips > FAMFS_MAX_STRIPS) {
				pr_err("%s: nstrips %lld exceeds max %d\n",
				       __func__, nstrips,
				       FAMFS_MAX_STRIPS);
				errs++;
			}

			/* Get the strip list for this interleaved set */
			strip_list_size =
				nstrips * sizeof(struct famfs_ioc_simple_extent);
			rc = copy_from_user(tmp_ext_list, tmp_ie[i].ie_strips,
					    strip_list_size);
			if (rc) {
				pr_err("%s: copy_from_user rc=%d for strips\n",
					 __func__, rc);
				goto errout;
			}

			meta->ie[i].fie_chunk_size = tmp_ie[i].ie_chunk_size;
			meta->ie[i].fie_nstrips    = tmp_ie[i].ie_nstrips;
			meta->ie[i].fie_nbytes     = tmp_ie[i].ie_nbytes;

			/* Allocate strip extent array */
			meta->ie[i].ie_strips = kcalloc(tmp_ie[i].ie_nstrips,
					sizeof(meta->ie[i].ie_strips[0]),
							GFP_KERNEL);
			if (!meta->ie[i].ie_strips) {
				rc = -ENOMEM;
				goto errout;
			}
			/* Inner loop is over strips */
			for (j = 0; j < nstrips; j++) {
				u64 devindex = tmp_ext_list[j].devindex;
				u64 offset   = tmp_ext_list[j].offset;
				u64 len      = tmp_ext_list[j].len;

				errs += famfs_check_ext_alignment(&tmp_ext_list[j]);

				meta->ie[i].ie_strips[j].dev_index  = devindex;
				meta->ie[i].ie_strips[j].ext_offset = offset;
				meta->ie[i].ie_strips[j].ext_len    = len;
				extent_total += len;
			}
			size_remainder -= tmp_ie[i].ie_nbytes;
		}

		if (size_remainder > 0) {
			/* Sum of interleaved extent sizes is less than file size! */
			pr_err("%s: size_remainder %lld\n", __func__, size_remainder);
			rc = -EINVAL;
			goto errout;
		}
		
		break;
	}

	default:
		return -EINVAL;
	  
	}

	if (errs > 0) {
		pr_err("%s: %d alignment errors found\n", __func__, errs);
		rc = -EINVAL;
		goto errout;
	}

	/* More sanity checks */
	if (extent_total < meta->file_size) {
		pr_err("%s: file size %ld larger than map size %ld\n",
		       __func__, meta->file_size, extent_total);
		rc = -EINVAL;
		goto errout;
	}

	*metap = meta;

	return 0;
errout:
	famfs_meta_free(meta);
	return rc;
}

static int
famfs_file_init_dax_v2(struct file *file, void __user *arg)
{
	struct famfs_file_meta *meta = NULL;
	struct famfs_ioc_fmap fmap;
	struct famfs_fs_info *fsi;
	struct super_block *sb;
	struct inode *inode;
	int rc;

	inode = file_inode(file);
	if (!inode) {
		rc = -EBADF;
		goto errout;
	}

	if (inode->i_private)
		return -EEXIST;

	sb  = inode->i_sb;
	fsi = sb->s_fs_info;
	if (fsi->deverror)
		return -ENODEV;

	rc = copy_from_user(&fmap, arg, sizeof(fmap));
	if (rc)
		return -EFAULT;

	if (fmap.fioc_nextents < 1) {
		rc = -EINVAL;
		goto errout;
	}

	if (fmap.fioc_nextents > FAMFS_MAX_EXTENTS) {
		rc = -E2BIG;
		goto errout;
	}

	/* This fully populates the metadata, unlike the v1 allocator */
	rc = famfs_meta_alloc_v2(&fmap, &meta, fmap.fioc_ext_type);
	if (rc)
		goto errout;

	/* Publish the famfs metadata on inode->i_private */
	inode_lock(inode);
	if (inode->i_private) {
		rc = -EEXIST; /* file already has famfs metadata */
	} else {
		inode->i_private = meta;
		i_size_write(inode, meta->file_size);
		inode->i_flags |= S_DAX;
	}
	inode_unlock(inode);

 errout:
	if (rc)
		famfs_meta_free(meta);

	return rc;
}


static int
famfs_prepare_getmap_v2(
	struct inode *inode,
	struct famfs_ioc_get_fmap *ifmap)
{
	struct famfs_file_meta *meta = inode->i_private;
	int i;

	memset(ifmap, 0, sizeof(*ifmap));

	ifmap->iocmap.fioc_file_size = meta->file_size;
	ifmap->iocmap.fioc_file_type = meta->file_type;
	ifmap->iocmap.fioc_ext_type  = meta->fm_extent_type;

	switch (meta->fm_extent_type) {
	case FAMFS_IOC_EXT_SIMPLE:
		ifmap->iocmap.fioc_nextents  = meta->fm_nextents;
		for (i = 0; i < meta->fm_nextents; i++) {
			ifmap->ikse[i].devindex = meta->se[i].dev_index;
			ifmap->ikse[i].offset   = meta->se[i].ext_offset;
			ifmap->ikse[i].len      = meta->se[i].ext_len;
		}
		break;

	case FAMFS_IOC_EXT_INTERLEAVE:
		/* We only support one striped "extent", but it has multiple
		 * simple extents to describe its strips
		 */
		if (meta->fm_nextents != 1)
			return -EINVAL;

		ifmap->iocmap.fioc_niext  = meta->fm_niext;
		ifmap->ks.ikie.ie_nstrips = meta->ie->fie_nstrips;
		ifmap->ks.ikie.ie_chunk_size = meta->ie->fie_chunk_size;
		ifmap->ks.ikie.ie_nbytes = meta->ie->fie_nbytes;

		for (i = 0; i < meta->ie->fie_nstrips; i++) {
			struct famfs_meta_simple_ext *strips = meta->ie->ie_strips;

			ifmap->ks.kie_strips[i].devindex = strips[i].dev_index;
			ifmap->ks.kie_strips[i].offset   = strips[i].ext_offset;
			ifmap->ks.kie_strips[i].len      = strips[i].ext_len;
		}
		break;
	default:
		return -1;
	}

	return 0;
}

/**
 * famfs_file_ioctl() - Top-level famfs file ioctl handler
 * @file: the file
 * @cmd:  ioctl opcode
 * @arg:  ioctl opcode argument (if any)
 */
static long
famfs_file_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	struct famfs_fs_info *fsi = inode->i_sb->s_fs_info;
	long rc;

	if (fsi->deverror && (cmd != FAMFSIOC_NOP))
		return -ENODEV;

	switch (cmd) {
	case FAMFSIOC_NOP:
		rc = 0;
		break;

	case FAMFSIOC_MAP_CREATE:
		rc = famfs_file_init_dax_v1(file, (void *)arg);
		break;

	case FAMFSIOC_MAP_GET: {
		struct inode *inode = file_inode(file);
		struct famfs_file_meta *meta = inode->i_private;
		struct famfs_ioc_map umeta;

		memset(&umeta, 0, sizeof(umeta));

		if (meta) {
			/* TODO: do more to harmonize these structures */
			umeta.extent_type    = meta->fm_extent_type;
			umeta.file_size      = i_size_read(inode);
			umeta.ext_list_count = meta->fm_nextents;

			rc = copy_to_user((void __user *)arg, &umeta,
					  sizeof(umeta));
			if (rc)
				pr_err("%s: copy_to_user returned %ld\n",
				       __func__, rc);

		} else {
			pr_err("%s: FAMFS_IOC_MAP_GET: file has no meta!\n", __func__);
			rc = -EINVAL;
		}
		break;
	}
	case FAMFSIOC_MAP_GETEXT: {
		struct inode *inode = file_inode(file);
		struct famfs_file_meta *meta = inode->i_private;
		struct famfs_extent ext_list[FAMFS_MAX_EXTENTS] = { 0 };
		int i;

		/* This legacy ioctl only supports simple extent lists */
		if (meta->fm_extent_type != SIMPLE_DAX_EXTENT)
			return -EINVAL;

		if (meta->fm_nextents > FAMFS_MAX_EXTENTS)
			return -EINVAL;

		for (i = 0; i < meta->fm_nextents; i++) {
			ext_list[i].offset = meta->se[i].ext_offset;
			ext_list[i].len = meta->se[i].ext_len;
		}

		if (meta)
			rc = copy_to_user((void __user *)arg, ext_list,
			      meta->fm_nextents * sizeof(struct famfs_extent));
		else
			rc = -EINVAL;
		break;
	}
	case FAMFSIOC_MAP_CREATE_V2:
		rc = famfs_file_init_dax_v2(file, (void *)arg);
		break;

	case FAMFSIOC_MAP_GET_V2: {
		struct famfs_ioc_get_fmap ifmap;

		rc = famfs_prepare_getmap_v2(inode, &ifmap);
		if (rc)
			return rc;

		rc = copy_to_user((void __user *)arg, &ifmap, sizeof(ifmap));
		break;
	}

	default:
		rc = -ENOTTY;
		break;
	}

	return rc;
}

/*********************************************************************
 * iomap_operations
 *
 * This stuff uses the iomap (dax-related) helpers to resolve file offsets to
 * offsets within a dax device.
 */

static ssize_t famfs_file_invalid(struct inode *inode);

static int
famfs_meta_to_dax_offset_v2(struct inode *inode, struct iomap *iomap,
			 loff_t file_offset, off_t len, unsigned int flags)
{
	struct famfs_fs_info  *fsi = inode->i_sb->s_fs_info;
	struct famfs_file_meta *meta = inode->i_private;
	loff_t local_offset = file_offset;
	int i;

	/* This function is only for extent_type INTERLEAVED_EXTENT */
	if (meta->fm_extent_type != INTERLEAVED_EXTENT) {
		pr_err("%s: bad extent type\n", __func__);
		goto err_out;
	}

	if (fsi->deverror || famfs_file_invalid(inode))
		goto err_out;

	iomap->offset = file_offset;

	for (i = 0; i < meta->fm_niext; i++) {
		/* TODO: check devindex too */
		struct famfs_meta_interleaved_ext *fei = &meta->ie[i];
		u64 chunk_size = fei->fie_chunk_size;
		u64 nstrips = fei->fie_nstrips;
		u64 ext_size = fei->fie_nbytes;

		ext_size = min_t(u64, ext_size, meta->file_size);

		if (ext_size == 0)
			goto err_out;

		/* Is the data is in this striped extent? */
		if (local_offset < ext_size) {
			u64 chunk_num       = local_offset / chunk_size;
			u64 chunk_offset    = local_offset % chunk_size;
			u64 stripe_num      = chunk_num / nstrips;
			u64 strip_num       = chunk_num % nstrips;
			u64 chunk_remainder = chunk_size - chunk_offset;
			u64 strip_offset    = chunk_offset + (stripe_num * chunk_size);
			u64 strip_dax_ofs   = fei->ie_strips[strip_num].ext_offset;

			iomap->addr    = strip_dax_ofs + strip_offset;
			iomap->offset  = file_offset;
			iomap->length  = min_t(loff_t, len, chunk_remainder);
			iomap->dax_dev = fsi->dax_devp;
			iomap->type    = IOMAP_MAPPED;
			iomap->flags   = flags;

			return 0;
		}
		local_offset -= ext_size; /* offset is beyond this striped extent */
	}

 err_out:

	/* We fell out the end of the extent list.
	 * Set iomap to zero length in this case, and return 0
	 * This just means that the r/w is past EOF
	 */
	iomap->addr    = 0; /* there is no valid dax device offset */
	iomap->offset  = file_offset; /* file offset */
	iomap->length  = 0; /* this had better result in no access to dax mem */
	iomap->dax_dev = fsi->dax_devp;
	iomap->type    = IOMAP_MAPPED;
	iomap->flags   = flags;

	return 0;
}

/**
 * famfs_meta_to_dax_offset() - Resolve (file, offset, len) to (daxdev, offset, len)
 *
 * This function is called by famfs_iomap_begin() to resolve an offset in a
 * file to an offset in a dax device. This is upcalled from dax from calls to
 * both  * dax_iomap_fault() and dax_iomap_rw(). Dax finishes the job resolving
 * a fault to a specific physical page (the fault case) or doing a memcpy
 * variant (the rw case)
 *
 * Pages can be PTE (4k), PMD (2MiB) or (theoretically) PuD (1GiB)
 * (these sizes are for X86; may vary on other cpu architectures
 *
 * @inode:  The file where the fault occurred
 * @iomap:       To be filled in to indicate where to find the right memory,
 *               relative  to a dax device.
 * @file_offset: Within the file where the fault occurred (will be page boundary)
 * @len:         The length of the faulted mapping (will be a page multiple)
 *               (will be trimmed in *iomap if it's disjoint in the extent list)
 * @flags:
 *
 * Return values: 0. (info is returned in a modified @iomap struct)
 */
static int
famfs_meta_to_dax_offset(struct inode *inode, struct iomap *iomap,
			 loff_t file_offset, off_t len, unsigned int flags)
{
	struct famfs_fs_info  *fsi = inode->i_sb->s_fs_info;
	struct famfs_file_meta *meta = inode->i_private;
	loff_t local_offset = file_offset;
	int i;

	if (fsi->deverror || famfs_file_invalid(inode))
		goto err_out;

	if (meta->fm_extent_type == INTERLEAVED_EXTENT)
		return famfs_meta_to_dax_offset_v2(inode, iomap, file_offset, len, flags);

	iomap->offset = file_offset;

	for (i = 0; i < meta->fm_nextents; i++) {
		/* TODO: check devindex too */
		loff_t dax_ext_offset = meta->se[i].ext_offset;
		loff_t dax_ext_len    = meta->se[i].ext_len;

		if ((dax_ext_offset == 0) &&
		    (meta->file_type != FAMFS_SUPERBLOCK))
			pr_warn("%s: zero offset on non-superblock file!!\n",
				__func__);

		/* local_offset is the offset minus the size of extents skipped
		 * so far; If local_offset < dax_ext_len, the data of interest
		 * starts in this extent
		 */
		if (local_offset < dax_ext_len) {
			loff_t ext_len_remainder = dax_ext_len - local_offset;

			/*
			 * OK, we found the file metadata extent where this
			 * data begins
			 * @local_offset      - The offset within the current
			 *                      extent
			 * @ext_len_remainder - Remaining length of ext after
			 *                      skipping local_offset
			 * Outputs:
			 * iomap->addr:   the offset within the dax device where
			 *                the  data starts
			 * iomap->offset: the file offset
			 * iomap->length: the valid length resolved here
			 */
			iomap->addr    = dax_ext_offset + local_offset;
			iomap->offset  = file_offset;
			iomap->length  = min_t(loff_t, len, ext_len_remainder);
			iomap->dax_dev = fsi->dax_devp;
			iomap->type    = IOMAP_MAPPED;
			iomap->flags   = flags;

			return 0;
		}
		local_offset -= dax_ext_len; /* Get ready for the next extent */
	}

 err_out:
	/* We fell out the end of the extent list.
	 * Set iomap to zero length in this case, and return 0
	 * This just means that the r/w is past EOF
	 */
	iomap->addr    = 0; /* there is no valid dax device offset */
	iomap->offset  = file_offset; /* file offset */
	iomap->length  = 0; /* this had better result in no access to dax mem */
	iomap->dax_dev = fsi->dax_devp;
	iomap->type    = IOMAP_MAPPED;
	iomap->flags   = flags;

	return 0;
}

/**
 * famfs_iomap_begin() - Handler for iomap_begin upcall from dax
 *
 * This function is pretty simple because files are
 * * never partially allocated
 * * never have holes (never sparse)
 * * never "allocate on write"
 *
 * @inode:  inode for the file being accessed
 * @offset: offset within the file
 * @length: Length being accessed at offset
 * @flags:
 * @iomap:  iomap struct to be filled in, resolving (offset, length) to
 *          (daxdev, offset, len)
 * @srcmap:
 */
static int
famfs_iomap_begin(struct inode *inode, loff_t offset, loff_t length,
		  unsigned int flags, struct iomap *iomap, struct iomap *srcmap)
{
	struct famfs_file_meta *meta = inode->i_private;
	size_t size;

	size = i_size_read(inode);

	WARN_ON(size != meta->file_size);

	return famfs_meta_to_dax_offset(inode, iomap, offset, length, flags);
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

	ret = dax_iomap_fault(vmf, pe_size, &pfn, NULL, &famfs_iomap_ops);
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
	struct famfs_file_meta *meta = inode->i_private;
	size_t i_size = i_size_read(inode);

	if (!meta) {
		pr_debug("%s: un-initialized famfs file\n", __func__);
		return -EIO;
	}
	if (meta->error) {
		pr_debug("%s: previously detected metadata errors\n", __func__);
		return -EIO;
	}
	if (i_size != meta->file_size) {
		pr_warn("%s: i_size overwritten from %ld to %ld\n",
		       __func__, meta->file_size, i_size);
		meta->error = true;
		return -ENXIO;
	}
	if (!IS_DAX(inode)) {
		pr_debug("%s: inode %llx IS_DAX is false\n", __func__, (u64)inode);
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

	rc = dax_iomap_rw(iocb, to, &famfs_iomap_ops);

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

	return dax_iomap_rw(iocb, from, &famfs_iomap_ops);
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
	.unlocked_ioctl    = famfs_file_ioctl,
	.mmap		   = famfs_file_mmap,

	/* Force PMD alignment for mmap */
	.get_unmapped_area = thp_get_unmapped_area,

	/* Generic Operations */
	.fsync		   = noop_fsync,
	.splice_read	   = filemap_splice_read,
	.splice_write	   = iter_file_splice_write,
	.llseek		   = generic_file_llseek,
};

