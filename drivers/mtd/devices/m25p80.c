/*
 * MTD SPI driver for ST M25Pxx (and similar) serial flash chips
 *
 * Author: Mike Lavender, mike@steroidmicros.com
 *
 * Copyright (c) 2005, Intec Automation Inc.
 *
 * Some parts are based on lart.c by Abraham Van Der Merwe
 *
 * Cleaned up and generalized based on mtd_dataflash.c
 *
 * This code is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */

#include <linux/init.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/interrupt.h>
#include <linux/mutex.h>
#include <linux/math64.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/mod_devicetable.h>

#include <linux/mtd/cfi.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#include <linux/of_platform.h>

#include <linux/spi/spi.h>
#include <linux/spi/flash.h>

/* Flash opcodes. */
#define	OPCODE_WREN		0x06	/* Write enable */
#define	OPCODE_RDSR		0x05	/* Read status register */
#define	OPCODE_WRSR		0x01	/* Write status register 1 byte */
#define	OPCODE_NORM_READ	0x03	/* Read data bytes (low frequency) */
#define	OPCODE_FAST_READ	0x0b	/* Read data bytes (high frequency) */
#define	OPCODE_PP		0x02	/* Page program (up to 256 bytes) */
#define	OPCODE_BE_4K		0x20	/* Erase 4KiB block */
#define	OPCODE_BE_32K		0x52	/* Erase 32KiB block */
#define	OPCODE_CHIP_ERASE	0xc7	/* Erase whole flash chip */
#define	OPCODE_SE		0xd8	/* Sector erase (usually 64KiB) */
#define	OPCODE_RDID		0x9f	/* Read JEDEC ID */

/* Used for SST flashes only. */
#define	OPCODE_BP		0x02	/* Byte program */
#define	OPCODE_WRDI		0x04	/* Write disable */
#define	OPCODE_AAI_WP		0xad	/* Auto address increment word program */

/* Used for Macronix flashes only. */
#define	OPCODE_EN4B		0xb7	/* Enter 4-byte mode */
#define	OPCODE_EX4B		0xe9	/* Exit 4-byte mode */

/* Used for Spansion flashes only. */
#define	OPCODE_BRWR		0x17	/* Bank register write */

/* Status Register bits. */
#define	SR_WIP			1	/* Write in progress */
#define	SR_WEL			2	/* Write enable latch */
/* meaning of other SR_* bits may differ between vendors */
#define	SR_BP0			4	/* Block protect 0 */
#define	SR_BP1			8	/* Block protect 1 */
#define	SR_BP2			0x10	/* Block protect 2 */
#define	SR_SRWD			0x80	/* SR write protect */

/* Define max times to check status register before we give up. */
#define	MAX_READY_WAIT_JIFFIES	(40 * HZ)	/* M25P16 specs 40s max chip erase */
#define	MAX_CMD_SIZE		5

#ifdef CONFIG_M25PXX_USE_FAST_READ
#define OPCODE_READ 	OPCODE_FAST_READ
#define FAST_READ_DUMMY_BYTE 1
#else
#define OPCODE_READ 	OPCODE_NORM_READ
#define FAST_READ_DUMMY_BYTE 0
#endif

#define JEDEC_MFR(_jedec_id)	((_jedec_id) >> 16)

/****************************************************************************/

struct m25p {
	struct spi_device	*spi;
	struct mutex		lock;
	struct mtd_info		mtd;
	u16			page_size;
	u16			addr_width;
	u8			erase_opcode;
	u8			*command;
};

static inline struct m25p *mtd_to_m25p(struct mtd_info *mtd)
{
	return container_of(mtd, struct m25p, mtd);
}

/****************************************************************************/

/*
 * Internal helper functions
 */

/*
 * Read the status register, returning its value in the location
 * Return the status register value.
 * Returns negative if error occurred.
 */
/* 读状态寄存器 */
static int read_sr(struct m25p *flash)
{
	ssize_t retval;
	u8 code = OPCODE_RDSR;
	u8 val;

	retval = spi_write_then_read(flash->spi, &code, 1, &val, 1);

	if (retval < 0) {
		dev_err(&flash->spi->dev, "error %d reading SR\n",
				(int) retval);
		return retval;
	}

	return val;
}

/*
 * Write status register 1 byte
 * Returns negative if error occurred.
 */
/* 写状态寄存器 一个字节 */
static int write_sr(struct m25p *flash, u8 val)
{
	flash->command[0] = OPCODE_WRSR;
	flash->command[1] = val;

	return spi_write(flash->spi, flash->command, 2);
}

/*
 * Set write enable latch with Write Enable command.
 * Returns negative if error occurred.
 */
/* 设置写使能 */
static inline int write_enable(struct m25p *flash)
{
	u8	code = OPCODE_WREN;

	return spi_write_then_read(flash->spi, &code, 1, NULL, 0);
}

/*
 * Send write disble instruction to the chip.
 */
/* 设置写失能 */
static inline int write_disable(struct m25p *flash)
{
	u8	code = OPCODE_WRDI;

	return spi_write_then_read(flash->spi, &code, 1, NULL, 0);
}

/*
 * Enable/disable 4-byte addressing mode.
 */
/* 使能/失能 4字节地址线模式
 * 这个功能与不同芯片有关，该函数比较老了，某些芯片并不适用 */
static inline int set_4byte(struct m25p *flash, u32 jedec_id, int enable)
{
	switch (JEDEC_MFR(jedec_id)) {
	case CFI_MFR_MACRONIX:
		flash->command[0] = enable ? OPCODE_EN4B : OPCODE_EX4B;
		return spi_write(flash->spi, flash->command, 1);
	default:
		/* Spansion style */
		flash->command[0] = OPCODE_BRWR;
		flash->command[1] = enable << 7;
		return spi_write(flash->spi, flash->command, 2);
	}
}

