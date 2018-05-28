#ifndef __CS75XX_ETH_IO_H__
#define __CS75XX_ETH_IO_H__

#include "registers.h"

extern void __iomem*	g_iobase_global;
extern void __iomem*	g_iobase_ni;
extern void __iomem*	g_iobase_mdio;
extern void __iomem*	g_iobase_dma_lso;
extern void __iomem*	g_iobase_fe;
extern void __iomem*	g_iobase_qm;
extern void __iomem*	g_iobase_tm;
extern void __iomem*	g_iobase_sch;


#define	GLOBAL_ADDR_TO_OFFS(reg)	((reg) - (u32)GLOBAL_JTAG_ID)

static __u32 inline GLOBAL_READL(u32  reg)
{
	void __iomem*  addr = g_iobase_global + GLOBAL_ADDR_TO_OFFS(reg);
	return readl(addr);
}

static void inline GLOBAL_WRITEL(u32 val, u32 reg)
{
	void __iomem*  addr = g_iobase_global + GLOBAL_ADDR_TO_OFFS(reg);
	writel(val, addr);
}

static void inline GLOBAL_WRITE_BITS(u32 val, u32 mask, u32 reg)
{
	volatile __u32 ltmp;

	ltmp = GLOBAL_READL(reg);
	ltmp = (ltmp & ~(mask)) | (val & mask);
	GLOBAL_WRITEL(ltmp, reg);
}


#define	NI_ADDR_TO_OFFS(reg)	((reg) - (u32)NI_TOP_NI_INTF_RST_CONFIG)

static __u32 inline NI_READL(u32  reg)
{
	void __iomem*  addr = g_iobase_ni + NI_ADDR_TO_OFFS(reg);
	return readl(addr);
}

static void inline NI_WRITEL(u32 val, u32 reg)
{
	void __iomem*  addr = g_iobase_ni + NI_ADDR_TO_OFFS(reg);
	writel(val, addr);
}

static void inline NI_WRITE_BITS(u32 val, u32 mask, u32 reg)
{
	volatile __u32 ltmp;

	ltmp = NI_READL(reg);
	ltmp = (ltmp & ~(mask)) | (val & mask);
	NI_WRITEL(ltmp, reg);
}


#define	MDIO_ADDR_TO_OFFS(reg)	((reg) - (u32)PER_MDIO_CFG)

static __u32 inline MDIO_READL(u32  reg)
{
	void __iomem*  addr = g_iobase_mdio + MDIO_ADDR_TO_OFFS(reg);
	return readl(addr);
}

static void inline MDIO_WRITEL(u32 val, u32 reg)
{
	void __iomem*  addr = g_iobase_mdio + MDIO_ADDR_TO_OFFS(reg);
	writel(val, addr);
}

static void inline MDIO_WRITE_BITS(u32 val, u32 mask, u32 reg)
{
	volatile __u32 ltmp;

	ltmp = MDIO_READL(reg);
	ltmp = (ltmp & ~(mask)) | (val & mask);
	MDIO_WRITEL(ltmp, reg);
}


#define	DMA_LSO_ADDR_TO_OFFS(reg)	((reg) - (u32)DMA_DMA_LSO_RXDMA_CONTROL)

static __u32 inline DMA_LSO_READL(u32  reg)
{
	void __iomem*  addr = g_iobase_dma_lso + DMA_LSO_ADDR_TO_OFFS(reg);
	return readl(addr);
}

static void inline DMA_LSO_WRITEL(u32 val, u32 reg)
{
	void __iomem*  addr = g_iobase_dma_lso + DMA_LSO_ADDR_TO_OFFS(reg);
	writel(val, addr);
}

static void inline DMA_LSO_WRITE_BITS(u32 val, u32 mask, u32 reg)
{
	volatile __u32 ltmp;

	ltmp = DMA_LSO_READL(reg);
	ltmp = (ltmp & ~(mask)) | (val & mask);
	DMA_LSO_WRITEL(ltmp, reg);
}


#define	FE_ADDR_TO_OFFS(reg)	((reg) - (u32)FETOP_FE_SCRATCH)

static __u32 inline FE_READL(u32  reg)
{
	void __iomem*  addr = g_iobase_fe + FE_ADDR_TO_OFFS(reg);
	return readl(addr);
}

static void inline FE_WRITEL(u32 val, u32 reg)
{
	void __iomem*  addr = g_iobase_fe + FE_ADDR_TO_OFFS(reg);
	writel(val, addr);
}

static void inline FE_WRITE_BITS(u32 val, u32 mask, u32 reg)
{
	volatile __u32 ltmp;

	ltmp = FE_READL(reg);
	ltmp = (ltmp & ~(mask)) | (val & mask);
	FE_WRITEL(ltmp, reg);
}


#define	QM_ADDR_TO_OFFS(reg)	((reg) - (u32)QM_CONFIG_0)

static __u32 inline QM_READL(u32  reg)
{
	void __iomem*  addr = g_iobase_qm + QM_ADDR_TO_OFFS(reg);
	return readl(addr);
}

static void inline QM_WRITEL(u32 val, u32 reg)
{
	void __iomem*  addr = g_iobase_qm + QM_ADDR_TO_OFFS(reg);
	writel(val, addr);
}

static void inline QM_WRITE_BITS(u32 val, u32 mask, u32 reg)
{
	volatile __u32 ltmp;

	ltmp = QM_READL(reg);
	ltmp = (ltmp & ~(mask)) | (val & mask);
	QM_WRITEL(ltmp, reg);
}


#define	TM_ADDR_TO_OFFS(reg)	((reg) - (u32)TM_BM_CONFIG_0)

static __u32 inline TM_READL(u32  reg)
{
	void __iomem*  addr = g_iobase_tm + TM_ADDR_TO_OFFS(reg);
	return readl(addr);
}

static void inline TM_WRITEL(u32 val, u32 reg)
{
	void __iomem*  addr = g_iobase_tm + TM_ADDR_TO_OFFS(reg);
	writel(val, addr);
}

static void inline TM_WRITE_BITS(u32 val, u32 mask, u32 reg)
{
	volatile __u32 ltmp;

	ltmp = TM_READL(reg);
	ltmp = (ltmp & ~(mask)) | (val & mask);
	TM_WRITEL(ltmp, reg);
}


#define	SCH_ADDR_TO_OFFS(reg)	((reg) - (u32)SCH_CONTROL)

static __u32 inline SCH_READL(u32  reg)
{
	void __iomem*  addr = g_iobase_sch + SCH_ADDR_TO_OFFS(reg);
	return readl(addr);
}

static void inline SCH_WRITEL(u32 val, u32 reg)
{
	void __iomem*  addr = g_iobase_sch + SCH_ADDR_TO_OFFS(reg);
	writel(val, addr);
}

static void inline SCH_WRITE_BITS(u32 val, u32 mask, u32 reg)
{
	volatile __u32 ltmp;

	ltmp = SCH_READL(reg);
	ltmp = (ltmp & ~(mask)) | (val & mask);
	SCH_WRITEL(ltmp, reg);
}


#endif	/* __CS75XX_ETH_IO_H__ */
