// SPDX-License-Identifier: GPL-2.0
/* Copyright 2023 Collabora ltd. */

#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/iopoll.h>
#include <linux/iosys-map.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>

#include "pancsf_device.h"
#include "pancsf_gem.h"
#include "pancsf_gpu.h"
#include "pancsf_regs.h"
#include "pancsf_mcu.h"
#include "pancsf_mmu.h"
#include "pancsf_sched.h"

#define CSF_FW_NAME "mali_csffw.bin"

struct pancsf_fw_mem {
	struct drm_mm_node mm_node;
	u32 num_pages;
	struct page **pages;
	struct sg_table sgt;
	void *kmap;
};

struct pancsf_fw_hdr {
	u32 magic;
	u8 minor;
	u8 major;
	u16 padding1;
	u32 version_hash;
	u32 padding2;
	u32 size;
};

enum pancsf_fw_entry_type {
	CSF_FW_ENTRY_TYPE_IFACE = 0,
	CSF_FW_ENTRY_TYPE_CONFIG = 1,
	CSF_FW_ENTRY_TYPE_FUTF_TEST = 2,
	CSF_FW_ENTRY_TYPE_TRACE_BUFFER = 3,
	CSF_FW_ENTRY_TYPE_TIMELINE_METADATA = 4,
};

#define CSF_FW_ENTRY_TYPE(ehdr)			((ehdr) & 0xff)
#define CSF_FW_ENTRY_SIZE(ehdr)			(((ehdr) >> 8) & 0xff)
#define CSF_FW_ENTRY_UPDATE			BIT(30)
#define CSF_FW_ENTRY_OPTIONAL			BIT(31)

#define CSF_FW_IFACE_ENTRY_RD			BIT(0)
#define CSF_FW_IFACE_ENTRY_WR			BIT(1)
#define CSF_FW_IFACE_ENTRY_EX			BIT(2)
#define CSF_FW_IFACE_ENTRY_CACHE_MODE_NONE	(0 << 3)
#define CSF_FW_IFACE_ENTRY_CACHE_MODE_CACHED	(1 << 3)
#define CSF_FW_IFACE_ENTRY_CACHE_MODE_UNCACHED_COHERENT (2 << 3)
#define CSF_FW_IFACE_ENTRY_CACHE_MODE_CACHED_COHERENT (3 << 3)
#define CSF_FW_IFACE_ENTRY_CACHE_MODE_MASK	GENMASK(4, 3)
#define CSF_FW_IFACE_ENTRY_PROT			BIT(5)
#define CSF_FW_IFACE_ENTRY_SHARED		BIT(30)
#define CSF_FW_IFACE_ENTRY_ZERO			BIT(31)

#define CSF_FW_IFACE_ENTRY_SUPPORTED_FLAGS      \
	(CSF_FW_IFACE_ENTRY_RD |		\
	 CSF_FW_IFACE_ENTRY_WR |		\
	 CSF_FW_IFACE_ENTRY_EX |		\
	 CSF_FW_IFACE_ENTRY_CACHE_MODE_MASK |	\
	 CSF_FW_IFACE_ENTRY_PROT |		\
	 CSF_FW_IFACE_ENTRY_SHARED  |		\
	 CSF_FW_IFACE_ENTRY_ZERO)

struct pancsf_fw_section_entry_hdr {
	u32 flags;
	struct {
		u32 start;
		u32 end;
	} va;
	struct {
		u32 start;
		u32 end;
	} data;
};

struct pancsf_fw_iter {
	const void *data;
	size_t size;
	size_t offset;
};

struct pancsf_fw_section {
	struct list_head node;
	u32 flags;
	struct pancsf_fw_mem *mem;
	const char *name;
	/* Keep data around so we can reload writeable sections after an MCU
	 * reset.
	 */
	struct {
		const void *buf;
		size_t size;
	} data;
};

#define CSF_MCU_SHARED_REGION_START		0x04000000ULL
#define CSF_MCU_SHARED_REGION_END		0x08000000ULL

#define CSF_FW_HEADER_MAGIC			0xc3f13a6e
#define CSF_FW_HEADER_MAJOR_MAX			0

