// SPDX-License-Identifier: GPL-2.0
/*
 * Auxiliary DMA operations used by arch-independent dma-mapping
 * routines when Phytium software IO tlb is required.
 *
 * Copyright (c) 2024, Phytium Technology Co., Ltd.
 */
#include <linux/memblock.h> /* for max_pfn */
#include <linux/acpi.h>
#include <linux/export.h>
#include <linux/gfp.h>
#include <linux/of_device.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/dma-iommu.h>
#include "pswiotlb-dma.h"

#include <asm/cacheflush.h>

dma_addr_t pswiotlb_dma_direct_map_page_distribute(struct device *dev, struct page *page,
			size_t offset, size_t size, enum dma_data_direction dir,
			unsigned long attrs)
{
	dma_addr_t addr;

	addr = pswiotlb_dma_direct_map_page(dev, page, offset, size, dir, attrs);
	if (!is_device_dma_coherent(dev) &&
	    (attrs & DMA_ATTR_SKIP_CPU_SYNC) == 0)
		__dma_map_area(phys_to_virt(dma_to_phys(dev, addr)), size, dir);

	return addr;
}

dma_addr_t pswiotlb_dma_iommu_map_page_distribute(struct device *dev, struct page *page,
			size_t offset, size_t size, enum dma_data_direction dir,
			unsigned long attrs)
{
	return pswiotlb_iommu_dma_map_page(dev, page, offset, size, dir, attrs);
}

void pswiotlb_dma_direct_unmap_page_attrs_distribute(struct device *dev,
			dma_addr_t addr, size_t size, enum dma_data_direction dir,
			unsigned long attrs)
{
	pswiotlb_dma_direct_unmap_page(dev, addr, size, dir, attrs);
}

void pswiotlb_dma_iommu_unmap_page_attrs_distribute(struct device *dev,
			dma_addr_t addr, size_t size, enum dma_data_direction dir,
			unsigned long attrs)
{
	pswiotlb_iommu_dma_unmap_page(dev, addr, size, dir, attrs);
}

int pswiotlb_dma_direct_map_sg_attrs_distribute(struct device *dev,
			struct scatterlist *sgl, int nents, enum dma_data_direction dir,
			unsigned long attrs)
{
	int ents, i;
	struct scatterlist *sg;

	ents = pswiotlb_dma_direct_map_sg(dev, sgl, nents, dir, attrs);
	if (!is_device_dma_coherent(dev) &&
	    (attrs & DMA_ATTR_SKIP_CPU_SYNC) == 0)
		for_each_sg(sgl, sg, ents, i)
			__dma_map_area(phys_to_virt(dma_to_phys(dev, sg->dma_address)),
				       sg->length, dir);

	return ents;
}

int pswiotlb_dma_iommu_map_sg_attrs_distribute(struct device *dev,
			struct scatterlist *sg, int nents, int prot, unsigned long attrs)
{
	int ents;

	ents = pswiotlb_iommu_dma_map_sg(dev, sg, nents, prot, attrs);
	return ents;
}

void pswiotlb_dma_direct_unmap_sg_attrs_distribute(struct device *dev,
			struct scatterlist *sg, int nents, enum dma_data_direction dir,
			unsigned long attrs)
{
	pswiotlb_dma_direct_unmap_sg(dev, sg, nents, dir, attrs);
}

void pswiotlb_dma_iommu_unmap_sg_attrs_distribute(struct device *dev,
			struct scatterlist *sg, int nents, enum dma_data_direction dir,
			unsigned long attrs)
{
	pswiotlb_iommu_dma_unmap_sg(dev, sg, nents, dir, attrs);
}

void pswiotlb_dma_direct_sync_single_for_cpu_distribute(struct device *dev,
			dma_addr_t addr, size_t size, enum dma_data_direction dir)
{
	pswiotlb_dma_direct_sync_single_for_cpu(dev, addr, size, dir);
}

void pswiotlb_dma_iommu_sync_single_for_cpu_distribute(struct device *dev,
			dma_addr_t addr, size_t size, enum dma_data_direction dir)
{
	pswiotlb_iommu_dma_sync_single_for_cpu(dev, addr, size, dir);
}

void pswiotlb_dma_direct_sync_single_for_device_distribute(struct device *dev,
			dma_addr_t addr, size_t size, enum dma_data_direction dir)
{
	pswiotlb_dma_direct_sync_single_for_device(dev, addr, size, dir);
	if (!is_device_dma_coherent(dev))
		__dma_map_area(phys_to_virt(dma_to_phys(dev, addr)), size, dir);
}

void pswiotlb_dma_iommu_sync_single_for_device_distribute(struct device *dev,
			dma_addr_t addr, size_t size, enum dma_data_direction dir)
{
	pswiotlb_iommu_dma_sync_single_for_device(dev, addr, size, dir);
}

void pswiotlb_dma_direct_sync_sg_for_cpu_distribute(struct device *dev,
			struct scatterlist *sg, int nelems, enum dma_data_direction dir)
{
	pswiotlb_dma_direct_sync_sg_for_cpu(dev, sg, nelems, dir);
}

void pswiotlb_dma_iommu_sync_sg_for_cpu_distribute(struct device *dev,
			struct scatterlist *sg, int nelems, enum dma_data_direction dir)
{
	pswiotlb_iommu_dma_sync_sg_for_cpu(dev, sg, nelems, dir);
}

void pswiotlb_dma_direct_sync_sg_for_device_distribute(struct device *dev,
			struct scatterlist *sgl, int nelems, enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	pswiotlb_dma_direct_sync_sg_for_device(dev, sgl, nelems, dir);
	if (!is_device_dma_coherent(dev))
		for_each_sg(sgl, sg, nelems, i)
			__dma_map_area(phys_to_virt(dma_to_phys(dev, sg->dma_address)),
				       sg->length, dir);
}
void pswiotlb_dma_iommu_sync_sg_for_device_distribute(struct device *dev,
			struct scatterlist *sg, int nelems, enum dma_data_direction dir)
{
	pswiotlb_iommu_dma_sync_sg_for_device(dev, sg, nelems, dir);
}
