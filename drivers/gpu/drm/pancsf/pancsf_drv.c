// SPDX-License-Identifier: GPL-2.0
/* Copyright 2018 Marty E. Plummer <hanetzer@startmail.com> */
/* Copyright 2019 Linaro, Ltd., Rob Herring <robh@kernel.org> */
/* Copyright 2019 Collabora ltd. */

#include <linux/module.h>
#include <linux/of_platform.h>
#include <linux/pagemap.h>
#include <linux/pm_runtime.h>
#include <drm/pancsf_drm.h>
#include <drm/drm_drv.h>
#include <drm/drm_ioctl.h>
#include <drm/drm_syncobj.h>
#include <drm/drm_utils.h>
#include <drm/drm_debugfs.h>

#include "pancsf_sched.h"
#include "pancsf_device.h"
#include "pancsf_gem.h"
#include "pancsf_heap.h"
#include "pancsf_mcu.h"
#include "pancsf_mmu.h"
#include "pancsf_gpu.h"
#include "pancsf_regs.h"

#define DRM_PANCSF_SYNC_OP_MIN_SIZE		24
#define DRM_PANCSF_QUEUE_SUBMIT_MIN_SIZE	40
#define DRM_PANCSF_QUEUE_CREATE_MIN_SIZE	8
#define DRM_PANCSF_VM_BIND_OP_MIN_SIZE		48

static int pancsf_ioctl_dev_query(struct drm_device *ddev, void *data, struct drm_file *file)
{
	struct drm_pancsf_dev_query *args = data;
	struct pancsf_device *pfdev = ddev->dev_private;
	const void *src;
	size_t src_size;

	switch (args->type) {
	case DRM_PANCSF_DEV_QUERY_GPU_INFO:
		src_size = sizeof(pfdev->gpu_info);
		src = &pfdev->gpu_info;
		break;
	case DRM_PANCSF_DEV_QUERY_CSIF_INFO:
		src_size = sizeof(pfdev->csif_info);
		src = &pfdev->csif_info;
		break;
	default:
		return -EINVAL;
	}

	if (!args->pointer) {
		args->size = src_size;
		return 0;
	}

	args->size = min_t(unsigned long, src_size, args->size);
	if (copy_to_user((void __user *)(uintptr_t)args->pointer, src, args->size))
		return -EFAULT;

	return 0;
}

#define PANCSF_MAX_VMS_PER_FILE		32
#define PANCSF_VM_CREATE_FLAGS		0

static int pancsf_ioctl_vm_create(struct drm_device *ddev, void *data,
				  struct drm_file *file)
{
	struct pancsf_device *pfdev = ddev->dev_private;
	struct pancsf_file *pfile = file->driver_priv;
	struct drm_pancsf_vm_create *args = data;
	int ret;

	if (args->flags & ~PANCSF_VM_CREATE_FLAGS)
		return -EINVAL;

	ret = pancsf_vm_pool_create_vm(pfdev, pfile->vms);
	if (ret < 0)
		return ret;

	args->id = ret;
	return 0;
}

static int pancsf_ioctl_vm_destroy(struct drm_device *ddev, void *data,
				   struct drm_file *file)
{
	struct pancsf_file *pfile = file->driver_priv;
	struct drm_pancsf_vm_destroy *args = data;

	pancsf_vm_pool_destroy_vm(pfile->vms, args->id);
	return 0;
}

#define PANCSF_BO_FLAGS		0

static int pancsf_ioctl_bo_create(struct drm_device *ddev, void *data,
				  struct drm_file *file)
{
	struct pancsf_file *pfile = file->driver_priv;
	struct pancsf_gem_object *bo;
	struct drm_pancsf_bo_create *args = data;
	struct pancsf_vm *vm = NULL;

	if (!args->size || args->pad ||
	    (args->flags & ~PANCSF_BO_FLAGS))
		return -EINVAL;

	if (args->vm_id) {
		vm = pancsf_vm_pool_get_vm(pfile->vms, args->vm_id);
		if (!vm)
			return -EINVAL;
	}

	bo = pancsf_gem_create_with_handle(file, ddev, vm, args->size, args->flags,
					   &args->handle);

	pancsf_vm_put(vm);

	if (IS_ERR(bo))
		return PTR_ERR(bo);

	return 0;
}

