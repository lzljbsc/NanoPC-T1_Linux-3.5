#include <linux/notifier.h>

/**
 * struct subsys_private - structure to hold the private to the driver core portions of the bus_type/class structure.
 *
 * @subsys - the struct kset that defines this subsystem
 * @devices_kset - the subsystem's 'devices' directory
 * @interfaces - list of subsystem interfaces associated
 * @mutex - protect the devices, and interfaces lists.
 *
 * @drivers_kset - the list of drivers associated
 * @klist_devices - the klist to iterate over the @devices_kset
 * @klist_drivers - the klist to iterate over the @drivers_kset
 * @bus_notifier - the bus notifier list for anything that cares about things
 *                 on this bus.
 * @bus - pointer back to the struct bus_type that this structure is associated
 *        with.
 *
 * @glue_dirs - "glue" directory to put in-between the parent device to
 *              avoid namespace conflicts
 * @class - pointer back to the struct class that this structure is associated
 *          with.
 *
 * This structure is the one that is the actual kobject allowing struct
 * bus_type/class to be statically allocated safely.  Nothing outside of the
 * driver core should ever touch these fields.
 */
/* bus_type / class 结构的驱动核心部分 */
/* 本结构体可以理解为 bus_type 和 class 的上层，包含了bus和class
 * 是为了隐藏 bus core 内部的状态 */
struct subsys_private {
    /* 用于描述本 subsystem 的 kset ，用于代表其自身 */
    /* 这是 bus_type 或 class 在设备模型中的 kset
     * 会将这个 kset 初始化后，加入到设备模型中 */
	struct kset subsys;
    /* 表示 subsystem 的 device 目录 */
    /* 是 /sys/bus/[bus.name]/devices 目录
     * 目录中会放所有属于 本bus 的 device 的符号链接 */
	struct kset *devices_kset;
    /* interfaces 是一个 list_head 类型数据，用于保存与之相关的 interface
     * 在内核中 interface 用于抽象 bus 下所有关联设备的一些特殊的功能 */
    /* 参考 subsys_interface_register 相关函数 */
	struct list_head interfaces;
    /* 用于保护设备和 interface 链表 */
	struct mutex mutex;

    /* 表示 subsystem 中驱动相关链表 */
    /* 是 /sys/bus/[bus.name]/drivers 目录
     * 目录中会放所有属于 本bus 的 drivers 目录 */
	struct kset *drivers_kset;
    /* 设备链表，用于保存本bus下所有的device的指针，方便查找 */
	struct klist klist_devices;
    /* 驱动链表，用于保存本bus下所有的device_driver的指针，方便查找 */
	struct klist klist_drivers;
    /* bus_notifier 是一个总线通知列表，用于监测bus发生的任何事情 */
	struct blocking_notifier_head bus_notifier;
    /* 用于控制该bus下的 drivers 或者 device 是否具有自动 probe 属性 */
	unsigned int drivers_autoprobe:1;
    /* 是一个指向与之关联的 struct bus_type 类型的指针，用于保存上层的bus */
    /* 为什么要保存上层的 bus 呢？ 不能使用 container_of 反找吗？
     * 不可以，因为在 bus class 模块的实现中， subsys_private 结构是动态分配的
     * 通过 bus_type class 结构中的 p 指针指向的，所以无法通过 container_of 反向
     * 查找包含它的 bus_type class 结构 */
	struct bus_type *bus;

    /* 表示 glue目录，用于放在父设备之间，以避免名称空间出现冲突 */
    /* glue_dirs 的作用，是为了把属于某个设备的子设备，按照所属的 class 进行分类
     * 当然，子设备需要属于某个class 才可以
     * 可以参考 get_device_parent() 函数中的分析理解 */
	struct kset glue_dirs;
    /* 是一个指向与之关联的 struct class 类型的指针，用于保存上层的class */
    /* 为什么要有本指针，参考上面的 bus 指针分析 */
	struct class *class;
};
/* 通过 subsys_private.subsys.kobj 反向定位 subsys_private 结构 */
#define to_subsys_private(obj) container_of(obj, struct subsys_private, subsys.kobj)

/* 设备驱动 driver_private 私有数据结构
 * 此部分数据仅有设备驱动核心访问 */
struct driver_private {
    /* 设备驱动模型 kobject 用于添加到 sysfs 中进行管理
     * 也是通过 kobject 添加到 bus 的 drivers_kset 中的
     * 这样就组成了设备驱动模型 层次结构 */
	struct kobject kobj;
    /* 挂接所有匹配到本驱动的设备链表 */
	struct klist klist_devices;
    /* 挂接到所属 bus 的 klist_node 节点 */
	struct klist_node knode_bus;
	struct module_kobject *mkobj;
    /* 指向包含本私有数据结构的 device_driver 结构
     * 用于在通过 kobject 反查 device_driver 时使用 */
	struct device_driver *driver;
};
/* 通过 driver_private 结构中的 kobj 反查 driver_private 结构 */
#define to_driver(obj) container_of(obj, struct driver_private, kobj)

