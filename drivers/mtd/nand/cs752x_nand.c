/*
 *  Copyright (C) 2003 Rick Bronson
 *
 *  Derived from drivers/mtd/nand/autcpu12.c
 *	 Copyright (c) 2001 Thomas Gleixner (gleixner@autronix.de)
 *
 *  Derived from drivers/mtd/spia.c
 *	 Copyright (C) 2000 Steven J. Hill (sjhill@cotw.com)
 *
 *
 *  Add Hardware ECC support for AT91SAM9260 / AT91SAM9263
 *     Richard Genoud (richard.genoud@gmail.com), Adeneo Copyright (C) 2007
 *
 *     Derived from Das U-Boot source code
 *     		(u-boot-1.1.5/board/atmel/at91sam9263ek/nand.c)
 *     (C) Copyright 2006 ATMEL Rousset, Lacressonniere Nicolas
 *
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 */


#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/module.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>
#include <linux/platform_device.h>
#include <linux/mtd/mtd.h>
#include <linux/mtd/nand.h>
#include <linux/mtd/nand_ecc.h>
#include <linux/mtd/partitions.h>
#include <linux/bitops.h>
#include <linux/err.h>
#include <linux/dma-mapping.h>
#include <linux/gpio.h>
#include <linux/io.h>
#include <linux/interrupt.h>
#include <asm/delay.h>
#include <asm/io.h>



#if defined(CONFIG_CS752X_NAND_ECC_HW_BCH_8_512) || defined(CONFIG_CS752X_NAND_ECC_HW_BCH_12_512)
  #define CONFIG_CS752X_NAND_ECC_HW_BCH
#endif

#if defined(CONFIG_CS752X_NAND_ECC_HW_HAMMING_256) || defined(CONFIG_CS752X_NAND_ECC_HW_HAMMING_512)
  #define CONFIG_CS752X_NAND_ECC_HW_HAMMING
#endif



#define	CSW_USE_DMA



#define	CS75XX_CMD_MAX_NUM	(3)
#define	CS75XX_BUF_SIZE		(sizeof(struct nand_buffers))

struct cs752x_nand_host {
	struct nand_chip	*nand_chip;
	struct mtd_info		*mtd;
	struct device		*dev;

	void __iomem*		iobase_fl;
	void __iomem*		iobase_dma_ssp;
	u32			dma_phy_base;

	/* NAND command and parameter caching, before submitting */
	unsigned int		cmd_array[CS75XX_CMD_MAX_NUM];
	unsigned int		cmd_cnt;
	int			page;

	unsigned int		buf_offs;
	unsigned int		buf_data_len;
	unsigned char		*buf_top;
	unsigned char		own_buf[CS75XX_BUF_SIZE];

	int			flag_status_req;
};

struct cs752x_nand_host *cs752x_host;
static int		g_nand_page = 0;
static int		g_nand_col = 0;
static volatile int	dummy;


#ifdef	CONFIG_CS752X_NAND_ECC_HW_BCH

#define	BCH_ERASE_TAG_LEN	(1)

#define	BCH_ERASE_TAG_SECTION	(0xFF)

static int cs752x_ooblayout_ecc_bch16(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct nand_ecc_ctrl *ecc = &chip->ecc;

	if (section > 1)
		return -ERANGE;
	else {
		if (section == 0) {
			oobregion->offset = 0;
			oobregion->length = 4;
		} else{
			oobregion->offset = 6;
			oobregion->length = ecc->total - 4;
		}
	}

	return 0;
}

static int cs752x_ooblayout_free_bch16(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *oobregion)
{
	if (section == BCH_ERASE_TAG_SECTION) {
		oobregion->offset = 15;
		oobregion->length = 1 - BCH_ERASE_TAG_LEN;
		/* use 1 byte for erase tag, so actually there's no space */
	} else
		return -ERANGE;

	return 0;
}

static const struct mtd_ooblayout_ops cs752x_ooblayout_ops_bch16 = {
	.ecc = cs752x_ooblayout_ecc_bch16,
	.free = cs752x_ooblayout_free_bch16,
};

static int cs752x_ooblayout_ecc_bch_lp(struct mtd_info *mtd, int section,
				 struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct nand_ecc_ctrl *ecc = &chip->ecc;

	if (section)
		return -ERANGE;

	oobregion->length = ecc->total;
	oobregion->offset = mtd->oobsize - ecc->total;

	return 0;
}

static int cs752x_ooblayout_free_bch_lp(struct mtd_info *mtd, int section,
				  struct mtd_oob_region *oobregion)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	struct nand_ecc_ctrl *ecc = &chip->ecc;

	if ((section == 0) || (section == BCH_ERASE_TAG_SECTION)) {
		oobregion->length = mtd->oobsize - ecc->total - 2 - BCH_ERASE_TAG_LEN;
		oobregion->offset = 2;
	} else
		return -ERANGE;

	return 0;
}

const struct mtd_ooblayout_ops cs752x_ooblayout_ops_bch_lp = {
	.ecc = cs752x_ooblayout_ecc_bch_lp,
	.free = cs752x_ooblayout_free_bch_lp,
};
#else	/* CONFIG_CS752X_NAND_ECC_HW_BCH */
/*
 * for humming code
 * we can use nand_base.c:nand_ooblayout_{sp,lp}_ops
 */
#endif	/* CONFIG_CS752X_NAND_ECC_HW_BCH */


/* Generic flash bbt decriptors
*/
static uint8_t bbt_pattern[] = {'B', 'b', 't', '0' };
static uint8_t mirror_pattern[] = {'1', 't', 'b', 'B' };

static struct nand_bbt_descr cs752x_bbt_main_descr = {
	.options = NAND_BBT_LASTBLOCK | NAND_BBT_CREATE | NAND_BBT_WRITE
		| NAND_BBT_2BIT | NAND_BBT_VERSION | NAND_BBT_PERCHIP,
	.offs =	8,
	.len = 4,
	.veroffs = 12,
	.maxblocks = 4,
	.pattern = bbt_pattern
};

static struct nand_bbt_descr cs752x_bbt_mirror_descr = {
	.options = NAND_BBT_LASTBLOCK | NAND_BBT_CREATE | NAND_BBT_WRITE
		| NAND_BBT_2BIT | NAND_BBT_VERSION | NAND_BBT_PERCHIP,
	.offs =	8,
	.len = 4,
	.veroffs = 12,
	.maxblocks = 4,
	.pattern = mirror_pattern
};

static unsigned int CHIP_EN;

#define FLASH_ID		0xf0050000
#define FLASH_TYPE		0xf005000c
#define FLASH_STATUS		0xf0050008
#define FLASH_NF_ACCESS		0xf0050028
#define FLASH_NF_COUNT		0xf005002c
#define FLASH_NF_COMMAND	0xf0050030
#define FLASH_NF_ADDRESS_1	0xf0050034
#define FLASH_NF_ADDRESS_2	0xf0050038
#define FLASH_NF_DATA		0xf005003c
#define FLASH_NF_ECC_STATUS	0xf0050044
#define FLASH_NF_ECC_CONTROL	0xf0050048
#define FLASH_NF_ECC_OOB	0xf005004c
#define FLASH_NF_ECC_GEN0	0xf0050050
#define FLASH_NF_ECC_GEN1	0xf0050054
#define FLASH_NF_FIFO_CONTROL	0xf0050090
#define FLASH_FLASH_ACCESS_START 0xf00500a4
#define FLASH_NF_ECC_RESET	0xf00500a8
#define FLASH_FLASH_INTERRUPT	0xf00500ac
#define FLASH_NF_BCH_STATUS	0xf00500b4
#define FLASH_NF_BCH_ERROR_LOC01 0xf00500b8
#define FLASH_NF_BCH_CONTROL	0xf00500d0
#define FLASH_NF_BCH_OOB0	0xf00500d4
#define FLASH_NF_BCH_GEN0_0	0xf00500e8
#define FLASH_NF_BCH_GEN0_1	0xf00500ec
#define FLASH_NF_BCH_GEN1_0	0xf00500fc

typedef volatile union {
  struct {
    u32 rsrvd1               :  9 ;
    u32 flashSize            :  2 ; /* bits 10:9 */
    u32 flashWidth           :  1 ; /* bits 11:11 */
    u32 flashType            :  3 ; /* bits 14:12 */
    u32 flashPin             :  1 ; /* bits 15:15 */
    u32 rsrvd2               : 16 ;
  } bf ;
  u32     wrd ;
} FLASH_TYPE_t;

typedef volatile union {
  struct {
    u_int32_t nflashExtAddr	:  8 ; /* bits 7:0 */
    u_int32_t rsrvd1	       :  2 ;
    u_int32_t nflashRegWidth       :  2 ; /* bits 11:10 */
    u_int32_t rsrvd2	       :  3 ;
    u_int32_t nflashCeAlt	  :  1 ; /* bits 15:15 */
    u_int32_t autoReset	    :  1 ; /* bits 16:16 */
    u_int32_t rsrvd3	       :  7 ;
    u_int32_t FIFO_RDTH	    :  2 ; /* bits 25:24 */
    u_int32_t FIFO_WRTH	    :  2 ; /* bits 27:26 */
    u_int32_t rsrvd4	       :  4 ;
  } bf ;
  u_int32_t     wrd ;
} FLASH_NF_ACCESS_t;

#define	NFLASH_WiDTH8	0x0
#define	NFLASH_WiDTH16	0x1
#define	NFLASH_WiDTH32	0x2

#define NFLASH_CHIP0_EN	0x0
#define NFLASH_CHIP1_EN	0x1

typedef volatile union {
  struct {
    u_int32_t nflashRegCmdCount    :  2 ; /* bits 1:0 */
    u_int32_t rsrvd1	       :  2 ;
    u_int32_t nflashRegAddrCount   :  3 ; /* bits 6:4 */
    u_int32_t rsrvd2	       :  1 ;
    u_int32_t nflashRegDataCount   : 14 ; /* bits 21:8 */
    u_int32_t nflashRegOobCount    : 10 ; /* bits 31:22 */
  } bf ;
  u_int32_t     wrd ;
} FLASH_NF_COUNT_t;

#define	NCNT_EMPTY_OOB  0x3FF
#define	NCNT_512P_OOB   0x0F
#define	NCNT_2kP_OOB    0x3F
#define	NCNT_4kP_OOB    0x7F
#define	NCNT_M4kP_OOB   0xdF
#define	NCNT_EMPTY_DATA 0x3FFF
#define	NCNT_512P_DATA  0x1FF
#define	NCNT_2kP_DATA   0x7FF
#define	NCNT_4kP_DATA   0xFFF
#define	NCNT_DATA_1    	0x0
#define	NCNT_DATA_2    	0x1
#define	NCNT_DATA_3    	0x2
#define	NCNT_DATA_4    	0x3
#define	NCNT_DATA_5    	0x4
#define	NCNT_DATA_6    	0x5
#define	NCNT_DATA_7    	0x6
#define	NCNT_DATA_8    	0x7

#define	NCNT_EMPTY_ADDR	0x7
#define	NCNT_ADDR_5	0x4
#define	NCNT_ADDR_4	0x3
#define	NCNT_ADDR_3	0x2
#define	NCNT_ADDR_2	0x1
#define	NCNT_ADDR_1	0x0
#define	NCNT_EMPTY_CMD  0x3
#define	NCNT_CMD_3    	0x2
#define	NCNT_CMD_2    	0x1
#define	NCNT_CMD_1    	0x0

typedef volatile union {
  struct {
    u_int32_t eccStatus	    :  2 ; /* bits 1:0 */
    u_int32_t rsrvd1	       :  1 ;
    u_int32_t eccErrBit	    :  4 ; /* bits 6:3 */
    u_int32_t eccErrByte	   :  9 ; /* bits 15:7 */
    u_int32_t eccErrWord	   :  8 ; /* bits 23:16 */
    u_int32_t rsrvd2	       :  7 ;
    u_int32_t eccDone	      :  1 ; /* bits 31:31 */
  } bf ;
  u_int32_t     wrd ;
} FLASH_NF_ECC_STATUS_t;

typedef volatile union {
  struct {
    u_int32_t rsrvd1	       :  1 ;
    u_int32_t eccGenMode	   :  1 ; /* bits 1:1 */
    u_int32_t rsrvd2	       :  2 ;
    u_int32_t eccCodeSel	   :  4 ; /* bits 7:4 */
    u_int32_t eccEn		:  1 ; /* bits 8:8 */
    u_int32_t rsrvd3	       : 23 ;
  } bf ;
  u_int32_t     wrd ;
} FLASH_NF_ECC_CONTROL_t;

typedef volatile union {
  struct {
    u_int32_t eccCodeOob	   : 32 ; /* bits 31:0 */
  } bf ;
  u_int32_t     wrd ;
} FLASH_NF_ECC_OOB_t;

typedef volatile union {
  struct {
    u_int32_t fifoCmd	      :  2 ; /* bits 1:0 */
    u_int32_t rsrvd1	       :  2 ;
    u_int32_t fifoDbgSel	   :  4 ; /* bits 7:4 */
    u_int32_t rsrvd2	       : 24 ;
  } bf ;
  u_int32_t     wrd ;
} FLASH_NF_FIFO_CONTROL_t;

typedef volatile union {
  struct {
    u_int32_t nflashRegReq	 :  1 ; /* bits 0:0 */
    u_int32_t sflashRegReq	 :  1 ; /* bits 1:1 */
    u_int32_t fifoReq	      :  1 ; /* bits 2:2 */
    u_int32_t rsrvd1	       :  9 ;
    u_int32_t nflashRegCmd	 :  2 ; /* bits 13:12 */
    u_int32_t rsrvd2	       : 18 ;
  } bf ;
  u_int32_t     wrd ;
} FLASH_FLASH_ACCESS_START_t;

#define	FLASH_GO	0x1

#define	FLASH_RD	0x2
#define	FLASH_WT	0x3

typedef volatile union {
  struct {
    u_int32_t eccClear	     :  1 ; /* bits 0:0 */
    u_int32_t fifoClear	    :  1 ; /* bits 1:1 */
    u_int32_t nflash_reset	 :  1 ; /* bits 2:2 */
    u_int32_t rsrvd1	       : 29 ;
  } bf ;
  u_int32_t     wrd ;
} FLASH_NF_ECC_RESET_t;

#define	NF_RESET	0x1
#define	ECC_CLR		0x1
#define	FIFO_CLR	0x1

typedef volatile union {
  struct {
    u_int32_t regIrq	       :  1 ; /* bits 0:0 */
    u_int32_t fifoIrq	      :  1 ; /* bits 1:1 */
    u_int32_t f_addr_err	   :  1 ; /* bits 2:2 */
    u_int32_t eccIrq	       :  1 ; /* bits 3:3 */
    u_int32_t nfWdtIrq	     :  1 ; /* bits 4:4 */
    u_int32_t rsrvd1	       :  1 ;
    u_int32_t bchGenIrq	    :  1 ; /* bits 6:6 */
    u_int32_t bchDecIrq	    :  1 ; /* bits 7:7 */
    u_int32_t rsrvd2	       : 24 ;
  } bf ;
  u_int32_t     wrd ;
} FLASH_FLASH_INTERRUPT_t;

typedef volatile union {
  struct {
    u_int32_t bchDecStatus	 :  2 ; /* bits 1:0 */
    u_int32_t rsrvd1	       :  2 ;
    u_int32_t bchErrNum	    :  4 ; /* bits 7:4 */
    u_int32_t rsrvd2	       : 22 ;
    u_int32_t bchDecDone	   :  1 ; /* bits 30:30 */
    u_int32_t bchGenDone	   :  1 ; /* bits 31:31 */
  } bf ;
  u_int32_t     wrd ;
} FLASH_NF_BCH_STATUS_t;

#define	BCH_UNCORRECTABLE	0x3
#define	BCH_CORRECTABLE_ERR	0x2
#define	BCH_NO_ERR		0x1

typedef volatile union {
  struct {
    u_int32_t bchErrLoc0	   : 13 ; /* bits 12:0 */
    u_int32_t rsrvd1	       :  3 ;
    u_int32_t bchErrLoc1	   : 13 ; /* bits 28:16 */
    u_int32_t rsrvd2	       :  3 ;
  } bf ;
  u_int32_t     wrd ;
} FLASH_NF_BCH_ERROR_LOC01_t;

