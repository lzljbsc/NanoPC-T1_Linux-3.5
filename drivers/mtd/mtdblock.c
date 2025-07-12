/*
 * Direct MTD block device access
 *
 * Copyright © 1999-2010 David Woodhouse <dwmw2@infradead.org>
 * Copyright © 2000-2003 Nicolas Pitre <nico@fluxnic.net>
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

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/vmalloc.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/blktrans.h>
#include <linux/mutex.h>


/* mtdblock 模块私有结构
 * 该结构用来描述所有 mtdblock 设备信息
 * mtd_blktrans_dev 结构是 mtd_blktrans 模块中的
 * 包含了更多信息，比如块设备结构，块设备文件操作函数等 */
struct mtdblk_dev {
    /* mtd_blktrans_dev 设备结构
     * 都是基于这个结构操作的 */
	struct mtd_blktrans_dev mbd;
    /* 本模块中 mtdblk_dev 设备的打开计数 */
	int count;
	struct mutex cache_mutex;
    /* cache 相关， mtdblock 设备中，只缓存一个擦除块
     * 按照最后写入的地址缓存 */
	unsigned char *cache_data;
	unsigned long cache_offset;
	unsigned int cache_size;
	enum { STATE_EMPTY, STATE_CLEAN, STATE_DIRTY } cache_state;
};

static DEFINE_MUTEX(mtdblks_lock);

/*
 * Cache stuff...
 *
 * Since typical flash erasable sectors are much larger than what Linux's
 * buffer cache can handle, we must implement read-modify-write on flash
 * sectors for each block write requests.  To avoid over-erasing flash sectors
 * and to speed things up, we locally cache a whole flash sector while it is
 * being written to until a different sector is required.
 */

/* 擦除操作的回调函数
 * 会在 flash 驱动中调用，用于唤醒这里的擦除操作 */
static void erase_callback(struct erase_info *done)
{
	wait_queue_head_t *wait_q = (wait_queue_head_t *)done->priv;
	wake_up(wait_q);
}

/* 擦除并写入 操作 */
static int erase_write (struct mtd_info *mtd, unsigned long pos,
			int len, const char *buf)
{
	struct erase_info erase;
	DECLARE_WAITQUEUE(wait, current);
	wait_queue_head_t wait_q;
	size_t retlen;
	int ret;

	/*
	 * First, let's erase the flash block.
	 */
    /* 需要先擦除 flash 块，再写入数据 */

    /* 组织擦除信息结构，定义唤醒队列 */
	init_waitqueue_head(&wait_q);
	erase.mtd = mtd;
	erase.callback = erase_callback;
	erase.addr = pos;
	erase.len = len;
	erase.priv = (u_long)&wait_q;

    /* 设置线程状态，后面会进入到休眠状态，等待被唤醒 */
	set_current_state(TASK_INTERRUPTIBLE);
	add_wait_queue(&wait_q, &wait);

    /* 调用擦除操作，这是个异步操作 */
	ret = mtd_erase(mtd, &erase);
	if (ret) {
        /* 有错误返回，则线程继续执行，取消等待队列，返回错误 */
		set_current_state(TASK_RUNNING);
		remove_wait_queue(&wait_q, &wait);
		printk (KERN_WARNING "mtdblock: erase of region [0x%lx, 0x%x] "
				     "on \"%s\" failed\n",
			pos, len, mtd->name);
		return ret;
	}

    /* 调度，等待擦除完成被唤醒 */
	schedule();  /* Wait for erase to finish. */
	remove_wait_queue(&wait_q, &wait);

	/*
	 * Next, write the data to flash.
	 */

    /* 擦除完成了，将数据直接写入 flahs 中 */
	ret = mtd_write(mtd, pos, len, &retlen, buf);
	if (ret)
		return ret;
	if (retlen != len)
		return -EIO;
	return 0;
}


