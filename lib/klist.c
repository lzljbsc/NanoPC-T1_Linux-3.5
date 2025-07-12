/*
 * klist.c - Routines for manipulating klists.
 *
 * Copyright (C) 2005 Patrick Mochel
 *
 * This file is released under the GPL v2.
 *
 * This klist interface provides a couple of structures that wrap around
 * struct list_head to provide explicit list "head" (struct klist) and list
 * "node" (struct klist_node) objects. For struct klist, a spinlock is
 * included that protects access to the actual list itself. struct
 * klist_node provides a pointer to the klist that owns it and a kref
 * reference count that indicates the number of current users of that node
 * in the list.
 * 这个 klist接口提供了两个结构体，它们围绕 struct list_head 提供显示的
 * 列表头(struct klist) 和 列表节点 (struct klist_node) 对象。
 * 对于结构体 klist，它包含了一个自旋锁来保护对实际列表本身的访问。
 * struct klist_node 提供了一个指向拥有它的 klist 的指针和一个 kref 引用计数，
 * 该计数表示列表中该节点的当前用户数量。
 *
 * The entire point is to provide an interface for iterating over a list
 * that is safe and allows for modification of the list during the
 * iteration (e.g. insertion and removal), including modification of the
 * current node on the list.
 * 主要的功能是提供一个安全的遍历列表的接口，并允许在迭代过程中修改列表
 * （例如插入和删除），包括修改列表上的当前节点。
 *
 * It works using a 3rd object type - struct klist_iter - that is declared
 * and initialized before an iteration. klist_next() is used to acquire the
 * next element in the list. It returns NULL if there are no more items.
 * Internally, that routine takes the klist's lock, decrements the
 * reference count of the previous klist_node and increments the count of
 * the next klist_node. It then drops the lock and returns.
 * 它使用第三个对象类型 - struct klist_iter - 在迭代之前声明并初始化。
 * klist_next() 用于获取列表中的下一个元素。
 * 如果没有更多的项目，它返回NULL。
 * 在内部，这个方法获取 klist 的锁，减少前一个 klist_node 的引用计数，
 * 增加下一个 klist_node 的引用计数。
 * 然后释放锁并返回。
 *
 * There are primitives for adding and removing nodes to/from a klist.
 * When deleting, klist_del() will simply decrement the reference count.
 * Only when the count goes to 0 is the node removed from the list.
 * klist_remove() will try to delete the node from the list and block until
 * it is actually removed. This is useful for objects (like devices) that
 * have been removed from the system and must be freed (but must wait until
 * all accessors have finished).
 * 有一些原语用于向列表中添加和从列表中删除节点。
 * 删除时， klist_del() 只会减少引用计数。
 * 只有当计数变为0时，节点才会从列表中移除。
 * klist_remove() 将尝试从列表中删除节点，并阻塞直到它被实际删除。
 * 这对于已经从系统中删除并且必须释放（但必须等到所有访问者完成）的对象（如设备）
 * 非常有用。
 */

#include <linux/klist.h>
#include <linux/export.h>
#include <linux/sched.h>

/*
 * Use the lowest bit of n_klist to mark deleted nodes and exclude
 * dead ones from iteration.
 */
/* 使用 n_klist 的最低位标记是否已被删除 */
/* 注意，这里的删除是被请求删除，而非真正的将节点移除了
 * 删除标记的节点不会再被新的请求占用了，一直等待真正被移除 */
#define KNODE_DEAD		1LU
#define KNODE_KLIST_MASK	~KNODE_DEAD

/* 通过 knode节点，找到链表头 */
static struct klist *knode_klist(struct klist_node *knode)
{
    /* knode 的 n_klist 指针就是指向链表头的
     * 注意，需要地址对齐 */
	return (struct klist *)
		((unsigned long)knode->n_klist & KNODE_KLIST_MASK);
}

/* 检测 knode节点，是否已被请求删除 */
static bool knode_dead(struct klist_node *knode)
{
    /* 使用 n_klist 指针的最低位表示 */
	return (unsigned long)knode->n_klist & KNODE_DEAD;
}

/* 设置 knode节点的链表头 */
static void knode_set_klist(struct klist_node *knode, struct klist *klist)
{
    /* n_klist 指针指向链表头 */
	knode->n_klist = klist;
	/* no knode deserves to start its life dead */
    /* 任何节点都不应该一开始就死掉 */
	WARN_ON(knode_dead(knode));
}

