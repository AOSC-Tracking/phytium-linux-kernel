// SPDX-License-Identifier: GPL-2.0
/*
 * DMA operations based on Phytium software IO tlb that
 * map physical memory indirectly with an IOMMU.
 *
 * Copyright (c) 2024, Phytium Technology Co., Ltd.
 */

#define pr_fmt(fmt)    "pswiotlb iommu: " fmt

#include <linux/acpi_iort.h>
#include <linux/device.h>
#include <linux/dma-iommu.h>
#include <linux/kernel.h>
#include <linux/bits.h>
#include <linux/bug.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/gfp.h>
#include <linux/huge_mm.h>
#include <linux/iommu.h>
#include <linux/idr.h>
#include <linux/notifier.h>
#include <linux/err.h>
#include <linux/iova.h>
#include <linux/irq.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include <linux/bitops.h>
#include <linux/property.h>
#include <linux/fsl/mc.h>
#include <linux/module.h>
#include <trace/events/iommu.h>
#include <linux/swiotlb.h>
#include <linux/scatterlist.h>
#include <linux/vmalloc.h>
#include <linux/crash_dump.h>
#include <linux/dma-direct.h>

#include <linux/atomic.h>
#include <linux/crash_dump.h>
#include <linux/list_sort.h>
#include <linux/memremap.h>
#include <linux/of_iommu.h>
#include <linux/spinlock.h>
#include <asm/cacheflush.h>
#include <linux/pswiotlb.h>
#ifdef CONFIG_ARCH_PHYTIUM
#include <asm/cputype.h>
#endif

#include "pswiotlb-dma.h"

enum iommu_dma_cookie_type {
	IOMMU_DMA_IOVA_COOKIE,
	IOMMU_DMA_MSI_COOKIE,
};

struct iommu_dma_cookie {
	enum iommu_dma_cookie_type	type;
	union {
		/* Full allocator for IOMMU_DMA_IOVA_COOKIE */
		struct iova_domain	iovad;
		/* Trivial linear page allocator for IOMMU_DMA_MSI_COOKIE */
		dma_addr_t		msi_iova;
	};
	struct list_head		msi_page_list;
	spinlock_t			msi_lock;

	/* Domain for flush queue callback; NULL if flush queue not in use */
	struct iommu_domain		*fq_domain;
};

#define IOMMU_MAPPING_ERROR	0
/*
 * The following functions are ported from
 * ./drivers/iommu/dma-iommu.c
 * ./drivers/iommu/iommu.c
 * static dma_addr_t iommu_dma_alloc_iova(struct iommu_domain *domain,
 *		size_t size, u64 dma_limit, struct device *dev);
 * static void iommu_dma_free_iova(struct iommu_dma_cookie *cookie,
 *		dma_addr_t iova, size_t size);
 * static void __iommu_dma_unmap(struct iommu_domain *domain, dma_addr_t dma_addr,
 *		size_t size);
 * static dma_addr_t __iommu_dma_map(struct device *dev, phys_addr_t phys,
 *		size_t size, int prot, struct iommu_domain *domain);
 * static int __finalise_sg(struct device *dev, struct scatterlist *sg, int nents,
 *		dma_addr_t dma_addr);
 * static void __invalidate_sg(struct scatterlist *sg, int nents);
 */
