/*
 * kobject.c - library routines for handling generic kernel objects
 *
 * Copyright (c) 2002-2003 Patrick Mochel <mochel@osdl.org>
 * Copyright (c) 2006-2007 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (c) 2006-2007 Novell Inc.
 *
 * This file is released under the GPLv2.
 *
 *
 * Please see the file Documentation/kobject.txt for critical information
 * about using the kobject interface.
 */

#include <linux/kobject.h>
#include <linux/string.h>
#include <linux/export.h>
#include <linux/stat.h>
#include <linux/slab.h>

/*
 * populate_dir - populate directory with attributes.
 * @kobj: object we're working on.
 *
 * Most subsystems have a set of default attributes that are associated
 * with an object that registers with them.  This is a helper called during
 * object registration that loops through the default attributes of the
 * subsystem and creates attributes files for them in sysfs.
 */
/* 使用属性填充目录 */
/* 创建的是 kobject -> kobj_type -> attribute 属性文件
 * 每一个属性做为一个文件
 * 注意，不是所有的 kobj_type 都会有 attribute 属性的
 * 比如 devices_kset 中的 kobject, 默认为 kset_ktype
 * 而 kset_ktype 是没有 attribute 属性的，即 devices 目录下没有文件 */
static int populate_dir(struct kobject *kobj)
{
	struct kobj_type *t = get_ktype(kobj);
	struct attribute *attr;
	int error = 0;
	int i;

    /* 若存在 kobj_type, 并具有 default_attrs 结构，
     * 则逐个遍历添加到 sysfs 中
     * 这里可以看出 default_attrs 最后一个成员一定需要是NULL*/
	if (t && t->default_attrs) {
		for (i = 0; (attr = t->default_attrs[i]) != NULL; i++) {
			error = sysfs_create_file(kobj, attr);
			if (error)
				break;
		}
	}
	return error;
}

/* 创建目录， kobject name 即是目录名 */
static int create_dir(struct kobject *kobj)
{
	int error = 0;
    /* 先创建目录
     * 创建目录的位置，与 kobj->parent 有关
     * 如果有 parent ，则会在 parent 目录中创建，做为子目录
     * 如果没有 parent，则在 /sys/ 目录中创建 */
	error = sysfs_create_dir(kobj);
	if (!error) {
        /* 再创建目录内文件 */
        /* 目录下的文件，就是 kobject.default_attrs 属性 */
		error = populate_dir(kobj);
		if (error)
			sysfs_remove_dir(kobj);
	}
	return error;
}

/* 获取 kobject 路径长度，如 /sys/kernel/config/ 
 * 其实就是遍历 kobject 的所有父 kobject ，并计算所有的 name 长度
 * 还需要考虑 '/' ，所以每级目录都 加 1  */
static int get_kobj_path_length(struct kobject *kobj)
{
	int length = 1;
	struct kobject *parent = kobj;

	/* walk up the ancestors until we hit the one pointing to the
	 * root.
	 * Add 1 to strlen for leading '/' of each level.
	 */
	do {
		if (kobject_name(parent) == NULL)
			return 0;
		length += strlen(kobject_name(parent)) + 1;
		parent = parent->parent;
	} while (parent);
	return length;
}

/* 填充 kobject 的路径，遍历所有的 父目录，填充 name 和 '/' */
static void fill_kobj_path(struct kobject *kobj, char *path, int length)
{
	struct kobject *parent;

	--length;
    /* 调用本函数时，已经确认了总的路径长度，参数 length 就是总长度
     * 遍历时，是从最深层开始遍历的，所以是从最深层路径开始填充
     * 每次填充后补 / ，以此向前遍历 父 就可以 */
	for (parent = kobj; parent; parent = parent->parent) {
		int cur = strlen(kobject_name(parent));
		/* back up enough to print this name with '/' */
		length -= cur;
		strncpy(path + length, kobject_name(parent), cur);
		*(path + --length) = '/';
	}

	pr_debug("kobject: '%s' (%p): %s: path = '%s'\n", kobject_name(kobj),
		 kobj, __func__, path);
}

/**
 * kobject_get_path - generate and return the path associated with a given kobj and kset pair.
 *
 * @kobj:	kobject in question, with which to build the path
 * @gfp_mask:	the allocation type used to allocate the path
 *
 * The result must be freed by the caller with kfree().
 */
/* 生成指定的 kobject 的路径并返回
 * 先获取了路径长度，然后分配内存，再填充路径 */
/* 填充路径的内存是动态分配的，使用完后需调用者释放 */
char *kobject_get_path(struct kobject *kobj, gfp_t gfp_mask)
{
	char *path;
	int len;

	len = get_kobj_path_length(kobj);
	if (len == 0)
		return NULL;
	path = kzalloc(len, gfp_mask);
	if (!path)
		return NULL;
	fill_kobj_path(kobj, path, len);

	return path;
}
EXPORT_SYMBOL_GPL(kobject_get_path);

/* add the kobject to its kset's list */
/* 添加 kobject 到 它所属的 kset 中 */
static void kobj_kset_join(struct kobject *kobj)
{
	if (!kobj->kset)
		return;

    /* 递增 kset 引用计数，其实是递增的 kset 中内嵌的 kobject 的引用计数 */
	kset_get(kobj->kset);
	spin_lock(&kobj->kset->list_lock);
    /* 将 kobject 添加到 kset 双向链表的尾部 */
	list_add_tail(&kobj->entry, &kobj->kset->list);
	spin_unlock(&kobj->kset->list_lock);
}

