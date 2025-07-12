/*
 * Core registration and callback routines for MTD
 * drivers and users.
 *
 * Copyright © 1999-2010 David Woodhouse <dwmw2@infradead.org>
 * Copyright © 2006      Red Hat UK Limited 
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
#include <linux/kernel.h>
#include <linux/ptrace.h>
#include <linux/seq_file.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/major.h>
#include <linux/fs.h>
#include <linux/err.h>
#include <linux/ioctl.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include <linux/idr.h>
#include <linux/backing-dev.h>
#include <linux/gfp.h>

#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>

#include "mtdcore.h"

/* backing device info 驱动 
 * 与数据回写有关，此处不分析 */
/*
 * backing device capabilities for non-mappable devices (such as NAND flash)
 * - permits private mappings, copies are taken of the data
 */
static struct backing_dev_info mtd_bdi_unmappable = {
	.capabilities	= BDI_CAP_MAP_COPY,
};

/*
 * backing device capabilities for R/O mappable devices (such as ROM)
 * - permits private mappings, copies are taken of the data
 * - permits non-writable shared mappings
 */
static struct backing_dev_info mtd_bdi_ro_mappable = {
	.capabilities	= (BDI_CAP_MAP_COPY | BDI_CAP_MAP_DIRECT |
			   BDI_CAP_EXEC_MAP | BDI_CAP_READ_MAP),
};

/*
 * backing device capabilities for writable mappable devices (such as RAM)
 * - permits private mappings, copies are taken of the data
 * - permits non-writable shared mappings
 */
static struct backing_dev_info mtd_bdi_rw_mappable = {
	.capabilities	= (BDI_CAP_MAP_COPY | BDI_CAP_MAP_DIRECT |
			   BDI_CAP_EXEC_MAP | BDI_CAP_READ_MAP |
			   BDI_CAP_WRITE_MAP),
};

static int mtd_cls_suspend(struct device *dev, pm_message_t state);
static int mtd_cls_resume(struct device *dev);

/* mtd class , 所有mtd设备均属于 mtd class */
static struct class mtd_class = {
	.name = "mtd",
	.owner = THIS_MODULE,
	.suspend = mtd_cls_suspend,
	.resume = mtd_cls_resume,
};

/* idr 数据结构
 * 注册mtd 设备时，用 idr 数据结构分配 mtd编号
 * 一定是 从 0 开始分配的 */
static DEFINE_IDR(mtd_idr);

/* These are exported solely for the purpose of mtd_blkdevs.c. You
   should not use them for _anything_ else */
DEFINE_MUTEX(mtd_table_mutex);
EXPORT_SYMBOL_GPL(mtd_table_mutex);

/* 按照 idr 序号，查找 mtd 设备，返回 mtd_info 结构 */
struct mtd_info *__mtd_next_device(int i)
{
	return idr_get_next(&mtd_idr, &i);
}
EXPORT_SYMBOL_GPL(__mtd_next_device);

/* mtd 设备注册时的通知机制
 * 用于其它驱动中注册回调函数，解析特定的 mtd 设备 */
static LIST_HEAD(mtd_notifiers);


/* 按照 mtd设备的序号生成 mtd设备号
 * mtd的字符设备主设备号是确定的，次设备号由索引生成 */
#if defined(CONFIG_MTD_CHAR) || defined(CONFIG_MTD_CHAR_MODULE)
#define MTD_DEVT(index) MKDEV(MTD_CHAR_MAJOR, (index)*2)
#else
#define MTD_DEVT(index) 0
#endif

/* REVISIT once MTD uses the driver model better, whoever allocates
 * the mtd_info will probably want to use the release() hook...
 */
/* mtd device_type .release 回调函数 */
static void mtd_release(struct device *dev)
{
	struct mtd_info __maybe_unused *mtd = dev_get_drvdata(dev);
	dev_t index = MTD_DEVT(mtd->index);

	/* remove /dev/mtdXro node if needed */
	if (index)
		device_destroy(&mtd_class, index + 1);
}

/* mtd class 回调函数 */
static int mtd_cls_suspend(struct device *dev, pm_message_t state)
{
	struct mtd_info *mtd = dev_get_drvdata(dev);

	return mtd ? mtd_suspend(mtd) : 0;
}

static int mtd_cls_resume(struct device *dev)
{
	struct mtd_info *mtd = dev_get_drvdata(dev);

	if (mtd)
		mtd_resume(mtd);
	return 0;
}

/* /sys/class/mtd/mtd0/ 属性文件，mtd设备的公共信息 */
static ssize_t mtd_type_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mtd_info *mtd = dev_get_drvdata(dev);
	char *type;

	switch (mtd->type) {
	case MTD_ABSENT:
		type = "absent";
		break;
	case MTD_RAM:
		type = "ram";
		break;
	case MTD_ROM:
		type = "rom";
		break;
	case MTD_NORFLASH:
		type = "nor";
		break;
	case MTD_NANDFLASH:
		type = "nand";
		break;
	case MTD_DATAFLASH:
		type = "dataflash";
		break;
	case MTD_UBIVOLUME:
		type = "ubi";
		break;
	default:
		type = "unknown";
	}

	return snprintf(buf, PAGE_SIZE, "%s\n", type);
}
static DEVICE_ATTR(type, S_IRUGO, mtd_type_show, NULL);

static ssize_t mtd_flags_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mtd_info *mtd = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "0x%lx\n", (unsigned long)mtd->flags);

}
static DEVICE_ATTR(flags, S_IRUGO, mtd_flags_show, NULL);

static ssize_t mtd_size_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mtd_info *mtd = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%llu\n",
		(unsigned long long)mtd->size);

}
static DEVICE_ATTR(size, S_IRUGO, mtd_size_show, NULL);

static ssize_t mtd_erasesize_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mtd_info *mtd = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%lu\n", (unsigned long)mtd->erasesize);

}
static DEVICE_ATTR(erasesize, S_IRUGO, mtd_erasesize_show, NULL);

static ssize_t mtd_writesize_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mtd_info *mtd = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%lu\n", (unsigned long)mtd->writesize);

}
static DEVICE_ATTR(writesize, S_IRUGO, mtd_writesize_show, NULL);

