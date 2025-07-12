/*
 * drivers/base/dd.c - The core device/driver interactions.
 * drivers/base/dd.c - 核心设备/驱动程序交互。
 *
 * This file contains the (sometimes tricky) code that controls the
 * interactions between devices and drivers, which primarily includes
 * driver binding and unbinding.
 * 该文件包含控制设备和驱动程序之间交互的（有时很棘手的）代码，
 * 主要包括驱动程序绑定和解除绑定。
 *
 * All of this code used to exist in drivers/base/bus.c, but was
 * relocated to here in the name of compartmentalization (since it wasn't
 * strictly code just for the 'struct bus_type'.
 * 所有这些代码原本都存在于 drivers/base/bus.c 文件中，但为了实现模块化设计，现
 * 已迁移到此处（因为这些代码并非仅仅针对“struct bus_type”这一结构体而言的）。
 *
 * Copyright (c) 2002-5 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 * Copyright (c) 2007-2009 Greg Kroah-Hartman <gregkh@suse.de>
 * Copyright (c) 2007-2009 Novell Inc.
 *
 * This file is released under the GPLv2
 */

#include <linux/device.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/async.h>
#include <linux/pm_runtime.h>
#include <scsi/scsi_scan.h>

#include "base.h"
#include "power/power.h"

/*
 * Deferred Probe infrastructure.
 *
 * Sometimes driver probe order matters, but the kernel doesn't always have
 * dependency information which means some drivers will get probed before a
 * resource it depends on is available.  For example, an SDHCI driver may
 * first need a GPIO line from an i2c GPIO controller before it can be
 * initialized.  If a required resource is not available yet, a driver can
 * request probing to be deferred by returning -EPROBE_DEFER from its probe hook
 *
 * Deferred probe maintains two lists of devices, a pending list and an active
 * list.  A driver returning -EPROBE_DEFER causes the device to be added to the
 * pending list.  A successful driver probe will trigger moving all devices
 * from the pending to the active list so that the workqueue will eventually
 * retry them.
 *
 * The deferred_probe_mutex must be held any time the deferred_probe_*_list
 * of the (struct device*)->p->deferred_probe pointers are manipulated
 */
/* 延迟探测基础结构
 *
 * 有时驱动程序的探测顺序确实很重要，但内核并不总是具备依赖关系信息，这意味着某
 * 些驱动程序可能会在所需资源尚未准备好时就被探测到。例如，一个 SDHCI 驱动程序可
 * 能首先需要从一个 i2c GPIO 控制器获取一条 GPIO 线路，然后才能进行初始化。如果
 * 所需资源尚未准备好，驱动程序可以通过在其探测钩子中返回 -EPROBE_DEFER 来请求推
 * 迟探测操作。
 *
 * 延迟探测会维护两个设备列表，即待处理列表和活动列表。如果驱动程序返回
 * -EPROBE_DEFER，那么该设备就会被添加到待处理列表中。而如果驱动程序的探测操作成
 * 功，则会将所有设备从待处理列表移动到活动列表中，以便工作队列最终能够再次尝试
 * 处理这些设备。
 *
 * 在对 (struct device*)->p->deferred_probe 指针所对应的
 * deferred_probe_xxx_list 及逆行操作的任何时候，都必须持有 deferred_probe_mutex
 * 保护锁。
 * */
/* deferred_probe_pending_list deferred_probe_active_list 链表保护锁 */
static DEFINE_MUTEX(deferred_probe_mutex);
/* 延迟探测 待处理链表 */
static LIST_HEAD(deferred_probe_pending_list);
/* 延迟探测 活动链表 */
static LIST_HEAD(deferred_probe_active_list);
/* 工作队列
 * 延迟探测的 重新启动探测就是在这个工作队列中处理的 */
static struct workqueue_struct *deferred_wq;

/**
 * deferred_probe_work_func() - Retry probing devices in the active list.
 */
/* 重试对活动列表中的设备进行探测
 * 这是真正进行重启探测的处理函数，由工作队列调用 */
