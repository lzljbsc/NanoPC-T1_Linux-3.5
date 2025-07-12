/*
 *  linux/fs/char_dev.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/init.h>
#include <linux/fs.h>
#include <linux/kdev_t.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <linux/major.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/seq_file.h>

#include <linux/kobject.h>
#include <linux/kobj_map.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/backing-dev.h>
#include <linux/tty.h>

#include "internal.h"

/*
 * capabilities for /dev/mem, /dev/kmem and similar directly mappable character
 * devices
 * - permits shared-mmap for read, write and/or exec
 * - does not permit private mmap in NOMMU mode (can't do COW)
 * - no readahead or I/O queue unplugging required
 */
struct backing_dev_info directly_mappable_cdev_bdi = {
	.name = "char",
	.capabilities	= (
#ifdef CONFIG_MMU
		/* permit private copies of the data to be taken */
		BDI_CAP_MAP_COPY |
#endif
		/* permit direct mmap, for read, write or exec */
		BDI_CAP_MAP_DIRECT |
		BDI_CAP_READ_MAP | BDI_CAP_WRITE_MAP | BDI_CAP_EXEC_MAP |
		/* no writeback happens */
		BDI_CAP_NO_ACCT_AND_WRITEBACK),
};

/* char dev 使用的 kobj_map 
 * 该结构是一个基于哈希的表， 通过设备号 和 范围 管理
 * 将需要注册的设备号 和 范围添加(map) 到管理结构中 
 * 使用时通过 设备号 进行查找，返回的是 cdev 结构中的 kobj 
 * 再通过 kobj 反查 cdev 即可， 过程见 chrdev_open 过程 
 * */
static struct kobj_map *cdev_map;

/* cdev 操作过程中互斥访问 mutex  */
static DEFINE_MUTEX(chrdevs_lock);

/* char dev 设备号管理结构
 * 该结构主要用于管理注册的主次设备号，字符设备名 
 * 会将所有注册的字符设备通过链表管理 
 * 插在哪个链表中，是根据主设备号 哈希得到的数组索引
 * 结构中的 cdev 是在注销字符设备时，将 对应的cdev 移除时使用的
 *
 * 注意，在 chrdev_open 函数中，使用某个具体的设备号 查找对应的 cdev 时，
 * 并不会使用本结构进行查询，而是使用 kobj_map 数据结构查询
 *
 * 为什么同组 （主设备号 + 次设备号范围） 需要两个结构管理 （chrdevs 、 kobj_map） 
 * 根据下面的代码分析，理解是：
 * chrdevs 结构用于管理所有注册的设备号，重点管理是 防止有重复的注册，
 *          并且提供注册的设备号 和 设备名 
 * kobj_map 结构用于管理注册的字符设备的查询，即给定一个特定的设备号，查询对应的
 *          cdev, kobj_map 是一个通用的管理方式，不关心管理的是什么结构，只是根据
 *          设备号管理/查询，并调用其它模块提供的匹配函数
 */
static struct char_device_struct {
	struct char_device_struct *next;    /* 链表管理 */
	unsigned int major;                 /* 主设备号 */
	unsigned int baseminor;             /* 次设备号 */
	int minorct;                        /* 次设备号个数 */
	char name[64];                      /* 字符设备名 */
	struct cdev *cdev;		/* will die */      /* 指向具体的 cdev 结构 */
} *chrdevs[CHRDEV_MAJOR_HASH_SIZE];

/* index in the above */
/* 根据主设备号计算 chrdevs 数组中的索引
 * 这里这样计算的目的，是为了把各个主设备号分散开，以后查找的时候，
 * 不用遍历太多就能找到 */
static inline int major_to_index(unsigned major)
{
    /* 主设备号 直接对 CHRDEV_MAJOR_HASH_SIZE 取余即可 */
	return major % CHRDEV_MAJOR_HASH_SIZE;
}

#ifdef CONFIG_PROC_FS

/* proc 文件系统支持函数 
 * 在 fs/proc/devices.c 文件中使用，用于在 cat /proc/devices 时，
 * 遍历每个主设备号下的 字符设备名 */
