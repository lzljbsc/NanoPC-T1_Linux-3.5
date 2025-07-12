/*
 * drivers/base/devres.c - device resource management
 *
 * Copyright (c) 2006  SUSE Linux Products GmbH
 * Copyright (c) 2006  Tejun Heo <teheo@suse.de>
 *
 * This file is released under the GPLv2.
 */

/* 设备资源管理
 * devres 基本上是与 struct device 关联的任意大小的内存区域的链表。
 * 每个 devres 条目都与一个释放函数相关联。
 * 一个 devres 可以通过几种方式释放。
 * 无论如何，所有 devres 条目都在驱动程序分离时释放。
 * 在释放时，将调用关联的释放函数，然后释放 devres 条目。
 * 这就是用来关联与设备相关联的资源的，比如 内存、GPIO等，
 * 使用设备资源管理的接口，可以将申请的资源与设备相关联
 * 当释放或销毁设备时，所关联的资源将被释放
 * 这样可以避免漏释放资源
 * */

#include <linux/device.h>
#include <linux/module.h>
#include <linux/slab.h>

#include "base.h"

/* devres_node 节点
 * 申请的资源 由本结构管理
 * 本结构管理的是加入到设备资源链表中，并记录资源释放函数
 * 加入到设备资源链表中，记录释放回调函数 */
struct devres_node {
    /* 链表节点，链接到 device devres_head 链表 */
	struct list_head		entry;
    /* 资源释放函数，由 release 释放被管理的资源
     * 注意，这里的被管理的资源并不是 devres 的 data 内存区域
     * 而是由其它驱动模块中，在 data 中存放的设备资源
     * 比如 gpio 号 */
	dr_release_t			release;
#ifdef CONFIG_DEBUG_DEVRES
    /* devres 调试信息，申请的名字及资源大小 */
	const char			*name;
	size_t				size;
#endif
};

/* 设备资源管理结构
 * 本结构包含 devres_node 节点 及 申请的内存资源 */
struct devres {
    /* devres_node 节点，管理结构 */
	struct devres_node		node;
	/* -- 3 pointers */
    /* data 内存区域，申请的一片内存区域
     * 其它的驱动模块中，将设备资源存放在 data 内存中 */
    /* 这里使用了可变数组，data 只是一个指针
     * 访问 data 后面的内存是可以的，合法的
     * 如果申请了大于 devres 结构大小的内存
     * 那通过访问 data[x] 即可访问后面的内存区域 */
	unsigned long long		data[];	/* guarantee ull alignment */
};

/* 设备资源管理组
 * 用于创建一个资源管理组，属于该组的资源可以统一释放
 * 组内也可以嵌套其它组，当然 组也可以跨组，组与组交叉 */
/* 组是一个扩展的功能，先创建一个组头[node[0]], 
 * 后面添加的资源节点都属于这个组，
 * 当不再添加资源节点时，可以关闭改组，即把 组尾[node[1]]
 * 添加到链表中，这样 node[0] 和 node[1] 之间的所有设备资源
 * 都是属于该组的
 * 组尾是可以没有的，即开放的，这样组头后面所有的资源都是属于该组的 */
struct devres_group {
    /* node[0] 是组的开始，即组头
     * node[1] 是组的结尾，即组尾
     * 这两个节点没有管理设备资源的功能，只是用来标记一个组 */
	struct devres_node		node[2];
    /* 组的 id，用来查找、定位某个组  */
	void				*id;
    /* color 用于在释放资源时，标记某个组是否被其它组完全包含
     * == 1 标识未被完全包含
     * == 2 标识被完全包含 */
	int				color;
	/* -- 8 pointers */
};

/* devres 的调试信息
 * 用于记录使用 devres 分配的资源，包括名字，大小 */
#ifdef CONFIG_DEBUG_DEVRES
/* devres 日志控制
 * 做为模块参数，如果 == 1，则将输出 devres_log 中的信息 */
static int log_devres = 0;
module_param_named(log, log_devres, int, S_IRUGO | S_IWUSR);

/* 设置 devres_node 调试信息
 * 包括 node 名字 和 资源大小 */
static void set_node_dbginfo(struct devres_node *node, const char *name,
			     size_t size)
{
	node->name = name;
	node->size = size;
}

/* devres 日志
 * 受 log_devres 参数控制 */
static void devres_log(struct device *dev, struct devres_node *node,
		       const char *op)
{
	if (unlikely(log_devres))
		dev_printk(KERN_ERR, dev, "DEVRES %3s %p %s (%lu bytes)\n",
			   op, node, node->name, (unsigned long)node->size);
}
#else /* CONFIG_DEBUG_DEVRES */
/* 未配置 CONFIG_DEBUG_DEVRES ，则调试相关的均为空操作 */
#define set_node_dbginfo(node, n, s)	do {} while (0)
#define devres_log(dev, node, op)	do {} while (0)
#endif /* CONFIG_DEBUG_DEVRES */

/*
 * Release functions for devres group.  These callbacks are used only
 * for identification.
 */
/* devres组的 release 函数。
 * 这些 release 仅用于标识用途
 * 在 devre group 中， node[0] 的release 对应 group_open_release
 * node[1] 的release 对应 group_close_release */
/* devres_group 的 node[0] 对应的 release */
static void group_open_release(struct device *dev, void *res)
{
	/* noop */
}

/* devres_group 的 node[1] 对应的 release */
static void group_close_release(struct device *dev, void *res)
{
	/* noop */
}

/* 根据 devres_node 结构 找到所属的 devres_group
 * 借助 devres_node 中的 release 回调 */
static struct devres_group * node_to_group(struct devres_node *node)
{
    /* group_open_release 对应 node[0] 节点 */
	if (node->release == &group_open_release)
		return container_of(node, struct devres_group, node[0]);
    /* group_open_release 对应 node[1] 节点 */
	if (node->release == &group_close_release)
		return container_of(node, struct devres_group, node[1]);
	return NULL;
}

