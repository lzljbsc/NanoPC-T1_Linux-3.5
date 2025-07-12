/*
 * bus.c - bus driver management
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
#include <linux/init.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include "base.h"
#include "power/power.h"

/* /sys/devices/system */
static struct kset *system_kset;

/* 通过 attr 找到包含它的 bus_attribute 结构
 * 参考 bus_attribute 属性结构 */
#define to_bus_attr(_attr) container_of(_attr, struct bus_attribute, attr)

/*
 * sysfs bindings for drivers
 */

/* 通过 attr 找到包含它的 driver_attribute 结构
 * 参考 driver_attribute 属性结构 */
#define to_drv_attr(_attr) container_of(_attr, struct driver_attribute, attr)


/* Helper for bus_rescan_devices's iter */
/* bus_rescan_devices 迭代器的 实际处理函数 */
static int __must_check bus_rescan_devices_helper(struct device *dev,
						void *data);

/* 递增 bus 的引用计数
 * bus 的 kobj 在 bus结构的 subsys_private 结构中的 subsys(kset) */
static struct bus_type *bus_get(struct bus_type *bus)
{
	if (bus) {
        /* 递增 kset 引用计数，实际是kset内嵌的 kobject  */
		kset_get(&bus->p->subsys);
		return bus;
	}
	return NULL;
}

/* 递减 bus 的引用计数 */
static void bus_put(struct bus_type *bus)
{
	if (bus)
        /* 递减 kset 引用计数，实际是kset内嵌的 kobject */
		kset_put(&bus->p->subsys);
}

/* 驱动的属性文件 show 方法 */
static ssize_t drv_attr_show(struct kobject *kobj, struct attribute *attr,
			     char *buf)
{
    /* 通过通用的属性结构反查包含它的驱动属性结构 */
	struct driver_attribute *drv_attr = to_drv_attr(attr);
    /* 通过 kobj 反查包含它的驱动核心数据结构 */
	struct driver_private *drv_priv = to_driver(kobj);
	ssize_t ret = -EIO;

    /* 调用驱动的属性对应的特定show方法
     * 提供了驱动结构参数 */
	if (drv_attr->show)
		ret = drv_attr->show(drv_priv->driver, buf);
	return ret;
}

/* 驱动的属性文件 shore 方法 */
static ssize_t drv_attr_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
    /* 通过通用的属性结构反查包含它的驱动属性结构 */
	struct driver_attribute *drv_attr = to_drv_attr(attr);
    /* 通过 kobj 反查包含它的驱动核心数据结构 */
	struct driver_private *drv_priv = to_driver(kobj);
	ssize_t ret = -EIO;

    /* 调用驱动的属性对应的特定store方法
     * 提供了驱动结构参数 */
	if (drv_attr->store)
		ret = drv_attr->store(drv_priv->driver, buf, count);
	return ret;
}

/* 驱动 kobj_type 的 属性文件方法
 * 注册到驱动的属性结构为 driver_attribute */
static const struct sysfs_ops driver_sysfs_ops = {
	.show	= drv_attr_show,
	.store	= drv_attr_store,
};

/* 驱动 kobj_type 的 release 回调函数
 * 在驱动引用计数递减为 0 时调用 */
static void driver_release(struct kobject *kobj)
{
    /* 通过 kobj 反查包含它的 驱动私有核心数据 driver_private */
	struct driver_private *drv_priv = to_driver(kobj);

	pr_debug("driver: '%s': %s\n", kobject_name(kobj), __func__);
    /* 释放驱动的私有核心数据
     * 该数据是在 bus_add_driver 中动态分配的 */
	kfree(drv_priv);
}

/* 所有注册的驱动 的 kobj_type
 * 所有的驱动都是这种类型，可用于判断某个 kobject 是否为 驱动所有 */
static struct kobj_type driver_ktype = {
	.sysfs_ops	= &driver_sysfs_ops,
	.release	= driver_release,
};

/*
 * sysfs bindings for buses
 */
/* sysfs 中 bus 的属性 show 回调函数 */
static ssize_t bus_attr_show(struct kobject *kobj, struct attribute *attr,
			     char *buf)
{
    /* 找到 bus_attribute 结构，包含了真正需要的 show 回调函数 */
	struct bus_attribute *bus_attr = to_bus_attr(attr);
    /* 通过 kobj 找到包含它的 subsys_private 结构
     * subsys_private 结构中有包含它的 bus 指针 */
	struct subsys_private *subsys_priv = to_subsys_private(kobj);
	ssize_t ret = 0;

    /* 调用 bus_attribute 结构中的 show指针，与属性唯一对应的
     * subsys_priv->bus 指向了 bus_type 结构
     * 这里调用 show 时，竟然没有把 bus_attribute 传递进去
     * 难道所有的 bus 的属性，都是属性与操作函数唯一对应吗？  */
	if (bus_attr->show)
		ret = bus_attr->show(subsys_priv->bus, buf);
	return ret;
}

/* sysfs 中 bus 的属性 store 回调函数 */
static ssize_t bus_attr_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
    /* 找到 bus_attribute 结构，包含了真正需要的 store 回调函数 */
	struct bus_attribute *bus_attr = to_bus_attr(attr);
    /* 通过 kobj 找到包含它的 subsys_private 结构
     * subsys_private 结构中有包含它的 bus 指针 */
	struct subsys_private *subsys_priv = to_subsys_private(kobj);
	ssize_t ret = 0;

    /* 调用 bus_attribute 结构中的 show指针，与属性唯一对应的
     * subsys_priv->bus 指向了 bus_type 结构
     * 这里调用 show 时，竟然没有把 bus_attribute 传递进去
     * 难道所有的 bus 的属性，都是属性与操作函数唯一对应吗？  */
	if (bus_attr->store)
		ret = bus_attr->store(subsys_priv->bus, buf, count);
	return ret;
}

/* 所有 bus 总线的 属性的处理回调函数
 * 属于 bus_ktype, 所有的 bus 都是 bus_ktype 类型
 * 与 dev_sysfs_ops 类似 */
static const struct sysfs_ops bus_sysfs_ops = {
	.show	= bus_attr_show,
	.store	= bus_attr_store,
};

/* bus 创建属性文件 */
int bus_create_file(struct bus_type *bus, struct bus_attribute *attr)
{
	int error;
	if (bus_get(bus)) {
		error = sysfs_create_file(&bus->p->subsys.kobj, &attr->attr);
		bus_put(bus);
	} else
		error = -EINVAL;
	return error;
}
EXPORT_SYMBOL_GPL(bus_create_file);

/* bus 移除属性文件 */
void bus_remove_file(struct bus_type *bus, struct bus_attribute *attr)
{
	if (bus_get(bus)) {
		sysfs_remove_file(&bus->p->subsys.kobj, &attr->attr);
		bus_put(bus);
	}
}
EXPORT_SYMBOL_GPL(bus_remove_file);

/* bus_ktype
 * 所有的 bus_type 总线的 subsys_private 的 subsys 的 kset 都是这种type的
 * kobject, 所有的 bus 都属于 bus_ktype 类型
 * 从这里可以理解 bus_type 的 subsys_private 的 subsys 是 bus_type 的基类
 * bus_type 是一个实际的总线了，但无论何种bus，最基本的都是 bus_ktype 类型
 * 这种思想在整个设备驱动模型中都是一样的，就像所有的 device 基本类型都是
 * device_ktype 一样，无论怎么变，最基本的类型都是一样的 */
/* 这里的 bus_ktype 为什么没有 release 回调函数呢？？  */
static struct kobj_type bus_ktype = {
	.sysfs_ops	= &bus_sysfs_ops,
};

/* bus_kset 的 kset_uevent_ops 的 filter 回调函数
 * 筛选 bus 类型的 kobject 发送 uevent 消息 */
static int bus_uevent_filter(struct kset *kset, struct kobject *kobj)
{
	struct kobj_type *ktype = get_ktype(kobj);

    /* 检测要发送 uevent 的 kobject ，必须是 bus_ktype 类型才允许发送  */
	if (ktype == &bus_ktype)
		return 1;
	return 0;
}

/* bus_kset 的 kset_uevent_ops
 * 仅提供了 filter 回调函数，用于筛选 bus 类型的 kobject 发送 uevent 消息 */
static const struct kset_uevent_ops bus_uevent_ops = {
	.filter = bus_uevent_filter,
};

/* bus 模块顶层 kset
 * 是所有以后注册的 bus 的 kobject 的 kset 容器
 * 所有 bus 的 kobject 都属于这个 bus_kset */
static struct kset *bus_kset;


#ifdef CONFIG_HOTPLUG
/* Manually detach a device from its associated driver. */
/* 手动将设备与其关联的驱动程序分离(解绑)
 * 在 /sys/bus 的 驱动目录下，通过 echo dev_name > unbind
 * 将 指定的设备 与 驱动分离 */
