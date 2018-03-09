
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/of_platform.h>
#include <linux/irqchip.h>

#include <asm/mach/arch.h>
#include <asm/mach/map.h>

#include "hardware.h"
#include "platform.h"



static void gg_restart(enum reboot_mode mode, const char *cmd)
{
	/*
	 * To reset, use watchdog to reset whole system
	 */
	unsigned int reg_v;

	reg_v = __raw_readl((void __iomem*)IO_ADDRESS(GLOBAL_GLOBAL_CONFIG));

	/* enable axi & L2 reset */
	reg_v &= ~0x00000300;

	/* wd*_enable are exclusive with wd0_reset_subsys_enable */
	reg_v &= ~0x0000000E;

	/* reset remap, all block & subsystem */
	reg_v |= 0x000000F0;
	__raw_writel(reg_v, (void __iomem*)IO_ADDRESS(GLOBAL_GLOBAL_CONFIG));

#if 0
	/* Stall RCPU0/1, stall and clocken */
	__raw_writel(0x129, (void __iomem*)IO_ADDRESS(GLOBAL_RECIRC_CPU_CTL));
#endif

	/* Reset external device */
	reg_v = __raw_readl((void __iomem*)IO_ADDRESS(GLOBAL_SCRATCH));
	reg_v |= 0x400;
	__raw_writel(reg_v, (void __iomem*)IO_ADDRESS(GLOBAL_SCRATCH));

	//mdelay(10);

	reg_v &= ~0x400;
	__raw_writel(reg_v, (void __iomem*)IO_ADDRESS(GLOBAL_SCRATCH));

	/* Fire */
	__raw_writel(0, (void __iomem*)IO_ADDRESS(GOLDENGATE_TWD_BASE + 0x28)); /* Disable WD */
	__raw_writel(10, (void __iomem*)IO_ADDRESS(GOLDENGATE_TWD_BASE + 0x20)); /* LOAD */

	/* Enable watchdog - prescale=256, watchdog mode=1, enable=1 */
	__raw_writel(0x0000FF09, (void __iomem*)IO_ADDRESS(GOLDENGATE_TWD_BASE + 0x28)); /* Enable WD */
}

static struct map_desc goldengate_io_desc[] __initdata = {

	/*
	 * include various SoC periphels
	 *	UART, GPIO, Ethernet, SPI, I2C, ...
	 */
	{
	 .virtual = IO_ADDRESS(GOLDENGATE_GLOBAL_BASE),
	 .pfn = __phys_to_pfn(GOLDENGATE_GLOBAL_BASE),
	 .length = SZ_8M,
	 .type = MT_DEVICE,
	 },

