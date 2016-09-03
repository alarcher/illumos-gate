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

/*
 * Graphics support for loader emulation.
 * The interface in loader and here needs some more development,
 * we can get colormap from gfx_private, but loader is currently
 * relying on tem fg/bg colors for drawing, once the menu code
 * will get some facelift, we would need to provide colors as menu component
 * attributes and stop depending on tem.
 */

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/fbio.h>
#include <string.h>
#include "gfx_fb.h"

struct framebuffer fb;

#define	abs(x)	((x) < 0? -(x):(x))
#define max(x, y)	((x) >= (y) ? (x) : (y))

void gfx_framework_init(void)
{
	struct fbgattr attr;
	char buf[10];

	fb.fd = open("/dev/fb0", O_RDWR);
	if (fb.fd < 0)
		return;

	/* make sure we have GFX framebuffer */
	if (ioctl(fb.fd, VIS_GETIDENTIFIER, &fb.ident) < 0 ||
	    strcmp(fb.ident.name, "illumos_fb") != 0) {
		close(fb.fd);
		fb.fd = -1;
		return;
	}

	if (ioctl(fb.fd, FBIOGATTR, &attr) < 0) {
		close(fb.fd);
		fb.fd = -1;
		return;
	}

	fb.fb_height = attr.fbtype.fb_height;
	fb.fb_width = attr.fbtype.fb_width;
	fb.fb_depth = attr.fbtype.fb_depth;
	fb.fb_size = attr.fbtype.fb_size;
	fb.fb_bpp = attr.fbtype.fb_depth >> 3;
	if (attr.fbtype.fb_depth == 15)
		fb.fb_bpp = 2;
	fb.fb_pitch = fb.fb_width * fb.fb_bpp;

	fb.fb_addr = (uint8_t *)mmap(0, fb.fb_size, (PROT_READ | PROT_WRITE),
	    MAP_SHARED, fb.fd, 0);

	if (fb.fb_addr == NULL) {
		close(fb.fd);
		fb.fd = -1;
		return;
	}
	snprintf(buf, sizeof (buf), "%d", fb.fb_height);
	setenv("screen-height", buf, 1);
	snprintf(buf, sizeof (buf), "%d", fb.fb_width);
	setenv("screen-width", buf, 1);
}

void gfx_framework_fini(void)
{
	if (fb.fd < 0)
		return;

	(void) munmap((caddr_t)fb.fb_addr, fb.fb_size);
	close(fb.fd);
	fb.fd = -1;
}

static int isqrt(int num) {
	int res = 0;
	int bit = 1 << 30;

	/* "bit" starts at the highest power of four <= the argument. */
	while (bit > num)
		bit >>= 2;

	while (bit != 0) {
		if (num >= res + bit) {
			num -= res + bit;
			res = (res >> 1) + bit;
		} else
			res >>= 1;
		bit >>= 2;
	}
	return (res);
}

void gfx_fb_setpixel(int x, int y)
{
	uint32_t c, offset;

	if (fb.fd < 0)
		return;
	c = 0;		/* black */

	if (x < 0 || y < 0)
		return;

	if (x >= fb.fb_width || y >= fb.fb_height)
		return;

	offset = y * fb.fb_pitch + x * fb.fb_bpp;
	switch (fb.fb_depth) {
	case 8:
		fb.fb_addr[offset] = c & 0xff;
		break;
	case 15:
	case 16:
		*(uint16_t *)(fb.fb_addr + offset) = c & 0xffff;
		break;
	case 24:
		fb.fb_addr[offset] = (c >> 16) & 0xff;
		fb.fb_addr[offset + 1] = (c >> 8) & 0xff;
		fb.fb_addr[offset + 2] = c & 0xff;
		break;
	case 32:
		*(uint32_t *)(fb.fb_addr + offset) = c;
		break;
	}
}

void gfx_fb_drawrect(int x1, int y1, int x2, int y2, int fill)
{
	int x, y;

	if (fb.fd < 0)
		return;

	for (y = y1; y <= y2; y++) {
		if (fill || (y == y1) || (y == y2)) {
			for (x = x1; x <= x2; x++)
				gfx_fb_setpixel(x, y);
		} else {
			gfx_fb_setpixel(x1, y);
			gfx_fb_setpixel(x2, y);
		}
	}
}