#define PANCSF_VMA_MAP_FLAGS (PANCSF_VMA_MAP_READONLY | \
			      PANCSF_VMA_MAP_NOEXEC | \
			      PANCSF_VMA_MAP_UNCACHED | \
			      PANCSF_VMA_MAP_FRAG_SHADER | \
			      PANCSF_VMA_MAP_ON_FAULT | \
			      PANCSF_VMA_MAP_AUTO_VA)

static int pancsf_ioctl_vm_map(struct drm_device *ddev, void *data,
			       struct drm_file *file)
{
	struct pancsf_file *pfile = file->driver_priv;
	struct drm_pancsf_vm_map *args = data;
	struct drm_gem_object *gem;
	struct pancsf_vm *vm;
	int ret;

	if (args->flags & ~PANCSF_VMA_MAP_FLAGS)
		return -EINVAL;

	gem = drm_gem_object_lookup(file, args->bo_handle);
	if (!gem)
		return -EINVAL;

	vm = pancsf_vm_pool_get_vm(pfile->vms, args->vm_id);
	if (vm) {
		ret = pancsf_vm_map_bo_range(vm, to_pancsf_bo(gem), args->bo_offset,
					     args->size, &args->va, args->flags);
	} else {
		ret = -EINVAL;
	}

	pancsf_vm_put(vm);
	drm_gem_object_put(gem);
	return ret;
}

#define PANCSF_VMA_UNMAP_FLAGS 0

static int pancsf_ioctl_vm_unmap(struct drm_device *ddev, void *data,
				 struct drm_file *file)
{
	struct pancsf_file *pfile = file->driver_priv;
	struct drm_pancsf_vm_unmap *args = data;
	struct pancsf_vm *vm;
	int ret;

	if (args->flags & ~PANCSF_VMA_UNMAP_FLAGS)
		return -EINVAL;

	vm = pancsf_vm_pool_get_vm(pfile->vms, args->vm_id);
	if (vm)
		ret = pancsf_vm_unmap_range(vm, args->va, args->size);
	else
		ret = -EINVAL;

	pancsf_vm_put(vm);
	return ret;
}

static void *pancsf_get_obj_array(struct drm_pancsf_obj_array *in, u32 min_stride, u32 obj_size)
{
	u32 cpy_elem_size = min_t(u32, in->stride, obj_size);
	int ret = 0;
	void *out;

	if (in->stride < min_stride)
		return ERR_PTR(-EINVAL);

	out = kvmalloc_array(in->count, obj_size, GFP_KERNEL | __GFP_ZERO);
	if (!out)
		return ERR_PTR(-ENOMEM);

	if (obj_size == in->stride) {
		if (copy_from_user(out, u64_to_user_ptr(in->array), obj_size * in->count))
			ret = -EFAULT;
	} else {
		void __user *in_ptr = u64_to_user_ptr(in->array);
		void *out_ptr = out;
		u32 i;

		for (i = 0; i < in->count; i++) {
			if (copy_from_user(out_ptr, in_ptr, cpy_elem_size)) {
				ret = -EFAULT;
				break;
			}

			out_ptr += obj_size;
			in_ptr += in->stride;
		}
	}

	if (ret) {
		kvfree(out);
		return ERR_PTR(ret);
	}

	return out;
}

static int pancsf_add_job_deps(struct drm_file *file, struct pancsf_job *job,
			       struct drm_pancsf_sync_op *sync_ops, u32 sync_op_count)
{
	u32 i;

	for (i = 0; i < sync_op_count; i++) {
		struct dma_fence *fence;
		int ret;

		if (sync_ops[i].op_type != DRM_PANCSF_SYNC_OP_WAIT)
			continue;

		switch (sync_ops[i].handle_type) {
		case DRM_PANCSF_SYNC_HANDLE_TYPE_SYNCOBJ:
		case DRM_PANCSF_SYNC_HANDLE_TYPE_TIMELINE_SYNCOBJ:
			ret = drm_syncobj_find_fence(file, sync_ops[i].handle,
						     sync_ops[i].timeline_value,
						     0, &fence);
			if (ret)
				return ret;

			ret = pancsf_add_job_dep(job, fence);
			if (ret) {
				dma_fence_put(fence);
				return ret;
			}
			break;

		default:
			return -EINVAL;
		}
	}

	return 0;
}