typedef volatile union {
  struct {
    u_int32_t bchCompare	   :  1 ; /* bits 0:0 */
    u_int32_t bchOpcode	    :  1 ; /* bits 1:1 */
    u_int32_t rsrvd1	       :  2 ;
    u_int32_t bchCodeSel	   :  4 ; /* bits 7:4 */
    u_int32_t bchEn		:  1 ; /* bits 8:8 */
    u_int32_t bchErrCap	    :  1 ; /* bits 9:9 */
    u_int32_t rsrvd2	       :  6 ;
    u_int32_t bchTestCtrl	  :  4 ; /* bits 19:16 */
    u_int32_t rsrvd3	       : 12 ;
  } bf ;
  u_int32_t     wrd ;
} FLASH_NF_BCH_CONTROL_t;

#define	BCH_ENABLE	0x01
#define	BCH_DISABLE	0x00

#define	BCH_DECODE	0x01
#define	BCH_ENCODE	0x00

#define BCH_ERR_CAP_8_512	0x0
#define BCH_ERR_CAP_12_512	0x1

#define	FLASH_STATUS_MASK_nState	(0x0FUL << 8)

static FLASH_TYPE_t			flash_type;
static FLASH_NF_ACCESS_t		nf_access;
static FLASH_NF_COUNT_t			nf_cnt;
static FLASH_NF_ECC_STATUS_t		ecc_sts;
static FLASH_NF_ECC_CONTROL_t 		ecc_ctl;
static FLASH_NF_ECC_OOB_t		ecc_oob;
static FLASH_NF_FIFO_CONTROL_t		fifo_ctl;
static FLASH_FLASH_ACCESS_START_t 	flash_start;
static FLASH_NF_ECC_RESET_t		ecc_reset;
static FLASH_FLASH_INTERRUPT_t		flash_int_sts;
static FLASH_NF_BCH_STATUS_t		bch_sts;
static FLASH_NF_BCH_ERROR_LOC01_t	bch_err_loc01;
static FLASH_NF_BCH_CONTROL_t		bch_ctrl;


#ifdef CSW_USE_DMA

/* DMA regs */
#define DMA_DMA_SSP_RXDMA_CONTROL	0xf0090400
#define DMA_DMA_SSP_TXDMA_CONTROL	0xf0090404
#define DMA_DMA_SSP_TXQ5_CONTROL	0xf0090414
#define DMA_DMA_SSP_RXQ5_BASE_DEPTH	0xf0090438
#define DMA_DMA_SSP_RXQ5_WPTR		0xf0090444
#define DMA_DMA_SSP_RXQ5_RPTR		0xf0090448
#define DMA_DMA_SSP_TXQ5_BASE_DEPTH	0xf009045c
#define DMA_DMA_SSP_TXQ5_WPTR		0xf0090468
#define DMA_DMA_SSP_TXQ5_RPTR		0xf009046c
#define DMA_DMA_SSP_RXQ5_INTERRUPT	0xf00904c8
#define DMA_DMA_SSP_TXQ5_INTERRUPT	0xf00904e0

typedef volatile union {
  struct {
    u32 rx_dma_enable        :  1 ; /* bits 0:0 */
    u32 rx_check_own         :  1 ; /* bits 1:1 */
    u32 rsrvd1               : 30 ;
  } bf ;
  u32     wrd ;
} DMA_DMA_SSP_RXDMA_CONTROL_t;

typedef volatile union {
  struct {
    u32 tx_dma_enable        :  1 ; /* bits 0:0 */
    u32 tx_check_own         :  1 ; /* bits 1:1 */
    u32 rsrvd1               : 30 ;
  } bf ;
  u32     wrd ;
} DMA_DMA_SSP_TXDMA_CONTROL_t;

typedef volatile union {
  struct {
    u_int32_t txq5_en	      :  1 ; /* bits 0:0 */
    u_int32_t rsrvd1	       :  1 ;
    u_int32_t txq5_flush_en	:  1 ; /* bits 2:2 */
    u_int32_t rsrvd2	       : 29 ;
  } bf ;
  u_int32_t     wrd ;
} DMA_DMA_SSP_TXQ5_CONTROL_t;

typedef volatile union {
  struct {
    u32 depth                :  4 ; /* bits 3:0 */
    u32 base                 : 28 ; /* bits 31:4 */
  } bf ;
  u32     wrd ;
} DMA_DMA_SSP_RXQ5_BASE_DEPTH_t;

typedef volatile union {
  struct {
    u_int32_t index		: 13 ; /* bits 12:0 */
    u_int32_t rsrvd1	       : 19 ;
  } bf ;
  u_int32_t     wrd ;
} DMA_DMA_SSP_RXQ5_WPTR_t;

typedef volatile union {
  struct {
    u_int32_t index		: 13 ; /* bits 12:0 */
    u_int32_t rsrvd1	       : 19 ;
  } bf ;
  u_int32_t     wrd ;
} DMA_DMA_SSP_RXQ5_RPTR_t;

typedef volatile union {
  struct {
    u32 depth                :  4 ; /* bits 3:0 */
    u32 base                 : 28 ; /* bits 31:4 */
  } bf ;
  u32     wrd ;
} DMA_DMA_SSP_TXQ5_BASE_DEPTH_t;

typedef volatile union {
  struct {
    u_int32_t index		: 13 ; /* bits 12:0 */
    u_int32_t rsrvd1	       : 19 ;
  } bf ;
  u_int32_t     wrd ;
} DMA_DMA_SSP_TXQ5_WPTR_t;

typedef volatile union {
  struct {
    u_int32_t index		: 13 ; /* bits 12:0 */
    u_int32_t rsrvd1	       : 19 ;
  } bf ;
  u_int32_t     wrd ;
} DMA_DMA_SSP_TXQ5_RPTR_t;

typedef volatile union {
  struct {
    u_int32_t rxq5_eof	     :  1 ; /* bits 0:0 */
    u_int32_t rxq5_full	    :  1 ; /* bits 1:1 */
    u_int32_t rxq5_overrun	 :  1 ; /* bits 2:2 */
    u_int32_t rxq5_cntmsb	  :  1 ; /* bits 3:3 */
    u_int32_t rxq5_full_drop_overrun :  1 ; /* bits 4:4 */
    u_int32_t rxq5_full_drop_cntmsb :  1 ; /* bits 5:5 */
    u_int32_t rsrvd1	       : 26 ;
  } bf ;
  u_int32_t     wrd ;
} DMA_DMA_SSP_RXQ5_INTERRUPT_t;

typedef volatile union {
  struct {
    u_int32_t txq5_eof	     :  1 ; /* bits 0:0 */
    u_int32_t txq5_empty	   :  1 ; /* bits 1:1 */
    u_int32_t txq5_overrun	 :  1 ; /* bits 2:2 */
    u_int32_t txq5_cntmsb	  :  1 ; /* bits 3:3 */
    u_int32_t rsrvd1	       : 28 ;
  } bf ;
  u_int32_t     wrd ;
} DMA_DMA_SSP_TXQ5_INTERRUPT_t;

typedef struct tx_descriptor_t {
	union tx_word0_t {
		struct {
			u32 buf_size	:  16 ; /* bits 15:0 */
			u32 desccnt     	:  6 ;  /* bits 21:16 */
			u32 sgm_rsrvd	:  5 ;  /* bits 26:22 */
			u32 sof_eof_rsrvd :  2 ;  /* bits 28:27 */
			u32 cache_rsrvd	:  1 ;  /* bits 29 */
			u32 share_rsrvd	:  1 ;  /* bits 30 */
			u32 own		:  1 ;  /* bits 31:31 */
		} bf ;
		u32     wrd ;
	}word0;
	u32 buf_adr;	/* data buffer address */
	u32 word2;	/* data buffer address */
	u32 word3;	/* data buffer address */
} DMA_SSP_TX_DESC_T;

typedef struct rx_descriptor_t {
	union rx_word0_t {
		struct {
			u32 buf_size    :  16 ; /* bits 15:0 */
			u32 desccnt     :  6 ;  /* bits 21:16 */
			u32 rqsts_rsrvd :  7 ;  /* bits 28:22 */
			u32 cache_rsrvd :  1 ;  /* bits 29 */
			u32 share_rsrvd :  1 ;  /* bits 30 */
			u32 own		:  1 ;  /* bits 31 */
		} bf ;
		u32     wrd ;
	}word0;
	u32 buf_adr;	/* data buffer address */
	u32 word2;	/* data buffer address */
	u32 word3;	/* data buffer address */
} DMA_SSP_RX_DESC_T;

static DMA_DMA_SSP_TXQ5_CONTROL_t 	dma_txq5_ctrl;
static DMA_DMA_SSP_RXQ5_WPTR_t		dma_rxq5_wptr;
static DMA_DMA_SSP_RXQ5_RPTR_t		dma_rxq5_rptr;
static DMA_DMA_SSP_TXQ5_WPTR_t		dma_txq5_wptr;
static DMA_DMA_SSP_TXQ5_RPTR_t		dma_txq5_rptr;
static DMA_DMA_SSP_RXQ5_INTERRUPT_t	dma_ssp_rxq5_intsts;
static DMA_DMA_SSP_TXQ5_INTERRUPT_t	dma_ssp_txq5_intsts;
static DMA_SSP_TX_DESC_T *tx_desc;
static DMA_SSP_RX_DESC_T *rx_desc;

#define FDMA_DEPTH	3
#define FDMA_DESC_NUM	(1 << FDMA_DEPTH)

#define OWN_DMA	0
#define OWN_SW	1
#endif


static int cs752x_nand_dev_ready(struct mtd_info *mtd);


/**
 * nand_calculate_512_ecc - [NAND Interface] Calculate 3-byte ECC for 256/512-byte
 *			 block
 * @mtd:	MTD block structure
 * @buf:	input buffer with raw data
 * @code:	output buffer with ECC
 */
int nand_calculate_512_ecc(struct mtd_info *mtd, const unsigned char *data_buf,
		       unsigned char *ecc_buf)
{
	unsigned long i, ALIGN_FACTOR;
	unsigned long tmp;
	unsigned long uiparity = 0;
	unsigned long parityCol, ecc = 0;
	unsigned long parityCol4321 = 0, parityCol4343 = 0, parityCol4242 =
	    0, parityColTot = 0;
	unsigned long *Data;
	unsigned long Xorbit = 0;

	ALIGN_FACTOR = (unsigned long)data_buf % 4;
	Data = (unsigned long *)(data_buf + ALIGN_FACTOR);

	for (i = 0; i < 16; i++) {
		parityCol = *Data++;
		tmp = *Data++;
		parityCol ^= tmp;
		parityCol4242 ^= tmp;
		tmp = *Data++;
		parityCol ^= tmp;
		parityCol4343 ^= tmp;
		tmp = *Data++;
		parityCol ^= tmp;
		parityCol4343 ^= tmp;
		parityCol4242 ^= tmp;
		tmp = *Data++;
		parityCol ^= tmp;
		parityCol4321 ^= tmp;
		tmp = *Data++;
		parityCol ^= tmp;
		parityCol4242 ^= tmp;
		parityCol4321 ^= tmp;
		tmp = *Data++;
		parityCol ^= tmp;
		parityCol4343 ^= tmp;
		parityCol4321 ^= tmp;
		tmp = *Data++;
		parityCol ^= tmp;
		parityCol4242 ^= tmp;
		parityCol4343 ^= tmp;
		parityCol4321 ^= tmp;

		parityColTot ^= parityCol;

		tmp = (parityCol >> 16) ^ parityCol;
		tmp = (tmp >> 8) ^ tmp;
		tmp = (tmp >> 4) ^ tmp;
		tmp = ((tmp >> 2) ^ tmp) & 0x03;
		if ((tmp == 0x01) || (tmp == 0x02)) {
			uiparity ^= i;
			Xorbit ^= 0x01;
		}
	}

	tmp = (parityCol4321 >> 16) ^ parityCol4321;
	tmp = (tmp << 8) ^ tmp;
	tmp = (tmp >> 4) ^ tmp;
	tmp = (tmp >> 2) ^ tmp;
	ecc |= ((tmp << 1) ^ tmp) & 0x200;	/*  p128 */

	tmp = (parityCol4343 >> 16) ^ parityCol4343;
	tmp = (tmp >> 8) ^ tmp;
	tmp = (tmp << 4) ^ tmp;
	tmp = (tmp << 2) ^ tmp;
	ecc |= ((tmp << 1) ^ tmp) & 0x80;	/*  p64 */

	tmp = (parityCol4242 >> 16) ^ parityCol4242;
	tmp = (tmp >> 8) ^ tmp;
	tmp = (tmp << 4) ^ tmp;
	tmp = (tmp >> 2) ^ tmp;
	ecc |= ((tmp << 1) ^ tmp) & 0x20;	/*  p32 */

	tmp = parityColTot & 0xFFFF0000;
	tmp = tmp >> 16;
	tmp = (tmp >> 8) ^ tmp;
	tmp = (tmp >> 4) ^ tmp;
	tmp = (tmp << 2) ^ tmp;
	ecc |= ((tmp << 1) ^ tmp) & 0x08;	/*  p16 */

	tmp = parityColTot & 0xFF00FF00;
	tmp = (tmp >> 16) ^ tmp;
	tmp = (tmp >> 8);
	tmp = (tmp >> 4) ^ tmp;
	tmp = (tmp >> 2) ^ tmp;
	ecc |= ((tmp << 1) ^ tmp) & 0x02;	/*  p8 */

	tmp = parityColTot & 0xF0F0F0F0;
	tmp = (tmp << 16) ^ tmp;
	tmp = (tmp >> 8) ^ tmp;
	tmp = (tmp << 2) ^ tmp;
	ecc |= ((tmp << 1) ^ tmp) & 0x800000;	/*  p4 */

	tmp = parityColTot & 0xCCCCCCCC;
	tmp = (tmp << 16) ^ tmp;
	tmp = (tmp >> 8) ^ tmp;
	tmp = (tmp << 4) ^ tmp;
	tmp = (tmp >> 2);
	ecc |= ((tmp << 1) ^ tmp) & 0x200000;	/*  p2 */

	tmp = parityColTot & 0xAAAAAAAA;
	tmp = (tmp << 16) ^ tmp;
	tmp = (tmp >> 8) ^ tmp;
	tmp = (tmp >> 4) ^ tmp;
	tmp = (tmp << 2) ^ tmp;
	ecc |= (tmp & 0x80000);	/*  p1 */

	ecc |= (uiparity & 0x01) << 11;
	ecc |= (uiparity & 0x02) << 12;
	ecc |= (uiparity & 0x04) << 13;
	ecc |= (uiparity & 0x08) << 14;

	if (Xorbit) {
		ecc |= (ecc ^ 0x00AAAAAA) >> 1;
	} else {
		ecc |= (ecc >> 1);
	}

	ecc = ~ecc;
	*(ecc_buf + 2) = (uint8_t) (ecc >> 16);
	*(ecc_buf + 1) = (uint8_t) (ecc >> 8);
	*(ecc_buf + 0) = (uint8_t) (ecc);

	return 0;
}
EXPORT_SYMBOL(nand_calculate_512_ecc);


/**
 * nand_correct_512_data - [NAND Interface] Detect and correct bit error(s)
 * @mtd:	MTD block structure
 * @buf:	raw data read from the chip
 * @read_ecc:	ECC from the chip
 * @calc_ecc:	the ECC calculated from raw data
 *
 * Detect and correct a 1 bit error for 256/512 byte block
 */