/*
 * Service routine to read status register until ready, or timeout occurs.
 * Returns non-zero if error.
 */
/* 读状态寄存器，直到就绪状态 或 超时 */
static int wait_till_ready(struct m25p *flash)
{
	unsigned long deadline;
	int sr;

	deadline = jiffies + MAX_READY_WAIT_JIFFIES;

	do {
		if ((sr = read_sr(flash)) < 0)
			break;
		else if (!(sr & SR_WIP))
			return 0;

		cond_resched();

	} while (!time_after_eq(jiffies, deadline));

	return 1;
}

/*
 * Erase the whole flash memory
 *
 * Returns 0 if successful, non-zero otherwise.
 */
/* 擦除整个芯片 */
static int erase_chip(struct m25p *flash)
{
	pr_debug("%s: %s %lldKiB\n", dev_name(&flash->spi->dev), __func__,
			(long long)(flash->mtd.size >> 10));

	/* Wait until finished previous write command. */
	if (wait_till_ready(flash))
		return 1;

	/* Send write enable, then erase commands. */
	write_enable(flash);

	/* Set up command buffer. */
	flash->command[0] = OPCODE_CHIP_ERASE;

	spi_write(flash->spi, flash->command, 1);

	return 0;
}

/* 转换地址到 cmd 数组中，cmd[0] 存放的是 opcode  */
static void m25p_addr2cmd(struct m25p *flash, unsigned int addr, u8 *cmd)
{
	/* opcode is in cmd[0] */
	cmd[1] = addr >> (flash->addr_width * 8 -  8);
	cmd[2] = addr >> (flash->addr_width * 8 - 16);
	cmd[3] = addr >> (flash->addr_width * 8 - 24);
	cmd[4] = addr >> (flash->addr_width * 8 - 32);
}

/* 计算 cmd 长度 */
static int m25p_cmdsz(struct m25p *flash)
{
	return 1 + flash->addr_width;
}

/*
 * Erase one sector of flash memory at offset ``offset'' which is any
 * address within the sector which should be erased.
 *
 * Returns 0 if successful, non-zero otherwise.
 */
/* 擦除一个扇区 */
static int erase_sector(struct m25p *flash, u32 offset)
{
	pr_debug("%s: %s %dKiB at 0x%08x\n", dev_name(&flash->spi->dev),
			__func__, flash->mtd.erasesize / 1024, offset);

	/* Wait until finished previous write command. */
	if (wait_till_ready(flash))
		return 1;

	/* Send write enable, then erase commands. */
	write_enable(flash);

	/* Set up command buffer. */
	flash->command[0] = flash->erase_opcode;
	m25p_addr2cmd(flash, offset, flash->command);

	spi_write(flash->spi, flash->command, m25p_cmdsz(flash));

	return 0;
}

/****************************************************************************/

/*
 * MTD implementation
 */

/*
 * Erase an address range on the flash chip.  The address range may extend
 * one or more erase sectors.  Return an error is there is a problem erasing.
 */
/* 擦除某个地址范围 */
static int m25p80_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct m25p *flash = mtd_to_m25p(mtd);
	u32 addr,len;
	uint32_t rem;

	pr_debug("%s: %s at 0x%llx, len %lld\n", dev_name(&flash->spi->dev),
			__func__, (long long)instr->addr,
			(long long)instr->len);

	div_u64_rem(instr->len, mtd->erasesize, &rem);
	if (rem)
		return -EINVAL;

	addr = instr->addr;
	len = instr->len;

	mutex_lock(&flash->lock);

	/* whole-chip erase? */
	if (len == flash->mtd.size) {
		if (erase_chip(flash)) {
			instr->state = MTD_ERASE_FAILED;
			mutex_unlock(&flash->lock);
			return -EIO;
		}

	/* REVISIT in some cases we could speed up erasing large regions
	 * by using OPCODE_SE instead of OPCODE_BE_4K.  We may have set up
	 * to use "small sector erase", but that's not always optimal.
	 */

	/* "sector"-at-a-time erase */
	} else {
		while (len) {
			if (erase_sector(flash, addr)) {
				instr->state = MTD_ERASE_FAILED;
				mutex_unlock(&flash->lock);
				return -EIO;
			}

			addr += mtd->erasesize;
			len -= mtd->erasesize;
		}
	}

	mutex_unlock(&flash->lock);

    /* 调用擦除完成回调函数，通知已完成 */
	instr->state = MTD_ERASE_DONE;
	mtd_erase_callback(instr);

	return 0;
}

/*
 * Read an address range from the flash chip.  The address range
 * may be any size provided it is within the physical boundaries.
 */
