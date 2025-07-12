/*
 * Copyright © 1999-2010 David Woodhouse <dwmw2@infradead.org>
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

#include <linux/device.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mutex.h>
#include <linux/backing-dev.h>
#include <linux/compat.h>
#include <linux/mount.h>
#include <linux/blkpg.h>
#include <linux/magic.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/mtd/map.h>

#include <asm/uaccess.h>

static DEFINE_MUTEX(mtd_mutex);

/*
 * Data structure to hold the pointer to the mtd device as well
 * as mode information of various use cases.
 */
/* mtdchar 中使用的私有数据结构，
 * 用于在 open 时记录被打开的 mtd 对应的 mtd_info 
 * 并把这个私有结构给 file->private_data ,
 * 这样其它的文件操作都能找到被打开的 mtd 了，
 * 这是因为在 open 时，只有 inode 结构中的设备号才能匹配到 mtd_info
 * 但其它的函数 read/write 的入参没有 inode 参数 */
struct mtd_file_info {
	struct mtd_info *mtd;
	struct inode *ino;
	enum mtd_file_modes mode;
};

/* 对应应用层的 fseek 调用，设置文件的偏移位置 */
static loff_t mtdchar_lseek(struct file *file, loff_t offset, int orig)
{
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;

    /* SEEK_SET: 相对文件开头
     *           MTD设备的偏移 0 处
     * SEEK_CUR: 相对当前位置
     *           MTD设备当前偏移处
     * SEEK_END: 相对文件结尾 
     *           MTD设备总大小偏移处 */
	switch (orig) {
	case SEEK_SET:
		break;
	case SEEK_CUR:
		offset += file->f_pos;
		break;
	case SEEK_END:
		offset += mtd->size;
		break;
	default:
		return -EINVAL;
	}

    /* 偏移后的位置有效才真正偏移，
     * 偏移就是把 file 的 f_pos 指针置为偏移处 */
	if (offset >= 0 && offset <= mtd->size)
		return file->f_pos = offset;

	return -EINVAL;
}

/* // TODO: 文件系统相关，待分析 */
static int count;
static struct vfsmount *mnt;
static struct file_system_type mtd_inodefs_type;

/* 这是 mtd 字符设备的 open 入口函数
 * 是从 chrdev_open 字符设备 open 总入口调用的
 * inode中包含了设备号，cdev结构 */
static int mtdchar_open(struct inode *inode, struct file *file)
{
    /* iminor 从 inode 中获取从设备号， i_rdev */
	int minor = iminor(inode);
    /* mtd 的从设备号从 0开始分配
     * 但每个 mtd 都会创建 mtdx mtdxro 两个字符设备，占用两个从设备号
     * 所以，这里的设备序号 devnum 需要除 2 */
	int devnum = minor >> 1;
	int ret = 0;
	struct mtd_info *mtd;
	struct mtd_file_info *mfi;
	struct inode *mtd_ino;

	pr_debug("MTD_open\n");

    /* /dev/mtd0ro 是只读设备，不能以 RW方式打开 */
	/* You can't open the RO devices RW */
	if ((file->f_mode & FMODE_WRITE) && (minor & 1))
		return -EACCES;

    /* // TODO: 文件系统相关，待分析 */
	ret = simple_pin_fs(&mtd_inodefs_type, &mnt, &count);
	if (ret)
		return ret;

	mutex_lock(&mtd_mutex);
    /* 按照 mtd 设备序号查找 mtd设备，返回对应的 mtd_info
     * 到这里已经通过打开 /dev/mtd0 设备节点，确认了需要操作的 mtd设备
     * mtd_info 中包含了具体的 mtd 设备的所有信息，属性/操作函数等 */
	mtd = get_mtd_device(NULL, devnum);

	if (IS_ERR(mtd)) {
		ret = PTR_ERR(mtd);
		goto out;
	}

    /* 判断是否为 MTD_ABSENT 设备，这种状态指示该设备不存在或未初始化 */
	if (mtd->type == MTD_ABSENT) {
		ret = -ENODEV;
		goto out1;
	}

    /* // TODO: 文件系统相关，待分析 */
	mtd_ino = iget_locked(mnt->mnt_sb, devnum);
	if (!mtd_ino) {
		ret = -ENOMEM;
		goto out1;
	}
	if (mtd_ino->i_state & I_NEW) {
		mtd_ino->i_private = mtd;
		mtd_ino->i_mode = S_IFCHR;
		mtd_ino->i_data.backing_dev_info = mtd->backing_dev_info;
		unlock_new_inode(mtd_ino);
	}
	file->f_mapping = mtd_ino->i_mapping;

    /* 判断将要操作的 mtd 设备的可属性，如果使用写方式打开，那 mtd属性也需要支持
     * 写，在上面的判断是确认不会以写方式打开 /dev/mtd0ro ，而这里是确认 具体的
     * MTD 设备是否具有写入属性 */
	/* You can't open it RW if it's not a writeable device */
	if ((file->f_mode & FMODE_WRITE) && !(mtd->flags & MTD_WRITEABLE)) {
		ret = -EACCES;
		goto out2;
	}

    /* mtd 字符设备的私有数据结构 */
	mfi = kzalloc(sizeof(*mfi), GFP_KERNEL);
	if (!mfi) {
		ret = -ENOMEM;
		goto out2;
	}
    /* 记录关键数据结构信息，并给 file->private_data
     * 其它的文件操作就可以使用了*/
	mfi->ino = mtd_ino;
	mfi->mtd = mtd;
	file->private_data = mfi;
	mutex_unlock(&mtd_mutex);
	return 0;

out2:
	iput(mtd_ino);
out1:
	put_mtd_device(mtd);
out:
	mutex_unlock(&mtd_mutex);
	simple_release_fs(&mnt, &count);
	return ret;
} /* mtdchar_open */

/*====================================================================*/

