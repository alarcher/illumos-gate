/*
 * This file and its contents are supplied under the terms of the
 * Common Development and Distribution License ("CDDL"), version 1.0.
 * You may only use this file in accordance with the terms of version
 * 1.0 of the CDDL.
 *
 * A full copy of the text of the CDDL should have accompanied this
 * source.  A copy of the CDDL is also available via the Internet at
 * http://www.illumos.org/license/CDDL.
 */

/*
 * Copyright 2016 Toomas Soome <tsoome@me.com>
 */

#ifndef _SYS_FRAMEBUFFER_H
#define	_SYS_FRAMEBUFFER_H

/*
 * Framebuffer info from boot loader. Collect linear framebuffer data
 * provided by boot loader and early boot setup.
 * Note the UEFI and multiboot2 present data with slight differences.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <sys/font.h>
#include <sys/rgb.h>

typedef struct fb_info_pixel_coord {
	uint16_t x;
	uint16_t y;
} fb_info_pixel_coord_t;

typedef struct fb_info_char_coord {
	uint16_t x;
	uint16_t y;
} fb_info_char_coord_t;

typedef struct fb_info {
	uint64_t paddr;		/* FB address from bootloader */
	uint8_t *fb;		/* kernel mapped frame buffer */
	uint8_t *shadow_fb;
	uint64_t fb_size;	/* mapped FB size in bytes */
	uint32_t pitch;		/* scan line in bytes */
	uint8_t bpp;		/* bytes per pixel */
	uint8_t depth;		/* bits per pixel */
	rgb_t	rgb;
	fb_info_pixel_coord_t screen;		/* screen size */
	fb_info_pixel_coord_t terminal_origin;	/* terminal upper left corner */
	fb_info_char_coord_t terminal;		/* terminal size in chars */
	struct {
		boolean_t visible;
		fb_info_pixel_coord_t origin;	/* cursor upper left */
		fb_info_char_coord_t pos;	/* cursor coord in chars */
		uint8_t *data;			/* cursor data */
		uint8_t *buf;			/* saved screen area */
		uint32_t size;			/* buffer size in bytes */
	} cursor;
	uint16_t font_width;
	uint16_t font_height;
	boolean_t inverse;
	boolean_t inverse_screen;
} fb_info_t;

extern fb_info_t fb_info;
void boot_fb_cursor(boolean_t);
void boot_fb_set(fb_info_t *);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_FRAMEBUFFER_H */