int nand_correct_512_data(struct mtd_info *mtd, unsigned char *pPagedata,
		      unsigned char *iEccdata1, unsigned char *iEccdata2)
{
	uint32_t iCompecc = 0, iEccsum = 0;
	uint32_t iFindbyte = 0;
	uint32_t iIndex;
	uint32_t nT1 = 0, nT2 = 0;

	uint8_t iNewvalue;
	uint8_t iFindbit = 0;

	uint8_t *pEcc1 = (uint8_t *) iEccdata1;
	uint8_t *pEcc2 = (uint8_t *) iEccdata2;

	for (iIndex = 0; iIndex < 2; iIndex++) {
		nT1 ^= (((*pEcc1) >> iIndex) & 0x01);
		nT2 ^= (((*pEcc2) >> iIndex) & 0x01);
	}

	for (iIndex = 0; iIndex < 3; iIndex++)
		iCompecc |= ((~(*pEcc1++) ^ ~(*pEcc2++)) << iIndex * 8);

	for (iIndex = 0; iIndex < 24; iIndex++) {
		iEccsum += ((iCompecc >> iIndex) & 0x01);
	}

	switch (iEccsum) {
	case 0:
		/* printf("RESULT : no error\n"); */
		return 1;	/* ECC_NO_ERROR; */

	case 1:
		/* printf("RESULT : ECC code 1 bit fail\n"); */
		return 1;	/* ECC_ECC_ERROR; */

	case 12:
		if (nT1 != nT2) {
			iFindbyte =
			    ((iCompecc >> 17 & 1) << 8) +
			    ((iCompecc >> 15 & 1) << 7) +
			    ((iCompecc >> 13 & 1) << 6)
			    + ((iCompecc >> 11 & 1) << 5) +
			    ((iCompecc >> 9 & 1) << 4) +
			    ((iCompecc >> 7 & 1) << 3)
			    + ((iCompecc >> 5 & 1) << 2) +
			    ((iCompecc >> 3 & 1) << 1) + (iCompecc >> 1 & 1);
			iFindbit =
			    (uint8_t) (((iCompecc >> 23 & 1) << 2) +
				       ((iCompecc >> 21 & 1) << 1) +
				       (iCompecc >> 19 & 1));
			iNewvalue =
			    (uint8_t) (pPagedata[iFindbyte] ^ (1 << iFindbit));

			/* printf("iCompecc = %d\n",iCompecc); */
			/* printf("RESULT : one bit error\r\n"); */
			/* printf("byte = %d, bit = %d\r\n", iFindbyte, iFindbit); */
			/* printf("corrupted = %x, corrected = %x\r\n", pPagedata[iFindbyte], iNewvalue); */

			return 1;	/* ECC_CORRECTABLE_ERROR; */
		} else
			return -1;	/* ECC_UNCORRECTABLE_ERROR; */

	default:
		/* printf("RESULT : unrecoverable error\n"); */
		return -1;	/* ECC_UNCORRECTABLE_ERROR; */
	}
	/* middle not yet */
	return 0;
}

#ifdef NO_NEED
static cs_status_t cs752x_nand_pwr_notifier(cs_pm_freq_notifier_data_t *data)
{
	struct nand_chip *chip = cs752x_host->nand_chip;
	struct mtd_info *mtd = cs752x_host->mtd;

	if (data->event == CS_PM_FREQ_PRECHANGE) {

		nand_get_device(chip, mtd, FL_FREQ_CHANGE);

		//printk(KERN_ERR ">>>start %x! per %d -> %d, axi %d -> %d \n",
		//(int)data->data,
		//data->old_peripheral_clk, data->new_peripheral_clk,
		//data->old_axi_clk, data->new_axi_clk);


	} else if (data->event == CS_PM_FREQ_POSTCHANGE) {

		nand_release_device(mtd);
		//printk(KERN_ERR "<<<stop %x! per %d -> %d, axi %d -> %d \n",
		//       (int)data->data,
		//       data->old_peripheral_clk, data->new_peripheral_clk,
		//       data->old_axi_clk, data->new_axi_clk);
	}

	return CS_E_OK;
}

static cs_pm_freq_notifier_t n = {
	.notifier = cs752x_nand_pwr_notifier,
	.data = (void *)0x1,
};
#endif	/* NO_NEED */

#ifdef CSW_USE_DMA
inline static u32 dma_readl(u32 reg)
{
	reg -= DMA_DMA_SSP_RXDMA_CONTROL;
	return readl((void __iomem*)(cs752x_host->iobase_dma_ssp + reg));
}

inline static void dma_writel(u32 reg, u32 val)
{
	reg -= DMA_DMA_SSP_RXDMA_CONTROL;
	writel(val, (void __iomem*)(cs752x_host->iobase_dma_ssp + reg));
}
#endif	/* CSW_USE_DMA */

inline static u32 fl_readl(u32 reg)
{
	reg -= FLASH_ID;
	return readl((void __iomem*)(cs752x_host->iobase_fl + reg));
}

inline static void fl_writel(u32 reg, u32 val)
{
	reg -= FLASH_ID;
	writel(val, (void __iomem*)(cs752x_host->iobase_fl + reg));
}


static void check_flash_ctrl_status(void)
{
	unsigned long	timeo;
	unsigned int	val;

	timeo = jiffies + HZ;
	do {
		val = fl_readl(FLASH_STATUS);
		val &= FLASH_STATUS_MASK_nState;
		if (val == 0)
			return;
	} while (time_before(jiffies, timeo));

	printk("FLASH_STATUS ERROR: %x\n", val);
}

inline static u32 mk_nf_command(u8 cmd0, u8 cmd1, u8 cmd2)
{
	return ((u32)cmd2 << 16) | ((u32)cmd1 << 8) | cmd0;
}

/**
 * cs752x_nand_erase_block - [GENERIC] erase a block
 * @mtd:	MTD device structure
 * @page:	page address
 *
 * Erase a block.
 */

static int cs752x_nand_erase_block(struct mtd_info *mtd, int page)
{
	struct nand_chip *this = mtd_to_nand(mtd);
	u64 test;
	unsigned long	timeo;
	u32 ul_cmd;

	check_flash_ctrl_status();

	/* Send commands to erase a page */
	fl_writel(FLASH_NF_ECC_CONTROL, 0);

	nf_cnt.wrd = 0;
	nf_cnt.bf.nflashRegOobCount = NCNT_EMPTY_OOB;
	nf_cnt.bf.nflashRegDataCount = NCNT_EMPTY_DATA;
	nf_cnt.bf.nflashRegCmdCount = NCNT_CMD_2;

	test = 0x10000 * mtd->writesize;
	if (this->chipsize > test) {
		nf_cnt.bf.nflashRegAddrCount = NCNT_ADDR_3;
	} else {
		nf_cnt.bf.nflashRegAddrCount = NCNT_ADDR_2;
	}

	ul_cmd = mk_nf_command(NAND_CMD_ERASE1, NAND_CMD_ERASE2, 0);
	fl_writel(FLASH_NF_COUNT, nf_cnt.wrd);
	fl_writel(FLASH_NF_COMMAND, ul_cmd);
	fl_writel(FLASH_NF_ADDRESS_1, page);
	fl_writel(FLASH_NF_ADDRESS_2, 0x00UL);

	nf_access.wrd = 0;
	nf_access.bf.nflashCeAlt = CHIP_EN;
	/* nf_access.bf.nflashDirWr = ; */
	nf_access.bf.nflashRegWidth = NFLASH_WiDTH8;
	fl_writel(FLASH_NF_ACCESS, nf_access.wrd);

	flash_start.wrd = 0;
	flash_start.bf.nflashRegReq = FLASH_GO;
	flash_start.bf.nflashRegCmd = FLASH_RD;	/* no data access use read.. */
	fl_writel(FLASH_FLASH_ACCESS_START, flash_start.wrd);

	timeo = jiffies + HZ;
	do {
		flash_start.wrd = fl_readl(FLASH_FLASH_ACCESS_START);
		if(!flash_start.bf.nflashRegReq)
			return 0;
	} while (time_before(jiffies, timeo));

	return 0;
}

/**
 * cs752x_nand_read_subpage - [REPLACABLE] software ecc based sub-page read function
 * @mtd:	mtd info structure
 * @chip:	nand chip info structure
 * @data_offs:	offset of requested data within the page
 * @readlen:	data length
 * @bufpoi:	buffer to store read data
 */
static int cs752x_nand_read_subpage(struct mtd_info *mtd,
				    struct nand_chip *chip, uint32_t data_offs,
				    uint32_t readlen, uint8_t * bufpoi, int page)
{
	int start_step, end_step, num_steps;
	int datafrag_len;

	/* Column address wihin the page aligned to ECC size (256bytes). */
	start_step = data_offs / chip->ecc.size;
	end_step = (data_offs + readlen - 1) / chip->ecc.size;
	num_steps = end_step - start_step + 1;

	/* Data size aligned to ECC ecc.size */
	datafrag_len = num_steps * chip->ecc.size;

	page = g_nand_page;
	chip->pagebuf = -1;
	/* chip->ecc.read_page(mtd, chip, chip->buffers->databuf, (page<<this->page_shift)); */
	chip->ecc.read_page(mtd, chip, chip->buffers->databuf, false, page);

	memcpy(bufpoi, chip->buffers->databuf + data_offs, datafrag_len);

	return 0;
}


/**
 * cs752x_hw_nand_correct_data - [NAND Interface] Detect and correct bit error(s)
 * @mtd:	MTD block structure
 * @buf:	raw data read from the chip
 * @read_ecc:	ECC from the chip
 * @calc_ecc:	the ECC calculated from raw data
 *
 * Detect and correct a 1 bit error for 256/512 byte block
 */
int cs752x_hw_nand_correct_data(struct mtd_info *mtd, unsigned char *buf,
		      unsigned char *read_ecc, unsigned char *calc_ecc)
{
	/* 256 or 512 bytes/ecc  */
	/* const uint32_t eccsize_mult = (chip->ecc.size) >> 8;//eccsize >> 8; */
	/* int eccsize = chip->ecc.size */
	/*
	 * b0 to b2 indicate which bit is faulty (if any)
	 * we might need the xor result  more than once,
	 * so keep them in a local var
	 */
	/* middle not yet */

	return 1;
}

/**
 * cs752x_hw_nand_calculate_ecc - [NAND Interface] Calculate 3-byte ECC for 256/512-byte
 *			 block
 * @mtd:	MTD block structure
 * @buf:	input buffer with raw data
 * @code:	output buffer with ECC
 */
int cs752x_hw_nand_calculate_ecc(struct mtd_info *mtd, const unsigned char *buf,
		       unsigned char *code)
{
	/* 256 or 512 bytes/ecc  */
	/* const uint32_t eccsize_mult = */
	/* rp0..rp15..rp17 are the various accumulated parities (per byte) */
	/* middle not yet */

	return 0;
}

static u32 mk_nf_addr( u64 chipsize, u32 page, u32* paddr1, u32* paddr2, u32 oob_offs)
{
	u32 addr_cnt = 0;

	if (chipsize < (32 * SZ_1M)) {
		*paddr1 = ((page & 0x00ffffff) << 8);
		*paddr2 = ((page & 0xff000000) >> 24);
		addr_cnt = NCNT_ADDR_3;
	} else{
		if (((32 * SZ_1M) <= chipsize) && (chipsize <= (128 * SZ_1M))) {
			*paddr1 = (((page & 0xffff) << 16) + (oob_offs & 0xFFFFUL));
			*paddr2 = ((page & 0xffff0000) >> 16);
			addr_cnt = NCNT_ADDR_4;
		} else{
			/* when > SZ_128M */

			*paddr1 = (((page & 0xffff) << 16) + (oob_offs & 0xFFFFUL));
			*paddr2 = ((page & 0xffff0000) >> 16);
			addr_cnt = NCNT_ADDR_5;
		}
	}
	return addr_cnt;
}

static void do_pio_write_buf(void *buf, int bytes)
{
	u32 *ulp = buf;
	int n = bytes / sizeof(u32);
	int i;

	for (i = 0 ; i < n ; i++) {
		nf_access.wrd = 0;
		nf_access.bf.nflashCeAlt = CHIP_EN;
		/* nf_access.bf.nflashDirWr = ; */
		nf_access.bf.nflashRegWidth = NFLASH_WiDTH32;
		fl_writel(FLASH_NF_ACCESS, nf_access.wrd);

		fl_writel(FLASH_NF_DATA, *ulp++);

		flash_start.wrd = 0;
		flash_start.bf.nflashRegReq = FLASH_GO;
		flash_start.bf.nflashRegCmd = FLASH_WT;
		/* flash_start.bf.nflash_random_access = RND_ENABLE; */
		fl_writel(FLASH_FLASH_ACCESS_START, flash_start.wrd);

		for (;;) {
			flash_start.wrd = fl_readl(FLASH_FLASH_ACCESS_START);
			if (flash_start.bf.nflashRegReq == 0) {
				break;
			}
			udelay(1);
			schedule();
		}
	}
}

static void do_pio_read_buf(void *buf, int bytes)
{
	u32 *ulp = buf;
	int n = bytes / sizeof(u32);
	int i;

	for (i = 0 ; i < n ; i++) {
		nf_access.wrd = 0;
		nf_access.bf.nflashCeAlt = CHIP_EN;
		/* nf_access.bf.nflashDirWr = ; */
		nf_access.bf.nflashRegWidth = NFLASH_WiDTH32;
		fl_writel(FLASH_NF_ACCESS, nf_access.wrd);

		flash_start.wrd = 0;
		flash_start.bf.nflashRegReq = FLASH_GO;
		flash_start.bf.nflashRegCmd = FLASH_RD;
		/* flash_start.bf.nflash_random_access = RND_ENABLE; */
		fl_writel(FLASH_FLASH_ACCESS_START, flash_start.wrd);

		for (;;) {
			flash_start.wrd = fl_readl(FLASH_FLASH_ACCESS_START);
			if (flash_start.bf.nflashRegReq == 0) {
				break;
			}
			udelay(1);
			schedule();
		}

		*ulp++ = fl_readl(FLASH_NF_DATA);
	}
}

/**
 * cs752x_nand_write_oob_std - [REPLACABLE] the most common OOB data write function
 * @mtd:	mtd info structure
 * @chip:	nand chip info structure
 * @page:	page number to write
 */
static int cs752x_nand_write_oob_std(struct mtd_info *mtd, struct nand_chip *chip,
			      int page)
{
	int status = 0;
	u32 ul_cmd;
	u32 ul_addr1;
	u32 ul_addr2;
	u32 addr_cnt;
	/* const uint8_t *buf = chip->oob_poi; */
	/* int length = mtd->oobsize; */

	check_flash_ctrl_status();

	chip->cmdfunc(mtd, NAND_CMD_SEQIN, mtd->writesize, page);
	/* chip->write_buf(mtd, buf, length); */
	/* Send command to program the OOB data */
	chip->cmdfunc(mtd, NAND_CMD_PAGEPROG, -1, -1);

	fl_writel(FLASH_NF_ECC_CONTROL, 0x0);	/* disable ecc gen */

	nf_cnt.wrd = 0;
	nf_cnt.bf.nflashRegOobCount = mtd->oobsize - 1;
	nf_cnt.bf.nflashRegDataCount = NCNT_EMPTY_DATA;

	if (chip->chipsize < SZ_32M) {
		if (mtd->writesize > NCNT_512P_DATA) {
			nf_cnt.bf.nflashRegCmdCount	= NCNT_CMD_2;
			ul_cmd = mk_nf_command(NAND_CMD_SEQIN, NAND_CMD_PAGEPROG, 0);
		} else {
			nf_cnt.bf.nflashRegCmdCount	= NCNT_CMD_3;
			ul_cmd = mk_nf_command(NAND_CMD_READOOB, NAND_CMD_SEQIN, NAND_CMD_PAGEPROG);
		}
	} else if (chip->chipsize <= SZ_128M) {
		nf_cnt.bf.nflashRegCmdCount		= NCNT_CMD_2;
		ul_cmd = mk_nf_command(NAND_CMD_SEQIN, NAND_CMD_PAGEPROG, 0);
	} else {		/* if((chip->chipsize > (128 * SZ_1M)) )) */
		nf_cnt.bf.nflashRegCmdCount		= NCNT_CMD_2;
		ul_cmd = mk_nf_command(NAND_CMD_SEQIN, NAND_CMD_PAGEPROG, 0);
	}
	addr_cnt = mk_nf_addr(chip->chipsize, page, &ul_addr1, &ul_addr2, mtd->writesize);
	nf_cnt.bf.nflashRegAddrCount = addr_cnt;

	fl_writel(FLASH_NF_COUNT, nf_cnt.wrd);
	fl_writel(FLASH_NF_COMMAND, ul_cmd);
	fl_writel(FLASH_NF_ADDRESS_1, ul_addr1);
	fl_writel(FLASH_NF_ADDRESS_2, ul_addr2);

	do_pio_write_buf(chip->oob_poi, mtd->oobsize);

	status = chip->waitfunc(mtd, chip);

	return status & NAND_STATUS_FAIL ? -EIO : 0;

}