static void deferred_probe_work_func(struct work_struct *work)
{
	struct device *dev;
	struct device_private *private;
	/*
	 * This block processes every device in the deferred 'active' list.
	 * Each device is removed from the active list and passed to
	 * bus_probe_device() to re-attempt the probe.  The loop continues
	 * until every device in the active list is removed and retried.
	 *
	 * Note: Once the device is removed from the list and the mutex is
	 * released, it is possible for the device get freed by another thread
	 * and cause a illegal pointer dereference.  This code uses
	 * get/put_device() to ensure the device structure cannot disappear
	 * from under our feet.
	 */
    /* 此代码块处理延迟'active'列表中的每一个设备。每个设备都会从 'active'列表中
     * 移除，并传递给 bus_probe_device() 函数以再次尝试进行探测。该循环会持续进
     * 行，直到列表中的所有设备都被移除和重新尝试探测。
     * 注意：一旦设备从列中移除且互斥锁被释放，该设备有可能被其它线程回收从而导
     * 致非法指针解引用错误。此代码使用 get_device()/put_device() 函数来确保设备
     * 结构不会从我们的处理流程中消失。 */
	mutex_lock(&deferred_probe_mutex);
    /* 遍历 deferred_probe_active_list 链表
     * 直到链表为空时退出
     * 也有可能被 queue_work 重新启动 */
	while (!list_empty(&deferred_probe_active_list)) {
        /* 通过链表节点，反查设备私有数据结构 device_private */
		private = list_first_entry(&deferred_probe_active_list,
					typeof(*dev->p), deferred_probe);
        /* 找到设备结构 */
		dev = private->device;
        /* 将设备从延迟探测列表中移除
         * 下面要进行探测了，如果还没成功，会再次加入到链表中的 */
		list_del_init(&private->deferred_probe);

        /* 引用一下要操作的设备，避免被释放 */
		get_device(dev);

		/*
		 * Drop the mutex while probing each device; the probe path may
		 * manipulate the deferred list
		 */
        /* 在探测每个设备时取消互斥锁；探测路径可能会对延迟列表进行操作
         * 在重新探测的流程中，如果仍然没有成功，会再次加入到延迟探测链表中
         * 是通过调用 driver_deferred_probe_add() 加入的
         * 添加时会获取 deferred_probe_mutex, 所以这里要先释放
         * 不然会出现死锁情况 */
		mutex_unlock(&deferred_probe_mutex);
		dev_dbg(dev, "Retrying from deferred list\n");
        /* 启动为设备探测驱动程序流程
         * 这是一个完整的流程，与新注册设备时 探测驱动是一样的 */
		bus_probe_device(dev);
		mutex_lock(&deferred_probe_mutex);

        /* 释放对设备的引用 */
		put_device(dev);
	}
	mutex_unlock(&deferred_probe_mutex);
}
/* 声明延迟探测工作任务
 * 工作任务的处理函数为 deferred_probe_work_func */
static DECLARE_WORK(deferred_probe_work, deferred_probe_work_func);

/* 将设备添加到延迟探测列表中
 * 注意，这里加入的是设备，因为一个设备只能对应到一个驱动
 * 会确定只探测这一个设备
 * 如果加入的是驱动，那驱动会遍历所有的设备，即使设备已经 probe 成功了 */
static void driver_deferred_probe_add(struct device *dev)
{
	mutex_lock(&deferred_probe_mutex);
    /* 确认设备没有在延迟探测链表中
     * 设备通过 deferred_probe 链表节点加入到延迟探测链表中 */
	if (list_empty(&dev->p->deferred_probe)) {
		dev_dbg(dev, "Added to deferred list\n");
        /* 通过设备的 deferred_probe 链接到 deferred_probe_pending_list 链表中 */
		list_add_tail(&dev->p->deferred_probe, &deferred_probe_pending_list);
	}
	mutex_unlock(&deferred_probe_mutex);
}

/* 将设备从延迟探测列表中移除
 * 在设备已经完成了探测绑定处理时 需要移除
 * 如果需要移除设备了，也需要从延迟探测列表中移除
 * 注意，设备不一定在列表中，如果在，就移除掉 */
void driver_deferred_probe_del(struct device *dev)
{
	mutex_lock(&deferred_probe_mutex);
    /* 如果设备在延迟探测列表中
     * 是通过 deferred_probe 链表节点连接的
     * 判断 节点是否为 NULL 即可确认是否在列表中 */
	if (!list_empty(&dev->p->deferred_probe)) {
		dev_dbg(dev, "Removed from deferred list\n");
        /* 从延迟探测列表中移除
         * 注意，设备可能在 deferred_probe_pending_list 中
         * 也可能在 deferred_probe_active_list 中 */
		list_del_init(&dev->p->deferred_probe);
	}
	mutex_unlock(&deferred_probe_mutex);
}

/* 延迟探测 功能是否已使能
 * 延迟探测 是在工作队列中处理的，必须要在队列建成之后才能触发 */
static bool driver_deferred_probe_enable = false;
/**
 * driver_deferred_probe_trigger() - Kick off re-probing deferred devices
 *
 * This functions moves all devices from the pending list to the active
 * list and schedules the deferred probe workqueue to process them.  It
 * should be called anytime a driver is successfully bound to a device.
 */
/* 触发 启动重新探测延迟的设备
 * 此函数会将所有设备从待处理列表移至活动列表，并安排延迟的探测工作队列来处理这
 * 些设备。每当驱动程序成功与设备绑定时，都应调用此函数。
 *
 * 触发延迟探测的三种情况：
 * 1. 延迟探测功能初始化时: deferred_probe_initcall
 * 2. 有新的设备注册，并成功绑定了驱动
 * 3. 有新的驱动注册，并成功匹配到了设备
 * */
static void driver_deferred_probe_trigger(void)
{
    /* 还未初始化延迟探测功能，则直接返回 */
	if (!driver_deferred_probe_enable)
		return;

	/*
	 * A successful probe means that all the devices in the pending list
	 * should be triggered to be reprobed.  Move all the deferred devices
	 * into the active list so they can be retried by the workqueue
	 */
    /* 一次成功的探测意味着待探测列表中的所有设备都应被重新触发进行探测。将所有
     * 被推迟的设备移至活动列表中，以便由工作队列对其进行重新尝试。
     * 新添加的设备会加入到 deferred_probe_pending_list 链表中
     * 当启动一次探测时，把所有的设备都移动到 deferred_probe_active_list 中 */
	mutex_lock(&deferred_probe_mutex);
	list_splice_tail_init(&deferred_probe_pending_list,
			      &deferred_probe_active_list);
	mutex_unlock(&deferred_probe_mutex);

	/*
	 * Kick the re-probe thread.  It may already be scheduled, but it is
	 * safe to kick it again.
	 */
    /* 将工作任务添加到工作队列中
     * 调用本操作时，队列可能已经在调度运行了，但再次启动也是安全的 */
	queue_work(deferred_wq, &deferred_probe_work);
}

