/*
 *  skl.c - Implementation of ASoC Intel SKL HD Audio driver
 *
 *  Copyright (C) 2014-2015 Intel Corp
 *  Author: Jeeja KP <jeeja.kp@intel.com>
 *
 *  Derived mostly from Intel HDA driver with following copyrights:
 *  Copyright (c) 2004 Takashi Iwai <tiwai@suse.de>
 *                     PeiSen Hou <pshou@realtek.com.tw>
 *  ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; version 2 of the License.
 *
 *  This program is distributed in the hope that it will be useful, but
 *  WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  General Public License for more details.
 *
 * ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/firmware.h>
#include <linux/delay.h>
#include <sound/pcm.h>
#include <sound/soc-acpi.h>
#include <sound/soc-acpi-intel-match.h>
#include <sound/hda_register.h>
#include <sound/hdaudio.h>
#include <sound/hda_i915.h>
#include <sound/compress_driver.h>
#include "skl.h"
#include "skl-sst-dsp.h"
#include "skl-sst-ipc.h"
#include "skl-topology.h"

#if !IS_ENABLED(CONFIG_SND_SOC_INTEL_CNL_FPGA)
static struct skl_machine_pdata skl_dmic_data;
#endif
#include "virtio/skl-virtio.h"

/*
 * initialize the PCI registers
 */
static void skl_update_pci_byte(struct pci_dev *pci, unsigned int reg,
			    unsigned char mask, unsigned char val)
{
	unsigned char data;

	pci_read_config_byte(pci, reg, &data);
	data &= ~mask;
	data |= (val & mask);
	pci_write_config_byte(pci, reg, data);
}

static void skl_init_pci(struct skl *skl)
{
	struct hdac_bus *bus = skl_to_bus(skl);

	/*
	 * Clear bits 0-2 of PCI register TCSEL (at offset 0x44)
	 * TCSEL == Traffic Class Select Register, which sets PCI express QOS
	 * Ensuring these bits are 0 clears playback static on some HD Audio
	 * codecs.
	 * The PCI register TCSEL is defined in the Intel manuals.
	 */
	dev_dbg(bus->dev, "Clearing TCSEL\n");
	skl_update_pci_byte(skl->pci, AZX_PCIREG_TCSEL, 0x07, 0);
}

static void update_pci_dword(struct pci_dev *pci,
			unsigned int reg, u32 mask, u32 val)
{
	u32 data = 0;

	pci_read_config_dword(pci, reg, &data);
	data &= ~mask;
	data |= (val & mask);
	pci_write_config_dword(pci, reg, data);
}

/*
 * skl_enable_miscbdcge - enable/dsiable CGCTL.MISCBDCGE bits
 *
 * @dev: device pointer
 * @enable: enable/disable flag
 */
static void skl_enable_miscbdcge(struct device *dev, bool enable)
{
	struct pci_dev *pci = to_pci_dev(dev);
	u32 val;

	val = enable ? AZX_CGCTL_MISCBDCGE_MASK : 0;

	update_pci_dword(pci, AZX_PCIREG_CGCTL, AZX_CGCTL_MISCBDCGE_MASK, val);
}

/**
 * skl_clock_power_gating: Enable/Disable clock and power gating
 *
 * @dev: Device pointer
 * @enable: Enable/Disable flag
 */
static void skl_clock_power_gating(struct device *dev, bool enable)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct hdac_bus *bus = pci_get_drvdata(pci);
	u32 val;

	/* Update PDCGE bit of CGCTL register */
	val = enable ? AZX_CGCTL_ADSPDCGE : 0;
	update_pci_dword(pci, AZX_PCIREG_CGCTL, AZX_CGCTL_ADSPDCGE, val);

	/* Update L1SEN bit of EM2 register */
	val = enable ? AZX_REG_VS_EM2_L1SEN : 0;
	snd_hdac_chip_updatel(bus, VS_EM2, AZX_REG_VS_EM2_L1SEN, val);

	/* Update ADSPPGD bit of PGCTL register */
	val = enable ? 0 : AZX_PGCTL_ADSPPGD;
	update_pci_dword(pci, AZX_PCIREG_PGCTL, AZX_PGCTL_ADSPPGD, val);
}

int skl_request_tplg(struct skl *skl, const struct firmware **fw)
{
	return request_firmware(fw, skl->tplg_name, skl->skl_sst->dev);
}


/*
 * While performing reset, controller may not come back properly causing
 * issues, so recommendation is to set CGCTL.MISCBDCGE to 0 then do reset
 * (init chip) and then again set CGCTL.MISCBDCGE to 1
 */
static int skl_init_chip(struct hdac_bus *bus, bool full_reset)
{
	struct hdac_ext_link *hlink;
	int ret;

	skl_enable_miscbdcge(bus->dev, false);
	ret = snd_hdac_bus_init_chip(bus, full_reset);

	/* Reset stream-to-link mapping */
	list_for_each_entry(hlink, &bus->hlink_list, list)
		bus->io_ops->reg_writel(0, hlink->ml_addr + AZX_REG_ML_LOSIDV);

	skl_enable_miscbdcge(bus->dev, true);

	return ret;
}