void chrdev_show(struct seq_file *f, off_t offset)
{
	struct char_device_struct *cd;

	if (offset < CHRDEV_MAJOR_HASH_SIZE) {
		mutex_lock(&chrdevs_lock);
		for (cd = chrdevs[offset]; cd; cd = cd->next)
			seq_printf(f, "%3d %s\n", cd->major, cd->name);
		mutex_unlock(&chrdevs_lock);
	}
}

#endif /* CONFIG_PROC_FS */

/*
 * Register a single major with a specified minor range.
 *
 * If major == 0 this functions will dynamically allocate a major and return
 * its number.
 *
 * If major > 0 this function will attempt to reserve the passed range of
 * minors and will return zero on success.
 *
 * Returns a -ve errno on failure.
 */
/* 注册一个字符设备结构，并指定了该设备的次设备号起始值和个数
 *
 * major    主设备号
 * baseminor 次设备号起始值
 * minorct   次设备号数量
 * name     字符设备名称
 * 
 * 如果 major = 0, 则自动分配一个主设备号
 * major > 0, 则内核以 major 为主设备号
 * 成功则返回 char_device_struct 结构体
 * */
static struct char_device_struct *
__register_chrdev_region(unsigned int major, unsigned int baseminor,
			   int minorct, const char *name)
{
	struct char_device_struct *cd, **cp;
	int ret = 0;
	int i;

    /* 分配一个结构体内存， chrdevs 只是个指针数组 */
	cd = kzalloc(sizeof(struct char_device_struct), GFP_KERNEL);
	if (cd == NULL)
		return ERR_PTR(-ENOMEM);

	mutex_lock(&chrdevs_lock);

    /* 如果 major = 0, 则从 chrdevs 数组中找一个空元素的索引做为主设备号 */
	/* temporary */
	if (major == 0) {
		for (i = ARRAY_SIZE(chrdevs)-1; i > 0; i--) {
			if (chrdevs[i] == NULL)
				break;
		}

		if (i == 0) {
			ret = -EBUSY;
			goto out;
		}
		major = i;
		ret = major;
	}

    /* 无论是传入的主设备号还是自动分配的，到这里 major 都是主设备号了 
     * 现在主设备号、次设备号、次设备号数量、设备名都已确定了 */
	cd->major = major;
	cd->baseminor = baseminor;
	cd->minorct = minorct;
	strlcpy(cd->name, name, sizeof(cd->name));

    /* 根据主设备号计算在 chrdevs 中的索引 */
	i = major_to_index(major);

    /* 在本次需要注册的字符设备的主设备号所在的 chrdevs 数组索引中
     * 寻找需要插入的位置 
     * 插入的各个字符设备结构是按照 主设备、次设备号 从小到大排列的
     * 先以主设备号为准，如果主设备号相同，则以次设备排列 */
	for (cp = &chrdevs[i]; *cp; cp = &(*cp)->next)
		if ((*cp)->major > major ||
		    ((*cp)->major == major &&
		     (((*cp)->baseminor >= baseminor) ||
		      ((*cp)->baseminor + (*cp)->minorct > baseminor))))
			break;

    /* 通过上面的位置查找 cp 要么为NULL (chrdevs 无任何元素/查找到最后一个)
     * 要么为某个元素 
     * 如果为某个元素，那就说明有可能已经注册过相同主设备号了
     * 这时就需要判断次设备号是否已经被占用了 */
	/* Check for overlapping minor ranges.  */
	if (*cp && (*cp)->major == major) {
		int old_min = (*cp)->baseminor;
		int old_max = (*cp)->baseminor + (*cp)->minorct - 1;
		int new_min = baseminor;
		int new_max = baseminor + minorct - 1;

        /* 新注册的次设备号范围在已注册的范围之内，则无法注册 */
		/* New driver overlaps from the left.  */
		if (new_max >= old_min && new_max <= old_max) {
			ret = -EBUSY;
			goto out;
		}

        /* 新注册的次设备号范围在已注册的范围之内，则无法注册 */
		/* New driver overlaps from the right.  */
		if (new_min <= old_max && new_min >= old_min) {
			ret = -EBUSY;
			goto out;
		}
	}

    /* 将新设备结构插入链表中 */
	cd->next = *cp;
	*cp = cd;
	mutex_unlock(&chrdevs_lock);
	return cd;
out:
	mutex_unlock(&chrdevs_lock);
	kfree(cd);
	return ERR_PTR(ret);
}