/**
 * deferred_probe_initcall() - Enable probing of deferred devices
 *
 * We don't want to get in the way when the bulk of drivers are getting probed.
 * Instead, this initcall makes sure that deferred probing is delayed until
 * late_initcall time.
 */
/* 延迟探测设备的探测功能初始化
 *
 * 我们不想在大量驱动程序被探测时造成干扰。相反，这个 initcall 函数确保延迟探测
 * 被延迟到 late_initcall 时间。
 *
 * 在系统启动初始化阶段，有大量的设备/驱动添加，会有大量的探测处理。
 * 这里为了不造成频繁的延迟探测，重复探测（可能会有无效的重复探测）
 * 把这个初始化放在了 late_initcall 中
 * 这是一个比较晚的初始化阶段，设备/驱动都已经添加完成了
 * 如果需要进行延迟探测的，那都已经加入到延迟探测列表中了
 * 此时初始化一下，并启动一次探测，会更高效
 * */
static int deferred_probe_initcall(void)
{
    /* 创建一个单线程工作队列 */
	deferred_wq = create_singlethread_workqueue("deferwq");
	if (WARN_ON(!deferred_wq))
		return -ENOMEM;

    /* 设置 driver_deferred_probe_enable 为 true
     * 启用延迟探测功能了 */
	driver_deferred_probe_enable = true;
    /* 触发一次探测 */
	driver_deferred_probe_trigger();
	return 0;
}
late_initcall(deferred_probe_initcall);

/* 设备与驱动的绑定
 * 将设备连接到驱动的设备链表上，并调用bus的 bus_notifier */
static void driver_bound(struct device *dev)
{
    /* 检查设备的 knode_driver 是否已经被链接到 某个 klist 中了
     * knode_driver 是连接到匹配的驱动的 klist_devices 链表的节点
     * 没有绑定过驱动的设备，这个 knode 一定为 NULL
     * 绑定了驱动后，一定连接到某个驱动的 klist_devices */
	if (klist_node_attached(&dev->p->knode_driver)) {
		printk(KERN_WARNING "%s: device %s already bound\n",
			__func__, kobject_name(&dev->kobj));
		return;
	}

	pr_debug("driver: '%s': %s: bound to device '%s'\n", dev_name(dev),
		 __func__, dev->driver->name);

    /* 将设备的 knode_driver 连接到 驱动的 klist_devices 链表中
     * 这里完成了设备与驱动的相互绑定，设备指向驱动，并连接到驱动的链表上 */
	klist_add_tail(&dev->p->knode_driver, &dev->driver->p->klist_devices);

	/*
	 * Make sure the device is no longer in one of the deferred lists and
	 * kick off retrying all pending devices
	 */
    /* 确保该设备不再在延迟探测列表中
     * 并开始重试所有挂起的设备 */
    /* 新匹配的这个设备已经完成了与驱动程序的绑定
     * 这个设备无论是否是延迟探测的，那都从延迟探测列表中移除 */
	driver_deferred_probe_del(dev);
    /* 触发 启动重新探测延迟的设备
     * 这里不管新匹配绑定的设备是什么设备，是否为延迟探测的设备所依赖的
     * 都需要触发一下，万一是依赖的设备呢
     * 无法判断一个新匹配绑定成功的设备是否为延迟探测的设备依赖的 */
	driver_deferred_probe_trigger();

    /* 设备所属的 bus 通知机制
     * 已经绑定驱动了，通知所有注册的bus通知列表
     * BUS_NOTIFY_BOUND_DRIVER - 已经绑定了 */
	if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_BOUND_DRIVER, dev);
}

/* 处理设备所在 bus 的 bus_notifier 通知器（要绑定了）
 *
 * 添加sysfs文件系统中
 * 驱动目录下的设备名符号链接
 * 设备目录下的driver符号链接
 * 
 * 本函数算是绑定前的准备工作，即开始绑定了 */
static int driver_sysfs_add(struct device *dev)
{
	int ret;

    /* 设备所属的 bus 通知机制
     * 要绑定驱动了，通知所有注册的bus通知列表
     * BUS_NOTIFY_BIND_DRIVER - 将要绑定了 */
	if (dev->bus)
		blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
					     BUS_NOTIFY_BIND_DRIVER, dev);

    /* 在驱动目录下，创建一个以设备名命名的符号链接，指向设备 */
	ret = sysfs_create_link(&dev->driver->p->kobj, &dev->kobj,
			  kobject_name(&dev->kobj));
	if (ret == 0) {
        /* 在设备目录下，创建一个 driver 符号链接，指向驱动 */
		ret = sysfs_create_link(&dev->kobj, &dev->driver->p->kobj,
					"driver");
		if (ret)
			sysfs_remove_link(&dev->driver->p->kobj,
					kobject_name(&dev->kobj));
	}
	return ret;
}

