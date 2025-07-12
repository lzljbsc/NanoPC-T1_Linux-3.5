/*
 * Simple MTD partitioning layer
 *
 * Copyright © 2000 Nicolas Pitre <nico@fluxnic.net>
 * Copyright © 2002 Thomas Gleixner <gleixner@linutronix.de>
 * Copyright © 2000-2010 David Woodhouse <dwmw2@infradead.org>
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

#include <linux/module.h>
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/kmod.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/err.h>

#include "mtdcore.h"

/* Our partition linked list */
static LIST_HEAD(mtd_partitions);
static DEFINE_MUTEX(mtd_partitions_mutex);

/* Our partition node structure */
/* MTD分区结构 
 * 其中 mtd_info mtd 结构用于描述该分区信息，
 * 每个分区都是被看成一个MTD原始设备,
 * 按照下面 part_ 函数的使用方法， mtd 成员只能在第一个，
 * 因为通过 mtd 查找 mtd_part 时，使用的 PART(x) 宏，
 * 该宏是直接强转类型的 */
struct mtd_part {
	struct mtd_info mtd;    /* 分区的详细信息,只能在第一个成员 */
	struct mtd_info *master;/* 指向 该分区 所属的 主MTD设备 */
	uint64_t offset;        /* 分区偏移量 */
	struct list_head list;  /* 将所有 mtd_part 链到链表 mtd_partitions 中 */
};

/*
 * Given a pointer to the MTD object in the mtd_part structure, we can retrieve
 * the pointer to that structure with this macro.
 */
/* 给定结构转换为  struct mtd_part 结构
 * 这里直接强转，所以 对 struct mtd_part 结构中的成员排列有要求 */
#define PART(x)  ((struct mtd_part *)(x))


/*
 * MTD methods which simply translate the effective address and pass through
 * to the _real_ device.
 * MTD 方法，简单地转换有效地址并传递到真实设备。 
 * 对于MTD分区而言，它所在地真实MTD设备由 master 指向，真正的操作函数都是在主MTD
 * 设备中的 
 */

/* part_read 转换偏移地址，直接调用主MTD设备方法*/
static int part_read(struct mtd_info *mtd, loff_t from, size_t len,
		size_t *retlen, u_char *buf)
{
	struct mtd_part *part = PART(mtd);
	struct mtd_ecc_stats stats;
	int res;

	stats = part->master->ecc_stats;
	res = part->master->_read(part->master, from + part->offset, len,
				  retlen, buf);
	if (unlikely(mtd_is_eccerr(res)))
		mtd->ecc_stats.failed +=
			part->master->ecc_stats.failed - stats.failed;
	else
		mtd->ecc_stats.corrected +=
			part->master->ecc_stats.corrected - stats.corrected;
	return res;
}

/* part_point 调用主MTD方法 */
static int part_point(struct mtd_info *mtd, loff_t from, size_t len,
		size_t *retlen, void **virt, resource_size_t *phys)
{
	struct mtd_part *part = PART(mtd);

	return part->master->_point(part->master, from + part->offset, len,
				    retlen, virt, phys);
}

/* part_unpoint 调用主MTD方法 */
static int part_unpoint(struct mtd_info *mtd, loff_t from, size_t len)
{
	struct mtd_part *part = PART(mtd);

	return part->master->_unpoint(part->master, from + part->offset, len);
}

/* part_get_unmapped_area 调用主MTD方法 */
static unsigned long part_get_unmapped_area(struct mtd_info *mtd,
					    unsigned long len,
					    unsigned long offset,
					    unsigned long flags)
{
	struct mtd_part *part = PART(mtd);

	offset += part->offset;
	return part->master->_get_unmapped_area(part->master, len, offset,
						flags);
}

/* part_read_oob 调用主MTD方法 */
static int part_read_oob(struct mtd_info *mtd, loff_t from,
		struct mtd_oob_ops *ops)
{
	struct mtd_part *part = PART(mtd);
	int res;

	if (from >= mtd->size)
		return -EINVAL;
	if (ops->datbuf && from + ops->len > mtd->size)
		return -EINVAL;