void skl_update_d0i3c(struct device *dev, bool enable)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct hdac_bus *bus = pci_get_drvdata(pci);
	u8 reg;
	int timeout = 50;

	reg = snd_hdac_chip_readb(bus, VS_D0I3C);
	/* Do not write to D0I3C until command in progress bit is cleared */
	while ((reg & AZX_REG_VS_D0I3C_CIP) && --timeout) {
		udelay(10);
		reg = snd_hdac_chip_readb(bus, VS_D0I3C);
	}

	/* Highly unlikely. But if it happens, flag error explicitly */
	if (!timeout) {
		dev_err(bus->dev, "Before D0I3C update: D0I3C CIP timeout\n");
		return;
	}

	if (enable)
		reg = reg | AZX_REG_VS_D0I3C_I3;
	else
		reg = reg & (~AZX_REG_VS_D0I3C_I3);

	snd_hdac_chip_writeb(bus, VS_D0I3C, reg);

	timeout = 50;
	/* Wait for cmd in progress to be cleared before exiting the function */
	reg = snd_hdac_chip_readb(bus, VS_D0I3C);
	while ((reg & AZX_REG_VS_D0I3C_CIP) && --timeout) {
		udelay(10);
		reg = snd_hdac_chip_readb(bus, VS_D0I3C);
	}

	/* Highly unlikely. But if it happens, flag error explicitly */
	if (!timeout) {
		dev_err(bus->dev, "After D0I3C update: D0I3C CIP timeout\n");
		return;
	}

	dev_dbg(bus->dev, "D0I3C register = 0x%x\n",
			snd_hdac_chip_readb(bus, VS_D0I3C));
}

static void skl_get_total_bytes_transferred(struct hdac_stream *hstr)
{
	int pos, no_of_bytes;
	unsigned int prev_pos;

	div_u64_rem(hstr->curr_pos,
		   hstr->stream->runtime->buffer_size, &prev_pos);
	pos = snd_hdac_stream_get_pos_posbuf(hstr);

	if (pos < prev_pos)
		no_of_bytes = (hstr->stream->runtime->buffer_size - prev_pos) +  pos;
	else
		no_of_bytes = pos - prev_pos;

	hstr->curr_pos += no_of_bytes;
}

/*
 * skl_dum_set - Set the DUM bit in EM2 register to fix the IP bug
 * of incorrect postion reporting for capture stream.
 */
static void skl_dum_set(struct hdac_bus *bus)
{
	u32 reg;
	u8 val;

	/*
	 * For the DUM bit to be set, CRST needs to be out of reset state
	 */
	val = snd_hdac_chip_readb(bus, GCTL) & AZX_GCTL_RESET;
	if (!val) {
		skl_enable_miscbdcge(bus->dev, false);
		snd_hdac_bus_exit_link_reset(bus);
		skl_enable_miscbdcge(bus->dev, true);
	}
	/*
	 * Set the DUM bit in EM2 register to fix the IP bug of incorrect
	 * postion reporting for capture stream.
	 */
	reg  = snd_hdac_chip_readl(bus, VS_EM2);
	snd_hdac_chip_writel(bus, VS_EM2, (reg | AZX_EM2_DUM_MASK));
}

/* called from IRQ */
static void skl_stream_update(struct hdac_bus *bus, struct hdac_stream *hstr)
{
	if (hstr->substream) {
		snd_pcm_period_elapsed(hstr->substream);
	}
	else if (hstr->stream) {
		skl_get_total_bytes_transferred(hstr);
		snd_compr_fragment_elapsed(hstr->stream);
	}
}

static irqreturn_t skl_interrupt(int irq, void *dev_id)
{
	struct hdac_bus *bus = dev_id;
	u32 status;
	u32 mask, int_enable;
	int ret = IRQ_NONE;

	if (!pm_runtime_active(bus->dev))
		return ret;

	spin_lock(&bus->reg_lock);

	status = snd_hdac_chip_readl(bus, INTSTS);
	if (status == 0 || status == 0xffffffff) {
		spin_unlock(&bus->reg_lock);
		return ret;
	}

	/* clear rirb int */
	status = snd_hdac_chip_readb(bus, RIRBSTS);
	if (status & RIRB_INT_MASK) {
		if (status & RIRB_INT_RESPONSE)
			snd_hdac_bus_update_rirb(bus);
		snd_hdac_chip_writeb(bus, RIRBSTS, RIRB_INT_MASK);
	}

	mask = (0x1 << bus->num_streams) - 1;

	status = snd_hdac_chip_readl(bus, INTSTS);
	status &= mask;
	if (status) {
		/* Disable stream interrupts; Re-enable in bottom half */
		int_enable = snd_hdac_chip_readl(bus, INTCTL);
		snd_hdac_chip_writel(bus, INTCTL, (int_enable & (~mask)));
		ret = IRQ_WAKE_THREAD;
	} else
		ret = IRQ_HANDLED;

	spin_unlock(&bus->reg_lock);
	return ret;

}

