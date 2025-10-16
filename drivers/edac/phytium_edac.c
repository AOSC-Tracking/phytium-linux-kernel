// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Phytium Pe220x EDAC (error detection and correction)
 *
 * Copyright (c) 2023-2024 Phytium Technology Co., Ltd.
 */

#include <linux/ctype.h>
#include <linux/edac.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <ras/ras_event.h>
#include <linux/uaccess.h>
#include "edac_module.h"
#include <linux/of_address.h>
#include <linux/of_device.h>

#define EDAC_MOD_STR			"phytium_edac"

/* register offset */
#define ERR_STATUS(n)			(0x10 + ((n) * 64))
#define ERR_CTLR(n)			(0x08 + ((n) * 64))
#define ERR_MISC0(n)			(0x20 + ((n) * 64))
#define ERR_INJECT			0x7C
#define ERR_DEVID			0xFC8
#define ERR_GSR				0xE00

#define CTLR_ED				BIT(0)
#define CTLR_UI				BIT(2)
#define CTLR_CFI			BIT(8)

#define MISC0_CEC(x)			((u64)(x) << 32)

#define ERR_STATUS_CLEAR		GENMASK(31, 0)

#define CORRECTED_ERROR			0
#define UNCORRECTED_ERROR		1

#define EDAC_DRIVER_VERSION "1.1.2"

struct ras_error_info {
	u32 index;
	u32 error_type;
	const char *error_str;
};

struct phytium_edac {
	struct device		*dev;
	void __iomem		**ras_base;
	struct dentry		*dfs;
	struct edac_device_ctl_info *edac_dev;
	int num_err_group;
	const struct ras_error_info **error_info;
};

/* error severity definition */
enum {
	SEV_NO = 0x0,
	SEV_CORRECTED = 0x1,
	SEV_RECOVERABLE = 0x2,
	SEV_PANIC = 0x3,
};

/* soc error record */
static const struct ras_error_info pe220x_ras_soc_error[] = {
	{ 0, UNCORRECTED_ERROR, "lsd_nfc_ras_error" },
	{ 1, UNCORRECTED_ERROR, "lsd_lpc_ras_long_wait_to" },
	{ 2, UNCORRECTED_ERROR, "lsd_lpc_ras_short_wait_to" },
	{ 3, UNCORRECTED_ERROR, "lsd_lpc_ras_sync_err" },
	{ 4, UNCORRECTED_ERROR, "lsd_lbc_ras_err" },
	{ 5, UNCORRECTED_ERROR, "usb3_err_0" },
	{ 6, UNCORRECTED_ERROR, "usb3_err_1" },
	{ 7, UNCORRECTED_ERROR, "gsd_gmu_mac0_asf_nonfatal_int" },
	{ 8, UNCORRECTED_ERROR, "gsd_gmu_mac0_asf_fatal_int" },
	{ 9, UNCORRECTED_ERROR, "gsd_gmu_mac0_asf_trans_to_err" },
	{ 10, UNCORRECTED_ERROR, "gsd_gmu_mac0_asf_protocol_err" },
	{ 11, UNCORRECTED_ERROR, "gsd_gmu_mac1_asf_nonfatal_int" },
	{ 12, UNCORRECTED_ERROR, "gsd_gmu_mac1_asf_fatal_int" },
	{ 13, UNCORRECTED_ERROR, "gsd_gmu_mac1_asf_trans_to_err" },
	{ 14, UNCORRECTED_ERROR, "gsd_gmu_mac1_asf_protocol_err" },
	{ 15, UNCORRECTED_ERROR, "gsd_gmu_mac2_asf_nonfatal_int" },
	{ 16, UNCORRECTED_ERROR, "gsd_gmu_mac2_asf_fatal_int" },
	{ 17, UNCORRECTED_ERROR, "gsd_gmu_mac2_asf_trans_to_err" },
	{ 18, UNCORRECTED_ERROR, "gsd_gmu_mac2_asf_protocol_err" },
	{ 19, UNCORRECTED_ERROR, "gsd_gmu_mac3_asf_nonfatal_int" },
	{ 20, UNCORRECTED_ERROR, "gsd_gmu_mac3_asf_fatal_int" },
	{ 21, UNCORRECTED_ERROR, "gsd_gmu_mac3_asf_trans_to_err" },
	{ 22, UNCORRECTED_ERROR, "gsd_gmu_mac3_asf_protocol_err" },
	{ 23, CORRECTED_ERROR, "dmu_ras_ecc_corrected_error" },
	{ 24, UNCORRECTED_ERROR, "dmu_ras_ecc_uncorrected_error" },
	{ 25, UNCORRECTED_ERROR, "cci_ras_nERRIRQ" },
	{ 26, UNCORRECTED_ERROR, "smmu_tcu_ras_irpt" },
	{ 27, UNCORRECTED_ERROR, "smmu_tbu0_ras_irpt" },
	{ 28, UNCORRECTED_ERROR, "smmu_tbu1_ras_irpt" },
	{ 29, UNCORRECTED_ERROR, "smmu_tbu2_ras_irpt" },
	{ 30, UNCORRECTED_ERROR, "ocm_sram_ue" },
	{ 31, CORRECTED_ERROR, "ocm_sram_ce" },
	{ 32, UNCORRECTED_ERROR, "int_axim_err" },
	{ 33, UNCORRECTED_ERROR, "int_fatal_error" },
	{ 34, UNCORRECTED_ERROR, "nEXTERRIRQ_clust0" },
	{ 35, UNCORRECTED_ERROR, "nINTERRIRQ_clust0" },
	{ 36, UNCORRECTED_ERROR, "nEXTERRIRQ_clust1" },
	{ 37, UNCORRECTED_ERROR, "nINTERRIRQ_clust1" },
	{ 38, UNCORRECTED_ERROR, "nEXTERRIRQ_clust2" },
	{ 39, UNCORRECTED_ERROR, "nINTERRIRQ_clust2" },
	{ 40, UNCORRECTED_ERROR, "ras_err_amu0" },
	{ 41, UNCORRECTED_ERROR, "ras_err_amu1" },
	{ 42, UNCORRECTED_ERROR, "ras_err_ame0" },
	{ 43, UNCORRECTED_ERROR, "ras_err_ame1" },
};