struct pancsf_sync_signal {
	struct drm_syncobj *syncobj;
	struct dma_fence_chain *chain;
	u64 point;
};

struct pancsf_sync_signal_array {
	struct pancsf_sync_signal *signals;
	u32 count;
};

static void
pancsf_free_sync_signal_array(struct pancsf_sync_signal_array *array)
{
	u32 i;

	for (i = 0; i < array->count; i++) {
		drm_syncobj_put(array->signals[i].syncobj);
		dma_fence_chain_free(array->signals[i].chain);
	}

	kvfree(array->signals);
	array->signals = NULL;
	array->count = 0;
}

static int
pancsf_collect_sync_signal_array(struct drm_file *file,
				 struct drm_pancsf_sync_op *sync_ops, u32 sync_op_count,
				 struct pancsf_sync_signal_array *array)
{
	u32 count = 0, i;
	int ret;

	for (i = 0; i < sync_op_count; i++) {
		if (sync_ops[i].op_type == DRM_PANCSF_SYNC_OP_SIGNAL)
			count++;
	}

	array->signals = kvmalloc_array(count, sizeof(*array->signals), GFP_KERNEL | __GFP_ZERO);
	if (!array->signals)
		return -ENOMEM;

	for (i = 0; i < sync_op_count; i++) {
		if (sync_ops[i].op_type != DRM_PANCSF_SYNC_OP_SIGNAL)
			continue;

		switch (sync_ops[i].handle_type) {
		case DRM_PANCSF_SYNC_HANDLE_TYPE_TIMELINE_SYNCOBJ:
			array->signals[array->count].chain = dma_fence_chain_alloc();
			if (!array->signals[array->count].chain) {
				ret = -ENOMEM;
				goto err;
			}

			array->signals[array->count].point = sync_ops[i].timeline_value;
			fallthrough;

		case DRM_PANCSF_SYNC_HANDLE_TYPE_SYNCOBJ:
			array->signals[array->count].syncobj = drm_syncobj_find(file, sync_ops[i].handle);
			if (!array->signals[array->count].syncobj) {
				ret = -EINVAL;
				goto err;
			}

			array->count++;
			break;

		default:
			ret = -EINVAL;
			goto err;
		}
	}

	return 0;

err:
	pancsf_free_sync_signal_array(array);
	return ret;
}

static void pancsf_attach_done_fence(struct drm_file *file, struct dma_fence *done_fence,
				     struct pancsf_sync_signal_array *signal_array)
{
	u32 i;

	for (i = 0; i < signal_array->count; i++) {
		if (signal_array->signals[i].chain) {
			drm_syncobj_add_point(signal_array->signals[i].syncobj,
					      signal_array->signals[i].chain,
					      done_fence,
					      signal_array->signals[i].point);
			signal_array->signals[i].chain = NULL;
		} else {
			drm_syncobj_replace_fence(signal_array->signals[i].syncobj, done_fence);
		}
	}
}

static int pancsf_ioctl_bo_mmap_offset(struct drm_device *ddev, void *data,
				       struct drm_file *file)
{
	struct drm_pancsf_bo_mmap_offset *args = data;
	struct drm_gem_object *obj;
	int ret;

	obj = drm_gem_object_lookup(file, args->handle);
	if (!obj)
		return -ENOENT;

	ret = drm_gem_create_mmap_offset(obj);
	if (ret)
		goto out;

	args->offset = drm_vma_node_offset_addr(&obj->vma_node);

out:
	drm_gem_object_put(obj);
	return ret;
}

static int pancsf_ioctl_group_submit(struct drm_device *ddev, void *data,
				     struct drm_file *file)
{
	struct pancsf_file *pfile = file->driver_priv;
	struct drm_pancsf_group_submit *args = data;
	struct drm_pancsf_queue_submit *queue_submits;
	struct pancsf_job **jobs = NULL;
	struct drm_pancsf_sync_op *sync_ops = NULL;
	struct pancsf_sync_signal_array *sync_signal_arrays;
	int ret = 0;
	u32 i;

	queue_submits = pancsf_get_obj_array(&args->queue_submits,
					     DRM_PANCSF_QUEUE_SUBMIT_MIN_SIZE,
					     sizeof(*queue_submits));
	jobs = kvmalloc_array(args->queue_submits.count, sizeof(*jobs), GFP_KERNEL | __GFP_ZERO);
	sync_signal_arrays = kvmalloc_array(args->queue_submits.count, sizeof(*sync_signal_arrays),
					    GFP_KERNEL | __GFP_ZERO);
	if (!queue_submits || !jobs || !sync_signal_arrays) {
		ret = -ENOMEM;
		goto out_free_tmp_objs;
	}

