
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




static struct map_desc goldengate_io_desc[] __initdata = {

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
	 .virtual = IO_ADDRESS(GOLDENGATE_RTC_BASE),
	 .pfn = __phys_to_pfn(GOLDENGATE_RTC_BASE),
	 .length = SZ_4K,
	 .type = MT_DEVICE,
	 },
	{
	 .virtual = IO_ADDRESS(GOLDENGATE_L220_BASE),
	 .pfn = __phys_to_pfn(GOLDENGATE_L220_BASE),
	 .length = SZ_8K,
	 .type = MT_DEVICE,
	 },
	{
         .virtual = IO_ADDRESS(GOLDENGATE_AHCI_BASE),
         .pfn = __phys_to_pfn(GOLDENGATE_AHCI_BASE),
         .length = SZ_4K,
         .type = MT_DEVICE,
         },
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

#if 0
#ifdef CONFIG_CS75xx_IPC2RCPU
	/* Share memory for Re-circulation CPU */
	{
	 .virtual = GOLDENGATE_IPC_BASE_VADDR,
	 .pfn = __phys_to_pfn(GOLDENGATE_IPC_BASE),
	 .length = GOLDENGATE_IPC_MEM_SIZE,
	 .type = MT_DEVICE,
	 },
#endif
#endif
	{
	 .virtual = IO_ADDRESS( GOLDENGATE_OTP_BASE ),
	 .pfn = __phys_to_pfn( GOLDENGATE_OTP_BASE ),
	 .length = SZ_1K,
	 .type = MT_DEVICE,
	},
};

static void __init gg_map_io(void)
{
#if 0
	debug_ll_io_init();
#endif
	iotable_init(goldengate_io_desc, ARRAY_SIZE(goldengate_io_desc));
}

#if 0
static void __init gg_irq_init(void)
{
	irqchip_init();
}
#endif


static const char* const gg_match[] __initconst = {
	"cortina,cs75xx",
	NULL,
};

DT_MACHINE_START(CS75XX_DT, "Cortina CS75xx (Device Tree)")
	.map_io		= gg_map_io,
#if 0
	.init_irq	= gg_irq_init,
#endif
	.dt_compat	= gg_match,
MACHINE_END