/* pcie controller error record */
static const struct ras_error_info pe220x_ras_peu_psu_error[] = {
	{ 0, CORRECTED_ERROR, "pio_rd_addr_error" },
	{ 1, UNCORRECTED_ERROR, "pio_wr_addr_error" },
	{ 2, CORRECTED_ERROR, "pio_rd_timeout" },
	{ 3, CORRECTED_ERROR, "pio_wr_timeout" },
	{ 4, CORRECTED_ERROR, "axi_b_rsp_error" },
	{ 5, CORRECTED_ERROR, "axi_r_rsp_error" },
};

static const struct ras_error_info pe220x_ras_peu_error[] = {
	{ 0, CORRECTED_ERROR, "pio_rd_addr_error" },
	{ 1, UNCORRECTED_ERROR, "pio_wr_addr_error" },
	{ 2, CORRECTED_ERROR, "pio_rd_timeout" },
	{ 3, CORRECTED_ERROR, "pio_wr_timeout" },
	{ 4, CORRECTED_ERROR, "axi_b_rsp_error" },
	{ 5, CORRECTED_ERROR, "axi_r_rsp_error" },
};

/* pd2208 error */
static const struct ras_error_info pd2208_ras_err[] = {
	{0, CORRECTED_ERROR, "lmu0_ras_ecc_corrected_err"},
	{1, UNCORRECTED_ERROR, "lmu0_ras_ecc_uncorrected_err"},
	{2, CORRECTED_ERROR, "lmu1_ras_ecc_corrected_err"},
	{3, UNCORRECTED_ERROR, "lmu1_ras_ecc_uncorrected_err"},
	{4, CORRECTED_ERROR, "sram_corrected_err"},
	{5, UNCORRECTED_ERROR, "sram_uncorrected_err"},
	{6, UNCORRECTED_ERROR, "qspi_ras_addr_err"},
	{7, UNCORRECTED_ERROR, "qspi_ras_pstrb_err"},
	{8, UNCORRECTED_ERROR, "intreq_err"},
	{9, UNCORRECTED_ERROR, "gic_axim_err"},
	{10, UNCORRECTED_ERROR, "gic_ecc_fatal"},
	{11, UNCORRECTED_ERROR, "lsd_lbc_ras_err"},
	{12, UNCORRECTED_ERROR, "nEXTERRIRQ_cluster0"},
	{13, UNCORRECTED_ERROR, "nINTERRIRQ_cluster0"},
	{14, UNCORRECTED_ERROR, "nEXTERRIRQ_cluster1"},
	{15, UNCORRECTED_ERROR, "nINTERRIRQ_cluster1"},
	{16, UNCORRECTED_ERROR, "nEXTERRIRQ_cluster2"},
	{17, UNCORRECTED_ERROR, "nINTERRIRQ_cluster2"},
	{18, UNCORRECTED_ERROR, "nEXTERRIRQ_cluster3"},
	{19, UNCORRECTED_ERROR, "nINTERRIRQ_cluster3"},
	{20, CORRECTED_ERROR, "lbc_ecc_corrected_err"},
	{21, UNCORRECTED_ERROR, "lbc_ecc_uncorrected_err"},
};