/* 删除 konde节点 */
static void knode_kill(struct klist_node *knode)
{
	/* and no knode should die twice ever either, see we're very humane */
    /* 任何节点都不应该死两次，已经死过的，不应该再让死一次了。。。 */
	WARN_ON(knode_dead(knode));
    /* 设置 n_klist 指针的值，设置已被删除标志位 */
	*(unsigned long *)&knode->n_klist |= KNODE_DEAD;
}

/**
 * klist_init - Initialize a klist structure.
 * @k: The klist we're initializing.
 * @get: The get function for the embedding object (NULL if none)
 * @put: The put function for the embedding object (NULL if none)
 *
 * Initialises the klist structure.  If the klist_node structures are
 * going to be embedded in refcounted objects (necessary for safe
 * deletion) then the get/put arguments are used to initialise
 * functions that take and release references on the embedding
 * objects.
 */
/* klist 结构初始化
 * @k:  将要初始化的 klist
 * @get:嵌入对象的get函数
 * @put:嵌入对象的put函数
 * */
void klist_init(struct klist *k, void (*get)(struct klist_node *),
		void (*put)(struct klist_node *))
{
	INIT_LIST_HEAD(&k->k_list);
	spin_lock_init(&k->k_lock);
	k->get = get;
	k->put = put;
}
EXPORT_SYMBOL_GPL(klist_init);

/* 将 knode节点加入到 klist链表头 */
static void add_head(struct klist *k, struct klist_node *n)
{
	spin_lock(&k->k_lock);
	list_add(&n->n_node, &k->k_list);
	spin_unlock(&k->k_lock);
}

/* 将 knode节点加入到 klist链表尾 */
static void add_tail(struct klist *k, struct klist_node *n)
{
	spin_lock(&k->k_lock);
	list_add_tail(&n->n_node, &k->k_list);
	spin_unlock(&k->k_lock);
}

/* 初始化 knode节点
 * 注意，在节点的引用计数初始化时，因为引用计数变为1，
 * 所以也要调用相应的 get() 函数 */
static void klist_node_init(struct klist *k, struct klist_node *n)
{
    /* 初始化 knode 中的链表 n_node */
	INIT_LIST_HEAD(&n->n_node);
    /* 初始化 n_ref引用计数，已经变为1了 */
	kref_init(&n->n_ref);
    /* 设置 knode节点的链表头
     * 使用 knode 就可以找到所属的链表头了 */
	knode_set_klist(n, k);
    /* 如函数头注释，这里需要调用 get() 回调函数了 */
	if (k->get)
		k->get(n);
}

/**
 * klist_add_head - Initialize a klist_node and add it to front.
 * @n: node we're adding.
 * @k: klist it's going on.
 */
/* 将 knode节点初始化，并加入链表头 */
void klist_add_head(struct klist_node *n, struct klist *k)
{
	klist_node_init(k, n);
	add_head(k, n);
}
EXPORT_SYMBOL_GPL(klist_add_head);

/**
 * klist_add_tail - Initialize a klist_node and add it to back.
 * @n: node we're adding.
 * @k: klist it's going on.
 */
/* 将 knode节点初始化，并加入链表尾 */
void klist_add_tail(struct klist_node *n, struct klist *k)
{
	klist_node_init(k, n);
	add_tail(k, n);
}
EXPORT_SYMBOL_GPL(klist_add_tail);

/**
 * klist_add_after - Init a klist_node and add it after an existing node
 * @n: node we're adding.
 * @pos: node to put @n after
 */
/* 将一个 konde节点，添加到已经存在的指定节点后面 */
void klist_add_after(struct klist_node *n, struct klist_node *pos)
{
    /* 从已存在的节点中获取 klist，两个 knode 都属于这个 klist */
	struct klist *k = knode_klist(pos);

    /* 初始化需要新加入的 knode ，加入到 klist 中 */
	klist_node_init(k, n);
	spin_lock(&k->k_lock);
	list_add(&n->n_node, &pos->n_node);
	spin_unlock(&k->k_lock);
}
EXPORT_SYMBOL_GPL(klist_add_after);

/**
 * klist_add_before - Init a klist_node and add it before an existing node
 * @n: node we're adding.
 * @pos: node to put @n after
 */
/* 将一个 knode节点，添加到已经存在的指定节点前面 */
void klist_add_before(struct klist_node *n, struct klist_node *pos)
{
    /* 从已存在的节点中获取 klist，两个 knode 都属于这个 klist */
	struct klist *k = knode_klist(pos);

    /* 初始化需要新加入的 knode ，加入到 klist 中 */
	klist_node_init(k, n);
	spin_lock(&k->k_lock);
	list_add_tail(&n->n_node, &pos->n_node);
	spin_unlock(&k->k_lock);
}
EXPORT_SYMBOL_GPL(klist_add_before);