/* 分配设备资源管理结构 struct devres
 * 并且也会分配 @size 大小的内存，做为设备资源内存
 * 返回的是设备资源结构指针 struct devres */
static __always_inline struct devres * alloc_dr(dr_release_t release,
						size_t size, gfp_t gfp)
{
    /* 需要分配的内存大小
     * @size是需要使用的内存大小，被管理的内存部分
     * 所以，需要加上管理结构 struct devres 的大小 */
	size_t tot_size = sizeof(struct devres) + size;
	struct devres *dr;

    /* 使用 kmalloc_track_caller 分配内存
     * kmalloc_track_caller 跟踪内存分配的调用者信息
     * 更方便调试和性能分析 */
    /* 分配的内存，直接是 struct devres 内存, 全部由其管理 */
	dr = kmalloc_track_caller(tot_size, gfp);
	if (unlikely(!dr))
		return NULL;

    /* 全部内存区域 置 0 */
	memset(dr, 0, tot_size);
    /* 初始化链表头，后面要加入到设备的 devres_head 链表中 */
	INIT_LIST_HEAD(&dr->node.entry);
    /* 资源的释放函数回调，资源释放时将调用此函数
     * 注意，这里的 release 只是释放被管理的资源，不是 devres 结构 */
	dr->node.release = release;
    /* 返回的是 struct devres 结构指针 */
	return dr;
}

/* 将设备资源管理节点 devres_node 添加到 设备资源管理 链表中 */
static void add_dr(struct device *dev, struct devres_node *node)
{
    /* 输出 ADD 调试信息, 是否输出与 log_devres 参数有关 */
	devres_log(dev, node, "ADD");
    /* 检查资源的链表节点是否为空
     * 此处必须为空，不为空则表示已被加入到某个设备的链表中了 */
	BUG_ON(!list_empty(&node->entry));
    /* 添加到 设备的 devres_head 链表中 */
	list_add_tail(&node->entry, &dev->devres_head);
}

#ifdef CONFIG_DEBUG_DEVRES
/* 调试版本的 devres_alloc
 * 启用 CONFIG_DEBUG_DEVRES 时， devres_alloc 的实现就是这个 */
void * __devres_alloc(dr_release_t release, size_t size, gfp_t gfp,
		      const char *name)
{
	struct devres *dr;

    /* 分配设备资源管理结构 struct devres,
     * 也会分配被管理资源的内存，由 dr->data 指向 */
	dr = alloc_dr(release, size, gfp);
	if (unlikely(!dr))
		return NULL;
    /* 记录分配的 devres 调试信息 */
	set_node_dbginfo(&dr->node, name, size);
    /* 返回的是 设备资源管理结构 中的 数据内存指针
     * 其它模块中，调用 devres 函数时，也是传入这个指针 */
	return dr->data;
}
EXPORT_SYMBOL_GPL(__devres_alloc);
#else
/**
 * devres_alloc - Allocate device resource data
 * @release: Release function devres will be associated with
 * @size: Allocation size
 * @gfp: Allocation flags
 *
 * Allocate devres of @size bytes.  The allocated area is zeroed, then
 * associated with @release.  The returned pointer can be passed to
 * other devres_*() functions.
 *
 * RETURNS:
 * Pointer to allocated devres on success, NULL on failure.
 */
/* devres_alloc - 分配设备资源数据
 * @release:    与 devres 关联的资源释放函数
 * @size:       申请的资源内存大小
 * @gfp:        分配 gfp 标志
 *
 * 分配大小为 @size 字节的设备资源。分配的区域会被清零，然后与 @release 关联起来。
 * 返回的指针可以传递给其他 devres_*() 函数。
 *
 * 这是只是分配 struct devres 结构 及 指定长度 @size 的内存区域
 * 并不会加入到任何设备资源链表中
 * 这是原始的分配 struct devres 结构
 * */
void * devres_alloc(dr_release_t release, size_t size, gfp_t gfp)
{
	struct devres *dr;

    /* 分配设备资源管理结构 struct devres,
     * 也会分配被管理资源的内存，由 dr->data 指向 */
	dr = alloc_dr(release, size, gfp);
	if (unlikely(!dr))
		return NULL;
    /* 返回的是 设备资源管理结构 中的 数据内存指针
     * 其它模块中，调用 devres 函数时，也是传入这个指针 */
	return dr->data;
}
EXPORT_SYMBOL_GPL(devres_alloc);
#endif

/**
 * devres_free - Free device resource data
 * @res: Pointer to devres data to free
 *
 * Free devres created with devres_alloc().
 */
/* 释放设备资源管理结构
 * @res:    指向资源管理结构中的 data 数据指针
 *
 * 释放使用 devres_alloc() 分配的 struct devres 结构 */
void devres_free(void *res)
{
	if (res) {
        /* res 就是 struct devres 结构中的 data 指针 */
		struct devres *dr = container_of(res, struct devres, data);

        /* 将要释放的 struct devres 结构，必须是没有链到设备资源链表中的
         * 调用本函数释放的，必须保证已经将其从设备资源链表中移除了 */
		BUG_ON(!list_empty(&dr->node.entry));
        /* 释放 struct devres 结构内存 */
		kfree(dr);
	}
}
EXPORT_SYMBOL_GPL(devres_free);

/**
 * devres_add - Register device resource
 * @dev: Device to add resource to
 * @res: Resource to register
 *
 * Register devres @res to @dev.  @res should have been allocated
 * using devres_alloc().  On driver detach, the associated release
 * function will be invoked and devres will be freed automatically.
 */
/* 注册设备资源
 * @dev:    用于添加资源的设备
 * @res:    注册所需资源
 *
 * 将 devres 对象 @res 注册到 @dev 上。@res 应该是通过 devres_alloc() 函数分配而
 * 来的。在驱动程序卸载时，与之相关的释放函数将会被调用，并且 devres 会自动被释
 * 放。
 * 
 * @res 必须是通过 devres_alloc 分配得到的
 * 函数内部直接反查 struct devres 结构，如果提供了其它的内存，则会出错 */
