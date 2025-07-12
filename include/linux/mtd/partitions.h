/*
 * MTD partitioning layer definitions
 *
 * (C) 2000 Nicolas Pitre <nico@fluxnic.net>
 *
 * This code is GPL
 */

#ifndef MTD_PARTITIONS_H
#define MTD_PARTITIONS_H

#include <linux/types.h>


/*
 * Partition definition structure:
 *
 * An array of struct partition is passed along with a MTD object to
 * mtd_device_register() to create them.
 *
 * For each partition, these fields are available:
 * name: string that will be used to label the partition's MTD device.
 * size: the partition size; if defined as MTDPART_SIZ_FULL, the partition
 * 	will extend to the end of the master MTD device.
 * offset: absolute starting position within the master MTD device; if
 * 	defined as MTDPART_OFS_APPEND, the partition will start where the
 *	previous one ended; if MTDPART_OFS_NXTBLK, at the next erase block;
 *	if MTDPART_OFS_RETAIN, consume as much as possible, leaving size
 *	after the end of partition.
 * mask_flags: contains flags that have to be masked (removed) from the
 * 	master MTD flag set for the corresponding MTD partition.
 * 	For example, to force a read-only partition, simply adding
 * 	MTD_WRITEABLE to the mask_flags will do the trick.
 *
 * Note: writeable partitions require their size and offset be
 * erasesize aligned (e.g. use MTDPART_OFS_NEXTBLK).
 */
/* 分区定义结构体
 * 将 struct partition 与 MTD对象 一起传到 mtd_device_register 函数中，可以创建
 * 它们 
 * 对于每一个分区，这些字段是可用的：
 * name: 字符串，用于标记分区的MTD设备 
 * size: 分区大小，如果定义为 MTDPART_SIZ_FULL ,则将分区扩展到主MTD设备的末端 
 * offset: 主MTD设备中的绝对起始地址； 如果定义为 MTDPART_OFS_APPEND, 则分区将从
 * 前一个分区结束的地方开始； 如果定义为 MTDPART_OFS_NXTBLK, 则在下一个擦除块开
 * 始； 如果是 MTDPART_OFS_RETAIN， 则尽可能多的消耗，但要最终保留下 size 大小的空间 
 *
 * MTDPART_OFS_APPEND 可以方便的让内核自动计算偏移地址，只需要提供分区大小即可； 
 * MTDPART_OFS_NXTBLK 让分区起始地址保持与块地址对齐；
 * MTDPART_OFS_RETAIN 可以在不明确MTD具体大小时，但需要预留一个特定大小的空间时
 * 使用，这样在最后面会预留指定大小的空间，其余空间全部用于分区 */

struct mtd_partition {
	char *name;			/* identifier string */
	uint64_t size;			/* partition size */
	uint64_t offset;		/* offset within the master MTD space */
	uint32_t mask_flags;		/* master MTD flags to mask out for this partition */
	struct nand_ecclayout *ecclayout;	/* out of band layout for this partition (NAND only) */
};

#define MTDPART_OFS_RETAIN	(-3)
#define MTDPART_OFS_NXTBLK	(-2)
#define MTDPART_OFS_APPEND	(-1)
#define MTDPART_SIZ_FULL	(0)


struct mtd_info;
struct device_node;

/**
 * struct mtd_part_parser_data - used to pass data to MTD partition parsers.
 * @origin: for RedBoot, start address of MTD device
 * @of_node: for OF parsers, device node containing partitioning information
 */
struct mtd_part_parser_data {
	unsigned long origin;
	struct device_node *of_node;
};


/*
 * Functions dealing with the various ways of partitioning the space
 */

struct mtd_part_parser {
	struct list_head list;
	struct module *owner;
	const char *name;
	int (*parse_fn)(struct mtd_info *, struct mtd_partition **,
			struct mtd_part_parser_data *);
};

extern int register_mtd_parser(struct mtd_part_parser *parser);
extern int deregister_mtd_parser(struct mtd_part_parser *parser);

int mtd_is_partition(struct mtd_info *mtd);
int mtd_add_partition(struct mtd_info *master, char *name,
		      long long offset, long long length);
int mtd_del_partition(struct mtd_info *master, int partno);

#endif
