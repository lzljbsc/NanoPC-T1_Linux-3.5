/*
 * device.h - generic, centralized driver model
 *
 * Copyright (c) 2001-2003 Patrick Mochel <mochel@osdl.org>
 * Copyright (c) 2004-2009 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2008-2009 Novell Inc.
 *
 * This file is released under the GPLv2
 *
 * See Documentation/driver-model/ for more information.
 * 更多信息参考 Documentation/driver-model/ 目录
 */

#ifndef _DEVICE_H_
#define _DEVICE_H_

#include <linux/ioport.h>
#include <linux/kobject.h>
#include <linux/klist.h>
#include <linux/list.h>
#include <linux/lockdep.h>
#include <linux/compiler.h>
#include <linux/types.h>
#include <linux/mutex.h>
#include <linux/pm.h>
#include <linux/atomic.h>
#include <linux/ratelimit.h>
#include <asm/device.h>

struct device;
struct device_private;
struct device_driver;
struct driver_private;
struct module;
struct class;
struct subsys_private;
struct bus_type;
struct device_node;
struct iommu_ops;

/* bus 属性，与 kobj_attribute 结构形式一致
 * 只是这里 show/store 方法的参数类型变成了 bus_type */
/* 所有的 xxx_attribute 属性结构处理方法都是类似的
 * 定义了一个通用的 show/store 方法，这两个方法中，根据 attr 找到所属的
 * xxx_attribute 结构，然后再调用 xxx_attribute 结构中的 show/store 方法 */
struct bus_attribute {
	struct attribute	attr;
	ssize_t (*show)(struct bus_type *bus, char *buf);
	ssize_t (*store)(struct bus_type *bus, const char *buf, size_t count);
};

/* bus_attribute 属性创建宏 */
#define BUS_ATTR(_name, _mode, _show, _store)	\
struct bus_attribute bus_attr_##_name = __ATTR(_name, _mode, _show, _store)

/* bus 创建/移除 属性文件 */
extern int __must_check bus_create_file(struct bus_type *,
					struct bus_attribute *);
extern void bus_remove_file(struct bus_type *, struct bus_attribute *);

/**
 * struct bus_type - The bus type of the device
 *
 * @name:	The name of the bus.
 * @dev_name:	Used for subsystems to enumerate devices like ("foo%u", dev->id).
 * @dev_root:	Default device to use as the parent.
 * @bus_attrs:	Default attributes of the bus.
 * @dev_attrs:	Default attributes of the devices on the bus.
 * @drv_attrs:	Default attributes of the device drivers on the bus.
 * @match:	Called, perhaps multiple times, whenever a new device or driver
 *		is added for this bus. It should return a nonzero value if the
 *		given device can be handled by the given driver.
 * @uevent:	Called when a device is added, removed, or a few other things
 *		that generate uevents to add the environment variables.
 * @probe:	Called when a new device or driver add to this bus, and callback
 *		the specific driver's probe to initial the matched device.
 * @remove:	Called when a device removed from this bus.
 * @shutdown:	Called at shut-down time to quiesce the device.
 * @suspend:	Called when a device on this bus wants to go to sleep mode.
 * @resume:	Called to bring a device on this bus out of sleep mode.
 * @pm:		Power management operations of this bus, callback the specific
 *		device driver's pm-ops.
 * @iommu_ops:  IOMMU specific operations for this bus, used to attach IOMMU
 *              driver implementations to a bus and allow the driver to do
 *              bus-specific setup
 * @p:		The private data of the driver core, only the driver core can
 *		touch this.
 *
 * A bus is a channel between the processor and one or more devices. For the
 * purposes of the device model, all devices are connected via a bus, even if
 * it is an internal, virtual, "platform" bus. Buses can plug into each other.
 * A USB controller is usually a PCI device, for example. The device model
 * represents the actual connections between buses and the devices they control.
 * A bus is represented by the bus_type structure. It contains the name, the
 * default attributes, the bus' methods, PM operations, and the driver core's
 * private data.
 */
/* 设备 bus 类型 */
struct bus_type {
    /* 总线名称，如 platform  usb  */
	const char		*name;
    /* 用于子系统枚举设备等，如 ("foo%u", dev->id)
     * 在 device_add 中，如果没有设置设备名，就使用 dev_name 组织设备名
     * dev_set_name(dev, "%s%u", dev->bus->dev_name, dev->id);  */
	const char		*dev_name;
    /* 表示要用于父设备的默认设备 */
    /* bus_type 的根设备，用于 subsys 子系统中
     * subsys 会注册一个以 subsys 命名的虚拟设备做为 subsys 的 dev_root
     * 属于 subsys 的 设备，可能以 dev_root 为 父设备
     * 参考 subsys_system_register 函数流程 */
	struct device		*dev_root;
    /* bus 默认属性 */
	struct bus_attribute	*bus_attrs;
    /* 此bus上的 device 默认属性 */
	struct device_attribute	*dev_attrs;
    /* 此bus上的 driver 默认属性 */
	struct driver_attribute	*drv_attrs;

    /* 是一个需要由具体bus驱动实现的回调函数，当属于该bus的所有 device 和 driver
     * 添加到内核时，内核都会调用该接口函数 */
	int (*match)(struct device *dev, struct device_driver *drv);
    /* 是一个由具体的bus驱动实现的回调函数，当属于该bus的设备，触发添加、移除或
     * 者其它动作时，bus模块核心就会调用该接口，这样可以让bus的驱动能够修改环境
     * 变量 */
	int (*uevent)(struct device *dev, struct kobj_uevent_env *env);
    /* probe/remove 两个回调函数，当有新的设备或者驱动添加到这个 bus 时，内核则
     * 会首先调用这个bus的 probe，然后再调用具体驱动程序的 probe 去初始化匹配设
     * 备；当有设备从这个 bus 上移除的时候则会调用 remove，非常重要的两个函数 */
    /* 注意，这里的 probe 是 bus 的优先，bus未提供时则调用 驱动程序的
     * bus 和 驱动程序 两个并不会都调用 */
	int (*probe)(struct device *dev);
	int (*remove)(struct device *dev);
    
    /* 在需要 shutdown 的时候调用，以让设备停止工作。与电源管理有关 */
	void (*shutdown)(struct device *dev);

    /* 需要让设备休眠或退出休眠时调用 */
	int (*suspend)(struct device *dev, pm_message_t state);
	int (*resume)(struct device *dev);

    /* bus 的电源管理操作，会调用执行特定驱动程序的 pm 的 ops */
	const struct dev_pm_ops *pm;

	struct iommu_ops *iommu_ops;

    /* 驱动核心的私有数据，只有驱动核心可以访问使用 */
	struct subsys_private *p;
};

/* This is a #define to keep the compiler from merging different
 * instances of the __key variable */
/* 一个宏定义，用于 bus 注册 
 * 这个宏使用时，每个 __key 变量都是不同的，不会被合并
 * 注意，这里的 __key 变量，在以后的版本中被加到了 bus_type 结构中 
 * 就把这个宏取消掉了 */
#define bus_register(subsys)			\
({						\
	static struct lock_class_key __key;	\
	__bus_register(subsys, &__key);	\
})
/* 注册 / 注销  bus 总线 */
extern int __must_check __bus_register(struct bus_type *bus,
				       struct lock_class_key *key);
extern void bus_unregister(struct bus_type *bus);

