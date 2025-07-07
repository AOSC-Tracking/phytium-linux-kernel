/* SPDX-License-Identifier: GPL-2.0 */
/*
 * DMA operations based on Phytium software IO tlb that
 * map physical memory.
 *
 * Copyright (c) 2024, Phytium Technology Co., Ltd.
 */
#ifndef _KERNEL_PSWIOTLB_DMA_DIRECT_H
#define _KERNEL_PSWIOTLB_DMA_DIRECT_H

#include <linux/dma-direct.h>
#include <linux/iommu.h>
#include <linux/swiotlb.h>
#include <linux/pswiotlb.h>

struct pswiotlb_dma_map_ops {
	void* (*alloc)(struct device *dev, size_t size,
				dma_addr_t *dma_handle, gfp_t gfp,
				unsigned long attrs);
	void (*free)(struct device *dev, size_t size,
			      void *vaddr, dma_addr_t dma_handle,
			      unsigned long attrs);
	int (*mmap)(struct device *dev, struct vm_area_struct *vma,
			  void *cpu_addr, dma_addr_t dma_addr, size_t size,
			  unsigned long attrs);

	int (*get_sgtable)(struct device *dev, struct sg_table *sgt, void *cpu_addr,
			   dma_addr_t handle, size_t size, unsigned long attrs);

	dma_addr_t (*map_page)(struct device *dev, struct page *page,
			       unsigned long offset, size_t size,
			       enum dma_data_direction dir,
			       unsigned long attrs);
	void (*unmap_page)(struct device *dev, dma_addr_t dma_handle,
			   size_t size, enum dma_data_direction dir,
			   unsigned long attrs);
	/*
	 * map_sg returns 0 on error and a value > 0 on success.
	 * It should never return a value < 0.
	 */
	int (*map_sg)(struct device *dev, struct scatterlist *sg,
		      int nents, enum dma_data_direction dir,
		      unsigned long attrs);
	void (*unmap_sg)(struct device *dev,
			 struct scatterlist *sg, int nents,
			 enum dma_data_direction dir,
			 unsigned long attrs);
	dma_addr_t (*map_resource)(struct device *dev, phys_addr_t phys_addr,
			       size_t size, enum dma_data_direction dir,
			       unsigned long attrs);
	void (*unmap_resource)(struct device *dev, dma_addr_t dma_handle,
			   size_t size, enum dma_data_direction dir,
			   unsigned long attrs);
	void (*sync_single_for_cpu)(struct device *dev,
				    dma_addr_t dma_handle, size_t size,
				    enum dma_data_direction dir);
	void (*sync_single_for_device)(struct device *dev,
				       dma_addr_t dma_handle, size_t size,
				       enum dma_data_direction dir);
	void (*sync_sg_for_cpu)(struct device *dev,
				struct scatterlist *sg, int nents,
				enum dma_data_direction dir);
	void (*sync_sg_for_device)(struct device *dev,
				   struct scatterlist *sg, int nents,
				   enum dma_data_direction dir);
	void (*cache_sync)(struct device *dev, void *vaddr, size_t size,
			enum dma_data_direction direction);
	int (*mapping_error)(struct device *dev, dma_addr_t dma_addr);
	int (*dma_supported)(struct device *dev, u64 mask);
#ifdef ARCH_HAS_DMA_GET_REQUIRED_MASK
	u64 (*get_required_mask)(struct device *dev);
#endif
};

#if defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_DEVICE) || \
	defined(CONFIG_PSWIOTLB)
void pswiotlb_dma_direct_sync_sg_for_device(struct device *dev, struct scatterlist *sgl,
		int nents, enum dma_data_direction dir);
#else
static inline void pswiotlb_dma_direct_sync_sg_for_device(struct device *dev,
		struct scatterlist *sgl, int nents, enum dma_data_direction dir)
{
}
#endif

#if defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU) || \
	defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU_ALL) || \
	defined(CONFIG_PSWIOTLB)
void pswiotlb_dma_direct_unmap_sg(struct device *dev, struct scatterlist *sgl,
		int nents, enum dma_data_direction dir, unsigned long attrs);
void pswiotlb_dma_direct_sync_sg_for_cpu(struct device *dev,
		struct scatterlist *sgl, int nents, enum dma_data_direction dir);