static ssize_t driver_unbind(struct device_driver *drv,
			     const char *buf, size_t count)
{
    /* 引用驱动所属的 bus */
	struct bus_type *bus = bus_get(drv->bus);
	struct device *dev;
	int err = -ENODEV;

    /* 从驱动所在的 bus总线 查找 指定名称的 设备 */
	dev = bus_find_device_by_name(bus, NULL, buf);
    /* 需要确认设备所匹配的驱动确实是本驱动程序
     * 即 dev->driver == drv
     * 如果设备所匹配的驱动不是本驱动，那不能解绑 */
	if (dev && dev->driver == drv) {
		if (dev->parent)	/* Needed for USB */
			device_lock(dev->parent);
        /* 将设备从匹配的驱动程序中分离设备 */
		device_release_driver(dev);
		if (dev->parent)
			device_unlock(dev->parent);
		err = count;
	}
    /* 递减设备引用计数
     * bus_find_device_by_name 中有递增设备引用计数 */
	put_device(dev);
    /* 递减 bus 总线引用计数 */
	bus_put(bus);
	return err;
}
/* driver_attr_unbind */
static DRIVER_ATTR(unbind, S_IWUSR, NULL, driver_unbind);

/*
 * Manually attach a device to a driver.
 * Note: the driver must want to bind to the device,
 * it is not possible to override the driver's id table.
 */
/* 手动将设备绑定到驱动程序
 * */
static ssize_t driver_bind(struct device_driver *drv,
			   const char *buf, size_t count)
{
    /* 引用驱动所属的 bus */
	struct bus_type *bus = bus_get(drv->bus);
	struct device *dev;
	int err = -ENODEV;

    /* 从驱动所在的 bus总线 查找 指定名称的 设备 */
	dev = bus_find_device_by_name(bus, NULL, buf);
    /* 匹配的设备，必须没有匹配过驱动(已经匹配驱动了，就不用再匹配了)
     * 将设备与驱动调用 driver_match_device() 进行匹配
     * 这里其实调用的是 bus 的 match 函数，由 bus 进行匹配 */
	if (dev && dev->driver == NULL && driver_match_device(drv, dev)) {
		if (dev->parent)	/* Needed for USB */
			device_lock(dev->parent);
		device_lock(dev);
        /* 尝试将 设备 和 驱动 绑定在一起 */
        /* 注意，绑定成功返回 1， 其它错误情况返回 0 或 负值 */
		err = driver_probe_device(drv, dev);
		device_unlock(dev);
		if (dev->parent)
			device_unlock(dev->parent);

        /* 绑定成功返回
         * 错误返回 ENODEV */
		if (err > 0) {
			/* success */
			err = count;
		} else if (err == 0) {
			/* driver didn't accept device */
			err = -ENODEV;
		}
	}
    /* 递减设备引用计数
     * bus_find_device_by_name 中有递增设备引用计数 */
	put_device(dev);
    /* 递减 bus 总线引用计数 */
	bus_put(bus);
	return err;
}
/* driver_attr_bind */
static DRIVER_ATTR(bind, S_IWUSR, NULL, driver_bind);

/* drivers_autoprobe  属性 show 方法 */
static ssize_t show_drivers_autoprobe(struct bus_type *bus, char *buf)
{
	return sprintf(buf, "%d\n", bus->p->drivers_autoprobe);
}

/* drivers_autoprobe  属性 store 方法 */
static ssize_t store_drivers_autoprobe(struct bus_type *bus,
				       const char *buf, size_t count)
{
    /* 接受 0 或 非0 值 */
	if (buf[0] == '0')
		bus->p->drivers_autoprobe = 0;
	else
		bus->p->drivers_autoprobe = 1;
	return count;
}

/* drivers_probe 属性 store 方法 */
static ssize_t store_drivers_probe(struct bus_type *bus,
				   const char *buf, size_t count)
{
	struct device *dev;

    /* 从驱动所在的 bus总线 查找 指定名称的 设备 */
	dev = bus_find_device_by_name(bus, NULL, buf);
	if (!dev)
		return -ENODEV;
    /* 将 指定的设备 尝试与总线上的驱动进行绑定
     * 绑定成功时 返回 0 */
	if (bus_rescan_devices_helper(dev, NULL) != 0)
		return -EINVAL;
	return count;
}
#endif

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
		dev_prv = to_device_private_bus(n);
		dev = dev_prv->device;
	}
	return dev;
}

/**
 * bus_for_each_dev - device iterator.
 * @bus: bus type.
 * @start: device to start iterating from.
 * @data: data for the callback.
 * @fn: function to be called for each device.
 *
 * Iterate over @bus's list of devices, and call @fn for each,
 * passing it @data. If @start is not NULL, we use that device to
 * begin iterating from.
 *
 * We check the return of @fn each time. If it returns anything
 * other than 0, we break out and return that value.
 *
 * NOTE: The device that returns a non-zero value is not retained
 * in any way, nor is its refcount incremented. If the caller needs
 * to retain this data, it should do so, and increment the reference
 * count in the supplied callback.
 */
/* bus 总线的设备迭代器
 *
 * 遍历指定 bus 下挂载的设备，并为每一个设备调用 @fn 回调函数
 * 若回调函数 fn 返回非零，则退出迭代 */
int bus_for_each_dev(struct bus_type *bus, struct device *start,
		     void *data, int (*fn)(struct device *, void *))
{
    /* 使用 klist_iter 迭代器 */
	struct klist_iter i;
	struct device *dev;
	int error = 0;

	if (!bus)
		return -EINVAL;

    /* 初始化迭代器
     * 总线的设备链表在 klist_devices , 所有属于该bus 的设备都通过
     * 设备自身的 knode_bus 挂接到 总线链表中 */
	klist_iter_init_node(&bus->p->klist_devices, &i,
			     (start ? &start->p->knode_bus : NULL));
    /* 遍历每一个设备，若 dev = NULL，则遍历完成
     * 在每次有效的遍历时，调用 fn 回调函数，并传 data 参数 */
    /* 当设备匹配成功，则会递增设备引用计数，并退出迭代 */
	while ((dev = next_device(&i)) && !error)
		error = fn(dev, data);
    /* 退出迭代器 */
	klist_iter_exit(&i);
	return error;
}
EXPORT_SYMBOL_GPL(bus_for_each_dev);

/**
 * bus_find_device - device iterator for locating a particular device.
 * @bus: bus type
 * @start: Device to begin with
 * @data: Data to pass to match function
 * @match: Callback function to check device
 *
 * This is similar to the bus_for_each_dev() function above, but it
 * returns a reference to a device that is 'found' for later use, as
 * determined by the @match callback.
 *
 * The callback should return 0 if the device doesn't match and non-zero
 * if it does.  If the callback returns non-zero, this function will
 * return to the caller and not iterate over any more devices.
 */
/* 用于查找特定设备的设备迭代器
 *
 * 与 bus_for_each_dev() 函数类似，但本函数返回一个对设备的引用，
 * 该设备是由 @match 回调确定，以供返回后其它程序使用
 * 如果没有任何匹配，则返回 NULL，如果匹配成功，则返回设备结构指针。
 * 如果 match 回调返回非零值，则此函数将返回，并且不再迭代任何设备 */
