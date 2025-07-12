/*
 * class.c - basic device class management
 *
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2003-2004 Greg Kroah-Hartman
 * Copyright (c) 2003-2004 IBM Corp.
 *
 * This file is released under the GPLv2
 *
 */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/string.h>
#include <linux/kdev_t.h>
#include <linux/err.h>
#include <linux/slab.h>
#include <linux/genhd.h>
#include <linux/mutex.h>
#include "base.h"

/* 基本设备类管理 */

/* 通过通用属性结构反查 class_attribute 属性结构 */
#define to_class_attr(_attr) container_of(_attr, struct class_attribute, attr)

/* class 属性  show 方法 */
static ssize_t class_attr_show(struct kobject *kobj, struct attribute *attr,
			       char *buf)
{
    /* 找到属性对应的 class_attribute 结构 */
	struct class_attribute *class_attr = to_class_attr(attr);
    /* 通过 kobj 反查 class 私有核心数据 subsys_private */
	struct subsys_private *cp = to_subsys_private(kobj);
	ssize_t ret = -EIO;

    /* 调用 class 属性的 show 方法
     * 私有核心数据 cp 的 class 指向包含本私有数据结构的 class */
	if (class_attr->show)
		ret = class_attr->show(cp->class, class_attr, buf);
	return ret;
}

/* class 属性  store 方法 */
static ssize_t class_attr_store(struct kobject *kobj, struct attribute *attr,
				const char *buf, size_t count)
{
    /* 找到属性对应的 class_attribute 结构 */
	struct class_attribute *class_attr = to_class_attr(attr);
    /* 通过 kobj 反查 class 私有核心数据 subsys_private */
	struct subsys_private *cp = to_subsys_private(kobj);
	ssize_t ret = -EIO;

    /* 调用 class 属性的 store 方法
     * 私有核心数据 cp 的 class 指向包含本私有数据结构的 class */
	if (class_attr->store)
		ret = class_attr->store(cp->class, class_attr, buf, count);
	return ret;
}

/* class 属性  namespace 方法 */
static const void *class_attr_namespace(struct kobject *kobj,
					const struct attribute *attr)
{
    /* 找到属性对应的 class_attribute 结构 */
	struct class_attribute *class_attr = to_class_attr(attr);
    /* 通过 kobj 反查 class 私有核心数据 subsys_private */
	struct subsys_private *cp = to_subsys_private(kobj);
	const void *ns = NULL;

    /* 调用 class 属性的 namespace 方法
     * 私有核心数据 cp 的 class 指向包含本私有数据结构的 class */
	if (class_attr->namespace)
		ns = class_attr->namespace(cp->class, class_attr);
	return ns;
}

/* class ktype release 回调函数 */
static void class_release(struct kobject *kobj)
{
    /* 通过 kobj 反查 class 私有核心数据 subsys_private */
	struct subsys_private *cp = to_subsys_private(kobj);
	struct class *class = cp->class;

	pr_debug("class '%s': release.\n", class->name);

    /* 调用 class 结构的 class_release 回调函数
     * 该函数应该都要提供的，否则会报警告 */
	if (class->class_release)
		class->class_release(class);
	else
		pr_debug("class '%s' does not have a release() function, "
			 "be careful\n", class->name);

    /* 释放class 的私有核心数据结构，在 注册时分配的 */
	kfree(cp);
}

/* class ns_type 回调，返回 class 的 ns_type 结构指针 */
static const struct kobj_ns_type_operations *class_child_ns_type(struct kobject *kobj)
{
    /* 通过 kobj 反查 class 私有核心数据 subsys_private */
	struct subsys_private *cp = to_subsys_private(kobj);
	struct class *class = cp->class;

    /* 返回 ns_type 结构指针 */
	return class->ns_type;
}

/* class 属性操作函数 */
static const struct sysfs_ops class_sysfs_ops = {
	.show	   = class_attr_show,
	.store	   = class_attr_store,
	.namespace = class_attr_namespace,
};

/* class 类型的 ktype
 * 所有的 class 的 设备模型kobj 都是 class_ktype
 * 可以用于区分 kobj 类型 */
static struct kobj_type class_ktype = {
	.sysfs_ops	= &class_sysfs_ops,
	.release	= class_release,
	.child_ns_type	= class_child_ns_type,
};

