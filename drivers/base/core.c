/*
 * drivers/base/core.c - core driver model code (device registration, etc)
 *
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2006 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2006 Novell, Inc.
 *
 * This file is released under the GPLv2
 *
 */

#include <linux/device.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/kdev_t.h>
#include <linux/notifier.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/genhd.h>
#include <linux/kallsyms.h>
#include <linux/mutex.h>
#include <linux/async.h>
#include <linux/pm_runtime.h>
#include <linux/netdevice.h>

#include "base.h"
#include "power/power.h"

/* sysfs_deprecated 是一个遗留问题
 * 关于 /sys/block 目录的存放位置 
 * 根据网上资料， /sys/block 是最初的目录，
 * 但 block 更适合在 /sys/class/block/ 下 
 * 2.6.22 就已经更改了，但为了兼容，就预留了 下面的配置项
 * 旧的接口 /sys/block/ 保留了，但内容已经变成了指向他们在 
 * /sys/device 中真实设备的符号链接文件了 */
#ifdef CONFIG_SYSFS_DEPRECATED
#ifdef CONFIG_SYSFS_DEPRECATED_V2
long sysfs_deprecated = 1;
#else
long sysfs_deprecated = 0;
#endif
/* 一个启动参数，可在 uboot 启动参数中指定 sysfs_deprecated */
static __init int sysfs_deprecated_setup(char *arg)
{
	return strict_strtol(arg, 10, &sysfs_deprecated);
}
early_param("sysfs.deprecated", sysfs_deprecated_setup);
#endif

int (*platform_notify)(struct device *dev) = NULL;
int (*platform_notify_remove)(struct device *dev) = NULL;
/* 对应目录  /sys/dev  */
static struct kobject *dev_kobj;
/* 对应目录  /sys/dev/char/ */
struct kobject *sysfs_dev_char_kobj;
/* 对应目录  /sys/dev/block/ */
struct kobject *sysfs_dev_block_kobj;

/* 用于判断设备是否为一个分区类型
 * 只有块设备才具有该属性
 * 分区类型，具有特有的 device_type , 包含了 uevent 等回调函数 */
#ifdef CONFIG_BLOCK
static inline int device_is_not_partition(struct device *dev)
{
	return !(dev->type == &part_type);
}
#else
static inline int device_is_not_partition(struct device *dev)
{
	return 1;
}
#endif

/**
 * dev_driver_string - Return a device's driver name, if at all possible
 * @dev: struct device to get the name of
 *
 * Will return the device's driver's name if it is bound to a device.  If
 * the device is not bound to a driver, it will return the name of the bus
 * it is attached to.  If it is not attached to a bus either, an empty
 * string will be returned.
 */
/* 如果可能的话，返回设备的驱动程序名称
 * 如果绑定到设备，将返回设备的驱动程序名称。如果设备没有绑定到驱动，它将返回连
 * 接到的总线的名称。如果它也没有连接到总线，则将返回一个空字符串。*/
const char *dev_driver_string(const struct device *dev)
{
	struct device_driver *drv;

	/* dev->driver can change to NULL underneath us because of unbinding,
	 * so be careful about accessing it.  dev->bus and dev->class should
	 * never change once they are set, so they don't need special care.
	 */
    /* dev->driver 可能在取消绑定的时候设置为 NULL， 
     * 所以在访问时需要小心； 而 bus class 不会更改，所以直接访问即可 */
	drv = ACCESS_ONCE(dev->driver);
	return drv ? drv->name :
			(dev->bus ? dev->bus->name :
			(dev->class ? dev->class->name : ""));
}
EXPORT_SYMBOL(dev_driver_string);

/* 使用 device 中包含的 kobject 反向找 device  */
#define to_dev(obj) container_of(obj, struct device, kobj)
/* 通过通用的 attribute 找到所属的 device_attribute */
#define to_dev_attr(_attr) container_of(_attr, struct device_attribute, attr)

/* 用来匹配 attr 和 对应的 show store 方法的， 
 * 使用这种方法，可以将 属性 / 方法 进行 一一对应，方便匹配调用 
 * 参考 kobj_attribute */
static ssize_t dev_attr_show(struct kobject *kobj, struct attribute *attr,
			     char *buf)
{
    /* 查找属性、设备，并调用对应的 show 方法，传入的参数即是 设备、属性 */
	struct device_attribute *dev_attr = to_dev_attr(attr);
	struct device *dev = to_dev(kobj);
	ssize_t ret = -EIO;

	if (dev_attr->show)
		ret = dev_attr->show(dev, dev_attr, buf);
	if (ret >= (ssize_t)PAGE_SIZE) {
		print_symbol("dev_attr_show: %s returned bad count\n",
				(unsigned long)dev_attr->show);
	}
	return ret;
}

static ssize_t dev_attr_store(struct kobject *kobj, struct attribute *attr,
			      const char *buf, size_t count)
{
    /* 查找属性、设备，并调用对应的 show 方法，传入的参数即是 设备、属性 */
	struct device_attribute *dev_attr = to_dev_attr(attr);
	struct device *dev = to_dev(kobj);
	ssize_t ret = -EIO;

	if (dev_attr->store)
		ret = dev_attr->store(dev, dev_attr, buf, count);
	return ret;
}

/* 
 * device 属性操作函数
 * 添加到 device 的属性，使用 attribute_group ，
 * 配合 device_attribute 定义属性结构，
 * 每个属性都有对应的 show store 方法，使用时比较方便 */
static const struct sysfs_ops dev_sysfs_ops = {
	.show	= dev_attr_show,
	.store	= dev_attr_store,
};

/* device 模块提供的扩展属性结构
 * dev_ext_attribute 结构中提供了一个 void *var 指针
 * 可以指向一个 ulong 或 int 变量 
 * 如果属性操作的是一个 ulong 或 int 变量，
 * 使用 dev_ext_attribute 属性结构可以简化 show store 方法
 * 这类属性的操作全部使用下面的 device_store_ulong 这几个函数 */
#define to_ext_attr(x) container_of(x, struct dev_ext_attribute, attr)

/* ulong 类型的属性 store 方法 */
ssize_t device_store_ulong(struct device *dev,
			   struct device_attribute *attr,
			   const char *buf, size_t size)
{
    /* 通过 device_attribute attr 指针找到 dev_ext_attribute 结构
     * 变量在 dev_ext_attribute 结构中的 var 指针 */
	struct dev_ext_attribute *ea = to_ext_attr(attr);
	char *end;
	unsigned long new = simple_strtoul(buf, &end, 0);
	if (end == buf)
		return -EINVAL;
	*(unsigned long *)(ea->var) = new;
	/* Always return full write size even if we didn't consume all */
	return size;
}
EXPORT_SYMBOL_GPL(device_store_ulong);

/* ulong 类型的属性 show 方法 */
ssize_t device_show_ulong(struct device *dev,
			  struct device_attribute *attr,
			  char *buf)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);
	return snprintf(buf, PAGE_SIZE, "%lx\n", *(unsigned long *)(ea->var));
}
EXPORT_SYMBOL_GPL(device_show_ulong);

/* int 类型的属性 shore 方法 */
ssize_t device_store_int(struct device *dev,
			 struct device_attribute *attr,
			 const char *buf, size_t size)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);
	char *end;
	long new = simple_strtol(buf, &end, 0);
	if (end == buf || new > INT_MAX || new < INT_MIN)
		return -EINVAL;
	*(int *)(ea->var) = new;
	/* Always return full write size even if we didn't consume all */
	return size;
}
EXPORT_SYMBOL_GPL(device_store_int);

/* int 类型的属性 show 方法 */
ssize_t device_show_int(struct device *dev,
			struct device_attribute *attr,
			char *buf)
{
	struct dev_ext_attribute *ea = to_ext_attr(attr);

	return snprintf(buf, PAGE_SIZE, "%d\n", *(int *)(ea->var));
}
EXPORT_SYMBOL_GPL(device_show_int);

/**
 *	device_release - free device structure.
 *	@kobj:	device's kobject.
 *
 *	This is called once the reference count for the object
 *	reaches 0. We forward the call to the device's release
 *	method, which should handle actually freeing the structure.
 */
/* 设备结构释放回调函数
 * 被 kobject_cleanup 函数调用 */
static void device_release(struct kobject *kobj)
{
	struct device *dev = to_dev(kobj);
	struct device_private *p = dev->p;

    /* 以设备自身具有的 release 函数为优先 */
	if (dev->release)
		dev->release(dev);
	else if (dev->type && dev->type->release)
		dev->type->release(dev);
	else if (dev->class && dev->class->dev_release)
		dev->class->dev_release(dev);
	else
		WARN(1, KERN_ERR "Device '%s' does not have a release() "
			"function, it is broken and must be fixed.\n",
			dev_name(dev));
    /* 最终要单独释放私有数据成员 */
	kfree(p);
}

/* 命名空间相关，待分析 */
static const void *device_namespace(struct kobject *kobj)
{
	struct device *dev = to_dev(kobj);
	const void *ns = NULL;

	if (dev->class && dev->class->ns_type)
		ns = dev->class->namespace(dev);

	return ns;
}

/*
static struct attribute xxx_attr = 
{
    .name = "xxx",
};

static struct attribute *default_attrs[] = 
{
    &xxx_attr,
    NULL,
};
 */

/* 所有的 device 都会具有该 ktype 
 * kobj_type 标识了 kobject 的类型，也可以认为是特有的一些属性 
 * 通过让所有的 device 都具有这个类型 
 * 可以统一一些处理，比如资源释放(release) 属性类操作(sysfs_ops) */
/* 将所有的 device 都标识为同一种 ktype,
 * 那使用 DEVICE_ATTR 定义属性结构，并将所有的属性添加到 attribute_group 中
 * 再注册到 device 中，那就可以使用统一的属性处理操作 sysfs_ops 
 * 这样就简化了设备的属性创建和操作
 * 并且，所有的设备都具有相同的 kobj_type，那就可以通过 ktype 判断一个 kobject
 * 是否是 device 了
 * 参考 dev_uevent_filter 函数，用于在发送 uevent 时过滤掉不是 device 的请求 */
/* device_ktype 中并未添加 kobject 的默认属性，默认属性不是必要的
 * 当然，这里也是可以添加的，如下面的 default_attrs 
 * 如果添加了默认属性，那所有的 device 目录下，都会有这个属性文件 */
static struct kobj_type device_ktype = {
	.release	= device_release,
	.sysfs_ops	= &dev_sysfs_ops,
    //.default_attrs  = default_attrs,
	.namespace	= device_namespace,
};


/* uevent 过滤处理函数
 * kset:  kobj 所属的 kset ，可能需要往上好几个父才能找到的
 * kobj:  需要产生 uevent 事件的 kobject
 * 这里简单的判断了需要产生 uevent 事件的 kobj 的 kobj_type 
 * kobj_type 必须得是 device_ktype 这保证了 kobj 是属于一个 设备 device 的
 * 在 device_initialize 函数中会设置    dev->kobj.kset = devices_kset;
                                        kobject_init(&dev->kobj, &device_ktype);
 * 另外，这个设备还必须属于一个 bus 或者 class */
static int dev_uevent_filter(struct kset *kset, struct kobject *kobj)
{
	struct kobj_type *ktype = get_ktype(kobj);

    /* 借助 ktype 判断传入的 kobj 是否属于一个 device */
	if (ktype == &device_ktype) {
		struct device *dev = to_dev(kobj);
		if (dev->bus)
			return 1;
		if (dev->class)
			return 1;
	}
	return 0;
}

/* uevent 获取 dev name 
 * 判断 dev->bus 或 dev->class ，返回对应的 name */
/* 注意，这里的 name 是设置环境变量 SUBSYSTEM=%name
 * SUBSYSTEM 用于标识设备所属的子系统，例如 usb pci block net 等 */
static const char *dev_uevent_name(struct kset *kset, struct kobject *kobj)
{
	struct device *dev = to_dev(kobj);

    /* 返回的是 bus 或 class 的名字
     * 如 platform 设备，bus->name 设置为 platform
     * 那这里返回的就是 platform */
	if (dev->bus)
		return dev->bus->name;
	if (dev->class)
		return dev->class->name;
	return NULL;
}

/* uevent 设置 dev 特有的环境变量
 * 该函数在 kobject_uevent.c 中调用
 * 当需要产生 uevent事件时，先设置了公共的部分环境变量信息，
 * 然后调用 kobj 所属的 kset 特有的 uevent 函数，设置更多的环境变量 
 * 该函数就是 dev 设备驱动中设备所特有的 uevent 处理函数 */
/* 参数：  kset : kobj 所属的 kset 
 *         kobj : 需要产生 uevent 事件的设备的 kobject 
 *         env  : 存放环境变量的缓冲区 */
static int dev_uevent(struct kset *kset, struct kobject *kobj,
		      struct kobj_uevent_env *env)
{
    /* 通过 kobj 反向找所属的 device 
     * 找到 device 就找到更多的信息了 */
	struct device *dev = to_dev(kobj);
	int retval = 0;

    /* 判断设备号，有设备号则添加设备号环境变量 */
	/* add device node properties if present */
	if (MAJOR(dev->devt)) {
		const char *tmp;
		const char *name;
		umode_t mode = 0;

        /* 添加主次设备号环境变量 */
		add_uevent_var(env, "MAJOR=%u", MAJOR(dev->devt));
		add_uevent_var(env, "MINOR=%u", MINOR(dev->devt));
        /* 设备节点的相对路径名
         * 对于大部分设备，这里返回的与设备名一致，如：
         * ttyS0 mtd0 mtd0ro mtdblock0 
         * 某些设备返回的是有设备总线号的，如：
         * usb2 返回的是 bus/usb/002/001
         * usb1 返回的是 bus/usb/001/001
         * 这与设备类型有关 */
        /* 这个就是生成的 /dev 下的 设备节点名 */
		name = device_get_devnode(dev, &mode, &tmp);
		if (name) {
			add_uevent_var(env, "DEVNAME=%s", name);
			kfree(tmp);
			if (mode)
				add_uevent_var(env, "DEVMODE=%#o", mode & 0777);
		}
	}