/* 移除一个字符设备结构
 * 指定主设备号、次设备号起始值、次设备号个数
 * 需要三者完全匹配才可以 */
static struct char_device_struct *
__unregister_chrdev_region(unsigned major, unsigned baseminor, int minorct)
{
	struct char_device_struct *cd = NULL, **cp;
	int i = major_to_index(major);

    /* 根据主设备号计算的数组索引，
     * 依次遍历所有链表成员，找到完全匹配项 */
	mutex_lock(&chrdevs_lock);
	for (cp = &chrdevs[i]; *cp; cp = &(*cp)->next)
		if ((*cp)->major == major &&
		    (*cp)->baseminor == baseminor &&
		    (*cp)->minorct == minorct)
			break;

    /* 将找到的项使用下一个成员替代
     * cp 是一个二级指针，cp 的值是 chrdevs[i] / ->next 的地址，是数组成员所在的地址 
     * *cp 是 chrdevs[i] / ->next 这个数组中的值，也就是里面某个元素所在的地址，
     *      比如有一个 struct char_device_struct 结构了 cds1 , 放在了 chrdevs[2] 
     *      那 cp 是 chrdevs[2] 这个数组元素所在的地址， *cp 是 cds1 的地址 
     * 特别注意的， chrdevs 是一个指针数组，也就是里面放的都是指针 
     * 默认里面所有的元素都是NULL， 就是 &chrdevs[2] 是有地址的，
     * 但 chrdevs[2] 是里面的内容了，也即是 NULL
     * 所以上面的处理中， 定义的是 **cp  cp = &chrdevs[i] 
     * 而在使用时，是  (*cp)->major 
     * 这样的处理，就能保证在 chrdevs[i] 没有注册设备时为 NULL 
     * 当已经注册设备后，chrdevs[i] 就是第一个设备结构 */
	if (*cp) {
		cd = *cp;
		*cp = cd->next;
	}
	mutex_unlock(&chrdevs_lock);
	return cd;
}

/**
 * register_chrdev_region() - register a range of device numbers
 * @from: the first in the desired range of device numbers; must include
 *        the major number.
 * @count: the number of consecutive device numbers required
 * @name: the name of the device or driver.
 *
 * Return value is zero on success, a negative error code on failure.
 */
/* 注册指定范围的设备号
 *
 * from     起始设备号，必须包括主设备号，也就是一个完整的设备号 MKDEV(major, minor)
 * count    注册请求的总数量 
 * name     设备或驱动名 */
int register_chrdev_region(dev_t from, unsigned count, const char *name)
{
	struct char_device_struct *cd;
	dev_t to = from + count;
	dev_t n, next;

    /* from 是起始的设备号 to 是结束的设备号  
     * 注意，如果 count 过大，to 可能跨主设备号 
     * 这里遍历时，先计算下一个主设备号/次设备号为0的设备号，检查是否跨主设备号 
     * 如果跨主设备号了，则每次最多只注册到当前主设备号下的最多次设备号的数量 
     * 然后再次检查下一个主设备号，依次循环判断 */
	for (n = from; n < to; n = next) {
		next = MKDEV(MAJOR(n)+1, 0);
		if (next > to)
			next = to;
		cd = __register_chrdev_region(MAJOR(n), MINOR(n),
			       next - n, name);
		if (IS_ERR(cd))
			goto fail;
	}
	return 0;
fail:
	to = n;
	for (n = from; n < to; n = next) {
		next = MKDEV(MAJOR(n)+1, 0);
		kfree(__unregister_chrdev_region(MAJOR(n), MINOR(n), next - n));
	}
	return PTR_ERR(cd);
}

/**
 * alloc_chrdev_region() - register a range of char device numbers
 * @dev: output parameter for first assigned number
 * @baseminor: first of the requested range of minor numbers
 * @count: the number of minor numbers required
 * @name: the name of the associated device or driver
 *
 * Allocates a range of char device numbers.  The major number will be
 * chosen dynamically, and returned (along with the first minor number)
 * in @dev.  Returns zero or a negative error code.
 */
/* 注册一个主设备号由内核动态分配，次设备号为 baseminor - baseminor+count 
 * 的设备 
 *
 * dev:     注册的首个设备号 
 * baseminor    首个次设备号 
 * count    注册总数量 
 * name     设备名
 * */
