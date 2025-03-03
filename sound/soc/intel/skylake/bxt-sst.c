/*
 *  bxt-sst.c - DSP library functions for BXT platform
 *
 *  Copyright (C) 2015-16 Intel Corp
 *  Author:Rafal Redzimski <rafal.f.redzimski@intel.com>
 *	   Jeeja KP <jeeja.kp@intel.com>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/firmware.h>
#include <linux/device.h>

#include "../common/sst-dsp.h"
#include "skl-fwlog.h"
#include "skl-sst-ipc.h"

#define BXT_BASEFW_TIMEOUT	3000
#define BXT_INIT_TIMEOUT	300
#define BXT_ROM_INIT_TIMEOUT	70
#define BXT_IPC_PURGE_FW	0x01004000

#define BXT_ROM_INIT		0x5
#define BXT_ADSP_SRAM0_BASE	0x80000

/* BXT SSP/I2S Registers */
#define I2S_SSC1_REG_OFF	BIT(2)
#define SET_SLAVE_MASK		GENMASK(25, 24)

/*BXT I2S Clock Gating*/
#define BXT_DSP_CLK_CTL			0x378
#define BXT_DISABLE_4_SSP_CLK_GT	GENMASK(21, 18)
#define BXT_DISABLE_ALL_SSP_CLK_GT	GENMASK(23, 18)

/* Trace Buffer Window */
#define BXT_ADSP_SRAM2_BASE	0x0C0000
#define BXT_ADSP_W2_SIZE	0x2000
#define BXT_ADSP_WP_DSP0	(BXT_ADSP_SRAM0_BASE+0x30)
#define BXT_ADSP_WP_DSP1	(BXT_ADSP_SRAM0_BASE+0x34)
#define BXT_ADSP_NR_DSP		2

/* Firmware status window */
#define BXT_ADSP_FW_STATUS	BXT_ADSP_SRAM0_BASE
#define BXT_ADSP_ERROR_CODE     (BXT_ADSP_FW_STATUS + 0x4)

#define BXT_ADSP_SRAM1_BASE	0xA0000

#define BXT_ADSP_FW_BIN_HDR_OFFSET 0x2000

/* Delay before scheduling D0i3 entry */
#define BXT_D0I3_DELAY 5000

#define BXT_FW_INIT_RETRY 10

#define GET_SSP_BASE(N)	(N > 4 ? 0x2000 : 0x4000)

#define BXTP_NUM_I2S_PORTS	6

static void bxt_set_ssp_slave(struct sst_dsp *ctx)
{
	u32 mask, i2s_base_addr;
	int i;

	if (BXTP_NUM_I2S_PORTS == 4)
		mask = BXT_DISABLE_4_SSP_CLK_GT;
	else
		mask = BXT_DISABLE_ALL_SSP_CLK_GT;

	/* disable clock gating on all SSPs */
	sst_dsp_shim_update_bits_unlocked(ctx,
			BXT_DSP_CLK_CTL, mask, mask);

	/* set all SSPs to slave */
	i2s_base_addr = GET_SSP_BASE(BXTP_NUM_I2S_PORTS);
	for (i = 0; i < BXTP_NUM_I2S_PORTS; i++) {
		sst_dsp_shim_update_bits_unlocked(ctx,
			(i2s_base_addr + (i * 0x1000) + I2S_SSC1_REG_OFF),
					SET_SLAVE_MASK, SET_SLAVE_MASK);
	}

	/* re-enable clock gating */
	sst_dsp_shim_update_bits_unlocked(ctx, BXT_DSP_CLK_CTL, mask, 0);
}

static unsigned int bxt_get_errorcode(struct sst_dsp *ctx)
{
	 return sst_dsp_shim_read(ctx, BXT_ADSP_ERROR_CODE);
}

