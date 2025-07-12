/*
 * driver.c - centralized device driver management
 *
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2007 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2007 Novell Inc.
 *
 * This file is released under the GPLv2
 *
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include "base.h"

/* 迭代下一个设备结构
 * 本函数专用于本文件，迭代器已由调用者处理好 */
static struct device *next_device(struct klist_iter *i)
{
    /* 迭代下一个 klist_node 节点 */
	struct klist_node *n = klist_next(i);
	struct device *dev = NULL;
	struct device_private *dev_prv;

	if (n) {
        /* 通过 device_private 结构的 knode_driver 反查该结构
         * device_private 结构的 device 指向包含它的设备结构 */
		dev_prv = to_device_private_driver(n);
		dev = dev_prv->device;
	}
	return dev;
}

/**
 * driver_for_each_device - Iterator for devices bound to a driver.
 * @drv: Driver we're iterating.
 * @start: Device to begin with
 * @data: Data to pass to the callback.
 * @fn: Function to call for each device.
 *
 * Iterate over the @drv's list of devices calling @fn for each one.
 */
/* 遍历绑定到驱动程序上的设备
 * @drv:    需要遍历的驱动
 * @start:  首个开始遍历的设备，可以为 NULL，表示从第一个开始
 * @data:   传给回调函数的参数，本函数不会使用，只会传给 fn
 * @fn:     对每一个遍历的设备都会调用的回调函数
 *
 * 遍历某个驱动程序下挂载的设备，并为每一个设备调用 @fn 回调函数 */
int driver_for_each_device(struct device_driver *drv, struct device *start,
			   void *data, int (*fn)(struct device *, void *))
{
    /* 使用 klist_iter 迭代器 */
	struct klist_iter i;
	struct device *dev;
	int error = 0;

	if (!drv)
		return -EINVAL;

    /* 初始化迭代器
     * 驱动程序的设备链表在 klist_devices ，所有匹配本驱动的设备都使用
     * klist_node 挂载到该链表中
     * 设备挂载到驱动链表中，使用的是 knode_driver */
	klist_iter_init_node(&drv->p->klist_devices, &i,
			     start ? &start->p->knode_driver : NULL);
    /* 遍历每一个设备，若 dev = NULL，则遍历完成
     * 在每次有效的遍历时，调用 fn 回调函数，并传 data 参数 */
	while ((dev = next_device(&i)) && !error)
		error = fn(dev, data);
    /* 退出迭代器 */
	klist_iter_exit(&i);
	return error;
}
EXPORT_SYMBOL_GPL(driver_for_each_device);

/**
 * driver_find_device - device iterator for locating a particular device.
 * @drv: The device's driver
 * @start: Device to begin with
 * @data: Data to pass to match function
 * @match: Callback function to check device
 *
 * This is similar to the driver_for_each_device() function above, but
 * it returns a reference to a device that is 'found' for later use, as
 * determined by the @match callback.
 *
 * The callback should return 0 if the device doesn't match and non-zero
 * if it does.  If the callback returns non-zero, this function will
 * return to the caller and not iterate over any more devices.
 */
/* 用于定位特定设备的设备迭代器
 * @drv:    需要遍历的驱动
 * @start:  首个开始遍历的设备，可以为 NULL，表示从第一个开始
 * @data:   传给回调函数的参数，本函数不会使用，只会传给 fn
 * @match:  设备匹配回调函数
 *
 * 与上面的 driver_for_each_device() 函数类似，但它返回一个对设备的引用，
 * 该设备是由 @match 回调确定的，引用外部使用
 *
 * 如果设备不匹配，match回调应返回0，如果匹配，则返回非零值。
 * 如果回调返回非零，则此函数将返回给调用者并且不再迭代任何设备 */
struct device *driver_find_device(struct device_driver *drv,
				  struct device *start, void *data,
				  int (*match)(struct device *dev, void *data))
{
    /* 使用 klist_iter 迭代器 */
	struct klist_iter i;
	struct device *dev;

	if (!drv || !drv->p)
		return NULL;

    /* 初始化迭代器
     * 驱动程序的设备链表在 klist_devices ，所有匹配本驱动的设备都使用
     * klist_node 挂载到该链表中
     * 设备挂载到驱动链表中，使用的是 knode_driver */
	klist_iter_init_node(&drv->p->klist_devices, &i,
			     (start ? &start->p->knode_driver : NULL));
    /* 遍历每一个设备，若 dev = NULL，则遍历完成
     * 在每次有效的遍历时，调用 match 回调函数，并传 data 参数 */
    /* 当设备匹配成功，则会递增设备引用计数，并退出迭代 */
	while ((dev = next_device(&i)))
		if (match(dev, data) && get_device(dev))
			break;
    /* 退出迭代器 */
	klist_iter_exit(&i);
	return dev;
}
EXPORT_SYMBOL_GPL(driver_find_device);

/**
 * driver_create_file - create sysfs file for driver.
 * @drv: driver.
 * @attr: driver attribute descriptor.
 */
/* 在设备驱动目录下创建属性文件 */
int driver_create_file(struct device_driver *drv,
		       const struct driver_attribute *attr)
{
	int error;
	if (drv)
		error = sysfs_create_file(&drv->p->kobj, &attr->attr);
	else
		error = -EINVAL;
	return error;
}
EXPORT_SYMBOL_GPL(driver_create_file);

/**
 * driver_remove_file - remove sysfs file for driver.
 * @drv: driver.
 * @attr: driver attribute descriptor.
 */
/* 移除设备驱动目录下的属性文件 */
void driver_remove_file(struct device_driver *drv,
			const struct driver_attribute *attr)
{
	if (drv)
		sysfs_remove_file(&drv->p->kobj, &attr->attr);
}
EXPORT_SYMBOL_GPL(driver_remove_file);