/* klist_waiter 结构作用
 * 有线程申请删除某节点，但节点的引用计数仍存在，所以只能把请求删除的线程阻塞，
 * 就是用 klist_waiter 阻塞在 klist_remove_waiters 上
 * 所以在 klist_release() 调用时，还要将阻塞的线程唤醒
 * knode_kill() 将节点设为已请求删除，而且还会调用 put() 函数 */
struct klist_waiter {
    /* 用于加入到 klist_remove_waiters 链表中 */
	struct list_head list;
    /* 将要删除的 knode 节点 */
	struct klist_node *node;
    /* 等待线程句柄 */
	struct task_struct *process;
    /* 是否唤醒等待线程
     * 就是个标志位，找到了需要移除的节点，就设置了这个标志
     * 表示 klist_remove 的请求已完成了 */
	int woken;
};

static DEFINE_SPINLOCK(klist_remove_lock);
static LIST_HEAD(klist_remove_waiters);

/* kref动态删除 release 回调函数
 * 当 kref 递减为零时，调用本函数 */
/* 原理是，需要移除某个 knode 节点时，所有请求的线程都被加入到
 * klist_remove_waiters等待队列中了，并且记录了需要移除的knode节点
 *
 * 当某个 knode 节点的引用计数已经递减到零了，就会调用本函数
 * 进入到本函数，就已经确认了需要移除的 knode节点
 * 那就把在 klist_remove_waiters 队列中的所有等待的 knode 都对比一下
 * 如果 knode 一致，那就说明对应的 线程请求了移除操作，那就唤醒线程，
 * 完成移除操作 */
static void klist_release(struct kref *kref)
{
	struct klist_waiter *waiter, *tmp;
    /* 通过 kref 节点，找到 所属的 klist_node 结构 */
	struct klist_node *n = container_of(kref, struct klist_node, n_ref);

    /* 检查 knode 是否已被标记为请求删除
     * 未标记为请求删除的进行告警 */
	WARN_ON(!knode_dead(n));
    /* 将 knode 从 klist 的链表中摘除 */
	list_del(&n->n_node);
	spin_lock(&klist_remove_lock);
    /* 遍历 klist_remove_waiters 链表中的所有 klist_waiter 成员
     * 只要是等待移除 knode 的，都在这个链表中 */
	list_for_each_entry_safe(waiter, tmp, &klist_remove_waiters, list) {
        /* 根据 knode 节点，判断 waiter 是否请求移除这个 knode节点了 */
		if (waiter->node != n)
			continue;

        /* 设置唤醒标志位，woken=1 等待线程才会退出 */
		waiter->woken = 1;
        /* 内存屏障，确保内存访问不会出错的，主要是 woken 变量不会访问旧数据 */
		mb();
        /* 唤醒 等待线程
         * 注意，这里只是将等待线程设置为可运行状态
         * 本函数并不会触发调度动作 */
		wake_up_process(waiter->process);
        /* 将 waiter 等待线程，从 klist_remove_waiters 等待队列中移除 */
        /* 补充一下，这个从链表中移除的操作，应该刚在 waiter->woken = 1; 前后
         * 至少要在上面的 wake_up_process 操作前面
         * 因为通过 wake_up_process 唤醒了线程，无法确定切换线程的实际
         * 而在 klist_remove 函数中， waiter 是一个局部变量，如果先切换了线程
         * 那这里的 waite 就是野指针了
         * 这个问题在 后面的版本中已经改了，比如 5.4.2 版本中 */
		list_del(&waiter->list);
	}
	spin_unlock(&klist_remove_lock);
    /* 将 knode 从 klist 中移除 
     * 上面的 list_del(&n->n_node) 已经从klist链表中移除了
     * 这里是把 knode 中的 n_klist 指向为 NULL */
	knode_set_klist(n, NULL);
}

/* 对 kref_put 的封装
 * 减少节点引用计数，并在引用计数为零时调用 klist_release */
static int klist_dec_and_del(struct klist_node *n)
{
    /* 递减 klist_node 的 n_ref 引用计数 */
    /* 注意这里的返回值，如果 kref_put 返回了 1，表示 release 已经被调用了
     * 如果返回了 0，表示 引用计数并没有到 0，没有调用 release */
	return kref_put(&n->n_ref, klist_release);
}

/* klist_node 释放处理
 * 参数 kill 指示是否请求删除 */