    /* device_type 中包含 name ， 则需设置 DEVTYPE  */
	if (dev->type && dev->type->name)
		add_uevent_var(env, "DEVTYPE=%s", dev->type->name);

    /* 有对应的驱动，则设置驱动 name */
	if (dev->driver)
		add_uevent_var(env, "DRIVER=%s", dev->driver->name);

	of_device_uevent(dev, env);

    /* 所属的 bus 特有的 uevent ，一般会添加 MODALIAS 环境变量 */
	/* have the bus specific function add its stuff */
	if (dev->bus && dev->bus->uevent) {
		retval = dev->bus->uevent(dev, env);
		if (retval)
			pr_debug("device: '%s': %s: bus uevent() returned %d\n",
				 dev_name(dev), __func__, retval);
	}

    /* class 特有的 uevent  */
	/* have the class specific function add its stuff */
	if (dev->class && dev->class->dev_uevent) {
		retval = dev->class->dev_uevent(dev, env);
		if (retval)
			pr_debug("device: '%s': %s: class uevent() "
				 "returned %d\n", dev_name(dev),
				 __func__, retval);
	}

    /* type 特有的 uevent */
	/* have the device type specific function add its stuff */
	if (dev->type && dev->type->uevent) {
		retval = dev->type->uevent(dev, env);
		if (retval)
			pr_debug("device: '%s': %s: dev_type uevent() "
				 "returned %d\n", dev_name(dev),
				 __func__, retval);
	}

	return retval;
}

/* devices_kset 的 kset_uevent_ops
 * 会被底层的 kobject_uevent.c 中的函数调用 */
static const struct kset_uevent_ops device_uevent_ops = {
	.filter =	dev_uevent_filter,
	.name =		dev_uevent_name,
	.uevent =	dev_uevent,
};

/* 各个 device 下的 uevent 属性 show 方法 
 * 在 openwrt 下的一个示例： 
 * # cat /sys/devices/platform/leds/uevent 
 * DRIVER=leds-gpio
 * OF_NAME=leds
 * OF_FULLNAME=/leds
 * OF_COMPATIBLE_0=gpio-leds
 * OF_COMPATIBLE_N=1
 * MODALIAS=of:NledsT(null)Cgpio-leds
 *
 * # cat /sys/devices/platform/keys/uevent 
 * DRIVER=gpio-keys
 * OF_NAME=keys
 * OF_FULLNAME=/keys
 * OF_COMPATIBLE_0=gpio-keys
 * OF_COMPATIBLE_N=1
 * MODALIAS=of:NkeysT(null)Cgpio-keys
 *
 * # cat /sys/class/block/mtdblock3/uevent
 * MAJOR=31
 * MINOR=3
 * DEVNAME=mtdblock3
 * DEVTYPE=disk
 *
 * */
static ssize_t show_uevent(struct device *dev, struct device_attribute *attr,
			   char *buf)
{
	struct kobject *top_kobj;
	struct kset *kset;
	struct kobj_uevent_env *env = NULL;
	int i;
	size_t count = 0;
	int retval;

    /* 找 dev 的 kobj 所属的顶层 kobj 
     * 对于设备来讲，就是 devices_kset 的 kobj */
	/* search the kset, the device belongs to */
	top_kobj = &dev->kobj;
	while (!top_kobj->kset && top_kobj->parent)
		top_kobj = top_kobj->parent;
	if (!top_kobj->kset)
		goto out;

    /* 顶层 kobj 的 kset ， 就是 devices_kset */
	kset = top_kobj->kset;
	if (!kset->uevent_ops || !kset->uevent_ops->uevent)
		goto out;

    /* 下面的过程是 kobject_uevent.c 中的 kobj_uevent_env 流程的一部分
     * 是 device 专属的 uevent 处理回调 */

    /* filter 回调就是上面的 dev_uevent_filter 函数
     * 用于判断是否为 device_ktype */
	/* respect filter */
	if (kset->uevent_ops && kset->uevent_ops->filter)
		if (!kset->uevent_ops->filter(kset, &dev->kobj))
			goto out;

	env = kzalloc(sizeof(struct kobj_uevent_env), GFP_KERNEL);
	if (!env)
		return -ENOMEM;

    /* uevent 回调就是上面的 dev_uevent 函数
     * 根据 device 添加特定的环境变量 */
	/* let the kset specific function add its keys */
	retval = kset->uevent_ops->uevent(kset, &dev->kobj, env);
	if (retval)
		goto out;

    /* 将环境变量拷贝到 file 中，实现用户空间输出
     * 这里的环境变量与实际发送一个 uevent 是一致的，只是这里是显示出来 */
	/* copy keys to file */
	for (i = 0; i < env->envp_idx; i++)
		count += sprintf(&buf[count], "%s\n", env->envp[i]);
out:
	kfree(env);
	return count;
}

/* uevent 属性 store 节点， 用于产生一个 uevent 事件的
 * 在 /sys/class/block/mtdblock3/uevent 实测， 
 * 通过 "remove" "add" 可以移除、添加 mtdblock3 设备 */
static ssize_t store_uevent(struct device *dev, struct device_attribute *attr,
			    const char *buf, size_t count)
{
	enum kobject_action action;

    /* 应用层产生的 uevent 事件，与 device_add device_del 等函数效果一样 */
	if (kobject_action_type(buf, count, &action) == 0)
		kobject_uevent(&dev->kobj, action);
	else
		dev_err(dev, "uevent: unknown action-string\n");
	return count;
}

/* uevent 属性，每个 device 目录下都会有的属性
 * 在 device_add 时，自动添加的 */
static struct device_attribute uevent_attr =
	__ATTR(uevent, S_IRUGO | S_IWUSR, show_uevent, store_uevent);

/* 向指定设备下添加属性 
 * 因为设备中包含了一个 kobject ，对应到一个目录中 
 * 添加的属性就在这个目录下 
 * 设备对应的 name 会设置到 kobject 的 name 中 dev_set_name 
 * 在 /sys/ 下就会有以设备name 命名的文件夹，下面具有很多的属性 */
static int device_add_attributes(struct device *dev,
				 struct device_attribute *attrs)
{
	int error = 0;
	int i;

    /* 是一组属性，逐个添加 
     * 其中一个出错，则移除所有的 */
	if (attrs) {
		for (i = 0; attr_name(attrs[i]); i++) {
			error = device_create_file(dev, &attrs[i]);
			if (error)
				break;
		}
		if (error)
			while (--i >= 0)
				device_remove_file(dev, &attrs[i]);
	}
	return error;
}

/* 移除指定设备下的属性文件 */
static void device_remove_attributes(struct device *dev,
				     struct device_attribute *attrs)
{
	int i;

	if (attrs)
		for (i = 0; attr_name(attrs[i]); i++)
			device_remove_file(dev, &attrs[i]);
}

/* 添加 bin 类型的属性文件 */
static int device_add_bin_attributes(struct device *dev,
				     struct bin_attribute *attrs)
{
	int error = 0;
	int i;

	if (attrs) {
		for (i = 0; attr_name(attrs[i]); i++) {
			error = device_create_bin_file(dev, &attrs[i]);
			if (error)
				break;
		}
		if (error)
			while (--i >= 0)
				device_remove_bin_file(dev, &attrs[i]);
	}
	return error;
}

/* 指定设备下的 bin 属性文件 */
static void device_remove_bin_attributes(struct device *dev,
					 struct bin_attribute *attrs)
{
	int i;

	if (attrs)
		for (i = 0; attr_name(attrs[i]); i++)
			device_remove_bin_file(dev, &attrs[i]);
}

/* 指定设备下添加属性组 */
static int device_add_groups(struct device *dev,
			     const struct attribute_group **groups)
{
	int error = 0;
	int i;

	if (groups) {
		for (i = 0; groups[i]; i++) {
			error = sysfs_create_group(&dev->kobj, groups[i]);
			if (error) {
				while (--i >= 0)
					sysfs_remove_group(&dev->kobj,
							   groups[i]);
				break;
			}
		}
	}
	return error;
}

/* 指定设备下移除属性组 */
static void device_remove_groups(struct device *dev,
				 const struct attribute_group **groups)
{
	int i;

	if (groups)
		for (i = 0; groups[i]; i++)
			sysfs_remove_group(&dev->kobj, groups[i]);
}

/* 针对一个特定的设备，添加其拥有的属性
 * 一个设备可能属于一个class，所以拥有公共的 class属性 
 * 还可能有特定的类型 device_type， 如 part_type ，也有单独的属性 
 * 还可能有自身特有的一些属性， dev->groups */
static int device_add_attrs(struct device *dev)
{
	struct class *class = dev->class;
	const struct device_type *type = dev->type;
	int error;

    /* 属于某个 class ，那需要把 class 的所有属性添加上
     * 注意，class 中属于 device 的属性，定义的结构使用的是 
     * device_attribute 
     *
     * class 中有两种属于 device 的属性，一种普通属性，一种bin属性
     * 两种属性所在的结构不同，分别处理 */
	if (class) {
		error = device_add_attributes(dev, class->dev_attrs);
		if (error)
			return error;
		error = device_add_bin_attributes(dev, class->dev_bin_attrs);
		if (error)
			goto err_remove_class_attrs;
	}

    /* 设备所属的 device_type 可能也带有属性
     * 是某类型设备共有的 
     * 可以参考 mtdcore.c 中的 mtd_devtype */
	if (type) {
		error = device_add_groups(dev, type->groups);
		if (error)
			goto err_remove_class_bin_attrs;
	}

    /* 设备私有的属性，在 groups 中 */
	error = device_add_groups(dev, dev->groups);
	if (error)
		goto err_remove_type_groups;

	return 0;

 err_remove_type_groups:
	if (type)
		device_remove_groups(dev, type->groups);
 err_remove_class_bin_attrs:
	if (class)
		device_remove_bin_attributes(dev, class->dev_bin_attrs);
 err_remove_class_attrs:
	if (class)
		device_remove_attributes(dev, class->dev_attrs);

	return error;
}

/* 移除指定设备的属性文件，包含了 class type */
static void device_remove_attrs(struct device *dev)
{
	struct class *class = dev->class;
	const struct device_type *type = dev->type;

	device_remove_groups(dev, dev->groups);

	if (type)
		device_remove_groups(dev, type->groups);

	if (class) {
		device_remove_attributes(dev, class->dev_attrs);
		device_remove_bin_attributes(dev, class->dev_bin_attrs);
	}
}


/* 打印设备号 方法 
 * 每个 device 都会具有这个属性，用于查看其设备号 */
static ssize_t show_dev(struct device *dev, struct device_attribute *attr,
			char *buf)
{
	return print_dev_t(buf, dev->devt);
}

/* dev 属性在 device_add 函数中添加，所有设备都会有该属性 */
static struct device_attribute devt_attr =
	__ATTR(dev, S_IRUGO, show_dev, NULL);

/* kset to create /sys/devices/  */
/* 指向 devices 的 kset  */
/* 所有的 device 的 kset 都指向 devices_kset
 * 这样就都可以发送 uevent 了 */
struct kset *devices_kset;

/**
 * device_create_file - create sysfs attribute file for device.
 * @dev: device.
 * @attr: device attribute descriptor.
 */
/* 专用于设备驱动的，创建 sysfs 的属性文件
 * 实现方式是在 device 的 kobject 下直接创建 属性文件 
 * 直接调用 sysfs_create_file 
 * 导出的符号，被很多驱动调用 */
int device_create_file(struct device *dev,
		       const struct device_attribute *attr)
{
	int error = 0;
	if (dev)
		error = sysfs_create_file(&dev->kobj, &attr->attr);
	return error;
}

/**
 * device_remove_file - remove sysfs attribute file.
 * @dev: device.
 * @attr: device attribute descriptor.
 */
/* 专用于设备驱动的，移除 sysfs 的属性文件 
 * 直接调用 sysfs_remove_file 实现 */
void device_remove_file(struct device *dev,
			const struct device_attribute *attr)
{
	if (dev)
		sysfs_remove_file(&dev->kobj, &attr->attr);
}

/**
 * device_create_bin_file - create sysfs binary attribute file for device.
 * @dev: device.
 * @attr: device binary attribute descriptor.
 */
/* 专用于设备驱动的，创建 bin 类型的属性文件 */
int device_create_bin_file(struct device *dev,
			   const struct bin_attribute *attr)
{
	int error = -EINVAL;
	if (dev)
		error = sysfs_create_bin_file(&dev->kobj, attr);
	return error;
}
EXPORT_SYMBOL_GPL(device_create_bin_file);

/**
 * device_remove_bin_file - remove sysfs binary attribute file
 * @dev: device.
 * @attr: device binary attribute descriptor.
 */
/* 专用于设备驱动的，移除 bin 类型属性文件 */
void device_remove_bin_file(struct device *dev,
			    const struct bin_attribute *attr)
{
	if (dev)
		sysfs_remove_bin_file(&dev->kobj, attr);
}
EXPORT_SYMBOL_GPL(device_remove_bin_file);

