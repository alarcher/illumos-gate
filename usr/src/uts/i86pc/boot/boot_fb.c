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
 * dboot and early kernel needs simple putchar(int) interface to implement
 * printf() support. So we implement simple interface on top of
 * linear frame buffer, since we can not use tem directly, we are
 * just borrowing bits from it.
 *
 * Note, this implementation is assuming UEFI linear frame buffer and
 * 32bit depth, which should not be issue as GOP is supposed to provide those.
 * At the time of writing, this is the only case for frame buffer anyhow.
 */

#include <sys/types.h>
#include <sys/systm.h>
#include <sys/multiboot2.h>
#include <sys/framebuffer.h>
#include <sys/bootinfo.h>
#include <sys/boot_console.h>

/*
 * Simplified visual_io data structures from visual_io.h
 */

struct vis_consdisplay {
	uint16_t row;		/* Row to display data at */
	uint16_t col;		/* Col to display data at */
	uint16_t width;		/* Width of data */
	uint16_t height;	/* Height of data */
	uint8_t  *data;		/* Data to display */
};

struct vis_conscopy {
	uint16_t s_row;		/* Starting row */
	uint16_t s_col;		/* Starting col */
	uint16_t e_row;		/* Ending row */
	uint16_t e_col;		/* Ending col */
	uint16_t t_row;		/* Row to move to */
	uint16_t t_col;		/* Col to move to */
};

/* we have built in fonts 12x22, 6x10, 7x14 and depth 32. */
#define	MAX_GLYPH	(12 * 22 * 4)

static struct font	boot_fb_font; /* set by set_font() */
static uint8_t		glyph[MAX_GLYPH];
static uint8_t		blank[MAX_GLYPH];
static uint8_t		cursor[MAX_GLYPH];
static uint32_t		last_line_size;
static fb_info_pixel_coord_t last_line;
static fb_info_t	*fb;

#define	WHITE		(0)
#define	BLACK		(1)
#define	WHITE_16	(0xFFFF)
#define	BLACK_16	(0x0000)
#define	WHITE_32	(0xFFFFFFFF)
#define	BLACK_32	(0x00000000)

/*
 * extract data from MB2 framebuffer tag and set up initial frame buffer.
 */
boolean_t
xbi_fb_init(struct xboot_info *xbi)
{
	multiboot_tag_framebuffer_t *tag;

	if (xbi->bi_framebuffer == NULL)
		return (B_FALSE);
	tag = (multiboot_tag_framebuffer_t *)(uintptr_t)xbi->bi_framebuffer;

	if (tag->common.framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB)
		return (B_FALSE);

	fb_info.paddr = tag->common.framebuffer_addr;
	fb_info.pitch = tag->common.framebuffer_pitch;
	fb_info.bpp = tag->common.framebuffer_bpp >> 3;
	fb_info.depth = tag->common.framebuffer_bpp;
	fb_info.screen.x = tag->common.framebuffer_width;
	fb_info.screen.y = tag->common.framebuffer_height;

	fb_info.rgb.red.size = tag->u.fb2.framebuffer_red_mask_size;
	fb_info.rgb.red.pos = tag->u.fb2.framebuffer_red_field_position;
	fb_info.rgb.green.size = tag->u.fb2.framebuffer_green_mask_size;
	fb_info.rgb.green.pos = tag->u.fb2.framebuffer_green_field_position;
	fb_info.rgb.blue.size = tag->u.fb2.framebuffer_blue_mask_size;
	fb_info.rgb.blue.pos = tag->u.fb2.framebuffer_blue_field_position;

	fb_info.inverse = B_FALSE;
	fb_info.inverse_screen = B_FALSE;
	return (B_TRUE);
}

/* set font and pass the data to fb_info */
static void
boot_fb_set_font(uint16_t height, uint16_t width)
{
	set_font(&boot_fb_font, (short *)&fb_info.terminal.y,
	    (short *)&fb_info.terminal.x, (short)height, (short)width);
	fb_info.font_width = boot_fb_font.width;
	fb_info.font_height = boot_fb_font.height;
}

/* set boot frame buffer pointer */
void
boot_fb_set(fb_info_t *fbp)
{
	fb = fbp;
}