/* 关闭 mtd 设备 */
static int mtdchar_close(struct inode *inode, struct file *file)
{
    /* open 方法中记录的各信息，可以找到 mtd_info 数据 */
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;

	pr_debug("MTD_close\n");

    /* 以 RW方式打开的mtd设备，关闭前需同步数据 */
	/* Only sync if opened RW */
	if ((file->f_mode & FMODE_WRITE))
		mtd_sync(mtd);

    /* // TODO: 文件系统相关，待分析 */
	iput(mfi->ino);

    /* 释放 MTD设备
     * file 文件私有指针置 NULL 
     * 释放 mfi 私有结构 */
	put_mtd_device(mtd);
	file->private_data = NULL;
	kfree(mfi);
    /* // TODO: 文件系统相关，待分析 */
	simple_release_fs(&mnt, &count);

	return 0;
} /* mtdchar_close */

/* Back in June 2001, dwmw2 wrote:
 *
 *   FIXME: This _really_ needs to die. In 2.5, we should lock the
 *   userspace buffer down and use it directly with readv/writev.
 *
 * The implementation below, using mtd_kmalloc_up_to, mitigates
 * allocation failures when the system is under low-memory situations
 * or if memory is highly fragmented at the cost of reducing the
 * performance of the requested transfer due to a smaller buffer size.
 *
 * A more complex but more memory-efficient implementation based on
 * get_user_pages and iovecs to cover extents of those pages is a
 * longer-term goal, as intimated by dwmw2 above. However, for the
 * write case, this requires yet more complex head and tail transfer
 * handling when those head and tail offsets and sizes are such that
 * alignment requirements are not met in the NAND subdriver.
 */
/* mtd 字符设备 read 方法 */
static ssize_t mtdchar_read(struct file *file, char __user *buf, size_t count,
			loff_t *ppos)
{
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;
	size_t retlen;
	size_t total_retlen=0;
	int ret=0;
	int len;
	size_t size = count;
	char *kbuf;

	pr_debug("MTD_read\n");

    /* 读位置是否超限制，若超限制则限制读的数量 */
	if (*ppos + count > mtd->size)
		count = mtd->size - *ppos;

	if (!count)
		return 0;

    /* 申请内核缓冲区，注意，这里的 size 是参数中的 count
     * 可能比实际的大； 这里尽量分配比较大的内存，但不一定能分到；
     * 这里的 size 是申请的内存的实际大小 */
	kbuf = mtd_kmalloc_up_to(mtd, &size);
	if (!kbuf)
		return -ENOMEM;

    /* 因为内核缓冲区 size 可能比较小，所以可能需要读多次 */
	while (count) {
        /* 按照实际缓冲区 或 读的数量 来读，那个小用那个 */
		len = min_t(size_t, count, size);

        /* mtd 的文件模式，在 mtd_abi.h 中定义
         * 可以通过 ioctl(MTDFILEMODE) 设置 */
		switch (mfi->mode) {
		case MTD_FILE_MODE_OTP_FACTORY:
			ret = mtd_read_fact_prot_reg(mtd, *ppos, len,
						     &retlen, kbuf);
			break;
		case MTD_FILE_MODE_OTP_USER:
			ret = mtd_read_user_prot_reg(mtd, *ppos, len,
						     &retlen, kbuf);
			break;
		case MTD_FILE_MODE_RAW:
		{
			struct mtd_oob_ops ops;

			ops.mode = MTD_OPS_RAW;
			ops.datbuf = kbuf;
			ops.oobbuf = NULL;
			ops.len = len;

			ret = mtd_read_oob(mtd, *ppos, &ops);
			retlen = ops.retlen;
			break;
		}
		default:
            /* 默认的读操作， mtd_read 中会调用 mtd 真实设备的 _read 方法 */
			ret = mtd_read(mtd, *ppos, len, &retlen, kbuf);
		}
		/* Nand returns -EBADMSG on ECC errors, but it returns
		 * the data. For our userspace tools it is important
		 * to dump areas with ECC errors!
		 * For kernel internal usage it also might return -EUCLEAN
		 * to signal the caller that a bitflip has occurred and has
		 * been corrected by the ECC algorithm.
		 * Userspace software which accesses NAND this way
		 * must be aware of the fact that it deals with NAND
		 */
        /* 有两种情况需要将数据返回给用户：
         * 1、数据读取完全正确，此时 ret = 0 
         * 2、数据读取成功了，但 发生了 ECC错误 或 位反转（已纠正）
         * 发生了 ECC错误或位反转（已纠正）是需要将信息返回给用户的 */
		if (!ret || mtd_is_bitflip_or_eccerr(ret)) {
            /* retlen 是实际读出的数据长度
             * 调整 *ppos 文件指针位置
             * 拷贝已读数据到用户空间
             * total_retlen 是所有读取数据的长度（可能分多次读） */
			*ppos += retlen;
			if (copy_to_user(buf, kbuf, retlen)) {
				kfree(kbuf);
				return -EFAULT;
			}
			else
				total_retlen += retlen;

            /* 计算用户空间偏移，准备下一次读操作 */
			count -= retlen;
			buf += retlen;
			if (retlen == 0)
				count = 0;
		}
		else {
            /* 发生的非预期错误，则释放空间并返回错误 */
			kfree(kbuf);
			return ret;
		}

	}

    /* 释放空间，返回已读数据长度 */
	kfree(kbuf);
	return total_retlen;
} /* mtdchar_read */