/**
 * device_schedule_callback_owner - helper to schedule a callback for a device
 * @dev: device.
 * @func: callback function to invoke later.
 * @owner: module owning the callback routine
 *
 * Attribute methods must not unregister themselves or their parent device
 * (which would amount to the same thing).  Attempts to do so will deadlock,
 * since unregistration is mutually exclusive with driver callbacks.
 *
 * Instead methods can call this routine, which will attempt to allocate
 * and schedule a workqueue request to call back @func with @dev as its
 * argument in the workqueue's process context.  @dev will be pinned until
 * @func returns.
 *
 * This routine is usually called via the inline device_schedule_callback(),
 * which automatically sets @owner to THIS_MODULE.
 *
 * Returns 0 if the request was submitted, -ENOMEM if storage could not
 * be allocated, -ENODEV if a reference to @owner isn't available.
 *
 * NOTE: This routine won't work if CONFIG_SYSFS isn't set!  It uses an
 * underlying sysfs routine (since it is intended for use by attribute
 * methods), and if sysfs isn't available you'll get nothing but -ENOSYS.
 */
/* // TODO: 待分析，回调机制
 * 使用的驱动模块很少，暂不分析 */
int device_schedule_callback_owner(struct device *dev,
		void (*func)(struct device *), struct module *owner)
{
	return sysfs_schedule_callback(&dev->kobj,
			(void (*)(void *)) func, dev, owner);
}
EXPORT_SYMBOL_GPL(device_schedule_callback_owner);

/* 遍历子设备时的 get 回调
 * 是通过 子设备的 klist_node 找到子设备 dev，递增引用计数 */
static void klist_children_get(struct klist_node *n)
{
    /* 通过 klist_node 找到 device_private 结构 */
	struct device_private *p = to_device_private_parent(n);
    /* device_private 结构中 dev 指向包含它的 device 结构 */
	struct device *dev = p->device;

    /* 递增引用计数 */
	get_device(dev);
}

/* 遍历子设备时的 put 回调
 * 是通过 子设备的 klist_node 找到子设备 dev，递减引用计数 */
static void klist_children_put(struct klist_node *n)
{
    /* 通过 klist_node 找到 device_private 结构 */
	struct device_private *p = to_device_private_parent(n);
    /* device_private 结构中 dev 指向包含它的 device 结构 */
	struct device *dev = p->device;

    /* 递增引用计数 */
	put_device(dev);
}

/**
 * device_initialize - init device structure.
 * @dev: device.
 *
 * This prepares the device for use by other layers by initializing
 * its fields.
 * It is the first half of device_register(), if called by
 * that function, though it can also be called separately, so one
 * may use @dev's fields. In particular, get_device()/put_device()
 * may be used for reference counting of @dev after calling this
 * function.
 *
 * All fields in @dev must be initialized by the caller to 0, except
 * for those explicitly set to some other value.  The simplest
 * approach is to use kzalloc() to allocate the structure containing
 * @dev.
 *
 * NOTE: Use put_device() to give up your reference instead of freeing
 * @dev directly once you have called this function.
 */
/* 
 * device_initialize - 初始化 device 结构体
 *
 * 这将通过初始化其字段来准备设备，以便其它层使用。
 *
 * 参数 dev 中的所有字段必须由调用者初始化为0，除了那些显式设置为其他值的字段。
 * 最简单的方法是使用 kzalloc() 来分配包含 dev 的结构 */
/* 该函数为 device_register 的前半部分，当然，也是可以单独调用的
 * 使用 get_device() / put_device() 可以递增 dev 的引用计数 */
void device_initialize(struct device *dev)
{
    /* 设置device 的 kobject 的 所属 kset 
     * 所有的 dev 都属于 devices_kset , 都在 /sys/devices/ 目录下 */
	dev->kobj.kset = devices_kset;
    /* dev 的 kobject 初始化，设置 ktype 为 device_ktype 
     * 这样，device 的属性都会有统一的处理方式 */
	kobject_init(&dev->kobj, &device_ktype);
    /* dma 相关的链表头 */
	INIT_LIST_HEAD(&dev->dma_pools);
	mutex_init(&dev->mutex);
    /* 未使用，暂不分析 */
	lockdep_set_novalidate_class(&dev->mutex);
	spin_lock_init(&dev->devres_lock);
	INIT_LIST_HEAD(&dev->devres_head);
    /* 电源管理相关，暂不分析 */
	device_pm_init(dev);
    /* NUMA 相关，未定义 NUMA 相关宏 */
	set_dev_node(dev, -1);
}

/* 创建 /sys/devices/virtual/ 目录 
 * 这个目录存放的是，属于某个 class，但并未特别指定父设备的设备
 * 所以，这个目录下 都是以 class name 命名的目录 */
static struct kobject *virtual_device_parent(struct device *dev)
{
	static struct kobject *virtual_dir = NULL;

    /* 创建 virtual 目录，父为 devices_kset->kobj  */
	if (!virtual_dir)
		virtual_dir = kobject_create_and_add("virtual",
						     &devices_kset->kobj);

	return virtual_dir;
}

/* 用于 class 的 glue_dirs (胶合目录) 管理
 * kobj 用于创建一个 胶合目录，用于做为某类设备的父 kobject
 * class 指向这个 class_dir 所属的 class */
struct class_dir {
	struct kobject kobj;
	struct class *class;
};

/* 使用 kobj 反查 class_dir 结构，用于在 release 时使用 */
#define to_class_dir(obj) container_of(obj, struct class_dir, kobj)

/* class_dir 的 kobject 的 解引用 release 回调
 * 用于释放 class_dir 结构 */
static void class_dir_release(struct kobject *kobj)
{
    /* 反查 class_dir 结构，并释放内存 */
	struct class_dir *dir = to_class_dir(kobj);
	kfree(dir);
}

/* 反查 class_dir 所属的 class 的 ns_type */
static const
struct kobj_ns_type_operations *class_dir_child_ns_type(struct kobject *kobj)
{
    /* 反查 class_dir 结构，并返回 class->ns_type 指针 */
	struct class_dir *dir = to_class_dir(kobj);
	return dir->class->ns_type;
}

/* class_dir 的 kobject 的 ktype
 * 这是专用于 class glue_dirs 的 ktype
 * 新创建的 class_dir 的 kobject 需要有单独的 release 回调和sysfs_ops */
/* glue_dirs 胶合目录中，也可以创建 属性文件  */
static struct kobj_type class_dir_ktype = {
	.release	= class_dir_release,
	.sysfs_ops	= &kobj_sysfs_ops,
	.child_ns_type	= class_dir_child_ns_type
};

/* 创建 class 命名的目录，并且添加到 sysfs 中
 * 提供的 parent_kobj 是父kobj，也是 sysfs 中的目录层次
 * 这是用来创建 胶合目录 的 */
static struct kobject *
class_dir_create_and_add(struct class *class, struct kobject *parent_kobj)
{
	struct class_dir *dir;
	int retval;

    /* 创建一个 class_dir 结构，由该结构构建新的目录 */
	dir = kzalloc(sizeof(*dir), GFP_KERNEL);
	if (!dir)
		return NULL;

    /* 新创建的目录，记录一下所属的 class */
	dir->class = class;
    /* 初始化新目录的 kobj, 类型设置为 class_dir_ktype
     * 标识这是一个 class_dir, 主要得有一个 release 回调 */
	kobject_init(&dir->kobj, &class_dir_ktype);

    /* 新创建的 kobj 属于 class 的 glue_dirs 这个 kset
     * glue_dirs 是一个 "胶合" 目录，专门记录新创建的这些 kobj
     * 注意，这里的 glue_dirs 虽然是个 kset 但并未注册到 sysfs 中
     * 也就是说，sysfs 中没有对应目录
     * 这里只是使用 glue_dirs 做为一个链表头，记录所有属于该class的胶合目录 */
	dir->kobj.kset = &class->p->glue_dirs;

    /* 创建一个以 class name 命名的 kobj, 以 parent_kobj 为父 kobj
     * 并且添加到 sysfs 系统中
     * 
     * 假设提供的父为 virtual 目录，class 为 gpio_class
     * 那这里创建的就是 /sys/devices/virtual/gpio 目录 */
	retval = kobject_add(&dir->kobj, parent_kobj, "%s", class->name);
	if (retval < 0) {
		kobject_put(&dir->kobj);
		return NULL;
	}
    /* 将新创建的 kobject 返回  */
	return &dir->kobj;
}


/* 给设备找一个父, 设备一般都会有个父
 * 比如各种平台设备，它们的父是 platform 设备
 * 设备与父设备，在 /sys 中组成了目录层次结构 */
static struct kobject *get_device_parent(struct device *dev,
					 struct device *parent)
{
    /* 设备属于某个 class
     * 很多设备都会属于某个 class 的，比如 leds  input  gpio  rtc  mtd
     * 如果使用内核已经提供的设备驱动模块接口，都会将设备归属某个 class
     * 比如注册 leds 设备，input 设备等 */
	if (dev->class) {
		static DEFINE_MUTEX(gdp_mutex);
		struct kobject *kobj = NULL;
		struct kobject *parent_kobj;
		struct kobject *k;

#ifdef CONFIG_BLOCK
        /* 配置了具有 /sys/block/ 目录，并且设备属于 block 设备
         * 这种情况需要单独处理一下，block设备比较特殊 */
        /* block 设备的归类目录，因历史原因，有 /sys/block/ 
         * 和 /sys/class/block/ 两个，可以参考 sysfs_deprecated */
		/* block disks show up in /sys/block */
		if (sysfs_deprecated && dev->class == &block_class) {
            /* 如果设备本身是有父的，并且父设备也是一个 block 设备类
             * 那这种情况就直接使用指定的父设备就好了 */
			if (parent && parent->class == &block_class)
				return &parent->kobj;
            /* 没有父设备，或者父设备不是 block 类
             * 这时把 基本的 block_class 做为父设备
             * 这对应的目录为 /sys/class/block/ */
			return &block_class.p->subsys.kobj;
		}
#endif

		/*
		 * If we have no parent, we live in "virtual".
		 * Class-devices with a non class-device as parent, live
		 * in a "glue" directory to prevent namespace collisions.
		 */
        /* 下面处理的情况就比较复杂了
         * 1、如果没有父设备，那把设备放在 "virtual" 目录中，这里并不是把
         * virtual 当作父设备，而是要放在这个目录中，以目录中的某个子目录做为父
         * 目录
         * 2、设备是有父的，并且父有 class，另外 设备本身不属于某个 ns_type
         * 这种情况是，设备没有在某个命名空间中，而且父也是正常的某个类设备
         * 这样就以自己的父做为父设备（很合理啊）
         * 3、其它情况，一律先以指定的父设备做为父设备参考；注意，这里并不会
         * 直接将父设备做为父设备，而是在 父设备的目录下创建 glue_dirs（胶合）
         * 目录，此目录以 设备所属的class 命名。
         * 比如本代码中的 leds 设备，本代码中，注册了两个 leds 设备: sys  mmcblk0
         * 它们都属 leds_class ，该类名为 leds, 它们的父设备是 leds-gpio
         * 注意哈，如果直接以 leds-gpio 做为父设备，但 leds-gpio 设备目录下本身
         * 就已经有一些属性文件了，如果注册的 leds 设备与属性文件重名了，那就不
         * 对了啊，所以，这里就创建了 "胶合" 目录，以 class name 命名，相当于是
         * 给子设备增加了一个单独的目录
         * leds 的目录如下： (/sys/devices/platform/)
         * # tree leds-gpio/
         * leds-gpio/
         * |-- driver -> ../../../bus/platform/drivers/leds-gpio
         * |-- leds
         * |   |-- mmcblk0
         * |   |   |-- brightness
         * |   |   |-- device -> ../../../leds-gpio
         * |   |   |-- max_brightness
         * |   |   |-- power
         * |   |   |-- subsystem -> ../../../../../class/leds
         * |   |   |-- trigger
         * |   |   `-- uevent
         * |   `-- sys
         * |       |-- brightness
         * |       |-- device -> ../../../leds-gpio
         * |       |-- max_brightness
         * |       |-- power
         * |       |-- subsystem -> ../../../../../class/leds
         * |       |-- trigger
         * |       `-- uevent
         * |-- modalias
         * |-- power
         * |-- subsystem -> ../../../bus/platform
         * `-- uevent
         *
         * 类似这种情况有很多，比如 usb 设备的子设备，有 block  net 等等类型
         * 如果都直接放在 usb设备目录下，那重名概率很大，但增加了 class 目录，就
         * 把各种类型都分开了
         *
         * 在本代码中， mmc leds input 都是这种情况
         *
         * 在下面的三种处理中，第一种和第三种情况，parent_kobj 都是一个参考父设
         * 备，是要在这个目录中找设备的父目录
         * 比如，/sys/devices/virtual 目录，是找到新注册的设备所属的 class的胶合
         * 目录，是否在 /sys/devices/virtual/ 目录中已经有了
         * 设备所属的某个class 可能会有多个胶合目录（属于不同的设备,比如
         * USB/SDHC 等总线，都可以有 block）
         * 但需要找到这些目录的父是指定目录的（/sys/devices/virtual)
         *
         * 比如 input 这个class，有多种设备可以注册为 input
         * 那就可能在多个设备下 有input 目录，但每个 input 目录的父是不一样的
         * /sys/devices/virtual/input
         * /sys/devices/platform/gpio-keys.0/input/ */
		if (parent == NULL)
			parent_kobj = virtual_device_parent(dev);
		else if (parent->class && !dev->class->ns_type)
			return &parent->kobj;
		else
			parent_kobj = &parent->kobj;

        /* gdp_mutex 这玩意保护的啥？
         * 还是个静态的，大家都公用的，保护 parent_kobj 的话，不应该放到上面吗？？ */
		mutex_lock(&gdp_mutex);

		/* find our class-directory at the parent and reference it */
        /* 这里就是找 parent_kobj 这个目录下的 class 同名的 glue_dirs
         * 所有的 kobject 都会链接到 glue_dirs 中
         * 如果设备所属的class 的某个 glue_dirs 的父 与 指定的(parent_kobj) 一致
         * 就说明这个是在 parent_kobj 目录下的 class 同名的 胶合目录 */
		spin_lock(&dev->class->p->glue_dirs.list_lock);
		list_for_each_entry(k, &dev->class->p->glue_dirs.list, entry)
			if (k->parent == parent_kobj) {
                /* 找到合适的 kobject 了，就使用这个
                 * 这就是 新注册的设备的父设备了 */
				kobj = kobject_get(k);
				break;
			}
		spin_unlock(&dev->class->p->glue_dirs.list_lock);
		if (kobj) {
            /* 找到了有效的父设备，则直接返回 */
			mutex_unlock(&gdp_mutex);
			return kobj;
		}

		/* or create a new class-directory at the parent device */
        /* 到这里了，就说明在 glue_dirs 中并未找到
         * 这种情况主要是在第一次注册这类设备时，此时需要创建一个 */
        /* 创建好了，就使用新创建的这个目录做为父设备了 */
		k = class_dir_create_and_add(dev->class, parent_kobj);
		/* do not emit an uevent for this simple "glue" directory */
		mutex_unlock(&gdp_mutex);
		return k;
	}