int alloc_chrdev_region(dev_t *dev, unsigned baseminor, unsigned count,
			const char *name)
{
    /* 直接调用 __register_chrdev_region 
     * 主设备号传入 0 即可， __register_chrdev_region 函数中已做处理 */
	struct char_device_struct *cd;
	cd = __register_chrdev_region(0, baseminor, count, name);
	if (IS_ERR(cd))
		return PTR_ERR(cd);
	*dev = MKDEV(cd->major, cd->baseminor);
	return 0;
}

/**
 * __register_chrdev() - create and register a cdev occupying a range of minors
 * @major: major device number or 0 for dynamic allocation
 * @baseminor: first of the requested range of minor numbers
 * @count: the number of minor numbers required
 * @name: name of this range of devices
 * @fops: file operations associated with this devices
 *
 * If @major == 0 this functions will dynamically allocate a major and return
 * its number.
 *
 * If @major > 0 this function will attempt to reserve a device with the given
 * major number and will return zero on success.
 *
 * Returns a -ve errno on failure.
 *
 * The name of this device has nothing to do with the name of the device in
 * /dev. It only helps to keep track of the different owners of devices. If
 * your module name has only one type of devices it's ok to use e.g. the name
 * of the module here.
 */
/* 创建并注册一定次设备号范围的 cdev 设备 
 *
 * major    主设备号， 0为动态分配 
 * baseminor 次设备号起始值 
 * count    总的设备号数量 
 * name     设备名 
 * fops     设备的文件操作方法
 *
 * 注意，该设备的名称与 /dev 目录下的设备名称无关 
 * 它只有助于跟踪设备的不同所有者。 如果你的模块名称只有一种类型的设备，
 * 可以使用这里的模块名称 */
int __register_chrdev(unsigned int major, unsigned int baseminor,
		      unsigned int count, const char *name,
		      const struct file_operations *fops)
{
	struct char_device_struct *cd;
	struct cdev *cdev;
	int err = -ENOMEM;

    /* 注册字符设备 */
	cd = __register_chrdev_region(major, baseminor, count, name);
	if (IS_ERR(cd))
		return PTR_ERR(cd);

    /* 分配一个 cdev 结构，里面做了 kobject 的初始化 */
	cdev = cdev_alloc();
	if (!cdev)
		goto out2;

    /* 初始化 cdev ，设置 kobject 名 
     * 很重要的，要设置 ops */
	cdev->owner = fops->owner;
	cdev->ops = fops;
	kobject_set_name(&cdev->kobj, "%s", name);

    /* 添加注册 cdev 到系统 */
	err = cdev_add(cdev, MKDEV(cd->major, baseminor), count);
	if (err)
		goto out;

	cd->cdev = cdev;

	return major ? 0 : cd->major;
out:
	kobject_put(&cdev->kobj);
out2:
	kfree(__unregister_chrdev_region(cd->major, baseminor, count));
	return err;
}

/**
 * unregister_chrdev_region() - return a range of device numbers
 * @from: the first in the range of numbers to unregister
 * @count: the number of device numbers to unregister
 *
 * This function will unregister a range of @count device numbers,
 * starting with @from.  The caller should normally be the one who
 * allocated those numbers in the first place...
 */
/* 注销特定设备号范围的设备 
 * from     设备号起始值 
 * count    设备号数量 */
void unregister_chrdev_region(dev_t from, unsigned count)
{
	dev_t to = from + count;
	dev_t n, next;

	for (n = from; n < to; n = next) {
		next = MKDEV(MAJOR(n)+1, 0);
		if (next > to)
			next = to;
		kfree(__unregister_chrdev_region(MAJOR(n), MINOR(n), next - n));
	}
}

/**
 * __unregister_chrdev - unregister and destroy a cdev
 * @major: major device number
 * @baseminor: first of the range of minor numbers
 * @count: the number of minor numbers this cdev is occupying
 * @name: name of this range of devices
 *
 * Unregister and destroy the cdev occupying the region described by
 * @major, @baseminor and @count.  This function undoes what
 * __register_chrdev() did.
 */