/* Hotplug events for classes go to the class subsys */
/* class 顶层 kset，对应 /sys/class/ 目录 */
static struct kset *class_kset;


/* 在 class 目录下，创建 属性文件 */
int class_create_file(struct class *cls, const struct class_attribute *attr)
{
	int error;
    /* 将属性文件创建到 class 对应的 kobject 目录下 */
	if (cls)
		error = sysfs_create_file(&cls->p->subsys.kobj,
					  &attr->attr);
	else
		error = -EINVAL;
	return error;
}

/* 移除 class 目录下的属性文件 */
void class_remove_file(struct class *cls, const struct class_attribute *attr)
{
	if (cls)
		sysfs_remove_file(&cls->p->subsys.kobj, &attr->attr);
}

/* 增加 class 的引用
 * 本质是增加 class 内的 kobject 的引用 */
static struct class *class_get(struct class *cls)
{
	if (cls)
		kset_get(&cls->p->subsys);
	return cls;
}

/* 减少 class 的引用
 * 本质是减少 class 内的 kobject 的引用 */
static void class_put(struct class *cls)
{
	if (cls)
		kset_put(&cls->p->subsys);
}

/* 添加 class 的默认属性
 * 本文件中专用函数，用于在 注册一个class 时添加默认属性 */
static int add_class_attrs(struct class *cls)
{
	int i;
	int error = 0;

	if (cls->class_attrs) {
        /* 存在 class_attrs 属性结构
         * 则按照属性逐个添加 */
		for (i = 0; attr_name(cls->class_attrs[i]); i++) {
			error = class_create_file(cls, &cls->class_attrs[i]);
			if (error)
				goto error;
		}
	}
done:
	return error;
error:
	while (--i >= 0)
		class_remove_file(cls, &cls->class_attrs[i]);
	goto done;
}

/* 移除 class 的默认属性
 * 本文件中专用函数，用于在 注销一个class 时移除默认属性 */
static void remove_class_attrs(struct class *cls)
{
	int i;

	if (cls->class_attrs) {
        /* 存在 class_attrs 属性结构
         * 则按照属性逐个移除 */
		for (i = 0; attr_name(cls->class_attrs[i]); i++)
			class_remove_file(cls, &cls->class_attrs[i]);
	}
}

/* klist 链表管理使用，get 用于对设备进行引用
 * 当一个设备结构被加入到 klist 中管理，那就是多了一个引用 */
static void klist_class_dev_get(struct klist_node *n)
{
    /* 在 struct device 结构中，knode_class 是链接到
     * 所属的 class 的 knode 节点 */
	struct device *dev = container_of(n, struct device, knode_class);

    /* 增加对 device 的引用 */
	get_device(dev);
}

/* klist 链表管理使用，put 用于对设备进行解引用
 * 当一个设备结构不再被 klist 管理，那就是少了一个引用 */
static void klist_class_dev_put(struct klist_node *n)
{
    /* 在 struct device 结构中，knode_class 是链接到
     * 所属的 class 的 knode 节点 */
	struct device *dev = container_of(n, struct device, knode_class);

    /* 减少对 device 的引用 */
	put_device(dev);
}