static void cs752x_do_read_oob(struct mtd_info *mtd, struct nand_chip *chip, int page)
{
	u32 ul_cmd;
	u32 ul_addr1;
	u32 ul_addr2;
	u32 addr_cnt;

	check_flash_ctrl_status();

	fl_writel(FLASH_NF_ECC_CONTROL, 0x0);	/* disable ecc gen */

	nf_cnt.wrd = 0;
	nf_cnt.bf.nflashRegOobCount = mtd->oobsize - 1;
	nf_cnt.bf.nflashRegDataCount = NCNT_EMPTY_DATA;

	if (chip->chipsize < (32 * SZ_1M)) {
		nf_cnt.bf.nflashRegCmdCount	= NCNT_CMD_1;
		if (mtd->writesize > NCNT_512P_DATA)
			ul_cmd = mk_nf_command(NAND_CMD_READ0, 0, 0);
		else
			ul_cmd = mk_nf_command(NAND_CMD_READOOB, 0, 0);
	} else if ((chip->chipsize >= (32 * SZ_1M)) && (chip->chipsize <= (128 * SZ_1M))) {
		nf_cnt.bf.nflashRegCmdCount	= NCNT_CMD_1;
		ul_cmd = mk_nf_command(NAND_CMD_READ0, 0, 0);

		/*  Jeneng */
		if (mtd->writesize > NCNT_512P_DATA) {
			nf_cnt.bf.nflashRegCmdCount = NCNT_CMD_2;
			ul_cmd = mk_nf_command(NAND_CMD_READ0, NAND_CMD_READSTART, 0);
		}
	} else {		/* if((chip->chipsize > (128 * SZ_1M)) )) */
		nf_cnt.bf.nflashRegCmdCount	= NCNT_CMD_2;
		ul_cmd = mk_nf_command(NAND_CMD_READ0, NAND_CMD_READSTART, 0);
	}
	addr_cnt = mk_nf_addr(chip->chipsize, page, &ul_addr1, &ul_addr2, mtd->writesize);
	nf_cnt.bf.nflashRegAddrCount = addr_cnt;

	fl_writel(FLASH_NF_COUNT, nf_cnt.wrd);
	fl_writel(FLASH_NF_COMMAND, ul_cmd);
	fl_writel(FLASH_NF_ADDRESS_1, ul_addr1);
	fl_writel(FLASH_NF_ADDRESS_2, ul_addr2);

	do_pio_read_buf(cs752x_host->own_buf, mtd->oobsize);
}

static void reset_ecc_bch_registers( void )
{
	ecc_reset.wrd = 3;
	ecc_reset.bf.eccClear = ECC_CLR;
	ecc_reset.bf.fifoClear = FIFO_CLR;
	fl_writel(FLASH_NF_ECC_RESET, ecc_reset.wrd);

	flash_int_sts.bf.regIrq = 1;
	fl_writel(FLASH_FLASH_INTERRUPT, flash_int_sts.wrd);

	ecc_reset.wrd = 0;
	ecc_reset.bf.eccClear = 1;
	fl_writel(FLASH_NF_ECC_RESET, ecc_reset.wrd);

	/*  Disable ECC function */
	fl_writel(FLASH_NF_BCH_CONTROL, 0);
	fl_writel(FLASH_NF_ECC_CONTROL, 0);
}

/**
 * cs752x_nand_write_page_raw - [Intern] raw page write function
 * @mtd:	mtd info structure
 * @chip:	nand chip info structure
 * @buf:	data buffer
 *
 * Not for syndrome calculating ecc controllers, which use a special oob layout
 */
static int cs752x_nand_write_page_raw(struct mtd_info *mtd, struct nand_chip *chip,
				const uint8_t *buf, int oob_required, int page)
{
	uint32_t addr;
	u32 ul_cmd;
	u32 ul_addr1;
	u32 ul_addr2;
	u32 addr_cnt;
	uint8_t *vaddr;

///oob_required = true;

	check_flash_ctrl_status();

	page = g_nand_page;

	reset_ecc_bch_registers();

#ifdef CSW_USE_DMA
	/* disable txq5 */
	dma_txq5_ctrl.bf.txq5_en = 0;
	dma_writel(DMA_DMA_SSP_TXQ5_CONTROL, dma_txq5_ctrl.wrd);
	/* clr tx/rx eof */

	dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_TXQ5_INTERRUPT, dma_ssp_txq5_intsts.wrd);
	dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_RXQ5_INTERRUPT, dma_ssp_rxq5_intsts.wrd);
#endif
	nf_cnt.wrd = 0;
	if (oob_required)
		nf_cnt.bf.nflashRegOobCount = mtd->oobsize - 1;
	nf_cnt.bf.nflashRegDataCount = mtd->writesize - 1;

	if (chip->chipsize < (32 * SZ_1M)) {
		nf_cnt.bf.nflashRegCmdCount = NCNT_CMD_2;
		ul_cmd = mk_nf_command(NAND_CMD_SEQIN, NAND_CMD_PAGEPROG, 0);
	} else if ((chip->chipsize >= (32 * SZ_1M)) && (chip->chipsize <= (128 * SZ_1M))) {
		nf_cnt.bf.nflashRegCmdCount = NCNT_CMD_2;
		ul_cmd = mk_nf_command(NAND_CMD_SEQIN, NAND_CMD_PAGEPROG, 0);
	} else {		/* if((chip->chipsize > (128 * SZ_1M)) )) */
		nf_cnt.bf.nflashRegCmdCount = NCNT_CMD_2;
		ul_cmd = mk_nf_command(NAND_CMD_SEQIN, NAND_CMD_PAGEPROG, 0);
	}
	addr_cnt = mk_nf_addr(chip->chipsize, page, &ul_addr1, &ul_addr2, 0);
	nf_cnt.bf.nflashRegAddrCount = addr_cnt;

	fl_writel(FLASH_NF_COUNT, nf_cnt.wrd);
	fl_writel(FLASH_NF_COMMAND, ul_cmd);
	fl_writel(FLASH_NF_ADDRESS_1, ul_addr1);
	fl_writel(FLASH_NF_ADDRESS_2, ul_addr2);

	nf_access.wrd = 0;
	nf_access.bf.nflashCeAlt = CHIP_EN;
	nf_access.bf.nflashRegWidth = NFLASH_WiDTH8;
	nf_access.bf.nflashExtAddr = ((page << chip->page_shift) / SZ_128M);
	fl_writel(FLASH_NF_ACCESS, nf_access.wrd);

#ifdef CSW_USE_DMA
	addr = cs752x_host->dma_phy_base + ((page << chip->page_shift) % SZ_128M);

#if 0	/* trial: removed */
	/* The fifo depth is 64 bytes. We have a sync at each frame and frame
	 * length is 64 bytes. --> for vmalloc not kmalloc
	 */
	vaddr = 0;
	if (buf >= high_memory) {
		struct page *p1;

		if (((size_t)buf & PAGE_MASK) !=
			((size_t)(buf + mtd->writesize - 1) & PAGE_MASK))
			goto out_copy;
		p1 = vmalloc_to_page(buf);
		if (!p1)
			goto out_copy;
		buf = page_address(p1) + ((size_t)buf & ~PAGE_MASK);

	}
	goto out_copy_done;
#endif	/* trial */

out_copy:
	vaddr = buf;
	chip->pagebuf = -1;
	buf = chip->buffers->databuf;
	memcpy(buf, vaddr, mtd->writesize);
out_copy_done:

	/* page data tx desc */
	dma_txq5_wptr.wrd = dma_readl(DMA_DMA_SSP_TXQ5_WPTR);
	tx_desc[dma_txq5_wptr.bf.index].word0.bf.own = OWN_DMA;
	tx_desc[dma_txq5_wptr.bf.index].word0.bf.buf_size = mtd->writesize;
	tx_desc[dma_txq5_wptr.bf.index].buf_adr =
	    dma_map_single(NULL, (void *)buf, mtd->writesize, DMA_TO_DEVICE);

	dma_rxq5_rptr.wrd = dma_readl(DMA_DMA_SSP_RXQ5_RPTR);
	rx_desc[dma_rxq5_rptr.bf.index].word0.bf.own = OWN_DMA;
	rx_desc[dma_rxq5_rptr.bf.index].word0.bf.buf_size = mtd->writesize;
	rx_desc[dma_rxq5_rptr.bf.index].buf_adr = addr;

	if (oob_required) {
		/*
		 * build oob rx desc
		 */
		addr = (unsigned int *)((unsigned int)addr + mtd->writesize);
		/* printk("  oob : addr(%p)  chip->oob_poi(%p) \n",addr, chip->oob_poi); */

		dma_rxq5_rptr.bf.index = (dma_rxq5_rptr.bf.index + 1) % FDMA_DESC_NUM;
		rx_desc[dma_rxq5_rptr.bf.index].word0.bf.own = OWN_DMA;
		rx_desc[dma_rxq5_rptr.bf.index].word0.bf.buf_size = mtd->oobsize;
		rx_desc[dma_rxq5_rptr.bf.index].buf_adr = addr;
	}

	wmb();
	dummy = rx_desc[dma_rxq5_rptr.bf.index].word0.wrd;

	/* update page tx write ptr */
	dma_txq5_wptr.bf.index = (dma_txq5_wptr.bf.index + 1) % FDMA_DESC_NUM;
	dma_writel(DMA_DMA_SSP_TXQ5_WPTR, dma_txq5_wptr.wrd);

	/* set axi_bus_len = 8 */
	/* set fifo control */
	fifo_ctl.wrd = 0;
	fifo_ctl.bf.fifoCmd = FLASH_WT;
	fl_writel(FLASH_NF_FIFO_CONTROL, fifo_ctl.wrd);

	flash_start.wrd = 0;
	flash_start.bf.fifoReq = FLASH_GO;
	/* flash_start.bf.nflashRegCmd = FLASH_WT; */
	fl_writel(FLASH_FLASH_ACCESS_START, flash_start.wrd);

	/* enable txq5 */
	dma_txq5_ctrl.bf.txq5_en = 1;
	dma_writel(DMA_DMA_SSP_TXQ5_CONTROL, dma_txq5_ctrl.wrd);

	dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	while (!dma_ssp_rxq5_intsts.bf.rxq5_eof) {
		udelay(1);
		schedule();
		dma_ssp_rxq5_intsts.wrd =
		    dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	}

	dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	while (!dma_ssp_txq5_intsts.bf.txq5_eof) {
		udelay(1);
		schedule();
		dma_ssp_txq5_intsts.wrd =
		    dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	}

	if (!oob_required)
		goto l_skip_oob_stage;

	/* clr tx/rx eof */
	dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_TXQ5_INTERRUPT, dma_ssp_txq5_intsts.wrd);
	dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_RXQ5_INTERRUPT, dma_ssp_rxq5_intsts.wrd);

#ifdef	CONFIG_CS752X_NAND_ECC_HW_BCH
	/* jenfeng clear erase tag */
	{
	struct mtd_oob_region region;
	mtd_ooblayout_free(mtd, BCH_ERASE_TAG_SECTION, &region);
	chip->oob_poi[region.offset + region.length] = 0;
	}
#endif

	/* dma_txq5_wptr.wrd = dma_readl(DMA_DMA_SSP_TXQ5_WPTR); */
	tx_desc[dma_txq5_wptr.bf.index].word0.bf.own = OWN_DMA;
	tx_desc[dma_txq5_wptr.bf.index].word0.bf.buf_size = mtd->oobsize;
	tx_desc[dma_txq5_wptr.bf.index].buf_adr =
	    dma_map_single(NULL, (void *)chip->oob_poi, mtd->oobsize,
			   DMA_TO_DEVICE);

	wmb();
	dummy = tx_desc[dma_txq5_wptr.bf.index].word0.wrd;

	/* update tx write ptr */
	dma_txq5_wptr.bf.index = (dma_txq5_wptr.bf.index + 1) % FDMA_DESC_NUM;
	dma_writel(DMA_DMA_SSP_TXQ5_WPTR, dma_txq5_wptr.wrd);

	dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	while (!dma_ssp_rxq5_intsts.bf.rxq5_eof) {
		udelay(1);
		schedule();
		dma_ssp_rxq5_intsts.wrd =
		    dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	}
	dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	while (!dma_ssp_txq5_intsts.bf.txq5_eof) {
		udelay(1);
		schedule();
		dma_ssp_txq5_intsts.wrd =
		    dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	}

  l_skip_oob_stage:;

	flash_start.wrd = fl_readl(FLASH_FLASH_ACCESS_START);
	while (flash_start.bf.fifoReq) {
		udelay(1);
		schedule();
		flash_start.wrd = fl_readl(FLASH_FLASH_ACCESS_START);
	}

	/* update rx read ptr */
	/* dma_rxq5_rptr.wrd = dma_readl(DMA_DMA_SSP_RXQ5_RPTR); */
	dma_rxq5_rptr.bf.index = (dma_rxq5_rptr.bf.index + 1) % FDMA_DESC_NUM;
	dma_writel(DMA_DMA_SSP_RXQ5_RPTR, dma_rxq5_rptr.wrd);

	/* clr tx/rx eof */
	dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_TXQ5_INTERRUPT, dma_ssp_txq5_intsts.wrd);
	dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_RXQ5_INTERRUPT, dma_ssp_rxq5_intsts.wrd);

#else	/* CSW_USE_DMA */

	do_pio_write_buf((void*)buf, mtd->writesize);
	if (oob_required)
		do_pio_write_buf(chip->oob_poi, mtd->oobsize);

#endif	/* CSW_USE_DMA */

	return 0;
}

/*
 */
static int noinline cs752x_nand_read_page(struct mtd_info *mtd, struct nand_chip *chip,
			      uint8_t *buf, int oob_required, int page)
{
	unsigned int addr;
	u32 ul_cmd;
	u32 ul_addr1;
	u32 ul_addr2;
	u32 addr_cnt;
	uint8_t *vaddr;

///oob_required = true;

#ifdef CSW_USE_DMA

	/* disable txq5 */
	dma_txq5_ctrl.bf.txq5_en = 0;
	dma_writel(DMA_DMA_SSP_TXQ5_CONTROL, dma_txq5_ctrl.wrd);

	/* clr tx/rx eof */
	dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_TXQ5_INTERRUPT, dma_ssp_txq5_intsts.wrd);
	dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_RXQ5_INTERRUPT, dma_ssp_rxq5_intsts.wrd);

#endif	/* CSW_USE_DMA */

	/* for indirect access with DMA, because DMA not ready  */
	nf_cnt.wrd = 0;
	if (oob_required)
		nf_cnt.bf.nflashRegOobCount = mtd->oobsize - 1;
	nf_cnt.bf.nflashRegDataCount = mtd->writesize - 1;

	if (chip->chipsize < SZ_32M) {
		nf_cnt.bf.nflashRegCmdCount	= NCNT_CMD_1;
		ul_cmd = mk_nf_command(NAND_CMD_READ0, 0, 0);
	} else if (chip->chipsize <= SZ_128M) {
		nf_cnt.bf.nflashRegCmdCount	= NCNT_CMD_1;
		ul_cmd = mk_nf_command(NAND_CMD_READ0, 0, 0);

		if (mtd->writesize > NCNT_512P_DATA) {
			nf_cnt.bf.nflashRegCmdCount = NCNT_CMD_2;
			ul_cmd = mk_nf_command(NAND_CMD_READ0, NAND_CMD_READSTART, 0);
		}
	} else {		/* if((chip->chipsize > SZ_128M ) )) */
		nf_cnt.bf.nflashRegCmdCount	= NCNT_CMD_2;
		ul_cmd = mk_nf_command(NAND_CMD_READ0, NAND_CMD_READSTART, 0);
	}
	addr_cnt = mk_nf_addr(chip->chipsize, page, &ul_addr1, &ul_addr2, 0);
	nf_cnt.bf.nflashRegAddrCount = addr_cnt;

	fl_writel(FLASH_NF_COUNT, nf_cnt.wrd);
	fl_writel(FLASH_NF_COMMAND, ul_cmd);		/* write read id command */
	fl_writel(FLASH_NF_ADDRESS_1, ul_addr1);	/* write address 0x0 */
	fl_writel(FLASH_NF_ADDRESS_2, ul_addr2);

	nf_cnt.wrd = fl_readl(FLASH_NF_COUNT);

	nf_access.wrd = 0;
	nf_access.bf.nflashCeAlt = CHIP_EN;
	nf_access.bf.nflashRegWidth = NFLASH_WiDTH8;
	nf_access.bf.nflashExtAddr = ((page << chip->page_shift) / SZ_128M);
	fl_writel(FLASH_NF_ACCESS, nf_access.wrd);

	nf_access.wrd = fl_readl(FLASH_NF_ACCESS);