void devres_add(struct device *dev, void *res)
{
    /* res 就是 struct devres 的 data 成员
     * 找到资源的管理结构 struct devres */
	struct devres *dr = container_of(res, struct devres, data);
	unsigned long flags;

    /* 将资源加入到设备资源链表操作 原子操作 */
	spin_lock_irqsave(&dev->devres_lock, flags);
    /* 将资源通过 dr->node 加入到设备资源链表中 */
	add_dr(dev, &dr->node);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
}
EXPORT_SYMBOL_GPL(devres_add);

/* 在设备资源链表中，查找匹配的资源管理结构
 * 两个重要的匹配原则：
 * 1、资源释放函数 匹配， release 必须匹配，可以认为资源类型是一致的
 * 2、match 匹配函数匹配成功，这是外部提供的，用于检测被管理的资源的
 * match 匹配函数 不是必须的，未提供则认为全部匹配
 * */
static struct devres *find_dr(struct device *dev, dr_release_t release,
			      dr_match_t match, void *match_data)
{
    /* struct devres 通过 devres_node 的 entry 链接到设备的资源链表中 */
	struct devres_node *node;

    /* 遍历设备的 devres_head 链表，查找每一个 node 节点 */
	list_for_each_entry_reverse(node, &dev->devres_head, entry) {
        /* 通过 devres_node 反查 struct devres 结构 */
		struct devres *dr = container_of(node, struct devres, node);

        /* 设备资源中的 release 必须与提供的 release 一致
         * 不一致则继续遍历 */
		if (node->release != release)
			continue;
        /* 如果提供了匹配函数，则匹配函数返回 1 认为匹配成功 */
		if (match && !match(dev, dr->data, match_data))
			continue;
        /* 匹配成功了，找到了，返回 struct devres 结构指针 */
		return dr;
	}

    /* 未找到任何匹配的，返回 NULL */
	return NULL;
}

/**
 * devres_find - Find device resource
 * @dev: Device to lookup resource from
 * @release: Look for resources associated with this release function
 * @match: Match function (optional)
 * @match_data: Data for the match function
 *
 * Find the latest devres of @dev which is associated with @release
 * and for which @match returns 1.  If @match is NULL, it's considered
 * to match all.
 *
 * RETURNS:
 * Pointer to found devres, NULL if not found.
 */
/* devres_find - 查找设备资源
 * @dev:    用于查找资源的设备
 * @release:查找与 release 释放函数相关的资源
 * @match:  匹配函数（可选）, 用于匹配 match_data 与 设备资源
 * @match_data: 用于匹配的数据
 * 找出与 @release 相关联且 @match 函数返回值为 1 的 @dev 的devres。
 * 如果 @match 为 NULL，则视为匹配所有devres。
 *
 * 本函数查找特定的(release/match 匹配) 设备资源
 * 返回值为 设备资源管理结构的内存指针 dr->data
 * */
void * devres_find(struct device *dev, dr_release_t release,
		   dr_match_t match, void *match_data)
{
    /* dr 指向查找到的设备管理结构 */
	struct devres *dr;
	unsigned long flags;

    /* 设备资源链表操作 原子操作 */
	spin_lock_irqsave(&dev->devres_lock, flags);
    /* 在设备资源链表中，查找匹配的资源管理结构
     * 查找成功，返回的是 struct devres 结构指针 */
	dr = find_dr(dev, release, match, match_data);
	spin_unlock_irqrestore(&dev->devres_lock, flags);

    /* 查找成功了，返回设备资源结构的资源指针 */
	if (dr)
		return dr->data;
	return NULL;
}
EXPORT_SYMBOL_GPL(devres_find);

/**
 * devres_get - Find devres, if non-existent, add one atomically
 * @dev: Device to lookup or add devres for
 * @new_res: Pointer to new initialized devres to add if not found
 * @match: Match function (optional)
 * @match_data: Data for the match function
 *
 * Find the latest devres of @dev which has the same release function
 * as @new_res and for which @match return 1.  If found, @new_res is
 * freed; otherwise, @new_res is added atomically.
 *
 * RETURNS:
 * Pointer to found or added devres.
 */
/* devres_get - 查找设备资源，如果不存在，则自动添加一个
 * @dev:    用于查找 或 添加 devres 的设备
 * @new_res:指向一个新初始化的 devres 结构的data指针，
 *          如果没有找到，则该设备资源将被添加到设备资源链表中
 * @match:  匹配函数（可选）, 用于匹配 match_data 与 设备资源
 * @match_data: 用于匹配的数据
 *
 * 查找与 @new_res 具有相同 release 功能且 @match 返回 1 的 @dev 的最新版本。
 * 如果找到，则释放 @new_res；否则，以原子方式将 @new_res 添加进来。
 *
 * 这里是要添加一个，但有可能已经存在了一个相同的资源
 * 所以先创建一个 new_res, 再使用本函数获取，如果找到了，那就释放 new_res
 * 如果没有找到，就将 new_res 添加到设备资源里
 *
 * 在实际使用中，都是先使用 devres_find 查找一下
 * 如果没有找到，就 devres_alloc 分配一下，然后使用本函数添加
 * */
void * devres_get(struct device *dev, void *new_res,
		  dr_match_t match, void *match_data)
{
    /* 先找到新分配的 new_res 对应的 struct devres */
	struct devres *new_dr = container_of(new_res, struct devres, data);
	struct devres *dr;
	unsigned long flags;

    /* 设备资源链表操作 原子操作 */
	spin_lock_irqsave(&dev->devres_lock, flags);
    /* 在设备资源链表中，查找与 new_dr 的 release 相同的 资源结构
     * 当然，match 及 match_data 也需要匹配 */
	dr = find_dr(dev, new_dr->node.release, match, match_data);
	if (!dr) {
        /* 如果没有找到，那就把新分配的添加到设备资源链表中 */
		add_dr(dev, &new_dr->node);
        /* new_dr 就是唯一匹配的设备资源了，也是需要获取的设备资源 */
		dr = new_dr;
        /* 将 new_dr 指针清空，避免后面的 devres_free 释放了 */
		new_dr = NULL;
	}
	spin_unlock_irqrestore(&dev->devres_lock, flags);
    /* 无论前面的是否查找到了，都释放一下 new_dr
     * 如果没找到，new_dr == NULL, 释放操作无效
     * 如果找到了，那就把参数中新分配的 new_dr 释放掉 */
	devres_free(new_dr);

    /* 返回设备资源结构的资源指针 */
	return dr->data;
}
EXPORT_SYMBOL_GPL(devres_get);

