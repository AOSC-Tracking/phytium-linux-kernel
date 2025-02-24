// SPDX-License-Identifier: GPL-2.0-only
/*
 * Phytium Ethernet Controller driver
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/clk.h>
#include <linux/crc32.h>
#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/circ_buf.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/gpio.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/dma-mapping.h>
#include <linux/platform_device.h>
#include <linux/phy.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_mdio.h>
#include <linux/of_net.h>
#include <linux/ip.h>
#include <linux/udp.h>
#include <linux/tcp.h>
#include <linux/acpi.h>
#include <linux/pci.h>
#include <net/ncsi.h>
#include <linux/netdevice.h>
#include <linux/prefetch.h>
#include <linux/skbuff.h>
#include "phytmac.h"
#include "phytmac_ptp.h"

static int debug;
module_param(debug, int, 0);
MODULE_PARM_DESC(debug, "Debug level (0=none,...,16=all)");

#define RX_BUFFER_MULTIPLE	64  /* bytes */
#define MAX_MTU 3072
#define RING_ADDR_INTERVAL 128
#define MAX_RING_ADDR_ALLOC_TIMES 3

#define RX_RING_BYTES(pdata)	(sizeof(struct phytmac_dma_desc)	\
				 * (pdata)->rx_ring_size)

#define TX_RING_BYTES(pdata)	(sizeof(struct phytmac_dma_desc)\
				 * (pdata)->tx_ring_size)

/* Max length of transmit frame must be a multiple of 8 bytes */
#define PHYTMAC_TX_LEN_ALIGN		8
/* Limit maximum TX length as per Cadence TSO errata. This is to avoid a
 * false amba_error in TX path from the DMA assuming there is not enough
 * space in the SRAM (16KB) even when there is.
 */

static int phytmac_queue_phyaddr_check(struct phytmac *pdata, dma_addr_t ring_base_addr,
				       int offset)
{
	u32 bus_addr_high;
	int i;

	/* Check the high address of the DMA ring. */
	bus_addr_high = upper_32_bits(ring_base_addr);
	for (i = 1; i < pdata->queues_num; i++) {
		ring_base_addr += offset;
		if (bus_addr_high != upper_32_bits(ring_base_addr))
			return -EFAULT;
	}

	return 0;
}

static int phytmac_change_mtu(struct net_device *ndev, int new_mtu)
{
	if (netif_running(ndev))
		return -EBUSY;

	if (new_mtu > MAX_MTU) {
		netdev_info(ndev, "Can not set MTU over %d.\n", MAX_MTU);
		return -EINVAL;
	}

	ndev->mtu = new_mtu;

	return 0;
}

static inline void phytmac_hw_addr_set(struct net_device *dev, const u8 *addr)
{
	memcpy(dev->dev_addr, addr, ETH_ALEN);
}

static int phytmac_set_mac_address(struct net_device *netdev, void *addr)
{
	struct phytmac *pdata = netdev_priv(netdev);
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	struct sockaddr *saddr = addr;

	if (netif_msg_drv(pdata))
		netdev_info(netdev, "phytmac set mac address");

	if (!is_valid_ether_addr(saddr->sa_data))
		return -EADDRNOTAVAIL;

	phytmac_hw_addr_set(netdev, saddr->sa_data);

	hw_if->set_mac_address(pdata, saddr->sa_data);

	phytmac_set_bios_wol_enable(pdata, pdata->wol ? 1 : 0);

	return 0;
}

static int phytmac_get_mac_address(struct phytmac *pdata)
{
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	u8 addr[6];

	hw_if->get_mac_address(pdata, addr);

	if (is_valid_ether_addr(addr)) {
		phytmac_hw_addr_set(pdata->ndev, addr);
		return 0;
	}
	dev_info(pdata->dev, "invalid hw address, using random\n");
	eth_hw_addr_random(pdata->ndev);

	return 0;
}

static int phytmac_mdio_read(struct mii_bus *bus, int mii_id, int regnum)
{
	struct phytmac *pdata = bus->priv;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	int data;

	data = hw_if->mdio_read(pdata, mii_id, regnum, regnum & MII_ADDR_C45);

	if (netif_msg_link(pdata))
		netdev_info(pdata->ndev, "mdio read mii_id=%d, regnum=%x, data=%x\n",
			    mii_id, regnum, data);

	return data;
}

static int phytmac_mdio_write(struct mii_bus *bus, int mii_id, int regnum, u16 data)
{
	struct phytmac *pdata = bus->priv;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	int ret;

	ret = hw_if->mdio_write(pdata, mii_id, regnum, regnum & MII_ADDR_C45, data);
	if (ret)
		netdev_err(pdata->ndev, "mdio %d, reg %x, data %x write failed!\n",
			   mii_id, regnum, data);

	if (netif_msg_link(pdata))
		netdev_info(pdata->ndev, "mdio write mii_id=%d, regnum=%x, data=%x\n",
			    mii_id, regnum, data);

	return 0;
}

static inline int hash_bit_value(int bitnr, __u8 *addr)
{
	if (addr[bitnr / 8] & (1 << (bitnr % 8)))
		return 1;
	return 0;
}

/* Return the hash index value for the specified address. */
static int phytmac_get_hash_index(__u8 *addr)
{
	int i, j, bitval;
	int hash_index = 0;

	for (j = 0; j < 6; j++) {
		for (i = 0, bitval = 0; i < 8; i++)
			bitval ^= hash_bit_value(i * 6 + j, addr);

		hash_index |= (bitval << j);
	}

	return hash_index;
}

static void phytmac_set_mac_hash_table(struct net_device *ndev)
{
	struct phytmac *pdata = netdev_priv(ndev);
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	struct netdev_hw_addr *ha;
	unsigned long mc_filter[2];
	unsigned int bitnr;

	mc_filter[0] = 0;
	mc_filter[1] = 0;

	netdev_for_each_mc_addr(ha, ndev) {
		bitnr = phytmac_get_hash_index(ha->addr);
		mc_filter[bitnr >> 5] |= 1 << (bitnr & 31);
	}

	hw_if->set_hash_table(pdata, mc_filter);
}

/* Enable/Disable promiscuous and multicast modes. */
static void phytmac_set_rx_mode(struct net_device *ndev)
{
	struct phytmac *pdata = netdev_priv(ndev);
	struct phytmac_hw_if *hw_if = pdata->hw_if;

	hw_if->enable_promise(pdata, ndev->flags & IFF_PROMISC);

	hw_if->enable_multicast(pdata, ndev->flags & IFF_ALLMULTI);
	if (!netdev_mc_empty(ndev))
		phytmac_set_mac_hash_table(ndev);
}

static struct net_device_stats *phytmac_get_stats(struct net_device *dev)
{
	struct phytmac *pdata = netdev_priv(dev);
	struct net_device_stats *nstat = &pdata->ndev->stats;
	struct phytmac_stats *stat = &pdata->stats;
	struct phytmac_hw_if *hw_if = pdata->hw_if;

	if (pdata->power_state == PHYTMAC_POWEROFF)
		return nstat;

	hw_if->get_stats(pdata);

	nstat->rx_errors = (stat->rx_fcs_errors +
			    stat->rx_alignment_errors +
			    stat->rx_overruns +
			    stat->rx_oversize_packets +
			    stat->rx_jabbers +
			    stat->rx_undersized_packets +
			    stat->rx_length_errors);
	nstat->rx_dropped = stat->rx_resource_over;
	nstat->tx_errors = (stat->tx_late_collisions +
			    stat->tx_excessive_collisions +
			    stat->tx_underrun +
			    stat->tx_carrier_sense_errors);
	nstat->multicast = stat->rx_mcast_packets;
	nstat->collisions = (stat->tx_single_collisions +
			     stat->tx_multiple_collisions +
			     stat->tx_excessive_collisions +
			     stat->tx_late_collisions);
	nstat->rx_length_errors = (stat->rx_oversize_packets +
				   stat->rx_jabbers +
				   stat->rx_undersized_packets +
				   stat->rx_length_errors);
	nstat->rx_over_errors = stat->rx_resource_over;
	nstat->rx_crc_errors = stat->rx_fcs_errors;
	nstat->rx_frame_errors = stat->rx_alignment_errors;
	nstat->rx_fifo_errors = stat->rx_overruns;
	nstat->tx_aborted_errors = stat->tx_excessive_collisions;
	nstat->tx_carrier_errors = stat->tx_carrier_sense_errors;
	nstat->tx_fifo_errors = stat->tx_underrun;

	return nstat;
}

static inline int phytmac_calc_rx_buf_len(void)
{
#if (PAGE_SIZE < 8192)
	return rounddown(PHYTMAC_MAX_FRAME_BUILD_SKB, RX_BUFFER_MULTIPLE);
#endif
	return rounddown(PHYTMAC_RXBUFFER_2048, RX_BUFFER_MULTIPLE);
}

struct phytmac_dma_desc *phytmac_get_rx_desc(struct phytmac_queue *queue,
					     unsigned int index)
{
	return &queue->rx_ring[index & (queue->pdata->rx_ring_size - 1)];
}

struct phytmac_tx_skb *phytmac_get_tx_skb(struct phytmac_queue *queue,
					  unsigned int index)
{
	return &queue->tx_skb[index & (queue->pdata->tx_ring_size - 1)];
}

struct phytmac_dma_desc *phytmac_get_tx_desc(struct phytmac_queue *queue,
					     unsigned int index)
{
	return &queue->tx_ring[index & (queue->pdata->tx_ring_size - 1)];
}

static void phytmac_rx_unmap(struct phytmac_queue *queue)
{
	struct phytmac_rx_buffer *rx_buffer_info;
	struct phytmac *pdata = queue->pdata;
	int i;

	if (queue->rx_buffer_info) {
		/* Free all the Rx ring sk_buffs */
		i = queue->rx_tail;

		while (i != queue->rx_next_to_alloc) {
			rx_buffer_info = &queue->rx_buffer_info[i];

			/* Invalidate cache lines that may have been written to by
			 * device so that we avoid corrupting memory.
			 */
			dma_sync_single_range_for_cpu(pdata->dev,
						      rx_buffer_info->addr,
						      rx_buffer_info->page_offset,
						      pdata->rx_buffer_len,
						      DMA_FROM_DEVICE);

			/* free resources associated with mapping */
			dma_unmap_page_attrs(pdata->dev,
					     rx_buffer_info->addr,
					     PHYTMAC_RX_PAGE_SIZE,
					     DMA_FROM_DEVICE,
					     PHYTMAC_RX_DMA_ATTR);

			__page_frag_cache_drain(rx_buffer_info->page,
						rx_buffer_info->pagecnt_bias);

			i++;
			if (i == pdata->rx_ring_size)
				i = 0;
		}

		queue->rx_tail = 0;
		queue->rx_head = 0;
		queue->rx_next_to_alloc = 0;
	}
}