/* 注销并销毁 特定设备号的 cdev  */
void __unregister_chrdev(unsigned int major, unsigned int baseminor,
			 unsigned int count, const char *name)
{
	struct char_device_struct *cd;

    /* 直接按照 主设备号 次设备号 数量 释放即可
     * 若成功释放，需单独再释放 cdev 这个步骤会释放 设备结构体 */
	cd = __unregister_chrdev_region(major, baseminor, count);
	if (cd && cd->cdev)
		cdev_del(cd->cdev);
	kfree(cd);
}

/* 定义操作 cdev 时自旋锁  */
static DEFINE_SPINLOCK(cdev_lock);

/* 获取 cdev ，返回的是 cdev 中的 kobject  
 * 主要是判断 owner 及 获取 kobject */
static struct kobject *cdev_get(struct cdev *p)
{
	struct module *owner = p->owner;
	struct kobject *kobj;

	if (owner && !try_module_get(owner))
		return NULL;
	kobj = kobject_get(&p->kobj);
	if (!kobj)
		module_put(owner);
	return kobj;
}

/* 释放 cdev 
 * 主要是 释放 kobject 及释放 module owner */
void cdev_put(struct cdev *p)
{
	if (p) {
		struct module *owner = p->owner;
		kobject_put(&p->kobj);
		module_put(owner);
	}
}

/*
 * Called every time a character special file is opened
 */
/* 每次一个 字符设备打开的时候都会调用
 * 这个函数会在文件系统中调用 打开时，通过判断打开的节点类型
 * 如果节点为 字符设备，open 会调用到这里 */
static int chrdev_open(struct inode *inode, struct file *filp)
{
	struct cdev *p;
	struct cdev *new = NULL;
	int ret = 0;

    /* 传入的 inode 结构中，包含有设备号信息  inode->i_rdev
     * 还有 与设备类型对应的结构， 字符设备则为 inode->i_cdev */
    /* p 就是注册的字符设备的 cdev 
     * i_cdev 可能为NULL，因为第一次打开时，还无法对应 cdev */
	spin_lock(&cdev_lock);
	p = inode->i_cdev;
	if (!p) {
        /* 如果 i_cdev = NULL , 则需要根据设备号查找 */
		struct kobject *kobj;
		int idx;
		spin_unlock(&cdev_lock);
        /* kobj_lookup 用于根据 设备号查找对应的 kobj 结构 
         * 返回的是 cdev 结构中的 kobj 结构指针 ,这个指针是在 kobj_lookup 函数中
         * 匹配后，又调用了 probe 回调（就是本文件中的 exact_match）得到的
         * idx 是查找的设备号的次设备号 相对 这个主设备号注册时的起始次设备号的
         * 偏移 这里并没有使用，但 kobj_lookup 要求必须传一个 int 指针 */
		kobj = kobj_lookup(cdev_map, inode->i_rdev, &idx);
		if (!kobj)
			return -ENXIO;
        /* 使用 kobj_lookup 函数查到的 kobj 反向查 cdev 结构，
         * 这样就完成了 使用设备号 查询 cdev 结构 
         * 也就是应用层打开设备节点，根据设备号找到内核中字符设备结构了 */
		new = container_of(kobj, struct cdev, kobj);
		spin_lock(&cdev_lock);
		/* Check i_cdev again in case somebody beat us to it while
		   we dropped the lock. */
        /* 再次检查 inode->i_cdev 
         * 如果为 NULL ，则赋值为刚才查到的 cdev 
         * 字符设备 和 文件系统 inode 绑定了 */
		p = inode->i_cdev;
		if (!p) {
			inode->i_cdev = p = new;
			list_add(&inode->i_devices, &p->list);
			new = NULL;
		} else if (!cdev_get(p))
			ret = -ENXIO;
	} else if (!cdev_get(p))
		ret = -ENXIO;
	spin_unlock(&cdev_lock);
	cdev_put(new);
	if (ret)
		return ret;

    /* p 就是打开的设备节点对应的字符设备 
     * p->ops 就是在注册字符设备时注册的 ops  */
	ret = -ENXIO;
    /* 这是把 cdev 对应的 fops 赋给 文件描述符 f_op 
     * 这样在以后操作该文件描述符时，就使用 设备的文件操作方法 了*/
	filp->f_op = fops_get(p->ops);
	if (!filp->f_op)
		goto out_cdev_put;

    /* 字符设备注册时会有 open方法， 这里需要调用一下
     * 到这里，是首次调用到 字符设备的文件操作方法 
     * open 函数把 inode filp 做为参数，在 open中就可以使用 iminor 获取次设备号
     * 使用 filp->private_data 指向驱动私有数据，供其它文件操作方法使用 */
	if (filp->f_op->open) {
		ret = filp->f_op->open(inode, filp);
		if (ret)
			goto out_cdev_put;
	}

	return 0;

 out_cdev_put:
	cdev_put(p);
	return ret;
}