	for (i = 0; i < args->queue_submits.count; i++) {
		struct drm_pancsf_queue_submit *qsubmit = &queue_submits[i];
		struct pancsf_call_info cs_call = {
			.start = qsubmit->stream_addr,
			.size = qsubmit->stream_size,
			.latest_flush = qsubmit->latest_flush,
		};

		jobs[i] = pancsf_create_job(pfile, args->group_handle,
					    qsubmit->queue_index, &cs_call);
		if (IS_ERR(jobs[i])) {
			ret = PTR_ERR(jobs[i]);
			jobs[i] = NULL;
			goto out_free_tmp_objs;
		}

		sync_ops = pancsf_get_obj_array(&qsubmit->syncs, DRM_PANCSF_SYNC_OP_MIN_SIZE,
						sizeof(*sync_ops));
		if (IS_ERR(sync_ops)) {
			ret = PTR_ERR(sync_ops);
			sync_ops = NULL;
			goto out_free_tmp_objs;
		}

		ret = pancsf_add_job_deps(file, jobs[i], sync_ops, qsubmit->syncs.count);
		if (ret)
			goto out_free_tmp_objs;

		ret = pancsf_collect_sync_signal_array(file, sync_ops, qsubmit->syncs.count,
						       &sync_signal_arrays[i]);
		if (ret)
			goto out_free_tmp_objs;

		kvfree(sync_ops);
		sync_ops = NULL;
	}

	for (i = 0; i < args->queue_submits.count; i++) {
		pancsf_attach_done_fence(file, pancsf_get_job_done_fence(jobs[i]),
					 &sync_signal_arrays[i]);
	}

	for (i = 0; i < args->queue_submits.count; i++) {
		struct dma_fence *done_fence = pancsf_get_job_done_fence(jobs[i]);

		if (!ret) {
			ret = pancsf_push_job(jobs[i]);
			if (ret) {
				dma_fence_set_error(done_fence, ret);
				dma_fence_signal(done_fence);
			}
		} else  {
			dma_fence_set_error(done_fence, -ECANCELED);
			dma_fence_signal(done_fence);
		}
	}

out_free_tmp_objs:
	if (sync_signal_arrays) {
		for (i = 0; i < args->queue_submits.count; i++)
			pancsf_free_sync_signal_array(&sync_signal_arrays[i]);
		kvfree(sync_signal_arrays);
	}

	if (jobs) {
		for (i = 0; i < args->queue_submits.count; i++)
			pancsf_put_job(jobs[i]);

		kvfree(jobs);
	}

	kvfree(queue_submits);
	kvfree(sync_ops);

	return ret;
}

static int pancsf_ioctl_group_destroy(struct drm_device *ddev, void *data,
				      struct drm_file *file)
{
	struct pancsf_file *pfile = file->driver_priv;
	struct drm_pancsf_group_destroy *args = data;

	pancsf_destroy_group(pfile, args->group_handle);
	return 0;
}

static int pancsf_ioctl_group_create(struct drm_device *ddev, void *data,
				     struct drm_file *file)
{
	struct pancsf_file *pfile = file->driver_priv;
	struct drm_pancsf_group_create *args = data;
	struct drm_pancsf_queue_create *queue_args;
	int ret;

	if (!args->queues.count)
		return -EINVAL;

	queue_args = pancsf_get_obj_array(&args->queues, DRM_PANCSF_QUEUE_CREATE_MIN_SIZE,
					  sizeof(*queue_args));
	if (IS_ERR(queue_args))
		return PTR_ERR(queue_args);

	ret = pancsf_create_group(pfile, args, queue_args);
	if (ret >= 0) {
		args->group_handle = ret;
		ret = 0;
	}

	kvfree(queue_args);
	return ret;
}

static int pancsf_ioctl_group_get_state(struct drm_device *ddev, void *data,
					struct drm_file *file)
{
	struct pancsf_file *pfile = file->driver_priv;
	struct drm_pancsf_group_get_state *args = data;

	return pancsf_group_get_state(pfile, args);
}