#ifdef CSW_USE_DMA
	addr = cs752x_host->dma_phy_base + ((page << chip->page_shift) % SZ_128M);

#if 0	/* trial: removed */
	/* The fifo depth is 64 bytes. We have a sync at each frame and frame
	 * length is 64 bytes. --> for vmalloc not kmalloc
	 */
	vaddr = 0;
	if (buf >= high_memory) {
		struct page *p1;

		if (((size_t)buf & PAGE_MASK) !=
			((size_t)(buf + mtd->writesize - 1) & PAGE_MASK))
			goto out_copy;
		p1 = vmalloc_to_page(buf);
		if (!p1)
			goto out_copy;
		buf = page_address(p1) + ((size_t)buf & ~PAGE_MASK);
	}
	goto out_copy_done;
#endif	/* trial */

out_copy:
	vaddr = buf;
	chip->pagebuf = -1;
	buf = chip->buffers->databuf;
out_copy_done:

	dma_txq5_wptr.wrd = dma_readl(DMA_DMA_SSP_TXQ5_WPTR);

	tx_desc[dma_txq5_wptr.bf.index].word0.bf.own = OWN_DMA;
	tx_desc[dma_txq5_wptr.bf.index].word0.bf.buf_size = mtd->writesize;
	tx_desc[dma_txq5_wptr.bf.index].buf_adr = addr;

	/* page data rx desc */
	dma_rxq5_rptr.wrd = dma_readl(DMA_DMA_SSP_RXQ5_RPTR);
	rx_desc[dma_rxq5_rptr.bf.index].word0.bf.own = OWN_DMA;
	rx_desc[dma_rxq5_rptr.bf.index].word0.bf.buf_size = mtd->writesize;

	rx_desc[dma_rxq5_rptr.bf.index].buf_adr =
	    dma_map_single(NULL, (void *)buf, mtd->writesize, DMA_FROM_DEVICE);

	wmb();
	dummy = rx_desc[dma_rxq5_rptr.bf.index].word0.wrd;
	/* set axi_bus_len = 8 */

	/* set fifo control */
	fifo_ctl.wrd = 0;
	fifo_ctl.bf.fifoCmd = FLASH_RD;
	fl_writel(FLASH_NF_FIFO_CONTROL, fifo_ctl.wrd);

	flash_start.wrd = 0;
	flash_start.bf.fifoReq = FLASH_GO;
	/* flash_start.bf.nflashRegCmd = FLASH_RD; */
	fl_writel(FLASH_FLASH_ACCESS_START, flash_start.wrd);

	/* update tx write ptr */
	dma_txq5_wptr.bf.index = (dma_txq5_wptr.bf.index + 1) % FDMA_DESC_NUM;
	dma_writel(DMA_DMA_SSP_TXQ5_WPTR, dma_txq5_wptr.wrd);
	/* dma_txq5_wptr.wrd = dma_readl(DMA_DMA_SSP_TXQ5_WPTR); */

	/* enable txq5 */
	dma_txq5_ctrl.bf.txq5_en = 1;
	dma_writel(DMA_DMA_SSP_TXQ5_CONTROL, dma_txq5_ctrl.wrd);

	dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);

	while (!dma_ssp_rxq5_intsts.bf.rxq5_eof ) { //444 + 2
		udelay(1);
		schedule();
		dma_ssp_rxq5_intsts.wrd =
		    dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	}
	dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	while (!dma_ssp_txq5_intsts.bf.txq5_eof  ) { //46c +2
		udelay(1);
		schedule();
		dma_ssp_txq5_intsts.wrd =
		    dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	}

	if (!oob_required)
		goto l_skip_oob_stage;

	/* clr tx/rx eof */
	dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_TXQ5_INTERRUPT, dma_ssp_txq5_intsts.wrd);
	dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_RXQ5_INTERRUPT, dma_ssp_rxq5_intsts.wrd);

	dma_map_single(NULL, (void *)buf, mtd->writesize, DMA_FROM_DEVICE);
	wmb();

	if (vaddr != 0) {
		memcpy(vaddr, buf, mtd->writesize);
        }

	/******************************************************/

	/* oob tx desc */
	//dma_txq5_wptr.bf.index = (dma_txq5_wptr.bf.index + 1) % FDMA_DESC_NUM;

	//addr +=  mtd->writesize;
	tx_desc[dma_txq5_wptr.bf.index].word0.bf.own = OWN_DMA;
	tx_desc[dma_txq5_wptr.bf.index].word0.bf.buf_size = mtd->oobsize;
	tx_desc[dma_txq5_wptr.bf.index].buf_adr = (addr + mtd->writesize);

	/* oob rx desc */
	dma_rxq5_rptr.bf.index = (dma_rxq5_rptr.bf.index + 1) % FDMA_DESC_NUM;
	rx_desc[dma_rxq5_rptr.bf.index].word0.bf.own = OWN_DMA;
	rx_desc[dma_rxq5_rptr.bf.index].word0.bf.buf_size = mtd->oobsize;
	rx_desc[dma_rxq5_rptr.bf.index].buf_adr =
	    dma_map_single(NULL, (void *)chip->oob_poi, mtd->oobsize,
			   DMA_FROM_DEVICE);

	wmb();
	dummy = rx_desc[dma_rxq5_rptr.bf.index].word0.wrd;
	/* set axi_bus_len = 8 */

	/* update tx write ptr */
	dma_txq5_wptr.bf.index = (dma_txq5_wptr.bf.index + 1) % FDMA_DESC_NUM;
	dma_writel(DMA_DMA_SSP_TXQ5_WPTR, dma_txq5_wptr.wrd);
	/* dma_txq5_wptr.wrd = dma_readl(DMA_DMA_SSP_TXQ5_WPTR); */

	dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);

	while (!dma_ssp_rxq5_intsts.bf.rxq5_eof ) { //444 + 2
		udelay(1);
		schedule();
		dma_ssp_rxq5_intsts.wrd =
		    dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	}
	dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	while (!dma_ssp_txq5_intsts.bf.txq5_eof  ) { //46c +2
		udelay(1);
		schedule();
		dma_ssp_txq5_intsts.wrd =
		    dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	}

  l_skip_oob_stage:;

	flash_start.wrd = fl_readl(FLASH_FLASH_ACCESS_START);
	while (flash_start.bf.fifoReq) {
		udelay(1);
		schedule();
		flash_start.wrd = fl_readl(FLASH_FLASH_ACCESS_START);
	}

	/* clr tx/rx eof */
	dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_TXQ5_INTERRUPT, dma_ssp_txq5_intsts.wrd);
	dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_RXQ5_INTERRUPT, dma_ssp_rxq5_intsts.wrd);

	dma_rxq5_rptr.bf.index = (dma_rxq5_rptr.bf.index + 1) % FDMA_DESC_NUM;
	dma_writel(DMA_DMA_SSP_RXQ5_RPTR, dma_rxq5_rptr.wrd);

	dma_map_single(NULL, (void *)chip->oob_poi, mtd->oobsize, DMA_FROM_DEVICE);
	wmb();

#else	/* CSW_USE_DMA */

	do_pio_read_buf(buf, mtd->writesize);
	if (oob_required)
		do_pio_read_buf(chip->oob_poi, mtd->oobsize);

#endif	/* CSW_USE_DMA */

	return 0;
}

/**
 * cs752x_nand_read_page_raw - [Intern] read raw page data without ecc
 * @mtd:	mtd info structure
 * @chip:	nand chip info structure
 * @buf:	buffer to store read data
 * @page:	page number to read
 *
 * Not for syndrome calculating ecc controllers, which use a special oob layout
 */
static int cs752x_nand_read_page_raw(struct mtd_info *mtd, struct nand_chip *chip,
			      uint8_t *buf, int oob_required, int page)
{
///oob_required = true;

	check_flash_ctrl_status();

	ecc_reset.wrd = 0;
	ecc_reset.bf.eccClear	= ECC_CLR;
	ecc_reset.bf.fifoClear	= FIFO_CLR;
	fl_writel(FLASH_NF_ECC_RESET, ecc_reset.wrd);

	flash_int_sts.bf.regIrq = 1;
	fl_writel(FLASH_FLASH_INTERRUPT, flash_int_sts.wrd);

	ecc_reset.wrd		= 0;
	ecc_reset.bf.eccClear	= 1;
	fl_writel(FLASH_NF_ECC_RESET, ecc_reset.wrd);

	bch_ctrl.wrd = 0;
	fl_writel(FLASH_NF_BCH_CONTROL, bch_ctrl.wrd);

	ecc_ctl.wrd = 0;
	fl_writel(FLASH_NF_ECC_CONTROL, ecc_ctl.wrd);

	return cs752x_nand_read_page(mtd, chip, buf, oob_required, page);
}

#ifdef CONFIG_CS752X_NAND_ECC_HW_BCH

static void fill_bch_oob_data(struct mtd_info *mtd, struct nand_chip *chip)
{
	int i;
	int j;
	int k;
	int tail_offs;
	int eccsteps;
	u32 addr;
	u32 ul_bch_gen00;
	u8* ecc_calc = chip->buffers->ecccalc;
	const u32 reg_offset = FLASH_NF_BCH_GEN0_1 - FLASH_NF_BCH_GEN0_0;
	const u32 group_offset = FLASH_NF_BCH_GEN1_0 - FLASH_NF_BCH_GEN0_0;

	addr = FLASH_NF_BCH_GEN0_0;
	k = 0;

	for (eccsteps = chip->ecc.steps ; eccsteps ; --eccsteps, addr += group_offset) {
		tail_offs = k + chip->ecc.bytes;

		for (i = 0 ; k < tail_offs ; ++i) {
			ul_bch_gen00 = fl_readl(addr + reg_offset * i);

			for (j = 0 ; (j < 4) && (k < tail_offs); ++j, ++k) {
				ecc_calc[k] = ((ul_bch_gen00 >> (j * 8)) & 0xff);
			}
		}
	}
	mtd_ooblayout_set_eccbytes(mtd, ecc_calc, chip->oob_poi, 0, chip->ecc.total);

	/* erase tag */
	{
	struct mtd_oob_region region;
	mtd_ooblayout_free(mtd, BCH_ERASE_TAG_SECTION, &region);
	chip->oob_poi[region.offset + region.length] = 0;
	}
}

static void bch_correct(struct mtd_info *mtd, struct nand_chip *chip, uint8_t *buf)
{
	int i;
	int j;
	int k;
	int m;
	int tail_offs;
	int eccsteps;
	int eccbytes;
	int eccsize;
	u8* ecc_code = chip->buffers->ecccode;
	uint8_t *p = buf;
	u32 ul_bch_oob0;

	mtd_ooblayout_get_eccbytes(mtd, ecc_code, chip->oob_poi, 0, chip->ecc.total);

	eccsteps = chip->ecc.steps;
	eccbytes = chip->ecc.bytes;
	eccsize = chip->ecc.size;

	m = 0;
	for (i = 0 ; eccsteps ; eccsteps--, i += eccbytes, p += eccsize) {
		tail_offs = m + chip->ecc.bytes;

		for (j = 0 ; j < chip->ecc.bytes ; j += 4) {
			ul_bch_oob0 = 0;
			for (k = 0 ; (k < 4) && (m < tail_offs) ; ++k, ++m) {
				ul_bch_oob0 |= ecc_code[m] << (8 * k);
			}

			fl_writel(FLASH_NF_BCH_OOB0 + j, ul_bch_oob0);
		}

		/* enable ecc compare */
		bch_ctrl.wrd = fl_readl(FLASH_NF_BCH_CONTROL);
		bch_ctrl.bf.bchCodeSel = (i / eccbytes);
		bch_ctrl.bf.bchCompare = 1;
		fl_writel(FLASH_NF_BCH_CONTROL, bch_ctrl.wrd);

		bch_sts.wrd = fl_readl(FLASH_NF_BCH_STATUS);
		while (!bch_sts.bf.bchDecDone) {
			udelay(1);
			schedule();
			bch_sts.wrd = fl_readl(FLASH_NF_BCH_STATUS);
		}

		switch (bch_sts.bf.bchDecStatus) {
		case BCH_CORRECTABLE_ERR:
			for (j = 0 ; j < ((bch_sts.bf.bchErrNum + 1) / 2); j++) {
				bch_err_loc01.wrd = fl_readl(FLASH_NF_BCH_ERROR_LOC01 + j * 4);

				if ((j + 1) * 2 <= bch_sts.bf.bchErrNum) {
					if (((bch_err_loc01.bf.bchErrLoc1 & 0x1fff) >> 3) < 0x200) {
						p[(bch_err_loc01.bf.bchErrLoc1 & 0x1fff) >> 3] ^=
							(1 << (bch_err_loc01.bf.bchErrLoc1 & 0x07));
					}
				}

				if (((bch_err_loc01.bf.bchErrLoc0 & 0x1fff) >> 3) < 0x200) {
					p[(bch_err_loc01.bf.bchErrLoc0 & 0x1fff) >> 3] ^=
							(1 << (bch_err_loc01.bf.bchErrLoc0 & 0x07));
				}
			}
			break;
		case BCH_UNCORRECTABLE:
			printk("g_nand_page :%x, uncorrectable error!!\n",
			       g_nand_page);
			mtd->ecc_stats.failed++;
			break;
		}

		/* disable ecc compare */
		bch_ctrl.wrd = fl_readl(FLASH_NF_BCH_CONTROL);
		bch_ctrl.bf.bchCompare = 0;
		fl_writel(FLASH_NF_BCH_CONTROL, bch_ctrl.wrd);
	}
}
#endif	/* CONFIG_CS752X_NAND_ECC_HW_BCH */

#ifdef CONFIG_CS752X_NAND_ECC_HW_HAMMING

static void fill_hamming_oob_data(struct nand_chip *chip, struct mtd_info *mtd)
{
	u32 i, j;
	u32 *eccpos = chip->ecc.layout->eccpos;
	int eccsteps = chip->ecc.steps;
	int eccbytes = chip->ecc.bytes;
	u32 ul_ecc_gen0;
	u8 *ecc_calc = chip->buffers->ecccalc;

	for (i = 0, j = 0; eccsteps; eccsteps--, i++, j += eccbytes) {
		ul_ecc_gen0 = fl_readl(FLASH_NF_ECC_GEN0 + 4 * i);
		ecc_calc[j]	= ul_ecc_gen0 & 0xff;
		ecc_calc[j + 1]	= (ul_ecc_gen0 >> 8) & 0xff;
		ecc_calc[j + 2]	= (ul_ecc_gen0 >> 16) & 0xff;
	}
	mtd_ooblayout_set_eccbytes(mtd, ecc_calc, chip->ecc_poi, 0, chip->ecc.total);
}
#endif	/* CONFIG_CS752X_NAND_ECC_HW_HAMMING */