int
bxt_load_library(struct sst_dsp *ctx, struct skl_lib_info *linfo, int lib_count)
{
	struct snd_dma_buffer dmab;
	struct skl_sst *skl = ctx->thread_context;
	struct firmware stripped_fw;
	int ret = 0, i, dma_id, stream_tag;

	/* library indices start from 1 to N. 0 represents base FW */
	for (i = 1; i < lib_count; i++) {
		ret = skl_prepare_lib_load(skl, &skl->lib_info[i], &stripped_fw,
					BXT_ADSP_FW_BIN_HDR_OFFSET, i);
		if (ret < 0)
			goto load_library_failed;

		stream_tag = ctx->dsp_ops.prepare(ctx->dev, 0x40,
					stripped_fw.size, &dmab,
					SNDRV_PCM_STREAM_PLAYBACK);
		if (stream_tag <= 0) {
			dev_err(ctx->dev, "Lib prepare DMA err: %x\n",
					stream_tag);
			ret = stream_tag;
			goto load_library_failed;
		}

		dma_id = stream_tag - 1;
		memcpy(dmab.area, stripped_fw.data, stripped_fw.size);

		ctx->dsp_ops.trigger(ctx->dev, true, stream_tag,
						SNDRV_PCM_STREAM_PLAYBACK);
		ret = skl_sst_ipc_load_library(&skl->ipc, dma_id, i, true);
		if (ret < 0)
			dev_err(ctx->dev, "IPC Load Lib for %s fail: %d\n",
					linfo[i].name, ret);

		ctx->dsp_ops.trigger(ctx->dev, false, stream_tag,
						SNDRV_PCM_STREAM_PLAYBACK);
		ctx->dsp_ops.cleanup(ctx->dev, &dmab, stream_tag,
						SNDRV_PCM_STREAM_PLAYBACK);
	}

	return ret;

load_library_failed:
	skl_release_library(linfo, lib_count);
	return ret;
}

int sst_fw_status_poll(struct sst_dsp *ctx, u32 module, u32 state,
			 u32 time, char *operation)
{
	union {
		struct {
			u32 state:24;
			u32 waits:4;
			u32 module:3;
			u32 halted:1;
		};
		u32 raw;
	} fwsts;
	unsigned long timeout;
	int k = 0, s;
	int ret;
	char *state_desc;

	/*
	 * split the loop into sleeps of varying resolution.
	 * By observation, FW responds in 10 to 20ms.
	 */

	timeout = jiffies + msecs_to_jiffies(time);

	fwsts.raw = sst_dsp_shim_read_unlocked(ctx, BXT_ADSP_FW_STATUS);
	while (!fwsts.halted
			&& (fwsts.module == module)
			&& (fwsts.state != state)
			&& time_before(jiffies, timeout)) {

		k++;
		switch (k) {
		case 1:
		case 12:
				s = 10000;
				break;
		case 2:
				s = 1000;
				break;
		}

		usleep_range(s, s+100);

		fwsts.raw = sst_dsp_shim_read_unlocked(ctx, BXT_ADSP_FW_STATUS);
	}

	if (!fwsts.halted && (fwsts.module == module)
			&& (fwsts.state == state)) {
		state_desc = "success";
		ret = 0;
	} else if (fwsts.halted || fwsts.module != module) {
		state_desc = "unexpected state";
		ret = -EPROTO;
	} else {
		state_desc = "timeout";
		ret = -ETIME;
	}
	dev_dbg(ctx->dev, "FW Status=%08x %s %s\n", fwsts.raw,
			operation, state_desc);

	return ret;
}

/*
 * First boot sequence has some extra steps. Core 0 waits for power
 * status on core 1, so power up core 1 also momentarily, keep it in
 * reset/stall and then turn it off
 */