static int pancsf_ioctl_tiler_heap_create(struct drm_device *ddev, void *data,
					  struct drm_file *file)
{
	struct pancsf_device *pfdev = ddev->dev_private;
	struct pancsf_file *pfile = file->driver_priv;
	struct drm_pancsf_tiler_heap_create *args = data;
	struct pancsf_heap_pool *pool;
	struct pancsf_vm *vm;
	int ret;

	vm = pancsf_vm_pool_get_vm(pfile->vms, args->vm_id);
	if (!vm)
		return -EINVAL;

	mutex_lock(&pfile->heaps_lock);
	if (IS_ERR_OR_NULL(pfile->heaps))
		pfile->heaps = pancsf_heap_pool_create(pfdev, vm);
	pool = pfile->heaps;
	mutex_unlock(&pfile->heaps_lock);

	if (IS_ERR(pool)) {
		ret = PTR_ERR(pool);
		goto out_vm_put;
	}

	ret = pancsf_heap_create(pool,
				 args->initial_chunk_count,
				 args->chunk_size,
				 args->max_chunks,
				 args->target_in_flight,
				 &args->tiler_heap_ctx_gpu_va,
				 &args->first_heap_chunk_gpu_va);
	if (ret < 0)
		goto out_vm_put;

	args->handle = ret;
	ret = 0;

out_vm_put:
	pancsf_vm_put(vm);
	return ret;
}

static int pancsf_ioctl_tiler_heap_destroy(struct drm_device *ddev, void *data,
					   struct drm_file *file)
{
	struct pancsf_file *pfile = file->driver_priv;
	struct drm_pancsf_tiler_heap_destroy *args = data;
	struct pancsf_heap_pool *pool;

	mutex_lock(&pfile->heaps_lock);
	pool = pfile->heaps;
	mutex_unlock(&pfile->heaps_lock);

	if (IS_ERR_OR_NULL(pool))
		return -EINVAL;

	return pancsf_heap_destroy(pool, args->handle);
}

static int pancsf_ioctl_vm_bind_queue_create(struct drm_device *ddev, void *data,
					     struct drm_file *file)
{
	struct pancsf_device *pfdev = ddev->dev_private;
	struct pancsf_file *pfile = file->driver_priv;
	struct drm_pancsf_vm_bind_queue_create *args = data;
	int ret;

	ret = pancsf_vm_bind_queue_pool_create_queue(pfdev, pfile->vm_bind_queues, args->priority);
	if (ret < 0)
		return ret;

	args->handle = ret;
	return 0;
}

static int pancsf_ioctl_vm_bind_queue_destroy(struct drm_device *ddev, void *data,
					      struct drm_file *file)
{
	struct pancsf_file *pfile = file->driver_priv;
	struct drm_pancsf_vm_bind_queue_destroy *args = data;

	pancsf_vm_bind_queue_pool_destroy_queue(pfile->vm_bind_queues, args->handle);
	return 0;
}

static int pancsf_vm_bind_job_add_deps(struct drm_file *file, struct pancsf_vm_bind_job *job,
				       struct drm_pancsf_sync_op *sync_ops, u32 sync_op_count)
{
	u32 i;

	for (i = 0; i < sync_op_count; i++) {
		struct dma_fence *fence;
		int ret;

		if (sync_ops[i].op_type != DRM_PANCSF_SYNC_OP_WAIT)
			continue;

		switch (sync_ops[i].handle_type) {
		case DRM_PANCSF_SYNC_HANDLE_TYPE_SYNCOBJ:
		case DRM_PANCSF_SYNC_HANDLE_TYPE_TIMELINE_SYNCOBJ:
			ret = drm_syncobj_find_fence(file, sync_ops[i].handle,
						     sync_ops[i].timeline_value,
						     0, &fence);
			if (ret)
				return ret;

			ret = pancsf_vm_bind_job_add_dep(job, fence);
			if (ret) {
				dma_fence_put(fence);
				return ret;
			}
			break;

		default:
			return -EINVAL;
		}
	}

	return 0;
}

