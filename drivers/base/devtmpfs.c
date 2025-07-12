/*
 * devtmpfs - kernel-maintained tmpfs-based /dev
 *
 * Copyright (C) 2009, Kay Sievers <kay.sievers@vrfy.org>
 *
 * During bootup, before any driver core device is registered,
 * devtmpfs, a tmpfs-based filesystem is created. Every driver-core
 * device which requests a device node, will add a node in this
 * filesystem.
 * 在启动期间，在注册任何驱动程序核心设备之前，将创建一个基于 tmpfs
 * 的文件系统 devtmpfs。 每个请求设备节点的驱动核心设备都会在这个文件
 * 系统中添加一个节点。
 * By default, all devices are named after the name of the device,
 * owned by root and have a default mode of 0600. Subsystems can
 * overwrite the default setting if needed.
 * 默认情况下，所有设备都以设备名称命名，归root所有，默认模式为 0600
 * 如果需要，子系统可以覆盖默认配置
 */

#include <linux/kernel.h>
#include <linux/syscalls.h>
#include <linux/mount.h>
#include <linux/device.h>
#include <linux/genhd.h>
#include <linux/namei.h>
#include <linux/fs.h>
#include <linux/shmem_fs.h>
#include <linux/ramfs.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/kthread.h>

/* devtmpfsd 线程句柄，调用导出函数时，用于判断 devtmpfs 是否正常启动了
 * 用于唤醒处理线程 */
static struct task_struct *thread;

/* 控制 devtmpfs 是否自动挂载
 * 如果没有自动挂载，可以在系统启动后，使用
 * mount -t devtmpfs devtmpfs /dev 手动挂载
 * 注意，虽然没有自动挂载，但在 devtmpfs 文件系统下，
 * 已经完成了各设备文件的创建，devtmpfsd 线程中的各种处理已经完成了，
 * 只是没有把文件系统挂载上，没有展现出来而已（已经过实测）
 * 当调用挂载后，会将各设备文件立即显示出来 */
#if defined CONFIG_DEVTMPFS_MOUNT
static int mount_dev = 1;
#else
static int mount_dev;
#endif

/* 保护 struct req  /  requests
 * 在 devtmpfsd 线程处理具体请求和休眠时需要释放
 * requests 用于获取一串 struct req 请求 */
static DEFINE_SPINLOCK(req_lock);

/* 设备节点 处理请求结构
 * 用于对外API组织数据，由后台线程 devtmpfsd 处理请求 的交互过程使用 */
static struct req {
    /* 链接下一个 struct req  */
	struct req *next;
    /* 完成量，同步一个处理操作  */
	struct completion done;
    /* 设备节点操作返回的错误码 */
	int err;
    /* 需要操作的设备节点名 */
	const char *name;
    /* 设备节点读写权限，0表示移除设备节点 */
	umode_t mode;	/* 0 => delete */
	struct device *dev;
} *requests;

/* 内核启动参数，通过uboot传参控制 devtmpfs 是否自动挂载
 * uboot bootargs 中设置 devtmpfs.mount=0/1 即可 */
static int __init mount_param(char *str)
{
	mount_dev = simple_strtoul(str, NULL, 0);
	return 1;
}
__setup("devtmpfs.mount=", mount_param);

/* 文件系统 mount 时回调 */
static struct dentry *dev_mount(struct file_system_type *fs_type, int flags,
		      const char *dev_name, void *data)
{
    /* 挂载时关键处理，用于填充文件系统的超级块
     * 根据是否配置了 tmpfs ，使用不同的处理 
     * // TODO: 具体过程待分析 */
#ifdef CONFIG_TMPFS
	return mount_single(fs_type, flags, data, shmem_fill_super);
#else
	return mount_single(fs_type, flags, data, ramfs_fill_super);
#endif
}

/* devtmpfs 文件系统
 * 在挂载时，将调用 dev_mount 回调
 * 具体过程，可以参考 devtmpfs_mount 或直接参考 sys_mount 系统调用 */