/**
 * devres_remove - Find a device resource and remove it
 * @dev: Device to find resource from
 * @release: Look for resources associated with this release function
 * @match: Match function (optional)
 * @match_data: Data for the match function
 *
 * Find the latest devres of @dev associated with @release and for
 * which @match returns 1.  If @match is NULL, it's considered to
 * match all.  If found, the resource is removed atomically and
 * returned.
 *
 * RETURNS:
 * Pointer to removed devres on success, NULL if not found.
 */
/* devres_remove - 找到一个设备资源并将其移除
 * @dev:    用于查找资源的设备
 * @release:查找与此 release 相关的资源
 * @match:  匹配函数（可选）, 用于匹配 match_data 与 设备资源
 * @match_data: 用于匹配的数据
 * 
 * 找出与 @release 相关联且 @match 函数返回值为 1 的 @dev 的最新资源。如果
 * @match 为 NULL，则视为匹配所有资源。若找到相应资源，则以原子方式将其从
 * 设备资源链表中移除 并返回 资源指针 (dr->data)
 *
 * 本函数是查找匹配的资源，将其从设备链表中移除，并不会释放掉任何资源
 * */
void * devres_remove(struct device *dev, dr_release_t release,
		     dr_match_t match, void *match_data)
{
    /* dr 指向查找到的设备管理结构 */
	struct devres *dr;
	unsigned long flags;

    /* 设备资源链表操作 原子操作 */
	spin_lock_irqsave(&dev->devres_lock, flags);
    /* 在设备资源链表中，查找匹配的资源管理结构
     * 查找成功，返回的是 struct devres 结构指针 */
	dr = find_dr(dev, release, match, match_data);
	if (dr) {
        /* 将 dr 资源结构从 设备资源链表中移除 */
		list_del_init(&dr->node.entry);
        /* 输出 REM 日志 */
		devres_log(dev, &dr->node, "REM");
	}
	spin_unlock_irqrestore(&dev->devres_lock, flags);

    /* 查找成功了，返回设备资源结构的资源指针 */
	if (dr)
		return dr->data;
	return NULL;
}
EXPORT_SYMBOL_GPL(devres_remove);

/**
 * devres_destroy - Find a device resource and destroy it
 * @dev: Device to find resource from
 * @release: Look for resources associated with this release function
 * @match: Match function (optional)
 * @match_data: Data for the match function
 *
 * Find the latest devres of @dev associated with @release and for
 * which @match returns 1.  If @match is NULL, it's considered to
 * match all.  If found, the resource is removed atomically and freed.
 *
 * Note that the release function for the resource will not be called,
 * only the devres-allocated data will be freed.  The caller becomes
 * responsible for freeing any other data.
 *
 * RETURNS:
 * 0 if devres is found and freed, -ENOENT if not found.
 */
/* devres_destroy - 找到一个设备资源并将其销毁
 * @dev:    用于查找资源的设备
 * @release:查找与此 release 相关的资源
 * @match:  匹配函数（可选）, 用于匹配 match_data 与 设备资源
 * @match_data: 用于匹配的数据
 *
 * 找出与 @release 相关联且 @match 函数返回值为 1 的 @dev 的最新资源。如果
 * @match 为 NULL，则视为匹配所有资源。若找到相应资源，则会以原子方式将其删除并
 * 释放。
 *
 * 请注意，该资源的释放功能不会被调用，只有分配给开发资源的数据会被释放。调用者
 * 需自行负责释放任何其他数据。
 *
 * 本函数中，只会释放 struct devres 资源结构，并不会调用 release 释放被管理的资源
 * */
int devres_destroy(struct device *dev, dr_release_t release,
		   dr_match_t match, void *match_data)
{
	void *res;

    /* 从设备资源链表中查找匹配的资源管理结构，并将其从链表中移除
     * 这里只是把 设备资源管理结构 从设备资源链表中移除了
     * 并没有做其它的操作 */
	res = devres_remove(dev, release, match, match_data);
	if (unlikely(!res))
		return -ENOENT;

    /* 释放设备资源管理结构 struct devres */
	devres_free(res);
	return 0;
}
EXPORT_SYMBOL_GPL(devres_destroy);


/**
 * devres_release - Find a device resource and destroy it, calling release
 * @dev: Device to find resource from
 * @release: Look for resources associated with this release function
 * @match: Match function (optional)
 * @match_data: Data for the match function
 *
 * Find the latest devres of @dev associated with @release and for
 * which @match returns 1.  If @match is NULL, it's considered to
 * match all.  If found, the resource is removed atomically, the
 * release function called and the resource freed.
 *
 * RETURNS:
 * 0 if devres is found and freed, -ENOENT if not found.
 */
/* devres_destroy - 找到一个设备资源并将其销毁, 会调用资源释放回调
 * @dev:    用于查找资源的设备
 * @release:查找与此 release 相关的资源
 * @match:  匹配函数（可选）, 用于匹配 match_data 与 设备资源
 * @match_data: 用于匹配的数据
 *
 * 找出与 @release 相关联且 @match 函数返回值为 1 的 @dev 的最新资源。如果
 * @match 为 NULL，则视为匹配所有资源。若找到相应资源，则会以原子方式将其删除并
 * 释放, 当然会调用资源释放回调，将被管理的资源释放掉
 * */