static int pancsf_ioctl_vm_bind_submit(struct drm_device *ddev, void *data,
				       struct drm_file *file)
{
	struct pancsf_sync_signal_array sync_signal_array = {};
	struct pancsf_file *pfile = file->driver_priv;
	struct drm_pancsf_vm_bind_submit *args = data;
	struct pancsf_vm_bind_queue *queue = NULL;
	struct drm_pancsf_vm_bind_op *bind_ops = NULL;
	struct drm_pancsf_sync_op *sync_ops = NULL;
	struct pancsf_vm_bind_job *job = NULL;
	struct pancsf_vm *vm = NULL;
	int ret = 0;

	queue = pancsf_vm_bind_queue_pool_get_queue(pfile->vm_bind_queues, args->queue_handle);
	if (!queue)
		return -EINVAL;

	vm = pancsf_vm_pool_get_vm(pfile->vms, args->vm_id);
	if (!vm) {
		ret = -EINVAL;
		goto out;
	}

	bind_ops = pancsf_get_obj_array(&args->ops, DRM_PANCSF_VM_BIND_OP_MIN_SIZE, sizeof(*bind_ops));
	sync_ops = pancsf_get_obj_array(&args->syncs, DRM_PANCSF_SYNC_OP_MIN_SIZE, sizeof(sync_ops));
	if (!bind_ops || !sync_ops) {
		ret = -ENOMEM;
		goto out;
	}

	ret = pancsf_collect_sync_signal_array(file, sync_ops, args->syncs.count, &sync_signal_array);
	if (ret)
		goto out;

	job = pancsf_vm_bind_job_create(file, queue, vm, bind_ops, args->ops.count);
	if (IS_ERR(job)) {
		ret = PTR_ERR(job);
		goto out;
	}

	ret = pancsf_vm_bind_job_add_deps(file, job, sync_ops, args->syncs.count);
	if (ret)
		goto out;

	ret = pancsf_vm_bind_job_push(job);
	if (ret)
		goto out;

	pancsf_attach_done_fence(file, pancsf_vm_bind_job_done_fence(job), &sync_signal_array);

out:
	if (ret && !IS_ERR_OR_NULL(job))
		pancsf_vm_bind_job_destroy(job);

	pancsf_free_sync_signal_array(&sync_signal_array);
	kvfree(sync_ops);
	kvfree(bind_ops);
	pancsf_vm_put(vm);
	pancsf_vm_bind_queue_put(queue);
	return ret;
}

static int
pancsf_open(struct drm_device *ddev, struct drm_file *file)
{
	int ret;
	struct pancsf_device *pfdev = ddev->dev_private;
	struct pancsf_file *pfile;

	pfile = kzalloc(sizeof(*pfile), GFP_KERNEL);
	if (!pfile)
		return -ENOMEM;

	/* Heap pool is created on-demand, we just init the lock to serialize
	 * the pool creation/destruction here.
	 */
	mutex_init(&pfile->heaps_lock);

	pfile->pfdev = pfdev;

	ret = pancsf_vm_pool_create(pfile);
	if (ret)
		goto err_destroy_heaps_lock;

	ret = pancsf_group_pool_create(pfile);
	if (ret)
		goto err_destroy_vm_pool;

	file->driver_priv = pfile;
	return 0;

err_destroy_vm_pool:
	pancsf_vm_pool_destroy(pfile);

err_destroy_heaps_lock:
	mutex_destroy(&pfile->heaps_lock);
	kfree(pfile);
	return ret;
}

static void
pancsf_postclose(struct drm_device *ddev, struct drm_file *file)
{
	struct pancsf_file *pfile = file->driver_priv;

	pancsf_group_pool_destroy(pfile);

	mutex_lock(&pfile->heaps_lock);
	pancsf_heap_pool_destroy(pfile->heaps);
	mutex_unlock(&pfile->heaps_lock);
	mutex_destroy(&pfile->heaps_lock);

	pancsf_vm_pool_destroy(pfile);

	kfree(pfile);
}