/* mtd 设备写功能 */
static ssize_t mtdchar_write(struct file *file, const char __user *buf, size_t count,
			loff_t *ppos)
{
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;
	size_t size = count;
	char *kbuf;
	size_t retlen;
	size_t total_retlen=0;
	int ret=0;
	int len;

	pr_debug("MTD_write\n");

    /* 参数有效性判断
     * 写入位置、写入数量需在有效地址范围内 */
	if (*ppos == mtd->size)
		return -ENOSPC;

	if (*ppos + count > mtd->size)
		count = mtd->size - *ppos;

	if (!count)
		return 0;

    /* 分配内核缓冲区，参考 MTD_read 中解释 */
	kbuf = mtd_kmalloc_up_to(mtd, &size);
	if (!kbuf)
		return -ENOMEM;

    /* 不一定能一次写入完成
     * 整个流程基本与 mtd_read 一致 */
	while (count) {
		len = min_t(size_t, count, size);

		if (copy_from_user(kbuf, buf, len)) {
			kfree(kbuf);
			return -EFAULT;
		}

		switch (mfi->mode) {
		case MTD_FILE_MODE_OTP_FACTORY:
            /* MTD OTP 的 factory 区域不能写操作 */
			ret = -EROFS;
			break;
		case MTD_FILE_MODE_OTP_USER:
			ret = mtd_write_user_prot_reg(mtd, *ppos, len,
						      &retlen, kbuf);
			break;

		case MTD_FILE_MODE_RAW:
		{
			struct mtd_oob_ops ops;

			ops.mode = MTD_OPS_RAW;
			ops.datbuf = kbuf;
			ops.oobbuf = NULL;
			ops.ooboffs = 0;
			ops.len = len;

			ret = mtd_write_oob(mtd, *ppos, &ops);
			retlen = ops.retlen;
			break;
		}

		default:
			ret = mtd_write(mtd, *ppos, len, &retlen, kbuf);
		}
		if (!ret) {
			*ppos += retlen;
			total_retlen += retlen;
			count -= retlen;
			buf += retlen;
		}
		else {
			kfree(kbuf);
			return ret;
		}
	}

    /* 释放空间，返回写入的总数量 */
	kfree(kbuf);
	return total_retlen;
} /* mtdchar_write */

/*======================================================================

    IOCTL calls for getting device parameters.

======================================================================*/
/* mtd 擦除完成回调，擦除过程非常耗时
 * 所以在擦除过程不会一直等待，当设备驱动擦除完成后，调用本函数唤醒
 * 本函数以回调方式提供，在 MEMERASE / MEMERASE64 命令中设置 */
static void mtdchar_erase_callback (struct erase_info *instr)
{
    /* instr->priv 指向等待队列句柄 */
	wake_up((wait_queue_head_t *)instr->priv);
}

#ifdef CONFIG_HAVE_MTD_OTP
/* otp 文件模式选择，模式会在 read/write 方法中有效，用于调用不同的flash处理 */
static int otp_select_filemode(struct mtd_file_info *mfi, int mode)
{
	struct mtd_info *mtd = mfi->mtd;
	size_t retlen;
	int ret = 0;

	/*
	 * Make a fake call to mtd_read_fact_prot_reg() to check if OTP
	 * operations are supported.
	 */
    /* 检测 flash 是否支持 OTP 操作 */
	if (mtd_read_fact_prot_reg(mtd, -1, 0, &retlen, NULL) == -EOPNOTSUPP)
		return -EOPNOTSUPP;

	switch (mode) {
	case MTD_OTP_FACTORY:
		mfi->mode = MTD_FILE_MODE_OTP_FACTORY;
		break;
	case MTD_OTP_USER:
		mfi->mode = MTD_FILE_MODE_OTP_USER;
		break;
	default:
		ret = -EINVAL;
	case MTD_OTP_OFF:
		break;
	}
	return ret;
}
#else
# define otp_select_filemode(f,m)	-EOPNOTSUPP
#endif

/* mtd 写 oob 函数 */
static int mtdchar_writeoob(struct file *file, struct mtd_info *mtd,
	uint64_t start, uint32_t length, void __user *ptr,
	uint32_t __user *retp)
{
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_oob_ops ops;
	uint32_t retlen;
	int ret = 0;

	if (!(file->f_mode & FMODE_WRITE))
		return -EPERM;

	if (length > 4096)
		return -EINVAL;

	if (!mtd->_write_oob)
		ret = -EOPNOTSUPP;
	else
		ret = access_ok(VERIFY_READ, ptr, length) ? 0 : -EFAULT;

	if (ret)
		return ret;

	ops.ooblen = length;
	ops.ooboffs = start & (mtd->writesize - 1);
	ops.datbuf = NULL;
	ops.mode = (mfi->mode == MTD_FILE_MODE_RAW) ? MTD_OPS_RAW :
		MTD_OPS_PLACE_OOB;

	if (ops.ooboffs && ops.ooblen > (mtd->oobsize - ops.ooboffs))
		return -EINVAL;

	ops.oobbuf = memdup_user(ptr, length);
	if (IS_ERR(ops.oobbuf))
		return PTR_ERR(ops.oobbuf);

	start &= ~((uint64_t)mtd->writesize - 1);
	ret = mtd_write_oob(mtd, start, &ops);

	if (ops.oobretlen > 0xFFFFFFFFU)
		ret = -EOVERFLOW;
	retlen = ops.oobretlen;
	if (copy_to_user(retp, &retlen, sizeof(length)))
		ret = -EFAULT;

	kfree(ops.oobbuf);
	return ret;
}