/* set up out simple console. */
void
boot_fb_init(int console)
{
	fb_info_pixel_coord_t window;
	int i;

	/* frame buffer address is mapped in dboot. */
	fb_info.fb = (uint8_t *)(uintptr_t)fb_info.paddr;
	fb_info.fb_size = fb_info.screen.x * fb_info.screen.y * fb_info.bpp;

	boot_fb_set_font(fb_info.screen.y, fb_info.screen.x);
	window.x =
	    (fb_info.screen.x - fb_info.terminal.x * boot_fb_font.width) / 2;
	window.y =
	    (fb_info.screen.y - fb_info.terminal.y * boot_fb_font.height) / 2;
	fb_info.terminal_origin.x = window.x;
	fb_info.terminal_origin.y = window.y;
	fb_info.cursor.origin.x = window.x;
	fb_info.cursor.origin.y = window.y;
	fb_info.cursor.pos.x = 0;
	fb_info.cursor.pos.y = 0;
	fb_info.cursor.visible = B_TRUE;

	/* clear the screen */
	if (console == CONS_FRAMEBUFFER) {
		for (i = 0; i < fb_info.screen.y; i++) {
			uint8_t *dest = fb_info.fb + i * fb_info.pitch;
			(void) memset(dest, 0, fb_info.pitch);
		}
	}

	/* set up pre-calculated last line */
	last_line_size = fb_info.terminal.x * boot_fb_font.width *
	    fb_info.bpp;
	last_line.x = window.x;
	last_line.y = window.y + (fb_info.terminal.y - 1) * boot_fb_font.height;

	/* set framefuffer pointer */
	boot_fb_set(&fb_info);
}

/* copy rectangle to framebuffer. */
static void
boot_fb_blit(struct vis_consdisplay *rect)
{
	uint32_t size;	/* write size per scanline */
	uint8_t *fbp, *sfbp;	/* fb + calculated offset */
	int i;

	/* make sure we will not write past FB */
	if (rect->col >= fb->screen.x ||
	    rect->row >= fb->screen.y ||
	    rect->col + rect->width > fb->screen.x ||
	    rect->row + rect->height > fb->screen.y)
		return;

	size = rect->width * fb->bpp;
	fbp = fb->fb + rect->col * fb->bpp +
	    rect->row * fb->pitch;
	if (fb->shadow_fb != NULL) {
		sfbp = fb->shadow_fb + rect->col * fb->bpp +
		    rect->row * fb->pitch;
	} else {
		sfbp = NULL;
	}

	/* write all scanlines in rectangle */
	for (i = 0; i < rect->height; i++) {
		uint8_t *dest = fbp + i * fb->pitch;
		uint8_t *src = rect->data + i * size;
		(void) memcpy(dest, src, size);
		if (sfbp != NULL) {
			dest = sfbp + i * fb->pitch;
			(void) memcpy(dest, src, size);
		}
	}
}

static void
bit_to_pix(uchar_t c)
{
	switch (fb->depth) {
	case 8:
		font_bit_to_pix8(&boot_fb_font, (uint8_t *)glyph, c,
		    WHITE, BLACK);
		break;
	case 15:
	case 16:
		font_bit_to_pix16(&boot_fb_font, (uint16_t *)glyph, c,
		    WHITE_16, BLACK_16);
		break;
	case 24:
		font_bit_to_pix24(&boot_fb_font, (uint8_t *)glyph, c,
		    WHITE_32, BLACK_32);
		break;
	case 32:
		font_bit_to_pix32(&boot_fb_font, (uint32_t *)glyph, c,
		    WHITE_32, BLACK_32);
		break;
	}
}

/*
 * move the terminal window lines [1..y] to [0..y-1] and clear last line.
 */
static void
boot_fb_scroll(void)
{
	struct vis_conscopy c_copy;
	uint32_t soffset, toffset;
	uint32_t width, height;
	uint8_t *src, *dst, *sdst;
	int i;

	/* support for scrolling. set up the console copy data and last line */
	c_copy.s_row = fb->terminal_origin.y + boot_fb_font.height;
	c_copy.s_col = fb->terminal_origin.x;
	c_copy.e_row = fb->screen.y - fb->terminal_origin.y;
	c_copy.e_col = fb->screen.x - fb->terminal_origin.x;
	c_copy.t_row = fb->terminal_origin.y;
	c_copy.t_col = fb->terminal_origin.x;

	soffset = c_copy.s_col * fb->bpp + c_copy.s_row * fb->pitch;
	toffset = c_copy.t_col * fb->bpp + c_copy.t_row * fb->pitch;
	if (fb->shadow_fb != NULL) {
		src = fb->shadow_fb + soffset;
		sdst = fb->shadow_fb + toffset;
	} else {
		src = fb->fb + soffset;
		sdst = NULL;
	}
	dst = fb->fb + toffset;

	width = (c_copy.e_col - c_copy.s_col + 1) * fb->bpp;
	height = c_copy.e_row - c_copy.s_row + 1;
	for (i = 0; i < height; i++) {
		uint32_t increment = i * fb->pitch;
		(void) memcpy(dst + increment, src + increment, width);
		if (sdst != NULL)
			(void) memcpy(sdst + increment, src + increment, width);
	}

	/* now clean up the last line */
	toffset = last_line.x * fb->bpp + last_line.y * fb->pitch;
	dst = fb->fb + toffset;
	for (i = 0; i < boot_fb_font.height; i++) {
		uint8_t *dest = dst + i * fb->pitch;
		(void) memset(dest, 0, last_line_size);
		if (sdst != NULL) {
			dest = sdst + i * fb->pitch;
			(void) memset(dest, 0, last_line_size);
		}
	}
}

