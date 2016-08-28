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
 * Common functions to implement graphical framebuffer support for console.
 */

#include <sys/cdefs.h>
#include <stand.h>
#if defined(EFI)
#include <efi.h>
#include <efilib.h>
#else
#include <btxv86.h>
#endif
#include <sys/tem.h>
#include <sys/consplat.h>
#include <sys/visual_io.h>
#include <sys/multiboot2.h>
#include <gfx_fb.h>

/*
 * Global framebuffer struct, to be updated with mode changes.
 */
multiboot_tag_framebuffer_t gfx_fb;

/* To support setenv, keep track of inverses and colors. */
static int gfx_inverse = 0;
static int gfx_inverse_screen = 0;
static uint8_t gfx_fg = DEFAULT_ANSI_FOREGROUND;
static uint8_t gfx_bg = DEFAULT_ANSI_BACKGROUND;

static int gfx_fb_cons_clear(struct vis_consclear *);
static void gfx_fb_cons_copy(struct vis_conscopy *);
static void gfx_fb_cons_display(struct vis_consdisplay *);

/*
 * Translate platform specific FB address.
 */
static uint8_t *
gfx_get_fb_address(void)
{
#if defined(EFI)
	return ((uint8_t *)(uintptr_t)gfx_fb.common.framebuffer_addr);
#else
	return ((uint8_t *)
	    PTOV((uint32_t) gfx_fb.common.framebuffer_addr & 0xffffffff));
#endif
}

/*
 * Generic platform callbacks for tem.
 */
void
plat_tem_get_prom_font_size(int *charheight, int *windowtop)
{
	*charheight = 0;
	*windowtop = 0;
}

void
plat_tem_get_colors(uint8_t *fg, uint8_t *bg)
{
	*fg = gfx_fg;
	*bg = gfx_bg;
}

void
plat_tem_get_inverses(int *inverse, int *inverse_screen)
{
	*inverse = gfx_inverse;
	*inverse_screen = gfx_inverse_screen;
}

/*
 * Support for color mapping.
 */
uint32_t
gfx_fb_color_map(uint8_t index)
{
	uint8_t c;
	int pos, size;
	uint32_t color;

	if (gfx_fb.common.framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB)
		return (index);

	c = cmap4_to_24.red[index];
	pos = gfx_fb.u.fb2.framebuffer_red_field_position;
	size = gfx_fb.u.fb2.framebuffer_red_mask_size;
	color = ((c >> 8 - size) & ((1 << size) - 1)) << pos;

	c = cmap4_to_24.green[index];
	pos = gfx_fb.u.fb2.framebuffer_green_field_position;
	size = gfx_fb.u.fb2.framebuffer_green_mask_size;
	color |= ((c >> 8 - size) & ((1 << size) - 1)) << pos;

	c = cmap4_to_24.blue[index];
	pos = gfx_fb.u.fb2.framebuffer_blue_field_position;
	size = gfx_fb.u.fb2.framebuffer_blue_mask_size;
	color |= ((c >> 8 - size) & ((1 << size) - 1)) << pos;

	return (color);
}

static int
color_name_to_ansi(const char *name, int *val)
{
	if (strcasecmp(name, "black") == 0) {
		*val = 0;
		return (0);
	}
	if (strcasecmp(name, "red") == 0) {
		*val = 1;
		return (0);
	}
	if (strcasecmp(name, "green") == 0) {
		*val = 2;
		return (0);
	}
	if (strcasecmp(name, "yellow") == 0) {
		*val = 3;
		return (0);
	}
	if (strcasecmp(name, "blue") == 0) {
		*val = 4;
		return (0);
	}
	if (strcasecmp(name, "magenta") == 0) {
		*val = 5;
		return (0);
	}
	if (strcasecmp(name, "cyan") == 0) {
		*val = 6;
		return (0);
	}
	if (strcasecmp(name, "white") == 0) {
		*val = 7;
		return (0);
	}
	return (1);
}