#else
static inline void pswiotlb_dma_direct_unmap_sg(struct device *dev,
		struct scatterlist *sgl, int nents, enum dma_data_direction dir,
		unsigned long attrs)
{
}
static inline void pswiotlb_dma_direct_sync_sg_for_cpu(struct device *dev,
		struct scatterlist *sgl, int nents, enum dma_data_direction dir)
{
}
#endif
#ifdef CONFIG_PSWIOTLB
int pswiotlb_dma_direct_map_sg(struct device *dev, struct scatterlist *sgl, int nents,
		enum dma_data_direction dir, unsigned long attrs);
dma_addr_t pswiotlb_iommu_dma_map_page(struct device *dev, struct page *page,
		unsigned long offset, size_t size, enum dma_data_direction dir,
		unsigned long attrs);
void pswiotlb_iommu_dma_unmap_page(struct device *dev, dma_addr_t dma_handle,
		size_t size, enum dma_data_direction dir, unsigned long attrs);
int pswiotlb_iommu_dma_map_sg(struct device *dev, struct scatterlist *sg,
		int nents, int prot, unsigned long attrs);
void pswiotlb_iommu_dma_unmap_sg(struct device *dev, struct scatterlist *sg,
		int nents, enum dma_data_direction dir, unsigned long attrs);
void pswiotlb_iommu_dma_sync_single_for_cpu(struct device *dev,
		dma_addr_t dma_handle, size_t size, enum dma_data_direction dir);
void pswiotlb_iommu_dma_sync_single_for_device(struct device *dev,
		dma_addr_t dma_handle, size_t size, enum dma_data_direction dir);
void pswiotlb_iommu_dma_sync_sg_for_cpu(struct device *dev,
		struct scatterlist *sgl, int nelems,
		enum dma_data_direction dir);
void pswiotlb_iommu_dma_sync_sg_for_device(struct device *dev,
		struct scatterlist *sgl, int nelems,
		enum dma_data_direction dir);

static inline void pswiotlb_dma_direct_sync_single_for_device(struct device *dev,
		dma_addr_t addr, size_t size, enum dma_data_direction dir)
{
	phys_addr_t paddr = dma_to_phys(dev, addr);
	int nid = dev->numa_node;
	struct p_io_tlb_pool *pool;

	if (unlikely(is_swiotlb_buffer(paddr)))
		swiotlb_sync_single_for_device(dev, paddr, size, dir);

	if (is_pswiotlb_active(dev)) {
		if (unlikely(is_pswiotlb_buffer(dev, nid, paddr, &pool)))
			pswiotlb_sync_single_for_device(dev, nid, paddr, size, dir, pool);
	}
}

static inline void pswiotlb_dma_direct_sync_single_for_cpu(struct device *dev,
		dma_addr_t addr, size_t size, enum dma_data_direction dir)
{
	phys_addr_t paddr = dma_to_phys(dev, addr);
	int nid = dev->numa_node;
	struct p_io_tlb_pool *pool;

	if (unlikely(is_swiotlb_buffer(paddr)))
		swiotlb_sync_single_for_cpu(dev, paddr, size, dir);

	if (is_pswiotlb_active(dev)) {
		if (unlikely(is_pswiotlb_buffer(dev, nid, paddr, &pool)))
			pswiotlb_sync_single_for_cpu(dev, nid, paddr, size, dir, pool);
	}

	if (dir == DMA_FROM_DEVICE)
		dma_mark_clean(phys_to_virt(paddr), size);
}

static inline dma_addr_t pswiotlb_dma_direct_map_page(struct device *dev,
		struct page *page, unsigned long offset, size_t size,
		enum dma_data_direction dir, unsigned long attrs)
{
	phys_addr_t phys = page_to_phys(page) + offset;
	dma_addr_t dma_addr = phys_to_dma(dev, phys);
	int nid = dev->numa_node;

	if (unlikely(!dma_capable(dev, dma_addr, size))) {
		if (swiotlb_force != SWIOTLB_NO_FORCE)
			return swiotlb_map_page(dev, page, offset, size, dir, attrs);

		dev_WARN_ONCE(dev, 1,
			     "DMA addr %pad+%zu overflow (mask %llx).\n",
			     &dma_addr, size, *dev->dma_mask);
		return DMA_MAPPING_ERROR;
	}

