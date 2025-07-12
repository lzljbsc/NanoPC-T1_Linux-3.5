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
#include <linux/delay.h>
#include <linux/gpio_keys.h>
#include <linux/i2c/pca954x.h>
#include <linux/spi/spi.h>
#include <linux/spi/spi_gpio.h>
#include <linux/spi/flash.h>
#include <media/gpio-ir-recv.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>

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
#include <plat/ehci.h>

#include <mach/map.h>
#include <mach/regs-pmu.h>

#ifdef CONFIG_EXYNOS4_DEV_DWMCI
#include <mach/dwmci.h>
#endif

#include <mach/ohci.h>

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

#ifdef CONFIG_S5P_DEV_USB_EHCI
/* USB EHCI */
static struct s5p_ehci_platdata smdk4x12_ehci_pdata;

static void __init smdk4x12_ehci_init(void)
{
    struct s5p_ehci_platdata *pdata = &smdk4x12_ehci_pdata;

    s5p_ehci_set_platdata(pdata);

#define GPIO_USBH_RESET                 EXYNOS4_GPM2(4)
    if (gpio_request_one(GPIO_USBH_RESET, GPIOF_OUT_INIT_HIGH, "USBH_RESET")) {
        pr_err("failed to request GPM2_4 for USB reset control\n");
        return;
    }

    s3c_gpio_setpull(GPIO_USBH_RESET, S3C_GPIO_PULL_UP);
    gpio_set_value(GPIO_USBH_RESET, 0);
    mdelay(1);
    gpio_set_value(GPIO_USBH_RESET, 1);
    //gpio_free(GPIO_USBH_RESET);
}
#endif

#ifdef CONFIG_EXYNOS4_DEV_USB_OHCI
/* USB OHCI */
static struct exynos4_ohci_platdata smdk4x12_ohci_pdata;

static void __init smdk4x12_ohci_init(void)
{
    struct exynos4_ohci_platdata *pdata = &smdk4x12_ohci_pdata;
    exynos4_ohci_set_platdata(pdata);
}
#endif

#ifdef CONFIG_I2C_MUX_PCA954x
static struct pca954x_platform_mode pca954x_modes[] __initdata = {
    {
        .adap_id = 10,
        .deselect_on_exit = 1,
    }, {
        .adap_id = 11,
        .deselect_on_exit = 1,
    }, {
        .adap_id = 12,
        .deselect_on_exit = 1,
    }, {
        .adap_id = 13,
        .deselect_on_exit = 1,
    }, {
        .adap_id = 14,
        .deselect_on_exit = 1,
    }, {
        .adap_id = 15,
        .deselect_on_exit = 1,
    }, {
        .adap_id = 16,
        .deselect_on_exit = 1,
    }, {
        .adap_id = 17,
        .deselect_on_exit = 1,
    },
};

static struct pca954x_platform_data pca954x_pdata __initdata = {
    .modes = pca954x_modes,
    .num_modes = ARRAY_SIZE(pca954x_modes),
};
#endif

static struct i2c_board_info nanopc_t1_i2c_devs0[] __initdata = {
#ifdef CONFIG_I2C_MUX_PCA954x
    {
        I2C_BOARD_INFO("pca9548", 0x03),
        .platform_data = &pca954x_pdata,
    },
#endif
#ifdef CONFIG_EEPROM_AT24
    {
        I2C_BOARD_INFO("24c512", 0x50),
        .platform_data = NULL,
    },
#endif
};

#ifdef CONFIG_SPI_GPIO
#ifdef CONFIG_MTD_M25P80
static struct mtd_partition nanopc_t1_spi_gpio_flash_partitions[] = {
    {
        .name = "nor-config",
        .size = 0x0100000,
        .offset = 0,
    },
    {
        .name = "nor-data",
        .size = MTDPART_SIZ_FULL,
        .offset = MTDPART_OFS_NXTBLK,
    },
};

static struct flash_platform_data nanopc_t1_spi_gpio_flash_data = {
    .name = "m25p80",
    .parts = nanopc_t1_spi_gpio_flash_partitions,
    .nr_parts = ARRAY_SIZE(nanopc_t1_spi_gpio_flash_partitions),
    .type = "w25q128",
};
#endif

static struct spi_board_info nanopc_t1_spi_gpio_board_info[] __initdata = {
#ifdef CONFIG_MTD_M25P80
/* GPX0_0   ->  hold
 * GPX0_3   ->  cs
 * GPX0_5   ->  wp
 * */
    {
        .modalias = "m25p80", 
        .max_speed_hz = 20000000,
        .bus_num = 4,
        .chip_select = 0,
        .controller_data = (void *)EXYNOS4_GPX0(3),
        .platform_data = &nanopc_t1_spi_gpio_flash_data,
        .mode = SPI_MODE_0,
    },
#endif
};
#endif

/*********************************** platform_device ***************************/
#if defined(CONFIG_KEYBOARD_GPIO) || defined(CONFIG_KEYBOARD_GPIO_POLLED)
static struct gpio_keys_button nanopc_t1_keys_button[] = {
    {
        .code              = KEY_ESC,
        .gpio              = EXYNOS4_GPX3(2),
        .desc              = "key-esc",
        .type              = EV_KEY,
        .debounce_interval = 100,
    }, {
        .code              = KEY_ENTER,
        .gpio              = EXYNOS4_GPX3(3),
        .desc              = "key-enter",
        .type              = EV_KEY,
        .debounce_interval = 100,
    },
};