static const struct ras_error_info pd2208_ras_sram_err[] = {
	{0, CORRECTED_ERROR, "scp_sram_corrected_err"},
	{1, UNCORRECTED_ERROR, "scp_sram_uncorrected_err"},
	{2, CORRECTED_ERROR, "scp_sharemem_corrected_err"},
	{3, UNCORRECTED_ERROR, "scp_sharemem_uncorrected_err"},
	{4, CORRECTED_ERROR, "wr_cmd_buf_corrected_err"},
	{5, UNCORRECTED_ERROR, "wr_cmd_buf_uncorrected_err"},
	{6, CORRECTED_ERROR, "rd_dat_buf_corrected_err"},
	{7, UNCORRECTED_ERROR, "rd_dat_buf_uncorrected_err"},
	{8, CORRECTED_ERROR, "wr_cmd_buf_corrected_err"},
	{9, UNCORRECTED_ERROR, "wr_cmd_buf_uncorrected_err"},
	{10, CORRECTED_ERROR, "wr_dat_buf_corrected_err"},
	{11, UNCORRECTED_ERROR, "wr_dat_buf_uncorrected_err"},
	{12, CORRECTED_ERROR, "drtrch0_corrected_err"},
	{13, UNCORRECTED_ERROR, "drtrch0_uncorrected_err"},
	{14, CORRECTED_ERROR, "drtrch0_corrected_err"},
	{15, UNCORRECTED_ERROR, "drtrch0_uncorrected_err"},
	{16, CORRECTED_ERROR, "dmac_corrected_err"},
	{17, UNCORRECTED_ERROR, "dmac_uncorrected_err"},
	{18, CORRECTED_ERROR, "rmram0_corrected_err"},
	{19, UNCORRECTED_ERROR, "rmram0_uncorrected_err"},
	{20, CORRECTED_ERROR, "rmram1_corrected_err"},
	{21, UNCORRECTED_ERROR, "rmram1_uncorrected_err"},
	{22, CORRECTED_ERROR, "rmram2_corrected_err"},
	{23, UNCORRECTED_ERROR, "rmram2_uncorrected_err"},
	{24, CORRECTED_ERROR, "rmram3_corrected_err"},
	{25, UNCORRECTED_ERROR, "rmram3_uncorrected_err"},
	{26, CORRECTED_ERROR, "gmactx0_corrected_err"},
	{27, UNCORRECTED_ERROR, "gmactx0_uncorrected_err"},
	{28, CORRECTED_ERROR, "gmactx1_corrected_err"},
	{29, UNCORRECTED_ERROR, "gmactx1_uncorrected_err"},
	{30, CORRECTED_ERROR, "gmactx2_corrected_err"},
	{31, UNCORRECTED_ERROR, "gmactx2_uncorrected_err"},
	{32, CORRECTED_ERROR, "gmactx3_corrected_err"},
	{33, UNCORRECTED_ERROR, "gmactx3_uncorrected_err"},
};