/* remove the kobject from its kset's list */
/* 移除 kobject 从所属的 kset 中 */
/* kobj 是挂到 kset 链表中的，从 kset 中移除 */
static void kobj_kset_leave(struct kobject *kobj)
{
    /* 这个 kobj 必须属于某个 kset */
	if (!kobj->kset)
		return;

	spin_lock(&kobj->kset->list_lock);
	list_del_init(&kobj->entry);        /* 从 kset 链表中移除 */
	spin_unlock(&kobj->kset->list_lock);
	kset_put(kobj->kset);               /* 递减 kset 引用计数 */
}

/* kobject 内部初始化，最基本初始化 */
/* 这是仅仅是初始化了引用计数、链表、置位了初始化标志
 * 并不涉及 所属kset、父kobject、ktype 等信息 */
static void kobject_init_internal(struct kobject *kobj)
{
	if (!kobj)
		return;
    /* 这里将 kref 置为 1了，表示已经开始使用了 */
	kref_init(&kobj->kref);
    /* entry 是用于链接到 kset 的 */
	INIT_LIST_HEAD(&kobj->entry);
	kobj->state_in_sysfs = 0;
	kobj->state_add_uevent_sent = 0;
	kobj->state_remove_uevent_sent = 0;
	kobj->state_initialized = 1;
}


/* 将 kobject 加到 目录层次中 */
static int kobject_add_internal(struct kobject *kobj)
{
	int error = 0;
	struct kobject *parent;

	if (!kobj)
		return -ENOENT;

    /* 添加到系统的 kobject 必须具有名字
     * 没名字怎么在 sysfs 中显示！！！ */
	if (!kobj->name || !kobj->name[0]) {
		WARN(1, "kobject: (%p): attempted to be registered with empty "
			 "name!\n", kobj);
		return -EINVAL;
	}

    /* 注册时，会递增父 kobject 的计数值
     * 每添加一个新的 kobject ,就会递增父的引用计数 
     * 当然，这里的父也可能为 NULL，即没有父 */
    /* 在 sysfs 中，/sys/ 根目录下的 目录项是没有 parent 的
     * 比如在创建 devices 时，传入的 parent 就是 NULL
     * 一个没有 parent 的 kobject ，会直接在 /sys/ 目录下 */
	parent = kobject_get(kobj->parent);

    /* 如果本 kobject 属于一个 kset ,则需要将 kobject 加入到 kset 链表中 */
	/* join kset if set, use it as parent if we do not already have one */
    /* 注意，kobject 可以不属于 kset
     * 典型的， devices_kset 是一个顶层的 kset , 
     * 其中内嵌的 kobject 就不属于任何 kset */
    /* 这里主要是处理两个事情：
     * 1、将 kobj 挂到 所属的 kset 的链表中；
     * 2、如果 kobj 未指定 parent, 则以 kset->kobj 为 parent */
	if (kobj->kset) {
        /* 这里需要留意，如果 kobject 属于一个 kset，
         * 并且在注册时没有指定 父 parent , 
         * 那将会把所属的kset中的 kobject 做为父
         * 如果指定了父，那还是以指定的父 */
		if (!parent)
			parent = kobject_get(&kobj->kset->kobj);
        /* 将 kobject 添加到 kset 链表中 */
		kobj_kset_join(kobj);
        /* 父重新指向一下，可能会指向 kset 中 kobject，也可能不变 */
        /* 如果原来没有指定 parent，并且属于某个 kset，这里会指向kset的kobj
         * 如果 parent 和 kset 都没有指定，那不会走到这里， parent 还是 NULL
         * 如果指定了 parent，那无论是否走到这里， parent 都不会变 */
		kobj->parent = parent;
	}

	pr_debug("kobject: '%s' (%p): %s: parent: '%s', set: '%s'\n",
		 kobject_name(kobj), kobj, __func__,
		 parent ? kobject_name(parent) : "<NULL>",
		 kobj->kset ? kobject_name(&kobj->kset->kobj) : "<NULL>");

    /* 根据 kobject 创建目录，
     * create_dir 操作完成后，为 kobject 创建了 name 目录
     * 并把 kobject -> ktype 中的 attribute 属性逐个创建了文件
     * // TODO: 待分析 */
	error = create_dir(kobj);
	if (error) {
        /* 创建失败了，需要把所有的清理一下 */
        /* 从所属的 kset 链表中移除 kobj 自身 */
        /* kobj_kset_leave 是处理的将要被释放的 kobj
         * 是要把自身从 kset 中移除 */
		kobj_kset_leave(kobj);
        /* kobject_put(parent) 这里是处理父
         * 释放了自身，也要把父parent 递减一下，因为前面把父递增了 */
		kobject_put(parent);
		kobj->parent = NULL;
        /* 这里有个疑问，创建失败了，做清理的时候，
         * 将 kobj 从 所属 kset 中移除了，也释放了 父kobj 计数
         * 但并没有 kobject_put(kobj), 也就是并没有 put 自身
         * // TODO: 确认一下为什么
         * 猜测一下，可能是因为并未加到 sysfs 中，也就是整个创建过程还没完成呢
         * 前面的之后做了一些初始化操作，原没有到清理这一步 */

		/* be noisy on error issues */
		if (error == -EEXIST)
			WARN(1, "%s failed for %s with "
			     "-EEXIST, don't try to register things with "
			     "the same name in the same directory.\n",
			     __func__, kobject_name(kobj));
		else
			WARN(1, "%s failed for %s (error: %d parent: %s)\n",
			     __func__, kobject_name(kobj), error,
			     parent ? kobject_name(parent) : "'none'");
	} else
        /* 全部成功则标记已在 sysfs 系统中 */
		kobj->state_in_sysfs = 1;

	return error;
}