/* mtd 读 oob 函数 */
static int mtdchar_readoob(struct file *file, struct mtd_info *mtd,
	uint64_t start, uint32_t length, void __user *ptr,
	uint32_t __user *retp)
{
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_oob_ops ops;
	int ret = 0;

	if (length > 4096)
		return -EINVAL;

	if (!access_ok(VERIFY_WRITE, ptr, length))
		return -EFAULT;

	ops.ooblen = length;
	ops.ooboffs = start & (mtd->writesize - 1);
	ops.datbuf = NULL;
	ops.mode = (mfi->mode == MTD_FILE_MODE_RAW) ? MTD_OPS_RAW :
		MTD_OPS_PLACE_OOB;

	if (ops.ooboffs && ops.ooblen > (mtd->oobsize - ops.ooboffs))
		return -EINVAL;

	ops.oobbuf = kmalloc(length, GFP_KERNEL);
	if (!ops.oobbuf)
		return -ENOMEM;

	start &= ~((uint64_t)mtd->writesize - 1);
	ret = mtd_read_oob(mtd, start, &ops);

	if (put_user(ops.oobretlen, retp))
		ret = -EFAULT;
	else if (ops.oobretlen && copy_to_user(ptr, ops.oobbuf,
					    ops.oobretlen))
		ret = -EFAULT;

	kfree(ops.oobbuf);

	/*
	 * NAND returns -EBADMSG on ECC errors, but it returns the OOB
	 * data. For our userspace tools it is important to dump areas
	 * with ECC errors!
	 * For kernel internal usage it also might return -EUCLEAN
	 * to signal the caller that a bitflip has occured and has
	 * been corrected by the ECC algorithm.
	 *
	 * Note: currently the standard NAND function, nand_read_oob_std,
	 * does not calculate ECC for the OOB area, so do not rely on
	 * this behavior unless you have replaced it with your own.
	 */
	if (mtd_is_bitflip_or_eccerr(ret))
		return 0;

	return ret;
}

/*
 * Copies (and truncates, if necessary) data from the larger struct,
 * nand_ecclayout, to the smaller, deprecated layout struct,
 * nand_ecclayout_user. This is necessary only to support the deprecated
 * API ioctl ECCGETLAYOUT while allowing all new functionality to use
 * nand_ecclayout flexibly (i.e. the struct may change size in new
 * releases without requiring major rewrites).
 */
/* 已废弃，不再分析 */
static int shrink_ecclayout(const struct nand_ecclayout *from,
		struct nand_ecclayout_user *to)
{
	int i;

	if (!from || !to)
		return -EINVAL;

	memset(to, 0, sizeof(*to));

	to->eccbytes = min((int)from->eccbytes, MTD_MAX_ECCPOS_ENTRIES);
	for (i = 0; i < to->eccbytes; i++)
		to->eccpos[i] = from->eccpos[i];

	for (i = 0; i < MTD_MAX_OOBFREE_ENTRIES; i++) {
		if (from->oobfree[i].length == 0 &&
				from->oobfree[i].offset == 0)
			break;
		to->oobavail += from->oobfree[i].length;
		to->oobfree[i] = from->oobfree[i];
	}

	return 0;
}

/* mtd 分区操作
 * 使用 BLKPG 命令，添加/删除分区 
 * 注意，只能主 mtd 设备 才可以添加分区
 * 应用层的使用方法，可以参考 mtd 应用层工具的 mtdpart 命令源码 */
static int mtdchar_blkpg_ioctl(struct mtd_info *mtd,
			   struct blkpg_ioctl_arg __user *arg)
{
    /* a.op 是 添加/删除 分区的命令
     * p 结构中包含分区名，起始地址，分区长度 */
	struct blkpg_ioctl_arg a;
	struct blkpg_partition p;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;

	if (copy_from_user(&a, arg, sizeof(struct blkpg_ioctl_arg)))
		return -EFAULT;

	if (copy_from_user(&p, a.data, sizeof(struct blkpg_partition)))
		return -EFAULT;

    /* 执行具体的操作 */
	switch (a.op) {
	case BLKPG_ADD_PARTITION:

		/* Only master mtd device must be used to add partitions */
        /* 只有主mtd设备才可以添加分区 */
		if (mtd_is_partition(mtd))
			return -EINVAL;

        /* 添加分区 */
		return mtd_add_partition(mtd, p.devname, p.start, p.length);

	case BLKPG_DEL_PARTITION:

        /* 移除分区，有效分区才可移除 */
		if (p.pno < 0)
			return -EINVAL;

        /* 删除分区 */
		return mtd_del_partition(mtd, p.pno);

	default:
		return -EINVAL;
	}
}

/* mtd MEMWRITE ioctl 函数
 * 允许同时请求写入 data 和 OOB */
static int mtdchar_write_ioctl(struct mtd_info *mtd,
		struct mtd_write_req __user *argp)
{
	struct mtd_write_req req;
	struct mtd_oob_ops ops;
	void __user *usr_data, *usr_oob;
	int ret;

	if (copy_from_user(&req, argp, sizeof(req)) ||
			!access_ok(VERIFY_READ, req.usr_data, req.len) ||
			!access_ok(VERIFY_READ, req.usr_oob, req.ooblen))
		return -EFAULT;
	if (!mtd->_write_oob)
		return -EOPNOTSUPP;

	ops.mode = req.mode;
	ops.len = (size_t)req.len;
	ops.ooblen = (size_t)req.ooblen;
	ops.ooboffs = 0;

    /* 需要处理 data OOB 两个数据
     * 这个与 MEMWRITEOOB 类命令有区别，MEMWRITEOOB 只处理 OOB */
	usr_data = (void __user *)(uintptr_t)req.usr_data;
	usr_oob = (void __user *)(uintptr_t)req.usr_oob;

	if (req.usr_data) {
		ops.datbuf = memdup_user(usr_data, ops.len);
		if (IS_ERR(ops.datbuf))
			return PTR_ERR(ops.datbuf);
	} else {
		ops.datbuf = NULL;
	}

	if (req.usr_oob) {
		ops.oobbuf = memdup_user(usr_oob, ops.ooblen);
		if (IS_ERR(ops.oobbuf)) {
			kfree(ops.datbuf);
			return PTR_ERR(ops.oobbuf);
		}
	} else {
		ops.oobbuf = NULL;
	}

	ret = mtd_write_oob(mtd, (loff_t)req.start, &ops);

	kfree(ops.datbuf);
	kfree(ops.oobbuf);

	return ret;
}

/* mtd 字符设备的 ioctl 回调 
 * ioctl 处理非标准的文件操作，在mtd子系统中，处理除了
 * 读/写 之外的其它操作，如 擦除、获取mtd信息等 */