    /* 设备不属于任何 class ，但设备属于某个 bus 时(并没有指定父设备)
     * 注意下面的判断条件，属于某个 bus， 并且 bus 的 dev_root 也是有效的
     * dev_root 是在注册 subsys 类型的 bus 时才会设置的
     * dev_root 是 subsys bus 的根设备
     * 也就是说，这个设备是属于某个 subsys bus 总线的
     * 这时就将 subsys bus 的 dev_root 设置为 设备的父设备
     *
     * 在 EXYNOS4 设备中，使用这种方式的设备有：
     * clocksource    cpu    exynos-core  这三种
     * 目录在 /sys/devices/system/ 中
     * 可以参考 arch/arm/mach-exynos/common.c 中的代码
     * 注册了一个 exynos_subsys 类型的 subsys bus
     * 需要注册的设备 exynos4_dev 属于 exynos_subsys 这个 bus
     * 那在注册这个 exynos4_dev 这个设备时，就会使用 subsys 的 dev_root
     * 做为父设备
     * sysfs 中的目录为 ： /sys/devices/system/exynos-core/exynos-core0 */
	/* subsystems can specify a default root directory for their devices */
	if (!parent && dev->bus && dev->bus->dev_root)
		return &dev->bus->dev_root->kobj;

    /* 设备不属于任何 class，并且有指定的父设备
     * 那这种情况就直接使用指定的父设备就好了
     * 各种直接注册的 platform 设备都是这种情况
     * 一些使用设备类的方法注册的设备，会属于某个 class
     * 比如注册的 mtd 设备，它属于 mtd class， input 设备属于 input clas */
	if (parent)
		return &parent->kobj;
    /* 什么都不符合，那就是没有父设备了，直接返回 NULL */
	return NULL;
}

/* 清理设备的 glue_dir 父目录
 * 不是所有设备都有的，只有属于某个 class 的才可能有 */
static void cleanup_glue_dir(struct device *dev, struct kobject *glue_dir)
{
	/* see if we live in a "glue" directory */
    /* 如果有 glue_dir 的
     * 设备必须属于某个 class
     * 并且 glue_dir 的 kset 必须是 设备所属 class 的 glue_dirs */
	if (!glue_dir || !dev->class ||
	    glue_dir->kset != &dev->class->p->glue_dirs)
		return;

    /* 减少其引用计数，具体的释放在 class_dir_release 中处理 */
	kobject_put(glue_dir);
}

/* 清理设备的 父目录项
 * 专用于 glue_dirs 目录的解引用 */
static void cleanup_device_parent(struct device *dev)
{
	cleanup_glue_dir(dev, dev->kobj.parent);
}

/* 添加设备相关的 class 符号链接
 * 在 /sys 目录下，devices class 的目录中
 * 有些目录是互相指向的符号链接，就是在这里创建的
 * 下面是几个例子：
 *
 * /sys/devices/virtual/mtd/mtd0 目录中，subsystem 链接:
 * subsystem -> ../../../../class/mtd 
 * subsystem 链接到了 class 目录中的 mtd 目录
 *
 * 在 /sys/class/mtd 目录中， mtd0 链接：
 * mtd0 -> ../../devices/virtual/mtd/mtd0 
 *
 * 在 /sys/devices/virtual/mtd/mtd0/mtdblock0 目录中， device 链接：
 * device -> ../../mtd0 */
static int device_add_class_symlinks(struct device *dev)
{
	int error;

    /* 必须要属于某个 class
     * 创建的链接依赖 class */
	if (!dev->class)
		return 0;

    /* 创建 subsystem 链接，指向了所属类的 subsys  */
	error = sysfs_create_link(&dev->kobj,
				  &dev->class->p->subsys.kobj,
				  "subsystem");
	if (error)
		goto out;

    /* 有父 parent 的设备，并且不是分区设备
     * 创建 device 链接，指向自己的父 */
	if (dev->parent && device_is_not_partition(dev)) {
		error = sysfs_create_link(&dev->kobj, &dev->parent->kobj,
					  "device");
		if (error)
			goto out_subsys;
	}

#ifdef CONFIG_BLOCK
    /* /sys/block/ 目录中，有了块设备的符号链接
     * mtdblock0 -> ../devices/virtual/mtd/mtd0/mtdblock0 */
	/* /sys/block has directories and does not need symlinks */
	if (sysfs_deprecated && dev->class == &block_class)
		return 0;
#endif

	/* link in the class directory pointing to the device */
    /* class 目录中创建以设备命名的符号链接，指向真实的设备目录  */
	error = sysfs_create_link(&dev->class->p->subsys.kobj,
				  &dev->kobj, dev_name(dev));
	if (error)
		goto out_device;

	return 0;

out_device:
	sysfs_remove_link(&dev->kobj, "device");

out_subsys:
	sysfs_remove_link(&dev->kobj, "subsystem");
out:
	return error;
}

/* 移除设备相关的 class 符号链接 */
static void device_remove_class_symlinks(struct device *dev)
{
	if (!dev->class)
		return;

	if (dev->parent && device_is_not_partition(dev))
		sysfs_remove_link(&dev->kobj, "device");
	sysfs_remove_link(&dev->kobj, "subsystem");
#ifdef CONFIG_BLOCK
	if (sysfs_deprecated && dev->class == &block_class)
		return;
#endif
	sysfs_delete_link(&dev->class->p->subsys.kobj, &dev->kobj, dev_name(dev));
}

/**
 * dev_set_name - set a device name
 * @dev: device
 * @fmt: format string for the device's name
 */
/* 设置 device 名
 * 真正设置的是 device 内嵌的 kobject 的 name */
int dev_set_name(struct device *dev, const char *fmt, ...)
{
	va_list vargs;
	int err;

	va_start(vargs, fmt);
	err = kobject_set_name_vargs(&dev->kobj, fmt, vargs);
	va_end(vargs);
	return err;
}
EXPORT_SYMBOL_GPL(dev_set_name);

/**
 * device_to_dev_kobj - select a /sys/dev/ directory for the device
 * @dev: device
 *
 * By default we select char/ for new entries.  Setting class->dev_obj
 * to NULL prevents an entry from being created.  class->dev_kobj must
 * be set (or cleared) before any devices are registered to the class
 * otherwise device_create_sys_dev_entry() and
 * device_remove_sys_dev_entry() will disagree about the presence of
 * the link.
 */
/* 为 设备 选择 /sys/dev/ 目录
 * 根据设备所属 class 选择目录
 * */
static struct kobject *device_to_dev_kobj(struct device *dev)
{
	struct kobject *kobj;

    /* 如果设备具有 class， 那根据设备的 class 选择
     * 没有归属 class 的设备，一律是 /sys/dev/char/ 目录 */
    /* 这里就两种情况，sysfs_dev_char_kobj  sysfs_dev_block_kobj
     * 可参考 struct class 结构体定义中 成员作用分析 */
	if (dev->class)
		kobj = dev->class->dev_kobj;
	else
		kobj = sysfs_dev_char_kobj;

	return kobj;
}

/* 设备在 /sys/dev/ 目录下创建链接文件
 * 所有的设备，在 /sys/dev/ 目录下都有以设备号命名的链接文件
 * 链接到实际的设备目录 */
static int device_create_sys_dev_entry(struct device *dev)
{
    /* 找到链接创建的目标目录 /sys/dev/block/ 或 /sys/dev/char/ */
	struct kobject *kobj = device_to_dev_kobj(dev);
	int error = 0;
	char devt_str[15];

	if (kobj) {
        /* 以设备组成链接文件名，形式如： 253:0  */
		format_dev_t(devt_str, dev->devt);
		error = sysfs_create_link(kobj, &dev->kobj, devt_str);
	}

	return error;
}

/* 移除 /sys/dev/ 目录下的设备链接文件 */
static void device_remove_sys_dev_entry(struct device *dev)
{
	struct kobject *kobj = device_to_dev_kobj(dev);
	char devt_str[15];

	if (kobj) {
		format_dev_t(devt_str, dev->devt);
		sysfs_remove_link(kobj, devt_str);
	}
}

/* device 结构私有数据初始化
 * 分配 device_private 结构，并初始化 */
int device_private_init(struct device *dev)
{
    /* 分配 device_private 结构体 */
	dev->p = kzalloc(sizeof(*dev->p), GFP_KERNEL);
	if (!dev->p)
		return -ENOMEM;
    /* 回指包含本结构的 device 结构 */
	dev->p->device = dev;
    /* klist 相关初始化，提供了两个回调 get put  */
    /* klist_children 是本设备的子设备链表
     * 用来挂接属于这个设备的子设备
     * 提供了 get put 两个回调 */
	klist_init(&dev->p->klist_children, klist_children_get,
		   klist_children_put);
	INIT_LIST_HEAD(&dev->p->deferred_probe);
	return 0;
}

/**
 * device_add - add device to device hierarchy.
 * @dev: device.
 *
 * This is part 2 of device_register(), though may be called
 * separately _iff_ device_initialize() has been called separately.
 *
 * This adds @dev to the kobject hierarchy via kobject_add(), adds it
 * to the global and sibling lists for the device, then
 * adds it to the other relevant subsystems of the driver model.
 *
 * Do not call this routine or device_register() more than once for
 * any device structure.  The driver model core is not designed to work
 * with devices that get unregistered and then spring back to life.
 * (Among other things, it's very hard to guarantee that all references
 * to the previous incarnation of @dev have been dropped.)  Allocate
 * and register a fresh new struct device instead.
 *
 * NOTE: _Never_ directly free @dev after calling this function, even
 * if it returned an error! Always use put_device() to give up your
 * reference instead.
 */
/* 将设备添加到设备层次结构中
 *
 * 这是 device_register() 函数的第二部分，如果 device_initialize() 已经单独调用
 * 那本函数也是可以单独调用的
 *
 * 这将通过 kobject_add() 将 @dev 添加到 kobject 层次结构中，
 * 将其添加到设备的全局和兄弟列表中，然后将其添加到驱动程序模型的其它相关子系统
 * 中
 *
 * 对于任何设备结构，不要多次调用这个函数或 device_register().
 * 驱动程序模型核心不是设计用来处理未注册然后又恢复正常的设备的。
 * （除此之外，很难保证所有对 @dev 之前版本的引用都被删除了）
 * 需要多次注册时，分配并注册一个新的设备结构体 */
