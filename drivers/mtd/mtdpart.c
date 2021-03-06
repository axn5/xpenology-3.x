/*
 * Simple MTD partitioning layer
 *
 * Copyright © 2000 Nicolas Pitre <nico@fluxnic.net>
 * Copyright © 2002 Thomas Gleixner <gleixner@linutronix.de>
 * Copyright © 2000-2010 David Woodhouse <dwmw2@infradead.org>
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
#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/list.h>
#include <linux/kmod.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/partitions.h>
#if defined(CONFIG_SYNO_COMCERTO)
#include <linux/root_dev.h>
#include <linux/magic.h>
#endif
#include <linux/err.h>
#ifdef MY_ABC_HERE
#include <linux/rtnetlink.h>
#include <linux/netdevice.h>
#include <linux/if_arp.h>
#endif

#ifdef MY_ABC_HERE
extern unsigned char grgbLanMac[4][16];
#endif

#ifdef MY_ABC_HERE
extern char gszSerialNum[];
extern char gszCustomSerialNum[];
#define SYNO_SN_TAG "SN="
#define SYNO_CHKSUM_TAG "CHK="
#define SYNO_SN_12_SIG SYNO_SN_TAG  // signature for 12 serial number
#endif

#include "mtdcore.h"

#if defined(CONFIG_SYNO_COMCERTO)
#define MTD_ERASE_PARTIAL	0x8000 /* partition only covers parts of an erase block */
#endif

/* Our partition linked list */
static LIST_HEAD(mtd_partitions);
static DEFINE_MUTEX(mtd_partitions_mutex);

/* Our partition node structure */
struct mtd_part {
	struct mtd_info mtd;
	struct mtd_info *master;
	uint64_t offset;
	struct list_head list;
};

/*
 * Given a pointer to the MTD object in the mtd_part structure, we can retrieve
 * the pointer to that structure with this macro.
 */
#define PART(x)  ((struct mtd_part *)(x))
#if defined(CONFIG_SYNO_COMCERTO)
#define IS_PART(mtd) (mtd->read == part_read)
#endif

/*
 * MTD methods which simply translate the effective address and pass through
 * to the _real_ device.
 */

static int part_read(struct mtd_info *mtd, loff_t from, size_t len,
		size_t *retlen, u_char *buf)
{
	struct mtd_part *part = PART(mtd);
	struct mtd_ecc_stats stats;
	int res;

	stats = part->master->ecc_stats;

	if (from >= mtd->size)
		len = 0;
	else if (from + len > mtd->size)
		len = mtd->size - from;
	res = part->master->read(part->master, from + part->offset,
				   len, retlen, buf);
	if (unlikely(res)) {
		if (mtd_is_bitflip(res))
			mtd->ecc_stats.corrected += part->master->ecc_stats.corrected - stats.corrected;
		if (mtd_is_eccerr(res))
			mtd->ecc_stats.failed += part->master->ecc_stats.failed - stats.failed;
	}
	return res;
}

static int part_point(struct mtd_info *mtd, loff_t from, size_t len,
		size_t *retlen, void **virt, resource_size_t *phys)
{
	struct mtd_part *part = PART(mtd);
	if (from >= mtd->size)
		len = 0;
	else if (from + len > mtd->size)
		len = mtd->size - from;
	return part->master->point (part->master, from + part->offset,
				    len, retlen, virt, phys);
}

static void part_unpoint(struct mtd_info *mtd, loff_t from, size_t len)
{
	struct mtd_part *part = PART(mtd);

	part->master->unpoint(part->master, from + part->offset, len);
}

static unsigned long part_get_unmapped_area(struct mtd_info *mtd,
					    unsigned long len,
					    unsigned long offset,
					    unsigned long flags)
{
	struct mtd_part *part = PART(mtd);

	offset += part->offset;
	return part->master->get_unmapped_area(part->master, len, offset,
					       flags);
}

static int part_read_oob(struct mtd_info *mtd, loff_t from,
		struct mtd_oob_ops *ops)
{
	struct mtd_part *part = PART(mtd);
	int res;

	if (from >= mtd->size)
		return -EINVAL;
	if (ops->datbuf && from + ops->len > mtd->size)
		return -EINVAL;

	/*
	 * If OOB is also requested, make sure that we do not read past the end
	 * of this partition.
	 */
	if (ops->oobbuf) {
		size_t len, pages;

		if (ops->mode == MTD_OPS_AUTO_OOB)
			len = mtd->oobavail;
		else
			len = mtd->oobsize;
		pages = mtd_div_by_ws(mtd->size, mtd);
		pages -= mtd_div_by_ws(from, mtd);
		if (ops->ooboffs + ops->ooblen > pages * len)
			return -EINVAL;
	}

	res = part->master->read_oob(part->master, from + part->offset, ops);
	if (unlikely(res)) {
		if (mtd_is_bitflip(res))
			mtd->ecc_stats.corrected++;
		if (mtd_is_eccerr(res))
			mtd->ecc_stats.failed++;
	}
	return res;
}

static int part_read_user_prot_reg(struct mtd_info *mtd, loff_t from,
		size_t len, size_t *retlen, u_char *buf)
{
	struct mtd_part *part = PART(mtd);
	return part->master->read_user_prot_reg(part->master, from,
					len, retlen, buf);
}

