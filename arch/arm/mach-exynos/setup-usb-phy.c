/*
 * Copyright (C) 2011 Samsung Electronics Co.Ltd
 * Author: Joonyoung Shim <jy0922.shim@samsung.com>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 */

#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/io.h>
#include <linux/platform_device.h>
#include <mach/regs-pmu.h>
#include <mach/regs-usb-phy.h>
#include <plat/cpu.h>
#include <plat/usb-phy.h>

/* USB PHY Mode Selection */
#define EXYNOS4x12_USB_CFG              (S3C_VA_SYS + 0x21C)

static atomic_t host_usage;

static int exynos4_usb_host_phy_is_on(void)
{
	return (readl(EXYNOS4_PHYPWR) & PHY1_STD_ANALOG_POWERDOWN) ? 0 : 1;
}

static void exynos4210_usb_phy_clkset(struct platform_device *pdev)
{
	struct clk *xusbxti_clk;
	u32 phyclk;

	/* set clock frequency for PLL */
	phyclk = readl(EXYNOS4_PHYCLK) & ~CLKSEL_MASK;

	xusbxti_clk = clk_get(&pdev->dev, "xusbxti");
	if (xusbxti_clk && !IS_ERR(xusbxti_clk)) {
		switch (clk_get_rate(xusbxti_clk)) {
		case 12 * MHZ:
			phyclk |= CLKSEL_12M;
			break;
		case 24 * MHZ:
			phyclk |= CLKSEL_24M;
			break;
		default:
		case 48 * MHZ:
			/* default reference clock */
			break;
		}
		clk_put(xusbxti_clk);
	}

	writel(phyclk, EXYNOS4_PHYCLK);
}

static int exynos4210_usb_phy0_init(struct platform_device *pdev)
{
	u32 rstcon;

	writel(readl(S5P_USBDEVICE_PHY_CONTROL) | S5P_USBDEVICE_PHY_ENABLE,
			S5P_USBDEVICE_PHY_CONTROL);

	exynos4210_usb_phy_clkset(pdev);

	/* set to normal PHY0 */
	writel((readl(EXYNOS4_PHYPWR) & ~PHY0_NORMAL_MASK), EXYNOS4_PHYPWR);

	/* reset PHY0 and Link */
	rstcon = readl(EXYNOS4_RSTCON) | PHY0_SWRST_MASK;
	writel(rstcon, EXYNOS4_RSTCON);
	udelay(10);

	rstcon &= ~PHY0_SWRST_MASK;
	writel(rstcon, EXYNOS4_RSTCON);

	return 0;
}

static int exynos4210_usb_phy0_exit(struct platform_device *pdev)
{
	writel((readl(EXYNOS4_PHYPWR) | PHY0_ANALOG_POWERDOWN |
				PHY0_OTG_DISABLE), EXYNOS4_PHYPWR);

	writel(readl(S5P_USBDEVICE_PHY_CONTROL) & ~S5P_USBDEVICE_PHY_ENABLE,
			S5P_USBDEVICE_PHY_CONTROL);

	return 0;
}

static int exynos4210_usb_phy1_init(struct platform_device *pdev)
{
	struct clk *otg_clk;
	u32 rstcon;
	int err;

	atomic_inc(&host_usage);

	otg_clk = clk_get(&pdev->dev, "otg");
	if (IS_ERR(otg_clk)) {
		dev_err(&pdev->dev, "Failed to get otg clock\n");
		return PTR_ERR(otg_clk);
	}

	err = clk_enable(otg_clk);
	if (err) {
		clk_put(otg_clk);
		return err;
	}

	if (exynos4_usb_host_phy_is_on())
		return 0;

	writel(readl(S5P_USBHOST_PHY_CONTROL) | S5P_USBHOST_PHY_ENABLE,
			S5P_USBHOST_PHY_CONTROL);

	exynos4210_usb_phy_clkset(pdev);

	/* floating prevention logic: disable */
	writel((readl(EXYNOS4_PHY1CON) | FPENABLEN), EXYNOS4_PHY1CON);

	/* set to normal HSIC 0 and 1 of PHY1 */
	writel((readl(EXYNOS4_PHYPWR) & ~PHY1_HSIC_NORMAL_MASK),
			EXYNOS4_PHYPWR);

	/* set to normal standard USB of PHY1 */
	writel((readl(EXYNOS4_PHYPWR) & ~PHY1_STD_NORMAL_MASK), EXYNOS4_PHYPWR);

	/* reset all ports of both PHY and Link */
	rstcon = readl(EXYNOS4_RSTCON) | HOST_LINK_PORT_SWRST_MASK |
		PHY1_SWRST_MASK;
	writel(rstcon, EXYNOS4_RSTCON);
	udelay(10);

	rstcon &= ~(HOST_LINK_PORT_SWRST_MASK | PHY1_SWRST_MASK);
	writel(rstcon, EXYNOS4_RSTCON);
	udelay(80);

	clk_disable(otg_clk);
	clk_put(otg_clk);

	return 0;
}

