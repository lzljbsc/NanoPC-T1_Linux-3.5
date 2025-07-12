/*
 * Interface to Linux block layer for MTD 'translation layers'.
 *
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

#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/fs.h>
#include <linux/mtd/blktrans.h>
#include <linux/mtd/mtd.h>
#include <linux/blkdev.h>
#include <linux/blkpg.h>
#include <linux/spinlock.h>
#include <linux/hdreg.h>
#include <linux/init.h>
#include <linux/mutex.h>
#include <linux/kthread.h>
#include <asm/uaccess.h>

#include "mtdcore.h"

/* blktrans_majors 用于连接所有的 mtd_blktrans_ops 结构
 * 在每次注册 mtd 字符设备时，通过 mtd_notifier 机制调用 */
static LIST_HEAD(blktrans_majors);
static DEFINE_MUTEX(blktrans_ref_mutex);

/* mtd_blktrans_dev 释放函数
 * 在注销 mtdblock 设备时会调用到
 * 用于清理结构体 */
static void blktrans_dev_release(struct kref *kref)
{
	struct mtd_blktrans_dev *dev =
		container_of(kref, struct mtd_blktrans_dev, ref);

    /* 磁盘结构 private_data 原本指向 mtd_blktrans_dev  */
	dev->disk->private_data = NULL;
    /* 清理请求队列 */
	blk_cleanup_queue(dev->rq);
    /* 这里会释放 gendisk 磁盘结构 */
	put_disk(dev->disk);
    /* 从链表中移除自身 这里是将 mtd_blktrans_dev 结构 
     * 从 mtd_blktrans_ops 结构中的 devs 中移除
     * devs 结构链接了所有使用同一个 ops 的 mtd_blktrans_dev */
	list_del(&dev->list);
    /* 释放 mtd_blktrans_dev 结构 */
	kfree(dev);
}

/* 从 磁盘结构 gendisk 获取 mtd_blktrans_dev 结构
 * 这里重点是要更新引用计数 dev->ref */
static struct mtd_blktrans_dev *blktrans_dev_get(struct gendisk *disk)
{
	struct mtd_blktrans_dev *dev;

	mutex_lock(&blktrans_ref_mutex);
    /* 在 add_mtd_blktrans_dev 中，将 mtd_blktrans_dev 结构
     * 赋值给了 gendisk 的 private_data */
	dev = disk->private_data;

	if (!dev)
		goto unlock;
    /* 重点这个引用计数，这会在释放/注销 mtd_blktrans_dev 时
     * 用于判断是否能够真的将其全部释放 */
	kref_get(&dev->ref);
unlock:
	mutex_unlock(&blktrans_ref_mutex);
	return dev;
}

/* 释放 mtd_blktrans_dev
 * 注意，这里并不一定是真的就调用了 release 函数
 * 只有在 dev->ref 引用计数到 1时才会调用 */
static void blktrans_dev_put(struct mtd_blktrans_dev *dev)
{
	mutex_lock(&blktrans_ref_mutex);
    /* 当 dev->ref 递减到 1 时，会调用 blktrans_dev_release  */
	kref_put(&dev->ref, blktrans_dev_release);
	mutex_unlock(&blktrans_ref_mutex);
}


/* mtd blkdevs 实际处理读写请求过程
 * 该函数被 mtd_blktrans_thread 调用
 * 当有一个请求时，则调用到本函数 */
static int do_blktrans_request(struct mtd_blktrans_ops *tr,
			       struct mtd_blktrans_dev *dev,
			       struct request *req)
{
	unsigned long block, nsect;
	char *buf;

    /* 请求的块设备的操作，是按照扇区操作的
     * blk_rq_pos 给出了扇区地址, 扇区按照 512字节
     * 所以这里转化为块序号 <<9 >> tr->blkshift
     * 无论物理设备的扇区大小是多少，内核和设备驱动的扇区都是 512字节
     *
     * 提供的写入数量按照字节提供，再转化为块的数量
     * 注意，这里的块并非 flash芯片中的块，而是 mtdblock 设备中定义的块
     * 这个块的大小 blkshift 与 mtd_blktrans_ops 中的 blksize 有关
     * 这里的 blkshift 在 add_mtd_blktrans_dev 中被计算出来 */
	block = blk_rq_pos(req) << 9 >> tr->blkshift;
	nsect = blk_rq_cur_bytes(req) >> tr->blkshift;

    /* 待写入数据的缓冲区 */
	buf = req->buffer;

    /* 内核文件系统请求，mtdblock 只支持这种请求 */
	if (req->cmd_type != REQ_TYPE_FS)
		return -EIO;

    /* 请求的扇区不能超过容量限制（容量以扇区表示） */
	if (blk_rq_pos(req) + blk_rq_cur_sectors(req) >
	    get_capacity(req->rq_disk))
		return -EIO;

    /* 数据丢弃请求，用于块设备的性能优化
     * mtdblock 中并未提供 discard 回调，不支持该请求 */
	if (req->cmd_flags & REQ_DISCARD)
		return tr->discard(dev, block, nsect);