/* Callback to check and set colors */
static int
gfx_set_colors(struct env_var *ev, int flags, const void *value)
{
	int val = 0;
	char buf[2];
	const void *evalue;

	if (value == NULL)
		return (CMD_OK);

	if (color_name_to_ansi(value, &val)) {
		val = (int) strtol(value, NULL, 0);
		evalue = value;
	} else {
		snprintf(buf, sizeof (buf), "%d", val);
		evalue = buf;
	}

	/* invalid value? */
	if (val < 0 || val > 7)
		return (CMD_OK);

	if (strcmp(ev->ev_name, "tem.fg_color") == 0) {
		/* is it already set? */
		if (gfx_fg == val)
			return (CMD_OK);
		gfx_fg = val;
	}
	if (strcmp(ev->ev_name, "tem.bg_color") == 0) {
		/* is it already set? */
		if (gfx_bg == val)
			return (CMD_OK);
		gfx_bg = val;
	}
	env_setenv(ev->ev_name, flags | EV_NOHOOK, evalue, NULL, NULL);
	plat_cons_update_mode();
	return (CMD_OK);
}

/* Callback to check and set inverses */
static int
gfx_set_inverses(struct env_var *ev, int flags, const void *value)
{
	int t, f;

	if (value == NULL)
		return (CMD_OK);

	t = strcmp(value, "true");
	f = strcmp(value, "false");

	/* invalid value? */
	if (t != 0 && f != 0)
		return (CMD_OK);

	if (strcmp(ev->ev_name, "tem.inverse") == 0) {
		/* is it already set? */
		if (gfx_inverse == (t == 0))
			return (CMD_OK);
		gfx_inverse = (t == 0);
	}
	if (strcmp(ev->ev_name, "tem.inverse-screen") == 0) {
		/* is it already set? */
		if (gfx_inverse_screen == (t == 0))
			return (CMD_OK);
		gfx_inverse_screen = (t == 0);
	}
	env_setenv(ev->ev_name, flags | EV_NOHOOK, value, NULL, NULL);
	plat_cons_update_mode();
	return (CMD_OK);
}

/*
 * Initialize gfx framework.
 */
void
gfx_framework_init(struct visual_ops *fb_ops)
{
	int rc;
	char *env, buf[2];

	/* Add visual io callbacks */
	fb_ops->cons_clear = gfx_fb_cons_clear;
	fb_ops->cons_copy = gfx_fb_cons_copy;
	fb_ops->cons_display = gfx_fb_cons_display;

	/* set up tem inverse controls */
	env = getenv("tem.inverse");
	if (env != NULL) {
		if (strcmp(env, "true") == 0)
			gfx_inverse = 1;
		unsetenv("tem.inverse");
	}

	env = getenv("tem.inverse-screen");
	if (env != NULL) {
		if (strcmp(env, "true") == 0)
			gfx_inverse_screen = 1;
		unsetenv("tem.inverse-screen");
	}

	if (gfx_inverse)
		env = "true";
	else
		env = "false";
	env_setenv("tem.inverse", EV_VOLATILE, env, gfx_set_inverses,
            env_nounset);

	if (gfx_inverse_screen)
		env = "true";
	else
		env = "false";

	env_setenv("tem.inverse-screen", EV_VOLATILE, env, gfx_set_inverses,
            env_nounset);

	/* set up tem color controls */
	env = getenv("tem.fg_color");
	if (env != NULL) {
		rc = (int) strtol(env, NULL, 0);
		if (rc >= 0 && rc <= 7)
			gfx_fg = rc;
		unsetenv("tem.fg_color");
	}

	env = getenv("tem.bg_color");
	if (env != NULL) {
		rc = (int) strtol(env, NULL, 0);
		if (rc >= 0 && rc <= 7)
			gfx_bg = rc;
		unsetenv("tem.bg_color");
	}

	snprintf(buf, sizeof (buf), "%d", gfx_fg);
	env_setenv("tem.fg_color", EV_VOLATILE, buf, gfx_set_colors,
	    env_nounset);
	snprintf(buf, sizeof (buf), "%d", gfx_bg);
	env_setenv("tem.bg_color", EV_VOLATILE, buf, gfx_set_colors,
	    env_nounset);
}

/*
 * visual io callbacks.
 */

