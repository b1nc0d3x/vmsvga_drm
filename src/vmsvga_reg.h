/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * VMware SVGA-II hardware register and FIFO command definitions.
 *
 * Derived from VMware's public SVGA device interface specification
 * (svga_reg.h, svga_overlay.h, svga3d_reg.h) used by the Linux
 * vmwgfx driver.  Only the 2D scaffold subset is reproduced here --
 * no 3D, no GMR, no screen objects, no surfaces, no DMA contexts.
 *
 * Reference: linux/drivers/gpu/drm/vmwgfx/device_include/svga_reg.h
 */

#ifndef _VMSVGA_REG_H_
#define _VMSVGA_REG_H_

#include <sys/types.h>

/*
 * PCI identifiers.  Used by VBox arm64 (the only GPU it exposes to
 * arm64 guests), VMware Fusion / Workstation / ESXi.
 */
#define	PCI_VENDOR_ID_VMWARE		0x15ad
#define	PCI_DEVICE_ID_VMWARE_SVGA2	0x0405
#define	PCI_DEVICE_ID_VMWARE_SVGA2_VBOX	0x0406  /* VBox arm64 calls it -II also */

/*
 * SVGA register magic and ID values.  Write SVGA_ID_2 to SVGA_REG_ID
 * to negotiate version 2.  Read back to see what the host accepted.
 */
#define	SVGA_MAGIC		0x900000UL
#define	SVGA_MAKE_ID(v)		(SVGA_MAGIC << 8 | (v))
#define	SVGA_ID_0		SVGA_MAKE_ID(0)
#define	SVGA_ID_1		SVGA_MAKE_ID(1)
#define	SVGA_ID_2		SVGA_MAKE_ID(2)
#define	SVGA_ID_INVALID		0xffffffff

/*
 * BAR layout (x86 IO-port edition).  On arm64 the device is wired
 * with the same indexed-register interface but accessed through MMIO
 * BAR0 instead of IO ports.  We resolve which path at attach time.
 *
 *   BAR0 (IO port): SVGA_INDEX_PORT / SVGA_VALUE_PORT
 *                   SVGA_BIOS_PORT  / SVGA_IRQSTATUS_PORT
 *   BAR1 (memory) : linear framebuffer (VRAM)
 *   BAR2 (memory) : FIFO command ring
 */
#define	SVGA_INDEX_PORT		0x0
#define	SVGA_VALUE_PORT		0x1
#define	SVGA_BIOS_PORT		0x2
#define	SVGA_IRQSTATUS_PORT	0x8

/*
 * Indexed registers (write index to INDEX_PORT, then read/write VALUE_PORT).
 */
enum {
	SVGA_REG_ID			= 0,
	SVGA_REG_ENABLE			= 1,
	SVGA_REG_WIDTH			= 2,
	SVGA_REG_HEIGHT			= 3,
	SVGA_REG_MAX_WIDTH		= 4,
	SVGA_REG_MAX_HEIGHT		= 5,
	SVGA_REG_DEPTH			= 6,
	SVGA_REG_BITS_PER_PIXEL		= 7,
	SVGA_REG_PSEUDOCOLOR		= 8,
	SVGA_REG_RED_MASK		= 9,
	SVGA_REG_GREEN_MASK		= 10,
	SVGA_REG_BLUE_MASK		= 11,
	SVGA_REG_BYTES_PER_LINE		= 12,
	SVGA_REG_FB_START		= 13,	/* (registers 13-15 deprecated, use BAR1) */
	SVGA_REG_FB_OFFSET		= 14,
	SVGA_REG_VRAM_SIZE		= 15,
	SVGA_REG_FB_SIZE		= 16,