struct device *bus_find_device(struct bus_type *bus,
			       struct device *start, void *data,
			       int (*match)(struct device *dev, void *data))
{
    /* 使用 klist_iter 迭代器 */
	struct klist_iter i;
	struct device *dev;

	if (!bus)
		return NULL;

    /* 初始化迭代器
     * 总线的设备链表在 klist_devices , 所有属于该bus 的设备都通过
     * 设备自身的 knode_bus 挂接到 总线链表中 */
	klist_iter_init_node(&bus->p->klist_devices, &i,
			     (start ? &start->p->knode_bus : NULL));
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
EXPORT_SYMBOL_GPL(bus_find_device);

/* 设备名匹配函数
 * 比对 设备名 与 data 提供的设备，返回比较结果 */
static int match_name(struct device *dev, void *data)
{
	const char *name = data;

    /* sysfs_streq 比较两个字符串是否相同
     * 相同则返回 true, 不同则返回 false */
	return sysfs_streq(name, dev_name(dev));
}

/**
 * bus_find_device_by_name - device iterator for locating a particular device of a specific name
 * @bus: bus type
 * @start: Device to begin with
 * @name: name of the device to match
 *
 * This is similar to the bus_find_device() function above, but it handles
 * searching by a name automatically, no need to write another strcmp matching
 * function.
 */
/* 设备迭代器，查找具有特定名称的特定设备结构
 *
 * 与 bus_find_device() 类似，但它自动处理按设备名搜索，不需要编写另一个
 * strcmp 匹配函数 */
struct device *bus_find_device_by_name(struct bus_type *bus,
				       struct device *start, const char *name)
{
    /* 还是调用 bus_find_device() 搜索，内置提供了 match_name 匹配函数 */
	return bus_find_device(bus, start, (void *)name, match_name);
}
EXPORT_SYMBOL_GPL(bus_find_device_by_name);

/**
 * subsys_find_device_by_id - find a device with a specific enumeration number
 * @subsys: subsystem
 * @id: index 'id' in struct device
 * @hint: device to check first
 *
 * Check the hint's next object and if it is a match return it directly,
 * otherwise, fall back to a full list search. Either way a reference for
 * the returned object is taken.
 */
/* 查找具有特定枚举号的设备
 * @subsys: subsystem
 * @id:     设备结构的索引 id
 * @hint:   第一个开始检查的设备结构指针
 *
 * 检查 subsys 的每一个设备，如果 id 匹配，则引用设备并返回；
 * 本函数与 bus_find_device 等匹配函数类似
 * 这里使用了 设备枚举号，并由本函数内部进行匹配检查 */
struct device *subsys_find_device_by_id(struct bus_type *subsys, unsigned int id,
					struct device *hint)
{
    /* 使用 klist_iter 迭代器 */
	struct klist_iter i;
	struct device *dev;

	if (!subsys)
		return NULL;

    /* 这里区分了 hint == NULL 与 hint != NULL 两种情况
     * 按照其它类似的函数，完全没必要区分
     * 重要的遍历流程都是一样的 */
	if (hint) {
        /* 遍历设备，匹配 id ，这里是从 hint 指定的设备开始 */
		klist_iter_init_node(&subsys->p->klist_devices, &i, &hint->p->knode_bus);
		dev = next_device(&i);
		if (dev && dev->id == id && get_device(dev)) {
			klist_iter_exit(&i);
			return dev;
		}
		klist_iter_exit(&i);
	}

    /* 遍历设备，匹配 id，这里是从设备链表头开始遍历 */
	klist_iter_init_node(&subsys->p->klist_devices, &i, NULL);
	while ((dev = next_device(&i))) {
		if (dev->id == id && get_device(dev)) {
			klist_iter_exit(&i);
			return dev;
		}
	}
	klist_iter_exit(&i);
	return NULL;
}
EXPORT_SYMBOL_GPL(subsys_find_device_by_id);

/* 迭代下一个驱动结构
 * 本函数专用于本文件，迭代器已由调用者处理好 */
static struct device_driver *next_driver(struct klist_iter *i)
{
    /* 迭代下一个 klist_node 节点 */
	struct klist_node *n = klist_next(i);
	struct driver_private *drv_priv;

	if (n) {
        /* 通过 device_private 结构的 knode_bus 反查该结构
         * device_private 结构的 driver 指向包含它的设备结构 */
		drv_priv = container_of(n, struct driver_private, knode_bus);
		return drv_priv->driver;
	}
	return NULL;
}

/**
 * bus_for_each_drv - driver iterator
 * @bus: bus we're dealing with.
 * @start: driver to start iterating on.
 * @data: data to pass to the callback.
 * @fn: function to call for each driver.
 *
 * This is nearly identical to the device iterator above.
 * We iterate over each driver that belongs to @bus, and call
 * @fn for each. If @fn returns anything but 0, we break out
 * and return it. If @start is not NULL, we use it as the head
 * of the list.
 *
 * NOTE: we don't return the driver that returns a non-zero
 * value, nor do we leave the reference count incremented for that
 * driver. If the caller needs to know that info, it must set it
 * in the callback. It must also be sure to increment the refcount
 * so it doesn't disappear before returning to the caller.
 */
/* bus 总线上的 驱动迭代器
 * @bus:    将要处理的 bus
 * @start:  起始 driver 结构指针
 * @data:   传入 fn 回调的参数
 * @fn:     每次迭代时调用的回调
 *
 * 遍历所属 @bus 的每一个驱动程序，并为每个驱动程序调用 @fn。
 * 如果 @fn 的返回值不是 0，则跳出迭代并返回错误值。
 * 如果 @start 不是 NULL，则从 @start 开始遍历 */
int bus_for_each_drv(struct bus_type *bus, struct device_driver *start,
		     void *data, int (*fn)(struct device_driver *, void *))
{
    /* 使用 klist_iter 迭代器 */
	struct klist_iter i;
	struct device_driver *drv;
	int error = 0;

	if (!bus)
		return -EINVAL;

    /* 初始化迭代器
     * 驱动程序在 bus 的 klist_drivers 链表上
     * 驱动程序使用 knode_bus 挂载到该链表中 */
	klist_iter_init_node(&bus->p->klist_drivers, &i,
			     start ? &start->p->knode_bus : NULL);
    /* 遍历每一个驱动程序，若 drv = NULL，则遍历完成
     * 在每次有效的遍历时，调用 fn 回调函数，并传 data 参数 */
	while ((drv = next_driver(&i)) && !error)
		error = fn(drv, data);
    /* 退出迭代器 */
	klist_iter_exit(&i);
	return error;
}
EXPORT_SYMBOL_GPL(bus_for_each_drv);

/* 向 device 设备下，添加 bus 总线默认的设备属性
 * 这是 bus 总线中附带的设备属性
 * 只要是属于这个bus的设备，那就会添加这些属性 */
static int device_add_attrs(struct bus_type *bus, struct device *dev)
{
	int error = 0;
	int i;

    /* 不是所有的 bus 都会有设备默认属性，没有就返回成功。。。 */
	if (!bus->dev_attrs)
		return 0;

    /* 所有的属性都在 bus->dev_attrs 数组里了
     * 直接将每个属性创建到 设备 目录下就好了 */
	for (i = 0; attr_name(bus->dev_attrs[i]); i++) {
		error = device_create_file(dev, &bus->dev_attrs[i]);
		if (error) {
			while (--i >= 0)
				device_remove_file(dev, &bus->dev_attrs[i]);
			break;
		}
	}
	return error;
}

/* 移除 device 设备下，bus 总线默认的设备属性 */
static void device_remove_attrs(struct bus_type *bus, struct device *dev)
{
	int i;

    /* 将所有属性 bus->dev_attrs 依次从设备目录下删除就好了  */
	if (bus->dev_attrs) {
		for (i = 0; attr_name(bus->dev_attrs[i]); i++)
			device_remove_file(dev, &bus->dev_attrs[i]);
	}
}

/**
 * bus_add_device - add device to bus
 * @dev: device being added
 *
 * - Add device's bus attributes.
 * - Create links to device's bus.
 * - Add the device to its bus's list of devices.
 */
/* 添加 device 到 bus 总线 */
int bus_add_device(struct device *dev)
{
    /* 递增 dev -> bus 的引用计数 */
    /* 只要有新的设备注册，那就需要递增
     * 所以 bus 的引用计数会随着注册的 device 一直增大
     * 当然，当有设备注销时，也会减小的 */
	struct bus_type *bus = bus_get(dev->bus);
	int error = 0;

    /* 存在有效的 bus 则继续处理
     * 无 bus 时，直接返回成功 0 */
	if (bus) {
		pr_debug("bus: '%s': add device %s\n", bus->name, dev_name(dev));
        /* 将 bus 总线中默认的设备属性添加到 设备 目录中 */
		error = device_add_attrs(bus, dev);
		if (error)
			goto out_put;
        /* 在 /sys/bus/platform/devices/ 目录下
         * 创建以 设备名命名 的 软连接，指向实际的设备目录
         * 如: /sys/bus/platform/devices/ 目录中
         * leds-gpio -> ../../../devices/platform/leds-gpio
         * */
		error = sysfs_create_link(&bus->p->devices_kset->kobj,
						&dev->kobj, dev_name(dev));
		if (error)
			goto out_id;
        /* 在实际的设备目录下，创建一个 subsystem 软连接
         * 指向 设备所属的 bus 目录
         * 如: /sys/devices/platform/leds-gpio/ 
         * subsystem -> ../../../bus/platform */
		error = sysfs_create_link(&dev->kobj,
				&dev->bus->p->subsys.kobj, "subsystem");
		if (error)
			goto out_subsys;
        /* 将 device 链接到 bus 的设备链表中
         * 这里使用了 klist 链表，设备提供了 klist_node 节点
         * 并将设备节点链接到了 bus 的 klist 链表头 */
		klist_add_tail(&dev->p->knode_bus, &bus->p->klist_devices);
	}
	return 0;

out_subsys:
	sysfs_remove_link(&bus->p->devices_kset->kobj, dev_name(dev));
out_id:
	device_remove_attrs(bus, dev);
out_put:
	bus_put(dev->bus);
	return error;
}

/**
 * bus_probe_device - probe drivers for a new device
 * @dev: device to probe
 *
 * - Automatically probe for a driver if the bus allows it.
 */
/* 为新设备探测驱动程序 */
/* 被 device_add 调用，用于为设备匹配驱动 */
void bus_probe_device(struct device *dev)
{
	struct bus_type *bus = dev->bus;
	struct subsys_interface *sif;
	int ret;

    /* 如果设备不属于某个 bus，那没法匹配驱动 */
	if (!bus)
		return;

    /* 支持 drivers_autoprobe 功能
     * 自动为设备匹配驱动 */
	if (bus->p->drivers_autoprobe) {
        /* 尝试为设备匹配驱动
         * 将设备附加到一个驱动上 */
		ret = device_attach(dev);
		WARN_ON(ret < 0);
	}

	mutex_lock(&bus->p->mutex);
    /* 遍历设备所属 bus 的 interfaces 链表
     * 该链表是bus 的设备功能接口，由其它子系统注册
     * 参考 subsys_interface_register 相关函数
     * 存在 add_dev 钩子函数的，每一个都调用 */
	list_for_each_entry(sif, &bus->p->interfaces, node)
		if (sif->add_dev)
			sif->add_dev(dev, sif);
	mutex_unlock(&bus->p->mutex);
}

/**
 * bus_remove_device - remove device from bus
 * @dev: device to be removed
 *
 * - Remove device from all interfaces.
 * - Remove symlink from bus' directory.
 * - Delete device from bus's list.
 * - Detach from its driver.
 * - Drop reference taken in bus_add_device().
 */
/* 从 bus 中移除一个 device
 * 需要接触在 bus_add_device() 中对bus 的引用计数递增 */
void bus_remove_device(struct device *dev)
{
	struct bus_type *bus = dev->bus;
	struct subsys_interface *sif;

    /* 如果设备不属于某个 bus
     * 那也没做过匹配，无需从 bus 移除 */
	if (!bus)
		return;

	mutex_lock(&bus->p->mutex);
    /* 遍历设备所属 bus 的 interfaces 链表
     * 该链表是bus 的设备功能接口，由其它子系统注册
     * 参考 subsys_interface_register 相关函数
     * 存在 remove_dev 钩子函数的，每一个都调用 */
	list_for_each_entry(sif, &bus->p->interfaces, node)
		if (sif->remove_dev)
			sif->remove_dev(dev, sif);
	mutex_unlock(&bus->p->mutex);

    /* 移除 设备目录下的 subsystem 软连接 */
	sysfs_remove_link(&dev->kobj, "subsystem");
    /* 移除 /sys/bus/[bus.name]/devices 的 设备名 软连接 */
	sysfs_remove_link(&dev->bus->p->devices_kset->kobj,
			  dev_name(dev));
    /* 移除bus的默认设备属性 */
	device_remove_attrs(dev->bus, dev);
    /* 将设备的 klist_node 节点从 bus 的 klist 链表中移除 */
	if (klist_node_attached(&dev->p->knode_bus))
		klist_del(&dev->p->knode_bus);

	pr_debug("bus: '%s': remove device %s\n",
		 dev->bus->name, dev_name(dev));
    /* 从驱动程序中分离设备，解除 设备-驱动 的对应关系 */
	device_release_driver(dev);
    /* 递减 bus 引用计数 */
	bus_put(dev->bus);
}

/* 向驱动添加 所属bus 的 默认驱动属性
 * 注意，这是驱动所在的bus 自带的 驱动属性，
 * 所有添加到该bus的驱动，都会具有这些属性 */
static int driver_add_attrs(struct bus_type *bus, struct device_driver *drv)
{
	int error = 0;
	int i;

    /* bus的默认驱动属性在 drv_attrs 中
     * 是一个结构体指针数组 */
	if (bus->drv_attrs) {
        /* 遍历添加，以 NULL 结尾 */
		for (i = 0; attr_name(bus->drv_attrs[i]); i++) {
			error = driver_create_file(drv, &bus->drv_attrs[i]);
			if (error)
				goto err;
		}
	}
done:
	return error;
err:
	while (--i >= 0)
		driver_remove_file(drv, &bus->drv_attrs[i]);
	goto done;
}

/* 移除驱动 所属bus 的 默认驱动属性 */
static void driver_remove_attrs(struct bus_type *bus,
				struct device_driver *drv)
{
	int i;

    /* bus的默认驱动属性在 drv_attrs 中
     * 依次遍历移除即可  以 NULL 结尾 */
	if (bus->drv_attrs) {
		for (i = 0; attr_name(bus->drv_attrs[i]); i++)
			driver_remove_file(drv, &bus->drv_attrs[i]);
	}
}

/* 配置了 CONFIG_HOTPLUG 选项，则默认会加 probe 相关属性
 * drivers_autoprobe  drivers_probe 两个属性是默认加的,所有bus 都有 */
#ifdef CONFIG_HOTPLUG
/*
 * Thanks to drivers making their tables __devinit, we can't allow manual
 * bind and unbind from userspace unless CONFIG_HOTPLUG is enabled.
 */
/* 由于驱动程序将其设为 __devinit, 除非启用了 CONFIG_HOTPLUG, 
 * 否则我们不能允许从用户空间手动绑定和接触绑定 */
/* 驱动的 bind/unbind 可以在用户空间手动的绑定和解绑 设备
 * 这为调试提供了极大的方便 */
static int __must_check add_bind_files(struct device_driver *drv)
{
	int ret;

    /* 在驱动目录下创建 unbind/bind 属性文件 */
	ret = driver_create_file(drv, &driver_attr_unbind);
	if (ret == 0) {
		ret = driver_create_file(drv, &driver_attr_bind);
		if (ret)
			driver_remove_file(drv, &driver_attr_unbind);
	}
	return ret;
}

/* 移除驱动的 bind/unbind 属性 */
static void remove_bind_files(struct device_driver *drv)
{
    /* 在驱动目录下移除 unbind/bind 属性文件 */
	driver_remove_file(drv, &driver_attr_bind);
	driver_remove_file(drv, &driver_attr_unbind);
}

/* drivers_autoprobe  drivers_probe  属性 */
static BUS_ATTR(drivers_probe, S_IWUSR, NULL, store_drivers_probe);
static BUS_ATTR(drivers_autoprobe, S_IWUSR | S_IRUGO,
		show_drivers_autoprobe, store_drivers_autoprobe);

/* 添加 probe 相关属性文件
 * 两个 drivers_autoprobe  drivers_probe */
static int add_probe_files(struct bus_type *bus)
{
	int retval;

	retval = bus_create_file(bus, &bus_attr_drivers_probe);
	if (retval)
		goto out;

	retval = bus_create_file(bus, &bus_attr_drivers_autoprobe);
	if (retval)
		bus_remove_file(bus, &bus_attr_drivers_probe);
out:
	return retval;
}

/* 移除 probe 相关属性文件
 * 两个 drivers_autoprobe  drivers_probe */
static void remove_probe_files(struct bus_type *bus)
{
	bus_remove_file(bus, &bus_attr_drivers_autoprobe);
	bus_remove_file(bus, &bus_attr_drivers_probe);
}
#else
/* 未开启 CONFIG_HOTPLUG 配置，动态的 设备/驱动 操作属性都没有了 */
static inline int add_bind_files(struct device_driver *drv) { return 0; }
static inline void remove_bind_files(struct device_driver *drv) {}
static inline int add_probe_files(struct bus_type *bus) { return 0; }
static inline void remove_probe_files(struct bus_type *bus) {}
#endif

/* 驱动的 uevent 属性 store 方法
 * 所有的驱动都有 uevent 属性 */
static ssize_t driver_uevent_store(struct device_driver *drv,
				   const char *buf, size_t count)
{
	enum kobject_action action;

    /* 通过 字符串 反查 uevent 操作命令 UEVENT_ADD 等
     * 并使用 kobject_uevent 发送 uevent 事件 */
	if (kobject_action_type(buf, count, &action) == 0)
		kobject_uevent(&drv->p->kobj, action);
	return count;
}
static DRIVER_ATTR(uevent, S_IWUSR, NULL, driver_uevent_store);

/**
 * bus_add_driver - Add a driver to the bus.
 * @drv: driver.
 */
/* 添加一个驱动到它所属的bus总线
 * bus总线 由 驱动中的 drv->bus 指定 */
int bus_add_driver(struct device_driver *drv)
{
	struct bus_type *bus;
	struct driver_private *priv;
	int error = 0;

    /* 获取新注册驱动所属的bus
     * 该bus必须有效，无效bus则返回错误
     * 这里是不是还得判断 bus->p 指针有效性？ */
	bus = bus_get(drv->bus);
	if (!bus)
		return -EINVAL;

	pr_debug("bus: '%s': add driver %s\n", bus->name, drv->name);

    /* 分配驱动私有核心数据
     * 私有核心数据用于添加到设备驱动模型层次结构
     * 另外也用于挂到 bus上，链接匹配的设备节点 */
	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		error = -ENOMEM;
		goto out_put_bus;
	}
    /* 初始化 klist_devices 链表头
     * klist_devices 用于所有匹配本驱动的设备挂接 */
	klist_init(&priv->klist_devices, NULL, NULL);
    /* 私有核心数据 driver 指向包含它的驱动结构
     * 用于通过 私有核心数据 反查包含它的驱动结构 */
	priv->driver = drv;
    /* 驱动结构的 p 指向它的私有核心数据 */
	drv->p = priv;
    /* 驱动的设备模型结构 kobj 属于 所在 bus 的 driver_ktype(drivers目录)
     * 这里就体现了 bus 下的目录结构:
     * /sys/bus/[bus.name]/drivers/[driver.name] */
	priv->kobj.kset = bus->p->drivers_kset;
    /* 注册驱动的设备模型结构 kobject
     * 类型为 driver_ktype, 以驱动名 命名
     * 所有注册的驱动都属于 driver_ktype 类型，具有统一的模型 */
	error = kobject_init_and_add(&priv->kobj, &driver_ktype, NULL,
				     "%s", drv->name);
	if (error)
		goto out_unregister;

    /* 如果 驱动所属的 bus 具有 autoprobe 功能
     * 则尝试将驱动绑定到设备 */
	if (drv->bus->p->drivers_autoprobe) {
        /* 尝试绑定驱动到设备 */
		error = driver_attach(drv);
		if (error)
			goto out_unregister;
	}
    /* 将驱动 klist_node 添加到 bus 的 klist_drivers 链表中
     * 这样 驱动与总线 就绑定了 */
	klist_add_tail(&priv->knode_bus, &bus->p->klist_drivers);

    /* // TODO: module 相关处理待详细分析 */
	module_add_driver(drv->owner, drv);

    /* 创建驱动目录下的 uevent 属性文件 */
	error = driver_create_file(drv, &driver_attr_uevent);
	if (error) {
		printk(KERN_ERR "%s: uevent attr (%s) failed\n",
			__func__, drv->name);
	}
    /* 添加驱动所属的 bus 的驱动默认属性 */
	error = driver_add_attrs(bus, drv);
	if (error) {
		/* How the hell do we get out of this pickle? Give up */
		printk(KERN_ERR "%s: driver_add_attrs(%s) failed\n",
			__func__, drv->name);
	}

    /* 驱动的 bind/unbind 属性
     * 依赖 驱动的 suppress_bind_attrs 控制 */
	if (!drv->suppress_bind_attrs) {
        /* 添加 bind/unbind 属性 */
		error = add_bind_files(drv);
		if (error) {
			/* Ditto */
			printk(KERN_ERR "%s: add_bind_files(%s) failed\n",
				__func__, drv->name);
		}
	}

    /* 发送 驱动 的 uevent 添加事件 */
	kobject_uevent(&priv->kobj, KOBJ_ADD);
	return 0;

out_unregister:
	kobject_put(&priv->kobj);
	kfree(drv->p);
	drv->p = NULL;
out_put_bus:
	bus_put(bus);
	return error;
}