static struct file_system_type dev_fs_type = {
	.name = "devtmpfs",
	.mount = dev_mount,
	.kill_sb = kill_litter_super,
};

/* 判断设备是否为 块设备
 * 通过判断设备的 class 确定
 * 当系统编译未使用块设备时，直接返回否 */
#ifdef CONFIG_BLOCK
static inline int is_blockdev(struct device *dev)
{
	return dev->class == &block_class;
}
#else
static inline int is_blockdev(struct device *dev) { return 0; }
#endif

/* devtmpfs 创建节点
 * 该函数是在 device_add 函数中调用
 * 在注册设备时，会调用该函数创建设备节点 */
int devtmpfs_create_node(struct device *dev)
{
	const char *tmp = NULL;
	struct req req;

    /* devtmpfsd 线程运行正常才可以发送创建请求 */
	if (!thread)
		return 0;

    /* 获取将要创建的 设备节点名 及 读写属性
     * 必须有设备节点名，不然怎么创建。。。。 */
	req.mode = 0;
	req.name = device_get_devnode(dev, &req.mode, &tmp);
	if (!req.name)
		return -ENOMEM;

    /* 未设置读写属性的，给个默认属性 0600 */
	if (req.mode == 0)
		req.mode = 0600;
    /* 确认设备属于 字符设备 or 块设备，这很关键！！！ */
	if (is_blockdev(dev))
		req.mode |= S_IFBLK;
	else
		req.mode |= S_IFCHR;

    /* 指向待创建的设备结构 */
	req.dev = dev;

    /* 初始化完成量
     * 用于同步操作， devtmpfsd 线程将在创建完成后发送完成量信号 */
	init_completion(&req.done);

    /* 将请求结构 req 添加到链表 requests 中
     * 将插在未处理的链表中的最前面，后到先处理
     * 这里需要结合 devtmpfsd 线程中的处理一起看
     * 线程中从 requests 中取出，并立即将其置为 NULL
     * 先后从 req.next 链表中依次处理 */
	spin_lock(&req_lock);
	req.next = requests;
	requests = &req;
	spin_unlock(&req_lock);

    /* 唤醒 devtmpfsd 线程，处理这一次的节点创建请求
     * 并等待完成量唤醒 */
	wake_up_process(thread);
	wait_for_completion(&req.done);

    /* tmp 可能指向 device_get_devnode 中分配的空间
     * 该空间用于设备名的创建
     * 大部分情况下，是不会新分配空间存放的 */
	kfree(tmp);

	return req.err;
}

/* 移除 devtmpfs 设备节点  */
int devtmpfs_delete_node(struct device *dev)
{
	const char *tmp = NULL;
	struct req req;

    /* devtmpfsd 线程运行正常才可以发送创建请求 */
	if (!thread)
		return 0;

    /* 获取将要移除的设备节点名 */
	req.name = device_get_devnode(dev, NULL, &tmp);
	if (!req.name)
		return -ENOMEM;

    /* mode = 0  则为移除操作 */
	req.mode = 0;
	req.dev = dev;

    /* 初始化完成量
     * 用于同步操作， devtmpfsd 线程将在移除完成后发送完成量信号 */
	init_completion(&req.done);

    /* 将请求结构 req 添加到链表 requests 中
     * 将插在未处理的链表中的最前面，后到先处理
     * 这里需要结合 devtmpfsd 线程中的处理一起看
     * 线程中从 requests 中取出，并立即将其置为 NULL
     * 先后从 req.next 链表中依次处理 */
	spin_lock(&req_lock);
	req.next = requests;
	requests = &req;
	spin_unlock(&req_lock);

    /* 唤醒 devtmpfsd 线程，处理这一次的节点移除请求
     * 并等待完成量唤醒 */
	wake_up_process(thread);
	wait_for_completion(&req.done);

    /* tmp 可能指向 device_get_devnode 中分配的空间
     * 该空间用于设备名的创建
     * 大部分情况下，是不会新分配空间存放的 */
	kfree(tmp);

	return req.err;
}