static int
gfx_fb_cons_clear(struct vis_consclear *ca)
{
	uint8_t *fb;
	uint32_t data, *fb32;
	uint16_t *fb16;
	int i, width, height, size, pitch;
#if defined (EFI)
	EFI_TPL tpl;
#endif

	fb = gfx_get_fb_address();
	pitch = gfx_fb.common.framebuffer_pitch;
	width = gfx_fb.common.framebuffer_width;
	height = gfx_fb.common.framebuffer_height;
	size = height * pitch;

	data = gfx_fb_color_map(ca->bg_color);
#if defined (EFI)
	tpl = BS->RaiseTPL(TPL_NOTIFY);
#endif
	switch (gfx_fb.common.framebuffer_bpp) {
	case 8:		/* 8 bit */
		for (i = 0; i < height; i++) {
			(void) memset(fb + i * pitch, ca->bg_color, pitch);
		}
		break;
	case 16:		/* 16 bit */
		fb16 = (uint16_t *)fb;
		for (i = 0; i < height * width; i++)
			fb16[i] = (uint16_t)data;
		break;
	case 24:		/* 24 bit */
		for (i = 0; i < size; i += 3) {
			fb[i] = (data >> 16) & 0xff;
			fb[i+1] = (data >> 8) & 0xff;
			fb[i+2] = data & 0xff;
		}
		break;
	case 32:		/* 32 bit */
		fb32 = (uint32_t *)fb;
		for (i = 0; i < height * width; i++)
			fb32[i] = data;
		break;
	}
#if defined (EFI)
	BS->RestoreTPL(tpl);
#endif

	return (0);
}

static void
gfx_fb_cons_copy(struct vis_conscopy *ma)
{
	uint32_t soffset, toffset;
	uint32_t width, height;
	uint8_t *src, *dst, *fb;
	int i, bpp, pitch;
#if defined (EFI)
	EFI_TPL tpl;
#endif

	fb = gfx_get_fb_address();
	bpp = gfx_fb.common.framebuffer_bpp >> 3;
	pitch = gfx_fb.common.framebuffer_pitch;

	soffset = ma->s_col * bpp + ma->s_row * pitch;
	toffset = ma->t_col * bpp + ma->t_row * pitch;
	src = fb + soffset;
	dst = fb + toffset;
	width = (ma->e_col - ma->s_col + 1) * bpp;
	height = ma->e_row - ma->s_row + 1;

#if defined (EFI)
	tpl = BS->RaiseTPL(TPL_NOTIFY);
#endif
	if (toffset <= soffset) {
		for (i = 0; i < height; i++) {
			uint32_t increment = i * pitch;
			(void) memmove(dst + increment, src + increment, width);
		}
	} else {
		for (i = height - 1; i >= 0; i--) {
			uint32_t increment = i * pitch;
			(void) memmove(dst + increment, src + increment, width);
		}
	}
#if defined (EFI)
	BS->RestoreTPL(tpl);
#endif
}

/*
 * Implements alpha blending for RGBA data, could use pixels for arguments,
 * but byte stream seems more generic.
 * The generic alpha blending is:
 * blend = alpha * fg + (1.0 - alpha) * bg.
 * Since our alpha is not from range [0..1], we scale appropriately.
 */
static uint8_t
alpha_blend(uint8_t fg, uint8_t bg, uint8_t alpha)
{
	uint16_t blend, h, l;

	/* trivial corner cases */
	if (alpha == 0)
		return (bg);
	if (alpha == 0xFF)
		return (fg);
	blend = (alpha * fg + (0xFF - alpha) * bg);
	/* Division by 0xFF */
	h = blend >> 8;
	l = blend & 0xFF;
	if (h + l >= 0xFF)
		h++;
	return (h);
}

/* Copy memory to framebuffer or to memory. */
static void
bitmap_cpy(uint8_t *dst, uint8_t *src, uint32_t len, int bpp)
{
	uint32_t i;
	uint8_t a;

	switch (bpp) {
	case 4:
		for (i = 0; i < len; i += bpp) {
			a = src[i+3];
			dst[i] = alpha_blend(src[i], dst[i], a);
			dst[i+1] = alpha_blend(src[i+1], dst[i+1], a);
			dst[i+2] = alpha_blend(src[i+2], dst[i+2], a);
			dst[i+3] = a;
		}
		break;
	default:
		(void) memcpy(dst, src, len);
		break;
	}
}

