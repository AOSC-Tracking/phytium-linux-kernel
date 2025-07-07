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

static void *pswiotlb_dma_common_alloc_distribute(struct device *dev, size_t size,
		dma_addr_t *handle, gfp_t gfp, unsigned long attrs)
{
	const struct dma_map_ops *ops = dev->orig_dma_ops;
	void *cpu_addr;

	check_if_pswiotlb_is_applicable(dev);

	cpu_addr = ops->alloc(dev, size, handle, gfp, attrs);

	return cpu_addr;
}

dma_addr_t pswiotlb_dma_direct_map_page_attrs_distribute(struct device *dev,
			struct page *page, size_t offset, size_t size,
			enum dma_data_direction dir, unsigned long attrs)
{
	dma_addr_t dev_addr;

	if (!pswiotlb_bypass_is_needed(dev, 0, dir)) {
		dev_addr = pswiotlb_dma_direct_map_page(dev, page, offset, size, dir, attrs);
		if (!is_device_dma_coherent(dev) &&
		    (attrs & DMA_ATTR_SKIP_CPU_SYNC) == 0)
			__dma_map_area(phys_to_virt(dma_to_phys(dev, dev_addr)), size, dir);
	} else {
		dev_addr = swiotlb_map_page(dev, page, offset, size, dir, attrs);
		if (!is_device_dma_coherent(dev) &&
		    (attrs & DMA_ATTR_SKIP_CPU_SYNC) == 0)
			__dma_map_area(phys_to_virt(dma_to_phys(dev, dev_addr)),
						size, dir);
	}

	return dev_addr;
}

static void __iommu_sync_single_for_device(struct device *dev,
					   dma_addr_t dev_addr, size_t size,
					   enum dma_data_direction dir)
{
	phys_addr_t phys;

	if (is_device_dma_coherent(dev))
		return;

	phys = iommu_iova_to_phys(iommu_get_domain_for_dev(dev), dev_addr);
	__dma_map_area(phys_to_virt(phys), size, dir);
}


dma_addr_t pswiotlb_dma_iommu_map_page_attrs_distribute(struct device *dev, struct page *page,
			size_t offset, size_t size, enum dma_data_direction dir,
			unsigned long attrs)
{
	bool coherent = is_device_dma_coherent(dev);
	int prot = dma_info_to_prot(dir, coherent, attrs);
	dma_addr_t dev_addr;

	if (!pswiotlb_bypass_is_needed(dev, 0, dir))
		dev_addr = pswiotlb_iommu_dma_map_page(dev, page, offset, size, dir, attrs);
	else {
		dev_addr = iommu_dma_map_page(dev, page, offset, size, prot);
		if (!iommu_dma_mapping_error(dev, dev_addr) &&
		    (attrs & DMA_ATTR_SKIP_CPU_SYNC) == 0)
			__iommu_sync_single_for_device(dev, dev_addr, size, dir);
	}

	return dev_addr;
}

void pswiotlb_dma_direct_unmap_page_attrs_distribute(struct device *dev,
			dma_addr_t dev_addr, size_t size, enum dma_data_direction dir,
			unsigned long attrs)
{
	if (!is_device_dma_coherent(dev) &&
	    (attrs & DMA_ATTR_SKIP_CPU_SYNC) == 0)
		__dma_unmap_area(phys_to_virt(dma_to_phys(dev, dev_addr)), size, dir);
	pswiotlb_dma_direct_unmap_page(dev, dev_addr, size, dir, attrs);
}

void pswiotlb_dma_iommu_unmap_page_attrs_distribute(struct device *dev,
			dma_addr_t addr, size_t size, enum dma_data_direction dir,
			unsigned long attrs)
{
	pswiotlb_iommu_dma_unmap_page(dev, addr, size, dir, attrs);
}

int pswiotlb_dma_direct_map_sg_attrs_distribute(struct device *dev,
			struct scatterlist *sgl, int nelems, enum dma_data_direction dir,
			unsigned long attrs)
{
	int ents, i;
	struct scatterlist *sg;

	if (!pswiotlb_bypass_is_needed(dev, nelems, dir)) {
		ents = pswiotlb_dma_direct_map_sg(dev, sgl, nelems, dir, attrs);
		if (!is_device_dma_coherent(dev) &&
		    (attrs & DMA_ATTR_SKIP_CPU_SYNC) == 0)
			for_each_sg(sgl, sg, ents, i)
				__dma_map_area(phys_to_virt(dma_to_phys(dev, sg->dma_address)),
					       sg->length, dir);
	} else {
		ents = swiotlb_map_sg_attrs(dev, sgl, nelems, dir, attrs);
		if (!is_device_dma_coherent(dev) &&
		    (attrs & DMA_ATTR_SKIP_CPU_SYNC) == 0)
			for_each_sg(sgl, sg, ents, i)
				__dma_map_area(phys_to_virt(dma_to_phys(dev, sg->dma_address)),
							sg->length, dir);
	}

	return ents;
}