/* 创建一个目录
 * // TODO: 内部详细流程待分析 */
static int dev_mkdir(const char *name, umode_t mode)
{
	struct dentry *dentry;
	struct path path;
	int err;

	dentry = kern_path_create(AT_FDCWD, name, &path, 1);
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

	err = vfs_mkdir(path.dentry->d_inode, dentry, mode);
	if (!err)
		/* mark as kernel-created inode */
		dentry->d_inode->i_private = &thread;
	dput(dentry);
	mutex_unlock(&path.dentry->d_inode->i_mutex);
	path_put(&path);
	return err;
}

/* 创建目录 */
static int create_path(const char *nodepath)
{
	char *path;
	char *s;
	int err = 0;

	/* parent directories do not exist, create them */
    /* 父目录不存在是，创建父目录 */
	path = kstrdup(nodepath, GFP_KERNEL);
	if (!path)
		return -ENOMEM;

    /* 目录是逐级创建的
     * 假设有多级目录  a/b/c/d/e/
     * 在下面的处理中，会先将 a目录后面的 / 替换为 \0
     * 此时就变成了 a 一个目录了，可以直接创建成功
     * 接着再把目录字符串变成  a/b 创建b目录
     * 这样依次创建完成所有目录 */
	s = path;
	for (;;) {
		s = strchr(s, '/');
		if (!s)
			break;
		s[0] = '\0';
		err = dev_mkdir(path, 0755);
		if (err && err != -EEXIST)
			break;
		s[0] = '/';
		s++;
	}
	kfree(path);
	return err;
}

/* 设备节点创建处理流程
 * // TODO: 内部涉及文件系统的处理 待分析 */
static int handle_create(const char *nodename, umode_t mode, struct device *dev)
{
	struct dentry *dentry;
	struct path path;
	int err;

    /* 对 kern_path_create 不太了解
     * 下面的过程应该是直接创建 设备节点目录及设备节点 的 path 结构
     * 但如果有不存在的目录，那 kern_path_create 将会失败
     * 此时，将使用 create_path 逐级创建目录，
     * 然后再使用 kern_path_create 创建设备节点 的 path 结构
     * 这里的 path 结构 应该不会真正创建文件，只是记录了要如何处理这个文件
     * 记录下这个文件的父目录等等信息 */
	dentry = kern_path_create(AT_FDCWD, nodename, &path, 0);
	if (dentry == ERR_PTR(-ENOENT)) {
		create_path(nodename);
		dentry = kern_path_create(AT_FDCWD, nodename, &path, 0);
	}
	if (IS_ERR(dentry))
		return PTR_ERR(dentry);

    /* 创建设备文件
     * // TODO: 具体流程待分析 */
	err = vfs_mknod(path.dentry->d_inode,
			dentry, mode, dev->devt);
	if (!err) {
		struct iattr newattrs;

		/* fixup possibly umasked mode */
		newattrs.ia_mode = mode;
		newattrs.ia_valid = ATTR_MODE;
		mutex_lock(&dentry->d_inode->i_mutex);
		notify_change(dentry, &newattrs);
		mutex_unlock(&dentry->d_inode->i_mutex);

		/* mark as kernel-created inode */
		dentry->d_inode->i_private = &thread;
	}
	dput(dentry);

	mutex_unlock(&path.dentry->d_inode->i_mutex);
	path_put(&path);
	return err;
}

/* 删除目录 *
 * // TODO: 内部详细流程待分析 */