static int part_get_user_prot_info(struct mtd_info *mtd,
		struct otp_info *buf, size_t len)
{
	struct mtd_part *part = PART(mtd);
	return part->master->get_user_prot_info(part->master, buf, len);
}

static int part_read_fact_prot_reg(struct mtd_info *mtd, loff_t from,
		size_t len, size_t *retlen, u_char *buf)
{
	struct mtd_part *part = PART(mtd);
	return part->master->read_fact_prot_reg(part->master, from,
					len, retlen, buf);
}

static int part_get_fact_prot_info(struct mtd_info *mtd, struct otp_info *buf,
		size_t len)
{
	struct mtd_part *part = PART(mtd);
	return part->master->get_fact_prot_info(part->master, buf, len);
}

static int part_write(struct mtd_info *mtd, loff_t to, size_t len,
		size_t *retlen, const u_char *buf)
{
	struct mtd_part *part = PART(mtd);
	if (!(mtd->flags & MTD_WRITEABLE))
		return -EROFS;
	if (to >= mtd->size)
		len = 0;
	else if (to + len > mtd->size)
		len = mtd->size - to;
	return part->master->write(part->master, to + part->offset,
				    len, retlen, buf);
}

static int part_panic_write(struct mtd_info *mtd, loff_t to, size_t len,
		size_t *retlen, const u_char *buf)
{
	struct mtd_part *part = PART(mtd);
	if (!(mtd->flags & MTD_WRITEABLE))
		return -EROFS;
	if (to >= mtd->size)
		len = 0;
	else if (to + len > mtd->size)
		len = mtd->size - to;
	return part->master->panic_write(part->master, to + part->offset,
				    len, retlen, buf);
}

static int part_write_oob(struct mtd_info *mtd, loff_t to,
		struct mtd_oob_ops *ops)
{
	struct mtd_part *part = PART(mtd);

	if (!(mtd->flags & MTD_WRITEABLE))
		return -EROFS;

	if (to >= mtd->size)
		return -EINVAL;
	if (ops->datbuf && to + ops->len > mtd->size)
		return -EINVAL;
	return part->master->write_oob(part->master, to + part->offset, ops);
}

static int part_write_user_prot_reg(struct mtd_info *mtd, loff_t from,
		size_t len, size_t *retlen, u_char *buf)
{
	struct mtd_part *part = PART(mtd);
	return part->master->write_user_prot_reg(part->master, from,
					len, retlen, buf);
}

static int part_lock_user_prot_reg(struct mtd_info *mtd, loff_t from,
		size_t len)
{
	struct mtd_part *part = PART(mtd);
	return part->master->lock_user_prot_reg(part->master, from, len);
}

static int part_writev(struct mtd_info *mtd, const struct kvec *vecs,
		unsigned long count, loff_t to, size_t *retlen)
{
	struct mtd_part *part = PART(mtd);
	if (!(mtd->flags & MTD_WRITEABLE))
		return -EROFS;
	return part->master->writev(part->master, vecs, count,
					to + part->offset, retlen);
}

static int part_erase(struct mtd_info *mtd, struct erase_info *instr)
{
	struct mtd_part *part = PART(mtd);
	int ret;
	if (!(mtd->flags & MTD_WRITEABLE))
		return -EROFS;
	if (instr->addr >= mtd->size)
		return -EINVAL;

#if defined(CONFIG_SYNO_COMCERTO)
	instr->partial_start = false;
	if (mtd->flags & MTD_ERASE_PARTIAL) {
		size_t readlen = 0;
		u64 mtd_ofs;

		instr->erase_buf = kmalloc(part->master->erasesize, GFP_ATOMIC);
		if (!instr->erase_buf)
			return -ENOMEM;

		mtd_ofs = part->offset + instr->addr;
		instr->erase_buf_ofs = do_div(mtd_ofs, part->master->erasesize);

		if (instr->erase_buf_ofs > 0) {
			instr->addr -= instr->erase_buf_ofs;
			ret = part->master->read(part->master,
				instr->addr + part->offset,
				part->master->erasesize,
				&readlen, instr->erase_buf);

			instr->partial_start = true;
		} else {
			mtd_ofs = part->offset + part->mtd.size;
			instr->erase_buf_ofs = part->master->erasesize -
				do_div(mtd_ofs, part->master->erasesize);

			if (instr->erase_buf_ofs > 0) {
				instr->len += instr->erase_buf_ofs;
				ret = part->master->read(part->master,
					part->offset + instr->addr +
					instr->len - part->master->erasesize,
					part->master->erasesize, &readlen,
					instr->erase_buf);
			} else {
				ret = 0;
			}
		}
		if (ret < 0) {
			kfree(instr->erase_buf);
			return ret;
		}

	}
#endif

	instr->addr += part->offset;
	ret = part->master->erase(part->master, instr);
	if (ret) {
		if (instr->fail_addr != MTD_FAIL_ADDR_UNKNOWN)
			instr->fail_addr -= part->offset;
		instr->addr -= part->offset;
#if defined(CONFIG_SYNO_COMCERTO)
		if (mtd->flags & MTD_ERASE_PARTIAL)
			kfree(instr->erase_buf);
#endif
	}
	return ret;
}

