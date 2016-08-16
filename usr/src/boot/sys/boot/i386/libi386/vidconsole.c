/*-
 * Copyright (c) 1998 Michael Smith (msmith@freebsd.org)
 * Copyright (c) 1997 Kazutaka YOKOTA (yokota@zodiac.mech.utsunomiya-u.ac.jp)
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * 	Id: probe_keyboard.c,v 1.13 1997/06/09 05:10:55 bde Exp
 */

#include <sys/cdefs.h>

#include <stand.h>
#include <bootstrap.h>
#include <sys/tem.h>
#include <sys/visual_io.h>
#include <sys/multiboot2.h>
#include <btxv86.h>
#include <machine/psl.h>
#include <machine/metadata.h>
#include "libi386.h"
#include "vbe.h"
#include <gfx_fb.h>
#include <sys/vgareg.h>
#include <sys/vgasubr.h>
#include <machine/cpufunc.h>

#if KEYBOARD_PROBE

static int	probe_keyboard(void);
#endif
static void	vidc_probe(struct console *cp);
static int	vidc_init(struct console *cp, int arg);
static void	vidc_putchar(struct console *cp, int c);
static int	vidc_getchar(struct console *cp);
static int	vidc_ischar(struct console *cp);
static int	vidc_ioctl(struct console *cp, int cmd, void *data);
static void	vidc_biosputchar(int c);

static int vidc_vbe_devinit(struct vis_devinit *);
static void vidc_cons_cursor(struct vis_conscursor *);
static int vidc_vbe_cons_put_cmap(struct vis_cmap *);

static int vidc_text_devinit(struct vis_devinit *);
static int vidc_text_cons_clear(struct vis_consclear *);
static void vidc_text_cons_copy(struct vis_conscopy *);
static void vidc_text_cons_display(struct vis_consdisplay *);
static void vidc_text_set_cursor(screen_pos_t, screen_pos_t, boolean_t);
static void vidc_text_get_cursor(screen_pos_t *, screen_pos_t *);
static int vidc_text_cons_put_cmap(struct vis_cmap *);

static int vidc_started;
static uint16_t	*vgatext;

static const unsigned char solaris_color_to_pc_color[16] = {
	15, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14
};

/* mode change callback and argument from tem */
static vis_modechg_cb_t modechg_cb;
static struct vis_modechg_arg *modechg_arg;
static tem_vt_state_t tem;

#define	KEYBUFSZ	10
#define DEFAULT_FGCOLOR	7
#define DEFAULT_BGCOLOR	0

static uint8_t	keybuf[KEYBUFSZ];	/* keybuf for extended codes */

struct console text = {
	.c_name = "text",
	.c_desc = "internal video/keyboard",
	.c_flags = 0,
	.c_probe = vidc_probe,
	.c_init = vidc_init,
	.c_out = vidc_putchar,
	.c_in = vidc_getchar,
	.c_ready = vidc_ischar,
	.c_ioctl = vidc_ioctl,
	.c_private = NULL
};

static struct vis_identifier fb_ident = { "vidc_fb" };
static struct vis_identifier text_ident = { "vidc_text" };

struct visual_ops fb_ops = {
	.ident = &fb_ident,
	.kdsetmode = NULL,
	.devinit = vidc_vbe_devinit,
	.cons_copy = NULL,
	.cons_display = NULL,
	.cons_cursor = vidc_cons_cursor,
	.cons_clear = NULL,
	.cons_put_cmap = vidc_vbe_cons_put_cmap
};

struct visual_ops text_ops = {
	.ident = &text_ident,
	.kdsetmode = NULL,
	.devinit = vidc_text_devinit,
	.cons_copy = vidc_text_cons_copy,
	.cons_display = vidc_text_cons_display,
	.cons_cursor = vidc_cons_cursor,
	.cons_clear = vidc_text_cons_clear,
	.cons_put_cmap = vidc_text_cons_put_cmap
};