static int exynos4210_usb_phy1_exit(struct platform_device *pdev)
{
	struct clk *otg_clk;
	int err;

	if (atomic_dec_return(&host_usage) > 0)
		return 0;

	otg_clk = clk_get(&pdev->dev, "otg");
	if (IS_ERR(otg_clk)) {
		dev_err(&pdev->dev, "Failed to get otg clock\n");
		return PTR_ERR(otg_clk);
	}

	err = clk_enable(otg_clk);
	if (err) {
		clk_put(otg_clk);
		return err;
	}

	writel((readl(EXYNOS4_PHYPWR) | PHY1_STD_ANALOG_POWERDOWN),
			EXYNOS4_PHYPWR);

	writel(readl(S5P_USBHOST_PHY_CONTROL) & ~S5P_USBHOST_PHY_ENABLE,
			S5P_USBHOST_PHY_CONTROL);

	clk_disable(otg_clk);
	clk_put(otg_clk);

	return 0;
}

/* exynos4212  exynos4412 */
static void exynos4x12_usb_phy_clkset(struct platform_device *pdev)
{
	struct clk *xusbxti_clk;
	u32 phyclk;

	/* set clock frequency for PLL */
	phyclk = readl(EXYNOS4_PHYCLK) & ~EXYNOS4X12_CLKSEL_MASK;

	xusbxti_clk = clk_get(&pdev->dev, "xusbxti");
	if (xusbxti_clk && !IS_ERR(xusbxti_clk)) {
		switch (clk_get_rate(xusbxti_clk)) {
        case 9600 * 1000: //KHZ:
            phyclk |= EXYNOS4X12_CLKSEL_9600K;
            break;
		case 10 * MHZ:
			phyclk |= EXYNOS4X12_CLKSEL_10M;
			break;
		case 12 * MHZ:
			phyclk |= EXYNOS4X12_CLKSEL_12M;
			break;
        case 19200 * 1000: //KHZ:
            phyclk |= EXYNOS4X12_CLKSEL_19200K;
            break;
		case 20 * MHZ:
			phyclk |= EXYNOS4X12_CLKSEL_20M;
			break;
        default:
		case 24 * MHZ:
			/* default reference clock */
			phyclk |= EXYNOS4X12_CLKSEL_24M;
			break;
		}
		clk_put(xusbxti_clk);
	}

	writel(phyclk, EXYNOS4_PHYCLK);
}

