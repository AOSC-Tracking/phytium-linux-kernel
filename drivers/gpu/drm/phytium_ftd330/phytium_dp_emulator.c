// SPDX-License-Identifier: GPL-2.0
/* Phytium display drm driver
 *
 * Copyright (C) 2021-2025 Phytium Technology Co., Ltd.
 */
#include <linux/version.h>
#if LINUX_VERSION_CODE < KERNEL_VERSION(5, 4, 18)
#include <linux/acpi.h>
#endif

#include "phytium_dp.h"
#include "phytium_dp_reg.h"
#include "phytium_se_communicate.h"
#include "ftd330_dp.h"
#include "phytium_panel.h"
#include "phytium_edp_pwm.h"
#include "phytium_psr.h"
#include "phytium_vrr.h"
#include "phytium_dp_debugfs.h"
#include "uapi/linux/media-bus-format.h"
#include "ftd330_crtc.h"
#include "FTD330/ftd330_dc.h"

#define SINK_AVALIABLE 0

static struct drm_display_mode phytium_drm_modes[] = {
	/* 0x04 - 640x480@60Hz */
	{ DRM_MODE("640x480", DRM_MODE_TYPE_DRIVER, 25175, 640, 656,
			752, 800, 0, 480, 490, 492, 525, 0,
			DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 0x55 - 1280x720@60Hz */
	{ DRM_MODE("1280x720", DRM_MODE_TYPE_DRIVER, 74250, 1280, 1390,
			1430, 1650, 0, 720, 725, 730, 750, 0,
			DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
	/* 0x52 - 1920x1080@60Hz */
	{ DRM_MODE("1920x1080", DRM_MODE_TYPE_DRIVER, 148500, 1920, 2008,
			2052, 2200, 0, 1080, 1084, 1089, 1125, 0,
			DRM_MODE_FLAG_NHSYNC | DRM_MODE_FLAG_NVSYNC) },
	/* 97 - 3840x2160@60Hz 16:9 */
	{ DRM_MODE("3840x2160", DRM_MODE_TYPE_DRIVER, 594000, 3840, 4016,
			4104, 4400, 0, 2160, 2168, 2178, 2250, 0,
			DRM_MODE_FLAG_PHSYNC | DRM_MODE_FLAG_PVSYNC) },
};

static int phytium_port_virtual_to_physical(struct phytium_dp_device *phytium_dp)
{
        struct drm_device *dev =  phytium_dp->dev;
        struct ftd330_drm_private *priv = dev->dev_private;
        int port = phytium_dp->port;
        int i = 0;
        int count = 0;
        bool found = false;

        for (i = 0; i < DISPLAY_NUM; i++) {
                if (priv->info.pipe_mask & BIT(i)) {
                        if (count == port) {
                                found = true;
                                break;
                        } else {
                                count++;
                        }
                }
        }

        if (found) {
                return i;
        } else {
                pr_err("%s: no right dp port found\n", __func__);
                return -1;
        }
}

bool is_dp_powered(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	uint32_t group_offset = priv->dp_reg_base[phytium_dp->port];
	uint32_t dp_ver = 0;

	dp_ver = phytium_readl_reg(priv, group_offset, PHYTIUM_DP_DISPLAYPORT_VERSION);

	if (dp_ver) {
		return true;	
	} else {
		return false;
	}
}

static void phytium_get_native_mode(struct phytium_dp_device *phytium_dp)
{
	struct drm_display_mode *t, *mode;
	struct drm_connector *connector = &phytium_dp->connector;
	struct drm_display_mode *native_mode = &phytium_dp->native_mode;
	struct drm_display_mode temp_mode;

	list_for_each_entry_safe(mode, t, &connector->probed_modes, head) {
		if (mode->type & DRM_MODE_TYPE_PREFERRED) {
			memcpy(&temp_mode, mode, sizeof(*mode));
		}
	}

	list_for_each_entry_safe(mode, t, &connector->probed_modes, head) {
			if(mode->hdisplay == temp_mode.hdisplay &&
				mode->vdisplay == temp_mode.vdisplay &&
				drm_mode_vrefresh(mode) <= 61 &&
				drm_mode_vrefresh(mode) >= 59) {
				if (mode->hdisplay != native_mode->hdisplay ||
					mode->vdisplay != native_mode->vdisplay) {
					memcpy(native_mode, mode, sizeof(*mode));
					drm_mode_set_crtcinfo(native_mode, 0);
					break;
			}
		}
	}
	FTD330_LOG("native_mode is (%dx%d),clock is %d\n",
			native_mode->hdisplay, native_mode->vdisplay, native_mode->clock);
}



static int phytium_add_modes_noedid(struct drm_connector *connector, int hdisplay, int vdisplay)
{
	int i = 0;
	struct drm_display_mode *ptr = NULL;
	int num_mode = 0;
	int count = ARRAY_SIZE(phytium_drm_modes);
	struct drm_display_mode *mode = NULL;
	struct drm_device *dev = connector->dev;

	if (hdisplay < 0)
		hdisplay = 0;
	if (vdisplay < 0)
		vdisplay = 0;

	for (i = 0; i < count; i++) {
		ptr = &phytium_drm_modes[i];
		if (hdisplay && vdisplay) {
			/*
			 * Only when two are valid, they will be used to check
			 * whether the mode should be added to the mode list of
			 * the connector.
			 */
			if (ptr->hdisplay > hdisplay ||
					ptr->vdisplay > vdisplay)
				continue;
		}
		if (drm_mode_vrefresh(ptr) > 61)
			continue;
		mode = drm_mode_duplicate(dev, ptr);
		if (mode) {
			drm_mode_probed_add(connector, mode);
			num_mode++;
		}
	}

	pr_info("count = %d, num_mode = %d\n", count, num_mode);
	return num_mode;

}


static int phytium_connector_get_modes(struct drm_connector *connector)
{
	struct phytium_dp_device *phytium_dp = connector_to_dp_device(connector);

	phytium_add_modes_noedid(connector, 3840, 2160);
	drm_set_preferred_mode(connector, 1920, 1080);
	phytium_get_native_mode(phytium_dp);

	return 1;
}


#if 0
static struct drm_encoder *phytium_dp_best_encoder(struct drm_connector *connector)
{
	struct phytium_dp_device *phytium_dp = connector_to_dp_device(connector);

	return &phytium_dp->encoder;
}
#endif

static const
struct drm_connector_helper_funcs phytium_connector_helper_funcs = {
	.get_modes  = phytium_connector_get_modes,
	//.best_encoder = phytium_dp_best_encoder,
};

static void phytium_dp_registers_init(struct ftd330_drm_private *priv, uint32_t port)
{
	uint32_t group_offset = priv->dp_reg_base[port];

	phytium_writel_reg(priv, TRANSMITTER_OUTPUT_DISABLE,
			group_offset, PHYTIUM_DP_TRANSMITTER_OUTPUT_ENABLE);

	phytium_writel_reg(priv, 0, group_offset, PHYTIUM_DP_SOFT_RESET);
	phytium_writel_reg(priv, (~VIRTUAL_SOURCE_0_ENABLE)&VIRTUAL_SOURCE_0_ENABLE_MASK,
			group_offset, PHYTIUM_INPUT_SOURCE_ENABLE);
	phytium_writel_reg(priv, SST_MST_SOURCE_0_DISABLE,
			group_offset, PHYTIUM_DP_VIDEO_STREAM_ENABLE);
	phytium_writel_reg(priv, VIRTUAL_SOURCE_0_ENABLE,
			group_offset, PHYTIUM_INPUT_SOURCE_ENABLE);

	phytium_writel_reg(priv, 1,
                        group_offset, PHYTIUM_DP_HPD_STATE_RESET);

	phytium_writel_reg(priv, AUX_CLK_DIVIDER_100, group_offset, PHYTIUM_DP_AUX_CLK_DIVIDER);

	phytium_writel_reg(priv, 0, group_offset, PHYTIUM_DP_SOFT_RESET);


	phytium_writel_reg(priv, TRANSMITTER_OUTPUT_ENABLE,
			group_offset, PHYTIUM_DP_TRANSMITTER_OUTPUT_ENABLE);
	phytium_writel_reg(priv, SCRAMBLING_ENABLE, group_offset,
			PHYTIUM_DP_SCRAMBLING_DISABLE);
#ifdef CONFIG_PHYTIUM_LANE_TRAIN
	phytium_writel_reg(priv, 0x3c, group_offset, PHYTIUM_DP_INTERRUPT_MASK);
#endif

	phytium_writel_reg(priv, 0,
                        group_offset, PHYTIUM_DP_HPD_STATE_RESET);

	return;
}


void phytium_dp_hw_disable_video(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;
	uint32_t group_offset = priv->dp_reg_base[port];

	phytium_writel_reg(priv, SST_MST_SOURCE_0_DISABLE,
			   group_offset, PHYTIUM_DP_VIDEO_STREAM_ENABLE);
}

static bool phytium_dp_hw_video_is_enable(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port, config;
	uint32_t group_offset = priv->dp_reg_base[port];

	config = phytium_readl_reg(priv, group_offset, PHYTIUM_DP_VIDEO_STREAM_ENABLE);
	return config ? true : false;
}

void phytium_dp_hw_enable_video(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;
	uint32_t group_offset = priv->dp_reg_base[port];

	phytium_writel_reg(priv, SST_MST_SOURCE_0_ENABLE,
			   group_offset, PHYTIUM_DP_VIDEO_STREAM_ENABLE);

}

void phytium_dp_hw_config_video(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;
	uint32_t group_offset = priv->dp_reg_base[port];
	unsigned long link_bw, date_rate = 0;
	struct drm_display_info *display_info = &phytium_dp->connector.display_info;
	unsigned char tu_size = 64;
	unsigned long data_per_tu = 0;
	int symbols_per_tu, frac_symbols_per_tu, symbol_count, udc, value;

	phytium_dp->bpc = display_info->bpc;

	/* cal M/N and tu_size */
	phytium_writel_reg(priv, phytium_dp->mode.crtc_clock/10, group_offset, PHYTIUM_DP_M_VID);
	phytium_writel_reg(priv, phytium_dp->link_rate/10, group_offset, PHYTIUM_DP_N_VID);
	link_bw = phytium_dp->link_rate * phytium_dp->link_lane_count;
	date_rate = (phytium_dp->mode.crtc_clock * phytium_dp->bpc * 3)/8;

	/* mul 10 for register setting */
	data_per_tu = 10*tu_size * date_rate/link_bw;
	symbols_per_tu = (data_per_tu/10)&0xff;
	if (symbols_per_tu == 63)
		frac_symbols_per_tu = 0;
	else
		frac_symbols_per_tu = (data_per_tu%10*16/10) & 0xf;
	phytium_writel_reg(priv, frac_symbols_per_tu<<24 | symbols_per_tu<<16 | tu_size,
			   group_offset, PHYTIUM_DP_TRANSFER_UNIT_SIZE);

	symbol_count = (phytium_dp->mode.crtc_hdisplay*phytium_dp->bpc*3 + 7)/8;
	udc = (symbol_count + phytium_dp->link_lane_count - 1)/phytium_dp->link_lane_count;
	phytium_writel_reg(priv, udc, group_offset, PHYTIUM_DP_DATA_COUNT);

	/* config main stream attributes */
	phytium_writel_reg(priv, phytium_dp->mode.crtc_htotal,
			   group_offset, PHYTIUM_DP_MAIN_LINK_HTOTAL);
	phytium_writel_reg(priv, phytium_dp->mode.crtc_hdisplay,
			   group_offset, PHYTIUM_DP_MAIN_LINK_HRES);
	phytium_writel_reg(priv,
			   phytium_dp->mode.crtc_hsync_end - phytium_dp->mode.crtc_hsync_start,
			   group_offset, PHYTIUM_DP_MAIN_LINK_HSWIDTH);
	phytium_writel_reg(priv, phytium_dp->mode.crtc_htotal - phytium_dp->mode.crtc_hsync_start,
			   group_offset, PHYTIUM_DP_MAIN_LINK_HSTART);
	phytium_writel_reg(priv, phytium_dp->mode.crtc_vtotal,
			   group_offset, PHYTIUM_DP_MAIN_LINK_VTOTAL);
	phytium_writel_reg(priv, phytium_dp->mode.crtc_vdisplay,
			   group_offset, PHYTIUM_DP_MAIN_LINK_VRES);
	phytium_writel_reg(priv,
			   phytium_dp->mode.crtc_vsync_end - phytium_dp->mode.crtc_vsync_start,
			   group_offset, PHYTIUM_DP_MAIN_LINK_VSWIDTH);
	phytium_writel_reg(priv, phytium_dp->mode.crtc_vtotal - phytium_dp->mode.crtc_vsync_start,
			   group_offset, PHYTIUM_DP_MAIN_LINK_VSTART);
	
	value = 0;
	if (phytium_dp->mode.flags & DRM_MODE_FLAG_PHSYNC)
		value = value & (~HSYNC_POLARITY_LOW);
	else
		value = value | HSYNC_POLARITY_LOW;

	if (phytium_dp->mode.flags & DRM_MODE_FLAG_PVSYNC)
		value = value & (~PHYYNC_POLARITY_LOW);
	else
		value = value | PHYYNC_POLARITY_LOW;
	phytium_writel_reg(priv, value, group_offset, PHYTIUM_DP_MAIN_LINK_POLARITY);

	switch (phytium_dp->bpc) {
	case 10:
		value = (MISC0_BIT_DEPTH_10BIT << MISC0_BIT_DEPTH_OFFSET);
		break;
	case 6:
		value = (MISC0_BIT_DEPTH_6BIT << MISC0_BIT_DEPTH_OFFSET);
		break;
	default:
		value = (MISC0_BIT_DEPTH_8BIT << MISC0_BIT_DEPTH_OFFSET);
		break;
	}
	value |= (MISC0_COMPONENT_FORMAT_RGB << MISC0_COMPONENT_FORMAT_SHIFT)
		| MISC0_SYNCHRONOUS_CLOCK;
	phytium_writel_reg(priv, value, group_offset, PHYTIUM_DP_MAIN_LINK_MISC0);
	phytium_writel_reg(priv, 0, group_offset, PHYTIUM_DP_MAIN_LINK_MISC1);

	value = USER_ODDEVEN_POLARITY_HIGH | USER_DATA_ENABLE_POLARITY_HIGH;
	if (phytium_dp->mode.flags & DRM_MODE_FLAG_PHSYNC)
		value = value | USER_HSYNC_POLARITY_HIGH;
	else
		value = value & (~USER_HSYNC_POLARITY_HIGH);
	if (phytium_dp->mode.flags & DRM_MODE_FLAG_PVSYNC)
		value = value | USER_PHYYNC_POLARITY_HIGH;
	else
		value = value & (~USER_PHYYNC_POLARITY_HIGH);
	phytium_writel_reg(priv, value, group_offset, PHYTIUM_DP_USER_SYNC_POLARITY);
	phytium_dp->freq = drm_mode_vrefresh(&phytium_dp->mode);

	phytium_writel_reg(priv, DISABLE, group_offset, PHYTIUM_DP_IDLE_PATTERN_DISBALE);
}
static void phytium_dp_hw_disable_output(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;
	uint32_t group_offset = priv->dp_reg_base[port];

	phytium_writel_reg(priv, TRANSMITTER_OUTPUT_DISABLE,
			   group_offset, PHYTIUM_DP_TRANSMITTER_OUTPUT_ENABLE);
	phytium_writel_reg(priv, LINK_SOFT_RESET, group_offset, PHYTIUM_DP_SOFT_RESET);
}

static void phytium_dp_hw_enable_output(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;
	uint32_t group_offset = priv->dp_reg_base[port];

	phytium_writel_reg(priv, LINK_SOFT_RESET, group_offset, PHYTIUM_DP_SOFT_RESET);
	phytium_writel_reg(priv, TRANSMITTER_OUTPUT_ENABLE,
			   group_offset, PHYTIUM_DP_TRANSMITTER_OUTPUT_ENABLE);
}

static void phytium_dp_hw_enable_input_source(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;
	uint32_t group_offset = priv->dp_reg_base[port];

	phytium_writel_reg(priv, VIRTUAL_SOURCE_0_ENABLE,
			   group_offset, PHYTIUM_INPUT_SOURCE_ENABLE);
}

static void phytium_dp_hw_disable_input_source(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;

	phytium_writel_reg(priv, (~VIRTUAL_SOURCE_0_ENABLE)&VIRTUAL_SOURCE_0_ENABLE_MASK,
			   priv->dp_reg_base[port], PHYTIUM_INPUT_SOURCE_ENABLE);
}

#if 0
static bool phytium_dp_hw_output_is_enable(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;
	uint32_t group_offset = priv->dp_reg_base[port];
	int config = 0;

	config = phytium_readl_reg(priv, group_offset, PHYTIUM_DP_TRANSMITTER_OUTPUT_ENABLE);
	return config ? true : false;
}
#endif
#ifdef CONFIG_PHYTIUM_PSR
static int phytium_dplp_hw_get_psr_state(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;
	uint32_t val = 0;
	uint32_t group_offset = priv->dplp_reg_base[port];

	val = phytium_dplp_read_reg(priv, group_offset, PHYTIUM_DPLP_FRAME_COMP_INIT_STATE);

	if (val & PSR_AVAILABLE) {

		FTD330_LOG("get psr avaliable irq\n");
		phytium_dp->dplp_frame_compare_state.psr_available = true;
		phytium_dp->dplp_frame_compare_state.frame_change_in_psr = false;
#ifdef CONFIG_PHYTIUM_PSR_SF_UPDATE
		phytium_dp->dplp_frame_compare_state.psr_exit = false;
#endif
		phytium_dplp_write_reg(priv, PSR_AVAILABLE, group_offset,
								PHYTIUM_DPLP_FRAME_COMP_INIT_CLEAR);
		return 1;
	} else if (val & FRAME_CHANGE_IN_PSR) {
		FTD330_LOG("get FRAME_CHANGE_IN_PSR irq\n");
		phytium_dp->dplp_frame_compare_state.frame_change_in_psr = true;
		phytium_dp->dplp_frame_compare_state.psr_available = false;
#ifdef CONFIG_PHYTIUM_PSR_SF_UPDATE
		phytium_dp->psr.frame_change_irq_nums += 1;
#endif
		phytium_dplp_write_reg(priv, FRAME_CHANGE_IN_PSR, group_offset,
								PHYTIUM_DPLP_FRAME_COMP_INIT_CLEAR);
		return 1;
	}
	return 0;
}
#endif

#ifdef CONFIG_PHYTIUM_LOW_FPS
static int phytium_dplp_hw_get_low_fps_state(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;
	uint32_t val = 0;
	uint32_t group_offset = priv->dplp_reg_base[port];

	val = phytium_dplp_read_reg(priv, group_offset, PHYTIUM_DPLP_FRAME_COMP_INIT_STATE);

	if ((val&LOWFPS_AVAILABLE) && !(val&FRAME_CHANGE_IN_WAIT_LOWFPS)) {
		phytium_dp->dplp_frame_compare_state.lowfps_available = true;
		phytium_dp->dplp_frame_compare_state.lowfps_exit = false;
		phytium_dplp_write_reg(priv, LOWFPS_AVAILABLE, group_offset,
								PHYTIUM_DPLP_FRAME_COMP_INIT_CLEAR);
		FTD330_LOG("get LOWFPS_AVAILABLE irq\n");
		return 1;
	} else if (val & LOWFPS_EXIT) {
		phytium_dp->dplp_frame_compare_state.lowfps_exit = true;
		phytium_dp->dplp_frame_compare_state.lowfps_available = false;
		phytium_dplp_write_reg(priv, LOWFPS_EXIT, group_offset,
								PHYTIUM_DPLP_FRAME_COMP_INIT_CLEAR);
		FTD330_LOG("get LOWFPS_EXIT irq\n");
		return 1;
	}
	return 0;
}
#endif
static void phytium_dp_hw_get_hpd_state(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;
	uint32_t val = 0, raw_state = 0;
	uint32_t group_offset = priv->dp_reg_base[port];

	val = phytium_readl_reg(priv, group_offset, PHYTIUM_DP_INTERRUPT_RAW_STATUS);

	/* maybe miss hpd, so used for clear PHYTIUM_DP_INTERRUPT_RAW_STATUS */
	phytium_readl_reg(priv, group_offset, PHYTIUM_DP_INTERRUPT_STATUS);
	raw_state = phytium_readl_reg(priv, group_offset, PHYTIUM_DP_SINK_HPD_STATE);
	if (val & HPD_EVENT) {
		phytium_dp->dp_hpd_state.hpd_event_state = true;
		FTD330_LOG("dp_%d hpd evnet\n",phytium_dp->port);
	}

	if (val & HPD_IRQ) {
		phytium_dp->dp_hpd_state.hpd_irq_state = true;
		FTD330_LOG("dp_%d hpd irq\n",phytium_dp->port);
	}

	if (raw_state & HPD_CONNECT) {
		phytium_dp->dp_hpd_state.hpd_raw_state = true;
	} else {
		phytium_dp->dp_hpd_state.hpd_raw_state = false;
	}

		FTD330_LOG("dp_%d hpd raw state = %d\n",phytium_dp->port, phytium_dp->dp_hpd_state.hpd_raw_state);
}
#ifdef CONFIG_PHYTIUM_PSR
static void phytium_dplp_hw_psr_irq_setup(struct phytium_dp_device *phytium_dp, bool enable)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;
	uint32_t dplp_offset = priv->dplp_reg_base[port];
	uint32_t val;

	val = phytium_dplp_read_reg(priv, dplp_offset, PHYTIUM_DPLP_FRAME_COMP_INIT_MASK);
	if (enable) {
		phytium_dplp_write_reg(priv, PSR_AVAILABLE | LOWFPS_AVAILABLE, dplp_offset,
					PHYTIUM_DPLP_FRAME_COMP_INIT_CLEAR);
		val = val ? (val&PSR_AVAILABLE_MASK) : PSR_AVAILABLE_MASK;
		val &= LOW_FPS_ENABLE_MASK;
		phytium_dplp_write_reg(priv, val, dplp_offset, PHYTIUM_DPLP_FRAME_COMP_INIT_MASK);
	}
	else {
		val |= (PSR_SHIELD_MASK | LOWFPS_AVAILABLE_MASK);
		phytium_dplp_write_reg(priv, val, dplp_offset, PHYTIUM_DPLP_FRAME_COMP_INIT_MASK);
	}
}
#endif

void phytium_dp_hw_hpd_irq_setup(struct phytium_dp_device *phytium_dp, bool enable, bool handle_irq)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;
	uint32_t group_offset = priv->dp_reg_base[port];

	FTD330_LOG_TRACE;
	if (enable) {
		phytium_writel_reg(priv, HPD_OTHER_MASK, group_offset, PHYTIUM_DP_INTERRUPT_MASK);
	} else {
		phytium_writel_reg(priv, HPD_IRQ_MASK|HPD_EVENT_MASK|HPD_OTHER_MASK,
				   group_offset, PHYTIUM_DP_INTERRUPT_MASK);
	}
}


void phytium_dp_hpd_irq_setup(struct drm_device *dev, bool enable, bool handle_irq)
{
	struct phytium_dp_device *phytium_dp;
	struct drm_encoder *encoder;
	
	FTD330_LOG_TRACE;
	drm_for_each_encoder(encoder, dev) {
		phytium_dp = encoder_to_dp_device(encoder);
		phytium_dp_hw_hpd_irq_setup(phytium_dp, enable, handle_irq);
	}
}

#ifndef CONFIG_PHYTIUM_PCIE
#ifdef CONFIG_PHYTIUM_POWER_OPERATION
static void
phytium_display_power_request_acpi(struct ftd330_drm_private *priv, bool enable, int display_id)
{
	struct platform_device *pdev = priv->pdev;
	struct device *dev = &pdev->dev;
	acpi_handle handle = ACPI_HANDLE(dev);
	union acpi_object args[3];
	struct acpi_object_list arg_list = {
		.pointer = args,
		.count = ARRAY_SIZE(args),
	};
	acpi_status status;
	long long ret;

	args[0].type = ACPI_TYPE_INTEGER;
	args[0].integer.value = 2;
	args[1].type = ACPI_TYPE_INTEGER;
	args[1].integer.value = display_id;
	args[2].type = ACPI_TYPE_INTEGER;
	args[2].integer.value = 0x0;
	if (!has_acpi_companion(dev))
		pr_err("get acpi device failed\n");
	if (enable) {
		status = acpi_evaluate_integer(handle, "PPWO", &arg_list, &ret);
		if (ACPI_FAILURE(status)) {
			pr_err("No PS0 Method\n");
			goto out;
		}
		if (ret < 0) {
			pr_err("Failed to suspend");
			goto out;
		}
	} else {
		status = acpi_evaluate_integer(handle, "PPWD", &arg_list, &ret);
		if (ACPI_FAILURE(status)) {
			pr_err("No PS3 Method\n");
			goto out;
		}
		if (ret < 0) {
			pr_err("Failed to resume");
			goto out;
		}
	}
out:
	pr_info("FTD330 acpi power operation succeed\n");
	return;
}
#endif
#endif

#ifdef CONFIG_PHYTIUM_POWER_OPERATION
void phytium_display_power_request(struct ftd330_drm_private *priv, bool enable, int display_id)
{
#ifdef CONFIG_PHYTIUM_PCIE
	phytium_display_power_request_se(priv, enable, display_id);
#else
	struct platform_device *pdev = priv->pdev;
	int val = 0;
	bool power_status;
	u32 group_offset = 0;

	group_offset = PHYTIUM_FTD330_DP_REG_OFFSET + display_id*PHYTIUM_FTD330_DP_REG_INTERVAL;
	val = phytium_readl_reg(priv, group_offset, PHYTIUM_DP_DISPLAYPORT_VERSION);

	if (val != 0) {
		power_status = true;
	} else {
		power_status = false;
	}

	if (enable && power_status) {
		return;
	}

	if (!enable && !power_status) {
		return;
	}

	FTD330_LOG_TRACE;
	if (pdev->dev.of_node)
		phytium_display_power_request_se(priv, enable, display_id);
	else if (has_acpi_companion(&pdev->dev))
		phytium_display_power_request_acpi(priv, enable, display_id);
#endif

}
#endif


void phytium_dp_hpd_work_func(struct work_struct *work)
{
	struct ftd330_drm_private *priv =
		container_of(work, struct ftd330_drm_private, hotplug_work);
	struct drm_device *dev = priv->drm_dev;
	struct drm_connector_list_iter conn_iter;
	struct drm_connector *connector;
	enum drm_connector_status old_status;
	bool changed = false;

	FTD330_LOG_TRACE;

	mutex_lock(&dev->mode_config.mutex);
	drm_connector_list_iter_begin(dev, &conn_iter);
	drm_for_each_connector_iter(connector, &conn_iter) { //遍历每一个connector
		if (!connector->force) {
			old_status = connector->status;
			connector->status = drm_helper_probe_detect(connector, NULL, false);
			if (old_status != connector->status) {
				const char *old, *new;

				old = drm_get_connector_status_name(old_status);
				new = drm_get_connector_status_name(connector->status);
				pr_info("[CONNECTOR:%d:%s] status updated from %s to %s\n",
					connector->base.id,
					connector->name,
					old, new);
				changed = true;
			}
		}
	}
	drm_connector_list_iter_end(&conn_iter);
	mutex_unlock(&dev->mode_config.mutex);

	if (changed)
		drm_kms_helper_hotplug_event(dev);

	phytium_dp_hpd_irq_setup(dev, true, true);

}

void phytium_display_power_request_on(struct drm_device *dev, int physical_display_id, bool handle_irq)
{
	struct ftd330_drm_private *priv = dev->dev_private;
	struct phytium_dp_device *phytium_dp = NULL;
	
	FTD330_LOG_TRACE;

	phytium_dp = priv->phytium_dp[physical_display_id];
	if (physical_display_id == DISPLAY_0) {
		mutex_lock(&priv->power_mutex);
		phytium_display_power_request(priv, true, DC_0);
		phytium_dc_registers_init(priv, DC_0);
		phytium_dp_registers_init(priv, priv->phytium_dp[DISPLAY_0]->port);
		phytium_dplp_init_port(phytium_dp);
		mutex_unlock(&priv->power_mutex);
		phytium_dp_hw_hpd_irq_setup(priv->phytium_dp[DISPLAY_0], true, handle_irq);
	} else {
		mutex_lock(&priv->power_mutex);
		if (!priv->phytium_dp[DISPLAY_1]->dp_power_enable_status &&
				!priv->phytium_dp[DISPLAY_2]->dp_power_enable_status) {
			priv->phytium_dp[physical_display_id]->dp_power_enable_status = true;
			phytium_display_power_request(priv, true, DC_1);
			phytium_dc_registers_init(priv, DC_1);
			
			phytium_dp = priv->phytium_dp[DISPLAY_1];
			phytium_dplp_init_port(phytium_dp);
			phytium_dp_registers_init(priv, priv->phytium_dp[DISPLAY_1]->port);

			phytium_dp = priv->phytium_dp[DISPLAY_2];
			phytium_dplp_init_port(phytium_dp);
			phytium_dp_registers_init(priv, priv->phytium_dp[DISPLAY_2]->port);

			priv->phytium_dp[DISPLAY_1]->dp_power_enable_status = true;
			priv->phytium_dp[DISPLAY_2]->dp_power_enable_status = true;

			phytium_dp_hw_hpd_irq_setup(priv->phytium_dp[DISPLAY_1], true, handle_irq);
			phytium_dp_hw_hpd_irq_setup(priv->phytium_dp[DISPLAY_2], true, handle_irq);
		}
		mutex_unlock(&priv->power_mutex);
	}
}


void phytium_display_power_request_off(struct drm_device *dev, int physical_display_id)
{
	struct ftd330_drm_private *priv = dev->dev_private;
	
	FTD330_LOG_TRACE;
	mutex_lock(&priv->power_mutex);

	if (physical_display_id == DISPLAY_0) {
#ifdef CONFIG_PHYTIUM_POWER_OPERATION
		phytium_dp_hw_hpd_irq_setup(priv->phytium_dp[DISPLAY_0], false, false);
		phytium_display_power_request(priv, false, DC_0);
#endif
	} else {
		priv->phytium_dp[physical_display_id]->dp_power_enable_status = false;
#ifdef CONFIG_PHYTIUM_POWER_OPERATION
		if (!priv->phytium_dp[DISPLAY_1]->dp_power_enable_status &&
				  !priv->phytium_dp[DISPLAY_2]->dp_power_enable_status) {
			phytium_dp_hw_hpd_irq_setup(priv->phytium_dp[DISPLAY_1], false, false);
			phytium_dp_hw_hpd_irq_setup(priv->phytium_dp[DISPLAY_2], false, false);
			phytium_display_power_request(priv, false, DC_1);
		}
#endif
	}
	mutex_unlock(&priv->power_mutex);
}


void phytium_dp_power_work_func(struct work_struct *work)
{
	struct phytium_dp_device *phytium_dp =
		container_of(work, struct phytium_dp_device, power_work);
	struct drm_device *dev = phytium_dp->dev;
	int temp_port = 0;

	FTD330_LOG_TRACE;

	temp_port = phytium_port_virtual_to_physical(phytium_dp);
	phytium_display_power_request_on(dev, temp_port, true);
}

#ifdef CONFIG_PHYTIUM_PSR
static void phytium_dp_psr_work_func(struct work_struct *work)
{
	struct phytium_dp_device *phytium_dp =
		container_of(work, struct phytium_dp_device, psr_work);
	struct drm_device *dev = phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	uint32_t group_offset = priv->dplp_reg_base[phytium_dp->port];

	FTD330_LOG_TRACE;
	mutex_lock(&phytium_dp->low_power_mutex);
	if (phytium_dp->dplp_frame_compare_state.psr_available == true) {
		phytium_dplp_write_reg(priv, ENTER_PSR_REQ, group_offset,
								PHYTIUM_DPLP_FRAME_COMP_REQ);
		phytium_psr_enable(phytium_dp);
#ifdef CONFIG_PHYTIUM_PSR_SF_UPDATE
	} else if (phytium_dp->dplp_frame_compare_state.frame_change_in_psr == true) {
		phytium_psr_exit_sf_detect(phytium_dp);
	}

	if (phytium_dp->dplp_frame_compare_state.psr_exit == true) {
		FTD330_LOG("exit psr\n");
		phytium_dp->psr.frame_change_irq_nums = 0;
		phytium_psr_disable(phytium_dp);
		phytium_dplp_write_reg(priv, EXIT_PSR_REQ, group_offset,
								PHYTIUM_DPLP_FRAME_COMP_REQ);
	} else {
		phytium_psr_sf_update(phytium_dp);
	}
#else
	} else if (phytium_dp->dplp_frame_compare_state.frame_change_in_psr == true) {
		FTD330_LOG("get frame_change_in_psr irq\n");
		phytium_psr_disable(phytium_dp);
		phytium_dplp_write_reg(priv, EXIT_PSR_REQ, group_offset,
								PHYTIUM_DPLP_FRAME_COMP_REQ);
		phytium_dplp_write_reg(priv, PSR_AVAILABLE|FRAME_CHANGE_IN_PSR, group_offset,
						PHYTIUM_DPLP_FRAME_COMP_INIT_CLEAR);
		phytium_dplp_hw_psr_irq_setup(phytium_dp, true);
	}
#endif
	mutex_unlock(&phytium_dp->low_power_mutex);
}
#endif

#ifdef CONFIG_PHYTIUM_LOW_FPS
static void phytium_dp_low_fps_work_func(struct work_struct *work)
{
	struct phytium_dp_device *phytium_dp =
		container_of(work, struct phytium_dp_device, low_fps_work);
	struct drm_device *dev = phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	uint32_t group_offset = priv->dplp_reg_base[phytium_dp->port];

	FTD330_LOG_TRACE;
	mutex_lock(&phytium_dp->low_power_mutex);
	if (phytium_dp->dplp_frame_compare_state.lowfps_available == true) {
		phytium_dplp_write_reg(priv, ENTER_LOWFPS_REQ, group_offset,
								PHYTIUM_DPLP_FRAME_COMP_REQ);
		phytium_change_fps(phytium_dp, true, 0);
	} else if (phytium_dp->dplp_frame_compare_state.lowfps_exit == true) {
		phytium_change_fps(phytium_dp, false, 0);
		phytium_dplp_write_reg(priv, EXIT_LOWFPS_REQ, group_offset,
								PHYTIUM_DPLP_FRAME_COMP_REQ);
	}
	mutex_unlock(&phytium_dp->low_power_mutex);
}
#endif

irqreturn_t phytium_dp_hpd_irq_handler(int irq, void *data)
{
	struct phytium_dp_device *phytium_dp = data;
	struct phytium_dp_device *temp_phytium_dp = NULL;
	struct drm_device *dev = phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	struct drm_encoder *encoder = NULL;
#ifdef CONFIG_PHYTIUM_WRITEBACK
	pr_info("Using writeback Connector,no hotplug event handled\n");
	return IRQ_HANDLED;
#endif
	bool changed = false;
	unsigned long irq_flag;
#ifdef CONFIG_PHYTIUM_LOW_FPS
	bool lowfps_changed = false;
#endif

	FTD330_LOG_TRACE;

	spin_lock_irqsave(&priv->hotplug_irq_lock, irq_flag);
	drm_for_each_encoder(encoder, dev) {
			temp_phytium_dp = encoder_to_dp_device(encoder);
			phytium_dp_hw_get_hpd_state(temp_phytium_dp);
			if (temp_phytium_dp->dp_hpd_state.hpd_event_state
			|| temp_phytium_dp->dp_hpd_state.hpd_irq_state) {
		changed = true;
			}
	}

	spin_unlock_irqrestore(&priv->hotplug_irq_lock,irq_flag);

	if (changed) {
		phytium_dp_hpd_irq_setup(dev, false, false);
		schedule_work(&priv->hotplug_work);
	}


	return IRQ_HANDLED;
}

irqreturn_t phytium_dp_power_on_irq_handler(int irq, void *data)
{
	struct phytium_dp_device *phytium_dp = data;
	struct drm_device *dev = phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	uint32_t group_offset = priv->dplp_reg_base[phytium_dp->port];
	uint32_t status;

	FTD330_LOG_TRACE;
#ifdef CONFIG_PHYTIUM_WRITEBACK
	pr_info("Using writeback Connector,no hotplug event handled\n");
	return IRQ_HANDLED;
#endif

	status = phytium_dplp_read_reg(priv, group_offset, 0x4);
        phytium_dplp_write_reg(priv, CLEAN_HPD_CONNECT, group_offset,
                                          PHYTIUM_DPLP_HPD_PWRUP_INT_CLEAR);
	if (!(status & 0x01)) {
		return IRQ_HANDLED;
	}

	phytium_dplp_deinit_port(phytium_dp);
	schedule_work(&phytium_dp->power_work);
	return IRQ_HANDLED;
}


static enum drm_connector_status
phytium_connector_detect(struct drm_connector *connector, bool force)
{
#ifdef CONFIG_VERISILICON_POWER_OPERATION
		struct phytium_dp_device *phytium_dp = connector_to_dp_device(connector);
		struct drm_device *dev = phytium_dp->dev;
		struct ftd330_drm_private *priv = dev->dev_private;
		uint32_t group_offset = priv->dp_reg_base[phytium_dp->port];
	
		if ((phytium_readl_reg(priv, group_offset, PHYTIUM_DP_SINK_HPD_STATE) & 0x00000001) == 0)
			return connector_status_disconnected;
	
		return connector_status_connected;
#else
		return connector_status_connected;
#endif
}

static void
phytium_connector_destroy(struct drm_connector *connector)
{
	struct phytium_dp_device *phytium_dp = connector_to_dp_device(connector);
	struct drm_device *dev = phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;

	cancel_work_sync(&priv->hotplug_work);

	drm_connector_cleanup(connector);
	if (phytium_dp)
		kfree(phytium_dp);
}

static int
phytium_dp_connector_register(struct drm_connector *connector)
{
	struct phytium_dp_device *phytium_dp = connector_to_dp_device(connector);
	int ret = 0;
	struct drm_device *dev = phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	struct platform_device *pdev = priv->pdev;
	int temp_port = phytium_port_virtual_to_physical(phytium_dp);

	/* maybe miss hpd, clear PHYTIUM_DP_INTERRUPT_RAW_STATUS before enable interrupt*/

	ret = request_irq(platform_get_irq(pdev, (temp_port + 2)),
						phytium_dp_hpd_irq_handler, IRQF_SHARED,
						dev_name(&pdev->dev), phytium_dp);//下电中断
	if (ret < 0) {
		pr_info("Failed to install irq\n");
		return ret;
	}

	ret = request_irq(platform_get_irq(pdev, (temp_port + 5)),
					phytium_dp_power_on_irq_handler, IRQF_SHARED,
					dev_name(&pdev->dev), phytium_dp);//上电中断
	if (ret < 0) {
		pr_info("Failed to install irq\n");
		return ret;
	}

#ifdef CONFIG_DEBUG_FS
	ret = phytium_dp_debugfs_connector_add(connector);
	if (ret)
		DRM_ERROR("failed to register phytium connector debugfs(ret=%d)\n", ret);
#endif

	return 0;
}

static void
phytium_dp_connector_unregister(struct drm_connector *connector)
{

	struct phytium_dp_device *phytium_dp = connector_to_dp_device(connector);

	struct drm_device *dev = phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	struct platform_device *pdev = priv->pdev;
	int temp_port = phytium_port_virtual_to_physical(phytium_dp);

	free_irq(platform_get_irq(pdev, (temp_port + 2)), phytium_dp);
	free_irq(platform_get_irq(pdev, (temp_port + 5)), phytium_dp);

}

static const struct drm_connector_funcs phytium_connector_funcs = {
	.dpms			= drm_helper_connector_dpms,
	.detect			= phytium_connector_detect,
	.fill_modes		= drm_helper_probe_single_connector_modes,
	.destroy		= phytium_connector_destroy,
	.reset			= drm_atomic_helper_connector_reset,
	.atomic_duplicate_state	= drm_atomic_helper_connector_duplicate_state,
	.atomic_destroy_state	= drm_atomic_helper_connector_destroy_state,
	.late_register		= phytium_dp_connector_register,
	.early_unregister	= phytium_dp_connector_unregister,
};

static void phytium_dp_encoder_mode_set(struct drm_encoder *encoder,
						    struct drm_display_mode *mode,
						    struct drm_display_mode *adjusted)
{
	struct phytium_dp_device *phytium_dp = encoder_to_dp_device(encoder);