static void __iommu_sync_sg_for_device(struct device *dev,
				       struct scatterlist *sgl, int nelems,
				       enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	if (is_device_dma_coherent(dev))
		return;

	for_each_sg(sgl, sg, nelems, i)
		__dma_map_area(sg_virt(sg), sg->length, dir);
}

int pswiotlb_dma_iommu_map_sg_attrs_distribute(struct device *dev,
			struct scatterlist *sgl, int nelems,
			enum dma_data_direction dir, unsigned long attrs)
{
	bool coherent = is_device_dma_coherent(dev);

	if (!pswiotlb_bypass_is_needed(dev, nelems, dir)) {
		if ((dir == DMA_TO_DEVICE) && !(attrs & DMA_ATTR_SKIP_CPU_SYNC))
			pswiotlb_dma_iommu_sync_sg_for_device_distribute(dev, sgl, nelems, dir);

		return pswiotlb_iommu_dma_map_sg(dev, sgl, nelems,
					dma_info_to_prot(dir, coherent, attrs), attrs);
	} else {
		if ((attrs & DMA_ATTR_SKIP_CPU_SYNC) == 0)
			__iommu_sync_sg_for_device(dev, sgl, nelems, dir);

		return iommu_dma_map_sg(dev, sgl, nelems,
					dma_info_to_prot(dir, coherent, attrs));
	}
}

void pswiotlb_dma_direct_unmap_sg_attrs_distribute(struct device *dev,
			struct scatterlist *sgl, int nelems, enum dma_data_direction dir,
			unsigned long attrs)
{
	struct scatterlist *sg;
	int i;

	if (!is_device_dma_coherent(dev) &&
	    (attrs & DMA_ATTR_SKIP_CPU_SYNC) == 0)
		for_each_sg(sgl, sg, nelems, i)
			__dma_unmap_area(phys_to_virt(dma_to_phys(dev, sg->dma_address)),
					 sg->length, dir);
	pswiotlb_dma_direct_unmap_sg(dev, sgl, nelems, dir, attrs);
}

void pswiotlb_dma_iommu_unmap_sg_attrs_distribute(struct device *dev,
				   struct scatterlist *sgl, int nelems,
				   enum dma_data_direction dir,
				   unsigned long attrs)
{
	if ((dir == DMA_TO_DEVICE) && !(attrs & DMA_ATTR_SKIP_CPU_SYNC))
		pswiotlb_dma_iommu_sync_sg_for_cpu_distribute(dev, sgl, nelems, dir);

	pswiotlb_iommu_dma_unmap_sg(dev, sgl, nelems, dir, attrs);
}

void pswiotlb_dma_direct_sync_single_for_cpu_distribute(struct device *dev,
			dma_addr_t dev_addr, size_t size, enum dma_data_direction dir)
{
	if (!is_device_dma_coherent(dev))
		__dma_unmap_area(phys_to_virt(dma_to_phys(dev, dev_addr)), size, dir);
	pswiotlb_dma_direct_sync_single_for_cpu(dev, dev_addr, size, dir);
}

void pswiotlb_dma_iommu_sync_single_for_cpu_distribute(struct device *dev,
					dma_addr_t dev_addr, size_t size,
					enum dma_data_direction dir)
{
	phys_addr_t phys;

	if (!is_device_dma_coherent(dev)) {
		phys = iommu_iova_to_phys(iommu_get_domain_for_dev(dev), dev_addr);
		__dma_unmap_area(phys_to_virt(phys), size, dir);
	}
	pswiotlb_iommu_dma_sync_single_for_cpu(dev, dev_addr, size, dir);
}

void pswiotlb_dma_direct_sync_single_for_device_distribute(struct device *dev,
			dma_addr_t dev_addr, size_t size, enum dma_data_direction dir)
{
	pswiotlb_dma_direct_sync_single_for_device(dev, dev_addr, size, dir);
	if (!is_device_dma_coherent(dev))
		__dma_map_area(phys_to_virt(dma_to_phys(dev, dev_addr)), size, dir);
}

void pswiotlb_dma_iommu_sync_single_for_device_distribute(struct device *dev,
					   dma_addr_t dev_addr, size_t size,
					   enum dma_data_direction dir)
{
	phys_addr_t phys;

	pswiotlb_iommu_dma_sync_single_for_device(dev, dev_addr, size, dir);
	if (!is_device_dma_coherent(dev)) {
		phys = iommu_iova_to_phys(iommu_get_domain_for_dev(dev), dev_addr);
		__dma_map_area(phys_to_virt(phys), size, dir);
	}
}

void pswiotlb_dma_direct_sync_sg_for_cpu_distribute(struct device *dev,
			struct scatterlist *sgl, int nelems, enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;

	if (!is_device_dma_coherent(dev))
		for_each_sg(sgl, sg, nelems, i)
			__dma_unmap_area(phys_to_virt(dma_to_phys(dev, sg->dma_address)),
					 sg->length, dir);
	pswiotlb_dma_direct_sync_sg_for_cpu(dev, sgl, nelems, dir);
}