    /* 数据请求方向，只支持读写两种请求
     * readsect 和 writesect 是 mtdblock.c 中的 */
	switch(rq_data_dir(req)) {
	case READ:
        /* 读请求，按照扇区数量写入
         * 这里也是模拟了块设备写入，每次只读入一个 mtdblock 块 blksize */
		for (; nsect > 0; nsect--, block++, buf += tr->blksize)
			if (tr->readsect(dev, block, buf))
				return -EIO;
		rq_flush_dcache_pages(req);
		return 0;
	case WRITE:
        /* 无写入回调，返回失败 */
		if (!tr->writesect)
			return -EIO;

		rq_flush_dcache_pages(req);
        /* 与 READ 一样，每次写入一个 mtdblock 块 blksize */
		for (; nsect > 0; nsect--, block++, buf += tr->blksize)
			if (tr->writesect(dev, block, buf))
				return -EIO;
		return 0;
	default:
		printk(KERN_NOTICE "Unknown request %u\n", rq_data_dir(req));
		return -EIO;
	}
}

/* 后台处理流程 是否需要退出的判断
 * 两种情况下需要退出后台处理流程
 * 1. 有请求结束本线程 kthread_stop
 * 2. 设置了 bg_stop 标志
 * 后台处理 可以参考 mtdswap.c 流程， mtdswap_background 函数 */
int mtd_blktrans_cease_background(struct mtd_blktrans_dev *dev)
{
    /* 判断是否应当结束线程
     * 由其它线程调用 kthread_stop 发起 */
	if (kthread_should_stop())
		return 1;

    /* bg_stop 标志在 mtd_blktrans_request 中设置 */
	return dev->bg_stop;
}
EXPORT_SYMBOL_GPL(mtd_blktrans_cease_background);

/* mtd blktrans 处理线程
 * 每注册一个 mtd_blktrans_dev 就会创建一个
 * 该线程用来等待请求队列，唤醒该线程进行处理
 * 也可以提供一个后台处理回调 background
 * 在无请求队列时执行 */
static int mtd_blktrans_thread(void *arg)
{
    /* 创建线程时，指定的参数就是 mtd_blktrans_dev 结构 */
	struct mtd_blktrans_dev *dev = arg;
    /* dev->tr 指向了注册的 mtd_blktrans_ops ，回调函数 */
	struct mtd_blktrans_ops *tr = dev->tr;
    /* 请求队列 */
	struct request_queue *rq = dev->rq;
    /* 这是一个具体的请求 */
	struct request *req = NULL;
    /* background_done 的值 与 bg_stop 相关
     * 同时，只有进入到后台处理流程中时，才可能设置 background_done 的值
     * 按照下面的流程，假设进入到 background 后台流程函数中了
     * 那 background 返回有三种情况
     * 1. bg_stop 被设置为 1 
     * 2. 其它线程调用了 kthread_stop ，主动停止
     * 3. background 函数主动返回了
     * 第一种情况， bg_stop = 1, 第二、三种情况 bg_stop = 0
     *
     * 那分 bg_stop = 0 和 bg_stop = 1 两种情况分析
     *
     * bg_stop = 0 时，
     * 按照下面代码 background_done = !dev->bg_stop 
     * 那 background_done = 1, 并且 后面的代码为 continue 
     * 所以，会进入到下一次的 while 循环
     * 如果是第二种情况，在 while (!kthread_should_stop) 时会主动退出，不再循环
     * 如果是第三种情况，那在没有新的请求队列（req = NULL）时，也不会再处理
     * background 了，因为 background_done = 1, 只有在处理了请求后，在设置了 
     * background_done = 0 时，才会在没有请求队列时再次进入后台流程
     * 个人分析，这种设置还挺好，就是当没有任何数据读写时，不处理后台流程，一直
     * 等待有新的数据请求，当有新的数据请求时，再后台处理； 这样可以节省CPU资源
     *
     * bg_stop = 1 时，
     * 在目前的代码中，只有在 mtd_blktrans_request 中才把 bg_stop 设置为 1
     * 同时也是有数据请求的标志, 按照下面流程，background_done 会被设置为 0
     * 并且进入到下一次的 while 循环
     * 如果没有数据请求队列，那还是会进入到 background 流程中
     * 如果有数据队列请求，那将会处理队列请求，并在处理之后重新设置 
     * background_done = 0, 以便在没有数据队列请求时，重新进入 后台流程 */
	int background_done = 0;

	spin_lock_irq(rq->queue_lock);

    /* 只在 kthread_stop 时退出
     * 本代码中，只有 del_mtd_blktrans_dev 中调用 */
	while (!kthread_should_stop()) {
		int res;

        /* bg_stop 默认值，不影响后台处理流程  */
		dev->bg_stop = false;
        /* 获取数据请求队列，req 指向新的请求队列
         * 调用 blk_fetch_request 时，必须持有 rq->queue_lock 锁 */
		if (!req && !(req = blk_fetch_request(rq))) {
            /* 没有新的数据请求，并且提供了 background 后台函数
             * 那就进入后台处理中 background_done 作用见上方分析 */
			if (tr->background && !background_done) {
				spin_unlock_irq(rq->queue_lock);
				mutex_lock(&dev->lock);
                /* 后台处理回调，参考 mtdswap.c 中相关函数 */
				tr->background(dev);
				mutex_unlock(&dev->lock);
				spin_lock_irq(rq->queue_lock);
				/*
				 * Do background processing just once per idle
				 * period.
				 */
                /* 在空闲周期中，仅处理一次后台流程 */
				background_done = !dev->bg_stop;
                /* 继续下一次循环，主要是判断是否新的数据请求 */
				continue;
			}
            /* 没有数据请求，也不需要处理后台流程时
             * 则设置线程状态为可中断状态，进入到休眠状态，
             * 等待被唤醒, wake_up_process */
			set_current_state(TASK_INTERRUPTIBLE);

            /* 被设置为需要停止线程了
             * 那需要把线程状态设置为 TASK_RUNNING 状态，以便线程退出 */
			if (kthread_should_stop())
				set_current_state(TASK_RUNNING);

            /* 线程状态设置好了，调度吧骚年 */
			spin_unlock_irq(rq->queue_lock);
			schedule();
			spin_lock_irq(rq->queue_lock);
			continue;
		}

		spin_unlock_irq(rq->queue_lock);

        /* 数据请求的处理是在 do_blktrans_request 中执行的
         * 这里完成了数据的读写,完成了 块设备的最底层数据操作 */
		mutex_lock(&dev->lock);
		res = do_blktrans_request(dev->tr, dev, req);
		mutex_unlock(&dev->lock);

		spin_lock_irq(rq->queue_lock);

        /* 完成了这个数据请求,就是告知块设备子系统，这个请求结束了
         * 可以释放资源/或读取数据了, 
         * __blk_end_request_cur 必须在持有 rq->queue_lock 锁的情况下调用 */
		if (!__blk_end_request_cur(req, res))
			req = NULL;

        /* 重新设置后台处理流程标志，可以在没有数据请求时进入后台流程中 */
		background_done = 0;
	}

    /* 线程要退出了，如果还有数据请求，需要释放所有的请求 */
	if (req)
		__blk_end_request_all(req, -EIO);

	spin_unlock_irq(rq->queue_lock);

	return 0;
}