	{
	 .virtual = IO_ADDRESS(GOLDENGATE_SCU_BASE),
	 .pfn = __phys_to_pfn(GOLDENGATE_SCU_BASE),
	 .length = SZ_8K,
	 .type = MT_DEVICE,
	 },
	{
	 .virtual = IO_ADDRESS(GOLDENGATE_L220_BASE),
	 .pfn = __phys_to_pfn(GOLDENGATE_L220_BASE),
	 .length = SZ_8K,
	 .type = MT_DEVICE,
	 },

#if 1
	/*
	 * USB, SDC, AHCI, LCDC, RTC, CIR, POWERC, SPDIF
	 */
	{
	 .virtual = IO_ADDRESS(GOLDENGATE_EHCI_BASE),
	 .pfn = __phys_to_pfn(GOLDENGATE_EHCI_BASE),
	 .length = SZ_16M,
	 .type = MT_DEVICE,
	 },
#else
	{
	 .virtual = IO_ADDRESS(GOLDENGATE_RTC_BASE),
	 .pfn = __phys_to_pfn(GOLDENGATE_RTC_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE,
	 },
	{
         .virtual = IO_ADDRESS(GOLDENGATE_AHCI_BASE),
         .pfn = __phys_to_pfn(GOLDENGATE_AHCI_BASE),
         .length = SZ_4K,
         .type = MT_DEVICE,
         },
#endif
#if 0
	/* RCPU I/DRAM  */
	{
	 .virtual = IO_ADDRESS(GOLDENGATE_RCPU_DRAM0_BASE),
	 .pfn = __phys_to_pfn(GOLDENGATE_RCPU_DRAM0_BASE),
	 .length = SZ_256K,
	 .type = MT_DEVICE,
	 },
	/* RRAM0 */
	{
	 .virtual = IO_ADDRESS(GOLDENGATE_RCPU_RRAM0_BASE),
	 .pfn = __phys_to_pfn(GOLDENGATE_RCPU_RRAM0_BASE),
	 .length = SZ_32K,
	 .type = MT_DEVICE,
	 },
	/* RRAM1 */
	{
	 .virtual = IO_ADDRESS(GOLDENGATE_RCPU_RRAM1_BASE),
	 .pfn = __phys_to_pfn(GOLDENGATE_RCPU_RRAM1_BASE),
	 .length = SZ_32K,
	 .type = MT_DEVICE,
	 },
	/* Crypto Core0 */
	{
	 .virtual = IO_ADDRESS(GOLDENGATE_RCPU_CRYPT0_BASE),
	 .pfn = __phys_to_pfn(GOLDENGATE_RCPU_CRYPT0_BASE),
	 .length = SZ_64K,
	 .type = MT_DEVICE,
	 },
	/* Crypto Core0 */
	{
	 .virtual = IO_ADDRESS(GOLDENGATE_RCPU_CRYPT1_BASE),
	 .pfn = __phys_to_pfn(GOLDENGATE_RCPU_CRYPT1_BASE),
	 .length = SZ_64K,
	 .type = MT_DEVICE,
	 },
	/* RCPU_REG  */
	{
	 .virtual = IO_ADDRESS(GOLDENGATE_RCPU_REG_BASE),
	 .pfn = __phys_to_pfn(GOLDENGATE_RCPU_REG_BASE),
	 .length = SZ_512K,
	 .type = MT_DEVICE,
	 },
	/* RCPU SADB */
	{
	 .virtual = IO_ADDRESS(GOLDENGATE_RCPU_SADB_BASE),
	 .pfn = __phys_to_pfn(GOLDENGATE_RCPU_SADB_BASE),
	 .length = SZ_64K,
	 .type = MT_DEVICE,
	 },
	/* RCPU PKT Buffer */
	{
	 .virtual = IO_ADDRESS(GOLDENGATE_RCPU_PKBF_BASE),
	 .pfn = __phys_to_pfn(GOLDENGATE_RCPU_PKBF_BASE),
	 .length = SZ_256K,
	 .type = MT_DEVICE,
	 },
#ifdef CONFIG_CS75xx_IPC2RCPU
	/* Share memory for Re-circulation CPU */
	{
	 .virtual = GOLDENGATE_IPC_BASE_VADDR,
	 .pfn = __phys_to_pfn(GOLDENGATE_IPC_BASE),
	 .length = GOLDENGATE_IPC_MEM_SIZE,
	 .type = MT_DEVICE,
	 },
#endif
	{
	 .virtual = IO_ADDRESS( GOLDENGATE_OTP_BASE ),
	 .pfn = __phys_to_pfn( GOLDENGATE_OTP_BASE ),
	 .length = SZ_1K,
	 .type = MT_DEVICE,
	},
#endif
};

static void __init gg_map_io(void)
{
#if 0	/* this region will be mapped below iotable_init() call */
	debug_ll_io_init();
#endif
	iotable_init(goldengate_io_desc, ARRAY_SIZE(goldengate_io_desc));
}


static const char* const gg_match[] __initconst = {
	"cortina,cs75xx",
	NULL,
};

#ifdef CONFIG_SMP
extern const struct smp_operations	gg_smp_ops;
#endif

DT_MACHINE_START(CS75XX_DT, "Cortina CS75xx (Device Tree)")
	.smp		= smp_ops(gg_smp_ops),
	.map_io		= gg_map_io,
	.restart	= gg_restart,
	.dt_compat	= gg_match,
MACHINE_END