/**
 * bus_remove_driver - delete driver from bus's knowledge.
 * @drv: driver.
 *
 * Detach the driver from the devices it controls, and remove
 * it from its bus's list of drivers. Finally, we drop the reference
 * to the bus we took in bus_add_driver().
 */
/* 从 bus 总线中，移除指定的设备驱动
 * 将驱动从它控制设备中分离，并将其从其所属的总线链表中移除。
 * 最后，删除在 bus_add_driver() 时对总线的引用 */
void bus_remove_driver(struct device_driver *drv)
{
	if (!drv->bus)
		return;

    /* 移除 bind/unbind 属性 */
	if (!drv->suppress_bind_attrs)
		remove_bind_files(drv);
    /* 移除 bus 的默认驱动属性 */
	driver_remove_attrs(drv->bus, drv);
    /* 移除 驱动下的 uevent 属性 */
	driver_remove_file(drv, &driver_attr_uevent);
    /* 将驱动从 bus 驱动链表中移除 */
    /* 注意，这里可能是被阻塞的，因为需要移除的驱动可能还在使用
     * 在 klist_remove 中会等待真正移除完成才会返回 */
	klist_remove(&drv->p->knode_bus);
	pr_debug("bus: '%s': remove driver %s\n", drv->bus->name, drv->name);
    /* 将驱动程序与其控制的所有设备分离 */
	driver_detach(drv);
    /* // TODO: module 相关处理待详细分析 */
	module_remove_driver(drv);
    /* 解除 驱动程序的引用
     * 这应该是最后一次解除引用，会调用 release 函数了 */
	kobject_put(&drv->p->kobj);
    /* 解除 bus 的引用 */
	bus_put(drv->bus);
}