/* mtd 操作请求入口函数
 * 块设备驱动中，会将对 此块设备的请求都放到 request_queue 中
 * 并通过 gendisk 结构中的 queue 的 request_fn 回调函数，调用到这里 */
static void mtd_blktrans_request(struct request_queue *rq)
{
	struct mtd_blktrans_dev *dev;
	struct request *req = NULL;

    /* 在 add_mtd_blktrans_dev 中，将 rq->queuedata 指向了 mtd_blktrans_dev 结构
     * 这样很方便互相找到 
     * 到这里，已经很明确这次的队列请求需要操作的是哪个设备了 dev */
	dev = rq->queuedata;

	if (!dev)
        /* 未找到具体的设备时，则将所有的请求遍历
         * 但返回的状态为  ENODEV */
		while ((req = blk_fetch_request(rq)) != NULL)
			__blk_end_request_all(req, -ENODEV);
	else {
        /* 找到了具体的设备，那就唤醒处理线程，由线程进行具体的处理 */
        /* bg_stop 用于停止后台处理
         * 每个 mtd_blktrans_thread 中都有一个后台处理，当然，这需要提供 
         * background 回调函数，当无任何数据请求时，则处理后台操作
         * 此处有数据请求了，要把后台处理先停止
         * 后台处理流程，可以参考 mtdswap.c ，提供了后台处理回调
         * mtdswap_background ，在后台处理函数中，不断地处理数据，并且
         * 也不断地判断 mtd_blktrans_cease_background 的返回值（该函数的返回值
         * 与 bg_stop 有关），当 bg_stop = true 时，退出后台处理流程 */
		dev->bg_stop = true;
		wake_up_process(dev->thread);
	}
}

/* 块设备的打开回调函数
 * 实际是在 block_dev.c 中的 __blkdev_get 中的 disk->fops->open 调用的
 * 传入的参数为 块设备结构 block_device 和 节点打开模式 mode */
