.. SPDX-License-Identifier: GPL-2.0

.. _famfs_index:

==================================================================
famfs: The kernel component of the famfs shared memory file system
==================================================================

- Copyright (C) 2024 Micron Technology, Inc.

Introduction
============
Compute Express Link (CXL) provides a mechanism for disaggregated or
fabric-attached memory (FAM). This creates opportunities for data sharing;
clustered apps that would otherwise have to shard or replicate data can
share one copy in disaggregated memory.

Famfs, which is not CXL-specific in any way, provides a mechanism for
multiple hosts to use data in shared memory, by giving it a file system
interface. With famfs, any app that understands files (which is almost
all apps) can access data sets in shared memory. Although famfs
supports read and write, the real point is to support mmap, which
provides direct (dax) access to the memory - either writable or read-only.

Shared memory can pose complex coherency and synchronization issues, but
there are also simple cases. Two simple and eminently useful patterns that
occur frequently in data analytics and AI are:

* Serial Sharing - Only one host or process at a time has access to a file
* Read-only Sharing - Multiple hosts or processes share read-only access
  to a file

The famfs kernel file system is part of the famfs framework; User space
components [1] handle metadata allocation and distribution, and direct the
famfs kernel module to instantiate files that map to specific memory.

The famfs framework manages coherency of its own metadata and structures,
but does not attempt to manage coherency for applications.

Famfs also provides data isolation between files. That is, even though
the host has access to an entire memory "device" (as a dax device), apps
cannot write to memory for which the file is read-only, and mapping one
file provides isolation from the memory of all other files. This is pretty
basic, but some experimental shared memory usage patterns provide no such
isolation.

Principles of Operation
=======================

Without its user space components, the famfs kernel module doesn't do
anything useful. The user space components maintain superblocks and
metadata logs, and use the famfs kernel component to provide a file system
view of shared memory across multiple hosts.

Each host has an independent instance of the famfs kernel module. After
mount, files are not visible until the user space component instantiates
them (normally by playing the famfs metadata log).

Once instantiated, files on each host can point to the same shared memory,
but in-memory metadata (inodes, etc.) is ephemeral on each host that has a
famfs instance mounted. Like ramfs, the famfs in-kernel file system has no
backing store for metadata modifications. If metadata mutations are ever
persisted, that must be done by the user space components. However,
mutations to file data are saved to the shared memory - subject to write
permission and processor cache behavior.


Famfs is Not a Conventional File System
---------------------------------------

Famfs files can be accessed by conventional means, but there are
limitations. The kernel component of famfs is not involved in the
allocation of backing memory for files at all; the famfs user space
creates files and passes the allocation extent lists into the kernel via
the per-file FAMFSIOC_MAP_CREATE ioctl. A file that lacks this metadata is
treated as invalid by the famfs kernel module. As a practical matter files
must be created via the famfs library or cli, but they can be consumed as
if they were conventional files.

Famfs differs in some important ways from conventional file systems:

* Files must be pre-allocated by the famfs framework; Allocation is never
  performed on (or after) write.
* Any operation that changes a file's size is considered to put the file
  in an invalid state, disabling access to the data. It may be possible to
  revisit this in the future. (Typically the famfs user space can restore
  files to a valid state by replaying the famfs metadata log.)

Famfs exists to apply the existing file system abstractions to shared
memory so applications and workflows can more easily adapt to an
environment with disaggregated shared memory.

Memory Error Handling
=====================

Possible memory errors include timeouts, poison and unexpected
reconfiguration of an underlying dax device. In all of these cases, famfs
receives a call via its iomap_ops->notify_failure() function. If any
memory errors have been detected, Access to the affected famfs mount is
disabled to avoid further errors or corruption. Testing indicates that
a famfs instance that has encountered errors can be unmounted cleanly, but
Repairing memory errors or corruption is outside the scope of famfs.

Key Requirements
================

The primary requirements for famfs are:

1. Must support a file system abstraction backed by sharable dax memory
2. Files must efficiently handle VMA faults
3. Must support metadata distribution in a sharable way
4. Must handle clients with a stale copy of metadata

The famfs kernel component takes care of 1-2 above by caching each file's
mapping metadata in the kernel.

Requirements 3 and 4 are handled by the user space components, and are
largely orthogonal to the functionality of the famfs kernel module.

Requirements 3 and 4 cannot be met by conventional fs-dax file systems
(e.g. xfs and ext4) because they use write-back metadata; it is not valid
to mount such a file system on two hosts from the same in-memory image.


Famfs Usage
===========

Famfs usage is documented at [1].


References
==========

- [1] Famfs user space repository and documentation
      https://github.com/cxl-micron-reskit/famfs