int devres_release(struct device *dev, dr_release_t release,
		   dr_match_t match, void *match_data)
{
	void *res;

    /* 从设备资源链表中查找匹配的资源管理结构，并将其从链表中移除
     * 这里只是把 设备资源管理结构 从设备资源链表中移除了
     * 并没有做其它的操作 */
	res = devres_remove(dev, release, match, match_data);
	if (unlikely(!res))
		return -ENOENT;

    /* 调用资源释放 release 回调
     * 这是对被管理的资源的释放，如存放在 data 内存中的 GPIO号 */
	(*release)(dev, res);
    /* 释放设备资源管理结构 struct devres */
	devres_free(res);
	return 0;
}
EXPORT_SYMBOL_GPL(devres_release);

/* 移除设备资源链表中 从 first 到 end 之间的 资源节点
 * 需要注意，[first, end] 之间 可能存在其它的 devres_group
 * 也有可能只包含某些 devres_group 的部分节点
 * 所以，需要处理 跨组，包含组 的情况 */
static int remove_nodes(struct device *dev,
			struct list_head *first, struct list_head *end,
			struct list_head *todo)
{
    /* cnt 记录移除的节点数量
     * nr_groups 记录移除的 devres group 的数量 */
	int cnt = 0, nr_groups = 0;
	struct list_head *cur;

	/* First pass - move normal devres entries to @todo and clear
	 * devres_group colors.
	 */
    /* 第一步，将常规的 devres 项移动到 @todo 中 */
    /* 设置 cur 从 first 开始遍历
     * 如果遍历到 devres_group 的节点，清除其 color 值 */
	cur = first;
    /* 遍历所有 node， 直到遍历最后 end */
	while (cur != end) {
		struct devres_node *node;
		struct devres_group *grp;

        /* 通过 cur (list_head) 查找链表节点，即 devres_node 结构 */
		node = list_entry(cur, struct devres_node, entry);
        /* 下一个链表节点 */
		cur = cur->next;

        /* 通过 node 节点，反查所属的 devres group 结构
         * 当然，这里可能为 NULL, 即 node是常规 devres
         * 如果不是常规 devres, 则返回所属的 devres_group */
		grp = node_to_group(node);
		if (grp) {
            /* 如果是一个 devres_group */
			/* clear color of group markers in the first pass */
            /* 则清除该 devres_group 的 color == 0
             * color 是用来在检查 该group 是否完全在 [first, end] 之间的计数变量 */
			grp->color = 0;
            /* 记录遍历到的 devres_group 数量
             * 这里不需要精确的数量，只是标记遍历到 devres_group 了
             * [first, end] 之间有 group 需要进行第二步处理 */
			nr_groups++;
		} else {
			/* regular devres entry */
            /* 如果是一个常规 devres 节点 */
            /* 对于常规 devres 节点，这里检查了 是否为 first
             * 即检查 first 是否为常规节点
             * 如果是常规节点，则 first == 下一个节点 */
            /* 如果 first 是一个 devres_group 中的节点，
             * 那无论如何遍历，这里的条件都是不成立的
             * 所以 first 还是指向 devres_group 的节点
             *
             * 如果 first 传进来的时候是一个 常规 devres 节点
             * 那这里会成立，直到遍历到 devres_group 节点，
             * first 指向了第一个遍历到的 devres_group 节点 */
			if (&node->entry == first)
				first = first->next;
            /* 将常规 devres_node 移动到 todo链表中 */
			list_move_tail(&node->entry, todo);
            /* 递增常规 devres_node 节点数量 */
			cnt++;
		}
	}

    /* 遍历的 devres_group 中间没有一个嵌入到内部的其它 devres_group
     * 说明需要移除的这些节点都是普通节点 */
    /* 如果都是普通节点，则直接返回需要移除的数量 */
	if (!nr_groups)
		return cnt;

	/* Second pass - Scan groups and color them.  A group gets
	 * color value of two iff the group is wholly contained in
	 * [cur, end).  That is, for a closed group, both opening and
	 * closing markers should be in the range, while just the
	 * opening marker is enough for an open group.
	 */
    /* 第二步，对 groups 进行扫描并为其着色(color值).
     * 若一个组完全位于 [cur, end] 范围内，则该组的 color == 2.
     * 也就是说，对于一个属于该组的闭合的其它组，
     * 起始标记和结束标记都应该在该范围内。
     * 而对于一个开放的组，只要起始标记在该范围内即可 */
    /* 这次从 first 开始遍历，first 一定指向一个 devres_group 节点 */
	cur = first;
	while (cur != end) {
		struct devres_node *node;
		struct devres_group *grp;

        /* 通过 cur (list_head) 查找链表节点，即 devres_node 结构 */
		node = list_entry(cur, struct devres_node, entry);
        /* 下一个链表节点 */
		cur = cur->next;

        /* 通过 node 节点，反查所属的 devres_group 结构
         * 按照第一步的操作，常规 devres_node 都移动到 todo 链表中了
         * 所以，这里一定能查找所属的 devres_group 结构
         * 并且 其 node[0] 一定在 设备的 devres_head 链表中 */
		grp = node_to_group(node);
        /* 再判断一下，如果不满足预期，那就是 bug 了。。。 */
		BUG_ON(!grp || list_empty(&grp->node[0].entry));

        /* 递增一下遍历到的 devres_group 的 color 值
         * 如果该 devres_group 是第一次遍历到，那 color == 1
         * 当然，有可能是第二次遍历到，那就 == 2 */
		grp->color++;
        /* 检查 devres_group 的 node[1] 节点，判断是否为 空链表
         * 如果 node[1] 为 空，说明 devres_group 的 node[1] 
         * 没有被添加到 设备的 devres_head 链表中
         * 也就是说， 该 devres_group 没有被 Close
         * 是一个非闭合的 devres_group
         * 非闭合的组，后面也不会再次遍历到了，所以其 color 递增为 2 */
		if (list_empty(&grp->node[1].entry))
			grp->color++;

        /* 检查 devres_group 的 color，一定是 1 或 2 这两个数
         * 1 标识该 devres_group只被遍历到一次，闭合的
         * 2 标识该 devres_group被遍历到 node[0] 和 node[1] 了，
         *   或者遍历到非闭合的组了 */
		BUG_ON(grp->color <= 0 || grp->color > 2);
        /* 如果 devres_group 的 color == 2
         * 说明遍历到一个完全在 [first, end] 之间的组
         * 或者是一个非闭合的组
         * 此时需要把这个组移除掉 */
		if (grp->color == 2) {
			/* No need to update cur or end.  The removed
			 * nodes are always before both.
			 */
            /* 将 devres_group 的 node[0] 移动到 todo 链表中
             * 后面将遍历 todo 链表，进行节点释放 */
			list_move_tail(&grp->node[0].entry, todo);
            /* 移除 devres_group 的 node[1] 节点
             * node[1] 只是做为 group 封闭使用
             * 无 添加/移除 节点的作用 */
			list_del_init(&grp->node[1].entry);
		}
	}