/* 读特定地址范围数据 */
static int m25p80_read(struct mtd_info *mtd, loff_t from, size_t len,
	size_t *retlen, u_char *buf)
{
	struct m25p *flash = mtd_to_m25p(mtd);
	struct spi_transfer t[2];
	struct spi_message m;

	pr_debug("%s: %s from 0x%08x, len %zd\n", dev_name(&flash->spi->dev),
			__func__, (u32)from, len);

	spi_message_init(&m);
	memset(t, 0, (sizeof t));

	/* NOTE:
	 * OPCODE_FAST_READ (if available) is faster.
	 * Should add 1 byte DUMMY_BYTE.
	 */
	t[0].tx_buf = flash->command;
	t[0].len = m25p_cmdsz(flash) + FAST_READ_DUMMY_BYTE;
	spi_message_add_tail(&t[0], &m);

	t[1].rx_buf = buf;
	t[1].len = len;
	spi_message_add_tail(&t[1], &m);

	mutex_lock(&flash->lock);

	/* Wait till previous write/erase is done. */
	if (wait_till_ready(flash)) {
		/* REVISIT status return?? */
		mutex_unlock(&flash->lock);
		return 1;
	}

	/* FIXME switch to OPCODE_FAST_READ.  It's required for higher
	 * clocks; and at this writing, every chip this driver handles
	 * supports that opcode.
	 */

	/* Set up the write data buffer. */
	flash->command[0] = OPCODE_READ;
	m25p_addr2cmd(flash, from, flash->command);

	spi_sync(flash->spi, &m);

	*retlen = m.actual_length - m25p_cmdsz(flash) - FAST_READ_DUMMY_BYTE;

	mutex_unlock(&flash->lock);

	return 0;
}

/*
 * Write an address range to the flash chip.  Data must be written in
 * FLASH_PAGESIZE chunks.  The address range may be any size provided
 * it is within the physical boundaries.
 */
/* 写特定地址范围数据 */
static int m25p80_write(struct mtd_info *mtd, loff_t to, size_t len,
	size_t *retlen, const u_char *buf)
{
	struct m25p *flash = mtd_to_m25p(mtd);
	u32 page_offset, page_size;
	struct spi_transfer t[2];
	struct spi_message m;

	pr_debug("%s: %s to 0x%08x, len %zd\n", dev_name(&flash->spi->dev),
			__func__, (u32)to, len);

	spi_message_init(&m);
	memset(t, 0, (sizeof t));

	t[0].tx_buf = flash->command;
	t[0].len = m25p_cmdsz(flash);
	spi_message_add_tail(&t[0], &m);

	t[1].tx_buf = buf;
	spi_message_add_tail(&t[1], &m);

	mutex_lock(&flash->lock);

	/* Wait until finished previous write command. */
	if (wait_till_ready(flash)) {
		mutex_unlock(&flash->lock);
		return 1;
	}

	write_enable(flash);

	/* Set up the opcode in the write buffer. */
	flash->command[0] = OPCODE_PP;
	m25p_addr2cmd(flash, to, flash->command);

	page_offset = to & (flash->page_size - 1);

	/* do all the bytes fit onto one page? */
	if (page_offset + len <= flash->page_size) {
		t[1].len = len;

		spi_sync(flash->spi, &m);

		*retlen = m.actual_length - m25p_cmdsz(flash);
	} else {
		u32 i;

		/* the size of data remaining on the first page */
		page_size = flash->page_size - page_offset;

		t[1].len = page_size;
		spi_sync(flash->spi, &m);

		*retlen = m.actual_length - m25p_cmdsz(flash);

		/* write everything in flash->page_size chunks */
		for (i = page_size; i < len; i += page_size) {
			page_size = len - i;
			if (page_size > flash->page_size)
				page_size = flash->page_size;

			/* write the next page to flash */
			m25p_addr2cmd(flash, to + i, flash->command);

			t[1].tx_buf = buf + i;
			t[1].len = page_size;

			wait_till_ready(flash);

			write_enable(flash);

			spi_sync(flash->spi, &m);

			*retlen += m.actual_length - m25p_cmdsz(flash);
		}
	}

	mutex_unlock(&flash->lock);

	return 0;
}

/* sst 芯片特有的写操作 */
static int sst_write(struct mtd_info *mtd, loff_t to, size_t len,
		size_t *retlen, const u_char *buf)
{
	struct m25p *flash = mtd_to_m25p(mtd);
	struct spi_transfer t[2];
	struct spi_message m;
	size_t actual;
	int cmd_sz, ret;

	pr_debug("%s: %s to 0x%08x, len %zd\n", dev_name(&flash->spi->dev),
			__func__, (u32)to, len);

	spi_message_init(&m);
	memset(t, 0, (sizeof t));

	t[0].tx_buf = flash->command;
	t[0].len = m25p_cmdsz(flash);
	spi_message_add_tail(&t[0], &m);

	t[1].tx_buf = buf;
	spi_message_add_tail(&t[1], &m);

	mutex_lock(&flash->lock);

	/* Wait until finished previous write command. */
	ret = wait_till_ready(flash);
	if (ret)
		goto time_out;

	write_enable(flash);

	actual = to % 2;
	/* Start write from odd address. */
	if (actual) {
		flash->command[0] = OPCODE_BP;
		m25p_addr2cmd(flash, to, flash->command);

		/* write one byte. */
		t[1].len = 1;
		spi_sync(flash->spi, &m);
		ret = wait_till_ready(flash);
		if (ret)
			goto time_out;
		*retlen += m.actual_length - m25p_cmdsz(flash);
	}
	to += actual;

	flash->command[0] = OPCODE_AAI_WP;
	m25p_addr2cmd(flash, to, flash->command);

	/* Write out most of the data here. */
	cmd_sz = m25p_cmdsz(flash);
	for (; actual < len - 1; actual += 2) {
		t[0].len = cmd_sz;
		/* write two bytes. */
		t[1].len = 2;
		t[1].tx_buf = buf + actual;

		spi_sync(flash->spi, &m);
		ret = wait_till_ready(flash);
		if (ret)
			goto time_out;
		*retlen += m.actual_length - cmd_sz;
		cmd_sz = 1;
		to += 2;
	}
	write_disable(flash);
	ret = wait_till_ready(flash);
	if (ret)
		goto time_out;

	/* Write out trailing byte if it exists. */
	if (actual != len) {
		write_enable(flash);
		flash->command[0] = OPCODE_BP;
		m25p_addr2cmd(flash, to, flash->command);
		t[0].len = m25p_cmdsz(flash);
		t[1].len = 1;
		t[1].tx_buf = buf + actual;

		spi_sync(flash->spi, &m);
		ret = wait_till_ready(flash);
		if (ret)
			goto time_out;
		*retlen += m.actual_length - m25p_cmdsz(flash);
		write_disable(flash);
	}

time_out:
	mutex_unlock(&flash->lock);
	return ret;
}