/* Helper for bus_rescan_devices's iter */
/* bus_rescan_devices 迭代器的 实际处理函数 */
static int __must_check bus_rescan_devices_helper(struct device *dev,
						  void *data)
{
	int ret = 0;

    /* 设备不能是已绑定驱动的
     * 已经绑定过驱动的，无需再次尝试绑定 */
	if (!dev->driver) {
		if (dev->parent)	/* Needed for USB */
			device_lock(dev->parent);
        /* 尝试将设备绑定到驱动程序上 */
        /* 就是遍历设备所在的bus总线，将设备与总线的每一个驱动
         * 尝试匹配 */
        /* 注意： 匹配成功时返回 1 */
		ret = device_attach(dev);
		if (dev->parent)
			device_unlock(dev->parent);
	}
    /* 返回值转换一下，上面匹配成功了返回 1，但本函数需要返回 0 */
	return ret < 0 ? ret : 0;
}

/**
 * bus_rescan_devices - rescan devices on the bus for possible drivers
 * @bus: the bus to scan.
 *
 * This function will look for devices on the bus with no driver
 * attached and rescan it against existing drivers to see if it matches
 * any by calling device_attach() for the unbound devices.
 */
/* 重新扫描总线上的设备，以查找可能的驱动程序
 * 本函数将查找总线上没有附加驱动程序的设备，并根据现有驱动程序重新扫描它，
 * 通过调用 device_attach() 来查找未绑定的设备，以查看它是否匹配任何驱动程序 */
/* 就是把总线上的所有设备重新与驱动匹配一下，能匹配成功的就绑定上  */
int bus_rescan_devices(struct bus_type *bus)
{
    /* 通过 bus_for_each_dev 实现
     * 遍历每一个设备，并为每个设备调用 bus_rescan_devices_helper */
	return bus_for_each_dev(bus, NULL, NULL, bus_rescan_devices_helper);
}
EXPORT_SYMBOL_GPL(bus_rescan_devices);

/**
 * device_reprobe - remove driver for a device and probe for a new driver
 * @dev: the device to reprobe
 *
 * This function detaches the attached driver (if any) for the given
 * device and restarts the driver probing process.  It is intended
 * to use if probing criteria changed during a devices lifetime and
 * driver attachment should change accordingly.
 */
/* 移除设备的驱动程序，并探测新驱动程序
 * 本函数为给定的设备解绑驱动程序（如果有的话），并重新启动驱动程序探测过程。
 * 本函数适用于在设备生命周期内探测条件发生变化且驱动程序连接也应该
 * 相应更改的情况 */
int device_reprobe(struct device *dev)
{
	if (dev->driver) {
		if (dev->parent)        /* Needed for USB */
			device_lock(dev->parent);
        /* 手动将设备与其关联的驱动程序分离 */
		device_release_driver(dev);
		if (dev->parent)
			device_unlock(dev->parent);
	}
    /* 将 指定的设备 尝试与总线上的驱动进行绑定
     * 绑定成功时 返回 0 */
	return bus_rescan_devices_helper(dev, NULL);
}
EXPORT_SYMBOL_GPL(device_reprobe);

/**
 * find_bus - locate bus by name.
 * @name: name of bus.
 *
 * Call kset_find_obj() to iterate over list of buses to
 * find a bus by name. Return bus if found.
 *
 * Note that kset_find_obj increments bus' reference count.
 */
#if 0
struct bus_type *find_bus(char *name)
{
	struct kobject *k = kset_find_obj(bus_kset, name);
	return k ? to_bus(k) : NULL;
}
#endif  /*  0  */


/**
 * bus_add_attrs - Add default attributes for this bus.
 * @bus: Bus that has just been registered.
 */

/* 添加bus 默认的属性
 * 参考 drivers/pci/pci-driver.c 中的 pci_bus_type */
static int bus_add_attrs(struct bus_type *bus)
{
	int error = 0;
	int i;

    /* bus_attrs 是一个结构体数组，以 NULL 结尾
     * 遍历 bus_attrs 所有元素，依次添加属性文件 */
	if (bus->bus_attrs) {
		for (i = 0; attr_name(bus->bus_attrs[i]); i++) {
			error = bus_create_file(bus, &bus->bus_attrs[i]);
			if (error)
				goto err;
		}
	}
done:
	return error;
err:
	while (--i >= 0)
		bus_remove_file(bus, &bus->bus_attrs[i]);
	goto done;
}

/* 移除 bus 默认属性 */
static void bus_remove_attrs(struct bus_type *bus)
{
	int i;

	if (bus->bus_attrs) {
		for (i = 0; attr_name(bus->bus_attrs[i]); i++)
			bus_remove_file(bus, &bus->bus_attrs[i]);
	}
}

/* klist 链表 节点迭代时使用的 get() 回调
 * 用于增加设备引用计数 */
static void klist_devices_get(struct klist_node *n)
{
    /* 通过接入 bus 的 klist_node 成员
     * 找到 device_private 结构 */
	struct device_private *dev_prv = to_device_private_bus(n);
    /* device_private 结构的 device 指针指向包含它的 device */
	struct device *dev = dev_prv->device;

    /* 递增 device 的引用计数 */
	get_device(dev);
}

/* klist 链表 节点迭代时使用的 put() 回调
 * 用于递减设备引用计数 */