/*
 * platform specific functions for tem
 */
int
plat_stdout_is_framebuffer(void)
{
	if (vbe_available() && VBE_VALID_MODE(vbe_get_mode())) {
		return (1);
	}
        return (0);
}

void
plat_tem_hide_prom_cursor(void)
{
	vidc_text_set_cursor(0, 0, B_FALSE);
}

void
plat_tem_get_prom_pos(uint32_t *row, uint32_t *col)
{
	screen_pos_t x, y;

	if (plat_stdout_is_framebuffer()) {
		*row = 0;
		*col = 0;
	} else {
		vidc_text_get_cursor(&y, &x);
		*row = (uint32_t) y;
		*col = (uint32_t) x;
	}
}

/*
 * plat_tem_get_prom_size() is supposed to return screen size
 * in chars. Return real data for text mode and TEM defaults for graphical
 * mode, so the tem can compute values based on default and font.
 */
void
plat_tem_get_prom_size(size_t *height, size_t *width)
{
	if (plat_stdout_is_framebuffer()) {
		*height = TEM_DEFAULT_ROWS;
		*width = TEM_DEFAULT_COLS;
	} else {
		*height = TEXT_ROWS;
		*width = TEXT_COLS;
	}
}

void
plat_cons_update_mode(void)
{
	struct vis_devinit devinit;

	if (tem == NULL)	/* tem is not set up */
		return;

	if (plat_stdout_is_framebuffer()) {
		devinit.version = VIS_CONS_REV;
		devinit.width = gfx_fb.common.framebuffer_width;
		devinit.height = gfx_fb.common.framebuffer_height;
		devinit.depth = gfx_fb.common.framebuffer_bpp;
		devinit.linebytes = gfx_fb.common.framebuffer_pitch;
		devinit.color_map = gfx_fb_color_map;
		devinit.mode = VIS_PIXEL;
		text.c_private = &fb_ops;
	} else {
		devinit.version = VIS_CONS_REV;
		devinit.width = TEXT_COLS;
		devinit.height = TEXT_ROWS;
		devinit.depth = 4;
		devinit.linebytes = TEXT_COLS;
		devinit.color_map = NULL;
		devinit.mode = VIS_TEXT;
		text.c_private = &text_ops;
	}

	modechg_cb(modechg_arg, &devinit);
}

static int
vidc_vbe_devinit(struct vis_devinit *devinit)
{
	if (plat_stdout_is_framebuffer() == 0)
		return (1);

	devinit->version = VIS_CONS_REV;
	devinit->width = gfx_fb.common.framebuffer_width;
	devinit->height = gfx_fb.common.framebuffer_height;
	devinit->depth = gfx_fb.common.framebuffer_bpp;
	devinit->linebytes = gfx_fb.common.framebuffer_pitch;
	devinit->color_map = gfx_fb_color_map;
	devinit->mode = VIS_PIXEL;

	modechg_cb = devinit->modechg_cb;
	modechg_arg = devinit->modechg_arg;

	return (0);
}

static int
vidc_text_devinit(struct vis_devinit *devinit)
{
	if (plat_stdout_is_framebuffer())
		return (1);

	devinit->version = VIS_CONS_REV;
	devinit->width = TEXT_COLS;
	devinit->height = TEXT_ROWS;
	devinit->depth = 4;
	devinit->linebytes = TEXT_COLS;
	devinit->color_map = NULL;
	devinit->mode = VIS_TEXT;

	modechg_cb = devinit->modechg_cb;
	modechg_arg = devinit->modechg_arg;

	return (0);
}

static int
vidc_text_cons_clear(struct vis_consclear *ca)
{
	uint16_t val;
	int i;

	val = (solaris_color_to_pc_color[ca->bg_color & 0xf] << 4) |
	    DEFAULT_FGCOLOR;
	val = (val << 8) | ' ';

	for (i = 0; i < TEXT_ROWS * TEXT_COLS; i++)
		vgatext[i] = val;

	return (0);
}