#define MIN_CS_PER_CSG				8
#define MIN_CSGS				3
#define MAX_CSG_PRIO				0xf

#define CSF_IFACE_VERSION(major, minor, patch)	\
	(((major) << 24) | ((minor) << 16) | (patch))
#define CSF_IFACE_VERSION_MAJOR(v)		((v) >> 24)
#define CSF_IFACE_VERSION_MINOR(v)		(((v) >> 16) & 0xff)
#define CSF_IFACE_VERSION_PATCH(v)		((v) & 0xffff)

#define CSF_GROUP_CONTROL_OFFSET		0x1000
#define CSF_STREAM_CONTROL_OFFSET		0x40
#define CSF_UNPRESERVED_REG_COUNT		4

struct pancsf_mcu {
	struct pancsf_vm *vm;
	int as;

	struct list_head sections;
	struct pancsf_fw_section *shared_section;
	struct pancsf_fw_iface iface;

	bool booted;
	wait_queue_head_t booted_event;

	int job_irq;
};

static irqreturn_t pancsf_job_irq_handler(int irq, void *data)
{
	struct pancsf_device *pfdev = data;
	irqreturn_t ret = IRQ_NONE;

	while (true) {
		u32 status = gpu_read(pfdev, JOB_INT_STAT);

		if (!status)
			break;

		gpu_write(pfdev, JOB_INT_CLEAR, status);

		if (!pfdev->mcu->booted) {
			if (status & JOB_INT_GLOBAL_IF) {
				pfdev->mcu->booted = true;
				wake_up_all(&pfdev->mcu->booted_event);
			}

			return IRQ_HANDLED;
		}

		pancsf_sched_handle_job_irqs(pfdev, status);
		ret = IRQ_HANDLED;
	}

	return ret;
}

static int pancsf_fw_iter_read(struct pancsf_device *pfdev,
			       struct pancsf_fw_iter *iter,
				     void *out, size_t size)
{
	size_t new_offset = iter->offset + size;

	if (new_offset > iter->size || new_offset < iter->offset) {
		dev_err(pfdev->dev, "Firmware too small\n");
		return -EINVAL;
	}

	memcpy(out, iter->data + iter->offset, size);
	iter->offset = new_offset;
	return 0;
}

static void pancsf_fw_init_section_mem(struct pancsf_device *pfdev,
				       struct pancsf_fw_section *section)
{
	size_t data_len = section->data.size;
	size_t data_offs = 0;
	u32 page;

	for (page = 0; page < section->mem->num_pages; page++) {
		void *mem = kmap_local_page(section->mem->pages[page]);
		u32 copy_len = min_t(u32, PAGE_SIZE, data_len);

		memcpy(mem, section->data.buf + data_offs, copy_len);
		data_len -= copy_len;
		data_offs += copy_len;

		if (section->flags & CSF_FW_IFACE_ENTRY_ZERO)
			memset(mem + copy_len, 0, PAGE_SIZE - copy_len);

		kunmap_local(mem);
	}
}

u64 pancsf_fw_mem_va(struct pancsf_fw_mem *mem)
{
	return mem->mm_node.start << PAGE_SHIFT;
}

void pancsf_fw_mem_vunmap(struct pancsf_fw_mem *mem)
{
	if (mem->kmap)
		vunmap(mem->kmap);
}

void *pancsf_fw_mem_vmap(struct pancsf_fw_mem *mem, pgprot_t prot)
{
	if (!mem->kmap)
		mem->kmap = vmap(mem->pages, mem->num_pages, VM_MAP, prot);

	return mem->kmap;
}

void pancsf_fw_mem_free(struct pancsf_device *pfdev, struct pancsf_fw_mem *mem)
{
	unsigned int i;

	if (IS_ERR_OR_NULL(mem))
		return;

	pancsf_fw_mem_vunmap(mem);

	if (drm_mm_node_allocated(&mem->mm_node))
		pancsf_vm_unmap_mcu_pages(pfdev->mcu->vm, &mem->mm_node);

	dma_unmap_sgtable(pfdev->dev, &mem->sgt, DMA_BIDIRECTIONAL, 0);
	sg_free_table(&mem->sgt);

	for (i = 0; i < mem->num_pages; i++)
		__free_page(mem->pages[i]);

	kfree(mem->pages);
	kfree(mem);
}