static ssize_t __iommu_map_sg_dma(struct device *dev, struct iommu_domain *domain,
			unsigned long iova, struct scatterlist *sg, unsigned int nents,
			int prot, unsigned long attrs)
{
	struct scatterlist *s;
	size_t mapped = 0;
	unsigned int i, min_pagesz;
	int ret;
	int nid = dev->numa_node;
	enum dma_data_direction dir = prot & (DMA_TO_DEVICE | DMA_FROM_DEVICE | DMA_BIDIRECTIONAL);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	size_t aligned_size;
	struct scatterlist *sg_orig = sg;

	if (unlikely(domain->pgsize_bitmap == 0UL))
		return 0;

	min_pagesz = 1 << __ffs(domain->pgsize_bitmap);

	for_each_sg(sg, s, nents, i) {
		phys_addr_t phys = page_to_phys(sg_page(s)) + s->offset;

		/*
		 * We are mapping on IOMMU page boundaries, so offset within
		 * the page must be 0. However, the IOMMU may support pages
		 * smaller than PAGE_SIZE, so s->offset may still represent
		 * an offset of that boundary within the CPU page.
		 */
		if (!IS_ALIGNED(s->offset, min_pagesz))
			goto out_err;

		/* check whether dma addr is in local node */
		if (dir != DMA_TO_DEVICE) {
			aligned_size = s->length;
			if ((!dma_is_in_local_node(dev, nid, phys,
				aligned_size))) {
				aligned_size = iova_align(iovad, s->length);
				phys = pswiotlb_tbl_map_single(dev, nid,
				phys, s->length, aligned_size, iova_mask(iovad), dir, attrs);
				if (phys == DMA_MAPPING_ERROR) {
					phys = page_to_phys(sg_page(s)) + s->offset;
					dev_warn_once(dev,
						"Failed to allocate memory from pswiotlb, fall back to non-local dma\n");
				}
			}
		}
		if (!is_device_dma_coherent(dev) && !(attrs & DMA_ATTR_SKIP_CPU_SYNC))
			__dma_map_area(phys_to_virt(phys), s->length, dir);

		ret = iommu_map(domain, iova + mapped, phys, s->length, prot);
		if (ret)
			goto out_err;

		mapped += s->length;
	}

	return mapped;

out_err:
	/* undo mappings already done */
	iommu_dma_unmap_sg_pswiotlb(dev, sg_orig, iova,
				mapped, i, dir, attrs | DMA_ATTR_SKIP_CPU_SYNC);
	iommu_unmap(domain, iova, mapped);

	return ret;
}

static ssize_t pswiotlb_iommu_map_sg_atomic_dma(struct device *dev,
			struct iommu_domain *domain, unsigned long iova,
			struct scatterlist *sg, unsigned int nents, int prot,
			unsigned long attrs)
{
	return __iommu_map_sg_dma(dev, domain, iova, sg, nents, prot, attrs);
}

static dma_addr_t iommu_dma_alloc_iova(struct iommu_domain *domain,
		size_t size, dma_addr_t dma_limit, struct device *dev)
{
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	unsigned long shift, iova_len, iova = 0;

	if (cookie->type == IOMMU_DMA_MSI_COOKIE) {
		cookie->msi_iova += size;
		return cookie->msi_iova - size;
	}

	shift = iova_shift(iovad);
	iova_len = size >> shift;
	/*
	 * Freeing non-power-of-two-sized allocations back into the IOVA caches
	 * will come back to bite us badly, so we have to waste a bit of space
	 * rounding up anything cacheable to make sure that can't happen. The
	 * order of the unadjusted size will still match upon freeing.
	 */
	if (iova_len < (1 << (IOVA_RANGE_CACHE_MAX_SIZE - 1)))
		iova_len = roundup_pow_of_two(iova_len);

	if (dev->bus_dma_mask)
		dma_limit &= dev->bus_dma_mask;

	if (domain->geometry.force_aperture)
		dma_limit = min(dma_limit, domain->geometry.aperture_end);

	/* Try to get PCI devices a SAC address */
	if (dma_limit > DMA_BIT_MASK(32) && dev_is_pci(dev))
		iova = alloc_iova_fast(iovad, iova_len,
				       DMA_BIT_MASK(32) >> shift, false);

	if (!iova)
		iova = alloc_iova_fast(iovad, iova_len, dma_limit >> shift,
				       true);

	return (dma_addr_t)iova << shift;
}

static void iommu_dma_free_iova(struct iommu_dma_cookie *cookie,
		dma_addr_t iova, size_t size)
{
	struct iova_domain *iovad = &cookie->iovad;

	/* The MSI case is only ever cleaning up its most recent allocation */
	if (cookie->type == IOMMU_DMA_MSI_COOKIE)
		cookie->msi_iova -= size;
	else if (cookie->fq_domain)	/* non-strict mode */
		queue_iova(iovad, iova_pfn(iovad, iova),
				size >> iova_shift(iovad), 0);
	else
		free_iova_fast(iovad, iova_pfn(iovad, iova),
				size >> iova_shift(iovad));
}

static void __iommu_dma_unmap(struct iommu_domain *domain, dma_addr_t dma_addr,
		size_t size)
{
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	size_t iova_off = iova_offset(iovad, dma_addr);

	dma_addr -= iova_off;
	size = iova_align(iovad, size + iova_off);

	WARN_ON(iommu_unmap_fast(domain, dma_addr, size) != size);
	if (!cookie->fq_domain)
		iommu_tlb_sync(domain);
	iommu_dma_free_iova(cookie, dma_addr, size);
}