static ssize_t mtd_subpagesize_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mtd_info *mtd = dev_get_drvdata(dev);
	unsigned int subpagesize = mtd->writesize >> mtd->subpage_sft;

	return snprintf(buf, PAGE_SIZE, "%u\n", subpagesize);

}
static DEVICE_ATTR(subpagesize, S_IRUGO, mtd_subpagesize_show, NULL);

static ssize_t mtd_oobsize_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mtd_info *mtd = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%lu\n", (unsigned long)mtd->oobsize);

}
static DEVICE_ATTR(oobsize, S_IRUGO, mtd_oobsize_show, NULL);

static ssize_t mtd_numeraseregions_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mtd_info *mtd = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%u\n", mtd->numeraseregions);

}
static DEVICE_ATTR(numeraseregions, S_IRUGO, mtd_numeraseregions_show,
	NULL);

static ssize_t mtd_name_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct mtd_info *mtd = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%s\n", mtd->name);

}
static DEVICE_ATTR(name, S_IRUGO, mtd_name_show, NULL);

static ssize_t mtd_ecc_strength_show(struct device *dev,
				     struct device_attribute *attr, char *buf)
{
	struct mtd_info *mtd = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%u\n", mtd->ecc_strength);
}
static DEVICE_ATTR(ecc_strength, S_IRUGO, mtd_ecc_strength_show, NULL);

static ssize_t mtd_bitflip_threshold_show(struct device *dev,
					  struct device_attribute *attr,
					  char *buf)
{
	struct mtd_info *mtd = dev_get_drvdata(dev);

	return snprintf(buf, PAGE_SIZE, "%u\n", mtd->bitflip_threshold);
}

static ssize_t mtd_bitflip_threshold_store(struct device *dev,
					   struct device_attribute *attr,
					   const char *buf, size_t count)
{
	struct mtd_info *mtd = dev_get_drvdata(dev);
	unsigned int bitflip_threshold;
	int retval;

	retval = kstrtouint(buf, 0, &bitflip_threshold);
	if (retval)
		return retval;

	mtd->bitflip_threshold = bitflip_threshold;
	return count;
}
static DEVICE_ATTR(bitflip_threshold, S_IRUGO | S_IWUSR,
		   mtd_bitflip_threshold_show,
		   mtd_bitflip_threshold_store);

static struct attribute *mtd_attrs[] = {
	&dev_attr_type.attr,
	&dev_attr_flags.attr,
	&dev_attr_size.attr,
	&dev_attr_erasesize.attr,
	&dev_attr_writesize.attr,
	&dev_attr_subpagesize.attr,
	&dev_attr_oobsize.attr,
	&dev_attr_numeraseregions.attr,
	&dev_attr_name.attr,
	&dev_attr_ecc_strength.attr,
	&dev_attr_bitflip_threshold.attr,
	NULL,
};

static struct attribute_group mtd_group = {
	.attrs		= mtd_attrs,
};

static const struct attribute_group *mtd_groups[] = {
	&mtd_group,
	NULL,
};

/* 所有MTD设备的 type , 包含了公共使用的属性
 * 这样每个注册的MTD设备，都有相同的属性可以查询 */
static struct device_type mtd_devtype = {
	.name		= "mtd",
	.groups		= mtd_groups,
	.release	= mtd_release,
};

/**
 *	add_mtd_device - register an MTD device
 *	@mtd: pointer to new MTD device info structure
 *
 *	Add a device to the list of MTD devices present in the system, and
 *	notify each currently active MTD 'user' of its arrival. Returns
 *	zero on success or 1 on failure, which currently will only happen
 *	if there is insufficient memory or a sysfs error.
 */
/* 将一个MTD设备添加到系统中存在的MTD设备列表中，并通知每个当前活动的MTD“user”它
 * 的到来。 成功时返回0，失败时返回1，目前只有在内存不足或出现 sysfs 错误时才会
 * 发生这种情况。 */