static int sst_bxt_prepare_fw(struct sst_dsp *ctx,
			const void *fwdata, u32 fwsize, u32 *fwerr, u32 *fwsts)
{
	int stream_tag, ret;

	stream_tag = ctx->dsp_ops.prepare(ctx->dev, 0x40, fwsize, &ctx->dmab,
						SNDRV_PCM_STREAM_PLAYBACK);
	if (stream_tag <= 0) {
		dev_err(ctx->dev, "Failed to prepare DMA FW loading err: %x\n",
				stream_tag);
		return stream_tag;
	}

	ctx->dsp_ops.stream_tag = stream_tag;
	memcpy(ctx->dmab.area, fwdata, fwsize);

	/* Step 1: Power up core 0 and core1 */
	ret = skl_dsp_core_power_up(ctx, SKL_DSP_CORE0_MASK |
				SKL_DSP_CORE_MASK(1));
	if (ret < 0) {
		dev_err(ctx->dev, "dsp core0/1 power up failed\n");
		goto base_fw_load_failed;
	}

	/* DSP is powered up, set all SSPs to slave mode */
	bxt_set_ssp_slave(ctx);

	/* Step 2: Purge FW request */
	sst_dsp_shim_write(ctx, SKL_ADSP_REG_HIPCI, SKL_ADSP_REG_HIPCI_BUSY |
				(BXT_IPC_PURGE_FW | ((stream_tag - 1) << 9)));

	/* Step 3: Unset core0 reset state & unstall/run core0 */
	ret = skl_dsp_start_core(ctx, SKL_DSP_CORE0_MASK);
	if (ret < 0) {
		dev_err(ctx->dev, "Start dsp core failed ret: %d\n", ret);
		ret = -EIO;
		goto base_fw_load_failed;
	}

	/* Step 4: Wait for DONE Bit */
	ret = sst_dsp_register_poll(ctx, SKL_ADSP_REG_HIPCIE,
					SKL_ADSP_REG_HIPCIE_DONE,
					SKL_ADSP_REG_HIPCIE_DONE,
					BXT_INIT_TIMEOUT, "HIPCIE Done");
	if (ret < 0) {
		dev_err(ctx->dev, "Timeout for Purge Request%d\n", ret);
		goto base_fw_load_failed;
	}

	/* Step 5: power down core1 */
	ret = skl_dsp_core_power_down(ctx, SKL_DSP_CORE_MASK(1));
	if (ret < 0) {
		dev_err(ctx->dev, "dsp core1 power down failed\n");
		goto base_fw_load_failed;
	}

	/* Step 6: Enable Interrupt */
	skl_ipc_int_enable(ctx);
	skl_ipc_op_int_enable(ctx);

	/* Step 7: Wait for ROM init */
	ret = sst_fw_status_poll(ctx, 0,
			SKL_FW_INIT, BXT_ROM_INIT_TIMEOUT, "ROM Load");
	if (ret < 0) {
		dev_err(ctx->dev, "Timeout for ROM init, ret:%d\n", ret);
		goto base_fw_load_failed;
	}

	return ret;

base_fw_load_failed:
	if (fwerr)
		*fwerr = sst_dsp_shim_read(ctx, BXT_ADSP_ERROR_CODE);
	if (fwsts)
		*fwsts = sst_dsp_shim_read(ctx, BXT_ADSP_FW_STATUS);

	ctx->dsp_ops.cleanup(ctx->dev, &ctx->dmab, stream_tag,
						SNDRV_PCM_STREAM_PLAYBACK);

	skl_dsp_core_power_down(ctx, SKL_DSP_CORE_MASK(1));
	skl_dsp_disable_core(ctx, SKL_DSP_CORE0_MASK);
	return ret;
}

static int sst_transfer_fw_host_dma(struct sst_dsp *ctx)
{
	int ret;

	ctx->dsp_ops.trigger(ctx->dev, true, ctx->dsp_ops.stream_tag,
						SNDRV_PCM_STREAM_PLAYBACK);
	ret = sst_fw_status_poll(ctx, 0,
			BXT_ROM_INIT, BXT_BASEFW_TIMEOUT, "Firmware boot");

	ctx->dsp_ops.trigger(ctx->dev, false, ctx->dsp_ops.stream_tag,
						SNDRV_PCM_STREAM_PLAYBACK);
	ctx->dsp_ops.cleanup(ctx->dev, &ctx->dmab, ctx->dsp_ops.stream_tag,
						SNDRV_PCM_STREAM_PLAYBACK);

	return ret;
}