int device_add(struct device *dev)
{
	struct device *parent = NULL;
	struct kobject *kobj;
	struct class_interface *class_intf;
	int error = -EINVAL;

    /* 递增 dev 的引用计数，确保不会被释放 */
    /* 在设备的 device_initialize 初始化时，引用计数已经设置为 1 了
     * 初始化时设置为1，是可以在以后需要释放时还能 减 1，不然 为0 怎么减！！！
     * 这里需要使用 设备结构device 了，就是一次新的引用了
     * 所以这里需要递增一下，避免被误释放 */
	dev = get_device(dev);
	if (!dev)
		goto done;

    /* 未分配私有结构的，分配并初始化 */
	if (!dev->p) {
		error = device_private_init(dev);
		if (error)
			goto done;
	}

	/*
	 * for statically allocated devices, which should all be converted
	 * some day, we need to initialize the name. We prevent reading back
	 * the name, and force the use of dev_name()
	 */
    /* 对于静态分配的设备，需要初始化名称。
     * 为了防止回读名称，强制使用 dev_name()
     * 静态分配的，使用 init_name 初始化设备名
     * 这里会将 init_name 设备名 设置到 kobject 中
     * 并将静态的设备名 init_name 设置为 NULL */
	if (dev->init_name) {
		dev_set_name(dev, "%s", dev->init_name);
        /* 这里把 init_name 设置为 NULL
         * 应该是为了只有一个地方存放设备名（kobject）
         * 避免名字冗余，差异吧。。。
         * 所有获取名字的地方，都应该使用 dev_name() */
		dev->init_name = NULL;
	}

	/* subsystems can specify simple device enumeration */
    /* 对于未设置设备名的，子系统可以指定简单的设备枚举
     * 按照设备所在的 bus 设置一个名称 */
	if (!dev_name(dev) && dev->bus && dev->bus->dev_name)
		dev_set_name(dev, "%s%u", dev->bus->dev_name, dev->id);

    /* 无有效的设备名，返回失败
     * 注意，设备名可以在注册设备之前，使用 dev_set_name 先设置 
     * 这样就可以自主设置需要的设备名了
     * 使用 dev_set_name 是大部分驱动的做法 */
	if (!dev_name(dev)) {
		error = -EINVAL;
		goto name_error;
	}

	pr_debug("device: '%s': %s\n", dev_name(dev), __func__);

    /* 递增父的引用计数，避免父先被释放
     * 注意，不是所有的设备都有父，这里的 parent 可能为 NULL */
	parent = get_device(dev->parent);
    /* get_device_parent 是为待注册的 dev 寻找一个 parent
     * 当然，有些 dev 是没有父的，比如 platform 这个设备 
     * platform 这个设备是为了给其它设备做父的
     * 这个父也会体现在 设备模型 层次结构中，以目录形式体现 */
    /* 在 NANOPC-T1 中实测，无 parent 的 dev 有：
     * platform  cpu  exynos-core  clocksource  breakpoint  software
     * 其它的各 dev 都是有父的，比如 mtd0 mtd0ro 的父为 mtd 
     * mtdblock0 的父为 mtd0 */
	kobj = get_device_parent(dev, parent);
	if (kobj)
		dev->kobj.parent = kobj;

	/* use parent numa_node */
    /* 无 NUMA 配置，不分析 */
	if (parent)
		set_dev_node(dev, dev_to_node(parent));

	/* first, register with generic layer. */
	/* we require the name to be set before, and pass NULL */
    /* 首先，注册通用层
     * 要求在这之前设置名称，并在 kobject_add 时传递 NULL
     * 这里将 dev 内嵌的 kobj 添加到 设备模型层次结构 中了，
     * 就在 /sys 目录中有了目录、文件了，有了层次了 */
	error = kobject_add(&dev->kobj, dev->kobj.parent, NULL);
	if (error)
		goto Error;

	/* notify platform of device entry */
    /* platform_notify 回调，仅在 acpi 驱动中使用了 */
	if (platform_notify)
		platform_notify(dev);

    /* 所有注册的 device 都会添加 uevent 属性 */
	error = device_create_file(dev, &uevent_attr);
	if (error)
		goto attrError;

    /* 有实际有效的设备号，则添加 dev 属性
     * dev 属性可以查看设备的设备号 */
	if (MAJOR(dev->devt)) {
		error = device_create_file(dev, &devt_attr);
		if (error)
			goto ueventattrError;

        /* 创建 /sys/dev/ 目录下的设备链接文件 */
		error = device_create_sys_dev_entry(dev);
		if (error)
			goto devtattrError;

        /* devtmpfs 文件系统下创建设备节点
         * 如果 devtmpfs 可用，那在 /dev 目录下就已经有本设备的设备节点了
         * 应用层就已经能看到了
         * 当然，如果还有 mdev udev 等机制，设备节点可能还会做一些调整 */
		devtmpfs_create_node(dev);
	}

    /* 创建设备相关的 class 符号链接 */
	error = device_add_class_symlinks(dev);
	if (error)
		goto SymlinkError;
    /* 添加设备的属性文件
     * 这些属性都会在设备文件夹中显示 */
	error = device_add_attrs(dev);
	if (error)
		goto AttrsError;
    /* 向 bus 总线添加 device
     * 注册的bus就是 dev->bus 指定的
     * 注意，不是所有的 device 都会有 bus 的
     * 比如在 platform 初始化时，会注册 platform device, 就是没有 bus 的
     * 没有 bus 的 device, 下面的函数会直接返回
     * 有有效 bus 的 device，才会真正注册到bus上 */
	error = bus_add_device(dev);
	if (error)
		goto BusError;
    /* // TODO:  dpm_sysfs_add 待详细分析
     * dpm_sysfs_add 与电源管理有关，在设备目录下创建了 power 属性目录
     * 另外处理了一些电源管理相关操作 */
	error = dpm_sysfs_add(dev);
	if (error)
		goto DPMError;
    /* // TODO: device_pm_add 待详细分析
     * 与电源管理有关 */
	device_pm_add(dev);

	/* Notify clients of device addition.  This call must come
	 * after dpm_sysfs_add() and before kobject_uevent().
	 */
    /* 通知客户端设备添加。这个调用必须在dpm_sysfs_add（）之后
     * 和kobject_uevent（） 之前。
     * 参考 i2c-dev.c 代码，注册相应的 notifier_block */
	if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_ADD_DEVICE, dev);

    /* 新注册的设备发送 uevent add 事件
     * 当发送到用户空间时，就要创建设备节点了。。 */
	kobject_uevent(&dev->kobj, KOBJ_ADD);
    /* 为新设备探测驱动程序 */
	bus_probe_device(dev);
    /* 新注册设备有父设备时
     * 将新设备添加到 父设备 链表中
     * 这里就将设备结构形成层级关系了
     * 每个设备的结构都可以找到自己的父，也可以找到属于它的子设备 */
	if (parent)
		klist_add_tail(&dev->p->knode_parent,
			       &parent->p->klist_children);

    /* 如果设备还属于某个 class
     * 需要将设备加入到 class 中 */
	if (dev->class) {
		mutex_lock(&dev->class->p->mutex);
		/* tie the class to the device */
        /* 将 device 的 klist_node 节点 链接到 class 的 klist 设备链表中
         * 这个与 bus 的 klist_devices 一样，都是 klist 设备链表 */
        /* 这里需要注意， device 的 bus klist_node 节点在 device_private 结构中
         * 但 class klist_node 节点在 device 结构中 */
		klist_add_tail(&dev->knode_class,
			       &dev->class->p->klist_devices);

		/* notify any interfaces that the device is here */
        /* 新注册的设备属于某个class
         * 则需要遍历该class 上所有的 interfaces
         * 为设备调用class 中的每一个 class_interface add_dev 回调 */
		list_for_each_entry(class_intf,
				    &dev->class->p->interfaces, node)
			if (class_intf->add_dev)
				class_intf->add_dev(dev, class_intf);
		mutex_unlock(&dev->class->p->mutex);
	}
done:
    /* 在本函数开始时，使用了 get_device()
     * 这里需要对应释放一下 */
	put_device(dev);
	return error;
 DPMError:
	bus_remove_device(dev);
 BusError:
	device_remove_attrs(dev);
 AttrsError:
	device_remove_class_symlinks(dev);
 SymlinkError:
	if (MAJOR(dev->devt))
		devtmpfs_delete_node(dev);
	if (MAJOR(dev->devt))
		device_remove_sys_dev_entry(dev);
 devtattrError:
	if (MAJOR(dev->devt))
		device_remove_file(dev, &devt_attr);
 ueventattrError:
	device_remove_file(dev, &uevent_attr);
 attrError:
	kobject_uevent(&dev->kobj, KOBJ_REMOVE);
	kobject_del(&dev->kobj);
 Error:
	cleanup_device_parent(dev);
	if (parent)
		put_device(parent);
name_error:
	kfree(dev->p);
	dev->p = NULL;
	goto done;
}

/**
 * device_register - register a device with the system.
 * @dev: pointer to the device structure
 *
 * This happens in two clean steps - initialize the device
 * and add it to the system. The two steps can be called
 * separately, but this is the easiest and most common.
 * I.e. you should only call the two helpers separately if
 * have a clearly defined need to use and refcount the device
 * before it is added to the hierarchy.
 *
 * For more information, see the kerneldoc for device_initialize()
 * and device_add().
 *
 * NOTE: _Never_ directly free @dev after calling this function, even
 * if it returned an error! Always use put_device() to give up the
 * reference initialized in this function instead.
 */
/* device_register - 向系统注册设备
 * @dev: 设备结构指针
 *
 * 这需要两个简单的步骤 - 初始化设备并将其添加到系统。
 * 这两个步骤可以单独调用，但这是最简单、最常见的。
 * 也就是说，只有在明确需要将设备添加到层次结构之前使用和引用计数设备时，
 * 才应该单独调用这两个辅助函数。
 *
 * 可通过内核文档，查找 device_initialize() 和 device_add() 的更多信息。
 *
 * 注意：调用此函数后，切勿直接释放 @dev， 即使它返回了错误！！！！
 * 务必使用 put_device() 来释放此函数中初始化的引用。
 *
 * 这里一定要注意需要释放dev时，调用 put_device(), 
 * 在 device_add 中，可能分配了私有数据结构 ，并且添加了很多属性，
 * 增加了对bus、driver 的引用，如果直接释放掉 dev，那影响很大的
 * 所以需要先解除引用（第一次的引用是在 device_initialize 中） */
int device_register(struct device *dev)
{
    /* 初始化 设备结构, 有对 内嵌 kobject 的初始化 */
	device_initialize(dev);
    /* 将设备结构添加到系统中 */
	return device_add(dev);
}

/**
 * get_device - increment reference count for device.
 * @dev: device.
 *
 * This simply forwards the call to kobject_get(), though
 * we do take care to provide for the case that we get a NULL
 * pointer passed in.
 */
/* 递增 device 的引用计数
 * 简单的递增 device 结构中的 kobj 引用计数 */
struct device *get_device(struct device *dev)
{
	return dev ? to_dev(kobject_get(&dev->kobj)) : NULL;
}

/**
 * put_device - decrement reference count.
 * @dev: device in question.
 */
/* 递减 device 的引用计数 */
void put_device(struct device *dev)
{
	/* might_sleep(); */
	if (dev)
		kobject_put(&dev->kobj);
}

/**
 * device_del - delete device from system.
 * @dev: device.
 *
 * This is the first part of the device unregistration
 * sequence. This removes the device from the lists we control
 * from here, has it removed from the other driver model
 * subsystems it was added to in device_add(), and removes it
 * from the kobject hierarchy.
 *
 * NOTE: this should be called manually _iff_ device_add() was
 * also called manually.
 */
/* 从系统中移除一个 设备
 * @dev: 设备结构
 *
 * 这是设备注销序列的第一部分。
 * 它会将设备从我们控制列表中移除，并将其从在 device_add() 中添加的
 * 其它驱动模型子系统中移除，并将其从 kobject 层次结构中移除。
 *
 * 本函数就是 device_add() 的逆向操作，移除在各个子系统中注册的痕迹 */
void device_del(struct device *dev)
{
    /* 找一下本设备的父，不一定真的有父设备 */
	struct device *parent = dev->parent;
	struct class_interface *class_intf;

	/* Notify clients of device removal.  This call must come
	 * before dpm_sysfs_remove().
	 */
    /* 通知客户端设备移除。这个调用必须在dpm_sysfs_remove（）之前进行。 */
	if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_DEL_DEVICE, dev);
    /* // TODO: device_pm_remove 待详细分析
     * 与电源管理有关 */
	device_pm_remove(dev);
    /* // TODO:  dpm_sysfs_remove 待详细分析
     * dpm_sysfs_add 与电源管理有关，在设备目录下创建了 power 属性目录
     * 另外处理了一些电源管理相关操作 */
	dpm_sysfs_remove(dev);
    /* 如果设备有父设备，则从父设备链表中移除 */
	if (parent)
		klist_del(&dev->p->knode_parent);
    /* 有设备号的，需要移除设备节点相关内容 */
	if (MAJOR(dev->devt)) {
        /* 移除 devtmpfs 设备节点  */
		devtmpfs_delete_node(dev);
        /* 移除 /sys/dev/ 目录下的设备链接文件 */
		device_remove_sys_dev_entry(dev);
        /* 移除设备目录下 dev 属性 */
		device_remove_file(dev, &devt_attr);
	}
	if (dev->class) {
        /* 移除设备相关的 class 符号链接 */
		device_remove_class_symlinks(dev);

		mutex_lock(&dev->class->p->mutex);
		/* notify any interfaces that the device is now gone */
        /* 将要移除的设备属于某个class
         * 则需要遍历该class 上所有的 interfaces
         * 为设备调用class 中的每一个 class_interface remove_dev 回调 */
		list_for_each_entry(class_intf,
				    &dev->class->p->interfaces, node)
			if (class_intf->remove_dev)
				class_intf->remove_dev(dev, class_intf);
		/* remove the device from the class list */
		klist_del(&dev->knode_class);
		mutex_unlock(&dev->class->p->mutex);
	}
    /* 移除设备目录下的 uevent 属性 */
	device_remove_file(dev, &uevent_attr);
    /* 移除指定设备的属性文件，包含了 class type */
	device_remove_attrs(dev);
    /* 从bus上移除设备, 这里将会把 设备/驱动 解绑 */
    /* 内部存在 设备/驱动 解绑过程，使用了 klist_remove()
     * 将设备从驱动链表中移除，只有无任何驱动引用时，本函数才会返回 */
	bus_remove_device(dev);
    /* 从延迟探测列表中移除
     * 如果还在延迟探测列表中，说明将要移除的设备还未真的被 probe 成功
     * 但这里要移除设备了，即使没有 probe 成功，也需要移除了，避免再次被探测 */
	driver_deferred_probe_del(dev);

	/*
	 * Some platform devices are driven without driver attached
	 * and managed resources may have been acquired.  Make sure
	 * all resources are released.
	 */
    /* 某些平台设备在未安装驱动程序的情况下也能运行，并且可能已经获取了管理资源。
     * 请务必确保所有资源均已释放。 */
    /* 释放所有与 dev 相关的资源 */
	devres_release_all(dev);

	/* Notify the platform of the removal, in case they
	 * need to do anything...
	 */
    /* platform_notify 回调，仅在 acpi 驱动中使用了 */
	if (platform_notify_remove)
		platform_notify_remove(dev);
    /* 发送 remove  uevent 事件，通知用户空间
     * 这里用户空间会将 设备节点 移除 */
	kobject_uevent(&dev->kobj, KOBJ_REMOVE);
    /* 清理一下自己的父设备(父目录项)
     * 这里主要是释放一下 glue_dirs 胶合目录的引用
     * 如果设备在 class 的胶合目录下，那需要对胶合目录解引用 */
	cleanup_device_parent(dev);
    /* 移除本设备的 设备模型
     * 注意，这里只是从 sysfs 中移除了，并未减少引用计数 */
	kobject_del(&dev->kobj);
    /* 释放父设备引用
     * 在 device_add 中 get_device() 父设备了 */
	put_device(parent);
}

