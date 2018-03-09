/*
 *  linux/arch/arm/mach-goldengate/platsmp.c
 *
 *  Copyright (C) 2002 ARM Ltd.
 *  Copyright (c) Cortina-Systems Limited 2010.  All rights reserved.
 *                Jason Li <jason.li@cortina-systems.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/init.h>
#include <linux/errno.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/jiffies.h>
#include <linux/smp.h>
#include <linux/io.h>

#include <asm/cacheflush.h>
#include <asm/mach-types.h>
#include <asm/unified.h>

#include <asm/smp_scu.h>

#include "hardware.h"
#include "registers.h"




extern void goldengate_secondary_startup(void);


static void __iomem *scu_base_addr(void)
{
	return (void __iomem*)IO_ADDRESS( scu_a9_get_base() );
}

static inline unsigned int get_core_count(void)
{
	void __iomem *scu_base = scu_base_addr();
	if (scu_base)
		return scu_get_core_count(scu_base);
	return 1;
}

static DEFINE_SPINLOCK(boot_lock);

static void gg_secondary_init(unsigned int cpu)
{
	trace_hardirqs_off();

	/*
	 * let the primary processor know we're out of the
	 * pen, then head off into the C entry point
	 */
	pen_release = -1;
	smp_wmb();

	/*
	 * Synchronise with the boot thread.
	 */
	spin_lock(&boot_lock);
	spin_unlock(&boot_lock);
}

static int gg_boot_secondary(unsigned int cpu, struct task_struct *idle)
{
	unsigned long timeout;

	/*
	 * set synchronisation state between this boot processor
	 * and the secondary one
	 */
	spin_lock(&boot_lock);

	/*
	 * The secondary processor is waiting to be released from
	 * the holding pen - release it, then wait for it to flag
	 * that it has been released by resetting pen_release.
	 *
	 * Note that "pen_release" is the hardware CPU ID, whereas
	 * "cpu" is Linux's internal ID.
	 */
	pen_release = cpu;
	flush_cache_all();

	/*
	 * Send the secondary CPU a soft interrupt, thereby causing
	 * the boot monitor to read the system wide flags register,
	 * and branch to the address found there.
	 */
	arch_send_wakeup_ipi_mask(cpumask_of(cpu));

	timeout = jiffies + (1 * HZ);
	while (time_before(jiffies, timeout)) {
		smp_rmb();
		if (pen_release == -1)
			break;

		udelay(10);
	}

#if 0
	/* Clear RRAM1 after 2nd core up. Bug#38901 */
	unsigned int *vaddr = GOLDENGATE_RCPU_RRAM1_BASE;
	memset(vaddr, 0, SZ_32K);
#endif

	/*
	 * now the secondary core is starting up let it run its
	 * calibrations, then wait for it to finish
	 */
	spin_unlock(&boot_lock);

	return pen_release != -1 ? -ENOSYS : 0;
}

static void __init poke_milo(void)
{
	void __iomem *reg;

	/* nobody is to be released from the pen yet */
	pen_release = -1;

	/*
	 * Write the address of secondary startup into the system-wide flags
	 * register. The BootMonitor waits for this register to become
	 * non-zero.
	 */
	reg = (void __iomem*)IO_ADDRESS(GLOBAL_SOFTWARE);
	__raw_writel(virt_to_phys(goldengate_secondary_startup), reg);

	mb();
}

/*
 * Initialise the CPU possible map early - this describes the CPUs
 * which may be present or become present in the system.
 */
static void __init gg_smp_init_cpus(void)
{
	unsigned int i, ncores = get_core_count();

	if (ncores > NR_CPUS) {
		printk(KERN_WARNING
		       "GoldenGate: no. of cores (%d) greater than configured "
		       "maximum of %d - clipping\n",
		       ncores, NR_CPUS);
		ncores = NR_CPUS;
	}

	for (i = 0; i < ncores; i++)
		set_cpu_possible(i, true);
}

static void __init gg_smp_prepare_cpus(unsigned int max_cpus)
{
	scu_enable(scu_base_addr());
	poke_milo();
}

const struct smp_operations gg_smp_ops __initconst = {
	.smp_init_cpus		= gg_smp_init_cpus,
	.smp_prepare_cpus	= gg_smp_prepare_cpus,
	.smp_boot_secondary	= gg_boot_secondary,
	.smp_secondary_init	= gg_secondary_init,
};