/* 重新扫描总线上的设备，以查找可能的驱动程序 */
extern int __must_check bus_rescan_devices(struct bus_type *bus);

/* iterator helpers for buses */
/* subsys 使用的迭代器
 * 是一个普通的 klist_iter 迭代器
 * 增加了一个设备 device_type 指针，记录迭代节点的设备类型指针
 * 用来在迭代时过滤设备使用 */
struct subsys_dev_iter {
	struct klist_iter		ki;
	const struct device_type	*type;
};
/* subsys 设备迭代器 */
/* subsys 迭代器初始化，可以提供用于过滤的 device_type */
void subsys_dev_iter_init(struct subsys_dev_iter *iter,
			 struct bus_type *subsys,
			 struct device *start,
			 const struct device_type *type);
/* subsys 迭代下一个设备结构 */
struct device *subsys_dev_iter_next(struct subsys_dev_iter *iter);
/* subsys 迭代器退出 */
void subsys_dev_iter_exit(struct subsys_dev_iter *iter);

/* bus 总线设备遍历，为每一个设备调用 @fn 回调函数 */
int bus_for_each_dev(struct bus_type *bus, struct device *start, void *data,
		     int (*fn)(struct device *dev, void *data));
/* bus 总线查找指定设备，匹配规则由 match 提供 */
struct device *bus_find_device(struct bus_type *bus, struct device *start,
			       void *data,
			       int (*match)(struct device *dev, void *data));
/* bus 设备迭代器，查找具有特定名称的特定设备结构 */
struct device *bus_find_device_by_name(struct bus_type *bus,
				       struct device *start,
				       const char *name);
/* subsys 查找具有特定枚举号的设备 */
struct device *subsys_find_device_by_id(struct bus_type *bus, unsigned int id,
					struct device *hint);
/* bus 总线驱动遍历，为每一个驱动调用 @fn 回调函数 */
int bus_for_each_drv(struct bus_type *bus, struct device_driver *start,
		     void *data, int (*fn)(struct device_driver *, void *));
/* 对bus上的所有设备 进行 广度优先 算法排序 */
void bus_sort_breadthfirst(struct bus_type *bus,
			   int (*compare)(const struct device *a,
					  const struct device *b));
/*
 * Bus notifiers: Get notified of addition/removal of devices
 * and binding/unbinding of drivers to devices.
 * In the long run, it should be a replacement for the platform
 * notify hooks.
 */
/* 总线通知程序：获取设备添加/移除以及设备绑定/解绑定驱动程序的通知。从长远来看，
 * 它应该是平台通知钩子的替代品。 */
struct notifier_block;

/* 注册 bus 总线通知器 */
extern int bus_register_notifier(struct bus_type *bus,
				 struct notifier_block *nb);
/* 注销 bus 总线通知器 */
extern int bus_unregister_notifier(struct bus_type *bus,
				   struct notifier_block *nb);

/* All 4 notifers below get called with the target struct device *
 * as an argument. Note that those functions are likely to be called
 * with the device lock held in the core, so be careful.
 */
/* bus 通知器的事件
 * 注意，通知器的回调函数，可能在设备被锁定（device_lock）时调用 */
/* 添加设备 */
#define BUS_NOTIFY_ADD_DEVICE		0x00000001 /* device added */
/* 移除设备 */
#define BUS_NOTIFY_DEL_DEVICE		0x00000002 /* device removed */
/* 设备将要与驱动绑定（此时还没有绑定） */
#define BUS_NOTIFY_BIND_DRIVER		0x00000003 /* driver about to be
						      bound */
/* 设备与驱动程序绑定（已经绑定了） */
#define BUS_NOTIFY_BOUND_DRIVER		0x00000004 /* driver bound to device */
/* 设备将要与驱动解绑（此时还没有解绑） */
#define BUS_NOTIFY_UNBIND_DRIVER	0x00000005 /* driver about to be
						      unbound */
/* 设备与驱动程序解绑（已经解绑了） */
#define BUS_NOTIFY_UNBOUND_DRIVER	0x00000006 /* driver is unbound
						      from the device */

/* 获取bus的 kset 结构, 谨慎使用 */
extern struct kset *bus_get_kset(struct bus_type *bus);
/* 获取 bus 的 klist_devices 链表指针, 谨慎使用 */
extern struct klist *bus_get_device_klist(struct bus_type *bus);

/**
 * struct device_driver - The basic device driver structure
 * @name:	Name of the device driver.
 * @bus:	The bus which the device of this driver belongs to.
 * @owner:	The module owner.
 * @mod_name:	Used for built-in modules.
 * @suppress_bind_attrs: Disables bind/unbind via sysfs.
 * @of_match_table: The open firmware table.
 * @probe:	Called to query the existence of a specific device,
 *		whether this driver can work with it, and bind the driver
 *		to a specific device.
 * @remove:	Called when the device is removed from the system to
 *		unbind a device from this driver.
 * @shutdown:	Called at shut-down time to quiesce the device.
 * @suspend:	Called to put the device to sleep mode. Usually to a
 *		low power state.
 * @resume:	Called to bring a device from sleep mode.
 * @groups:	Default attributes that get created by the driver core
 *		automatically.
 * @pm:		Power management operations of the device which matched
 *		this driver.
 * @p:		Driver core's private data, no one other than the driver
 *		core can touch this.
 *
 * The device driver-model tracks all of the drivers known to the system.
 * The main reason for this tracking is to enable the driver core to match
 * up drivers with new devices. Once drivers are known objects within the
 * system, however, a number of other things become possible. Device drivers
 * can export information and configuration variables that are independent
 * of any specific device.
 */
/* 设备驱动结构
 *
 * 设备驱动模型跟踪系统已知的所有驱动程序。
 * 这种跟踪的主要原因是使驱动程序核心能够匹配驱动程序与新设备。
 * 然而，一旦驱动程序成为系统内的已知对象，许多其它的事情就成为可能。
 * 设备驱动程序可以导出独立于任何特定设备的信息和配置变量。
 * */
struct device_driver {
    /* 驱动名称，将与设备名称进行匹配 */
    /* 该名字也是驱动程序对应的设备模型kobject 的名字 
     * 会被添加到 /sys/bus/[bus.name]/drivers/ 目录中 */
	const char		*name;
    /* 此驱动程序的设备所属的总线 */
    /* 一个驱动程序必须依附在一个总线上，没有游离的驱动程序 */
	struct bus_type		*bus;

	struct module		*owner;
	const char		*mod_name;	/* used for built-in modules */

    /* 抑制 bind/unbind 属性
     * 注意，这里是抑制，如果本参数为 true ，则不添加属性
     * 默认属性为0，也就是 false，会添加属性 */
	bool suppress_bind_attrs;	/* disables bind/unbind via sysfs */

    /* 设备树的匹配表  open firmware */
	const struct of_device_id	*of_match_table;

    /* 设备驱动 回调函数
     * probe: 用来查询特定设备是否存在、该驱动程序是否可以与其一起工作以及将驱动
     * 程序绑定到特定设备 */
	int (*probe) (struct device *dev);
    /* remove: 当设备从系统中删除时调用以解除设备与驱动程序的绑定 */
	int (*remove) (struct device *dev);
    /* shutdown: 在关机时调用以使设备停止工作 */
	void (*shutdown) (struct device *dev);
    /* suspend: 调用此函数将设备置于睡眠模式。通常为低功耗状态 */
	int (*suspend) (struct device *dev, pm_message_t state);
    /* resume: 调用此函数将设备从睡眠模式唤醒 */
	int (*resume) (struct device *dev);
    /* 由驱动内核自动创建的默认属性 */
	const struct attribute_group **groups;