	/*
	 * If OOB is also requested, make sure that we do not read past the end
	 * of this partition.
	 */
	if (ops->oobbuf) {
		size_t len, pages;

		if (ops->mode == MTD_OPS_AUTO_OOB)
			len = mtd->oobavail;
		else
			len = mtd->oobsize;
		pages = mtd_div_by_ws(mtd->size, mtd);
		pages -= mtd_div_by_ws(from, mtd);
		if (ops->ooboffs + ops->ooblen > pages * len)
			return -EINVAL;
	}

	res = part->master->_read_oob(part->master, from + part->offset, ops);
	if (unlikely(res)) {
		if (mtd_is_bitflip(res))
			mtd->ecc_stats.corrected++;
		if (mtd_is_eccerr(res))
			mtd->ecc_stats.failed++;
	}
	return res;
}

/* part_read_user_prot_reg 调用主MTD方法 */
static int part_read_user_prot_reg(struct mtd_info *mtd, loff_t from,
		size_t len, size_t *retlen, u_char *buf)
{
	struct mtd_part *part = PART(mtd);
	return part->master->_read_user_prot_reg(part->master, from, len,
						 retlen, buf);
}

/* part_get_user_prot_info 调用主MTD方法 */
static int part_get_user_prot_info(struct mtd_info *mtd,
		struct otp_info *buf, size_t len)
{
	struct mtd_part *part = PART(mtd);
	return part->master->_get_user_prot_info(part->master, buf, len);
}

/* part_read_fact_prot_reg 调用主MTD方法 */
static int part_read_fact_prot_reg(struct mtd_info *mtd, loff_t from,
		size_t len, size_t *retlen, u_char *buf)
{
	struct mtd_part *part = PART(mtd);
	return part->master->_read_fact_prot_reg(part->master, from, len,
						 retlen, buf);
}

/* part_get_fact_prot_info 调用主MTD方法 */
static int part_get_fact_prot_info(struct mtd_info *mtd, struct otp_info *buf,
		size_t len)
{
	struct mtd_part *part = PART(mtd);
	return part->master->_get_fact_prot_info(part->master, buf, len);
}

/* part_write 转换偏移地址，调用主MTD设备方法 */
static int part_write(struct mtd_info *mtd, loff_t to, size_t len,
		size_t *retlen, const u_char *buf)
{
	struct mtd_part *part = PART(mtd);
	return part->master->_write(part->master, to + part->offset, len,
				    retlen, buf);
}

/* part_panic_write 转换偏移地址，调用主MTD设备方法 */
static int part_panic_write(struct mtd_info *mtd, loff_t to, size_t len,
		size_t *retlen, const u_char *buf)
{
	struct mtd_part *part = PART(mtd);
	return part->master->_panic_write(part->master, to + part->offset, len,
					  retlen, buf);
}

/* part_write_oob 调用主MTD方法 */
static int part_write_oob(struct mtd_info *mtd, loff_t to,
		struct mtd_oob_ops *ops)
{
	struct mtd_part *part = PART(mtd);

	if (to >= mtd->size)
		return -EINVAL;
	if (ops->datbuf && to + ops->len > mtd->size)
		return -EINVAL;
	return part->master->_write_oob(part->master, to + part->offset, ops);
}

/* part_write_user_prot_reg 调用主MTD方法 */
static int part_write_user_prot_reg(struct mtd_info *mtd, loff_t from,
		size_t len, size_t *retlen, u_char *buf)
{
	struct mtd_part *part = PART(mtd);
	return part->master->_write_user_prot_reg(part->master, from, len,
						  retlen, buf);
}

/* part_lock_user_prot_reg 调用主MTD方法 */
static int part_lock_user_prot_reg(struct mtd_info *mtd, loff_t from,
		size_t len)
{
	struct mtd_part *part = PART(mtd);
	return part->master->_lock_user_prot_reg(part->master, from, len);
}

/* part_writev 调用主MTD方法 */
static int part_writev(struct mtd_info *mtd, const struct kvec *vecs,
		unsigned long count, loff_t to, size_t *retlen)
{
	struct mtd_part *part = PART(mtd);
	return part->master->_writev(part->master, vecs, count,
				     to + part->offset, retlen);
}

/* part_erase 调用主MTD方法 */
static int part_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct mtd_part *part = PART(mtd);
	int ret;

	instr->addr += part->offset;
	ret = part->master->_erase(part->master, instr);
	if (ret) {
		if (instr->fail_addr != MTD_FAIL_ADDR_UNKNOWN)
			instr->fail_addr -= part->offset;
		instr->addr -= part->offset;
	}
	return ret;
}

