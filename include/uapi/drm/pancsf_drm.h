/* SPDX-License-Identifier: MIT */
/* Copyright (C) 2023 Collabora ltd. */
#ifndef _PANCSF_DRM_H_
#define _PANCSF_DRM_H_

#include "drm.h"

#if defined(__cplusplus)
extern "C" {
#endif

/*
 * Userspace driver controls GPU cache flushling through CS instructions, but
 * the flush reduction mechanism requires a flush_id. This flush_id could be
 * queried with an ioctl, but Arm provides a well-isolated register page
 * containing only this read-only register, so let's expose this page through
 * a static mmap offset and allow direct mapping of this MMIO region so we
 * can avoid the user <-> kernel round-trip.
 */
#define DRM_PANCSF_USER_MMIO_OFFSET		(0xffffull << 48)
#define DRM_PANCSF_USER_FLUSH_ID_MMIO_OFFSET	(DRM_PANCSF_USER_MMIO_OFFSET | 0)

/* Place new ioctls at the end, don't re-oder. */
enum drm_pancsf_ioctl_id {
	DRM_PANCSF_DEV_QUERY = 0,
	DRM_PANCSF_VM_CREATE,
	DRM_PANCSF_VM_DESTROY,
	DRM_PANCSF_BO_CREATE,
	DRM_PANCSF_BO_MMAP_OFFSET,
	DRM_PANCSF_VM_MAP,
	DRM_PANCSF_VM_UNMAP,
	DRM_PANCSF_GROUP_CREATE,
	DRM_PANCSF_GROUP_DESTROY,
	DRM_PANCSF_GROUP_GET_STATE,
	DRM_PANCSF_TILER_HEAP_CREATE,
	DRM_PANCSF_TILER_HEAP_DESTROY,
	DRM_PANCSF_GROUP_SUBMIT,
	DRM_PANCSF_VM_BIND_QUEUE_CREATE,
	DRM_PANCSF_VM_BIND_QUEUE_DESTROY,
	DRM_PANCSF_VM_BIND_SUBMIT,
};

#define DRM_IOCTL_PANCSF_DEV_QUERY		DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_DEV_QUERY, struct drm_pancsf_dev_query)
#define DRM_IOCTL_PANCSF_VM_CREATE		DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_VM_CREATE, struct drm_pancsf_vm_create)
#define DRM_IOCTL_PANCSF_VM_DESTROY		DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_VM_DESTROY, struct drm_pancsf_vm_destroy)
#define DRM_IOCTL_PANCSF_BO_CREATE		DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_BO_CREATE, struct drm_pancsf_bo_create)
#define DRM_IOCTL_PANCSF_BO_MMAP_OFFSET		DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_BO_MMAP_OFFSET, struct drm_pancsf_bo_mmap_offset)
#define DRM_IOCTL_PANCSF_VM_MAP			DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_VM_MAP, struct drm_pancsf_vm_map)
#define DRM_IOCTL_PANCSF_VM_UNMAP		DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_VM_UNMAP, struct drm_pancsf_vm_unmap)
#define DRM_IOCTL_PANCSF_GROUP_CREATE		DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_GROUP_CREATE, struct drm_pancsf_group_create)
#define DRM_IOCTL_PANCSF_GROUP_DESTROY		DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_GROUP_DESTROY, struct drm_pancsf_group_destroy)
#define DRM_IOCTL_PANCSF_GROUP_GET_STATE	DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_GROUP_GET_STATE, struct drm_pancsf_group_get_state)
#define DRM_IOCTL_PANCSF_TILER_HEAP_CREATE	DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_TILER_HEAP_CREATE, struct drm_pancsf_tiler_heap_create)
#define DRM_IOCTL_PANCSF_TILER_HEAP_DESTROY	DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_TILER_HEAP_DESTROY, struct drm_pancsf_tiler_heap_destroy)
#define DRM_IOCTL_PANCSF_GROUP_SUBMIT		DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_GROUP_SUBMIT, struct drm_pancsf_group_submit)
#define DRM_IOCTL_PANCSF_VM_BIND_QUEUE_CREATE	DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_VM_BIND_QUEUE_CREATE, struct drm_pancsf_vm_bind_queue_create)
#define DRM_IOCTL_PANCSF_VM_BIND_QUEUE_DESTROY	DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_VM_BIND_QUEUE_DESTROY, struct drm_pancsf_vm_bind_queue_destroy)
#define DRM_IOCTL_PANCSF_VM_BIND_SUBMIT		DRM_IOWR(DRM_COMMAND_BASE + DRM_PANCSF_VM_BIND_SUBMIT, struct drm_pancsf_vm_bind_submit)

/* Place new types at the end, don't re-oder. */
enum drm_pancsf_dev_query_type {
	DRM_PANCSF_DEV_QUERY_GPU_INFO = 0,
	DRM_PANCSF_DEV_QUERY_CSIF_INFO,
};

struct drm_pancsf_gpu_info {
#define DRM_PANCSF_ARCH_MAJOR(x)		((x) >> 28)
#define DRM_PANCSF_ARCH_MINOR(x)		(((x) >> 24) & 0xf)
#define DRM_PANCSF_ARCH_REV(x)			(((x) >> 20) & 0xf)
#define DRM_PANCSF_PRODUCT_MAJOR(x)		(((x) >> 16) & 0xf)
#define DRM_PANCSF_VERSION_MAJOR(x)		(((x) >> 12) & 0xf)
#define DRM_PANCSF_VERSION_MINOR(x)		(((x) >> 4) & 0xff)
#define DRM_PANCSF_VERSION_STATUS(x)		((x) & 0xf)
	__u32 gpu_id;
	__u32 gpu_rev;
#define DRM_PANCSF_CSHW_MAJOR(x)		(((x) >> 26) & 0x3f)
#define DRM_PANCSF_CSHW_MINOR(x)		(((x) >> 20) & 0x3f)
#define DRM_PANCSF_CSHW_REV(x)			(((x) >> 16) & 0xf)
#define DRM_PANCSF_MCU_MAJOR(x)			(((x) >> 10) & 0x3f)
#define DRM_PANCSF_MCU_MINOR(x)			(((x) >> 4) & 0x3f)
#define DRM_PANCSF_MCU_REV(x)			((x) & 0xf)
	__u32 csf_id;
	__u32 l2_features;
	__u32 tiler_features;
	__u32 mem_features;
	__u32 mmu_features;
	__u32 thread_features;
	__u32 max_threads;
	__u32 thread_max_workgroup_size;
	__u32 thread_max_barrier_size;
	__u32 coherency_features;
	__u32 texture_features[4];
	__u32 as_present;
	__u32 core_group_count;
	__u32 pad;
	__u64 shader_present;
	__u64 l2_present;
	__u64 tiler_present;
};

struct drm_pancsf_csif_info {
	__u32 csg_slot_count;
	__u32 cs_slot_count;
	__u32 cs_reg_count;
	__u32 scoreboard_slot_count;
	__u32 unpreserved_cs_reg_count;
	__u32 pad;
};

struct drm_pancsf_dev_query {
	/** @type: the query type (see enum drm_pancsf_dev_query_type). */
	__u32 type;

	/**
	 * @size: size of the type being queried.
	 *
	 * If pointer is NULL, size is updated by the driver to provide the
	 * output structure size. If pointer is not NULL, the the driver will
	 * only copy min(size, actual_structure_size) bytes to the pointer,
	 * and update the size accordingly. This allows us to extend query
	 * types without breaking userspace.
	 */
	__u32 size;

	/**
	 * @pointer: user pointer to a query type struct.
	 *
	 * Pointer can be NULL, in which case, nothing is copied, but the
	 * actual structure size is returned. If not NULL, it must point to
	 * a location that's large enough to hold size bytes.
	 */
	__u64 pointer;
};

struct drm_pancsf_vm_create {
	/** @flags: VM flags, MBZ. */
	__u32 flags;

	/** @id: Returned VM ID */
	__u32 id;
};

struct drm_pancsf_vm_destroy {
	/** @id: ID of the VM to destroy */
	__u32 id;

	/** @pad: MBZ. */
	__u32 pad;
};

struct drm_pancsf_bo_create {
	/**
	 * @size: Requested size for the object
	 *
	 * The (page-aligned) allocated size for the object will be returned.
	 */
	__u64 size;

	/**
	 * @flags: Flags, currently unused, MBZ.
	 */
	__u32 flags;

	/**
	 * @vm_id: Attached VM, if any
	 *
	 * If a VM is specified, this BO must:
	 *
	 *  1. Only ever be bound to that VM.
	 *
	 *  2. Cannot be exported as a PRIME fd.
	 */
	__u32 vm_id;

	/**
	 * @handle: Returned handle for the object.
	 *
	 * Object handles are nonzero.
	 */
	__u32 handle;

	/* @pad: MBZ. */
	__u32 pad;
};

struct drm_pancsf_bo_mmap_offset {
	/** @handle: Handle for the object being mapped. */
	__u32 handle;

	/** @pad: MBZ. */
	__u32 pad;

	/** @offset: The fake offset to use for subsequent mmap call */
	__u64 offset;
};

#define PANCSF_VMA_MAP_READONLY		0x1
#define PANCSF_VMA_MAP_NOEXEC		0x2
#define PANCSF_VMA_MAP_UNCACHED		0x4
#define PANCSF_VMA_MAP_FRAG_SHADER	0x8
#define PANCSF_VMA_MAP_ON_FAULT		0x10
#define PANCSF_VMA_MAP_AUTO_VA		0x20

struct drm_pancsf_vm_map {
	/** @vm_id: VM to map BO range to */
	__u32 vm_id;

	/** @flags: Combination of PANCSF_VMA_MAP_ flags */
	__u32 flags;

	/** @pad: MBZ. */
	__u32 pad;

	/** @bo_handle: Buffer object to map. */
	__u32 bo_handle;

	/** @bo_offset: Buffer object offset. */
	__u64 bo_offset;

	/**
	 * @va: Virtual address to map the BO to. Mapping address returned here if
	 *	PANCSF_VMA_MAP_ON_FAULT is set.
	 */
	__u64 va;

	/** @size: Size to map. */
	__u64 size;
};

struct drm_pancsf_vm_unmap {
	/** @vm_id: VM to map BO range to */
	__u32 vm_id;

	/** @flags: MBZ. */
	__u32 flags;

	/** @va: Virtual address to unmap. */
	__u64 va;

	/** @size: Size to unmap. */
	__u64 size;
};

enum drm_pancsf_sync_op_type {
	DRM_PANCSF_SYNC_OP_WAIT = 0,
	DRM_PANCSF_SYNC_OP_SIGNAL,
};

enum drm_pancsf_sync_handle_type {
	DRM_PANCSF_SYNC_HANDLE_TYPE_SYNCOBJ = 0,
	DRM_PANCSF_SYNC_HANDLE_TYPE_TIMELINE_SYNCOBJ,
};

struct drm_pancsf_sync_op {
	/** @op_type: Sync operation type. */
	__u32 op_type;

	/** @handle_type: Sync handle type. */
	__u32 handle_type;

	/** @handle: Sync handle. */
	__u32 handle;

	/** @flags: MBZ. */
	__u32 flags;

	/** @timeline_value: MBZ if handle_type != DRM_PANCSF_SYNC_HANDLE_TYPE_TIMELINE_SYNCOBJ. */
	__u64 timeline_value;
};

struct drm_pancsf_obj_array {
	/** @stride: Stride of object struct. Used for versioning. */
	__u32 stride;

	/** @count: Number of objects in the array. */
	__u32 count;

	/** @array: User pointer to an array of objects. */
	__u64 array;
};

#define DRM_PANCSF_OBJ_ARRAY(cnt, ptr) \
	{ .stride = sizeof(ptr[0]), .count = cnt, .array = (__u64)(uintptr_t)ptr }

struct drm_pancsf_queue_submit {
	/** @queue_index: Index of the queue inside a group. */
	__u32 queue_index;

	/** @stream_size: Size of the command stream to execute. */
	__u32 stream_size;

	/** @stream_addr: GPU address of the command stream to execute. */
	__u64 stream_addr;

	/**
	 * @lastest_flush: FLUSH_ID read at the time the stream was built.
	 *
	 * This allows cache flush elimination for the automatic
	 * flush+invalidate(all) done at submission time, which is needed to
	 * ensure the GPU doesn't get garbage when reading the linear CS
	 * buffers. If you want the cache flush to happen unconditionally,
	 * pass a zero here.
	 */
	__u32 latest_flush;

	/** @pad: MBZ. */
	__u32 pad;

	/** @syncs: Array of sync operations. */
	struct drm_pancsf_obj_array syncs;
};

struct drm_pancsf_group_submit {
	/** @group_handle: Handle of the group to queue jobs to. */
	__u32 group_handle;

	/** @pad: MBZ. */
	__u32 pad;

	/** @syncs: Array of queue submit operations. */
	struct drm_pancsf_obj_array queue_submits;
};

struct drm_pancsf_queue_create {
	/**
	 * @priority: Defines the priority of queues inside a group. Goes from 0 to 15,
	 *	      15 being the highest priority.
	 */
	__u8 priority;

	/** @pad: Padding fields, MBZ. */
	__u8 pad[3];

	/** @ringbuf_size: Size of the ring buffer to allocate to this queue. */
	__u32 ringbuf_size;
};

enum drm_pancsf_group_priority {
	PANCSF_GROUP_PRIORITY_LOW = 0,
	PANCSF_GROUP_PRIORITY_MEDIUM,
	PANCSF_GROUP_PRIORITY_HIGH,
};

struct drm_pancsf_group_create {
	/** @queues: Array of drm_pancsf_create_cs_queue elements. */
	struct drm_pancsf_obj_array queues;

	/**
	 * @max_compute_cores: Maximum number of cores that can be
	 *		       used by compute jobs across CS queues
	 *		       bound to this group.
	 */
	__u8 max_compute_cores;

	/**
	 * @max_fragment_cores: Maximum number of cores that can be
	 *			used by fragment jobs across CS queues
	 *			bound to this group.
	 */
	__u8 max_fragment_cores;

	/**
	 * @max_tiler_cores: Maximum number of tilers that can be
	 *		     used by tiler jobs across CS queues
	 *		     bound to this group.
	 */
	__u8 max_tiler_cores;

	/** @priority: Group priority (see drm_drm_pancsf_cs_group_priority). */
	__u8 priority;

	/** @pad: Padding field, MBZ. */
	__u32 pad;

	/** @compute_core_mask: Mask encoding cores that can be used for compute jobs. */
	__u64 compute_core_mask;

	/** @fragment_core_mask: Mask encoding cores that can be used for fragment jobs. */
	__u64 fragment_core_mask;

	/** @tiler_core_mask: Mask encoding cores that can be used for tiler jobs. */
	__u64 tiler_core_mask;

	/**
	 * @vm_id: VM ID to bind this group to. All submission to queues bound to this
	 *	   group will use this VM.
	 */
	__u32 vm_id;

	/*
	 * @group_handle: Returned group handle. Passed back when submitting jobs or
	 *		  destroying a group.
	 */
	__u32 group_handle;
};

struct drm_pancsf_group_destroy {
	/** @group_handle: Group to destroy */
	__u32 group_handle;

	/** @pad: Padding field, MBZ. */
	__u32 pad;
};

struct drm_pancsf_group_get_state {
	/** @group_handle: Handle of the group to query state on */
	__u32 group_handle;

#define DRM_PANCSF_GROUP_STATE_DESTROYED		0x1
#define DRM_PANCSF_GROUP_STATE_TIMEDOUT			0x2
#define DRM_PANCSF_GROUP_STATE_FATAL_FAULT		0x4
	/** @state: Combination of DRM_PANCSF_GROUP_STATE_* flags encoding the
	 *	    group state.
	 */
	__u32 state;

	/** @fatal_queues: Bitmask of queues that faced fatal faults. */
	__u32 fatal_queues;

	/** @pad: MBZ */
	__u32 pad;
};


struct drm_pancsf_tiler_heap_create {
	/** @vm_id: VM ID the tiler heap should be mapped to */
	__u32 vm_id;

	/** @initial_chunk_count: Initial number of chunks to allocate. */
	__u32 initial_chunk_count;

	/** @chunk_size: Chunk size. Must be a power of two at least 256KB large. */
	__u32 chunk_size;

	/* @max_chunks: Maximum number of chunks that can be allocated. */
	__u32 max_chunks;

	/** @target_in_flight: Maximum number of in-flight render passes.
	 * If exceeded the FW will wait for render passes to finish before
	 * queuing new tiler jobs.
	 */
	__u32 target_in_flight;

	/** @handle: Returned heap handle. Passed back to DESTROY_TILER_HEAP. */
	__u32 handle;

	/** @tiler_heap_ctx_gpu_va: Returned heap GPU virtual address returned */
	__u64 tiler_heap_ctx_gpu_va;
	__u64 first_heap_chunk_gpu_va;
};

struct drm_pancsf_tiler_heap_destroy {
	/** @handle: Handle of the tiler heap to destroy */
	__u32 handle;

	/** @pad: Padding field, MBZ. */
	__u32 pad;
};

enum drm_pancsf_bind_queue_priority {
	PANCSF_BIND_QUEUE_PRIORITY_LOW = 0,
	PANCSF_BIND_QUEUE_PRIORITY_MEDIUM,
	PANCSF_BIND_QUEUE_PRIORITY_HIGH,
};

struct drm_pancsf_vm_bind_queue_create {
	/** @handle: Returned bind queue handle/ */
	__u32 handle;

	/** @priority: Bind queue priority. */
	__u8 priority;

	/** @pad: MBZ. */
	__u8 pad[3];
};

struct drm_pancsf_vm_bind_queue_destroy {
	/** @handle: Bind queue to destroy */
	__u32 handle;

	/** @pad: MBZ. */
	__u32 pad;
};

enum drm_pancsf_vm_bind_op_type {
	DRM_PANCSF_BIND_OP_MAP = 0,
	DRM_PANCSF_BIND_OP_UNMAP,
};

struct drm_pancsf_vm_bind_op {
	/** @type: Bind operation type (see drm_pancsf_vm_bind_op_type) */
	__u32 type;

	/** @pad: MBZ. */
	__u32 pad;

	union {
		/** @map: Map arguments. */
		struct drm_pancsf_vm_map map;

		/** @unmap: Unmap arguments. */
		struct drm_pancsf_vm_unmap unmap;
	};
};

struct drm_pancsf_vm_bind_submit {
	/** @vm_id: VM this bind queue acts on. */
	__u32 vm_id;

	/** @queue_handle: Bind queue handle. */
	__u32 queue_handle;

	/** @syncs: Array of sync objects. */
	struct drm_pancsf_obj_array syncs;

	/** @ops: Array of bind operations. */
	struct drm_pancsf_obj_array ops;
};

#if defined(__cplusplus)
}
#endif

#endif /* _PANCSF_DRM_H_ */