static void
vidc_text_cons_copy(struct vis_conscopy *ma)
{
	uint16_t  *from;
	uint16_t *to;
	int cnt;
	screen_size_t chars_per_row;
	uint16_t *to_row_start;
	uint16_t *from_row_start;
	screen_size_t rows_to_move;
	uint16_t *base;

	/*
	 * Sanity checks.  Note that this is a last-ditch effort to avoid
	 * damage caused by broken-ness or maliciousness above.
	 */
	if (ma->s_col < 0 || ma->s_col >= TEXT_COLS ||
	    ma->s_row < 0 || ma->s_row >= TEXT_ROWS ||
	    ma->e_col < 0 || ma->e_col >= TEXT_COLS ||
	    ma->e_row < 0 || ma->e_row >= TEXT_ROWS ||
	    ma->t_col < 0 || ma->t_col >= TEXT_COLS ||
	    ma->t_row < 0 || ma->t_row >= TEXT_ROWS ||
	    ma->s_col > ma->e_col ||
	    ma->s_row > ma->e_row)
		return;

	/*
	 * Remember we're going to copy shorts because each
	 * character/attribute pair is 16 bits.
	 */
	chars_per_row = ma->e_col - ma->s_col + 1;
	rows_to_move = ma->e_row - ma->s_row + 1;

	/* More sanity checks. */
	if (ma->t_row + rows_to_move > TEXT_ROWS ||
	    ma->t_col + chars_per_row > TEXT_COLS)
		return;

	base = vgatext;

	to_row_start = base + ((ma->t_row * TEXT_COLS) + ma->t_col);
	from_row_start = base + ((ma->s_row * TEXT_COLS) + ma->s_col);

	if (to_row_start < from_row_start) {
		while (rows_to_move-- > 0) {
			to = to_row_start;
			from = from_row_start;
			to_row_start += TEXT_COLS;
			from_row_start += TEXT_COLS;
			for (cnt = chars_per_row; cnt-- > 0; )
				*to++ = *from++;
		}
	} else {
		/*
		 * Offset to the end of the region and copy backwards.
		 */
		cnt = rows_to_move * TEXT_COLS + chars_per_row;
		to_row_start += cnt;
		from_row_start += cnt;

		while (rows_to_move-- > 0) {
			to_row_start -= TEXT_COLS;
			from_row_start -= TEXT_COLS;
			to = to_row_start;
			from = from_row_start;
			for (cnt = chars_per_row; cnt-- > 0; )
				*--to = *--from;
		}
	}
}

static void
vidc_text_cons_display(struct vis_consdisplay *da)
{
	int i;
	uint8_t attr;
	struct cgatext {
		uint8_t ch;
		uint8_t attr;
	} *addr;

	attr = (solaris_color_to_pc_color[da->bg_color & 0xf] << 4) |
	    solaris_color_to_pc_color[da->fg_color & 0xf];
	addr = (struct cgatext *) vgatext + (da->row * TEXT_COLS + da->col);

	for (i = 0; i < da->width; i++) {
		addr[i].ch = da->data[i];
		addr[i].attr = attr;
	}
}

static void
vidc_text_set_cursor(screen_pos_t row, screen_pos_t col, boolean_t visible)
{
	uint16_t addr;
	uint8_t msl, s, e;

	msl = vga_get_crtc(VGA_REG_ADDR, VGA_CRTC_MAX_S_LN) & 0x1f;
	s = vga_get_crtc(VGA_REG_ADDR, VGA_CRTC_CSSL) & 0xC0;
	e = vga_get_crtc(VGA_REG_ADDR, VGA_CRTC_CESL);

	if (visible == B_TRUE) {
		addr = row * TEXT_COLS + col;
		vga_set_crtc(VGA_REG_ADDR, VGA_CRTC_CLAH, addr >> 8);
		vga_set_crtc(VGA_REG_ADDR, VGA_CRTC_CLAL, addr & 0xff);
		e = msl;
	} else {
		s |= (1<<5);
	}
	vga_set_crtc(VGA_REG_ADDR, VGA_CRTC_CSSL, s);
	vga_set_crtc(VGA_REG_ADDR, VGA_CRTC_CESL, e);
}