/****************************************************************************/

/*
 * SPI device driver setup and teardown
 */
/* 本驱动中所使用的 SPI 信息 */
struct flash_info {
	/* JEDEC id zero means "no ID" (most older chips); otherwise it has
	 * a high byte of zero plus three data bytes: the manufacturer id,
	 * then a two byte device id.
	 */
    /* JEDEC id 如果是0，意味着没有 ID，这主要是一些老设备 
     * 否则，它有一个高字节0加上三个数据字节：制造商ID，两字节的设备ID
     * ext_id 某些芯片有，一些功能的小差异 */
	u32		jedec_id;
	u16             ext_id;

	/* The size listed here is what works with OPCODE_SE, which isn't
	 * necessarily called a "sector" by the vendor.
	 */
    /* 扇区大小，整个芯片的扇区数量，
     * 这两个决定了整个芯片的大小 */
	unsigned	sector_size;
	u16		n_sectors;

    /* 页大小 */
	u16		page_size;
    /* 地址宽度，小于16M的一般为 3字节地址，大于16M的为4字节地址 */
	u16		addr_width;

	u16		flags;
#define	SECT_4K		0x01		/* OPCODE_BE_4K works uniformly */
#define	M25P_NO_ERASE	0x02		/* No erase command needed */
};

#define INFO(_jedec_id, _ext_id, _sector_size, _n_sectors, _flags)	\
	((kernel_ulong_t)&(struct flash_info) {				\
		.jedec_id = (_jedec_id),				\
		.ext_id = (_ext_id),					\
		.sector_size = (_sector_size),				\
		.n_sectors = (_n_sectors),				\
		.page_size = 256,					\
		.flags = (_flags),					\
	})

#define CAT25_INFO(_sector_size, _n_sectors, _page_size, _addr_width)	\
	((kernel_ulong_t)&(struct flash_info) {				\
		.sector_size = (_sector_size),				\
		.n_sectors = (_n_sectors),				\
		.page_size = (_page_size),				\
		.addr_width = (_addr_width),				\
		.flags = M25P_NO_ERASE,					\
	})

/* NOTE: double check command sets and memory organization when you add
 * more flash chips.  This current list focusses on newer chips, which
 * have been converging on command sets which including JEDEC ID.
 */