struct pancsf_fw_mem *
pancsf_fw_mem_alloc(struct pancsf_device *pfdev,
		    unsigned int num_pages,
		    u32 mcu_va_start, u32 mcu_va_end,
		    int prot)
{
	struct pancsf_fw_mem *mem = kzalloc(sizeof(*mem), GFP_KERNEL);
	int ret;

	if (!mem)
		return ERR_PTR(-ENOMEM);

	mem->pages = kcalloc(num_pages, sizeof(*mem->pages), GFP_KERNEL);
	if (!mem->pages) {
		ret = -ENOMEM;
		goto err_free_mem;
	}

	mem->num_pages = alloc_pages_bulk_array(GFP_KERNEL, num_pages, mem->pages);
	if (num_pages != mem->num_pages) {
		ret = -ENOMEM;
		goto err_free_mem;
	}

	ret = sg_alloc_table_from_pages(&mem->sgt, mem->pages, num_pages,
					0, num_pages << PAGE_SHIFT, GFP_KERNEL);
	if (ret) {
		ret = -ENOMEM;
		goto err_free_mem;
	}

	ret = dma_map_sgtable(pfdev->dev, &mem->sgt, DMA_BIDIRECTIONAL, 0);
	if (ret)
		goto err_free_mem;

	ret = pancsf_vm_map_mcu_pages(pfdev->mcu->vm, &mem->mm_node,
				      &mem->sgt, num_pages,
				      mcu_va_start, mcu_va_end,
				      prot);
	if (ret)
		goto err_free_mem;

	return mem;

err_free_mem:
	pancsf_fw_mem_free(pfdev, mem);
	return ERR_PTR(ret);
}

struct pancsf_fw_mem *pancsf_fw_alloc_queue_iface_mem(struct pancsf_device *pfdev)
{
	return pancsf_fw_mem_alloc(pfdev, 2,
				   CSF_MCU_SHARED_REGION_START,
				   CSF_MCU_SHARED_REGION_END,
				   IOMMU_READ | IOMMU_WRITE);
}

struct pancsf_fw_mem *
pancsf_fw_alloc_suspend_buf_mem(struct pancsf_device *pfdev, size_t size)
{
	size_t page_count = DIV_ROUND_UP(size, PAGE_SIZE);

	if (!page_count)
		return NULL;

	return pancsf_fw_mem_alloc(pfdev, page_count,
				   CSF_MCU_SHARED_REGION_START,
				   CSF_MCU_SHARED_REGION_END,
				   IOMMU_READ | IOMMU_WRITE |
				   IOMMU_NOEXEC | IOMMU_CACHE);
}

