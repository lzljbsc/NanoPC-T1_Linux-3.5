/*
 *  linux/drivers/base/map.c
 *
 * (C) Copyright Al Viro 2002,2003
 *	Released under GPL v2.
 *
 * NOTE: data structure needs to be changed.  It works, but for large dev_t
 * it will be too slow.  It is isolated, though, so these changes will be
 * local to that file.
 */

#include <linux/module.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/kdev_t.h>
#include <linux/kobject.h>
#include <linux/kobj_map.h>

/* kobj_map 数据结构
 * 基于哈希的管理结构，主要用于设备号管理
 * 通过设备号查找对应的 data(设备结构) ，在字符设备中有使用案例 */
struct kobj_map {
    /* struct probe 是一个指针数组
     * *data 是被管理的数据结构，对于字符设备，就是 cdev */
	struct probe {
		struct probe *next;
		dev_t dev;
		unsigned long range;
		struct module *owner;
		kobj_probe_t *get;
		int (*lock)(dev_t, void *);
		void *data;
	} *probes[255];
	struct mutex *lock;
};

int kobj_map(struct kobj_map *domain, dev_t dev, unsigned long range,
	     struct module *module, kobj_probe_t *probe,
	     int (*lock)(dev_t, void *), void *data)
{
    /* n 是给定的设备号/范围 中包含的设备号 跨越的主设备号数量 
     * 一般情况下，是不会跨越主设备号的，所以这里一般 n = 1 */
	unsigned n = MAJOR(dev + range - 1) - MAJOR(dev) + 1;
    /* index 是给定的主设备号，用于后面计算在数组中的索引 */
	unsigned index = MAJOR(dev);
	unsigned i;
	struct probe *p;

    /* 最多 255个数组元素 
     * 每个元素对应一个主设备号的哈希（主设备号 % 255）
     * 如果跨越了 255 个主设备号，那也只能按照 255个处理 */
	if (n > 255)
		n = 255;

    /* 申请 struct probe 管理结构，每个 主设备号 是单独的 */
	p = kmalloc(sizeof(struct probe) * n, GFP_KERNEL);

	if (p == NULL)
		return -ENOMEM;

    /* 所有的 struct probe 结构都初始化一样的
     * 就是传入的参数值，这里为什么都是一样？？ 
     * 为什么不按照跨越主设备号单独设置？？？ 
     * 这里的理解是，无论 主设备号是多少，都会映射到这 255 个数组中
     * 如果给出的一个 range 跨越了主设备号，那就是说明 
     * 所提供的 data 数据 适用于很多个主设备号 和 一个很大范围的次设备号 
     * 这就是为什么下面初始化 probe 结构时，设备号和范围都是参数中的值 
     * 而且也会有多个 probe 结构，这样只要给定一个在这个范围内的设备号，
     * 就可以找到对应的 probe 结构，进一步就能找到 data数据 
     *
     * 这里感觉是个优化点，对于这种情况，可以共用一个 probe 结构
     * 立即思考结果，不行！！！因为它们的下一个元素（next）不一样。。。。 */
	for (i = 0; i < n; i++, p++) {
		p->owner = module;
		p->get = probe;
		p->lock = lock;
		p->dev = dev;
		p->range = range;
		p->data = data;
	}
	mutex_lock(domain->lock);
    /* 遍历所有需要插入的 probe 结构，并且使用 index 计算数组索引
     * p -= n 是让 p 指向上面分配的内存最开始位置 
     * i < n 是要遍历的 probe 数量 
     * index 是第一个开始的主设备号，依次向后遍历 n 个*/
	for (i = 0, p -= n; i < n; i++, p++, index++) {
        /* 数组元素仍然是使用主设备号 哈希得到  */
		struct probe **s = &domain->probes[index % 255];
        /* 沿着链表依次向后遍历，需要找到比参数给定的 range 大的那个
         * 也就是说 probe 中是按照 range 升序排列的
         *
         * 这里有个隐含的点，就是在 kobj_map_init 初始化时，
         * 把 range 初始化为 ~0 0xFFFFFFFF ,最大值，那在 probe 链表中，
         * 最后一个元素一定是 kobj_map_init 中的默认 probe结构 
         * 也就确保了一定能找到比 range 大的那个元素 */
		while (*s && (*s)->range < range)
			s = &(*s)->next;
        /* 把新的 probe结果插入到链表中， range 升序排列 */
		p->next = *s;
		*s = p;
	}
	mutex_unlock(domain->lock);
	return 0;
}