    /* 电源管理相关 */
	const struct dev_pm_ops *pm;

    /* 驱动私有数据 */
	struct driver_private *p;
};


/* 驱动注册与注销
 * 在 drivers/base/driver.c 中实现 */
extern int __must_check driver_register(struct device_driver *drv);
extern void driver_unregister(struct device_driver *drv);

/* 在特定 bus 总线上，查找指定名字的驱动
 * 在 drivers/base/driver.c 中实现 */
extern struct device_driver *driver_find(const char *name,
					 struct bus_type *bus);
/* 确定探测序列是否完成 */
extern int driver_probe_done(void);
/* 等待设备探测完成 */
extern void wait_for_device_probe(void);


/* sysfs interface for exporting driver attributes */

/* 驱动属性结构， 参考 kobj_attribute */
struct driver_attribute {
	struct attribute attr;
	ssize_t (*show)(struct device_driver *driver, char *buf);
	ssize_t (*store)(struct device_driver *driver, const char *buf,
			 size_t count);
};

/* 驱动属性创建宏
 * 创建的属性结构具有 driver_attr_ 前缀 */
#define DRIVER_ATTR(_name, _mode, _show, _store)	\
struct driver_attribute driver_attr_##_name =		\
	__ATTR(_name, _mode, _show, _store)

/* 创建/移除 驱动属性
 * 在 drivers/base/driver.c 中实现 */
extern int __must_check driver_create_file(struct device_driver *driver,
					const struct driver_attribute *attr);
extern void driver_remove_file(struct device_driver *driver,
			       const struct driver_attribute *attr);

/* 遍历绑定到驱动程序上的设备 */
extern int __must_check driver_for_each_device(struct device_driver *drv,
					       struct device *start,
					       void *data,
					       int (*fn)(struct device *dev,
							 void *));
/* 用于定位特定设备的设备迭代器 */
struct device *driver_find_device(struct device_driver *drv,
				  struct device *start, void *data,
				  int (*match)(struct device *dev, void *data));

/**
 * struct subsys_interface - interfaces to device functions
 * @name:       name of the device function
 * @subsys:     subsytem of the devices to attach to
 * @node:       the list of functions registered at the subsystem
 * @add_dev:    device hookup to device function handler
 * @remove_dev: device hookup to device function handler
 *
 * Simple interfaces attached to a subsystem. Multiple interfaces can
 * attach to a subsystem and its devices. Unlike drivers, they do not
 * exclusively claim or control devices. Interfaces usually represent
 * a specific functionality of a subsystem/class of devices.
 */
/* struct subsys_interface - 设备功能接口
 * @name:   设备功能名称(应该只是个名字，没看到用的地方)
 * @subsys: 要连接的设备的子系统(一个有效的bus_type)
 * @node:   链表节点，连接到 bus_type 的私有数据 interface 链表头
 * @add_dev:添加设备时的钩子函数
 * @remove_dev:移除设备时的钩子函数
 *
 * 连接到子系统的简单接口。多个接口可以连接到一个子系统及其设备。
 * 与驱动程序不同，它们并不独占或控制设备。
 * 接口通常代表子系统/设备类的特定功能。
 *
 * 可以理解为这是一个某 bus 总线上的设备功能钩子函数。将其附加到某个总线上，
 * 当总线上添加/移除设备时，会调用相应的设备。
 * 目前只在 cpufreq 模块的代码中有实际应用 */
struct subsys_interface {
    /* 功能接口名称 */
	const char *name;
    /* 连接到的子系统 */
	struct bus_type *subsys;
    /* 附加到子系统 interface 链表的节点 */
	struct list_head node;
    /* 当子系统添加设备时调用 */
	int (*add_dev)(struct device *dev, struct subsys_interface *sif);
    /* 当子系统移除设备时调用 */
	int (*remove_dev)(struct device *dev, struct subsys_interface *sif);
};

/* 向 subsys 注册 subsys_interface */
int subsys_interface_register(struct subsys_interface *sif);
/* 从 subsys 移除 subsys_interface */
void subsys_interface_unregister(struct subsys_interface *sif);

/* 注册 subsys
 * subsys 本质是一个 bus_type，但会注册一个同名的虚拟设备
 * 以后注册的所有属于本 subsys 的设备都在 虚拟设备 子目录下 */
int subsys_system_register(struct bus_type *subsys,
			   const struct attribute_group **groups);

/**
 * struct class - device classes
 * @name:	Name of the class.
 * @owner:	The module owner.
 * @class_attrs: Default attributes of this class.
 * @dev_attrs:	Default attributes of the devices belong to the class.
 * @dev_bin_attrs: Default binary attributes of the devices belong to the class.
 * @dev_kobj:	The kobject that represents this class and links it into the hierarchy.
 * @dev_uevent:	Called when a device is added, removed from this class, or a
 *		few other things that generate uevents to add the environment
 *		variables.
 * @devnode:	Callback to provide the devtmpfs.
 * @class_release: Called to release this class.
 * @dev_release: Called to release the device.
 * @suspend:	Used to put the device to sleep mode, usually to a low power
 *		state.
 * @resume:	Used to bring the device from the sleep mode.
 * @ns_type:	Callbacks so sysfs can detemine namespaces.
 * @namespace:	Namespace of the device belongs to this class.
 * @pm:		The default device power management operations of this class.
 * @p:		The private data of the driver core, no one other than the
 *		driver core can touch this.
 *
 * A class is a higher-level view of a device that abstracts out low-level
 * implementation details. Drivers may see a SCSI disk or an ATA disk, but,
 * at the class level, they are all simply disks. Classes allow user space
 * to work with devices based on what they do, rather than how they are
 * connected or how they work.
 */
struct class {
    /* class 名， /sys/class 中的目录名 */
	const char		*name;
	struct module		*owner;

    /* class 默认的属性 */
	struct class_attribute		*class_attrs;
    /* 属于这个 class 的设备的默认属性 */
	struct device_attribute		*dev_attrs;
    /* 属于这个 class 的设备的默认bin属性 */
	struct bin_attribute		*dev_bin_attrs;
    /* 表示该类的基类类型（block char），也是将链接到层次结构中的 kobject
     * 有两种可能指向： sysfs_dev_block_kobj  sysfs_dev_char_kobj 
     * 分别表示 块设备目录 /sys/dev/block/  字符设备目录 /sys/dev/char/ 
     * 这里的理解是，这个新的类，它的基类是什么，要么是字符设备，要么是块设备
     * 比如 mtd 设备，在创建 mtd类 时，会把 mtd_class 结构中的 dev_kobj 设置为
     * sysfs_dev_char_kobj ，表示它是一个字符设备类
     * 在注册mtd设备时，会使用该类的类型，在 /sys/dev/ 下选择 block 或者 char 目
     * 录 用于创建设备的链接文件，参考 device_create_sys_dev_entry */
	struct kobject			*dev_kobj;

