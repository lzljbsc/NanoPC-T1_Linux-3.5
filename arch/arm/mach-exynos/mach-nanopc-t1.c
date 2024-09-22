/*
 * linux/arch/arm/mach-exynos4/mach-nanopc-t1.c
 *
 * Copyright (c) 2011 Samsung Electronics Co., Ltd.
 *		http://www.samsung.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <linux/gpio.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/io.h>
#include <linux/mfd/max8997.h>
#include <linux/mmc/host.h>
#include <linux/platform_device.h>
#include <linux/pwm_backlight.h>
#include <linux/regulator/machine.h>
#include <linux/serial_core.h>
#include <linux/clk.h>
#include <linux/leds.h>

#include <asm/mach/arch.h>
#include <asm/hardware/gic.h>
#include <asm/mach-types.h>

#include <plat/backlight.h>
#include <plat/clock.h>
#include <plat/cpu.h>
#include <plat/devs.h>
#include <plat/gpio-cfg.h>
#include <plat/iic.h>
#include <plat/keypad.h>
#include <plat/mfc.h>
#include <plat/regs-serial.h>
#include <plat/sdhci.h>

#ifdef CONFIG_EXYNOS4_DEV_DWMCI
#include <mach/dwmci.h>
#endif

#include <mach/map.h>
#include <mach/regs-pmu.h>

#include "common.h"

/* boot devices */
static int exynos_boot_dev = 0;
#define is_bootfrom_emmc()  ((0x6 == exynos_boot_dev) || (0x7 == exynos_boot_dev))
#define is_bootfrom_sd()    ((0x3 == exynos_boot_dev))

/* board HW revision */
static int nanopc_t1_hw_rev = 0;

/* Following are default values for UCON, ULCON and UFCON UART registers */
#define SMDK4X12_UCON_DEFAULT	(S3C2410_UCON_TXILEVEL |	\
				 S3C2410_UCON_RXILEVEL |	\
				 S3C2410_UCON_TXIRQMODE |	\
				 S3C2410_UCON_RXIRQMODE |	\
				 S3C2410_UCON_RXFIFO_TOI |	\
				 S3C2443_UCON_RXERR_IRQEN)

#define SMDK4X12_ULCON_DEFAULT	(S3C2410_LCON_CS8)

#define SMDK4X12_UFCON_DEFAULT	(S3C2410_UFCON_FIFOMODE |	\
				 S5PV210_UFCON_TXTRIG4 |	\
				 S5PV210_UFCON_RXTRIG4)

static struct s3c2410_uartcfg smdk4x12_uartcfgs[] __initdata = {
	[0] = {
		.hwport		= 0,
		.flags		= 0,
		.ucon		= SMDK4X12_UCON_DEFAULT,
		.ulcon		= SMDK4X12_ULCON_DEFAULT,
		.ufcon		= SMDK4X12_UFCON_DEFAULT,
	},
	[1] = {
		.hwport		= 1,
		.flags		= 0,
		.ucon		= SMDK4X12_UCON_DEFAULT,
		.ulcon		= SMDK4X12_ULCON_DEFAULT,
		.ufcon		= SMDK4X12_UFCON_DEFAULT,
	},
	[2] = {
		.hwport		= 2,
		.flags		= 0,
		.ucon		= SMDK4X12_UCON_DEFAULT,
		.ulcon		= SMDK4X12_ULCON_DEFAULT,
		.ufcon		= SMDK4X12_UFCON_DEFAULT,
	},
	[3] = {
		.hwport		= 3,
		.flags		= 0,
		.ucon		= SMDK4X12_UCON_DEFAULT,
		.ulcon		= SMDK4X12_ULCON_DEFAULT,
		.ufcon		= SMDK4X12_UFCON_DEFAULT,
	},
};


static void __init smdk4x12_fixup(struct tag *tags, char **cmdline, struct meminfo *mi)
{
}

static void __init smdk4x12_reserve(void)
{
    // TODO: mfc 相关功能临时屏蔽
	//s5p_mfc_reserve_mem(0x43000000, 8 << 20, 0x51000000, 8 << 20);
}

static void __init smdk4x12_map_io(void)
{
	exynos_init_io(NULL, 0);
}

static void __init smdk4x12_init_early(void)
{
    clk_xusbxti.rate = 24000000;

    s3c24xx_init_clocks(clk_xusbxti.rate);
    s3c24xx_init_uarts(smdk4x12_uartcfgs, ARRAY_SIZE(smdk4x12_uartcfgs));
}