static const struct drm_ioctl_desc pancsf_drm_driver_ioctls[] = {
#define PANCSF_IOCTL(n, func, flags) \
	DRM_IOCTL_DEF_DRV(PANCSF_##n, pancsf_ioctl_##func, flags)

	PANCSF_IOCTL(DEV_QUERY, dev_query, DRM_RENDER_ALLOW),
	PANCSF_IOCTL(VM_CREATE, vm_create, DRM_RENDER_ALLOW),
	PANCSF_IOCTL(VM_DESTROY, vm_destroy, DRM_RENDER_ALLOW),
	PANCSF_IOCTL(BO_CREATE, bo_create, DRM_RENDER_ALLOW),
	PANCSF_IOCTL(BO_MMAP_OFFSET, bo_mmap_offset, DRM_RENDER_ALLOW),
	PANCSF_IOCTL(VM_MAP, vm_map, DRM_RENDER_ALLOW),
	PANCSF_IOCTL(VM_UNMAP, vm_unmap, DRM_RENDER_ALLOW),
	PANCSF_IOCTL(GROUP_CREATE, group_create, DRM_RENDER_ALLOW),
	PANCSF_IOCTL(GROUP_DESTROY, group_destroy, DRM_RENDER_ALLOW),
	PANCSF_IOCTL(GROUP_GET_STATE, group_get_state, DRM_RENDER_ALLOW),
	PANCSF_IOCTL(TILER_HEAP_CREATE, tiler_heap_create, DRM_RENDER_ALLOW),
	PANCSF_IOCTL(TILER_HEAP_DESTROY, tiler_heap_destroy, DRM_RENDER_ALLOW),
	PANCSF_IOCTL(GROUP_SUBMIT, group_submit, DRM_RENDER_ALLOW),
	PANCSF_IOCTL(VM_BIND_QUEUE_CREATE, vm_bind_queue_create, DRM_RENDER_ALLOW),
	PANCSF_IOCTL(VM_BIND_QUEUE_DESTROY, vm_bind_queue_destroy, DRM_RENDER_ALLOW),
	PANCSF_IOCTL(VM_BIND_SUBMIT, vm_bind_submit, DRM_RENDER_ALLOW),
};

static int pancsf_mmap_io(struct file *filp, struct vm_area_struct *vma)
{
	struct drm_file *priv = filp->private_data;
	struct pancsf_file *pfile = priv->driver_priv;
	struct pancsf_device *pfdev = pfile->pfdev;
	u64 offset = vma->vm_pgoff << PAGE_SHIFT;
	phys_addr_t phys_offset;
	size_t size;

	switch (offset) {
	case DRM_PANCSF_USER_FLUSH_ID_MMIO_OFFSET:
		if (vma->vm_flags & VM_WRITE)
			return -EINVAL;

		size = PAGE_SIZE;
		phys_offset = CSF_GPU_LATEST_FLUSH_ID;
		break;
	default:
		return -EINVAL;
	}

	if (vma->vm_end - vma->vm_start != size)
		return -EINVAL;

	vma->vm_flags |= VM_IO | VM_DONTCOPY | VM_DONTEXPAND | VM_NORESERVE |
			 VM_DONTDUMP | VM_PFNMAP;

	vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);

	return io_remap_pfn_range(vma,
				  vma->vm_start,
				  (pfdev->phys_addr + phys_offset) >> PAGE_SHIFT,
				  size,
				  vma->vm_page_prot);
}

static int pancsf_mmap(struct file *filp, struct vm_area_struct *vma)
{
	if (vma->vm_pgoff >= (DRM_PANCSF_USER_MMIO_OFFSET >> PAGE_SHIFT))
		return pancsf_mmap_io(filp, vma);

	return drm_gem_mmap(filp, vma);
}

static const struct file_operations pancsf_drm_driver_fops = {
	.open = drm_open,
	.release = drm_release,
	.unlocked_ioctl = drm_ioctl,
	.compat_ioctl = drm_compat_ioctl,
	.poll = drm_poll,
	.read = drm_read,
	.llseek = noop_llseek,
	.mmap = pancsf_mmap,
};

/*
 * PanCSF driver version:
 * - 1.0 - initial interface
 */
static const struct drm_driver pancsf_drm_driver = {
	.driver_features	= DRIVER_RENDER | DRIVER_GEM | DRIVER_SYNCOBJ |
				  DRIVER_SYNCOBJ_TIMELINE,
	.open			= pancsf_open,
	.postclose		= pancsf_postclose,
	.ioctls			= pancsf_drm_driver_ioctls,
	.num_ioctls		= ARRAY_SIZE(pancsf_drm_driver_ioctls),
	.fops			= &pancsf_drm_driver_fops,
	.name			= "pancsf",
	.desc			= "pancsf DRM",
	.date			= "20230120",
	.major			= 1,
	.minor			= 0,

	.gem_create_object	= pancsf_gem_create_object,
	.prime_handle_to_fd	= pancsf_gem_prime_handle_to_fd,
	.prime_fd_to_handle	= drm_gem_prime_fd_to_handle,
	.gem_prime_import_sg_table = pancsf_gem_prime_import_sg_table,
	.gem_prime_mmap		= drm_gem_prime_mmap,
};