static void
vidc_text_get_cursor(screen_pos_t *row, screen_pos_t *col)
{
	uint16_t addr;

	addr = (vga_get_crtc(VGA_REG_ADDR, VGA_CRTC_CLAH) << 8) +
	    vga_get_crtc(VGA_REG_ADDR, VGA_CRTC_CLAL);

	*row = addr / TEXT_COLS;
	*col = addr % TEXT_COLS;
}

static void
vidc_cons_cursor(struct vis_conscursor *cc)
{
	switch (cc->action) {
	case VIS_HIDE_CURSOR:
		if (plat_stdout_is_framebuffer())
			gfx_fb_display_cursor(cc);
		else
			vidc_text_set_cursor(cc->row, cc->col, B_FALSE);
		break;
	case VIS_DISPLAY_CURSOR:
		if (plat_stdout_is_framebuffer())
			gfx_fb_display_cursor(cc);
		else
			vidc_text_set_cursor(cc->row, cc->col, B_TRUE);
		break;
	case VIS_GET_CURSOR:
		if (plat_stdout_is_framebuffer()) {
			cc->row = 0;
			cc->col = 0;
		} else {
			vidc_text_get_cursor(&cc->row, &cc->col);
		}
		break;
	}
}

static int
vidc_vbe_cons_put_cmap(struct vis_cmap *cm)
{
	int i, bits, rc = 0;
	struct paletteentry pe;

	bits = 0;
	rc = biosvbe_palette_format(&bits);
	if (rc != VBE_SUCCESS)
		return (rc);

	bits = 8 - (bits >> 8);
	pe.Alignment = 0xFF;
	for (i = 0; i < cm->count; i++) {
		pe.Red = cm->red[i] >> bits;
		pe.Green = cm->green[i] >> bits;
		pe.Blue = cm->blue[i] >> bits;
		rc = vbe_set_palette(&pe, cm->index + i);
		if (rc != 0)
			break;
	}
	return (rc);
}

static int
vidc_text_cons_put_cmap(struct vis_cmap *cm)
{
	return (1);
}

static int
vidc_ioctl(struct console *cp, int cmd, void *data)
{
	struct visual_ops *ops = cp->c_private;

	switch (cmd) {
	case VIS_GETIDENTIFIER:
		memmove(data, ops->ident, sizeof (struct vis_identifier));
		break;
	case VIS_DEVINIT:
		return (ops->devinit(data));
	case VIS_CONSCLEAR:
		return (ops->cons_clear(data));
	case VIS_CONSCOPY:
		ops->cons_copy(data);
		break;
	case VIS_CONSDISPLAY:
		ops->cons_display(data);
		break;
	case VIS_CONSCURSOR:
		ops->cons_cursor(data);
		break;
	case VIS_PUTCMAP:
		ops->cons_put_cmap(data);
		break;
	case VIS_GETCMAP:
	default:
		return (EINVAL);
	}
	return (0);
}

static void
vidc_probe(struct console *cp)
{

	/* look for a keyboard */
#if KEYBOARD_PROBE
	if (probe_keyboard())
#endif
	{
		cp->c_flags |= C_PRESENTIN;
	}

	/* XXX for now, always assume we can do BIOS screen output */
	cp->c_flags |= C_PRESENTOUT;
	vbe_init();
	tem = NULL;
}