/*
 * Very simple block cursor. Save space below the cursor and restore
 * when cursor is invisible. Of course the space below is usually black
 * screen, but never know when someone will add kmdb to have support for
 * arrow keys... kmdb is the only possible consumer for such case.
 */
static void
boot_fb_cursor_create(struct vis_consdisplay *cur)
{
	uint32_t offset;
	uint8_t *src, *dst;
	uint32_t size = cur->width * fb->bpp;
	int i;

	/* save data under the cursor to blank */
	offset = cur->col * fb->bpp + cur->row * fb->pitch;
	for (i = 0; i < cur->height; i++) {
		src = fb->fb + offset + i * fb->pitch;
		dst = blank + i * size;
		(void) memcpy(dst, src, size);
	}
	/* set cursor buffer */
	for (i = 0; i < cur->height * size; i++) {
		cur->data[i] = 0xFF ^ blank[i];
	}
}

void
boot_fb_cursor(boolean_t visible)
{
	struct vis_consdisplay cur;

	fb_info.cursor.visible = visible;
	cur.col = fb->cursor.origin.x;
	cur.row = fb->cursor.origin.y;
	cur.width = boot_fb_font.width;
	cur.height = boot_fb_font.height;

	if (visible == B_TRUE) {
		cur.data = cursor;
		boot_fb_cursor_create(&cur);
	} else {
		cur.data = blank;
	}
	boot_fb_blit(&cur);
}

static void
set_cursor_row(void)
{
	fb->cursor.pos.y++;
	fb->cursor.pos.x = 0;
	fb->cursor.origin.x = fb->terminal_origin.x;

	if (fb->cursor.pos.y < fb->terminal.y) {
		fb->cursor.origin.y += boot_fb_font.height;
	} else {
		fb->cursor.pos.y--;
		boot_fb_scroll();
	}
}

static void
set_cursor_col(void)
{
	fb->cursor.pos.x++;
	if (fb->cursor.pos.x < fb->terminal.x) {
		fb->cursor.origin.x += boot_fb_font.width;
	} else {
		fb->cursor.pos.x = 0;
		fb->cursor.origin.x = fb->terminal_origin.x;
		set_cursor_row();
	}
}

void
boot_fb_putchar(uint8_t c)
{
	struct vis_consdisplay display;
	boolean_t bs = B_FALSE;
	boolean_t cstate = fb->cursor.visible;

	/* early tem startup will switch cursor off, if so, keep it off  */
	if (cstate == B_TRUE)
		boot_fb_cursor(B_FALSE);	/* cursor off */
	switch (c) {
	case '\n':
		set_cursor_row();
		if (cstate == B_TRUE)
			boot_fb_cursor(B_TRUE);
		return;
	case '\r':
		fb->cursor.pos.x = 0;
		fb->cursor.origin.x = fb->terminal_origin.x;
		if (cstate == B_TRUE)
			boot_fb_cursor(B_TRUE);
		return;
	case '\b':
		if (fb->cursor.pos.x > 0) {
			fb->cursor.pos.x--;
			fb->cursor.origin.x -= boot_fb_font.width;
		}
		c = ' ';
		bs = B_TRUE;
		break;
	}

	bit_to_pix(c);
	display.col = fb->cursor.origin.x;
	display.row = fb->cursor.origin.y;
	display.width = boot_fb_font.width;
	display.height = boot_fb_font.height;
	display.data = glyph;

	boot_fb_blit(&display);
	if (bs == B_FALSE)
		set_cursor_col();
	if (cstate == B_TRUE)
		boot_fb_cursor(B_TRUE);
}