static void klist_devices_put(struct klist_node *n)
{
    /* 通过接入 bus 的 klist_node 成员
     * 找到 device_private 结构 */
	struct device_private *dev_prv = to_device_private_bus(n);
    /* device_private 结构的 device 指针指向包含它的 device */
	struct device *dev = dev_prv->device;

    /* 递减 device 的引用计数 */
	put_device(dev);
}

/* bus 的 uevent 属性
 * 所有的 bus 下，都会有 uevent 属性 */
static ssize_t bus_uevent_store(struct bus_type *bus,
				const char *buf, size_t count)
{
	enum kobject_action action;

    /* 根据用户空间提供的字符串，查找需要处理的 uevent 操作
     * 用户空间需要给 uevent 操作的字符串，参考 kobject_actions 数组
     * 有 add remove 等操作
     * 有效的 uevent 操作，则使用 bus 的 kobj 发送 uevent 消息 */
	if (kobject_action_type(buf, count, &action) == 0)
		kobject_uevent(&bus->p->subsys.kobj, action);
	return count;
}
static BUS_ATTR(uevent, S_IWUSR, NULL, bus_uevent_store);

/**
 * __bus_register - register a driver-core subsystem
 * @bus: bus to register
 * @key: lockdep class key
 *
 * Once we have that, we register the bus with the kobject
 * infrastructure, then register the children subsystems it has:
 * the devices and drivers that belong to the subsystem.
 */
/* 注册驱动核心子系统，注册一个 bus 总线
 * 一旦我们有了这个，就用 kobject 基础设施注册总线，然后注册它
 * 所拥有的子系统：属于该子系统的设备和驱动程序。
 *
 * 以 platform 为例，注册时提供的结构为：
 * struct bus_type platform_bus_type = {
 *  .name       = "platform",
 *  .dev_attrs  = platform_dev_attrs,
 *  .match      = platform_match,
 *  .uevent     = platform_uevent,
 *  .pm         = &platform_dev_pm_ops,
 * };
 *
 * 注册 bus 时，提供的信息主要是 bus name，match 匹配函数
 * 其它的回调函数是可选的
 * 上面的 platform_bus_type 中提供了挂载到 platform 总线的设备具有的
 * 默认属性，由 platform_dev_attrs 提供，只要挂载到了 platform 总线
 * 的设备，都会添加该属性
 * */
int __bus_register(struct bus_type *bus, struct lock_class_key *key)
{
	int retval;
	struct subsys_private *priv;

    /* 注册 bus时提供的 bus_type 结构体，不包含 subsys_private 结构
     * 这里所有注册 bus时，都会自动分配 subsys_private 结构 */
	priv = kzalloc(sizeof(struct subsys_private), GFP_KERNEL);
	if (!priv)
		return -ENOMEM;

    /* 驱动核心私有数据 bus 指向包含本数据的 bus_type */
    /* bus_type 的 kset 是在 subsys_private 中的
     * 而在设备驱动模型中，都是通过 kset/kobject 管理的
     * 通过 kset/kobject 是能够找到 subsys_private 结构的（container_of）
     * 需要再通过 subsys_private 找到 bus_type
     * 所以这里就使用 bus指针指向包含 subsys_private 的 bus_type结构 */
	priv->bus = bus;
    /* bus_type 结构的 p 指针，指向 priv 驱动核心私有数据 */
	bus->p = priv;

    /* 初始化 bus 总线通知列表 */
	BLOCKING_INIT_NOTIFIER_HEAD(&priv->bus_notifier);

    /* bus_ktype 的私有数据 p 结构中的 subsys 是一个 kset
     * 用来实现注册到设备驱动模型中
     * 这里设置 kobj 的名字为 bus->name
     * 这个名字会在 /sys/bus 中 */
	retval = kobject_set_name(&priv->subsys.kobj, "%s", bus->name);
	if (retval)
		goto out;

    /* bus_type 的 kobj 都属于 bus_kset 这个 kset
     * ktype 都是 bus_ktype
     * 设置 drivers_autoprobe = 1*/
    /* 配置了 CONFIG_HOTPLUG ， 默认会添加 autoprobe 属性
     * 这里不管何种配置，默认设置 drivers_autoprobe = 1
     * 以后可以通过 sysfs 中的 autoprobe 属性文件修改 */
	priv->subsys.kobj.kset = bus_kset;
	priv->subsys.kobj.ktype = &bus_ktype;
	priv->drivers_autoprobe = 1;

    /* 注册 bus 的 kset
     * 这里就将 bus 放到 设备驱动模型中管理了
     * 会在 /sys/bus/ 中出现 bus name 的目录 */
	retval = kset_register(&priv->subsys);
	if (retval)
		goto out;

    /* 创建 uevent 属性文件，所有的 bus 都会有该属性 */
	retval = bus_create_file(bus, &bus_attr_uevent);
	if (retval)
		goto bus_uevent_fail;

    /* 创建 devices 目录，如 platform bus 下的目录:
     * /sys/bus/platform/devices 目录
     * 这个目录下会放本bus 下所有的 device 的符号链接
     * priv->devices_kset 指向新创建的 kset */
	priv->devices_kset = kset_create_and_add("devices", NULL,
						 &priv->subsys.kobj);
	if (!priv->devices_kset) {
		retval = -ENOMEM;
		goto bus_devices_fail;
	}

    /* 创建 drivers 目录，如 platform bus 下的目录:
     * /sys/bus/platform/drivers/ 目录
     * 这个目录下会放本bus 下所有的 drivers
     * priv->drivers_kset 指向新创建的 kset */
	priv->drivers_kset = kset_create_and_add("drivers", NULL,
						 &priv->subsys.kobj);
	if (!priv->drivers_kset) {
		retval = -ENOMEM;
		goto bus_drivers_fail;
	}

    /* bus interfaces 链表头初始化 */
	INIT_LIST_HEAD(&priv->interfaces);
    /* bus 操作保护用的 mutex */
	__mutex_init(&priv->mutex, "subsys mutex", key);
    /* 本 bus 的设备链表，所有添加到本bus 的设备都挂在这个链表中
     * 使用 klist 链表管理
     * 提供了 get put 回调 */
	klist_init(&priv->klist_devices, klist_devices_get, klist_devices_put);
    /* 与 klist_devices 链表类似
     * 是本bus的驱动链表，所有添加到本bus 的驱动都挂在这个链表中 */
	klist_init(&priv->klist_drivers, NULL, NULL);

    /* 创建 drivers_autoprobe  drivers_probe  属性文件 */
	retval = add_probe_files(bus);
	if (retval)
		goto bus_probe_files_fail;

    /* 添加 bus 的默认属性，如 drivers/pci/pci-driver.c 中的 pci_bus_type  */
	retval = bus_add_attrs(bus);
	if (retval)
		goto bus_attrs_fail;

	pr_debug("bus: '%s': registered\n", bus->name);
	return 0;

bus_attrs_fail:
	remove_probe_files(bus);
bus_probe_files_fail:
	kset_unregister(bus->p->drivers_kset);
bus_drivers_fail:
	kset_unregister(bus->p->devices_kset);
bus_devices_fail:
	bus_remove_file(bus, &bus_attr_uevent);
bus_uevent_fail:
	kset_unregister(&bus->p->subsys);
out:
	kfree(bus->p);
	bus->p = NULL;
	return retval;
}
EXPORT_SYMBOL_GPL(__bus_register);

/**
 * bus_unregister - remove a bus from the system
 * @bus: bus.
 *
 * Unregister the child subsystems and the bus itself.
 * Finally, we call bus_put() to release the refcount
 */
/* 从系统中移除一个bus总线
 * __bus_register 逆向操作 */
void bus_unregister(struct bus_type *bus)
{
	pr_debug("bus: '%s': unregistering\n", bus->name);
    /* 注销的bus具有根虚拟设备，需要先注销掉
     * 有根虚拟设备，说明这是一个 subsys */
	if (bus->dev_root)
		device_unregister(bus->dev_root);
    /* 移除 bus 默认属性 */
	bus_remove_attrs(bus);
    /* 移除 probe 相关属性 */
	remove_probe_files(bus);
    /* 注销 drivers_kset ， 删除了 /sys/bus/<name>/drivers 目录 */
	kset_unregister(bus->p->drivers_kset);
    /* 注销 devices_kset ， 删除了 /sys/bus/<name>/devices 目录 */
	kset_unregister(bus->p->devices_kset);
    /* 移除 bus 的 uevent 属性 */
	bus_remove_file(bus, &bus_attr_uevent);
    /* 注销 bus 的私有核心数据,释放内存 */
	kset_unregister(&bus->p->subsys);
	kfree(bus->p);
	bus->p = NULL;
}
EXPORT_SYMBOL_GPL(bus_unregister);

/* bus 总线通知器
 * 在添加/移除设备，设备绑定驱动、解绑驱动 操作时
 * 将调用注册到对应bus 的通知器回调
 * 是一种通知处理机制
 * 比如在 i2c-dev.c 中，注册了 i2cdev_notifier 通知器
 * 在 i2c bus 的 设备 绑定/解绑驱动 时，会调用对应的回调函数 */