static const struct spi_device_id m25p_ids[] = {
	/* Atmel -- some are (confusingly) marketed as "DataFlash" */
	{ "at25fs010",  INFO(0x1f6601, 0, 32 * 1024,   4, SECT_4K) },
	{ "at25fs040",  INFO(0x1f6604, 0, 64 * 1024,   8, SECT_4K) },

	{ "at25df041a", INFO(0x1f4401, 0, 64 * 1024,   8, SECT_4K) },
	{ "at25df321a", INFO(0x1f4701, 0, 64 * 1024,  64, SECT_4K) },
	{ "at25df641",  INFO(0x1f4800, 0, 64 * 1024, 128, SECT_4K) },

	{ "at26f004",   INFO(0x1f0400, 0, 64 * 1024,  8, SECT_4K) },
	{ "at26df081a", INFO(0x1f4501, 0, 64 * 1024, 16, SECT_4K) },
	{ "at26df161a", INFO(0x1f4601, 0, 64 * 1024, 32, SECT_4K) },
	{ "at26df321",  INFO(0x1f4700, 0, 64 * 1024, 64, SECT_4K) },

	/* EON -- en25xxx */
	{ "en25f32", INFO(0x1c3116, 0, 64 * 1024,  64, SECT_4K) },
	{ "en25p32", INFO(0x1c2016, 0, 64 * 1024,  64, 0) },
	{ "en25q32b", INFO(0x1c3016, 0, 64 * 1024,  64, 0) },
	{ "en25p64", INFO(0x1c2017, 0, 64 * 1024, 128, 0) },

	/* Everspin */
	{ "mr25h256", CAT25_INFO(  32 * 1024, 1, 256, 2) },

	/* Intel/Numonyx -- xxxs33b */
	{ "160s33b",  INFO(0x898911, 0, 64 * 1024,  32, 0) },
	{ "320s33b",  INFO(0x898912, 0, 64 * 1024,  64, 0) },
	{ "640s33b",  INFO(0x898913, 0, 64 * 1024, 128, 0) },

	/* Macronix */
	{ "mx25l2005a",  INFO(0xc22012, 0, 64 * 1024,   4, SECT_4K) },
	{ "mx25l4005a",  INFO(0xc22013, 0, 64 * 1024,   8, SECT_4K) },
	{ "mx25l8005",   INFO(0xc22014, 0, 64 * 1024,  16, 0) },
	{ "mx25l1606e",  INFO(0xc22015, 0, 64 * 1024,  32, SECT_4K) },
	{ "mx25l3205d",  INFO(0xc22016, 0, 64 * 1024,  64, 0) },
	{ "mx25l6405d",  INFO(0xc22017, 0, 64 * 1024, 128, 0) },
	{ "mx25l12805d", INFO(0xc22018, 0, 64 * 1024, 256, 0) },
	{ "mx25l12855e", INFO(0xc22618, 0, 64 * 1024, 256, 0) },
	{ "mx25l25635e", INFO(0xc22019, 0, 64 * 1024, 512, 0) },
	{ "mx25l25655e", INFO(0xc22619, 0, 64 * 1024, 512, 0) },

	/* Spansion -- single (large) sector size only, at least
	 * for the chips listed here (without boot sectors).
	 */
	{ "s25sl004a",  INFO(0x010212,      0,  64 * 1024,   8, 0) },
	{ "s25sl008a",  INFO(0x010213,      0,  64 * 1024,  16, 0) },
	{ "s25sl016a",  INFO(0x010214,      0,  64 * 1024,  32, 0) },
	{ "s25sl032a",  INFO(0x010215,      0,  64 * 1024,  64, 0) },
	{ "s25sl032p",  INFO(0x010215, 0x4d00,  64 * 1024,  64, SECT_4K) },
	{ "s25sl064a",  INFO(0x010216,      0,  64 * 1024, 128, 0) },
	{ "s25fl256s0", INFO(0x010219, 0x4d00, 256 * 1024, 128, 0) },
	{ "s25fl256s1", INFO(0x010219, 0x4d01,  64 * 1024, 512, 0) },
	{ "s25fl512s",  INFO(0x010220, 0x4d00, 256 * 1024, 256, 0) },
	{ "s70fl01gs",  INFO(0x010221, 0x4d00, 256 * 1024, 256, 0) },
	{ "s25sl12800", INFO(0x012018, 0x0300, 256 * 1024,  64, 0) },
	{ "s25sl12801", INFO(0x012018, 0x0301,  64 * 1024, 256, 0) },
	{ "s25fl129p0", INFO(0x012018, 0x4d00, 256 * 1024,  64, 0) },
	{ "s25fl129p1", INFO(0x012018, 0x4d01,  64 * 1024, 256, 0) },
	{ "s25fl016k",  INFO(0xef4015,      0,  64 * 1024,  32, SECT_4K) },
	{ "s25fl064k",  INFO(0xef4017,      0,  64 * 1024, 128, SECT_4K) },

	/* SST -- large erase sizes are "overlays", "sectors" are 4K */
	{ "sst25vf040b", INFO(0xbf258d, 0, 64 * 1024,  8, SECT_4K) },
	{ "sst25vf080b", INFO(0xbf258e, 0, 64 * 1024, 16, SECT_4K) },
	{ "sst25vf016b", INFO(0xbf2541, 0, 64 * 1024, 32, SECT_4K) },
	{ "sst25vf032b", INFO(0xbf254a, 0, 64 * 1024, 64, SECT_4K) },
	{ "sst25wf512",  INFO(0xbf2501, 0, 64 * 1024,  1, SECT_4K) },
	{ "sst25wf010",  INFO(0xbf2502, 0, 64 * 1024,  2, SECT_4K) },
	{ "sst25wf020",  INFO(0xbf2503, 0, 64 * 1024,  4, SECT_4K) },
	{ "sst25wf040",  INFO(0xbf2504, 0, 64 * 1024,  8, SECT_4K) },

	/* ST Microelectronics -- newer production may have feature updates */
	{ "m25p05",  INFO(0x202010,  0,  32 * 1024,   2, 0) },
	{ "m25p10",  INFO(0x202011,  0,  32 * 1024,   4, 0) },
	{ "m25p20",  INFO(0x202012,  0,  64 * 1024,   4, 0) },
	{ "m25p40",  INFO(0x202013,  0,  64 * 1024,   8, 0) },
	{ "m25p80",  INFO(0x202014,  0,  64 * 1024,  16, 0) },
	{ "m25p16",  INFO(0x202015,  0,  64 * 1024,  32, 0) },
	{ "m25p32",  INFO(0x202016,  0,  64 * 1024,  64, 0) },
	{ "m25p64",  INFO(0x202017,  0,  64 * 1024, 128, 0) },
	{ "m25p128", INFO(0x202018,  0, 256 * 1024,  64, 0) },

	{ "m25p05-nonjedec",  INFO(0, 0,  32 * 1024,   2, 0) },
	{ "m25p10-nonjedec",  INFO(0, 0,  32 * 1024,   4, 0) },
	{ "m25p20-nonjedec",  INFO(0, 0,  64 * 1024,   4, 0) },
	{ "m25p40-nonjedec",  INFO(0, 0,  64 * 1024,   8, 0) },
	{ "m25p80-nonjedec",  INFO(0, 0,  64 * 1024,  16, 0) },
	{ "m25p16-nonjedec",  INFO(0, 0,  64 * 1024,  32, 0) },
	{ "m25p32-nonjedec",  INFO(0, 0,  64 * 1024,  64, 0) },
	{ "m25p64-nonjedec",  INFO(0, 0,  64 * 1024, 128, 0) },
	{ "m25p128-nonjedec", INFO(0, 0, 256 * 1024,  64, 0) },

	{ "m45pe10", INFO(0x204011,  0, 64 * 1024,    2, 0) },
	{ "m45pe80", INFO(0x204014,  0, 64 * 1024,   16, 0) },
	{ "m45pe16", INFO(0x204015,  0, 64 * 1024,   32, 0) },

	{ "m25pe80", INFO(0x208014,  0, 64 * 1024, 16,       0) },
	{ "m25pe16", INFO(0x208015,  0, 64 * 1024, 32, SECT_4K) },

	{ "m25px32",    INFO(0x207116,  0, 64 * 1024, 64, SECT_4K) },
	{ "m25px32-s0", INFO(0x207316,  0, 64 * 1024, 64, SECT_4K) },
	{ "m25px32-s1", INFO(0x206316,  0, 64 * 1024, 64, SECT_4K) },
	{ "m25px64",    INFO(0x207117,  0, 64 * 1024, 128, 0) },

	/* Winbond -- w25x "blocks" are 64K, "sectors" are 4KiB */
	{ "w25x10", INFO(0xef3011, 0, 64 * 1024,  2,  SECT_4K) },
	{ "w25x20", INFO(0xef3012, 0, 64 * 1024,  4,  SECT_4K) },
	{ "w25x40", INFO(0xef3013, 0, 64 * 1024,  8,  SECT_4K) },
	{ "w25x80", INFO(0xef3014, 0, 64 * 1024,  16, SECT_4K) },
	{ "w25x16", INFO(0xef3015, 0, 64 * 1024,  32, SECT_4K) },
	{ "w25x32", INFO(0xef3016, 0, 64 * 1024,  64, SECT_4K) },
	{ "w25q32", INFO(0xef4016, 0, 64 * 1024,  64, SECT_4K) },
	{ "w25x64", INFO(0xef3017, 0, 64 * 1024, 128, SECT_4K) },
	{ "w25q64", INFO(0xef4017, 0, 64 * 1024, 128, SECT_4K) },
	{ "w25q128", INFO(0xef4018, 0, 64 * 1024, 256, SECT_4K) },
	{ "w25q80", INFO(0xef5014, 0, 64 * 1024,  16, SECT_4K) },

	/* Catalyst / On Semiconductor -- non-JEDEC */
	{ "cat25c11", CAT25_INFO(  16, 8, 16, 1) },
	{ "cat25c03", CAT25_INFO(  32, 8, 16, 2) },
	{ "cat25c09", CAT25_INFO( 128, 8, 32, 2) },
	{ "cat25c17", CAT25_INFO( 256, 8, 32, 2) },
	{ "cat25128", CAT25_INFO(2048, 8, 64, 2) },
	{ },
};
MODULE_DEVICE_TABLE(spi, m25p_ids);