static dma_addr_t __iommu_dma_map(struct device *dev, phys_addr_t phys,
		size_t size, int prot, struct iommu_domain *domain)
{
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	size_t iova_off = 0;
	dma_addr_t iova;

	if (cookie->type == IOMMU_DMA_IOVA_COOKIE) {
		iova_off = iova_offset(&cookie->iovad, phys);
		size = iova_align(&cookie->iovad, size + iova_off);
	}

	iova = iommu_dma_alloc_iova(domain, size, dma_get_mask(dev), dev);
	if (!iova)
		return IOMMU_MAPPING_ERROR;

	if (iommu_map(domain, iova, phys - iova_off, size, prot)) {
		iommu_dma_free_iova(cookie, iova, size);
		return IOMMU_MAPPING_ERROR;
	}
	return iova + iova_off;
}

void pswiotlb_iommu_dma_sync_single_for_cpu(struct device *dev,
		dma_addr_t dma_handle, size_t size, enum dma_data_direction dir)
{
	phys_addr_t phys;
	int nid = dev->numa_node;

	if (is_pswiotlb_active(dev)) {
		phys = iommu_iova_to_phys(iommu_get_domain_for_dev(dev), dma_handle);

		if (is_pswiotlb_buffer(dev, nid, phys))
			pswiotlb_sync_single_for_cpu(dev, nid, phys, size, dir);
	}
}

void pswiotlb_iommu_dma_sync_single_for_device(struct device *dev,
		dma_addr_t dma_handle, size_t size, enum dma_data_direction dir)
{
	phys_addr_t phys;
	int nid = dev->numa_node;

	if (is_pswiotlb_active(dev)) {
		phys = iommu_iova_to_phys(iommu_get_domain_for_dev(dev), dma_handle);
		if (is_pswiotlb_buffer(dev, nid, phys))
			pswiotlb_sync_single_for_device(dev, nid, phys, size, dir);
	}
}

void pswiotlb_iommu_dma_sync_sg_for_cpu(struct device *dev,
		struct scatterlist *sgl, int nelems,
		enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;
	int nid = dev->numa_node;
	dma_addr_t start_orig;
	phys_addr_t phys;
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;

	if (is_pswiotlb_active(dev)) {
		start_orig = sg_dma_address(sgl);
		for_each_sg(sgl, sg, nelems, i) {
			if (dir != DMA_TO_DEVICE) {
				unsigned int s_iova_off = iova_offset(iovad, sg->offset);

				if (i > 0)
					start_orig += s_iova_off;
				phys = iommu_iova_to_phys(iommu_get_domain_for_dev(dev),
							start_orig);
				if (!is_device_dma_coherent(dev))
					__dma_unmap_area(phys_to_virt(phys), sg->length, dir);

				if (is_pswiotlb_buffer(dev, nid, phys))
					pswiotlb_sync_single_for_cpu(dev, nid, phys,
									sg->length, dir);
				start_orig -= s_iova_off;
				start_orig += iova_align(iovad, sg->length + s_iova_off);
			} else {
				if (!is_device_dma_coherent(dev))
					__dma_unmap_area(sg_virt(sg), sg->length, dir);
			}
		}
	} else {
		if (is_device_dma_coherent(dev))
			return;

		for_each_sg(sgl, sg, nelems, i)
			__dma_unmap_area(sg_virt(sg), sg->length, dir);
	}
}

void pswiotlb_iommu_dma_sync_sg_for_device(struct device *dev,
		struct scatterlist *sgl, int nelems,
		enum dma_data_direction dir)
{
	struct scatterlist *sg;
	int i;
	int nid = dev->numa_node;

	if (is_pswiotlb_active(dev)) {
		for_each_sg(sgl, sg, nelems, i) {
			if (dir != DMA_TO_DEVICE) {
				if (is_pswiotlb_buffer(dev, nid, sg_phys(sg)))
					pswiotlb_sync_single_for_device(dev, nid, sg_phys(sg),
									   sg->length, dir);
			} else
				if (!is_device_dma_coherent(dev))
					__dma_map_area(sg_virt(sg), sg->length, dir);
		}
	} else {
		if (is_device_dma_coherent(dev))
			return;

		for_each_sg(sgl, sg, nelems, i)
			__dma_map_area(sg_virt(sg), sg->length, dir);
	}
}