static void configure_hwecc_reg(int is_write)
{
#if defined( CONFIG_CS752X_NAND_ECC_HW_BCH_8_512 )

	bch_ctrl.wrd = 0;
	bch_ctrl.bf.bchEn = BCH_ENABLE;
	bch_ctrl.bf.bchOpcode = is_write ? BCH_ENCODE : BCH_DECODE;
	bch_ctrl.bf.bchErrCap = BCH_ERR_CAP_8_512;
	fl_writel(FLASH_NF_BCH_CONTROL, bch_ctrl.wrd);

#elif  defined( CONFIG_CS752X_NAND_ECC_HW_BCH_12_512 )

	bch_ctrl.wrd = 0;
	bch_ctrl.bf.bchEn = BCH_ENABLE;
	bch_ctrl.bf.bchOpcode = is_write ? BCH_ENCODE : BCH_DECODE;
	bch_ctrl.bf.bchErrCap = BCH_ERR_CAP_12_512;
	fl_writel(FLASH_NF_BCH_CONTROL, bch_ctrl.wrd);

#elif defined( CONFIG_CS752X_NAND_ECC_HW_HAMMING_512 )

	ecc_ctl.wrd = 0;
	ecc_ctl.bf.eccGenMode = ECC_GEN_512;
	ecc_ctl.bf.eccEn = ECC_ENABLE;
	fl_writel(FLASH_NF_ECC_CONTROL, ecc_ctl.wrd);

#elif defined( CONFIG_CS752X_NAND_ECC_HW_HAMMING_256 )

	ecc_ctl.wrd = 0;
	ecc_ctl.bf.eccGenMode = ECC_GEN_256;
	ecc_ctl.bf.eccEn = ECC_ENABLE;
	fl_writel(FLASH_NF_ECC_CONTROL, ecc_ctl.wrd);

#endif
}

/**
 * cs752x_nand_write_page_hwecc - [REPLACABLE] hardware ecc based page write function
 * @mtd:	mtd info structure
 * @chip:	nand chip info structure
 * @buf:	data buffer
 */
static int cs752x_nand_write_page_hwecc(struct mtd_info *mtd, struct nand_chip *chip,
				  const uint8_t *buf, int oob_required, int page)
{
	int col;
	uint32_t addr;
	u32	ul_cmd;
	u32	ul_addr1;
	u32	ul_addr2;
	u32	addr_cnt;
	uint8_t *vaddr;

	page = g_nand_page;
	col = g_nand_col;

	check_flash_ctrl_status();
	reset_ecc_bch_registers();

#ifdef CSW_USE_DMA
	dma_txq5_ctrl.bf.txq5_en = 0;
	dma_writel(DMA_DMA_SSP_TXQ5_CONTROL, dma_txq5_ctrl.wrd);

	/* clr tx/rx eof */
	dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_TXQ5_INTERRUPT, dma_ssp_txq5_intsts.wrd);
	dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_RXQ5_INTERRUPT, dma_ssp_rxq5_intsts.wrd);
#endif
	configure_hwecc_reg( true );

	nf_cnt.wrd = 0;
	nf_cnt.bf.nflashRegOobCount = mtd->oobsize - 1;
	nf_cnt.bf.nflashRegDataCount = mtd->writesize - 1;

	addr_cnt = mk_nf_addr(chip->chipsize, page, &ul_addr1, &ul_addr2, 0);
	nf_cnt.bf.nflashRegAddrCount = addr_cnt;

	ul_cmd = mk_nf_command(NAND_CMD_SEQIN, NAND_CMD_PAGEPROG, 0);
	nf_cnt.bf.nflashRegCmdCount	= NCNT_CMD_2;

	fl_writel(FLASH_NF_COUNT, nf_cnt.wrd);
	fl_writel(FLASH_NF_COMMAND, ul_cmd);
	fl_writel(FLASH_NF_ADDRESS_1, ul_addr1);
	fl_writel(FLASH_NF_ADDRESS_2, ul_addr2);

	nf_access.wrd = 0;
	nf_access.bf.nflashCeAlt = CHIP_EN;
	nf_access.bf.nflashRegWidth = NFLASH_WiDTH8;
	nf_access.bf.nflashExtAddr = ((page << chip->page_shift) / SZ_128M);
	fl_writel(FLASH_NF_ACCESS, nf_access.wrd);

#ifdef CSW_USE_DMA
	addr = cs752x_host->dma_phy_base + ((page << chip->page_shift) % SZ_128M);

#if 0	/* trial: removed */

	/* The fifo depth is 64 bytes. We have a sync at each frame and frame
	 * length is 64 bytes. --> for vmalloc not kmalloc
	 */
	vaddr = 0;
	if (buf >= high_memory) {
		struct page *p1;

		if (((size_t)buf & PAGE_MASK) != ((size_t)(buf + mtd->writesize - 1) & PAGE_MASK))
			goto out_copy;
		p1 = vmalloc_to_page(buf);
		if (!p1)
			goto out_copy;
		buf = page_address(p1) + ((size_t)buf & ~PAGE_MASK);

	}
	goto out_copy_done;
#endif	/* trial */

out_copy:
	vaddr = buf;
	chip->pagebuf = -1;
	buf = chip->buffers->databuf;
	memcpy((void*)buf, vaddr, mtd->writesize);
out_copy_done:

	/* page data tx desc */
	dma_txq5_wptr.wrd = dma_readl(DMA_DMA_SSP_TXQ5_WPTR);
	tx_desc[dma_txq5_wptr.bf.index].word0.bf.own = OWN_DMA;
	tx_desc[dma_txq5_wptr.bf.index].word0.bf.buf_size = mtd->writesize;
	tx_desc[dma_txq5_wptr.bf.index].buf_adr =
	    dma_map_single(NULL, (void *)buf, mtd->writesize, DMA_TO_DEVICE);

	dma_rxq5_rptr.wrd = dma_readl(DMA_DMA_SSP_RXQ5_RPTR);
	rx_desc[dma_rxq5_rptr.bf.index].word0.bf.own = OWN_DMA;
	rx_desc[dma_rxq5_rptr.bf.index].word0.bf.buf_size = mtd->writesize;
	rx_desc[dma_rxq5_rptr.bf.index].buf_adr = addr;

	/* update page tx write ptr */
	dma_txq5_wptr.bf.index = (dma_txq5_wptr.bf.index + 1) % FDMA_DESC_NUM;
	dma_writel(DMA_DMA_SSP_TXQ5_WPTR, dma_txq5_wptr.wrd);
	/* set axi_bus_len = 8 */
	/* set fifo control */
	fifo_ctl.wrd = 0;
	fifo_ctl.bf.fifoCmd = FLASH_WT;
	fl_writel(FLASH_NF_FIFO_CONTROL, fifo_ctl.wrd);

	wmb();
	dummy = rx_desc[dma_rxq5_rptr.bf.index].word0.wrd;

	flash_start.wrd = 0;
	flash_start.bf.fifoReq = FLASH_GO;
	/* flash_start.bf.nflashRegCmd = FLASH_WT; */
	fl_writel(FLASH_FLASH_ACCESS_START, flash_start.wrd);

	/* enable txq5 */
	dma_txq5_ctrl.bf.txq5_en = 1;
	dma_writel(DMA_DMA_SSP_TXQ5_CONTROL, dma_txq5_ctrl.wrd);

	dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	while (!dma_ssp_rxq5_intsts.bf.rxq5_eof) {
		udelay(1);
		schedule();
		dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	}

	dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	while (!dma_ssp_txq5_intsts.bf.txq5_eof) {
		udelay(1);
		schedule();
		dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	}

	/* clr tx/rx eof */
	dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_TXQ5_INTERRUPT, dma_ssp_txq5_intsts.wrd);
	dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_RXQ5_INTERRUPT, dma_ssp_rxq5_intsts.wrd);

#else	/* CSW_USE_DMA */

	do_pio_write_buf((void*)buf, mtd->writesize);

#endif	/* CSW_USE_DMA */

#ifdef	CONFIG_CS752X_NAND_ECC_HW_BCH
	bch_sts.wrd = fl_readl(FLASH_NF_BCH_STATUS);
	while (!bch_sts.bf.bchGenDone) {
		udelay(1);
		schedule();
		bch_sts.wrd = fl_readl(FLASH_NF_BCH_STATUS);
	}
	bch_ctrl.wrd = fl_readl(FLASH_NF_BCH_CONTROL);	/* disable ecc gen */
	bch_ctrl.bf.bchEn = BCH_DISABLE;
	fl_writel(FLASH_NF_BCH_CONTROL, bch_ctrl.wrd);
#else
	ecc_sts.wrd = fl_readl(FLASH_NF_ECC_STATUS);
	while (!ecc_sts.bf.eccDone) {
		udelay(1);
		schedule();
		ecc_sts.wrd = fl_readl(FLASH_NF_ECC_STATUS);
	}

	ecc_ctl.wrd = fl_readl(FLASH_NF_ECC_CONTROL);
	ecc_ctl.bf.eccEn = 0;
	fl_writel(FLASH_NF_ECC_CONTROL, ecc_ctl.wrd);	/* disable ecc gen */
#endif

#if defined( CONFIG_CS752X_NAND_ECC_HW_BCH )
	fill_bch_oob_data(mtd, chip);
#else
	fill_hamming_oob_data(chip, mtd);
#endif

#ifdef CSW_USE_DMA
	/* oob rx desc */
	//addr +=  mtd->writesize;

	dma_rxq5_rptr.bf.index = (dma_rxq5_rptr.bf.index + 1) % FDMA_DESC_NUM;
	rx_desc[dma_rxq5_rptr.bf.index].word0.bf.own = OWN_DMA;
	rx_desc[dma_rxq5_rptr.bf.index].word0.bf.buf_size = mtd->oobsize;
	rx_desc[dma_rxq5_rptr.bf.index].buf_adr = (addr + mtd->writesize);

	/* dma_txq5_wptr.wrd = dma_readl(DMA_DMA_SSP_TXQ5_WPTR); */
	tx_desc[dma_txq5_wptr.bf.index].word0.bf.own = OWN_DMA;
	tx_desc[dma_txq5_wptr.bf.index].word0.bf.buf_size = mtd->oobsize;
	tx_desc[dma_txq5_wptr.bf.index].buf_adr =
	    dma_map_single(NULL, (void *)chip->oob_poi, mtd->oobsize, DMA_TO_DEVICE);

	wmb();
	dummy = tx_desc[dma_txq5_wptr.bf.index].word0.wrd;

	/* dma_cache_sync(NULL, chip->oob_poi, mtd->oobsize, DMA_BIDIRECTIONAL); */
	/* update tx write ptr */
	dma_txq5_wptr.bf.index = (dma_txq5_wptr.bf.index + 1) % FDMA_DESC_NUM;
	dma_writel(DMA_DMA_SSP_TXQ5_WPTR, dma_txq5_wptr.wrd);

	dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	while (!dma_ssp_rxq5_intsts.bf.rxq5_eof) {
		udelay(1);
		schedule();
		dma_ssp_rxq5_intsts.wrd =
		    dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	}
	dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	while (!dma_ssp_txq5_intsts.bf.txq5_eof) {
		udelay(1);
		schedule();
		dma_ssp_txq5_intsts.wrd =
		    dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	}

	flash_start.wrd = fl_readl(FLASH_FLASH_ACCESS_START);
	while (flash_start.bf.fifoReq) {
		udelay(1);
		schedule();
		flash_start.wrd = fl_readl(FLASH_FLASH_ACCESS_START);
	}

	/* update rx read ptr */
	/* dma_rxq5_rptr.wrd = dma_readl(DMA_DMA_SSP_RXQ5_RPTR); */
	dma_rxq5_rptr.bf.index = (dma_rxq5_rptr.bf.index + 1) % FDMA_DESC_NUM;
	dma_writel(DMA_DMA_SSP_RXQ5_RPTR, dma_rxq5_rptr.wrd);

	/* clr tx/rx eof */
	dma_ssp_txq5_intsts.wrd = dma_readl(DMA_DMA_SSP_TXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_TXQ5_INTERRUPT, dma_ssp_txq5_intsts.wrd);
	dma_ssp_rxq5_intsts.wrd = dma_readl(DMA_DMA_SSP_RXQ5_INTERRUPT);
	dma_writel(DMA_DMA_SSP_RXQ5_INTERRUPT, dma_ssp_rxq5_intsts.wrd);

#else	/* CSW_USE_DMA */

	do_pio_write_buf(chip->oob_poi, mtd->oobsize);

#endif

	return 0;
}


/**
 * cs752x_nand_read_page_hwecc - [REPLACABLE] hardware ecc based page read function
 * @mtd:	mtd info structure
 * @chip:	nand chip info structure
 * @buf:	buffer to store read data
 * @page:	page number to read
 *
 * Not for syndrome calculating ecc controllers which need a special oob layout
 */
static int cs752x_nand_read_page_hwecc(struct mtd_info *mtd, struct nand_chip *chip,
				uint8_t *buf, int oob_required, int page)
{
	int  col;
	uint8_t *p = buf;
#ifndef CONFIG_CS752X_NAND_ECC_HW_BCH
	uint8_t *ecc_code = chip->buffers->ecccode;
	u32 ul_ecc_gen0;
#endif

	/* ### note ### : Here, must be true */
	oob_required = true;

	col  = g_nand_col;
	p = buf;

	check_flash_ctrl_status();
	reset_ecc_bch_registers();

	configure_hwecc_reg( false );

	cs752x_nand_read_page(mtd, chip, buf, oob_required, page);

#if defined( CONFIG_CS752X_NAND_ECC_HW_BCH )
	bch_sts.wrd=fl_readl(FLASH_NF_BCH_STATUS);
	while(!bch_sts.bf.bchGenDone) {
		udelay(1);
		schedule();
		bch_sts.wrd=fl_readl(FLASH_NF_BCH_STATUS);
	}
#else
	ecc_sts.wrd=fl_readl(FLASH_NF_ECC_STATUS);
	while(!ecc_sts.bf.eccDone) {
		udelay(1);
		schedule();
		ecc_sts.wrd=fl_readl(FLASH_NF_ECC_STATUS);
	}

	ecc_ctl.wrd = fl_readl(FLASH_NF_ECC_CONTROL );
	ecc_ctl.bf.eccEn = 0;
	fl_writel(FLASH_NF_ECC_CONTROL, ecc_ctl.wrd);
#endif

#ifdef	CONFIG_CS752X_NAND_ECC_HW_BCH
	{
	struct mtd_oob_region region;
	mtd_ooblayout_free(mtd, BCH_ERASE_TAG_SECTION, &region);
	if (chip->oob_poi[region.offset + region.length] == (u8)0xFF){
		/*  Erase tag is on , No needs to check. */
		goto BCH_EXIT;
	}
	}

	bch_correct( mtd, chip, buf);

#else	/* CONFIG_CS752X_NAND_ECC_HW_BCH */
	int i ;
	int eccsteps;
	int eccbytes;
	int eccsize;

	eccsteps = chip->ecc.steps;
	eccbytes = chip->ecc.bytes;
	eccsize = chip->ecc.size;

	mtd_ooblayout_get_eccbytes(mtd, ecc_code, chip->oob_poi, 0, chip->ecc.total);

	for (i = 0 ; eccsteps; eccsteps--, i += eccbytes, p += eccsize) {
		ecc_oob.wrd = ecc_code[i] | (ecc_code[i + 1] << 8) | (ecc_code[i + 2] << 16);
		fl_writel(FLASH_NF_ECC_OOB, ecc_oob.wrd);

		ecc_ctl.wrd = fl_readl(FLASH_NF_ECC_CONTROL);
		ecc_ctl.bf.eccCodeSel = (i / eccbytes);
		fl_writel(FLASH_NF_ECC_CONTROL, ecc_ctl.wrd);

		ecc_sts.wrd = fl_readl(FLASH_NF_ECC_STATUS);

		switch(ecc_sts.bf.eccStatus) {
			case ECC_NO_ERR:
				break;
			case ECC_1BIT_DATA_ERR:
				/* flip the bit */
				p[ecc_sts.bf.eccErrByte] ^= (1 << ecc_sts.bf.eccErrBit);
				ul_ecc_gen0 = fl_readl(FLASH_NF_ECC_GEN0 + (4 * (i / eccbytes)));

				printk("\nECC one bit data error(%x)!!(org: %x) HW(%xs) page(%x)\n",
					(i/eccbytes),ecc_oob.wrd, ul_ecc_gen0, page);
				break;
			case ECC_1BIT_ECC_ERR:
				ul_ecc_gen0 = fl_readl(FLASH_NF_ECC_GEN0 + (4 * (i / eccbytes)));
				printk("\nECC one bit ECC error(%x)!!(org: %x) HW(%xs) page(%x)\n",
					(i/eccbytes), ecc_oob.wrd, ul_ecc_gen0, page);
				break;
			case ECC_UNCORRECTABLE:
				mtd->ecc_stats.failed++;
				ul_ecc_gen0 = fl_readl(FLASH_NF_ECC_GEN0 + (4 * (i / eccbytes)));
				printk("\nECC uncorrectable error(%x)!!(org: %x) HW(%xs) page(%x)\n",
					(i/eccbytes), ecc_oob.wrd, ul_ecc_gen0, page);
				break;
		}
	}
#endif	/* CONFIG_CS752X_NAND_ECC_HW_BCH */


#ifdef	CONFIG_CS752X_NAND_ECC_HW_BCH
BCH_EXIT:
	bch_ctrl.wrd = 0;
	fl_writel(FLASH_NF_BCH_CONTROL, bch_ctrl.wrd);
#endif

	return 0;
}