static irqreturn_t skl_threaded_handler(int irq, void *dev_id)
{
	struct hdac_bus *bus = dev_id;
	struct skl *skl = bus_to_skl(bus);
	u32 status;
	u32 int_enable;
	u32 mask;
	unsigned long flags;

	status = snd_hdac_chip_readl(bus, INTSTS);

	snd_hdac_bus_handle_stream_irq(bus, status, skl->skl_sst->hda_irq_ack);

	/* Re-enable stream interrupts */
	mask = (0x1 << bus->num_streams) - 1;
	spin_lock_irqsave(&bus->reg_lock, flags);
	int_enable = snd_hdac_chip_readl(bus, INTCTL);
	snd_hdac_chip_writel(bus, INTCTL, (int_enable | mask));
	spin_unlock_irqrestore(&bus->reg_lock, flags);
	return IRQ_HANDLED;
}

static int skl_acquire_irq(struct hdac_bus *bus, int do_disconnect)
{
	struct skl *skl = bus_to_skl(bus);
	int ret;

	ret = request_threaded_irq(skl->pci->irq, skl_interrupt,
			skl_threaded_handler,
			IRQF_SHARED,
			KBUILD_MODNAME, bus);
	if (ret) {
		dev_err(bus->dev,
			"unable to grab IRQ %d, disabling device\n",
			skl->pci->irq);
		return ret;
	}

	bus->irq = skl->pci->irq;
	pci_intx(skl->pci, 1);

	return 0;
}

static int skl_suspend_late(struct device *dev)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct hdac_bus *bus = pci_get_drvdata(pci);
	struct skl *skl = bus_to_skl(bus);

	return skl_suspend_late_dsp(skl);
}

#ifdef CONFIG_PM
static int _skl_suspend(struct hdac_bus *bus)
{
	struct skl *skl = bus_to_skl(bus);
	struct pci_dev *pci = to_pci_dev(bus->dev);
	int ret;

	snd_hdac_ext_bus_link_power_down_all(bus);

	ret = skl_suspend_dsp(skl);
	if (ret < 0)
		return ret;

	snd_hdac_bus_stop_chip(bus);
	update_pci_dword(pci, AZX_PCIREG_PGCTL,
		AZX_PGCTL_LSRMD_MASK, AZX_PGCTL_LSRMD_MASK);
	skl_enable_miscbdcge(bus->dev, false);
	snd_hdac_bus_enter_link_reset(bus);
	skl_enable_miscbdcge(bus->dev, true);
	skl_cleanup_resources(skl);

	return 0;
}

static int _skl_resume(struct hdac_bus *bus)
{
	struct skl *skl = bus_to_skl(bus);

	skl_init_pci(skl);
	skl_init_chip(bus, true);

	return skl_resume_dsp(skl);
}
#endif

#ifdef CONFIG_PM_SLEEP
/*
 * power management
 */
static int skl_suspend(struct device *dev)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct hdac_bus *bus = pci_get_drvdata(pci);
	struct skl *skl  = bus_to_skl(bus);
	int ret = 0;

	/*
	 * Do not suspend if streams which are marked ignore suspend are
	 * running, we need to save the state for these and continue
	 */
	if (skl->supend_active) {
		/* turn off the links and stop the CORB/RIRB DMA if it is On */
		snd_hdac_ext_bus_link_power_down_all(bus);

		if (bus->cmd_dma_state)
			snd_hdac_bus_stop_cmd_io(bus);

		enable_irq_wake(bus->irq);
		pci_save_state(pci);
	} else {
		ret = _skl_suspend(bus);
		if (ret < 0)
			return ret;
		skl->skl_sst->fw_loaded = false;
	}

	if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)) {
		ret = snd_hdac_display_power(bus, false);
		if (ret < 0)
			dev_err(bus->dev,
				"Cannot turn OFF display power on i915\n");
	}

	return ret;
}