static void klist_put(struct klist_node *n, bool kill)
{
    /* 通过 klist_node 找到所属的 klist */
	struct klist *k = knode_klist(n);
    /* klist 结构中的 put 回调 */
	void (*put)(struct klist_node *) = k->put;

	spin_lock(&k->k_lock);
    /* 指示请求删除，则将 klist_node 标记为已请求删除 */
	if (kill)
		knode_kill(n);
    /* 递减 knode 引用计数，确认是否已调用 release 回调
     * 如果没有调用 release，这里就不会调用 put 函数 */
	if (!klist_dec_and_del(n))
		put = NULL;
	spin_unlock(&k->k_lock);
	if (put)
		put(n);
}

/**
 * klist_del - Decrement the reference count of node and try to remove.
 * @n: node we're deleting.
 */
/* 释放引用计数，并尝试将其进行移除
 * 全部操作都是由 klist_put 完成 */
/* klist_del 只是尝试移除 klist_node ，并不会等待实际操作完成 */
void klist_del(struct klist_node *n)
{
	klist_put(n, true);
}
EXPORT_SYMBOL_GPL(klist_del);

/**
 * klist_remove - Decrement the refcount of node and wait for it to go away.
 * @n: node we're removing.
 */
/* 释放引用计数，并尝试将其进行移除
 * 调用 klist_del 进行移除操作请求
 * klist_remove 函数请求移除，并加入到等待链表中，等待真正移除完成 */
void klist_remove(struct klist_node *n)
{
	struct klist_waiter waiter;

    /* 构建 klist_waiter 结构
     * 就是使用这个结构加入到移除等待队列中 */
	waiter.node = n;
	waiter.process = current;
	waiter.woken = 0;
	spin_lock(&klist_remove_lock);
    /* 加入到 klist_remove_waiters 等待队列中 */
	list_add(&waiter.list, &klist_remove_waiters);
	spin_unlock(&klist_remove_lock);

    /* 请求删除 knode 节点 */
	klist_del(n);

	for (;;) {
        /* 设置线程模式为不可中断的睡眠状态，等待被唤醒 */
		set_current_state(TASK_UNINTERRUPTIBLE);
        /* woken = 1 已经确认被移除了，退出等待状态 */
		if (waiter.woken)
			break;
		schedule();
	}
    /* 设置为可运行状态，移除请求已完成 */
	__set_current_state(TASK_RUNNING);
}
EXPORT_SYMBOL_GPL(klist_remove);

/**
 * klist_node_attached - Say whether a node is bound to a list or not.
 * @n: Node that we're testing.
 */
/* 检查 knode 节点是否被包含在某个 klist 链表中 */
int klist_node_attached(struct klist_node *n)
{
	return (n->n_klist != NULL);
}
EXPORT_SYMBOL_GPL(klist_node_attached);

/**
 * klist_iter_init_node - Initialize a klist_iter structure.
 * @k: klist we're iterating.
 * @i: klist_iter we're filling.
 * @n: node to start with.
 *
 * Similar to klist_iter_init(), but starts the action off with @n,
 * instead of with the list head.
 */
/* 初始化一个 klist_iter 结构
 * @k: 需要迭代的 klist
 * @i: 需要填充的 klist_iter
 * @n: 要开始的节点
 *
 * 与 klist_iter_init() 类似，但以 @n 节点开始，
 * 而不是以列表头开始 */
/* 注意，这里指定了有效的节点，则可以直接访问该节点 */
void klist_iter_init_node(struct klist *k, struct klist_iter *i,
			  struct klist_node *n)
{
	i->i_klist = k;
	i->i_cur = n;
    /* 从某个节点开始，会直接增加节点的 kref，可以直接对节点进行访问 */
	if (n)
		kref_get(&n->n_ref);
}
EXPORT_SYMBOL_GPL(klist_iter_init_node);

/**
 * klist_iter_init - Iniitalize a klist_iter structure.
 * @k: klist we're iterating.
 * @i: klist_iter structure we're filling.
 *
 * Similar to klist_iter_init_node(), but start with the list head.
 */
/* 初始化一个 klist_iter 结构
 * @k: 需要迭代的 klist
 * @i: 需要填充的 klist_iter
 *
 * 与 klist_iter_init_node() 类似，但这个是从链表头开始的 */
/* 注意， klist_iter_init 因为没有指定有效的节点，
 * 所以必须调用 klist_next() 访问下一个节点 */
void klist_iter_init(struct klist *k, struct klist_iter *i)
{
    /* 指定节点设置为 NULL 即可 */
	klist_iter_init_node(k, i, NULL);
}
EXPORT_SYMBOL_GPL(klist_iter_init);

