/*
 *  Generic setup routines for Broadcom MIPS boards
 *
 *  Copyright (C) 2005 Felix Fietkau <nbd@openwrt.org>
 *
 *  This program is free software; you can redistribute  it and/or modify it
 *  under  the terms of  the GNU General  Public License as published by the
 *  Free Software Foundation;  either version 2 of the  License, or (at your
 *  option) any later version.
 *
 *  THIS  SOFTWARE  IS PROVIDED   ``AS  IS'' AND   ANY  EXPRESS OR IMPLIED
 *  WARRANTIES,   INCLUDING, BUT NOT  LIMITED  TO, THE IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN
 *  NO  EVENT  SHALL   THE AUTHOR  BE    LIABLE FOR ANY   DIRECT, INDIRECT,
 *  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 *  NOT LIMITED   TO, PROCUREMENT OF  SUBSTITUTE GOODS  OR SERVICES; LOSS OF
 *  USE, DATA,  OR PROFITS; OR  BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 *  ANY THEORY OF LIABILITY, WHETHER IN  CONTRACT, STRICT LIABILITY, OR TORT
 *  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 *  THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *  You should have received a copy of the  GNU General Public License along
 *  with this program; if not, write  to the Free Software Foundation, Inc.,
 *  675 Mass Ave, Cambridge, MA 02139, USA.
 *
 *
 * Copyright 2005, Broadcom Corporation
 * All Rights Reserved.
 * 
 * THIS SOFTWARE IS OFFERED "AS IS", AND BROADCOM GRANTS NO WARRANTIES OF ANY
 * KIND, EXPRESS OR IMPLIED, BY STATUTE, COMMUNICATION OR OTHERWISE. BROADCOM
 * SPECIFICALLY DISCLAIMS ANY IMPLIED WARRANTIES OF MERCHANTABILITY, FITNESS
 * FOR A SPECIFIC PURPOSE OR NONINFRINGEMENT CONCERNING THIS SOFTWARE.
 *
 */

#include <linux/config.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/serialP.h>
#include <linux/ide.h>
#include <asm/bootinfo.h>
#include <asm/cpu.h>
#include <asm/time.h>
#include <asm/reboot.h>

#include <typedefs.h>
#include <osl.h>
#include <sbutils.h>
#include <bcmutils.h>
#include <bcmnvram.h>
#include <bcmdevs.h>
#include <sbhndmips.h>
#include <hndmips.h>
#include <trxhdr.h>
#include "bcm947xx.h"
#include <linux/mtd/mtd.h>
#ifdef CONFIG_MTD_PARTITIONS
#include <linux/mtd/partitions.h>
#endif
#include <linux/romfs_fs.h>
#include <linux/cramfs_fs.h>
#include <linux/minix_fs.h>
#include <linux/ext2_fs.h>

/* Global SB handle */
sb_t *bcm947xx_sbh = NULL;
spinlock_t bcm947xx_sbh_lock = SPIN_LOCK_UNLOCKED;
EXPORT_SYMBOL(bcm947xx_sbh);
EXPORT_SYMBOL(bcm947xx_sbh_lock);

/* Convenience */
#define sbh bcm947xx_sbh
#define sbh_lock bcm947xx_sbh_lock

extern void bcm947xx_time_init(void);
extern void bcm947xx_timer_setup(struct irqaction *irq);

#ifdef CONFIG_REMOTE_DEBUG
extern void set_debug_traps(void);
extern void rs_kgdb_hook(struct serial_state *);
extern void breakpoint(void);
#endif

#if defined(CONFIG_BLK_DEV_IDE) || defined(CONFIG_BLK_DEV_IDE_MODULE)
extern struct ide_ops std_ide_ops;
#endif

/* Kernel command line */
char arcs_cmdline[CL_SIZE] __initdata = CONFIG_CMDLINE;
extern void sb_serial_init(sb_t *sbh, void (*add)(void *regs, uint irq, uint baud_base, uint reg_shift));

void
bcm947xx_machine_restart(char *command)
{
	printk("Please stand by while rebooting the system...\n");

	if (sb_chip(sbh) == BCM4785_CHIP_ID)
		MTC0(C0_BROADCOM, 4, (1 << 22));

	/* Set the watchdog timer to reset immediately */
	__cli();
	sb_watchdog(sbh, 1);

	if (sb_chip(sbh) == BCM4785_CHIP_ID) {
		__asm__ __volatile__(
			".set\tmips3\n\t"
			"sync\n\t"
			"wait\n\t"
			".set\tmips0");
	}

	while (1);
}

void
bcm947xx_machine_halt(void)
{
	printk("System halted\n");

	/* Disable interrupts and watchdog and spin forever */
	__cli();
	sb_watchdog(sbh, 0);
	while (1);
}

#ifdef CONFIG_SERIAL

static int ser_line = 0;

typedef struct {
        void *regs;
        uint irq;
        uint baud_base;
        uint reg_shift;
} serial_port;

static serial_port ports[4];
static int num_ports = 0;