void mtd_erase_callback(struct erase_info *instr)
{
	if (instr->mtd->erase == part_erase) {
		struct mtd_part *part = PART(instr->mtd);
#if defined(CONFIG_SYNO_COMCERTO)
		size_t wrlen = 0;
#endif

#if defined(CONFIG_SYNO_COMCERTO)
		if (instr->mtd->flags & MTD_ERASE_PARTIAL) {
			if (instr->partial_start) {
				part->master->write(part->master,
					instr->addr, instr->erase_buf_ofs,
					&wrlen, instr->erase_buf);
				instr->addr += instr->erase_buf_ofs;
			} else {
				instr->len -= instr->erase_buf_ofs;
				part->master->write(part->master,
					instr->addr + instr->len,
					instr->erase_buf_ofs, &wrlen,
					instr->erase_buf +
					part->master->erasesize -
					instr->erase_buf_ofs);
			}
			kfree(instr->erase_buf);
		}
#endif
		if (instr->fail_addr != MTD_FAIL_ADDR_UNKNOWN)
			instr->fail_addr -= part->offset;
		instr->addr -= part->offset;
	}
	if (instr->callback)
		instr->callback(instr);
}
EXPORT_SYMBOL_GPL(mtd_erase_callback);

static int part_lock(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	struct mtd_part *part = PART(mtd);
	if ((len + ofs) > mtd->size)
		return -EINVAL;
	return part->master->lock(part->master, ofs + part->offset, len);
}

static int part_unlock(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	struct mtd_part *part = PART(mtd);
	if ((len + ofs) > mtd->size)
		return -EINVAL;
	return part->master->unlock(part->master, ofs + part->offset, len);
}

static int part_is_locked(struct mtd_info *mtd, loff_t ofs, uint64_t len)
{
	struct mtd_part *part = PART(mtd);
	if ((len + ofs) > mtd->size)
		return -EINVAL;
	return part->master->is_locked(part->master, ofs + part->offset, len);
}

static void part_sync(struct mtd_info *mtd)
{
	struct mtd_part *part = PART(mtd);
	part->master->sync(part->master);
}

static int part_suspend(struct mtd_info *mtd)
{
	struct mtd_part *part = PART(mtd);
	return part->master->suspend(part->master);
}

static void part_resume(struct mtd_info *mtd)
{
	struct mtd_part *part = PART(mtd);
	part->master->resume(part->master);
}

static int part_block_isbad(struct mtd_info *mtd, loff_t ofs)
{
	struct mtd_part *part = PART(mtd);
	if (ofs >= mtd->size)
		return -EINVAL;
	ofs += part->offset;
	return part->master->block_isbad(part->master, ofs);
}

static int part_block_markbad(struct mtd_info *mtd, loff_t ofs)
{
	struct mtd_part *part = PART(mtd);
	int res;

	if (!(mtd->flags & MTD_WRITEABLE))
		return -EROFS;
	if (ofs >= mtd->size)
		return -EINVAL;
	ofs += part->offset;
	res = part->master->block_markbad(part->master, ofs);
	if (!res)
		mtd->ecc_stats.badblocks++;
	return res;
}

static inline void free_partition(struct mtd_part *p)
{
	kfree(p->mtd.name);
	kfree(p);
}

/*
 * This function unregisters and destroy all slave MTD objects which are
 * attached to the given master MTD object.
 */

int del_mtd_partitions(struct mtd_info *master)
{
	struct mtd_part *slave, *next;
	int ret, err = 0;

	mutex_lock(&mtd_partitions_mutex);
	list_for_each_entry_safe(slave, next, &mtd_partitions, list)
		if (slave->master == master) {
			ret = del_mtd_device(&slave->mtd);
			if (ret < 0) {
				err = ret;
				continue;
			}
			list_del(&slave->list);
			free_partition(slave);
		}
	mutex_unlock(&mtd_partitions_mutex);

	return err;
}

