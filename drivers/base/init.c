/*
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 *
 * This file is released under the GPLv2
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/memory.h>

#include "base.h"

/**
 * driver_init - initialize driver model.
 *
 * Call the driver model init functions to initialize their
 * subsystems. Called early from init/main.c.
 */
/* 初始化驱动模块，
 * 这里初始化了各模块子系统
 * 板级设备，如platform 设备的注册，是在 后面的 do_initcalls 中
 * 这里主要是各个子系统打基础部分 */
void __init driver_init(void)
{
	/* These are the core pieces */
	devtmpfs_init();
	devices_init();
	buses_init();
	classes_init();
	firmware_init();
	hypervisor_init();

	/* These are also core pieces, but must come after the
	 * core core pieces.
	 */
	platform_bus_init();
	cpu_dev_init();
	memory_dev_init();
}