    /* 当一个设备从这个class 中被添加、移除，或者一些其它的 uevent 事件时，用于
     * 添加环境变量 */
	int (*dev_uevent)(struct device *dev, struct kobj_uevent_env *env);
    /* device_get_devnode 函数中使用
     * 用于在生成设备节点名称时使用
     * 大部分的设备都是以设备名 命名设备节点名
     * 这里可以参考 input 子系统，提供了 input_devnode 回调
     * 生成的设备节点路径为 input/eventx 等设备节点
     * 带有一个 input 目录，就是在 input_devnode 回调中处理的 */
	char *(*devnode)(struct device *dev, umode_t *mode);

    /* 释放 class 时回调函数 */
	void (*class_release)(struct class *class);
    /* 将某个设备从 class 中移除时回调函数 */
	void (*dev_release)(struct device *dev);

	int (*suspend)(struct device *dev, pm_message_t state);
	int (*resume)(struct device *dev);

	const struct kobj_ns_type_operations *ns_type;
	const void *(*namespace)(struct device *dev);

	const struct dev_pm_ops *pm;

    /* 驱动核心的私有数据，只有驱动核心可以访问使用 */
	struct subsys_private *p;
};

/* class 使用的迭代器
 * 是一个普通的 klist_iter 迭代器
 * 增加了一个设备 device_type 指针，记录迭代节点的设备类型指针
 * 用来在迭代时过滤设备使用 */
struct class_dev_iter {
	struct klist_iter		ki;
	const struct device_type	*type;
};

/* 两个最基本的设备类型
 * 任何设备都属于这两种类型中的某一种
 * 也就是说，任何 class ，都属于这两种 基类的一种
 * 一个是 块设备类型，另一个是 字符设备类型
 * 这两个是用来标识设备最基本的类型的
 * 在 class 结构中，使用 dev_kobj 指向，
 * 在 给设备创建 /sys/dev/ 下的链接时，
 * 用于选择 /sys/dev/block/ 还是 /sys/dev/char/ 目录 */
extern struct kobject *sysfs_dev_block_kobj;
extern struct kobject *sysfs_dev_char_kobj;

/* 注册/注销一个 class */
extern int __must_check __class_register(struct class *class,
					 struct lock_class_key *key);
extern void class_unregister(struct class *class);

/* This is a #define to keep the compiler from merging different
 * instances of the __key variable */
/* 与 bus_register 类似
 * 这里的宏也是为了防止合并 __key 变量
 * 这是一个程序块，所以内部是一个独立的静态变量 */
#define class_register(class)			\
({						\
	static struct lock_class_key __key;	\
	__class_register(class, &__key);	\
})

/* 兼容class 处理
 * 是一个简单的 /sys/class/ 下的目录，用于兼容以前的驱动/应用程序 */
struct class_compat;
/* 兼容class 注册/注销 */
struct class_compat *class_compat_register(const char *name);
void class_compat_unregister(struct class_compat *cls);
/* 兼容class 目录下，创建设备链接；设备目录下创建 device 链接 */
int class_compat_create_link(struct class_compat *cls, struct device *dev,
			     struct device *device_link);
/* 移除兼容class 相关链接 */
void class_compat_remove_link(struct class_compat *cls, struct device *dev,
			      struct device *device_link);

/* class 的设备迭代器初始化 */
extern void class_dev_iter_init(struct class_dev_iter *iter,
				struct class *class,
				struct device *start,
				const struct device_type *type);
/* class 迭代下一个设备结构 */
extern struct device *class_dev_iter_next(struct class_dev_iter *iter);
/* class 退出迭代器 */
extern void class_dev_iter_exit(struct class_dev_iter *iter);

/* class 遍历每一个属于class 的设备，并调用 @fn 回调 */
extern int class_for_each_device(struct class *class, struct device *start,
				 void *data,
				 int (*fn)(struct device *dev, void *data));
/* class 定位特定设备的迭代器， match 函数确认特定设备 */
extern struct device *class_find_device(struct class *class,
					struct device *start, void *data,
					int (*match)(struct device *, void *));

/* class 属性结构
 * 与 device 等其它结构的属性相比，多了 namespace 回调 */
struct class_attribute {
    /* 通用的 sysfs 属性结构 */
	struct attribute attr;
    /* 属性 show 方法 */
	ssize_t (*show)(struct class *class, struct class_attribute *attr,
			char *buf);
    /* 属性 store 方法 */
	ssize_t (*store)(struct class *class, struct class_attribute *attr,
			const char *buf, size_t count);
    /* 属性 namespace 方法 */
	const void *(*namespace)(struct class *class,
				 const struct class_attribute *attr);
};

/* class 属性定义宏，简化属性结构定义 */
#define CLASS_ATTR(_name, _mode, _show, _store)			\
struct class_attribute class_attr_##_name = __ATTR(_name, _mode, _show, _store)

/* 创建/移除 class 的 属性文件 */
extern int __must_check class_create_file(struct class *class,
					  const struct class_attribute *attr);
extern void class_remove_file(struct class *class,
			      const struct class_attribute *attr);

/* Simple class attribute that is just a static string */

/* 用于简化 class 属性处理的，提供了一种仅针对字符串操作的属性结构
 * 注意，属性的字符串需要是静态字符串
 * 在 show_class_attr_string 方法中，只是读取了一下 */
struct class_attribute_string {
    /* class 属性结构，使用该属性结构进行注册 */
	struct class_attribute attr;
    /* 静态字符串，属性 show 方法需要操作的数据 */
	char *str;
};

/* Currently read-only only */
/* 快速定义 class_attribute_string 结构，定义一个属性
 * 注意，当前仅支持读(show) 方法 */
#define _CLASS_ATTR_STRING(_name, _mode, _str) \
	{ __ATTR(_name, _mode, show_class_attr_string, NULL), _str }
#define CLASS_ATTR_STRING(_name, _mode, _str) \
	struct class_attribute_string class_attr_##_name = \
		_CLASS_ATTR_STRING(_name, _mode, _str)

/* class_attribute_string 属性结构的 show 方法 */
extern ssize_t show_class_attr_string(struct class *class, struct class_attribute *attr,
                        char *buf);

/* class 类 的 设备功能接口
 * 可以参考 subsys_interface 结构注释 */
struct class_interface {
    /* 附加到子系统 interfaces 链表的节点 */
	struct list_head	node;
    /* 链接到的子系统，所属的 class */
	struct class		*class;

    /* 当子系统添加设备时调用 */
	int (*add_dev)		(struct device *, struct class_interface *);
    /* 当子系统移除设备时调用 */
	void (*remove_dev)	(struct device *, struct class_interface *);
};

/* 向 class 注册 class_interface */
extern int __must_check class_interface_register(struct class_interface *);
/* 从 class 移除 class_interface */
extern void class_interface_unregister(struct class_interface *);

/* 创建/销毁 一个 struct class
 * 注意，创建后会自动注册 */
extern struct class * __must_check __class_create(struct module *owner,
						  const char *name,
						  struct lock_class_key *key);
extern void class_destroy(struct class *cls);

/* This is a #define to keep the compiler from merging different
 * instances of the __key variable */
/* 创建一个 struct class
 * 与 class_register 类似，为了避免 __key 被覆盖 */
#define class_create(owner, name)		\
({						\
	static struct lock_class_key __key;	\
	__class_create(owner, name, &__key);	\
})

/*
 * The type of device, "struct device" is embedded in. A class
 * or bus can contain devices of different types
 * like "partitions" and "disks", "mouse" and "event".
 * This identifies the device type and carries type-specific
 * information, equivalent to the kobj_type of a kobject.
 * If "name" is specified, the uevent will contain it in
 * the DEVTYPE variable.
 */