/**
 * device_unregister - unregister device from system.
 * @dev: device going away.
 *
 * We do this in two parts, like we do device_register(). First,
 * we remove it from all the subsystems with device_del(), then
 * we decrement the reference count via put_device(). If that
 * is the final reference count, the device will be cleaned up
 * via device_release() above. Otherwise, the structure will
 * stick around until the final reference to the device is dropped.
 */
/* 从系统中注销一个设备
 *
 * 像执行 device_register() 一样，将其分为两部分。
 * 首先，使用 device_del() 从所有子系统中移除该设备，然后使用
 * put_device() 减少其引用计数。如果这是最终的引用计数，则设备将
 * 通过上面的 device_release() 进行清除。否则，该结构将一直保留，
 * 直到对该设备的最终引用被删除。
 * */
void device_unregister(struct device *dev)
{
	pr_debug("device: '%s': %s\n", dev_name(dev), __func__);
    /* 将设备结构从各子系统中移除
     * 此处并未释放设备，只是不会再有新的引用了,其它子系统(bus/class/driver) 无
     * 法看到了 */
	device_del(dev);
    /* 递减设备引用计数，到 0 时，将使用 device_release 函数释放掉 */
	put_device(dev);
}

/* 迭代下一个设备结构
 * 本函数专用于本文件，并且是在遍历子设备时使用
 * 函数内部固定了通过 knode_parent 反查 device_private 结构 */
static struct device *next_device(struct klist_iter *i)
{
    /* 迭代下一个 klist_node 节点，即 knode_parent 节点 */
	struct klist_node *n = klist_next(i);
	struct device *dev = NULL;
	struct device_private *p;

	if (n) {
        /* 通过 knode_parent 成员反查 device_private 结构 */
		p = to_device_private_parent(n);
        /* device_private 结构的 device 指向包含它的设备结构 */
		dev = p->device;
	}
	return dev;
}

/**
 * device_get_devnode - path of device node file
 * @dev: device
 * @mode: returned file access mode
 * @tmp: possibly allocated string
 *
 * Return the relative path of a possible device node.
 * Non-default names may need to allocate a memory to compose
 * a name. This memory is returned in tmp and needs to be
 * freed by the caller.
 * 设备节点文件 的 路径
 * 返回一个可能的设备节点的相对路径。
 * 非默认名称可能需要分配内存来组成名称。
 * 该内存在 tmp 中返回，需要由调用者释放
 */
const char *device_get_devnode(struct device *dev,
			       umode_t *mode, const char **tmp)
{
	char *s;

	*tmp = NULL;

    /* device_type 可以提供特殊的名字 */
	/* the device type may provide a specific name */
	if (dev->type && dev->type->devnode)
		*tmp = dev->type->devnode(dev, mode);
	if (*tmp)
		return *tmp;

    /* device class 也能提供特殊的名字 */
	/* the class may provide a specific name */
	if (dev->class && dev->class->devnode)
		*tmp = dev->class->devnode(dev, mode);
	if (*tmp)
		return *tmp;

    /* 最后以 device init_name 做为名字，
     * 如果名字中不包含 '!', 则直接返回 */
	/* return name without allocation, tmp == NULL */
	if (strchr(dev_name(dev), '!') == NULL)
		return dev_name(dev);

    /* 名字中包含 '!' 则需要替换为 '/' */
	/* replace '!' in the name with '/' */
    /* 包含有 '/' 的，在生成设备节点时，会以目录方式呈现
     * 如 /dev/bus/usb/002/001 */
	*tmp = kstrdup(dev_name(dev), GFP_KERNEL);
	if (!*tmp)
		return NULL;
	while ((s = strchr(*tmp, '!')))
		s[0] = '/';
	return *tmp;
}

/**
 * device_for_each_child - device child iterator.
 * @parent: parent struct device.
 * @data: data for the callback.
 * @fn: function to be called for each device.
 *
 * Iterate over @parent's child devices, and call @fn for each,
 * passing it @data.
 *
 * We check the return of @fn each time. If it returns anything
 * other than 0, we break out and return that value.
 */
/* 某设备的子设备迭代器
 * @parent: 将要进行遍历子设备的父设备结构
 * @data:   @fn 回调函数的参数
 * @fn:     迭代每一个子设备时的回调函数
 *
 * 迭代 @parent 设备的每一个子设备，并且为它们调用 @fn ，传递 data 参数。
 *
 * 每次迭代将检查 @fn 返回值。如果它返回的值不是0，将跳出迭代并返回该值。 */
int device_for_each_child(struct device *parent, void *data,
			  int (*fn)(struct device *dev, void *data))
{
	struct klist_iter i;
	struct device *child;
	int error = 0;

    /* 私有核心数据有效性检查
     * 子设备在 p->klist_children 链表中 */
	if (!parent->p)
		return 0;

    /* 初始化迭代器
     * 子设备链表在 klist_children
     * 子设备通过自身的 knode_parent 挂载到该链表中 */
	klist_iter_init(&parent->p->klist_children, &i);
    /* 遍历每一个设备，若 child = NULL，则遍历完成
     * 在每次有效的遍历时，调用 fn 回调函数，并传 data 参数 */
	while ((child = next_device(&i)) && !error)
		error = fn(child, data);
    /* 退出迭代器 */
	klist_iter_exit(&i);
	return error;
}

/**
 * device_find_child - device iterator for locating a particular device.
 * @parent: parent struct device
 * @data: Data to pass to match function
 * @match: Callback function to check device
 *
 * This is similar to the device_for_each_child() function above, but it
 * returns a reference to a device that is 'found' for later use, as
 * determined by the @match callback.
 *
 * The callback should return 0 if the device doesn't match and non-zero
 * if it does.  If the callback returns non-zero and a reference to the
 * current device can be obtained, this function will return to the caller
 * and not iterate over any more devices.
 */
/* 用于定位特定子设备的设备迭代器
 *
 * 与上面的 device_for_each_child() 类似，但它返回一个对设备的应用，
 * 该设备是有 @match 回调确定的，引用外部使用
 *
 * 如果设备不匹配，match回调应返回0，如果匹配，则返回非零值。
 * 如果回调返回非零，则此函数将返回给调用者并且不再迭代任何设备。 */
struct device *device_find_child(struct device *parent, void *data,
				 int (*match)(struct device *dev, void *data))
{
	struct klist_iter i;
	struct device *child;

	if (!parent)
		return NULL;

    /* 初始化迭代器
     * 子设备链表在 klist_children
     * 子设备通过自身的 knode_parent 挂载到该链表中 */
	klist_iter_init(&parent->p->klist_children, &i);
    /* 遍历每一个设备，若 child = NULL，则遍历完成
     * 在每次有效的遍历时，调用 match 回调函数，并传 data 参数 */
    /* 当设备匹配成功，则会递增设备引用计数，并退出迭代 */
	while ((child = next_device(&i)))
		if (match(child, data) && get_device(child))
			break;
    /* 退出迭代器 */
	klist_iter_exit(&i);
	return child;
}

/* 设备初始化，内核启动时候调用，用于创建设备结构最初的数据结构
 * 创建之后，在 /sys/ 下的目录结构为: 
 * /sys/
 *     --devices 
 *     --dev 
 *       --block 
 *       --char */
int __init devices_init(void)
{
    /* 创建 devices 注意这里是一个 kset 
     * kset 是能够产生 uevent 事件的，
     * 通过这里的 kset ，也就在注册设备时，会产生 uevent 事件 */
    /* 在设备初始化时，将 设备的 kobj.kset 指向 devices_kset 
     *  dev->kobj.kset = devices_kset;
     *  */
	devices_kset = kset_create_and_add("devices", &device_uevent_ops, NULL);
	if (!devices_kset)
		return -ENOMEM;
    
    /* dev_kobj sysfs_dev_block_kobj  sysfs_dev_char_kobj 
     * 都只是 kobject ， 只是用来建立目录结构的，并且不属于 kset */
	dev_kobj = kobject_create_and_add("dev", NULL);
	if (!dev_kobj)
		goto dev_kobj_err;
    /* /sys/dev/block/ 目录 */
	sysfs_dev_block_kobj = kobject_create_and_add("block", dev_kobj);
	if (!sysfs_dev_block_kobj)
		goto block_kobj_err;
    /* /sys/dev/char/ 目录 */
	sysfs_dev_char_kobj = kobject_create_and_add("char", dev_kobj);
	if (!sysfs_dev_char_kobj)
		goto char_kobj_err;

	return 0;

 char_kobj_err:
	kobject_put(sysfs_dev_block_kobj);
 block_kobj_err:
	kobject_put(dev_kobj);
 dev_kobj_err:
	kset_unregister(devices_kset);
	return -ENOMEM;
}

EXPORT_SYMBOL_GPL(device_for_each_child);
EXPORT_SYMBOL_GPL(device_find_child);

EXPORT_SYMBOL_GPL(device_initialize);
EXPORT_SYMBOL_GPL(device_add);
EXPORT_SYMBOL_GPL(device_register);

EXPORT_SYMBOL_GPL(device_del);
EXPORT_SYMBOL_GPL(device_unregister);
EXPORT_SYMBOL_GPL(get_device);
EXPORT_SYMBOL_GPL(put_device);

EXPORT_SYMBOL_GPL(device_create_file);
EXPORT_SYMBOL_GPL(device_remove_file);

/* root device 根设备结构
 * 是一个虚拟的设备，一般用于其它设备的父设备
 * 使用该功能的驱动不多，常见的驱动并没有使用 */
struct root_device {
    /* 一个普通的设备结构，
     * 使用普通设备的方式注册到驱动模型中 */
	struct device dev;
    /* 模块的“拥有者”
     * 如果使用 root_device_register() 注册的 root_device
     * 那这里就是 THIS_MODULE */
	struct module *owner;
};

/* 通过 dev 指针反查 root_device 结构 */
inline struct root_device *to_root_device(struct device *d)
{
	return container_of(d, struct root_device, dev);
}

/* root_device 设备的 release 回调
 * 用于释放 注册时分配的 root_device 结构内存 */
static void root_device_release(struct device *dev)
{
    /* 释放掉 root_device 结构的内存 */
	kfree(to_root_device(dev));
}

/**
 * __root_device_register - allocate and register a root device
 * @name: root device name
 * @owner: owner module of the root device, usually THIS_MODULE
 *
 * This function allocates a root device and registers it
 * using device_register(). In order to free the returned
 * device, use root_device_unregister().
 *
 * Root devices are dummy devices which allow other devices
 * to be grouped under /sys/devices. Use this function to
 * allocate a root device and then use it as the parent of
 * any device which should appear under /sys/devices/{name}
 *
 * The /sys/devices/{name} directory will also contain a
 * 'module' symlink which points to the @owner directory
 * in sysfs.
 *
 * Returns &struct device pointer on success, or ERR_PTR() on error.
 *
 * Note: You probably want to use root_device_register().
 */
/* 分配并注册一个 root device
 *
 * 此函数分配一个根设备并使用 device_register() 进行注册。
 * 要释放已分配的设备，请使用 root_device_unregister()
 *
 * 根设备是虚拟设备，允许将其它设备分组到 /sys/devices/ 下。
 * 使用此函数分配一个根设备，然后将其用作 /sys/devices/{name} 下
 * 任何设备的父设备
 *
 * /sys/devices/{name} 目录还将包含一个 "module" 符号链接，
 * 该符号链接指向 sysfs 中的 @owner 目录。
 * 
 * 应该是 root_device_register() 进行注册 */
struct device *__root_device_register(const char *name, struct module *owner)
{
	struct root_device *root;
	int err = -ENOMEM;

    /* 分配 root_device 结构
     * root_device 根设备 都是自动创建的，只需要提供 设备名 即可 */
	root = kzalloc(sizeof(struct root_device), GFP_KERNEL);
	if (!root)
		return ERR_PTR(err);

    /* 设置 root_device 设备名 */
	err = dev_set_name(&root->dev, "%s", name);
	if (err) {
		kfree(root);
		return ERR_PTR(err);
	}

    /* root_device  release 回调，用于释放上面分配的内存 */
	root->dev.release = root_device_release;

    /* 将 root_device 中的 dev(标准设备) 注册到设备模型中
     * 这样就会有一个 /sys/devices/{name} 的目录了 */
	err = device_register(&root->dev);
	if (err) {
		put_device(&root->dev);
		return ERR_PTR(err);
	}

#ifdef CONFIG_MODULES	/* gotta find a "cleaner" way to do this */
	if (owner) {
		struct module_kobject *mk = &owner->mkobj;

        /* 在 root_device 的目录中，创建一个指向 @owner 的 module 符号链接 */
		err = sysfs_create_link(&root->dev.kobj, &mk->kobj, "module");
		if (err) {
			device_unregister(&root->dev);
			return ERR_PTR(err);
		}
        /* root_device 的 owner 指向 参数指定的 @owner */
		root->owner = owner;
	}
#endif

	return &root->dev;
}
EXPORT_SYMBOL_GPL(__root_device_register);

/**
 * root_device_unregister - unregister and free a root device
 * @dev: device going away
 *
 * This function unregisters and cleans up a device that was created by
 * root_device_register().
 */
/* 注销并释放一个 root_device
 * 清理 root_device ，注销设备 */
void root_device_unregister(struct device *dev)
{
    /* 通过 dev 结构，反查 root_device 结构 */
	struct root_device *root = to_root_device(dev);

    /* 移除 module 符号链接 */
	if (root->owner)
		sysfs_remove_link(&root->dev.kobj, "module");

    /* 注销 root_device 中的 普通设备结构 */
	device_unregister(dev);
}
EXPORT_SYMBOL_GPL(root_device_unregister);