dma_addr_t pswiotlb_iommu_dma_map_page(struct device *dev, struct page *page,
		unsigned long offset, size_t size, enum dma_data_direction dir,
		unsigned long attrs)
{
	phys_addr_t phys = page_to_phys(page) + offset;
	bool coherent = is_device_dma_coherent(dev);

	int prot = dma_info_to_prot(dir, coherent, attrs);
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	size_t aligned_size = size;
	dma_addr_t iova;
	int nid = dev->numa_node;

	/* check whether dma addr is in local node */
	if (is_pswiotlb_active(dev)) {
		if (dir != DMA_TO_DEVICE) {
			if (unlikely(!dma_is_in_local_node(dev, nid, phys, aligned_size))) {
				aligned_size = iova_align(iovad, size);
				phys = pswiotlb_tbl_map_single(dev, nid, phys, size,
							aligned_size, iova_mask(iovad),
							dir, attrs);
				if (phys == DMA_MAPPING_ERROR) {
					phys = page_to_phys(page) + offset;
					dev_warn_once(dev,
						"Failed to allocate memory from pswiotlb, fall back to non-local dma\n");
				}
			}
		}
	}

	if (!coherent && !(attrs & DMA_ATTR_SKIP_CPU_SYNC))
		__dma_map_area(phys_to_virt(phys), size, dir);

	iova = __iommu_dma_map(dev, phys, size, prot, domain);
	if (iova == DMA_MAPPING_ERROR && is_pswiotlb_buffer(dev, nid, phys))
		pswiotlb_tbl_unmap_single(dev, nid, phys, 0, size, dir, attrs);
	return iova;
}

void pswiotlb_iommu_dma_unmap_page(struct device *dev, dma_addr_t dma_handle,
		size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	phys_addr_t phys;
	int nid = dev->numa_node;

	phys = iommu_iova_to_phys(domain, dma_handle);
	if (WARN_ON(!phys))
		return;

	if (((attrs & DMA_ATTR_SKIP_CPU_SYNC) == 0) && !is_device_dma_coherent(dev))
		__dma_unmap_area(phys_to_virt(phys), size, dir);

	__iommu_dma_unmap(domain, dma_handle, size);

	if (is_pswiotlb_active(dev) &&
		is_pswiotlb_buffer(dev, nid, phys))
		pswiotlb_tbl_unmap_single(dev, nid, phys, 0, size, dir, attrs);
}

static void iommu_dma_unmap_page_sg(struct device *dev, dma_addr_t dma_handle,
		size_t offset, size_t size, enum dma_data_direction dir, unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	phys_addr_t phys;
	int nid = dev->numa_node;

	phys = iommu_iova_to_phys(domain, dma_handle);

	if (WARN_ON(!phys))
		return;

	if (!(attrs & DMA_ATTR_SKIP_CPU_SYNC) && !is_device_dma_coherent(dev))
		__dma_unmap_area(phys_to_virt(phys), size, dir);

	if (is_pswiotlb_buffer(dev, nid, phys))
		pswiotlb_tbl_unmap_single(dev, nid, phys, offset, size, dir, attrs);
}

/*
 * Prepare a successfully-mapped scatterlist to give back to the caller.
 *
 * At this point the segments are already laid out by pswiotlb_iommu_dma_map_sg() to
 * avoid individually crossing any boundaries, so we merely need to check a
 * segment's start address to avoid concatenating across one.
 */