/* 将 cache 缓存中的数据写入到 mtd 设备中 */
static int write_cached_data (struct mtdblk_dev *mtdblk)
{
	struct mtd_info *mtd = mtdblk->mbd.mtd;
	int ret;

    /* STATE_DIRTY 表示缓存中的数据是被修改过的
     * 未被修改过的数据无需更新 */
	if (mtdblk->cache_state != STATE_DIRTY)
		return 0;

	pr_debug("mtdblock: writing cached data for \"%s\" "
			"at 0x%lx, size 0x%x\n", mtd->name,
			mtdblk->cache_offset, mtdblk->cache_size);

    /* 将整个 cache 的数据更新到 mtd 设备中 */
	ret = erase_write (mtd, mtdblk->cache_offset,
			   mtdblk->cache_size, mtdblk->cache_data);
	if (ret)
		return ret;

	/*
	 * Here we could arguably set the cache state to STATE_CLEAN.
	 * However this could lead to inconsistency since we will not
	 * be notified if this content is altered on the flash by other
	 * means.  Let's declare it empty and leave buffering tasks to
	 * the buffer cache instead.
	 */
    /* 将 cache 缓存设置为 空 STATE_EMPTY
     * 如果只有一个驱动在用这个设备，那这里是可以设置为 STATE_CLEAN 的
     * 这表示数据是有效的，并且与 mtd 设备中是一致的
     * 但这里假设有其它方式也可以更改 flash 中的数据，如果设置为 STATE_CLEAN
     * 这将会导致数据一致性问题，所以设置为 STATE_EMPTY */
	mtdblk->cache_state = STATE_EMPTY;
	return 0;
}


/* 有 cache 的读操作 */
static int do_cached_write (struct mtdblk_dev *mtdblk, unsigned long pos,
			    int len, const char *buf)
{
    /* 获取 mtd设备的 mtd_info */
	struct mtd_info *mtd = mtdblk->mbd.mtd;
	unsigned int sect_size = mtdblk->cache_size;
	size_t retlen;
	int ret;

	pr_debug("mtdblock: write on \"%s\" at 0x%lx, size 0x%x\n",
		mtd->name, pos, len);

    /* 无 cache 的，直接写 */
	if (!sect_size)
		return mtd_write(mtd, pos, len, &retlen, buf);

    /* 一些注意事项，参考 do_cached_read 中的注释 */
	while (len > 0) {
        /* 这三个变量的作用，参考 do_cached_read 中的注释 */
		unsigned long sect_start = (pos/sect_size)*sect_size;
		unsigned int offset = pos - sect_start;
		unsigned int size = sect_size - offset;
		if( size > len )
			size = len;

		if (size == sect_size) {
            /* 需要写入整个扇区时，就不必使用缓存了
             * 直接整个扇区擦除/写入了 */
			/*
			 * We are covering a whole sector.  Thus there is no
			 * need to bother with the cache while it may still be
			 * useful for other partial writes.
			 */
			ret = erase_write (mtd, pos, size, buf);
			if (ret)
				return ret;
		} else {
			/* Partial sector: need to use the cache */
            /* 部分扇区时，需要使用缓存 */

            /* 如果缓存中存在数据 STATE_DIRTY
             * 并且缓存的数据块与现在需要的不符，
             * 那需要先把缓存中的数据写入 */
			if (mtdblk->cache_state == STATE_DIRTY &&
			    mtdblk->cache_offset != sect_start) {
				ret = write_cached_data(mtdblk);
				if (ret)
					return ret;
			}

            /* 当 cache 中无数据时 (STATE_EMPTY)
             * 或者 cache 中的数据不符合需要写入数据的地址范围
             * 那需要先从 flash 中读取出数据到 cache 中 */
			if (mtdblk->cache_state == STATE_EMPTY ||
			    mtdblk->cache_offset != sect_start) {
				/* fill the cache with the current sector */
				mtdblk->cache_state = STATE_EMPTY;
                /* 从 mtd 设备中直接读取数据到 cache 中
                 * 注意，这里读取的长度是 cache_size/sect_size
                 * 这个长度是要远大于 内核中的块设备扇区大小的 512 */
				ret = mtd_read(mtd, sect_start, sect_size,
					       &retlen, mtdblk->cache_data);
				if (ret)
					return ret;
				if (retlen != sect_size)
					return -EIO;

                /* 更新 cache 中的数据成功 */
				mtdblk->cache_offset = sect_start;
				mtdblk->cache_size = sect_size;
				mtdblk->cache_state = STATE_CLEAN;
			}

			/* write data to our local cache */
            /* 将数据写入到 cache 中，并且标记 cache 的数据为 已更改 STATE_DIRTY */
			memcpy (mtdblk->cache_data + offset, buf, size);
			mtdblk->cache_state = STATE_DIRTY;
		}

        /* 已经写入了 size 长度了，下次写的位置向后调整 */
		buf += size;
		pos += size;
		len -= size;
	}

	return 0;
}