static int dev_rmdir(const char *name)
{
	struct nameidata nd;
	struct dentry *dentry;
	int err;

	err = kern_path_parent(name, &nd);
	if (err)
		return err;

	mutex_lock_nested(&nd.path.dentry->d_inode->i_mutex, I_MUTEX_PARENT);
	dentry = lookup_one_len(nd.last.name, nd.path.dentry, nd.last.len);
	if (!IS_ERR(dentry)) {
		if (dentry->d_inode) {
			if (dentry->d_inode->i_private == &thread)
				err = vfs_rmdir(nd.path.dentry->d_inode,
						dentry);
			else
				err = -EPERM;
		} else {
			err = -ENOENT;
		}
		dput(dentry);
	} else {
		err = PTR_ERR(dentry);
	}

	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);
	path_put(&nd.path);
	return err;
}

/* 移除设备节点所在的目录 */
static int delete_path(const char *nodepath)
{
	const char *path;
	int err = 0;

	path = kstrdup(nodepath, GFP_KERNEL);
	if (!path)
		return -ENOMEM;

    /* 目录的移除是逐级的
     * 如存在某个目录 a/b/c/d/e
     * 需要先移除最深的目录 e，再移除 d
     * 逐个遍历最右侧的 / ，依次移除目录 */
	for (;;) {
		char *base;

		base = strrchr(path, '/');
		if (!base)
			break;
		base[0] = '\0';
		err = dev_rmdir(path);
		if (err)
			break;
	}

	kfree(path);
	return err;
}

/* 确认 节点是否为 devtmpfs 创建的
 * // TODO: 详细流程待分析 */
static int dev_mynode(struct device *dev, struct inode *inode, struct kstat *stat)
{
	/* did we create it */
	if (inode->i_private != &thread)
		return 0;

	/* does the dev_t match */
	if (is_blockdev(dev)) {
		if (!S_ISBLK(stat->mode))
			return 0;
	} else {
		if (!S_ISCHR(stat->mode))
			return 0;
	}
	if (stat->rdev != dev->devt)
		return 0;

	/* ours */
	return 1;
}

/* 设备节点移除 *
 * // TODO: 内部设计文件系统的处理 待分析 */
static int handle_remove(const char *nodename, struct device *dev)
{
	struct nameidata nd;
	struct dentry *dentry;
	struct kstat stat;
	int deleted = 1;
	int err;

	err = kern_path_parent(nodename, &nd);
	if (err)
		return err;

	mutex_lock_nested(&nd.path.dentry->d_inode->i_mutex, I_MUTEX_PARENT);
	dentry = lookup_one_len(nd.last.name, nd.path.dentry, nd.last.len);
	if (!IS_ERR(dentry)) {
		if (dentry->d_inode) {
			err = vfs_getattr(nd.path.mnt, dentry, &stat);
			if (!err && dev_mynode(dev, dentry->d_inode, &stat)) {
				struct iattr newattrs;
				/*
				 * before unlinking this node, reset permissions
				 * of possible references like hardlinks
				 */
				newattrs.ia_uid = 0;
				newattrs.ia_gid = 0;
				newattrs.ia_mode = stat.mode & ~0777;
				newattrs.ia_valid =
					ATTR_UID|ATTR_GID|ATTR_MODE;
				mutex_lock(&dentry->d_inode->i_mutex);
				notify_change(dentry, &newattrs);
				mutex_unlock(&dentry->d_inode->i_mutex);
				err = vfs_unlink(nd.path.dentry->d_inode,
						 dentry);
				if (!err || err == -ENOENT)
					deleted = 1;
			}
		} else {
			err = -ENOENT;
		}
		dput(dentry);
	} else {
		err = PTR_ERR(dentry);
	}
	mutex_unlock(&nd.path.dentry->d_inode->i_mutex);

	path_put(&nd.path);
	if (deleted && strchr(nodename, '/'))
		delete_path(nodename);
	return err;
}

/*
 * If configured, or requested by the commandline, devtmpfs will be
 * auto-mounted after the kernel mounted the root filesystem.
 */