static struct mtd_part *allocate_partition(struct mtd_info *master,
			const struct mtd_partition *part, int partno,
			uint64_t cur_offset)
{
	struct mtd_part *slave;
	char *name;

	/* allocate the partition structure */
	slave = kzalloc(sizeof(*slave), GFP_KERNEL);
	name = kstrdup(part->name, GFP_KERNEL);
	if (!name || !slave) {
		printk(KERN_ERR"memory allocation error while creating partitions for \"%s\"\n",
		       master->name);
		kfree(name);
		kfree(slave);
		return ERR_PTR(-ENOMEM);
	}

	/* set up the MTD object for this partition */
	slave->mtd.type = master->type;
	slave->mtd.flags = master->flags & ~part->mask_flags;
	slave->mtd.size = part->size;
	slave->mtd.writesize = master->writesize;
	slave->mtd.writebufsize = master->writebufsize;
	slave->mtd.oobsize = master->oobsize;
	slave->mtd.oobavail = master->oobavail;
	slave->mtd.subpage_sft = master->subpage_sft;

	slave->mtd.name = name;
	slave->mtd.owner = master->owner;
	slave->mtd.backing_dev_info = master->backing_dev_info;

	/* NOTE:  we don't arrange MTDs as a tree; it'd be error-prone
	 * to have the same data be in two different partitions.
	 */
	slave->mtd.dev.parent = master->dev.parent;

	slave->mtd.read = part_read;
	slave->mtd.write = part_write;

	if (master->panic_write)
		slave->mtd.panic_write = part_panic_write;

	if (master->point && master->unpoint) {
		slave->mtd.point = part_point;
		slave->mtd.unpoint = part_unpoint;
	}

	if (master->get_unmapped_area)
		slave->mtd.get_unmapped_area = part_get_unmapped_area;
	if (master->read_oob)
		slave->mtd.read_oob = part_read_oob;
	if (master->write_oob)
		slave->mtd.write_oob = part_write_oob;
	if (master->read_user_prot_reg)
		slave->mtd.read_user_prot_reg = part_read_user_prot_reg;
	if (master->read_fact_prot_reg)
		slave->mtd.read_fact_prot_reg = part_read_fact_prot_reg;
	if (master->write_user_prot_reg)
		slave->mtd.write_user_prot_reg = part_write_user_prot_reg;
	if (master->lock_user_prot_reg)
		slave->mtd.lock_user_prot_reg = part_lock_user_prot_reg;
	if (master->get_user_prot_info)
		slave->mtd.get_user_prot_info = part_get_user_prot_info;
	if (master->get_fact_prot_info)
		slave->mtd.get_fact_prot_info = part_get_fact_prot_info;
	if (master->sync)
		slave->mtd.sync = part_sync;
	if (!partno && !master->dev.class && master->suspend && master->resume) {
			slave->mtd.suspend = part_suspend;
			slave->mtd.resume = part_resume;
	}
	if (master->writev)
		slave->mtd.writev = part_writev;
	if (master->lock)
		slave->mtd.lock = part_lock;
	if (master->unlock)
		slave->mtd.unlock = part_unlock;
	if (master->is_locked)
		slave->mtd.is_locked = part_is_locked;
	if (master->block_isbad)
		slave->mtd.block_isbad = part_block_isbad;
	if (master->block_markbad)
		slave->mtd.block_markbad = part_block_markbad;
	slave->mtd.erase = part_erase;
	slave->master = master;
	slave->offset = part->offset;

	if (slave->offset == MTDPART_OFS_APPEND)
		slave->offset = cur_offset;
	if (slave->offset == MTDPART_OFS_NXTBLK) {
		slave->offset = cur_offset;
		if (mtd_mod_by_eb(cur_offset, master) != 0) {
			/* Round up to next erasesize */
			slave->offset = (mtd_div_by_eb(cur_offset, master) + 1) * master->erasesize;
			printk(KERN_NOTICE "Moving partition %d: "
			       "0x%012llx -> 0x%012llx\n", partno,
			       (unsigned long long)cur_offset, (unsigned long long)slave->offset);
		}
	}
	if (slave->offset == MTDPART_OFS_RETAIN) {
		slave->offset = cur_offset;
		if (master->size - slave->offset >= slave->mtd.size) {
			slave->mtd.size = master->size - slave->offset
							- slave->mtd.size;
		} else {
			printk(KERN_ERR "mtd partition \"%s\" doesn't have enough space: %#llx < %#llx, disabled\n",
				part->name, master->size - slave->offset,
				slave->mtd.size);
			/* register to preserve ordering */
			goto out_register;
		}
	}
	if (slave->mtd.size == MTDPART_SIZ_FULL)
		slave->mtd.size = master->size - slave->offset;

	printk(KERN_NOTICE "0x%012llx-0x%012llx : \"%s\"\n", (unsigned long long)slave->offset,
		(unsigned long long)(slave->offset + slave->mtd.size), slave->mtd.name);

	/* let's do some sanity checks */
	if (slave->offset >= master->size) {
		/* let's register it anyway to preserve ordering */
		slave->offset = 0;
		slave->mtd.size = 0;
		printk(KERN_ERR"mtd: partition \"%s\" is out of reach -- disabled\n",
			part->name);
		goto out_register;
	}
	if (slave->offset + slave->mtd.size > master->size) {
		slave->mtd.size = master->size - slave->offset;
		printk(KERN_WARNING"mtd: partition \"%s\" extends beyond the end of device \"%s\" -- size truncated to %#llx\n",
			part->name, master->name, (unsigned long long)slave->mtd.size);
	}
	if (master->numeraseregions > 1) {
		/* Deal with variable erase size stuff */
		int i, max = master->numeraseregions;
		u64 end = slave->offset + slave->mtd.size;
		struct mtd_erase_region_info *regions = master->eraseregions;

		/* Find the first erase regions which is part of this
		 * partition. */
		for (i = 0; i < max && regions[i].offset <= slave->offset; i++)
			;
		/* The loop searched for the region _behind_ the first one */
		if (i > 0)
			i--;

		/* Pick biggest erasesize */
		for (; i < max && regions[i].offset < end; i++) {
			if (slave->mtd.erasesize < regions[i].erasesize) {
				slave->mtd.erasesize = regions[i].erasesize;
			}
		}
		BUG_ON(slave->mtd.erasesize == 0);
	} else {
		/* Single erase size */
		slave->mtd.erasesize = master->erasesize;
	}

	if ((slave->mtd.flags & MTD_WRITEABLE) &&
	    mtd_mod_by_eb(slave->offset, &slave->mtd)) {
		/* Doesn't start on a boundary of major erase size */
#if defined(CONFIG_SYNO_COMCERTO)
		slave->mtd.flags |= MTD_ERASE_PARTIAL;
		if (((u32) slave->mtd.size) > master->erasesize)
			slave->mtd.flags &= ~MTD_WRITEABLE;
		else
			slave->mtd.erasesize = slave->mtd.size;
#else
		/* FIXME: Let it be writable if it is on a boundary of
		 * _minor_ erase size though */
		slave->mtd.flags &= ~MTD_WRITEABLE;
		printk(KERN_WARNING"mtd: partition \"%s\" doesn't start on an erase block boundary -- force read-only\n",
			part->name);
#endif
	}
	if ((slave->mtd.flags & MTD_WRITEABLE) &&
#if defined(CONFIG_SYNO_COMCERTO)
	    mtd_mod_by_eb(slave->offset + slave->mtd.size, &slave->mtd)) {
		slave->mtd.flags |= MTD_ERASE_PARTIAL;

		if ((u32) slave->mtd.size > master->erasesize)
			slave->mtd.flags &= ~MTD_WRITEABLE;
		else
			slave->mtd.erasesize = slave->mtd.size;
#else
	    mtd_mod_by_eb(slave->mtd.size, &slave->mtd)) {
		slave->mtd.flags &= ~MTD_WRITEABLE;
		printk(KERN_WARNING"mtd: partition \"%s\" doesn't end on an erase block -- force read-only\n",
			part->name);
#endif
	}
#if defined(CONFIG_SYNO_COMCERTO)
	if ((slave->mtd.flags & (MTD_ERASE_PARTIAL|MTD_WRITEABLE)) == MTD_ERASE_PARTIAL)
		printk(KERN_WARNING"mtd: partition \"%s\" must either start or end on erase block boundary or be smaller than an erase block -- forcing read-only\n",
				part->name);
#endif

	slave->mtd.ecclayout = master->ecclayout;
	if (master->block_isbad) {
		uint64_t offs = 0;

		while (offs < slave->mtd.size) {
			if (master->block_isbad(master,
						offs + slave->offset))
				slave->mtd.ecc_stats.badblocks++;
			offs += slave->mtd.erasesize;
		}
	}

out_register:

#if defined(MY_ABC_HERE) 
	if ((memcmp(part->name, "vender", 7)==0) ||
		(memcmp(part->name, "vendor", 7)==0)) {
			int gVenderMacNumber = 0;

			u_char rgbszBuf[128];
			size_t retlen;
			int i, n, x;
			unsigned int Sum;
			u_char ucSum;
			char rgbLanMac[4][6];

			part_read(&slave->mtd, 0, 128, &retlen, rgbszBuf);
#ifdef MY_ABC_HERE
			x = 0;
			gVenderMacNumber = 0;
			for (n = 0; n<4; n++) {
				for (Sum=0,ucSum=0,i=0; i<6; i++) {
					Sum+=rgbszBuf[i+x];
					ucSum+=rgbszBuf[i+x];
					rgbLanMac[n][i] = rgbszBuf[i+x];
				}
				x+=6;

				if (Sum==0 || ucSum!=rgbszBuf[x]) {
					printk("vender Mac%d checksum error ucSum:0x%02x Buf:0x%02x Sum:%d.\n", 
							n, ucSum, rgbszBuf[x], Sum);
					grgbLanMac[n][0] = '\0';
				} else {
					snprintf(grgbLanMac[n], sizeof(grgbLanMac),
							"%02x%02x%02x%02x%02x%02x",
					rgbLanMac[n][0],
					rgbLanMac[n][1],
					rgbLanMac[n][2],
					rgbLanMac[n][3],
					rgbLanMac[n][4],
					rgbLanMac[n][5]);
				}

				x++;
				gVenderMacNumber++;
			}
#endif

#ifdef MY_ABC_HERE
			char szSerialBuffer[32];
			char *ptr;
			char szSerial[32];
			char szCheckSum[32];
			unsigned int uichksum = 0;
			unsigned int uiTemp = 0;

			memset(szSerial, 0, sizeof(szSerial));
			memset(szCheckSum, 0, sizeof(szCheckSum));
			memset(gszSerialNum, 0, 32);
			memcpy(szSerialBuffer, &(rgbszBuf[32]), 32);

			// this is new defined SN
			if (0 == strncmp(szSerialBuffer, SYNO_SN_12_SIG,strlen(SYNO_SN_12_SIG))) {
				//paring serial number with format 'SN=1350KKN99999'
				ptr = strstr(szSerialBuffer, SYNO_SN_TAG);
				if (NULL == ptr) {
					printk("no serial tag found, serial buffer='%s'\n", szSerialBuffer);
					goto SKIP_SERIAL;
				}
				ptr += strlen(SYNO_SN_TAG);
				i = 0;
				while (0 != *ptr && ',' != *ptr) {
					szSerial[i++] = *ptr;
					ptr++;
				}
				szSerial[i] = '\0';

				//paring serial number with format 'CHK=125'
				ptr = strstr(szSerialBuffer, SYNO_CHKSUM_TAG);
				if (NULL == ptr) {
					printk("no checksum tag found, serial buffer='%s'\n", szSerialBuffer);
					goto SKIP_SERIAL;
				}
				ptr += strlen(SYNO_CHKSUM_TAG);
				i = 0;
				while (0 != *ptr && ',' != *ptr) {
					szCheckSum[i++] = *ptr;
					ptr++;
				}
				szCheckSum[i] = '\0';

				//calculate checksum
				for (i = 0 ; i < strlen(szSerial); i++) {
					uichksum += szSerial[i];
				}

				//------ check checksum ------
				if (0 != strict_strtoul(szCheckSum, 10, &uiTemp)) {
					printk("string conversion error: '%s'\n", szCheckSum);
					goto SKIP_SERIAL;
				} else if (uichksum != uiTemp) {
					printk("serial number checksum error, serial='%s', checksum='%u' not '%u'\n", szSerial, uichksum, uiTemp);
					goto SKIP_SERIAL;
				}
			} else {
				unsigned char ucChkSum = 0;
				//calculate checksum
				for (i = 0 ; i < 10; i++) {
					ucChkSum += szSerialBuffer[i];
				}
				//------ check checksum ------
				if (ucChkSum != szSerialBuffer[10]) {
					printk("serial number checksum error, serial='%s', checksum='%d' not '%d'", szSerialBuffer, ucChkSum, szSerialBuffer[10]);
					goto SKIP_SERIAL;
				} else {
					memcpy(szSerial, szSerialBuffer, 10);
				}
			}
			snprintf(gszSerialNum, 32, "%s", szSerial);
SKIP_SERIAL:
			printk("serial number='%s'", gszSerialNum);

			//read custom serial number out, it is in the vender partion shift 64 bytes.
			x = 64;
			for (Sum=0,ucSum=0,i=0; i<31; i++) {
				Sum+=rgbszBuf[i+x];
				ucSum+=rgbszBuf[i+x];
				gszCustomSerialNum[i] = rgbszBuf[i+x];
			}
			x+=31;
			if (Sum==0 || ucSum!=rgbszBuf[x]) {
				for (i=0; i<32; i++) {
					gszCustomSerialNum[i] = 0;
				}
			} else {
				printk("Custom Serial Number: %s\n", gszCustomSerialNum);
			}
#endif
        }
#endif

	return slave;
}