#ifdef CONFIG_EXYNOS4_DEV_DWMCI
static void exynos_dwmci_cfg_gpio(int width)
{
    unsigned int gpio;

    for (gpio = EXYNOS4_GPK0(0); gpio < EXYNOS4_GPK0(2); gpio++) {
        s3c_gpio_cfgpin(gpio, S3C_GPIO_SFN(3));
        s3c_gpio_setpull(gpio, S3C_GPIO_PULL_NONE);
        s5p_gpio_set_drvstr(gpio, S5P_GPIO_DRVSTR_LV2);
    }

    switch (width) {
        case MMC_BUS_WIDTH_8:
            for (gpio = EXYNOS4_GPK1(3); gpio <= EXYNOS4_GPK1(6); gpio++) {
                s3c_gpio_cfgpin(gpio, S3C_GPIO_SFN(4));
                s3c_gpio_setpull(gpio, S3C_GPIO_PULL_UP);
                s5p_gpio_set_drvstr(gpio, S5P_GPIO_DRVSTR_LV2);
            }
        case MMC_BUS_WIDTH_4:
            for (gpio = EXYNOS4_GPK0(3); gpio <= EXYNOS4_GPK0(6); gpio++) {
                s3c_gpio_cfgpin(gpio, S3C_GPIO_SFN(3));
                s3c_gpio_setpull(gpio, S3C_GPIO_PULL_UP);
                s5p_gpio_set_drvstr(gpio, S5P_GPIO_DRVSTR_LV2);
            }
            break;
        case MMC_BUS_WIDTH_1:
            gpio = EXYNOS4_GPK0(3);
            s3c_gpio_cfgpin(gpio, S3C_GPIO_SFN(3));
            s3c_gpio_setpull(gpio, S3C_GPIO_PULL_UP);
            s5p_gpio_set_drvstr(gpio, S5P_GPIO_DRVSTR_LV2);
        default:
            break;
    }
}

static int exynos4_dwmci_init(u32 slot_id, irq_handler_t handler, void *data)
{
    struct dw_mci *host = (struct dw_mci *)data;

    host->hclk = clk_get(&host->dev, "dwmmc");
    if (IS_ERR(host->hclk)) {
        dev_err(&host->dev, "failed to get hclk\n");
        return -ENODEV;
    }
    clk_enable(host->hclk);

    host->cclk = clk_get(&host->dev, "sclk_dwmmc");
    if (IS_ERR(host->cclk)) {
        dev_err(&host->dev, "failed to get cclk\n");
        clk_disable(host->hclk);
        clk_put(host->hclk);
        return -ENODEV;
    }
    clk_enable(host->cclk);

    host->pdata->bus_hz = 66 * 1000 * 1000;

    exynos_dwmci_cfg_gpio(MMC_BUS_WIDTH_8);

	return 0;
}

#define DWMCI_CLKSEL    0x09c
static void exynos4_dwmci_set_io_timing(void *data, unsigned char timing)
{
    struct dw_mci *host = (struct dw_mci *)data;
    u32 clksel = 0;
    u32 clkdrv = 0;
    u32 ddr_timing = 0;
    u32 sdr_timing = 0;

    /* Set Phase Shift Register */
    if (soc_is_exynos4212() || soc_is_exynos4412()) {
        ddr_timing = 0x00010001;
        sdr_timing = 0x00010001;
    }

    if (timing == MMC_TIMING_MMC_HS200 || timing == MMC_TIMING_UHS_SDR104) {
        if(host->bus_hz != 200 * 1000 * 1000) {
            host->bus_hz = 200 * 1000 * 1000;
            clk_set_rate(host->cclk, 800 * 1000 * 1000);
        }
        clksel = __raw_readl(host->regs + DWMCI_CLKSEL);
        clksel = (clksel & 0xfff8ffff) | (clkdrv << 16);
        __raw_writel(clksel, host->regs + DWMCI_CLKSEL);
    } else if (timing == MMC_TIMING_UHS_DDR50) {
        if (host->bus_hz != 100 * 1000 * 1000) {
            host->bus_hz = 100 * 1000 * 1000;
            clk_set_rate(host->cclk, 400 * 1000 * 1000);
            host->current_speed = 0;
        }
        __raw_writel(ddr_timing, host->regs + DWMCI_CLKSEL);
    } else {
        if (host->bus_hz != 50 * 1000 * 1000) {
            host->bus_hz = 50 * 1000 * 1000;
            clk_set_rate(host->cclk, 200 * 1000 * 1000);
        }
        __raw_writel(sdr_timing, host->regs + DWMCI_CLKSEL);
    }
}

static struct dw_mci_board exynos_dwmci_pdata __initdata = {
    .num_slots      = 1,
    .quirks         = DW_MCI_QUIRK_BROKEN_CARD_DETECTION | DW_MCI_QUIRK_HIGHSPEED,
    .bus_hz         = 100 * 1000 * 1000,
    .caps           = MMC_CAP_UHS_DDR50 | MMC_CAP_1_8V_DDR | MMC_CAP_8_BIT_DATA | MMC_CAP_CMD23,
    .fifo_depth     = 0x80,
    .detect_delay_ms= 200,
    .init           = exynos4_dwmci_init,
    .set_io_timing  = exynos4_dwmci_set_io_timing,
};
#endif

static const struct gpio_led nanopc_t1_leds[] __initconst = {
    {
        .name               = "sys",
        .default_trigger    = "heartbeat",
        .gpio               = EXYNOS4_GPM4(0),
        .active_low         = 1,
        .default_state      = LEDS_GPIO_DEFSTATE_OFF,
    },
    {
        .name               = "mmcblk0",
        .default_trigger    = "mmc1",
        .gpio               = EXYNOS4_GPM4(1),
        .active_low         = 1,
        .default_state      = LEDS_GPIO_DEFSTATE_OFF,
    },
};