/* 移除sysfs文件系统中
 * device 设备目录下的 driver 符号链接
 * 在设备目录下，driver 符号链接指向 匹配的驱动
 * driver 驱动目录下的 device.name 符号链接
 * 在驱动目录下，以设备名命名的符号链接，指向设备
 * */
static void driver_sysfs_remove(struct device *dev)
{
    /* 设备匹配的驱动程序 */
	struct device_driver *drv = dev->driver;

	if (drv) {
        /* 移除驱动目录下以设备名命名的符号链接 */
		sysfs_remove_link(&drv->p->kobj, kobject_name(&dev->kobj));
        /* 移除设备目录下，driver 符号链接 */
		sysfs_remove_link(&dev->kobj, "driver");
	}
}

/**
 * device_bind_driver - bind a driver to one device.
 * @dev: device.
 *
 * Allow manual attachment of a driver to a device.
 * Caller must have already set @dev->driver.
 *
 * Note that this does not modify the bus reference count
 * nor take the bus's rwsem. Please verify those are accounted
 * for before calling this. (It is ok to call with no other effort
 * from a driver's probe() method.)
 *
 * This function must be called with the device lock held.
 */
/* device_bind_driver - 将驱动程序绑定到一个设备
 * @dev:    device
 *
 * 允许手动连接驱动程序到设备。
 * 调用者必须已经设置了 @dev->driver 
 *
 * 请注意，此操作不会修改bus总线的引用计数，也不会使用bus总线的rwsem。
 * 在调用此函数之前，请务必确认已经将这些情况考虑清楚了。
 * （在驱动程序的 probe() 方法中无需进行其它操作即可调用此函数也是可行的。)
 *
 * 此函数必须在设备锁定状态下调用
 *
 * 本函数是 really_probe() 的一部分
 * 只是添加了 sysfs 中的属性，并完成了设备和驱动程序的绑定
 * 但并不会调用关键的 probe() 回调
 * 很少有驱动会直接使用本函数绑定设备和驱动
 * 在明确设备所对应的驱动程序时，可以先设置 dev->driver 驱动程序
 * 然后调用本函数将设备与驱动程序绑定 */
int device_bind_driver(struct device *dev)
{
	int ret;

    /* 处理设备所在 bus 的 bus_notifier 通知器（要绑定了）
     * 添加sysfs文件系统中属性符号链接 */
	ret = driver_sysfs_add(dev);
	if (!ret)
        /* 设备与驱动的绑定
         * 将设备连接到驱动的设备链表上，并调用bus的 bus_notifier */
		driver_bound(dev);
	return ret;
}
EXPORT_SYMBOL_GPL(device_bind_driver);

/* probe_count 用于实现一种简单的设备探测同步机制
 * 当进行设备与驱动程序的绑定前，先递增该计数，探测完成后递减
 * 通过判断 probe_count 是否为 0，可简单判断匹配是否完成 */
static atomic_t probe_count = ATOMIC_INIT(0);
/* probe_waitqueue 用于实现等待唤醒
 * 通过与 probe_count 配合，实现等待匹配操作完成
 * 在每个设备与驱动程序的匹配完成之后都会唤醒 probe_waitqueue
 * 所以还需要通过一些其它的判断 确认是否是等待的设备匹配完成
 * 比如判断 设备的设备号是否已经注册 */
static DECLARE_WAIT_QUEUE_HEAD(probe_waitqueue);

/* 真正的 probe 处理
 * 此函数中完成设备与驱动程序的绑定，并且调用 probe() 回调
 * 需要完成设备/驱动的绑定，sysfs 的处理 */
/* 如果匹配的设备需要依赖一个其它设备，但被依赖的设备还没有准备好
 * 此时被依赖的设备应该返回 EPROBE_DEFER 错误码
 * 新匹配的设备将会添加到延迟探测列表中，过一段时间再尝试匹配 */