static int phytmac_free_tx_resource(struct phytmac *pdata)
{
	struct phytmac_queue *queue;
	struct phytmac_dma_desc *tx_ring_base = NULL;
	dma_addr_t tx_ring_base_addr;
	unsigned int q;
	int size;

	queue = pdata->queues;
	if (queue->tx_ring) {
		tx_ring_base = queue->tx_ring;
		tx_ring_base_addr = queue->tx_ring_addr;
	}

	for (q = 0, queue = pdata->queues; q < pdata->queues_num; ++q, ++queue) {
		kfree(queue->tx_skb);
		queue->tx_skb = NULL;

		if (queue->tx_ring)
			queue->tx_ring = NULL;
	}

	if (tx_ring_base) {
		size = pdata->queues_num * (TX_RING_BYTES(pdata) + pdata->tx_bd_prefetch +
					    RING_ADDR_INTERVAL);
		dma_free_coherent(pdata->dev, size, tx_ring_base, tx_ring_base_addr);
	}

	return 0;
}

static int phytmac_free_rx_resource(struct phytmac *pdata)
{
	struct phytmac_queue *queue;
	struct phytmac_dma_desc *rx_ring_base = NULL;
	dma_addr_t rx_ring_base_addr;
	unsigned int q;
	int size;

	queue = pdata->queues;
	if (queue->rx_ring) {
		rx_ring_base = queue->rx_ring;
		rx_ring_base_addr = queue->rx_ring_addr;
	}

	for (q = 0, queue = pdata->queues; q < pdata->queues_num; ++q, ++queue) {
		phytmac_rx_unmap(queue);

		if (queue->rx_ring)
			queue->rx_ring = NULL;

		if (queue->rx_buffer_info) {
			vfree(queue->rx_buffer_info);
			queue->rx_buffer_info = NULL;
		}
	}

	if (rx_ring_base) {
		size = pdata->queues_num * (RX_RING_BYTES(pdata) + pdata->rx_bd_prefetch +
					    RING_ADDR_INTERVAL);
		dma_free_coherent(pdata->dev, size, rx_ring_base, rx_ring_base_addr);
	}

	return 0;
}

static int phytmac_alloc_tx_resource(struct phytmac *pdata)
{
	struct phytmac_queue *queue;
	struct phytmac_dma_desc *tx_ring_base;
	dma_addr_t tx_ring_base_addr;
	unsigned int q;
	int tx_offset;
	int tx_size;
	int size = 0;
	int ret, i;

	tx_offset = TX_RING_BYTES(pdata) + pdata->tx_bd_prefetch + RING_ADDR_INTERVAL;
	tx_offset = ALIGN(tx_offset, 4096);
	tx_size = pdata->queues_num * tx_offset;
	for (i = 0; i < MAX_RING_ADDR_ALLOC_TIMES + 1; i++) {
		if (i == MAX_RING_ADDR_ALLOC_TIMES)
			goto err;

		tx_ring_base = dma_alloc_coherent(pdata->dev, tx_size,
						  &tx_ring_base_addr, GFP_KERNEL);
		if (!tx_ring_base)
			continue;

		ret = phytmac_queue_phyaddr_check(pdata, tx_ring_base_addr,
						  tx_offset);
		if (ret) {
			dma_free_coherent(pdata->dev, tx_size, tx_ring_base,
					  tx_ring_base_addr);
			continue;
		} else {
			break;
		}
	}

	for (q = 0, queue = pdata->queues; q < pdata->queues_num; ++q, ++queue) {
		queue->tx_ring = (void *)tx_ring_base + q * tx_offset;
		queue->tx_ring_addr = tx_ring_base_addr + q * tx_offset;
		if (!queue->tx_ring)
			goto err;

		if (netif_msg_drv(pdata))
			netdev_info(pdata->ndev,
				    "Allocated TX ring for queue %u of %d bytes at %08lx\n",
				    q, tx_offset, (unsigned long)queue->tx_ring_addr);

		size = pdata->tx_ring_size * sizeof(struct phytmac_tx_skb);
		queue->tx_skb = kzalloc(size, GFP_KERNEL);
		if (!queue->tx_skb)
			goto err;

		if (netif_msg_drv(pdata))
			netdev_info(pdata->ndev,
				    "Allocated %d TX struct tx_skb entries at %p\n",
				    pdata->tx_ring_size, queue->tx_skb);
	}

	return 0;
err:
	phytmac_free_tx_resource(pdata);

	return -ENOMEM;
}

static int phytmac_alloc_rx_resource(struct phytmac *pdata)
{
	struct phytmac_queue *queue;
	struct phytmac_dma_desc *rx_ring_base;
	dma_addr_t rx_ring_base_addr;
	int rx_offset;
	int rx_size;
	unsigned int q;
	int size = 0;
	int ret, i;

	rx_offset = RX_RING_BYTES(pdata) + pdata->rx_bd_prefetch + RING_ADDR_INTERVAL;
	rx_offset = ALIGN(rx_offset, 4096);
	rx_size = pdata->queues_num * rx_offset;
	for (i = 0; i < MAX_RING_ADDR_ALLOC_TIMES + 1; i++) {
		if (i == MAX_RING_ADDR_ALLOC_TIMES)
			goto err;

		rx_ring_base = dma_alloc_coherent(pdata->dev, rx_size,
						  &rx_ring_base_addr, GFP_KERNEL);
		if (!rx_ring_base)
			continue;

		ret = phytmac_queue_phyaddr_check(pdata, rx_ring_base_addr,
						  rx_offset);
		if (ret) {
			dma_free_coherent(pdata->dev, rx_size, rx_ring_base,
					  rx_ring_base_addr);
			continue;
		} else {
			break;
		}
	}

	for (q = 0, queue = pdata->queues; q < pdata->queues_num; ++q, ++queue) {
		queue->rx_ring = (void *)rx_ring_base + q * rx_offset;
		queue->rx_ring_addr = rx_ring_base_addr + q * rx_offset;
		if (!queue->rx_ring)
			goto err;

		if (netif_msg_drv(pdata))
			netdev_info(pdata->ndev,
				    "Allocated RX ring for queue %u of %d bytes at %08lx\n",
				    q, rx_offset, (unsigned long)queue->rx_ring_addr);

		size = pdata->rx_ring_size * sizeof(struct phytmac_rx_buffer);
		queue->rx_buffer_info = vzalloc(size);
		if (!queue->rx_buffer_info)
			goto err;
	}

	return 0;
err:
	phytmac_free_rx_resource(pdata);

	return -ENOMEM;
}

static int phytmac_alloc_resource(struct phytmac *pdata)
{
	int ret;

	pdata->rx_buffer_len = phytmac_calc_rx_buf_len();

	if (netif_msg_drv(pdata))
		netdev_info(pdata->ndev, "alloc resource, rx_buffer_len = %d\n",
			    pdata->rx_buffer_len);

	ret = phytmac_alloc_tx_resource(pdata);
	if (ret)
		return ret;

	ret = phytmac_alloc_rx_resource(pdata);
	if (ret) {
		phytmac_free_tx_resource(pdata);
		return ret;
	}

	return 0;
}

static void phytmac_free_resource(struct phytmac *pdata)
{
	phytmac_free_tx_resource(pdata);
	phytmac_free_rx_resource(pdata);
}

static irqreturn_t phytmac_irq(int irq, void *data)
{
	struct phytmac_queue *queue = data;
	struct phytmac *pdata = queue->pdata;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	u32 status;

	status = hw_if->get_irq(pdata, queue->index);

	if (netif_msg_intr(pdata))
		netdev_info(pdata->ndev, "phymac irq status = %x\n", status);

	if (unlikely(!status))
		return IRQ_NONE;

	while (status) {
		if (status & pdata->rx_irq_mask) {
			/* Disable RX interrupts */
			hw_if->disable_irq(pdata, queue->index, pdata->rx_irq_mask);
			hw_if->clear_irq(pdata, queue->index, PHYTMAC_INT_RX_COMPLETE);

			if (napi_schedule_prep(&queue->rx_napi))
				__napi_schedule(&queue->rx_napi);
		}

		if (status & (PHYTMAC_INT_TX_COMPLETE)) {
			/* Disable TX interrupts */
			hw_if->disable_irq(pdata, queue->index, PHYTMAC_INT_TX_COMPLETE);
			hw_if->clear_irq(pdata, queue->index, PHYTMAC_INT_TX_COMPLETE);

			if (napi_schedule_prep(&queue->tx_napi))
				__napi_schedule(&queue->tx_napi);
		}

		if (status & PHYTMAC_INT_TX_ERR)
			hw_if->clear_irq(pdata, queue->index, PHYTMAC_INT_TX_ERR);

		if (status & PHYTMAC_INT_RX_OVERRUN) {
			hw_if->clear_irq(pdata, queue->index, PHYTMAC_INT_RX_OVERRUN);
			pdata->stats.rx_overruns++;
		}
		status = hw_if->get_irq(pdata, queue->index);
	}

	return IRQ_HANDLED;
}

static irqreturn_t phytmac_intx_irq(int irq, void *data)
{
	struct phytmac *pdata = data;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	u32 irq_mask;
	int i;

	irq_mask = hw_if->get_intx_mask(pdata);

	if (unlikely(!irq_mask))
		return IRQ_NONE;

	for (i = 0; i < pdata->queues_num; i++) {
		if (irq_mask & BIT(i))
			phytmac_irq(irq, &pdata->queues[i]);
	}

	return IRQ_HANDLED;
}

static void phytmac_dump_pkt(struct phytmac *pdata, struct sk_buff *skb, bool tx)
{
	struct net_device *ndev = pdata->ndev;

	if (tx) {
		netdev_dbg(ndev, "start_xmit: queue %u len %u head %p data %p tail %p end %p\n",
			   skb->queue_mapping, skb->len, skb->head, skb->data,
			   skb_tail_pointer(skb), skb_end_pointer(skb));
	} else {
		netdev_dbg(ndev, "queue %u received skb of length %u, csum: %08x\n",
			   skb->queue_mapping, skb->len, skb->csum);
		print_hex_dump(KERN_DEBUG, " mac: ", DUMP_PREFIX_ADDRESS, 16, 1,
			       skb_mac_header(skb), 16, true);
	}

	print_hex_dump(KERN_DEBUG, "data: ", DUMP_PREFIX_OFFSET, 16, 1,
		       skb->data, skb->len, true);
}

static bool phytmac_alloc_mapped_page(struct phytmac *pdata,
				      struct phytmac_rx_buffer *bi)
{
	struct page *page = bi->page;
	dma_addr_t dma;

	/* since we are recycling buffers we should seldom need to alloc */
	if (likely(page))
		return true;