static void
gfx_fb_cons_display(struct vis_consdisplay *da)
{
	uint32_t size;		/* write size per scanline */
	uint8_t *fbp;		/* fb + calculated offset */
	int i, bpp, pitch;
#if defined (EFI)
	EFI_TPL tpl;
#endif

	/* make sure we will not write past FB */
	if (da->col >= gfx_fb.common.framebuffer_width ||
	    da->row >= gfx_fb.common.framebuffer_height ||
	    da->col + da->width > gfx_fb.common.framebuffer_width ||
	    da->row + da->height > gfx_fb.common.framebuffer_height)
		return;

	bpp = gfx_fb.common.framebuffer_bpp >> 3;
	pitch = gfx_fb.common.framebuffer_pitch;

	size = da->width * bpp;
	fbp = gfx_get_fb_address();
	fbp += da->col * bpp + da->row * pitch;

	/* write all scanlines in rectangle */
#if defined (EFI)
	tpl = BS->RaiseTPL(TPL_NOTIFY);
#endif
	for (i = 0; i < da->height; i++) {
		uint8_t *dest = fbp + i * pitch;
		uint8_t *src = da->data + i * size;
		bitmap_cpy(dest, src, size, bpp);
	}
#if defined (EFI)
	BS->RestoreTPL(tpl);
#endif
}

void
gfx_fb_display_cursor(struct vis_conscursor *ca)
{
	uint32_t fg, bg;
	uint32_t offset, size, *fb32;
	uint16_t *fb16;
	uint8_t *fb8, *fb;
	int i, j, bpp, pitch;
#if defined (EFI)
	EFI_TPL tpl;
#endif

	fb = gfx_get_fb_address();
	bpp = gfx_fb.common.framebuffer_bpp >> 3;
	pitch = gfx_fb.common.framebuffer_pitch;

	size = ca->width * bpp;

	/*
	 * Build cursor image. We are building mirror image of data on
	 * frame buffer by (D xor FG) xor BG.
	 */
	offset = ca->col * bpp + ca->row * pitch;
#if defined (EFI)
	tpl = BS->RaiseTPL(TPL_NOTIFY);
#endif
	switch (bpp) {
	case 1:		/* 8 bit */
		fg = ca->fg_color.mono;
		bg = ca->bg_color.mono;
		for (i = 0; i < ca->height; i++) {
			fb8 = fb + offset + i * pitch;
			for (j = 0; j < size; j += 1) {
				fb8[j] = (fb8[j] ^ (fg & 0xff)) ^ (bg & 0xff);
			}
		}
		break;
	case 2:		/* 16 bit */
		fg = ca->fg_color.sixteen[0] << 8;
		fg |= ca->fg_color.sixteen[1];
		bg = ca->bg_color.sixteen[0] << 8;
		bg |= ca->bg_color.sixteen[1];
		for (i = 0; i < ca->height; i++) {
			fb16 = (uint16_t *)(fb + offset + i * pitch);
			for (j = 0; j < ca->width; j++) {
				fb16[j] = (fb16[j] ^ (fg & 0xffff)) ^
				    (bg & 0xffff);
			}
		}
		break;
	case 3:		/* 24 bit */
		fg = ca->fg_color.twentyfour[0] << 16;
		fg |= ca->fg_color.twentyfour[1] << 8;
		fg |= ca->fg_color.twentyfour[2];
		bg = ca->bg_color.twentyfour[0] << 16;
		bg |= ca->bg_color.twentyfour[1] << 8;
		bg |= ca->bg_color.twentyfour[2];

		for (i = 0; i < ca->height; i++) {
			fb8 = fb + offset + i * pitch;
			for (j = 0; j < size; j += 3) {
				fb8[j] = (fb8[j] ^ ((fg >> 16) & 0xff)) ^
				    ((bg >> 16) & 0xff);
				fb8[j+1] = (fb8[j+1] ^ ((fg >> 8) & 0xff)) ^
				    ((bg >> 8) & 0xff);
				fb8[j+2] = (fb8[j+2] ^ (fg & 0xff)) ^
				    (bg & 0xff);
			}
		}
		break;
	case 4:		/* 32 bit */
		fg = ca->fg_color.twentyfour[0] << 16;
		fg |= ca->fg_color.twentyfour[1] << 8;
		fg |= ca->fg_color.twentyfour[2];
		bg = ca->bg_color.twentyfour[0] << 16;
		bg |= ca->bg_color.twentyfour[1] << 8;
		bg |= ca->bg_color.twentyfour[2];
		for (i = 0; i < ca->height; i++) {
			fb32 = (uint32_t *)(fb + offset + i * pitch);
			for (j = 0; j < ca->width; j++)
				fb32[j] = (fb32[j] ^ fg) ^ bg;
		}
		break;
	}
#if defined (EFI)
	BS->RestoreTPL(tpl);
#endif
}