/* 注册一个设备类 class */
int __class_register(struct class *cls, struct lock_class_key *key)
{
	struct subsys_private *cp;
	int error;

	pr_debug("device class '%s': registering\n", cls->name);

    /* 先分配 class 的私有核心数据结构
     * 这是一个关键数据结构，用于设备驱动模型，构建层次结构 */
	cp = kzalloc(sizeof(*cp), GFP_KERNEL);
	if (!cp)
		return -ENOMEM;
    /* 初始化属于该类的 设备链表，属于该类的设备使用的 klist 链表管理 */
	klist_init(&cp->klist_devices, klist_class_dev_get, klist_class_dev_put);
    /* class interfaces 链表头初始化 */
	INIT_LIST_HEAD(&cp->interfaces);
    /* 初始化 “胶合” 目录的 kset
     * 这里的 glue_dirs 只是做为 kobject 的链表头使用，并不会注册到 sysfs 中
     * 在 /sys 目录中，并没有这个 kset 对应的目录
     * 它是做为所有从属它的子目录的 链表头 ，管理链表使用的 */
	kset_init(&cp->glue_dirs);
    /* 保护 interfaces 链表的，仅内部使用 */
	__mutex_init(&cp->mutex, "subsys mutex", key);
    /* 设置class 在设备模型层次结构中的 kobj, 以类名称命名 */
	error = kobject_set_name(&cp->subsys.kobj, "%s", cls->name);
	if (error) {
		kfree(cp);
		return error;
	}

	/* set the default /sys/dev directory for devices of this class */
    /* 设置所属本 class 的设备，在 /sys/dev/ 中的默认目录
     * 其实就是设置属于本class 的设备，默认是 char 还是 block 设备
     * 没有指定的，则一律默认为 char 设备 */
	if (!cls->dev_kobj)
		cls->dev_kobj = sysfs_dev_char_kobj;

#if defined(CONFIG_BLOCK)
	/* let the block class directory show up in the root of sysfs */
    /* 针对 block 目录的特殊处理，原因参考 sysfs_deprecated 的注释
     * 如果没有设置 sysfs_deprecated ，那就不需要 /sys/block/ 目录
     * 这里将设置所有新注册的 class 都放在 /sys/class/ 目录下
     * 如果设置了 sysfs_deprecated , 那对 genhd.c 中的 block_class
     * 做个特殊处理，不设置 block_class 的 kset，那它就会在 /sys/block/ 了 */
	if (!sysfs_deprecated || cls != &block_class)
		cp->subsys.kobj.kset = class_kset;
#else
    /* 如果没有配置 block 设备，那所有的都放在 /sys/class/ 中 */
	cp->subsys.kobj.kset = class_kset;
#endif
    /* 所有的 class ，设备模型 ktype 都是 class_ktype */
	cp->subsys.kobj.ktype = &class_ktype;
    /* 私有核心数据 中 class 指针指向 包含它的 class 结构 */
	cp->class = cls;
    /* class 结构 p 指针，指向自己的 私有核心数据 */
	cls->p = cp;

    /* 注册 class 的设备模型 kset 到 设备模型层次结构中
     * 这样，注册的 class 就在 /sys/class/ 中出现对应的目录了 */
	error = kset_register(&cp->subsys);
	if (error) {
		kfree(cp);
		return error;
	}
    /* 添加 class 的属性文件
     * 这里先增加了一次class 引用，添加后又释放了引用 */
	error = add_class_attrs(class_get(cls));
	class_put(cls);
	return error;
}
EXPORT_SYMBOL_GPL(__class_register);

/* 注销一个设备类 class */
void class_unregister(struct class *cls)
{
	pr_debug("device class '%s': unregistering\n", cls->name);
    /* 移除 class 的默认属性文件 */
	remove_class_attrs(cls);
    /* 从设备模型中，注销 class 的 kset */
	kset_unregister(&cls->p->subsys);
}

/* 动态创建的class 的 class_release 回调函数
 * 用于在注销 class 时释放 class 结构的内存 */
static void class_create_release(struct class *cls)
{
	pr_debug("%s called for %s\n", __func__, cls->name);
	kfree(cls);
}

/**
 * class_create - create a struct class structure
 * @owner: pointer to the module that is to "own" this struct class
 * @name: pointer to a string for the name of this class.
 * @key: the lock_class_key for this class; used by mutex lock debugging
 *
 * This is used to create a struct class pointer that can then be used
 * in calls to device_create().
 *
 * Returns &struct class pointer on success, or ERR_PTR() on error.
 *
 * Note, the pointer created here is to be destroyed when finished by
 * making a call to class_destroy().
 */
/* 创建一个 class
 * 这用于创建一个 struct class 结构，在调用 device_create 时使用该指针
 *
 * 成功返回 struct class 指针，使用 ERR_PTR() 测试失败
 *
 * 注意，这里创建的指针将在调用 class_destroy() 结束时销毁 */
struct class *__class_create(struct module *owner, const char *name,
			     struct lock_class_key *key)
{
	struct class *cls;
	int retval;

    /* 分配一个 struct class 结构 */
	cls = kzalloc(sizeof(*cls), GFP_KERNEL);
	if (!cls) {
		retval = -ENOMEM;
		goto error;
	}

    /* 设置 class 名称
     * 指定 class_release 回调，用于注销时释放 class 结构 */
	cls->name = name;
	cls->owner = owner;
	cls->class_release = class_create_release;