static int skl_resume(struct device *dev)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct hdac_bus *bus = pci_get_drvdata(pci);
	struct skl *skl  = bus_to_skl(bus);
	struct hdac_ext_link *hlink = NULL;
	int ret = 0;

	/* Turned OFF in HDMI codec driver after codec reconfiguration */
	if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)) {
		ret = snd_hdac_display_power(bus, true);
		if (ret < 0) {
			dev_err(bus->dev,
				"Cannot turn on display power on i915\n");
			return ret;
		}
	}

	/*
	 * resume only when we are not in suspend active, otherwise need to
	 * restore the device
	 */
	if (skl->supend_active) {
		pci_restore_state(pci);
		snd_hdac_ext_bus_link_power_up_all(bus);
		disable_irq_wake(bus->irq);
		/*
		 * turn On the links which are On before active suspend
		 * and start the CORB/RIRB DMA if On before
		 * active suspend.
		 */
		list_for_each_entry(hlink, &bus->hlink_list, list) {
			if (hlink->ref_count)
				snd_hdac_ext_bus_link_power_up(hlink);
		}

		ret = 0;
		if (bus->cmd_dma_state)
			snd_hdac_bus_init_cmd_io(bus);
	} else {
		ret = _skl_resume(bus);
		skl_dum_set(bus);

		/* turn off the links which are off before suspend */
		list_for_each_entry(hlink, &bus->hlink_list, list) {
			if (!hlink->ref_count)
				snd_hdac_ext_bus_link_power_down(hlink);
		}

		if (!bus->cmd_dma_state)
			snd_hdac_bus_stop_cmd_io(bus);
	}

	return ret;
}
#endif /* CONFIG_PM_SLEEP */

#ifdef CONFIG_PM
static int skl_runtime_suspend(struct device *dev)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct hdac_bus *bus = pci_get_drvdata(pci);

	dev_dbg(bus->dev, "in %s\n", __func__);

	return _skl_suspend(bus);
}

static int skl_runtime_resume(struct device *dev)
{
	struct pci_dev *pci = to_pci_dev(dev);
	struct hdac_bus *bus = pci_get_drvdata(pci);

	dev_dbg(bus->dev, "in %s\n", __func__);

	return _skl_resume(bus);
}
#endif /* CONFIG_PM */

static const struct dev_pm_ops skl_pm = {
	SET_SYSTEM_SLEEP_PM_OPS(skl_suspend, skl_resume)
	SET_RUNTIME_PM_OPS(skl_runtime_suspend, skl_runtime_resume, NULL)
	.suspend_late = skl_suspend_late,
};

/*
 * destructor
 */
static int skl_free(struct hdac_bus *bus)
{
	struct skl *skl  = bus_to_skl(bus);

	skl->init_done = 0; /* to be sure */

	snd_hdac_ext_stop_streams(bus);

	if (bus->irq >= 0)
		free_irq(bus->irq, (void *)bus);
	snd_hdac_bus_free_stream_pages(bus);
	snd_hdac_stream_free_all(bus);
	snd_hdac_link_free_all(bus);

	if (bus->remap_addr)
		iounmap(bus->remap_addr);

	pci_release_regions(skl->pci);
	pci_disable_device(skl->pci);

	snd_hdac_ext_bus_exit(bus);

	cancel_work_sync(&skl->probe_work);
	if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI))
		snd_hdac_i915_exit(bus);

	return 0;
}

/*
 * For each ssp there are 3 clocks (mclk/sclk/sclkfs).
 * e.g. for ssp0, clocks will be named as
 *      "ssp0_mclk", "ssp0_sclk", "ssp0_sclkfs"
 * So for skl+, there are 6 ssps, so 18 clocks will be created.
 */
static struct skl_ssp_clk skl_ssp_clks[] = {
	{.name = "ssp0_mclk"}, {.name = "ssp1_mclk"}, {.name = "ssp2_mclk"},
	{.name = "ssp3_mclk"}, {.name = "ssp4_mclk"}, {.name = "ssp5_mclk"},
	{.name = "ssp0_sclk"}, {.name = "ssp1_sclk"}, {.name = "ssp2_sclk"},
	{.name = "ssp3_sclk"}, {.name = "ssp4_sclk"}, {.name = "ssp5_sclk"},
	{.name = "ssp0_sclkfs"}, {.name = "ssp1_sclkfs"},
						{.name = "ssp2_sclkfs"},
	{.name = "ssp3_sclkfs"}, {.name = "ssp4_sclkfs"},
						{.name = "ssp5_sclkfs"},
};

static int skl_find_machine(struct skl *skl, void *driver_data)
{
	struct hdac_bus *bus = skl_to_bus(skl);
	struct snd_soc_acpi_mach *mach = driver_data;
	struct skl_machine_pdata *pdata;

	if (IS_ENABLED(CONFIG_SND_SOC_RT700) ||
	    IS_ENABLED(CONFIG_SND_SOC_INTEL_CNL_FPGA))
		goto out;

	mach = snd_soc_acpi_find_machine(mach);
	if (mach == NULL) {
		dev_err(bus->dev, "No matching machine driver found\n");
		return -ENODEV;
	}

out:
	skl->mach = mach;
	skl->fw_name = mach->fw_filename;
	pdata = mach->pdata;

	if (pdata) {
		skl->use_tplg_pcm = pdata->use_tplg_pcm;
		pdata->dmic_num = skl_get_dmic_geo(skl);
	}

	return 0;
}

#if IS_ENABLED(CONFIG_SND_SOC_INTEL_SKYLAKE_VIRTIO_BE)