static int blktrans_open(struct block_device *bdev, fmode_t mode)
{
    /* 在 block_dev.c 的 open 过程中，使用块设备节点设备号，
     * 查找到了 gendisk 结构, 将 gendisk 结构指针赋给了 bdev->bd_disk 
     * 将 bdev 做为参数调用了本函数
     * 所以下面使用了 bdev->bd_disk 获取了 gendisk 结构
     * blktrans_dev_get 使用 gendisk->private_data 
     * 获取了 mtd_blktrans_dev 结构 
     * 找到了 mtd_blktrans_dev 结构，就找到了 mtdblock 的所有信息 */
	struct mtd_blktrans_dev *dev = blktrans_dev_get(bdev->bd_disk);
	int ret = 0;

	if (!dev)
		return -ERESTARTSYS; /* FIXME: busy loop! -arnd*/

	mutex_lock(&dev->lock);

    /* 打开计数，如果已被打开过，则 open != 0
     * 此时不需要下面的操作了 */
	if (dev->open)
		goto unlock;

	kref_get(&dev->ref);
	__module_get(dev->tr->owner);

    /* 这种异常到底什么情况下会发生呢？
     * 在 add_mtd_blktrans_dev 是，已经将 mtd 给到了 mtd_blktrans_dev 结构
     * 那这种情况什么时候会发生，内核中很多这种判断。。。
     * 有一种可能，就是在 注销设备 和 打开设备时，但这种情况能否从上层中做一些保
     * 护，这样内核中的处理就可以省掉很多判断了 */
	if (!dev->mtd)
		goto unlock;

    /* 直接调用了 mtdblock 的 open 回调，无其它多余操作 */
	if (dev->tr->open) {
		ret = dev->tr->open(dev);
		if (ret)
			goto error_put;
	}

    /* 这里有点奇怪，为啥不是先调用 __get_mtd_device 函数？？
     * 而且这里调用 mtdchar 子系统的函数，是不是太直接了啊。。。 
     * 分析 mtdblock.c 中的 mtdblock_open 函数，里面并未操作任何 mtd_info 相关的
     * 数据，所以这里在第一次打开的时候要 __get_mtd_device */
	ret = __get_mtd_device(dev->mtd);
	if (ret)
		goto error_release;
    /* 设置文件打开模式 */
	dev->file_mode = mode;

    /* 已经被打开过的情况，会直接跳到 unlock
     * 递增打开计数
     * 这里还调用了 blktrans_dev_put 函数，对应 blktrans_dev_get 函数 */
unlock:
	dev->open++;
	mutex_unlock(&dev->lock);
	blktrans_dev_put(dev);
	return ret;

error_release:
	if (dev->tr->release)
		dev->tr->release(dev);
    /* 这里的错误返回，调用了 
     * kref_put(&dev->ref, blktrans_dev_release)
     * 和 blktrans_dev_put 两个函数
     * 而 blktrans_dev_put 中也是 上面的 kref_put 操作
     * 为什么会有重复的操作呢？ 
     * 这里两个操作的作用不同，虽然是一样的操作
     * kref_put 对应到上面的 kref_get 操作
     * 而 blktrans_dev_put 对应到 blktrans_dev_get 操作 */
error_put:
	module_put(dev->tr->owner);
	kref_put(&dev->ref, blktrans_dev_release);
	mutex_unlock(&dev->lock);
	blktrans_dev_put(dev);
	return ret;
}

/* 块设备的打开回调函数
 * 实际是在 block_dev.c 中的 __blkdev_put 中的 disk->fops->release 调用的
 * */
static int blktrans_release(struct gendisk *disk, fmode_t mode)
{
    /* 与 blktrans_open 不同，这里的参数是 gendisk */
	struct mtd_blktrans_dev *dev = blktrans_dev_get(disk);
	int ret = 0;

	if (!dev)
		return ret;

	mutex_lock(&dev->lock);

    /* 递减使用计数
     * 未到0，则表示还有其它进程打开，不能真的释放 */
	if (--dev->open)
		goto unlock;

    /* 打开计数已经到 0 了，需要真的释放了
     * 先释放 ref 计数，并调用 blktrans_dev_release 做清理 */
	kref_put(&dev->ref, blktrans_dev_release);
	module_put(dev->tr->owner);

    /* 调用 mtd_blktrans_ops 的 release 回调函数
     * 调用 mtdchar 设备的 __put_mtd_device */
	if (dev->mtd) {
		ret = dev->tr->release ? dev->tr->release(dev) : 0;
		__put_mtd_device(dev->mtd);
	}
    /* 不是最后一个关闭的，那不用做任何操作 */
unlock:
	mutex_unlock(&dev->lock);
	blktrans_dev_put(dev);
	return ret;
}

/* getgeo 回调用来获取设备的硬件驱动信息
 * 模拟磁盘的特性，如 盘面数、柱面数、扇区数 
 * 每个扇区为 512字节，这里调用底层驱动返回 mtd 设备的磁盘特性
 * 当然了，对于 mtdblock 来说，是没有这个信息的，mtdblock 不会模拟为磁盘
 * 但在 FTL 驱动中，会实现这个，FTL模拟了磁盘几何特性 */
static int blktrans_getgeo(struct block_device *bdev, struct hd_geometry *geo)
{
	struct mtd_blktrans_dev *dev = blktrans_dev_get(bdev->bd_disk);
	int ret = -ENXIO;

	if (!dev)
		return ret;

	mutex_lock(&dev->lock);

	if (!dev->mtd)
		goto unlock;

    /* 直接调用底层驱动接口，mtd trans 层不需要做任何处理 */
	ret = dev->tr->getgeo ? dev->tr->getgeo(dev, geo) : 0;
unlock:
	mutex_unlock(&dev->lock);
	blktrans_dev_put(dev);
	return ret;
}

/* 块设备的 ioctl 回调函数
 * 块设备支持很多标准操作，如设置只读，刷新块设备数据等 */
static int blktrans_ioctl(struct block_device *bdev, fmode_t mode,
			      unsigned int cmd, unsigned long arg)
{
	struct mtd_blktrans_dev *dev = blktrans_dev_get(bdev->bd_disk);
	int ret = -ENXIO;

	if (!dev)
		return ret;

	mutex_lock(&dev->lock);

	if (!dev->mtd)
		goto unlock;

    /* 对于 mtd 设备，只支持 BLKFLSBUF 这一种命令
     * 直接就调用了驱动 flush 接口，全部由底层驱动处理 */
	switch (cmd) {
	case BLKFLSBUF:
		ret = dev->tr->flush ? dev->tr->flush(dev) : 0;
		break;
	default:
		ret = -ENOTTY;
	}
unlock:
	mutex_unlock(&dev->lock);
	blktrans_dev_put(dev);
	return ret;
}