static int pancsf_fw_load_section_entry(struct pancsf_device *pfdev,
					const struct firmware *fw,
					struct pancsf_fw_iter *iter,
					u32 ehdr)
{
	struct pancsf_fw_section_entry_hdr hdr;
	struct pancsf_fw_section *section;
	u32 name_len, num_pages;
	int ret;

	ret = pancsf_fw_iter_read(pfdev, iter, &hdr, sizeof(hdr));
	if (ret)
		return ret;

	if (hdr.data.end < hdr.data.start) {
		dev_err(pfdev->dev, "Firmware corrupted, data.end < data.start (0x%x < 0x%x)\n",
			hdr.data.end, hdr.data.start);
		return -EINVAL;
	}

	if (hdr.va.end < hdr.va.start) {
		dev_err(pfdev->dev, "Firmware corrupted, hdr.va.end < hdr.va.start (0x%x < 0x%x)\n",
			hdr.va.end, hdr.va.start);
		return -EINVAL;
	}

	if (hdr.data.end > fw->size) {
		dev_err(pfdev->dev, "Firmware corrupted, file truncated? data_end=0x%x > fw size=0x%zx\n",
			hdr.data.end, fw->size);
		return -EINVAL;
	}

	if ((hdr.va.start & ~PAGE_MASK) != 0 ||
	    (hdr.va.end & ~PAGE_MASK) != 0) {
		dev_err(pfdev->dev, "Firmware corrupted, virtual addresses not page aligned: 0x%x-0x%x\n",
			hdr.va.start, hdr.va.end);
		return -EINVAL;
	}

	if (hdr.flags & ~CSF_FW_IFACE_ENTRY_SUPPORTED_FLAGS) {
		dev_err(pfdev->dev, "Firmware contains interface with unsupported flags (0x%x)\n",
			hdr.flags);
		return -EINVAL;
	}

	if (hdr.flags & CSF_FW_IFACE_ENTRY_PROT) {
		dev_warn(pfdev->dev,
			 "Firmware protected mode entry not be supported, ignoring");
		return 0;
	}

	if (hdr.va.start == CSF_MCU_SHARED_REGION_START &&
	    !(hdr.flags & CSF_FW_IFACE_ENTRY_SHARED)) {
		dev_err(pfdev->dev,
			"Interface at 0x%llx must be shared", CSF_MCU_SHARED_REGION_START);
		return -EINVAL;
	}

	name_len = iter->size - iter->offset;

	section = devm_kzalloc(pfdev->dev, sizeof(*section), GFP_KERNEL);
	if (!section)
		return -ENOMEM;

	section->flags = hdr.flags;
	section->data.size = hdr.data.end - hdr.data.start;

	if (section->data.size > 0) {
		void *data = devm_kmalloc(pfdev->dev, section->data.size, GFP_KERNEL);

		if (!data)
			return -ENOMEM;

		memcpy(data, fw->data + hdr.data.start, section->data.size);
		section->data.buf = data;
	}

	if (name_len > 0) {
		char *name = devm_kmalloc(pfdev->dev, name_len + 1, GFP_KERNEL);

		if (!name)
			return -ENOMEM;

		memcpy(name, iter->data + iter->offset, name_len);
		name[name_len] = '\0';
		section->name = name;
	}

	num_pages = (hdr.va.end - hdr.va.start) >> PAGE_SHIFT;
	if (num_pages > 0) {
		u32 cache_mode = hdr.flags & CSF_FW_IFACE_ENTRY_CACHE_MODE_MASK;
		int prot = 0;

		if (hdr.flags & CSF_FW_IFACE_ENTRY_RD)
			prot |= IOMMU_READ;

		if (hdr.flags & CSF_FW_IFACE_ENTRY_WR)
			prot |= IOMMU_WRITE;

		if (!(hdr.flags & CSF_FW_IFACE_ENTRY_EX))
			prot |= IOMMU_NOEXEC;

		/* TODO: CSF_FW_IFACE_ENTRY_CACHE_MODE_*_COHERENT are mapped to
		 * non-cacheable for now. We might want to introduce a new
		 * IOMMU_xxx flag (or abuse IOMMU_MMIO, which maps to device
		 * memory and is currently not used by our driver) for
		 * AS_MEMATTR_AARCH64_SHARED memory, so we can take benefit
		 * from IO-coherent systems.
		 */
		if (cache_mode == CSF_FW_IFACE_ENTRY_CACHE_MODE_CACHED)
			prot |= IOMMU_CACHE;

		section->mem = pancsf_fw_mem_alloc(pfdev, num_pages,
						   hdr.va.start, hdr.va.end, prot);
		if (IS_ERR(section->mem))
			return PTR_ERR(section->mem);

		pancsf_fw_init_section_mem(pfdev, section);

		dma_sync_sgtable_for_device(pfdev->dev, &section->mem->sgt, DMA_TO_DEVICE);

		if (section->flags & CSF_FW_IFACE_ENTRY_SHARED) {
			pgprot_t kmap_prot = PAGE_KERNEL;

			if (cache_mode != CSF_FW_IFACE_ENTRY_CACHE_MODE_CACHED)
				kmap_prot = pgprot_writecombine(kmap_prot);

			if (!pancsf_fw_mem_vmap(section->mem, kmap_prot))
				return -ENOMEM;
		}
	}

	if (hdr.va.start == CSF_MCU_SHARED_REGION_START)
		pfdev->mcu->shared_section = section;

	list_add_tail(&section->node, &pfdev->mcu->sections);
	return 0;
}