static int bxt_load_base_firmware(struct sst_dsp *ctx)
{
	struct firmware stripped_fw;
	struct skl_sst *skl = ctx->thread_context;
	int ret, i;
	u32 fwerr = 0, fwsts = 0;

	if (ctx->fw == NULL) {
		ret = request_firmware(&ctx->fw, ctx->fw_name, ctx->dev);
		if (ret < 0) {
			dev_err(ctx->dev, "Request firmware failed %d\n", ret);
			return ret;
		}
	}

	/* prase uuids on first boot */
	if (skl->is_first_boot) {
		ret = snd_skl_parse_uuids(ctx, ctx->fw, BXT_ADSP_FW_BIN_HDR_OFFSET, 0);
		if (ret < 0)
			goto sst_load_base_firmware_failed;
	}

	stripped_fw.data = ctx->fw->data;
	stripped_fw.size = ctx->fw->size;
	skl_dsp_strip_extended_manifest(&stripped_fw);

	for (i = 0; i < BXT_FW_INIT_RETRY; i++) {
		ret = sst_bxt_prepare_fw(ctx, stripped_fw.data,
					stripped_fw.size, &fwerr, &fwsts);
		if (ret < 0) {
			dev_err(ctx->dev, "Error code=0x%x: FW status=0x%x\n", fwerr, fwsts);
			dev_err(ctx->dev, "Iteration %d Core En/ROM load fail:%d\n", i, ret);
			continue;
		}
		dev_dbg(ctx->dev, "Iteration %d ROM load Success:%d\n", i, ret);

		ret = sst_transfer_fw_host_dma(ctx);
		if (ret < 0) {
			dev_err(ctx->dev, "Iteration %d Transfer firmware failed %d\n", i, ret);
			dev_info(ctx->dev, "Error code=0x%x: FW status=0x%x\n",
				sst_dsp_shim_read(ctx, BXT_ADSP_ERROR_CODE),
				sst_dsp_shim_read(ctx, BXT_ADSP_FW_STATUS));

			skl_dsp_core_power_down(ctx, SKL_DSP_CORE_MASK(1));
			skl_dsp_disable_core(ctx, SKL_DSP_CORE0_MASK);
			continue;
		}
		dev_dbg(ctx->dev, "Iteration %d FW transfer Success:%d\n", i, ret);

		if (ret == 0)
			break;
	}

        if (ret < 0) {
		dev_err(ctx->dev, "Firmware download failed\n");
		goto sst_load_base_firmware_failed;
	} else {
		dev_dbg(ctx->dev, "Firmware download successful\n");
		ret = wait_event_timeout(skl->boot_wait, skl->boot_complete,
					msecs_to_jiffies(SKL_IPC_BOOT_MSECS));
		if (ret == 0) {
			dev_err(ctx->dev, "DSP boot fail, FW Ready timeout\n");
			skl_dsp_disable_core(ctx, SKL_DSP_CORE0_MASK);
			ret = -EIO;
		} else {
			ret = 0;
			skl->fw_loaded = true;
		}
	}

	return ret;

sst_load_base_firmware_failed:
	release_firmware(ctx->fw);
	ctx->fw = NULL;
	return ret;
}

/*
 * Decide the D0i3 state that can be targeted based on the usecase
 * ref counts and DSP state
 *
 * Decision Matrix:  (X= dont care; state = target state)
 *
 * DSP state != SKL_DSP_RUNNING ; state = no d0i3
 *
 * DSP state == SKL_DSP_RUNNING , the following matrix applies
 * non_d0i3 >0; streaming =X; non_streaming =X; state = no d0i3
 * non_d0i3 =X; streaming =0; non_streaming =0; state = no d0i3
 * non_d0i3 =0; streaming >0; non_streaming =X; state = streaming d0i3
 * non_d0i3 =0; streaming =0; non_streaming =X; state = non-streaming d0i3
 */
static int bxt_d0i3_target_state(struct sst_dsp *ctx)
{
	struct skl_sst *skl = ctx->thread_context;
	struct skl_d0i3_data *d0i3 = &skl->d0i3;

	if (skl->cores.state[SKL_DSP_CORE0_ID] != SKL_DSP_RUNNING)
		return SKL_DSP_D0I3_NONE;

	if (d0i3->non_d0i3)
		return SKL_DSP_D0I3_NONE;
	else if (d0i3->streaming)
		return SKL_DSP_D0I3_STREAMING;
	else if (d0i3->non_streaming)
		return SKL_DSP_D0I3_NON_STREAMING;
	else
		return SKL_DSP_D0I3_NONE;
}