/* 块设备操作函数结构体
 * 当打开块设备时，会调用到这里
 * 真烦，结构体名 mtd_blktrans_ops 与 mtdblock.c 中的结构体类型重名
 * 这种真的好烦啊，找的时候会找错。。。。 */
static const struct block_device_operations mtd_blktrans_ops = {
	.owner		= THIS_MODULE,
	.open		= blktrans_open,
	.release	= blktrans_release,
	.ioctl		= blktrans_ioctl,
	.getgeo		= blktrans_getgeo,
};

int add_mtd_blktrans_dev(struct mtd_blktrans_dev *new)
{
	struct mtd_blktrans_ops *tr = new->tr;
	struct mtd_blktrans_dev *d;
	int last_devnum = -1;
	struct gendisk *gd;
	int ret;

    /* 任何操作 mtd 的地方，都要加这个锁。。。 */
	if (mutex_trylock(&mtd_table_mutex)) {
		mutex_unlock(&mtd_table_mutex);
		BUG();
	}

	mutex_lock(&blktrans_ref_mutex);
    /* mtd_blktrans_ops 的 devs 链表，链接了所有使用本ops的 mtd_blktrans_dev
     * 使用同一个 ops 的所有 dev 的 devnum 都必须是唯一的 
     * 这里遍历 ops devs 链表中的所有成员，判断新注册的 new dev 的 devnum 合法性
     * 在 mtdblock.c 中， devnum 被设置为 mtd 的 index , 根据这里的处理分析，
     * devnum 也可以设置为 -1 ，由本函数自动分配一个
     * 注意，devs 链表中，所有的 dev 成员都是按照 devnum 升序排列的
     * 所以这里可以借助 last_devnum 实现自动分配第一个未使用的 devnum */
	list_for_each_entry(d, &tr->devs, list) {
		if (new->devnum == -1) {
            /* new->devnum = -1 自动分配一个，递增查找未使用的即可
             * 注意，这里有可能按照这种方法找不到
             * 比如已经注册的devnum为 0 1 2
             * 那每次遍历的 d->devnum 都与 last_devnum+1 相等
             * 这时就会因查找到最后一个元素为 NULL 而跳出循环
             * 使用下面的处理 */
			/* Use first free number */
			if (d->devnum != last_devnum+1) {
				/* Found a free devnum. Plug it in here */
                /* 使用第一个未使用的 devnum
                 * 并把新注册的dev new 插入链表中 */
				new->devnum = last_devnum+1;
				list_add_tail(&new->list, &d->list);
				goto added;
			}
		} else if (d->devnum == new->devnum) {
			/* Required number taken */
            /* 新注册的 devnum 已经被注册过了，返回失败 */
			mutex_unlock(&blktrans_ref_mutex);
			return -EBUSY;
		} else if (d->devnum > new->devnum) {
			/* Required number was free */
            /* 已注册的dev中，已经遍历到 devnum 大于新注册的 dev 了
             * 说明新注册的这个是空闲的，可以使用
             * 这是基于 devs 链表是以 devnum 递增排序的 */
			list_add_tail(&new->list, &d->list);
			goto added;
		}
		last_devnum = d->devnum;
	}

    /* 上述过程中未找到合适的 devnum
     * 这个过程就是 if (d->devnum != last_devnum+1) 中分析的 */
	ret = -EBUSY;
	if (new->devnum == -1)
		new->devnum = last_devnum+1;

	/* Check that the device and any partitions will get valid
	 * minor numbers and that the disk naming code below can cope
	 * with this number. */
    /* part_bits 是给分区预留的 bit 数量，如 part_bits = 2
     * 则表示要预留 4 个从设备号（分区），0 1 2 3
     * 所以，而 devnum 是首个从设备号相关的， devnum << part_bits 
     * 是首个从设备号，所以 devnum 不能超过限值
     * 按照下面设置 disk_name 的处理， devnum 最多只分成了两位
     * 所以，如果 part_bits != 0 (表示有多个分区)
     * 那 devnum 不能超过 27 * 26 (aa - zz) 
     * 下面的计算是从 26开始的，所以需要 27 * 26 */
	if (new->devnum > (MINORMASK >> tr->part_bits) ||
	    (tr->part_bits && new->devnum >= 27 * 26)) {
		mutex_unlock(&blktrans_ref_mutex);
		goto error1;
	}

    /* 已经确认好 devnum了，将 mtd_blktrans_dev 添加到 ops 链表中吧
     * 下次再进来的时候就能遍历到这个 devnum 了 O(∩_∩)O */
	list_add_tail(&new->list, &tr->devs);
 added:
	mutex_unlock(&blktrans_ref_mutex);

	mutex_init(&new->lock);
	kref_init(&new->ref);
    /* mtd_blktrans_ops 没有提供 writesect 回调函数
     * 那只能设置为 只读了 */
	if (!tr->writesect)
		new->readonly = 1;

	/* Create gendisk */
    /* 创建磁盘结构，参数 1 << tr->part_bits 是从设备号数量
     * 从设备号数量就是这个磁盘支持的最多分区数量
     * 如果参数为 1，则不支持分区
     * 对于 mtdblock, tr->part_bits = 0, 也就是不支持分区
     * 默认的 mtdblock 不支持分区，只要修改了 mtdblock.c 中的 
     * mtdblock_tr 结构中的 part_bits 就可以支持分区了
     * alloc_disk 的参数，在分配 gd 的时候，会被赋值给 minors  */
	ret = -ENOMEM;
	gd = alloc_disk(1 << tr->part_bits);

	if (!gd)
		goto error2;

    /* 又是结构体互相指，为了能够互相找到对方
     * 块设备结构 gd private_data 指向 mtd_blktrans_dev new
     * 这样在打开块设备的时候，就能找到具体操作的 mtd_blktrans_dev 了 */
	new->disk = gd;
	gd->private_data = new;
    /* 本磁盘的主设备及次设备号起始值
     * 注意，次设备号最大数量已经在 alloc_disk 时赋值了 */
	gd->major = tr->major;
	gd->first_minor = (new->devnum) << tr->part_bits;
    /* 块设备操作回调函数，注意， mtd_blktrans_ops 是这个函数上面的。。。 */
	gd->fops = &mtd_blktrans_ops;

    /* 根据是否有分区设置块设备名
     * 如果没有分区 tr->part_bits = 0
     *  那块设备名直接使用设备编号命名，如 mtdblock1
     * 如果有分区 tr->part_bits != 0
     * 那需要判断设备序号，这里最多分成两种情况：
     *  设备序号小于 26时    mtdblocka mtdblockb 
     *  设备序号大于 26时    mtdblockab mtdblockdc */
	if (tr->part_bits)
		if (new->devnum < 26)
			snprintf(gd->disk_name, sizeof(gd->disk_name),
				 "%s%c", tr->name, 'a' + new->devnum);
		else
			snprintf(gd->disk_name, sizeof(gd->disk_name),
				 "%s%c%c", tr->name,
				 'a' - 1 + new->devnum / 26,
				 'a' + new->devnum % 26);
	else
		snprintf(gd->disk_name, sizeof(gd->disk_name),
			 "%s%d", tr->name, new->devnum);

    /* 设置容量，是扇区数，每个扇区 512字节 
     * 这里设置容量的扇区数量，不管设备物理扇区是多少，
     * 内核和块设备驱动之间的扇区都是 512自己 */
	set_capacity(gd, (new->size * tr->blksize) >> 9);

	/* Create the request queue */
    /* queue_lock 是请求队列锁
     * new->queue_lock 是 mtd_blktrans_dev 结构中的
     * 用来保护 mtdblock 的各种操作的
     * 这个也会在 blk_init_queue 初始化时，给 request_queue 中的 queue_lock 
     * 同时用来保护 mtdblock 中的操作
     * 关于 request_queue 的机制还不清楚，总之就是用来唤醒、处理的队列
     * 在下面的处理中，会把 new->rq 给 gd->queue ，会在块设备驱动中使用
     * mtd_blktrans_request 是处理 I/O 请求的函数
     * 这里是唤醒指定的 mtdblock 的处理线程 */
	spin_lock_init(&new->queue_lock);
	new->rq = blk_init_queue(mtd_blktrans_request, &new->queue_lock);

	if (!new->rq)
		goto error3;

    /* 将 mtd_blktrans_dev new 给 request_queue 的 queuedata
     * 这个会在 mtd_blktrans_request 处理中用于从 request_queue 中得到 new 
     * 得到 new 了，就能确定要操作的 mtdblock 设备了
     * queue 的逻辑块大小设置为 mtd_blktrans_ops 中的 blksize
     * 这里的效果是逻辑块、物理块、最小IO 都设置为 blksize 了 */
	new->rq->queuedata = new;
	blk_queue_logical_block_size(new->rq, tr->blksize);

    /* 设置队列标识为 QUEUE_FLAG_NONROT
     * 这个标识块设备请求队列对应的块设备是一种非旋转存储设备
     * 意味着没有旋转延迟（寻道时间等），这样内核可以做一些优化 */
	queue_flag_set_unlocked(QUEUE_FLAG_NONROT, new->rq);

    /* 块设备丢弃操作，与 TRIM 命令相关
     * 这是一个块设备优化操作，可以提高IO写入性能
     * 对于 mtdblock 设备，不支持。。。 */
	if (tr->discard) {
		queue_flag_set_unlocked(QUEUE_FLAG_DISCARD, new->rq);
		new->rq->limits.max_discard_sectors = UINT_MAX;
	}

    /* 块设备请求队列给块设备结构，会用这个请求块设备操作 */
	gd->queue = new->rq;

	/* Create processing thread */
	/* TODO: workqueue ? */
    /* 创建处理线程，每个 mtdblock 都会创建一个处理线程
     * 给线程的参数就是 mtd_blktrans_dev 结构，包含了各种信息
     * 另外，又将线程句柄给了 thread ，会在 mtd_blktrans_request 中唤醒 */
	new->thread = kthread_run(mtd_blktrans_thread, new,
			"%s%d", tr->name, new->mtd->index);
	if (IS_ERR(new->thread)) {
		ret = PTR_ERR(new->thread);
		goto error4;
	}
    /* 指向了 mtd 设备的 dev ，不确定具体作用 */
	gd->driverfs_dev = &new->mtd->dev;

    /* 若为只读设备，则设置块设备为只读 */
	if (new->readonly)
		set_disk_ro(gd, 1);

    /* 向内核注册块设备
     * 后面就可以使用 /dev/mtdblock1 等设备节点访问了 */
	add_disk(gd);

    /* mtd_blktrans_dev 中有特殊的节点属性，则在 sys 中创建 */
	if (new->disk_attributes) {
		ret = sysfs_create_group(&disk_to_dev(gd)->kobj,
					new->disk_attributes);
		WARN_ON(ret);
	}
	return 0;
error4:
	blk_cleanup_queue(new->rq);
error3:
	put_disk(new->disk);
error2:
	list_del(&new->list);
error1:
	return ret;
}