/* struct device_type 表示设备的类型，该结构体一般被嵌入使用。
 * 一个 class 或一个 总线 可以包含不同类型的设备， 比如 “分区” 和 “磁盘” 
 * “鼠标” 和 “事件” 
 * 该结构体的理解类似于 kobject 的 kobj_type , 如果特指了 name ，
 * uevent 将把它包含在 DEVTYPE 变量中 */
struct device_type {
    /* 设备类型名称，当设备添加到内核时，会触发 "DEVTYPE=name"的 uevent 事件 
     * 通知用户空间某个类型的设备可用 */
	const char *name;
    /* 该类型设备默认的属性集合 */
	const struct attribute_group **groups;
    /* 同一设备类型公共的 uevent 发送接口，和 struct kobj_type 类似 */
	int (*uevent)(struct device *dev, struct kobj_uevent_env *env);
    /* 用于在生成设备节点名称时使用
     * 对某些设备类型，可以提供 devnode 回调特殊处理一下
     * 与 class 中的 devnode 回调类似 */
	char *(*devnode)(struct device *dev, umode_t *mode);
    /* 同一设备类型共用的释放接口，如果设备有自定义，优先使用自己的 */
	void (*release)(struct device *dev);

    /* 电源管理相关 */
	const struct dev_pm_ops *pm;
};

/* interface for exporting device attributes */
/* 设备属性结构， 参考 kobj_attribute */
struct device_attribute {
	struct attribute	attr;
	ssize_t (*show)(struct device *dev, struct device_attribute *attr,
			char *buf);
	ssize_t (*store)(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count);
};

/* 设备扩展属性结构
 * 相比 device_attribute 结构多了一个 void *var 指针
 * 用来简化对 ulong 或 int 类型数据的属性操作
 * 提供了通用的 device_show_ulong 等几个操作函数
 * 定义该类属性时，使用 DEVICE_ULONG_ATTR DEVICE_INT_ATTR 等宏 */
struct dev_ext_attribute {
	struct device_attribute attr;
	void *var;
};

/* 用于使用 dev_ext_attribute 创建的属性
 * 属性为 ulong 或 int 类型，具有统一的操作，简化驱动编写 */
ssize_t device_show_ulong(struct device *dev, struct device_attribute *attr,
			  char *buf);
ssize_t device_store_ulong(struct device *dev, struct device_attribute *attr,
			   const char *buf, size_t count);
ssize_t device_show_int(struct device *dev, struct device_attribute *attr,
			char *buf);
ssize_t device_store_int(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count);

/* 设备属性创建宏，简化驱动编写 */
#define DEVICE_ATTR(_name, _mode, _show, _store) \
	struct device_attribute dev_attr_##_name = __ATTR(_name, _mode, _show, _store)
#define DEVICE_ULONG_ATTR(_name, _mode, _var) \
	struct dev_ext_attribute dev_attr_##_name = \
		{ __ATTR(_name, _mode, device_show_ulong, device_store_ulong), &(_var) }
#define DEVICE_INT_ATTR(_name, _mode, _var) \
	struct dev_ext_attribute dev_attr_##_name = \
		{ __ATTR(_name, _mode, device_show_int, device_store_int), &(_var) }
#define DEVICE_ATTR_IGNORE_LOCKDEP(_name, _mode, _show, _store) \
	struct device_attribute dev_attr_##_name =		\
		__ATTR_IGNORE_LOCKDEP(_name, _mode, _show, _store)

/* 在指定的设备下创建/移除 属性
 * 可以由其它驱动直接调用，直接创建属性
 * 推荐的方法是使用 device 结构中的 groups ，这样可以自动完成属性文件创建 */
extern int device_create_file(struct device *device,
			      const struct device_attribute *entry);
extern void device_remove_file(struct device *dev,
			       const struct device_attribute *attr);
/* 创建/移除 设备二进制bin属性 */
extern int __must_check device_create_bin_file(struct device *dev,
					const struct bin_attribute *attr);
extern void device_remove_bin_file(struct device *dev,
				   const struct bin_attribute *attr);
extern int device_schedule_callback_owner(struct device *dev,
		void (*func)(struct device *dev), struct module *owner);

/* This is a macro to avoid include problems with THIS_MODULE */
#define device_schedule_callback(dev, func)			\
	device_schedule_callback_owner(dev, func, THIS_MODULE)

/* device resource management */
/* 设备资源管理 使用的回调函数 */
/* dr_release_t 资源释放函数 */
typedef void (*dr_release_t)(struct device *dev, void *res);
/* dr_match_t 查找资源时 的匹配函数 */
typedef int (*dr_match_t)(struct device *dev, void *res, void *match_data);

/* devres 调试版本配置 */
#ifdef CONFIG_DEBUG_DEVRES
/* 配置了 CONFIG_DEBUG_DEVRES 调试配置
 * 则实际实现为 __devres_alloc */
extern void *__devres_alloc(dr_release_t release, size_t size, gfp_t gfp,
			     const char *name);