static void
pancsf_reload_fw_sections(struct pancsf_device *pfdev, bool full_reload)
{
	struct pancsf_fw_section *section;

	list_for_each_entry(section, &pfdev->mcu->sections, node) {
		if (!full_reload && !(section->flags & CSF_FW_IFACE_ENTRY_WR))
			continue;

		pancsf_fw_init_section_mem(pfdev, section);
		dma_sync_sgtable_for_device(pfdev->dev, &section->mem->sgt, DMA_TO_DEVICE);
	}
}

static int pancsf_fw_load_entry(struct pancsf_device *pfdev,
				const struct firmware *fw,
				struct pancsf_fw_iter *iter)
{
	struct pancsf_fw_iter eiter;
	u32 ehdr;
	int ret;

	ret = pancsf_fw_iter_read(pfdev, iter, &ehdr, sizeof(ehdr));
	if (ret)
		return ret;

	if ((iter->offset % sizeof(u32)) ||
	    (CSF_FW_ENTRY_SIZE(ehdr) % sizeof(u32))) {
		dev_err(pfdev->dev, "Firmware entry isn't 32 bit aligned, offset=0x%x size=0x%x\n",
			(u32)(iter->offset - sizeof(u32)), CSF_FW_ENTRY_SIZE(ehdr));
		return -EINVAL;
	}

	eiter.offset = 0;
	eiter.data = iter->data + iter->offset;
	eiter.size = CSF_FW_ENTRY_SIZE(ehdr) - sizeof(ehdr);
	iter->offset += eiter.size;

	switch (CSF_FW_ENTRY_TYPE(ehdr)) {
	case CSF_FW_ENTRY_TYPE_IFACE:
		return pancsf_fw_load_section_entry(pfdev, fw, &eiter, ehdr);

	/* FIXME: handle those entry types? */
	case CSF_FW_ENTRY_TYPE_CONFIG:
	case CSF_FW_ENTRY_TYPE_FUTF_TEST:
	case CSF_FW_ENTRY_TYPE_TRACE_BUFFER:
	case CSF_FW_ENTRY_TYPE_TIMELINE_METADATA:
		return 0;
	default:
		break;
	}

	if (ehdr & CSF_FW_ENTRY_OPTIONAL)
		return 0;

	dev_err(pfdev->dev,
		"Unsupported non-optional entry type %u in firmware\n",
		CSF_FW_ENTRY_TYPE(ehdr));
	return -EINVAL;
}

static int pancsf_fw_init(struct pancsf_device *pfdev)
{
	const struct firmware *fw = NULL;
	struct pancsf_fw_iter iter = {};
	struct pancsf_fw_hdr hdr;
	int ret;

	ret = request_firmware(&fw, CSF_FW_NAME, pfdev->dev);
	if (ret) {
		dev_err(pfdev->dev, "Failed to load firmware image '%s'\n",
			CSF_FW_NAME);
		return ret;
	}

	iter.data = fw->data;
	iter.size = fw->size;
	ret = pancsf_fw_iter_read(pfdev, &iter, &hdr, sizeof(hdr));
	if (ret)
		goto out;

	if (hdr.magic != CSF_FW_HEADER_MAGIC) {
		ret = -EINVAL;
		dev_err(pfdev->dev, "Invalid firmware magic\n");
		goto out;
	}

	if (hdr.major != CSF_FW_HEADER_MAJOR_MAX) {
		ret = -EINVAL;
		dev_err(pfdev->dev, "Unsupported firmware header version %d.%d (expected %d.x)\n",
			hdr.major, hdr.minor, CSF_FW_HEADER_MAJOR_MAX);
		goto out;
	}

	if (hdr.size > iter.size) {
		dev_err(pfdev->dev, "Firmware image is truncated\n");
		goto out;
	}

	iter.size = hdr.size;

	while (iter.offset < hdr.size) {
		ret = pancsf_fw_load_entry(pfdev, fw, &iter);
		if (ret)
			goto out;
	}

	if (!pfdev->mcu->shared_section) {
		dev_err(pfdev->dev, "Shared interface region not found\n");
		ret = -EINVAL;
		goto out;
	}

out:
	release_firmware(fw);
	return ret;
}