int skl_virt_device_register(struct skl *skl)
{
	struct hdac_bus *bus = skl_to_bus(skl);
	struct platform_device *pdev;
	struct skl_virt_pdata *pdata;
	int ret;

	pdev = platform_device_alloc("skl-virt-audio", -1);
	if (pdev == NULL) {
		dev_err(bus->dev, "platform device alloc failed\n");
		return -EIO;
	}

	ret = platform_device_add(pdev);
	if (ret) {
		dev_err(bus->dev, "failed to add virtualization device\n");
		ret = -EIO;
		goto out_pdev_put;
	}

	pdata = kzalloc(sizeof(struct skl_virt_pdata), GFP_KERNEL);
	if (pdata == NULL) {
		ret = -ENOMEM;
		goto out_pdev_put;
	}

	pdata->skl = skl;
	skl->virt_dev = pdev;
	ret = platform_device_add_data(pdev, pdata, sizeof(struct skl_virt_pdata));
	if (ret) {
		dev_err(bus->dev, "failed to add platform data\n");
		ret = -EIO;
	}

	kfree(pdata);
out_pdev_put:
	if (ret) {
		platform_device_put(pdev);
	}
	return ret;
}

void skl_virt_device_unregister(struct skl *skl)
{
	if (skl->virt_dev)
		platform_device_unregister(skl->virt_dev);
}

#endif

static int skl_machine_device_register(struct skl *skl)
{
	struct hdac_bus *bus = skl_to_bus(skl);
	struct snd_soc_acpi_mach *mach = skl->mach;
	struct platform_device *pdev;
	int ret;

	pdev = platform_device_alloc(mach->drv_name, -1);
	if (pdev == NULL) {
		dev_err(bus->dev, "platform device alloc failed\n");
		return -EIO;
	}

	ret = platform_device_add(pdev);
	if (ret) {
		dev_err(bus->dev, "failed to add machine device\n");
		platform_device_put(pdev);
		return -EIO;
	}

	if (mach->pdata)
		dev_set_drvdata(&pdev->dev, mach->pdata);

	skl->i2s_dev = pdev;

	return 0;
}

static void skl_machine_device_unregister(struct skl *skl)
{
	if (skl->i2s_dev)
		platform_device_unregister(skl->i2s_dev);
}

static int skl_dmic_device_register(struct skl *skl)
{
	struct hdac_bus *bus = skl_to_bus(skl);
	struct platform_device *pdev;
	int ret;

	/* SKL has one dmic port, so allocate dmic device for this */
	pdev = platform_device_alloc("dmic-codec", -1);
	if (!pdev) {
		dev_err(bus->dev, "failed to allocate dmic device\n");
		return -ENOMEM;
	}

	ret = platform_device_add(pdev);
	if (ret) {
		dev_err(bus->dev, "failed to add dmic device: %d\n", ret);
		platform_device_put(pdev);
		return ret;
	}
	skl->dmic_dev = pdev;

	return 0;
}

static void skl_dmic_device_unregister(struct skl *skl)
{
	if (skl->dmic_dev)
		platform_device_unregister(skl->dmic_dev);
}

static struct skl_clk_parent_src skl_clk_src[] = {
	{ .clk_id = SKL_XTAL, .name = "xtal" },
	{ .clk_id = SKL_CARDINAL, .name = "cardinal", .rate = 24576000 },
	{ .clk_id = SKL_PLL, .name = "pll", .rate = 96000000 },
};

struct skl_clk_parent_src *skl_get_parent_clk(u8 clk_id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(skl_clk_src); i++) {
		if (skl_clk_src[i].clk_id == clk_id)
			return &skl_clk_src[i];
	}

	return NULL;
}

static void init_skl_xtal_rate(int pci_id)
{
	switch (pci_id) {
	case 0x9d70:
	case 0x9d71:
		skl_clk_src[0].rate = 24000000;
		return;

	default:
		skl_clk_src[0].rate = 19200000;
		return;
	}
}

static int skl_clock_device_register(struct skl *skl)
{
	struct platform_device_info pdevinfo = {NULL};
	struct skl_clk_pdata *clk_pdata;

	clk_pdata = devm_kzalloc(&skl->pci->dev, sizeof(*clk_pdata),
							GFP_KERNEL);
	if (!clk_pdata)
		return -ENOMEM;

	init_skl_xtal_rate(skl->pci->device);

	clk_pdata->parent_clks = skl_clk_src;
	clk_pdata->ssp_clks = skl_ssp_clks;
	clk_pdata->num_clks = ARRAY_SIZE(skl_ssp_clks);

	/* Query NHLT to fill the rates and parent */
	skl_get_clks(skl, clk_pdata->ssp_clks);
	clk_pdata->pvt_data = skl;

	/* Register Platform device */
	pdevinfo.parent = &skl->pci->dev;
	pdevinfo.id = -1;
	pdevinfo.name = "skl-ssp-clk";
	pdevinfo.data = clk_pdata;
	pdevinfo.size_data = sizeof(*clk_pdata);
	skl->clk_dev = platform_device_register_full(&pdevinfo);
	return PTR_ERR_OR_ZERO(skl->clk_dev);
}