/*
 * Public graphics primitives.
 */

/* set pixel in framebuffer using gfx coordinates */
void
gfx_fb_setpixel(int x, int y)
{
	uint32_t c, offset, pitch, bpp;
	uint8_t *fb;
	text_color_t fg, bg;

	if (plat_stdout_is_framebuffer() == 0)
		return;

	tem_get_colors((tem_vt_state_t)tems.ts_active, &fg, &bg);
	c = gfx_fb_color_map(fg);
	if (x >= gfx_fb.common.framebuffer_width ||
	    y >= gfx_fb.common.framebuffer_height)
		return;

	fb = gfx_get_fb_address();
	pitch = gfx_fb.common.framebuffer_pitch;
	bpp = gfx_fb.common.framebuffer_bpp >> 3;

	offset = y * pitch + x * bpp;
	switch (gfx_fb.common.framebuffer_bpp) {
	case 8:
		fb[offset] = c & 0xff;
		break;
	case 16:
		*(uint16_t *)(fb + offset) = c & 0xffff;
		break;
	case 24:
		fb[offset] = (c >> 16) & 0xff;
		fb[offset + 1] = (c >> 8) & 0xff;
		fb[offset + 2] = c & 0xff;
		break;
	case 32:
		*(uint32_t *)(fb + offset) = c;
		break;
	}
}

/*
 * draw rectangle in framebuffer using gfx coordinates.
 * The function is borrowed from fbsd vt_fb.c
 */