	FTD330_LOG("%s_%d: crtc_hdisplay:%d  %d\n", __func__, __LINE__,
			mode->crtc_hdisplay, adjusted->crtc_hdisplay);

	/*
	 * this func was called before dc_check when S3 return
	 * (screen off-> S3 ->return) on kylin.So incase ajusted
	 * was not set,we set it here to avoid bad dp configuration
	 */
	drm_mode_copy(&phytium_dp->mode, adjusted);
}

static void phytium_encoder_disable(struct drm_encoder *encoder)
{
	struct phytium_dp_device *phytium_dp = encoder_to_dp_device(encoder);

	FTD330_LOG_TRACE;

	phytium_dp_hw_disable_video(phytium_dp);

	mdelay(50);
}


#if 0
static void
phytium_dp_modify_dc_hsync_time(struct phytium_dp_device *phytium_dp)
{
        struct drm_device *dev = NULL;
        struct ftd330_drm_private *priv = NULL;
        struct ftd330_dc *dc = NULL;
        struct drm_connector *connector = NULL;
        struct drm_crtc *crtc = NULL;
        struct ftd330_crtc *ftd330_crtc = NULL;
        struct dc_hw *hw = NULL;
	u32 display_offset = 0;
	struct drm_display_mode temp_mode = phytium_dp->mode;
	int back_porch = 0;
	int lines_add = 0;
	bool h_sync_polarity = false;
	int fifo_value = 0;
	uint32_t group_offset;

        dev = phytium_dp->dev;
        if (!dev) {
                DRM_INFO("%s: dev is null\n", __func__);
                return;
        }

        priv = dev->dev_private;
        if (!priv) {
                DRM_INFO("%s: priv is null\n", __func__);
                return;
        }

        dc = dev_get_drvdata(priv->dc_dev);
        if (!dc) {
                DRM_INFO("%s: dc is null\n", __func__);
                return;
        }

        connector = &phytium_dp->connector;
        if (!connector) {
                DRM_INFO("%s: connector is null\n", __func__);
                return;
        }

        crtc = connector->state->crtc;
        if (!crtc) {
                return;
        }

        ftd330_crtc = to_ftd330_crtc(crtc);
        if (!ftd330_crtc) {
                DRM_INFO("%s: ftd330_crtc is null\n", __func__);
                return;
        }

        hw = &dc->hw;
        if (!hw) {
                DRM_INFO("%s: hw is null\n", __func__);
                return;
        }
	
	group_offset = priv->dp_reg_base[phytium_dp->port];;
	fifo_value = phytium_readl_reg(priv, group_offset, PHYTIUM_DP_DATA_CONTROL);
	if (fifo_value != PHYTIUM_DP_DEFAULT_FIFO_VALUE) {
		FTD330_LOG("fifo value changed,no hsync change\n");
		return;
	}


	FTD330_LOG("before modify hdisplay:%d,h_start:%d,h_end:%d,h_total:%d\n",
	                temp_mode.crtc_hdisplay, temp_mode.crtc_hsync_start,
	                temp_mode.crtc_hsync_end, temp_mode.crtc_htotal);


        display_offset = display_get_addr_offset(hw, ftd330_crtc->id);

	back_porch = temp_mode.crtc_htotal - temp_mode.crtc_hsync_end;
	lines_add = phytium_dp->link_lane_count * 30 - (temp_mode.crtc_hsync_end - temp_mode.crtc_hsync_start);

	if (lines_add > 0) {
		if (lines_add < back_porch) {
			temp_mode.crtc_hsync_end += lines_add;
		} else {
			temp_mode.crtc_hsync_end += (back_porch -1);
			temp_mode.crtc_hsync_start -= (lines_add - back_porch + 1);

			if (temp_mode.crtc_hsync_start < temp_mode.crtc_hdisplay) {
				temp_mode.crtc_hsync_start = temp_mode.crtc_hdisplay + 1;
			}
		}
	} else {
		if ((temp_mode.crtc_hsync_end + lines_add) > temp_mode.crtc_hsync_start) {
			temp_mode.crtc_hsync_end += lines_add;
		} else {
			if ((temp_mode.crtc_hsync_end + lines_add) > temp_mode.crtc_hdisplay) {
				temp_mode.crtc_hsync_start = temp_mode.crtc_hsync_end + lines_add;
			} else {
				temp_mode.crtc_hsync_start = temp_mode.crtc_hdisplay + 1;
			}
			temp_mode.crtc_hsync_end = temp_mode.crtc_hsync_start + 1;
		}
	}

	if (temp_mode.flags & DRM_MODE_FLAG_PHSYNC) {
		h_sync_polarity = true;
	} else {
		h_sync_polarity = false;
	}

	dc_write(hw, DC_DISPLAY_H_SYNC + display_offset,
			temp_mode.crtc_hsync_start | (temp_mode.crtc_hsync_end << 15) |
				(h_sync_polarity ? 0 : BIT(31)) | BIT(30));
	FTD330_LOG("after modify hdisplay:%d,h_start:%d,h_end:%d,h_total:%d\n",
			temp_mode.crtc_hdisplay, temp_mode.crtc_hsync_start,
			temp_mode.crtc_hsync_end, temp_mode.crtc_htotal);

}
#endif


enum drm_mode_status
phytium_encoder_mode_valid(struct drm_encoder *encoder, const struct drm_display_mode *mode)
{

	struct phytium_dp_device *phytium_dp = encoder_to_dp_device(encoder);
	struct drm_display_info *display_info = &phytium_dp->connector.display_info;
	enum drm_mode_status ret;

	switch (display_info->bpc) {
	case 10:
	case 6:
	case 8:
		break;
	default:
		DRM_INFO("not support bpc(%d)\n", display_info->bpc);
		display_info->bpc = 8;
		break;
	}

	if ((display_info->color_formats & DRM_COLOR_FORMAT_RGB444) == 0) {
		DRM_INFO("not support color_format(%d)\n", display_info->color_formats);
		display_info->color_formats = DRM_COLOR_FORMAT_RGB444;
	}

	if ((mode->hdisplay == 1600) && (mode->vdisplay == 900)) {
		ret = MODE_BAD_HVALUE;
		goto status_in_total;
	}

	if ((mode->hdisplay == 1024) && (mode->clock > 78000)) {
		ret = MODE_BAD_HVALUE;
		goto status_in_total;
	}

	if ((mode->hdisplay < 640) || (mode->vdisplay < 480)) {
		ret = MODE_BAD_HVALUE;
		goto status_in_total;
	}

	if ((mode->hdisplay == 2560) && (mode->vdisplay == 1440)) {
		if (drm_mode_vrefresh(mode) > 120) {
			ret = MODE_HBLANK_NARROW;
			goto status_in_total;
		}
	}
	ret = MODE_OK;

status_in_total:
	return ret;

}

static int phytium_encoder_atomic_check(struct drm_encoder *encoder,
                                   struct drm_crtc_state *crtc_state,
                                   struct drm_connector_state *conn_state)
{
	struct drm_connector *connector = conn_state->connector;
	struct phytium_dp_device *phytium_dp = connector_to_dp_device(connector);
	struct drm_display_info *info = &connector->display_info;
	struct phytium_display_mode *phytium_mode = NULL;
	struct ftd330_crtc_state *ftd330_crtc_state = to_ftd330_crtc_state(crtc_state);
	u32 bus_format;