static int really_probe(struct device *dev, struct device_driver *drv)
{
	int ret = 0;

    /* 开始设备与驱动程序的匹配，递增 probe_count 值 */
	atomic_inc(&probe_count);
	pr_debug("bus: '%s': %s: probing driver %s with device %s\n",
		 drv->bus->name, __func__, drv->name, dev_name(dev));
    /* 检查设备资源列表，没有任何资源的设备，报警告 */
	WARN_ON(!list_empty(&dev->devres_head));

    /* 将设备的 driver 指针指向匹配的驱动 */
	dev->driver = drv;
    /* 上面的 dev->driver = drv; 将设备与驱动绑定了
     * 这里的绑定，是设备的 driver 指针指向了 匹配的驱动
     * 但还没有调用 bus/drv 的 probe 完成全部的绑定流程
     * 此时要处理 sysfs 中的设备/驱动关联属性了
     * 这里把 bus 的 bus_notifier 也放在了 driver_sysfs_add 中处理
     * 有点把代码结构搞乱了。。。。 */
	if (driver_sysfs_add(dev)) {
		printk(KERN_ERR "%s: driver_sysfs_add(%s) failed\n",
			__func__, dev_name(dev));
		goto probe_failed;
	}

    /* 要调用 probe 回调了
     * 优先调用bus的，如果bus未提供，则调用 drv的
     * 这里是优先原则，如果调用了 bus，就不会调用drv的了 */
	if (dev->bus->probe) {
		ret = dev->bus->probe(dev);
		if (ret)
			goto probe_failed;
	} else if (drv->probe) {
		ret = drv->probe(dev);
		if (ret)
			goto probe_failed;
	}

    /* 完成设备与驱动的绑定的最后步骤
     * 因为完成了一个设备与驱动程序的绑定
     * driver_bound() 中会触发 延迟探测 机制 */
	driver_bound(dev);
    /* 上面的流程都正确处理了
     * 即完成了设备与驱动的匹配绑定
     * 则将返回值设置为 1，因为在 遍历匹配的处理中，返回 1 才会停止 */
	ret = 1;
	pr_debug("bus: '%s': %s: bound device %s to driver %s\n",
		 drv->bus->name, __func__, dev_name(dev), drv->name);
    /* 绑定成功了，则跳转到 done 返回了 */
	goto done;

/* 绑定过程中各种失败的处理
 * 清除各种已配置/可能配置 的内容 */
probe_failed:
    /* 释放所有与设备相关的被管理的资源 */
	devres_release_all(dev);
    /* 移除 sysfs 下，匹配的设备和驱动 互相链接的符号链接 */
	driver_sysfs_remove(dev);
    /* 设置设备匹配的驱动为NULL
     * 这样设备没有绑定的驱动了 */
	dev->driver = NULL;

    /* 如果驱动程序请求重试  probe */
	if (ret == -EPROBE_DEFER) {
		/* Driver requested deferred probing */
        /* 驱动程序请求延迟探测 */
		dev_info(dev, "Driver %s requests probe deferral\n", drv->name);
        /* 将设备添加到延迟探测列表中
         * 这里只是加入到等待列表中了，以后有新的设备/驱动添加时会调用 */
		driver_deferred_probe_add(dev);
	} else if (ret != -ENODEV && ret != -ENXIO) {
		/* driver matched but the probe failed */
        /* 驱动是匹配的，但探测失败 */
		printk(KERN_WARNING
		       "%s: probe of %s failed with error %d\n",
		       drv->name, dev_name(dev), ret);
	} else {
        /* 驱动拒绝匹配设备，可能是不兼容 */
		pr_debug("%s: probe of %s rejects match %d\n",
		       drv->name, dev_name(dev), ret);
	}
	/*
	 * Ignore errors returned by ->probe so that the next driver can try
	 * its luck.
	 */
    /* 这里对于错误的情况，返回时忽略了 ->probe 返回的错误
     * 以便让下一个驱动程序可以试试运气
     * 本函数返回 0，在 bus_for_each_drv / bus_for_each_dev 等函数中
     * 则不会停止遍历，而是继续遍历下一个驱动/设备 */
	ret = 0;
done:
    /* 到这里，无论是完成了设备与驱动程序的匹配
     * 还是设备被加入到了 延迟探测 列表中
     * 这里都会把 probe_count 递减
     * 所以，单靠 probe_count 无法确定所需的设备是否完成了注册 */
	atomic_dec(&probe_count);
    /* 唤醒 probe_waitqueue
     * 某些驱动中，会等待 设备匹配完成
     * 比如在 init/do_mounts.c 中，等待已经注册的设备完成探测 */
	wake_up(&probe_waitqueue);
	return ret;
}

/**
 * driver_probe_done
 * Determine if the probe sequence is finished or not.
 *
 * Should somehow figure out how to use a semaphore, not an atomic variable...
 */
/* 确定探测序列是否完成
 * 通过简单的判断 probe_count 是否为 0 确定 */
int driver_probe_done(void)
{
	pr_debug("%s: probe_count = %d\n", __func__,
		 atomic_read(&probe_count));
    /* 通过简单的判断 probe_count 是否为 0
     * 如果不为 0，则说明仍然有在匹配中的 */
	if (atomic_read(&probe_count))
		return -EBUSY;
	return 0;
}

/**
 * wait_for_device_probe
 * Wait for device probing to be completed.
 */
/* 等待设备探测完成
 * 调用本函数可能会休眠，由 probe_waitqueue 唤醒 */
void wait_for_device_probe(void)
{
	/* wait for the known devices to complete their probing */
    /* 等待被唤醒，并检查 probe_count 是否为 0
     * wait_event 在条件满足时返回，否则会继续休眠等待 */
	wait_event(probe_waitqueue, atomic_read(&probe_count) == 0);
    /* 同步所有的异步函数调用
     * 该函数等待直到所有异步函数调用完成 */
	async_synchronize_full();
    /* scsi 等待异步扫描完成 */
	scsi_complete_async_scans();
}
EXPORT_SYMBOL_GPL(wait_for_device_probe);

/**
 * driver_probe_device - attempt to bind device & driver together
 * @drv: driver to bind a device to
 * @dev: device to try to bind to the driver
 *
 * This function returns -ENODEV if the device is not registered,
 * 1 if the device is bound successfully and 0 otherwise.
 *
 * This function must be called with @dev lock held.  When called for a
 * USB interface, @dev->parent lock must be held as well.
 */