static int pancsf_probe(struct platform_device *pdev)
{
	struct pancsf_device *pfdev;
	struct drm_device *ddev;
	int err;

	pfdev = devm_kzalloc(&pdev->dev, sizeof(*pfdev), GFP_KERNEL);
	if (!pfdev)
		return -ENOMEM;

	pfdev->pdev = pdev;
	pfdev->dev = &pdev->dev;

	platform_set_drvdata(pdev, pfdev);

	pfdev->comp = of_device_get_match_data(&pdev->dev);
	if (!pfdev->comp)
		return -ENODEV;

	pfdev->coherent = device_get_dma_attr(&pdev->dev) == DEV_DMA_COHERENT;

	/* Allocate and initialize the DRM device. */
	ddev = drm_dev_alloc(&pancsf_drm_driver, &pdev->dev);
	if (IS_ERR(ddev))
		return PTR_ERR(ddev);

	ddev->dev_private = pfdev;
	pfdev->ddev = ddev;

	err = pancsf_device_init(pfdev);
	if (err) {
		if (err != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Fatal error during GPU init\n");
		goto err_out0;
	}

	pm_runtime_set_active(pfdev->dev);
	pm_runtime_mark_last_busy(pfdev->dev);
	pm_runtime_enable(pfdev->dev);
	pm_runtime_set_autosuspend_delay(pfdev->dev, 50); /* ~3 frames */
	pm_runtime_use_autosuspend(pfdev->dev);

	/*
	 * Register the DRM device with the core and the connectors with
	 * sysfs
	 */
	err = drm_dev_register(ddev, 0);
	if (err < 0)
		goto err_out1;

	return 0;

err_out1:
	pm_runtime_disable(pfdev->dev);
	pancsf_device_fini(pfdev);
	pm_runtime_set_suspended(pfdev->dev);
err_out0:
	drm_dev_put(ddev);
	return err;
}

static int pancsf_remove(struct platform_device *pdev)
{
	struct pancsf_device *pfdev = platform_get_drvdata(pdev);
	struct drm_device *ddev = pfdev->ddev;

	drm_dev_unregister(ddev);

	pm_runtime_get_sync(pfdev->dev);
	pm_runtime_disable(pfdev->dev);
	pancsf_device_fini(pfdev);
	pm_runtime_set_suspended(pfdev->dev);

	drm_dev_put(ddev);
	return 0;
}

/*
 * The OPP core wants the supply names to be NULL terminated, but we need the
 * correct num_supplies value for regulator core. Hence, we NULL terminate here
 * and then initialize num_supplies with ARRAY_SIZE - 1.
 */
static const char * const rockchip_rk3588_supplies[] = { "mali", "sram", NULL };
static const char * const rockchip_rk3588_clks[] = { "coregroup", "stacks" };
static const struct pancsf_compatible rockchip_rk3588_data = {
	.num_supplies = ARRAY_SIZE(rockchip_rk3588_supplies) - 1,
	.supply_names = rockchip_rk3588_supplies,
	.num_pm_domains = 1,
	.pm_domain_names = NULL,
	.num_clks = ARRAY_SIZE(rockchip_rk3588_clks),
	.clk_names = rockchip_rk3588_clks,
};

static const struct of_device_id dt_match[] = {
	{ .compatible = "rockchip,rk3588-mali", .data = &rockchip_rk3588_data },
	{}
};
MODULE_DEVICE_TABLE(of, dt_match);

static const struct dev_pm_ops pancsf_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend, pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(pancsf_device_suspend, pancsf_device_resume, NULL)
};

static struct platform_driver pancsf_driver = {
	.probe		= pancsf_probe,
	.remove		= pancsf_remove,
	.driver		= {
		.name	= "pancsf",
		.pm	= &pancsf_pm_ops,
		.of_match_table = dt_match,
	},
};
module_platform_driver(pancsf_driver);

MODULE_AUTHOR("Panfrost Project Developers");
MODULE_DESCRIPTION("Panfrost CSF DRM Driver");
MODULE_LICENSE("GPL v2");
