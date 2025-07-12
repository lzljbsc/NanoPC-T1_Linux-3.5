/*
 * Copyright © 2003-2010 David Woodhouse <dwmw2@infradead.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifndef __MTD_TRANS_H__
#define __MTD_TRANS_H__

#include <linux/mutex.h>
#include <linux/kref.h>
#include <linux/sysfs.h>

struct hd_geometry;
struct mtd_info;
struct mtd_blktrans_ops;
struct file;
struct inode;

/* mtd_blktrans 模块结构
 * 管理注册到 mtd块转换层的所有 mtd设备
 * 包含了所有需要用到的信息 */
struct mtd_blktrans_dev {
    /* 操作函数结构，需要 mtdblock 设备实现 */
	struct mtd_blktrans_ops *tr;
    /* list 用来将所有的 mtd_blktrans_dev 链接到 mtd_blktrans_ops 的 devs 链表中 */
	struct list_head list;
    /* 指向注册的 mtd_info ，这样就对应上了
     * 总得知道管理的是哪个 mtd_info 吧，要不然怎么调用操作函数 */
	struct mtd_info *mtd;
	struct mutex lock;
    /* 注册为 mtdblock 设备时的序号，即设备节点名称序号 */
	int devnum;
    /* 后台处理流程结束控制标志 */
	bool bg_stop;
    /* 总容量大小，指扇区数量 */
	unsigned long size;
    /* 只读标志 */
	int readonly;
    /* 打开计数，用于打开时是否申请资源/关闭时是否释放资源 */
	int open;
    /* kref，引用计数，用于无任何引用时释放资源 */
	struct kref ref;
    /* 块设备数据结构，注册块设备的 */
	struct gendisk *disk;
    /* 属性文件 */
	struct attribute_group *disk_attributes;
    /* 处理线程句柄 */
	struct task_struct *thread;
    /* 数据请求队列指针，从中获得数据队列 */
	struct request_queue *rq;
    /* request_queue 锁，必须配合使用的 */
	spinlock_t queue_lock;
	void *priv;
    /* 文件打开模式 open 时传入的 */
	fmode_t file_mode;
};

/* 注册 mtd_blktrans_dev 时提供的结构
 * 主要是一些回调函数
 * 需要注册为 mtd_blktrans_dev 的，主要是实现这些回调
 * 用来将 mtd 虚拟为块设备，如 mtdblock.c ftl.c nftl.c */
struct mtd_blktrans_ops {
    /* 设备名，以该名字注册块设备，并形成设备节点前缀 */
	char *name;
    /* 块设备主设备号 */
	int major;
    /* 分区占用的 bit数，若 = 2， 则可以分4区 0 1 2 3 */
	int part_bits;
    /* 设备块大小，虚拟的概念，可以理解为扇区 */
	int blksize;
    /* 块大小偏移量，使用 blksize 计算的， = blksize
     *  tr->blkshift = ffs(tr->blksize) - 1;  */
	int blkshift;

	/* Access functions */
    /* 设备回调函数，在 mtd_blktrans.c 中，大部分都是直接调用回调了
     * 并没有太多的处理过程 */
	int (*readsect)(struct mtd_blktrans_dev *dev,
		    unsigned long block, char *buffer);
	int (*writesect)(struct mtd_blktrans_dev *dev,
		     unsigned long block, char *buffer);
	int (*discard)(struct mtd_blktrans_dev *dev,
		       unsigned long block, unsigned nr_blocks);
	void (*background)(struct mtd_blktrans_dev *dev);

	/* Block layer ioctls */
	int (*getgeo)(struct mtd_blktrans_dev *dev, struct hd_geometry *geo);
	int (*flush)(struct mtd_blktrans_dev *dev);

	/* Called with mtd_table_mutex held; no race with add/remove */
	int (*open)(struct mtd_blktrans_dev *dev);
	int (*release)(struct mtd_blktrans_dev *dev);

	/* Called on {de,}registration and on subsequent addition/removal
	   of devices, with mtd_table_mutex held. */
    /* 注册/移除设备时的回调 */
	void (*add_mtd)(struct mtd_blktrans_ops *tr, struct mtd_info *mtd);
	void (*remove_dev)(struct mtd_blktrans_dev *dev);

    /* devs 用来链接所有使用本 ops 的 mtd_blktrans_dev 结构 */
	struct list_head devs;
    /* list 用来把所有的 ops 链接到 blktrans_majors 链表中
     * 在新注册一个 mtd设备时，所有的 ops 都会调用到 */
	struct list_head list;
	struct module *owner;
};

extern int register_mtd_blktrans(struct mtd_blktrans_ops *tr);
extern int deregister_mtd_blktrans(struct mtd_blktrans_ops *tr);
extern int add_mtd_blktrans_dev(struct mtd_blktrans_dev *dev);
extern int del_mtd_blktrans_dev(struct mtd_blktrans_dev *dev);
extern int mtd_blktrans_cease_background(struct mtd_blktrans_dev *dev);


#endif /* __MTD_TRANS_H__ */