/**
 * kobject_set_name_vargs - Set the name of an kobject
 * @kobj: struct kobject to set the name of
 * @fmt: format string used to build the name
 * @vargs: vargs to format the string.
 */
/* 设置 kobject 名字
 * 注意，这里会分配 name 内存 */
int kobject_set_name_vargs(struct kobject *kobj, const char *fmt,
				  va_list vargs)
{
	const char *old_name = kobj->name;
	char *s;

	if (kobj->name && !fmt)
		return 0;

	kobj->name = kvasprintf(GFP_KERNEL, fmt, vargs);
	if (!kobj->name)
		return -ENOMEM;

	/* ewww... some of these buggers have '/' in the name ... */
	while ((s = strchr(kobj->name, '/')))
		s[0] = '!';

	kfree(old_name);
	return 0;
}

/**
 * kobject_set_name - Set the name of a kobject
 * @kobj: struct kobject to set the name of
 * @fmt: format string used to build the name
 *
 * This sets the name of the kobject.  If you have already added the
 * kobject to the system, you must call kobject_rename() in order to
 * change the name of the kobject.
 */
/* 注意，如果已经将 kobject 添加到系统中了，那必须使用 kobject_rename()
 * 才能更改名字 */
/* 设置 kobject 名字，可变参数列表 */
int kobject_set_name(struct kobject *kobj, const char *fmt, ...)
{
	va_list vargs;
	int retval;

	va_start(vargs, fmt);
	retval = kobject_set_name_vargs(kobj, fmt, vargs);
	va_end(vargs);

	return retval;
}
EXPORT_SYMBOL(kobject_set_name);

/**
 * kobject_init - initialize a kobject structure
 * @kobj: pointer to the kobject to initialize
 * @ktype: pointer to the ktype for this kobject.
 *
 * This function will properly initialize a kobject such that it can then
 * be passed to the kobject_add() call.
 *
 * After this function is called, the kobject MUST be cleaned up by a call
 * to kobject_put(), not by a call to kfree directly to ensure that all of
 * the memory is cleaned up properly.
 */
/* 初始化 kobject ,这里会将 kobject 的 kref 置为 1 */
/* 很重要，要给 kobject 一个 ktype ，这确定了 kobject 类型、属性文件 */
void kobject_init(struct kobject *kobj, struct kobj_type *ktype)
{
	char *err_str;

	if (!kobj) {
		err_str = "invalid kobject pointer!";
		goto error;
	}
	if (!ktype) {
		err_str = "must have a ktype to be initialized properly!\n";
		goto error;
	}
	if (kobj->state_initialized) {
		/* do not error out as sometimes we can recover */
		printk(KERN_ERR "kobject (%p): tried to init an initialized "
		       "object, something is seriously wrong.\n", kobj);
		dump_stack();
	}

	kobject_init_internal(kobj);
	kobj->ktype = ktype;        /* 将属性结构赋给 ktype */
	return;

error:
	printk(KERN_ERR "kobject (%p): %s\n", kobj, err_str);
	dump_stack();
}
EXPORT_SYMBOL(kobject_init);

/* 将 kobject 添加到系统中 
 * 这里需要 kobject 的 父parent 了，当然，parent 可以为 NULL */
static int kobject_add_varg(struct kobject *kobj, struct kobject *parent,
			    const char *fmt, va_list vargs)
{
	int retval;

    /* 设置一下 kobject 的名字 */
	retval = kobject_set_name_vargs(kobj, fmt, vargs);
	if (retval) {
		printk(KERN_ERR "kobject: can not set name properly!\n");
		return retval;
	}

    /* 指向函数参数中指定的 父 kobject  */
	kobj->parent = parent;
	return kobject_add_internal(kobj);  /* 添加到目录层次并创建目录 */
}

/**
 * kobject_add - the main kobject add function
 * @kobj: the kobject to add
 * @parent: pointer to the parent of the kobject.
 * @fmt: format to name the kobject with.
 *
 * The kobject name is set and added to the kobject hierarchy in this
 * function.
 *
 * If @parent is set, then the parent of the @kobj will be set to it.
 * If @parent is NULL, then the parent of the @kobj will be set to the
 * kobject associted with the kset assigned to this kobject.  If no kset
 * is assigned to the kobject, then the kobject will be located in the
 * root of the sysfs tree.
 *
 * If this function returns an error, kobject_put() must be called to
 * properly clean up the memory associated with the object.
 * Under no instance should the kobject that is passed to this function
 * be directly freed with a call to kfree(), that can leak memory.
 *
 * Note, no "add" uevent will be created with this call, the caller should set
 * up all of the necessary sysfs files for the object and then call
 * kobject_uevent() with the UEVENT_ADD parameter to ensure that
 * userspace is properly notified of this kobject's creation.
 */
/* 在此函数中，设置并添加 kobject名称到 kobject 层次结构中。
 *
 * 如果提供了 parent 参数，将会设置为 kobj 的 parent 。
 * 如果 parent = NULL, 将会把所属的 kset 设置为 kobj 的 parent. 但如果
 * 也没有 kset，那 kobj 将会放在 sysfs 的根目录下，即 /sys 下 */
/* 添加 kobject 到目录层次, 没有父 parent 则会在 sys 顶层目录下
 * add 操作不会递增本 kobject 的引用计数，只会递增父 的引用计数 */