/* 移除 mtdblock 设备
 * 这个是由 mtd_blktrans_ops 结构中的 remove_dev 调用的
 * mtdblock 设备中，是有 mtdblock.c 中的 mtdblock_remove_dev 调用 */
int del_mtd_blktrans_dev(struct mtd_blktrans_dev *old)
{
	unsigned long flags;

	if (mutex_trylock(&mtd_table_mutex)) {
		mutex_unlock(&mtd_table_mutex);
		BUG();
	}

    /* 先移除 sysfs 下的属性文件 */
	if (old->disk_attributes)
		sysfs_remove_group(&disk_to_dev(old->disk)->kobj,
						old->disk_attributes);

	/* Stop new requests to arrive */
    /* 移除块设备 磁盘设备
     * 所有的文件操作请求都会停止了 */
	del_gendisk(old->disk);


	/* Stop the thread */
    /* 停止 mtdblock 处理的线程 */
	kthread_stop(old->thread);

	/* Kill current requests */
    /* 删除请求队列中所有请求 */
	spin_lock_irqsave(&old->queue_lock, flags);
	old->rq->queuedata = NULL;
	blk_start_queue(old->rq);
	spin_unlock_irqrestore(&old->queue_lock, flags);

	/* If the device is currently open, tell trans driver to close it,
		then put mtd device, and don't touch it again */
    /* 如果这个设备还是打开的（open != 0）
     * 那就调用 release ，通知 trans 驱动要关闭了
     * 还要释放 mtd 设备，因为在 open 是引用了(get) */
	mutex_lock(&old->lock);
	if (old->open) {
		if (old->tr->release)
			old->tr->release(old);
		__put_mtd_device(old->mtd);
	}

    /* mtd 清空，没用了 */
	old->mtd = NULL;

	mutex_unlock(&old->lock);
    /* 释放 mtd_blktrans_dev 结构 */
	blktrans_dev_put(old);
	return 0;
}