/* 有 cache 的读操作 */
static int do_cached_read (struct mtdblk_dev *mtdblk, unsigned long pos,
			   int len, char *buf)
{
    /* 获取 mtd设备的 mtd_info */
	struct mtd_info *mtd = mtdblk->mbd.mtd;
	unsigned int sect_size = mtdblk->cache_size;
	size_t retlen;
	int ret;

	pr_debug("mtdblock: read on \"%s\" at 0x%lx, size 0x%x\n",
			mtd->name, pos, len);

    /* 无需 cache 的设备，直接读 */
	if (!sect_size)
		return mtd_read(mtd, pos, len, &retlen, buf);

    /* 这里需要留意
     * 参数中 pos 是按照内核中的块设备扇区地址（512字节对齐）
     * 参数中 len 在块设备中，应该都是 512 字节的倍数，当然，不是倍数也没关系
     * sect_size 是 cache_size ，也就是 mtd设备的擦除块大小，一般是 4096 
     * 这个是比内核中的扇区 512字节 要大很多的
     * 所以在读取的时候，要考虑实际擦除块的大小，而 cache 是按照实际擦除块大小设
     * 置的，所以如果读 cache 内的数据，需要计算好 pos 在 cache 中的偏移 */
	while (len > 0) {
        /* sect_start 是按照mtd设备实际擦除块大小计算的扇区起始地址
         * offset 是实际擦除块中的地址偏移
         * size 第一次计算的值是 首个要读取的擦除块中，offset 后面剩余的大小
         *
         * 在第一次进入时，pos 与 擦除块地址可能不是对齐的，所以需要计算偏移以及
         * 首块中后面需要的数据长度（size），当进入第二次循环的时候，因为已经把
         * 首块中未对齐的部分读取完成了，pos += size 了，所以 pos 就与 擦除块地
         * 址对齐了，这样再计算出来的 sect_start 就是擦除块地址，offset = 0，
         * size = sect_size 了
         *
         * 当然了，循环几次，要看 len 参数，太小的值就不会有后面的循环了 */
		unsigned long sect_start = (pos/sect_size)*sect_size;
		unsigned int offset = pos - sect_start;
		unsigned int size = sect_size - offset;
        /* len 是剩余待读取的数据长度，按照实际的读取 */
		if (size > len)
			size = len;

		/*
		 * Check if the requested data is already cached
		 * Read the requested amount of data from our internal cache if it
		 * contains what we want, otherwise we read the data directly
		 * from flash.
		 */
        /* 检查请求的数据是否已经缓存了，如果缓存符合，则从缓存中读取
         * mtdblock 打开后的首次读取 cache_state == STATE_EMPTY 的
         * 肯定会从mtd中直接读取的
         * 在以后有写入的情况了，那就要判断是否已经缓存了 */
		if (mtdblk->cache_state != STATE_EMPTY &&
		    mtdblk->cache_offset == sect_start) {
            /* 缓存中的数据，拷贝到 buf 中即可 */
			memcpy (buf, mtdblk->cache_data + offset, size);
		} else {
            /* 缓存中没有的，那就从 mtd 中直接读取 */
			ret = mtd_read(mtd, pos, size, &retlen, buf);
			if (ret)
				return ret;
			if (retlen != size)
				return -EIO;
		}

        /* 已经读取了 size 长度了，下次读的位置向后调整 */
		buf += size;
		pos += size;
		len -= size;
	}

	return 0;
}

/* mtdblock 设备 readsect 回调 */
static int mtdblock_readsect(struct mtd_blktrans_dev *dev,
			      unsigned long block, char *buf)
{
	struct mtdblk_dev *mtdblk = container_of(dev, struct mtdblk_dev, mbd);
    /* mtdblock 设备中， blk 固定为 512字节，所以这里直接参数固定了 */
	return do_cached_read(mtdblk, block<<9, 512, buf);
}