int kobject_add(struct kobject *kobj, struct kobject *parent,
		const char *fmt, ...)
{
	va_list args;
	int retval;

	if (!kobj)
		return -EINVAL;

	if (!kobj->state_initialized) {
		printk(KERN_ERR "kobject '%s' (%p): tried to add an "
		       "uninitialized object, something is seriously wrong.\n",
		       kobject_name(kobj), kobj);
		dump_stack();
		return -EINVAL;
	}
    /* 与 kobject_init_and_add 后半段流程一致 */
	va_start(args, fmt);
	retval = kobject_add_varg(kobj, parent, fmt, args);
	va_end(args);

	return retval;
}
EXPORT_SYMBOL(kobject_add);

/**
 * kobject_init_and_add - initialize a kobject structure and add it to the kobject hierarchy
 * @kobj: pointer to the kobject to initialize
 * @ktype: pointer to the ktype for this kobject.
 * @parent: pointer to the parent of this kobject.
 * @fmt: the name of the kobject.
 *
 * This function combines the call to kobject_init() and
 * kobject_add().  The same type of error handling after a call to
 * kobject_add() and kobject lifetime rules are the same here.
 */
/* 初始化 kobject 并将其添加到 kobject 的目录层次（sysfs）中 */
/* 如果没有 parent 父kobject ，则本 kobject 会在 sys 顶层目录中
 * 如果没有属性结构 (ktype->attr) ，则不会创建属性文件
 * 属性文件可以在以后使用 sysfs_create_files 添加 */
int kobject_init_and_add(struct kobject *kobj, struct kobj_type *ktype,
			 struct kobject *parent, const char *fmt, ...)
{
	va_list args;
	int retval;

    /* 初始化一下 kobject */
	kobject_init(kobj, ktype);

    /* 将 kobject 添加到 目录层次中 */
	va_start(args, fmt);
	retval = kobject_add_varg(kobj, parent, fmt, args);
	va_end(args);

	return retval;
}
EXPORT_SYMBOL_GPL(kobject_init_and_add);

/**
 * kobject_rename - change the name of an object
 * @kobj: object in question.
 * @new_name: object's new name
 *
 * It is the responsibility of the caller to provide mutual
 * exclusion between two different calls of kobject_rename
 * on the same kobject and to ensure that new_name is valid and
 * won't conflict with other kobjects.
 */
/* 重命名 kobject 名字，需要借助 uevent */
/* 已经添加到系统中的 kobject ，重命名时必须使用本接口 */
int kobject_rename(struct kobject *kobj, const char *new_name)
{
	int error = 0;
	const char *devpath = NULL;
	const char *dup_name = NULL, *name;
	char *devpath_string = NULL;
	char *envp[2];

	kobj = kobject_get(kobj);
	if (!kobj)
		return -EINVAL;
    /* 无父 不可以重命名 */
	if (!kobj->parent)
		return -EINVAL;

	devpath = kobject_get_path(kobj, GFP_KERNEL);
	if (!devpath) {
		error = -ENOMEM;
		goto out;
	}
	devpath_string = kmalloc(strlen(devpath) + 15, GFP_KERNEL);
	if (!devpath_string) {
		error = -ENOMEM;
		goto out;
	}
	sprintf(devpath_string, "DEVPATH_OLD=%s", devpath);
	envp[0] = devpath_string;
	envp[1] = NULL;

	name = dup_name = kstrdup(new_name, GFP_KERNEL);
	if (!name) {
		error = -ENOMEM;
		goto out;
	}

	error = sysfs_rename_dir(kobj, new_name);
	if (error)
		goto out;

	/* Install the new kobject name */
	dup_name = kobj->name;
	kobj->name = name;

	/* This function is mostly/only used for network interface.
	 * Some hotplug package track interfaces by their name and
	 * therefore want to know when the name is changed by the user. */
	kobject_uevent_env(kobj, KOBJ_MOVE, envp);

out:
	kfree(dup_name);
	kfree(devpath_string);
	kfree(devpath);
	kobject_put(kobj);

	return error;
}
EXPORT_SYMBOL_GPL(kobject_rename);

/**
 * kobject_move - move object to another parent
 * @kobj: object in question.
 * @new_parent: object's new parent (can be NULL)
 */
/* 移动 kobject 到其他目录, 换个父 kobject */
int kobject_move(struct kobject *kobj, struct kobject *new_parent)
{
	int error;
	struct kobject *old_parent;
	const char *devpath = NULL;
	char *devpath_string = NULL;
	char *envp[2];

	kobj = kobject_get(kobj);
	if (!kobj)
		return -EINVAL;
	new_parent = kobject_get(new_parent);
	if (!new_parent) {
		if (kobj->kset)
			new_parent = kobject_get(&kobj->kset->kobj);
	}
	/* old object path */
	devpath = kobject_get_path(kobj, GFP_KERNEL);
	if (!devpath) {
		error = -ENOMEM;
		goto out;
	}
	devpath_string = kmalloc(strlen(devpath) + 15, GFP_KERNEL);
	if (!devpath_string) {
		error = -ENOMEM;
		goto out;
	}
	sprintf(devpath_string, "DEVPATH_OLD=%s", devpath);
	envp[0] = devpath_string;
	envp[1] = NULL;
	error = sysfs_move_dir(kobj, new_parent);
	if (error)
		goto out;
	old_parent = kobj->parent;
	kobj->parent = new_parent;
	new_parent = NULL;
	kobject_put(old_parent);
	kobject_uevent_env(kobj, KOBJ_MOVE, envp);
out:
	kobject_put(new_parent);
	kobject_put(kobj);
	kfree(devpath_string);
	kfree(devpath);
	return error;
}