/**
 * klist_iter_exit - Finish a list iteration.
 * @i: Iterator structure.
 *
 * Must be called when done iterating over list, as it decrements the
 * refcount of the current node. Necessary in case iteration exited before
 * the end of the list was reached, and always good form.
 */
/* 结束一个 klist 迭代器
 * 不再使用迭代器时，需要调用本函数结束 */
/* 在遍历列表结束时必须调用此方法，它会减少当前节点的引用计数。
 * 如果在到达列表末尾之前退出了迭代，这是必要的，而且始终是个良好的习惯 */
void klist_iter_exit(struct klist_iter *i)
{
    /* 具有有效的 knode节点，则需要释放一下
     * 因为在 klist_next 遍历时，会被递增一下 */
	if (i->i_cur) {
		klist_put(i->i_cur, false);
		i->i_cur = NULL;
	}
}
EXPORT_SYMBOL_GPL(klist_iter_exit);

/* 通过 list_head 结构，找到包含它的 klist_node 结构 */
static struct klist_node *to_klist_node(struct list_head *n)
{
    /* klist_node 结构中， list_head 节点为 n_node */
	return container_of(n, struct klist_node, n_node);
}

/**
 * klist_next - Ante up next node in list.
 * @i: Iterator structure.
 *
 * First grab list lock. Decrement the reference count of the previous
 * node, if there was one. Grab the next node, increment its reference
 * count, drop the lock, and return that next node.
 */
/* 在 klist 链表中找到下一个节点
 * */
struct klist_node *klist_next(struct klist_iter *i)
{
    /* 获取迭代的klist链表中的 put 回调函数 */
	void (*put)(struct klist_node *) = i->i_klist->put;
    /* 当前已迭代的节点
     * 这个节点可能是 NULL  或  指定的开始节点
     * 也可能是之前已迭代的某个节点 */
	struct klist_node *last = i->i_cur;
	struct klist_node *next;

    /* 需要操作 klist 链表时，必须要加锁 */
	spin_lock(&i->i_klist->k_lock);

	if (last) {
        /* 当存在有效的上一个 knode节点时
         * 这里需要对上一个迭代的 knode节点进行引用计数递减 */
        /* 这里先通过上一个迭代的 knode节点，找下一个节点（next就是指向下一个节
         * 点的） */
		next = to_klist_node(last->n_node.next);
        /* 存在有效的迭代节点，则递减引用计数
         * 如果并没有实际调用 release回调，则不应该调用 put 函数 */
		if (!klist_dec_and_del(last))
			put = NULL;
	} else
        /* 没有有效的迭代节点，则通过 klist 链表找下一个
         * 这种情况发生在从链表头开始遍历时
         * 没有指定有效的迭代起始节点，所以从 klist 的链表头开始 */
		next = to_klist_node(i->i_klist->k_list.next);

    /* 清空当前迭代的节点变量
     * 是为了在没有遍历到任何节点时返回 NULL
     * 或者遍历完成的时候返回 NULL */
	i->i_cur = NULL;
    /* 检查是否已遍历到链表头
     * i->i_klist->k_list 就是需要迭代的 klist 的链表头 
     * 这里判断了 klist 的 knode 是否与 next 一致
     * 那这里有个隐含的情形，就是无论从哪个节点开始遍历
     * 都是遍历到链表头，从链表头到起始的节点无法遍历 */
	while (next != to_klist_node(&i->i_klist->k_list)) {
        /* 检查遍历的节点是否已被请求删除，请求删除的节点要跳过 */
		if (likely(!knode_dead(next))) {
            /* 节点是正常状态
             * 递增节点的引用计数
             * 记录这个节点是当前被迭代到的节点 */
			kref_get(&next->n_ref);
			i->i_cur = next;
			break;
		}
        /* 如果刚才的节点已被请求删除了，那就不可以用了
         * 那就找刚才的节点的下一个节点 */
		next = to_klist_node(next->n_node.next);
	}

	spin_unlock(&i->i_klist->k_lock);

    /* 存在有效的上一个迭代节点，并且上一个有效节点引用计数已被递减到零
     * 这时就需要调用 put 回调了 */
	if (put && last)
		put(last);
    /* 返回最新迭代的节点
     * 注意，这里有可能是个有效的节点，也可能 NULL
     * 是 NULL 就表示已经遍历完了 */
	return i->i_cur;
}
EXPORT_SYMBOL_GPL(klist_next);