static int __finalise_sg(struct device *dev, struct scatterlist *sg, int nents,
		dma_addr_t dma_addr)
{
	struct scatterlist *s, *cur = sg;
	unsigned long seg_mask = dma_get_seg_boundary(dev);
	unsigned int cur_len = 0, max_len = dma_get_max_seg_size(dev);
	int i, count = 0;

	for_each_sg(sg, s, nents, i) {
		/* Restore this segment's original unaligned fields first */
		unsigned int s_iova_off = sg_dma_address(s);
		unsigned int s_length = sg_dma_len(s);
		unsigned int s_iova_len = s->length;

		s->offset += s_iova_off;
		s->length = s_length;
		sg_dma_address(s) = DMA_MAPPING_ERROR;
		sg_dma_len(s) = 0;

		/*
		 * Now fill in the real DMA data. If...
		 * - there is a valid output segment to append to
		 * - and this segment starts on an IOVA page boundary
		 * - but doesn't fall at a segment boundary
		 * - and wouldn't make the resulting output segment too long
		 */
		if (cur_len && !s_iova_off && (dma_addr & seg_mask) &&
		    (max_len - cur_len >= s_length)) {
			/* ...then concatenate it with the previous one */
			cur_len += s_length;
		} else {
			/* Otherwise start the next output segment */
			if (i > 0)
				cur = sg_next(cur);
			cur_len = s_length;
			count++;

			sg_dma_address(cur) = dma_addr + s_iova_off;
		}

		sg_dma_len(cur) = cur_len;
		dma_addr += s_iova_len;

		if (s_length + s_iova_off < s_iova_len)
			cur_len = 0;
	}
	return count;
}

/*
 * If mapping failed, then just restore the original list,
 * but making sure the DMA fields are invalidated.
 */
static void __invalidate_sg(struct scatterlist *sg, int nents)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i) {
		if (sg_dma_address(s) != DMA_MAPPING_ERROR)
			s->offset += sg_dma_address(s);
		if (sg_dma_len(s))
			s->length = sg_dma_len(s);
		sg_dma_address(s) = DMA_MAPPING_ERROR;
		sg_dma_len(s) = 0;
	}
}

void iommu_dma_unmap_sg_pswiotlb(struct device *dev, struct scatterlist *sg,
		unsigned long iova_start, size_t mapped, int nents,
		enum dma_data_direction dir, unsigned long attrs)
{
	dma_addr_t start, start_orig;
	struct scatterlist *s;
	struct scatterlist *sg_orig = sg;
	int i;

	start = iova_start;
	start_orig = start;
	for_each_sg(sg_orig, s, nents, i) {
		if (!mapped || (start_orig > (start + mapped)))
			break;
		if (s->length == 0)
			break;
		iommu_dma_unmap_page_sg(dev, start_orig, 0,
				s->length, dir, attrs);
		start_orig += s->length;
	}
}

static void iommu_dma_unmap_sg_pswiotlb_pagesize(struct device *dev, struct scatterlist *sg,
		int nents, enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i)
		pswiotlb_iommu_dma_unmap_page(dev, sg_dma_address(s),
				sg_dma_len(s), dir, attrs);
}

static int iommu_dma_map_sg_pswiotlb_pagesize(struct device *dev, struct scatterlist *sg,
		int nents, enum dma_data_direction dir, unsigned long attrs)
{
	struct scatterlist *s;
	int i;

	for_each_sg(sg, s, nents, i) {
		sg_dma_address(s) = pswiotlb_iommu_dma_map_page(dev, sg_page(s),
				s->offset, s->length, dir, attrs);
		if (sg_dma_address(s) == DMA_MAPPING_ERROR)
			goto out_unmap;
		sg_dma_len(s) = s->length;
	}

	return nents;

out_unmap:
	iommu_dma_unmap_sg_pswiotlb_pagesize(dev, sg, i, dir, attrs | DMA_ATTR_SKIP_CPU_SYNC);
	return -EIO;
}

/*
 * The DMA API client is passing in a scatterlist which could describe
 * any old buffer layout, but the IOMMU API requires everything to be
 * aligned to IOMMU pages. Hence the need for this complicated bit of
 * impedance-matching, to be able to hand off a suitably-aligned list,
 * but still preserve the original offsets and sizes for the caller.
 */
