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

#include <mach/map.h>

#include "common.h"


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

static void __init smdk4x12_init_machine(void)
{
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