static const struct ras_error_info pd2208_ras_peu_sram0_err[] = {
	{0, CORRECTED_ERROR, "c0p2a_corrected_err"},
	{1, UNCORRECTED_ERROR, "c0p2a_uncorrected_err"},
	{2, CORRECTED_ERROR, "c0a2p_corrected_err"},
	{3, UNCORRECTED_ERROR, "c0a2p_uncorrected_err"},
	{4, CORRECTED_ERROR, "c0rxbuf0_corrected_err"},
	{5, UNCORRECTED_ERROR, "c0rxbuf0_uncorrected_err"},
	{6, CORRECTED_ERROR, "c0rxbuf1_corrected_err"},
	{7, UNCORRECTED_ERROR, "c0rxbuf1 _uncorrected_err"},
	{8, CORRECTED_ERROR, "c0rxbuf2_corrected_err"},
	{9, UNCORRECTED_ERROR, "c0rxbuf2 _uncorrected_err"},
	{10, CORRECTED_ERROR, "c0rxbuf3_corrected_err"},
	{11, UNCORRECTED_ERROR, "c0rxbuf3 _uncorrected_err"},
	{12, CORRECTED_ERROR, "c0txbuf0_corrected_err"},
	{13, UNCORRECTED_ERROR, "c0txbuf0_uncorrected_err"},
	{14, CORRECTED_ERROR, "c0txbuf1_corrected_err"},
	{15, UNCORRECTED_ERROR, "c0txbuf1 _uncorrected_err"},
	{16, CORRECTED_ERROR, "c0txbuf2_corrected_err"},
	{17, UNCORRECTED_ERROR, "c0txbuf2 _uncorrected_err"},
	{18, CORRECTED_ERROR, "c0txbuf3_corrected_err"},
	{19, UNCORRECTED_ERROR, "c0txbuf3 _uncorrected_err"},
	{20, CORRECTED_ERROR, "c1p2a_corrected_err"},
	{21, UNCORRECTED_ERROR, "c1p2a_uncorrected_err"},
	{22, CORRECTED_ERROR, "c1a2p_corrected_err"},
	{23, UNCORRECTED_ERROR, "c1a2p_uncorrected_err"},
	{24, CORRECTED_ERROR, "c1rxbuf0_corrected_err"},
	{25, UNCORRECTED_ERROR, "c1rxbuf0_uncorrected_err"},
	{26, CORRECTED_ERROR, "c1rxbuf1_corrected_err"},
	{27, UNCORRECTED_ERROR, "c1rxbuf1 _uncorrected_err"},
	{28, CORRECTED_ERROR, "c1rxbuf2_corrected_err"},
	{29, UNCORRECTED_ERROR, "c1rxbuf2 _uncorrected_err"},
	{30, CORRECTED_ERROR, "c1rxbuf3_corrected_err"},
	{31, UNCORRECTED_ERROR, "c1rxbuf3 _uncorrected_err"},
	{32, CORRECTED_ERROR, "c1txbuf0_corrected_err"},
	{33, UNCORRECTED_ERROR, "c1txbuf0_uncorrected_err"},
	{34, CORRECTED_ERROR, "c1txbuf1_corrected_err"},
	{35, UNCORRECTED_ERROR, "c1txbuf1 _uncorrected_err"},
	{36, CORRECTED_ERROR, "c1txbuf2_corrected_err"},
	{37, UNCORRECTED_ERROR, "c1txbuf2 _uncorrected_err"},
	{38, CORRECTED_ERROR, "c1txbuf3_corrected_err"},
	{39, UNCORRECTED_ERROR, "c1txbuf3 _uncorrected_err"},
	{40, CORRECTED_ERROR, "c2p2a_corrected_err"},
	{41, UNCORRECTED_ERROR, "c2p2a_uncorrected_err"},
	{42, CORRECTED_ERROR, "c2a2p_corrected_err"},
	{43, UNCORRECTED_ERROR, "c2a2p_uncorrected_err"},
	{44, CORRECTED_ERROR, "c2rxbuf0_corrected_err"},
	{45, UNCORRECTED_ERROR, "c2rxbuf0_uncorrected_err"},
	{46, CORRECTED_ERROR, "c2rxbuf1_corrected_err"},
	{47, UNCORRECTED_ERROR, "c2rxbuf1 _uncorrected_err"},
	{48, CORRECTED_ERROR, "c2rxbuf2_corrected_err"},
	{49, UNCORRECTED_ERROR, "c2rxbuf2 _uncorrected_err"},
	{50, CORRECTED_ERROR, "c2rxbuf3_corrected_err"},
	{51, UNCORRECTED_ERROR, "c2rxbuf3 _uncorrected_err"},
	{52, CORRECTED_ERROR, "c2txbuf0_corrected_err"},
	{53, UNCORRECTED_ERROR, "c2txbuf0_uncorrected_err"},
	{54, CORRECTED_ERROR, "c2txbuf1_corrected_err"},
	{55, UNCORRECTED_ERROR, "c2txbuf1 _uncorrected_err"},
};