static void *pancsf_mcu_to_cpu_addr(struct pancsf_device *pfdev, u32 mcu_va)
{
	u64 shared_mem_start = pfdev->mcu->shared_section->mem->mm_node.start << PAGE_SHIFT;
	u64 shared_mem_end = (pfdev->mcu->shared_section->mem->mm_node.start +
			      pfdev->mcu->shared_section->mem->mm_node.size) << PAGE_SHIFT;
	if (mcu_va < shared_mem_start || mcu_va >= shared_mem_end)
		return NULL;

	return pfdev->mcu->shared_section->mem->kmap + (mcu_va - shared_mem_start);
}

static int pancsf_init_cs_iface(struct pancsf_device *pfdev,
				unsigned int csg_idx, unsigned int cs_idx)
{
	const struct pancsf_fw_global_iface *glb_iface = pancsf_get_glb_iface(pfdev);
	const struct pancsf_fw_csg_iface *csg_iface = pancsf_get_csg_iface(pfdev, csg_idx);
	struct pancsf_fw_cs_iface *cs_iface = &pfdev->mcu->iface.groups[csg_idx].streams[cs_idx];
	u64 shared_section_sz = pfdev->mcu->shared_section->mem->mm_node.size << PAGE_SHIFT;
	u32 iface_offset = CSF_GROUP_CONTROL_OFFSET +
			   (csg_idx * glb_iface->control->group_stride) +
			   CSF_STREAM_CONTROL_OFFSET +
			   (cs_idx * csg_iface->control->stream_stride);

	if (iface_offset + sizeof(*cs_iface) >= shared_section_sz)
		return -EINVAL;

	cs_iface->control = pfdev->mcu->shared_section->mem->kmap + iface_offset;
	cs_iface->input = pancsf_mcu_to_cpu_addr(pfdev, cs_iface->control->input_va);
	cs_iface->output = pancsf_mcu_to_cpu_addr(pfdev, cs_iface->control->output_va);

	if (!cs_iface->input || !cs_iface->output) {
		dev_err(pfdev->dev, "Invalid stream control interface input/output VA");
		return -EINVAL;
	}

	if (csg_idx > 0 || cs_idx > 0) {
		const struct pancsf_fw_cs_iface *first_cs_iface = pancsf_get_cs_iface(pfdev, 0, 0);

		if (cs_iface->control->features != first_cs_iface->control->features) {
			dev_err(pfdev->dev, "Expecting identical CS slots");
			return -EINVAL;
		}
	} else {
		u32 reg_count = CS_FEATURES_WORK_REGS(cs_iface->control->features);

		pfdev->csif_info.cs_reg_count = reg_count;
		pfdev->csif_info.unpreserved_cs_reg_count = CSF_UNPRESERVED_REG_COUNT;
	}

	return 0;
}

static int pancsf_init_csg_iface(struct pancsf_device *pfdev,
				 unsigned int csg_idx)
{
	const struct pancsf_fw_global_iface *glb_iface = pancsf_get_glb_iface(pfdev);
	struct pancsf_fw_csg_iface *csg_iface = &pfdev->mcu->iface.groups[csg_idx];
	u64 shared_section_sz = pfdev->mcu->shared_section->mem->mm_node.size << PAGE_SHIFT;
	u32 iface_offset = CSF_GROUP_CONTROL_OFFSET + (csg_idx * glb_iface->control->group_stride);
	unsigned int i;

	if (iface_offset + sizeof(*csg_iface) >= shared_section_sz)
		return -EINVAL;

	csg_iface->control = pfdev->mcu->shared_section->mem->kmap + iface_offset;
	csg_iface->input = pancsf_mcu_to_cpu_addr(pfdev, csg_iface->control->input_va);
	csg_iface->output = pancsf_mcu_to_cpu_addr(pfdev, csg_iface->control->output_va);

	if (csg_iface->control->stream_num < MIN_CS_PER_CSG ||
	    csg_iface->control->stream_num > MAX_CS_PER_CSG)
		return -EINVAL;

	if (!csg_iface->input || !csg_iface->output) {
		dev_err(pfdev->dev, "Invalid group control interface input/output VA");
		return -EINVAL;
	}

	if (csg_idx > 0) {
		const struct pancsf_fw_csg_iface *first_csg_iface = pancsf_get_csg_iface(pfdev, 0);
		u32 first_protm_suspend_size = first_csg_iface->control->protm_suspend_size;

		if (first_csg_iface->control->features != csg_iface->control->features ||
		    first_csg_iface->control->suspend_size != csg_iface->control->suspend_size ||
		    first_protm_suspend_size != csg_iface->control->protm_suspend_size ||
		    first_csg_iface->control->stream_num != csg_iface->control->stream_num) {
			dev_err(pfdev->dev, "Expecting identical CSG slots");
			return -EINVAL;
		}
	}

	for (i = 0; i < csg_iface->control->stream_num; i++) {
		int ret = pancsf_init_cs_iface(pfdev, csg_idx, i);

		if (ret)
			return ret;
	}

	return 0;
}