/**
 * cs752x_nand_write_page - [REPLACEABLE] write one page
 * @mtd:	MTD device structure
 * @chip:	NAND chip descriptor
 * @buf:	the data to write
 * @page:	page number to write
 * @cached:	cached programming
 * @raw:	use _raw version of write_page
 */
static int cs752x_nand_write_page(struct mtd_info *mtd, struct nand_chip *chip,
			uint32_t offset, int data_len, const uint8_t *buf,
			int oob_required, int page, int cached, int raw)
{
	int status;

///oob_required = true;

	chip->cmdfunc(mtd, NAND_CMD_SEQIN, 0x00, page);

	if (unlikely(raw))
		chip->ecc.write_page_raw(mtd, chip, buf, oob_required, page);
	else
		chip->ecc.write_page(mtd, chip, buf, oob_required, page);

	/*
	 * Cached progamming disabled for now, Not sure if its worth the
	 * trouble. The speed gain is not very impressive. (2.3->2.6Mib/s)
	 */
	cached = 0;

	if (!cached || !(chip->options & NAND_CACHEPRG)) {
		chip->cmdfunc(mtd, NAND_CMD_PAGEPROG, -1, -1);
		status = chip->waitfunc(mtd, chip);
		/*
		 * See if operation failed and additional status checks are
		 * available
		 */
		if ((status & NAND_STATUS_FAIL) && (chip->errstat))
			status = chip->errstat(mtd, chip, FL_WRITING, status, page);

		if (status & NAND_STATUS_FAIL) {
			return -EIO;
		}
	} else {
		chip->cmdfunc(mtd, NAND_CMD_CACHEDPROG, -1, -1);
		status = chip->waitfunc(mtd, chip);
		printk("%s: ------------------------------->NAND_CMD_CACHEDPROG\n", __func__);
	}

#ifdef CONFIG_MTD_NAND_VERIFY_WRITE
	/* Send command to read back the data */
	chip->cmdfunc(mtd, NAND_CMD_READ0, 0, page);

	if (chip->verify_buf(mtd, buf, mtd->writesize)) {
		return -EIO;
	}
#endif
	return 0;
}

static int cs752x_configure_for_chip(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	int err = 0;

#ifdef CONFIG_CS752X_NAND_ECC_HW_BCH
	if (mtd->oobsize == 16)
		mtd_set_ooblayout(mtd, &cs752x_ooblayout_ops_bch16);
	else
		mtd_set_ooblayout(mtd, &cs752x_ooblayout_ops_bch_lp);
#elif defined(CONFIG_CS752X_NAND_ECC_HW_HAMMING)
	if ((mtd->oobsize == 8) || (mtd->oobsize == 16))
		mtd_set_ooblayout(mtd, &nand_ooblayout_sp_ops);
	else
		mtd_set_ooblayout(mtd, &nand_ooblayout_lp_ops);
#endif

	/*********** controller specific function hooks ***********/

	chip->write_page = cs752x_nand_write_page;

	switch (chip->ecc.mode) {
	case NAND_ECC_HW:
		/* Use standard hwecc read page function ? */
		chip->ecc.read_page		= cs752x_nand_read_page_hwecc;
		chip->ecc.write_page		= cs752x_nand_write_page_hwecc;
		chip->ecc.read_page_raw		= cs752x_nand_read_page_raw;
		chip->ecc.write_page_raw	= cs752x_nand_write_page_raw;
#if 0	/* hook nothing. use generic implementation */
		chip->ecc.read_oob		= NULL;
#endif
		chip->ecc.write_oob		= cs752x_nand_write_oob_std;

		/* HW ecc need read/write data to calculate */
		/* so calculate/correct use SW */
		if (chip->ecc.size == 256) {
			chip->ecc.calculate	= nand_calculate_ecc;
			chip->ecc.correct	= nand_correct_data;
		} else {
			chip->ecc.calculate	= nand_calculate_512_ecc;
			chip->ecc.correct	= nand_correct_512_data;
		}
		/* chip->ecc.calculate		= cs752x_hw_nand_calculate_ecc; */
		/* chip->ecc.correct		= cs752x_hw_nand_correct_data; */
		chip->ecc.read_subpage		= cs752x_nand_read_subpage;

		if (mtd->writesize >= chip->ecc.size)
			break;
		printk(KERN_WARNING "%d byte HW ECC not possible on "
		       "%d byte page size, fallback to SW ECC\n",
		       chip->ecc.size, mtd->writesize);
	default:
		printk(KERN_WARNING "Invalid NAND_ECC_MODE %d\n", chip->ecc.mode);
		BUG();
	}

	return err;
}

/**
 * cs752x_nand_read_buf - [DEFAULT] read chip data into buffer
 * @mtd:	MTD device structure
 * @buf:	buffer to store date
 * @len:	number of bytes to read
 *
 * Default read function for 8bit buswith
 */
static void cs752x_nand_read_buf(struct mtd_info *mtd, uint8_t *buf, int len)
{
	if (cs752x_host->buf_data_len > 0) {
		int len2;
		len2 = min_t(int, cs752x_host->buf_data_len, len);
		memcpy(buf, cs752x_host->buf_top, len2);
		return;
	}

	/* must not reach here */
	BUG();
}

/**
 * cs752x_nand_write_buf - [DEFAULT] write buffer to chip
 * @mtd:	MTD device structure
 * @buf:	data buffer
 * @len:	number of bytes to write
 *
 * Default write function for 8bit buswith
 */
static void cs752x_nand_write_buf(struct mtd_info *mtd, const uint8_t *buf, int len)
{
	/* must not reach here */
	BUG();
}

/**
 * cs752x_nand_read_byte - [DEFAULT] read one byte from the chip
 * @mtd:	MTD device structure
 *
 * Default read function for 8bit buswith
 */
static uint8_t cs752x_nand_read_byte(struct mtd_info *mtd)
{
	unsigned int data = 0xFF;

	/* for result of NAND_CMD_STATUS */
	if (cs752x_host->flag_status_req)
		return (fl_readl(FLASH_NF_DATA) & 0xff);

	if (cs752x_host->buf_data_len > 0) {
		if (cs752x_host->buf_offs < cs752x_host->buf_data_len) {
			data = cs752x_host->buf_top[cs752x_host->buf_offs];
			cs752x_host->buf_offs++;
		}
		return data;
	}

	/* must not reach here */
	BUG();
	return 0xFF;
}

static void  clear_command_cache(void)
{
	memset(cs752x_host->cmd_array, 0, sizeof(cs752x_host->cmd_array));
	cs752x_host->cmd_cnt = 0;
	cs752x_host->page = -1;
}

static int add_to_command_cache(unsigned int cmd)
{
	int res = 0;

	if (cs752x_host->cmd_cnt < CS75XX_CMD_MAX_NUM) {
		cs752x_host->cmd_array[cs752x_host->cmd_cnt++] = cmd;
	} else{
		res = -1;
	}
	return res;
}

static void do_readid_param(struct nand_chip *chip, int col, u8 cmd, int bytes)
{
	unsigned int opcode;
	unsigned int i;
	u32 ul_cmd;

	check_flash_ctrl_status();

	/* disable ecc gen */
	fl_writel(FLASH_NF_ECC_CONTROL, 0x00);

	flash_type.wrd = fl_readl(FLASH_TYPE);

	/* need to check extid byte counts */
	nf_cnt.wrd = 0;

	nf_cnt.bf.nflashRegOobCount = NCNT_EMPTY_OOB;
	nf_cnt.bf.nflashRegDataCount = bytes - 1;
	nf_cnt.bf.nflashRegAddrCount = NCNT_ADDR_1;
	nf_cnt.bf.nflashRegCmdCount = NCNT_CMD_1;

	fl_writel(FLASH_NF_COUNT, nf_cnt.wrd);

	ul_cmd = mk_nf_command(cmd, 0, 0);
	fl_writel(FLASH_NF_COMMAND, ul_cmd);
	fl_writel(FLASH_NF_ADDRESS_1, col);
	fl_writel(FLASH_NF_ADDRESS_2, 0x00UL);

	/* read maker code */
	nf_access.wrd = 0;
	nf_access.bf.nflashCeAlt = CHIP_EN;
	/* nf_access.bf.nflashDirWr = ; */
	nf_access.bf.nflashRegWidth = NFLASH_WiDTH8;
	fl_writel(FLASH_NF_ACCESS, nf_access.wrd);

	for (i = 0; i < bytes; i++) {

		flash_start.wrd = 0;
		flash_start.bf.nflashRegReq = FLASH_GO;
		flash_start.bf.nflashRegCmd = FLASH_RD;
		fl_writel(FLASH_FLASH_ACCESS_START, flash_start.wrd);

		flash_start.wrd = fl_readl(FLASH_FLASH_ACCESS_START);
		while (flash_start.bf.nflashRegReq) {
			udelay(1);
			schedule();
			flash_start.wrd = fl_readl(FLASH_FLASH_ACCESS_START);
		}

		opcode = fl_readl(FLASH_NF_DATA);
		cs752x_host->own_buf[i] = (opcode >> ((i << 3) % 32)) & 0xff;
	}
	cs752x_host->buf_top = cs752x_host->own_buf;
	cs752x_host->buf_offs = 0;
	cs752x_host->buf_data_len = bytes;

	ecc_reset.wrd = 0;
	ecc_reset.bf.eccClear = ECC_CLR;
	ecc_reset.bf.fifoClear = FIFO_CLR;
	ecc_reset.bf.nflash_reset = NF_RESET;
	fl_writel(FLASH_NF_ECC_RESET, ecc_reset.wrd);
}

#define	READID_DATA_LEN	(8)

static void do_read_id(struct nand_chip *chip, int col)
{
	do_readid_param(chip, col, NAND_CMD_READID, READID_DATA_LEN);
}

#define	PARAM_DATA_LEN	(sizeof(struct nand_onfi_params))

static void do_param_cmd(struct nand_chip *chip, int col)
{
	do_readid_param(chip, col, NAND_CMD_PARAM, PARAM_DATA_LEN);
}

/**
 * cs752x_nand_command - [DEFAULT] Send command to NAND device
 * @mtd:	MTD device structure
 * @command:	the command to be sent
 * @column:	the column address for this command, -1 if none
 * @page_addr:	the page address for this command, -1 if none
 *
 * Send command to NAND device. This function is used for small page
 * devices (256/512 Bytes per page)
 */
static void cs752x_nand_command(struct mtd_info *mtd, unsigned int command,
			 int column, int page_addr)
{
	register struct nand_chip *chip = mtd_to_nand(mtd);
	int ctrl = NAND_CTRL_CLE | NAND_CTRL_CHANGE;
	u32	ul_cmd;

	cs752x_host->flag_status_req = false;
	cs752x_host->buf_offs = 0;
	cs752x_host->buf_data_len = 0;
	cs752x_host->buf_top = NULL;

	/*
	 * Write out the command to the device.
	 */
	if (command == NAND_CMD_SEQIN) {
		int readcmd;

		if (column >= mtd->writesize) {
			/* OOB area */
			column -= mtd->writesize;
			readcmd = NAND_CMD_READOOB;
		} else if (column < 256) {
			/* First 256 bytes --> READ0 */
			readcmd = NAND_CMD_READ0;
		} else {
			column -= 256;
			readcmd = NAND_CMD_READ1;
		}
		chip->cmd_ctrl(mtd, readcmd, ctrl);
		ctrl &= ~NAND_CTRL_CHANGE;
	}
	chip->cmd_ctrl(mtd, command, ctrl);

	/*
	 * Address cycle, when necessary
	 */
	ctrl = NAND_CTRL_ALE | NAND_CTRL_CHANGE;
	/* Serially input address */
	if (column != -1) {
		/* Adjust columns for 16 bit buswidth */
		if (chip->options & NAND_BUSWIDTH_16)
			column >>= 1;
		chip->cmd_ctrl(mtd, column, ctrl);
		ctrl &= ~NAND_CTRL_CHANGE;
		g_nand_col = column;
	}
	if (page_addr != -1) {
		chip->cmd_ctrl(mtd, page_addr, ctrl);
		ctrl &= ~NAND_CTRL_CHANGE;
		chip->cmd_ctrl(mtd, page_addr >> 8, ctrl);
		/* One more address cycle for devices > 32MiB */
		if (chip->chipsize > (32 * SZ_1M))
			chip->cmd_ctrl(mtd, page_addr >> 16, ctrl);

		g_nand_page = page_addr;
	}
	chip->cmd_ctrl(mtd, NAND_CMD_NONE, NAND_NCE | NAND_CTRL_CHANGE);

	/*
	 * program and erase have their own busy handlers
	 * status and sequential in needs no delay
	 */
	switch (command) {

	case NAND_CMD_STATUS:
		{
		u32	ul_cmd;
		unsigned long	timeo;

		cs752x_host->flag_status_req = true;

		/* disable ecc gen */
		fl_writel(FLASH_NF_ECC_CONTROL, 0x0);

		nf_cnt.wrd = 0;
		nf_cnt.bf.nflashRegOobCount	= NCNT_EMPTY_OOB;
		nf_cnt.bf.nflashRegDataCount	= NCNT_DATA_1;
		nf_cnt.bf.nflashRegAddrCount	= NCNT_EMPTY_ADDR;
		nf_cnt.bf.nflashRegCmdCount	= NCNT_CMD_1;
		fl_writel(FLASH_NF_COUNT, nf_cnt.wrd);

		ul_cmd = mk_nf_command(NAND_CMD_STATUS, 0, 0);
		fl_writel(FLASH_NF_COMMAND, ul_cmd);

		nf_access.wrd = 0;
		nf_access.bf.nflashCeAlt = CHIP_EN;
		/* nf_access.bf.nflashDirWr = ; */
		nf_access.bf.nflashRegWidth = NFLASH_WiDTH8;
		fl_writel(FLASH_NF_ACCESS, nf_access.wrd);

		flash_start.wrd = 0;
		flash_start.bf.nflashRegReq = FLASH_GO;
		flash_start.bf.nflashRegCmd = FLASH_RD;
		fl_writel(FLASH_FLASH_ACCESS_START, flash_start.wrd);

		timeo = jiffies + HZ;
		do {
			flash_start.wrd = fl_readl(FLASH_FLASH_ACCESS_START);
			if(flash_start.bf.nflashRegReq == 0)
				break;
		} while (time_before(jiffies, timeo));

		clear_command_cache();
		return;
		}
		break;

	case NAND_CMD_READID:
		do_read_id(chip, column);
		clear_command_cache();
		return;
	case NAND_CMD_PARAM:
		do_param_cmd(chip, column);
		clear_command_cache();
		return;
		break;

	case NAND_CMD_ERASE1:
		add_to_command_cache(NAND_CMD_ERASE1);
		cs752x_host->page = page_addr;
		return;
		break;
	case NAND_CMD_ERASE2:
		if ((cs752x_host->cmd_cnt == 1) && (cs752x_host->cmd_array[0] == NAND_CMD_ERASE1)) {
			(void)cs752x_nand_erase_block(mtd, cs752x_host->page);
		}
		clear_command_cache();
		return;
		break;

	case NAND_CMD_READOOB:
		cs752x_do_read_oob(mtd, chip, page_addr);
		cs752x_host->buf_top = cs752x_host->own_buf;
		cs752x_host->buf_offs = column;
		cs752x_host->buf_data_len = mtd->oobsize - column;
		clear_command_cache();
		return;
		break;

	case NAND_CMD_PAGEPROG:
	case NAND_CMD_SEQIN:
	case NAND_CMD_READ0:
		/*
		 * Write out the command to the device.
		 */
		if (column != -1 || page_addr != -1) {

			/* Serially input address */
			if (column != -1)
				/* FLASH_WRITE_REG(NFLASH_ADDRESS,column); */
				g_nand_col = column;

			if (page_addr != -1)
				/* FLASH_WRITE_REG(NFLASH_ADDRESS,opcode|(page_addr<<8)); */
				g_nand_page = page_addr;

		}
		return;

	case NAND_CMD_RESET:
		check_flash_ctrl_status();
		udelay(chip->chip_delay);
		fl_writel(FLASH_NF_ECC_CONTROL, 0x0);
		nf_cnt.wrd = 0;
		nf_cnt.bf.nflashRegOobCount = NCNT_EMPTY_OOB;
		nf_cnt.bf.nflashRegDataCount = NCNT_EMPTY_DATA;
		nf_cnt.bf.nflashRegAddrCount = NCNT_EMPTY_ADDR;
		nf_cnt.bf.nflashRegCmdCount = NCNT_CMD_1;
		fl_writel(FLASH_NF_COUNT, nf_cnt.wrd);

		ul_cmd = mk_nf_command(NAND_CMD_RESET, 0, 0);
		fl_writel(FLASH_NF_COMMAND, ul_cmd);
		fl_writel(FLASH_NF_ADDRESS_1, 0x00UL);
		fl_writel(FLASH_NF_ADDRESS_2, 0x00UL);

		nf_access.wrd = 0;
		nf_access.bf.nflashCeAlt = CHIP_EN;
		/* nf_access.bf.nflashDirWr = ; */
		nf_access.bf.nflashRegWidth = NFLASH_WiDTH8;

		fl_writel(FLASH_NF_ACCESS, nf_access.wrd);
		flash_start.wrd = 0;
		flash_start.bf.nflashRegReq = FLASH_GO;
		flash_start.bf.nflashRegCmd = FLASH_WT;
		fl_writel(FLASH_FLASH_ACCESS_START, flash_start.wrd);

		flash_start.wrd = fl_readl(FLASH_FLASH_ACCESS_START);
		while (flash_start.bf.nflashRegReq) {
			udelay(1);
			schedule();
			flash_start.wrd = fl_readl(FLASH_FLASH_ACCESS_START);
		}

		udelay(100);
		break;

		/* This applies to read commands */
	default:
		/*
		 * If we don't have access to the busy pin, we apply the given
		 * command delay
		 */
		if (!chip->dev_ready) {
			udelay(chip->chip_delay);
			return;
		}
	}
	/* Apply this short delay always to ensure that we do wait tWB in
	 * any case on any machine. */

	udelay(100);
	nand_wait_ready(mtd);
}