	/* alloc new page for storage */
	page = __dev_alloc_pages(PHYTMAC_GFP_FLAGS, PHYTMAC_RX_PAGE_ORDER);
	if (unlikely(!page)) {
		netdev_err(pdata->ndev, "rx alloc page failed\n");
		return false;
	}

	/* map page for use */
	dma = dma_map_page_attrs(pdata->dev, page, 0,
				 PHYTMAC_RX_PAGE_SIZE,
				 DMA_FROM_DEVICE, PHYTMAC_RX_DMA_ATTR);
	if (dma_mapping_error(pdata->dev, dma)) {
		__free_pages(page, PHYTMAC_RX_PAGE_ORDER);
		return false;
	}

	bi->addr = dma;
	bi->page = page;
	bi->page_offset = PHYTMAC_SKB_PAD;
	bi->pagecnt_bias = 1;

	return true;
}

static inline bool phytmac_page_is_reserved(struct page *page)
{
	return (page_to_nid(page) != numa_mem_id()) || page_is_pfmemalloc(page);
}

static bool phytmac_can_reuse_rx_page(struct phytmac_rx_buffer *rx_buffer)
{
	unsigned int pagecnt_bias = rx_buffer->pagecnt_bias;
	struct page *page = rx_buffer->page;

	/* avoid re-using remote pages */
	if (unlikely(phytmac_page_is_reserved(page)))
		return false;

#if (PAGE_SIZE < 8192)
	/* if we are only owner of page we can reuse it */
	if (unlikely((page_ref_count(page) - pagecnt_bias) > 1))
		return false;
#else
#define PHYTMAC_LAST_OFFSET \
	(SKB_WITH_OVERHEAD(PAGE_SIZE) - PHYTMAC_RXBUFFER_2048)

	if (rx_buffer->page_offset > PHYTMAC_LAST_OFFSET)
		return false;
#endif

	/* If we have drained the page fragment pool we need to update
	 * the pagecnt_bias and page count so that we fully restock the
	 * number of references the driver holds.
	 */
	if (unlikely(!pagecnt_bias)) {
		page_ref_add(page, USHRT_MAX);
		rx_buffer->pagecnt_bias = USHRT_MAX;
	}

	return true;
}

static void phytmac_reuse_rx_page(struct phytmac_queue *queue,
				  struct phytmac_rx_buffer *old_buff)
{
	struct phytmac_rx_buffer *new_buff;
	struct phytmac *pdata = queue->pdata;
	u16 nta = queue->rx_next_to_alloc;

	new_buff = &queue->rx_buffer_info[nta & (pdata->rx_ring_size - 1)];

	/* update, and store next to alloc */
	nta++;
	queue->rx_next_to_alloc = (nta < pdata->rx_ring_size) ? nta : 0;

	/* Transfer page from old buffer to new buffer.
	 * Move each member individually to avoid possible store
	 * forwarding stalls.
	 */
	new_buff->addr		= old_buff->addr;
	new_buff->page		= old_buff->page;
	new_buff->page_offset	= old_buff->page_offset;
	new_buff->pagecnt_bias	= old_buff->pagecnt_bias;
}

static struct phytmac_rx_buffer *phytmac_get_rx_buffer(struct phytmac_queue *queue,
						       unsigned int index,
						       const unsigned int size)
{
	struct phytmac_rx_buffer *rx_buffer;
	struct phytmac *pdata = queue->pdata;

	rx_buffer = &queue->rx_buffer_info[index & (pdata->rx_ring_size - 1)];
	prefetchw(rx_buffer->page);

	/* we are reusing so sync this buffer for CPU use */
	dma_sync_single_range_for_cpu(pdata->dev,
				      rx_buffer->addr,
				      rx_buffer->page_offset,
				      size,
				      DMA_FROM_DEVICE);

	rx_buffer->pagecnt_bias--;

	return rx_buffer;
}

static void  phytmac_put_rx_buffer(struct phytmac_queue *queue,
				   struct phytmac_rx_buffer *rx_buffer)
{
	struct phytmac *pdata = queue->pdata;

	if (phytmac_can_reuse_rx_page(rx_buffer)) {
		/* hand second half of page back to the ring */
		phytmac_reuse_rx_page(queue, rx_buffer);
	}  else {
		dma_unmap_page_attrs(pdata->dev, rx_buffer->addr,
				     PHYTMAC_RX_PAGE_SIZE,
				     DMA_FROM_DEVICE, PHYTMAC_RX_DMA_ATTR);
		__page_frag_cache_drain(rx_buffer->page,
					rx_buffer->pagecnt_bias);
	}

	/* clear contents of rx_buffer */
	rx_buffer->page = NULL;
}

static void phytmac_add_rx_frag(struct phytmac_queue *queue,
				struct phytmac_rx_buffer *rx_buffer,
				struct sk_buff *skb,
				unsigned int size)
{
	unsigned int truesize;

#if (PAGE_SIZE < 8192)
	truesize = PHYTMAC_RX_PAGE_SIZE / 2;
#else
	truesize = SKB_DATA_ALIGN(PHYTMAC_SKB_PAD + size);
#endif

	skb_add_rx_frag(skb, skb_shinfo(skb)->nr_frags, rx_buffer->page,
			rx_buffer->page_offset, size, truesize);
#if (PAGE_SIZE < 8192)
	rx_buffer->page_offset ^= truesize;
#else
	rx_buffer->page_offset += truesize;
#endif
}

static struct sk_buff *phytmac_build_skb(struct phytmac_rx_buffer *rx_buffer,
					 unsigned int size)
{
	struct sk_buff *skb;
	unsigned int truesize;
	void *va;

#if (PAGE_SIZE < 8192)
	truesize = PHYTMAC_RX_PAGE_SIZE / 2;
#else
	truesize = SKB_DATA_ALIGN(sizeof(struct skb_shared_info)) +
				  SKB_DATA_ALIGN(PHYTMAC_SKB_PAD + size);
#endif

	va = page_address(rx_buffer->page) + rx_buffer->page_offset;
	/* prefetch first cache line of first page */
	prefetch(va);

	/* build an skb around the page buffer */
	skb = build_skb(va - PHYTMAC_SKB_PAD, truesize);
	if (unlikely(!skb))
		return NULL;

	/* update pointers within the skb to store the data */
	skb_reserve(skb, PHYTMAC_SKB_PAD);
	__skb_put(skb, size);

	/* update buffer offset */
#if (PAGE_SIZE < 8192)
	rx_buffer->page_offset ^= truesize;
#else
	rx_buffer->page_offset += truesize;
#endif

	return skb;
}

static struct sk_buff *phytmac_rx_single(struct phytmac_queue *queue, struct phytmac_dma_desc *desc)
{
	struct phytmac *pdata = queue->pdata;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	struct phytmac_rx_buffer *rx_buffer;
	struct sk_buff *skb = NULL;
	unsigned int len;

	len = hw_if->get_rx_pkt_len(pdata, desc);
	rx_buffer = phytmac_get_rx_buffer(queue, queue->rx_tail, len);
	hw_if->zero_rx_desc_addr(desc);

	skb = phytmac_build_skb(rx_buffer, len);
	if (unlikely(!skb)) {
		netdev_err(pdata->ndev,
			   "rx single build skb failed\n");
		pdata->ndev->stats.rx_dropped++;
		queue->stats.rx_dropped++;
		rx_buffer->pagecnt_bias++;
		return NULL;
	}

	phytmac_put_rx_buffer(queue, rx_buffer);

	skb->protocol = eth_type_trans(skb, pdata->ndev);
	skb_checksum_none_assert(skb);

	if (pdata->ndev->features & NETIF_F_RXCSUM &&
	    !(pdata->ndev->flags & IFF_PROMISC) &&
	    hw_if->rx_checksum(desc))
		skb->ip_summed = CHECKSUM_UNNECESSARY;

	if (netif_msg_pktdata(pdata))
		phytmac_dump_pkt(pdata, skb, false);

	return skb;
}

static struct sk_buff *phytmac_rx_frame(struct phytmac_queue *queue,
					unsigned int first_frag, unsigned int last_frag,
					unsigned int total_len)
{
	unsigned int frag = 0;
	struct sk_buff *skb;
	struct phytmac_dma_desc *desc;
	struct phytmac *pdata = queue->pdata;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	unsigned int frag_len = pdata->rx_buffer_len;
	unsigned int offset = frag_len;
	struct phytmac_rx_buffer *rx_buffer;

	if (netif_msg_drv(pdata))
		netdev_info(pdata->ndev, "rx frame %u - %u (len %u)\n",
			    first_frag, last_frag, total_len);

	desc = phytmac_get_rx_desc(queue, first_frag);
	rx_buffer = phytmac_get_rx_buffer(queue, first_frag, frag_len);
	hw_if->zero_rx_desc_addr(desc);

	skb = phytmac_build_skb(rx_buffer, frag_len);
	if (unlikely(!skb)) {
		netdev_err(pdata->ndev, "rx frame build skb failed\n");
		pdata->ndev->stats.rx_dropped++;
		queue->stats.rx_dropped++;
		rx_buffer->pagecnt_bias++;
		return NULL;
	}

	phytmac_put_rx_buffer(queue, rx_buffer);

	for (frag = first_frag + 1; ; frag++) {
		desc = phytmac_get_rx_desc(queue, frag);
		rx_buffer = phytmac_get_rx_buffer(queue, frag, frag_len);
		hw_if->zero_rx_desc_addr(desc);

		if (offset + frag_len > total_len) {
			if (unlikely(frag != last_frag)) {
				dev_kfree_skb_any(skb);
				phytmac_put_rx_buffer(queue, rx_buffer);
				return NULL;
			}
			frag_len = total_len - offset;
		}

		phytmac_add_rx_frag(queue, rx_buffer, skb, frag_len);
		phytmac_put_rx_buffer(queue, rx_buffer);

		offset += frag_len;

		if (frag == last_frag)
			break;
	}

	skb_checksum_none_assert(skb);

	if (pdata->ndev->features & NETIF_F_RXCSUM &&
	    !(pdata->ndev->flags & IFF_PROMISC) &&
	    hw_if->rx_checksum(phytmac_get_rx_desc(queue, last_frag)))
		skb->ip_summed = CHECKSUM_UNNECESSARY;

	skb->protocol = eth_type_trans(skb, pdata->ndev);
	if (netif_msg_pktdata(pdata))
		phytmac_dump_pkt(pdata, skb, false);

	return skb;
}

static struct sk_buff *phytmac_rx_mbuffer(struct phytmac_queue *queue)
{
	struct phytmac *pdata = queue->pdata;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	struct phytmac_dma_desc *desc;
	struct sk_buff *skb = NULL;
	unsigned int rx_tail = 0;
	int first_frag = -1;
	unsigned int len;