/**
 * kobject_del - unlink kobject from hierarchy.
 * @kobj: object.
 */
/* 将 kobject 从目录结构层次中移除 */
/* del 的作用是把 kobject 从 sysfs 中移除
 * 并未递减本 kobject 的引用计数
 * 只会递减父 的引用计数 */
void kobject_del(struct kobject *kobj)
{
	if (!kobj)
		return;

	sysfs_remove_dir(kobj);     /* 从目录中移除 */
	kobj->state_in_sysfs = 0;   /* 置为标志 */
	kobj_kset_leave(kobj);      /* 从所属的 kset 中移除 */
	kobject_put(kobj->parent);  /* 父 kobject 引用计数 - 1 */
	kobj->parent = NULL;        /* 父 kobject 置为 NULL */
}

/**
 * kobject_get - increment refcount for object.
 * @kobj: object.
 */
/* 递增引用计数 */
struct kobject *kobject_get(struct kobject *kobj)
{
	if (kobj)
		kref_get(&kobj->kref);
	return kobj;
}

/*
 * kobject_cleanup - free kobject resources.
 * @kobj: object to cleanup
 */
/* kobject 中的 kref = 0 时的释放函数
 * 这里会调用 kobject 中的 ktype 的 release 函数 */
static void kobject_cleanup(struct kobject *kobj)
{
	struct kobj_type *t = get_ktype(kobj);
	const char *name = kobj->name;

	pr_debug("kobject: '%s' (%p): %s\n",
		 kobject_name(kobj), kobj, __func__);

    /* 有属性结构，但没有释放函数，是不标准的，但不会出错 */
	if (t && !t->release)
		pr_debug("kobject: '%s' (%p): does not have a release() "
			 "function, it is broken and must be fixed.\n",
			 kobject_name(kobj), kobj);

    /* 如果 uevent 曾经发送过 add 消息，那这里需要发送 remove 消息 */
	/* send "remove" if the caller did not do it but sent "add" */
	if (kobj->state_add_uevent_sent && !kobj->state_remove_uevent_sent) {
		pr_debug("kobject: '%s' (%p): auto cleanup 'remove' event\n",
			 kobject_name(kobj), kobj);

        /* uevent 的 remove 消息 */
		kobject_uevent(kobj, KOBJ_REMOVE);
	}

    /* 如果已经在 sysfs 中了，需要移除它 */
	/* remove from sysfs if the caller did not do it */
	if (kobj->state_in_sysfs) {
		pr_debug("kobject: '%s' (%p): auto cleanup kobject_del\n",
			 kobject_name(kobj), kobj);
        /* 移除本 kobject */
		kobject_del(kobj);
	}

    /* 如果属性结构中有 release 回调，调用一下
     * 这里可以在回调函数中将更大的结构体释放 */
	if (t && t->release) {
		pr_debug("kobject: '%s' (%p): calling ktype release\n",
			 kobject_name(kobj), kobj);
		t->release(kobj);
	}

    /* 释放掉 name ，name 存储空间是在 add 时动态分配的 */
	/* free name if we allocated it */
	if (name) {
		pr_debug("kobject: '%s': free name\n", name);
		kfree(name);
	}
}

/* kobject 中 kref = 0 时回调函数，用于释放内存 */
static void kobject_release(struct kref *kref)
{
    /* 释放函数需要 kobject 做为参数 */
    /* 调用本函数时，传入的参数是 struct kref
     * 使用 struct kref 找到所属的 kobject
     * 使用 struct kobject 做为参数调用 kobject_cleanup */
	kobject_cleanup(container_of(kref, struct kobject, kref));
}

/**
 * kobject_put - decrement refcount for object.
 * @kobj: object.
 *
 * Decrement the refcount, and if 0, call kobject_cleanup().
 */
/* 释放 kobject ，本质是将 kref 引用计数 -1，
 * 如果 kref = 0，则调用 kobject_release 函数 */
void kobject_put(struct kobject *kobj)
{
	if (kobj) {
		if (!kobj->state_initialized)
			WARN(1, KERN_WARNING "kobject: '%s' (%p): is not "
			       "initialized, yet kobject_put() is being "
			       "called.\n", kobject_name(kobj), kobj);
        /* 引用计数 = 0 时，调用 kobject_release 函数
         * 这里调用 kobject_release 是因为 kref 相关函数，
         * 参数都是 kref,对于 kref 来讲，它不需要知道 kobject */
		kref_put(&kobj->kref, kobject_release);
	}
}

/* 默认的 kobject  kobj_type 的 release 回调函数*/
static void dynamic_kobj_release(struct kobject *kobj)
{
	pr_debug("kobject: (%p): %s\n", kobj, __func__);
	kfree(kobj);
}

/* kobject 默认的 kobj_type 的属性 
 * 用于简化 attribute 的操作 
 * 通过定义 kobj_attribute 属性结构，简化 attribute 操作 
 * 创建的 kobject 在操作属性文件时，都会调用到这里的 kobj_sysfs_ops 回调 
 * 然后在 kobj_sysfs_ops 回调中，找到 kobj_attribute 属性，
 * 再进一步调用 kobj_attribute 属性中的 show / store 方法 */
/* dynamic_kobj_ktype 仅限于 kobject_create 创建的 Kobject 使用
 * 使用 kobject_create 创建的 kobject, 确认没有被其它更大的结构分配，
 * 能够直接使用 dynamic_kobj_release 释放 */