/* mtd 块设备 mtd_notifier
 * 在移除 mtd字符设备时调用，参数为字符设备 mtd_info */
static void blktrans_notify_remove(struct mtd_info *mtd)
{
	struct mtd_blktrans_ops *tr;
	struct mtd_blktrans_dev *dev, *next;

    /* 遍历所有的 mtd_blktrans_ops
     * 注意，需要确认 mtd_info 一致才可真正移除
     * 在 tr->remove_dev 中，会调用到 mtdblock.c 中的 remove_dev */
	list_for_each_entry(tr, &blktrans_majors, list)
		list_for_each_entry_safe(dev, next, &tr->devs, list)
			if (dev->mtd == mtd)
				tr->remove_dev(dev);
}

/* mtd 块设备 mtd_notifier
 * 在注册 mtd字符设备时调用，参数为字符设备 mtd_info */
static void blktrans_notify_add(struct mtd_info *mtd)
{
	struct mtd_blktrans_ops *tr;

	/* https://web.git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=a43bdc376deab5fff1ceb93dca55bcab8dbdc1d6 */
    //if (mtd->type == MTD_ABSENT)
    if (mtd->type == MTD_ABSENT || mtd->type == MTD_UBIVOLUME)
		return;

    /* 所有的 mtd_blktrans_ops 都会链到 blktrans_majors 中
     * 遍历所有的 add_mtd 回调函数，使用 mtd_info 注册 */
	list_for_each_entry(tr, &blktrans_majors, list)
		tr->add_mtd(tr, mtd);
}

/* mtd 块设备 mtd_notifier
 * 当 mtdcore.c 中注册 mtd字符设备时，会遍历所有的 mtd_notifier
 * 这是会调用到 .add 回调函数，并将注册的 mtd_info 做为参数 */
static struct mtd_notifier blktrans_notifier = {
	.add = blktrans_notify_add,
	.remove = blktrans_notify_remove,
};

/* 注册 mtd 的块设备原始设备层
 * 这个函数会在很多其它驱动模块中调用，如
 * mtdblock.c drivers/mtd/ftl.c drivers/mtd/nftlcore.c */
