/*
 * kobject.h - generic kernel object infrastructure.
 *
 * Copyright (c) 2002-2003 Patrick Mochel
 * Copyright (c) 2002-2003 Open Source Development Labs
 * Copyright (c) 2006-2008 Greg Kroah-Hartman <greg@kroah.com>
 * Copyright (c) 2006-2008 Novell Inc.
 *
 * This file is released under the GPLv2.
 *
 * Please read Documentation/kobject.txt before using the kobject
 * interface, ESPECIALLY the parts about reference counts and object
 * destructors.
 */

#ifndef _KOBJECT_H_
#define _KOBJECT_H_

#include <linux/types.h>
#include <linux/list.h>
#include <linux/sysfs.h>
#include <linux/compiler.h>
#include <linux/spinlock.h>
#include <linux/kref.h>
#include <linux/kobject_ns.h>
#include <linux/kernel.h>
#include <linux/wait.h>
#include <linux/atomic.h>

/* uevent helper 路径最大长度 */
#define UEVENT_HELPER_PATH_LEN		256
/* uevent 环境变量最大数量 */
#define UEVENT_NUM_ENVP			32	/* number of env pointers */
/* uevent 环境变量缓冲区长度 */
#define UEVENT_BUFFER_SIZE		2048	/* buffer for the variables */

/* path to the userspace helper executed on an event */
extern char uevent_helper[];

/* counter to tag the uevent, read only except for the kobject core */
extern u64 uevent_seqnum;

/*
 * The actions here must match the index to the string array
 * in lib/kobject_uevent.c
 *
 * Do not add new actions here without checking with the driver-core
 * maintainers. Action strings are not meant to express subsystem
 * or device specific properties. In most cases you want to send a
 * kobject_uevent_env(kobj, KOBJ_CHANGE, env) with additional event
 * specific variables added to the event environment.
 */
/* kobject action 枚举，不能随意添加 */
enum kobject_action {
	KOBJ_ADD,
	KOBJ_REMOVE,
	KOBJ_CHANGE,
	KOBJ_MOVE,
	KOBJ_ONLINE,
	KOBJ_OFFLINE,
	KOBJ_MAX
};

struct kobject {
	const char		*name;      /* kobject 名字, sysfs 文件系统中目录名 */
	struct list_head	entry;  /* 与kset 和 其他 kobject 组成双向链表 */
	struct kobject		*parent;/* 指向父 kobject , sysfs 中的父目录 */
	struct kset		*kset;      /* 指向所属 kset 
                                 * kset 可以做为 parent 的候补：当注册时，
                                 * 传入的 parent 为 NULL 时，可以让 kset 来担当 */
	struct kobj_type	*ktype; /* kobject 的属性，即目录下的文件 */
	struct sysfs_dirent	*sd;    /* 指向 sysfs 中的节点， 类似 inode  */
	struct kref		kref;       /* 引用计数，原子变量 */
	unsigned int state_initialized:1;   /* 是否已初始化 */
	unsigned int state_in_sysfs:1;      /* 是否已在 sysfs 中 */
	unsigned int state_add_uevent_sent:1; /* 已发送 添加到 uevent 消息 */
	unsigned int state_remove_uevent_sent:1; /* 已发送 移除 uevent 消息  */
	unsigned int uevent_suppress:1;     /* 是否支持发送 uevent , =1 不发送 */
};

/* 设置 kobject 名字 */
extern __printf(2, 3)
int kobject_set_name(struct kobject *kobj, const char *name, ...);
extern int kobject_set_name_vargs(struct kobject *kobj, const char *fmt,
				  va_list vargs);

/* 返回 kobject 的 名字 */
static inline const char *kobject_name(const struct kobject *kobj)
{
	return kobj->name;
}

/* 初始化 kobject， 设置 kobj_type */
extern void kobject_init(struct kobject *kobj, struct kobj_type *ktype);
/* 将 kobject 加入到 sysfs 中 */
extern __printf(3, 4) __must_check
int kobject_add(struct kobject *kobj, struct kobject *parent,
		const char *fmt, ...);
extern __printf(4, 5) __must_check
int kobject_init_and_add(struct kobject *kobj,
			 struct kobj_type *ktype, struct kobject *parent,
			 const char *fmt, ...);

/* 将 kobject 从 sysfs 中移除 */
extern void kobject_del(struct kobject *kobj);

/* 创建 kobject ,并使用默认的 kobj_type  */
extern struct kobject * __must_check kobject_create(void);
extern struct kobject * __must_check kobject_create_and_add(const char *name,
						struct kobject *parent);

extern int __must_check kobject_rename(struct kobject *, const char *new_name);
extern int __must_check kobject_move(struct kobject *, struct kobject *);

extern struct kobject *kobject_get(struct kobject *kobj);
extern void kobject_put(struct kobject *kobj);

extern char *kobject_get_path(struct kobject *kobj, gfp_t flag);

struct kobj_type {
	void (*release)(struct kobject *kobj);      /* 释放函数，引用计数=0时调用 */
	const struct sysfs_ops *sysfs_ops;          /* 属性读写操作函数 */
	struct attribute **default_attrs;           /* 属性列表，可以有多个 */
	const struct kobj_ns_type_operations *(*child_ns_type)(struct kobject *kobj);
	const void *(*namespace)(struct kobject *kobj);
};

