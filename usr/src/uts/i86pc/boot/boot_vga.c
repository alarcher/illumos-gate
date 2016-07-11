/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2007 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

/*
 * Miniature VGA driver for bootstrap.
 */

#include <sys/archsystm.h>
#include <sys/vgareg.h>
#include <sys/framebuffer.h>

#include "boot_vga.h"

#if defined(_BOOT)
#include "../dboot/dboot_asm.h"
#include "../dboot/dboot_xboot.h"
#endif

#if defined(__xpv) && defined(_BOOT)

/*
 * Device memory address
 *
 * In dboot under the hypervisor we don't have any memory mappings
 * for the first meg of low memory so we can't access devices there.
 * Intead we've mapped the device memory that we need to access into
 * a local variable within dboot so we can access the device memory
 * there.
 */
extern unsigned short *video_fb;
#define	VGA_SCREEN		((unsigned short *)video_fb)

#else /* __xpv && _BOOT */

/* Device memory address */
#define	VGA_SCREEN		((unsigned short *)0xb8000)

#endif /* __xpv && _BOOT */


static void vga_set_crtc(int index, unsigned char val);
static unsigned char vga_get_crtc(int index);
static void vga_set_atr(int index, unsigned char val);
static unsigned char vga_get_atr(int index);

void
vga_init(void)
{
	unsigned char val;

	/* set 16bit colors */
	val = vga_get_atr(VGA_ATR_MODE);
	val &= ~VGA_ATR_MODE_BLINK;
	val &= ~VGA_ATR_MODE_9WIDE;
	vga_set_atr(VGA_ATR_MODE, val);
}

void
vga_cursor_display(void)
{
	unsigned char val, msl;

	/*
	 * Figure out the maximum scan line value.  We need this to set the
	 * cursor size.
	 */
	msl = vga_get_crtc(VGA_CRTC_MAX_S_LN) & 0x1f;

	/*
	 * Enable the cursor and set it's size.  Preserve the upper two
	 * bits of the control register.
	 * - Bits 0-4 are the starting scan line of the cursor.
	 *   Scanning is done from top-to-bottom.  The top-most scan
	 *   line is 0 and the bottom most scan line is the maximum scan
	 *   line value.
	 * - Bit 5 is the cursor disable bit.
	 */
	val = vga_get_crtc(VGA_CRTC_CSSL) & 0xc0;
	vga_set_crtc(VGA_CRTC_CSSL, val);

	/*
	 * Continue setting the cursors size.
	 * - Bits 0-4 are the ending scan line of the cursor.
	 *   Scanning is done from top-to-bottom.  The top-most scan
	 *   line is 0 and the bottom most scan line is the maximum scan
	 *   line value.
	 * - Bits 5-6 are the cursor skew.
	 */
	vga_set_crtc(VGA_CRTC_CESL, msl);
}


void
vga_clear(int color)
{
	unsigned short val;
	int i;

	val = (color << 8) | ' ';

	for (i = 0; i < VGA_TEXT_ROWS * VGA_TEXT_COLS; i++) {
		VGA_SCREEN[i] = val;
	}
}

void
vga_drawc(int c, int color)
{
	int row;
	int col;

	vga_getpos(&row, &col);
	VGA_SCREEN[row*VGA_TEXT_COLS + col] = (color << 8) | c;
}

void
vga_scroll(int color)
{
	unsigned short val;
	int i;

	val = (color << 8) | ' ';

	for (i = 0; i < (VGA_TEXT_ROWS-1)*VGA_TEXT_COLS; i++) {
		VGA_SCREEN[i] = VGA_SCREEN[i + VGA_TEXT_COLS];
	}
	for (; i < VGA_TEXT_ROWS * VGA_TEXT_COLS; i++) {
		VGA_SCREEN[i] = val;
	}
}

void
vga_setpos(int row, int col)
{
	int off;

	off = row * VGA_TEXT_COLS + col;
	vga_set_crtc(VGA_CRTC_CLAH, off >> 8);
	vga_set_crtc(VGA_CRTC_CLAL, off & 0xff);

	fb_info.cursor.pos.y = row;
	fb_info.cursor.pos.x = col;
}

void
vga_getpos(int *row, int *col)
{
	int off;

	off = (vga_get_crtc(VGA_CRTC_CLAH) << 8) + vga_get_crtc(VGA_CRTC_CLAL);
	*row = off / VGA_TEXT_COLS;
	*col = off % VGA_TEXT_COLS;
}

static void
vga_set_atr(int index, unsigned char val)
{
	(void) inb(VGA_REG_ADDR + CGA_STAT);
	outb(VGA_REG_ADDR + VGA_ATR_AD, index);
	outb(VGA_REG_ADDR + VGA_ATR_AD, val);

	(void) inb(VGA_REG_ADDR + CGA_STAT);
	outb(VGA_REG_ADDR + VGA_ATR_AD, VGA_ATR_ENB_PLT);
}

static unsigned char
vga_get_atr(int index)
{
	unsigned char val;

	(void) inb(VGA_REG_ADDR + CGA_STAT);
	outb(VGA_REG_ADDR + VGA_ATR_AD, index);
	val = inb(VGA_REG_ADDR + VGA_ATR_DATA);

	(void) inb(VGA_REG_ADDR + CGA_STAT);
	outb(VGA_REG_ADDR + VGA_ATR_AD, VGA_ATR_ENB_PLT);

	return (val);
}

static void
vga_set_crtc(int index, unsigned char val)
{
	outb(VGA_REG_ADDR + VGA_CRTC_ADR, index);
	outb(VGA_REG_ADDR + VGA_CRTC_DATA, val);
}

static unsigned char
vga_get_crtc(int index)
{
	outb(VGA_REG_ADDR + VGA_CRTC_ADR, index);
	return (inb(VGA_REG_ADDR + VGA_CRTC_DATA));
}