int add_mtd_device(struct mtd_info *mtd)
{
	struct mtd_notifier *not;
	int i, error;

    /* backing_dev_info 是内核bdi系统中的结构，与块设备相关
     * 这里先不做分析 */
	if (!mtd->backing_dev_info) {
		switch (mtd->type) {
		case MTD_RAM:
			mtd->backing_dev_info = &mtd_bdi_rw_mappable;
			break;
		case MTD_ROM:
			mtd->backing_dev_info = &mtd_bdi_ro_mappable;
			break;
		default:
			mtd->backing_dev_info = &mtd_bdi_unmappable;
			break;
		}
	}

	BUG_ON(mtd->writesize == 0);
	mutex_lock(&mtd_table_mutex);

    /* IDR是一种基于基数树(Radix Tree) 的数据结构，用于高效的分配和管理整数ID
     * idr_pre_get 用于预分配足够的内存
     * idr_get_new 用于从IDR结构中分配一个新的唯一ID */
	do {
		if (!idr_pre_get(&mtd_idr, GFP_KERNEL))
			goto fail_locked;
		error = idr_get_new(&mtd_idr, mtd, &i);
	} while (error == -EAGAIN);

	if (error)
		goto fail_locked;

    /* mtd->index 基于 IDR 的唯一ID 
     * 默认是从 0 开始分配 */
	mtd->index = i;
	mtd->usecount = 0;

    /* 设置一些默认值，仅在mtd设备驱动中未设置时 */
	/* default value if not set by driver */
	if (mtd->bitflip_threshold == 0)
		mtd->bitflip_threshold = mtd->ecc_strength;

    /* 判断 erasesize 和 writesize 是否为 2 的整数幂
     * 如果是 2的整数幂，可以设置 xx_shift ，一些计算会比较方便
     * xxx_mask 是对应掩码，也是为了方便计算
     * 如果擦除/写入不是 2的整数幂，那在计算一些地址/数量时，
     * 就无法使用移位操作，需要使用除法 div */
	if (is_power_of_2(mtd->erasesize))
		mtd->erasesize_shift = ffs(mtd->erasesize) - 1;
	else
		mtd->erasesize_shift = 0;

	if (is_power_of_2(mtd->writesize))
		mtd->writesize_shift = ffs(mtd->writesize) - 1;
	else
		mtd->writesize_shift = 0;

	mtd->erasesize_mask = (1 << mtd->erasesize_shift) - 1;
	mtd->writesize_mask = (1 << mtd->writesize_shift) - 1;

	/* Some chips always power up locked. Unlock them now */
    /* 一些mtd上电时默认是锁的，如果启用了写，需要解锁一下 */
	if ((mtd->flags & MTD_WRITEABLE) && (mtd->flags & MTD_POWERUP_LOCK)) {
		error = mtd_unlock(mtd, 0, mtd->size);
		if (error && error != -EOPNOTSUPP)
			printk(KERN_WARNING
			       "%s: unlock failed, writes may not work\n",
			       mtd->name);
	}

	/* Caller should have set dev.parent to match the
	 * physical device.
	 */
    /* 设置 device 的成员 
     * mtd_devtype 是通用的设备属性，所有的mtd都会有的，会在 /sys/class/mtd/ 中
     * 显示的属性文件
     * mtd_class 用于添加到 mtd class 中，在 /sys/class/mtd/ 中 
     * MTD_DEVT(i) 用于生成设备号,  
     * 这是mtd字符设备设备号， 有两个字符设备: mtd0 mtd0ro 
     * 它们的主设备号相同（90），此设备号为 0， 1
     *  dev_set_drvdata(&mtd->dev, mtd); 这个很重要，是在设备属性文件操作时用于
     * 反查 MTD 设备 */
	mtd->dev.type = &mtd_devtype;
	mtd->dev.class = &mtd_class;
	mtd->dev.devt = MTD_DEVT(i);
	dev_set_name(&mtd->dev, "mtd%d", i);
	dev_set_drvdata(&mtd->dev, mtd);
	if (device_register(&mtd->dev) != 0)
		goto fail_added;

	if (MTD_DEVT(i))
		device_create(&mtd_class, mtd->dev.parent,
			      MTD_DEVT(i) + 1,
			      NULL, "mtd%dro", i);

	pr_debug("mtd: Giving out device %d to %s\n", i, mtd->name);
	/* No need to get a refcount on the module containing
	   the notifier, since we hold the mtd_table_mutex */
    /* 不需要在包含通知器的模块上获取 refcount，因为这里还持有 mtd_table_mutex  */
    /* mtd_notifiers 是一种功能扩展，其它的模块中注册 notifier 回调函数，
     * 在这里，当新注册一个 MTD设备时，就将所有的 notifier 回调调用一次，
     * 这样可以对某些 MTD分区做一些功能扩展 
     * 最明显的是 mtdoops.c 中的功能，mtdoops.c 中实现了当内核 Oops时，将内核崩
     * 溃日志转储到MTD设备中，mtdoops.c 模块中，注册了 mtd_notifiers 回调，
     * 当注册MTD设备时，会将MTD设备传递到 notifier 回调中，这样就可以匹配MTD设备，
     * 做一些初始化的工作 
     * !!!! 这里有个非常重要的 mtd_notifiers ： blktrans_notifier 
     * 这是实现MTD块设备的关键, 具体参看 mtd_blkdevs.c */
	list_for_each_entry(not, &mtd_notifiers, list)
		not->add(mtd);

	mutex_unlock(&mtd_table_mutex);
	/* We _know_ we aren't being removed, because
	   our caller is still holding us here. So none
	   of this try_ nonsense, and no bitching about it
	   either. :) */
	__module_get(THIS_MODULE);
	return 0;

fail_added:
	idr_remove(&mtd_idr, i);
fail_locked:
	mutex_unlock(&mtd_table_mutex);
	return 1;
}

/**
 *	del_mtd_device - unregister an MTD device
 *	@mtd: pointer to MTD device info structure
 *
 *	Remove a device from the list of MTD devices present in the system,
 *	and notify each currently active MTD 'user' of its departure.
 *	Returns zero on success or 1 on failure, which currently will happen
 *	if the requested device does not appear to be present in the list.
 */

/* 注销一个 mtd设备
 * 注销 mtd设备，还需要通知到每一个使用该 mtd的 'user' */
int del_mtd_device(struct mtd_info *mtd)
{
	int ret;
	struct mtd_notifier *not;

	mutex_lock(&mtd_table_mutex);

    /* 使用 mtd设备 索引 查找，确认需要注销的mtd设备 */
	if (idr_find(&mtd_idr, mtd->index) != mtd) {
		ret = -ENODEV;
		goto out_error;
	}

	/* No need to get a refcount on the module containing
		the notifier, since we hold the mtd_table_mutex */
    /* 遍历所有注册的 mtd_notifiers, 通知它们要注销这个 mtd设备了 */
	list_for_each_entry(not, &mtd_notifiers, list)
		not->remove(mtd);

    /* usecount 指示当前使用计数，必须全部释放才可以注销 */
	if (mtd->usecount) {
		printk(KERN_NOTICE "Removing MTD device #%d (%s) with use count %d\n",
		       mtd->index, mtd->name, mtd->usecount);
		ret = -EBUSY;
	} else {
        /* 注销 mtd device  */
		device_unregister(&mtd->dev);

        /* 移除 idr 中的数据 */
		idr_remove(&mtd_idr, mtd->index);

		module_put(THIS_MODULE);
		ret = 0;
	}

out_error:
	mutex_unlock(&mtd_table_mutex);
	return ret;
}

/**
 * mtd_device_parse_register - parse partitions and register an MTD device.
 *
 * @mtd: the MTD device to register
 * @types: the list of MTD partition probes to try, see
 *         'parse_mtd_partitions()' for more information
 * @parser_data: MTD partition parser-specific data
 * @parts: fallback partition information to register, if parsing fails;
 *         only valid if %nr_parts > %0
 * @nr_parts: the number of partitions in parts, if zero then the full
 *            MTD device is registered if no partition info is found
 *
 * This function aggregates MTD partitions parsing (done by
 * 'parse_mtd_partitions()') and MTD device and partitions registering. It
 * basically follows the most common pattern found in many MTD drivers:
 *
 * * It first tries to probe partitions on MTD device @mtd using parsers
 *   specified in @types (if @types is %NULL, then the default list of parsers
 *   is used, see 'parse_mtd_partitions()' for more information). If none are
 *   found this functions tries to fallback to information specified in
 *   @parts/@nr_parts.
 * * If any partitioning info was found, this function registers the found
 *   partitions.
 * * If no partitions were found this function just registers the MTD device
 *   @mtd and exits.
 *
 * Returns zero in case of success and a negative error code in case of failure.
 */