int mtd_add_partition(struct mtd_info *master, char *name,
		      long long offset, long long length)
{
	struct mtd_partition part;
	struct mtd_part *p, *new;
	uint64_t start, end;
	int ret = 0;

	/* the direct offset is expected */
	if (offset == MTDPART_OFS_APPEND ||
	    offset == MTDPART_OFS_NXTBLK)
		return -EINVAL;

	if (length == MTDPART_SIZ_FULL)
		length = master->size - offset;

	if (length <= 0)
		return -EINVAL;

	part.name = name;
	part.size = length;
	part.offset = offset;
	part.mask_flags = 0;
	part.ecclayout = NULL;

	new = allocate_partition(master, &part, -1, offset);
	if (IS_ERR(new))
		return PTR_ERR(new);

	start = offset;
	end = offset + length;

	mutex_lock(&mtd_partitions_mutex);
	list_for_each_entry(p, &mtd_partitions, list)
		if (p->master == master) {
			if ((start >= p->offset) &&
			    (start < (p->offset + p->mtd.size)))
				goto err_inv;

			if ((end >= p->offset) &&
			    (end < (p->offset + p->mtd.size)))
				goto err_inv;
		}

	list_add(&new->list, &mtd_partitions);
	mutex_unlock(&mtd_partitions_mutex);

	add_mtd_device(&new->mtd);

	return ret;
err_inv:
	mutex_unlock(&mtd_partitions_mutex);
	free_partition(new);
	return -EINVAL;
}
EXPORT_SYMBOL_GPL(mtd_add_partition);