/* mtd 设备擦除回调，当执行完成擦除操作后，将会调用这个函数
 * 在 drivers/mtd/mtdchar.c 中的 MEMERASE/MEMERASE64 命令中，因为擦除操作耗时较
 * 久，为了防止一直处于等待状态，就设置为等待队列； 
 * 在 MEMERASE/MEMERASE64 命令中，设置了 callback 回调函数（唤醒队列） 
 * 这样执行了擦除后就可以进入休眠，当擦除完成后就调用该回调进行唤醒 */
void mtd_erase_callback(struct erase_info *instr)
{
	if (instr->mtd->_erase == part_erase) {
		struct mtd_part *part = PART(instr->mtd);

		if (instr->fail_addr != MTD_FAIL_ADDR_UNKNOWN)
			instr->fail_addr -= part->offset;
		instr->addr -= part->offset;
	}
	if (instr->callback)
		instr->callback(instr);
}
EXPORT_SYMBOL_GPL(mtd_erase_callback);

/* part_lock 调用主MTD方法 */
static int part_lock(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	struct mtd_part *part = PART(mtd);
	return part->master->_lock(part->master, ofs + part->offset, len);
}

/* part_unlock 调用主MTD方法 */
static int part_unlock(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	struct mtd_part *part = PART(mtd);
	return part->master->_unlock(part->master, ofs + part->offset, len);
}

/* part_is_locked 调用主MTD方法 */
static int part_is_locked(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	struct mtd_part *part = PART(mtd);
	return part->master->_is_locked(part->master, ofs + part->offset, len);
}

/* part_sync 调用主MTD方法 */
static void part_sync(struct mtd_info *mtd)
{
	struct mtd_part *part = PART(mtd);
	part->master->_sync(part->master);
}

/* part_suspend 调用主MTD方法 */
static int part_suspend(struct mtd_info *mtd)
{
	struct mtd_part *part = PART(mtd);
	return part->master->_suspend(part->master);
}

/* part_resume 调用主MTD方法 */
static void part_resume(struct mtd_info *mtd)
{
	struct mtd_part *part = PART(mtd);
	part->master->_resume(part->master);
}

/* part_block_isbad 调用主MTD方法 */
static int part_block_isbad(struct mtd_info *mtd, loff_t ofs)
{
	struct mtd_part *part = PART(mtd);
	ofs += part->offset;
	return part->master->_block_isbad(part->master, ofs);
}

/* part_block_markbad 调用主MTD方法 */
static int part_block_markbad(struct mtd_info *mtd, loff_t ofs)
{
	struct mtd_part *part = PART(mtd);
	int res;

	ofs += part->offset;
	res = part->master->_block_markbad(part->master, ofs);
	if (!res)
		mtd->ecc_stats.badblocks++;
	return res;
}

/* 释放mtd分区结构，所有分区结构都是自动分配的 */
static inline void free_partition(struct mtd_part *p)
{
	kfree(p->mtd.name);
	kfree(p);
}

/*
 * This function unregisters and destroy all slave MTD objects which are
 * attached to the given master MTD object.
 */

/* 移除 mtd设备的所有分区 */
int del_mtd_partitions(struct mtd_info *master)
{
	struct mtd_part *slave, *next;
	int ret, err = 0;

	mutex_lock(&mtd_partitions_mutex);
    /* 遍历所有已注册的 mtd分区 设备
     * 匹配原则是 mtd分区设备的主设备是参数中的主设备 
     * 匹配成功，则移除 mtd分区设备 ，释放分区 */
	list_for_each_entry_safe(slave, next, &mtd_partitions, list)
		if (slave->master == master) {
			ret = del_mtd_device(&slave->mtd);
			if (ret < 0) {
				err = ret;
				continue;
			}
			list_del(&slave->list);
			free_partition(slave);
		}
	mutex_unlock(&mtd_partitions_mutex);

	return err;
}

/* 创建mtd分区
 * master 是mtd主设备，是实际的mtd设备，物理存在的 
 * part 是要创建的分区信息，包含分区名，大小等信息 
 * partno 是分区号，从 0 开始 0表示主mtd设备，具有 _suspend 等回调 
 * cur_offset 是偏移量 */