static int mtdchar_ioctl(struct file *file, u_int cmd, u_long arg)
{
    /* 参考 mtdchar_open , private_data 指向打开的 mfi  */
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;
	void __user *argp = (void __user *)arg;
	int ret = 0;
	u_long size;
	struct mtd_info_user info;

	pr_debug("MTD_ioctl\n");

    /* 判断 cmd 的基本信息，读写、操作数范围是否正确 */
	size = (cmd & IOCSIZE_MASK) >> IOCSIZE_SHIFT;
	if (cmd & IOC_IN) {
		if (!access_ok(VERIFY_READ, argp, size))
			return -EFAULT;
	}
	if (cmd & IOC_OUT) {
		if (!access_ok(VERIFY_WRITE, argp, size))
			return -EFAULT;
	}

	switch (cmd) {
	case MEMGETREGIONCOUNT:
        /* 获取不同擦除区域的数量, 直接访问 numeraseregions 即可 */
		if (copy_to_user(argp, &(mtd->numeraseregions), sizeof(int)))
			return -EFAULT;
		break;

	case MEMGETREGIONINFO:
	{
		uint32_t ur_idx;
		struct mtd_erase_region_info *kr;
		struct region_info_user __user *ur = argp;

        /* 应用层使用 numeraseregions 指定区域 */
		if (get_user(ur_idx, &(ur->regionindex)))
			return -EFAULT;

		if (ur_idx >= mtd->numeraseregions)
			return -EINVAL;

        /* 查找对应的擦除区域属性，并返回给应用层 */
		kr = &(mtd->eraseregions[ur_idx]);

		if (put_user(kr->offset, &(ur->offset))
		    || put_user(kr->erasesize, &(ur->erasesize))
		    || put_user(kr->numblocks, &(ur->numblocks)))
			return -EFAULT;

		break;
	}

	case MEMGETINFO:
        /* 获取 mtd 的基本信息
         * 类型、标志、总大小、擦除/写入单元、oob */
		memset(&info, 0, sizeof(info));
		info.type	= mtd->type;
		info.flags	= mtd->flags;
		info.size	= mtd->size;
		info.erasesize	= mtd->erasesize;
		info.writesize	= mtd->writesize;
		info.oobsize	= mtd->oobsize;
		/* The below field is obsolete */
		info.padding	= 0;
		if (copy_to_user(argp, &info, sizeof(struct mtd_info_user)))
			return -EFAULT;
		break;

	case MEMERASE:
	case MEMERASE64:
	{
        /* 擦除指令，两种结构体，数据类型不同（32位宽 or 64位宽） */
        /* mtd 子系统中使用的擦除操作结构体，传递给flash驱动的 _erase 回调 */
		struct erase_info *erase;

        /* 擦除操作需要在文件写模式中操作 */
		if(!(file->f_mode & FMODE_WRITE))
			return -EPERM;

		erase=kzalloc(sizeof(struct erase_info),GFP_KERNEL);
		if (!erase)
			ret = -ENOMEM;
		else {
            /* 创建本次擦除的等待队列，擦除操作完成后将使用它唤醒 */
			wait_queue_head_t waitq;
			DECLARE_WAITQUEUE(wait, current);

			init_waitqueue_head(&waitq);

            /* 擦除分 MEMERASE 和 MEMERASE64 两种命令
             * 应用层的结构体不同，需要区分 */
			if (cmd == MEMERASE64) {
				struct erase_info_user64 einfo64;

				if (copy_from_user(&einfo64, argp,
					    sizeof(struct erase_info_user64))) {
					kfree(erase);
					return -EFAULT;
				}
				erase->addr = einfo64.start;
				erase->len = einfo64.length;
			} else {
				struct erase_info_user einfo32;

				if (copy_from_user(&einfo32, argp,
					    sizeof(struct erase_info_user))) {
					kfree(erase);
					return -EFAULT;
				}
				erase->addr = einfo32.start;
				erase->len = einfo32.length;
			}
            /* 所有参数都会使用 erase 结构体，统一参数 */
			erase->mtd = mtd;
			erase->callback = mtdchar_erase_callback;
			erase->priv = (unsigned long)&waitq;

			/*
			  FIXME: Allow INTERRUPTIBLE. Which means
			  not having the wait_queue head on the stack.

			  If the wq_head is on the stack, and we
			  leave because we got interrupted, then the
			  wq_head is no longer there when the
			  callback routine tries to wake us up.
			*/
            /* 调用 mtd_erase 函数，该函数可以执行操作后立即返回
             * 也可以采用最简单的等待擦除完成后返回
             * 当擦除完成后，会使用等待队列唤醒 */
			ret = mtd_erase(mtd, erase);
			if (!ret) {
                /* 添加到等待队列中，并主动调用调度函数
                 * 当擦除完成时会被等待队列唤醒 */
				set_current_state(TASK_UNINTERRUPTIBLE);
				add_wait_queue(&waitq, &wait);
				if (erase->state != MTD_ERASE_DONE &&
				    erase->state != MTD_ERASE_FAILED)
					schedule();
				remove_wait_queue(&waitq, &wait);
				set_current_state(TASK_RUNNING);

                /* 擦除失败返回 EIO */
				ret = (erase->state == MTD_ERASE_FAILED)?-EIO:0;
			}
			kfree(erase);
		}
		break;
	}

	case MEMWRITEOOB:
	{
        /* oob 写操作 */
		struct mtd_oob_buf buf;
		struct mtd_oob_buf __user *buf_user = argp;

		/* NOTE: writes return length to buf_user->length */
		if (copy_from_user(&buf, argp, sizeof(buf)))
			ret = -EFAULT;
		else
			ret = mtdchar_writeoob(file, mtd, buf.start, buf.length,
				buf.ptr, &buf_user->length);
		break;
	}

	case MEMREADOOB:
	{
        /* oob 读操作 */
		struct mtd_oob_buf buf;
		struct mtd_oob_buf __user *buf_user = argp;

		/* NOTE: writes return length to buf_user->start */
		if (copy_from_user(&buf, argp, sizeof(buf)))
			ret = -EFAULT;
		else
			ret = mtdchar_readoob(file, mtd, buf.start, buf.length,
				buf.ptr, &buf_user->start);
		break;
	}

	case MEMWRITEOOB64:
	{
        /* oob 写操作 64位 */
		struct mtd_oob_buf64 buf;
		struct mtd_oob_buf64 __user *buf_user = argp;

		if (copy_from_user(&buf, argp, sizeof(buf)))
			ret = -EFAULT;
		else
			ret = mtdchar_writeoob(file, mtd, buf.start, buf.length,
				(void __user *)(uintptr_t)buf.usr_ptr,
				&buf_user->length);
		break;
	}

	case MEMREADOOB64:
	{
        /* oob 读操作 64位 */
		struct mtd_oob_buf64 buf;
		struct mtd_oob_buf64 __user *buf_user = argp;

		if (copy_from_user(&buf, argp, sizeof(buf)))
			ret = -EFAULT;
		else
			ret = mtdchar_readoob(file, mtd, buf.start, buf.length,
				(void __user *)(uintptr_t)buf.usr_ptr,
				&buf_user->length);
		break;
	}

	case MEMWRITE:
	{
        /* 参考 mtd-abi.h 中的 struct mtd_write_req 结构介绍 */
		ret = mtdchar_write_ioctl(mtd,
		      (struct mtd_write_req __user *)arg);
		break;
	}

	case MEMLOCK:
	{
        /* mtd lock 操作，需要底层flash驱动支持 */
		struct erase_info_user einfo;

		if (copy_from_user(&einfo, argp, sizeof(einfo)))
			return -EFAULT;

		ret = mtd_lock(mtd, einfo.start, einfo.length);
		break;
	}

	case MEMUNLOCK:
	{
        /* mtd unlock 操作，需要底层flash驱动支持 */
		struct erase_info_user einfo;

		if (copy_from_user(&einfo, argp, sizeof(einfo)))
			return -EFAULT;

		ret = mtd_unlock(mtd, einfo.start, einfo.length);
		break;
	}

	case MEMISLOCKED:
	{
        /* mtd is lock 操作，需要底层flash驱动支持 */
		struct erase_info_user einfo;

		if (copy_from_user(&einfo, argp, sizeof(einfo)))
			return -EFAULT;

		ret = mtd_is_locked(mtd, einfo.start, einfo.length);
		break;
	}

	/* Legacy interface */
    /* 遗留的接口 */
	case MEMGETOOBSEL:
	{
        /* NAND ECC 操作，不分析 */
		struct nand_oobinfo oi;

		if (!mtd->ecclayout)
			return -EOPNOTSUPP;
		if (mtd->ecclayout->eccbytes > ARRAY_SIZE(oi.eccpos))
			return -EINVAL;

		oi.useecc = MTD_NANDECC_AUTOPLACE;
		memcpy(&oi.eccpos, mtd->ecclayout->eccpos, sizeof(oi.eccpos));
		memcpy(&oi.oobfree, mtd->ecclayout->oobfree,
		       sizeof(oi.oobfree));
		oi.eccbytes = mtd->ecclayout->eccbytes;

		if (copy_to_user(argp, &oi, sizeof(struct nand_oobinfo)))
			return -EFAULT;
		break;
	}

	case MEMGETBADBLOCK:
	{
        /* 确认某个地址所在的块是否为坏块 */
		loff_t offs;

		if (copy_from_user(&offs, argp, sizeof(loff_t)))
			return -EFAULT;
		return mtd_block_isbad(mtd, offs);
		break;
	}

	case MEMSETBADBLOCK:
	{
        /* 标记某个地址所在的块为坏块 */
		loff_t offs;

		if (copy_from_user(&offs, argp, sizeof(loff_t)))
			return -EFAULT;
		return mtd_block_markbad(mtd, offs);
		break;
	}

#ifdef CONFIG_HAVE_MTD_OTP
    /* MTD 支持 OTP 存储时使用
     * OTP存储是一种只能编程一次的存储类型
     * 调用的都是 otp的处理函数，与普通的数据区不同 */
	case OTPSELECT:
	{
        /* otp 操作模式选择，需要具体的flash支持才可以 */
		int mode;
		if (copy_from_user(&mode, argp, sizeof(int)))
			return -EFAULT;

		mfi->mode = MTD_FILE_MODE_NORMAL;

		ret = otp_select_filemode(mfi, mode);

		file->f_pos = 0;
		break;
	}

	case OTPGETREGIONCOUNT:
	case OTPGETREGIONINFO:
	{
        /* 获取 otp 区域信息，数量 */
		struct otp_info *buf = kmalloc(4096, GFP_KERNEL);
		if (!buf)
			return -ENOMEM;
		switch (mfi->mode) {
		case MTD_FILE_MODE_OTP_FACTORY:
			ret = mtd_get_fact_prot_info(mtd, buf, 4096);
			break;
		case MTD_FILE_MODE_OTP_USER:
			ret = mtd_get_user_prot_info(mtd, buf, 4096);
			break;
		default:
			ret = -EINVAL;
			break;
		}
		if (ret >= 0) {
			if (cmd == OTPGETREGIONCOUNT) {
				int nbr = ret / sizeof(struct otp_info);
				ret = copy_to_user(argp, &nbr, sizeof(int));
			} else
				ret = copy_to_user(argp, buf, ret);
			if (ret)
				ret = -EFAULT;
		}
		kfree(buf);
		break;
	}

	case OTPLOCK:
	{
        /* mtd otp user 区域 锁定 lock */
		struct otp_info oinfo;

        /* 只能在操作 user otp 区域模式 */
		if (mfi->mode != MTD_FILE_MODE_OTP_USER)
			return -EINVAL;
		if (copy_from_user(&oinfo, argp, sizeof(oinfo)))
			return -EFAULT;
		ret = mtd_lock_user_prot_reg(mtd, oinfo.start, oinfo.length);
		break;
	}
#endif

	/* This ioctl is being deprecated - it truncates the ECC layout */
    /* 这个 ioctl 已经被弃用了 - 它截断了 ECC 布局  */
	case ECCGETLAYOUT:
	{
        /* 已废弃，不再分析 */
		struct nand_ecclayout_user *usrlay;

		if (!mtd->ecclayout)
			return -EOPNOTSUPP;

		usrlay = kmalloc(sizeof(*usrlay), GFP_KERNEL);
		if (!usrlay)
			return -ENOMEM;

		shrink_ecclayout(mtd->ecclayout, usrlay);

		if (copy_to_user(argp, usrlay, sizeof(*usrlay)))
			ret = -EFAULT;
		kfree(usrlay);
		break;
	}

	case ECCGETSTATS:
	{
        /* 获取 ECC 状态 */
		if (copy_to_user(argp, &mtd->ecc_stats,
				 sizeof(struct mtd_ecc_stats)))
			return -EFAULT;
		break;
	}

	case MTDFILEMODE:
	{
        /* 设置 mtd 文件操作模式 */
		mfi->mode = 0;

		switch(arg) {
		case MTD_FILE_MODE_OTP_FACTORY:
		case MTD_FILE_MODE_OTP_USER:
            /* 这是操作 OTP 区域的模式 */
			ret = otp_select_filemode(mfi, arg);
			break;

		case MTD_FILE_MODE_RAW:
			if (!mtd_has_oob(mtd))
				return -EOPNOTSUPP;
			mfi->mode = arg;

		case MTD_FILE_MODE_NORMAL:
			break;
		default:
			ret = -EINVAL;
		}
        /* 重设模式，会将文件指针复位 */
		file->f_pos = 0;
		break;
	}

	case BLKPG:
	{
        /* mtd 分区操作，可在应用层 添加/删除 分区 */
		ret = mtdchar_blkpg_ioctl(mtd,
		      (struct blkpg_ioctl_arg __user *)arg);
		break;
	}

	case BLKRRPART:
	{
		/* No reread partition feature. Just return ok */
        /* 无重读分区特性，返回OK */
		ret = 0;
		break;
	}

	default:
		ret = -ENOTTY;
	}

	return ret;
} /* memory_ioctl */