	for (rx_tail = queue->rx_tail; ; rx_tail++) {
		desc = phytmac_get_rx_desc(queue, rx_tail);
		if (!hw_if->rx_complete(desc))
			return NULL;

		if (hw_if->rx_pkt_start(desc)) {
			if (first_frag != -1)
				hw_if->clear_rx_desc(queue, first_frag, rx_tail);
			first_frag = rx_tail;
			continue;
		}

		if (hw_if->rx_pkt_end(desc)) {
			queue->rx_tail = rx_tail;
			len = hw_if->get_rx_pkt_len(pdata, desc);
			skb = phytmac_rx_frame(queue, first_frag, rx_tail, len);
			first_frag = -1;
			break;
		}
	}
	return skb;
}

static void phytmac_rx_clean(struct phytmac_queue *queue)
{
	struct phytmac *pdata = queue->pdata;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	unsigned int index, space;
	struct phytmac_rx_buffer *rx_buf_info;

	space = CIRC_SPACE(queue->rx_head, queue->rx_tail,
			   pdata->rx_ring_size);

	while (space > 0) {
		index = queue->rx_head & (pdata->rx_ring_size - 1);
		rx_buf_info = &queue->rx_buffer_info[index];

		if (!phytmac_alloc_mapped_page(pdata, rx_buf_info))
			break;
		/* sync the buffer for use by the device */
		dma_sync_single_range_for_device(pdata->dev, rx_buf_info->addr,
						 rx_buf_info->page_offset,
						 pdata->rx_buffer_len,
						 DMA_FROM_DEVICE);

		hw_if->rx_map(queue, index, rx_buf_info->addr + rx_buf_info->page_offset);

		queue->rx_head++;
		if (queue->rx_head >= pdata->rx_ring_size)
			queue->rx_head &= (pdata->rx_ring_size - 1);

		space--;
	}

	queue->rx_next_to_alloc = queue->rx_head;
	/* make newly descriptor to hardware */
	wmb();
}

static int phytmac_rx(struct phytmac_queue *queue, struct napi_struct *napi,
		      int budget)
{
	struct phytmac *pdata = queue->pdata;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	struct sk_buff *skb;
	struct phytmac_dma_desc *desc;
	int count = 0;

	while (count < budget) {
		desc = phytmac_get_rx_desc(queue, queue->rx_tail);
		/* make newly desc to cpu */
		rmb();

		if (!hw_if->rx_complete(desc))
			break;
		/* Ensure ctrl is at least as up-to-date as rxused */
		 dma_rmb();

		if (hw_if->rx_single_buffer(desc))
			skb = phytmac_rx_single(queue, desc);
		else
			skb = phytmac_rx_mbuffer(queue);

		if (!skb) {
			netdev_warn(pdata->ndev, "phytmac rx skb is NULL\n");
			break;
		}

		pdata->ndev->stats.rx_packets++;
		queue->stats.rx_packets++;
		pdata->ndev->stats.rx_bytes += skb->len;
		queue->stats.rx_bytes += skb->len;
		queue->rx_tail = (queue->rx_tail + 1) & (pdata->rx_ring_size - 1);

		count++;

		if (IS_REACHABLE(CONFIG_PHYTMAC_ENABLE_PTP))
			phytmac_ptp_rxstamp(pdata, skb, desc);

		napi_gro_receive(napi, skb);
	}

	phytmac_rx_clean(queue);

	return count;
}

static void phytmac_tx_unmap(struct phytmac *pdata, struct phytmac_tx_skb *tx_skb, int budget)
{
	if (tx_skb->addr) {
		if (tx_skb->mapped_as_page)
			dma_unmap_page(pdata->dev, tx_skb->addr,
				       tx_skb->length, DMA_TO_DEVICE);
		else
			dma_unmap_single(pdata->dev, tx_skb->addr,
					 tx_skb->length, DMA_TO_DEVICE);
		tx_skb->addr = 0;
	}

	if (tx_skb->skb) {
		napi_consume_skb(tx_skb->skb, budget);
		tx_skb->skb = NULL;
	}
}

static int phytmac_maybe_stop_tx_queue(struct phytmac_queue *queue,
				       unsigned int count)
{
	struct phytmac *pdata = queue->pdata;
	int space = CIRC_SPACE(queue->tx_tail, queue->tx_head,
				pdata->tx_ring_size);

	if (space < count) {
		if (netif_msg_drv(pdata))
			netdev_info(pdata->ndev, "Tx queue %d stopped, not enough descriptors available\n",
				    queue->index);

		netif_stop_subqueue(pdata->ndev, queue->index);

		return NETDEV_TX_BUSY;
	}

	return 0;
}

static int phytmac_maybe_wake_tx_queue(struct phytmac_queue *queue)
{
	struct phytmac *pdata = queue->pdata;
	int space = CIRC_CNT(queue->tx_tail, queue->tx_head,
			     pdata->tx_ring_size);

	return (space <= (3 * pdata->tx_ring_size / 4)) ? 1 : 0;
}

static int phytmac_tx_clean(struct phytmac_queue *queue, int budget)
{
	struct phytmac *pdata = queue->pdata;
	u16 queue_index = queue->index;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	struct phytmac_tx_skb *tx_skb;
	struct phytmac_dma_desc *desc;
	int complete = 0;
	int packet_count = 0;
	unsigned int tail = queue->tx_tail;
	unsigned int head;

	spin_lock(&pdata->lock);

	for (head = queue->tx_head; head != tail && packet_count < budget; ) {
		struct sk_buff *skb;

		desc = phytmac_get_tx_desc(queue, head);
		/* make newly desc to cpu */
		rmb();

		if (!hw_if->tx_complete(desc))
			break;

		  /* Process all buffers of the current transmitted frame */
		for (;; head++) {
			tx_skb = phytmac_get_tx_skb(queue, head);
			skb = tx_skb->skb;
			if (skb) {
				complete = 1;
				if (IS_REACHABLE(CONFIG_PHYTMAC_ENABLE_PTP)) {
					if (unlikely(skb_shinfo(skb)->tx_flags &
						     SKBTX_HW_TSTAMP) &&
						     !phytmac_ptp_one_step(skb)) {
						phytmac_ptp_txstamp(queue, skb, desc);
					}
				}

				if (netif_msg_drv(pdata))
					netdev_info(pdata->ndev, "desc %u (data %p) tx complete\n",
						    head, tx_skb->skb->data);

				pdata->ndev->stats.tx_packets++;
				queue->stats.tx_packets++;
				pdata->ndev->stats.tx_bytes += tx_skb->skb->len;
				queue->stats.tx_bytes += tx_skb->skb->len;
				packet_count++;
			}

			  /* Now we can safely release resources */
			phytmac_tx_unmap(pdata, tx_skb, budget);

			if (complete) {
				complete = 0;
				break;
			}
		}

		head++;
		if (head >= pdata->tx_ring_size)
			head &= (pdata->tx_ring_size - 1);
	}

	queue->tx_head = head;
	if (__netif_subqueue_stopped(pdata->ndev, queue_index) &&
	    (phytmac_maybe_wake_tx_queue(queue)))
		netif_wake_subqueue(pdata->ndev, queue_index);
	spin_unlock(&pdata->lock);

	return packet_count;
}

static int phytmac_rx_poll(struct napi_struct *napi, int budget)
{
	struct phytmac_queue *queue = container_of(napi, struct phytmac_queue, rx_napi);
	struct phytmac *pdata = queue->pdata;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	struct phytmac_dma_desc *desc;
	int work_done;

	work_done = phytmac_rx(queue, napi, budget);

	if (netif_msg_drv(pdata))
		netdev_info(pdata->ndev, "RX poll: queue = %u, work_done = %d, budget = %d\n",
			    (unsigned int)(queue->index), work_done, budget);
	if (work_done < budget && napi_complete_done(napi, work_done)) {
		hw_if->enable_irq(pdata, queue->index, pdata->rx_irq_mask);

		desc = phytmac_get_rx_desc(queue, queue->rx_tail);
		/* make newly desc to cpu */
		rmb();

		if (hw_if->rx_complete(desc)) {
			hw_if->disable_irq(pdata, queue->index, pdata->rx_irq_mask);
			hw_if->clear_irq(pdata, queue->index, PHYTMAC_INT_RX_COMPLETE);

			napi_schedule(napi);
		}
	}

	return work_done;
}

static int phytmac_tx_poll(struct napi_struct *napi, int budget)
{
	struct phytmac_queue *queue = container_of(napi, struct phytmac_queue, tx_napi);
	struct phytmac *pdata = queue->pdata;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	struct phytmac_dma_desc *desc;
	int work_done;

	work_done = phytmac_tx_clean(queue, budget);

	if (netif_msg_drv(pdata))
		netdev_info(pdata->ndev, "TX poll: queue = %u, work_done = %d, budget = %d\n",
			    (unsigned int)(queue->index), work_done, budget);
	if (work_done < budget && napi_complete_done(napi, work_done)) {
		hw_if->enable_irq(pdata, queue->index, PHYTMAC_INT_TX_COMPLETE);
		if (queue->tx_head != queue->tx_tail) {
			desc = phytmac_get_tx_desc(queue, queue->tx_head);
			/* make newly desc to cpu */
			rmb();

			if (hw_if->tx_complete(desc)) {
				hw_if->disable_irq(pdata, queue->index, PHYTMAC_INT_TX_COMPLETE);
				hw_if->clear_irq(pdata, queue->index, PHYTMAC_INT_TX_COMPLETE);

				napi_schedule(napi);
			}
		}
	}

	return work_done;
}

static inline int phytmac_clear_csum(struct sk_buff *skb)
{
	if (skb->ip_summed != CHECKSUM_PARTIAL)
		return 0;

	/* make sure we can modify the header */
	if (unlikely(skb_cow_head(skb, 0)))
		return -1;

	*(__sum16 *)(skb_checksum_start(skb) + skb->csum_offset) = 0;
	return 0;
}