/**
 * struct device_private - structure to hold the private to the driver core portions of the device structure.
 *
 * @klist_children - klist containing all children of this device
 * @knode_parent - node in sibling list
 * @knode_driver - node in driver list
 * @knode_bus - node in bus list
 * @deferred_probe - entry in deferred_probe_list which is used to retry the
 *	binding of drivers which were unable to get all the resources needed by
 *	the device; typically because it depends on another driver getting
 *	probed first.
 * @driver_data - private pointer for driver specific info.  Will turn into a
 * list soon.
 * @device - pointer back to the struct class that this structure is
 * associated with.
 *
 * Nothing outside of the driver core should ever touch these fields.
 */
/* 保存设备结构中私有的驱动程序核心数据
 * 只能由驱动核心部分访问，其它任何程序不可以访问本数据 */
struct device_private {
    /* 子设备的 klist 链表 */
	struct klist klist_children;
    /* 接入父设备的 klist_children 时所需要的 klist 节点 */
	struct klist_node knode_parent;
    /* 接入驱动的设备链表时所需要的 klist 节点 */
	struct klist_node knode_driver;
    /* 接入总线的设备链表时所需要的 klist 节点 */
	struct klist_node knode_bus;
    /* 加入延迟探测列表所需要的链表节点 */
    /* 在“延迟探测列表”中的一个条目，用于重试那些未能获得设备所需全部资源的驱动
     * 程序的绑定操作；通常是因为这依赖于另一个驱动程序先完成探测操作。 */
	struct list_head deferred_probe;
    /* 给驱动提供的特殊信息 数据指针
     * 由设备创建者提供，设备驱动核心处理中并不使用，仅提供设置/获取接口 */
	void *driver_data;
    /* 回指 struct device 结构体指针 */
	struct device *device;
};
/* 通过设备结构私有数据的 parent driver bus 找包含它的 device_private 结构
 * 这主要用在通过 klist_node 节点找 设备结构 device 时
 * 通过下面的几个宏，可以找到 device_private 结构
 * 而该结构的 device 指针，指向了包含它的 设备结构 device */
#define to_device_private_parent(obj)	\
	container_of(obj, struct device_private, knode_parent)
#define to_device_private_driver(obj)	\
	container_of(obj, struct device_private, knode_driver)
#define to_device_private_bus(obj)	\
	container_of(obj, struct device_private, knode_bus)

/* 设备结构私有数据初始化 */
extern int device_private_init(struct device *dev);

/* initialisation functions */
/* 以下为初始化流程函数
 * 直接在 driver_init 中调用的 */
extern int devices_init(void);
extern int buses_init(void);
extern int classes_init(void);
extern int firmware_init(void);
#ifdef CONFIG_SYS_HYPERVISOR
extern int hypervisor_init(void);
#else
static inline int hypervisor_init(void) { return 0; }
#endif
extern int platform_bus_init(void);
extern void cpu_dev_init(void);

/* bus 提供的 device 接口
 * 添加、探测、移除 device */
extern int bus_add_device(struct device *dev);
extern void bus_probe_device(struct device *dev);
extern void bus_remove_device(struct device *dev);

/* bus 提供的 driver 接口
 * 添加、移除 driver */
extern int bus_add_driver(struct device_driver *drv);
extern void bus_remove_driver(struct device_driver *drv);

/* 将驱动程序与其控制的所有设备分离 */
extern void driver_detach(struct device_driver *drv);
/* 尝试将驱动程序绑定到设备 */
extern int driver_probe_device(struct device_driver *drv, struct device *dev);
/* 将设备从延迟探测列表中移除 */
extern void driver_deferred_probe_del(struct device *dev);
/* 驱动 匹配 设备
 * 调用驱动所属的bus进行匹配, 匹配规则由 bus 自行实现 */
static inline int driver_match_device(struct device_driver *drv,
				      struct device *dev)
{
    /* 使用驱动所属的bus的match进行匹配 */
	return drv->bus->match ? drv->bus->match(dev, drv) : 1;
}

/* 没有函数原型，也没有任何调用的地方，忽略掉。。。 */
extern char *make_class_name(const char *name, struct kobject *kobj);

/* 释放所有 dev 下被管理的资源 */
extern int devres_release_all(struct device *dev);

/* /sys/devices directory */
/* /sys/devices/ 目录 */
extern struct kset *devices_kset;

#if defined(CONFIG_MODULES) && defined(CONFIG_SYSFS)
extern void module_add_driver(struct module *mod, struct device_driver *drv);
extern void module_remove_driver(struct device_driver *drv);
#else
static inline void module_add_driver(struct module *mod,
				     struct device_driver *drv) { }
static inline void module_remove_driver(struct device_driver *drv) { }
#endif

/* devtmpfs 初始化函数
 * 直接在 driver_init 中调用 */
#ifdef CONFIG_DEVTMPFS
extern int devtmpfs_init(void);
#else
static inline int devtmpfs_init(void) { return 0; }
#endif