/* 如果通过 cmdline 参数或配置编译项了， devtmpfs 将在内核启动时自动挂载
 * 默认情况下都是让其自动挂载
 * 这里需要注意，只要 devtmpfs 启动了，即使没有自动挂载，那设备节点的创建/移除操
 * 作也会正常处理的，手动挂载时依然会展示出来 */
int devtmpfs_mount(const char *mntdir)
{
	int err;

    /* 控制是否自动挂载
     * 本函数 devtmpfs_mount 会被内核调用（无论是否配置了自动挂载）
     * 通过 mount_dev 参数控制自动挂载 */
	if (!mount_dev)
		return 0;

    /* devtmpfs 未初始化成功，则不挂载 */
	if (!thread)
		return 0;

    /* 将 devtmpfs 挂载到指定目录中 mntdir
     * 内核中默认为 /dev 目录 */
    /* do_mounts.c 中的处理 
     *  devtmpfs_mount("dev"); */
	err = sys_mount("devtmpfs", (char *)mntdir, "devtmpfs", MS_SILENT, NULL);
	if (err)
		printk(KERN_INFO "devtmpfs: error mounting %i\n", err);
	else
		printk(KERN_INFO "devtmpfs: mounted\n");
	return err;
}

/* devtmpfs_init 与 devtmpfsd 同步用 */
static DECLARE_COMPLETION(setup_done);

/* 设备节点请求处理
 * name 设备节点名
 * mode 设备节点读写模式
 * dev  指向需要创建设备节点的设备结构 */
static int handle(const char *name, umode_t mode, struct device *dev)
{
    /* mode = 0 表示为移除设备节点， != 0 表示创建 */
	if (mode)
		return handle_create(name, mode, dev);
	else
		return handle_remove(name, dev);
}

/* devtmpfs 处理线程
 * 接收 struct req 请求，处理节点创建/移除请求
 * 注意参数 p，这是初始化函数 devtmpfs_init 中的 err
 * 用于错误返回 */
static int devtmpfsd(void *p)
{
	char options[] = "mode=0755";
	int *err = p;
    /* sys_unshare 系统调用
     * // TODO: 待分析 */
	*err = sys_unshare(CLONE_NEWNS);
	if (*err)
		goto out;
    /* 挂载 devtmpfs 文件系统
     * 这里为什么要先将其挂载到 / 根目录下，不太理解
     * 其实在挂载根文件系统的处理中，会将 devtmpfs 再次挂载到 /dev 目录下
     * 猜测一下，这里先挂载了，是为了后面在驱动初始化的时候能够创建设备文件吧
     * 毕竟这个时候还未挂载根文件系统，也就根本没有 /dev 目录。。。。
     * // TODO: 待分析 */
	*err = sys_mount("devtmpfs", "/", "devtmpfs", MS_SILENT, options);
	if (*err)
		goto out;
    /* 切换到根目录， /.. 表示根目录的父目录，也就是根目录自己 */
	sys_chdir("/.."); /* will traverse into overmounted root */
    /* 更改当前进程的根目录，也就是将根目录 / 做为当前线程的根目录  */
	sys_chroot(".");
    /* 线程内部初始化操作完成，发送完成量
     * 在 devtmpfs_init 中等待线程状态呢。。。 */
	complete(&setup_done);
    /* 这个线程启动了就不再退出了
     * 毕竟是 devtmpfs 的后台，要一直处理设备节点请求 */
	while (1) {
		spin_lock(&req_lock);
        /* 当有新的请求时， requests 会指向 struct req
         * 只要 requests != NULL 就表示有新的请求了 */
		while (requests) {
            /* 取出 requests 链表，并将 requests 置为 NULL
             * 这是为了下次判断用
             * 所有的 struct req 在一个链表，把链表头 = NULL
             * 再次 != NULL 时，就表示有新的成员了 */
			struct req *req = requests;
			requests = NULL;
			spin_unlock(&req_lock);
            /* 逐个处理 req 请求，这个过程与 requests 没有关系了 */
			while (req) {
                /* 依次处理 req ， handle 中完成所有处理 */
				struct req *next = req->next;
				req->err = handle(req->name, req->mode, req->dev);
                /* 完成量通知已处理完成，通过这里分析，所有的设备节点操作请求都
                 * 是阻塞的，需要等待确定处理完成后才可以返回
                 * 同时，也将错误状态 req->err 返回了 */
				complete(&req->done);
				req = next;
			}
			spin_lock(&req_lock);
		}
        /* 无任何 请求了，就设置状态休眠，等待被唤醒 */
		__set_current_state(TASK_INTERRUPTIBLE);
		spin_unlock(&req_lock);
		schedule();
	}
	return 0;
out:
	complete(&setup_done);
	return *err;
}