/* mtdblock 设备 writesect 回调 */
static int mtdblock_writesect(struct mtd_blktrans_dev *dev,
			      unsigned long block, char *buf)
{
	struct mtdblk_dev *mtdblk = container_of(dev, struct mtdblk_dev, mbd);
    /* 需要 cache 的，第一次执行时需要分配 */
	if (unlikely(!mtdblk->cache_data && mtdblk->cache_size)) {
        /* 这里的参数好奇怪啊
         * 为什么 vmalloc 的参数不使用 mtdblk->cache_size 呢
         * 虽然它俩确定是一个值，但这样不会很奇怪吗 */
		mtdblk->cache_data = vmalloc(mtdblk->mbd.mtd->erasesize);
		if (!mtdblk->cache_data)
			return -EINTR;
		/* -EINTR is not really correct, but it is the best match
		 * documented in man 2 write for all cases.  We could also
		 * return -EAGAIN sometimes, but why bother?
		 */
	}
    /* mtdblock 设备中， blk 固定为 512字节，所以这里直接参数固定了 */
	return do_cached_write(mtdblk, block<<9, 512, buf);
}

/* mtdblock 设备 open 回调 */
static int mtdblock_open(struct mtd_blktrans_dev *mbd)
{
	struct mtdblk_dev *mtdblk = container_of(mbd, struct mtdblk_dev, mbd);

	pr_debug("mtdblock_open\n");

	mutex_lock(&mtdblks_lock);
    /* 已经被打开过了，则递增计数后直接返回 */
	if (mtdblk->count) {
		mtdblk->count++;
		mutex_unlock(&mtdblks_lock);
		return 0;
	}

	/* OK, it's not open. Create cache info for it */
    /* 首次打开，需要创建 cache info  */
	mtdblk->count = 1;
	mutex_init(&mtdblk->cache_mutex);
	mtdblk->cache_state = STATE_EMPTY;
    /* 需要擦除的设备，按照擦除单元大小创建 cache  */
	if (!(mbd->mtd->flags & MTD_NO_ERASE) && mbd->mtd->erasesize) {
        /* 注意，这里并没有直接分配 cache_data
         * 真正分配空间是在 写入操作时 */
		mtdblk->cache_size = mbd->mtd->erasesize;
		mtdblk->cache_data = NULL;
	}

	mutex_unlock(&mtdblks_lock);

	pr_debug("ok\n");

	return 0;
}

/* mtdblock 设备 release 回调 */
static int mtdblock_release(struct mtd_blktrans_dev *mbd)
{
	struct mtdblk_dev *mtdblk = container_of(mbd, struct mtdblk_dev, mbd);

	pr_debug("mtdblock_release\n");

	mutex_lock(&mtdblks_lock);

    /* 有 cache 缓存，要关闭了，将 cache 中的数据写入到 flash 中 */
	mutex_lock(&mtdblk->cache_mutex);
	write_cached_data(mtdblk);
	mutex_unlock(&mtdblk->cache_mutex);

    /* 最后一个关闭的，同步一下，并释放掉 cache  */
	if (!--mtdblk->count) {
		/*
		 * It was the last usage. Free the cache, but only sync if
		 * opened for writing.
		 */
        /* 只有以 write 方式打开才会调用 */
		if (mbd->file_mode & FMODE_WRITE)
			mtd_sync(mbd->mtd);
		vfree(mtdblk->cache_data);
	}

	mutex_unlock(&mtdblks_lock);

	pr_debug("ok\n");

	return 0;
}

/* mtdblock 设备 flush 回调 */
static int mtdblock_flush(struct mtd_blktrans_dev *dev)
{
	struct mtdblk_dev *mtdblk = container_of(dev, struct mtdblk_dev, mbd);

    /* flush 将 cache 中的数据刷写到 flash中 */
	mutex_lock(&mtdblk->cache_mutex);
	write_cached_data(mtdblk);
	mutex_unlock(&mtdblk->cache_mutex);
	mtd_sync(dev->mtd);
	return 0;
}