void gfx_term_drawrect(int row1, int col1, int row2, int col2)
{
	if (fb.fd < 0)
		return;
}

void gfx_fb_line(int x0, int y0, int x1, int y1, int width)
{
	int dx, sx, dy, sy;
	int err, e2, x2, y2, ed;

	if (fb.fd < 0)
		return;

	sx = x0 < x1? 1:-1;
	sy = y0 < y1? 1:-1;
	dx = abs(x1 - x0);
	dy = abs(y1 - y0);
	err = dx - dy;
	ed = dx + dy == 0 ? 1: isqrt(dx * dx + dy * dy);

	if (dx != 0 && dy != 0)
		width = (width + 1) >> 1;

	for (;;) {
		gfx_fb_setpixel(x0, y0);
		e2 = err;
		x2 = x0;
		if ((e2 << 1) >= -dx) {		/* x step */
			e2 += dy;
			y2 = y0;
			while (e2 < ed * width && (y1 != y2 || dx > dy)) {
				y2 += sy;
				gfx_fb_setpixel(x0, y2);
				e2 += dx;
			}
			if (x0 == x1)
				break;
			e2 = err;
			err -= dy;
			x0 += sx;
		}
		if ((e2 << 1) <= dy) {		/* y step */
			e2 = dx-e2;
			while (e2 < ed * width && (x1 != x2 || dx < dy)) {
				x2 += sx;
				gfx_fb_setpixel(x2, y0);
				e2 += dy;
			}
			if (y0 == y1)
				break;
			err += dx;
			y0 += sy;
		}
	}
}

void gfx_fb_bezier(int x0, int y0, int x1, int y1, int x2, int y2, int width)
{
	int sx, sy;
	int64_t xx, yy, xy;
	int64_t dx, dy, err, ed, curvature;
	int i;

	if (fb.fd < 0)
		return;

	sx = x2 - x1;
	sy = y2 - y1;
	xx = x0 - x1;
	yy = y0 - y1;
	curvature = xx*sy - yy*sx;

	if (!(xx * sx >= 0 && yy * sy >= 0))
		printf("sign of gradient must not change\n");

	if (sx * (int64_t)sx + sy * (int64_t)sy > xx * xx + yy * yy) {
		x2 = x0;
		x0 = sx + x1;
		y2 = y0;
		y0 = sy + y1;
		curvature = -curvature;
	}
	if (curvature != 0) {
		xx += sx;
		sx = x0 < x2? 1:-1;
		xx *= sx;
		yy += sy;
		sy = y0 < y2? 1:-1;
		yy *= sy;
		xy = 2 * xx * yy;
		xx *= xx;
		yy *= yy;
		if (curvature * sx * sy < 0) {
			xx = -xx;
			yy = -yy;
			xy = -xy;
			curvature = -curvature;
		}
		dx = 4 * sy * curvature * (x1 - x0) + xx - xy;
		dy = 4 * sx * curvature * (y0 - y1) + yy - xy;
		xx += xx;
		yy += yy;
		err = dx + dy + xy;
		do {
			gfx_fb_setpixel(x0, y0);

			if (x0 == x2 && y0 == y2)
				return;  /* last pixel -> curve finished */

			curvature = dx - err;
			ed = max(dx + xy, -xy - dy);
			x1 = x0;
			y1 = 2 * err + dy < 0;
			if (2 * err + dx > 0) {	/* x step */
				if (err - dy < ed)
					gfx_fb_setpixel(x0, y0 + sy);
				x0 += sx;
				dx -= xy;
				dy += yy;
				err += dy;
			}
			if (y1 != 0) {
				if (curvature < ed)
					gfx_fb_setpixel(x1 + sx, y0);
				y0 += sy;
				dy -= xy;
				dx += xx;
				err += dx;
			}
		} while (dy < dx ); /* gradient negates -> algorithm fails */
	}
	gfx_fb_line(x0, y0, x2, y2, width);
}