static struct kobj_type dynamic_kobj_ktype = {
	.release	= dynamic_kobj_release,
	.sysfs_ops	= &kobj_sysfs_ops,
};

/**
 * kobject_create - create a struct kobject dynamically
 *
 * This function creates a kobject structure dynamically and sets it up
 * to be a "dynamic" kobject with a default release function set up.
 *
 * If the kobject was not able to be created, NULL will be returned.
 * The kobject structure returned from here must be cleaned up with a
 * call to kobject_put() and not kfree(), as kobject_init() has
 * already been called on this structure.
 */
/* 创建一个 kobject 并使用默认的 kobj_type 属性结构 
 * 这里会将创建的 kobject 的属性结构 kobj_type 赋一个默认的 kobj_type 
 * 在单独使用 kobject 时，可以简化属性文件的定义及操作，搭配 kobj_attribute
 * 使用本函数创建之后，可以使用 sysfs_create_files  sysfs_create_group 等函数
 * 创建属性文件 */
struct kobject *kobject_create(void)
{
	struct kobject *kobj;

	kobj = kzalloc(sizeof(*kobj), GFP_KERNEL);
	if (!kobj)
		return NULL;

	kobject_init(kobj, &dynamic_kobj_ktype);
	return kobj;
}

/**
 * kobject_create_and_add - create a struct kobject dynamically and register it with sysfs
 *
 * @name: the name for the kobject
 * @parent: the parent kobject of this kobject, if any.
 *
 * This function creates a kobject structure dynamically and registers it
 * with sysfs.  When you are finished with this structure, call
 * kobject_put() and the structure will be dynamically freed when
 * it is no longer being used.
 *
 * If the kobject was not able to be created, NULL will be returned.
 */
/* 创建一个 kobject 并将其添加到 sysfs 目录层次结构中 */
/* 前半部分的作用与 kobject_create 一致，后半部分与 kobject_add 一致 */
struct kobject *kobject_create_and_add(const char *name, struct kobject *parent)
{
	struct kobject *kobj;
	int retval;

	kobj = kobject_create();
	if (!kobj)
		return NULL;

	retval = kobject_add(kobj, parent, "%s", name);
	if (retval) {
		printk(KERN_WARNING "%s: kobject_add error: %d\n",
		       __func__, retval);
		kobject_put(kobj);
		kobj = NULL;
	}
	return kobj;
}
EXPORT_SYMBOL_GPL(kobject_create_and_add);

/**
 * kset_init - initialize a kset for use
 * @k: kset
 */
/* 初始化 kset
 * 对内部 kobject 和 链表啥的初始化 */
void kset_init(struct kset *k)
{
    /* kobject 内部初始化使用
     * 一些内部数据结构设置
     * 这里并不会设置 parent 等外部可控的数据 */
	kobject_init_internal(&k->kobj);
    /* kset list 用于链接所有属于它的 kobject */
	INIT_LIST_HEAD(&k->list);
	spin_lock_init(&k->list_lock);
}

/* 默认的 kobj_type 的操作
 * 结合 kobj_attribute 结构使用，用于简化属性/(show/shore)的处理 */
/* default kobject attribute operations */
static ssize_t kobj_attr_show(struct kobject *kobj, struct attribute *attr,
			      char *buf)
{
	struct kobj_attribute *kattr;
	ssize_t ret = -EIO;

	kattr = container_of(attr, struct kobj_attribute, attr);
	if (kattr->show)
		ret = kattr->show(kobj, kattr, buf);
	return ret;
}

static ssize_t kobj_attr_store(struct kobject *kobj, struct attribute *attr,
			       const char *buf, size_t count)
{
	struct kobj_attribute *kattr;
	ssize_t ret = -EIO;

	kattr = container_of(attr, struct kobj_attribute, attr);
	if (kattr->store)
		ret = kattr->store(kobj, kattr, buf, count);
	return ret;
}

/* 这是默认用于 kobject 及 kset 的sysfs 属性操作函数
 * 这里提供的方法用于简化 struct attribute 操作
 * 使用 struct kobj_attribute 结构，将属性 、 操作方法进行绑定
 * 然后将属性注册到 sysfs 中，
 * 操作属性时将调用到本结构中的操作函数，
 * 从上面的两个函数可以看出，会进一步查找属性所在的 struct kobj_attribute 结构
 * 然后调用对应的属性操作函数
 * 虽然这里的操作变得负责了一点，但在创建属性、属性操作函数时，变得简单多了
 * 可以对每个属性提供不同的操作函数了
 *
 * struct kobj_attribute xxx_attribute = 
 *      __ATTR(xxx, 0666, xxx_show, xxx_store);
 *
 * struct attribute *xxx_attrs[] = {
 *      &xxx_attribute.attr,
 *      NULL,
 * };
 *
 * 使用 sysfs_create_files(kobj, xxx_attrs); 
 * 注册属性到某个 kobj, 就会沿着上面的流程调用了
 *
 * 注意，虽然 kobj_sysfs_ops 在这个文件中定义，但其它的驱动模块也是可以使用的
 * 比如 class_dir_ktype 中引用了本属性操作函数 */
const struct sysfs_ops kobj_sysfs_ops = {
	.show	= kobj_attr_show,
	.store	= kobj_attr_store,
};

/**
 * kset_register - initialize and add a kset.
 * @k: kset.
 */
/* 初始化并添加 kset 到目录结构层次中
 * 其实是把 kset 内部的 kobject 添加到sysfs中 */