/* mtd_device_parse_register - 解析分区并注册MTD设备
 * @mtd: mtd设备注册 
 * @types: 要尝试的MTD分区探测列表，请参阅 'parse_mtd_partitions' 了解更多信息 
 * @parser_data: 特定于MTD分区解析器的数据
 * @parts: 如果解析失败，要注册的回退分区信息；仅在 nr_parts > 0 时有效
 * @nr_parts: 分区的数量，如果为零，则在没有找到分区信息的情况下注册完整的MTD设
 * 备
 *
 * 本函数集合 MTD分区解析（通过 'parse_mtd_partitions' 完成）和 MTD设备和分区注
 * 册。它基本上遵循许多MTD驱动程序中最常见的模式：
 * * 首先使用 @types 中指定的解析器探测MTD设备@mtd上的分区（如果@types为NULL，则
 * 使用默认的解析器列表，请参阅 'parse_mtd_partitions' 了解更多细节）。如果没有
 * 找到，这个函数会尝试回退到 parts/nr_parts 中指定的信息。
 * 备注： types 的参数形式如下： 
 * static char const *part_probes[] = { "cmdlinepart", "saftlpart", NULL };
 * * 如果找到任何分区信息，该函数将注册找到的分区。
 * * 如果没有找到分区，这个函数只注册MTD设备并推出 
 * */
int mtd_device_parse_register(struct mtd_info *mtd, const char **types,
			      struct mtd_part_parser_data *parser_data,
			      const struct mtd_partition *parts,
			      int nr_parts)
{
	int err;
	struct mtd_partition *real_parts;

    /* 解析MTD分区信息，这是根据 cmdline / of 等方式进行解析，
     * parse_mtd_partitions 并未涉及到板级信息中的分区信息
     * 当然，该函数可能无法成功解析，因为并未提供上述信息。。。 
     * 如果解析失败，则会使用 parts/nr_parts 提供的板级分区信息 */
	err = parse_mtd_partitions(mtd, types, &real_parts, parser_data);
	if (err <= 0 && nr_parts && parts) {
        /* 未解析到任何分区时，则使用 parts 提供的板级分区信息 */
		real_parts = kmemdup(parts, sizeof(*parts) * nr_parts,
				     GFP_KERNEL);
		if (!real_parts)
			err = -ENOMEM;
		else
			err = nr_parts;
	}

    /* err > 0 时，表示有正确的分区信息，使用 real_parts 
     * err = 0 时，表示无任何分区，则添加完整的 MTD设备 
     * err < 0 时，存在错误 */
	if (err > 0) {
        /* 到这里，real_parts 就是最终要使用的分区信息了
         * 无论是从启动参数，设备树还是板级信息中解析到的 */
		err = add_mtd_partitions(mtd, real_parts, err);
		kfree(real_parts);
	} else if (err == 0) {
		err = add_mtd_device(mtd);
		if (err == 1)
			err = -ENODEV;
	}

	return err;
}
EXPORT_SYMBOL_GPL(mtd_device_parse_register);

/**
 * mtd_device_unregister - unregister an existing MTD device.
 *
 * @master: the MTD device to unregister.  This will unregister both the master
 *          and any partitions if registered.
 */
/* 注销一个 mtd设备
 * 注意：这里的参数是 master，说明这是一个mtd主设备
 * 并非 mtd分区设备 */
int mtd_device_unregister(struct mtd_info *master)
{
	int err;

    /* 移除该 mtd设备下的所有分区 */
	err = del_mtd_partitions(master);
	if (err)
		return err;

    /* 这里需要判断参数提供的主设备是否注册到 device 了
     * 因为在mtd注册时，如果 主mtd 有分区，则只会注册分区，并不会注册主mtd设备 */
	if (!device_is_registered(&master->dev))
		return 0;

    /* 主mtd已注册，则移除 */
	return del_mtd_device(master);
}
EXPORT_SYMBOL_GPL(mtd_device_unregister);

/**
 *	register_mtd_user - register a 'user' of MTD devices.
 *	@new: pointer to notifier info structure
 *
 *	Registers a pair of callbacks function to be called upon addition
 *	or removal of MTD devices. Causes the 'add' callback to be immediately
 *	invoked for each MTD device currently present in the system.
 */
/* 注册一个 ‘user’ 
 * 注册一对在添加或删除 mtd设备时调用的回调函数。
 * 在为系统中当前存在的每个 mtd设备注册时，将调用 'add' 回调函数 */
void register_mtd_user (struct mtd_notifier *new)
{
	struct mtd_info *mtd;

	mutex_lock(&mtd_table_mutex);

    /* 添加新的 mtd_notifier 到链表中 */
	list_add(&new->list, &mtd_notifiers);

	__module_get(THIS_MODULE);

    /* 添加时，也会遍历所有已经存在的 mtd设备，并调用 add 回调函数 */
	mtd_for_each_device(mtd)
		new->add(mtd);

	mutex_unlock(&mtd_table_mutex);
}
EXPORT_SYMBOL_GPL(register_mtd_user);

/**
 *	unregister_mtd_user - unregister a 'user' of MTD devices.
 *	@old: pointer to notifier info structure
 *
 *	Removes a callback function pair from the list of 'users' to be
 *	notified upon addition or removal of MTD devices. Causes the
 *	'remove' callback to be immediately invoked for each MTD device
 *	currently present in the system.
 */
/* 注销一个 'user' */
int unregister_mtd_user (struct mtd_notifier *old)
{
	struct mtd_info *mtd;

	mutex_lock(&mtd_table_mutex);

	module_put(THIS_MODULE);

    /* 遍历所有的 mtd设备，调用 remove 回调函数 */
	mtd_for_each_device(mtd)
		old->remove(mtd);

    /* 从链表中移除 */
	list_del(&old->list);
	mutex_unlock(&mtd_table_mutex);
	return 0;
}
EXPORT_SYMBOL_GPL(unregister_mtd_user);

/**
 *	get_mtd_device - obtain a validated handle for an MTD device
 *	@mtd: last known address of the required MTD device
 *	@num: internal device number of the required MTD device
 *
 *	Given a number and NULL address, return the num'th entry in the device
 *	table, if any.	Given an address and num == -1, search the device table
 *	for a device with that address and return if it's still present. Given
 *	both, return the num'th driver only if its address matches. Return
 *	error code if not.
 */