static const struct ras_error_info pd2208_ras_peu_sram1_err[] = {
	{0, CORRECTED_ERROR, "c2txbuf2_corrected_err"},
	{1, UNCORRECTED_ERROR, "c2txbuf2_uncorrected_err"},
	{2, CORRECTED_ERROR, "c2txbuf3_corrected_err"},
	{3, UNCORRECTED_ERROR, "c2txbuf3_uncorrected_err"},
	{4, CORRECTED_ERROR, "phy0_sram0_corrected_err"},
	{5, UNCORRECTED_ERROR, "phy0_sram0_uncorrected_err"},
	{6, CORRECTED_ERROR, "phy0_sram1_corrected_err"},
	{7, UNCORRECTED_ERROR, "phy0_sram1 _uncorrected_err"},
	{8, CORRECTED_ERROR, "phy0_sram2_corrected_err"},
	{9, UNCORRECTED_ERROR, "phy0_sram2 _uncorrected_err"},
	{10, CORRECTED_ERROR, "phy0_sram3_corrected_err"},
	{11, UNCORRECTED_ERROR, "phy0_sram3 _uncorrected_err"},
	{12, CORRECTED_ERROR, "phy1_sram0_corrected_err"},
	{13, UNCORRECTED_ERROR, "phy1_sram0_uncorrected_err"},
	{14, CORRECTED_ERROR, "mac0_rxdpram_corrected_err"},
	{15, UNCORRECTED_ERROR, "mac0_rxdpram _uncorrected_err"},
	{16, CORRECTED_ERROR, "mac0_txdpram_corrected_err"},
	{17, UNCORRECTED_ERROR, "mac0_txdpram _uncorrected_err"},
	{18, CORRECTED_ERROR, "mac1_rxdpram_corrected_err"},
	{19, UNCORRECTED_ERROR, "mac1_rxdpram _uncorrected_err"},
	{20, CORRECTED_ERROR, "mac1_txdpram_corrected_err"},
	{21, UNCORRECTED_ERROR, "mac1_txdpram _uncorrected_err"},
};

static const struct ras_error_info pd2208_ras_peu_base_err[] = {
	{0, UNCORRECTED_ERROR, "pio_rd_addr_error"},
	{1, UNCORRECTED_ERROR, "pio_rd_timeout"},
	{2, UNCORRECTED_ERROR, "pio_wr_addr_error"},
	{3, UNCORRECTED_ERROR, "pio_wr_timeout"},
	{4, CORRECTED_ERROR, "axi_b_rsp_error"},
	{5, UNCORRECTED_ERROR, "axi_r_rsp_error"},
	{6, UNCORRECTED_ERROR, "mac0_asf_trans_to_err"},
	{7, UNCORRECTED_ERROR, "mac0_asf_protocol_err"},
	{8, UNCORRECTED_ERROR, "mac0_asf_nonfatal_int"},
	{9, UNCORRECTED_ERROR, "mac0_asf_fatal_int"},
	{10, UNCORRECTED_ERROR, "mac1_asf_trans_to_err"},
	{11, UNCORRECTED_ERROR, "mac1_asf_protocol_err"},
	{12, UNCORRECTED_ERROR, "mac1_asf_nonfatal_int"},
	{13, UNCORRECTED_ERROR, "mac1_asf_fatal_int"},
};