/* 添加驱动的默认属性组
 * 这个函数只在本文件中使用，用来添加 device_driver 驱动中的 
 * attribute_group 属性组 */
static int driver_add_groups(struct device_driver *drv,
			     const struct attribute_group **groups)
{
	int error = 0;
	int i;

    /* 存在属性组时，按照属性组依次添加即可 */
	if (groups) {
        /* groups 是个数组，以 NULL 结尾 */
		for (i = 0; groups[i]; i++) {
            /* 每一个属性组创建一次，在 驱动的 目录下 */
			error = sysfs_create_group(&drv->p->kobj, groups[i]);
			if (error) {
				while (--i >= 0)
					sysfs_remove_group(&drv->p->kobj,
							   groups[i]);
				break;
			}
		}
	}
	return error;
}

/* 移除驱动的默认属性组 */
static void driver_remove_groups(struct device_driver *drv,
				 const struct attribute_group **groups)
{
	int i;

    /* 依次移除属性组即可
     * groups 是个数组，以 NULL 结尾 */
	if (groups)
		for (i = 0; groups[i]; i++)
			sysfs_remove_group(&drv->p->kobj, groups[i]);
}

/**
 * driver_register - register driver with bus
 * @drv: driver to register
 *
 * We pass off most of the work to the bus_add_driver() call,
 * since most of the things we have to do deal with the bus
 * structures.
 */
/* 注册一个驱动到 bus 总线
 * 总线由驱动结构中的 bus 指针提供，必须要提供 bus 总线结构 */
int driver_register(struct device_driver *drv)
{
	int ret;
	struct device_driver *other;

    /* 注册驱动时，必须要有有效的 bus
     * 驱动必须附属与某个bus */
	BUG_ON(!drv->bus->p);

    /* 关键的回调函数 probe remove shutdown
     * 所属 bus 和 将要注册的驱动 不可以同时都提供回调函数
     * 在设备和驱动匹配时，优先使用 bus 的
     * 如果 bus 没有，则使用 驱动提供的
     * 对于 platform bus，是由驱动提供的 */
	if ((drv->bus->probe && drv->probe) ||
	    (drv->bus->remove && drv->remove) ||
	    (drv->bus->shutdown && drv->shutdown))
		printk(KERN_WARNING "Driver '%s' needs updating - please use "
			"bus_type methods\n", drv->name);

    /* 使用新注册的驱动在所属的bus中查找一下
     * 新注册的不应该查到的，如果查到了，那就是重复注册
     * 或者驱动的名字是不可以重名的，bus 上名字唯一 */
	other = driver_find(drv->name, drv->bus);
	if (other) {
		printk(KERN_ERR "Error: Driver '%s' is already registered, "
			"aborting...\n", drv->name);
		return -EBUSY;
	}

    /* 向 bus 注册驱动 */
	ret = bus_add_driver(drv);
	if (ret)
		return ret;
    /* 添加驱动的默认属性组 */
	ret = driver_add_groups(drv, drv->groups);
	if (ret)
		bus_remove_driver(drv);
	return ret;
}
EXPORT_SYMBOL_GPL(driver_register);

/**
 * driver_unregister - remove driver from system.
 * @drv: driver.
 *
 * Again, we pass off most of the work to the bus-level call.
 */
/* 从驱动所属的 bus 中移除驱动
 * 大部分操作都由 bus 核心层处理 */
void driver_unregister(struct device_driver *drv)
{
    /* 驱动结构有效性判断，必须具备 driver_private 结构
     * 这个结构是 设备驱动模型 必须的 */
	if (!drv || !drv->p) {
		WARN(1, "Unexpected driver unregister!\n");
		return;
	}
    /* 移除驱动的默认属性组 */
	driver_remove_groups(drv, drv->groups);
    /* 从 bus 注销驱动 */
	bus_remove_driver(drv);
}
EXPORT_SYMBOL_GPL(driver_unregister);

/**
 * driver_find - locate driver on a bus by its name.
 * @name: name of the driver.
 * @bus: bus to scan for the driver.
 *
 * Call kset_find_obj() to iterate over list of drivers on
 * a bus to find driver by name. Return driver if found.
 *
 * This routine provides no locking to prevent the driver it returns
 * from being unregistered or unloaded while the caller is using it.
 * The caller is responsible for preventing this.
 */
/* 通过驱动名字在 bus 上找驱动
 * 通过 bus 的 kset 链表找 kobject
 * 所有的 驱动的 kobject 都属于 所在的 bus 的 kset
 * （是在 bus_add_driver 中处理的）
 * 
 * 在 /sys/bus 目录下，每种bus都有 drivers 目录
 * 所有属于该bus 的驱动，都会有一个对应的目录
 * 这里通过 kset_find_obj 就是查找 drivers 目录下有没有对应的目录(kobject) */
struct device_driver *driver_find(const char *name, struct bus_type *bus)
{
    /* 在 bus 的驱动列表中查找，成功则返回对应的 kobject */
	struct kobject *k = kset_find_obj(bus->p->drivers_kset, name);
	struct driver_private *priv;

    /* 找到了，则反查 device_driver 结构 */
	if (k) {
		/* Drop reference added by kset_find_obj() */
        /* kset_find_obj 中会递增 kobject 的引用计数 */
		kobject_put(k);
        /* 通过 kobject 反查 driver_private 结构 */
		priv = to_driver(k);
        /* driver_private 结构中的 driver 指向包含它的 device_driver
         * 这就是要查找的名字为 name 的设备驱动结构了 */
		return priv->driver;
	}
    /* 没找到，则返回 NULL */
	return NULL;
}
EXPORT_SYMBOL_GPL(driver_find);