static void skl_clock_device_unregister(struct skl *skl)
{
	if (skl->clk_dev)
		platform_device_unregister(skl->clk_dev);
}

/*
 * Probe the given codec address
 */
static int probe_codec(struct hdac_bus *bus, int addr)
{
	unsigned int cmd = (addr << 28) | (AC_NODE_ROOT << 20) |
		(AC_VERB_PARAMETERS << 8) | AC_PAR_VENDOR_ID;
	unsigned int res = -1;
	struct skl *skl = bus_to_skl(bus);
	struct hdac_device *hdev;

	mutex_lock(&bus->cmd_mutex);
	snd_hdac_bus_send_cmd(bus, cmd);
	snd_hdac_bus_get_response(bus, addr, &res);
	mutex_unlock(&bus->cmd_mutex);
	if (res == -1)
		return -EIO;
	dev_dbg(bus->dev, "codec #%d probed OK\n", addr);

	hdev = devm_kzalloc(&skl->pci->dev, sizeof(*hdev), GFP_KERNEL);
	if (!hdev)
		return -ENOMEM;

	return snd_hdac_ext_bus_device_init(bus, addr, hdev);
}

/* Codec initialization */
static void skl_codec_create(struct hdac_bus *bus)
{
	int c, max_slots;

	max_slots = HDA_MAX_CODECS;

	/* First try to probe all given codec slots */
	for (c = 0; c < max_slots; c++) {
		if ((bus->codec_mask & (1 << c))) {
			if (probe_codec(bus, c) < 0) {
				/*
				 * Some BIOSen give you wrong codec addresses
				 * that don't exist
				 */
				dev_warn(bus->dev,
					 "Codec #%d probe error; disabling it...\n", c);
				bus->codec_mask &= ~(1 << c);
				/*
				 * More badly, accessing to a non-existing
				 * codec often screws up the controller bus,
				 * and disturbs the further communications.
				 * Thus if an error occurs during probing,
				 * better to reset the controller bus to get
				 * back to the sanity state.
				 */
				snd_hdac_bus_stop_chip(bus);
				skl_init_chip(bus, true);
			}
		}
	}
}

static const struct hdac_bus_ops bus_core_ops = {
	.command = snd_hdac_bus_send_cmd,
	.get_response = snd_hdac_bus_get_response,
};

static int skl_i915_init(struct hdac_bus *bus)
{
	int err;

	/*
	 * The HDMI codec is in GPU so we need to ensure that it is powered
	 * up and ready for probe
	 */
	err = snd_hdac_i915_init(bus);
	if (err < 0)
		return err;

	err = snd_hdac_display_power(bus, true);
	if (err < 0)
		dev_err(bus->dev, "Cannot turn on display power on i915\n");

	return err;
}

static void skl_probe_work(struct work_struct *work)
{
	struct skl *skl = container_of(work, struct skl, probe_work);
	struct hdac_bus *bus = skl_to_bus(skl);
	struct hdac_ext_link *hlink = NULL;
	int err;

	if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)) {
		err = skl_i915_init(bus);
		if (err < 0)
			return;
	}

	err = skl_init_chip(bus, true);
	if (err < 0) {
		dev_err(bus->dev, "Init chip failed with err: %d\n", err);
		goto out_err;
	}

	/* codec detection */
	if (!bus->codec_mask)
		dev_info(bus->dev, "no hda codecs found!\n");

	/* create codec instances */
	skl_codec_create(bus);

	/* register platform dai and controls */
	err = skl_platform_register(bus->dev);
	if (err < 0) {
		dev_err(bus->dev, "platform register failed: %d\n", err);
		return;
	}

	if (bus->ppcap) {
		err = skl_machine_device_register(skl);
		if (err < 0) {
			dev_err(bus->dev, "machine register failed: %d\n", err);
			goto out_err;
		}
	}

	/*
	 * we are done probing so decrement link counts
	 */
	list_for_each_entry(hlink, &bus->hlink_list, list)
		snd_hdac_ext_bus_link_put(bus, hlink);

	if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI)) {
		err = snd_hdac_display_power(bus, false);
		if (err < 0) {
			dev_err(bus->dev, "Cannot turn off display power on i915\n");
			skl_machine_device_unregister(skl);
			return;
		}
	}

	/* configure PM */
	pm_runtime_put_noidle(bus->dev);
	pm_runtime_allow(bus->dev);
	skl->init_done = 1;

	return;

out_err:
	if (IS_ENABLED(CONFIG_SND_SOC_HDAC_HDMI))
		err = snd_hdac_display_power(bus, false);
}

/*
 * constructor
 */
static int skl_create(struct pci_dev *pci,
		      const struct hdac_io_ops *io_ops,
		      struct skl **rskl)
{
	struct skl *skl;
	struct hdac_bus *bus;

	int err;

	*rskl = NULL;

	err = pci_enable_device(pci);
	if (err < 0)
		return err;