static int phytmac_add_fcs(struct sk_buff **skb, struct net_device *ndev)
{
	bool cloned = skb_cloned(*skb) || skb_header_cloned(*skb) ||
				  skb_is_nonlinear(*skb);
	int padlen = ETH_ZLEN - (*skb)->len;
	int headroom = skb_headroom(*skb);
	int tailroom = skb_tailroom(*skb);
	struct sk_buff *nskb;
	u32 fcs;
	int i;

	if ((ndev->features & NETIF_F_HW_CSUM) ||
	    !((*skb)->ip_summed != CHECKSUM_PARTIAL) ||
	    skb_shinfo(*skb)->gso_size || phytmac_ptp_one_step(*skb))
		return 0;

	if (padlen <= 0) {
		if (tailroom >= ETH_FCS_LEN)
			goto add_fcs;
		else if (!cloned && headroom + tailroom >= ETH_FCS_LEN)
			padlen = 0;
		else
			padlen = ETH_FCS_LEN;
	} else {
		padlen += ETH_FCS_LEN;
	}

	if (!cloned && headroom + tailroom >= padlen) {
		(*skb)->data = memmove((*skb)->head, (*skb)->data, (*skb)->len);
		skb_set_tail_pointer(*skb, (*skb)->len);
	} else {
		nskb = skb_copy_expand(*skb, 0, padlen, GFP_ATOMIC);
		if (!nskb)
			return -ENOMEM;

		dev_consume_skb_any(*skb);
		*skb = nskb;
	}

	if (padlen > ETH_FCS_LEN)
		skb_put_zero(*skb, padlen - ETH_FCS_LEN);

add_fcs:
	fcs = crc32_le(~0, (*skb)->data, (*skb)->len);
	fcs = ~fcs;

	for (i = 0; i < 4; ++i)
		skb_put_u8(*skb, (fcs >> (i * 8)) & 0xff);
	return 0;
}

static int phytmac_packet_info(struct phytmac *pdata,
			       struct phytmac_queue *queue, struct sk_buff *skb,
			       struct packet_info *packet)
{
	int is_lso;
	unsigned int hdrlen, f;
	int desc_cnt;

	memset(packet, 0, sizeof(struct packet_info));

	is_lso = (skb_shinfo(skb)->gso_size != 0);

	if (is_lso) {
		/* length of headers */
		if (ip_hdr(skb)->protocol == IPPROTO_UDP) {
			/* only queue eth + ip headers separately for UDP */
			hdrlen = skb_transport_offset(skb);
			packet->lso = LSO_UFO;
			packet->mss = skb_shinfo(skb)->gso_size + hdrlen + ETH_FCS_LEN;
		} else {
			hdrlen = skb_transport_offset(skb) + tcp_hdrlen(skb);
			packet->lso = LSO_TSO;
			packet->mss = skb_shinfo(skb)->gso_size;
		}

		if (skb_headlen(skb) < hdrlen) {
			dev_err(pdata->dev, "Error - LSO headers fragmented!!!\n");
			return NETDEV_TX_BUSY;
		}
	} else {
		hdrlen = min(skb_headlen(skb), pdata->max_tx_length);
		packet->lso = 0;
		packet->mss = 0;
	}

	packet->hdrlen = hdrlen;

	if (is_lso && (skb_headlen(skb) > hdrlen))
		desc_cnt = TXD_USE_COUNT(pdata, (skb_headlen(skb) - hdrlen)) + 1;
	else
		desc_cnt = TXD_USE_COUNT(pdata, hdrlen);

	for (f = 0; f < skb_shinfo(skb)->nr_frags; f++)
		desc_cnt += TXD_USE_COUNT(pdata, skb_frag_size(&skb_shinfo(skb)->frags[f]));
	packet->desc_cnt = desc_cnt;

	if ((!(pdata->ndev->features & NETIF_F_HW_CSUM)) &&
	    skb->ip_summed != CHECKSUM_PARTIAL &&
	    !is_lso &&
	    !phytmac_ptp_one_step(skb))
		packet->nocrc = 1;
	else
		packet->nocrc = 0;

	if (netif_msg_pktdata(pdata)) {
		netdev_info(pdata->ndev, "packet info: desc_cnt=%d, nocrc=%d,ip_summed=%d\n",
			    desc_cnt, packet->nocrc, skb->ip_summed);
		netdev_info(pdata->ndev, "packet info: mss=%d, lso=%d,skb_len=%d, nr_frags=%d\n",
			    packet->mss, packet->lso, skb->len, skb_shinfo(skb)->nr_frags);
	}

	return 0;
}

static unsigned int phytmac_tx_map(struct phytmac *pdata,
				   struct phytmac_queue *queue,
				   struct sk_buff *skb,
				   struct packet_info *packet)
{
	dma_addr_t mapping;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	unsigned int len, i, tx_tail;
	struct phytmac_tx_skb *tx_skb = NULL;
	unsigned int offset, size, count = 0;
	unsigned int f, nr_frags = skb_shinfo(skb)->nr_frags;

	len = skb_headlen(skb);
	size = packet->hdrlen;

	offset = 0;
	tx_tail = queue->tx_tail;
	while (len) {
		tx_skb = phytmac_get_tx_skb(queue, tx_tail);

		mapping = dma_map_single(pdata->dev,
					 skb->data + offset,
					 size, DMA_TO_DEVICE);
		if (dma_mapping_error(pdata->dev, mapping))
			goto dma_error;

		/* Save info to properly release resources */
		tx_skb->skb = NULL;
		tx_skb->addr = mapping;
		tx_skb->length = size;
		tx_skb->mapped_as_page = false;

		len -= size;
		offset += size;
		count++;
		tx_tail++;

		size = min(len, pdata->max_tx_length);
	}

	/* Then, map paged data from fragments */
	for (f = 0; f < nr_frags; f++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[f];

		len = skb_frag_size(frag);
		offset = 0;
		while (len) {
			size = min(len, pdata->max_tx_length);
			tx_skb = phytmac_get_tx_skb(queue, tx_tail);
			mapping = skb_frag_dma_map(pdata->dev, frag,
						   offset, size, DMA_TO_DEVICE);
			if (dma_mapping_error(pdata->dev, mapping))
				goto dma_error;

			/* Save info to properly release resources */
			tx_skb->skb = NULL;
			tx_skb->addr = mapping;
			tx_skb->length = size;
			tx_skb->mapped_as_page = true;

			len -= size;
			offset += size;
			count++;
			tx_tail++;
		}
	}

	/* Should never happen */
	if (unlikely(!tx_skb)) {
		netdev_err(pdata->ndev, "BUG! empty skb!\n");
		return 0;
	}

	/* This is the last buffer of the frame: save socket buffer */
	tx_skb->skb = skb;

	if (hw_if->tx_map(queue, tx_tail, packet)) {
		netdev_err(pdata->ndev, "BUG!hw tx map failed!\n");
		return 0;
	}

	queue->tx_tail = tx_tail & (pdata->tx_ring_size - 1);

	return count;

dma_error:
	netdev_err(pdata->ndev, "TX DMA map failed\n");

	for (i = queue->tx_tail; i != tx_tail; i++) {
		tx_skb = phytmac_get_tx_skb(queue, i);
		phytmac_tx_unmap(pdata, tx_skb, 0);
	}

	return 0;
}

static inline void phytmac_init_ring(struct phytmac *pdata)
{
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	struct phytmac_queue *queue;
	unsigned int q = 0;
	int i;

	for (queue = pdata->queues; q < pdata->queues_num; ++q) {
		queue->tx_head = 0;
		queue->tx_tail = 0;
		hw_if->clear_tx_desc(queue);

		for (i = 0; i < pdata->rx_ring_size; i++)
			hw_if->init_rx_map(queue, i);
		queue->rx_head = 0;
		queue->rx_tail = 0;
		queue->rx_next_to_alloc = 0;
		phytmac_rx_clean(queue);
		++queue;
	}

	hw_if->init_ring_hw(pdata);
}

static netdev_tx_t phytmac_start_xmit(struct sk_buff *skb, struct net_device *ndev)
{
	struct phytmac *pdata = netdev_priv(ndev);
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	u16 queue_index = skb->queue_mapping;
	struct phytmac_queue *queue = &pdata->queues[queue_index];
	netdev_tx_t ret = NETDEV_TX_OK;
	struct packet_info packet;
	unsigned long flags;

	if (phytmac_clear_csum(skb)) {
		dev_kfree_skb_any(skb);
		return ret;
	}

	if (phytmac_add_fcs(&skb, ndev)) {
		dev_kfree_skb_any(skb);
		return ret;
	}

	ret = phytmac_packet_info(pdata, queue, skb, &packet);
	if (ret) {
		dev_kfree_skb_any(skb);
		return ret;
	}

	if (netif_msg_pktdata(pdata))
		phytmac_dump_pkt(pdata, skb, true);

	spin_lock_irqsave(&pdata->lock, flags);
	/* Check that there are enough descriptors available */
	ret = phytmac_maybe_stop_tx_queue(queue, packet.desc_cnt);
	if (ret)
		goto tx_return;

	/* Map socket buffer for DMA transfer */
	if (!phytmac_tx_map(pdata, queue, skb, &packet)) {
		dev_kfree_skb_any(skb);
		goto tx_return;
	}

	skb_tx_timestamp(skb);
	/* Make newly descriptor to hardware */
	wmb();

	hw_if->transmit(queue);

tx_return:
	spin_unlock_irqrestore(&pdata->lock, flags);
	return ret;
}

static bool phytmac_phy_connect_need(struct phytmac *pdata)
{
	struct fwnode_handle *fwnode = dev_fwnode(pdata->dev);
	struct fwnode_handle *dn;
	u8 pl_cfg_link_an_mode = 0;

	/* Fixed links is handled without needing a PHY */
	dn = fwnode_get_named_child_node(fwnode, "fixed-link");
	if (dn || fwnode_property_present(fwnode, "fixed-link"))
		pl_cfg_link_an_mode = MLO_AN_FIXED;
	fwnode_handle_put(dn);

	if (pl_cfg_link_an_mode == MLO_AN_FIXED ||
	    phy_interface_mode_is_8023z(pdata->phy_interface))
		return false;
	return true;
}

static int phytmac_phylink_connect(struct phytmac *pdata)
{
	struct net_device *ndev = pdata->ndev;
	struct phy_device *phydev;
	struct fwnode_handle *fwnode = dev_fwnode(pdata->dev);
	struct device_node *of_node = dev_of_node(pdata->dev);
	int ret = 0;

	if (of_node)
		ret = phylink_of_phy_connect(pdata->phylink, of_node, 0);

	if (!of_node && fwnode)
		ret = phytmac_phy_connect_need(pdata);

	if (!fwnode || ret) {
		if (pdata->mii_bus) {
			phydev = phy_find_first(pdata->mii_bus);
			if (!phydev) {
				dev_err(pdata->dev, "no PHY found\n");
				return -ENXIO;
			}
			/* attach the mac to the phy */
			ret = phylink_connect_phy(pdata->phylink, phydev);
		} else {
			netdev_err(ndev, "Not mii register\n");
			return -ENXIO;
		}
	}

	if (ret) {
		netdev_err(ndev, "Could not attach PHY (%d)\n", ret);
		return ret;
	}

	return 0;
}

int phytmac_pcs_config(struct phylink_pcs *pcs, unsigned int mode,
		       phy_interface_t interface,
		       const unsigned long *advertising,
		       bool permit_pause_to_mac)
{
	return 0;
}