static int exynos4x12_usb_phy0_init(struct platform_device *pdev)
{
    struct clk *otg_clk;
	u32 rstcon;
	int err;

	atomic_inc(&host_usage);

	otg_clk = clk_get(&pdev->dev, "otg");
	if (IS_ERR(otg_clk)) {
		dev_err(&pdev->dev, "Failed to get otg clock\n");
		return PTR_ERR(otg_clk);
	}

	err = clk_enable(otg_clk);
	if (err) {
		clk_put(otg_clk);
		return err;
	}

    if (exynos4_usb_host_phy_is_on()) {
        dev_info(&pdev->dev, "PHY already ON\n");
		return 0;
    }

    /* set XuhostOVERCUR to in-active by controlling ET6PUD[15:14]
     * 0x0 : pull-up/down disabled
     * 0x1 : pull-down enabled
     * 0x2 : reserved
     * 0x3 : pull-up enabled
     * */
    //void __iomem *gpio2_base;
    //gpio2_base = ioremap(EXYNOS4_PA_GPIO2, SZ_4K);
    //writel((__raw_readl(gpio2_base + ETC6PUD) & ~(0x3 << 14)) | (0x3 << 14),
            //gpio2_base + ETC6PUD);

    /* USB PHY */
    writel(S5P_USB_PHY_ENABLE, S5P_USB_PHY_CONTROL);

    /* HSIC 1 PHY */
    writel(S5P_HSIC_1_PHY_ENABLE, S5P_HSIC_1_PHY_CONTROL);
    /* HSIC 2 PHY */
    writel(S5P_HSIC_2_PHY_ENABLE, S5P_HSIC_2_PHY_CONTROL);

    /* USB MUX change from Host to Device  */
    writel(S5P_USB_PHY_DEVICE, EXYNOS4x12_USB_CFG);
    dev_dbg(&pdev->dev, "Change USB MUX to Device");

	exynos4x12_usb_phy_clkset(pdev);

    /* COMMON Block configuration during suspend */
    writel((readl(EXYNOS4_PHYCLK) & ~(PHY0_COMMON_ON_N | PHY1_COMMON_ON_N)), EXYNOS4_PHYCLK);

	/* floating prevention logic: disable */
	writel((readl(EXYNOS4_PHY1CON) | FPENABLEN), EXYNOS4_PHY1CON);

    /* set to normal of Device */
    writel(readl(EXYNOS4_PHYPWR) & ~PHY0_NORMAL_MASK, EXYNOS4_PHYPWR);

	/* set to normal standard USB of PHY1 */
	writel((readl(EXYNOS4_PHYPWR) & ~PHY1_STD_NORMAL_MASK), EXYNOS4_PHYPWR);

	/* set to normal HSIC 0 and 1 of PHY1 */
	writel((readl(EXYNOS4_PHYPWR) & ~(EXYNOS4X12_HSIC0_NORMAL_MASK | EXYNOS4X12_HSIC1_NORMAL_MASK)),
			EXYNOS4_PHYPWR);

	/* reset all ports of both PHY and Link */
	rstcon = readl(EXYNOS4_RSTCON) | PHY0_SWRST_MASK;
	writel(rstcon, EXYNOS4_RSTCON);
	udelay(10);
	rstcon &= ~PHY0_SWRST_MASK;
	writel(rstcon, EXYNOS4_RSTCON);
    udelay(80);

    rstcon = readl(EXYNOS4_RSTCON) | EXYNOS4X12_HOST_LINK_PORT_SWRST_MASK | EXYNOS4X12_PHY1_SWRST_MASK;
    writel(rstcon, EXYNOS4_RSTCON);
	udelay(10);
    rstcon &= ~(EXYNOS4X12_HOST_LINK_PORT_SWRST_MASK | EXYNOS4X12_PHY1_SWRST_MASK);
    writel(rstcon, EXYNOS4_RSTCON);
    udelay(80);

	clk_disable(otg_clk);
	clk_put(otg_clk);

	return 0;
}