    /* 返回值为 常规 devres node 的数量 */
	return cnt;
}

/* 释放所属 dev 的，在 [first, end] 之间的资源节点
 * 本函数将节点从 devres_head 中移除，并调用 release 回调
 * 被包含 [first, end] 中间的 devres_group 也会被释放掉 */
static int release_nodes(struct device *dev, struct list_head *first,
			 struct list_head *end, unsigned long flags)
	__releases(&dev->devres_lock)
{
    /* 链表头，todo 用来链接将要被释放的节点 */
	LIST_HEAD(todo);
	int cnt;
	struct devres *dr, *tmp;

    /* 从 dev 中移除 [first, end] 之间的资源节点
     * 满足条件的节点都被放在 todo 链表中
     * 返回值 cnt 是普通 devres_node 的数量 */
	cnt = remove_nodes(dev, first, end, &todo);

    /* 调用本函数时，一定是先进行了 spin_lock_irqsave 操作的
     * 所以，此处进行 spin_unlock_irqrestore 操作
     * 因为下面的释放操作是不需要加锁的 */
	spin_unlock_irqrestore(&dev->devres_lock, flags);

	/* Release.  Note that both devres and devres_group are
	 * handled as devres in the following loop.  This is safe.
	 */
    /* 释放资源。
     * 注意，在释放时， devres 和 devres_group 都会被视为 devres 处理。
     * 这是安全的。 */
    /* 这里把 devres_group 当作 devres 释放处理，在调用 kfree 时，
     * 其实依赖了 devres_group 结构体定义的形式
     * 这两个结构体中，都将 devres_node 放在了第一的位置
     * 并且 devres_group 中，加入到 devres_head 和 todo 链表中的
     * 都是以 node[0] 的，所以释放时可以采用同样的方法 */
    /* 遍历 todo 链表，所有需要释放的 node 都在该链表中
     * 注意，该遍历中，可能是常规 devres , 也可能是属于 devres_group 的 */
	list_for_each_entry_safe_reverse(dr, tmp, &todo, node.entry) {
        /* 记录 释放日志信息 */
		devres_log(dev, &dr->node, "REL");
        /* 调用 devres_node 的 release 回调函数
         * 用于释放被管理的资源 */
		dr->node.release(dev, dr->data);
        /* 释放 devres 或 devres_group 结构体内存 */
		kfree(dr);
	}

    /* 返回值为 常规 devres node 的数量 */
	return cnt;
}

/**
 * devres_release_all - Release all managed resources
 * @dev: Device to release resources for
 *
 * Release all resources associated with @dev.  This function is
 * called on driver detach.
 */
/* devres_release_all - 释放所有被管理的资源
 * @dev:    需要释放资源的设备
 *
 * 释放与 @dev 相关的所有资源。
 * 此函数在驱动程序卸载时被调用 */
int devres_release_all(struct device *dev)
{
	unsigned long flags;

	/* Looks like an uninitialized device structure */
    /* 检查设备的 devres_head 链表
     * 正确初始化的不会为 NULL */
	if (WARN_ON(dev->devres_head.next == NULL))
		return -ENODEV;
    /* 调用 release_nodes 之前都需要先对设备加锁 devres_lock  */
	spin_lock_irqsave(&dev->devres_lock, flags);
    /* 直接调用 release_nodes 进行处理
     * dev->devres_head.next 就是链表头的下一个元素
     * 也就是第一个 devres_node 元素
     * 结束是 dev->devres_head 也就是链表头
     * 这样就包含了所有的 devres_head 链表中的元素 */
	return release_nodes(dev, dev->devres_head.next, &dev->devres_head,
			     flags);
}

/**
 * devres_open_group - Open a new devres group
 * @dev: Device to open devres group for
 * @id: Separator ID
 * @gfp: Allocation flags
 *
 * Open a new devres group for @dev with @id.  For @id, using a
 * pointer to an object which won't be used for another group is
 * recommended.  If @id is NULL, address-wise unique ID is created.
 *
 * RETURNS:
 * ID of the new group, NULL on failure.
 */
/* devres_open_group - 创建一个新的 devres 组
 * @dev:    用于创建 devres 组的设备
 * @id:     分隔符 ID
 * @gfp:    分配 gfp 标志
 *
 * 为 @dev 创建一个 @id 新的 devres 组。
 * 对于 @id，建议使用指向一个不会用于其它 group 的对象的指针。
 * 如果 @id 为 NULL,则会生成一个使用 新分配的 devres_group 的内存地址的ID
 * */