    /* 注册新创建的 class  */
	retval = __class_register(cls, key);
	if (retval)
		goto error;

	return cls;

error:
	kfree(cls);
	return ERR_PTR(retval);
}
EXPORT_SYMBOL_GPL(__class_create);

/**
 * class_destroy - destroys a struct class structure
 * @cls: pointer to the struct class that is to be destroyed
 *
 * Note, the pointer to be destroyed must have been created with a call
 * to class_create().
 */
/* 销毁 struct class 结构
 * 注意，要销毁的指针必须是通过调用 class_create() 创建的 */
void class_destroy(struct class *cls)
{
	if ((cls == NULL) || (IS_ERR(cls)))
		return;

    /* 注销 class  */
	class_unregister(cls);
}

/**
 * class_dev_iter_init - initialize class device iterator
 * @iter: class iterator to initialize
 * @class: the class we wanna iterate over
 * @start: the device to start iterating from, if any
 * @type: device_type of the devices to iterate over, NULL for all
 *
 * Initialize class iterator @iter such that it iterates over devices
 * of @class.  If @start is set, the list iteration will start there,
 * otherwise if it is NULL, the iteration starts at the beginning of
 * the list.
 */
/* 初始化 class 的设备迭代器
 * 初始化一个 class 迭代器，用于迭代属于class 的 设备。
 * 如果设置了 @start, 则从该设备开始迭代，否则从链表头开始 */
void class_dev_iter_init(struct class_dev_iter *iter, struct class *class,
			 struct device *start, const struct device_type *type)
{
	struct klist_node *start_knode = NULL;

    /* 初始化迭代器
     * 所有属于该 class 的 设备，都链接在 klist_devices 链表中 */
	if (start)
		start_knode = &start->knode_class;
	klist_iter_init_node(&class->p->klist_devices, &iter->ki, start_knode);
    /* 记录需要筛选的 device_type */
	iter->type = type;
}
EXPORT_SYMBOL_GPL(class_dev_iter_init);

/**
 * class_dev_iter_next - iterate to the next device
 * @iter: class iterator to proceed
 *
 * Proceed @iter to the next device and return it.  Returns NULL if
 * iteration is complete.
 *
 * The returned device is referenced and won't be released till
 * iterator is proceed to the next device or exited.  The caller is
 * free to do whatever it wants to do with the device including
 * calling back into class code.
 */
/* 迭代下一个设备结构
 * 该函数是 class 设备链表遍历专用的
 * 需要先使用 class_dev_iter_init 进行迭代器初始化 */
struct device *class_dev_iter_next(struct class_dev_iter *iter)
{
	struct klist_node *knode;
	struct device *dev;

    /* 遍历每一个设备，直到主动返回 */
	while (1) {
        /* 迭代链表下一个节点，节点是设备链表 klist_devices 上的 */
		knode = klist_next(&iter->ki);
		if (!knode)
			return NULL;
        /* 设备结构通过自身的 knode_class 链接到 class 的 klist_devices 链表
         * 通过 klist_next 找到了 knode_class 节点
         * 通过反查 device 结构，定位到遍历的设备结构 */
		dev = container_of(knode, struct device, knode_class);
        /* 如果 iter->type == NULL，则所有设备都是符合要求的
         * 否则，需要检查 dev->type，一致则符合要求 */
		if (!iter->type || iter->type == dev->type)
			return dev;
	}
}
EXPORT_SYMBOL_GPL(class_dev_iter_next);

/**
 * class_dev_iter_exit - finish iteration
 * @iter: class iterator to finish
 *
 * Finish an iteration.  Always call this function after iteration is
 * complete whether the iteration ran till the end or not.
 */
/* 退出迭代，结束迭代
 * 与 klist_iter_exit 类似，中间退出时必须调用 */
void class_dev_iter_exit(struct class_dev_iter *iter)
{
    /* 实际就是 klist_iter_exit 函数 */
	klist_iter_exit(&iter->ki);
}
EXPORT_SYMBOL_GPL(class_dev_iter_exit);