void phytmac_pcs_link_up(struct phylink_pcs *pcs, unsigned int mode,
			 phy_interface_t interface, int speed, int duplex)
{
	struct phytmac *pdata = container_of(pcs, struct phytmac, phylink_pcs);
	struct phytmac_hw_if *hw_if = pdata->hw_if;

	if (netif_msg_link(pdata))
		netdev_info(pdata->ndev, "pcs link up, interface = %s, speed = %d, duplex = %d\n",
			    phy_modes(interface), speed, duplex);
	hw_if->pcs_linkup(pdata, interface, speed, duplex);
}

static const struct phylink_pcs_ops phytmac_pcs_phylink_ops = {
	.pcs_config = phytmac_pcs_config,
	.pcs_link_up = phytmac_pcs_link_up,
};

static int phytmac_mac_prepare(struct phylink_config *config, unsigned int mode,
			       phy_interface_t interface)
{
	struct phytmac *pdata = netdev_priv(to_net_dev(config->dev));

	if (interface == PHY_INTERFACE_MODE_USXGMII ||
	    interface == PHY_INTERFACE_MODE_10GBASER ||
	    interface == PHY_INTERFACE_MODE_SGMII ||
	    interface == PHY_INTERFACE_MODE_1000BASEX ||
	    interface == PHY_INTERFACE_MODE_2500BASEX) {
		pdata->phylink_pcs.ops = &phytmac_pcs_phylink_ops;
	} else {
		pdata->phylink_pcs.ops = NULL;
	}

	if (pdata->phylink_pcs.ops)
		phylink_set_pcs(pdata->phylink, &pdata->phylink_pcs);

	return 0;
}

static void phytmac_mac_config(struct phylink_config *config, unsigned int mode,
			       const struct phylink_link_state *state)
{
	struct phytmac *pdata = netdev_priv(to_net_dev(config->dev));
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	unsigned long flags;
	bool rx_pause;

	if (netif_msg_link(pdata)) {
		netdev_info(pdata->ndev, "mac config interface=%s, mode=%d\n",
			    phy_modes(state->interface), mode);
	}

	pdata->speed = state->speed;
	pdata->duplex = state->duplex;
	rx_pause = !!(state->pause & MLO_PAUSE_RX);

	spin_lock_irqsave(&pdata->lock, flags);
	hw_if->mac_config(pdata, mode, state);

	if (rx_pause != pdata->pause) {
		hw_if->enable_pause(pdata, rx_pause);
		pdata->pause = rx_pause;
	}
	spin_unlock_irqrestore(&pdata->lock, flags);
}

static void phytmac_mac_an_restart(struct phylink_config *config)
{
}

static void phytmac_mac_link_down(struct phylink_config *config, unsigned int mode,
				  phy_interface_t interface)
{
	struct net_device *ndev = to_net_dev(config->dev);
	struct phytmac *pdata = netdev_priv(ndev);
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	struct phytmac_queue *queue;
	unsigned int q;
	unsigned long flags;
	struct phytmac_tx_skb *tx_skb;
	int i;

	if (netif_msg_link(pdata)) {
		netdev_info(ndev, "link down interface:%s, mode=%d\n",
			    phy_modes(interface), mode);
	}

	if (pdata->use_ncsi)
		ncsi_stop_dev(pdata->ncsidev);

	spin_lock_irqsave(&pdata->lock, flags);
	for (q = 0, queue = pdata->queues; q < pdata->queues_num; ++q, ++queue) {
		hw_if->disable_irq(pdata, queue->index, pdata->rx_irq_mask | pdata->tx_irq_mask);
		hw_if->clear_irq(pdata, queue->index, pdata->rx_irq_mask | pdata->tx_irq_mask);
	}

	/* Disable Rx and Tx */
	hw_if->enable_network(pdata, false, PHYTMAC_RX | PHYTMAC_TX);

	/* Tx clean */
	for (q = 0, queue = pdata->queues; q < pdata->queues_num; ++q, ++queue) {
		for (i = 0; i < pdata->tx_ring_size; i++) {
			tx_skb = phytmac_get_tx_skb(queue, i);
			if (tx_skb)
				phytmac_tx_unmap(pdata, tx_skb, 0);
		}
	}

	spin_unlock_irqrestore(&pdata->lock, flags);

	netif_tx_stop_all_queues(ndev);
}

static void phytmac_mac_link_up(struct phylink_config *config,
				struct phy_device *phy,
				unsigned int mode, phy_interface_t interface,
				int speed, int duplex,
				bool tx_pause, bool rx_pause)
{
	struct net_device *ndev = to_net_dev(config->dev);
	struct phytmac *pdata = netdev_priv(ndev);
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	struct phytmac_queue *queue;
	unsigned long flags;
	unsigned int q;
	int ret;

	if (netif_msg_link(pdata))
		netdev_info(pdata->ndev, "link up interface:%s, speed:%d, duplex:%s\n",
			    phy_modes(interface), speed, duplex ? "full-duplex" : "half-duplex");

	spin_lock_irqsave(&pdata->lock, flags);

	hw_if->mac_linkup(pdata, interface, speed, duplex);

	if (rx_pause != pdata->pause) {
		hw_if->enable_pause(pdata, rx_pause);
		pdata->pause = rx_pause;
	}

	phytmac_init_ring(pdata);

	for (q = 0, queue = pdata->queues; q < pdata->queues_num; ++q, ++queue)
		hw_if->enable_irq(pdata, queue->index, pdata->rx_irq_mask | pdata->tx_irq_mask);

	/* Enable Rx and Tx */
	hw_if->enable_network(pdata, true, PHYTMAC_RX | PHYTMAC_TX);
	spin_unlock_irqrestore(&pdata->lock, flags);

	if (pdata->use_ncsi) {
		/* Start the NCSI device */
		ret = ncsi_start_dev(pdata->ncsidev);
		if (ret) {
			netdev_err(pdata->ndev, "Ncsi start dev failed (error %d)\n", ret);
			return;
		}
	}

	netif_tx_wake_all_queues(ndev);
}

int phytmac_mdio_register(struct phytmac *pdata)
{
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	int ret;

	pdata->mii_bus = mdiobus_alloc();
	if (!pdata->mii_bus) {
		ret = -ENOMEM;
		goto err_out;
	}

	pdata->mii_bus->name = "phytmac_mii_bus";
	pdata->mii_bus->read = &phytmac_mdio_read;
	pdata->mii_bus->write = &phytmac_mdio_write;

	if (pdata->platdev) {
		snprintf(pdata->mii_bus->id, MII_BUS_ID_SIZE, "%s-%s",
			 pdata->mii_bus->name, pdata->platdev->name);
	} else if (pdata->pcidev) {
		snprintf(pdata->mii_bus->id, MII_BUS_ID_SIZE, "%s-%s",
			 pdata->mii_bus->name, pci_name(pdata->pcidev));
	} else {
		ret = -ENOMEM;
		goto free_mdio;
	}

	pdata->mii_bus->priv = pdata;
	pdata->mii_bus->parent = pdata->dev;

	hw_if->enable_mdio_control(pdata, 1);

	return mdiobus_register(pdata->mii_bus);
free_mdio:
	mdiobus_free(pdata->mii_bus);
	pdata->mii_bus = NULL;

err_out:
	return ret;
}

static void phytmac_pcs_get_state(struct phylink_config *config,
				  struct phylink_link_state *state)
{
	struct phytmac *pdata = container_of(config, struct phytmac, phylink_config);
	struct phytmac_hw_if *hw_if = pdata->hw_if;

	state->link = hw_if->get_link(pdata, state->interface);
}

static void phytmac_validate(struct phylink_config *config,
			     unsigned long *supported,
			     struct phylink_link_state *state)
{
	struct net_device *ndev = to_net_dev(config->dev);
	__ETHTOOL_DECLARE_LINK_MODE_MASK(mask) = { 0, };
	struct phytmac *pdata = netdev_priv(ndev);

	if (state->interface != PHY_INTERFACE_MODE_SGMII &&
	    state->interface != PHY_INTERFACE_MODE_1000BASEX &&
	    state->interface != PHY_INTERFACE_MODE_2500BASEX &&
	    state->interface != PHY_INTERFACE_MODE_5GBASER &&
	    state->interface != PHY_INTERFACE_MODE_10GBASER &&
	    state->interface != PHY_INTERFACE_MODE_USXGMII &&
	    !phy_interface_mode_is_rgmii(state->interface)) {
		bitmap_zero(supported, __ETHTOOL_LINK_MODE_MASK_NBITS);
		return;
	}

	phylink_set_port_modes(mask);
	phylink_set(mask, Autoneg);
	phylink_set(mask, Asym_Pause);

	if (state->interface == PHY_INTERFACE_MODE_10GBASER ||
	    state->interface == PHY_INTERFACE_MODE_USXGMII) {
		pdata->speed = state->speed;
		pdata->duplex = state->duplex;
		if (pdata->speed == SPEED_5000) {
			phylink_set(mask, 5000baseT_Full);
		} else {
			phylink_set(mask, 10000baseCR_Full);
			phylink_set(mask, 10000baseER_Full);
			phylink_set(mask, 10000baseKR_Full);
			phylink_set(mask, 10000baseLR_Full);
			phylink_set(mask, 10000baseLRM_Full);
			phylink_set(mask, 10000baseSR_Full);
			phylink_set(mask, 10000baseT_Full);
		}
	}

	if (state->interface == PHY_INTERFACE_MODE_2500BASEX)
		phylink_set(mask, 2500baseX_Full);

	if (state->interface == PHY_INTERFACE_MODE_5GBASER)
		phylink_set(mask, 5000baseT_Full);

	if (state->interface == PHY_INTERFACE_MODE_1000BASEX)
		phylink_set(mask, 1000baseX_Full);

	if (state->interface == PHY_INTERFACE_MODE_SGMII ||
	    phy_interface_mode_is_rgmii(state->interface)) {
		phylink_set(mask, 1000baseT_Full);
		phylink_set(mask, 1000baseT_Half);
		phylink_set(mask, 10baseT_Half);
		phylink_set(mask, 10baseT_Full);
		phylink_set(mask, 100baseT_Half);
		phylink_set(mask, 100baseT_Full);
	}

	bitmap_and(supported, supported, mask, __ETHTOOL_LINK_MODE_MASK_NBITS);
	bitmap_and(state->advertising, state->advertising, mask,
		   __ETHTOOL_LINK_MODE_MASK_NBITS);
}

static const struct phylink_mac_ops phytmac_phylink_ops = {
	.validate = phytmac_validate,
	.mac_prepare = phytmac_mac_prepare,
	.mac_config = phytmac_mac_config,
	.mac_an_restart = phytmac_mac_an_restart,
	.mac_link_down = phytmac_mac_link_down,
	.mac_link_up = phytmac_mac_link_up,
};