int pswiotlb_iommu_dma_map_sg(struct device *dev, struct scatterlist *sg,
		int nents, int prot, unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	struct scatterlist *s, *prev = NULL;
	enum dma_data_direction dir = prot & (DMA_TO_DEVICE | DMA_FROM_DEVICE | DMA_BIDIRECTIONAL);
	dma_addr_t iova;
	size_t iova_len = 0;
	unsigned long mask = dma_get_seg_boundary(dev);
	ssize_t ret;
	int i;

	if (dir != DMA_TO_DEVICE && is_pswiotlb_active(dev)
				&& ((nents == 1) && (sg->length < PAGE_SIZE)))
		return iommu_dma_map_sg_pswiotlb_pagesize(dev, sg, nents, dir, attrs);

	/*
	 * Work out how much IOVA space we need, and align the segments to
	 * IOVA granules for the IOMMU driver to handle. With some clever
	 * trickery we can modify the list in-place, but reversibly, by
	 * stashing the unaligned parts in the as-yet-unused DMA fields.
	 */
	for_each_sg(sg, s, nents, i) {
		size_t s_iova_off = iova_offset(iovad, s->offset);
		size_t s_length = s->length;
		size_t pad_len = (mask - iova_len + 1) & mask;

		sg_dma_address(s) = s_iova_off;
		sg_dma_len(s) = s_length;
		s->offset -= s_iova_off;
		s_length = iova_align(iovad, s_length + s_iova_off);
		s->length = s_length;

		/*
		 * Due to the alignment of our single IOVA allocation, we can
		 * depend on these assumptions about the segment boundary mask:
		 * - If mask size >= IOVA size, then the IOVA range cannot
		 *   possibly fall across a boundary, so we don't care.
		 * - If mask size < IOVA size, then the IOVA range must start
		 *   exactly on a boundary, therefore we can lay things out
		 *   based purely on segment lengths without needing to know
		 *   the actual addresses beforehand.
		 * - The mask must be a power of 2, so pad_len == 0 if
		 *   iova_len == 0, thus we cannot dereference prev the first
		 *   time through here (i.e. before it has a meaningful value).
		 */
		if (pad_len && pad_len < s_length - 1) {
			prev->length += pad_len;
			iova_len += pad_len;
		}

		iova_len += s_length;
		prev = s;
	}

	iova = iommu_dma_alloc_iova(domain, iova_len, dma_get_mask(dev), dev);
	if (!iova)
		goto out_restore_sg;

	/*
	 * We'll leave any physical concatenation to the IOMMU driver's
	 * implementation - it knows better than we do.
	 */
	if (dir != DMA_TO_DEVICE && is_pswiotlb_active(dev))
		ret = pswiotlb_iommu_map_sg_atomic_dma(dev, domain, iova, sg, nents, prot, attrs);
	else
		ret = iommu_map_sg(domain, iova, sg, nents, prot);

	if (ret < iova_len)
		goto out_free_iova;

	return __finalise_sg(dev, sg, nents, iova);

out_free_iova:
	iommu_dma_free_iova(cookie, iova, iova_len);
out_restore_sg:
	__invalidate_sg(sg, nents);
	return 0;
}

void pswiotlb_iommu_dma_unmap_sg(struct device *dev, struct scatterlist *sg,
		int nents, enum dma_data_direction dir, unsigned long attrs)
{
	struct iommu_domain *domain = iommu_get_domain_for_dev(dev);
	struct iommu_dma_cookie *cookie = domain->iova_cookie;
	struct iova_domain *iovad = &cookie->iovad;
	dma_addr_t start, end, start_orig;
	struct scatterlist *tmp, *s;
	struct scatterlist *sg_orig = sg;
	int i;

	if ((dir != DMA_TO_DEVICE) && ((nents == 1) && (sg->length < PAGE_SIZE))) {
		iommu_dma_unmap_sg_pswiotlb_pagesize(dev, sg, nents, dir, attrs);
		return;
	}

	/*
	 * The scatterlist segments are mapped into a single
	 * contiguous IOVA allocation, so this is incredibly easy.
	 */
	start = sg_dma_address(sg);

	if (is_pswiotlb_active(dev)) {
		/* check whether dma addr is in local node */
		start_orig = start;
		if (dir != DMA_TO_DEVICE) {
			for_each_sg(sg_orig, s, nents, i) {
				unsigned int s_iova_off = iova_offset(iovad, s->offset);

				if (i > 0)
					start_orig += s_iova_off;
				iommu_dma_unmap_page_sg(dev, start_orig,
						s_iova_off, s->length,
						dir, attrs);
				start_orig -= s_iova_off;
				start_orig += iova_align(iovad, s->length + s_iova_off);
			}
		}
	}

	for_each_sg(sg_next(sg), tmp, nents - 1, i) {
		if (sg_dma_len(tmp) == 0)
			break;
		sg = tmp;
	}
	end = sg_dma_address(sg) + sg_dma_len(sg);
	__iommu_dma_unmap(domain, start, end - start);
}