/* kset 的注册，其实是将 kset 内嵌的 kobject 注册到系统中 */
int kset_register(struct kset *k)
{
	int err;

	if (!k)
		return -EINVAL;

    /* 初始化 kset 结构 */
	kset_init(k);
    /* 将内嵌的 kobj 加到目录结构中
     * 也就是在 sysfs 中创建目录/属性文件 */
	err = kobject_add_internal(&k->kobj);
	if (err)
		return err;

    /* kset 注册的时候，需要发送 uevent 消息 */
	kobject_uevent(&k->kobj, KOBJ_ADD);
	return 0;
}

/**
 * kset_unregister - remove a kset.
 * @k: kset.
 */
/* 移除一个 kset
 * 本质是移除内部的 kobject */
void kset_unregister(struct kset *k)
{
	if (!k)
		return;
	kobject_put(&k->kobj);
}

/**
 * kset_find_obj - search for object in kset.
 * @kset: kset we're looking in.
 * @name: object's name.
 *
 * Lock kset via @kset->subsys, and iterate over @kset->list,
 * looking for a matching kobject. If matching object is found
 * take a reference and return the object.
 */
/* 根据名字在 kset 中查找 kobject */
struct kobject *kset_find_obj(struct kset *kset, const char *name)
{
	struct kobject *k;
	struct kobject *ret = NULL;

	spin_lock(&kset->list_lock);

	list_for_each_entry(k, &kset->list, entry) {
		if (kobject_name(k) && !strcmp(kobject_name(k), name)) {
			ret = kobject_get(k);
			break;
		}
	}

	spin_unlock(&kset->list_lock);
	return ret;
}

/* kset 释放函数 */
static void kset_release(struct kobject *kobj)
{
	struct kset *kset = container_of(kobj, struct kset, kobj);
	pr_debug("kobject: '%s' (%p): %s\n",
		 kobject_name(kobj), kobj, __func__);
	kfree(kset);
}

/* kset 的 kobject 属性结构 */
static struct kobj_type kset_ktype = {
	.sysfs_ops	= &kobj_sysfs_ops,
	.release = kset_release,
};

/**
 * kset_create - create a struct kset dynamically
 *
 * @name: the name for the kset
 * @uevent_ops: a struct kset_uevent_ops for the kset
 * @parent_kobj: the parent kobject of this kset, if any.
 *
 * This function creates a kset structure dynamically.  This structure can
 * then be registered with the system and show up in sysfs with a call to
 * kset_register().  When you are finished with this structure, if
 * kset_register() has been called, call kset_unregister() and the
 * structure will be dynamically freed when it is no longer being used.
 *
 * If the kset was not able to be created, NULL will be returned.
 */
/* 创建 kset 
 * 动态分配的，并初始化了部分成员
 * 这里将 kset 中的 kobject 的属性文件赋了默认的 kset_ktype 
 * 用于简化属性文件操作，搭配 kobj_attribute */
static struct kset *kset_create(const char *name,
				const struct kset_uevent_ops *uevent_ops,
				struct kobject *parent_kobj)
{
	struct kset *kset;
	int retval;

    /* kset 本质是个 kobject ，名字需要设置在 kobject 中 */
    /* 内存动态分配 kset 结构，因为下面的 kset->kobj.ktype = &kset_ktype;
     * 把 kset 内嵌的 kobject 的 ktype 设置为了 kset_ktype
     * 所以会在释放时调用 kset_ktype 的 release 函数（见上方数据结构）
     * 在 release 中就明确知道这个 kobject 是属于 kset , 就会释放 kset 内存了 */
	kset = kzalloc(sizeof(*kset), GFP_KERNEL);
	if (!kset)
		return NULL;
    /* 设置 kobject 的名字，必须要有名字 */
	retval = kobject_set_name(&kset->kobj, name);
	if (retval) {
		kfree(kset);
		return NULL;
	}

    /* 成员赋值，设置父 kobject */
    /* kset 的重要成员 uevent_ops ，处理 uevent 事件
     * kset 中的 kobj 设置父kobj, 顶层的 kset / kobj 可以为 NULL
     * parent = NULL 的情况，就是在 /sys/ 根目录的目录项 */
	kset->uevent_ops = uevent_ops;
	kset->kobj.parent = parent_kobj;

	/*
	 * The kobject of this kset will have a type of kset_ktype and belong to
	 * no kset itself.  That way we can properly free it when it is
	 * finished being used.
	 */
    /* 本 kset 的 kobject 将具有 kset_ktype 类型，并且不属于 kset 本身。
     * 新创建的 kset 不属于任何 kset */
    /* kset 内部的 kobject 的属性结构指向特定的属性结构 kset_ktype */
	kset->kobj.ktype = &kset_ktype;
	kset->kobj.kset = NULL;

	return kset;
}

/**
 * kset_create_and_add - create a struct kset dynamically and add it to sysfs
 *
 * @name: the name for the kset
 * @uevent_ops: a struct kset_uevent_ops for the kset
 * @parent_kobj: the parent kobject of this kset, if any.
 *
 * This function creates a kset structure dynamically and registers it
 * with sysfs.  When you are finished with this structure, call
 * kset_unregister() and the structure will be dynamically freed when it
 * is no longer being used.
 *
 * If the kset was not able to be created, NULL will be returned.
 */
/* 动态分配创建一个 kset ，并将它添加到 sysfs 中
 *
 * name: kset 名
 * uevent_ops: kset 的 struct kset_uevent_ops 结构，提供 uevent 操作
 * parent_kobj: kset 的 父 kobject 结构，可以为 NULL
 *
 * 本函数创建一个 kset 结构 并注册到 sysfs 中。当不需要 kset 时，
 * 需要调用 kset_unregister 并且会动态释放掉 kset 结构
 *
 * 如果 kset 结构没有正确创建，将返回 NULL */