static struct mtd_part *allocate_partition(struct mtd_info *master,
			const struct mtd_partition *part, int partno,
			uint64_t cur_offset)
{
	struct mtd_part *slave;
	char *name;

    /* 分配 mtd分区结构 
     * 分配 mtd分区名字，使用传入的分区表中的分区名 */
	/* allocate the partition structure */
	slave = kzalloc(sizeof(*slave), GFP_KERNEL);
	name = kstrdup(part->name, GFP_KERNEL);
	if (!name || !slave) {
		printk(KERN_ERR"memory allocation error while creating partitions for \"%s\"\n",
		       master->name);
		kfree(name);
		kfree(slave);
		return ERR_PTR(-ENOMEM);
	}

    /* 单个分区的配置信息，与 主MTD设备的配置信息 基本一致
     * 设备类型，擦写大小等 
     * 当然，分区的总大小 与 传入的分区表中的大小相同 */
	/* set up the MTD object for this partition */
	slave->mtd.type = master->type;
	slave->mtd.flags = master->flags & ~part->mask_flags;
	slave->mtd.size = part->size;
	slave->mtd.writesize = master->writesize;
	slave->mtd.writebufsize = master->writebufsize;
	slave->mtd.oobsize = master->oobsize;
	slave->mtd.oobavail = master->oobavail;
	slave->mtd.subpage_sft = master->subpage_sft;

    /* mtd分区名 与分区表中一致，不再与主mtd设备有关 */
	slave->mtd.name = name;
	slave->mtd.owner = master->owner;
	slave->mtd.backing_dev_info = master->backing_dev_info;

	/* NOTE:  we don't arrange MTDs as a tree; it'd be error-prone
	 * to have the same data be in two different partitions.
	 */
	slave->mtd.dev.parent = master->dev.parent;

    /* mtd分区的操作函数都换成了 part_ 开头的，
     * 这些函数在本文件中定义 
     * 其实这些函数最终也是调用了 主MTD设备的操作函数，但因为有了分区的原因，所
     * 以做了一些起始地址偏移等操作 
     * 除了读/写/擦除 基本操作，其它一些操作不是必须的，为了不在 part_ 函数中再
     * 次做判断，这里在赋值函数时根据 主MTD设备是否有对应回调函数 选择性赋值 */
	slave->mtd._read = part_read;
	slave->mtd._write = part_write;

	if (master->_panic_write)
		slave->mtd._panic_write = part_panic_write;

	if (master->_point && master->_unpoint) {
		slave->mtd._point = part_point;
		slave->mtd._unpoint = part_unpoint;
	}

	if (master->_get_unmapped_area)
		slave->mtd._get_unmapped_area = part_get_unmapped_area;
	if (master->_read_oob)
		slave->mtd._read_oob = part_read_oob;
	if (master->_write_oob)
		slave->mtd._write_oob = part_write_oob;
	if (master->_read_user_prot_reg)
		slave->mtd._read_user_prot_reg = part_read_user_prot_reg;
	if (master->_read_fact_prot_reg)
		slave->mtd._read_fact_prot_reg = part_read_fact_prot_reg;
	if (master->_write_user_prot_reg)
		slave->mtd._write_user_prot_reg = part_write_user_prot_reg;
	if (master->_lock_user_prot_reg)
		slave->mtd._lock_user_prot_reg = part_lock_user_prot_reg;
	if (master->_get_user_prot_info)
		slave->mtd._get_user_prot_info = part_get_user_prot_info;
	if (master->_get_fact_prot_info)
		slave->mtd._get_fact_prot_info = part_get_fact_prot_info;
	if (master->_sync)
		slave->mtd._sync = part_sync;
	if (!partno && !master->dev.class && master->_suspend &&
	    master->_resume) {
			slave->mtd._suspend = part_suspend;
			slave->mtd._resume = part_resume;
	}
	if (master->_writev)
		slave->mtd._writev = part_writev;
	if (master->_lock)
		slave->mtd._lock = part_lock;
	if (master->_unlock)
		slave->mtd._unlock = part_unlock;
	if (master->_is_locked)
		slave->mtd._is_locked = part_is_locked;
	if (master->_block_isbad)
		slave->mtd._block_isbad = part_block_isbad;
	if (master->_block_markbad)
		slave->mtd._block_markbad = part_block_markbad;
	slave->mtd._erase = part_erase;
    /* mtd分区的 master 指向 主MTD设备，这样就能找到属于谁了 */
	slave->master = master;
    /* 地址偏移使用分区表中的，这个很重要，决定了分区从哪开始 */
	slave->offset = part->offset;

    /* 分区偏移地址 几种特殊情况，不是提供的具体偏移地址时使用 */

    /* 从上个分区结束的位置开始，那直接使用 cur_offset 即可 */
	if (slave->offset == MTDPART_OFS_APPEND)
		slave->offset = cur_offset;

    /* 分区从下一个块开始，这种情况先处理 上一个分区结束地址（cur_offset）是否为
     * 块开始地址，如果是，则直接使用即可，如果不是，则偏移延伸至下个块起始 */
	if (slave->offset == MTDPART_OFS_NXTBLK) {
		slave->offset = cur_offset;
		if (mtd_mod_by_eb(cur_offset, master) != 0) {
			/* Round up to next erasesize */
			slave->offset = (mtd_div_by_eb(cur_offset, master) + 1) * master->erasesize;
			printk(KERN_NOTICE "Moving partition %d: "
			       "0x%012llx -> 0x%012llx\n", partno,
			       (unsigned long long)cur_offset, (unsigned long long)slave->offset);
		}
	}

    /* 偏移为 MTDPART_OFS_RETAIN 时，表示在预留出 size 大小的情况下，尽可能多的
     * 分配空间 
     * 偏移地址使用上个分区结束地址，并判断主MTD剩余空间是否足够预留空间大小，只
     * 要足够，就把所有剩余的空间分配了 
     * 注意，如果不够了，那无论如何都无法分配的，所以就直接放弃了。。。。 */
	if (slave->offset == MTDPART_OFS_RETAIN) {
		slave->offset = cur_offset;
		if (master->size - slave->offset >= slave->mtd.size) {
			slave->mtd.size = master->size - slave->offset
							- slave->mtd.size;
		} else {
			printk(KERN_ERR "mtd partition \"%s\" doesn't have enough space: %#llx < %#llx, disabled\n",
				part->name, master->size - slave->offset,
				slave->mtd.size);
			/* register to preserve ordering */
			goto out_register;
		}
	}

    /* 某分区预留大小为 MTDPART_SIZ_FULL 时，则将所有剩余空间分配给该分区  */
	if (slave->mtd.size == MTDPART_SIZ_FULL)
		slave->mtd.size = master->size - slave->offset;

	printk(KERN_NOTICE "0x%012llx-0x%012llx : \"%s\"\n", (unsigned long long)slave->offset,
		(unsigned long long)(slave->offset + slave->mtd.size), slave->mtd.name);

	/* let's do some sanity checks */
    /* 分区起始偏移超过了主MTD总大小，那就无法分配该分区了 */
	if (slave->offset >= master->size) {
		/* let's register it anyway to preserve ordering */
		slave->offset = 0;
		slave->mtd.size = 0;
		printk(KERN_ERR"mtd: partition \"%s\" is out of reach -- disabled\n",
			part->name);
		goto out_register;
	}
    /* 分区的结束地址超过了主MTD总大小，只能缩小分区了，按照实际剩余空间分配 */
	if (slave->offset + slave->mtd.size > master->size) {
		slave->mtd.size = master->size - slave->offset;
		printk(KERN_WARNING"mtd: partition \"%s\" extends beyond the end of device \"%s\" -- size truncated to %#llx\n",
			part->name, master->name, (unsigned long long)slave->mtd.size);
	}
    /* 多擦除区域时，需要特殊处理，找到当前分区属于哪个擦除区域，设置擦除大小 */
	if (master->numeraseregions > 1) {
		/* Deal with variable erase size stuff */
		int i, max = master->numeraseregions;
		u64 end = slave->offset + slave->mtd.size;
		struct mtd_erase_region_info *regions = master->eraseregions;

		/* Find the first erase regions which is part of this
		 * partition. */
		for (i = 0; i < max && regions[i].offset <= slave->offset; i++)
			;
		/* The loop searched for the region _behind_ the first one */
		if (i > 0)
			i--;

		/* Pick biggest erasesize */
		for (; i < max && regions[i].offset < end; i++) {
			if (slave->mtd.erasesize < regions[i].erasesize) {
				slave->mtd.erasesize = regions[i].erasesize;
			}
		}
		BUG_ON(slave->mtd.erasesize == 0);
	} else {
		/* Single erase size */
		slave->mtd.erasesize = master->erasesize;
	}

    /* 如果分区不在主擦除大小的边界上，则强制设置为只读 
     * 这里就会影响分区的起始地址设置，如果起始地址设置不合理，未在擦除边界，那
     * 就无法写入了，所以，设置分区时需特别注意 */
	if ((slave->mtd.flags & MTD_WRITEABLE) &&
	    mtd_mod_by_eb(slave->offset, &slave->mtd)) {
		/* Doesn't start on a boundary of major erase size */
		/* FIXME: Let it be writable if it is on a boundary of
		 * _minor_ erase size though */
		slave->mtd.flags &= ~MTD_WRITEABLE;
		printk(KERN_WARNING"mtd: partition \"%s\" doesn't start on an erase block boundary -- force read-only\n",
			part->name);
	}
    /* 如果分区结束地址不在主擦除大小的边界上，则强制设置为只读 */
	if ((slave->mtd.flags & MTD_WRITEABLE) &&
	    mtd_mod_by_eb(slave->mtd.size, &slave->mtd)) {
		slave->mtd.flags &= ~MTD_WRITEABLE;
		printk(KERN_WARNING"mtd: partition \"%s\" doesn't end on an erase block -- force read-only\n",
			part->name);
	}

    /* NOR flash 一般没有这些， NAND flash 需要提供 */
	slave->mtd.ecclayout = master->ecclayout;
	slave->mtd.ecc_strength = master->ecc_strength;
	slave->mtd.bitflip_threshold = master->bitflip_threshold;

	if (master->_block_isbad) {
		uint64_t offs = 0;

		while (offs < slave->mtd.size) {
			if (mtd_block_isbad(master, offs + slave->offset))
				slave->mtd.ecc_stats.badblocks++;
			offs += slave->mtd.erasesize;
		}
	}

out_register:
	return slave;
}