/* 获取MTD设备的有效句柄
 * mtd: 所需MTD设备的最后已知地址
 * num: 所需MTD设备的内部设备号 
 *
 * 给定一个数字和NULL地址，返回设备表中的第 num 个条目（如果有）。
 * 给定一个地址和 -1，在设备表中搜索具有该地址的设备，如果它存在。
 * 给定两者，仅当其地址匹配时返回第 num个驱动程序。 */
struct mtd_info *get_mtd_device(struct mtd_info *mtd, int num)
{
	struct mtd_info *ret = NULL, *other;
	int err = -ENODEV;

	mutex_lock(&mtd_table_mutex);

	if (num == -1) {
        /* 只提供 mtd地址，未提供 num序号
         * 则遍历所有的 mtd 设备，寻找匹配的 */
		mtd_for_each_device(other) {
			if (other == mtd) {
				ret = mtd;
				break;
			}
		}
	} else if (num >= 0) {
        /* 对于提供 num 的情况，则先使用 num 查找 mtd_info
         * 结果只有两种情况: 有效 mtd_info or NULL
         * 如果参数中还提供了 mtd 结构地址，必须也得匹配才可以 */
		ret = idr_find(&mtd_idr, num);
		if (mtd && mtd != ret)
			ret = NULL;
	}

    /* 若无匹配 mtd 设备，则出错返回 */
	if (!ret) {
		ret = ERR_PTR(err);
		goto out;
	}

    /* 获取访问权限 */
	err = __get_mtd_device(ret);
	if (err)
		ret = ERR_PTR(err);
out:
	mutex_unlock(&mtd_table_mutex);
	return ret;
}
EXPORT_SYMBOL_GPL(get_mtd_device);


/* 获取 mtd设备的访问权限
 * 部分 mtd设备具有 _get_device 回调，需要处理一下
 * ubi 注册的 mtd 设备 具有 _get_device 回调 */
int __get_mtd_device(struct mtd_info *mtd)
{
	int err;

	if (!try_module_get(mtd->owner))
		return -ENODEV;

    /* 处理 _get_device 回调函数
     * 这是具体的 mtd 设备驱动提供的 */
	if (mtd->_get_device) {
		err = mtd->_get_device(mtd);

		if (err) {
			module_put(mtd->owner);
			return err;
		}
	}
    /* 使用计数递增，主要是在移除 mtd设备时判断是否已无其他使用者 */
	mtd->usecount++;
	return 0;
}
EXPORT_SYMBOL_GPL(__get_mtd_device);

/**
 *	get_mtd_device_nm - obtain a validated handle for an MTD device by
 *	device name
 *	@name: MTD device name to open
 *
 * 	This function returns MTD device description structure in case of
 * 	success and an error code in case of failure.
 */
/* 通过 mtd设备名，查找对应的 mtd设备 */
struct mtd_info *get_mtd_device_nm(const char *name)
{
	int err = -ENODEV;
	struct mtd_info *mtd = NULL, *other;

	mutex_lock(&mtd_table_mutex);

    /* 遍历所有的已注册 mtd设备，匹配 name */
	mtd_for_each_device(other) {
		if (!strcmp(name, other->name)) {
			mtd = other;
			break;
		}
	}

	if (!mtd)
		goto out_unlock;

	err = __get_mtd_device(mtd);
	if (err)
		goto out_unlock;

	mutex_unlock(&mtd_table_mutex);
	return mtd;

out_unlock:
	mutex_unlock(&mtd_table_mutex);
	return ERR_PTR(err);
}
EXPORT_SYMBOL_GPL(get_mtd_device_nm);

/* 释放 mtd设备 */
void put_mtd_device(struct mtd_info *mtd)
{
	mutex_lock(&mtd_table_mutex);
	__put_mtd_device(mtd);
	mutex_unlock(&mtd_table_mutex);

}
EXPORT_SYMBOL_GPL(put_mtd_device);

/* 释放 mtd设备 */
void __put_mtd_device(struct mtd_info *mtd)
{
    /* 递减使用计数 */
	--mtd->usecount;
	BUG_ON(mtd->usecount < 0);

    /* 调用 _put_device 回调函数 */
	if (mtd->_put_device)
		mtd->_put_device(mtd);

	module_put(mtd->owner);
}
EXPORT_SYMBOL_GPL(__put_mtd_device);

/*
 * Erase is an asynchronous operation.  Device drivers are supposed
 * to call instr->callback() whenever the operation completes, even
 * if it completes with a failure.
 * Callers are supposed to pass a callback function and wait for it
 * to be called before writing to the block.
 */
/* 擦除是一个异步操作 
 * 设备驱动当擦除完成后，调用 instr->callback 函数，失败时也需要调用
 * 调用者应该传递一个回调函数，并在写入块之前等待它被调用 */
int mtd_erase(struct mtd_info *mtd, struct erase_info *instr)
{
    /* 预判断参数
     * fail_addr 会返回擦除失败的地址
     * state 返回擦除的状态 */
	if (instr->addr > mtd->size || instr->len > mtd->size - instr->addr)
		return -EINVAL;
	if (!(mtd->flags & MTD_WRITEABLE))
		return -EROFS;
	instr->fail_addr = MTD_FAIL_ADDR_UNKNOWN;
	if (!instr->len) {
		instr->state = MTD_ERASE_DONE;
		mtd_erase_callback(instr);
		return 0;
	}
	return mtd->_erase(mtd, instr);
}
EXPORT_SYMBOL_GPL(mtd_erase);

/*
 * This stuff for eXecute-In-Place. phys is optional and may be set to NULL.
 */
/* mtd_point 操作函数 
 * 与 XIP 相关 */
int mtd_point(struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen,
	      void **virt, resource_size_t *phys)
{
	*retlen = 0;
	*virt = NULL;
	if (phys)
		*phys = 0;
	if (!mtd->_point)
		return -EOPNOTSUPP;
	if (from < 0 || from > mtd->size || len > mtd->size - from)
		return -EINVAL;
	if (!len)
		return 0;
	return mtd->_point(mtd, from, len, retlen, virt, phys);
}
EXPORT_SYMBOL_GPL(mtd_point);