#define devres_alloc(release, size, gfp) \
	__devres_alloc(release, size, gfp, #release)
#else
/* 分配一个设备资源管理结构, 不会加入到设备资源链表中 */
extern void *devres_alloc(dr_release_t release, size_t size, gfp_t gfp);
#endif
/* 释放设备资源管理结构 */
extern void devres_free(void *res);
/* 注册设备资源 到 设备中 */
extern void devres_add(struct device *dev, void *res);
/* 查找 匹配的设备资源，release/match 进行匹配 */
extern void *devres_find(struct device *dev, dr_release_t release,
			 dr_match_t match, void *match_data);
/* 查找匹配的资源管理结构，如果不存在，则将 new_res 添加到设备资源里 */
extern void *devres_get(struct device *dev, void *new_res,
			dr_match_t match, void *match_data);
/* 查找匹配的资源管理结构，并将其移除 */
extern void *devres_remove(struct device *dev, dr_release_t release,
			   dr_match_t match, void *match_data);
/* 查找匹配的资源管理结构，并将其移除 销毁 */
extern int devres_destroy(struct device *dev, dr_release_t release,
			  dr_match_t match, void *match_data);
/* 查找匹配的资源管理结构，并将管理的资源释放，资源结构移除 销毁 */
extern int devres_release(struct device *dev, dr_release_t release,
			  dr_match_t match, void *match_data);

/* devres group */
/* 创建/打开 一个新的 devres_group */
extern void * __must_check devres_open_group(struct device *dev, void *id,
					     gfp_t gfp);
/* 关闭一个 devres_group */
extern void devres_close_group(struct device *dev, void *id);
/* 移除一个 devres_group */
extern void devres_remove_group(struct device *dev, void *id);
/* 释放一个 devres_group */
extern int devres_release_group(struct device *dev, void *id);

/* managed kzalloc/kfree for device drivers, no kmalloc, always use kzalloc */
extern void *devm_kzalloc(struct device *dev, size_t size, gfp_t gfp);
extern void devm_kfree(struct device *dev, void *p);

void __iomem *devm_request_and_ioremap(struct device *dev,
			struct resource *res);

struct device_dma_parameters {
	/*
	 * a low level driver may set these to teach IOMMU code about
	 * sg limitations.
	 */
	unsigned int max_segment_size;
	unsigned long segment_boundary_mask;
};

/**
 * struct device - The basic device structure
 * @parent:	The device's "parent" device, the device to which it is attached.
 * 		In most cases, a parent device is some sort of bus or host
 * 		controller. If parent is NULL, the device, is a top-level device,
 * 		which is not usually what you want.
 * @p:		Holds the private data of the driver core portions of the device.
 * 		See the comment of the struct device_private for detail.
 * @kobj:	A top-level, abstract class from which other classes are derived.
 * @init_name:	Initial name of the device.
 * @type:	The type of device.
 * 		This identifies the device type and carries type-specific
 * 		information.
 * @mutex:	Mutex to synchronize calls to its driver.
 * @bus:	Type of bus device is on.
 * @driver:	Which driver has allocated this
 * @platform_data: Platform data specific to the device.
 * 		Example: For devices on custom boards, as typical of embedded
 * 		and SOC based hardware, Linux often uses platform_data to point
 * 		to board-specific structures describing devices and how they
 * 		are wired.  That can include what ports are available, chip
 * 		variants, which GPIO pins act in what additional roles, and so
 * 		on.  This shrinks the "Board Support Packages" (BSPs) and
 * 		minimizes board-specific #ifdefs in drivers.
 * @power:	For device power management.
 * 		See Documentation/power/devices.txt for details.
 * @pm_domain:	Provide callbacks that are executed during system suspend,
 * 		hibernation, system resume and during runtime PM transitions
 * 		along with subsystem-level and driver-level callbacks.
 * @numa_node:	NUMA node this device is close to.
 * @dma_mask:	Dma mask (if dma'ble device).
 * @coherent_dma_mask: Like dma_mask, but for alloc_coherent mapping as not all
 * 		hardware supports 64-bit addresses for consistent allocations
 * 		such descriptors.
 * @dma_parms:	A low level driver may set these to teach IOMMU code about
 * 		segment limitations.
 * @dma_pools:	Dma pools (if dma'ble device).
 * @dma_mem:	Internal for coherent mem override.
 * @archdata:	For arch-specific additions.
 * @of_node:	Associated device tree node.
 * @devt:	For creating the sysfs "dev".
 * @id:		device instance
 * @devres_lock: Spinlock to protect the resource of the device.
 * @devres_head: The resources list of the device.
 * @knode_class: The node used to add the device to the class list.
 * @class:	The class of the device.
 * @groups:	Optional attribute groups.
 * @release:	Callback to free the device after all references have
 * 		gone away. This should be set by the allocator of the
 * 		device (i.e. the bus driver that discovered the device).
 *
 * At the lowest level, every device in a Linux system is represented by an
 * instance of struct device. The device structure contains the information
 * that the device model core needs to model the system. Most subsystems,
 * however, track additional information about the devices they host. As a
 * result, it is rare for devices to be represented by bare device structures;
 * instead, that structure, like kobject structures, is usually embedded within
 * a higher-level representation of the device.
 */
struct device {
	/* 本设备的父节点，大部分情况下，父节点是bus
	   节点或主控制器，如果父节点为NULL，那就是
	   一个 (top-level)顶层设备，一般不是我们想
	   要的, 注意，父节点并不一定是一个真实的设备
       可能是虚拟设备，如 Platform 设备 */
    /* 在设备模型层次结构中，设备的父 可能是一个创造的 kobject
     * 这种情况参考 get_device_parent 函数中的分析
     * 这里的 parent 并不会变更，只会设置下面的 kobj 的 parent
     * 让设备在设备模型层次结构中体现出来 */
	struct device		*parent;

	/* 用于保存设备驱动核心部分的私有数据，详细的
	   描述可以参考 struct device_private 的描述 */
	struct device_private	*p;

    /* 该数据结构对应的 struct kobject
     * kobj 就是体现在 设备模型层次结构 中的目录 */
	struct kobject kobj;
    /* 设备的名称，同样会赋给 kobj , sysfs 中会显示 */
	const char		*init_name; /* initial name of the device */
    /* 设备类型，参考 struct device_type */
	const struct device_type *type;

    /* 使用互斥锁来同步对其驱动程序的调用
     * 在匹配/解绑 驱动程序时会进行锁定，保证匹配/解绑过程原子性 */
	struct mutex		mutex;	/* mutex to synchronize calls to
					 * its driver.
					 */

    /* 该设备挂在哪个总线上 */
	struct bus_type	*bus;		/* type of bus device is on */
    /* 该设备对应的驱动 */
	struct device_driver *driver;	/* which driver has allocated this
					   device */
    /* 设备的平台数据，例如：对于定制版的设备，典型的是嵌入式和基于SOC的硬件，
     * linux 经常使用 platform_data 指向该板的数据结构体，描述设备及其链接方式 
     * 这可能包含了可用的端口，芯片变量，GPIO等，这种做法缩小了 BSP， 
     * 并最小化驱动中的 #ifdefs */
	void		*platform_data;	/* Platform specific data, device
					   core doesn't touch it */
    /* 和电源管理相关 */
	struct dev_pm_info	power;
	struct dev_pm_domain	*pm_domain;

#ifdef CONFIG_NUMA
	int		numa_node;	/* NUMA node this device is close to */
#endif
    /* DMA 操作相关 */
	u64		*dma_mask;	/* dma mask (if dma'able device) */
	u64		coherent_dma_mask;/* Like dma_mask, but for
					     alloc_coherent mappings as
					     not all hardware supports
					     64 bit addresses for consistent
					     allocations such descriptors. */

	struct device_dma_parameters *dma_parms;

	struct list_head	dma_pools;	/* dma pools (if dma'ble) */

	struct dma_coherent_mem	*dma_mem; /* internal for coherent mem
					     override */
#ifdef CONFIG_CMA
	struct cma *cma_area;		/* contiguous memory area for dma
					   allocations */
#endif
	/* arch specific additions */
    /* 架构相关数据，定义在架构相关的 .h 中 */
	struct dev_archdata	archdata;

	struct device_node	*of_node; /* associated device tree node */

    /* 设备号， 创建 sysfs的 dev，一般由 Major 和 Minor 两部分组成 */
	dev_t			devt;	/* dev_t, creates the sysfs "dev" */
    /* 设备 id 号，用于 生成设备名的，用于一些使用序号命名的设备 */
	u32			id;	/* device instance */

    /* 设备资源保护锁 */
	spinlock_t		devres_lock;
    /* 设备的资源列表 */
	struct list_head	devres_head;

    /* 链接到所属的 class 的 klist_devices 链表节点 */
	struct klist_node	knode_class;
    /* 设备所属的 class */
	struct class		*class;
    /* 默认的属性 */
	const struct attribute_group **groups;	/* optional groups */

    /* 设备释放回调，会在引用计数为0时被调用 */
	void	(*release)(struct device *dev);
};

/* Get the wakeup routines, which depend on struct device */
#include <linux/pm_wakeup.h>

/* 获取设备名
 * 所有需要获取 device 的地方，都应该使用本函数 */
static inline const char *dev_name(const struct device *dev)
{
	/* Use the init name until the kobject becomes available */
    /* 在 kobject 可用之前，先使用 init_name */
	if (dev->init_name)
		return dev->init_name;

    /* 已添加到子系统中的设备，应该都是从 kobject 中获取的名字 */
	return kobject_name(&dev->kobj);
}

/* 设置 device 名
 * 真正设置的是 device 内嵌的 kobject 的 name */
extern __printf(2, 3)
int dev_set_name(struct device *dev, const char *name, ...);

#ifdef CONFIG_NUMA
static inline int dev_to_node(struct device *dev)
{
	return dev->numa_node;
}
static inline void set_dev_node(struct device *dev, int node)
{
	dev->numa_node = node;
}
#else
static inline int dev_to_node(struct device *dev)
{
	return -1;
}
static inline void set_dev_node(struct device *dev, int node)
{
}
#endif

static inline struct pm_subsys_data *dev_to_psd(struct device *dev)
{
	return dev ? dev->power.subsys_data : NULL;
}

/* 获取设备是否支持uevent 事件
 * 检查设备模型的 uevent_suppress 标志 */
static inline unsigned int dev_get_uevent_suppress(const struct device *dev)
{
	return dev->kobj.uevent_suppress;
}

/* 设置设备是否支持 uevent 事件
 * 直接设置设备模型的 uevent_suppress 标志 */
static inline void dev_set_uevent_suppress(struct device *dev, int val)
{
	dev->kobj.uevent_suppress = val;
}

/* 获取设备是否已经完成注册
 * 检查设备模型的 state_in_sysfs 标志，注册完成的设备会被添加到 sysfs 中 */
static inline int device_is_registered(struct device *dev)
{
	return dev->kobj.state_in_sysfs;
}

static inline void device_enable_async_suspend(struct device *dev)
{
	if (!dev->power.is_prepared)
		dev->power.async_suspend = true;
}

static inline void device_disable_async_suspend(struct device *dev)
{
	if (!dev->power.is_prepared)
		dev->power.async_suspend = false;
}

static inline bool device_async_suspend_enabled(struct device *dev)
{
	return !!dev->power.async_suspend;
}

static inline void pm_suspend_ignore_children(struct device *dev, bool enable)
{
	dev->power.ignore_children = enable;
}

/* 锁定设备，对设备进行原子操作前需要锁定,阻塞式 */
static inline void device_lock(struct device *dev)
{
	mutex_lock(&dev->mutex);
}

/* 尝试进行锁定设备, 非阻塞式 */
static inline int device_trylock(struct device *dev)
{
	return mutex_trylock(&dev->mutex);
}

/* 解锁设备 */
static inline void device_unlock(struct device *dev)
{
	mutex_unlock(&dev->mutex);
}

/* 驱动模块初始化总入口
 * 在 drivers/base/init.c 中实现
 * 在 init/main.c 中被调用 */
void driver_init(void);

/*
 * High level routines for use by the bus drivers
 */
/* 向系统 注册设备结构 */
extern int __must_check device_register(struct device *dev);
/* 从系统中注销一个设备 */
extern void device_unregister(struct device *dev);
/* device_initialize - 初始化 device 结构体 */
extern void device_initialize(struct device *dev);
/* 将设备添加到设备层次结构中 */
extern int __must_check device_add(struct device *dev);
/* 从系统中移除一个 设备 */
extern void device_del(struct device *dev);
/* 某设备的子设备迭代器, 为每个设备调用 @fn 回调 */
extern int device_for_each_child(struct device *dev, void *data,
		     int (*fn)(struct device *dev, void *data));
/* 用于定位特定子设备的设备迭代器 */
extern struct device *device_find_child(struct device *dev, void *data,
				int (*match)(struct device *dev, void *data));
/* 设备重命名，不要使用本函数，不要进行设备的重命名 */
extern int device_rename(struct device *dev, const char *new_name);
/* 将设备移动到新的父设备下 */
extern int device_move(struct device *dev, struct device *new_parent,
		       enum dpm_order dpm_order);
/* 获取设备的设备节点路径，比如 mtd0  input/event0 */
extern const char *device_get_devnode(struct device *dev,
				      umode_t *mode, const char **tmp);
/* 获取设备的 驱动特殊数据 */
extern void *dev_get_drvdata(const struct device *dev);
/* 设置设备的 驱动特殊数据 */
extern int dev_set_drvdata(struct device *dev, void *data);

/*
 * Root device objects for grouping under /sys/devices
 */
/* 用于在 /sys/devices/ 下分组的 根设备(root_device) 对象 */
extern struct device *__root_device_register(const char *name,
					     struct module *owner);

/*
 * This is a macro to avoid include problems with THIS_MODULE,
 * just as per what is done for device_schedule_callback() above.
 */
/* 注册 root_device 根设备，直接提供了 THIS_MODULE 参数 */
#define root_device_register(name) \
	__root_device_register(name, THIS_MODULE)

/* 注销并释放一个 root_device */
extern void root_device_unregister(struct device *root);

/* 获取设备的平台数据
 * 用于不同平台/设备 提供差异化数据，设备驱动核心 自身不使用
 * 由具体的设备驱动获取并使用 */
static inline void *dev_get_platdata(const struct device *dev)
{
	return dev->platform_data;
}

/*
 * Manual binding of a device to driver. See drivers/base/bus.c
 * for information on use.
 */
/* 将驱动程序绑定到一个设备 */
extern int __must_check device_bind_driver(struct device *dev);
/* 分离设备与驱动程序 */
extern void device_release_driver(struct device *dev);
/* 尝试将设备绑定到驱动程序上 */
extern int  __must_check device_attach(struct device *dev);
/* 尝试将驱动程序绑定到设备 */
extern int __must_check driver_attach(struct device_driver *drv);
/* 移除设备的驱动程序，并探测新驱动程序 */
extern int __must_check device_reprobe(struct device *dev);

/*
 * Easy functions for dynamically creating devices on the fly
 */
/* 创建一个设备，并生成设备节点 */
extern struct device *device_create_vargs(struct class *cls,
					  struct device *parent,
					  dev_t devt,
					  void *drvdata,
					  const char *fmt,
					  va_list vargs);
/* 创建一个设备，并生成设备节点
 * 必须要提供一个 struct class
 * 具体分析见函数原型 */
extern __printf(5, 6)
struct device *device_create(struct class *cls, struct device *parent,
			     dev_t devt, void *drvdata,
			     const char *fmt, ...);
/* 移除一个 使用 device_create() 创建的设备 */
extern void device_destroy(struct class *cls, dev_t devt);

/*
 * Platform "fixup" functions - allow the platform to have their say
 * about devices and actions that the general device layer doesn't
 * know about.
 */
/* Notify platform of device discovery */
/* platform_notify 回调，仅在 acpi 驱动中使用了 */
extern int (*platform_notify)(struct device *dev);

/* platform_notify 回调，仅在 acpi 驱动中使用了 */
extern int (*platform_notify_remove)(struct device *dev);


/*
 * get_device - atomically increment the reference count for the device.
 *
 */
/* 递增 device 的引用计数 */
extern struct device *get_device(struct device *dev);
/* 递减 device 的引用计数 */
extern void put_device(struct device *dev);

#ifdef CONFIG_DEVTMPFS
/* devtmpfs 创建节点 */
extern int devtmpfs_create_node(struct device *dev);
/* 移除 devtmpfs 设备节点  */
extern int devtmpfs_delete_node(struct device *dev);
/* 挂载 devtmpfs 到 mntdir 目录 */
extern int devtmpfs_mount(const char *mntdir);
#else
static inline int devtmpfs_create_node(struct device *dev) { return 0; }
static inline int devtmpfs_delete_node(struct device *dev) { return 0; }
static inline int devtmpfs_mount(const char *mountpoint) { return 0; }
#endif

/* drivers/base/power/shutdown.c */
/* 为每一个设备调用 shutdown() */
extern void device_shutdown(void);

/* debugging and troubleshooting/diagnostic helpers. */
/* 用于返回设备的驱动程序名
 * 如果没有匹配驱动程序，则可能返回 bus / class 的名称 */
extern const char *dev_driver_string(const struct device *dev);


#ifdef CONFIG_PRINTK

extern int __dev_printk(const char *level, const struct device *dev,
			struct va_format *vaf);
extern __printf(3, 4)
int dev_printk(const char *level, const struct device *dev,
	       const char *fmt, ...)
	;
extern __printf(2, 3)
int dev_emerg(const struct device *dev, const char *fmt, ...);
extern __printf(2, 3)
int dev_alert(const struct device *dev, const char *fmt, ...);
extern __printf(2, 3)
int dev_crit(const struct device *dev, const char *fmt, ...);
extern __printf(2, 3)
int dev_err(const struct device *dev, const char *fmt, ...);
extern __printf(2, 3)
int dev_warn(const struct device *dev, const char *fmt, ...);
extern __printf(2, 3)
int dev_notice(const struct device *dev, const char *fmt, ...);
extern __printf(2, 3)
int _dev_info(const struct device *dev, const char *fmt, ...);

#else

static inline int __dev_printk(const char *level, const struct device *dev,
			       struct va_format *vaf)
{ return 0; }
static inline __printf(3, 4)
int dev_printk(const char *level, const struct device *dev,
	       const char *fmt, ...)
{ return 0; }

static inline __printf(2, 3)
int dev_emerg(const struct device *dev, const char *fmt, ...)
{ return 0; }
static inline __printf(2, 3)
int dev_crit(const struct device *dev, const char *fmt, ...)
{ return 0; }
static inline __printf(2, 3)
int dev_alert(const struct device *dev, const char *fmt, ...)
{ return 0; }
static inline __printf(2, 3)
int dev_err(const struct device *dev, const char *fmt, ...)
{ return 0; }
static inline __printf(2, 3)
int dev_warn(const struct device *dev, const char *fmt, ...)
{ return 0; }
static inline __printf(2, 3)
int dev_notice(const struct device *dev, const char *fmt, ...)
{ return 0; }
static inline __printf(2, 3)
int _dev_info(const struct device *dev, const char *fmt, ...)
{ return 0; }

#endif

#define dev_level_ratelimited(dev_level, dev, fmt, ...)			\
do {									\
	static DEFINE_RATELIMIT_STATE(_rs,				\
				      DEFAULT_RATELIMIT_INTERVAL,	\
				      DEFAULT_RATELIMIT_BURST);		\
	if (__ratelimit(&_rs))						\
		dev_level(dev, fmt, ##__VA_ARGS__);			\
} while (0)

#define dev_emerg_ratelimited(dev, fmt, ...)				\
	dev_level_ratelimited(dev_emerg, dev, fmt, ##__VA_ARGS__)
#define dev_alert_ratelimited(dev, fmt, ...)				\
	dev_level_ratelimited(dev_alert, dev, fmt, ##__VA_ARGS__)
#define dev_crit_ratelimited(dev, fmt, ...)				\
	dev_level_ratelimited(dev_crit, dev, fmt, ##__VA_ARGS__)
#define dev_err_ratelimited(dev, fmt, ...)				\
	dev_level_ratelimited(dev_err, dev, fmt, ##__VA_ARGS__)
#define dev_warn_ratelimited(dev, fmt, ...)				\
	dev_level_ratelimited(dev_warn, dev, fmt, ##__VA_ARGS__)
#define dev_notice_ratelimited(dev, fmt, ...)				\
	dev_level_ratelimited(dev_notice, dev, fmt, ##__VA_ARGS__)
#define dev_info_ratelimited(dev, fmt, ...)				\
	dev_level_ratelimited(dev_info, dev, fmt, ##__VA_ARGS__)
#define dev_dbg_ratelimited(dev, fmt, ...)				\
	dev_level_ratelimited(dev_dbg, dev, fmt, ##__VA_ARGS__)

/*
 * Stupid hackaround for existing uses of non-printk uses dev_info
 *
 * Note that the definition of dev_info below is actually _dev_info
 * and a macro is used to avoid redefining dev_info
 */

#define dev_info(dev, fmt, arg...) _dev_info(dev, fmt, ##arg)

#if defined(CONFIG_DYNAMIC_DEBUG)
#define dev_dbg(dev, format, ...)		     \
do {						     \
	dynamic_dev_dbg(dev, format, ##__VA_ARGS__); \
} while (0)
#elif defined(DEBUG)
#define dev_dbg(dev, format, arg...)		\
	dev_printk(KERN_DEBUG, dev, format, ##arg)
#else
#define dev_dbg(dev, format, arg...)				\
({								\
	if (0)							\
		dev_printk(KERN_DEBUG, dev, format, ##arg);	\
	0;							\
})
#endif

#ifdef VERBOSE_DEBUG
#define dev_vdbg	dev_dbg
#else
#define dev_vdbg(dev, format, arg...)				\
({								\
	if (0)							\
		dev_printk(KERN_DEBUG, dev, format, ##arg);	\
	0;							\
})
#endif

/*
 * dev_WARN*() acts like dev_printk(), but with the key difference
 * of using a WARN/WARN_ON to get the message out, including the
 * file/line information and a backtrace.
 */
#define dev_WARN(dev, format, arg...) \
	WARN(1, "Device: %s\n" format, dev_driver_string(dev), ## arg);

#define dev_WARN_ONCE(dev, condition, format, arg...) \
	WARN_ONCE(condition, "Device %s\n" format, \
			dev_driver_string(dev), ## arg)

/* Create alias, so I can be autoloaded. */
#define MODULE_ALIAS_CHARDEV(major,minor) \
	MODULE_ALIAS("char-major-" __stringify(major) "-" __stringify(minor))
#define MODULE_ALIAS_CHARDEV_MAJOR(major) \
	MODULE_ALIAS("char-major-" __stringify(major) "-*")

/* /sys/block/ 目录的遗留兼容问题
 * 参考 sysfs_deprecated 定义处的注释 */
#ifdef CONFIG_SYSFS_DEPRECATED
extern long sysfs_deprecated;
#else
#define sysfs_deprecated 0
#endif

/**
 * module_driver() - Helper macro for drivers that don't do anything
 * special in module init/exit. This eliminates a lot of boilerplate.
 * Each module may only use this macro once, and calling it replaces
 * module_init() and module_exit().
 *
 * @__driver: driver name
 * @__register: register function for this driver type
 * @__unregister: unregister function for this driver type
 * @...: Additional arguments to be passed to __register and __unregister.
 *
 * Use this macro to construct bus specific macros for registering
 * drivers, and do not use it on its own.
 */
#define module_driver(__driver, __register, __unregister, ...) \
static int __init __driver##_init(void) \
{ \
	return __register(&(__driver) , ##__VA_ARGS__); \
} \
module_init(__driver##_init); \
static void __exit __driver##_exit(void) \
{ \
	__unregister(&(__driver) , ##__VA_ARGS__); \
} \
module_exit(__driver##_exit);

#endif /* _DEVICE_H_ */