/**
 * class_for_each_device - device iterator
 * @class: the class we're iterating
 * @start: the device to start with in the list, if any.
 * @data: data for the callback
 * @fn: function to be called for each device
 *
 * Iterate over @class's list of devices, and call @fn for each,
 * passing it @data.  If @start is set, the list iteration will start
 * there, otherwise if it is NULL, the iteration starts at the
 * beginning of the list.
 *
 * We check the return of @fn each time. If it returns anything
 * other than 0, we break out and return that value.
 *
 * @fn is allowed to do anything including calling back into class
 * code.  There's no locking restriction.
 */
/* class 设备迭代器
 * 遍历 @class 的设备链表，并为每一个设备调用 @fn，并将其传递 @data。
 * 如果设置了 @start，则列表迭代将从该设备开始，否则从链表头开始
 *
 * 如果在调用 @fn 时，返回了非0值，则将退出迭代并返回该值。 */
int class_for_each_device(struct class *class, struct device *start,
			  void *data, int (*fn)(struct device *, void *))
{
	struct class_dev_iter iter;
	struct device *dev;
	int error = 0;

	if (!class)
		return -EINVAL;
	if (!class->p) {
		WARN(1, "%s called for class '%s' before it was initialized",
		     __func__, class->name);
		return -EINVAL;
	}

    /* 初始化 class 迭代器 */
	class_dev_iter_init(&iter, class, start, NULL);
    /* 迭代设备，dev == NULL 时表示迭代结束 */
	while ((dev = class_dev_iter_next(&iter))) {
        /* 调用 fn 回调，并给 data 参数
         * 当 fn 返回 非0 值时，结束迭代并返回该值 */
		error = fn(dev, data);
		if (error)
			break;
	}
    /* 退出 class 迭代器 */
	class_dev_iter_exit(&iter);

	return error;
}
EXPORT_SYMBOL_GPL(class_for_each_device);

/**
 * class_find_device - device iterator for locating a particular device
 * @class: the class we're iterating
 * @start: Device to begin with
 * @data: data for the match function
 * @match: function to check device
 *
 * This is similar to the class_for_each_dev() function above, but it
 * returns a reference to a device that is 'found' for later use, as
 * determined by the @match callback.
 *
 * The callback should return 0 if the device doesn't match and non-zero
 * if it does.  If the callback returns non-zero, this function will
 * return to the caller and not iterate over any more devices.
 *
 * Note, you will need to drop the reference with put_device() after use.
 *
 * @fn is allowed to do anything including calling back into class
 * code.  There's no locking restriction.
 */
/* 用于定位特定设备的 class 设备迭代器
 * 本函数将返回一个对设备的引用，
 * 该设备是由 @match 回调确定的，以供返回后其它程序使用
 * 如果没有任何匹配，则返回 NULL，如果匹配成功，则返回设备结构指针。
 * 如果 match 回调返回非零值，则此函数将返回，并且不再迭代任何设备 */
struct device *class_find_device(struct class *class, struct device *start,
				 void *data,
				 int (*match)(struct device *, void *))
{
	struct class_dev_iter iter;
	struct device *dev;

	if (!class)
		return NULL;
	if (!class->p) {
		WARN(1, "%s called for class '%s' before it was initialized",
		     __func__, class->name);
		return NULL;
	}

    /* 初始化 class 迭代器 */
	class_dev_iter_init(&iter, class, start, NULL);
    /* 遍历每一个设备，若 dev = NULL，则遍历完成
     * 在每次有效的遍历时，调用 match 回调函数，并传 data 参数 */
    /* 当设备匹配成功，则会递增设备引用计数，并退出迭代 */
	while ((dev = class_dev_iter_next(&iter))) {
		if (match(dev, data)) {
			get_device(dev);
			break;
		}
	}
    /* 退出迭代器 */
	class_dev_iter_exit(&iter);

	return dev;
}
EXPORT_SYMBOL_GPL(class_find_device);

/* 向 class 注册 class_interface
 * 当 class 上已经有设备时，新注册的 add_dev 钩子函数也会被调用
 * 以后新注册设备时，也会调用 */