/* kobject 发送 uevent 时填充环境变量使用 */
struct kobj_uevent_env {
	char *envp[UEVENT_NUM_ENVP];
	int envp_idx;
	char buf[UEVENT_BUFFER_SIZE];
	int buflen;
};

/* kset uevent 操作函数
 * filter 用于特定的 kset 过滤函数，由使用 kset 者 实现
 * name 用于获取名字，指定了 kset 及要发送 uevent 的kobj 后生成 name
 * uevent 用于组织了 uevent 消息后，最后的处理 */
struct kset_uevent_ops {
	int (* const filter)(struct kset *kset, struct kobject *kobj);
	const char *(* const name)(struct kset *kset, struct kobject *kobj);
	int (* const uevent)(struct kset *kset, struct kobject *kobj,
		      struct kobj_uevent_env *env);
};

/* 为了方便匹配 属性 和 (show/store) 方法，提供了这样一个结构
 * */
struct kobj_attribute {
	struct attribute attr;
	ssize_t (*show)(struct kobject *kobj, struct kobj_attribute *attr,
			char *buf);
	ssize_t (*store)(struct kobject *kobj, struct kobj_attribute *attr,
			 const char *buf, size_t count);
};

extern const struct sysfs_ops kobj_sysfs_ops;

struct sock;

/**
 * struct kset - a set of kobjects of a specific type, belonging to a specific subsystem.
 *
 * A kset defines a group of kobjects.  They can be individually
 * different "types" but overall these kobjects all want to be grouped
 * together and operated on in the same manner.  ksets are used to
 * define the attribute callbacks and other common events that happen to
 * a kobject.
 *
 * @list: the list of all kobjects for this kset
 * @list_lock: a lock for iterating over the kobjects
 * @kobj: the embedded kobject for this kset (recursion, isn't it fun...)
 * @uevent_ops: the set of uevent operations for this kset.  These are
 * called whenever a kobject has something happen to it so that the kset
 * can add new environment variables, or filter out the uevents if so
 * desired.
 */
struct kset {
	struct list_head list;  /* 双向链表，将所有kobject链起来
                             * 与 kobj->entry 对应，用来组织本 kset 管理的 kobj */
	spinlock_t list_lock;   /* 自旋锁，互斥访问 */
	struct kobject kobj;    /* 内建的 kobject  */
	const struct kset_uevent_ops *uevent_ops;   /* uevent 相关 */
                                                /* kset 用于发送消息的操作函数集
                                                 * 需要指出的是，kset 能够发送它
                                                 * 所包含的各种子kobj、孙kobj 的
                                                 * 消息；即 kobj 或其父辈、爷爷
                                                 * 辈，都可以发送消息；优先父辈，
                                                 * 然后是爷爷辈，以此类推 */
};

extern void kset_init(struct kset *kset);
extern int __must_check kset_register(struct kset *kset);
extern void kset_unregister(struct kset *kset);
extern struct kset * __must_check kset_create_and_add(const char *name,
						const struct kset_uevent_ops *u,
						struct kobject *parent_kobj);

static inline struct kset *to_kset(struct kobject *kobj)
{
	return kobj ? container_of(kobj, struct kset, kobj) : NULL;
}

/* 递增 kset 引用计数
 * 实际递增的是内嵌的 kobject */
static inline struct kset *kset_get(struct kset *k)
{
    /* 递增的是 kset 中内嵌的 kobject 的引用计数 */
	return k ? to_kset(kobject_get(&k->kobj)) : NULL;
}

/* 递减 kset 引用计数
 * 实际递减的是内嵌的 kobject */
static inline void kset_put(struct kset *k)
{
    /* 递减的是 kset 中内嵌的 kobject 的引用计数 */
	kobject_put(&k->kobj);
}

/* 返回 kobject 中的 kobj_type 结构成员 */
static inline struct kobj_type *get_ktype(struct kobject *kobj)
{
	return kobj->ktype;
}

extern struct kobject *kset_find_obj(struct kset *, const char *);

/* The global /sys/kernel/ kobject for people to chain off of */
extern struct kobject *kernel_kobj;
/* The global /sys/kernel/mm/ kobject for people to chain off of */
extern struct kobject *mm_kobj;
/* The global /sys/hypervisor/ kobject for people to chain off of */
extern struct kobject *hypervisor_kobj;
/* The global /sys/power/ kobject for people to chain off of */
extern struct kobject *power_kobj;
/* The global /sys/firmware/ kobject for people to chain off of */
extern struct kobject *firmware_kobj;

/* 热插拔相关， kobject 产生 uevent 事件 */
#if defined(CONFIG_HOTPLUG)
int kobject_uevent(struct kobject *kobj, enum kobject_action action);
int kobject_uevent_env(struct kobject *kobj, enum kobject_action action,
			char *envp[]);

__printf(2, 3)
int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...);

int kobject_action_type(const char *buf, size_t count,
			enum kobject_action *type);
#else
static inline int kobject_uevent(struct kobject *kobj,
				 enum kobject_action action)
{ return 0; }
static inline int kobject_uevent_env(struct kobject *kobj,
				      enum kobject_action action,
				      char *envp[])
{ return 0; }

static inline __printf(2, 3)
int add_uevent_var(struct kobj_uevent_env *env, const char *format, ...)
{ return 0; }

static inline int kobject_action_type(const char *buf, size_t count,
				      enum kobject_action *type)
{ return -EINVAL; }
#endif

#endif /* _KOBJECT_H_ */