/* We probably shouldn't allow XIP if the unpoint isn't a NULL */
/* mtd_unpoint 操作函数 */
int mtd_unpoint(struct mtd_info *mtd, loff_t from, size_t len)
{
	if (!mtd->_point)
		return -EOPNOTSUPP;
	if (from < 0 || from > mtd->size || len > mtd->size - from)
		return -EINVAL;
	if (!len)
		return 0;
	return mtd->_unpoint(mtd, from, len);
}
EXPORT_SYMBOL_GPL(mtd_unpoint);

/*
 * Allow NOMMU mmap() to directly map the device (if not NULL)
 * - return the address to which the offset maps
 * - return -ENOSYS to indicate refusal to do the mapping
 */
/* mtd_get_unmapped_area 操作函数 */
unsigned long mtd_get_unmapped_area(struct mtd_info *mtd, unsigned long len,
				    unsigned long offset, unsigned long flags)
{
	if (!mtd->_get_unmapped_area)
		return -EOPNOTSUPP;
	if (offset > mtd->size || len > mtd->size - offset)
		return -EINVAL;
	return mtd->_get_unmapped_area(mtd, len, offset, flags);
}
EXPORT_SYMBOL_GPL(mtd_get_unmapped_area);

/* mtd_read 操作函数 */
int mtd_read(struct mtd_info *mtd, loff_t from, size_t len, size_t *retlen,
	     u_char *buf)
{
	int ret_code;
	*retlen = 0;
	if (from < 0 || from > mtd->size || len > mtd->size - from)
		return -EINVAL;
	if (!len)
		return 0;

	/*
	 * In the absence of an error, drivers return a non-negative integer
	 * representing the maximum number of bitflips that were corrected on
	 * any one ecc region (if applicable; zero otherwise).
	 */
	ret_code = mtd->_read(mtd, from, len, retlen, buf);
	if (unlikely(ret_code < 0))
		return ret_code;
	if (mtd->ecc_strength == 0)
		return 0;	/* device lacks ecc */
	return ret_code >= mtd->bitflip_threshold ? -EUCLEAN : 0;
}
EXPORT_SYMBOL_GPL(mtd_read);

/* mtd_write 操作函数 */
int mtd_write(struct mtd_info *mtd, loff_t to, size_t len, size_t *retlen,
	      const u_char *buf)
{
	*retlen = 0;
	if (to < 0 || to > mtd->size || len > mtd->size - to)
		return -EINVAL;
	if (!mtd->_write || !(mtd->flags & MTD_WRITEABLE))
		return -EROFS;
	if (!len)
		return 0;
	return mtd->_write(mtd, to, len, retlen, buf);
}
EXPORT_SYMBOL_GPL(mtd_write);

/*
 * In blackbox flight recorder like scenarios we want to make successful writes
 * in interrupt context. panic_write() is only intended to be called when its
 * known the kernel is about to panic and we need the write to succeed. Since
 * the kernel is not going to be running for much longer, this function can
 * break locks and delay to ensure the write succeeds (but not sleep).
 */
/* mtd_panic_write 操作函数 */
int mtd_panic_write(struct mtd_info *mtd, loff_t to, size_t len, size_t *retlen,
		    const u_char *buf)
{
	*retlen = 0;
	if (!mtd->_panic_write)
		return -EOPNOTSUPP;
	if (to < 0 || to > mtd->size || len > mtd->size - to)
		return -EINVAL;
	if (!(mtd->flags & MTD_WRITEABLE))
		return -EROFS;
	if (!len)
		return 0;
	return mtd->_panic_write(mtd, to, len, retlen, buf);
}
EXPORT_SYMBOL_GPL(mtd_panic_write);

/*
 * Method to access the protection register area, present in some flash
 * devices. The user data is one time programmable but the factory data is read
 * only.
 */
/* mtd otp factory 信息 */
int mtd_get_fact_prot_info(struct mtd_info *mtd, struct otp_info *buf,
			   size_t len)
{
	if (!mtd->_get_fact_prot_info)
		return -EOPNOTSUPP;
	if (!len)
		return 0;
	return mtd->_get_fact_prot_info(mtd, buf, len);
}
EXPORT_SYMBOL_GPL(mtd_get_fact_prot_info);

/* mtd otp factory 区域 读信息 */
int mtd_read_fact_prot_reg(struct mtd_info *mtd, loff_t from, size_t len,
			   size_t *retlen, u_char *buf)
{
	*retlen = 0;
	if (!mtd->_read_fact_prot_reg)
		return -EOPNOTSUPP;
	if (!len)
		return 0;
	return mtd->_read_fact_prot_reg(mtd, from, len, retlen, buf);
}
EXPORT_SYMBOL_GPL(mtd_read_fact_prot_reg);

/* mtd otp user 信息 */
int mtd_get_user_prot_info(struct mtd_info *mtd, struct otp_info *buf,
			   size_t len)
{
	if (!mtd->_get_user_prot_info)
		return -EOPNOTSUPP;
	if (!len)
		return 0;
	return mtd->_get_user_prot_info(mtd, buf, len);
}
EXPORT_SYMBOL_GPL(mtd_get_user_prot_info);

/* mtd otp user 区域 读信息 */
int mtd_read_user_prot_reg(struct mtd_info *mtd, loff_t from, size_t len,
			   size_t *retlen, u_char *buf)
{
	*retlen = 0;
	if (!mtd->_read_user_prot_reg)
		return -EOPNOTSUPP;
	if (!len)
		return 0;
	return mtd->_read_user_prot_reg(mtd, from, len, retlen, buf);
}
EXPORT_SYMBOL_GPL(mtd_read_user_prot_reg);

/* mtd otp user 区域写信息 */
int mtd_write_user_prot_reg(struct mtd_info *mtd, loff_t to, size_t len,
			    size_t *retlen, u_char *buf)
{
	*retlen = 0;
	if (!mtd->_write_user_prot_reg)
		return -EOPNOTSUPP;
	if (!len)
		return 0;
	return mtd->_write_user_prot_reg(mtd, to, len, retlen, buf);
}
EXPORT_SYMBOL_GPL(mtd_write_user_prot_reg);

/* mtd otp user 区域 锁定 lock */
int mtd_lock_user_prot_reg(struct mtd_info *mtd, loff_t from, size_t len)
{
	if (!mtd->_lock_user_prot_reg)
		return -EOPNOTSUPP;
	if (!len)
		return 0;
	return mtd->_lock_user_prot_reg(mtd, from, len);
}
EXPORT_SYMBOL_GPL(mtd_lock_user_prot_reg);