void bxt_set_dsp_D0i3(struct work_struct *work)
{
	int ret;
	struct skl_ipc_d0ix_msg msg;
	struct skl_sst *skl = container_of(work,
			struct skl_sst, d0i3.work.work);
	struct sst_dsp *ctx = skl->dsp;
	struct skl_d0i3_data *d0i3 = &skl->d0i3;
	int target_state;

	dev_dbg(ctx->dev, "In %s:\n", __func__);

	/* D0i3 entry allowed only if core 0 alone is running */
	if (skl_dsp_get_enabled_cores(ctx) !=  SKL_DSP_CORE0_MASK) {
		dev_warn(ctx->dev,
				"D0i3 allowed when only core0 running:Exit\n");
		return;
	}

	target_state = bxt_d0i3_target_state(ctx);
	if (target_state == SKL_DSP_D0I3_NONE)
		return;

	msg.instance_id = 0;
	msg.module_id = 0;
	msg.wake = 1;
	msg.streaming = 0;
	if (target_state == SKL_DSP_D0I3_STREAMING)
		msg.streaming = 1;

	ret =  skl_ipc_set_d0ix(&skl->ipc, &msg);

	if (ret < 0) {
		dev_err(ctx->dev, "Failed to set DSP to D0i3 state\n");
		return;
	}

	/* Set Vendor specific register D0I3C.I3 to enable D0i3*/
	if (skl->update_d0i3c)
		skl->update_d0i3c(skl->dev, true);

	d0i3->state = target_state;
	skl->cores.state[SKL_DSP_CORE0_ID] = SKL_DSP_RUNNING_D0I3;
}

int bxt_schedule_dsp_D0i3(struct sst_dsp *ctx)
{
	struct skl_sst *skl = ctx->thread_context;
	struct skl_d0i3_data *d0i3 = &skl->d0i3;

	/* Schedule D0i3 only if the usecase ref counts are appropriate */
	if (bxt_d0i3_target_state(ctx) != SKL_DSP_D0I3_NONE) {

		dev_dbg(ctx->dev, "%s: Schedule D0i3\n", __func__);

		schedule_delayed_work(&d0i3->work,
				msecs_to_jiffies(BXT_D0I3_DELAY));
	}

	return 0;
}

int bxt_set_dsp_D0i0(struct sst_dsp *ctx)
{
	int ret;
	struct skl_ipc_d0ix_msg msg;
	struct skl_sst *skl = ctx->thread_context;

	dev_dbg(ctx->dev, "In %s:\n", __func__);

	/* First Cancel any pending attempt to put DSP to D0i3 */
	cancel_delayed_work_sync(&skl->d0i3.work);

	/* If DSP is currently in D0i3, bring it to D0i0 */
	if (skl->cores.state[SKL_DSP_CORE0_ID] != SKL_DSP_RUNNING_D0I3)
		return 0;

	dev_dbg(ctx->dev, "Set DSP to D0i0\n");

	msg.instance_id = 0;
	msg.module_id = 0;
	msg.streaming = 0;
	msg.wake = 0;

	if (skl->d0i3.state == SKL_DSP_D0I3_STREAMING)
		msg.streaming = 1;

	/* Clear Vendor specific register D0I3C.I3 to disable D0i3*/
	if (skl->update_d0i3c)
		skl->update_d0i3c(skl->dev, false);

	ret =  skl_ipc_set_d0ix(&skl->ipc, &msg);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to set DSP to D0i0\n");
		return ret;
	}

	skl->cores.state[SKL_DSP_CORE0_ID] = SKL_DSP_RUNNING;
	skl->d0i3.state = SKL_DSP_D0I3_NONE;

	return 0;
}