void * devres_open_group(struct device *dev, void *id, gfp_t gfp)
{
    /* 设备的 devres组， struct devres_group */
	struct devres_group *grp;
	unsigned long flags;

    /* 分配内存用于 struct devres_group 结构 */
	grp = kmalloc(sizeof(*grp), gfp);
	if (unlikely(!grp))
		return NULL;

    /* 设置 node release 回调函数指针
     * 后续可用于区分 node */
	grp->node[0].release = &group_open_release;
	grp->node[1].release = &group_close_release;
    /* 初始化 node 链表节点 */
	INIT_LIST_HEAD(&grp->node[0].entry);
	INIT_LIST_HEAD(&grp->node[1].entry);
    /* 设置 devres node 调试信息
     * node[0] node[1] 设置不同的名字 */
	set_node_dbginfo(&grp->node[0], "grp<", 0);
	set_node_dbginfo(&grp->node[1], "grp>", 0);
    /* 先预设 id 为 新分配的结构体的内存地址
     * 如果参数指定了 id，再更新一下, 使用参数指定的 */
	grp->id = grp;
	if (id)
		grp->id = id;

	spin_lock_irqsave(&dev->devres_lock, flags);
    /* 将 devres group 的 node[0] 节点加入到 设备资源链表中
     * 这里只加入了 node[0]
     * node[0] 是 devres_group 组的开始 */
	add_dr(dev, &grp->node[0]);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
    /* 返回 devres_group 的 id 标识 */
	return grp->id;
}
EXPORT_SYMBOL_GPL(devres_open_group);

/* Find devres group with ID @id.  If @id is NULL, look for the latest. */
/* 查找 ID 为 @id 的 devres group. 如果 @id 为 NULL, 则查找最后添加的
 * 并且还是 open 状态的 group  */
static struct devres_group * find_group(struct device *dev, void *id)
{
	struct devres_node *node;

    /* 遍历设备的 devres_head 链表
     * 注意，本函数是查找 devres group , 只有 node[0] 在链表中 */
	list_for_each_entry_reverse(node, &dev->devres_head, entry) {
		struct devres_group *grp;

        /* devres group 的 node[0] 在链表中，对用 group_open_release 回调
         * 检查 release 回调，筛选 devres group 成员 */
		if (node->release != &group_open_release)
			continue;

        /* 通过 node[0] 反查 devres_group 结构体 */
		grp = container_of(node, struct devres_group, node[0]);

		if (id) {
            /* 如果参数指定了 @id, 则查找匹配的，并返回 devres_group 结构体指针 */
			if (grp->id == id)
				return grp;
		} else if (list_empty(&grp->node[1].entry))
            /* 未指定 @id 时，检查 node[1] 链表，为空链表则返回 */
            /* node[1] 链表为 空，标识此 devres_group 还未 close  */
			return grp;
	}

    /* 没有满足条件的，返回 NULL */
	return NULL;
}

/**
 * devres_close_group - Close a devres group
 * @dev: Device to close devres group for
 * @id: ID of target group, can be NULL
 *
 * Close the group identified by @id.  If @id is NULL, the latest open
 * group is selected.
 */
/* devres_close_group - 关闭一个 devres group
 * @dev: 要关闭的 devres_group 所属的设备
 * @id: 目标 group 的 ID, 可以为 NULL
 *
 * 关闭 ID 为 @id 的 devres_group, 如果 @id 为 NULL, 则关闭最后 open 的 group */
/* 这里是把 devres_group 关闭，即不能再向其中添加设备资源了
 * 没有对应的重新开放的 API */
void devres_close_group(struct device *dev, void *id)
{
    /* 设备的 devres组， struct devres_group */
	struct devres_group *grp;
	unsigned long flags;

	spin_lock_irqsave(&dev->devres_lock, flags);

    /* 根据参数 @id 查找 devres_group 结构体 */
    /* 如果 id == NULL，则返回的是最后添加的 open 状态的 devres_group */
	grp = find_group(dev, id);
	if (grp)
        /* 找到符合条件的，则将 node[1] 加入到设备资源链表中 */
        /* 将 node[1] 添加到 设备资源链表中 就将 devres_group 关闭了 */
		add_dr(dev, &grp->node[1]);
	else
		WARN_ON(1);

	spin_unlock_irqrestore(&dev->devres_lock, flags);
}
EXPORT_SYMBOL_GPL(devres_close_group);

/**
 * devres_remove_group - Remove a devres group
 * @dev: Device to remove group for
 * @id: ID of target group, can be NULL
 *
 * Remove the group identified by @id.  If @id is NULL, the latest
 * open group is selected.  Note that removing a group doesn't affect
 * any other resources.
 */
/* devres_remove_group - 移除一个 devres group
 * @dev:    待移除的 group 所属的设备
 * @id:     目标 group 的 ID, 可以为 NULL
 *
 * 移除 ID为 @id 的 devres_group, 如果 @id 为 NULL, 则移除最后添加的 
 * open 的 group. 
 * 注意，移除一个 group 并不会影响任何其它资源。 */
/* 移除的是一个 devres_group, 并不会影响已添加的设备资源节点 */
void devres_remove_group(struct device *dev, void *id)
{
    /* 设备的 devres组， struct devres_group */
	struct devres_group *grp;
	unsigned long flags;

	spin_lock_irqsave(&dev->devres_lock, flags);

    /* 根据参数 @id 查找 devres_group 结构体 */
	grp = find_group(dev, id);
	if (grp) {
        /* 将 node[0] node[1] 从 设备资源链表中 移除 */
		list_del_init(&grp->node[0].entry);
		list_del_init(&grp->node[1].entry);
		devres_log(dev, &grp->node[0], "REM");
	} else
		WARN_ON(1);

	spin_unlock_irqrestore(&dev->devres_lock, flags);

    /* 释放 devres_group 结构体内存 */
	kfree(grp);
}
EXPORT_SYMBOL_GPL(devres_remove_group);

/**
 * devres_release_group - Release resources in a devres group
 * @dev: Device to release group for
 * @id: ID of target group, can be NULL
 *
 * Release all resources in the group identified by @id.  If @id is
 * NULL, the latest open group is selected.  The selected group and
 * groups properly nested inside the selected group are removed.
 *
 * RETURNS:
 * The number of released non-group resources.
 */
/* devres_release_group - 释放 devres group 中的资源
 * @dev:    需要释放的 group 所属的设备
 * @id:     目标 group 的 ID, 可以为 NULL
 *
 * 释放 id 为 @id 的 group 的所有资源。
 * 如果 @id 为 NULL，则释放最后添加的 open状态 的 group 的资源。
 * 所选择的group以及位于所选group内部的其他组 都将被移除
 *
 * 返回值:
 *  已释放的 非组(非group) 资源的数量 */