int class_interface_register(struct class_interface *class_intf)
{
	struct class *parent;
	struct class_dev_iter iter;
	struct device *dev;

	if (!class_intf || !class_intf->class)
		return -ENODEV;

    /* 对链接到的 class 引用，class_intf 一定是连接到一个 class 上 */
	parent = class_get(class_intf->class);
	if (!parent)
		return -EINVAL;

    /* 私有核心数据 中的 mutex 是 interfaces 的专用保护 mutex */
	mutex_lock(&parent->p->mutex);
    /* 将新注册的 class_interface 链接到 interfaces 链表中 */
	list_add_tail(&class_intf->node, &parent->p->interfaces);
    /* 如果存在 add_dev 钩子函数，则遍历 class 上的所有设备 */
	if (class_intf->add_dev) {
        /* 初始化迭代器，从设备链表头开始 */
		class_dev_iter_init(&iter, parent, NULL, NULL);
        /* 迭代每一个设备，并调用 add_dev 钩子函数 */
		while ((dev = class_dev_iter_next(&iter)))
			class_intf->add_dev(dev, class_intf);
        /* 迭代完成，则退出迭代 */
		class_dev_iter_exit(&iter);
	}
	mutex_unlock(&parent->p->mutex);

	return 0;
}

/* 从 class 移除 class_interface
 * 当 class 上已经有设备时，新注册的 remove_dev 钩子函数也会被调用
 * 以后移除设备时，也会调用 */
void class_interface_unregister(struct class_interface *class_intf)
{
	struct class *parent = class_intf->class;
	struct class_dev_iter iter;
	struct device *dev;

	if (!parent)
		return;

    /* 私有核心数据 中的 mutex 是 interfaces 的专用保护 mutex */
	mutex_lock(&parent->p->mutex);
    /* 从 interfaces 链表中移除 本class_interface 节点 */
	list_del_init(&class_intf->node);
    /* 如果存在 remove_dev 钩子函数，则遍历 class 上的所有设备 */
	if (class_intf->remove_dev) {
        /* 初始化迭代器，从设备链表头开始 */
		class_dev_iter_init(&iter, parent, NULL, NULL);
        /* 迭代每一个设备，并调用 remove_dev 钩子函数 */
		while ((dev = class_dev_iter_next(&iter)))
			class_intf->remove_dev(dev, class_intf);
        /* 迭代完成，则退出迭代 */
		class_dev_iter_exit(&iter);
	}
	mutex_unlock(&parent->p->mutex);

    /* 解除对 class 的引用 */
	class_put(parent);
}

/* class_attribute_string 属性结构的 show 方法 */
ssize_t show_class_attr_string(struct class *class,
			       struct class_attribute *attr, char *buf)
{
	struct class_attribute_string *cs;
    /* 本质还是 class_attribute 属性结构
     * 通过 class_attribute 结构反查 class_attribute_string 结构
     * 找到需要操作的 字符串 */
	cs = container_of(attr, struct class_attribute_string, attr);
    /* 将属性静态字符串拷贝指 buf 中 */
	return snprintf(buf, PAGE_SIZE, "%s\n", cs->str);
}

EXPORT_SYMBOL_GPL(show_class_attr_string);

/* 用于创建 兼容class
 * 是一种轻量级的 class 目录表示方法
 * 只有一个 kobject ，并且是添加到 /sys/class/ 目录中的
 * 这是为了兼容老的驱动使用的 class 名称
 * 比如 i2c 设备，如果定义了 CONFIG_I2C_COMPAT
 * 则会创建 /sys/class/i2c-adapter/ 目录
 * 而 i2c 驱动中，默认的class 为 /sys/class/i2c-dev/ 目录 */
struct class_compat {
    /* 表示 class ，会被注册到 sysfs 中，会显示在 /sys/class/ 目录下 */
	struct kobject *kobj;
};

/**
 * class_compat_register - register a compatibility class
 * @name: the name of the class
 *
 * Compatibility class are meant as a temporary user-space compatibility
 * workaround when converting a family of class devices to a bus devices.
 */
/* 注册一个兼容class
 * 兼容类是将一类设备转换为总线设备时的一种临时用户空间兼容性解决方法。 */
/* 创建的兼容class以 name 命名，会在 /sys/class/ 目录下显示
 * 如 i2c 子系统中，创建了 i2c_adapter 兼容class */
struct class_compat *class_compat_register(const char *name)
{
	struct class_compat *cls;

    /* 分配 class_compat 内存，所有的兼容类都是自动分配的 */
	cls = kmalloc(sizeof(struct class_compat), GFP_KERNEL);
	if (!cls)
		return NULL;
    /* 创建 kobject , 并注册到 sysfs 文件系统中
     * 这里使用的 parent 为 class_kset->kobj, 所以会在 /sys/class/ 目录中 */
	cls->kobj = kobject_create_and_add(name, &class_kset->kobj);
	if (!cls->kobj) {
		kfree(cls);
		return NULL;
	}
	return cls;
}
EXPORT_SYMBOL_GPL(class_compat_register);