static int bxt_set_dsp_D0(struct sst_dsp *ctx, unsigned int core_id)
{
	struct skl_sst *skl = ctx->thread_context;
	int ret;
	struct skl_ipc_dxstate_info dx;
	unsigned int core_mask = SKL_DSP_CORE_MASK(core_id);

	if (skl->fw_loaded == false) {
		skl->boot_complete = false;
		ret = bxt_load_base_firmware(ctx);
		if (ret < 0) {
			dev_err(ctx->dev, "reload fw failed: %d\n", ret);
			return ret;
		}

		if (skl->lib_count > 1) {
			ret = bxt_load_library(ctx, skl->lib_info,
						skl->lib_count);
			if (ret < 0) {
				dev_err(ctx->dev, "reload libs failed: %d\n", ret);
				return ret;
			}
		}
		skl->cores.state[core_id] = SKL_DSP_RUNNING;
		return ret;
	}

	/* If core 0 is being turned on, turn on core 1 as well */
	if (core_id == SKL_DSP_CORE0_ID)
		ret = skl_dsp_core_power_up(ctx, core_mask |
				SKL_DSP_CORE_MASK(1));
	else
		ret = skl_dsp_core_power_up(ctx, core_mask);

	if (ret < 0)
		goto err;

	if (core_id == SKL_DSP_CORE0_ID) {

		 /* set all SSPs to slave mode */
		bxt_set_ssp_slave(ctx);

		/*
		 * Enable interrupt after SPA is set and before
		 * DSP is unstalled
		 */
		skl_ipc_int_enable(ctx);
		skl_ipc_op_int_enable(ctx);
		skl->boot_complete = false;
	}

	ret = skl_dsp_start_core(ctx, core_mask);
	if (ret < 0)
		goto err;

	if (core_id == SKL_DSP_CORE0_ID) {
		ret = wait_event_timeout(skl->boot_wait,
				skl->boot_complete,
				msecs_to_jiffies(SKL_IPC_BOOT_MSECS));

	/* If core 1 was turned on for booting core 0, turn it off */
		skl_dsp_core_power_down(ctx, SKL_DSP_CORE_MASK(1));
		if (ret == 0) {
			dev_err(ctx->dev, "%s: DSP boot timeout\n", __func__);
			dev_err(ctx->dev, "Error code=0x%x: FW status=0x%x\n",
				sst_dsp_shim_read(ctx, BXT_ADSP_ERROR_CODE),
				sst_dsp_shim_read(ctx, BXT_ADSP_FW_STATUS));
			dev_err(ctx->dev, "Failed to set core0 to D0 state\n");
			ret = -EIO;
			goto err;
		}
	}

	/* Tell FW if additional core in now On */

	if (core_id != SKL_DSP_CORE0_ID) {
		dx.core_mask = core_mask;
		dx.dx_mask = core_mask;

		ret = skl_ipc_set_dx(&skl->ipc, BXT_INSTANCE_ID,
					BXT_BASE_FW_MODULE_ID, &dx);
		if (ret < 0) {
			dev_err(ctx->dev, "IPC set_dx for core %d fail: %d\n",
								core_id, ret);
			goto err;
		}
	}

	skl->cores.state[core_id] = SKL_DSP_RUNNING;
	ret = skl_notify_tplg_change(skl, SKL_TPLG_CHG_NOTIFY_DSP_D0);
	if (ret < 0)
		dev_warn(ctx->dev,
			"update of topology event D0 failed\n");

	return 0;
err:
	if (core_id == SKL_DSP_CORE0_ID)
		core_mask |= SKL_DSP_CORE_MASK(1);
	skl_dsp_disable_core(ctx, core_mask);

	return ret;
}

static int bxt_set_dsp_D3(struct sst_dsp *ctx, unsigned int core_id)
{
	int ret;
	struct skl_ipc_dxstate_info dx;
	struct skl_sst *skl = ctx->thread_context;
	unsigned int core_mask = SKL_DSP_CORE_MASK(core_id);

	dx.core_mask = core_mask;
	dx.dx_mask = SKL_IPC_D3_MASK;

	dev_dbg(ctx->dev, "core mask=%x dx_mask=%x\n",
			dx.core_mask, dx.dx_mask);

	ret = skl_ipc_set_dx(&skl->ipc, BXT_INSTANCE_ID,
				BXT_BASE_FW_MODULE_ID, &dx);
	if (ret < 0) {
		dev_err(ctx->dev,
		"Failed to set DSP to D3:core id = %d;Continue reset\n",
		core_id);
		/*
		 * In case of D3 failure, re-download the firmware, so set
		 * fw_loaded to false.
		 */
		skl->fw_loaded = false;
	}

	if (core_id == SKL_DSP_CORE0_ID) {
		/* disable Interrupt */
		skl_ipc_op_int_disable(ctx);
		skl_ipc_int_disable(ctx);
	}
	ret = skl_dsp_disable_core(ctx, core_mask);
	if (ret < 0) {
		dev_err(ctx->dev, "Failed to disable core %d\n", ret);
		return ret;
	}
	skl->cores.state[core_id] = SKL_DSP_RESET;
	ret = skl_notify_tplg_change(skl, SKL_TPLG_CHG_NOTIFY_DSP_D3);
	if (ret < 0)
		dev_warn(ctx->dev,
			"update of topology event D3 failed\n");

	return 0;
}

static const struct skl_dsp_fw_ops bxt_fw_ops = {
	.set_state_D0 = bxt_set_dsp_D0,
	.set_state_D3 = bxt_set_dsp_D3,
	.set_state_D0i3 = bxt_schedule_dsp_D0i3,
	.set_state_D0i0 = bxt_set_dsp_D0i0,
	.load_fw = bxt_load_base_firmware,
	.get_fw_errcode = bxt_get_errorcode,
	.load_library = bxt_load_library,
};