	switch (info->bpc) {
		case 6:
			bus_format = MEDIA_BUS_FMT_RGB666_1X18;
			break;
		case 8:
			bus_format = MEDIA_BUS_FMT_RGB888_1X24;
			break;
		case 10:
			bus_format = MEDIA_BUS_FMT_RGB101010_1X30;
			break;
		default:
			bus_format = MEDIA_BUS_FMT_RGB888_1X24;
			break;
	}

	list_for_each_entry(phytium_mode, &phytium_dp->phytium_mode.list, list) {
				switch (phytium_mode->bpc) {
					case 6:
						bus_format = MEDIA_BUS_FMT_RGB666_1X18;
						break;
					case 8:
						bus_format = MEDIA_BUS_FMT_RGB888_1X24;
						break;
					case 10:
						bus_format = MEDIA_BUS_FMT_RGB101010_1X30;
						break;
					default:
						bus_format = MEDIA_BUS_FMT_RGB888_1X24;
						break;
				}
	}

	ftd330_crtc_state->output_fmt = bus_format;
	if (phytium_dp->is_edp) {
		ftd330_crtc_state->encoder_type = DRM_MODE_ENCODER_TMDS;
	} else {
		ftd330_crtc_state->encoder_type = DRM_MODE_ENCODER_DPMST;
	}