/* 通过 jedec 标准命令字 RDID 获取flash ID，确认具体型号，
 * 返回的为 m25p_ids 中对应型号的索引 */
static const struct spi_device_id *__devinit jedec_probe(struct spi_device *spi)
{
	int			tmp;
	u8			code = OPCODE_RDID;
	u8			id[5];
	u32			jedec;
	u16                     ext_jedec;
	struct flash_info	*info;

	/* JEDEC also defines an optional "extended device information"
	 * string for after vendor-specific data, after the three bytes
	 * we use here.  Supporting some chips might require using it.
	 */
    /* JEDEC RDID 指令有3个字节的厂商ID+设备ID，
     * 另外后面可能还有2字节的扩展ID */
	tmp = spi_write_then_read(spi, &code, 1, id, 5);
	if (tmp < 0) {
		pr_debug("%s: error %d reading JEDEC ID\n",
				dev_name(&spi->dev), tmp);
		return ERR_PTR(tmp);
	}
	jedec = id[0];
	jedec = jedec << 8;
	jedec |= id[1];
	jedec = jedec << 8;
	jedec |= id[2];

	ext_jedec = id[3] << 8 | id[4];

    /* 根据 jedec id 遍历 m25p_ids ，查找对应的信息，
     * 另外，如果 m25p_ids 表中定义了扩展ID（不为0），
     * 则扩展ID也必须相同才行 
     * 找到对应的信息后直接做为返回值返回了 */
	for (tmp = 0; tmp < ARRAY_SIZE(m25p_ids) - 1; tmp++) {
		info = (void *)m25p_ids[tmp].driver_data;
		if (info->jedec_id == jedec) {
			if (info->ext_id != 0 && info->ext_id != ext_jedec)
				continue;
			return &m25p_ids[tmp];
		}
	}
	dev_err(&spi->dev, "unrecognized JEDEC id %06x\n", jedec);
	return ERR_PTR(-ENODEV);
}

/* 一个板级定义的 spi flash 结构如下：
 * 分区信息
 * static struct mtd_partition xxx_spi_flash_partitions[] = {
 *  {
 *      .name = "bootloader(spi)",
 *      .size = 0x0100000,
 *      .offset = 0,
 *  },
 * };
 *
 * 实际使用的 SPI FLASH 芯片型号信息， .type 成员指示
 * static struct flash_platform_data xxx_spi_flash_data = {
 *  .name = "m25p80",
 *  .parts = xxx_spi_flash_partitions,
 *  .nr_parts = ARRAY_SIZE(xxx_spi_flash_partitions),
 *  .type = "w25x16",
 * };
 *
 * spi 板级信息，包含了设备名，bus号，并指到了真正的SPI FLASH
 * static struct spi_board_info xxx_spi_board_info[] __initdata = {
 *  {
 *      .modalias = "m25p80", 
 *      .max_speed_hz = 20000000,
 *      .bus_num = 0,
 *      .chip_select = 0,
 *      .platform_data = &xxx_spi_flash_data,
 *      .mode = SPI_MODE_0,
 *  },
 * };
 *
 * 使用 spi_register_board_info 进行板级设备注册时，最终是会调用到通用的设备/驱
 * 动匹配函数的 __device_attach , 当然如果注册驱动时，会调用到 __driver_attach
 * ，这两个函数中会调用 bus_type 中的 match 函数，对于spi就是 spi_match_device ,
 * 这里面就会使用设备树/id_table/driver.name 进行匹配，也就是板级信息中的 modalias 
 * 和 驱动中的 m25p_ids 或者 driver.name 匹配，匹配成功后就会调用 probe 函数
 *
 * 匹配设备/驱动时，板级信息中只会使用 spi_board_info 结构中的内容，并不会使用到
 * platform_data 指向的内容，这个内容是用来区分具体型号的，会在 probe 中进行解析
 *
 * */