/* 注册 bus 总线通知器 */
int bus_register_notifier(struct bus_type *bus, struct notifier_block *nb)
{
	return blocking_notifier_chain_register(&bus->p->bus_notifier, nb);
}
EXPORT_SYMBOL_GPL(bus_register_notifier);

/* 注销 bus 总线通知器 */
int bus_unregister_notifier(struct bus_type *bus, struct notifier_block *nb)
{
	return blocking_notifier_chain_unregister(&bus->p->bus_notifier, nb);
}
EXPORT_SYMBOL_GPL(bus_unregister_notifier);

/* 获取bus的 kset 结构
 * 注意，bus 的 kset 结构 属于 bus 的私有核心数据
 * 不应该由其它模块访问
 * 目前看到的，只有 pci 驱动使用了 */
struct kset *bus_get_kset(struct bus_type *bus)
{
	return &bus->p->subsys;
}
EXPORT_SYMBOL_GPL(bus_get_kset);

/* 获取 bus 的 klist_devices 链表指针
 * 获取的是 bus 的 设备链表
 * 注意，这是bus 的私有核心数据
 * 不应该由其它模块访问
 * 目前看到的，只有本代码中使用了 */
struct klist *bus_get_device_klist(struct bus_type *bus)
{
	return &bus->p->klist_devices;
}
EXPORT_SYMBOL_GPL(bus_get_device_klist);

/*
 * Yes, this forcibly breaks the klist abstraction temporarily.  It
 * just wants to sort the klist, not change reference counts and
 * take/drop locks rapidly in the process.  It does all this while
 * holding the lock for the list, so objects can't otherwise be
 * added/removed while we're swizzling.
 */
/* 是的，这会暂时强制破坏klist的抽象。
 * 它只想对klist进行排序，而不会更改引用计数，也不会在此过程中快速获取/释放锁。
 * 它在执行所有这些操作的同时还持有列表的锁，因此在 swizzling(这里理解为对klist
 * 的强制排序操作) 期间无法添加/删除对象。
 *
 * 这里的 bus_sort_breadthfirst 操作，是对 bus 的核心数据 device_klist 的直接操
 * 作。 按照 klist 的设计，不应该这样使用，但这里是要对 bus 的 device 进行排序，
 * 必须要对其最基本的 klist 进行链表的排序。而且直接使用 klist 的 k_lock，强制
 * 锁定了 klist ，所以其它模块无法操作 klist 了 。*/

/* 对设备 device 进行排序，插入 */
static void device_insertion_sort_klist(struct device *a, struct list_head *list,
					int (*compare)(const struct device *a,
							const struct device *b))
{
	struct list_head *pos;
	struct klist_node *n;
	struct device_private *dev_prv;
	struct device *b;

    /* 遍历 list 链表中的每一个节点
     * 注意，list 链表是已经排序过的链表 */
	list_for_each(pos, list) {
        /* 对 list 链表中的每一个节点 反查所属的 device 结构
         * b 变量就是 list 链表中的某个节点 对应的 device 结构 */
		n = container_of(pos, struct klist_node, n_node);
		dev_prv = to_device_private_bus(n);
		b = dev_prv->device;
        /* 新的需要排序的 a 设备，与 已经排序过的 list 的每个成员进行比较
         * compare 是由调用者提供的
         * 当比较结果满足 <= 0 ， 则将新的 a 移动到 b 的后面 */
		if (compare(a, b) <= 0) {
			list_move_tail(&a->p->knode_bus.n_node,
				       &b->p->knode_bus.n_node);
			return;
		}
	}
    /* 新的 a 设备没有被排序，则插入到 list 最后 */
	list_move_tail(&a->p->knode_bus.n_node, list);
}

/* 对bus上的所有设备 进行 广度优先 算法排序
 * 这里直接操作了 bus 的 device_klist 链表，遍历了每一个设备，并使用 compare
 * 回调进行了比较，做为排序依据
 * 本代码中，仅在 pci 驱动中使用了 */
void bus_sort_breadthfirst(struct bus_type *bus,
			   int (*compare)(const struct device *a,
					  const struct device *b))
{
    /* 被排序后的 device 先链接到 sorted_devices 链表中 */
	LIST_HEAD(sorted_devices);
	struct list_head *pos, *tmp;
	struct klist_node *n;
	struct device_private *dev_prv;
	struct device *dev;
	struct klist *device_klist;

    /* 获取 bus 的 私有核心数据 的 device_klist 链表
     * 所有设备都在这个链表上 */
	device_klist = bus_get_device_klist(bus);

    /* 强制锁定了 k_lock */
	spin_lock(&device_klist->k_lock);
    /* 遍历 device_klist 中的每一个节点
     * 节点是 klist_node 中的 list_head */
	list_for_each_safe(pos, tmp, &device_klist->k_list) {
        /* 通过 list_head 反查包含它的 klist_node */
		n = container_of(pos, struct klist_node, n_node);
        /* klist_node 就是 device_private 的 knode_bus 成员
         * 设备通过 knode_bus 链接到 device_klist 链表中 */
		dev_prv = to_device_private_bus(n);
        /* 再通过 device_private 的 device 指针找到包含它的 device 结构 */
        /* 这里的 dev 就是一个链接到 device_klist 的设备结构 */
		dev = dev_prv->device;
        /* 每一个设备都进行比较排序，排序后设备结构会被 移动 链接到
         * sorted_devices 链表上了
         * 注意，这里是被移动了， device_klist 中的会被删除 */
		device_insertion_sort_klist(dev, &sorted_devices, compare);
	}
    /* sorted_devices 已经是排序后的链表了
     * device_klist 中，经过排序后，应该没有任何链表节点了
     * 把 sorted_devices 合并到 device_klist 中
     * 那 device_klist 就是已经排序好的了 */
	list_splice(&sorted_devices, &device_klist->k_list);
	spin_unlock(&device_klist->k_lock);
}
EXPORT_SYMBOL_GPL(bus_sort_breadthfirst);

/**
 * subsys_dev_iter_init - initialize subsys device iterator
 * @iter: subsys iterator to initialize
 * @subsys: the subsys we wanna iterate over
 * @start: the device to start iterating from, if any
 * @type: device_type of the devices to iterate over, NULL for all
 *
 * Initialize subsys iterator @iter such that it iterates over devices
 * of @subsys.  If @start is set, the list iteration will start there,
 * otherwise if it is NULL, the iteration starts at the beginning of
 * the list.
 */
/* 初始化 subsys 设备迭代器
 * @iter: 需要初始化的子系统迭代器
 * @subsys: 希望迭代的 subsys
 * @start:  开始迭代的设备，如果为NULL，则从头开始
 * @type:   要迭代的设备的 device_type, 如果为 NULL 则表示所有设备
 *
 * 初始化一个设备迭代器，用来遍历 subsys 的设备链表，也可以指定需要遍历的
 * device_type，用于筛选指定的设备 */
void subsys_dev_iter_init(struct subsys_dev_iter *iter, struct bus_type *subsys,
			  struct device *start, const struct device_type *type)
{
	struct klist_node *start_knode = NULL;

    /* 指定了开始迭代的设备
     * 则从该设备节点开始 */
	if (start)
		start_knode = &start->p->knode_bus;
    /* 初始化迭代器，遍历的设备链表 klist_devices  */
	klist_iter_init_node(&subsys->p->klist_devices, &iter->ki, start_knode);
    /* 记录设备类型，用于遍历时筛选设备 */
	iter->type = type;
}
EXPORT_SYMBOL_GPL(subsys_dev_iter_init);

/**
 * subsys_dev_iter_next - iterate to the next device
 * @iter: subsys iterator to proceed
 *
 * Proceed @iter to the next device and return it.  Returns NULL if
 * iteration is complete.
 *
 * The returned device is referenced and won't be released till
 * iterator is proceed to the next device or exited.  The caller is
 * free to do whatever it wants to do with the device including
 * calling back into subsys code.
 */
/* 迭代下一个设备结构
 *
 * 迭代下一个设备结构，并返回 device 结构，已遍历完成，则返回 NULL */
/* 返回的设备将被引用，指导迭代器继续执行下一个设备或退出时才被释放。
 * 调用者可以自由地对设备执行任何操作，包括回调到子系统代码 */
/* 注意，对设备地引用是在 klist_next 中的 get() 回调执行的 */
struct device *subsys_dev_iter_next(struct subsys_dev_iter *iter)
{
	struct klist_node *knode;
	struct device *dev;

    /* 遍历每一个设备，直到主动返回 */
	for (;;) {
        /* 迭代链表下一个节点，节点是设备链表 klist_devices 上的 */
		knode = klist_next(&iter->ki);
		if (!knode)
			return NULL;
        /* klist_devices 链表的节点，是 device_private 的 knode_bus 连接的
         * 通过 knode_bus 反查 device_private 结构
         * 在 device_private 结构中，device 指针指向包含它的 设备结构 */
		dev = container_of(knode, struct device_private, knode_bus)->device;
        /* 如果 iter->type == NULL，则所有设备都是符合要求的
         * 否则，需要检查 dev->type，一致则符合要求 */
		if (!iter->type || iter->type == dev->type)
			return dev;
	}
}
EXPORT_SYMBOL_GPL(subsys_dev_iter_next);