static int phytmac_phylink_create(struct phytmac *pdata)
{
	struct fwnode_handle *fw_node = dev_fwnode(pdata->dev);

	pdata->phylink_config.dev = &pdata->ndev->dev;
	pdata->phylink_config.type = PHYLINK_NETDEV;

	if (pdata->phy_interface == PHY_INTERFACE_MODE_SGMII ||
	    pdata->phy_interface == PHY_INTERFACE_MODE_1000BASEX ||
	    pdata->phy_interface == PHY_INTERFACE_MODE_2500BASEX ||
	    pdata->phy_interface == PHY_INTERFACE_MODE_USXGMII ||
	    pdata->phy_interface == PHY_INTERFACE_MODE_10GBASER) {
		pdata->phylink_config.poll_fixed_state = true;
		pdata->phylink_config.get_fixed_state = phytmac_pcs_get_state;
		pdata->phylink_pcs.ops = &phytmac_pcs_phylink_ops;
	}

	pdata->phylink = phylink_create(&pdata->phylink_config, fw_node,
					pdata->phy_interface, &phytmac_phylink_ops);
	if (IS_ERR(pdata->phylink)) {
		dev_err(pdata->dev, "Could not create a phylink instance (%ld)\n",
			PTR_ERR(pdata->phylink));
		return PTR_ERR(pdata->phylink);
	}

	return 0;
}

static int phytmac_open(struct net_device *ndev)
{
	struct phytmac *pdata = netdev_priv(ndev);
	struct phytmac_queue *queue;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	unsigned int q = 0;
	int ret;

	if (netif_msg_probe(pdata))
		dev_dbg(pdata->dev, "open\n");

	/* phytmac_powerup */
	if (pdata->power_state == PHYTMAC_POWEROFF)
		hw_if->poweron(pdata, PHYTMAC_POWERON);

	if (hw_if->init_msg_ring)
		hw_if->init_msg_ring(pdata);

	ret = hw_if->get_feature(pdata);
	if (ret) {
		netdev_err(ndev, "phytmac get features failed\n");
		return ret;
	}

	hw_if->reset_hw(pdata);

	ret = netif_set_real_num_tx_queues(ndev, pdata->queues_num);
	if (ret) {
		netdev_err(ndev, "error setting real tx queue number\n");
		return ret;
	}
	ret = netif_set_real_num_rx_queues(ndev, pdata->queues_num);
	if (ret) {
		netdev_err(ndev, "error setting real tx queue number\n");
		return ret;
	}

	/* RX buffers initialization */
	ret = phytmac_alloc_resource(pdata);
	if (ret) {
		netdev_err(ndev, "Unable to allocate DMA memory (error %d)\n",
			   ret);
		goto reset_hw;
	}

	for (queue = pdata->queues; q < pdata->queues_num; ++q) {
		napi_enable(&queue->tx_napi);
		napi_enable(&queue->rx_napi);
		++queue;
	}

	hw_if->init_hw(pdata);

	ret = phytmac_phylink_connect(pdata);
	if (ret) {
		netdev_err(ndev, "phylink connect failed,(error %d)\n",
			   ret);
		goto reset_hw;
	}

	phylink_start(pdata->phylink);
	phytmac_set_bios_wol_enable(pdata, pdata->wol ? 1 : 0);

	netif_tx_start_all_queues(pdata->ndev);

	if (IS_REACHABLE(CONFIG_PHYTMAC_ENABLE_PTP)) {
		ret = phytmac_ptp_register(pdata);
		if (ret) {
			netdev_err(ndev, "ptp register failed, (error %d)\n",
				   ret);
			goto reset_hw;
		}

		phytmac_ptp_init(pdata->ndev);
	}

	return 0;

reset_hw:
	hw_if->reset_hw(pdata);
	for (q = 0, queue = pdata->queues; q < pdata->queues_num; ++q) {
		napi_disable(&queue->tx_napi);
		napi_disable(&queue->rx_napi);
		++queue;
	}
	phytmac_free_resource(pdata);
	return ret;
}

static int phytmac_close(struct net_device *ndev)
{
	struct phytmac *pdata = netdev_priv(ndev);
	struct phytmac_queue *queue;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	unsigned long flags;
	unsigned int q;

	if (netif_msg_probe(pdata))
		dev_dbg(pdata->dev, "close");

	netif_tx_stop_all_queues(ndev);

	for (q = 0, queue = pdata->queues; q < pdata->queues_num; ++q, ++queue) {
		napi_disable(&queue->tx_napi);
		napi_disable(&queue->rx_napi);
	}

	phylink_stop(pdata->phylink);
	phylink_disconnect_phy(pdata->phylink);

	netif_carrier_off(ndev);

	spin_lock_irqsave(&pdata->lock, flags);
	hw_if->reset_hw(pdata);
	spin_unlock_irqrestore(&pdata->lock, flags);

	phytmac_free_resource(pdata);

	if (IS_REACHABLE(CONFIG_PHYTMAC_ENABLE_PTP))
		phytmac_ptp_unregister(pdata);

	/* phytmac_powerup */
	if (pdata->power_state == PHYTMAC_POWERON)
		hw_if->poweron(pdata, PHYTMAC_POWEROFF);

	return 0;
}

static int phytmac_ioctl(struct net_device *dev, struct ifreq *rq, int cmd)
{
	struct phytmac *pdata = netdev_priv(dev);
	int ret = -EOPNOTSUPP;

	if (!netif_running(dev))
		return -EINVAL;

	switch (cmd) {
	case SIOCGMIIPHY:
	case SIOCGMIIREG:
	case SIOCSMIIREG:
		ret = phylink_mii_ioctl(pdata->phylink, rq, cmd);
		break;
#ifdef CONFIG_PHYTMAC_ENABLE_PTP
	case SIOCSHWTSTAMP:
		ret = phytmac_ptp_set_ts_config(dev, rq, cmd);
		break;
	case SIOCGHWTSTAMP:
		ret = phytmac_ptp_get_ts_config(dev, rq);
		break;
#endif
	default:
		break;
	}

	return ret;
}

static inline int phytmac_set_features(struct net_device *netdev,
				       netdev_features_t features)
{
	struct phytmac *pdata = netdev_priv(netdev);
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	netdev_features_t changed = features ^ netdev->features;

	/* TX checksum offload */
	if (changed & NETIF_F_HW_CSUM) {
		if (features & NETIF_F_HW_CSUM)
			hw_if->enable_tx_csum(pdata, 1);
		else
			hw_if->enable_tx_csum(pdata, 0);
	}

	/* RX checksum offload */
	if (changed & NETIF_F_RXCSUM) {
		if (features & NETIF_F_RXCSUM &&
		    !(netdev->flags & IFF_PROMISC))
			hw_if->enable_rx_csum(pdata, 1);
		else
			hw_if->enable_rx_csum(pdata, 0);
	}
	return 0;
}

static netdev_features_t phytmac_features_check(struct sk_buff *skb,
						struct net_device *dev,
						netdev_features_t features)
{
	unsigned int nr_frags, f;
	unsigned int hdrlen;

	/* there is only one buffer or protocol is not UDP */
	if (!skb_is_nonlinear(skb) || (ip_hdr(skb)->protocol != IPPROTO_UDP))
		return features;

	/* length of header */
	hdrlen = skb_transport_offset(skb);

	if (!IS_ALIGNED(skb_headlen(skb) - hdrlen, PHYTMAC_TX_LEN_ALIGN))
		return features & ~NETIF_F_TSO;

	nr_frags = skb_shinfo(skb)->nr_frags;
	/* No need to check last fragment */
	nr_frags--;
	for (f = 0; f < nr_frags; f++) {
		const skb_frag_t *frag = &skb_shinfo(skb)->frags[f];

		if (!IS_ALIGNED(skb_frag_size(frag), PHYTMAC_TX_LEN_ALIGN))
			return features & ~NETIF_F_TSO;
	}
	return features;
}

void phytmac_set_bios_wol_enable(struct phytmac *pdata, u32 wol)
{
	struct net_device *ndev = pdata->ndev;

	if (ndev->phydev) {
#ifdef CONFIG_ACPI
		if (has_acpi_companion(pdata->dev)) {
			acpi_handle handle = ACPI_HANDLE(pdata->dev);

			if (acpi_has_method(handle, "PWOL")) {
				union acpi_object args[] = {
					{ .type = ACPI_TYPE_INTEGER, },
				};
				struct acpi_object_list arg_input = {
					.pointer = args,
					.count = ARRAY_SIZE(args),
				};
				acpi_status status;

				/* Set the input parameters */
				args[0].integer.value = wol;

				status = acpi_evaluate_object(handle, "PWOL", &arg_input, NULL);
				if (ACPI_FAILURE(status))
					netdev_err(ndev, "The PWOL method failed to be executed.\n");
			}
		}
#endif
	}
}

int phytmac_reset_ringsize(struct phytmac *pdata, u32 rx_size, u32 tx_size)
{
	int ret = 0;
	int reset = 0;

	if (netif_running(pdata->ndev)) {
		reset = 1;
		phytmac_close(pdata->ndev);
	}

	pdata->rx_ring_size = rx_size;
	pdata->tx_ring_size = tx_size;

	if (reset)
		phytmac_open(pdata->ndev);

	return ret;
}

static const struct net_device_ops phytmac_netdev_ops = {
	.ndo_open		= phytmac_open,
	.ndo_stop		= phytmac_close,
	.ndo_start_xmit		= phytmac_start_xmit,
	.ndo_set_rx_mode	= phytmac_set_rx_mode,
	.ndo_get_stats		= phytmac_get_stats,
	.ndo_do_ioctl		= phytmac_ioctl,
	.ndo_validate_addr	= eth_validate_addr,
	.ndo_change_mtu		= phytmac_change_mtu,
	.ndo_set_mac_address	= phytmac_set_mac_address,
	.ndo_set_features	= phytmac_set_features,
	.ndo_features_check	= phytmac_features_check,
	.ndo_vlan_rx_add_vid	= ncsi_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid	= ncsi_vlan_rx_kill_vid,
};