static void
serial_add(void *regs, uint irq, uint baud_base, uint reg_shift)
{
        ports[num_ports].regs = regs;
        ports[num_ports].irq = irq;
        ports[num_ports].baud_base = baud_base;
        ports[num_ports].reg_shift = reg_shift;
        num_ports++;
}

static void
do_serial_add(serial_port *port)
{
        void *regs;
        uint irq;
        uint baud_base;
        uint reg_shift;
        struct serial_struct s;
        
        regs = port->regs;
        irq = port->irq;
        baud_base = port->baud_base;
        reg_shift = port->reg_shift;

        memset(&s, 0, sizeof(s));

        s.line = ser_line++;
        s.iomem_base = regs;
        s.irq = irq + 2;
        s.baud_base = baud_base / 16;
        s.flags = ASYNC_BOOT_AUTOCONF;
        s.io_type = SERIAL_IO_MEM;
        s.iomem_reg_shift = reg_shift;

        if (early_serial_setup(&s) != 0) {
                printk(KERN_ERR "Serial setup failed!\n");
        }
}

#endif /* CONFIG_SERIAL */

void __init
brcm_setup(void)
{
	int i;
	char *value;

	/* Get global SB handle */
	sbh = sb_kattach(SB_OSH);

	/* Initialize clocks and interrupts */
	sb_mips_init(sbh, SBMIPS_VIRTIRQ_BASE);

	if (BCM330X(current_cpu_data.processor_id) &&
		(read_c0_diag() & BRCM_PFC_AVAIL)) {
		/* 
		 * Now that the sbh is inited set the  proper PFC value 
		 */	
		printk("Setting the PFC to its default value\n");
		enable_pfc(PFC_AUTO);
	}


	value = nvram_get("kernel_args");
#ifdef CONFIG_SERIAL
	sb_serial_init(sbh, serial_add);

	/* reverse serial ports if nvram variable contains console=ttyS1 */
	/* Initialize UARTs */
	if (value && strlen(value) && strstr(value, "console=ttyS1")!=NULL) {
		for (i = num_ports; i; i--)
			do_serial_add(&ports[i - 1]);
	} else {
		for (i = 0; i < num_ports; i++)
			do_serial_add(&ports[i]);
	}
#endif

#if defined(CONFIG_BLK_DEV_IDE) || defined(CONFIG_BLK_DEV_IDE_MODULE)
	ide_ops = &std_ide_ops;
#endif

	/* Override default command line arguments */
	if (value && strlen(value) && strncmp(value, "empty", 5))
		strncpy(arcs_cmdline, value, sizeof(arcs_cmdline));


	/* Generic setup */
	_machine_restart = bcm947xx_machine_restart;
	_machine_halt = bcm947xx_machine_halt;
	_machine_power_off = bcm947xx_machine_halt;

	board_time_init = bcm947xx_time_init;
	board_timer_setup = bcm947xx_timer_setup;
}

const char *
get_system_type(void)
{
	static char s[40];

	if (bcm947xx_sbh) {
		sprintf(s, "Broadcom BCM%X chip rev %d pkg %d", sb_chip(bcm947xx_sbh),
			sb_chiprev(bcm947xx_sbh), sb_chippkg(bcm947xx_sbh));
		return s;
	}
	else
		return "Broadcom BCM947XX";
}

void __init
bus_error_init(void)
{
}


#ifdef CONFIG_MTD_PARTITIONS

static struct mtd_partition bcm947xx_parts[] = {
	{ name: "boot",	offset: 0, size: 0, },
	{ name: "linux", offset: 0, size: 0, },
	{ name: "rootfs", offset: 0, size: 0, mask_flags: MTD_WRITEABLE, },
	{ name: "nvram", offset: 0, size: 0, },
	{ name: "flashfs", offset: 0, size: 0, },
	{ name: NULL, },
};