static int exynos4x12_usb_phy0_exit(struct platform_device *pdev)
{
	struct clk *otg_clk;
	int err;

    if (atomic_dec_return(&host_usage) > 0) {
        dev_info(&pdev->dev, "still being used\n");
		return 0;
    }

	otg_clk = clk_get(&pdev->dev, "otg");
	if (IS_ERR(otg_clk)) {
		dev_err(&pdev->dev, "Failed to get otg clock\n");
		return PTR_ERR(otg_clk);
	}

	err = clk_enable(otg_clk);
	if (err) {
		clk_put(otg_clk);
		return err;
	}

    /* unset to normal of Device */
    writel((readl(EXYNOS4_PHYPWR) | PHY0_NORMAL_MASK), EXYNOS4_PHYPWR);

    /* unset to normal of Host */
    writel(readl(EXYNOS4_PHYPWR) | PHY1_STD_NORMAL_MASK | EXYNOS4X12_HSIC0_NORMAL_MASK | EXYNOS4X12_HSIC1_NORMAL_MASK, EXYNOS4_PHYPWR);
    writel((readl(EXYNOS4_PHYPWR) | PHY1_STD_ANALOG_POWERDOWN), EXYNOS4_PHYPWR);

    writel(readl(S5P_USB_PHY_CONTROL) & ~S5P_USB_PHY_ENABLE, S5P_USB_PHY_CONTROL);
    writel(readl(S5P_HSIC_1_PHY_CONTROL) & (~S5P_HSIC_1_PHY_ENABLE), S5P_HSIC_1_PHY_CONTROL);
    writel(readl(S5P_HSIC_2_PHY_CONTROL) & (~S5P_HSIC_2_PHY_ENABLE), S5P_HSIC_2_PHY_CONTROL);

    /* USB MUX change from Device to Host */
    writel(S5P_USB_PHY_HOST, EXYNOS4x12_USB_CFG);
    dev_dbg(&pdev->dev, "Change USB MUX to Host");

	clk_disable(otg_clk);
	clk_put(otg_clk);

	return 0;
}

static int exynos4x12_usb_phy1_init(struct platform_device *pdev)
{
	struct clk *otg_clk;
	u32 rstcon;
	int err;

	atomic_inc(&host_usage);

	otg_clk = clk_get(&pdev->dev, "otg");
	if (IS_ERR(otg_clk)) {
		dev_err(&pdev->dev, "Failed to get otg clock\n");
		return PTR_ERR(otg_clk);
	}

	err = clk_enable(otg_clk);
	if (err) {
		clk_put(otg_clk);
		return err;
	}

    if (exynos4_usb_host_phy_is_on()) {
        dev_info(&pdev->dev, "PHY already ON\n");
		return 0;
    }

    /* set XuhostOVERCUR to in-active by controlling ET6PUD[15:14]
     * 0x0 : pull-up/down disabled
     * 0x1 : pull-down enabled
     * 0x2 : reserved
     * 0x3 : pull-up enabled
     * */
    //void __iomem *gpio2_base;
    //gpio2_base = ioremap(EXYNOS4_PA_GPIO2, SZ_4K);
    //writel((__raw_readl(gpio2_base + ETC6PUD) & ~(0x3 << 14)) | (0x3 << 14),
            //gpio2_base + ETC6PUD);

    /* USB PHY */
    writel(S5P_USB_PHY_ENABLE, S5P_USB_PHY_CONTROL);

    /* HSIC 1 PHY */
    writel(S5P_HSIC_1_PHY_ENABLE, S5P_HSIC_1_PHY_CONTROL);
    /* HSIC 2 PHY */
    writel(S5P_HSIC_2_PHY_ENABLE, S5P_HSIC_2_PHY_CONTROL);

    /* USB MUX change from Device to Host */
    writel(S5P_USB_PHY_HOST, EXYNOS4x12_USB_CFG);
    dev_dbg(&pdev->dev, "Change USB MUX to Host");

	exynos4x12_usb_phy_clkset(pdev);

    /* COMMON Block configuration during suspend */
    writel((readl(EXYNOS4_PHYCLK) & ~(PHY0_COMMON_ON_N | PHY1_COMMON_ON_N)), EXYNOS4_PHYCLK);

	/* floating prevention logic: disable */
	writel((readl(EXYNOS4_PHY1CON) | FPENABLEN), EXYNOS4_PHY1CON);

    /* set to normal of Device */
    writel(readl(EXYNOS4_PHYPWR) & ~PHY0_NORMAL_MASK, EXYNOS4_PHYPWR);

	/* set to normal standard USB of PHY1 */
	writel((readl(EXYNOS4_PHYPWR) & ~PHY1_STD_NORMAL_MASK), EXYNOS4_PHYPWR);

	/* set to normal HSIC 0 and 1 of PHY1 */
	writel((readl(EXYNOS4_PHYPWR) & ~(EXYNOS4X12_HSIC0_NORMAL_MASK | EXYNOS4X12_HSIC1_NORMAL_MASK)),
			EXYNOS4_PHYPWR);

	/* reset all ports of both PHY and Link */
	rstcon = readl(EXYNOS4_RSTCON) | PHY0_SWRST_MASK;
	writel(rstcon, EXYNOS4_RSTCON);
	udelay(10);
	rstcon &= ~PHY0_SWRST_MASK;
	writel(rstcon, EXYNOS4_RSTCON);
    udelay(80);

    rstcon = readl(EXYNOS4_RSTCON) | EXYNOS4X12_HOST_LINK_PORT_SWRST_MASK | EXYNOS4X12_PHY1_SWRST_MASK;
    writel(rstcon, EXYNOS4_RSTCON);
	udelay(10);
    rstcon &= ~(EXYNOS4X12_HOST_LINK_PORT_SWRST_MASK | EXYNOS4X12_PHY1_SWRST_MASK);
    writel(rstcon, EXYNOS4_RSTCON);
    udelay(80);

	clk_disable(otg_clk);
	clk_put(otg_clk);

	return 0;
}