/* 在mtd主分区中添加一个分区
 * 第一个参数 master 必须是主mtd设备 */
int mtd_add_partition(struct mtd_info *master, char *name,
		      long long offset, long long length)
{
	struct mtd_partition part;
	struct mtd_part *p, *new;
	uint64_t start, end;
	int ret = 0;

	/* the direct offset is expected */
    /* offset 必须是有效的偏移地址，不能使用相对值 */
	if (offset == MTDPART_OFS_APPEND ||
	    offset == MTDPART_OFS_NXTBLK)
		return -EINVAL;

    /* 分区长度可以指定为偏移地址 到 结束地址 */
	if (length == MTDPART_SIZ_FULL)
		length = master->size - offset;

	if (length <= 0)
		return -EINVAL;

    /* 创建 mtd_partition 结构, 用户创建分区的参数 */
	part.name = name;
	part.size = length;
	part.offset = offset;
	part.mask_flags = 0;
	part.ecclayout = NULL;

    /* 根据分区参数，分配分区 mtd_part 结构，并初始化完成
     * -1 只是本分区并不是主mtd设备，不会设置 _suspend 等回调 */
    /* new 就是新创建的 mtd 分区对象了 */
	new = allocate_partition(master, &part, -1, offset);
	if (IS_ERR(new))
		return PTR_ERR(new);

	start = offset;
	end = offset + length;

    /* 遍历所有的 mtd分区 
     * p->master == master 说明遍历的这个分区与需要创建的新分区的主mtd设备是同一
     * 个，那这两个分区的地址空间不能有重叠 */
	mutex_lock(&mtd_partitions_mutex);
	list_for_each_entry(p, &mtd_partitions, list)
		if (p->master == master) {
            /* 判断起始地址是否有重叠，只要有重叠，则无法创建，并返回错误 */
			if ((start >= p->offset) &&
			    (start < (p->offset + p->mtd.size)))
				goto err_inv;

			if ((end >= p->offset) &&
			    (end < (p->offset + p->mtd.size)))
				goto err_inv;
		}

    /* 将新分区添加到分区链表中 */
	list_add(&new->list, &mtd_partitions);
	mutex_unlock(&mtd_partitions_mutex);

    /* 添加分区mtd到 mtd子系统中 */
	add_mtd_device(&new->mtd);

	return ret;
err_inv:
	mutex_unlock(&mtd_partitions_mutex);
	free_partition(new);
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(mtd_add_partition);

/* mtd 删除分区 */
int mtd_del_partition(struct mtd_info *master, int partno)
{
	struct mtd_part *slave, *next;
	int ret = -EINVAL;

    /* 遍历所有的mtd分区
     * 需要满足两个条件：
     * 1、mtd分区的主mtd设备与参数中的一致
     * 2、mtd分区的mtd编号与参数中的 partno 一致 
     * 找到后，移除 mtd 设备，释放mtd分区结构 */
	mutex_lock(&mtd_partitions_mutex);
	list_for_each_entry_safe(slave, next, &mtd_partitions, list)
		if ((slave->master == master) &&
		    (slave->mtd.index == partno)) {
			ret = del_mtd_device(&slave->mtd);
			if (ret < 0)
				break;

			list_del(&slave->list);
			free_partition(slave);
			break;
		}
	mutex_unlock(&mtd_partitions_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(mtd_del_partition);

/*
 * This function, given a master MTD object and a partition table, creates
 * and registers slave MTD objects which are bound to the master according to
 * the partition definitions.
 *
 * We don't register the master, or expect the caller to have done so,
 * for reasons of data integrity.
 */
/* 给定一个主MTD对象和一个分区表，该函数创建并注册根据分区表定义绑定到主MTD对象
 * 的从MTD对象 
 * 出于数据完整性的原因，我们不注册主MTD，也不期望调用者注册主MTD */
int add_mtd_partitions(struct mtd_info *master,
		       const struct mtd_partition *parts,
		       int nbparts)
{
	struct mtd_part *slave;
	uint64_t cur_offset = 0;
	int i;

	printk(KERN_NOTICE "Creating %d MTD partitions on \"%s\":\n", nbparts, master->name);

    /* 根据提供的分区表，依次创建mtd分区，并以分区mtd进行注册mtd设备 */
	for (i = 0; i < nbparts; i++) {
        /* 经过 allocate_partition 之后，将会把每个分区（parts提供）做成一个 mtd_part 
         * 而每个 mtd_part 中会有一个 mtd_info ，所有的分区相关信息都在这个
         * mtd_info 中，就把分区看成一个独立的 mtd 设备 
         * 在 mtd_part 中，又指向了所属的主MTD设备，是因为在实际操作分区时，还是
         * 需要主MTD的回调函数的 */
		slave = allocate_partition(master, parts + i, i, cur_offset);
		if (IS_ERR(slave))
			return PTR_ERR(slave);

        /* 所有的 mtd_part 都链到 mtd_partitions 中，方便查找使用 */
		mutex_lock(&mtd_partitions_mutex);
		list_add(&slave->list, &mtd_partitions);
		mutex_unlock(&mtd_partitions_mutex);

        /* 添加MTD设备，这里就会把 mtd0 mtd0ro 注册了 */
		add_mtd_device(&slave->mtd);

		cur_offset = slave->offset + slave->mtd.size;
	}

	return 0;
}

static DEFINE_SPINLOCK(part_parser_lock);
static LIST_HEAD(part_parsers);

/* 获取特定的 mtd分区解析器 
 * 注册的解析器都在 part_parsers 链表中，并提供了解析函数 和 解析器名字 
 * 本函数中通过解析器名字查找 */
static struct mtd_part_parser *get_partition_parser(const char *name)
{
	struct mtd_part_parser *p, *ret = NULL;

	spin_lock(&part_parser_lock);

	list_for_each_entry(p, &part_parsers, list)
		if (!strcmp(p->name, name) && try_module_get(p->owner)) {
			ret = p;
			break;
		}

	spin_unlock(&part_parser_lock);

	return ret;
}

#define put_partition_parser(p) do { module_put((p)->owner); } while (0)

/* 注册分区解析器，可由各个驱动模块提供，实现特定的分区类型解析 
 * 默认的有 drivers/mtd/cmdlinepart.c drivers/mtd/ofpart.c */
int register_mtd_parser(struct mtd_part_parser *p)
{
	spin_lock(&part_parser_lock);
	list_add(&p->list, &part_parsers);
	spin_unlock(&part_parser_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(register_mtd_parser);

/* 删除分区解析器 */
int deregister_mtd_parser(struct mtd_part_parser *p)
{
	spin_lock(&part_parser_lock);
	list_del(&p->list);
	spin_unlock(&part_parser_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(deregister_mtd_parser);

/*
 * Do not forget to update 'parse_mtd_partitions()' kerneldoc comment if you
 * are changing this array!
 */
/* 默认的 mtd 分区解析器类型
 * 这个是各个驱动中注册进来的，用来解析特有的分区类型
 * 如 cmdlinepart 是在 drivers/mtd/cmdlinepart.c 中注册
 * 如 ofpart 类型，是在 drivers/mtd/ofpart.c 中注册，并且提供了对应的解析函数 */
static const char *default_mtd_part_types[] = {
	"cmdlinepart",
	"ofpart",
	NULL
};

/**
 * parse_mtd_partitions - parse MTD partitions
 * @master: the master partition (describes whole MTD device)
 * @types: names of partition parsers to try or %NULL
 * @pparts: array of partitions found is returned here
 * @data: MTD partition parser-specific data
 *
 * This function tries to find partition on MTD device @master. It uses MTD
 * partition parsers, specified in @types. However, if @types is %NULL, then
 * the default list of parsers is used. The default list contains only the
 * "cmdlinepart" and "ofpart" parsers ATM.
 *
 * This function may return:
 * o a negative error code in case of failure
 * o zero if no partitions were found
 * o a positive number of found partitions, in which case on exit @pparts will
 *   point to an array containing this number of &struct mtd_info objects.
 */
/* 解析mtd分区信息 
 * 如果传进来的 types 为NULL，则默认使用上面的 default_mtd_part_types 中的分区解
 * 析方式，当然，某些驱动中会传入 types 以采用特定的分区解析函数 */
int parse_mtd_partitions(struct mtd_info *master, const char **types,
			 struct mtd_partition **pparts,
			 struct mtd_part_parser_data *data)
{
	struct mtd_part_parser *parser;
	int ret = 0;

    /* 未指定特有的分区解析器，则使用默认的 default_mtd_part_types  */
	if (!types)
		types = default_mtd_part_types;

    /* 通过 get_partition_parser 找到对应解析器 
     * 当然，解析可能是以模块的方式提供的，有可能并未加载 
     * 所以使用 request_module 进行动态加载，request_module 用于实现动态加载内核
     * 模块 */
	for ( ; ret <= 0 && *types; types++) {
		parser = get_partition_parser(*types);
		if (!parser && !request_module("%s", *types))
			parser = get_partition_parser(*types);
		if (!parser)
			continue;
        /* parse_fn 是分区解析器真正的解析函数，在各个模块中提供 
         * 解析成功的分区会由 pparts 返回 
         * 第三个参数 data 用于 设备树解析方式， 在一般的解析中（cmdlinepart）
         * 该参数是未使用的 */
		ret = (*parser->parse_fn)(master, pparts, data);
		if (ret > 0) {
			printk(KERN_NOTICE "%d %s partitions found on MTD device %s\n",
			       ret, parser->name, master->name);
		}
		put_partition_parser(parser);
	}
    /* 到这里就把分区解析完成了
     * 总结一下，分区的解析有多种方式，大类可以分为3种：
     * cmdlinepart 
     * ofpart
     * 模块特定的 
     * 无论是哪种方式，都是先注册了对应的解析器，在调用本函数时指定解析器名字
     * 这种好处是某些厂家/芯片驱动可以定制自己特有的分区类型 
     *
     * 如果 cmdlinepart 和 ofpart 两种方式都没有提供对应的分区信息，那本函数只能
     * 返回 0 了，也就是没找到分区信息 */
	return ret;
}

/* 判断一个 mtd设备是否为 分区设备 
 * 所有的分区MTD设备都会链到 mtd_partitions 链表中，只要在这个链表中
 * 那就是分区MTD设备 */
int mtd_is_partition(struct mtd_info *mtd)
{
	struct mtd_part *part;
	int ispart = 0;

	mutex_lock(&mtd_partitions_mutex);
	list_for_each_entry(part, &mtd_partitions, list)
		if (&part->mtd == mtd) {
			ispart = 1;
			break;
		}
	mutex_unlock(&mtd_partitions_mutex);

	return ispart;
}
EXPORT_SYMBOL_GPL(mtd_is_partition);