/* 动态创建的设备 的 release 回调
 * 用于释放 设备结构的内存 */
static void device_create_release(struct device *dev)
{
	pr_debug("device: '%s': %s\n", dev_name(dev), __func__);
    /* 释放 设备结构的内存 */
	kfree(dev);
}

/**
 * device_create_vargs - creates a device and registers it with sysfs
 * @class: pointer to the struct class that this device should be registered to
 * @parent: pointer to the parent struct device of this new device, if any
 * @devt: the dev_t for the char device to be added
 * @drvdata: the data to be added to the device for callbacks
 * @fmt: string for the device's name
 * @args: va_list for the device's name
 *
 * This function can be used by char device classes.  A struct device
 * will be created in sysfs, registered to the specified class.
 *
 * A "dev" file will be created, showing the dev_t for the device, if
 * the dev_t is not 0,0.
 * If a pointer to a parent struct device is passed in, the newly created
 * struct device will be a child of that device in sysfs.
 * The pointer to the struct device will be returned from the call.
 * Any further sysfs files that might be required can be created using this
 * pointer.
 *
 * Returns &struct device pointer on success, or ERR_PTR() on error.
 *
 * Note: the struct class passed to this function must have previously
 * been created with a call to class_create().
 */
/* 创建一个 device 并注册到 sysfs 中
 * @class:  指向该设备应注册的 struct class 的指针
 * @parent: 指向此新设备的 父设备 的指针，如果没有父则为 NULL
 * @devt:   要添加的字符设备的设备号 dev_t
 * @drvdata:需要添加到设备中用于回调的数据
 * @fmt:    设备名的格式化字符串
 * @args:   设备名格式化字符串参数
 *
 * 本函数可供字符设备类使用。它将在 sysfs 中创建一个 struct device,
 * 并注册到指定的设备类。
 *
 * 将创建一个 "dev" 设备文件，设备号为 dev_t，当然 dev_t 不能为 0,0.
 * 如果传入指向父设备结构的指针，则新创建的设备将成为 sysfs 中该设备的子设备。
 * 函数返回 struct device 的指针。
 * 所有需要的 sysfs 文件的操作，都可以返回的指针。
 *
 * 注意：提供给本函数的 struct class 必须先前通过调用 class_create() 来创建。 */
struct device *device_create_vargs(struct class *class, struct device *parent,
				   dev_t devt, void *drvdata, const char *fmt,
				   va_list args)
{
	struct device *dev = NULL;
	int retval = -ENODEV;

    /* 创建设备，必须要提供一个 struct class */
	if (class == NULL || IS_ERR(class))
		goto error;

    /* 分配 设备结构，注意，动态分配的，不使用时需要释放 */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		retval = -ENOMEM;
		goto error;
	}

    /* 填充设备结构数据，做注册的准备 */
    /* 设备号，有效的设备号，会创建设备节点 */
	dev->devt = devt;
    /* 设置所属的 class，注册过程会使用 */
	dev->class = class;
    /* 设置 父设备，可以为 NULL */
	dev->parent = parent;
    /* 设置设备释放时 release 回调，用于释放 设备结构的内存 */
	dev->release = device_create_release;
    /* 设置设备给驱动提供的私有数据，可以为 NULL */
	dev_set_drvdata(dev, drvdata);

    /* 设置 设备 内嵌的 kobj 名字，就是提供的设备名 */
	retval = kobject_set_name_vargs(&dev->kobj, fmt, args);
	if (retval)
		goto error;

    /* 注册设备，完成后就可以使用了
     * 本函数中也完成了设备节点的创建，uevent 事件发送 */
	retval = device_register(dev);
	if (retval)
		goto error;

	return dev;

error:
	put_device(dev);
	return ERR_PTR(retval);
}
EXPORT_SYMBOL_GPL(device_create_vargs);

/**
 * device_create - creates a device and registers it with sysfs
 * @class: pointer to the struct class that this device should be registered to
 * @parent: pointer to the parent struct device of this new device, if any
 * @devt: the dev_t for the char device to be added
 * @drvdata: the data to be added to the device for callbacks
 * @fmt: string for the device's name
 *
 * This function can be used by char device classes.  A struct device
 * will be created in sysfs, registered to the specified class.
 *
 * A "dev" file will be created, showing the dev_t for the device, if
 * the dev_t is not 0,0.
 * If a pointer to a parent struct device is passed in, the newly created
 * struct device will be a child of that device in sysfs.
 * The pointer to the struct device will be returned from the call.
 * Any further sysfs files that might be required can be created using this
 * pointer.
 *
 * Returns &struct device pointer on success, or ERR_PTR() on error.
 *
 * Note: the struct class passed to this function must have previously
 * been created with a call to class_create().
 */
/* 注意，使用 device_create 创建设备，如果需要自动生成设备节点
 * 那必须要提供一个 struct class, 使用 class_create() 创建一个 class
 * 为什么必须要提供一个 class 才能创建设备节点呢？
 * 按照之前对 kobject 的分析，一个 kobject 如果需要发送 uevent 事件，
 * 那必须要属于一个 kset，因为只有 kset 才能发送 uevent 事件。
 * 在所有创建/注册的设备时， device_initialize() 函数中会将设备的kobj
 * 设置所属的 kset 为 devices_kset, devices_kset 是 所有 device 的顶层kset，
 * 那就是说，所有创建/注册的设备 都是具有发送 uevent 的能力的，那为什么还要
 * 提供一个 class ？
 *
 * 1. 最直接的，在 device_create_vargs() 函数中，开头就是对参数 class 的判断:
 *  if (class == NULL || IS_ERR(class))
 *  这是最直接的原因，如果一个注册的设备不属于某个 class，那就不会真的注册；
 *  但这个原因也太简单了，不能看到本质。
 *  因为按照 device_register() 函数流程，一个设备不属于 class ，注册流程
 *  是不会出错的。
 *  将 上面的简单判断去掉，测试是否会注册成功呢？ 
 *  答案 还是不会创建节点，但在 /sys/devices/ 中会有相应的设备的。
 *  既然 /sys/devices/ 中有了设备目录，为什么不能创建节点呢？
 *  备注：！！！其实上面的，将class为NULL的判断去掉之后，是否能生成设备节点，
 *  是与 devtmpfs 是否开启有关，如果没有开启 devtmpfs ，那不会生成节点；
 *  如果开启了 devtmpfs ，是会生成节点的。
 *  这里假设没有开启，并不会生成节点，继续分析依赖 class 的原因。
 * 2. 在(1)的分析中，依赖 devtmpfs, 是因为在 device_add() 函数中，如果设备具有
 *  有效的设备号，则会调用 devtmpfs_create_node() 进行设备节点的创建，这样我们
 *  其实已经有了设备节点了。
 *  但如果没有 devtmpfs ，为什么不能创建节点呢，不是能发送 uevent 事件吗？
 *  在 device_add() 函数中，会调用 kobject_uevent() 发送 KOBJ_ADD 事件，
 *  这不都发送吗，那就进入函数内部看看。
 * 3. 在 kobject_uevent_env() 函数中，分析 设备所属的 kset, top_kobj 等，
 *  都是正常的，比如 属于 devices_kset, top_kobj 为自身 或 父设备。
 *  在真正发送 uevent 之前，还需要做一些工作，所属的kset的uevent_ops 回调，
 *  就是对 kset 下的 要发送 uevent 的 kobj 进行一些处理，保证发送正确的数据。
 * 4. uevent_ops 中，filter 是 过滤处理函数，而设备所属的 kset 即 devices_kset,
 *  filter 回调为 dev_uevent_filter(), 该回调中，判断了要发送 uevent 的 kobj
 *  需要是 device_ktype 类型，所有的 device 都是这种类型的，是满足的。
 *  但还有两个判断，需要设备必须属于一个 bus 或者 class，否则就会被过滤掉。
 *  这里看出注册的设备，如果通过发送 uevent 事件创建节点，必须属于某个 class。
 * 5. uevent_ops 中，还有另一个回调 name, 用于获取 kobj 的 subsystem，
 *  即设备所属的 子系统。
 *  对于 devices_kset ，name 对应 dev_uevent_name() , 也是返回的 bus / class
 *  的名字，如果没有bus/class，那返回NULL，即 subsystem == NULL, 也会被
 *  kobject_uevent_env() 函数给过滤掉。
 *  对于没有 name 回调的 kset, 是使用 kset->kobj 的 name，看着应该都会成功。
 *  对于 device，所有的都属于 devices_kset, 所以 device 必须要属于 bus 或 class.
 * 6. uevent_ops 中，还有一个 uevent 回调，用于添加一些补充的环境变量，
 *  比如 设备号，没有过滤机制。
 *
 * 综上，创建一个设备，并且要能自动生成设备节点，则需要让设备属于某个 class，
 * 是因为在 发送 uevent 事件时，需要对设备所属的 bus/class 进行过滤，要求能够
 * 发送 uevent 事件的设备，必须属于某个 bus / class。
 *
 * 经过实测，如果把所有的过滤机制去除，即使没有提供class，也是可以创建设备节点的。
 * 但这就破坏了 设备驱动 的原有逻辑。
 * 考虑一下，一个设备不可能是游离的，肯定属于某个 bus （一个实体设备），内核也
 * 创建了 platform_bus 这个虚拟bus 供一些设备连接；如果是一个虚拟的设备，那也应该
 * 要属于某一类class，比如 mtd 设备，mtd 不是真的设备，但属于 mtd_class 类。
 *
 * 所以，在设备驱动中，对于能够创建设备节点的设备做了限制，
 * 必须要属于某个bus或class。
 * 这看起来很合理啊。
 * */
/* 本函数，创建一个设备并生成设备节点 */
struct device *device_create(struct class *class, struct device *parent,
			     dev_t devt, void *drvdata, const char *fmt, ...)
{
	va_list vargs;
	struct device *dev;

    /* 所有的实现都在 device_create_vargs 中 */
	va_start(vargs, fmt);
	dev = device_create_vargs(class, parent, devt, drvdata, fmt, vargs);
	va_end(vargs);
	return dev;
}
EXPORT_SYMBOL_GPL(device_create);

/* 设备号匹配回调
 * 用于匹配提供的设备结构的设备号 与 参数@data 是否一致 */
static int __match_devt(struct device *dev, void *data)
{
    /* 用于匹配的设备号，由参数 @data 提供 */
	dev_t *devt = data;

    /* 一致则返回 1，否则返回 0 */
	return dev->devt == *devt;
}

/**
 * device_destroy - removes a device that was created with device_create()
 * @class: pointer to the struct class that this device was registered with
 * @devt: the dev_t of the device that was previously registered
 *
 * This call unregisters and cleans up a device that was created with a
 * call to device_create().
 */
/* 移除一个 使用 device_create() 创建的设备
 * @class:  创建设备时提供的 struct class 指针（必须要提供）
 * @devt:   创建设备时提供的 dev_t ，即设备号
 * 本函数将注销并清理通过 device_create() 创建的设备 */
void device_destroy(struct class *class, dev_t devt)
{
	struct device *dev;

    /* 在 class 中找到设备号匹配的设备结构
     * 注意，这里使用的是匹配设备号 */
	dev = class_find_device(class, NULL, &devt, __match_devt);
	if (dev) {
        /* 找到对应的有效设备，则释放设备引用 */
		put_device(dev);
        /* 注销设备 */
		device_unregister(dev);
	}
}
EXPORT_SYMBOL_GPL(device_destroy);

/**
 * device_rename - renames a device
 * @dev: the pointer to the struct device to be renamed
 * @new_name: the new name of the device
 *
 * It is the responsibility of the caller to provide mutual
 * exclusion between two different calls of device_rename
 * on the same device to ensure that new_name is valid and
 * won't conflict with other devices.
 *
 * Note: Don't call this function.  Currently, the networking layer calls this
 * function, but that will change.  The following text from Kay Sievers offers
 * some insight:
 *
 * Renaming devices is racy at many levels, symlinks and other stuff are not
 * replaced atomically, and you get a "move" uevent, but it's not easy to
 * connect the event to the old and new device. Device nodes are not renamed at
 * all, there isn't even support for that in the kernel now.
 *
 * In the meantime, during renaming, your target name might be taken by another
 * driver, creating conflicts. Or the old name is taken directly after you
 * renamed it -- then you get events for the same DEVPATH, before you even see
 * the "move" event. It's just a mess, and nothing new should ever rely on
 * kernel device renaming. Besides that, it's not even implemented now for
 * other things than (driver-core wise very simple) network devices.
 *
 * We are currently about to change network renaming in udev to completely
 * disallow renaming of devices in the same namespace as the kernel uses,
 * because we can't solve the problems properly, that arise with swapping names
 * of multiple interfaces without races. Means, renaming of eth[0-9]* will only
 * be allowed to some other name than eth[0-9]*, for the aforementioned
 * reasons.
 *
 * Make up a "real" name in the driver before you register anything, or add
 * some other attributes for userspace to find the device, or use udev to add
 * symlinks -- but never rename kernel devices later, it's a complete mess. We
 * don't even want to get into that and try to implement the missing pieces in
 * the core. We really have other pieces to fix in the driver core mess. :)
 */