/* Chip-supported device locking */
/* mtd lock 操作，底层flash驱动支持才可以 */
int mtd_lock(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	if (!mtd->_lock)
		return -EOPNOTSUPP;
	if (ofs < 0 || ofs > mtd->size || len > mtd->size - ofs)
		return -EINVAL;
	if (!len)
		return 0;
	return mtd->_lock(mtd, ofs, len);
}
EXPORT_SYMBOL_GPL(mtd_lock);

/* mtd unlock 操作，底层flash驱动支持才可以 */
int mtd_unlock(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	if (!mtd->_unlock)
		return -EOPNOTSUPP;
	if (ofs < 0 || ofs > mtd->size || len > mtd->size - ofs)
		return -EINVAL;
	if (!len)
		return 0;
	return mtd->_unlock(mtd, ofs, len);
}
EXPORT_SYMBOL_GPL(mtd_unlock);

/* mtd is lock 操作，底层flash驱动支持才可以 */
int mtd_is_locked(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	if (!mtd->_is_locked)
		return -EOPNOTSUPP;
	if (ofs < 0 || ofs > mtd->size || len > mtd->size - ofs)
		return -EINVAL;
	if (!len)
		return 0;
	return mtd->_is_locked(mtd, ofs, len);
}
EXPORT_SYMBOL_GPL(mtd_is_locked);

/* mtd is bad 操作，底层flash驱动支持才可以 */
int mtd_block_isbad(struct mtd_info *mtd, loff_t ofs)
{
	if (!mtd->_block_isbad)
		return 0;
	if (ofs < 0 || ofs > mtd->size)
		return -EINVAL;
	return mtd->_block_isbad(mtd, ofs);
}
EXPORT_SYMBOL_GPL(mtd_block_isbad);

/* mtd mark bad 操作，底层flash驱动支持才可以 */
int mtd_block_markbad(struct mtd_info *mtd, loff_t ofs)
{
	if (!mtd->_block_markbad)
		return -EOPNOTSUPP;
	if (ofs < 0 || ofs > mtd->size)
		return -EINVAL;
	if (!(mtd->flags & MTD_WRITEABLE))
		return -EROFS;
	return mtd->_block_markbad(mtd, ofs);
}
EXPORT_SYMBOL_GPL(mtd_block_markbad);

/*
 * default_mtd_writev - the default writev method
 * @mtd: mtd device description object pointer
 * @vecs: the vectors to write
 * @count: count of vectors in @vecs
 * @to: the MTD device offset to write to
 * @retlen: on exit contains the count of bytes written to the MTD device.
 *
 * This function returns zero in case of success and a negative error code in
 * case of failure.
 */
/* 默认的 mtd_writev 操作函数
 * mtd设备未提供 _writev 回调函数时调用 */
static int default_mtd_writev(struct mtd_info *mtd, const struct kvec *vecs,
			      unsigned long count, loff_t to, size_t *retlen)
{
	unsigned long i;
	size_t totlen = 0, thislen;
	int ret = 0;

	for (i = 0; i < count; i++) {
		if (!vecs[i].iov_len)
			continue;
		ret = mtd_write(mtd, to, vecs[i].iov_len, &thislen,
				vecs[i].iov_base);
		totlen += thislen;
		if (ret || thislen != vecs[i].iov_len)
			break;
		to += vecs[i].iov_len;
	}
	*retlen = totlen;
	return ret;
}

/*
 * mtd_writev - the vector-based MTD write method
 * @mtd: mtd device description object pointer
 * @vecs: the vectors to write
 * @count: count of vectors in @vecs
 * @to: the MTD device offset to write to
 * @retlen: on exit contains the count of bytes written to the MTD device.
 *
 * This function returns zero in case of success and a negative error code in
 * case of failure.
 */
/* mtd_writev 操作函数 */
int mtd_writev(struct mtd_info *mtd, const struct kvec *vecs,
	       unsigned long count, loff_t to, size_t *retlen)
{
	*retlen = 0;
	if (!(mtd->flags & MTD_WRITEABLE))
		return -EROFS;
	if (!mtd->_writev)
		return default_mtd_writev(mtd, vecs, count, to, retlen);
	return mtd->_writev(mtd, vecs, count, to, retlen);
}
EXPORT_SYMBOL_GPL(mtd_writev);

/**
 * mtd_kmalloc_up_to - allocate a contiguous buffer up to the specified size
 * @mtd: mtd device description object pointer
 * @size: a pointer to the ideal or maximum size of the allocation, points
 *        to the actual allocation size on success.
 *
 * This routine attempts to allocate a contiguous kernel buffer up to
 * the specified size, backing off the size of the request exponentially
 * until the request succeeds or until the allocation size falls below
 * the system page size. This attempts to make sure it does not adversely
 * impact system performance, so when allocating more than one page, we
 * ask the memory allocator to avoid re-trying, swapping, writing back
 * or performing I/O.
 *
 * Note, this function also makes sure that the allocated buffer is aligned to
 * the MTD device's min. I/O unit, i.e. the "mtd->writesize" value.
 *
 * This is called, for example by mtd_{read,write} and jffs2_scan_medium,
 * to handle smaller (i.e. degraded) buffer allocations under low- or
 * fragmented-memory situations where such reduced allocations, from a
 * requested ideal, are allowed.
 *
 * Returns a pointer to the allocated buffer on success; otherwise, NULL.
 */
/* 分配一个指定大小的连续缓冲区
 * mtd:  mtd 设备描述符 
 * size: 一个指向理想或最大分配大小的指针，成功是指向实际分配大小 
 *
 * 该方法中尝试分配一个指定大小的连续内核缓冲区，以指数方式减少请求的大小，直到
 * 请求成功或直到分配大小低于系统页面大小。 这是为了确保它不会对系统性能产生不利
 * 影响，因此在分配多个页面时，要求内存分配避免重试、交换、回写或执行I/O。
 *
 * 注意，这个函数还确保分配的缓冲区与MTD设备的最小I/O单元对齐，及 MTD->writesize.
 *
 * 例如，有 mtd_{read, write} 和 jffs2_scan_medium 调用，用于在低内存或碎片内存
 * 情况下处理更小（即降级）的缓冲区分配，从请求的理想状态来看，这种减少的分配是
 * 允许的。 
 *
 * 这个函数中主要是为了性能考虑，分配的内存不会太小（一个PAGE），但也不会在分配
 * 不到的情况下一直重试，交换等。 如果太大分不到，那就减小一些再试试。
 *
 * 对于 NORFLASH，如果请求的 *size 小于 PAGE_SIZE, 那就直接使用 kmalloc 分配请求
 * 大小的空间；如果 *size 比较大，那就尽量请求大的，实在申请不到，就申请小于
 * PAGE_SIZE 的空间 
 * */