static int phytmac_init(struct phytmac *pdata)
{
	struct net_device *ndev = pdata->ndev;
	unsigned int q;
	struct phytmac_queue *queue;
	int ret;

	if (netif_msg_probe(pdata))
		dev_dbg(pdata->dev, "phytmac init !\n");

	spin_lock_init(&pdata->lock);

	/* set the queue register mapping once for all: queue0 has a special
	 * register mapping but we don't want to test the queue index then
	 * compute the corresponding register offset at run time.
	 */
	for (q = 0; q < pdata->queues_num; ++q) {
		queue = &pdata->queues[q];
		queue->pdata = pdata;
		queue->index = q;
		spin_lock_init(&queue->tx_lock);

		netif_napi_add(ndev, &queue->tx_napi, phytmac_tx_poll, NAPI_POLL_WEIGHT);
		netif_napi_add(ndev, &queue->rx_napi, phytmac_rx_poll, NAPI_POLL_WEIGHT);

		if (pdata->irq_type == IRQ_TYPE_INT || pdata->irq_type == IRQ_TYPE_MSI) {
			queue->irq = pdata->queue_irq[q];
			if (pdata->irq_type == IRQ_TYPE_INT)
				ret = devm_request_irq(pdata->dev, queue->irq, phytmac_irq,
						       IRQF_SHARED, ndev->name, queue);
			else
				ret = devm_request_irq(pdata->dev, queue->irq, phytmac_irq,
						       0, ndev->name, queue);

			if (ret) {
				dev_err(pdata->dev,
					"Unable to request IRQ %d (error %d)\n",
					queue->irq, ret);
				return ret;
			}
		}
	}

	if (pdata->irq_type == IRQ_TYPE_INTX) {
		ret = devm_request_irq(pdata->dev, pdata->queue_irq[0], phytmac_intx_irq,
				       IRQF_SHARED, ndev->name, pdata);
		if (ret) {
			dev_err(pdata->dev,
				"Unable to request INTX IRQ %d (error %d)\n",
				pdata->queue_irq[0], ret);
			return ret;
		}
	}

	ndev->netdev_ops = &phytmac_netdev_ops;
	phytmac_set_ethtool_ops(ndev);

	if (ndev->hw_features & NETIF_F_NTUPLE) {
		INIT_LIST_HEAD(&pdata->rx_fs_list.list);
		pdata->rx_fs_list.count = 0;
		spin_lock_init(&pdata->rx_fs_lock);
	}

	device_set_wakeup_enable(pdata->dev, pdata->wol ? 1 : 0);

	return 0;
}

void phytmac_default_config(struct phytmac *pdata)
{
	struct net_device *ndev = pdata->ndev;

	pdata->rx_irq_mask = PHYTMAC_RX_INT_FLAGS;
	pdata->tx_irq_mask = PHYTMAC_TX_INT_FLAGS;
	pdata->tx_ring_size = DEFAULT_TX_RING_SIZE;
	pdata->rx_ring_size = DEFAULT_RX_RING_SIZE;
	pdata->max_tx_length = PHYTMAC_MAX_TX_LEN;
	pdata->min_tx_length = PHYTMAC_MIN_TX_LEN;
	pdata->pause = true;

	ndev->hw_features = NETIF_F_SG;

	if (pdata->capacities & PHYTMAC_CAPS_LSO)
		ndev->hw_features |= NETIF_F_TSO;

	if (pdata->use_ncsi) {
		ndev->hw_features &= ~(NETIF_F_HW_CSUM | NETIF_F_RXCSUM);
		ndev->hw_features |= NETIF_F_HW_VLAN_CTAG_FILTER;
	} else {
		ndev->hw_features |= NETIF_F_HW_CSUM | NETIF_F_RXCSUM;
	}

	if (pdata->capacities & PHYTMAC_CAPS_SG_DISABLED)
		ndev->hw_features &= ~NETIF_F_SG;

	ndev->hw_features |= NETIF_F_NTUPLE;

	ndev->min_mtu = ETH_MIN_MTU;
	if (pdata->capacities & PHYTMAC_CAPS_JUMBO)
		ndev->max_mtu = pdata->jumbo_len - ETH_HLEN - ETH_FCS_LEN;
	else
		ndev->max_mtu = ETH_DATA_LEN;

	ndev->features = ndev->hw_features;
}

static void phytmac_ncsi_handler(struct ncsi_dev *nd)
{
	if (unlikely(nd->state != ncsi_dev_state_functional))
		return;

	netdev_dbg(nd->dev, "NCSI interface %s\n",
		   nd->link_up ? "up" : "down");
}

int phytmac_drv_probe(struct phytmac *pdata)
{
	struct net_device *ndev = pdata->ndev;
	struct device *dev = pdata->dev;
	int ret = 0;

	if (netif_msg_probe(pdata))
		dev_dbg(pdata->dev, "phytmac drv probe start\n");

	phytmac_default_config(pdata);

	if (dma_set_mask(dev, DMA_BIT_MASK(40)) ||
	    dma_set_mask_and_coherent(dev, DMA_BIT_MASK(40))) {
		dev_err(dev, "dma_set_mask or coherent failed\n");
		return 1;
	}

	ret = phytmac_init(pdata);
	if (ret)
		goto err_out;

	if (pdata->use_ncsi) {
		pdata->ncsidev = ncsi_register_dev(ndev, phytmac_ncsi_handler);
		if (!pdata->ncsidev)
			goto err_out;
	}

	netif_carrier_off(ndev);
	ret = register_netdev(ndev);
	if (ret) {
		dev_err(pdata->dev, "Cannot register net device, aborting.\n");
		goto err_out;
	}

	if (pdata->use_mii && !pdata->mii_bus) {
		ret = phytmac_mdio_register(pdata);
		if (ret) {
			netdev_err(ndev, "MDIO bus registration failed\n");
			goto err_out_free_mdiobus;
		}
	}

	ret = phytmac_phylink_create(pdata);
	if (ret) {
		netdev_err(ndev, "phytmac phylink create failed, error %d\n", ret);
		goto err_phylink_init;
	}

	ret = phytmac_get_mac_address(pdata);
	if (ret) {
		netdev_err(ndev, "phytmac get mac address failed\n");
		goto err_phylink_init;
	}

	if (netif_msg_probe(pdata))
		dev_dbg(pdata->dev, "probe successfully! Phytium %s at 0x%08lx irq %d (%pM)\n",
			"MAC", ndev->base_addr, ndev->irq, ndev->dev_addr);

	return 0;

err_phylink_init:
	if (pdata->mii_bus)
		mdiobus_unregister(pdata->mii_bus);

err_out_free_mdiobus:
	if (pdata->mii_bus)
		mdiobus_free(pdata->mii_bus);

	unregister_netdev(ndev);

err_out:
	return ret;
}
EXPORT_SYMBOL_GPL(phytmac_drv_probe);

int phytmac_drv_remove(struct phytmac *pdata)
{
	struct net_device *ndev = pdata->ndev;

	if (ndev) {
		if (pdata->use_ncsi && pdata->ncsidev)
			ncsi_unregister_dev(pdata->ncsidev);

		unregister_netdev(ndev);

		if (pdata->use_mii && pdata->mii_bus) {
			mdiobus_unregister(pdata->mii_bus);
			mdiobus_free(pdata->mii_bus);
		}

		if (pdata->phylink)
			phylink_destroy(pdata->phylink);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(phytmac_drv_remove);

int phytmac_drv_suspend(struct phytmac *pdata)
{
	int q;
	unsigned long flags;
	struct phytmac_queue *queue;
	struct phytmac_hw_if *hw_if = pdata->hw_if;

	if (!netif_running(pdata->ndev))
		return 0;

	if (pdata->power_state == PHYTMAC_POWEROFF)
		return 0;

	netif_carrier_off(pdata->ndev);
	netif_device_detach(pdata->ndev);

	/* napi_disable */
	for (q = 0, queue = pdata->queues; q < pdata->queues_num;
	     ++q, ++queue) {
		napi_disable(&queue->tx_napi);
		napi_disable(&queue->rx_napi);
	}

	if (pdata->wol) {
		hw_if->set_wol(pdata, pdata->wol);
	} else {
		rtnl_lock();
		phylink_stop(pdata->phylink);
		rtnl_unlock();
		spin_lock_irqsave(&pdata->lock, flags);
		hw_if->reset_hw(pdata);
		hw_if->poweron(pdata, PHYTMAC_POWEROFF);
		spin_unlock_irqrestore(&pdata->lock, flags);
	}

	return 0;
}
EXPORT_SYMBOL_GPL(phytmac_drv_suspend);

int phytmac_drv_resume(struct phytmac *pdata)
{
	int q;
	struct phytmac_queue *queue;
	struct phytmac_hw_if *hw_if = pdata->hw_if;
	struct ethtool_rx_fs_item *item;

	if (!netif_running(pdata->ndev))
		return 0;

	if (pdata->power_state == PHYTMAC_POWEROFF)
		hw_if->poweron(pdata, PHYTMAC_POWERON);

	if (hw_if->init_msg_ring)
		hw_if->init_msg_ring(pdata);

	if (pdata->wol) {
		hw_if->set_wol(pdata, 0);
		rtnl_lock();
		phylink_stop(pdata->phylink);
		rtnl_unlock();
	}

	for (q = 0, queue = pdata->queues; q < pdata->queues_num;
	     ++q, ++queue) {
		napi_enable(&queue->tx_napi);
		napi_enable(&queue->rx_napi);
	}

	hw_if->init_hw(pdata);
	phytmac_set_rx_mode(pdata->ndev);
	phytmac_set_features(pdata->ndev, pdata->ndev->features);
	list_for_each_entry(item, &pdata->rx_fs_list.list, list)
		hw_if->add_fdir_entry(pdata, &item->fs);

	rtnl_lock();
	phylink_start(pdata->phylink);
	rtnl_unlock();

	netif_device_attach(pdata->ndev);

	return 0;
}
EXPORT_SYMBOL_GPL(phytmac_drv_resume);

struct phytmac *phytmac_alloc_pdata(struct device *dev)
{
	struct phytmac *pdata;
	struct net_device *netdev;

	netdev = alloc_etherdev_mq(sizeof(struct phytmac),
				   PHYTMAC_MAX_QUEUES);
	if (!netdev) {
		dev_err(dev, "alloc_etherdev_mq failed\n");
		return ERR_PTR(-ENOMEM);
	}
	SET_NETDEV_DEV(netdev, dev);
	pdata = netdev_priv(netdev);
	pdata->ndev = netdev;
	pdata->dev = dev;

	spin_lock_init(&pdata->lock);
	spin_lock_init(&pdata->msg_lock);
	spin_lock_init(&pdata->ts_clk_lock);
	pdata->msg_enable = netif_msg_init(debug, PHYTMAC_DEFAULT_MSG_ENABLE);

	return pdata;
}
EXPORT_SYMBOL_GPL(phytmac_alloc_pdata);

void phytmac_free_pdata(struct phytmac *pdata)
{
	struct net_device *netdev = pdata->ndev;

	free_netdev(netdev);
}
EXPORT_SYMBOL_GPL(phytmac_free_pdata);

MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Phytium Ethernet driver");
MODULE_AUTHOR("Wenting Song");
MODULE_ALIAS("platform:phytmac");
MODULE_VERSION(PHYTMAC_DRIVER_VERSION);