/**
 * subsys_dev_iter_exit - finish iteration
 * @iter: subsys iterator to finish
 *
 * Finish an iteration.  Always call this function after iteration is
 * complete whether the iteration ran till the end or not.
 */
/* 退出迭代，结束迭代
 * 与 klist_iter_exit 类似，中间退出时必须调用 */
void subsys_dev_iter_exit(struct subsys_dev_iter *iter)
{
    /* 实际就是 klist_iter_exit 函数 */
	klist_iter_exit(&iter->ki);
}
EXPORT_SYMBOL_GPL(subsys_dev_iter_exit);

/* 向 subsys 注册 subsys_interface */
/* 当 subsys 上已经有设备时，新注册的 add_dev 钩子函数也会被调用
 * 以后新注册设备时，也会调用 */
int subsys_interface_register(struct subsys_interface *sif)
{
	struct bus_type *subsys;
	struct subsys_dev_iter iter;
	struct device *dev;

	if (!sif || !sif->subsys)
		return -ENODEV;

    /* 对连接到的bus引用，sif一定是连接到一个bus上 */
	subsys = bus_get(sif->subsys);
	if (!subsys)
		return -EINVAL;

    /* 私有核心数据 中的 mutex 是 interfaces 的专用保护 mutex */
	mutex_lock(&subsys->p->mutex);
    /* 将新注册的 subsys_interface 连接到 interfaces 链表中 */
	list_add_tail(&sif->node, &subsys->p->interfaces);
    /* 如果存在 add_dev 钩子函数，则遍历 subsys 上的所有设备 */
	if (sif->add_dev) {
        /* 初始化迭代器，从设备链表头开始 */
		subsys_dev_iter_init(&iter, subsys, NULL, NULL);
        /* 迭代每一个设备，并调用 add_dev 钩子函数 */
		while ((dev = subsys_dev_iter_next(&iter)))
			sif->add_dev(dev, sif);
        /* 迭代完成，则退出迭代 */
		subsys_dev_iter_exit(&iter);
	}
	mutex_unlock(&subsys->p->mutex);

	return 0;
}
EXPORT_SYMBOL_GPL(subsys_interface_register);

/* 从 subsys 移除 subsys_interface */
/* 当 subsys 上已经有设备时，新注册的 remove_dev 钩子函数也会被调用
 * 以后移除设备时，也会调用 */
void subsys_interface_unregister(struct subsys_interface *sif)
{
	struct bus_type *subsys;
	struct subsys_dev_iter iter;
	struct device *dev;

	if (!sif || !sif->subsys)
		return;

    /* 连接到的 subsys，作用于该 subsys 上的设备链表 */
	subsys = sif->subsys;

    /* 私有核心数据 中的 mutex 是 interfaces 的专用保护 mutex */
	mutex_lock(&subsys->p->mutex);
    /* 从 interfaces 链表中移除 本subsys_interface 节点 */
	list_del_init(&sif->node);
    /* 如果存在 remove_dev 钩子函数，则遍历 subsys 上的所有设备 */
	if (sif->remove_dev) {
        /* 初始化迭代器，从设备链表头开始 */
		subsys_dev_iter_init(&iter, subsys, NULL, NULL);
        /* 迭代每一个设备，并调用 remove_dev 钩子函数 */
		while ((dev = subsys_dev_iter_next(&iter)))
			sif->remove_dev(dev, sif);
        /* 迭代完成，则退出迭代 */
		subsys_dev_iter_exit(&iter);
	}
	mutex_unlock(&subsys->p->mutex);

    /* 解除对 subsys 的引用 */
	bus_put(subsys);
}
EXPORT_SYMBOL_GPL(subsys_interface_unregister);

/* 虚拟设备 release 回调，在引用计数=0时调用，用来释放 dev 结构 */
static void system_root_device_release(struct device *dev)
{
	kfree(dev);
}
/**
 * subsys_system_register - register a subsystem at /sys/devices/system/
 * @subsys: system subsystem
 * @groups: default attributes for the root device
 *
 * All 'system' subsystems have a /sys/devices/system/<name> root device
 * with the name of the subsystem. The root device can carry subsystem-
 * wide attributes. All registered devices are below this single root
 * device and are named after the subsystem with a simple enumeration
 * number appended. The registered devices are not explicitely named;
 * only 'id' in the device needs to be set.
 *
 * Do not use this interface for anything new, it exists for compatibility
 * with bad ideas only. New subsystems should use plain subsystems; and
 * add the subsystem-wide attributes should be added to the subsystem
 * directory itself and not some create fake root-device placed in
 * /sys/devices/system/<name>.
 */
/* 注册一个 subsystem 在 /sys/devices/system/ 目录下
 * @subsys: system subsystem, system bus
 * @groups: 子系统根设备的默认属性
 *
 * 所有的 'system' subsystem 都会有一个以 subsystem 命名的
 * /sys/devices/system/<name> 根设备。根设备可以携带子系统范围的属性。
 * 所有注册的设备都在这个根设备下面，并以子系统命名，并附加一个简单的
 * 枚举号。注册的设备没有显式命名；只需要设置设备中的 "id" 即可。
 *
 * 不要把这个接口用于任何新的驱动，它的存在只是为了兼容以前的设备（坏的想法）。
 * 新子系统应该使用普通子系统；并且添加子系统范围的属性应该添加到子系统目录本身，
 * 而不是创建一些伪造的根设备并放置在 /sys/devices/system/<name> 中。 
 *
 * 本接口也是注册一个 bus_type，会注册到 /sys/bus 目录下
 * 但还做了一些别的工作:
 * 创建了一个 device，并将其放在了 /sys/devices/system/ 目录下
 * 这样就产生了一个新设备 /sys/devices//system/<name>
 * 但这个设备是虚拟的，是以 bus_type 的名字命名的
 * 还有一个重要的， subsys->dev_root = dev
 * 设置了 bus_type 的 dev_root 为新创建的 dev
 * 也就是这个 bus 有一个根设备
 *
 * 在 设备注册时，选择设备的父设备时将会用到 dev_root
 * 这也决定了新注册设备在 /sys/ 目录中的位置
 * 属于 subsys 的设备，会被放在 /sys/devices/system/<name>/ 下 */
int subsys_system_register(struct bus_type *subsys,
			   const struct attribute_group **groups)
{
	struct device *dev;
	int err;

    /* subsys 就是一个 bus_type
     * 注册一个 bus ， 在 /sys/bus/ 中会有相应目录 */
	err = bus_register(subsys);
	if (err < 0)
		return err;

    /* subsys 中，需要创建一个虚拟设备 */
	dev = kzalloc(sizeof(struct device), GFP_KERNEL);
	if (!dev) {
		err = -ENOMEM;
		goto err_dev;
	}

    /* 以 subsys->name 命名虚拟设备 */
	err = dev_set_name(dev, "%s", subsys->name);
	if (err < 0)
		goto err_name;

    /* subsys 的虚拟设备以 system_kset->kobj 为父设备
     * 所以本虚拟设备在 /sys/devices/system/ 目录中 */
	dev->kobj.parent = &system_kset->kobj;
    /* 虚拟设备的默认属性组 */
	dev->groups = groups;
    /* 虚拟设备 release 回调，在引用计数=0时调用，用来释放 dev 结构 */
	dev->release = system_root_device_release;

    /* 注册虚拟设备，在 /sys/devices/system/ 中出现相应目录 */
	err = device_register(dev);
	if (err < 0)
		goto err_dev_reg;

    /* subsys 的 dev_root 设置为 虚拟设备dev
     * 这是为属于 subsys 的设备的 父设备 提供备选的
     * 可以参考 get_device_parent 函数 */
	subsys->dev_root = dev;
	return 0;

err_dev_reg:
	put_device(dev);
	dev = NULL;
err_name:
	kfree(dev);
err_dev:
	bus_unregister(subsys);
	return err;
}
EXPORT_SYMBOL_GPL(subsys_system_register);

/* bus 模块初始化
 * 创建 /sys/bus  /sys/devices/system 两个目录 kset */
int __init buses_init(void)
{
    /* 创建 bus kset，对应到 /sys/bus 目录
     * 没有父，所以在 /sys 顶层
     * 提供了 uevent_ops / bus_uevent_ops */
	bus_kset = kset_create_and_add("bus", &bus_uevent_ops, NULL);
	if (!bus_kset)
		return -ENOMEM;

    /* 创建 system kset，对应到 /sys/devices/system 目录
     * 这里将父设置为了 devices_kset , 所以在 /sys/devices/ 目录下 */
	system_kset = kset_create_and_add("system", NULL, &devices_kset->kobj);
	if (!system_kset)
		return -ENOMEM;

	return 0;
}
