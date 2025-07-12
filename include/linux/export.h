#ifndef _LINUX_EXPORT_H
#define _LINUX_EXPORT_H
/*
 * Export symbols from the kernel to modules.  Forked from module.h
 * to reduce the amount of pointless cruft we feed to gcc when only
 * exporting a simple symbol or two.
 *
 * If you feel the need to add #include <linux/foo.h> to this file
 * then you are doing something wrong and should go away silently.
 */
/* 将内核中的符号导出至模块中。该代码源自 module.h，旨在减少在仅导出少量简单符号
 * 时传递给编译器的无用冗余内容。
 *
 * 如果你觉得有必要在这段代码中添加“#include <linux/foo.h>”这一行，那么说明你做
 * 错了，那就默默地离开吧。
 * */

/* Some toolchains use a `_' prefix for all user symbols. */
/* 某些工具链会为所有用户符号添加一个 '_' 前缀
 * gcc 工具链没有，这里并没有定义 CONFIG_SYMBOL_PREFIX */
#ifdef CONFIG_SYMBOL_PREFIX
#define MODULE_SYMBOL_PREFIX CONFIG_SYMBOL_PREFIX
#else
#define MODULE_SYMBOL_PREFIX ""
#endif

/* 符号表结构
 * 会为每个符号定义一个本结构，记录地址 和 符号名字 */
struct kernel_symbol
{
    /* 符号地址，使用 (unsigned long)&sym 获得 */
	unsigned long value;
    /* 符号名字， __kstrtab_ 前缀的变量地址引用 */
	const char *name;
};

#ifdef MODULE
extern struct module __this_module;
#define THIS_MODULE (&__this_module)
#else
#define THIS_MODULE ((struct module *)0)
#endif

/* 开启了 modules 功能特性
 * 则可以使用导出符号功能 */
#ifdef CONFIG_MODULES

/* __GENKSYMS__ 是用于 cmd_gensymtypes 命令执行时使用的
 * 在生成内核工具 genksyms 时会定义 本宏
 * 在编译工具时，下面的这些都无效 */
#ifndef __GENKSYMS__
#ifdef CONFIG_MODVERSIONS
/* Mark the CRC weak since genksyms apparently decides not to
 * generate a checksums for some symbols */
/* __CRC_SYMBOL 用于内核模块校验机制
 * 默认定义了一个以 __crc 为前缀的符号，weak 的
 * 并将所有的这个符号地址放在了 ___kcrctab 开头的段里
 * 具体的机制没有太搞清楚，好像就是检查 符号的crc，
 * 这个检查机制好像是 模块可以提供的，要不怎么会是 弱定义呢 。。 */
#define __CRC_SYMBOL(sym, sec)					\
	extern void *__crc_##sym __attribute__((weak));		\
	static const unsigned long __kcrctab_##sym		\
	__used							\
	__attribute__((section("___kcrctab" sec "+" #sym), unused))	\
	= (unsigned long) &__crc_##sym;
#else
#define __CRC_SYMBOL(sym, sec)
#endif

/* For every exported symbol, place a struct in the __ksymtab section */
/* 导出符号 真正的宏
 * 使用 extern typeof(sym) sym;  引用了需要导出的符号
 * __CRC_SYMBOL(sym, sec) 创建符号校验段
 * static const char __kstrtab_##sym[] 定义了一个变量，用于存放被导出符号的
 * 字符串，并且将其放在 __ksymtab_strings 段中
 * static const struct kernel_symbol __ksymtab_##sym 定义了一个
 * struct kernel_symbol 的变量，存放符号地址和符号字符串地址
 * 并且放在了 ___ksymtab 开头的段里
 * 使用 (unsigned long)&sym 引用符号的地址
 * 使用 __kstrtab_##sym 引用存放符号名字字符串的变量地址 
 * */
#define __EXPORT_SYMBOL(sym, sec)				\
	extern typeof(sym) sym;					\
	__CRC_SYMBOL(sym, sec)					\
	static const char __kstrtab_##sym[]			\
	__attribute__((section("__ksymtab_strings"), aligned(1))) \
	= MODULE_SYMBOL_PREFIX #sym;				\
	static const struct kernel_symbol __ksymtab_##sym	\
	__used							\
	__attribute__((section("___ksymtab" sec "+" #sym), unused))	\
	= { (unsigned long)&sym, __kstrtab_##sym }

/* 导出普通符号
 * 这种符号没有许可证限制，加载模块时不会进行验证 */
#define EXPORT_SYMBOL(sym)					\
	__EXPORT_SYMBOL(sym, "")

/* 导出 GPL 许可的符号
 * 这类符号只能对 GPL许可 的模块使用
 * 模块中需要使用 MODULE_LICENSE("GPL") */
#define EXPORT_SYMBOL_GPL(sym)					\
	__EXPORT_SYMBOL(sym, "_gpl")

/* 一种预留兼容机制
 * 使用本宏导出的符号，将来可能会被用于GPL许可证的模块，也可能在未来会被重新考虑
 * 其它的许可证。 */
#define EXPORT_SYMBOL_GPL_FUTURE(sym)				\
	__EXPORT_SYMBOL(sym, "_gpl_future")

#ifdef CONFIG_UNUSED_SYMBOLS
#define EXPORT_UNUSED_SYMBOL(sym) __EXPORT_SYMBOL(sym, "_unused")
#define EXPORT_UNUSED_SYMBOL_GPL(sym) __EXPORT_SYMBOL(sym, "_unused_gpl")
#else
#define EXPORT_UNUSED_SYMBOL(sym)
#define EXPORT_UNUSED_SYMBOL_GPL(sym)
#endif

#endif	/* __GENKSYMS__ */

#else /* !CONFIG_MODULES... */
/* 未开启 modules 功能特性
 * 所有符号导出宏 全为空 */

#define EXPORT_SYMBOL(sym)
#define EXPORT_SYMBOL_GPL(sym)
#define EXPORT_SYMBOL_GPL_FUTURE(sym)
#define EXPORT_UNUSED_SYMBOL(sym)
#define EXPORT_UNUSED_SYMBOL_GPL(sym)

#endif /* CONFIG_MODULES */

#endif /* _LINUX_EXPORT_H */