static int
vidc_init(struct console *cp, int arg)
{
	int i, rc;

	if (vidc_started && arg == 0)
		return (0);

	vidc_started = 1;
	gfx_framework_init(&fb_ops);

	/*
	 * Check Miscellaneous Output Register (Read at 3CCh, Write at 3C2h)
	 * for bit 1 (Input/Output Address Select), which means
	 * color/graphics adapter.
	 */
	if (vga_get_reg(VGA_REG_ADDR, VGA_MISC_R) & VGA_MISC_IOA_SEL)
		vgatext = (uint16_t *) PTOV(VGA_MEM_ADDR + VGA_COLOR_BASE);
	else
		vgatext = (uint16_t *) PTOV(VGA_MEM_ADDR + VGA_MONO_BASE);

	/* set 16bit colors */
	i = vga_get_atr(VGA_REG_ADDR, VGA_ATR_MODE);
	i &= ~VGA_ATR_MODE_BLINK;
	i &= ~VGA_ATR_MODE_9WIDE;
	vga_set_atr(VGA_REG_ADDR, VGA_ATR_MODE, i);

	plat_tem_hide_prom_cursor();

	memset(keybuf, 0, KEYBUFSZ);

	/* default to text mode */
	cp->c_private = &text_ops;

	if (vbe_available()) {
		rc = vbe_default_mode();
		/* if rc is not legal VBE mode, use text mode */
		if (VBE_VALID_MODE(rc)) {
			if (vbe_set_mode(rc) == 0)
				cp->c_private = &fb_ops;
			else
				bios_set_text_mode(3);
		}
	}

	rc = tem_info_init(cp);

	if (rc != 0) {
		bios_set_text_mode(3);
		cp->c_private = &text_ops;
		rc = tem_info_init(cp); /* try again */
	}
	if (rc == 0 && tem == NULL) {
		tem = tem_init();
		if (tem != NULL)
			tem_activate(tem, B_TRUE);
	}

	for (i = 0; i < 10 && vidc_ischar(cp); i++)
		(void)vidc_getchar(cp);

	return (0);	/* XXX reinit? */
}

static void
vidc_biosputchar(int c)
{
    v86.ctl = 0;
    v86.addr = 0x10;
    v86.eax = 0xe00 | (c & 0xff);
    v86.ebx = 0x7;
    v86int();
}

static void
vidc_putchar(struct console *cp, int c)
{
	uint8_t buf = c;

	/* make sure we have some console output, support for panic() */
	if (tem == NULL)
		vidc_biosputchar(c);
	else
		tem_write(tem, &buf, sizeof (buf));
}

static int
vidc_getchar(struct console *cp)
{
    int i, c;

    for (i = 0; i < KEYBUFSZ; i++) {
	if (keybuf[i] != 0) {
	    c = keybuf[i];
	    keybuf[i] = 0;
	    return (c);
	}
    }

    if (vidc_ischar(cp)) {
	v86.ctl = 0;
	v86.addr = 0x16;
	v86.eax = 0x0;
	v86int();
	if ((v86.eax & 0xff) != 0) {
		return (v86.eax & 0xff);
	}

	/* extended keys */
	switch (v86.eax & 0xff00) {
	case 0x4800:	/* up */
		keybuf[0] = '[';
		keybuf[1] = 'A';
		return (0x1b);	/* esc */
	case 0x4b00:	/* left */
		keybuf[0] = '[';
		keybuf[1] = 'D';
		return (0x1b);	/* esc */
	case 0x4d00:	/* right */
		keybuf[0] = '[';
		keybuf[1] = 'C';
		return (0x1b);	/* esc */
	case 0x5000:	/* down */
		keybuf[0] = '[';
		keybuf[1] = 'B';
		return (0x1b);	/* esc */
	default:
		return (-1);
	}
    } else {
	return (-1);
    }
}