static const struct ras_error_info *pe220x_ras_error[] = {
	pe220x_ras_soc_error,
	pe220x_ras_peu_psu_error,
	pe220x_ras_peu_error,
};

static const struct ras_error_info *pd2208_ras_error[] = {
	pd2208_ras_err,
	pd2208_ras_sram_err,
	pd2208_ras_peu_sram0_err,
	pd2208_ras_peu_sram1_err,
	pd2208_ras_peu_base_err,
};

static inline unsigned int get_error_num(const struct phytium_edac *edac,
					int err_group)
{
	unsigned int error_num = 0;

	error_num = readl(edac->ras_base[err_group] + ERR_DEVID);

	return error_num;
}

static inline void phytium_ras_setup(const struct phytium_edac *edac)
{
	u64 val = 0;
	unsigned int i = 0;

	/*
	 * enable error report and generate interrupt for corrected error event
	 */
	for (i = 0; i < edac->num_err_group; i++) {
		val = readq(edac->ras_base[i] + ERR_CTLR(0));
		val |= CTLR_ED | CTLR_UI | CTLR_CFI;
		writeq(val, edac->ras_base[i] + ERR_CTLR(0));
	}
}

static ssize_t phytium_edac_inject_ctrl_write(struct file *filp,
						const char __user *buf,
						size_t size, loff_t *ppos)
{
	int ret = 0;
	int res = 0;
	unsigned int error_group = 0;
	unsigned int error_id = 0;
	unsigned int error_num = 0;
	struct phytium_edac *edac = filp->private_data;
	char str[255];
	char *p_str = str;
	char *tmp = NULL;

	if (size > 255) {
		ret = -EFAULT;
		goto out;
	}

	if (copy_from_user(str, buf, size)) {
		ret = -EFAULT;
		goto out;
	} else {
		*ppos += size;
		ret = size;
	}
	str[size] = '\0';

	tmp = strsep(&p_str, ",");
	if (!tmp)
		goto out;

	res = kstrtouint(tmp, 0, &error_group);
	if (res || error_group >= edac->num_err_group) {
		dev_err(edac->dev, "invalid error group parameters");
		goto out;
	}

	res = kstrtouint(p_str, 0, &error_id);
	if (res) {
		dev_err(edac->dev, "invalid error id parameters");
		goto out;
	}

	error_num = get_error_num(edac, error_group);
	if (error_id >= error_num) {
		dev_err(edac->dev, "invalid ras error id.\n");
		goto out;
	}

	dev_dbg(edac->dev, "inject group: %d, error_id: %d\n",
			error_group, error_id);

	if (edac->error_info[error_group][error_id].error_type
			== CORRECTED_ERROR) {
		writeq(MISC0_CEC(0xFF),
			edac->ras_base[error_group] + ERR_MISC0(error_id));
	}

	writel(error_id, edac->ras_base[error_group] + ERR_INJECT);

out:
	return ret;
}

static const struct file_operations phytium_edac_debug_inject_fops[] = {
	{
	.open = simple_open,
	.write = phytium_edac_inject_ctrl_write,
	.llseek = generic_file_llseek, },
	{ }
};

static void phytium_edac_create_debugfs_nodes(struct phytium_edac *edac)
{
	if (!IS_ENABLED(CONFIG_EDAC_DEBUG) || !edac->dfs) {
		dev_info(edac->dev, "edac debug is disable");
		return;
	}

	edac_debugfs_create_file("error_inject_ctrl", S_IWUSR, edac->dfs, edac,
				 &phytium_edac_debug_inject_fops[0]);
}