/* driver_probe_device - 尝试将设备和驱动程序绑定在一起
 * @drv:    将设备绑定到的驱动程序
 * @dev:    试图绑定到驱动程序的设备
 *
 * 如果设备未注册，该函数返回 -ENODEV, 如果设备绑定成功返回 1，否则返回 0
 *
 * 本函数必须在持有 @dev 锁的情况下调用。
 * 当为 USB接口调用时，也必须持有 @dev->parent 锁
 * */
int driver_probe_device(struct device_driver *drv, struct device *dev)
{
	int ret = 0;

    /* 检查设备是否已注册
     * 绑定过程中需要在 sysfs 中创建一些文件，所以必须要完成在 sysfs 中的注册 */
	if (!device_is_registered(dev))
		return -ENODEV;

	pr_debug("bus: '%s': %s: matched device %s with driver %s\n",
		 drv->bus->name, __func__, dev_name(dev), drv->name);

    /* // TODO: 电源管理相关，暂不分析 */
	pm_runtime_get_noresume(dev);
	pm_runtime_barrier(dev);
    /* 完成真正的绑定处理 */
	ret = really_probe(dev, drv);
    /* // TODO: 电源管理相关，暂不分析 */
	pm_runtime_put_sync(dev);

	return ret;
}

/* device_attach 使用的匹配回调函数
 * 用于尝试将驱动程序绑定到设备
 * device_attach 函数中，遍历 bus 上的每一个驱动程序
 * 并将 设备结构 与 需要匹配的驱动程序结构 做为参数调用本函数
 * 本函数是为设备匹配驱动程序，设备是不变的，在遍历驱动程序
 * 所以当匹配成功了，就要退出遍历
 * 在 bus_for_each_drv 时，遍历函数返回 1 则退出遍历 */
static int __device_attach(struct device_driver *drv, void *data)
{
    /* 参数 data 就是需要进行匹配的设备结构 */
	struct device *dev = data;

    /* 调用 bus 的 match 回调进行尝试匹配
     * 对于 match 回调，如果匹配成功，是返回 1 的
     * 这里的判断是 如果返回了 0 ，则本函数返回 0
     * 也就是说，如果没有匹配成功，那本函数将直接返回 0
     * 如果匹配成功了，将继续向下执行
     * 无论匹配与否，都返回0，前面已经解释过了，即使匹配不成功，
     * 那也要继续匹配的，在 bus_for_each_drv 中，回调函数返回非0值
     * 将结束对设备的遍历, 所以这里必须要返回 0 */
	if (!driver_match_device(drv, dev))
		return 0;

    /* 进行设备与驱动程序的绑定
     * 注意，与 __driver_attach 中的调用不同
     * 这里会返回 driver_probe_device() 的处理结果
     * 如果绑定成功了，则返回 1 ， 否则 返回 0
     * 这里在绑定成功了，一定要返回 1
     * 这样就会结束 bus_for_each_drv 遍历，
     * 因为一个设备只能对应到一个驱动，只要绑定成功了，就立即结束 */
	return driver_probe_device(drv, dev);
}

/**
 * device_attach - try to attach device to a driver.
 * @dev: device.
 *
 * Walk the list of drivers that the bus has and call
 * driver_probe_device() for each pair. If a compatible
 * pair is found, break out and return.
 *
 * Returns 1 if the device was bound to a driver;
 * 0 if no matching driver was found;
 * -ENODEV if the device is not registered.
 *
 * When called for a USB interface, @dev->parent lock must be held.
 */
/* device_attach - 尝试将设备绑定到驱动程序上
 * @dev:    将要绑定驱动程序的设备
 *
 * 遍历总线上的驱动程序列表，并为每一对调用 driver_probe_device()
 * 如果找到一个兼容的，则退出遍历并返回
 * 为设备匹配驱动程序，只要找到了一个匹配成功的，则会绑定并返回
 * 因为一个设备只能对应到一个驱动
 * 如果设备与驱动程序绑定成功，则返回 1
 * 设备没有匹配到驱动程序，则返回 0
 * 设备并没有注册，则返回 -ENODEV */
/* 当调用USB接口时，必须持有 @dev->parent 锁 */
int device_attach(struct device *dev)
{
	int ret = 0;

    /* 锁定设备自己 */
	device_lock(dev);
    /* 检查设备是否已有指向的驱动程序 */
	if (dev->driver) {
        /* 检查设备的 knode_driver 是否已经被链接到 某个 klist 中了
         * knode_driver 是连接到匹配的驱动的 klist_devices 链表的节点
         * 没有绑定过驱动的设备，这个 knode 一定为 NULL
         * 绑定了驱动后，一定连接到某个驱动的 klist_devices */
		if (klist_node_attached(&dev->p->knode_driver)) {
            /* 如果已经在某个驱动程序的链表中，则直接退出，返回 1 */
			ret = 1;
			goto out_unlock;
		}
        /* 这里的设备，已经被设置过 driver 了
         * 所以直接调用了 device_bind_driver()
         * 这个函数中，并不会调用 probe() 回调 */
		ret = device_bind_driver(dev);
		if (ret == 0)
            /* 成功了则返回 1
             * 这个步骤中，几乎不会失败 */
			ret = 1;
		else {
            /* 绑定失败了，则清空设备已经指向的驱动程序
             * 并返回 0 */
			dev->driver = NULL;
			ret = 0;
		}
	} else {
        /* 如果设备没有指向的驱动程序
         * 则遍历设备所在的bus，为其匹配驱动程序 */
        /* // TODO: 电源管理相关，暂不分析 */
		pm_runtime_get_noresume(dev);
        /* 遍历设备所在的bus，设备匹配驱动程序
         * 这里遍历的是bus上的所有驱动程序，并调用 __device_attach */
		ret = bus_for_each_drv(dev->bus, NULL, dev, __device_attach);
        /* // TODO: 电源管理相关，暂不分析 */
		pm_runtime_put_sync(dev);
	}
out_unlock:
    /* 解锁设备自己 */
	device_unlock(dev);
	return ret;
}
EXPORT_SYMBOL_GPL(device_attach);