	skl = devm_kzalloc(&pci->dev, sizeof(*skl), GFP_KERNEL);
	if (!skl) {
		pci_disable_device(pci);
		return -ENOMEM;
	}

	bus = skl_to_bus(skl);
	snd_hdac_ext_bus_init(bus, &pci->dev, &bus_core_ops, io_ops, NULL);
	bus->use_posbuf = 1;
	skl->pci = pci;
	INIT_WORK(&skl->probe_work, skl_probe_work);
	bus->bdl_pos_adj = 0;

	*rskl = skl;

	return 0;
}

static int skl_first_init(struct hdac_bus *bus)
{
	struct skl *skl = bus_to_skl(bus);
	struct pci_dev *pci = skl->pci;
	int err;
	unsigned short gcap;
	int cp_streams, pb_streams, start_idx;

	err = pci_request_regions(pci, "Skylake HD audio");
	if (err < 0)
		return err;

	bus->addr = pci_resource_start(pci, 0);
	bus->remap_addr = pci_ioremap_bar(pci, 0);
	if (bus->remap_addr == NULL) {
		dev_err(bus->dev, "ioremap error\n");
		return -ENXIO;
	}

	snd_hdac_bus_reset_link(bus, true);

	snd_hdac_bus_parse_capabilities(bus);

	pci_set_master(pci);

	gcap = snd_hdac_chip_readw(bus, GCAP);
	dev_dbg(bus->dev, "chipset global capabilities = 0x%x\n", gcap);

	/* allow 64bit DMA address if supported by H/W */
	if (!dma_set_mask(bus->dev, DMA_BIT_MASK(64))) {
		dma_set_coherent_mask(bus->dev, DMA_BIT_MASK(64));
	} else {
		dma_set_mask(bus->dev, DMA_BIT_MASK(32));
		dma_set_coherent_mask(bus->dev, DMA_BIT_MASK(32));
	}

	/* read number of streams from GCAP register */
	cp_streams = (gcap >> 8) & 0x0f;
	pb_streams = (gcap >> 12) & 0x0f;

	if (!pb_streams && !cp_streams)
		return -EIO;

	bus->num_streams = cp_streams + pb_streams;

	/* initialize streams */
	snd_hdac_ext_stream_init_all
		(bus, 0, cp_streams, SNDRV_PCM_STREAM_CAPTURE);
	start_idx = cp_streams;
	snd_hdac_ext_stream_init_all
		(bus, start_idx, pb_streams, SNDRV_PCM_STREAM_PLAYBACK);

	err = snd_hdac_bus_alloc_stream_pages(bus);
	if (err < 0)
		return err;

	err = skl_acquire_irq(bus, 0);
	if (err < 0)
		return err;

	synchronize_irq(bus->irq);

	/* initialize chip */
	skl_init_pci(skl);

	skl_dum_set(bus);

	return skl_init_chip(bus, true);
}

static int skl_probe(struct pci_dev *pci,
		     const struct pci_device_id *pci_id)
{
	struct skl *skl;
	struct hdac_bus *bus = NULL;
	const struct firmware __maybe_unused *nhlt_fw = NULL;
	int err;

	/* we use ext core ops, so provide NULL for ops here */
	err = skl_create(pci, NULL, &skl);
	if (err < 0)
		return err;

	bus = skl_to_bus(skl);

	err = skl_first_init(bus);
	if (err < 0)
		goto out_free;

	skl->pci_id = pci->device;

	device_disable_async_suspend(bus->dev);

#if !IS_ENABLED(CONFIG_SND_SOC_INTEL_CNL_FPGA)
	skl->nhlt_version = skl_get_nhlt_version(bus->dev);
	skl->nhlt = skl_nhlt_init(bus->dev);

	if (skl->nhlt == NULL) {
		err = -ENODEV;
		goto out_free;
	}

	err = skl_nhlt_create_sysfs(skl);
	if (err < 0)
		goto out_nhlt_free;

	skl_nhlt_update_topology_bin(skl);

#else
	if (request_firmware(&nhlt_fw, "intel/nhlt_blob.bin", bus->dev)) {
		dev_err(bus->dev, "Request nhlt fw failed, continuing..\n");
		goto nhlt_continue;
	}

	skl->nhlt = devm_kzalloc(&pci->dev, nhlt_fw->size, GFP_KERNEL);
	if (skl->nhlt == NULL)
		return -ENOMEM;
	memcpy(skl->nhlt, nhlt_fw->data, nhlt_fw->size);
	release_firmware(nhlt_fw);

nhlt_continue:
#endif
	pci_set_drvdata(skl->pci, bus);

#if !IS_ENABLED(CONFIG_SND_SOC_INTEL_CNL_FPGA)
	skl_dmic_data.dmic_num = skl_get_dmic_geo(skl);
#endif

	/* check if dsp is there */
	if (bus->ppcap) {
		/* create device for dsp clk */
		err = skl_clock_device_register(skl);
		if (err < 0)
			goto out_clk_free;

		err = skl_find_machine(skl, (void *)pci_id->driver_data);
		if (err < 0)
			goto out_nhlt_free;
		err = skl_init_dsp(skl);
		if (err < 0) {
			dev_dbg(bus->dev, "error failed to register dsp\n");
			goto out_nhlt_free;
		}
		skl->skl_sst->enable_miscbdcge = skl_enable_miscbdcge;
		skl->skl_sst->clock_power_gating = skl_clock_power_gating;
		skl->skl_sst->request_tplg = skl_request_tplg;
		skl->skl_sst->hda_irq_ack = skl_stream_update;
	}
	if (bus->mlcap)
		snd_hdac_ext_bus_get_ml_capabilities(bus);

	snd_hdac_bus_stop_chip(bus);

	/* create device for soc dmic */
	err = skl_dmic_device_register(skl);
	if (err < 0)
		goto out_dsp_free;

	skl_virt_device_register(skl);
	schedule_work(&skl->probe_work);

	return 0;

out_dsp_free:
	skl_free_dsp(skl);
out_clk_free:
	skl_clock_device_unregister(skl);
out_nhlt_free:
	skl_nhlt_free(skl->nhlt);
out_free:
	skl_free(bus);

	return err;
}

