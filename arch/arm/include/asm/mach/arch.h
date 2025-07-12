/*
 *  arch/arm/include/asm/mach/arch.h
 *
 *  Copyright (C) 2000 Russell King
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#ifndef __ASSEMBLY__

struct tag;
struct meminfo;
struct sys_timer;
struct pt_regs;

struct machine_desc {
	unsigned int		nr;		/* architecture number	*/
	const char		*name;		/* architecture name	*/
	unsigned long		atag_offset;	/* tagged list (relative) */
	const char *const 	*dt_compat;	/* array of device tree
						 * 'compatible' strings	*/

	unsigned int		nr_irqs;	/* number of IRQs */

#ifdef CONFIG_ZONE_DMA
	unsigned long		dma_zone_size;	/* size of DMA-able area */
#endif

	unsigned int		video_start;	/* start of video RAM	*/
	unsigned int		video_end;	/* end of video RAM	*/

	unsigned char		reserve_lp0 :1;	/* never has lp0	*/
	unsigned char		reserve_lp1 :1;	/* never has lp1	*/
	unsigned char		reserve_lp2 :1;	/* never has lp2	*/
	char			restart_mode;	/* default restart mode	*/
	void			(*fixup)(struct tag *, char **,
					 struct meminfo *);
	void			(*reserve)(void);/* reserve mem blocks	*/
	void			(*map_io)(void);/* IO mapping function	*/
	void			(*init_early)(void);
	void			(*init_irq)(void);
	struct sys_timer	*timer;		/* system tick timer	*/
	void			(*init_machine)(void);
	void			(*init_late)(void);
#ifdef CONFIG_MULTI_IRQ_HANDLER
	void			(*handle_irq)(struct pt_regs *);
#endif
	void			(*restart)(char, const char *);
};

/*
 * Current machine - only accessible during boot.
 */
extern struct machine_desc *machine_desc;

/*
 * Machine type table - also only accessible during boot
 */
/* __arch_info_begin  __arch_info_end  是编译器给定的变量，
 * 标记了 ".arch.info.init" 段的起始位置 
 * for_each_machine_desc 用于遍历所有的 machine_desc */
extern struct machine_desc __arch_info_begin[], __arch_info_end[];
#define for_each_machine_desc(p)			\
	for (p = __arch_info_begin; p < __arch_info_end; p++)

/*
 * Set of macros to define architecture features.  This is built into
 * a table by the linker.
 */
/* MACHINE_START 与 MACHINE_END 
 * 用来定义一个板级设备， _type 会组成一个 MACH_TYPE__type 的宏
 * 该宏必须在 include/generated/mach-types.h 文件中 
 * 是一个唯一的数字，与 uboot 中的 mach_id 需要一致 
 * _name 只是一个名字，匹配后打印出来 */
/* 该宏定义的所有结构体都会存放在 ".arch.info.init" 段中，
 * 上电初始化时，会由 arch/arm/kernel/setup.c 中的
 * setup_machine_tags 函数 调用 for_each_machine_desc 进行匹配 */
#define MACHINE_START(_type,_name)			\
static const struct machine_desc __mach_desc_##_type	\
 __used							\
 __attribute__((__section__(".arch.info.init"))) = {	\
	.nr		= MACH_TYPE_##_type,		\
	.name		= _name,

#define MACHINE_END				\
};

#define DT_MACHINE_START(_name, _namestr)		\
static const struct machine_desc __mach_desc_##_name	\
 __used							\
 __attribute__((__section__(".arch.info.init"))) = {	\
	.nr		= ~0,				\
	.name		= _namestr,

#endif