/* 在需要释放 inode 结构时调用
 * 用于清理 inode 结构绑定的 i_cdev 结构 
 * 字符设备并未做很多工作，只是把 inode->i_cdev 设为 NULL */
void cd_forget(struct inode *inode)
{
	spin_lock(&cdev_lock);
	list_del_init(&inode->i_devices);
	inode->i_cdev = NULL;
	spin_unlock(&cdev_lock);
}

/* kobject .release 函数调用
 * 用于释放 cdev 在 inode 中的一些链接关系 
 * 在 chrdev_open 时，会把 cdev 链到  inode->i_devices 中 
 * 这里把它们都摘除， 并把 inode->i_cdev 设为 NULL */
static void cdev_purge(struct cdev *cdev)
{
	spin_lock(&cdev_lock);
	while (!list_empty(&cdev->list)) {
		struct inode *inode;
		inode = container_of(cdev->list.next, struct inode, i_devices);
		list_del_init(&inode->i_devices);
		inode->i_cdev = NULL;
	}
	spin_unlock(&cdev_lock);
}

/*
 * Dummy default file-operations: the only thing this does
 * is contain the open that then fills in the correct operations
 * depending on the special file...
 */
/* 虚拟默认文件操作： 它所做的唯一事情是包含打开，然后根据特殊文件
 * 填充正确的操作 
 * 这就是在 文件打开时使用的 见 init_special_inode  */
const struct file_operations def_chr_fops = {
	.open = chrdev_open,
	.llseek = noop_llseek,
};

/* kobj_map 功能使用的两个函数
 *
 * exact_match 用于在 kobj_lookup 找到具体的项时调用 
 * dev 是设备号 
 * *part 是次设备号偏移 
 * *data 是被 kobj 管理的数据（*data 成员），这里就是 cdev 结构 
 * 本函数用于返回找到的 cdev 结构中的 kobj 成员 */
static struct kobject *exact_match(dev_t dev, int *part, void *data)
{
	struct cdev *p = data;
	return &p->kobj;
}

/* exact_lock 函数用于在 kobj_lookup 中调用
 * 这里操作是  cdev_get ， 应该就是 chrdev_open 中调用 kobj_lookup 的
 * 流程中并未显式调用 cdev_get 的原因，kobj_lookup 中隐式调用了 */
static int exact_lock(dev_t dev, void *data)
{
	struct cdev *p = data;
	return cdev_get(p) ? 0 : -1;
}

/**
 * cdev_add() - add a char device to the system
 * @p: the cdev structure for the device
 * @dev: the first device number for which this device is responsible
 * @count: the number of consecutive minor numbers corresponding to this
 *         device
 *
 * cdev_add() adds the device represented by @p to the system, making it
 * live immediately.  A negative error code is returned on failure.
 */
/* 向系统中添加一个 cdev 
 * 这是在 __register_chrdev 函数中，已经注册了主次设备号
 * 并分配了 cdev 结构， 然后将 cdev 结构放到 kobj_map 中管理 */
int cdev_add(struct cdev *p, dev_t dev, unsigned count)
{
    /* cdev 赋值 设备号 和 设备数量 */
	p->dev = dev;
	p->count = count;
    /* 调用 kobj_map 将 cdev 结构管理起来 
     * kobj_map 功能并不关心管理的具体数据结构，所以传入的 p 是 void * 类型 */
	return kobj_map(cdev_map, dev, count, NULL, exact_match, exact_lock, p);
}

/* 从 kobj_map 中移除指定的设备号对应的管理结构  */
static void cdev_unmap(dev_t dev, unsigned count)
{
	kobj_unmap(cdev_map, dev, count);
}