static void skl_shutdown(struct pci_dev *pci)
{
	struct hdac_bus *bus = pci_get_drvdata(pci);
	struct hdac_stream *s;
	struct hdac_ext_stream *stream;
	struct skl *skl;

	if (!bus)
		return;

	skl = bus_to_skl(bus);

	if (!skl->init_done)
		return;

	snd_hdac_ext_stop_streams(bus);
	snd_hdac_ext_bus_link_power_down_all(bus);
	skl_dsp_sleep(skl->skl_sst->dsp);
	/* While doing the warm reboot testing, some times dsp core is on
	 * when system goes to shutdown. When cores.usage_count is
	 * equal to zero then driver puts the dsp core to zero. On few
	 * warm reboots cores.usage_count is not equal to zero and dsp
	 * core is ON even system goes to shutdown. Force the dsp cores
	 * off without checking the usage_count.
	 */
	skl_dsp_disable_core(skl->skl_sst->dsp, SKL_DSP_CORE0_ID);

	list_for_each_entry(s, &bus->stream_list, list) {
		stream = stream_to_hdac_ext_stream(s);
		snd_hdac_ext_stream_decouple(bus, stream, false);
	}

	snd_hdac_bus_stop_chip(bus);
}

static void skl_remove(struct pci_dev *pci)
{
	struct hdac_bus *bus = pci_get_drvdata(pci);
	struct skl *skl = bus_to_skl(bus);

	skl_delete_notify_kctl_list(skl->skl_sst);
	release_firmware(skl->tplg);

	pm_runtime_get_noresume(&pci->dev);

	/* codec removal, invoke bus_device_remove */
	snd_hdac_ext_bus_device_remove(bus);

	skl->debugfs = NULL;
	skl_virt_device_unregister(skl);
	skl_platform_unregister(&pci->dev);
	skl_free_dsp(skl);
	skl_machine_device_unregister(skl);
	skl_dmic_device_unregister(skl);
	skl_clock_device_unregister(skl);
	skl_nhlt_remove_sysfs(skl);
	skl_nhlt_free(skl->nhlt);
	skl_free(bus);
	dev_set_drvdata(&pci->dev, NULL);
}

/* PCI IDs */
static const struct pci_device_id skl_ids[] = {
	/* Sunrise Point-LP */
	{ PCI_DEVICE(0x8086, 0x9d70),
		.driver_data = (unsigned long)&snd_soc_acpi_intel_skl_machines},
	/* BXT-P */
	{ PCI_DEVICE(0x8086, 0x5a98),
		.driver_data = (unsigned long)&snd_soc_acpi_intel_bxt_machines},
	/* KBL */
	{ PCI_DEVICE(0x8086, 0x9D71),
		.driver_data = (unsigned long)&snd_soc_acpi_intel_kbl_machines},
	/* GLK */
	{ PCI_DEVICE(0x8086, 0x3198),
		.driver_data = (unsigned long)&snd_soc_acpi_intel_glk_machines},
	/* CNL */
	{ PCI_DEVICE(0x8086, 0x9dc8),
		.driver_data = (unsigned long)&snd_soc_acpi_intel_cnl_machines},
	/* ICL */
	{ PCI_DEVICE(0x8086, 0x34c8),
		.driver_data = (unsigned long)&snd_soc_acpi_intel_icl_machines},
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, skl_ids);

/* pci_driver definition */
static struct pci_driver skl_driver = {
	.name = KBUILD_MODNAME,
	.id_table = skl_ids,
	.probe = skl_probe,
	.remove = skl_remove,
	.shutdown = skl_shutdown,
	.driver = {
		.pm = &skl_pm,
	},
};
module_pci_driver(skl_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("Intel Skylake ASoC HDA driver");