	SVGA_REG_CAPABILITIES		= 17,
	SVGA_REG_MEM_START		= 18,	/* (FIFO MMIO BAR base, deprecated) */
	SVGA_REG_MEM_SIZE		= 19,
	SVGA_REG_CONFIG_DONE		= 20,
	SVGA_REG_SYNC			= 21,
	SVGA_REG_BUSY			= 22,
	SVGA_REG_GUEST_ID		= 23,
	SVGA_REG_CURSOR_ID		= 24,
	SVGA_REG_CURSOR_X		= 25,
	SVGA_REG_CURSOR_Y		= 26,
	SVGA_REG_CURSOR_ON		= 27,
	SVGA_REG_HOST_BITS_PER_PIXEL	= 28,
	SVGA_REG_SCRATCH_SIZE		= 29,
	SVGA_REG_MEM_REGS		= 30,
	SVGA_REG_NUM_DISPLAYS		= 31,
	SVGA_REG_PITCHLOCK		= 32,
	SVGA_REG_IRQMASK		= 33,

	SVGA_REG_NUM_GUEST_DISPLAYS	= 34,
	SVGA_REG_DISPLAY_ID		= 35,
	SVGA_REG_DISPLAY_IS_PRIMARY	= 36,
	SVGA_REG_DISPLAY_POSITION_X	= 37,
	SVGA_REG_DISPLAY_POSITION_Y	= 38,
	SVGA_REG_DISPLAY_WIDTH		= 39,
	SVGA_REG_DISPLAY_HEIGHT		= 40,

	SVGA_REG_GMR_ID			= 41,
	SVGA_REG_GMR_DESCRIPTOR		= 42,
	SVGA_REG_GMR_MAX_IDS		= 43,
	SVGA_REG_GMR_MAX_DESCRIPTOR_LENGTH = 44,

	SVGA_REG_TRACES			= 45,
	SVGA_REG_GMRS_MAX_PAGES		= 46,
	SVGA_REG_MEMORY_SIZE		= 47,

	SVGA_REG_TOP			= 48,
};

/*
 * Capability bits (SVGA_REG_CAPABILITIES).  Only the ones we use.
 */
#define	SVGA_CAP_NONE			0x00000000
#define	SVGA_CAP_RECT_COPY		0x00000002
#define	SVGA_CAP_CURSOR			0x00000020
#define	SVGA_CAP_CURSOR_BYPASS		0x00000040
#define	SVGA_CAP_CURSOR_BYPASS_2	0x00000080
#define	SVGA_CAP_8BIT_EMULATION		0x00000100
#define	SVGA_CAP_ALPHA_CURSOR		0x00000200
#define	SVGA_CAP_3D			0x00004000
#define	SVGA_CAP_EXTENDED_FIFO		0x00008000
#define	SVGA_CAP_MULTIMON		0x00010000
#define	SVGA_CAP_PITCHLOCK		0x00020000
#define	SVGA_CAP_IRQMASK		0x00040000
#define	SVGA_CAP_DISPLAY_TOPOLOGY	0x00080000
#define	SVGA_CAP_GMR			0x00100000
#define	SVGA_CAP_TRACES			0x00200000
#define	SVGA_CAP_GMR2			0x00400000
#define	SVGA_CAP_SCREEN_OBJECT_2	0x00800000

/*
 * FIFO command opcodes.  Only the 2D-update path is implemented for
 * the initial bring-up; everything else is left for follow-on work.
 */
#define	SVGA_CMD_INVALID_CMD		0
#define	SVGA_CMD_UPDATE			1
#define	SVGA_CMD_RECT_COPY		3
#define	SVGA_CMD_DEFINE_CURSOR		19
#define	SVGA_CMD_DEFINE_ALPHA_CURSOR	22
#define	SVGA_CMD_UPDATE_VERBOSE		25
#define	SVGA_CMD_FRONT_ROP_FILL		29
#define	SVGA_CMD_FENCE			30

/*
 * FIFO layout offsets, in 32-bit words.  The FIFO BAR is divided into
 * a fixed header followed by the variable-size command ring.
 */
#define	SVGA_FIFO_MIN			0
#define	SVGA_FIFO_MAX			1
#define	SVGA_FIFO_NEXT_CMD		2
#define	SVGA_FIFO_STOP			3
#define	SVGA_FIFO_NUM_REGS		4

#endif /* _VMSVGA_REG_H_ */