/* mtd unlocked_ioctl 回调
 * 在用户程序和内核的系统是相同位数（32/64）时，调用这个 ioctl */
static long mtdchar_unlocked_ioctl(struct file *file, u_int cmd, u_long arg)
{
	int ret;

    /* 所有的cmd都在 mtdchar_ioctl 中实现 */
	mutex_lock(&mtd_mutex);
	ret = mtdchar_ioctl(file, cmd, arg);
	mutex_unlock(&mtd_mutex);

	return ret;
}

#ifdef CONFIG_COMPAT
/* 兼容命令，32位 / 64位 兼容 
 * 是为了32位的应用成程序运行在 64位的内核上，调用 ioctl 时的问题 */

struct mtd_oob_buf32 {
	u_int32_t start;
	u_int32_t length;
	compat_caddr_t ptr;	/* unsigned char* */
};

#define MEMWRITEOOB32		_IOWR('M', 3, struct mtd_oob_buf32)
#define MEMREADOOB32		_IOWR('M', 4, struct mtd_oob_buf32)

/* 一些数据结构有差别，在 32位 64位 下需要保持一致
 * 大部分操作 最终还是调用了 mtdchar_ioctl 及逆行处理 */
static long mtdchar_compat_ioctl(struct file *file, unsigned int cmd,
	unsigned long arg)
{
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;
	void __user *argp = compat_ptr(arg);
	int ret = 0;

	mutex_lock(&mtd_mutex);

    /* // TODO: 为什么只有 MEMWRITEOOB32 和 MEMREADOOB32 
     * 这两个是需要32 / 64 兼容的 ？？？ */
	switch (cmd) {
	case MEMWRITEOOB32:
	{
		struct mtd_oob_buf32 buf;
		struct mtd_oob_buf32 __user *buf_user = argp;

		if (copy_from_user(&buf, argp, sizeof(buf)))
			ret = -EFAULT;
		else
			ret = mtdchar_writeoob(file, mtd, buf.start,
				buf.length, compat_ptr(buf.ptr),
				&buf_user->length);
		break;
	}

	case MEMREADOOB32:
	{
		struct mtd_oob_buf32 buf;
		struct mtd_oob_buf32 __user *buf_user = argp;

		/* NOTE: writes return length to buf->start */
		if (copy_from_user(&buf, argp, sizeof(buf)))
			ret = -EFAULT;
		else
			ret = mtdchar_readoob(file, mtd, buf.start,
				buf.length, compat_ptr(buf.ptr),
				&buf_user->start);
		break;
	}
	default:
		ret = mtdchar_ioctl(file, cmd, (unsigned long)argp);
	}

	mutex_unlock(&mtd_mutex);

	return ret;
}

