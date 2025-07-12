/*
 *	klist.h - Some generic list helpers, extending struct list_head a bit.
 *
 *	Implementations are found in lib/klist.c
 *
 *
 *	Copyright (C) 2005 Patrick Mochel
 *
 *	This file is rleased under the GPL v2.
 */

#ifndef _LINUX_KLIST_H
#define _LINUX_KLIST_H

#include <linux/spinlock.h>
#include <linux/kref.h>
#include <linux/list.h>

/* klist 专门为了设备驱动而设计
 * 假设一些设备被链接在设备链表中，一个线程命令卸载某设备，即将其从设备链表中删
 * 除，但这时该设备正在使用中，这就出现了冲突。
 * 当然可以设置临界区并加锁，但因为使用一个设备而锁住整个设备链表显然是不对的；
 * 又或者可以从设备本身做文章，让线程阻塞，当然也是可以的。
 * 
 * klist 实现了这个功能，给每个节点一个引用计数，当引用计数为零时进行删除
 * klist 把 kref 直接保存在了链表节点上。当有线程要求删除设备时，之前的使用仍存
 * 在，所以不能实际删除，但不应该有新的应用访问到该设备了。
 *
 * klist 中提供了引用计数，并提供了 KNODE_DEAD 标志，用于标记该系欸但是否已被请
 * 求删除，被请求删除的设备不会再次有新的访问了 */

struct klist_node;
/* 注意： struct klist 结构体是以 sizeof(void *)字节对齐
 * 假设是4字节对齐的话，说明 klist链表的实例地址的最低位是0
 * 所以可以利用最低位存储其它信息，但是取 klist链表的实例地址时
 * 需要把最低位置为 0 */
struct klist {
    /* 链表节点操作所需要的自旋锁 */
	spinlock_t		k_lock;
    /* 嵌入的双向链表 list  */
	struct list_head	k_list;
    /* 用于链表内的节点增加引用计数 */
    /* 这个是更上层的结构提供的，通过 klist_node 找到上层结构
     * 并增加引用计数 */
	void			(*get)(struct klist_node *);
    /* 用于链表内的节点减少引用计数 */
	void			(*put)(struct klist_node *);
} __attribute__ ((aligned (sizeof(void *))));

/* 初始化一个 klist 链表
 * 适用于一个预先定义的 klist结构 的初始化 */
#define KLIST_INIT(_name, _get, _put)					\
	{ .k_lock	= __SPIN_LOCK_UNLOCKED(_name.k_lock),		\
	  .k_list	= LIST_HEAD_INIT(_name.k_list),			\
	  .get		= _get,						\
	  .put		= _put, }

/* 新定义并初始化一个 klist 结构 */
#define DEFINE_KLIST(_name, _get, _put)					\
	struct klist _name = KLIST_INIT(_name, _get, _put)

/* klist 初始化操作，提供 get put 回调函数 */
extern void klist_init(struct klist *k, void (*get)(struct klist_node *),
		       void (*put)(struct klist_node *));

/* 注意， n_klist 指针是用来指向链表头的，
 * 它的最低位用来表示该节点是否已被请求删除(KNODE_DEAD)
 * 如果已经被请求删除的话，在 klist 链表中遍历是看不到该节点的 */
struct klist_node {
    /* 用于指向 klist 链表头 */
	void			*n_klist;	/* never access directly */
    /* 嵌入的双向链表 list */
	struct list_head	n_node;
    /* klist 链表节点的引用计数器 */
	struct kref		n_ref;
};

/* klist 链表操作
 * 基础操作，与遍历 klist 节点无关 */
extern void klist_add_tail(struct klist_node *n, struct klist *k);
extern void klist_add_head(struct klist_node *n, struct klist *k);
extern void klist_add_after(struct klist_node *n, struct klist_node *pos);
extern void klist_add_before(struct klist_node *n, struct klist_node *pos);

extern void klist_del(struct klist_node *n);
extern void klist_remove(struct klist_node *n);

extern int klist_node_attached(struct klist_node *n);


/* klist 迭代器结构
 * 在遍历 klist 链表时使用 */
struct klist_iter {
    /* 需要遍历的 klist 链表 */
	struct klist		*i_klist;
    /* 当前处理的 klist_node 节点 */
	struct klist_node	*i_cur;
};


/* klist 迭代器操作
 * 初始化迭代器，退出迭代，遍历下一个节点 */
extern void klist_iter_init(struct klist *k, struct klist_iter *i);
extern void klist_iter_init_node(struct klist *k, struct klist_iter *i,
				 struct klist_node *n);
extern void klist_iter_exit(struct klist_iter *i);
extern struct klist_node *klist_next(struct klist_iter *i);

#endif