void kobj_unmap(struct kobj_map *domain, dev_t dev, unsigned long range)
{
    /* n 和 index 与 kobj_map 中的含义一致 */
	unsigned n = MAJOR(dev + range - 1) - MAJOR(dev) + 1;
	unsigned index = MAJOR(dev);
	unsigned i;
	struct probe *found = NULL;

	if (n > 255)
		n = 255;

	mutex_lock(domain->lock);
    /* 给定的 dev 和 range 可能跨越多个主设备号，所以这里要每个主设备号都遍历 */
	for (i = 0; i < n; i++, index++) {
		struct probe **s;
        /* 遍历单个数组索引下所有 probe 结构，如果 dev 和 range 均一致
         * 则从链表中摘除, 并且在第一次找到的时候，记录该内存指针，用于后面进行释放 
         * 如果匹配到了，就 break 出来，遍历下一个数组索引 */
		for (s = &domain->probes[index % 255]; *s; s = &(*s)->next) {
			struct probe *p = *s;
			if (p->dev == dev && p->range == range) {
				*s = p->next;
				if (!found)
					found = p;
				break;
			}
		}
	}
	mutex_unlock(domain->lock);
    /* 释放找到的 probe 内存
     * 为什么可能有多个 probe结构，但只释放一次？？
     * 在 kobj_map 中， 需要多个 probe结构，也只申请了一次，
     * 并且内存最开始的那个 probe结构，是最早插入链表的，
     * 所以这里按照同样的方法找到的 probe结构，就是内存头 */
	kfree(found);
}

/* 给定一个具体的设备号，查找对应的数据结构 */
struct kobject *kobj_lookup(struct kobj_map *domain, dev_t dev, int *index)
{
	struct kobject *kobj;
	struct probe *p;
	unsigned long best = ~0UL;

retry:
	mutex_lock(domain->lock);
    /* 使用 主设备号 哈希后的值就是在数组中的索引 */
	for (p = domain->probes[MAJOR(dev) % 255]; p; p = p->next) {
		struct kobject *(*probe)(dev_t, int *, void *);
		struct module *owner;
		void *data;

        /* 给定的设备号不在遍历的 probe结构 的设备号范围内，则继续下一个 */
		if (p->dev > dev || p->dev + p->range - 1 < dev)
			continue;
        /* best 是个最大值，这里就是判断 range 是否为0，为0则无法使用 */
		if (p->range - 1 >= best)
			break;
        /* 到这里，不考虑 module_get 的话，
         * 说明要找的设备号就在这个 p 的 probe结构中 */
		if (!try_module_get(p->owner))
			continue;
        /* data 就是被管理的数据，对于字符设备，就是 cdev 
         * probe 就是在 kobj_map 时传入的回调函数 
         * best 是该结构中设备号范围
         * *index 是给定的设备号相对 probe 中（kobj_map 注册的）的起始设备号的偏
         * 移 */
		owner = p->owner;
		data = p->data;
		probe = p->get;
		best = p->range - 1;
		*index = dev - p->dev;
        /* 调用 kobj_map 时注册的 lock 函数，用于锁定 data数据结构 
         * 在字符设备中， lock回调函数用于进行 cdev_get 操作 */
		if (p->lock && p->lock(dev, data) < 0) {
			module_put(owner);
			continue;
		}
		mutex_unlock(domain->lock);
        /* 调用 kobj_map 时注册的 probe 函数，给定的参数是设备号偏移 和 data数据结构
         * probe 函数中要计算出 kobj结构，并做为返回值 */
		kobj = probe(dev, index, data);
		/* Currently ->owner protects _only_ ->probe() itself. */
		module_put(owner);
        /* 将 probe 回调函数中获得 kobj做为返回值 */
		if (kobj)
			return kobj;
		goto retry;
	}
	mutex_unlock(domain->lock);
	return NULL;
}

/* kobj_map 结构初始化
 * 分配一个 kobj_map 结构，并把 struct probe 数组赋值 */
struct kobj_map *kobj_map_init(kobj_probe_t *base_probe, struct mutex *lock)
{
	struct kobj_map *p = kmalloc(sizeof(struct kobj_map), GFP_KERNEL);
	struct probe *base = kzalloc(sizeof(*base), GFP_KERNEL);
	int i;

	if ((p == NULL) || (base == NULL)) {
		kfree(p);
		kfree(base);
		return NULL;
	}

    /* 默认的 struct probe 结构，所有的共用一个
     * dev 设备号只有 1， 对应主设备号为 0
     * range 是个无效值 0xFFFFFFFF 
     * */
	base->dev = 1;
	base->range = ~0;
	base->get = base_probe;
	for (i = 0; i < 255; i++)
		p->probes[i] = base;
	p->lock = lock;
	return p;
}