#endif /* CONFIG_COMPAT */

/*
 * try to determine where a shared mapping can be made
 * - only supported for NOMMU at the moment (MMU can't doesn't copy private
 *   mappings)
 */
#ifndef CONFIG_MMU
static unsigned long mtdchar_get_unmapped_area(struct file *file,
					   unsigned long addr,
					   unsigned long len,
					   unsigned long pgoff,
					   unsigned long flags)
{
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;
	unsigned long offset;
	int ret;

	if (addr != 0)
		return (unsigned long) -EINVAL;

	if (len > mtd->size || pgoff >= (mtd->size >> PAGE_SHIFT))
		return (unsigned long) -EINVAL;

	offset = pgoff << PAGE_SHIFT;
	if (offset > mtd->size - len)
		return (unsigned long) -EINVAL;

	ret = mtd_get_unmapped_area(mtd, len, offset, flags);
	return ret == -EOPNOTSUPP ? -ENOSYS : ret;
}
#endif

/*
 * set up a mapping for shared memory segments
 */
/* 为共享内存段建立映射 */
static int mtdchar_mmap(struct file *file, struct vm_area_struct *vma)
{
#ifdef CONFIG_MMU
	struct mtd_file_info *mfi = file->private_data;
	struct mtd_info *mtd = mfi->mtd;
	struct map_info *map = mtd->priv;
	unsigned long start;
	unsigned long off;
	u32 len;

	if (mtd->type == MTD_RAM || mtd->type == MTD_ROM) {
		off = vma->vm_pgoff << PAGE_SHIFT;
		start = map->phys;
		len = PAGE_ALIGN((start & ~PAGE_MASK) + map->size);
		start &= PAGE_MASK;
		if ((vma->vm_end - vma->vm_start + off) > len)
			return -EINVAL;

		off += start;
		vma->vm_pgoff = off >> PAGE_SHIFT;
		vma->vm_flags |= VM_IO | VM_RESERVED;

#ifdef pgprot_noncached
		if (file->f_flags & O_DSYNC || off >= __pa(high_memory))
			vma->vm_page_prot = pgprot_noncached(vma->vm_page_prot);
#endif
		if (io_remap_pfn_range(vma, vma->vm_start, off >> PAGE_SHIFT,
				       vma->vm_end - vma->vm_start,
				       vma->vm_page_prot))
			return -EAGAIN;

		return 0;
	}
	return -ENOSYS;
#else
	return vma->vm_flags & VM_SHARED ? 0 : -ENOSYS;
#endif
}