static void cs752x_nand_select_chip(struct mtd_info *mtd, int chip)
{
	switch (chip) {
	case -1:
		CHIP_EN = NFLASH_CHIP0_EN;
		break;
	case 0:
		CHIP_EN = NFLASH_CHIP0_EN;
		break;
	case 1:
		CHIP_EN = NFLASH_CHIP1_EN;
		break;

	default:
		/* BUG(); */
		CHIP_EN = NFLASH_CHIP0_EN;
	}
}

/*
*	read device ready pin
*/
static int cs752x_nand_dev_ready(struct mtd_info *mtd)
{
	struct nand_chip *chip = mtd_to_nand(mtd);
	int ready, old_sts;

	check_flash_ctrl_status();

	fl_writel(FLASH_NF_DATA, 0xffffffff);
	old_sts = fl_readl(FLASH_STATUS);
	if ((old_sts & 0xffff) != 0)
		printk("old_sts : %x      ", old_sts);

 RD_STATUS:
	chip->cmdfunc(mtd, NAND_CMD_STATUS, -1, -1);

	ready = chip->read_byte(mtd);
	if (ready == 0xff) {
		goto RD_STATUS;
	}

	return (ready & NAND_STATUS_READY);
}


/*
 *	hardware specific access to control-lines
 *	ctrl:
 */
static void cs752x_nand_hwcontrol(struct mtd_info *mtd, int cmd,
				   unsigned int ctrl)
{
	/* NOP */
}

static int cs752x_onfi_get_features(struct mtd_info *mtd,
				      struct nand_chip *chip, int addr,
				      u8 *subfeature_param)
{
	return -EOPNOTSUPP;
}

static int cs752x_onfi_set_features(struct mtd_info *mtd,
				      struct nand_chip *chip, int addr,
				      u8 *subfeature_param)
{
	return -EOPNOTSUPP;
}

#ifdef CSW_USE_DMA

static int init_DMA_SSP( void )
{
	int i;

	dma_addr_t dma_tx_handle;
	dma_addr_t dma_rx_handle;

	DMA_DMA_SSP_RXDMA_CONTROL_t dma_rxdma_ctrl;
	DMA_DMA_SSP_TXDMA_CONTROL_t dma_txdma_ctrl;

	DMA_DMA_SSP_RXQ5_BASE_DEPTH_t dma_rxq5_base_depth;
	DMA_DMA_SSP_TXQ5_BASE_DEPTH_t dma_txq5_base_depth;

	dma_rxdma_ctrl.wrd = dma_readl(DMA_DMA_SSP_RXDMA_CONTROL);
	dma_txdma_ctrl.wrd = dma_readl(DMA_DMA_SSP_TXDMA_CONTROL);

	if ((dma_rxdma_ctrl.bf.rx_check_own != 1) && (dma_rxdma_ctrl.bf.rx_dma_enable != 1)) {
		dma_rxdma_ctrl.bf.rx_check_own = 1;
		dma_rxdma_ctrl.bf.rx_dma_enable = 1;
		dma_writel(DMA_DMA_SSP_RXDMA_CONTROL, dma_rxdma_ctrl.wrd);
	}
	if ((dma_txdma_ctrl.bf.tx_check_own != 1) && (dma_txdma_ctrl.bf.tx_dma_enable != 1)) {
		dma_txdma_ctrl.bf.tx_check_own = 1;
		dma_txdma_ctrl.bf.tx_dma_enable = 1;
		dma_writel(DMA_DMA_SSP_TXDMA_CONTROL, dma_txdma_ctrl.wrd);
	}

	tx_desc = (DMA_SSP_TX_DESC_T *) dma_alloc_coherent(NULL,
						     (sizeof(DMA_SSP_TX_DESC_T)
						      * FDMA_DESC_NUM),
						     &dma_tx_handle,
						     GFP_KERNEL | GFP_DMA);
	rx_desc = (DMA_SSP_RX_DESC_T *) dma_alloc_coherent(NULL,
						     (sizeof(DMA_SSP_RX_DESC_T)
						      * FDMA_DESC_NUM),
						     &dma_rx_handle,
						     GFP_KERNEL | GFP_DMA);

	if (!rx_desc || !tx_desc) {
		printk("Buffer allocation for failed!\n");
		if (rx_desc) {
			kfree(rx_desc);
		}

		if (tx_desc) {
			kfree(tx_desc);
		}

		return 0;
	}

	/* set base address and depth */

	dma_rxq5_base_depth.bf.base = dma_rx_handle >> 4;
	dma_rxq5_base_depth.bf.depth = FDMA_DEPTH;
	dma_writel(DMA_DMA_SSP_RXQ5_BASE_DEPTH, dma_rxq5_base_depth.wrd);

	dma_txq5_base_depth.bf.base = dma_tx_handle >> 4;
	dma_txq5_base_depth.bf.depth = FDMA_DEPTH;
	dma_writel(DMA_DMA_SSP_TXQ5_BASE_DEPTH, dma_txq5_base_depth.wrd);

	memset((unsigned char *)tx_desc, 0, (sizeof(DMA_SSP_TX_DESC_T) * FDMA_DESC_NUM));
	memset((unsigned char *)rx_desc, 0, (sizeof(DMA_SSP_RX_DESC_T) * FDMA_DESC_NUM));

	for (i = 0; i < FDMA_DESC_NUM; i++) {
		/* set own by sw */
		tx_desc[i].word0.bf.own = OWN_SW;
		/* enable q5 Scatter-Gather memory copy */
		tx_desc[i].word0.bf.sgm_rsrvd = 0x15;
	}

	return 1;
}
#endif	/* CSW_USE_DMA */


/*
 * Probe for the NAND device.
 */
static int __init cs752x_nand_probe(struct platform_device *pdev)
{
	struct nand_chip *this;
	struct mtd_info *mtd;
	struct resource *rmem;
	int err = 0;

	printk("CS752X NAND init ...\n");

	/* Allocate memory for MTD device structure and private data */
	cs752x_host = kzalloc(sizeof(struct cs752x_nand_host), GFP_KERNEL);
	if (!cs752x_host) {
		printk("Unable to allocate cs752x_host NAND MTD device structure.\n");
		return -ENOMEM;
	}

	/* region of NAND memory DMA space */
	rmem = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!rmem) {
		dev_err(&pdev->dev, "no memory resource for NAND controller\n");
		err = -ENODEV;
		goto err_get_res;
	}
	cs752x_host->dma_phy_base = rmem->start;

	/* region of NAND controller */
	rmem = platform_get_resource(pdev, IORESOURCE_MEM, 1);
	if (!rmem) {
		dev_err(&pdev->dev, "no memory resource for NAND controller\n");
		err = -ENODEV;
		goto err_get_res;
	}
	cs752x_host->iobase_fl = devm_ioremap_resource(&pdev->dev, rmem);
	if (IS_ERR(cs752x_host->iobase_fl)) {
		err = PTR_ERR(cs752x_host->iobase_fl);
		goto err_get_res;
	}

	/* region of DMA controller */
	rmem = platform_get_resource(pdev, IORESOURCE_MEM, 2);
	if (!rmem) {
		dev_err(&pdev->dev, "no memory resource for DMA controller\n");
		err = -ENODEV;
		goto err_get_res;
	}
	cs752x_host->iobase_dma_ssp = devm_ioremap_resource(&pdev->dev, rmem);
	if (IS_ERR(cs752x_host->iobase_dma_ssp)) {
		err = PTR_ERR(cs752x_host->iobase_dma_ssp);
		goto err_get_res;
	}

	/* Get pointer to private data */
	/* Allocate memory for MTD device structure and private data */
	cs752x_host->nand_chip = (struct nand_chip *)kzalloc(sizeof(struct nand_chip), GFP_KERNEL);
	if (!cs752x_host->nand_chip) {
		printk("Unable to allocate CS752X NAND MTD device structure.\n");
		err = -ENOMEM;
		goto err_mtd;
	}
	this = cs752x_host->nand_chip;

	cs752x_host->mtd = nand_to_mtd(this);
	mtd = cs752x_host->mtd;

#ifdef CSW_USE_DMA
	if (init_DMA_SSP() == 0) {
		goto err_add;
	}
#endif

	/* Link the private data with the MTD structure */
	mtd->owner = THIS_MODULE;

	platform_set_drvdata(pdev, cs752x_host);

	/* set eccmode using hardware ECC */
	this->ecc.mode = NAND_ECC_HW;
#if defined( CONFIG_CS752X_NAND_ECC_HW_BCH_8_512 )
	printk("Error Correction Method: bch 8\n");
	this->ecc.size		= 512;
	this->ecc.bytes		= 13;
	this->ecc.strength	= 8;
#elif defined( CONFIG_CS752X_NAND_ECC_HW_BCH_12_512 )
	printk("Error Correction Method: bch 12\n");
	this->ecc.size		= 512;
	this->ecc.bytes		= 20;
	this->ecc.strength	= 12;
#elif defined( CONFIG_CS752X_NAND_ECC_HW_HAMMING_512 )
	printk("Error Correction Method: ecc 512\n");
	this->ecc.size		= 512;
	this->ecc.bytes		= 3;
	this->ecc.strength	= 1;
#else
	printk("Error Correction Method: ecc 256\n");
	this->ecc.size		= 256;
	this->ecc.bytes		= 3;
	this->ecc.strength	= 1;
#endif

	/* check for proper chip_delay setup, set 20us if not */
	this->chip_delay = 20;

	/* Set address of hardware control function */
	this->cmd_ctrl		= cs752x_nand_hwcontrol;
	this->dev_ready		= cs752x_nand_dev_ready;

	/* check, if a user supplied command function given */
	this->cmdfunc		= cs752x_nand_command;
#if 0	/* hook nothing. use generic implementation */
	this->waitfunc		= NULL;
#endif
	this->select_chip	= cs752x_nand_select_chip;
	this->read_byte		= cs752x_nand_read_byte;
	/* this->read_word	= cs752x_nand_read_word; */

#if 0	/* hook nothing. use generic implementation */
	this->block_bad		= NULL;
	this->block_markbad	= NULL;
#endif
	this->write_buf		= cs752x_nand_write_buf;
	this->read_buf		= cs752x_nand_read_buf;

	this->onfi_get_features	= cs752x_onfi_get_features;
	this->onfi_set_features	= cs752x_onfi_set_features;

	/* set the bad block tables to support debugging */
	this->bbt_td = &cs752x_bbt_main_descr;
	this->bbt_md = &cs752x_bbt_mirror_descr;

	this->options |= NAND_NO_SUBPAGE_WRITE;
	/*
	 * ### note ###
	 * do not enable NAND_SUBPAGE_READ
	 */

	if (!this->controller) {
		this->controller = &this->hwcontrol;
		spin_lock_init(&this->controller->lock);
		init_waitqueue_head(&this->controller->wq);
	}

	/* Scan to find existence of the device */
	err = nand_scan_ident(mtd, 1, NULL);
	if (err)
		goto err_scan;

	err = cs752x_configure_for_chip(mtd);
	if (err)
		goto err_scan;
	err = nand_scan_tail(mtd);
	if (err)
		goto err_scan;

	mtd->name = "cs752x_nand_flash";
	mtd_device_register(cs752x_host->mtd, NULL, 0);

	if (err)
		goto err_add;

#ifdef NO_NEED
	cs_pm_freq_register_notifier(&n, 1);
#endif

	/* Return happy */
	printk("cx752x NAND init ok...\n");

	return 0;

 err_add:
	nand_release(cs752x_host->mtd);

 err_scan:
	platform_set_drvdata(pdev, NULL);
 err_mtd:
	if (cs752x_host->mtd)
		kfree(cs752x_host->mtd);
	if (cs752x_host->nand_chip)
		kfree(cs752x_host->nand_chip);
 err_get_res:
	kfree(cs752x_host);
	return err;
}

/*
 * Remove a NAND device.
 */
static int __exit cs752x_nand_remove(struct platform_device *pdev)
{
	struct cs752x_nand_host *cs752x_host = platform_get_drvdata(pdev);

	mtd_device_unregister(cs752x_host->mtd);

#ifdef NO_NEED
	cs_pm_freq_unregister_notifier(&n);
#endif

	/* Release resources, unregister device */
	nand_release(cs752x_host->mtd);

	platform_set_drvdata(pdev, NULL);

	/* Free the MTD device structure */
	kfree(cs752x_host);

	return 0;
}


#ifdef CONFIG_OF
static const struct of_device_id cs752x_nand_dt_ids[] = {
	{ .compatible = "cortina,cs752x-nand" },
};

MODULE_DEVICE_TABLE(of, cs752x_nand_dt_ids);
#endif

static struct platform_driver cs752x_nand_driver = {
	.driver = {
		.name	= "cs752x_nand",
		.owner	= THIS_MODULE,
		.of_match_table = of_match_ptr(cs752x_nand_dt_ids),
	},
	.probe		= cs752x_nand_probe,
	.remove		= cs752x_nand_remove,
};

module_platform_driver(cs752x_nand_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Middle Huang <middle.huang@cortina-systems.com>");
MODULE_DESCRIPTION("NAND flash driver for Cortina CS752X flash module");
MODULE_ALIAS("platform:cs752x_nand");