void pswiotlb_dma_iommu_sync_sg_for_cpu_distribute(struct device *dev,
			struct scatterlist *sgl, int nelems, enum dma_data_direction dir)
{
	pswiotlb_iommu_dma_sync_sg_for_cpu(dev, sgl, nelems, dir);
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
			struct scatterlist *sgl, int nelems, enum dma_data_direction dir)
{
	pswiotlb_iommu_dma_sync_sg_for_device(dev, sgl, nelems, dir);
}

static bool iommu_domain_is_translated(struct device *dev, u64 dma_base, u64 size,
				  const struct iommu_ops *ops)
{
	struct iommu_domain *domain;

	if (!ops)
		return false;

	domain = iommu_get_domain_for_dev(dev);

	if (!domain)
		goto out_err;

	if (domain->type == IOMMU_DOMAIN_DMA) {
		if (iommu_dma_init_domain(domain, dma_base, size, dev))
			goto out_err;

		return true;
	}

	return false;

out_err:
	return false;
}

struct pswiotlb_dma_map_ops *pswiotlb_clone_orig_dma_ops(struct device *dev,
			const struct dma_map_ops *ops)
{
	struct pswiotlb_dma_map_ops *new_dma_ops = kmalloc(sizeof(struct pswiotlb_dma_map_ops),
				GFP_KERNEL);
	if (!new_dma_ops)
		return NULL;

	memcpy(new_dma_ops, ops, sizeof(struct pswiotlb_dma_map_ops));

	return new_dma_ops;
}

void pswiotlb_setup_dma_ops(struct device *dev, u64 dma_base,
			u64 size, const struct iommu_ops *iommu_ops)
{
	const struct dma_map_ops *orig_ops = get_dma_ops(dev);
	struct pswiotlb_dma_map_ops *new_ops;
	struct pci_dev *pdev;

	if (dev && dev_is_pci(dev) && (pswiotlb_force_disable != true) &&
			is_phytium_ps_socs()) {
		pdev = to_pci_dev(dev);
		pdev->dev.can_use_pswiotlb = pswiotlb_is_dev_in_passthroughlist(pdev);
		dev_info(&pdev->dev, "The device %s use pswiotlb because vendor 0x%04x %s in pswiotlb passthroughlist\n",
					pdev->dev.can_use_pswiotlb ? "would" : "would NOT",
					pdev->vendor, pdev->dev.can_use_pswiotlb ? "is NOT" : "is");
	}

	if (check_if_pswiotlb_is_applicable(dev)) {
		if (!iommu_domain_is_translated(dev, dma_base, size, iommu_ops)) {
			new_ops = pswiotlb_clone_orig_dma_ops(dev, orig_ops);
			if (!new_ops) {
				dev_warn(dev, "Failed to clone dma ops, pswiotlb is NOT applicable\n");
				return;
			}

			dev->orig_dma_ops = get_dma_ops(dev);
			new_ops->alloc		= pswiotlb_dma_common_alloc_distribute;
			new_ops->map_page	= pswiotlb_dma_direct_map_page_attrs_distribute;
			new_ops->unmap_page	= pswiotlb_dma_direct_unmap_page_attrs_distribute;
			new_ops->map_sg		= pswiotlb_dma_direct_map_sg_attrs_distribute;
			new_ops->unmap_sg	= pswiotlb_dma_direct_unmap_sg_attrs_distribute;
			new_ops->sync_single_for_cpu =
				pswiotlb_dma_direct_sync_single_for_cpu_distribute;
			new_ops->sync_single_for_device	=
				pswiotlb_dma_direct_sync_single_for_device_distribute;
			new_ops->sync_sg_for_cpu =
				pswiotlb_dma_direct_sync_sg_for_cpu_distribute;
			new_ops->sync_sg_for_device	=
				pswiotlb_dma_direct_sync_sg_for_device_distribute;

			set_dma_ops(dev, (const struct dma_map_ops *)new_ops);
		} else {
			new_ops = pswiotlb_clone_orig_dma_ops(dev, orig_ops);
			if (!new_ops) {
				dev_warn(dev, "Failed to clone dma ops, pswiotlb is NOT applicable\n");
				return;
			}

			dev->orig_dma_ops = get_dma_ops(dev);
			new_ops->alloc		= pswiotlb_dma_common_alloc_distribute;
			new_ops->map_page	= pswiotlb_dma_iommu_map_page_attrs_distribute;
			new_ops->unmap_page	= pswiotlb_dma_iommu_unmap_page_attrs_distribute;
			new_ops->map_sg		= pswiotlb_dma_iommu_map_sg_attrs_distribute;
			new_ops->unmap_sg	= pswiotlb_dma_iommu_unmap_sg_attrs_distribute;
			new_ops->sync_single_for_cpu =
				pswiotlb_dma_iommu_sync_single_for_cpu_distribute;
			new_ops->sync_single_for_device	=
				pswiotlb_dma_iommu_sync_single_for_device_distribute;
			new_ops->sync_sg_for_cpu =
				pswiotlb_dma_iommu_sync_sg_for_cpu_distribute;
			new_ops->sync_sg_for_device	=
				pswiotlb_dma_iommu_sync_sg_for_device_distribute;

			set_dma_ops(dev, (const struct dma_map_ops *)new_ops);
		}
	}
}