/* 重命名一个设备
 * @dev:    指向要重命名的 struct device 的指针
 * @new_name: 设备的新名字
 *
 * 调用者有责任在同一设备上对 device_rename() 的两次不同调用之间提供互斥，
 * 以确保 new_name 是有效的，不会与其它设备冲突。
 *
 * 注意：不要调用这个函数。目前，网络层调用这个函数，但这种情况将会改变。
 * Kay Sievers 的以下文章提供了一些见解：
 * 
 * 重命名设备在很多层面上都存在竟态条件，符号链接和其它内容无法原子性替换，
 * 而且会触发一个 “move” uevent 事件，但很难将该事件与旧设备和新设备关联起来。
 * 设备节点根本不会被重命名，内核目前甚至都不支持这一点。
 *
 * 与此同时，在重命名期间，您的目标名称可能会被另一个驱动程序占用，从而产生冲突。
 * 或者您重命名之后，旧名称直接被占用——那么您会收到相同 DEVPATH 的事件，甚至在看
 * 到“移动”事件之前就会收到。这简直是一团糟，任何新功能都不应依赖于内核设备重命
 * 名。除此之外，目前除了（从驱动程序核心角度来看非常简单的）网络设备之外，其他
 * 设备都不支持这种重命名功能。
 *
 * 目前，我们正计划将 udev 中的网络重命名功能进行修改，以完全禁止在与内核所使用
 * 的命名空间相同的范围内对设备进行重命名操作，因为我们无法妥善解决在不出现冲突
 * 的情况下更换多个接口名称所引发的问题。也就是说，出于上述原因，eth[0-9]* 的重
 * 命名操作将仅允许被重命名为除 eth[0-9]* 之外的其他名称。
 *
 * 在注册任何内容之前，先为驱动程序编造一个“真实”的名称，或者为用户空间添加一些
 * 其他属性以便找到该设备，或者使用 udev 来添加符号链接——但切记永远不要在之后重
 * 命名内核设备，那样会非常混乱。我们甚至都不想深入探讨这个问题，也不想尝试在核
 * 心部分补全缺失的部分。我们真正需要解决的是驱动程序核心部分的其他问题。:)
 *
 * 综上，核心思想就是 不要使用本函数，不要进行设备的重命名！！！！
 * */
int device_rename(struct device *dev, const char *new_name)
{
	char *old_class_name = NULL;
	char *new_class_name = NULL;
	char *old_device_name = NULL;
	int error;

    /* 引用一下将要进行重命名的设备 */
	dev = get_device(dev);
	if (!dev)
		return -EINVAL;

	pr_debug("device: '%s': %s: renaming to '%s'\n", dev_name(dev),
		 __func__, new_name);

    /* 创建一个旧名字的副本，这里并没有改变设备名
     * 只是新分配了空间，拷贝了一份旧名字 */
	old_device_name = kstrdup(dev_name(dev), GFP_KERNEL);
	if (!old_device_name) {
		error = -ENOMEM;
		goto out;
	}

	if (dev->class) {
        /* 设备属于某个 class，那需要把在 class 中的符号链接更名了 */
		error = sysfs_rename_link(&dev->class->p->subsys.kobj,
			&dev->kobj, old_device_name, new_name);
		if (error)
			goto out;
	}

    /* 重命名设备对应的 kobject
     * 设备在 sysfs 中的名字就体现在 kobject 中 */
	error = kobject_rename(&dev->kobj, new_name);
	if (error)
		goto out;

out:
	put_device(dev);

	kfree(new_class_name);
	kfree(old_class_name);
	kfree(old_device_name);

	return error;
}
EXPORT_SYMBOL_GPL(device_rename);

/* 设备属于某个 class，需要把 class 相关的一些符号链接改一下 */
static int device_move_class_links(struct device *dev,
				   struct device *old_parent,
				   struct device *new_parent)
{
	int error = 0;

	if (old_parent)
        /* 原来有父设备的，将 指向父设备的 device 符号链接删除 */
		sysfs_remove_link(&dev->kobj, "device");
	if (new_parent)
        /* 有新的父设备了，则创建新的 device 符号链接，指向新的父设备 */
		error = sysfs_create_link(&dev->kobj, &new_parent->kobj,
					  "device");
	return error;
}

/**
 * device_move - moves a device to a new parent
 * @dev: the pointer to the struct device to be moved
 * @new_parent: the new parent of the device (can by NULL)
 * @dpm_order: how to reorder the dpm_list
 */
/* 将设备移动到新的父设备下
 * @dev:    将要移动的设备的 struct device 指针
 * @new_parent: 设备的新父设备，可以为 NULL
 * @dpm_order: 如何重新排序 dpm_list 
 * */
int device_move(struct device *dev, struct device *new_parent,
		enum dpm_order dpm_order)
{
	int error;
	struct device *old_parent;
	struct kobject *new_parent_kobj;

    /* 引用一下将要被移动的设备 */
	dev = get_device(dev);
	if (!dev)
		return -EINVAL;

    /* 锁定 PM 核心使用的活动设备列表 */
	device_pm_lock();
    /* 引用一下新的父设备，可能为 NULL */
	new_parent = get_device(new_parent);
    /* 给设备找个新父设备
     * 这里是按照设备所属的class，是否有父设备等条件
     * 找到新的 父kobj, 就是设备将要移动到的目录 */
	new_parent_kobj = get_device_parent(dev, new_parent);

    /* 提示一下移动到哪里了。。。。 */
	pr_debug("device: '%s': %s: moving to '%s'\n", dev_name(dev),
		 __func__, new_parent ? dev_name(new_parent) : "<NULL>");
    /* 将设备的 kobj 移动到新的父 kobj 下 */
	error = kobject_move(&dev->kobj, new_parent_kobj);
	if (error) {
        /* 在上面 get_device_parent 找父的过程中
         * 可能将设备加入到 class 的 glue_dirs 中了
         * 如果移动出错了，需要清理一下 */
		cleanup_glue_dir(dev, new_parent_kobj);
		put_device(new_parent);
		goto out;
	}
    /* 原来的父设备, 需要做一些清理工作 */
	old_parent = dev->parent;
    /* 记录一下新的父设备，可能为 NULL */
	dev->parent = new_parent;
	if (old_parent)
        /* 如果原来有父设备，则将设备从原父设备的子设备链表中移除 */
		klist_remove(&dev->p->knode_parent);
	if (new_parent) {
        /* 如果有新的父设备，则将设备加入到新父设备的子设备链表中 */
		klist_add_tail(&dev->p->knode_parent,
			       &new_parent->p->klist_children);
		set_dev_node(dev, dev_to_node(new_parent));
	}

    /* 设备不属于任何 class，则完成了移动，退出即可 */
	if (!dev->class)
		goto out_put;
    /* 处理设备目录下的 指向父设备的 device 符号链接 */
	error = device_move_class_links(dev, old_parent, new_parent);
	if (error) {
        /* 如果出错了，这里是把上面的步骤逆向一下
         * 就是再把所有的移动动作撤销掉。。。 */
		/* We ignore errors on cleanup since we're hosed anyway... */
        /* 重新移动一下 指向父设备的 device 符号链接
         * 复原 老的父 和 新的父 设备 */
		device_move_class_links(dev, new_parent, old_parent);
        /* 将设备再移动到 原来的父设备 下 */
		if (!kobject_move(&dev->kobj, &old_parent->kobj)) {
			if (new_parent)
                /* 从新的父设备 的子设备链表中移除设备 */
				klist_remove(&dev->p->knode_parent);
            /* 将设备父设备 还原到 原来的父设备 */
			dev->parent = old_parent;
			if (old_parent) {
                /* 将设备添加到 原来的父设备的子设备链表中 */
				klist_add_tail(&dev->p->knode_parent,
					       &old_parent->p->klist_children);
				set_dev_node(dev, dev_to_node(old_parent));
			}
		}
        /* 清理一下新的父设备所属 class 的 glue_dirs */
		cleanup_glue_dir(dev, new_parent_kobj);
        /* 释放引用的设备 */
		put_device(new_parent);
        /* 既然都出错了，就带着错误码返回吧。。。 */
		goto out;
	}
    /* // TODO: dpm 相关，待详细分析 */
	switch (dpm_order) {
	case DPM_ORDER_NONE:
		break;
	case DPM_ORDER_DEV_AFTER_PARENT:
		device_pm_move_after(dev, new_parent);
		break;
	case DPM_ORDER_PARENT_BEFORE_DEV:
		device_pm_move_before(new_parent, dev);
		break;
	case DPM_ORDER_DEV_LAST:
		device_pm_move_last(dev);
		break;
	}
out_put:
	put_device(old_parent);
out:
	device_pm_unlock();
	put_device(dev);
	return error;
}
EXPORT_SYMBOL_GPL(device_move);

/**
 * device_shutdown - call ->shutdown() on each device to shutdown.
 */
/* 为每一个设备调用 shutdown() */
void device_shutdown(void)
{
	struct device *dev;

    /* 锁定设备 devices_kset, 不能再添加/移除设备了 */
	spin_lock(&devices_kset->list_lock);
	/*
	 * Walk the devices list backward, shutting down each in turn.
	 * Beware that device unplug events may also start pulling
	 * devices offline, even as the system is shutting down.
	 */
    /* 依次向后遍历设备列表，逐个关闭设备。
     * 请注意，即使系统正在关闭，设备拔出事件也可能导致设备离线。 */
	while (!list_empty(&devices_kset->list)) {
        /* 从 devices_kset 的链表中 找到的设备结构 struct device */
		dev = list_entry(devices_kset->list.prev, struct device,
				kobj.entry);
        /* 引用一下设备 */
		get_device(dev);
		/*
		 * Make sure the device is off the kset list, in the
		 * event that dev->*->shutdown() doesn't remove it.
		 */
        /* 将设备从 devices_kset 链表中移除
         * 以防 dev->*->shutdown() 无法将其删除 */
		list_del_init(&dev->kobj.entry);
        /* 解锁 devices_kset ，下面的操作不涉及 devices_kset */
		spin_unlock(&devices_kset->list_lock);

		/* Don't allow any more runtime suspends */
        /* // TODO: pm 相关，待详细分析 */
		pm_runtime_get_noresume(dev);
		pm_runtime_barrier(dev);

        /* 如果设备属于某个 bus，并且bus提供了 shutdown
         * 则优先调用 bus 的 shutdown
         * 否则调用 驱动提供的 shutdown
         * 这两者不会都调用，优先 bus 原则 */
		if (dev->bus && dev->bus->shutdown) {
			dev_dbg(dev, "shutdown\n");
			dev->bus->shutdown(dev);
		} else if (dev->driver && dev->driver->shutdown) {
			dev_dbg(dev, "shutdown\n");
			dev->driver->shutdown(dev);
		}
        /* 释放引用的设备 */
		put_device(dev);

        /* 再次锁定 devices_kset, 要再次遍历链表了 */
		spin_lock(&devices_kset->list_lock);
	}
    /* 解锁 devices_kset 锁，完成所有设备遍历操作了 */
	spin_unlock(&devices_kset->list_lock);
    /* 同步所有的异步函数调用
     * 此函数等待所有异步函数调用完成 */
	async_synchronize_full();
}

/*
 * Device logging functions
 */

#ifdef CONFIG_PRINTK
int __dev_printk(const char *level, const struct device *dev,
		 struct va_format *vaf)
{
	char dict[128];
	size_t dictlen = 0;
	const char *subsys;

	if (!dev)
		return printk("%s(NULL device *): %pV", level, vaf);

	if (dev->class)
		subsys = dev->class->name;
	else if (dev->bus)
		subsys = dev->bus->name;
	else
		goto skip;

	dictlen += snprintf(dict + dictlen, sizeof(dict) - dictlen,
			    "SUBSYSTEM=%s", subsys);

	/*
	 * Add device identifier DEVICE=:
	 *   b12:8         block dev_t
	 *   c127:3        char dev_t
	 *   n8            netdev ifindex
	 *   +sound:card0  subsystem:devname
	 */
	if (MAJOR(dev->devt)) {
		char c;

		if (strcmp(subsys, "block") == 0)
			c = 'b';
		else
			c = 'c';
		dictlen++;
		dictlen += snprintf(dict + dictlen, sizeof(dict) - dictlen,
				   "DEVICE=%c%u:%u",
				   c, MAJOR(dev->devt), MINOR(dev->devt));
	} else if (strcmp(subsys, "net") == 0) {
		struct net_device *net = to_net_dev(dev);

		dictlen++;
		dictlen += snprintf(dict + dictlen, sizeof(dict) - dictlen,
				    "DEVICE=n%u", net->ifindex);
	} else {
		dictlen++;
		dictlen += snprintf(dict + dictlen, sizeof(dict) - dictlen,
				    "DEVICE=+%s:%s", subsys, dev_name(dev));
	}
skip:
	return printk_emit(0, level[1] - '0',
			   dictlen ? dict : NULL, dictlen,
			   "%s %s: %pV",
			   dev_driver_string(dev), dev_name(dev), vaf);
}
EXPORT_SYMBOL(__dev_printk);

int dev_printk(const char *level, const struct device *dev,
	       const char *fmt, ...)
{
	struct va_format vaf;
	va_list args;
	int r;

	va_start(args, fmt);

	vaf.fmt = fmt;
	vaf.va = &args;

	r = __dev_printk(level, dev, &vaf);
	va_end(args);

	return r;
}
EXPORT_SYMBOL(dev_printk);

#define define_dev_printk_level(func, kern_level)		\
int func(const struct device *dev, const char *fmt, ...)	\
{								\
	struct va_format vaf;					\
	va_list args;						\
	int r;							\
								\
	va_start(args, fmt);					\
								\
	vaf.fmt = fmt;						\
	vaf.va = &args;						\
								\
	r = __dev_printk(kern_level, dev, &vaf);		\
	va_end(args);						\
								\
	return r;						\
}								\
EXPORT_SYMBOL(func);

define_dev_printk_level(dev_emerg, KERN_EMERG);
define_dev_printk_level(dev_alert, KERN_ALERT);
define_dev_printk_level(dev_crit, KERN_CRIT);
define_dev_printk_level(dev_err, KERN_ERR);
define_dev_printk_level(dev_warn, KERN_WARNING);
define_dev_printk_level(dev_notice, KERN_NOTICE);
define_dev_printk_level(_dev_info, KERN_INFO);

#endif