void *mtd_kmalloc_up_to(const struct mtd_info *mtd, size_t *size)
{
	gfp_t flags = __GFP_NOWARN | __GFP_WAIT |
		       __GFP_NORETRY | __GFP_NO_KSWAPD;
	size_t min_alloc = max_t(size_t, mtd->writesize, PAGE_SIZE);
	void *kbuf;

	*size = min_t(size_t, *size, KMALLOC_MAX_SIZE);

    /* min_alloc 是确认最小分配单元，例如 norflash的 writesize = 1, 所以最小分配
     * 就是 PAGE_SIZE, 如果 NANDflash writesize 太大，那就以 writesize 
     *
     * *size 是预计能分配的最大，如果 请求的 *size 太大，超过了 KMALLOC_MAX_SIZE，
     * 那也只能最大分配 KMALLOC_MAX_SIZE */

    /* 请求的 *size 超过了 预期的最小分配大小，那就先尝试分配一下 */
	while (*size > min_alloc) {
        /* 如果直接分配到了请求的 *size 空间，那是最好了，直接返回就可以了 */
		kbuf = kmalloc(*size, flags);
		if (kbuf)
			return kbuf;

        /* 如果分配失败，就将请求的大小减半
         * 注意，这里做了一个对齐操作，请求的大小与 writesize 对齐 */
		*size >>= 1;
		*size = ALIGN(*size, mtd->writesize);
	}

	/*
	 * For the last resort allocation allow 'kmalloc()' to do all sorts of
	 * things (write-back, dropping caches, etc) by using GFP_KERNEL.
	 */
    /* 最后的情况，分两种情况：
     * 如果 *size < min_alloc ,也就是请求一个比较小的空间，直接到这, 对于
     * norflash，min_alloc = PAGE_SIZE, 这意味着请求的 *size 是小于一页的
     *
     * 另外一种情况，就是请求的 *size 很大，但一直请求不到，上面的递减操作将
     * *size 递减到 PAGE_SIZE 以下了，只能这样分配了 */
	return kmalloc(*size, GFP_KERNEL);
}
EXPORT_SYMBOL_GPL(mtd_kmalloc_up_to);

#ifdef CONFIG_PROC_FS

/*====================================================================*/
/* Support for /proc/mtd */

/* /proc/mtd 节点 */
static struct proc_dir_entry *proc_mtd;

/* cat /proc/mtd 时会调用 */
static int mtd_proc_show(struct seq_file *m, void *v)
{
	struct mtd_info *mtd;

	seq_puts(m, "dev:    size   erasesize  name\n");
	mutex_lock(&mtd_table_mutex);
	mtd_for_each_device(mtd) {
		seq_printf(m, "mtd%d: %8.8llx %8.8x \"%s\"\n",
			   mtd->index, (unsigned long long)mtd->size,
			   mtd->erasesize, mtd->name);
	}
	mutex_unlock(&mtd_table_mutex);
	return 0;
}

static int mtd_proc_open(struct inode *inode, struct file *file)
{
	return single_open(file, mtd_proc_show, NULL);
}

static const struct file_operations mtd_proc_ops = {
	.open		= mtd_proc_open,
	.read		= seq_read,
	.llseek		= seq_lseek,
	.release	= single_release,
};
#endif /* CONFIG_PROC_FS */

/*====================================================================*/
/* Init code */

/* backing device info 驱动 
 * 与数据回写有关，此处不分析 */
static int __init mtd_bdi_init(struct backing_dev_info *bdi, const char *name)
{
	int ret;

	ret = bdi_init(bdi);
	if (!ret)
		ret = bdi_register(bdi, NULL, name);

	if (ret)
		bdi_destroy(bdi);

	return ret;
}

/* mtd 原始设备层
 * 实现屏蔽flash差异，统一mtd向上接口 */
static int __init init_mtd(void)
{
	int ret;

    /* /sys/class/mtd/ 目录，所有mtd设备都属于 mtd class 下 */
	ret = class_register(&mtd_class);
	if (ret)
		goto err_reg;

    /* backing device info 驱动 
     * 与数据回写有关，此处不分析 */
	ret = mtd_bdi_init(&mtd_bdi_unmappable, "mtd-unmap");
	if (ret)
		goto err_bdi1;

	ret = mtd_bdi_init(&mtd_bdi_ro_mappable, "mtd-romap");
	if (ret)
		goto err_bdi2;

	ret = mtd_bdi_init(&mtd_bdi_rw_mappable, "mtd-rwmap");
	if (ret)
		goto err_bdi3;

#ifdef CONFIG_PROC_FS
    /* /proc/mtd 文件，用于查看所有已注册的 mtd 设备 */
	proc_mtd = proc_create("mtd", 0, NULL, &mtd_proc_ops);
#endif /* CONFIG_PROC_FS */
	return 0;

err_bdi3:
	bdi_destroy(&mtd_bdi_ro_mappable);
err_bdi2:
	bdi_destroy(&mtd_bdi_unmappable);
err_bdi1:
	class_unregister(&mtd_class);
err_reg:
	pr_err("Error registering mtd class or bdi: %d\n", ret);
	return ret;
}

/* 卸载函数，逆向操作 */
static void __exit cleanup_mtd(void)
{
#ifdef CONFIG_PROC_FS
	if (proc_mtd)
		remove_proc_entry( "mtd", NULL);
#endif /* CONFIG_PROC_FS */
	class_unregister(&mtd_class);
	bdi_destroy(&mtd_bdi_unmappable);
	bdi_destroy(&mtd_bdi_ro_mappable);
	bdi_destroy(&mtd_bdi_rw_mappable);
}

module_init(init_mtd);
module_exit(cleanup_mtd);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Woodhouse <dwmw2@infradead.org>");
MODULE_DESCRIPTION("Core MTD registration and access routines");