static int phytium_edac_device_add(struct phytium_edac *edac)
{
	struct edac_device_ctl_info *edac_dev;
	int res = 0;

	edac_dev = edac_device_alloc_ctl_info(
			sizeof(struct edac_device_ctl_info),
			"ras", 1, "soc", 1, 0, NULL,
			0, edac_device_alloc_index());
	if (!edac_dev)
		res = -ENOMEM;

	edac_dev->dev = edac->dev;
	edac_dev->mod_name = EDAC_MOD_STR;
	edac_dev->ctl_name = "phytium ras";
	edac_dev->dev_name = "soc";

	phytium_edac_create_debugfs_nodes(edac);

	res = edac_device_add_device(edac_dev);
	if (res > 0) {
		dev_err(edac->dev, "edac_device_add_device failed\n");
		goto err_free;
	}

	edac->edac_dev = edac_dev;
	dev_info(edac->dev, "phytium edac device registered\n");
	return 0;

err_free:
	edac_device_free_ctl_info(edac_dev);
	return res;
}

static int phytium_edac_device_remove(struct phytium_edac *edac)
{
	struct edac_device_ctl_info *edac_dev = edac->edac_dev;

	debugfs_remove_recursive(edac->dfs);
	edac_device_del_device(edac_dev->dev);
	edac_device_free_ctl_info(edac_dev);
	return 0;
}

static int get_error_id(struct phytium_edac *edac, int *error_id,
						int *error_group)
{
	unsigned int error_num = 0;
	u64 error_bit = 0;
	int ret = 0;
	int i = 0;
	int err_id = 0;

	/* Iterate over the ras node to check error status */
	for (i = 0; i < edac->num_err_group; i++) {
		error_num = get_error_num(edac, i);
		error_bit = readq(edac->ras_base[i] + ERR_GSR);
		for (err_id = 0; err_id < error_num; err_id++) {
			if (!(error_bit & BIT(err_id)))
				continue;
			else
				break;
		}
		if (err_id < error_num) {
			*error_id = err_id;
			*error_group = i;
			break;
		}
	}

	if (i >= edac->num_err_group) {
		ret = -1;
		dev_warn(edac->dev, "no error detect.\n");
	}

	return ret;
}

static void phytium_edac_error_report(struct phytium_edac *edac,
				const int error_id,
				const int error_group)
{
	const struct ras_error_info *err_info =
		edac->error_info[error_group];

	/* ignore pe220x soc_err id 40~43 */
	if ((err_info == pe220x_ras_soc_error) &&
	    (error_id >= 40) && (error_id <= 43))
		return;

	if (err_info[error_id].error_type == UNCORRECTED_ERROR) {
		edac_printk(KERN_CRIT, EDAC_MOD_STR, "uncorrected error: %s\n",
			err_info[error_id].error_str);
		edac_device_handle_ue(edac->edac_dev, 0, 0,
				err_info[error_id].error_str);
		/* Report the error via the trace interface */
		if (IS_ENABLED(CONFIG_RAS))
			trace_non_standard_event(&NULL_UUID_LE, &NULL_UUID_LE,
					EDAC_MOD_STR, SEV_RECOVERABLE,
					err_info[error_id].error_str,
					strlen(err_info[error_id].error_str));
	} else {
		edac_printk(KERN_CRIT, EDAC_MOD_STR, "corrected error: %s\n",
			err_info[error_id].error_str);
		edac_device_handle_ce(edac->edac_dev, 0, 0,
				err_info[error_id].error_str);
		/* Report the error via the trace interface */
		if (IS_ENABLED(CONFIG_RAS))
			trace_non_standard_event(&NULL_UUID_LE, &NULL_UUID_LE,
					EDAC_MOD_STR, SEV_CORRECTED,
					err_info[error_id].error_str,
					strlen(err_info[error_id].error_str));
	}
}

/*
 * clear error status and set correct error counter to 0xFE for trigger
 * interrupt when next correct error event
 */
static void phytium_edac_clear_error_status(struct phytium_edac *edac,
					const int error_id,
					const int error_group)
{
	writeq(MISC0_CEC(0XFE), edac->ras_base[error_group] +
			ERR_MISC0(error_id));
	writeq(GENMASK(31, 0), edac->ras_base[error_group] +
			ERR_STATUS(error_id));
}