	/* check whether dma addr is in local node */
	if (is_pswiotlb_active(dev)) {
		if (dir != DMA_TO_DEVICE) {
			if (unlikely(!dma_is_in_local_node(dev, nid, dma_addr, size))) {
				dma_addr = pswiotlb_map(dev, nid, phys, size, dir, attrs);
				if (dma_addr == DMA_MAPPING_ERROR) {
					dma_addr = phys_to_dma(dev, phys);
					dev_warn_once(dev,
						"Failed to allocate memory from pswiotlb, fall back to non-local dma\n");
				} else
					return dma_addr;
			}
		}
	}

	return dma_addr;
}

static inline void pswiotlb_dma_direct_unmap_page(struct device *dev, dma_addr_t addr,
		size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	phys_addr_t phys = dma_to_phys(dev, addr);
	int nid = dev->numa_node;
	struct p_io_tlb_pool *pool;

	if (unlikely(is_swiotlb_buffer(phys)))
		swiotlb_tbl_unmap_single(dev, phys, size, dir, attrs);

	if (is_pswiotlb_active(dev)) {
		if (unlikely(is_pswiotlb_buffer(dev, nid, phys, &pool)))
			pswiotlb_tbl_unmap_single(dev, nid, phys, 0, size, dir, attrs, pool);

		if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC) && (dir == DMA_FROM_DEVICE))
			dma_mark_clean(phys_to_virt(phys), size);
	}
}
#else
static inline int pswiotlb_dma_direct_map_sg(struct device *dev,
			struct scatterlist *sgl, int nents, enum dma_data_direction dir,
			unsigned long attrs)
{
	return 0;
}
static inline dma_addr_t pswiotlb_iommu_dma_map_page(struct device *dev,
			struct page *page, unsigned long offset, size_t size,
			enum dma_data_direction dir, unsigned long attrs)
{
	return 0;
}
static inline void pswiotlb_iommu_dma_unmap_page(struct device *dev,
			dma_addr_t dma_handle, size_t size, enum dma_data_direction dir,
			unsigned long attrs)
{
}
static inline int pswiotlb_iommu_dma_map_sg(struct device *dev,
			struct scatterlist *sg, int nents, int prot, unsigned long attrs)
{
	return 0;
}
static inline void pswiotlb_iommu_dma_unmap_sg(struct device *dev,
			struct scatterlist *sg, int nents, enum dma_data_direction dir,
			unsigned long attrs)
{
}
static inline void pswiotlb_iommu_dma_sync_single_for_cpu(struct device *dev,
		dma_addr_t dma_handle, size_t size, enum dma_data_direction dir)
{
}
static inline void pswiotlb_iommu_dma_sync_single_for_device(struct device *dev,
		dma_addr_t dma_handle, size_t size, enum dma_data_direction dir)
{
}
static inline void pswiotlb_iommu_dma_sync_sg_for_cpu(struct device *dev,
		struct scatterlist *sgl, int nelems, enum dma_data_direction dir)
{
}
static inline void pswiotlb_iommu_dma_sync_sg_for_device(struct device *dev,
		struct scatterlist *sgl, int nelems, enum dma_data_direction dir)
{
}

static inline dma_addr_t pswiotlb_dma_direct_map_page(struct device *dev,
		struct page *page, unsigned long offset, size_t size,
		enum dma_data_direction dir, unsigned long attrs)
{
	return 0;
}
static inline void pswiotlb_dma_direct_sync_single_for_device(struct device *dev,
		dma_addr_t addr, size_t size, enum dma_data_direction dir)
{
}
static inline void pswiotlb_dma_direct_unmap_page(struct device *dev, dma_addr_t addr,
		size_t size, enum dma_data_direction dir, unsigned long attrs)
{
}
static inline void pswiotlb_dma_direct_sync_single_for_cpu(struct device *dev,
		dma_addr_t addr, size_t size, enum dma_data_direction dir)
{
}

#endif /* CONFIG_PSWIOTLB */
#endif /* _KERNEL_PSWIOTLB_DMA_DIRECT_H */
