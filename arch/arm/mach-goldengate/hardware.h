/*
 *  arch/arm/mach-goldengate/include/mach/hardware.h
 *
 *  This file contains the hardware definitions of the GoldenGate platform.
 *
 * Copyright (c) Cortina-Systems Limited 2010.  All rights reserved.
 *                Jason Li <jason.li@cortina-systems.com>
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
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */
#ifndef __ASM_ARCH_HARDWARE_H
#define __ASM_ARCH_HARDWARE_H

#include <asm/sizes.h>

#if 0
//For PCIe driver compiler used
//===============================================

#define pcibios_assign_all_busses()     1
#define PCIBIOS_MIN_IO                  0x00001000
#define PCIBIOS_MIN_MEM                 0x01000000
#define PCIMEM_BASE                     Upper_Base_Address_CFG_Region1 /* mem base for VGA */

//====================================================
#endif

/* macro to get at IO space when running virtually */
#ifdef CONFIG_MMU
/*
 * 0xFnnn_nnnn
 *   add +0x0700_0000
 *		0xF0xx_xxxx -> 0xF7xx_xxxx
 *		0xF1xx_xxxx -> 0xF8xx_xxxx
 *		...
 *		0xF8xx_xxxx -> 0xFFxx_xxxx
 *
 * 0xNnnn_nnnn (N = 0x0..0xE)
 *   mapped size limit = 64Kbytes
 *		0x0000_xxxx -> 0xF600_xxxx
 *		0x1000_xxxx -> 0xF100_xxxx
 *		...
 *		0xE000_xxxx -> 0xF6E0_xxxx
 */
#define IO_ADDRESS(x)		(((x) & 0xF0000000) ? \
					((x) + 0x07000000) : \
					(0xF6000000 | (((x) & 0xF0000000) >> 8) | ((x) & 0x0000FFFF)))
#else
#define IO_ADDRESS(x)		(x)
#endif

#endif