static struct gpio_keys_platform_data nanopc_t1_keys_data = {
    .name          = "nanopc_t1-key",
    .poll_interval = 20,
    .buttons       = nanopc_t1_keys_button,
    .nbuttons      = ARRAY_SIZE(nanopc_t1_keys_button),
};

static struct platform_device nanopc_t1_device_keys = {
#ifdef CONFIG_KEYBOARD_GPIO
    .name = "gpio-keys",
#elif CONFIG_KEYBOARD_GPIO_POLLED
    .name = "gpio-keys-polled",
#endif
    .dev = {
        .platform_data = &nanopc_t1_keys_data,
    },
};
#endif

#ifdef CONFIG_IR_GPIO_CIR
static struct gpio_ir_recv_platform_data nanopc_t1_ir_data = {
    .gpio_nr    = EXYNOS4_GPX2(7),
    .active_low = 1,
};

static struct platform_device nanopc_t1_device_ir = {
    .name = "gpio-rc-recv",
    .dev  = {
        .platform_data = &nanopc_t1_ir_data,
    },
};
#endif

#ifdef CONFIG_SPI_GPIO
/* GPX0_1   ->  clk
 * GPX0_2   ->  MOSI
 * GPX0_4   ->  MISO
 * */
static struct spi_gpio_platform_data nanopc_t1_spi_gpio_data = {
	.sck    = EXYNOS4_GPX0(1),
	.mosi   = EXYNOS4_GPX0(2),
	.miso   = EXYNOS4_GPX0(4),
	.num_chipselect = 1,
};

static struct platform_device nanopc_t1_device_spi_gpio = {
    .name   = "spi_gpio",
    .id     = 4,
    .dev    = {
        .platform_data = &nanopc_t1_spi_gpio_data,
    },
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

#ifdef CONFIG_S5P_DEV_USB_EHCI
    &s5p_device_ehci,
#endif

#ifdef CONFIG_EXYNOS4_DEV_USB_OHCI
    &exynos4_device_ohci,
#endif

    &s3c_device_i2c0,

#ifdef CONFIG_S3C_DEV_I2C1
    &s3c_device_i2c1,
#endif

#ifdef CONFIG_S3C_DEV_I2C3
    &s3c_device_i2c3,
#endif

#ifdef CONFIG_S3C_DEV_I2C7
    &s3c_device_i2c7,
#endif

#if defined(CONFIG_KEYBOARD_GPIO) || defined(CONFIG_KEYBOARD_GPIO_POLLED)
    &nanopc_t1_device_keys,
#endif

#ifdef CONFIG_IR_GPIO_CIR
    &nanopc_t1_device_ir,
#endif

#ifdef CONFIG_SPI_GPIO
    &nanopc_t1_device_spi_gpio,
#endif
};
/**************************** end of platform_device ***************************/

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

#ifdef CONFIG_SPI_GPIO
#ifdef CONFIG_MTD_M25P80
static void __init nanopc_t1_spi_flash_init(void)
{
    gpio_request_one(EXYNOS4_GPX0(0), GPIOF_OUT_INIT_HIGH, "flash-hold");
    s3c_gpio_setpull(EXYNOS4_GPX0(0), S3C_GPIO_PULL_NONE);
    gpio_set_value(EXYNOS4_GPX0(0), 0);
    mdelay(5);
    gpio_set_value(EXYNOS4_GPX0(0), 1);

    gpio_request_one(EXYNOS4_GPX0(5), GPIOF_OUT_INIT_HIGH, "flash-wp");
    s3c_gpio_setpull(EXYNOS4_GPX0(5), S3C_GPIO_PULL_NONE);
    gpio_set_value(EXYNOS4_GPX0(5), 1);
}
#endif
#endif

static void __init smdk4x12_init_machine(void)
{
    exynos_bootdev_init();
    nanopc_t1_hwrev_init();

    smdk4x12_pmu_wdt_init();

#ifdef CONFIG_SPI_GPIO
#ifdef CONFIG_MTD_M25P80
    nanopc_t1_spi_flash_init();
#endif
#endif

    gpio_led_register_device(-1, &nanopc_t1_leds_data);

#ifdef CONFIG_EXYNOS4_DEV_DWMCI
    exynos4_dwmci_set_platdata(&exynos_dwmci_pdata);
#endif

#ifdef CONFIG_S3C_DEV_HSMMC2
    s3c_sdhci2_set_platdata(&smdk4x12_hsmmc2_pdata);
#endif

#ifdef CONFIG_S5P_DEV_USB_EHCI
    smdk4x12_ehci_init();
#endif

#ifdef CONFIG_EXYNOS4_DEV_USB_OHCI
    smdk4x12_ohci_init();
#endif

    s3c_i2c0_set_platdata(NULL);
    i2c_register_board_info(0, nanopc_t1_i2c_devs0,
                            ARRAY_SIZE(nanopc_t1_i2c_devs0));

#ifdef CONFIG_SPI_GPIO
    spi_register_board_info(nanopc_t1_spi_gpio_board_info,
                            ARRAY_SIZE(nanopc_t1_spi_gpio_board_info));
#endif

#ifdef CONFIG_S3C_DEV_I2C1
    s3c_i2c1_set_platdata(NULL);
#endif

#ifdef CONFIG_S3C_DEV_I2C3
    s3c_i2c3_set_platdata(NULL);
#endif

#ifdef CONFIG_S3C_DEV_I2C7
    s3c_i2c7_set_platdata(NULL);
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