/* driver_attach 使用的匹配回调函数
 * 用于尝试将驱动程序绑定到设备
 * driver_attach 函数中，遍历 bus 上的每一个设备
 * 并将 设备结构 与 需要匹配的驱动程序结构 做为参数调用本函数
 * 与 __device_attach 不同
 * 本函数是为驱动程序匹配bus上的设备
 * 一个驱动程序可能匹配多个设备，所以本函数一直返回 0 */
static int __driver_attach(struct device *dev, void *data)
{
    /* 参数 data 就是需要进行匹配的驱动程序结构 */
	struct device_driver *drv = data;

	/*
	 * Lock device and try to bind to it. We drop the error
	 * here and always return 0, because we need to keep trying
	 * to bind to devices and some drivers will return an error
	 * simply if it didn't support the device.
     * 锁定设备并尝试绑定它。
     * 在这里忽略了错误，并始终返回0，是因为需要持续尝试绑定设备，
     * 而且某些驱动程序如果设备不受支持，会简单的返回错误。
     * 驱动程序匹配设备，需要遍历所有的设备，
     * 因为一个驱动程序，可以匹配多个设备的
	 *
	 * driver_probe_device() will spit a warning if there
	 * is an error.
     * 如果出现错误，driver_probe_device() 将发出警告。
	 */

    /* 调用 bus 的 match 回调进行尝试匹配
     * 对于 match 回调，如果匹配成功，是返回 1 的
     * 这里的判断是 如果返回了 0 ，则本函数返回 0
     * 也就是说，如果没有匹配成功，那本函数将直接返回 0
     * 如果匹配成功了，将继续向下执行
     * 无论匹配与否，都返回0，前面已经解释过了，即使匹配不成功，
     * 那也要继续匹配的，在 bus_for_each_dev 中，回调函数返回非0值
     * 将结束对设备的遍历, 所以这里必须要返回 0 */
	if (!driver_match_device(drv, dev))
		return 0;

    /* 如果有父设备，则需要先锁定父设备 */
	if (dev->parent)	/* Needed for USB */
		device_lock(dev->parent);
    /* 锁定设备自己 */
	device_lock(dev);
    /* 匹配前，需要先确认设备并没有匹配的驱动程序
     * 如果设备已经匹配了驱动程序，需要先解绑再重新匹配
     * (device_reprobe())*/
	if (!dev->driver)
		driver_probe_device(drv, dev);
    /* 解锁设备自己 */
	device_unlock(dev);
    /* 有父设备，将父设备解锁 */
	if (dev->parent)
		device_unlock(dev->parent);

	return 0;
}

/**
 * driver_attach - try to bind driver to devices.
 * @drv: driver.
 *
 * Walk the list of devices that the bus has on it and try to
 * match the driver with each one.  If driver_probe_device()
 * returns 0 and the @dev->driver is set, we've found a
 * compatible pair.
 */
/* 尝试将驱动程序绑定到设备
 * 遍历总线上的设备列表，并尝试将驱动程序与每个设备匹配。
 * 如果 driver_probe_device() 返回0，且 @dev->driver 已设置，
 * 则表示找到了兼容的驱动程序 */
int driver_attach(struct device_driver *drv)
{
    /* 遍历总线上的设备列表，为每一个设备调用 __driver_attach 函数 */
	return bus_for_each_dev(drv->bus, NULL, drv, __driver_attach);
}
EXPORT_SYMBOL_GPL(driver_attach);

/*
 * __device_release_driver() must be called with @dev lock held.
 * When called for a USB interface, @dev->parent lock must be held as well.
 */
/* __device_release_driver() 必须在持有 @dev 锁的情况下调用。
 * 当为USB接口设备调用时，也必须持有 @dev->parent 锁 */