/**
 * cdev_del() - remove a cdev from the system
 * @p: the cdev structure to be removed
 *
 * cdev_del() removes @p from the system, possibly freeing the structure
 * itself.
 */
/* 从系统中移除一个 cdev 
 * 本操作中，如果 cdev 是 cdev_alloc 分配的，会自动释放 */
void cdev_del(struct cdev *p)
{
    /* 先移除在 kobj_map 中管理的结构，再释放 kobject （同时会释放 cdev ） */
	cdev_unmap(p->dev, p->count);
	kobject_put(&p->kobj);
}


/* kobj_type 中的两个 release 回调函数 
 * 在调用 kobject_put 时会自动调用到，用于释放 cdev 自身 */
static void cdev_default_release(struct kobject *kobj)
{
	struct cdev *p = container_of(kobj, struct cdev, kobj);
	cdev_purge(p);
}

static void cdev_dynamic_release(struct kobject *kobj)
{
	struct cdev *p = container_of(kobj, struct cdev, kobj);
	cdev_purge(p);
	kfree(p);
}

/* 静态分配的 cdev 的 kobj_type 
 * 静态分配的，无需释放 cdev */
static struct kobj_type ktype_cdev_default = {
	.release	= cdev_default_release,
};

/* 动态分配的 cdev 的 kobj_type 
 * 动态分配的，需要释放 cdev */
static struct kobj_type ktype_cdev_dynamic = {
	.release	= cdev_dynamic_release,
};

/**
 * cdev_alloc() - allocate a cdev structure
 *
 * Allocates and returns a cdev structure, or NULL on failure.
 */
/* 动态分配一个 cdev 结构，并初始化了 kobject  */
struct cdev *cdev_alloc(void)
{
	struct cdev *p = kzalloc(sizeof(struct cdev), GFP_KERNEL);
	if (p) {
		INIT_LIST_HEAD(&p->list);
        /* cdev 的 kobject 中的 ktype 赋值
         * 只有一个 release 函数，用于释放 cdev 内存 */
		kobject_init(&p->kobj, &ktype_cdev_dynamic);
	}
	return p;
}

/**
 * cdev_init() - initialize a cdev structure
 * @cdev: the structure to initialize
 * @fops: the file_operations for this device
 *
 * Initializes @cdev, remembering @fops, making it ready to add to the
 * system with cdev_add().
 */
/* 初始化一个 cdev 结构， 这个结构时预分配的 
 * 这个和 cdev_alloc 区别就是代码中预分配 还是 自动分配 
 * 其它的初始化等操作都需要 */
void cdev_init(struct cdev *cdev, const struct file_operations *fops)
{
	memset(cdev, 0, sizeof *cdev);
	INIT_LIST_HEAD(&cdev->list);
	kobject_init(&cdev->kobj, &ktype_cdev_default);
	cdev->ops = fops;
}

/* kobj_map 管理结构中默认的 get 方法回调函数，正常流程中不会使用到 */
static struct kobject *base_probe(dev_t dev, int *part, void *data)
{
	if (request_module("char-major-%d-%d", MAJOR(dev), MINOR(dev)) > 0)
		/* Make old-style 2.4 aliases work */
		request_module("char-major-%d", MAJOR(dev));
	return NULL;
}

/* 初始化 chrdev 模块 
 * 初始化特定的数据结构 */
void __init chrdev_init(void)
{
    /* 初始化 kobj_map 管理结构，所有的 字符设备 都会被 cdev_map 管理 */
	cdev_map = kobj_map_init(base_probe, &chrdevs_lock);
	bdi_init(&directly_mappable_cdev_bdi);
}


/* Let modules do char dev stuff */
EXPORT_SYMBOL(register_chrdev_region);
EXPORT_SYMBOL(unregister_chrdev_region);
EXPORT_SYMBOL(alloc_chrdev_region);
EXPORT_SYMBOL(cdev_init);
EXPORT_SYMBOL(cdev_alloc);
EXPORT_SYMBOL(cdev_del);
EXPORT_SYMBOL(cdev_add);
EXPORT_SYMBOL(__register_chrdev);
EXPORT_SYMBOL(__unregister_chrdev);
EXPORT_SYMBOL(directly_mappable_cdev_bdi);