static int exynos4x12_usb_phy1_exit(struct platform_device *pdev)
{
	struct clk *otg_clk;
	int err;

    if (atomic_dec_return(&host_usage) > 0) {
        dev_info(&pdev->dev, "still being used\n");
		return 0;
    }

	otg_clk = clk_get(&pdev->dev, "otg");
	if (IS_ERR(otg_clk)) {
		dev_err(&pdev->dev, "Failed to get otg clock\n");
		return PTR_ERR(otg_clk);
	}

	err = clk_enable(otg_clk);
	if (err) {
		clk_put(otg_clk);
		return err;
	}

    /* unset to normal of Device */
    writel((readl(EXYNOS4_PHYPWR) | PHY0_NORMAL_MASK), EXYNOS4_PHYPWR);

    /* unset to normal of Host */
    writel(readl(EXYNOS4_PHYPWR) | PHY1_STD_NORMAL_MASK | EXYNOS4X12_HSIC0_NORMAL_MASK | EXYNOS4X12_HSIC1_NORMAL_MASK, EXYNOS4_PHYPWR);
    writel((readl(EXYNOS4_PHYPWR) | PHY1_STD_ANALOG_POWERDOWN), EXYNOS4_PHYPWR);

    writel(readl(S5P_USB_PHY_CONTROL) & ~S5P_USB_PHY_ENABLE, S5P_USB_PHY_CONTROL);
    writel(readl(S5P_HSIC_1_PHY_CONTROL) & (~S5P_HSIC_1_PHY_ENABLE), S5P_HSIC_1_PHY_CONTROL);
    writel(readl(S5P_HSIC_2_PHY_CONTROL) & (~S5P_HSIC_2_PHY_ENABLE), S5P_HSIC_2_PHY_CONTROL);

	clk_disable(otg_clk);
	clk_put(otg_clk);

	return 0;
}

int s5p_usb_phy_init(struct platform_device *pdev, int type)
{
    if (soc_is_exynos4212() || soc_is_exynos4412()) {
        if (type == S5P_USB_PHY_DEVICE)
            return exynos4x12_usb_phy0_init(pdev);
        else if (type == S5P_USB_PHY_HOST)
            return exynos4x12_usb_phy1_init(pdev);
    } else {
        if (type == S5P_USB_PHY_DEVICE)
            return exynos4210_usb_phy0_init(pdev);
        else if (type == S5P_USB_PHY_HOST)
            return exynos4210_usb_phy1_init(pdev);
    }

	return -EINVAL;
}

int s5p_usb_phy_exit(struct platform_device *pdev, int type)
{
    if (soc_is_exynos4212() || soc_is_exynos4412()) {
        if (type == S5P_USB_PHY_DEVICE)
            return exynos4x12_usb_phy0_exit(pdev);
        else if (type == S5P_USB_PHY_HOST)
            return exynos4x12_usb_phy1_exit(pdev);
    } else {
        if (type == S5P_USB_PHY_DEVICE)
            return exynos4210_usb_phy0_exit(pdev);
        else if (type == S5P_USB_PHY_HOST)
            return exynos4210_usb_phy1_exit(pdev);
    }

	return -EINVAL;
}