    return 0;
}



static void phytium_dp_encoder_destroy(struct drm_encoder *encoder)
{
	drm_encoder_cleanup(encoder);
}

static const struct drm_encoder_funcs phytium_encoder_funcs = {
	.destroy = phytium_dp_encoder_destroy,
};


//static int phytium_get_encoder_crtc_mask(struct phytium_dp_device *phytium_dp, int port)
//{
//	struct drm_device *dev =  phytium_dp->dev;
//	struct ftd330_drm_private *priv = dev->dev_private;
//	int i, mask = 0;
//
//	for_each_pipe(priv, i) {
//		if (i != port)
//			mask++;
//		else
//			break;
//	}
//
//	return BIT(mask);
//}


int phytium_dp_resume(struct drm_device *drm_dev)
{
#if defined(CONFIG_PHYTIUM_EDP_BL) || defined(CONFIG_PHYTIUM_LANE_TRAIN)
	struct phytium_dp_device *phytium_dp;
	struct phytium_panel *panel = NULL;
	struct drm_encoder *encoder;
	int ret = 0;
#endif
#if 0
	bool raw_state;
#endif
	FTD330_LOG_TRACE;

#if defined(CONFIG_PHYTIUM_EDP_BL) || defined(CONFIG_PHYTIUM_LANE_TRAIN)
	drm_for_each_encoder(encoder, drm_dev) {
		phytium_dp = encoder_to_dp_device(encoder);
		ret = phytium_dp_hw_init(phytium_dp);
		if (ret) {
			DRM_ERROR("failed to initialize dp %d\n", phytium_dp->port);
		}

		if (phytium_dp->is_edp) {
			panel = &phytium_dp->panel;
			panel->level = panel->save_level;
			phytium_edp_panel_poweron(phytium_dp);
			if (phytium_dp->panel.setup_backlight) {
				mutex_lock(&phytium_dp->panel.panel_lock);
				phytium_dp->panel.setup_backlight(&phytium_dp->panel);
				mutex_unlock(&phytium_dp->panel.panel_lock);
			} else {
				DRM_ERROR("edp-%d missing setup_backlight func\n", phytium_dp->port);
			}
		} else {
			phytium_set_train_link_rate_lane_count(phytium_dp, NULL);
		}

		if (phytium_dp->audio_info.sample_rate != 0)
			phytium_dp_hw_audio_hw_params(phytium_dp, phytium_dp->audio_info);
	}
#endif
	return 0;
}

int phytium_dp_suspend(struct drm_device *drm_dev)
{
	struct phytium_dp_device *phytium_dp;
	struct ftd330_drm_private *priv = drm_dev->dev_private;
	struct drm_encoder *encoder;
	struct phytium_panel *panel = NULL;
	int real_display_id = 0;
	drm_for_each_encoder(encoder, drm_dev) {
		phytium_dp = encoder_to_dp_device(encoder);
		real_display_id = phytium_display_virtual_to_physical(priv->info.pipe_mask, phytium_dp->port);
		if (phytium_dp->connector.status == connector_status_connected) {
			priv->power_status_save[real_display_id] =  true;
		} else {
			priv->power_status_save[real_display_id] =  false;
		}
		if (phytium_dp->is_edp) {
			panel = &phytium_dp->panel;
			panel->save_level = panel->level;
		}
		phytium_dp_hw_disable_video(phytium_dp);
	}

	return 0;
}


#if defined(CONFIG_PHYTIUM_LOW_FPS) || defined(CONFIG_PHYTIUM_PSR)
static void phytium_psr_lowfps_dplp_deinit(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;
	uint32_t dplp_offset = priv->dplp_reg_base[port];
	uint32_t dp_offset = priv->dp_reg_base[port];
	uint32_t val;

	/* mask all irq */
	phytium_dplp_write_reg(priv, ALL_SHIELD_MAKS, dplp_offset, PHYTIUM_DPLP_FRAME_COMP_INIT_MASK);

	/* disable frame comp*/
	phytium_dplp_write_reg(priv, 0x0, dplp_offset, PHYTIUM_DPLP_FRAME_COMP_CFG);

	/* disbale dp crc */
	val = phytium_readl_reg(priv, dp_offset, PHYTIUM_EDP_CRC_ENABLE);
	val &= ~ENABLE_CRC;
	phytium_writel_reg(priv, val, dp_offset, PHYTIUM_EDP_CRC_ENABLE);

	/* clean irq*/
	phytium_dplp_write_reg(priv, 0x3ff, dplp_offset, PHYTIUM_DPLP_FRAME_COMP_INIT_CLEAR);
}

#endif

#ifdef CONFIG_PHYTIUM_LOW_FPS
static void phytium_low_fps_dplp_init(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;
	uint32_t dplp_offset = priv->dplp_reg_base[port];
	uint32_t dp_offset = priv->dp_reg_base[port];
	uint32_t val;

	FTD330_LOG_TRACE;
	/* Init dp crc */
	val = phytium_readl_reg(priv, dp_offset, PHYTIUM_EDP_CRC_ENABLE);
	val |= ENABLE_CRC;
	phytium_writel_reg(priv, val, dp_offset, PHYTIUM_EDP_CRC_ENABLE);

	/* config dplp */
	phytium_dplp_write_reg(priv, LOWFPS_CNT_NUMS, dplp_offset, PHYTIUM_DPLP_LOWFPS_CNT);

	phytium_dplp_write_reg(priv, LOWFPS_AVAILABLE | LOWFPS_EXIT, dplp_offset,
						PHYTIUM_DPLP_FRAME_COMP_INIT_CLEAR);
	val = phytium_dplp_read_reg(priv, dplp_offset, PHYTIUM_DPLP_FRAME_COMP_INIT_MASK);
	val = val ? (val&LOW_FPS_ENABLE_MASK) : LOW_FPS_ENABLE_MASK;
	phytium_dplp_write_reg(priv, val, dplp_offset, PHYTIUM_DPLP_FRAME_COMP_INIT_MASK);

	val = phytium_dplp_read_reg(priv, dplp_offset, PHYTIUM_DPLP_FRAME_COMP_CFG);
	phytium_dplp_write_reg(priv, val | 0x03, dplp_offset, PHYTIUM_DPLP_FRAME_COMP_CFG);
}
#endif

#ifdef CONFIG_PHYTIUM_PSR
static void phytium_psr_dplp_init(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  phytium_dp->dev;
	struct ftd330_drm_private *priv = dev->dev_private;
	int port = phytium_dp->port;
	uint32_t dplp_offset = priv->dplp_reg_base[port];
	uint32_t dp_offset = priv->dp_reg_base[port];
	uint32_t val;

	/* Init dp crc */
	val = phytium_readl_reg(priv, dp_offset, PHYTIUM_EDP_CRC_ENABLE);
	val |= ENABLE_CRC;
	phytium_writel_reg(priv, val, dp_offset, PHYTIUM_EDP_CRC_ENABLE);

	/* Config dplp psr */
	val = phytium_dplp_read_reg(priv, dplp_offset, PHYTIUM_DPLP_LOWFPS_CNT);
	val = val ? val : LOWFPS_CNT_NUMS;
	phytium_dplp_write_reg(priv, val, dplp_offset, PHYTIUM_DPLP_LOWFPS_CNT);
	phytium_dplp_write_reg(priv, PSR_CNT_NUMS, dplp_offset, PHYTIUM_DPLP_PSR_CNT);

	/* Rely only on PSR_AVAILABLE interrupts*/
	phytium_dplp_write_reg(priv, PSR_AVAILABLE, dplp_offset,
						PHYTIUM_DPLP_FRAME_COMP_INIT_CLEAR);
	val = phytium_dplp_read_reg(priv, dplp_offset, PHYTIUM_DPLP_FRAME_COMP_INIT_MASK);
	val = val ? (val&PSR_AVAILABLE_MASK) : PSR_AVAILABLE_MASK;
	phytium_dplp_write_reg(priv, val, dplp_offset, PHYTIUM_DPLP_FRAME_COMP_INIT_MASK);

	val = phytium_dplp_read_reg(priv, dplp_offset, PHYTIUM_DPLP_FRAME_COMP_CFG);
	phytium_dplp_write_reg(priv, val|0x05, dplp_offset, PHYTIUM_DPLP_FRAME_COMP_CFG);
}
#endif

void phytium_dplp_init_port(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  NULL;
	struct ftd330_drm_private *priv = NULL;
	uint32_t group_offset = 0;

	FTD330_LOG_TRACE;

	if (!phytium_dp) {
		pr_err("phytium_dp not init,skip dplp_deinit\n");
	}
	dev =  phytium_dp->dev;
	priv = dev->dev_private;
	group_offset = priv->dplp_reg_base[phytium_dp->port];
		phytium_dplp_write_reg(priv, 0x06, group_offset, PHYTIUM_DPLP_HPD_PWEUP_INIT_MASK);
		phytium_dplp_write_reg(priv, 0x01, group_offset, PHYTIUM_DPLP_HPD_STATE_RESET);
		phytium_dplp_write_reg(priv, 0x00, group_offset, PHYTIUM_DPLP_HPD_STATE_RESET);
}
void phytium_dplp_init(struct ftd330_drm_private *priv)
{
	int i = 0;
	uint32_t physical_port = 0;
	FTD330_LOG_TRACE;
	for (i = 0; i < priv->info.total_pipes; i++) {
		physical_port = phytium_display_virtual_to_physical(priv->info.pipe_mask, i);
		phytium_dplp_init_port(priv->phytium_dp[physical_port]);
	}
}

void phytium_dplp_deinit_port(struct phytium_dp_device *phytium_dp)
{
	struct drm_device *dev =  NULL;
	struct ftd330_drm_private *priv = NULL;
	uint32_t group_offset = 0;
	FTD330_LOG_TRACE;
	if (!phytium_dp) {
		pr_err("phytium_dp not init,skip dplp_deinit\n");
	}
	dev =  phytium_dp->dev;
	priv = dev->dev_private;
	group_offset = priv->dplp_reg_base[phytium_dp->port];
	phytium_dplp_write_reg(priv, 0x07, group_offset, PHYTIUM_DPLP_HPD_PWEUP_INIT_MASK);
}
void phytium_dplp_deinit(struct ftd330_drm_private *priv)
{
	int i = 0;
	uint32_t physical_port = 0;
	FTD330_LOG_TRACE;
	for (i = 0; i < priv->info.total_pipes; i++) {
		physical_port = phytium_display_virtual_to_physical(priv->info.pipe_mask, i);
		phytium_dplp_deinit_port(priv->phytium_dp[physical_port]);
	}
}

static void phytium_encoder_enable(struct drm_encoder *encoder)
{
        struct phytium_dp_device *phytium_dp = encoder_to_dp_device(encoder);
        struct drm_device *dev = phytium_dp->dev;
        struct ftd330_drm_private *priv = dev->dev_private;
	uint32_t group_offset = priv->dp_reg_base[phytium_dp->port];

        /*hxb add,pls consider if this can be officailly use*/
        if( !phytium_dp_hw_video_is_enable(phytium_dp)) {
                phytium_dp_hw_disable_output(phytium_dp);
                phytium_dp_hw_disable_input_source(phytium_dp);
                phytium_dp_hw_disable_video(phytium_dp);
                phytium_dp_hw_enable_input_source(phytium_dp);
                phytium_dp_hw_enable_output(phytium_dp);

                phytium_writel_reg(priv, SCRAMBLING_ENABLE, group_offset,
                                   PHYTIUM_DP_SCRAMBLING_DISABLE);


                phytium_dp->max_link_rate = phytium_dp->link_rate = drm_dp_bw_code_to_link_rate(DP_LINK_BW_5_4);
                phytium_dp->max_link_lane_count = phytium_dp->link_lane_count = 4;

                phytium_writel_reg(priv, phytium_dp->link_lane_count,
                                   group_offset, PHYTIUM_DP_LANE_COUNT_SET);
                phytium_writel_reg(priv,
                                   drm_dp_link_rate_to_bw_code(phytium_dp->link_rate),
                                   group_offset, PHYTIUM_DP_LINK_BW_SET);
        }

//      phytium_dp_modify_dc_hsync_time(phytium_dp);
        phytium_dp_hw_config_video(phytium_dp);
        phytium_dp_hw_enable_video(phytium_dp);
}


static const struct drm_encoder_helper_funcs phytium_encoder_helper_funcs = {
        .mode_set = phytium_dp_encoder_mode_set,
        .disable = phytium_encoder_disable,
        .enable  = phytium_encoder_enable,
        .mode_valid = phytium_encoder_mode_valid,
        .atomic_check = phytium_encoder_atomic_check,
};


int phytium_dp_init(struct drm_device *dev, int port)
{
	struct phytium_dp_device *phytium_dp = NULL;
	struct ftd330_drm_private *priv = dev->dev_private;
	int ret = 0;
	int type;

	pr_info("FTD330 %s: port %d\n", __func__, port);
	phytium_dp = kzalloc(sizeof(*phytium_dp), GFP_KERNEL);
	if (!phytium_dp) {
		ret = -ENOMEM;
		goto failed_malloc_dp;
	}

	phytium_dp->dev = dev;
	phytium_dp->port = port;

	mutex_init(&phytium_dp->low_power_mutex);
	INIT_LIST_HEAD(&phytium_dp->phytium_mode.list);

	if (phytium_dp->is_edp) {
		ret = drm_encoder_init(dev, &phytium_dp->encoder, &phytium_encoder_funcs,
                                       DRM_MODE_ENCODER_TMDS, "DP %d", port);
	} else {
		ret = drm_encoder_init(dev, &phytium_dp->encoder, &phytium_encoder_funcs,
				       DRM_MODE_ENCODER_DPMST, "DP %d", port);
	}
	if (ret) {
		DRM_ERROR("failed to initialize encoder with drm\n");
		goto failed_encoder_init;
	}

	drm_encoder_helper_add(&phytium_dp->encoder, &phytium_encoder_helper_funcs);
	phytium_dp->encoder.possible_crtcs = BIT(port);

	phytium_dp->connector.dpms   = DRM_MODE_DPMS_OFF;
	ret = drm_connector_init(dev, &phytium_dp->connector, &phytium_connector_funcs, type);

	if (ret) {
		DRM_ERROR("failed to initialize connector with drm\n");
		goto failed_connector_init;
	}
	drm_connector_helper_add(&phytium_dp->connector, &phytium_connector_helper_funcs);
	drm_connector_attach_encoder(&phytium_dp->connector, &phytium_dp->encoder);

	phytium_dp->max_link_rate = phytium_dp->link_rate =
					drm_dp_bw_code_to_link_rate(DP_LINK_BW_8_1);
	if (phytium_dp->port == 0)
		phytium_dp->max_link_lane_count =
				phytium_dp->link_lane_count = source0_max_lane_count;
	else if (phytium_dp->port == 1)
		phytium_dp->max_link_lane_count =
				phytium_dp->link_lane_count = source1_max_lane_count;
	else if (phytium_dp->port == 2)
		phytium_dp->max_link_lane_count =
				phytium_dp->link_lane_count = source2_max_lane_count;
	else
		phytium_dp->max_link_lane_count = phytium_dp->link_lane_count = 4;

	phytium_dp->dp_hpd_state.hpd_event_state = false;
	phytium_dp->dp_hpd_state.hpd_irq_state = false;
	phytium_dp->dp_hpd_state.hpd_raw_state = false;

	INIT_WORK(&phytium_dp->power_work, phytium_dp_power_work_func);

	drm_connector_register(&phytium_dp->connector);

	priv->phytium_dp[phytium_display_virtual_to_physical(priv->info.pipe_mask, phytium_dp->port)] = phytium_dp;
	return 0;

#if defined(CONFIG_PHYTIUM_LANE_TRAIN) || defined(CONFIG_PHYTIUM_EDP_BL)
failed_init_dp:
#endif
failed_connector_init:
failed_encoder_init:
	if (phytium_dp)
		kfree(phytium_dp);
failed_malloc_dp:
	return ret;
}

void phytium_dp_disable_before_init(struct ftd330_drm_private *priv)
{
	int i = 0;
	u32 group_offset = 0;

	for (i = DISPLAY_0; i < DISPLAY_NUM; i++) {
		group_offset = PHYTIUM_FTD330_DP_REG_OFFSET +
                                          i*PHYTIUM_FTD330_DP_REG_INTERVAL;

		if (!(priv->info.edp_mask & BIT(i))) {
			phytium_writel_reg(priv, SST_MST_SOURCE_0_DISABLE,
	                          group_offset, PHYTIUM_DP_VIDEO_STREAM_ENABLE);
		}
	}
}


void phytium_dp_platform_init(struct drm_device *dev)
{
	int i = 0, j = 0;
	struct ftd330_drm_private *priv = dev->dev_private;
	int ret = 0;
	int display_index = 0;

	mutex_init(&priv->power_mutex);
	spin_lock_init(&priv->hotplug_irq_lock);
	
	for (i = DISPLAY_0; i < DISPLAY_NUM; i++) {
		if (priv->info.pipe_mask & BIT(i)) {
			priv->dp_reg_base[display_index] = PHYTIUM_FTD330_DP_REG_OFFSET +
							i*PHYTIUM_FTD330_DP_REG_INTERVAL;
			priv->dplp_reg_base[display_index] = PHYTIUM_FTD330_DPLP_REG_OFFSET +
							i*PHYTIUM_FTD330_DPLP_REG_INTERVAL;
			priv->phy_access_base[display_index] = PHYTIUM_FTD330_DP_PHY_REG_OFFSET +
							i*PHYTIUM_FTD330_DP_PHY_REG_INTERVAL;
			display_index++;
		}
	}

	if (priv->info.edp_mask)
		priv->edp_pwm_base = PHYTIUM_FTD330_EDP_PWM_REG_OFFSET + 0x400;

	for_each_pipe(priv, j) {
		ret = phytium_dp_init(dev, j);
		if (ret) {
			pr_info("phytium_dp_init(pipe %d) return failed\n", i);
			return;
		}
	}
}