int register_mtd_blktrans(struct mtd_blktrans_ops *tr)
{
	struct mtd_info *mtd;
	int ret;

	/* Register the notifier if/when the first device type is
	   registered, to prevent the link/init ordering from fucking
	   us over. */
    /* 注册 mtd块设备的 mtd_notifier
     * 这样在注册 mtd字符设备时，就可以主动调用到这里的 .add 回调函数
     * 在 .add 回调函数中，才会真正处理 mtd块设备的注册
     * 只有在第一次进入本函数时才会注册 mtd_notifier
     * 后面其它的注册的 mtd_blktrans_ops ，都是连接到 blktrans_majors 链表中
     * 这个链表在 mtd_notifier 的 .add 回调函数中遍历调用 */
	if (!blktrans_notifier.list.next)
		register_mtd_user(&blktrans_notifier);


	mutex_lock(&mtd_table_mutex);

    /* 按照 mtd_blktrans_ops 中的主设备、块设备名 注册块设备
     * 这里注册之后，就会在 /proc/devices 中查询到 */
	ret = register_blkdev(tr->major, tr->name);
	if (ret < 0) {
		printk(KERN_WARNING "Unable to register %s block device on major %d: %d\n",
		       tr->name, tr->major, ret);
		mutex_unlock(&mtd_table_mutex);
		return ret;
	}

    /* register_blkdev 返回值是主设备号 major 或 0
     * 所以这里不管调用 register_blkdev 时是否提供了 major
     * 只要没返回错误，并且不是 0 都是主设备号 */
	if (ret)
		tr->major = ret;

    /* blksize 是虚拟的块大小
     * blkshift 计算块的偏移位数 */
	tr->blkshift = ffs(tr->blksize) - 1;

    /* mtd_blktrans_ops 中的 devs 用于链接所有使用该 ops 的 mtd_blktrans_dev 
     * */
	INIT_LIST_HEAD(&tr->devs);
    /* 将新注册的 mtd_blktrans_ops 添加到 blktrans_majors 链表中
     * 这是为了在 mtd_notifier blktrans_notifier 的 .add 回调函数中遍历调用 */
	list_add(&tr->list, &blktrans_majors);

    /* 遍历所有的 mtd设备，调用 mtd_blktrans_ops 的 .add_mtd 回调函数
     * 对于 mtdblock.c ，就是 mtdblock_tr 的 .add_mtd 
     * 用于处理mtd字符设备 创建 mtd块设备的请求 */
	mtd_for_each_device(mtd)
        /* https://web.git.kernel.org/pub/scm/linux/kernel/git/torvalds/linux.git/commit/?id=a43bdc376deab5fff1ceb93dca55bcab8dbdc1d6 */
        //if (mtd->type != MTD_ABSENT)
        if (mtd->type != MTD_ABSENT && mtd->type != MTD_UBIVOLUME)
			tr->add_mtd(tr, mtd);

	mutex_unlock(&mtd_table_mutex);
	return 0;
}

/* 注销一个 mtd_blktrans_dev 设备 
 * 与 register_mtd_blktrans 接口对应 */
int deregister_mtd_blktrans(struct mtd_blktrans_ops *tr)
{
	struct mtd_blktrans_dev *dev, *next;

	mutex_lock(&mtd_table_mutex);

	/* Remove it from the list of active majors */
    /* 将 mtd_blktrans_ops 从 blktrans_majors 链表中移除
     * 所有的 mtd_blktrans_ops 都在 blktrans_majors 链表中
     * 用于在添加一个 mtd 设备时，遍历到所有的 mtd_blktrans_ops 
     * blktrans_notify_add 函数 */
	list_del(&tr->list);

    /* 所有注册的 mtd 设备都在 tr->devs 链表中
     * 需要遍历所有的 mtd设备，全部 remove_dev 掉 */
	list_for_each_entry_safe(dev, next, &tr->devs, list)
		tr->remove_dev(dev);

    /* 注销块设备，提供主设备号和块设备名 */
	unregister_blkdev(tr->major, tr->name);
	mutex_unlock(&mtd_table_mutex);

	BUG_ON(!list_empty(&tr->devs));
	return 0;
}

/* mtd_blkdevs.c 模块退出函数
 * 这个模块是没有 module_init 入口的
 * 这个模块主要提供一些注册接口，并不需要做初始化
 * 所以只有退出函数
 * 在退出函数中，用来卸载注册到 mtd 子系统中的 notifier 机制 */
static void __exit mtd_blktrans_exit(void)
{
	/* No race here -- if someone's currently in register_mtd_blktrans
	   we're screwed anyway. */
    /* 本模块只注册了一个 notifier ，那就注销掉就完了  */
	if (blktrans_notifier.list.next)
		unregister_mtd_user(&blktrans_notifier);
}

module_exit(mtd_blktrans_exit);

EXPORT_SYMBOL_GPL(register_mtd_blktrans);
EXPORT_SYMBOL_GPL(deregister_mtd_blktrans);
EXPORT_SYMBOL_GPL(add_mtd_blktrans_dev);
EXPORT_SYMBOL_GPL(del_mtd_blktrans_dev);

MODULE_AUTHOR("David Woodhouse <dwmw2@infradead.org>");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Common interface to block layer for MTD 'translation layers'");