/*
 * board specific setup should have ensured the SPI clock used here
 * matches what the READ command supports, at least until this driver
 * understands FAST_READ (for clocks over 25 MHz).
 */
static int __devinit m25p_probe(struct spi_device *spi)
{
    /* spi_get_device_id 用来匹配 id_table 和 dev->modalias 的，这里实际可以不用
     * 匹配的。 
     * 但如果在 spi_board_info 中指定的 platform_data 中并没有指定 type ，这里就
     * 有用了，相当于在 spi_board_info 中直接指定了具体的型号 */
	const struct spi_device_id	*id = spi_get_device_id(spi);
	struct flash_platform_data	*data;
	struct m25p			*flash;
	struct flash_info		*info;
	unsigned			i;
	struct mtd_part_parser_data	ppdata;

#ifdef CONFIG_MTD_OF_PARTS
	if (!of_device_is_available(spi->dev.of_node))
		return -ENODEV;
#endif

	/* Platform data helps sort out which chip type we have, as
	 * well as how this board partitions it.  If we don't have
	 * a chip ID, try the JEDEC id commands; they'll work for most
	 * newer chips, even if we don't recognize the particular chip.
	 */
    /* spi-dev.platform_data 在注册spi设备时被配置为 spi_board_info.platform_data 
     * 所以这里就可以获取具体的 flash型号和分区信息了
     * 这里可以看出，如果没有定义 platform_data 或者 type ，那就直接使用上面获取
     * 到的 id 了
     * 这里就是根据 platform_data 中的 type 查找具体的型号，这样就可以获取到
     * flash的具体信息了 */ 
	data = spi->dev.platform_data;
	if (data && data->type) {
		const struct spi_device_id *plat_id;

		for (i = 0; i < ARRAY_SIZE(m25p_ids) - 1; i++) {
			plat_id = &m25p_ids[i];
			if (strcmp(data->type, plat_id->name))
				continue;
			break;
		}

		if (i < ARRAY_SIZE(m25p_ids) - 1)
			id = plat_id;
		else
			dev_warn(&spi->dev, "unrecognized id %s\n", data->type);
	}

    /* id->driver_data 就是 m25p_ids ，flash的具体信息 */
	info = (void *)id->driver_data;

    /* jedec 标准，具有 jedec_id 的符合该标准 
     * jedec 是一个通用的标准，符合该标准的flash，可以通过使用一些通用命令字获取
     * flash的状态信息，这里就是通过读 ID 再次确认具体的flash型号 */
	if (info->jedec_id) {
		const struct spi_device_id *jid;

        /* 虽然前面根据 platform_data.type 确认了flash型号，
         * 但这里又根据 jedec 再次确认了一次，因为相比板级信息中提供的，
         * 通过 jedec 获取的更加准确，更能匹配到真正的信息
         * 如果两者不同，则以 jedec 获取到的为准 */
		jid = jedec_probe(spi);
		if (IS_ERR(jid)) {
			return PTR_ERR(jid);
		} else if (jid != id) {
			/*
			 * JEDEC knows better, so overwrite platform ID. We
			 * can't trust partitions any longer, but we'll let
			 * mtd apply them anyway, since some partitions may be
			 * marked read-only, and we don't want to lose that
			 * information, even if it's not 100% accurate.
			 */
			dev_warn(&spi->dev, "found %s, expected %s\n",
				 jid->name, id->name);
			id = jid;
			info = (void *)jid->driver_data;
		}
	}

    /* 至此，已经确认了所使用的 flash型号信息，
     * id 指向 m25p_ids 表中对应的 flash信息， 
     * data 指向板级信息中的 platform_data ，也就是上面的 xxx_spi_flash_data
     * info 指向 m25p_ids 表中对应的 flash具体信息 (flash_info) 
     * */

    /* flash 类型是 struct m25p 
     * 这个结构是本文件中用来描述所使用的flash的所有信息的, 
     * mtd子系统调用到本文件时都是传入的 mtd_info 结构，可以通过 mtd_info
     * 反向找到 struct m25p 结构，这样就能对应到具体的 flash了 
     * command 是一个命令缓冲区，操作flash时，先把命令放到这里面 */
	flash = kzalloc(sizeof *flash, GFP_KERNEL);
	if (!flash)
		return -ENOMEM;
	flash->command = kmalloc(MAX_CMD_SIZE + FAST_READ_DUMMY_BYTE, GFP_KERNEL);
	if (!flash->command) {
		kfree(flash);
		return -ENOMEM;
	}

    /* 指向 spi 设备，这样可以找到一些数据结构 
     * 再把 flash 结构给 dev 私有数据，这样可以互相找到 */
	flash->spi = spi;
	mutex_init(&flash->lock);
	dev_set_drvdata(&spi->dev, flash);

	/*
	 * Atmel, SST and Intel/Numonyx serial flash tend to power
	 * up with the software protection bits set
	 */
    /* 某些flash型号的特殊处理，无需关注  */
	if (JEDEC_MFR(info->jedec_id) == CFI_MFR_ATMEL ||
	    JEDEC_MFR(info->jedec_id) == CFI_MFR_INTEL ||
	    JEDEC_MFR(info->jedec_id) == CFI_MFR_SST) {
		write_enable(flash);
		write_sr(flash, 0);
	}

    /* 设置 mtd 设备的名字，按照 spi_board_info platform_data 中的 name 
     * 或者 spi dev 的名字 */
	if (data && data->name)
		flash->mtd.name = data->name;
	else
		flash->mtd.name = dev_name(&spi->dev);

    /* 组织 mtd_info 结构，用于注册mtd原始设备
     * 以 m25p64 为例，是 nor flash 可以写单个字节 
     * 每个扇区 64KiB， 总共128个扇区 总大小 64KiB * 128 
     * 另外再提供各个操作函数 */
	flash->mtd.type = MTD_NORFLASH;
	flash->mtd.writesize = 1;
	flash->mtd.flags = MTD_CAP_NORFLASH;
	flash->mtd.size = info->sector_size * info->n_sectors;
	flash->mtd._erase = m25p80_erase;
	flash->mtd._read = m25p80_read;

	/* sst flash chips use AAI word program */
	if (JEDEC_MFR(info->jedec_id) == CFI_MFR_SST)
		flash->mtd._write = sst_write;
	else
		flash->mtd._write = m25p80_write;

    /* 是否支持4K擦除，某些芯片支持4K扇区擦除  */
	/* prefer "small sector" erase if possible */
	if (info->flags & SECT_4K) {
		flash->erase_opcode = OPCODE_BE_4K;
		flash->mtd.erasesize = 4096;
	} else {
		flash->erase_opcode = OPCODE_SE;
		flash->mtd.erasesize = info->sector_size;
	}

    /* 无需擦除的flash使用
     * 如上面的 cat25c11 系列，它们实际是 eeprom，无需擦除 */
	if (info->flags & M25P_NO_ERASE)
		flash->mtd.flags |= MTD_NO_ERASE;

    /* 没有使用设备树，这里 ppdata.of_node = NULL */
	ppdata.of_node = spi->dev.of_node;
	flash->mtd.dev.parent = &spi->dev;
    /* page_size 被固定写为 256 了 */
	flash->page_size = info->page_size;
	flash->mtd.writebufsize = flash->page_size;

    /* 使用 INFO 定义的 flash类型， addr_width = 0
     * 对于总大小超过 16MiB 的，需要使用 4字节地址，其余 3字节地址
     * 需要注意，这里设置4字节地址的函数 set_4byte 是有一定局限性的
     * 该驱动比较早，很多flash 器件并未适配 
     * 如 Winbond 的 w25q256 型号，是支持4字节模式的，但它的id是 EF4019 
     * 但是它是支持 OPCODE_EN4B 和 OPCODE_EX4B 指令的，但本驱动中原始的代码是无
     * 法支持的 */
	if (info->addr_width)
		flash->addr_width = info->addr_width;
	else {
		/* enable 4-byte addressing if the device exceeds 16MiB */
		if (flash->mtd.size > 0x1000000) {
			flash->addr_width = 4;
			set_4byte(flash, info->jedec_id, 1);
		} else
			flash->addr_width = 3;
	}

	dev_info(&spi->dev, "%s (%lld Kbytes)\n", id->name,
			(long long)flash->mtd.size >> 10);

	pr_debug("mtd .name = %s, .size = 0x%llx (%lldMiB) "
			".erasesize = 0x%.8x (%uKiB) .numeraseregions = %d\n",
		flash->mtd.name,
		(long long)flash->mtd.size, (long long)(flash->mtd.size >> 20),
		flash->mtd.erasesize, flash->mtd.erasesize / 1024,
		flash->mtd.numeraseregions);

	if (flash->mtd.numeraseregions)
		for (i = 0; i < flash->mtd.numeraseregions; i++)
			pr_debug("mtd.eraseregions[%d] = { .offset = 0x%llx, "
				".erasesize = 0x%.8x (%uKiB), "
				".numblocks = %d }\n",
				i, (long long)flash->mtd.eraseregions[i].offset,
				flash->mtd.eraseregions[i].erasesize,
				flash->mtd.eraseregions[i].erasesize / 1024,
				flash->mtd.eraseregions[i].numblocks);


	/* partitions should match sector boundaries; and it may be good to
	 * use readonly partitions for writeprotected sectors (BP2..BP0).
	 */
    /* 提供 mtd 设备及分区信息进行mtd设备注册
     * 两种分区提供方式: ppdata(设备树方式)， parts/nr_parts 板级配置信息 */
	return mtd_device_parse_register(&flash->mtd, NULL, &ppdata,
			data ? data->parts : NULL,
			data ? data->nr_parts : 0);
}


/* 移除 m25p 驱动，只需要把所有注册的 mtd 设备卸载即可 */
static int __devexit m25p_remove(struct spi_device *spi)
{
	struct m25p	*flash = dev_get_drvdata(&spi->dev);
	int		status;

	/* Clean up MTD stuff. */
	status = mtd_device_unregister(&flash->mtd);
	if (status == 0) {
		kfree(flash->command);
		kfree(flash);
	}
	return 0;
}


static struct spi_driver m25p80_driver = {
	.driver = {
		.name	= "m25p80",
		.owner	= THIS_MODULE,
	},
	.id_table	= m25p_ids,
	.probe	= m25p_probe,
	.remove	= __devexit_p(m25p_remove),

	/* REVISIT: many of these chips have deep power-down modes, which
	 * should clearly be entered on suspend() to minimize power use.
	 * And also when they're otherwise idle...
	 */
};

module_spi_driver(m25p80_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Mike Lavender");
MODULE_DESCRIPTION("MTD SPI driver for ST M25Pxx flash chips");