void
gfx_fb_drawrect(int x1, int y1, int x2, int y2, int fill)
{
	int x, y;

	if (plat_stdout_is_framebuffer() == 0)
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

void
gfx_fb_line(int x0, int y0, int x1, int y1)
{
	int dx, sx, dy, sy;
	int err, e2;

	sx = x0 < x1? 1:-1;
	if (sx > 0)
		dx = x1 - x0;
	else
		dx = -(x1 - x0);
	sy = y0 < y1? 1:-1;
	if (sy > 0)
		dy = -(y1 - y0);
	else
		dy = y1 - y0;
	err = dx + dy;

	for (;;) {
		gfx_fb_setpixel(x0, y0);
		if (x0 == x1 && y0 == y1)
			break;
		e2 = err << 1;
		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

/*
 * quadratic Bézier curve limited to gradients without sign change.
 */
void
gfx_fb_bezier(int x0, int y0, int x1, int y1, int x2, int y2, int width)
{
	int sx, sy, xx, yy, xy;
	int dx, dy, err, curvature;
	int i;

	sx = x2 - x1;
	sy = y2 - y1;
	xx = x0 - x1;
	yy = y0 - y1;
	curvature = xx*sy - yy*sx;

	if (sx*sx + sy*sy > xx*xx+yy*yy) {
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
		xy = (xx*yy) << 1;
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
			for (i = 0; i <= width; i++)
				gfx_fb_setpixel(x0 + i, y0);
			if (x0 == x2 && y0 == y2)
				return;  /* last pixel -> curve finished */
			y1 = 2 * err < dx;
			if (2 * err > dy) {
				x0 += sx;
				dx -= xy;
				dy += yy;
				err += dy;
			}
			if (y1 != 0) {
				y0 += sy;
				dy -= xy;
				dx += xx;
				err += dx;
			}
		} while (dy < dx ); /* gradient negates -> algorithm fails */
	}
	gfx_fb_line(x0, y0, x2, y2);
}

/*
 * draw rectangle using terminal coordinates and current foreground color.
 */
void
gfx_term_drawrect(int row1, int col1, int row2, int col2)
{
	int x1, y1, x2, y2, xb, yb;
	int xshift, yshift;
	int width, i;

	if (plat_stdout_is_framebuffer() == 0)
		return;

	width = tems.ts_font.width / 4;			/* line width */
	xshift = (tems.ts_font.width - width) / 2;
	yshift = (tems.ts_font.height - width) / 2;
	/* Terminal coordinates start from (1,1) */
	row1--;
	col1--;
	row2--;
	col2--;

	/*
	 * Draw horizontal lines width points thick, shifted from outer edge.
	 */
	x1 = row1 * tems.ts_font.width + tems.ts_p_offset.x;
	x1 += tems.ts_font.width;
	y1 = col1 * tems.ts_font.height + tems.ts_p_offset.y + yshift;
	x2 = row2 * tems.ts_font.width + tems.ts_p_offset.x;
	gfx_fb_drawrect(x1, y1, x2, y1 + width, 1);
	y2 = col2 * tems.ts_font.height + tems.ts_p_offset.y;
	y2 += tems.ts_font.height - yshift - width;
	gfx_fb_drawrect(x1, y2, x2, y2 + width, 1);

	/*
	 * Draw vertical lines width points thick, shifted from outer edge.
	 */
	x1 = row1 * tems.ts_font.width + tems.ts_p_offset.x + xshift;
	y1 = col1 * tems.ts_font.height + tems.ts_p_offset.y;
	y1 += tems.ts_font.height;
	y2 = col2 * tems.ts_font.height + tems.ts_p_offset.y;
	gfx_fb_drawrect(x1, y1, x1 + width, y2, 1);
	x1 = row2 * tems.ts_font.width + tems.ts_p_offset.x;
	x1 += tems.ts_font.width - xshift - width;
	gfx_fb_drawrect(x1, y1, x1 + width, y2, 1);

	/* Draw upper left corner. */
	x1 = row1 * tems.ts_font.width + tems.ts_p_offset.x + xshift;
	y1 = col1 * tems.ts_font.height + tems.ts_p_offset.y;
	y1 += tems.ts_font.height;

	x2 = row1 * tems.ts_font.width + tems.ts_p_offset.x;
	x2 += tems.ts_font.width;
	y2 = col1 * tems.ts_font.height + tems.ts_p_offset.y + yshift;
	for (i = 0; i <= width; i++)
		gfx_fb_bezier(x1 + i, y1, x1 + i, y2 + i, x2, y2 + i, width-i);

	/* Draw lower left corner. */
	x1 = row1 * tems.ts_font.width + tems.ts_p_offset.x;
	x1 += tems.ts_font.width;
	y1 = col2 * tems.ts_font.height + tems.ts_p_offset.y;
	y1 += tems.ts_font.height - yshift;
	x2 = row1 * tems.ts_font.width + tems.ts_p_offset.x + xshift;
	y2 = col2 * tems.ts_font.height + tems.ts_p_offset.y;
	for (i = 0; i <= width; i++)
		gfx_fb_bezier(x1, y1 - i, x2 + i, y1 - i, x2 + i, y2, width-i);

	/* Draw upper right corner. */
	x1 = row2 * tems.ts_font.width + tems.ts_p_offset.x;
	y1 = col1 * tems.ts_font.height + tems.ts_p_offset.y + yshift;
	x2 = row2 * tems.ts_font.width + tems.ts_p_offset.x;
	x2 += tems.ts_font.width - xshift - width;
	y2 = col1 * tems.ts_font.height + tems.ts_p_offset.y;
	y2 += tems.ts_font.height;
	for (i = 0; i <= width; i++)
		gfx_fb_bezier(x1, y1 + i, x2 + i, y1 + i, x2 + i, y2, width-i);

	/* Draw lower right corner. */
	x1 = row2 * tems.ts_font.width + tems.ts_p_offset.x;
	y1 = col2 * tems.ts_font.height + tems.ts_p_offset.y;
	y1 += tems.ts_font.height - yshift;
	x2 = row2 * tems.ts_font.width + tems.ts_p_offset.x;
	x2 += tems.ts_font.width - xshift - width;
	y2 = col2 * tems.ts_font.height + tems.ts_p_offset.y;
	for (i = 0; i <= width; i++)
		gfx_fb_bezier(x1, y1 - i, x2 + i, y1 - i, x2 + i, y2, width-i);
}
