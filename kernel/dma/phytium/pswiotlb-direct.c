// SPDX-License-Identifier: GPL-2.0
/*
 * DMA operations based on Phytium software IO tlb that
 * map physical memory directly without using an IOMMU.
 *
 * Copyright (c) 2024, Phytium Technology Co., Ltd.
 */
#include <linux/memblock.h> /* for max_pfn */
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/scatterlist.h>
#include <linux/pfn.h>
#include <linux/vmalloc.h>
#include <linux/set_memory.h>
#include <linux/slab.h>
#include <linux/pci.h>
#include <linux/pswiotlb.h>
#include "pswiotlb-dma.h"

#if defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_DEVICE) || \
	defined(CONFIG_PSWIOTLB)
void pswiotlb_dma_direct_sync_sg_for_device(struct device *dev,
		struct scatterlist *sgl, int nents, enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;
	int nid = dev->numa_node;
	struct p_io_tlb_pool *pool;

	for_each_sg(sgl, sg, nents, i) {
		phys_addr_t paddr = dma_to_phys(dev, sg_dma_address(sg));

		if (unlikely(is_swiotlb_buffer(paddr)))
			swiotlb_sync_single_for_device(dev, paddr, sg->length,
						       dir);

		if (is_pswiotlb_active(dev) &&
			unlikely(is_pswiotlb_buffer(dev, nid, paddr, &pool)))
			pswiotlb_sync_single_for_device(dev, nid, paddr,
						sg->length, dir, pool);
	}
}
#endif

#if defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU) || \
	defined(CONFIG_ARCH_HAS_SYNC_DMA_FOR_CPU_ALL) || \
	defined(CONFIG_PSWIOTLB)
void pswiotlb_dma_direct_sync_sg_for_cpu(struct device *dev,
		struct scatterlist *sgl, int nents, enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;
	int nid = dev->numa_node;
	struct p_io_tlb_pool *pool;

	for_each_sg(sgl, sg, nents, i) {
		phys_addr_t paddr = dma_to_phys(dev, sg_dma_address(sg));

		if (unlikely(is_swiotlb_buffer(paddr)))
			swiotlb_sync_single_for_cpu(dev, paddr, sg->length,
						    dir);

		if (is_pswiotlb_active(dev) &&
			unlikely(is_pswiotlb_buffer(dev, nid, paddr, &pool)))
			pswiotlb_sync_single_for_cpu(dev, nid, paddr,
						sg->length, dir, pool);

		if (dir == DMA_FROM_DEVICE)
			dma_mark_clean(phys_to_virt(paddr), sg->length);
	}
}

/*
 * Unmaps segments, except for ones marked as pci_p2pdma which do not
 * require any further action as they contain a bus address.
 */
void pswiotlb_dma_direct_unmap_sg(struct device *dev, struct scatterlist *sgl,
		int nents, enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	for_each_sg(sgl, sg, nents, i)
		pswiotlb_dma_direct_unmap_page(dev, sg->dma_address, sg_dma_len(sg), dir,
			     attrs);
}
#endif

int pswiotlb_dma_direct_map_sg(struct device *dev, struct scatterlist *sgl, int nents,
		enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *sg;
	int i, ret;

	for_each_sg(sgl, sg, nents, i) {
		sg->dma_address = pswiotlb_dma_direct_map_page(dev, sg_page(sg),
				sg->offset, sg->length, dir, attrs);
		if (sg->dma_address == DMA_MAPPING_ERROR) {
			ret = -EIO;
			goto out_unmap;
		}
		sg_dma_len(sg) = sg->length;
	}

	return nents;

out_unmap:
	pswiotlb_dma_direct_unmap_sg(dev, sgl, i, dir, attrs | DMA_ATTR_SKIP_CPU_SYNC);
	return ret;
}