int mtd_del_partition(struct mtd_info *master, int partno)
{
	struct mtd_part *slave, *next;
	int ret = -EINVAL;

	mutex_lock(&mtd_partitions_mutex);
	list_for_each_entry_safe(slave, next, &mtd_partitions, list)
		if ((slave->master == master) &&
		    (slave->mtd.index == partno)) {
			ret = del_mtd_device(&slave->mtd);
			if (ret < 0)
				break;

			list_del(&slave->list);
			free_partition(slave);
			break;
		}
	mutex_unlock(&mtd_partitions_mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(mtd_del_partition);

#if defined(CONFIG_SYNO_COMCERTO) && defined(CONFIG_MTD_ROOTFS_SPLIT)
#define ROOTFS_SPLIT_NAME "rootfs_data"
#define ROOTFS_REMOVED_NAME "<removed>"

struct squashfs_super_block {
	__le32 s_magic;
	__le32 pad0[9];
	__le64 bytes_used;
};


static int split_squashfs(struct mtd_info *master, int offset, int *split_offset)
{
	struct squashfs_super_block sb;
	int len, ret;

	ret = master->read(master, offset, sizeof(sb), &len, (void *) &sb);
	if (ret || (len != sizeof(sb))) {
		printk(KERN_ALERT "split_squashfs: error occured while reading "
			"from \"%s\"\n", master->name);
		return -EINVAL;
	}

	if (SQUASHFS_MAGIC != le32_to_cpu(sb.s_magic) ) {
		printk(KERN_ALERT "split_squashfs: no squashfs found in \"%s\"\n",
			master->name);
		*split_offset = 0;
		return 0;
	}

	if (le64_to_cpu((sb.bytes_used)) <= 0) {
		printk(KERN_ALERT "split_squashfs: squashfs is empty in \"%s\"\n",
			master->name);
		*split_offset = 0;
		return 0;
	}

	len = (u32) le64_to_cpu(sb.bytes_used);
	len += (offset & 0x000fffff);
	len +=  (master->erasesize - 1);
	len &= ~(master->erasesize - 1);
	len -= (offset & 0x000fffff);
	*split_offset = offset + len;

	return 0;
}

static int split_rootfs_data(struct mtd_info *master, struct mtd_info *rpart, const struct mtd_partition *part)
{
	struct mtd_partition *dpart;
	struct mtd_part *slave = NULL;
	struct mtd_part *spart;
	int ret, split_offset = 0;

	spart = PART(rpart);
	ret = split_squashfs(master, spart->offset, &split_offset);
	if (ret)
		return ret;

	if (split_offset <= 0)
		return 0;

	dpart = kmalloc(sizeof(*part)+sizeof(ROOTFS_SPLIT_NAME)+1, GFP_KERNEL);
	if (dpart == NULL) {
		printk(KERN_INFO "split_squashfs: no memory for partition \"%s\"\n",
			ROOTFS_SPLIT_NAME);
		return -ENOMEM;
	}

	memcpy(dpart, part, sizeof(*part));
	dpart->name = (unsigned char *)&dpart[1];
	strcpy(dpart->name, ROOTFS_SPLIT_NAME);

	dpart->size = rpart->size - (split_offset - spart->offset);
	dpart->offset = split_offset;

	if (dpart == NULL)
		return 1;

	printk(KERN_INFO "mtd: partition \"%s\" created automatically, ofs=%llX, len=%llX \n",
		ROOTFS_SPLIT_NAME, dpart->offset, dpart->size);

	slave = allocate_partition(master, dpart, 0, split_offset);
	if (IS_ERR(slave))
		return PTR_ERR(slave);
	mutex_lock(&mtd_partitions_mutex);
	list_add(&slave->list, &mtd_partitions);
	mutex_unlock(&mtd_partitions_mutex);

	add_mtd_device(&slave->mtd);

	rpart->split = &slave->mtd;

	return 0;
}

static int refresh_rootfs_split(struct mtd_info *mtd)
{
	struct mtd_partition tpart;
	struct mtd_part *part;
	char *name;
	//int index = 0;
	int offset, size;
	int ret;

	part = PART(mtd);

	/* check for the new squashfs offset first */
	ret = split_squashfs(part->master, part->offset, &offset);
	if (ret)
		return ret;

	if ((offset > 0) && !mtd->split) {
		printk(KERN_INFO "%s: creating new split partition for \"%s\"\n", __func__, mtd->name);
		/* if we don't have a rootfs split partition, create a new one */
		tpart.name = (char *) mtd->name;
		tpart.size = mtd->size;
		tpart.offset = part->offset;

		return split_rootfs_data(part->master, &part->mtd, &tpart);
	} else if ((offset > 0) && mtd->split) {
		/* update the offsets of the existing partition */
		size = mtd->size + part->offset - offset;

		part = PART(mtd->split);
		part->offset = offset;
		part->mtd.size = size;
		printk(KERN_INFO "%s: %s partition \"" ROOTFS_SPLIT_NAME "\", offset: 0x%06x (0x%06x)\n",
			__func__, (!strcmp(part->mtd.name, ROOTFS_SPLIT_NAME) ? "updating" : "creating"),
			(u32) part->offset, (u32) part->mtd.size);
		name = kmalloc(sizeof(ROOTFS_SPLIT_NAME) + 1, GFP_KERNEL);
		strcpy(name, ROOTFS_SPLIT_NAME);
		part->mtd.name = name;
	} else if ((offset <= 0) && mtd->split) {
		printk(KERN_INFO "%s: removing partition \"%s\"\n", __func__, mtd->split->name);

		/* mark existing partition as removed */
		part = PART(mtd->split);
		name = kmalloc(sizeof(ROOTFS_SPLIT_NAME) + 1, GFP_KERNEL);
		strcpy(name, ROOTFS_REMOVED_NAME);
		part->mtd.name = name;
		part->offset = 0;
		part->mtd.size = 0;
	}

	return 0;
}
#endif /* CONFIG_SYNO_COMCERTO && CONFIG_MTD_ROOTFS_SPLIT */

/*
 * This function, given a master MTD object and a partition table, creates
 * and registers slave MTD objects which are bound to the master according to
 * the partition definitions.
 *
 * We don't register the master, or expect the caller to have done so,
 * for reasons of data integrity.
 */

int add_mtd_partitions(struct mtd_info *master,
		       const struct mtd_partition *parts,
		       int nbparts)
{
	struct mtd_part *slave;
	uint64_t cur_offset = 0;
	int i;
#if defined(CONFIG_SYNO_COMCERTO) && defined(CONFIG_MTD_ROOTFS_SPLIT)
	int ret;
#endif

	printk(KERN_NOTICE "Creating %d MTD partitions on \"%s\":\n", nbparts, master->name);

	for (i = 0; i < nbparts; i++) {
		slave = allocate_partition(master, parts + i, i, cur_offset);
		if (IS_ERR(slave))
			return PTR_ERR(slave);

		mutex_lock(&mtd_partitions_mutex);
		list_add(&slave->list, &mtd_partitions);
		mutex_unlock(&mtd_partitions_mutex);

		add_mtd_device(&slave->mtd);

#if defined(CONFIG_SYNO_COMCERTO) 
		if (!strcmp(parts[i].name, "rootfs")) {
#ifdef CONFIG_MTD_ROOTFS_ROOT_DEV
			if (ROOT_DEV == 0) {
				printk(KERN_NOTICE "mtd: partition \"rootfs\" "
					"set to be root filesystem\n");
				ROOT_DEV = MKDEV(MTD_BLOCK_MAJOR, slave->mtd.index);
			}
#endif
#ifdef CONFIG_MTD_ROOTFS_SPLIT
			ret = split_rootfs_data(master, &slave->mtd, &parts[i]);
			/* if (ret == 0)
			 * 	j++; */
#endif
		}
#endif
		cur_offset = slave->offset + slave->mtd.size;
	}

	return 0;
}

#if defined(CONFIG_SYNO_COMCERTO)
int mtd_device_refresh(struct mtd_info *mtd)
{
	int ret = 0;

	if (IS_PART(mtd)) {
		struct mtd_part *part;
		struct mtd_info *master;

		part = PART(mtd);
		master = part->master;
		if (master->refresh_device)
			ret = master->refresh_device(master);
	}

	if (!ret && mtd->refresh_device)
		ret = mtd->refresh_device(mtd);

#ifdef CONFIG_MTD_ROOTFS_SPLIT
	if (!ret && IS_PART(mtd) && !strcmp(mtd->name, "rootfs"))
		refresh_rootfs_split(mtd);
#endif

	return 0;
}
EXPORT_SYMBOL_GPL(mtd_device_refresh);
#endif

static DEFINE_SPINLOCK(part_parser_lock);
static LIST_HEAD(part_parsers);

static struct mtd_part_parser *get_partition_parser(const char *name)
{
	struct mtd_part_parser *p, *ret = NULL;

	spin_lock(&part_parser_lock);

	list_for_each_entry(p, &part_parsers, list)
		if (!strcmp(p->name, name) && try_module_get(p->owner)) {
			ret = p;
			break;
		}

	spin_unlock(&part_parser_lock);

	return ret;
}

#define put_partition_parser(p) do { module_put((p)->owner); } while (0)

int register_mtd_parser(struct mtd_part_parser *p)
{
	spin_lock(&part_parser_lock);
	list_add(&p->list, &part_parsers);
	spin_unlock(&part_parser_lock);

	return 0;
}
EXPORT_SYMBOL_GPL(register_mtd_parser);

int deregister_mtd_parser(struct mtd_part_parser *p)
{
	spin_lock(&part_parser_lock);
	list_del(&p->list);
	spin_unlock(&part_parser_lock);
	return 0;
}
EXPORT_SYMBOL_GPL(deregister_mtd_parser);

/*
 * Do not forget to update 'parse_mtd_partitions()' kerneldoc comment if you
 * are changing this array!
 */
static const char *default_mtd_part_types[] = {
	"cmdlinepart",
	"ofpart",
	NULL
};

/**
 * parse_mtd_partitions - parse MTD partitions
 * @master: the master partition (describes whole MTD device)
 * @types: names of partition parsers to try or %NULL
 * @pparts: array of partitions found is returned here
 * @data: MTD partition parser-specific data
 *
 * This function tries to find partition on MTD device @master. It uses MTD
 * partition parsers, specified in @types. However, if @types is %NULL, then
 * the default list of parsers is used. The default list contains only the
 * "cmdlinepart" and "ofpart" parsers ATM.
 * Note: If there are more then one parser in @types, the kernel only takes the
 * partitions parsed out by the first parser.
 *
 * This function may return:
 * o a negative error code in case of failure
 * o zero if no partitions were found
 * o a positive number of found partitions, in which case on exit @pparts will
 *   point to an array containing this number of &struct mtd_info objects.
 */
int parse_mtd_partitions(struct mtd_info *master, const char **types,
			 struct mtd_partition **pparts,
			 struct mtd_part_parser_data *data)
{
	struct mtd_part_parser *parser;
	int ret = 0;

	if (!types)
		types = default_mtd_part_types;

	for ( ; ret <= 0 && *types; types++) {
		parser = get_partition_parser(*types);
		if (!parser && !request_module("%s", *types))
				parser = get_partition_parser(*types);
		if (!parser)
			continue;
		ret = (*parser->parse_fn)(master, pparts, data);
		put_partition_parser(parser);
		if (ret > 0) {
			printk(KERN_NOTICE "%d %s partitions found on MTD device %s\n",
			       ret, parser->name, master->name);
			break;
		}
	}
	return ret;
}

int mtd_is_partition(const struct mtd_info *mtd)
{
	struct mtd_part *part;
	int ispart = 0;

	mutex_lock(&mtd_partitions_mutex);
	list_for_each_entry(part, &mtd_partitions, list)
		if (&part->mtd == mtd) {
			ispart = 1;
			break;
		}
	mutex_unlock(&mtd_partitions_mutex);

	return ispart;
}
EXPORT_SYMBOL_GPL(mtd_is_partition);

/* Returns the size of the entire flash chip */
uint64_t mtd_get_device_size(const struct mtd_info *mtd)
{
	if (!mtd_is_partition(mtd))
		return mtd->size;

	return PART(mtd)->master->size;
}
EXPORT_SYMBOL_GPL(mtd_get_device_size);

#ifdef MY_ABC_HERE
int SYNOMTDModifyPartInfo(struct mtd_info *mtd, unsigned long offset, unsigned long length)
{
	struct mtd_part *part = PART(mtd);

	part->offset = offset;
	part->offset &= part->master->size-1;

	mtd->size = length;

	if (part->offset + mtd->size > part->master->size) {
		return -EFAULT;
	}

	return 0;
}
#endif /* MY_ABC_HERE */