static struct sst_ops skl_ops = {
	.irq_handler = skl_dsp_sst_interrupt,
	.write = sst_shim32_write,
	.read = sst_shim32_read,
	.ram_read = sst_memcpy_fromio_32,
	.ram_write = sst_memcpy_toio_32,
	.free = skl_dsp_free,
};

static struct sst_dsp_device skl_dev = {
	.thread = skl_dsp_irq_thread_handler,
	.ops = &skl_ops,
};

int bxt_sst_dsp_init(struct device *dev, void __iomem *mmio_base, int irq,
			const char *fw_name, struct skl_dsp_loader_ops dsp_ops,
			struct skl_sst **dsp, void *ptr)
{
	struct skl_sst *skl;
	struct sst_dsp *sst;
	u32 dsp_wp[] = {BXT_ADSP_WP_DSP0, BXT_ADSP_WP_DSP1};
	int ret;

	ret = skl_sst_ctx_init(dev, irq, fw_name, dsp_ops, dsp, &skl_dev);
	if (ret < 0) {
		dev_err(dev, "%s: no device\n", __func__);
		return ret;
	}

	skl = *dsp;
	sst = skl->dsp;
	sst->fw_ops = bxt_fw_ops;
	sst->addr.lpe = mmio_base;
	sst->addr.shim = mmio_base;
	sst->addr.sram0_base = BXT_ADSP_SRAM0_BASE;
	sst->addr.sram1_base = BXT_ADSP_SRAM1_BASE;
	sst->addr.w0_stat_sz = SKL_ADSP_W0_STAT_SZ;
	sst->addr.w0_up_sz = SKL_ADSP_W0_UP_SZ;

	sst_dsp_mailbox_init(sst, (BXT_ADSP_SRAM0_BASE + SKL_ADSP_W0_STAT_SZ),
			SKL_ADSP_W0_UP_SZ, BXT_ADSP_SRAM1_BASE, SKL_ADSP_W1_SZ);
	ret = skl_dsp_init_trace_window(sst, dsp_wp, BXT_ADSP_SRAM2_BASE,
					BXT_ADSP_W2_SIZE, BXT_ADSP_NR_DSP);
	if (ret) {
		dev_err(dev, "FW tracing init failed : %x", ret);
		return ret;
	}

	ret = skl_ipc_init(dev, skl);
	if (ret) {
		skl_dsp_free(sst);
		return ret;
	}

	/* set the D0i3 check */
	skl->ipc.ops.check_dsp_lp_on = skl_ipc_check_D0i0;

	skl->boot_complete = false;
	init_waitqueue_head(&skl->boot_wait);
	INIT_DELAYED_WORK(&skl->d0i3.work, bxt_set_dsp_D0i3);
	skl->d0i3.state = SKL_DSP_D0I3_NONE;

	return skl_dsp_acquire_irq(sst);
}
EXPORT_SYMBOL_GPL(bxt_sst_dsp_init);

int bxt_sst_init_fw(struct device *dev, struct skl_sst *ctx)
{
	int ret;
	struct sst_dsp *sst = ctx->dsp;

	ret = sst->fw_ops.load_fw(sst);
	if (ret < 0) {
		dev_err(dev, "Load base fw failed: %x\n", ret);
		return ret;
	}

	skl_dsp_init_core_state(sst);

	if (ctx->lib_count > 1) {
		ret = sst->fw_ops.load_library(sst, ctx->lib_info,
						ctx->lib_count);
		if (ret < 0) {
			dev_err(dev, "Load Library failed : %x\n", ret);
			return ret;
		}
	}
	ctx->is_first_boot = false;

	return 0;
}
EXPORT_SYMBOL_GPL(bxt_sst_init_fw);

void bxt_sst_dsp_cleanup(struct device *dev, struct skl_sst *ctx)
{

	skl_release_library(ctx->lib_info, ctx->lib_count);
	if (ctx->dsp->fw)
		release_firmware(ctx->dsp->fw);
	skl_freeup_uuid_list(ctx);
	skl_ipc_free(&ctx->ipc);
	ctx->dsp->ops->free(ctx->dsp);
}
EXPORT_SYMBOL_GPL(bxt_sst_dsp_cleanup);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Intel Broxton IPC driver");