struct mtd_partition * __init
init_mtd_partitions(struct mtd_info *mtd, size_t size)
{
	struct minix_super_block *minixsb;
	struct ext2_super_block *ext2sb;
	struct romfs_super_block *romfsb;
	struct squashfs_super_block *squashfsb;
	struct cramfs_super *cramfsb;
	struct trx_header *trx;
	unsigned char buf[512];
	int off, trx_len = 0;
	size_t len;

	minixsb = (struct minix_super_block *) buf;
	ext2sb = (struct ext2_super_block *) buf;
	romfsb = (struct romfs_super_block *) buf;
	squashfsb = (struct squashfs_super_block *) buf;
	cramfsb = (struct cramfs_super *) buf;
	trx = (struct trx_header *) buf;

	/* Look at every 64 KB boundary */
	for (off = 0; off < size; off += (64 * 1024)) {
		memset(buf, 0xe5, sizeof(buf));

		/*
		 * Read block 0 to test for romfs and cramfs superblock
		 */
		if (MTD_READ(mtd, off, sizeof(buf), &len, buf) ||
		    len != sizeof(buf))
			continue;

		/* Try looking at TRX header for rootfs offset */
		if (le32_to_cpu(trx->magic) == TRX_MAGIC) {
			bcm947xx_parts[1].offset = off;
			trx_len = trx->len;
			if (le32_to_cpu(trx->offsets[2]) > off)
				off = le32_to_cpu(trx->offsets[2]);
			else if (le32_to_cpu(trx->offsets[1]) > off)
				off = le32_to_cpu(trx->offsets[1]);
			continue;
		}

		/* romfs is at block zero too */
		if (romfsb->word0 == ROMSB_WORD0 &&
		    romfsb->word1 == ROMSB_WORD1) {
			printk(KERN_NOTICE
			       "%s: romfs filesystem found at block %d\n",
			       mtd->name, off / BLOCK_SIZE);
			goto done;
		}

		/* squashfs is at block zero too */
		if (squashfsb->s_magic == SQUASHFS_MAGIC) {
			printk(KERN_NOTICE
			       "%s: squashfs filesystem found at block %d\n",
			       mtd->name, off / BLOCK_SIZE);
			goto done;
		}

		/* squashfs is at block zero too */
		if (squashfsb->s_magic == SQUASHFS_MAGIC) {
			printk(KERN_NOTICE
			       "%s: squashfs filesystem found at block %d\n",
			       mtd->name, off / BLOCK_SIZE);
			goto done;
		}

		/* squashfs is at block zero too */
		if (squashfsb->s_magic == SQUASHFS_MAGIC) {
			printk(KERN_NOTICE
			       "%s: squashfs filesystem found at block %d\n",
			       mtd->name, off / BLOCK_SIZE);
			goto done;
		}

		/* squashfs is at block zero too */
		if (squashfsb->s_magic == SQUASHFS_MAGIC) {
			printk(KERN_NOTICE
			       "%s: squashfs filesystem found at block %d\n",
			       mtd->name, off / BLOCK_SIZE);
			goto done;
		}

		/* squashfs is at block zero too */
		if (squashfsb->s_magic == SQUASHFS_MAGIC) {
			printk(KERN_NOTICE
			       "%s: squashfs filesystem found at block %d\n",
			       mtd->name, off / BLOCK_SIZE);
			goto done;
		}

		/* so is cramfs */
		if (cramfsb->magic == CRAMFS_MAGIC) {
			printk(KERN_NOTICE
			       "%s: cramfs filesystem found at block %d\n",
			       mtd->name, off / BLOCK_SIZE);
			goto done;
		}

		/*
		 * Read block 1 to test for minix and ext2 superblock
		 */
		if (MTD_READ(mtd, off + BLOCK_SIZE, sizeof(buf), &len, buf) ||
		    len != sizeof(buf))
			continue;

		/* Try minix */
		if (minixsb->s_magic == MINIX_SUPER_MAGIC ||
		    minixsb->s_magic == MINIX_SUPER_MAGIC2) {
			printk(KERN_NOTICE
			       "%s: Minix filesystem found at block %d\n",
			       mtd->name, off / BLOCK_SIZE);
			goto done;
		}

		/* Try ext2 */
		if (ext2sb->s_magic == cpu_to_le16(EXT2_SUPER_MAGIC)) {
			printk(KERN_NOTICE
			       "%s: ext2 filesystem found at block %d\n",
			       mtd->name, off / BLOCK_SIZE);
			goto done;
		}
	}

	printk(KERN_NOTICE
	       "%s: Couldn't find valid ROM disk image\n",
	       mtd->name);

 done:
	/* Find and size nvram */
	bcm947xx_parts[3].offset = size - ROUNDUP(NVRAM_SPACE, mtd->erasesize);
	bcm947xx_parts[3].size = size - bcm947xx_parts[3].offset;

	/* Find and size rootfs */
	if (off < size) {
		bcm947xx_parts[2].offset = off;
		bcm947xx_parts[2].size = bcm947xx_parts[3].offset - bcm947xx_parts[2].offset;
	}

	/* Size linux (kernel and rootfs) */
	bcm947xx_parts[1].size = bcm947xx_parts[3].offset - bcm947xx_parts[1].offset;

	/* Size pmon */
	bcm947xx_parts[0].size = bcm947xx_parts[1].offset - bcm947xx_parts[0].offset;

	/* Find and size flashfs -- nvram reserved + pmon */
	bcm947xx_parts[4].size = (128*1024 - bcm947xx_parts[3].size) + 
		(256*1024 - bcm947xx_parts[0].size);
	/* ... add unused space above 4MB */
	if (size > 0x400000) {
		if (trx_len <= 0x3a0000) // Small firmware - fixed amount
			bcm947xx_parts[4].size += size - 0x400000;
		else {
			bcm947xx_parts[4].size += size - (trx_len + 128*1024 + 256*1024);
			bcm947xx_parts[4].size &= (~0xFFFFUL); // Round down 64K
		}
	}
	bcm947xx_parts[4].offset = bcm947xx_parts[3].offset - bcm947xx_parts[4].size;
	if (bcm947xx_parts[4].size == 0) {
		bcm947xx_parts[4].name = NULL;
	}
	
	return bcm947xx_parts;
}

EXPORT_SYMBOL(init_mtd_partitions);

#endif