int devres_release_group(struct device *dev, void *id)
{
    /* 设备的 devres组， struct devres_group */
	struct devres_group *grp;
	unsigned long flags;
	int cnt = 0;

	spin_lock_irqsave(&dev->devres_lock, flags);

    /* 根据参数 @id 查找 devres_group 结构体 */
	grp = find_group(dev, id);
	if (grp) {
        /* devres group 开始的地方是 node[0]
         * 所以 first 为 node[0] 的 entry
         * 默认的结尾 end 设置为 设备的 devres_head
         * 也就是从 node[0] 遍历到 链表头 devres_head
         * 这里的 end 是默认的，如果设置了 node[1], 会重新设置 */
		struct list_head *first = &grp->node[0].entry;
		struct list_head *end = &dev->devres_head;

        /* 检查 devres_group 的 node[1] 是否为 NULL
         * 就是检查 devres_group 是否 close 了
         * 如果 node[1] 不为 NULL
         * 则 这个 devres_group 就是从 node[0] 到 node[1] 之间
         * 所以这里 node[1] 不为 NULL, 则 end 设置为 node[1] entry */
		if (!list_empty(&grp->node[1].entry))
			end = grp->node[1].entry.next;

        /* 释放所属 dev 的，在 [first, end] 之间的资源节点 */
		cnt = release_nodes(dev, first, end, flags);
	} else {
        /* 未查找到匹配的 devres_group
         * 报告警告，并释放 devres_lock 锁 */
		WARN_ON(1);
		spin_unlock_irqrestore(&dev->devres_lock, flags);
	}

    /* 返回值为 常规 devres node 的数量 */
	return cnt;
}
EXPORT_SYMBOL_GPL(devres_release_group);

/*
 * Managed kzalloc/kfree
 */
/* devm_kzalloc / devm_kfree 资源释放函数
 * 用来分配内存的，并不会管理其它的资源，所以函数内部为空操作 */
static void devm_kzalloc_release(struct device *dev, void *res)
{
	/* noop */
}

/* devm_kfree 使用的资源匹配函数
 * 用来在释放资源管理结构时，匹配设备资源链表中的资源是否是查找的哪个
 * 本质就是判断提供的 待释放的内存指针 是否与 资源链表中的 相同
 * 这种匹配方式，能够避免调用释放函数时，提供的内存指针是错误的 */
static int devm_kzalloc_match(struct device *dev, void *res, void *data)
{
	return res == data;
}

/**
 * devm_kzalloc - Resource-managed kzalloc
 * @dev: Device to allocate memory for
 * @size: Allocation size
 * @gfp: Allocation gfp flags
 *
 * Managed kzalloc.  Memory allocated with this function is
 * automatically freed on driver detach.  Like all other devres
 * resources, guaranteed alignment is unsigned long long.
 *
 * RETURNS:
 * Pointer to allocated memory on success, NULL on failure.
 */
/* 资源管理型 kzalloc
 * @dev:    需要分配内存的设备
 * @size:   内存分配的大小
 * @gfp:    分配 gfp 标志
 *
 * 内部使用了 kzalloc 函数进行内存分配。通过此函数分配的内存会在
 * 驱动程序卸载时自动释放。与所有其它的 devres 资源一样，分配的
 * 内存保证具有无符号长整型的对齐方式。
 *
 * 返回值：
 * 成功分配返回内存指针，失败则返回 NULL 
 * */
void * devm_kzalloc(struct device *dev, size_t size, gfp_t gfp)
{
    /* 设备资源管理结构 */
	struct devres *dr;

	/* use raw alloc_dr for kmalloc caller tracing */
    /* 分配设备资源管理结构 struct devres,
     * 也会分配被管理资源的内存，由 dr->data 指向 */
	dr = alloc_dr(devm_kzalloc_release, size, gfp);
	if (unlikely(!dr))
		return NULL;

    /* 设置 devres_node 调试信息 */
	set_node_dbginfo(&dr->node, "devm_kzalloc_release", size);
    /* 注册设备资源 到 设备(加入到设备的 devres_head 链表中) */
	devres_add(dev, dr->data);
    /* 返回被管理的资源，即一块内存区域 */
	return dr->data;
}
EXPORT_SYMBOL_GPL(devm_kzalloc);

/**
 * devm_kfree - Resource-managed kfree
 * @dev: Device this memory belongs to
 * @p: Memory to free
 *
 * Free memory allocated with devm_kzalloc().
 */
/* 资源管理型 kfree
 * @dev:    此内存属于该设备
 * @p:  需要释放的内存
 *
 * 释放使用 devm_kzalloc() 函数分配的内存。
 * 
 * 注意，必须是使用 devm_kzalloc 分配的内存
 * */
void devm_kfree(struct device *dev, void *p)
{
	int rc;

    /* 在设备资源链表中查找匹配的 资源结构
     * 并将其从链表中移除，释放掉资源结构内存 struct devres
     * 注意：这里并不会调用资源释放 release 回调 */
    /* 这里有个疑问：
     * 参数中提供的 p 就是 struct devres 的 data 指针了
     * 为什么不直接反查 struct devres 结构，并将其从链表中移除，释放资源？？？
     * 而是要在设备资源链表中逐个查找呢？
     * 有点奇怪。。。。 
     * 这里是不是为了复用这些函数啊，不然会有重复的代码？？
     * 还有一个处理，devm_kzalloc_match 判断了 提供的 p 内存指针
     * 是否与 设备资源链表中 已有的设备资源 匹配，避免提供了错误的指针 */
	rc = devres_destroy(dev, devm_kzalloc_release, devm_kzalloc_match, p);
    /* 检测返回值，操作成功 返回 0
     * 如果返回值 rc 非 0，则失败 告警提示 */
	WARN_ON(rc);
}
EXPORT_SYMBOL_GPL(devm_kfree);