/* 创建 kset 并将其添加到 sysfs 中
 * kset 也是可以有父 kobject 的，
 * 另外，kset 还有 uevent_ops */
struct kset *kset_create_and_add(const char *name,
				 const struct kset_uevent_ops *uevent_ops,
				 struct kobject *parent_kobj)
{
	struct kset *kset;
	int error;

    /* 创建一个新的 kset 并简单初始化一下 */
    /* 新建的 kset 已初始化部分属性
     * 很重要的 uevent_ops ， uevent_ops 是调用本函数时提供的
     * 比如 device_uevent_ops ，是要创建一个 devices 的 kset 
     * 那针对 devices ，需要提供一个 uevent_ops 回调
     * 这个 uevent_ops 是特定于 devices 的 */
	kset = kset_create(name, uevent_ops, parent_kobj);
	if (!kset)
		return NULL;
    /* 注册 kset ,这里还包含了初始化操作 */
    /* 上面 kset_create 已经将 kset 创建好了，但创建好并不会自动加到系统中，
     * 这里 kset_register 会把 kset 注册到系统中 */
	error = kset_register(kset);
	if (error) {
        /* 失败的清理工作，释放掉分配的 kset 结构
         * 这里是不是应该还需要释放掉 kset->kobj->name ？
         * name 也是内存分配的啊？？！！ */
		kfree(kset);
		return NULL;
	}
	return kset;
}
EXPORT_SYMBOL_GPL(kset_create_and_add);

// TODO: 待分析  kobject name space 

static DEFINE_SPINLOCK(kobj_ns_type_lock);
static const struct kobj_ns_type_operations *kobj_ns_ops_tbl[KOBJ_NS_TYPES];

int kobj_ns_type_register(const struct kobj_ns_type_operations *ops)
{
	enum kobj_ns_type type = ops->type;
	int error;

	spin_lock(&kobj_ns_type_lock);

	error = -EINVAL;
	if (type >= KOBJ_NS_TYPES)
		goto out;

	error = -EINVAL;
	if (type <= KOBJ_NS_TYPE_NONE)
		goto out;

	error = -EBUSY;
	if (kobj_ns_ops_tbl[type])
		goto out;

	error = 0;
	kobj_ns_ops_tbl[type] = ops;

out:
	spin_unlock(&kobj_ns_type_lock);
	return error;
}

int kobj_ns_type_registered(enum kobj_ns_type type)
{
	int registered = 0;

	spin_lock(&kobj_ns_type_lock);
	if ((type > KOBJ_NS_TYPE_NONE) && (type < KOBJ_NS_TYPES))
		registered = kobj_ns_ops_tbl[type] != NULL;
	spin_unlock(&kobj_ns_type_lock);

	return registered;
}

const struct kobj_ns_type_operations *kobj_child_ns_ops(struct kobject *parent)
{
	const struct kobj_ns_type_operations *ops = NULL;

	if (parent && parent->ktype->child_ns_type)
		ops = parent->ktype->child_ns_type(parent);

	return ops;
}

const struct kobj_ns_type_operations *kobj_ns_ops(struct kobject *kobj)
{
	return kobj_child_ns_ops(kobj->parent);
}


void *kobj_ns_grab_current(enum kobj_ns_type type)
{
	void *ns = NULL;

	spin_lock(&kobj_ns_type_lock);
	if ((type > KOBJ_NS_TYPE_NONE) && (type < KOBJ_NS_TYPES) &&
	    kobj_ns_ops_tbl[type])
		ns = kobj_ns_ops_tbl[type]->grab_current_ns();
	spin_unlock(&kobj_ns_type_lock);

	return ns;
}

const void *kobj_ns_netlink(enum kobj_ns_type type, struct sock *sk)
{
	const void *ns = NULL;

	spin_lock(&kobj_ns_type_lock);
	if ((type > KOBJ_NS_TYPE_NONE) && (type < KOBJ_NS_TYPES) &&
	    kobj_ns_ops_tbl[type])
		ns = kobj_ns_ops_tbl[type]->netlink_ns(sk);
	spin_unlock(&kobj_ns_type_lock);

	return ns;
}

const void *kobj_ns_initial(enum kobj_ns_type type)
{
	const void *ns = NULL;

	spin_lock(&kobj_ns_type_lock);
	if ((type > KOBJ_NS_TYPE_NONE) && (type < KOBJ_NS_TYPES) &&
	    kobj_ns_ops_tbl[type])
		ns = kobj_ns_ops_tbl[type]->initial_ns();
	spin_unlock(&kobj_ns_type_lock);

	return ns;
}

void kobj_ns_drop(enum kobj_ns_type type, void *ns)
{
	spin_lock(&kobj_ns_type_lock);
	if ((type > KOBJ_NS_TYPE_NONE) && (type < KOBJ_NS_TYPES) &&
	    kobj_ns_ops_tbl[type] && kobj_ns_ops_tbl[type]->drop_ns)
		kobj_ns_ops_tbl[type]->drop_ns(ns);
	spin_unlock(&kobj_ns_type_lock);
}

EXPORT_SYMBOL(kobject_get);
EXPORT_SYMBOL(kobject_put);
EXPORT_SYMBOL(kobject_del);

EXPORT_SYMBOL(kset_register);
EXPORT_SYMBOL(kset_unregister);