/* mtdblock 模块提供的注册回调
 * 当有新注册的mtd设备时，会调用， mtd 指向mtd设备信息结构 */
static void mtdblock_add_mtd(struct mtd_blktrans_ops *tr, struct mtd_info *mtd)
{
    /* 新分配一个 mtdblock 中私有管理结构
     * 新注册的 mtd 对应的 mtdblock 都在这个结构中管理 */
	struct mtdblk_dev *dev = kzalloc(sizeof(*dev), GFP_KERNEL);

	if (!dev)
		return;

    /* mtdblk_dev 结构内嵌 mtd_blktrans_dev
     * mtd_blktrans_dev 结构包含注册为 mtdblock 的信息
     * 并且要指向注册的 mtd_info
     * devnum 使用 mtd_info 的 index ， 这也是以后设备节点序号 /dev/mtdblockx */
	dev->mbd.mtd = mtd;
	dev->mbd.devnum = mtd->index;

    /* 设置 mtd_blktrans_dev 大小，这里是指扇区数量
     * mtd->size 是mtd设备的总大小， >>9 是因为每个扇区 512字节 blksize */
	dev->mbd.size = mtd->size >> 9;
    /* mtd_blktrans_dev 也要有 mtd_blktrans_ops  */
	dev->mbd.tr = tr;

    /* 如果注册的 mtd 本身是只读的，那 mtdblock 也要设置为只读 */
	if (!(mtd->flags & MTD_WRITEABLE))
		dev->mbd.readonly = 1;

    /* 注册 mtd_blktrans_dev 设备, 将注册块设备 mtdblock  */
	if (add_mtd_blktrans_dev(&dev->mbd))
		kfree(dev);
}

/* mtdblock 模块注销回调，用于移除已注册的设备 */
static void mtdblock_remove_dev(struct mtd_blktrans_dev *dev)
{
    /* 啥也没干，直接调用了 mtd_blktrans 模块移除函数
     * 全部都在 mtd_blktrans 模块中处理 */
	del_mtd_blktrans_dev(dev);
}

/* 注册 mtd blktrans 驱动
 * major 与 name 两个字段，还用于注册 block 设备
 * 从 cat /proc/devices 中可以查到  31 mtdblock
 * /dev/mtdblock 设备节点的命名也与 name 有关
 *
 * .add_mtd 回调函数，会在 register_mtd_blktrans 函数中调用
 * 另外，还会在注册新mtd设备时，由 mtd_notifier 机制调用
 *
 * part_bits 指示分区占用的bit位数 如果 = 2, 就是最多有4个分区
 * mtdblock 默认 part_bits = 0，不支持分区
 * 如果设置不为 0，那就可以分区了。。。 fdisk 
 * 但这意义不大。。。 */
static struct mtd_blktrans_ops mtdblock_tr = {
	.name		= "mtdblock",
	.major		= 31,
	.part_bits	= 0,
	.blksize 	= 512,
	.open		= mtdblock_open,
	.flush		= mtdblock_flush,
	.release	= mtdblock_release,
	.readsect	= mtdblock_readsect,
	.writesect	= mtdblock_writesect,
	.add_mtd	= mtdblock_add_mtd,
	.remove_dev	= mtdblock_remove_dev,
	.owner		= THIS_MODULE,
};

/* mtd block 设备的初始化入口 */
static int __init init_mtdblock(void)
{
    /* 注册一个 mtd_blktrans_dev 设备，用来屏蔽块设备细节的
     * 该注册接口中，包含块设备的注册 */
	return register_mtd_blktrans(&mtdblock_tr);
}

/* mtd block 设备退出函数 */
static void __exit cleanup_mtdblock(void)
{
    /* 移除 mtd_blktrans_dev 设备 */
	deregister_mtd_blktrans(&mtdblock_tr);
}

module_init(init_mtdblock);
module_exit(cleanup_mtdblock);


MODULE_LICENSE("GPL");
MODULE_AUTHOR("Nicolas Pitre <nico@fluxnic.net> et al.");
MODULE_DESCRIPTION("Caching read/erase/writeback block device emulation access to MTD devices");