static const struct gpio_led_platform_data nanopc_t1_leds_data __initconst = {
    .num_leds           = ARRAY_SIZE(nanopc_t1_leds),
    .leds               = nanopc_t1_leds,
};

#ifdef CONFIG_S3C_DEV_HSMMC2
static struct s3c_sdhci_platdata smdk4x12_hsmmc2_pdata __initdata = {
    .cd_type        = S3C_SDHCI_CD_INTERNAL,
};
#endif

static struct platform_device *smdk4x12_devices[] __initdata = {
#ifdef CONFIG_EXYNOS4_DEV_DWMCI
    &exynos4_device_dwmci,
#endif

#ifdef CONFIG_S3C_DEV_HSMMC2
    &s3c_device_hsmmc2,
#endif

#ifdef CONFIG_S3C_DEV_RTC
    &s3c_device_rtc,
#endif

#ifdef CONFIG_S3C_DEV_WDT 
    &s3c_device_wdt,
#endif
};

static void __init exynos_bootdev_init(void)
{
    /* 启动设备，已经在 uboot中检测，并存放在了 INFORM3 寄存器中 */
    exynos_boot_dev = __raw_readl(S5P_INFORM3);

    if (is_bootfrom_emmc()) {
#ifdef CONFIG_EXYNOS4_DEV_DWMCI
        printk(KERN_INFO "Boot from emmc\n");
        exynos_dwmci_pdata.caps2 |= MMC_CAP2_BOOT_DEVICE;
#endif
    } else if (is_bootfrom_sd()) {
#ifdef CONFIG_S3C_DEV_HSMMC2
        printk(KERN_INFO "Boot from sd\n");
        smdk4x12_hsmmc2_pdata.host_caps2 |= MMC_CAP2_BOOT_DEVICE;
#endif
    } else {
        /* oops...should never fly to here */
        printk(KERN_ERR "Unknown boot device\n");
        while (1);
    }
}

static void __init nanopc_t1_hwrev_init(void)
{
    struct gpio hw_rev_gpios[] = {
        { EXYNOS4_GPM3(5), GPIOF_IN, "hw_rev0" },
        { EXYNOS4_GPM3(6), GPIOF_IN, "hw_rev1" },
        { EXYNOS4_GPM3(7), GPIOF_IN, "hw_rev2" },
    };

    int i, ret;

    ret = gpio_request_array(hw_rev_gpios, ARRAY_SIZE(hw_rev_gpios));
    BUG_ON(ret);

    for (i = 0; i < ARRAY_SIZE(hw_rev_gpios); i++)
        nanopc_t1_hw_rev |= gpio_get_value(hw_rev_gpios[i].gpio) << i;

    printk("NanoPC T1 HW revision: %d\n", nanopc_t1_hw_rev);
}

static void smdk4x12_pmu_wdt_init(void)
{
    unsigned int value = 0;

    if (soc_is_exynos4212() || soc_is_exynos4412()) {
        value = __raw_readl(S5P_AUTOMATIC_WDT_RESET_DISABLE);
        value &= ~S5P_SYS_WDTRESET;
        __raw_writel(value, S5P_AUTOMATIC_WDT_RESET_DISABLE);
        value = __raw_readl(S5P_MASK_WDT_RESET_REQUEST);
        value &= ~S5P_SYS_WDTRESET;
        __raw_writel(value, S5P_MASK_WDT_RESET_REQUEST);
    }
}

static void __init smdk4x12_init_machine(void)
{
    exynos_bootdev_init();
    nanopc_t1_hwrev_init();

    smdk4x12_pmu_wdt_init();

    gpio_led_register_device(-1, &nanopc_t1_leds_data);

#ifdef CONFIG_EXYNOS4_DEV_DWMCI
    exynos4_dwmci_set_platdata(&exynos_dwmci_pdata);
#endif

#ifdef CONFIG_S3C_DEV_HSMMC2
    s3c_sdhci2_set_platdata(&smdk4x12_hsmmc2_pdata);
#endif

    platform_add_devices(smdk4x12_devices, ARRAY_SIZE(smdk4x12_devices));

    return;
}

MACHINE_START(NANOPC_T1, "NANOPC-T1")
    /* Maintainer: Allan */
    /* Maintainer: Kukjin Kim <kgene.kim@samsung.com> */
    /* Maintainer: Changhwan Youn <chaos.youn@samsung.com> */
    .atag_offset	= 0x100,
    .fixup          = smdk4x12_fixup,
    .reserve	    = smdk4x12_reserve,
    .map_io		    = smdk4x12_map_io,
    .init_early     = smdk4x12_init_early,
    .init_irq	    = exynos4_init_irq,
    .timer		    = &exynos4_timer,
    .init_machine	= smdk4x12_init_machine,
    .init_late	    = exynos_init_late,
#ifdef CONFIG_MULTI_IRQ_HANDLER
    .handle_irq	    = gic_handle_irq,
#endif
    .restart	    = exynos4_restart,
MACHINE_END