static u32 pancsf_get_instr_features(struct pancsf_device *pfdev)
{
	const struct pancsf_fw_global_iface *glb_iface = pancsf_get_glb_iface(pfdev);

	if (glb_iface->control->version < CSF_IFACE_VERSION(1, 1, 0))
		return 0;

	return glb_iface->control->instr_features;
}

static int pancsf_init_ifaces(struct pancsf_device *pfdev)
{
	struct pancsf_fw_global_iface *glb_iface;
	unsigned int i;

	if (!pfdev->mcu->shared_section->mem->kmap)
		return -EINVAL;

	pfdev->iface = &pfdev->mcu->iface;
	glb_iface = pancsf_get_glb_iface(pfdev);
	glb_iface->control = pfdev->mcu->shared_section->mem->kmap;

	if (!glb_iface->control->version) {
		dev_err(pfdev->dev, "Invalid CSF interface version %d.%d.%d (%x)",
			CSF_IFACE_VERSION_MAJOR(glb_iface->control->version),
			CSF_IFACE_VERSION_MINOR(glb_iface->control->version),
			CSF_IFACE_VERSION_PATCH(glb_iface->control->version),
			glb_iface->control->version);
		return -EINVAL;
	}

	glb_iface->input = pancsf_mcu_to_cpu_addr(pfdev, glb_iface->control->input_va);
	glb_iface->output = pancsf_mcu_to_cpu_addr(pfdev, glb_iface->control->output_va);
	if (!glb_iface->input || !glb_iface->output) {
		dev_err(pfdev->dev, "Invalid global control interface input/output VA");
		return -EINVAL;
	}

	if (glb_iface->control->group_num > MAX_CSGS ||
	    glb_iface->control->group_num < MIN_CSGS) {
		dev_err(pfdev->dev, "Invalid number of control groups");
		return -EINVAL;
	}

	for (i = 0; i < glb_iface->control->group_num; i++) {
		int ret = pancsf_init_csg_iface(pfdev, i);

		if (ret)
			return ret;
	}

	pfdev->iface = &pfdev->mcu->iface;
	dev_info(pfdev->dev, "CSF FW v%d.%d.%d, Features %x Instrumentation features %x",
		 CSF_IFACE_VERSION_MAJOR(glb_iface->control->version),
		 CSF_IFACE_VERSION_MINOR(glb_iface->control->version),
		 CSF_IFACE_VERSION_PATCH(glb_iface->control->version),
		 glb_iface->control->features,
		 pancsf_get_instr_features(pfdev));
	return 0;
}

static int pancsf_mcu_start(struct pancsf_device *pfdev)
{
	bool timedout = false;

	pfdev->mcu->booted = false;
	gpu_write(pfdev, JOB_INT_CLEAR, ~0);
	gpu_write(pfdev, JOB_INT_MASK, ~0);
	gpu_write(pfdev, MCU_CONTROL, MCU_CONTROL_AUTO);

	if (!wait_event_timeout(pfdev->mcu->booted_event,
				pfdev->mcu->booted,
				msecs_to_jiffies(1000))) {
		if (!pfdev->mcu->booted &&
		    !(gpu_read(pfdev, JOB_INT_STAT) & JOB_INT_GLOBAL_IF))
			timedout = true;
	}

	if (timedout) {
		dev_err(pfdev->dev, "Failed to boot MCU");
		return -ETIMEDOUT;
	}

	return 0;
}