/*
 * Create devtmpfs instance, driver-core devices will add their device
 * nodes here.
 */
/* devtmpfs 是Linux内核中的一种虚拟文件系统，用于动态管理设备文件。
 * 主要功能是自动创建和管理 /dev 目录中的设备节点，使得用户空间应用程序能够方便
 * 地访问硬件设备 
 * a、动态创建设备文件：devtmpfs 会在系统启动时自动创建设备文件，并在硬件设备被
 * 添加或移除时动态更新这些文件
 * b、内存文件系统：devtmpfs 是一种内存文件系统，其内容存储在内存中，而不是在磁
 * 盘上
 * c、非持久性：devtmpfs 的内容在系统重启后不会持久存在。每次启动时，内核会重新
 * 填充该文件系统
 * d、内核支持：devtmpfs 从 Linux内核 2.6.32 开始引入，因此使用该功能的系统需要
 * 确保内核版本满足要求
 * */
int __init devtmpfs_init(void)
{
    /* 向内核注册 devtmpfs 文件系统
     * 成功注册 devtmpfs 文件系统后，才可以挂载到 /dev 目录 */
	int err = register_filesystem(&dev_fs_type);
	if (err) {
		printk(KERN_ERR "devtmpfs: unable to register devtmpfs "
		       "type %i\n", err);
		return err;
	}

    /* 创建 devtmpfsd 内核线程，创建节点都是在这个线程中处理的
     * thread 为线程句柄，可用于判断 devtmpfs 是否已正常工作
     * &err 是传入线程的参数，是用来返回线程中的操作是否出错的
     * 如果线程中的处理发生错误，则这里也需要返回失败 */
	thread = kthread_run(devtmpfsd, &err, "kdevtmpfs");
	if (!IS_ERR(thread)) {
        /* 等待线程启动完成，启动完成后，错误码通过 err 返回 */
		wait_for_completion(&setup_done);
	} else {
        /* 创建线程出错，则直接返回错误 */
		err = PTR_ERR(thread);
		thread = NULL;
	}

    /* 初始化过程出错，则返回错误，注销 devtmpfs 文件系统 */
	if (err) {
		printk(KERN_ERR "devtmpfs: unable to create devtmpfs %i\n", err);
		unregister_filesystem(&dev_fs_type);
		return err;
	}

    /* 初始化成功 */
	printk(KERN_INFO "devtmpfs: initialized\n");
	return 0;
}

/* devtmpfs 与 udev
 * 现在 Linux /dev 的实现就是 devtmpfs + udev
 * devtmpfs 运行在内核空间，直接与内核交互。它在系统启动时由内核创建，并在内存中
 * 维护设备节点。
 * udev 运行在用户空间，依赖于内核提供的事件通知机制（如netlink套接字）来响应设
 * 备的添加和移除事件。
 *
 * devtmpfs: 可以独立工作，提供基本的设备节点。
 * udev: 通常与 devtmpfs 一起使用，以提供更高级的设备管理功能。
 * udev 可以在 devtmpfs 提供的设备节点基础上进行进一步的管理和配置。 
 * */