static int
vidc_ischar(struct console *cp)
{
    int i;

    for (i = 0; i < KEYBUFSZ; i++) {
	if (keybuf[i] != 0) {
	    return (1);
	}
    }

    v86.ctl = V86_FLAGS;
    v86.addr = 0x16;
    v86.eax = 0x100;
    v86int();
    return (!V86_ZR(v86.efl));
}

#if KEYBOARD_PROBE

#define PROBE_MAXRETRY	5
#define PROBE_MAXWAIT	400
#define IO_DUMMY	0x84
#define IO_KBD		0x060		/* 8042 Keyboard */

/* selected defines from kbdio.h */
#define KBD_STATUS_PORT 	4	/* status port, read */
#define KBD_DATA_PORT		0	/* data port, read/write 
					 * also used as keyboard command
					 * and mouse command port 
					 */
#define KBDC_ECHO		0x00ee
#define KBDS_ANY_BUFFER_FULL	0x0001
#define KBDS_INPUT_BUFFER_FULL	0x0002
#define KBD_ECHO		0x00ee

/* 7 microsec delay necessary for some keyboard controllers */
static void
delay7(void)
{
    /* 
     * I know this is broken, but no timer is available yet at this stage...
     * See also comments in `delay1ms()'.
     */
    inb(IO_DUMMY); inb(IO_DUMMY);
    inb(IO_DUMMY); inb(IO_DUMMY);
    inb(IO_DUMMY); inb(IO_DUMMY);
}

/*
 * This routine uses an inb to an unused port, the time to execute that
 * inb is approximately 1.25uS.  This value is pretty constant across
 * all CPU's and all buses, with the exception of some PCI implentations
 * that do not forward this I/O address to the ISA bus as they know it
 * is not a valid ISA bus address, those machines execute this inb in
 * 60 nS :-(.
 *
 */
static void
delay1ms(void)
{
    int i = 800;
    while (--i >= 0)
	(void)inb(0x84);
}

/* 
 * We use the presence/absence of a keyboard to determine whether the internal
 * console can be used for input.
 *
 * Perform a simple test on the keyboard; issue the ECHO command and see
 * if the right answer is returned. We don't do anything as drastic as
 * full keyboard reset; it will be too troublesome and take too much time.
 */
static int
probe_keyboard(void)
{
    int retry = PROBE_MAXRETRY;
    int wait;
    int i;

    while (--retry >= 0) {
	/* flush any noise */
	while (inb(IO_KBD + KBD_STATUS_PORT) & KBDS_ANY_BUFFER_FULL) {
	    delay7();
	    inb(IO_KBD + KBD_DATA_PORT);
	    delay1ms();
	}

	/* wait until the controller can accept a command */
	for (wait = PROBE_MAXWAIT; wait > 0; --wait) {
	    if (((i = inb(IO_KBD + KBD_STATUS_PORT)) 
                & (KBDS_INPUT_BUFFER_FULL | KBDS_ANY_BUFFER_FULL)) == 0)
		break;
	    if (i & KBDS_ANY_BUFFER_FULL) {
		delay7();
	        inb(IO_KBD + KBD_DATA_PORT);
	    }
	    delay1ms();
	}
	if (wait <= 0)
	    continue;

	/* send the ECHO command */
	outb(IO_KBD + KBD_DATA_PORT, KBDC_ECHO);

	/* wait for a response */
	for (wait = PROBE_MAXWAIT; wait > 0; --wait) {
	     if (inb(IO_KBD + KBD_STATUS_PORT) & KBDS_ANY_BUFFER_FULL)
		 break;
	     delay1ms();
	}
	if (wait <= 0)
	    continue;

	delay7();
	i = inb(IO_KBD + KBD_DATA_PORT);
#ifdef PROBE_KBD_BEBUG
        printf("probe_keyboard: got 0x%x.\n", i);
#endif
	if (i == KBD_ECHO) {
	    /* got the right answer */
	    return (1);
	}
    }

    return (0);
}
#endif /* KEYBOARD_PROBE */