/**
 * class_compat_unregister - unregister a compatibility class
 * @cls: the class to unregister
 */
/* 注销一个兼容class
 * 与 class_compat_register 相反 */
void class_compat_unregister(struct class_compat *cls)
{
    /* 释放 kobj 设备模型， /sys/class/ 目录中对应目录删除 */
	kobject_put(cls->kobj);
    /* 释放动态分配的内存 */
	kfree(cls);
}
EXPORT_SYMBOL_GPL(class_compat_unregister);

/**
 * class_compat_create_link - create a compatibility class device link to
 *			      a bus device
 * @cls: the compatibility class
 * @dev: the target bus device
 * @device_link: an optional device to which a "device" link should be created
 */
/* 创建到总线设备的兼容类设备链接
 * 兼容类中，仅做一些创建软连接的工作，并不会真的注册设备 */
int class_compat_create_link(struct class_compat *cls, struct device *dev,
			     struct device *device_link)
{
	int error;

    /* 在 兼容class 目录中
     * 创建一个链接到实际设备的 设备同名 链接 
     * 比如，在 /sys/class/i2c-adapter/ 目录中：
     * i2c-0 -> ../../devices/platform/s3c2440-i2c.0/i2c-0 */
	error = sysfs_create_link(cls->kobj, &dev->kobj, dev_name(dev));
	if (error)
		return error;

	/*
	 * Optionally add a "device" link (typically to the parent), as a
	 * class device would have one and we want to provide as much
	 * backwards compatibility as possible.
	 */
    /* 可选择添加 "device" 链接（通常是父设备），因为类设备会有一个，
     * 并且我们希望提供尽可能多的向后兼容性
     *
     * 这里就是可以让调用者选择性的多创建一个链接，该链接名为 device
     * 至于链接到谁，是由调用者指定的（一般都是自己的父设备）
     *
     * 如果有需要链接的，那就在 设备目录下，创建一个 device 链接文件，
     * 链接到 device_link 设备
     *
     * 比如在 i2c-0 这个设备下：
     * 目录为： /sys/devices/platform/s3c2440-i2c.0/i2c-0 
     * device -> ../../s3c2440-i2c.0
     *
     * device 就是链接到自己的父设备的 */
	if (device_link) {
		error = sysfs_create_link(&dev->kobj, &device_link->kobj,
					  "device");
		if (error)
			sysfs_remove_link(cls->kobj, dev_name(dev));
	}

	return error;
}
EXPORT_SYMBOL_GPL(class_compat_create_link);

/**
 * class_compat_remove_link - remove a compatibility class device link to
 *			      a bus device
 * @cls: the compatibility class
 * @dev: the target bus device
 * @device_link: an optional device to which a "device" link was previously
 * 		 created
 */
/* 移除兼容class下的 设备到总线设备的链接
 * 就是 class_compat_create_link 操作的逆向操作 */
void class_compat_remove_link(struct class_compat *cls, struct device *dev,
			      struct device *device_link)
{
    /* 先移除设备目录下的 device 链接文件 */
	if (device_link)
		sysfs_remove_link(&dev->kobj, "device");
    /* 移除兼容class 目录下的 设备同名 的链接文件 */
	sysfs_remove_link(cls->kobj, dev_name(dev));
}
EXPORT_SYMBOL_GPL(class_compat_remove_link);

/* 设备类 子系统初始化 */
int __init classes_init(void)
{
    /* 创建 设备类 顶层目录 /sys/class
     * 之后新创建的 class，都属于这个 class_kset */
	class_kset = kset_create_and_add("class", NULL, NULL);
	if (!class_kset)
		return -ENOMEM;
	return 0;
}

EXPORT_SYMBOL_GPL(class_create_file);
EXPORT_SYMBOL_GPL(class_remove_file);
EXPORT_SYMBOL_GPL(class_unregister);
EXPORT_SYMBOL_GPL(class_destroy);

EXPORT_SYMBOL_GPL(class_interface_register);
EXPORT_SYMBOL_GPL(class_interface_unregister);