static void __device_release_driver(struct device *dev)
{
	struct device_driver *drv;

    /* 设备所匹配的驱动 */
	drv = dev->driver;
	if (drv) {
        /* // TODO: 电源管理相关，暂不分析 */
		pm_runtime_get_sync(dev);

        /* 移除 sysfs 下，匹配的设备和驱动 互相链接的符号链接 */
		driver_sysfs_remove(dev);

        /* 设备所属的 bus 通知机制
         * 要解绑驱动了，通知所有注册的bus通知列表
         * BUS_NOTIFY_UNBIND_DRIVER - 将要解绑了 */
		if (dev->bus)
			blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
						     BUS_NOTIFY_UNBIND_DRIVER,
						     dev);

        /* // TODO: 电源管理相关，暂不分析 */
		pm_runtime_put_sync(dev);

        /* 如果设备属于某个bus，并且提供了 remove 回调
         * 则优先调用 bus 的 remove， 这时就不会调用驱动的 remove 回调了
         * 在驱动注册时，如果bus和drv都提供了 remove，会提示要修改 */
		if (dev->bus && dev->bus->remove)
			dev->bus->remove(dev);
		else if (drv->remove)
			drv->remove(dev);
        /* 释放所有与设备相关的被管理的资源 */
		devres_release_all(dev);
        /* 设置设备匹配的驱动为NULL
         * 这样设备没有绑定的驱动了 */
		dev->driver = NULL;
        /* 将设备从匹配的驱动程序的设备链表上移除 */
		klist_remove(&dev->p->knode_driver);
        /* BUS_NOTIFY_UNBOUND_DRIVER - 已经解绑了 */
		if (dev->bus)
			blocking_notifier_call_chain(&dev->bus->p->bus_notifier,
						     BUS_NOTIFY_UNBOUND_DRIVER,
						     dev);

	}
}

/**
 * device_release_driver - manually detach device from driver.
 * @dev: device.
 *
 * Manually detach device from driver.
 * When called for a USB interface, @dev->parent lock must be held.
 */
/* 分离设备与驱动程序
 * 手动从驱动程序中分离设备。
 * 当调用USB接口时，必须持有 @dev->parent 锁 */
void device_release_driver(struct device *dev)
{
	/*
	 * If anyone calls device_release_driver() recursively from
	 * within their ->remove callback for the same device, they
	 * will deadlock right here.
	 */
    /* 如果在同一设备的 ->remove 回调中递归调用了 device_release_driver()
     * 就会出现死锁情况
     * 调用 __device_release_driver() 前，会使用 device_lock() 锁定设备
     * 而 __device_release_driver() 中会调用 remove 回调
     * 如果在 remove 中调用了本函数 或 device_lock() 函数，就死锁了 */
	device_lock(dev);
	__device_release_driver(dev);
	device_unlock(dev);
}
EXPORT_SYMBOL_GPL(device_release_driver);

/**
 * driver_detach - detach driver from all devices it controls.
 * @drv: driver.
 */
/* 将驱动程序与其控制的所有设备分离 */
void driver_detach(struct device_driver *drv)
{
    /* device 的私有核心数据
     * 通过 device_private 的 knode_driver 挂接到匹配的驱动链表上
     * 通过驱动的设备链表，只能直接查到 device_private 数据结构 */
	struct device_private *dev_prv;
	struct device *dev;

	for (;;) {
        /* 所有匹配驱动的设备，都挂载到 klist_devices
         * 遍历所有的设备，直到遍历完成后主动退出，返回 */
		spin_lock(&drv->p->klist_devices.k_lock);
		if (list_empty(&drv->p->klist_devices.k_list)) {
			spin_unlock(&drv->p->klist_devices.k_lock);
			break;
		}
        /* 遍历 klist_devices 链表
         * 得到 设备的私有核心数据结构 device_private */
		dev_prv = list_entry(drv->p->klist_devices.k_list.prev,
				     struct device_private,
				     knode_driver.n_node);
        /* 找到 对应的 设备结构 device */
		dev = dev_prv->device;
        /* 引用一下要操作的设备，避免被释放 */
		get_device(dev);
		spin_unlock(&drv->p->klist_devices.k_lock);

        /* 如果有父设备，则需要先锁定父设备 */
		if (dev->parent)	/* Needed for USB */
			device_lock(dev->parent);
        /* 锁定设备自己 */
		device_lock(dev);
        /* 确认一下设备匹配的驱动，确实为需要操作的驱动 */
		if (dev->driver == drv)
            /* 完成设备与驱动的解绑 */
			__device_release_driver(dev);
        /* 解锁设备自己 */
		device_unlock(dev);
        /* 有父设备，将父设备解锁 */
		if (dev->parent)
			device_unlock(dev->parent);
        /* 释放对设备的引用 */
		put_device(dev);
	}
}

/*
 * These exports can't be _GPL due to .h files using this within them, and it
 * might break something that was previously working...
 */
/* 下面的两个函数在一些头文件中被引用了
 * 所以并未以 _GPL 方式导出 */

/* 获取设备的驱动特殊数据 */
void *dev_get_drvdata(const struct device *dev)
{
    /* 设备的私有核心数据结构有效
     * 则直接返回 driver_data 数据指针即可 */
	if (dev && dev->p)
		return dev->p->driver_data;
	return NULL;
}
EXPORT_SYMBOL(dev_get_drvdata);

/* 设置设备的驱动特殊数据 */
int dev_set_drvdata(struct device *dev, void *data)
{
	int error;

    /* 如果设备并没有私有核心数据结构
     * 则这里给分配一个
     * 因为这里也会分配，所以可以对设备结构先设置驱动特殊数据
     * 这样在 device_add 时就不重复分配了 */
	if (!dev->p) {
        /* device 结构私有数据分配并初始化 */
		error = device_private_init(dev);
		if (error)
			return error;
	}
    /* 驱动特殊数据 给 driver_data 指针 */
	dev->p->driver_data = data;
	return 0;
}
EXPORT_SYMBOL(dev_set_drvdata);