static irqreturn_t phytium_edac_isr(int irq, void *dev_id)
{
	struct phytium_edac *edac = dev_id;
	int ret = 0;
	int error_group;
	int error_id;

	ret = get_error_id(edac, &error_id, &error_group);
	if (ret < 0)
		goto out;

	phytium_edac_error_report(edac, error_id, error_group);
	phytium_edac_clear_error_status(edac, error_id, error_group);

out:
	return IRQ_HANDLED;
}

static inline int of_address_count(struct device_node *np)
{
	struct resource res;
	int count = 0;

	while (of_address_to_resource(np, count, &res) == 0)
		count++;

	return count;
}

static int phytium_edac_probe(struct platform_device *pdev)
{
	struct phytium_edac *edac;
	struct resource *res;
	int ret = 0;
	int irq_cnt = 0;
	int irq = 0;
	int i = 0;

	edac = devm_kzalloc(&pdev->dev, sizeof(*edac), GFP_KERNEL);
	if (!edac) {
		ret = -ENOMEM;
		goto out;
	}

	edac->dev = &pdev->dev;
	platform_set_drvdata(pdev, edac);

	edac->error_info =
	  (const struct ras_error_info **)of_device_get_match_data(&pdev->dev);

	edac->num_err_group = of_address_count(pdev->dev.of_node);
	if (edac->num_err_group <= 0) {
		dev_err(&pdev->dev, "can't get error group count");
		goto out;
	}

	edac->ras_base = devm_kcalloc(&pdev->dev, edac->num_err_group,
			sizeof(*edac->ras_base), GFP_KERNEL);
	if (!edac->ras_base) {
		return -ENOMEM;
		goto out;
	}

	for (i = 0; i < edac->num_err_group; i++) {
		res = platform_get_resource(pdev, IORESOURCE_MEM, i);
		edac->ras_base[i] = devm_ioremap_resource(&pdev->dev, res);
		if (IS_ERR(edac->ras_base[i])) {
			dev_err(&pdev->dev, "no resource address\n");
			ret = PTR_ERR(edac->ras_base[i]);
			goto out;
		}
	}

	edac->dfs = edac_debugfs_create_dir(EDAC_MOD_STR);

	ret = phytium_edac_device_add(edac);
	if (ret) {
		dev_err(&pdev->dev, "can't add edac device");
		goto out;
	}

	phytium_ras_setup(edac);

	irq_cnt = platform_irq_count(pdev);
	if (irq_cnt < 0) {
		dev_err(&pdev->dev, "no irq resource\n");
		ret = -EINVAL;
		goto out;
	}

	for (i = 0; i < irq_cnt; i++) {
		irq = platform_get_irq(pdev, i);
		if (irq < 0) {
			dev_err(&pdev->dev, "invalid irq resource\n");
			ret = -EINVAL;
			goto out;
		}
		ret = devm_request_irq(&pdev->dev, irq,
					  phytium_edac_isr, IRQF_SHARED,
					  EDAC_MOD_STR, edac);
		if (ret) {
			dev_err(&pdev->dev,
				"could not request irq %d\n", irq);
			goto out;
		}
	}

out:
	return ret;
}

static int phytium_edac_remove(struct platform_device *pdev)
{
	struct phytium_edac *edac = dev_get_drvdata(&pdev->dev);

	phytium_edac_device_remove(edac);

	return 0;
}

static const struct of_device_id phytium_edac_of_match[] = {
	{ .compatible = "phytium,pe220x-edac",
	  .data = pe220x_ras_error },
	{ .compatible = "phytium,pd2208-edac",
	  .data = pd2208_ras_error },
	{},
};
MODULE_DEVICE_TABLE(of, phytium_edac_of_match);

static struct platform_driver phytium_edac_driver = {
	.probe = phytium_edac_probe,
	.remove = phytium_edac_remove,
	.driver = {
		.name = "phytium-edac",
		.of_match_table = phytium_edac_of_match,
	},
};

module_platform_driver(phytium_edac_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Huangjie <huangjie1663@phytium.com.cn>");
MODULE_VERSION(EDAC_DRIVER_VERSION);