/* 字符设备文件操作方法
 * 会在文件操作 /dev/mtdx 时调用
 * 因为是字符设备，这些函数都是从字符设备驱动中调的
 * 所有的 mtd 设备，都会调用到这里，进一步再调用到具体的驱动中
 * 大部分的 flash操作都是在 ioctl 函数中，如擦写、oob 
 *
 * 早期的内核(2.6以前)，只有一个 ioctl 回调 
 * 后面内核将 ioctl 去掉了，增加了两个 unlocked_ioctl compat_ioctl
 * 在支持 64位的 driver 中，必须实现 compat_ioctl 回调，这是用来兼容
 * 32位的用户空间程序的，32位空间调用64位的内核 ioctl时，将会调用 
 * compat_ioctl ，否则会返回错误 
 *
 * 如果是64位的用户程序调用64位的内核中的 ioctl，则调用 unlocked_ioctl
 * 当然，32位的用户程序调用32位的内核中的 ioctl,还是调用 unlocked_ioctl */
static const struct file_operations mtd_fops = {
	.owner		= THIS_MODULE,
	.llseek		= mtdchar_lseek,
	.read		= mtdchar_read,
	.write		= mtdchar_write,
	.unlocked_ioctl	= mtdchar_unlocked_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl	= mtdchar_compat_ioctl,
#endif
	.open		= mtdchar_open,
	.release	= mtdchar_close,
	.mmap		= mtdchar_mmap,
#ifndef CONFIG_MMU
	.get_unmapped_area = mtdchar_get_unmapped_area,
#endif
};

/* // TODO: 文件系统相关，不清楚具体作用，待分析 */
static const struct super_operations mtd_ops = {
	.drop_inode = generic_delete_inode,
	.statfs = simple_statfs,
};

static struct dentry *mtd_inodefs_mount(struct file_system_type *fs_type,
				int flags, const char *dev_name, void *data)
{
	return mount_pseudo(fs_type, "mtd_inode:", &mtd_ops, NULL, MTD_INODE_FS_MAGIC);
}

static struct file_system_type mtd_inodefs_type = {
       .name = "mtd_inodefs",
       .mount = mtd_inodefs_mount,
       .kill_sb = kill_anon_super,
};

/* mtd 子系统 mtd字符设备初始化入口
 * mtd本质还是字符设备，属于 mtd字符设备下的 mtd设备
 * mtd的所有字符设备操作都是通过字符设备传递的，到了字符设备文件操作后
 * 再传递到具体的mtd设备 */
static int __init init_mtdchar(void)
{
	int ret;

    /* mtd子系统 mtd字符设备注册
     * MTD_CHAR_MAJOR 是字符设备主设备号
     * 注册的次设备号是整个次设备号范围
     * 把所有的次设备号都注册了，这样就可以很方便的随意增删 mtd设备
     * mtd_fops 是字符设备文件操作方法，会在 open/read  /dev/mtdx
     * 操作时调用到 */
	ret = __register_chrdev(MTD_CHAR_MAJOR, 0, 1 << MINORBITS,
				   "mtd", &mtd_fops);
	if (ret < 0) {
		pr_notice("Can't allocate major number %d for "
				"Memory Technology Devices.\n", MTD_CHAR_MAJOR);
		return ret;
	}

    /* // TODO: 文件系统相关，不清楚具体作用，待分析 */
	ret = register_filesystem(&mtd_inodefs_type);
	if (ret) {
		pr_notice("Can't register mtd_inodefs filesystem: %d\n", ret);
		goto err_unregister_chdev;
	}
	return ret;

err_unregister_chdev:
	__unregister_chrdev(MTD_CHAR_MAJOR, 0, 1 << MINORBITS, "mtd");
	return ret;
}

static void __exit cleanup_mtdchar(void)
{
    /* 卸载文件系统，注销所有 mtd 字符设备 */
	unregister_filesystem(&mtd_inodefs_type);
	__unregister_chrdev(MTD_CHAR_MAJOR, 0, 1 << MINORBITS, "mtd");
}

module_init(init_mtdchar);
module_exit(cleanup_mtdchar);

MODULE_ALIAS_CHARDEV_MAJOR(MTD_CHAR_MAJOR);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("David Woodhouse <dwmw2@infradead.org>");
MODULE_DESCRIPTION("Direct character-device access to MTD devices");
MODULE_ALIAS_CHARDEV_MAJOR(MTD_CHAR_MAJOR);