static void pancsf_mcu_stop(struct pancsf_device *pfdev)
{
	u32 status;

	gpu_write(pfdev, MCU_CONTROL, MCU_CONTROL_DISABLE);
	if (readl_poll_timeout(pfdev->iomem + MCU_CONTROL, status,
			       status == MCU_CONTROL_DISABLE, 10, 100000))
		dev_err(pfdev->dev, "Failed to stop MCU");
}

int pancsf_mcu_reset(struct pancsf_device *pfdev, bool full_fw_reload)
{
	pfdev->mcu->as = pancsf_vm_as_get(pfdev->mcu->vm);
	pancsf_reload_fw_sections(pfdev, full_fw_reload);

	return pancsf_mcu_start(pfdev);
}

int pancsf_mcu_init(struct pancsf_device *pfdev)
{
	struct pancsf_mcu *mcu;
	struct pancsf_fw_section *section;
	int ret, irq;

	mcu = devm_kzalloc(pfdev->dev, sizeof(*mcu), GFP_KERNEL);
	if (!mcu)
		return -ENOMEM;

	pfdev->mcu = mcu;
	init_waitqueue_head(&mcu->booted_event);
	INIT_LIST_HEAD(&pfdev->mcu->sections);
	mcu->as = -1;

	gpu_write(pfdev, JOB_INT_MASK, 0);

	irq = platform_get_irq_byname(to_platform_device(pfdev->dev), "job");
	if (irq <= 0)
		return -ENODEV;

	mcu->job_irq = irq;
	ret = devm_request_threaded_irq(pfdev->dev, irq,
					NULL, pancsf_job_irq_handler,
					IRQF_ONESHOT, KBUILD_MODNAME "-job",
					pfdev);
	if (ret) {
		dev_err(pfdev->dev, "failed to request job irq");
		return ret;
	}

	ret = pancsf_gpu_l2_power_on(pfdev);
	if (ret)
		return ret;

	mcu->vm = pancsf_vm_create(pfdev, true);
	if (IS_ERR(mcu->vm)) {
		ret = PTR_ERR(mcu->vm);
		mcu->vm = NULL;
		goto err_l2_pwroff;
	}

	ret = pancsf_fw_init(pfdev);
	if (ret)
		goto err_free_sections;

	mcu->as = pancsf_vm_as_get(mcu->vm);
	if (WARN_ON(mcu->as != 0)) {
		ret = -EINVAL;
		goto err_free_sections;
	}

	ret = pancsf_mcu_start(pfdev);
	if (ret)
		goto err_put_as;

	ret = pancsf_init_ifaces(pfdev);
	if (ret)
		goto err_stop_mcu;

	return 0;

err_stop_mcu:
	pancsf_mcu_stop(pfdev);

err_put_as:
	pancsf_vm_as_put(mcu->vm);

err_free_sections:
	list_for_each_entry(section, &pfdev->mcu->sections, node) {
		pancsf_fw_mem_free(pfdev, section->mem);
	}

err_l2_pwroff:
	pancsf_gpu_power_off(pfdev, L2,
			     pfdev->gpu_info.l2_present,
			     20000);

	return ret;
}

void pancsf_mcu_fini(struct pancsf_device *pfdev)
{
	struct pancsf_fw_section *section;

	if (!pfdev->mcu)
		return;

	gpu_write(pfdev, JOB_INT_MASK, 0);
	synchronize_irq(pfdev->mcu->job_irq);

	pancsf_mcu_stop(pfdev);

	list_for_each_entry(section, &pfdev->mcu->sections, node) {
		pancsf_fw_mem_free(pfdev, section->mem);
	}

	if (pfdev->mcu->vm && pfdev->mcu->as == 0)
		pancsf_vm_as_put(pfdev->mcu->vm);

	pancsf_vm_put(pfdev->mcu->vm);

	pancsf_gpu_power_off(pfdev, L2, pfdev->gpu_info.l2_present, 20000);
}

void pancsf_mcu_pre_reset(struct pancsf_device *pfdev)
{
	gpu_write(pfdev, JOB_INT_MASK, 0);
	synchronize_irq(pfdev->mcu->job_irq);
}
