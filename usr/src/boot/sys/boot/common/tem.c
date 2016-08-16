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
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 * Copyright 2016 Joyent, Inc.
 */

/*
 * ANSI terminal emulator module; parse ANSI X3.64 escape sequences and
 * the like.
 *
 * How Virtual Terminal Emulator Works:
 *
 * Every virtual terminal is associated with a tem_vt_state structure
 * and maintains a virtual screen buffer in tvs_screen_buf, which contains
 * all the characters which should be shown on the physical screen when
 * the terminal is activated.  There are also two other buffers, tvs_fg_buf
 * and tvs_bg_buf, which track the foreground and background colors of the
 * on screen characters
 *
 * Data written to a virtual terminal is composed of characters which
 * should be displayed on the screen when this virtual terminal is
 * activated, fg/bg colors of these characters, and other control
 * information (escape sequence, etc).
 *
 * When data is passed to a virtual terminal it first is parsed for
 * control information by tem_parse().  Subsequently the character
 * and color data are written to tvs_screen_buf, tvs_fg_buf, and
 * tvs_bg_buf.  They are saved in these buffers in order to refresh
 * the screen when this terminal is activated.  If the terminal is
 * currently active, the data (characters and colors) are also written
 * to the physical screen by invoking a callback function,
 * tem_text_callbacks() or tem_pix_callbacks().
 *
 * When rendering data to the framebuffer, if the framebuffer is in
 * VIS_PIXEL mode, the character data will first be converted to pixel
 * data using tem_pix_bit2pix(), and then the pixels get displayed
 * on the physical screen.  We only store the character and color data in
 * tem_vt_state since the bit2pix conversion only happens when actually
 * rendering to the physical framebuffer.
 */


#include <stand.h>
#include <sys/ascii.h>
#include <sys/errno.h>
#include <sys/tem.h>
#ifdef _HAVE_TEM_FIRMWARE
#include <sys/promif.h>
#endif /* _HAVE_TEM_FIRMWARE */
#include <sys/consplat.h>
#include <sys/kd.h>

/* Terminal emulator internal helper functions */
static void	tems_setup_terminal(struct vis_devinit *, size_t, size_t);
static void	tems_modechange_callback(struct vis_modechg_arg *,
		    struct vis_devinit *);

static void	tems_reset_colormap(void);

static void	tem_free_buf(struct tem_vt_state *);
static void	tem_internal_init(struct tem_vt_state *, boolean_t, boolean_t);
static void	tems_get_initial_color(tem_color_t *pcolor);

static void	tem_control(struct tem_vt_state *, uint8_t);
static void	tem_setparam(struct tem_vt_state *, int, int);
static void	tem_selgraph(struct tem_vt_state *);
static void	tem_chkparam(struct tem_vt_state *, uint8_t);
static void	tem_getparams(struct tem_vt_state *, uint8_t);
static void	tem_outch(struct tem_vt_state *, uint8_t);
static void	tem_parse(struct tem_vt_state *, uint8_t);

static void	tem_new_line(struct tem_vt_state *);
static void	tem_cr(struct tem_vt_state *);
static void	tem_lf(struct tem_vt_state *);
static void	tem_send_data(struct tem_vt_state *);
static void	tem_cls(struct tem_vt_state *);
static void	tem_tab(struct tem_vt_state *);
static void	tem_back_tab(struct tem_vt_state *);
static void	tem_clear_tabs(struct tem_vt_state *, int);
static void	tem_set_tab(struct tem_vt_state *);
static void	tem_mv_cursor(struct tem_vt_state *, int, int);
static void	tem_shift(struct tem_vt_state *, int, int);
static void	tem_scroll(struct tem_vt_state *, int, int, int, int);
static void	tem_clear_chars(struct tem_vt_state *tem,
			int count, screen_pos_t row, screen_pos_t col);
static void	tem_copy_area(struct tem_vt_state *tem,
			screen_pos_t s_col, screen_pos_t s_row,
			screen_pos_t e_col, screen_pos_t e_row,
			screen_pos_t t_col, screen_pos_t t_row);
#if 0
static void	tem_image_display(struct tem_vt_state *, uint8_t *,
			int, int, screen_pos_t, screen_pos_t);
#endif
static void	tem_bell(struct tem_vt_state *tem);
static void	tem_pix_clear_prom_output(struct tem_vt_state *tem);

static void	tem_virtual_cls(struct tem_vt_state *, int, screen_pos_t,
		    screen_pos_t);
static void	tem_virtual_display(struct tem_vt_state *,
		    unsigned char *, int, screen_pos_t, screen_pos_t,
		    text_color_t, text_color_t);
static void	tem_virtual_copy(struct tem_vt_state *, screen_pos_t,
		    screen_pos_t, screen_pos_t, screen_pos_t,
		    screen_pos_t, screen_pos_t);
static void	tem_align_cursor(struct tem_vt_state *tem);

static void	tem_check_first_time(struct tem_vt_state *tem);
static void	tem_reset_display(struct tem_vt_state *, boolean_t, boolean_t);
static void	tem_terminal_emulate(struct tem_vt_state *, uint8_t *, int);
static void	tem_text_cursor(struct tem_vt_state *, short);
static void	tem_text_cls(struct tem_vt_state *,
		    int count, screen_pos_t row, screen_pos_t col);
static void	tem_pix_display(struct tem_vt_state *, uint8_t *,
		    int, screen_pos_t, screen_pos_t,
		    text_color_t, text_color_t);
static void	tem_pix_copy(struct tem_vt_state *,
		    screen_pos_t, screen_pos_t,
		    screen_pos_t, screen_pos_t,
		    screen_pos_t, screen_pos_t);
static void	tem_pix_cursor(struct tem_vt_state *, short);
static void	tem_pix_clear_entire_screen(struct tem_vt_state *);
static void	tem_get_color(struct tem_vt_state *, text_color_t *,
		    text_color_t *, uint8_t);
static void	tem_blank_screen(struct tem_vt_state *);
static void	tem_unblank_screen(struct tem_vt_state *);
static void	tem_pix_align(struct tem_vt_state *);
static void	tem_text_display(struct tem_vt_state *, uint8_t *, int,
		    screen_pos_t, screen_pos_t, text_color_t, text_color_t);
static void	tem_text_copy(struct tem_vt_state *,
		    screen_pos_t, screen_pos_t, screen_pos_t, screen_pos_t,
		    screen_pos_t, screen_pos_t);
static void	tem_pix_bit2pix(struct tem_vt_state *, unsigned char,
		    unsigned char, unsigned char);
static void	tem_pix_cls_range(struct tem_vt_state *, screen_pos_t, int,
		    int, screen_pos_t, int, int, boolean_t);
static void	tem_pix_cls(struct tem_vt_state *, int,
		    screen_pos_t, screen_pos_t);

static void	bit_to_pix4(struct tem_vt_state *tem, uint8_t c,
		    text_color_t fg_color, text_color_t bg_color);
static void	bit_to_pix8(struct tem_vt_state *tem, uint8_t c,
		    text_color_t fg_color, text_color_t bg_color);
static void	bit_to_pix16(struct tem_vt_state *tem, uint8_t c,
		    text_color_t fg_color, text_color_t bg_color);
static void	bit_to_pix24(struct tem_vt_state *tem, uint8_t c,
		    text_color_t fg_color, text_color_t bg_color);
static void	bit_to_pix32(struct tem_vt_state *tem, uint8_t c,
		    text_color_t fg_color, text_color_t bg_color);

/*
 * Globals
 */
tem_state_t	tems;	/* common term info */

tem_callbacks_t tem_text_callbacks = {
	.tsc_display = &tem_text_display,
	.tsc_copy = &tem_text_copy,
	.tsc_cursor = &tem_text_cursor,
	.tsc_bit2pix = NULL,
	.tsc_cls = &tem_text_cls
};
tem_callbacks_t tem_pix_callbacks = {
	.tsc_display = &tem_pix_display,
	.tsc_copy = &tem_pix_copy,
	.tsc_cursor = &tem_pix_cursor,
	.tsc_bit2pix = &tem_pix_bit2pix,
	.tsc_cls = &tem_pix_cls
};

/* BEGIN CSTYLED */
/*                                  Bk  Rd  Gr  Br  Bl  Mg  Cy  Wh */
static text_color_t dim_xlate[] = {  1,  5,  3,  7,  2,  6,  4,  8 };
static text_color_t brt_xlate[] = {  9, 13, 11, 15, 10, 14, 12,  0 };
/* END CSTYLED */

text_cmap_t cmap4_to_24 = {
/* BEGIN CSTYLED */
/*	       0    1    2    3    4    5    6    7    8    9   10   11   12   13   14   15
	      Wh+  Bk   Bl   Gr   Cy   Rd   Mg   Br   Wh   Bk+  Bl+  Gr+  Cy+  Rd+  Mg+  Yw */
  .red   = {0xff,0x00,0x00,0x00,0x00,0x80,0x80,0x80,0x80,0x40,0x00,0x00,0x00,0xff,0xff,0xff},
  .green = {0xff,0x00,0x00,0x80,0x80,0x00,0x00,0x80,0x80,0x40,0x00,0xff,0xff,0x00,0x00,0xff},
  .blue  = {0xff,0x00,0x80,0x00,0x80,0x00,0x80,0x00,0x80,0x40,0xff,0x00,0xff,0x00,0xff,0x00}
/* END CSTYLED */
};

#define	tem_callback_display	(*tems.ts_callbacks->tsc_display)
#define	tem_callback_copy	(*tems.ts_callbacks->tsc_copy)
#define	tem_callback_cursor	(*tems.ts_callbacks->tsc_cursor)
#define	tem_callback_cls	(*tems.ts_callbacks->tsc_cls)
#define	tem_callback_bit2pix	(*tems.ts_callbacks->tsc_bit2pix)

static void
tem_add(struct tem_vt_state *tem)
{
	list_insert_head(&tems.ts_list, tem);
}

static void
tem_rm(struct tem_vt_state *tem)
{
	list_remove(&tems.ts_list, tem);
}

/*
 * This is the main entry point to the module.  It handles output requests
 * during normal system operation, when (e.g.) mutexes are available.
 */
void
tem_write(tem_vt_state_t tem_arg, uint8_t *buf, ssize_t len)
{
	struct tem_vt_state *tem = (struct tem_vt_state *)tem_arg;

	if (!tem->tvs_initialized) {
		return;
	}

	tem_check_first_time(tem);
	tem_terminal_emulate(tem, buf, len);
}

static void
tem_internal_init(struct tem_vt_state *ptem,
    boolean_t init_color, boolean_t clear_screen)
{
	int i, j;
	int width, height;
	int total;
	text_color_t fg;
	text_color_t bg;
	size_t	tc_size = sizeof (text_color_t);

	if (tems.ts_display_mode == VIS_PIXEL) {
		ptem->tvs_pix_data_size = tems.ts_pix_data_size;
		ptem->tvs_pix_data = malloc(ptem->tvs_pix_data_size);
	}

	ptem->tvs_outbuf_size = tems.ts_c_dimension.width;
	ptem->tvs_outbuf = malloc(ptem->tvs_outbuf_size);

	width = tems.ts_c_dimension.width;
	height = tems.ts_c_dimension.height;
	ptem->tvs_screen_buf_size = width * height;
	ptem->tvs_screen_buf = malloc(width * height);

	total = width * height * tc_size;
	ptem->tvs_fg_buf = malloc(total);
	ptem->tvs_bg_buf = malloc(total);
	ptem->tvs_color_buf_size = total;

	tem_reset_display(ptem, clear_screen, init_color);

	tem_get_color(ptem, &fg, &bg, TEM_ATTR_SCREEN_REVERSE);
	for (i = 0; i < height; i++)
		for (j = 0; j < width; j++) {
			ptem->tvs_screen_buf[i * width + j] = ' ';
			ptem->tvs_fg_buf[(i * width +j) * tc_size] = fg;
			ptem->tvs_bg_buf[(i * width +j) * tc_size] = bg;

		}

	ptem->tvs_initialized  = 1;
}

int
tem_initialized(tem_vt_state_t tem_arg)
{
	struct tem_vt_state *ptem = (struct tem_vt_state *)tem_arg;

	return (ptem->tvs_initialized);
}

tem_vt_state_t
tem_init(void)
{
	struct tem_vt_state *ptem;

	ptem = malloc(sizeof (struct tem_vt_state));
	if (ptem == NULL)
		return ((tem_vt_state_t)ptem);
	bzero(ptem, sizeof (*ptem));

	ptem->tvs_isactive = B_FALSE;
	ptem->tvs_fbmode = KD_TEXT;

	/*
	 * A tem is regarded as initialized only after tem_internal_init(),
	 * will be set at the end of tem_internal_init().
	 */
	ptem->tvs_initialized = 0;

	if (!tems.ts_initialized) {
		/*
		 * Only happens during early console configuration.
		 */
		tem_add(ptem);
		return ((tem_vt_state_t)ptem);
	}

	tem_internal_init(ptem, B_TRUE, B_FALSE);
	tem_add(ptem);

	return ((tem_vt_state_t)ptem);
}

/*
 * re-init the tem after video mode has changed and tems_info has
 * been re-inited.
 */
static void
tem_reinit(struct tem_vt_state *tem, boolean_t reset_display)
{
	tem_free_buf(tem); /* only free virtual buffers */

	/* reserve color */
	tem_internal_init(tem, B_FALSE, reset_display);
}

static void
tem_free_buf(struct tem_vt_state *tem)
{
	if (tem->tvs_outbuf != NULL) {
		free(tem->tvs_outbuf);
		tem->tvs_outbuf = NULL;
	}
	if (tem->tvs_pix_data != NULL) {
		free(tem->tvs_pix_data);
		tem->tvs_pix_data = NULL;
	}
	if (tem->tvs_screen_buf != NULL) {
		free(tem->tvs_screen_buf);
		tem->tvs_screen_buf = NULL;
	}
	if (tem->tvs_fg_buf != NULL) {
		free(tem->tvs_fg_buf);
		tem->tvs_fg_buf = NULL;
	}
	if (tem->tvs_bg_buf != NULL) {
		free(tem->tvs_bg_buf);
		tem->tvs_bg_buf = NULL;
	}
}

void
tem_destroy(tem_vt_state_t tem_arg)
{
	struct tem_vt_state *tem = (struct tem_vt_state *)tem_arg;

	if (tem->tvs_isactive && tem->tvs_fbmode == KD_TEXT)
		tem_blank_screen(tem);

	tem_free_buf(tem);
	tem_rm(tem);

	if (tems.ts_active == tem)
		tems.ts_active = NULL;

	free(tem);
}

static int
tems_failed(boolean_t finish_ioctl)
{
	if (finish_ioctl && tems.ts_hdl != NULL)
		(void) tems.ts_hdl->c_ioctl(tems.ts_hdl, VIS_DEVFINI, NULL);

	tems.ts_hdl = NULL;
	return (ENXIO);
}

/*
 * Only called once during boot
 */
int
tem_info_init(struct console *cp)
{
	int			ret;
	struct vis_devinit	temargs;
	size_t height = 0;
	size_t width = 0;
	struct tem_vt_state *p;

	if (tems.ts_initialized) {
		return (0);
	}

	list_create(&tems.ts_list, sizeof (struct tem_vt_state),
	    __offsetof(struct tem_vt_state, tvs_list_node));
	tems.ts_active = NULL;

	tems.ts_hdl = cp;
	bzero(&temargs, sizeof (temargs));
	temargs.modechg_cb  = (vis_modechg_cb_t)tems_modechange_callback;
	temargs.modechg_arg = NULL;

	/*
	 * Initialize the console and get the device parameters
	 */
	if (cp->c_ioctl(cp, VIS_DEVINIT, &temargs) != 0) {
		printf("terminal emulator: Compatible fb not found\n");
		ret = tems_failed(B_FALSE);
		return (ret);
	}

	/* Make sure the fb driver and terminal emulator versions match */
	if (temargs.version != VIS_CONS_REV) {
		printf(
		    "terminal emulator: VIS_CONS_REV %d (see sys/visual_io.h) "
		    "of console fb driver not supported\n", temargs.version);
		ret = tems_failed(B_TRUE);
		return (ret);
	}

	/* other sanity checks */
	if (!((temargs.depth == 4) || (temargs.depth == 8) ||
	    (temargs.depth == 15) || (temargs.depth == 16) ||
	    (temargs.depth == 24) || (temargs.depth == 32))) {
		printf("terminal emulator: unsupported depth\n");
		ret = tems_failed(B_TRUE);
		return (ret);
	}

	if ((temargs.mode != VIS_TEXT) && (temargs.mode != VIS_PIXEL)) {
		printf("terminal emulator: unsupported mode\n");
		ret = tems_failed(B_TRUE);
		return (ret);
	}

	plat_tem_get_prom_size(&height, &width);

	/*
	 * Initialize the common terminal emulator info
	 */
	tems_setup_terminal(&temargs, height, width);

	tems_reset_colormap();
	tems_get_initial_color(&tems.ts_init_color);

	tems.ts_initialized = 1; /* initialization flag */

	for (p = list_head(&tems.ts_list); p != NULL;
	    p = list_next(&tems.ts_list, p)) {
		tem_internal_init(p, B_TRUE, B_FALSE);
		if (temargs.mode == VIS_PIXEL)
			tem_pix_align(p);
	}

	return (0);
}

#define	TEMS_DEPTH_DIFF		0x01
#define	TEMS_DIMENSION_DIFF	0x02

static uint8_t
tems_check_videomode(struct vis_devinit *tp)
{
	uint8_t result = 0;

	if (tems.ts_pdepth != tp->depth)
		result |= TEMS_DEPTH_DIFF;

	if (tp->mode == VIS_TEXT) {
		if (tems.ts_c_dimension.width != tp->width ||
		    tems.ts_c_dimension.height != tp->height)
			result |= TEMS_DIMENSION_DIFF;
	} else {
		if (tems.ts_p_dimension.width != tp->width ||
		    tems.ts_p_dimension.height != tp->height)
			result |= TEMS_DIMENSION_DIFF;
	}

	return (result);
}

static void
tems_setup_terminal(struct vis_devinit *tp, size_t height, size_t width)
{
	int i;
	char env[8];

	tems.ts_pdepth = tp->depth;
	tems.ts_linebytes = tp->linebytes;
	tems.ts_display_mode = tp->mode;
	tems.ts_color_map = tp->color_map;

	switch (tp->mode) {
	case VIS_TEXT:
		tems.ts_p_dimension.width = 0;
		tems.ts_p_dimension.height = 0;
		tems.ts_c_dimension.width = tp->width;
		tems.ts_c_dimension.height = tp->height;
		tems.ts_callbacks = &tem_text_callbacks;

		sprintf(env, "%u", (unsigned)tems.ts_c_dimension.height);
		setenv("screen-#rows", env, 1);
		sprintf(env, "%u", (unsigned)tems.ts_c_dimension.width);
		setenv("screen-#cols", env, 1);

		/* ensure the following are not set for text mode */
		unsetenv("screen-height");
		unsetenv("screen-width");
		break;

	case VIS_PIXEL:
		/*
		 * First check to see if the user has specified a screen size.
		 * If so, use those values.  Else use 34x80 as the default.
		 */
		if (width == 0) {
			width = TEM_DEFAULT_COLS;
			height = TEM_DEFAULT_ROWS;
		}
		tems.ts_c_dimension.height = (screen_size_t)height;
		tems.ts_c_dimension.width = (screen_size_t)width;

		tems.ts_p_dimension.height = tp->height;
		tems.ts_p_dimension.width = tp->width;

		tems.ts_callbacks = &tem_pix_callbacks;

		/*
		 * set_font() will select a appropriate sized font for
		 * the number of rows and columns selected.  If we don't
		 * have a font that will fit, then it will use the
		 * default builtin font and adjust the rows and columns
		 * to fit on the screen.
		 */
		set_font(&tems.ts_font,
		    &tems.ts_c_dimension.height,
		    &tems.ts_c_dimension.width,
		    tems.ts_p_dimension.height,
		    tems.ts_p_dimension.width);

		sprintf(env, "%u", (unsigned)tems.ts_c_dimension.height);
		setenv("screen-#rows", env, 1);
		sprintf(env, "%u", (unsigned)tems.ts_c_dimension.width);
		setenv("screen-#cols", env, 1);

		sprintf(env, "%u", (unsigned)tems.ts_p_dimension.height);
		setenv("screen-height", env, 1);
		sprintf(env, "%u", (unsigned)tems.ts_p_dimension.width);
		setenv("screen-width", env, 1);

		tems.ts_p_offset.y = (tems.ts_p_dimension.height -
		    (tems.ts_c_dimension.height * tems.ts_font.height)) / 2;
		tems.ts_p_offset.x = (tems.ts_p_dimension.width -
		    (tems.ts_c_dimension.width * tems.ts_font.width)) / 2;

		tems.ts_pix_data_size =
		    tems.ts_font.width * tems.ts_font.height;

		tems.ts_pix_data_size *= 4;

		tems.ts_pdepth = tp->depth;

		break;
	}

	/* Now virtual cls also uses the blank_line buffer */
	free(tems.ts_blank_line);

	tems.ts_blank_line = malloc(tems.ts_c_dimension.width);
	for (i = 0; i < tems.ts_c_dimension.width; i++)
		tems.ts_blank_line[i] = ' ';
}

/*
 * This is a callback function that we register with the frame
 * buffer driver layered underneath.  It gets invoked from
 * the underlying frame buffer driver to reconfigure the terminal
 * emulator to a new screen size and depth in conjunction with
 * framebuffer videomode changes.
 * Here we keep the foreground/background color and attributes,
 * which may be different with the initial settings, so that
 * the color won't change while the framebuffer videomode changes.
 * And we also reset the kernel terminal emulator and clear the
 * whole screen.
 */
/* ARGSUSED */
void
tems_modechange_callback(struct vis_modechg_arg *arg,
    struct vis_devinit *devinit)
{
	uint8_t diff;
	struct tem_vt_state *p;
	tem_modechg_cb_t cb;
	tem_modechg_cb_arg_t cb_arg;
	size_t height = 0;
	size_t width = 0;

	diff = tems_check_videomode(devinit);
	if (diff == 0) {
		/*
		 * This is color related change, reset color and redraw the
		 * screen. Only need to reinit the active tem.
		 */
		struct tem_vt_state *active = tems.ts_active;
		tems_get_initial_color(&tems.ts_init_color);
		active->tvs_fg_color = tems.ts_init_color.fg_color;
		active->tvs_bg_color = tems.ts_init_color.bg_color;
		active->tvs_flags = tems.ts_init_color.a_flags;
		tem_reinit(active, B_TRUE);
		return;
	}

	diff = diff & TEMS_DIMENSION_DIFF;

	if (diff == 0) {
		/*
		 * Only need to reinit the active tem.
		 */
		struct tem_vt_state *active = tems.ts_active;
		tems.ts_pdepth = devinit->depth;
		/* color depth did change, reset colors */
		tems_reset_colormap();
		tems_get_initial_color(&tems.ts_init_color);
		tem_reinit(active, B_TRUE);

		return;
	}

	plat_tem_get_prom_size(&height, &width);

	tems_setup_terminal(devinit, height, width);

	tems_reset_colormap();
	tems_get_initial_color(&tems.ts_init_color);

	for (p = list_head(&tems.ts_list); p != NULL;
	    p = list_next(&tems.ts_list, p)) {
		tem_reinit(p, p->tvs_isactive);
	}


	if (tems.ts_modechg_cb == NULL) {
		return;
	}

	cb = tems.ts_modechg_cb;
	cb_arg = tems.ts_modechg_arg;

	cb(cb_arg);
}

/*
 * This function is used to clear entire screen via the underlying framebuffer
 * driver.
 */
int
tems_cls(struct vis_consclear *pda)
{
	if (tems.ts_hdl == NULL)
		return (1);
	return (tems.ts_hdl->c_ioctl(tems.ts_hdl, VIS_CONSCLEAR, pda));
}

/*
 * This function is used to display a rectangular blit of data
 * of a given size and location via the underlying framebuffer driver.
 * The blit can be as small as a pixel or as large as the screen.
 */
void
tems_display(struct vis_consdisplay *pda)
{
	if (tems.ts_hdl != NULL)
		(void) tems.ts_hdl->c_ioctl(tems.ts_hdl, VIS_CONSDISPLAY, pda);
}

/*
 * This function is used to invoke a block copy operation in the
 * underlying framebuffer driver.  Rectangle copies are how scrolling
 * is implemented, as well as horizontal text shifting escape seqs.
 * such as from vi when deleting characters and words.
 */
void
tems_copy(struct vis_conscopy *pma)
{
	if (tems.ts_hdl != NULL)
		(void) tems.ts_hdl->c_ioctl(tems.ts_hdl, VIS_CONSCOPY, pma);
}

/*
 * This function is used to show or hide a rectangluar monochrom
 * pixel inverting, text block cursor via the underlying framebuffer.
 */
void
tems_cursor(struct vis_conscursor *pca)
{
	if (tems.ts_hdl != NULL)
		(void) tems.ts_hdl->c_ioctl(tems.ts_hdl, VIS_CONSCURSOR, pca);
}

static void
tem_kdsetmode(int mode)
{
	if (tems.ts_hdl != NULL)
		(void) tems.ts_hdl->c_ioctl(tems.ts_hdl, KDSETMODE,
	     (void *)(intptr_t)mode);
}

static void
tems_reset_colormap(void)
{
	struct vis_cmap cm;

	switch (tems.ts_pdepth) {
	case 8:
		cm.index = 0;
		cm.count = 16;
		cm.red   = cmap4_to_24.red;   /* 8-bits (1/3 of TrueColor 24) */
		cm.blue  = cmap4_to_24.blue;  /* 8-bits (1/3 of TrueColor 24) */
		cm.green = cmap4_to_24.green; /* 8-bits (1/3 of TrueColor 24) */
		if (tems.ts_hdl != NULL)
			(void) tems.ts_hdl->c_ioctl(tems.ts_hdl,
			    VIS_PUTCMAP, &cm);
		break;
	}
}

void
tem_get_size(uint16_t *r, uint16_t *c, uint16_t *x, uint16_t *y)
{
	*r = (uint16_t)tems.ts_c_dimension.height;
	*c = (uint16_t)tems.ts_c_dimension.width;
	*x = (uint16_t)tems.ts_p_dimension.width;
	*y = (uint16_t)tems.ts_p_dimension.height;
}

void
tem_register_modechg_cb(tem_modechg_cb_t func, tem_modechg_cb_arg_t arg)
{
	tems.ts_modechg_cb = func;
	tems.ts_modechg_arg = arg;
}

/*
 * This function is to scroll up the OBP output, which has
 * different screen height and width with our kernel console.
 */
static void
tem_prom_scroll_up(struct tem_vt_state *tem, int nrows)
{
	struct vis_conscopy	ma;
	int	ncols, width;

	/* copy */
	ma.s_row = nrows * tems.ts_font.height;
	ma.e_row = tems.ts_p_dimension.height - 1;
	ma.t_row = 0;

	ma.s_col = 0;
	ma.e_col = tems.ts_p_dimension.width - 1;
	ma.t_col = 0;

	tems_copy(&ma);

	/* clear */
	width = tems.ts_font.width;
	ncols = (tems.ts_p_dimension.width + (width - 1))/ width;

	tem_pix_cls_range(tem, 0, nrows, tems.ts_p_offset.y,
	    0, ncols, 0, B_TRUE);
}

/*
 * This function is to compute the starting row of the console, according to
 * PROM cursor's position. Here we have to take different fonts into account.
 */
static int
tem_adjust_row(struct tem_vt_state *tem, int prom_row)
{
	int	tem_row;
	int	tem_y;
	int	prom_charheight = 0;
	int	prom_window_top = 0;
	int	scroll_up_lines;

	plat_tem_get_prom_font_size(&prom_charheight, &prom_window_top);
	if (prom_charheight == 0)
		prom_charheight = tems.ts_font.height;

	tem_y = (prom_row + 1) * prom_charheight + prom_window_top -
	    tems.ts_p_offset.y;
	tem_row = (tem_y + tems.ts_font.height - 1) /
	    tems.ts_font.height - 1;

	if (tem_row < 0) {
		tem_row = 0;
	} else if (tem_row >= (tems.ts_c_dimension.height - 1)) {
		/*
		 * Scroll up the prom outputs if the PROM cursor's position is
		 * below our tem's lower boundary.
		 */
		scroll_up_lines = tem_row -
		    (tems.ts_c_dimension.height - 1);
		tem_prom_scroll_up(tem, scroll_up_lines);
		tem_row = tems.ts_c_dimension.height - 1;
	}

	return (tem_row);
}

static void
tem_pix_align(struct tem_vt_state *tem)
{
	uint32_t row = 0;
	uint32_t col = 0;

	if (plat_stdout_is_framebuffer()) {
		plat_tem_hide_prom_cursor();

		/*
		 * We are getting the current cursor position in pixel
		 * mode so that we don't over-write the console output
		 * during boot.
		 */
		plat_tem_get_prom_pos(&row, &col);

		/*
		 * Adjust the row if necessary when the font of our
		 * kernel console tem is different with that of prom
		 * tem.
		 */
		row = tem_adjust_row(tem, row);

		/* first line of our kernel console output */
		tem->tvs_first_line = row + 1;

		/* re-set and align cursor position */
		tem->tvs_s_cursor.row = tem->tvs_c_cursor.row =
		    (screen_pos_t)row;
		tem->tvs_s_cursor.col = tem->tvs_c_cursor.col = 0;
	} else {
		tem_reset_display(tem, B_TRUE, B_TRUE);
	}
}

static void
tems_get_inverses(boolean_t *p_inverse, boolean_t *p_inverse_screen)
{
	int i_inverse = 0;
	int i_inverse_screen = 0;

	plat_tem_get_inverses(&i_inverse, &i_inverse_screen);

	*p_inverse = (i_inverse == 0) ? B_FALSE : B_TRUE;
	*p_inverse_screen = (i_inverse_screen == 0) ? B_FALSE : B_TRUE;
}

/*
 * Get the foreground/background color and attributes from environment.
 */
static void
tems_get_initial_color(tem_color_t *pcolor)
{
	boolean_t inverse, inverse_screen;
	unsigned short  flags = 0;

	pcolor->fg_color = DEFAULT_ANSI_FOREGROUND;
	pcolor->bg_color = DEFAULT_ANSI_BACKGROUND;
	plat_tem_get_colors(&pcolor->fg_color, &pcolor->bg_color);

	tems_get_inverses(&inverse, &inverse_screen);
	if (inverse)
		flags |= TEM_ATTR_REVERSE;
	if (inverse_screen)
		flags |= TEM_ATTR_SCREEN_REVERSE;

	/*
	 * In case of black on white we want bright white for BG.
	 * In case if white on black, to improve readability,
	 * we want bold white.
	 */
	if (flags != 0) {
		/*
		 * If either reverse flag is set, the screen is in
		 * white-on-black mode.  We set the bold flag to
		 * improve readability.
		 */
		flags |= TEM_ATTR_BOLD;
	} else {
		/*
		 * Otherwise, the screen is in black-on-white mode.
		 * The SPARC PROM console, which starts in this mode,
		 * uses the bright white background colour so we
		 * match it here.
		 */
		if (pcolor->bg_color == ANSI_COLOR_WHITE)
			flags |= TEM_ATTR_BRIGHT_BG;
	}

	pcolor->a_flags = flags;
}

uint8_t
tem_get_fbmode(tem_vt_state_t tem_arg)
{
	struct tem_vt_state *tem = (struct tem_vt_state *)tem_arg;

	return (tem->tvs_fbmode);
}

void
tem_set_fbmode(tem_vt_state_t tem_arg, uint8_t fbmode)
{
	struct tem_vt_state *tem = (struct tem_vt_state *)tem_arg;

	if (fbmode == tem->tvs_fbmode) {
		return;
	}

	tem->tvs_fbmode = fbmode;

	if (tem->tvs_isactive) {
		tem_kdsetmode(tem->tvs_fbmode);
		if (fbmode == KD_TEXT)
			tem_unblank_screen(tem);
	}
}

void
tem_activate(tem_vt_state_t tem_arg, boolean_t unblank)
{
	struct tem_vt_state *tem = (struct tem_vt_state *)tem_arg;

	tems.ts_active = tem;
	tem->tvs_isactive = B_TRUE;

	tem_kdsetmode(tem->tvs_fbmode);

	if (unblank)
		tem_unblank_screen(tem);
}

void
tem_switch(tem_vt_state_t tem_arg1, tem_vt_state_t tem_arg2)
{
	struct tem_vt_state *cur = (struct tem_vt_state *)tem_arg1;
	struct tem_vt_state *tobe = (struct tem_vt_state *)tem_arg2;

	tems.ts_active = tobe;
	cur->tvs_isactive = B_FALSE;
	tobe->tvs_isactive = B_TRUE;

	if (cur->tvs_fbmode != tobe->tvs_fbmode)
		tem_kdsetmode(tobe->tvs_fbmode);

	if (tobe->tvs_fbmode == KD_TEXT)
		tem_unblank_screen(tobe);
}

static void
tem_check_first_time(struct tem_vt_state *tem)
{
	static int first_time = 1;

	/*
	 * Realign the console cursor. We did this in tem_init().
	 * However, drivers in the console stream may emit additional
	 * messages before we are ready. This causes text overwrite
	 * on the screen. This is a workaround.
	 */
	if (!first_time)
		return;

	first_time = 0;
	if (tems.ts_display_mode == VIS_TEXT)
		tem_text_cursor(tem, VIS_GET_CURSOR);
	else
		tem_pix_cursor(tem, VIS_GET_CURSOR);
	tem_align_cursor(tem);
}

/*
 * This is the main entry point into the terminal emulator.
 *
 * For each data message coming downstream, ANSI assumes that it is composed
 * of ASCII characters, which are treated as a byte-stream input to the
 * parsing state machine. All data is parsed immediately -- there is
 * no enqueing.
 */
static void
tem_terminal_emulate(struct tem_vt_state *tem, uint8_t *buf, int len)
{
	if (tem->tvs_isactive)
		tem_callback_cursor(tem, VIS_HIDE_CURSOR);

	for (; len > 0; len--, buf++)
		tem_parse(tem, *buf);

	/*
	 * Send the data we just got to the framebuffer.
	 */
	tem_send_data(tem);

	if (tem->tvs_isactive)
		tem_callback_cursor(tem, VIS_DISPLAY_CURSOR);
}

/*
 * send the appropriate control message or set state based on the
 * value of the control character ch
 */

static void
tem_control(struct tem_vt_state *tem, uint8_t ch)
{
	tem->tvs_state = A_STATE_START;
	switch (ch) {
	case A_BEL:
		tem_bell(tem);
		break;

	case A_BS:
		tem_mv_cursor(tem,
		    tem->tvs_c_cursor.row,
		    tem->tvs_c_cursor.col - 1);
		break;

	case A_HT:
		tem_tab(tem);
		break;

	case A_NL:
		/*
		 * tem_send_data(tem, credp, called_from);
		 * tem_new_line(tem, credp, called_from);
		 * break;
		 */

	case A_VT:
		tem_send_data(tem);
		tem_lf(tem);
		break;

	case A_FF:
		tem_send_data(tem);
		tem_cls(tem);
		break;

	case A_CR:
		tem_send_data(tem);
		tem_cr(tem);
		break;

	case A_ESC:
		tem->tvs_state = A_STATE_ESC;
		break;

	case A_CSI:
		{
			int i;
			tem->tvs_curparam = 0;
			tem->tvs_paramval = 0;
			tem->tvs_gotparam = B_FALSE;
			/* clear the parameters */
			for (i = 0; i < TEM_MAXPARAMS; i++)
				tem->tvs_params[i] = -1;
			tem->tvs_state = A_STATE_CSI;
		}
		break;

	case A_GS:
		tem_back_tab(tem);
		break;

	default:
		break;
	}
}


/*
 * if parameters [0..count - 1] are not set, set them to the value
 * of newparam.
 */

static void
tem_setparam(struct tem_vt_state *tem, int count, int newparam)
{
	int i;

	for (i = 0; i < count; i++) {
		if (tem->tvs_params[i] == -1)
			tem->tvs_params[i] = newparam;
	}
}


/*
 * select graphics mode based on the param vals stored in a_params
 */
static void
tem_selgraph(struct tem_vt_state *tem)
{
	int curparam;
	int count = 0;
	int param;

	tem->tvs_state = A_STATE_START;

	curparam = tem->tvs_curparam;
	do {
		param = tem->tvs_params[count];

		switch (param) {
		case -1:
		case 0:
			/* reset to initial normal settings */
			tem->tvs_fg_color = tems.ts_init_color.fg_color;
			tem->tvs_bg_color = tems.ts_init_color.bg_color;
			tem->tvs_flags = tems.ts_init_color.a_flags;
			break;

		case 1: /* Bold Intense */
			tem->tvs_flags |= TEM_ATTR_BOLD;
			break;

		case 2: /* Faint Intense */
			tem->tvs_flags &= ~TEM_ATTR_BOLD;
			break;

		case 5: /* Blink */
			tem->tvs_flags |= TEM_ATTR_BLINK;
			break;

		case 7: /* Reverse video */
			if (tem->tvs_flags & TEM_ATTR_SCREEN_REVERSE) {
				tem->tvs_flags &= ~TEM_ATTR_REVERSE;
			} else {
				tem->tvs_flags |= TEM_ATTR_REVERSE;
			}
			break;

		case 30: /* black	(grey) 		foreground */
		case 31: /* red		(light red) 	foreground */
		case 32: /* green	(light green) 	foreground */
		case 33: /* brown	(yellow) 	foreground */
		case 34: /* blue	(light blue) 	foreground */
		case 35: /* magenta	(light magenta) foreground */
		case 36: /* cyan	(light cyan) 	foreground */
		case 37: /* white	(bright white) 	foreground */
			tem->tvs_fg_color = param - 30;
			tem->tvs_flags &= ~TEM_ATTR_BRIGHT_FG;
			break;

		case 39:
			/*
			 * Reset the foreground colour and brightness.
			 */
			tem->tvs_fg_color = tems.ts_init_color.fg_color;
			if (tems.ts_init_color.a_flags & TEM_ATTR_BRIGHT_FG)
				tem->tvs_flags |= TEM_ATTR_BRIGHT_FG;
			else
				tem->tvs_flags &= ~TEM_ATTR_BRIGHT_FG;
			break;

		case 40: /* black	(grey) 		background */
		case 41: /* red		(light red) 	background */
		case 42: /* green	(light green) 	background */
		case 43: /* brown	(yellow) 	background */
		case 44: /* blue	(light blue) 	background */
		case 45: /* magenta	(light magenta) background */
		case 46: /* cyan	(light cyan) 	background */
		case 47: /* white	(bright white) 	background */
			tem->tvs_bg_color = param - 40;
			tem->tvs_flags &= ~TEM_ATTR_BRIGHT_BG;
			break;

		case 49:
			/*
			 * Reset the background colour and brightness.
			 */
			tem->tvs_bg_color = tems.ts_init_color.bg_color;
			if (tems.ts_init_color.a_flags & TEM_ATTR_BRIGHT_BG)
				tem->tvs_flags |= TEM_ATTR_BRIGHT_BG;
			else
				tem->tvs_flags &= ~TEM_ATTR_BRIGHT_BG;
			break;

		case 90: /* black	(grey) 		foreground */
		case 91: /* red		(light red) 	foreground */
		case 92: /* green	(light green) 	foreground */
		case 93: /* brown	(yellow) 	foreground */
		case 94: /* blue	(light blue) 	foreground */
		case 95: /* magenta	(light magenta) foreground */
		case 96: /* cyan	(light cyan) 	foreground */
		case 97: /* white	(bright white) 	foreground */
			tem->tvs_fg_color = param - 90;
			tem->tvs_flags |= TEM_ATTR_BRIGHT_FG;
			break;

		case 100: /* black	(grey) 		background */
		case 101: /* red	(light red) 	background */
		case 102: /* green	(light green) 	background */
		case 103: /* brown	(yellow) 	background */
		case 104: /* blue	(light blue) 	background */
		case 105: /* magenta	(light magenta) background */
		case 106: /* cyan	(light cyan) 	background */
		case 107: /* white	(bright white) 	background */
			tem->tvs_bg_color = param - 100;
			tem->tvs_flags |= TEM_ATTR_BRIGHT_BG;
			break;

		default:
			break;
		}
		count++;
		curparam--;

	} while (curparam > 0);
}

/*
 * perform the appropriate action for the escape sequence
 *
 * General rule:  This code does not validate the arguments passed.
 *                It assumes that the next lower level will do so.
 */
static void
tem_chkparam(struct tem_vt_state *tem, uint8_t ch)
{
	int	i;
	int	row;
	int	col;

	row = tem->tvs_c_cursor.row;
	col = tem->tvs_c_cursor.col;

	switch (ch) {

	case 'm': /* select terminal graphics mode */
		tem_send_data(tem);
		tem_selgraph(tem);
		break;

	case '@':		/* insert char */
		tem_setparam(tem, 1, 1);
		tem_shift(tem, tem->tvs_params[0], TEM_SHIFT_RIGHT);
		break;

	case 'A':		/* cursor up */
		tem_setparam(tem, 1, 1);
		tem_mv_cursor(tem, row - tem->tvs_params[0], col);
		break;

	case 'd':		/* VPA - vertical position absolute */
		tem_setparam(tem, 1, 1);
		tem_mv_cursor(tem, tem->tvs_params[0] - 1, col);
		break;

	case 'e':		/* VPR - vertical position relative */
	case 'B':		/* cursor down */
		tem_setparam(tem, 1, 1);
		tem_mv_cursor(tem, row + tem->tvs_params[0], col);
		break;

	case 'a':		/* HPR - horizontal position relative */
	case 'C':		/* cursor right */
		tem_setparam(tem, 1, 1);
		tem_mv_cursor(tem, row, col + tem->tvs_params[0]);
		break;

	case '`':		/* HPA - horizontal position absolute */
		tem_setparam(tem, 1, 1);
		tem_mv_cursor(tem, row, tem->tvs_params[0] - 1);
		break;

	case 'D':		/* cursor left */
		tem_setparam(tem, 1, 1);
		tem_mv_cursor(tem, row, col - tem->tvs_params[0]);
		break;

	case 'E':		/* CNL cursor next line */
		tem_setparam(tem, 1, 1);
		tem_mv_cursor(tem, row + tem->tvs_params[0], 0);
		break;

	case 'F':		/* CPL cursor previous line */
		tem_setparam(tem, 1, 1);
		tem_mv_cursor(tem, row - tem->tvs_params[0], 0);
		break;

	case 'G':		/* cursor horizontal position */
		tem_setparam(tem, 1, 1);
		tem_mv_cursor(tem, row, tem->tvs_params[0] - 1);
		break;

	case 'g':		/* clear tabs */
		tem_setparam(tem, 1, 0);
		tem_clear_tabs(tem, tem->tvs_params[0]);
		break;

	case 'f':		/* HVP Horizontal and Vertical Position */
	case 'H':		/* CUP position cursor */
		tem_setparam(tem, 2, 1);
		tem_mv_cursor(tem,
		    tem->tvs_params[0] - 1, tem->tvs_params[1] - 1);
		break;

	case 'I':		/* CHT - Cursor Horizontal Tab */
		/* Not implemented */
		break;

	case 'J':		/* ED - Erase in Display */
		tem_send_data(tem);
		tem_setparam(tem, 1, 0);
		switch (tem->tvs_params[0]) {
		case 0:
			/* erase cursor to end of screen */
			/* FIRST erase cursor to end of line */
			tem_clear_chars(tem,
			    tems.ts_c_dimension.width -
			    tem->tvs_c_cursor.col,
			    tem->tvs_c_cursor.row,
			    tem->tvs_c_cursor.col);

			/* THEN erase lines below the cursor */
			for (row = tem->tvs_c_cursor.row + 1;
			    row < tems.ts_c_dimension.height;
			    row++) {
				tem_clear_chars(tem,
				    tems.ts_c_dimension.width, row, 0);
			}
			break;

		case 1:
			/* erase beginning of screen to cursor */
			/* FIRST erase lines above the cursor */
			for (row = 0;
			    row < tem->tvs_c_cursor.row;
			    row++) {
				tem_clear_chars(tem,
				    tems.ts_c_dimension.width, row, 0);
			}
			/* THEN erase beginning of line to cursor */
			tem_clear_chars(tem,
			    tem->tvs_c_cursor.col + 1,
			    tem->tvs_c_cursor.row, 0);
			break;

		case 2:
			/* erase whole screen */
			for (row = 0;
			    row < tems.ts_c_dimension.height;
			    row++) {
				tem_clear_chars(tem,
				    tems.ts_c_dimension.width, row, 0);
			}
			break;
		}
		break;

	case 'K':		/* EL - Erase in Line */
		tem_send_data(tem);
		tem_setparam(tem, 1, 0);
		switch (tem->tvs_params[0]) {
		case 0:
			/* erase cursor to end of line */
			tem_clear_chars(tem,
			    (tems.ts_c_dimension.width -
			    tem->tvs_c_cursor.col),
			    tem->tvs_c_cursor.row,
			    tem->tvs_c_cursor.col);
			break;

		case 1:
			/* erase beginning of line to cursor */
			tem_clear_chars(tem,
			    tem->tvs_c_cursor.col + 1,
			    tem->tvs_c_cursor.row, 0);
			break;

		case 2:
			/* erase whole line */
			tem_clear_chars(tem,
			    tems.ts_c_dimension.width,
			    tem->tvs_c_cursor.row, 0);
			break;
		}
		break;

	case 'L':		/* insert line */
		tem_send_data(tem);
		tem_setparam(tem, 1, 1);
		tem_scroll(tem,
		    tem->tvs_c_cursor.row,
		    tems.ts_c_dimension.height - 1,
		    tem->tvs_params[0], TEM_SCROLL_DOWN);
		break;

	case 'M':		/* delete line */
		tem_send_data(tem);
		tem_setparam(tem, 1, 1);
		tem_scroll(tem,
		    tem->tvs_c_cursor.row,
		    tems.ts_c_dimension.height - 1,
		    tem->tvs_params[0], TEM_SCROLL_UP);
		break;

	case 'P':		/* DCH - delete char */
		tem_setparam(tem, 1, 1);
		tem_shift(tem, tem->tvs_params[0], TEM_SHIFT_LEFT);
		break;

	case 'S':		/* scroll up */
		tem_send_data(tem);
		tem_setparam(tem, 1, 1);
		tem_scroll(tem, 0,
		    tems.ts_c_dimension.height - 1,
		    tem->tvs_params[0], TEM_SCROLL_UP);
		break;

	case 'T':		/* scroll down */
		tem_send_data(tem);
		tem_setparam(tem, 1, 1);
		tem_scroll(tem, 0,
		    tems.ts_c_dimension.height - 1,
		    tem->tvs_params[0], TEM_SCROLL_DOWN);
		break;

	case 'X':		/* erase char */
		tem_setparam(tem, 1, 1);
		tem_clear_chars(tem,
		    tem->tvs_params[0],
		    tem->tvs_c_cursor.row,
		    tem->tvs_c_cursor.col);
		break;

	case 'Z':		/* cursor backward tabulation */
		tem_setparam(tem, 1, 1);

		/*
		 * Rule exception - We do sanity checking here.
		 *
		 * Restrict the count to a sane value to keep from
		 * looping for a long time.  There can't be more than one
		 * tab stop per column, so use that as a limit.
		 */
		if (tem->tvs_params[0] > tems.ts_c_dimension.width)
			tem->tvs_params[0] = tems.ts_c_dimension.width;

		for (i = 0; i < tem->tvs_params[0]; i++)
			tem_back_tab(tem);
		break;
	}
	tem->tvs_state = A_STATE_START;
}


/*
 * Gather the parameters of an ANSI escape sequence
 */
static void
tem_getparams(struct tem_vt_state *tem, uint8_t ch)
{
	if (ch >= '0' && ch <= '9') {
		tem->tvs_paramval = ((tem->tvs_paramval * 10) + (ch - '0'));
		tem->tvs_gotparam = B_TRUE;  /* Remember got parameter */
		return; /* Return immediately */
	} else if (tem->tvs_state == A_STATE_CSI_EQUAL ||
	    tem->tvs_state == A_STATE_CSI_QMARK) {
		tem->tvs_state = A_STATE_START;
	} else {
		if (tem->tvs_curparam < TEM_MAXPARAMS) {
			if (tem->tvs_gotparam) {
				/* get the parameter value */
				tem->tvs_params[tem->tvs_curparam] =
				    tem->tvs_paramval;
			}
			tem->tvs_curparam++;
		}

		if (ch == ';') {
			/* Restart parameter search */
			tem->tvs_gotparam = B_FALSE;
			tem->tvs_paramval = 0; /* No parame value yet */
		} else {
			/* Handle escape sequence */
			tem_chkparam(tem, ch);
		}
	}
}

/*
 * Add character to internal buffer.
 * When its full, send it to the next layer.
 */
static void
tem_outch(struct tem_vt_state *tem, uint8_t ch)
{
	/* buffer up the character until later */

	tem->tvs_outbuf[tem->tvs_outindex++] = ch;
	tem->tvs_c_cursor.col++;
	if (tem->tvs_c_cursor.col >= tems.ts_c_dimension.width) {
		tem_send_data(tem);
		tem_new_line(tem);
	}
}

static void
tem_new_line(struct tem_vt_state *tem)
{
	tem_cr(tem);
	tem_lf(tem);
}

static void
tem_cr(struct tem_vt_state *tem)
{
	tem->tvs_c_cursor.col = 0;
	tem_align_cursor(tem);
}

static void
tem_lf(struct tem_vt_state *tem)
{
	int row;

	/*
	 * Sanity checking notes:
	 * . a_nscroll was validated when it was set.
	 * . Regardless of that, tem_scroll and tem_mv_cursor
	 *   will prevent anything bad from happening.
	 */
	row = tem->tvs_c_cursor.row + 1;

	if (row >= tems.ts_c_dimension.height) {
		if (tem->tvs_nscroll != 0) {
			tem_scroll(tem, 0,
			    tems.ts_c_dimension.height - 1,
			    tem->tvs_nscroll, TEM_SCROLL_UP);
			row = tems.ts_c_dimension.height -
			    tem->tvs_nscroll;
		} else {	/* no scroll */
			/*
			 * implement Esc[#r when # is zero.  This means no
			 * scroll but just return cursor to top of screen,
			 * do not clear screen.
			 */
			row = 0;
		}
	}

	tem_mv_cursor(tem, row, tem->tvs_c_cursor.col);

	if (tem->tvs_nscroll == 0) {
		/* erase rest of cursor line */
		tem_clear_chars(tem,
		    tems.ts_c_dimension.width -
		    tem->tvs_c_cursor.col,
		    tem->tvs_c_cursor.row,
		    tem->tvs_c_cursor.col);

	}

	tem_align_cursor(tem);
}

static void
tem_send_data(struct tem_vt_state *tem)
{
	text_color_t fg_color;
	text_color_t bg_color;

	if (tem->tvs_outindex == 0) {
		tem_align_cursor(tem);
		return;
	}

	tem_get_color(tem, &fg_color, &bg_color, TEM_ATTR_REVERSE);
	tem_virtual_display(tem,
	    tem->tvs_outbuf, tem->tvs_outindex,
	    tem->tvs_s_cursor.row, tem->tvs_s_cursor.col,
	    fg_color, bg_color);

	if (tem->tvs_isactive) {
		/*
		 * Call the primitive to render this data.
		 */
		tem_callback_display(tem,
		    tem->tvs_outbuf, tem->tvs_outindex,
		    tem->tvs_s_cursor.row, tem->tvs_s_cursor.col,
		    fg_color, bg_color);
	}

	tem->tvs_outindex = 0;

	tem_align_cursor(tem);
}


/*
 * We have just done something to the current output point.  Reset the start
 * point for the buffered data in a_outbuf.  There shouldn't be any data
 * buffered yet.
 */
static void
tem_align_cursor(struct tem_vt_state *tem)
{
	tem->tvs_s_cursor.row = tem->tvs_c_cursor.row;
	tem->tvs_s_cursor.col = tem->tvs_c_cursor.col;
}

/*
 * State machine parser based on the current state and character input
 * major terminations are to control character or normal character
 */

static void
tem_parse(struct tem_vt_state *tem, uint8_t ch)
{
	int	i;

	if (tem->tvs_state == A_STATE_START) {	/* Normal state? */
		if (ch == A_CSI || ch == A_ESC || ch < ' ') {
			/* Control */
			tem_control(tem, ch);
		} else {
			/* Display */
			tem_outch(tem, ch);
		}
		return;
	}

	/* In <ESC> sequence */
	if (tem->tvs_state != A_STATE_ESC) {	/* Need to get parameters? */
		if (tem->tvs_state != A_STATE_CSI) {
			tem_getparams(tem, ch);
			return;
		}

		switch (ch) {
		case '?':
			tem->tvs_state = A_STATE_CSI_QMARK;
			return;
		case '=':
			tem->tvs_state = A_STATE_CSI_EQUAL;
			return;
		case 's':
			/*
			 * As defined below, this sequence
			 * saves the cursor.  However, Sun
			 * defines ESC[s as reset.  We resolved
			 * the conflict by selecting reset as it
			 * is exported in the termcap file for
			 * sun-mon, while the "save cursor"
			 * definition does not exist anywhere in
			 * /etc/termcap.
			 * However, having no coherent
			 * definition of reset, we have not
			 * implemented it.
			 */

			/*
			 * Original code
			 * tem->tvs_r_cursor.row = tem->tvs_c_cursor.row;
			 * tem->tvs_r_cursor.col = tem->tvs_c_cursor.col;
			 * tem->tvs_state = A_STATE_START;
			 */

			tem->tvs_state = A_STATE_START;
			return;
		case 'u':
			tem_mv_cursor(tem, tem->tvs_r_cursor.row,
			    tem->tvs_r_cursor.col);
			tem->tvs_state = A_STATE_START;
			return;
		case 'p': 	/* sunbow */
			tem_send_data(tem);
			/*
			 * Don't set anything if we are
			 * already as we want to be.
			 */
			if (tem->tvs_flags & TEM_ATTR_SCREEN_REVERSE) {
				tem->tvs_flags &= ~TEM_ATTR_SCREEN_REVERSE;
				/*
				 * If we have switched the characters to be the
				 * inverse from the screen, then switch them as
				 * well to keep them the inverse of the screen.
				 */
				if (tem->tvs_flags & TEM_ATTR_REVERSE)
					tem->tvs_flags &= ~TEM_ATTR_REVERSE;
				else
					tem->tvs_flags |= TEM_ATTR_REVERSE;
			}
			tem_cls(tem);
			tem->tvs_state = A_STATE_START;
			return;
		case 'q':  	/* sunwob */
			tem_send_data(tem);
			/*
			 * Don't set anything if we are
			 * already where as we want to be.
			 */
			if (!(tem->tvs_flags & TEM_ATTR_SCREEN_REVERSE)) {
				tem->tvs_flags |= TEM_ATTR_SCREEN_REVERSE;
				/*
				 * If we have switched the characters to be the
				 * inverse from the screen, then switch them as
				 * well to keep them the inverse of the screen.
				 */
				if (!(tem->tvs_flags & TEM_ATTR_REVERSE))
					tem->tvs_flags |= TEM_ATTR_REVERSE;
				else
					tem->tvs_flags &= ~TEM_ATTR_REVERSE;
			}

			tem_cls(tem);
			tem->tvs_state = A_STATE_START;
			return;
		case 'r':	/* sunscrl */
			/*
			 * Rule exception:  check for validity here.
			 */
			tem->tvs_nscroll = tem->tvs_paramval;
			if (tem->tvs_nscroll > tems.ts_c_dimension.height)
				tem->tvs_nscroll = tems.ts_c_dimension.height;
			if (tem->tvs_nscroll < 0)
				tem->tvs_nscroll = 1;
			tem->tvs_state = A_STATE_START;
			return;
		default:
			tem_getparams(tem, ch);
			return;
		}
	}

	/* Previous char was <ESC> */
	if (ch == '[') {
		tem->tvs_curparam = 0;
		tem->tvs_paramval = 0;
		tem->tvs_gotparam = B_FALSE;
		/* clear the parameters */
		for (i = 0; i < TEM_MAXPARAMS; i++)
			tem->tvs_params[i] = -1;
		tem->tvs_state = A_STATE_CSI;
	} else if (ch == 'Q') {	/* <ESC>Q ? */
		tem->tvs_state = A_STATE_START;
	} else if (ch == 'C') {	/* <ESC>C ? */
		tem->tvs_state = A_STATE_START;
	} else {
		tem->tvs_state = A_STATE_START;
		if (ch == 'c') {
			/* ESC c resets display */
			tem_reset_display(tem, B_TRUE, B_TRUE);
		} else if (ch == 'H') {
			/* ESC H sets a tab */
			tem_set_tab(tem);
		} else if (ch == '7') {
			/* ESC 7 Save Cursor position */
			tem->tvs_r_cursor.row = tem->tvs_c_cursor.row;
			tem->tvs_r_cursor.col = tem->tvs_c_cursor.col;
		} else if (ch == '8') {
			/* ESC 8 Restore Cursor position */
			tem_mv_cursor(tem, tem->tvs_r_cursor.row,
			    tem->tvs_r_cursor.col);
		/* check for control chars */
		} else if (ch < ' ') {
			tem_control(tem, ch);
		} else {
			tem_outch(tem, ch);
		}
	}
}

/* ARGSUSED */
static void
tem_bell(struct tem_vt_state *tem)
{
		/* (void) beep(BEEP_CONSOLE); */
}


static void
tem_scroll(struct tem_vt_state *tem, int start, int end, int count,
    int direction)
{
	int	row;
	int	lines_affected;

	lines_affected = end - start + 1;
	if (count > lines_affected)
		count = lines_affected;
	if (count <= 0)
		return;

	switch (direction) {
	case TEM_SCROLL_UP:
		if (count < lines_affected) {
			tem_copy_area(tem, 0, start + count,
			    tems.ts_c_dimension.width - 1, end, 0, start);
		}
		for (row = (end - count) + 1; row <= end; row++) {
			tem_clear_chars(tem, tems.ts_c_dimension.width, row, 0);
		}
		break;

	case TEM_SCROLL_DOWN:
		if (count < lines_affected) {
			tem_copy_area(tem, 0, start,
			    tems.ts_c_dimension.width - 1,
			    end - count, 0, start + count);
		}
		for (row = start; row < start + count; row++) {
			tem_clear_chars(tem, tems.ts_c_dimension.width, row, 0);
		}
		break;
	}
}

static void
tem_copy_area(struct tem_vt_state *tem,
    screen_pos_t s_col, screen_pos_t s_row,
    screen_pos_t e_col, screen_pos_t e_row,
    screen_pos_t t_col, screen_pos_t t_row)
{
	int rows;
	int cols;

	if (s_col < 0 || s_row < 0 ||
	    e_col < 0 || e_row < 0 ||
	    t_col < 0 || t_row < 0 ||
	    s_col >= tems.ts_c_dimension.width ||
	    e_col >= tems.ts_c_dimension.width ||
	    t_col >= tems.ts_c_dimension.width ||
	    s_row >= tems.ts_c_dimension.height ||
	    e_row >= tems.ts_c_dimension.height ||
	    t_row >= tems.ts_c_dimension.height)
		return;

	if (s_row > e_row || s_col > e_col)
		return;

	rows = e_row - s_row + 1;
	cols = e_col - s_col + 1;
	if (t_row + rows > tems.ts_c_dimension.height ||
	    t_col + cols > tems.ts_c_dimension.width)
		return;

	tem_virtual_copy(tem, s_col, s_row, e_col, e_row, t_col, t_row);

	if (!tem->tvs_isactive)
		return;

	tem_callback_copy(tem, s_col, s_row, e_col, e_row, t_col, t_row);
}

static void
tem_clear_chars(struct tem_vt_state *tem, int count, screen_pos_t row,
    screen_pos_t col)
{
	if (row < 0 || row >= tems.ts_c_dimension.height ||
	    col < 0 || col >= tems.ts_c_dimension.width ||
	    count < 0)
		return;

	/*
	 * Note that very large values of "count" could cause col+count
	 * to overflow, so we check "count" independently.
	 */
	if (count > tems.ts_c_dimension.width ||
	    col + count > tems.ts_c_dimension.width)
		count = tems.ts_c_dimension.width - col;

	tem_virtual_cls(tem, count, row, col);

	if (!tem->tvs_isactive)
		return;

	tem_callback_cls(tem, count, row, col);
}

/*ARGSUSED*/
static void
tem_text_display(struct tem_vt_state *tem, uint8_t *string,
    int count, screen_pos_t row, screen_pos_t col,
    text_color_t fg_color, text_color_t bg_color)
{
	struct vis_consdisplay da;

	da.data = string;
	da.width = (screen_size_t)count;
	da.row = row;
	da.col = col;

	da.fg_color = fg_color;
	da.bg_color = bg_color;

	tems_display(&da);
}

#if 0
/*
 * This function is used to blit a rectangular color image,
 * unperturbed on the underlying framebuffer, to render
 * icons and pictures.  The data is a pixel pattern that
 * fills a rectangle bounded to the width and height parameters.
 * The color pixel data must to be pre-adjusted by the caller
 * for the current video depth.
 *
 * This function is unused now.
 */
/*ARGSUSED*/
static void
tem_image_display(struct tem_vt_state *tem, uint8_t *image,
    int height, int width, screen_pos_t row, screen_pos_t col)
{
	struct vis_consdisplay da;

	da.data = image;
	da.width = (screen_size_t)width;
	da.height = (screen_size_t)height;
	da.row = row;
	da.col = col;

	tems_display(&da);
}
#endif

/*ARGSUSED*/
static void
tem_text_copy(struct tem_vt_state *tem,
    screen_pos_t s_col, screen_pos_t s_row,
    screen_pos_t e_col, screen_pos_t e_row,
    screen_pos_t t_col, screen_pos_t t_row)
{
	struct vis_conscopy da;

	da.s_row = s_row;
	da.s_col = s_col;
	da.e_row = e_row;
	da.e_col = e_col;
	da.t_row = t_row;
	da.t_col = t_col;
	tems_copy(&da);
}

static void
tem_text_cls(struct tem_vt_state *tem,
    int count, screen_pos_t row, screen_pos_t col)
{
	struct vis_consdisplay da;

	da.data = tems.ts_blank_line;
	da.width = (screen_size_t)count;
	da.row = row;
	da.col = col;

	tem_get_color(tem, &da.fg_color, &da.bg_color, TEM_ATTR_SCREEN_REVERSE);
	tems_display(&da);
}

static void
tem_pix_display(struct tem_vt_state *tem,
    uint8_t *string, int count,
    screen_pos_t row, screen_pos_t col,
    text_color_t fg_color, text_color_t bg_color)
{
	struct vis_consdisplay da;
	int	i;

	da.data = (uint8_t *)tem->tvs_pix_data;
	da.width = tems.ts_font.width;
	da.height = tems.ts_font.height;
	da.row = (row * da.height) + tems.ts_p_offset.y;
	da.col = (col * da.width) + tems.ts_p_offset.x;

	for (i = 0; i < count; i++) {
		tem_callback_bit2pix(tem, string[i], fg_color, bg_color);
		tems_display(&da);
		da.col += da.width;
	}
}

static void
tem_pix_copy(struct tem_vt_state *tem,
    screen_pos_t s_col, screen_pos_t s_row,
    screen_pos_t e_col, screen_pos_t e_row,
    screen_pos_t t_col, screen_pos_t t_row)
{
	struct vis_conscopy ma;
	static boolean_t need_clear = B_TRUE;

	if (need_clear && tem->tvs_first_line > 0) {
		/*
		 * Clear OBP output above our kernel console term
		 * when our kernel console term begins to scroll up,
		 * we hope it is user friendly.
		 * (Also see comments on tem_pix_clear_prom_output)
		 *
		 * This is only one time call.
		 */
		tem_pix_clear_prom_output(tem);
	}
	need_clear = B_FALSE;

	ma.s_row = s_row * tems.ts_font.height + tems.ts_p_offset.y;
	ma.e_row = (e_row + 1) * tems.ts_font.height + tems.ts_p_offset.y - 1;
	ma.t_row = t_row * tems.ts_font.height + tems.ts_p_offset.y;

	/*
	 * Check if we're in process of clearing OBP's columns area,
	 * which only happens when term scrolls up a whole line.
	 */
	if (tem->tvs_first_line > 0 && t_row < s_row && t_col == 0 &&
	    e_col == tems.ts_c_dimension.width - 1) {
		/*
		 * We need to clear OBP's columns area outside our kernel
		 * console term. So that we set ma.e_col to entire row here.
		 */
		ma.s_col = s_col * tems.ts_font.width;
		ma.e_col = tems.ts_p_dimension.width - 1;

		ma.t_col = t_col * tems.ts_font.width;
	} else {
		ma.s_col = s_col * tems.ts_font.width + tems.ts_p_offset.x;
		ma.e_col = (e_col + 1) * tems.ts_font.width +
		    tems.ts_p_offset.x - 1;
		ma.t_col = t_col * tems.ts_font.width + tems.ts_p_offset.x;
	}

	tems_copy(&ma);

	if (tem->tvs_first_line > 0 && t_row < s_row) {
		/* We have scrolled up (s_row - t_row) rows. */
		tem->tvs_first_line -= (s_row - t_row);
		if (tem->tvs_first_line <= 0) {
			/* All OBP rows have been cleared. */
			tem->tvs_first_line = 0;
		}
	}
}

static void
tem_pix_bit2pix(struct tem_vt_state *tem, unsigned char c,
    unsigned char fg, unsigned char bg)
{
	void (*fp)(struct tem_vt_state *, unsigned char,
	    unsigned char, unsigned char);

	switch (tems.ts_pdepth) {
	case 4:
		fp = bit_to_pix4;
		break;
	case 8:
		fp = bit_to_pix8;
		break;
	case 15:
	case 16:
		fp = bit_to_pix16;
		break;
	case 24:
		fp = bit_to_pix24;
		break;
	case 32:
		fp = bit_to_pix32;
		break;
	default:
		return;
	}

	fp(tem, c, fg, bg);
}


/*
 * This function only clears count of columns in one row
 */
static void
tem_pix_cls(struct tem_vt_state *tem, int count,
    screen_pos_t row, screen_pos_t col)
{
	tem_pix_cls_range(tem, row, 1, tems.ts_p_offset.y,
	    col, count, tems.ts_p_offset.x, B_FALSE);
}

/*
 * This function clears OBP output above our kernel console term area
 * because OBP's term may have a bigger terminal window than that of
 * our kernel console term. So we need to clear OBP output garbage outside
 * of our kernel console term at a proper time, which is when the first
 * row output of our kernel console term scrolls at the first screen line.
 *
 *	_________________________________
 *	|   _____________________	|  ---> OBP's bigger term window
 *	|   |			|	|
 *	|___|			|	|
 *	| | |			|	|
 *	| | |			|	|
 *	|_|_|___________________|_______|
 *	  | |			|	   ---> first line
 *	  | |___________________|---> our kernel console term window
 *	  |
 *	  |---> columns area to be cleared
 *
 * This function only takes care of the output above our kernel console term,
 * and tem_prom_scroll_up takes care of columns area outside of our kernel
 * console term.
 */
static void
tem_pix_clear_prom_output(struct tem_vt_state *tem)
{
	int	nrows, ncols, width, height, offset;

	width = tems.ts_font.width;
	height = tems.ts_font.height;
	offset = tems.ts_p_offset.y % height;

	nrows = tems.ts_p_offset.y / height;
	ncols = (tems.ts_p_dimension.width + (width - 1))/ width;

	if (nrows > 0)
		tem_pix_cls_range(tem, 0, nrows, offset, 0, ncols, 0,
		    B_FALSE);
}

/*
 * clear the whole screen for pixel mode, just clear the
 * physical screen.
 */
static void
tem_pix_clear_entire_screen(struct tem_vt_state *tem)
{
	struct vis_consclear cl;
	text_color_t fg_color;
	text_color_t bg_color;
	int	nrows, ncols, width, height;

	/* call driver first, if error, clear terminal area */
	tem_get_color(tem, &fg_color, &bg_color, TEM_ATTR_SCREEN_REVERSE);
	cl.bg_color = bg_color;
	if (tems_cls(&cl) == 0)
		return;

	width = tems.ts_font.width;
	height = tems.ts_font.height;

	nrows = (tems.ts_p_dimension.height + (height - 1))/ height;
	ncols = (tems.ts_p_dimension.width + (width - 1))/ width;

	tem_pix_cls_range(tem, 0, nrows, tems.ts_p_offset.y, 0, ncols,
	    tems.ts_p_offset.x, B_FALSE);

	/*
	 * Since the whole screen is cleared, we don't need
	 * to clear OBP output later.
	 */
	if (tem->tvs_first_line > 0)
		tem->tvs_first_line = 0;
}

/*
 * clear the whole screen, including the virtual screen buffer,
 * and reset the cursor to start point.
 */
static void
tem_cls(struct tem_vt_state *tem)
{
	int	row;
	struct vis_consclear cl;
	text_color_t fg_color;
	text_color_t bg_color;

	tem_get_color(tem, &fg_color, &bg_color, TEM_ATTR_SCREEN_REVERSE);
	cl.bg_color = bg_color;

	for (row = 0; row < tems.ts_c_dimension.height; row++) {
		tem_virtual_cls(tem, tems.ts_c_dimension.width, row, 0);
	}
	tem->tvs_c_cursor.row = 0;
	tem->tvs_c_cursor.col = 0;
	tem_align_cursor(tem);

	if (!tem->tvs_isactive)
		return;

	(void)tems_cls(&cl);
}

static void
tem_back_tab(struct tem_vt_state *tem)
{
	int	i;
	screen_pos_t	tabstop;

	tabstop = 0;

	for (i = tem->tvs_ntabs - 1; i >= 0; i--) {
		if (tem->tvs_tabs[i] < tem->tvs_c_cursor.col) {
			tabstop = tem->tvs_tabs[i];
			break;
		}
	}

	tem_mv_cursor(tem, tem->tvs_c_cursor.row, tabstop);
}

static void
tem_tab(struct tem_vt_state *tem)
{
	int	i;
	screen_pos_t	tabstop;

	tabstop = tems.ts_c_dimension.width - 1;

	for (i = 0; i < tem->tvs_ntabs; i++) {
		if (tem->tvs_tabs[i] > tem->tvs_c_cursor.col) {
			tabstop = tem->tvs_tabs[i];
			break;
		}
	}

	tem_mv_cursor(tem, tem->tvs_c_cursor.row, tabstop);
}

static void
tem_set_tab(struct tem_vt_state *tem)
{
	int	i;
	int	j;

	if (tem->tvs_ntabs == TEM_MAXTAB)
		return;
	if (tem->tvs_ntabs == 0 ||
	    tem->tvs_tabs[tem->tvs_ntabs] < tem->tvs_c_cursor.col) {
			tem->tvs_tabs[tem->tvs_ntabs++] = tem->tvs_c_cursor.col;
			return;
	}
	for (i = 0; i < tem->tvs_ntabs; i++) {
		if (tem->tvs_tabs[i] == tem->tvs_c_cursor.col)
			return;
		if (tem->tvs_tabs[i] > tem->tvs_c_cursor.col) {
			for (j = tem->tvs_ntabs - 1; j >= i; j--)
				tem->tvs_tabs[j+ 1] = tem->tvs_tabs[j];
			tem->tvs_tabs[i] = tem->tvs_c_cursor.col;
			tem->tvs_ntabs++;
			return;
		}
	}
}

static void
tem_clear_tabs(struct tem_vt_state *tem, int action)
{
	int	i;
	int	j;

	switch (action) {
	case 3: /* clear all tabs */
		tem->tvs_ntabs = 0;
		break;
	case 0: /* clr tab at cursor */

		for (i = 0; i < tem->tvs_ntabs; i++) {
			if (tem->tvs_tabs[i] == tem->tvs_c_cursor.col) {
				tem->tvs_ntabs--;
				for (j = i; j < tem->tvs_ntabs; j++)
					tem->tvs_tabs[j] = tem->tvs_tabs[j + 1];
				return;
			}
		}
		break;
	}
}

static void
tem_mv_cursor(struct tem_vt_state *tem, int row, int col)
{
	/*
	 * Sanity check and bounds enforcement.  Out of bounds requests are
	 * clipped to the screen boundaries.  This seems to be what SPARC
	 * does.
	 */
	if (row < 0)
		row = 0;
	if (row >= tems.ts_c_dimension.height)
		row = tems.ts_c_dimension.height - 1;
	if (col < 0)
		col = 0;
	if (col >= tems.ts_c_dimension.width)
		col = tems.ts_c_dimension.width - 1;

	tem_send_data(tem);
	tem->tvs_c_cursor.row = (screen_pos_t)row;
	tem->tvs_c_cursor.col = (screen_pos_t)col;
	tem_align_cursor(tem);
}

/* ARGSUSED */
static void
tem_reset_emulator(struct tem_vt_state *tem, boolean_t init_color)
{
	int j;

	tem->tvs_c_cursor.row = 0;
	tem->tvs_c_cursor.col = 0;
	tem->tvs_r_cursor.row = 0;
	tem->tvs_r_cursor.col = 0;
	tem->tvs_s_cursor.row = 0;
	tem->tvs_s_cursor.col = 0;
	tem->tvs_outindex = 0;
	tem->tvs_state = A_STATE_START;
	tem->tvs_gotparam = B_FALSE;
	tem->tvs_curparam = 0;
	tem->tvs_paramval = 0;
	tem->tvs_nscroll = 1;

	if (init_color) {
		/* use initial settings */
		tem->tvs_fg_color = tems.ts_init_color.fg_color;
		tem->tvs_bg_color = tems.ts_init_color.bg_color;
		tem->tvs_flags = tems.ts_init_color.a_flags;
	}

	/*
	 * set up the initial tab stops
	 */
	tem->tvs_ntabs = 0;
	for (j = 8; j < tems.ts_c_dimension.width; j += 8)
		tem->tvs_tabs[tem->tvs_ntabs++] = (screen_pos_t)j;

	for (j = 0; j < TEM_MAXPARAMS; j++)
		tem->tvs_params[j] = 0;
}

static void
tem_reset_display(struct tem_vt_state *tem,
    boolean_t clear_txt, boolean_t init_color)
{
	tem_reset_emulator(tem, init_color);

	if (clear_txt) {
		if (tem->tvs_isactive)
			tem_callback_cursor(tem, VIS_HIDE_CURSOR);

		tem_cls(tem);

		if (tem->tvs_isactive)
			tem_callback_cursor(tem, VIS_DISPLAY_CURSOR);
	}
}

static void
tem_shift(struct tem_vt_state *tem, int count, int direction)
{
	int rest_of_line;

	rest_of_line = tems.ts_c_dimension.width - tem->tvs_c_cursor.col;
	if (count > rest_of_line)
		count = rest_of_line;

	if (count <= 0)
		return;

	switch (direction) {
	case TEM_SHIFT_LEFT:
		if (count < rest_of_line) {
			tem_copy_area(tem,
			    tem->tvs_c_cursor.col + count,
			    tem->tvs_c_cursor.row,
			    tems.ts_c_dimension.width - 1,
			    tem->tvs_c_cursor.row,
			    tem->tvs_c_cursor.col,
			    tem->tvs_c_cursor.row);
		}

		tem_clear_chars(tem, count, tem->tvs_c_cursor.row,
		    (tems.ts_c_dimension.width - count));
		break;
	case TEM_SHIFT_RIGHT:
		if (count < rest_of_line) {
			tem_copy_area(tem,
			    tem->tvs_c_cursor.col,
			    tem->tvs_c_cursor.row,
			    tems.ts_c_dimension.width - count - 1,
			    tem->tvs_c_cursor.row,
			    tem->tvs_c_cursor.col + count,
			    tem->tvs_c_cursor.row);
		}

		tem_clear_chars(tem, count, tem->tvs_c_cursor.row,
		    tem->tvs_c_cursor.col);
		break;
	}
}

static void
tem_text_cursor(struct tem_vt_state *tem, short action)
{
	struct vis_conscursor	ca;

	ca.row = tem->tvs_c_cursor.row;
	ca.col = tem->tvs_c_cursor.col;
	ca.action = action;

	tems_cursor(&ca);

	if (action == VIS_GET_CURSOR) {
		tem->tvs_c_cursor.row = ca.row;
		tem->tvs_c_cursor.col = ca.col;
	}
}

static void
tem_pix_cursor(struct tem_vt_state *tem, short action)
{
	struct vis_conscursor	ca;
	uint32_t color;
	text_color_t fg, bg;

	ca.row = tem->tvs_c_cursor.row * tems.ts_font.height +
	    tems.ts_p_offset.y;
	ca.col = tem->tvs_c_cursor.col * tems.ts_font.width +
	    tems.ts_p_offset.x;
	ca.width = tems.ts_font.width;
	ca.height = tems.ts_font.height;

	tem_get_color(tem, &fg, &bg, TEM_ATTR_REVERSE);

	switch (tems.ts_pdepth) {
	case 4:
	case 8:
		ca.fg_color.mono = fg;
		ca.bg_color.mono = bg;
		break;
	case 15:
	case 16:
		color = tems.ts_color_map(fg);
		ca.fg_color.sixteen[0] = (color >> 8) & 0xFF;
		ca.fg_color.sixteen[1] = color & 0xFF;
		color = tems.ts_color_map(bg);
		ca.bg_color.sixteen[0] = (color >> 8) & 0xFF;
		ca.bg_color.sixteen[1] = color & 0xFF;
		break;
	case 24:
	case 32:
		color = tems.ts_color_map(fg);
		ca.fg_color.twentyfour[0] = (color >> 16) & 0xFF;
		ca.fg_color.twentyfour[1] = (color >> 8) & 0xFF;
		ca.fg_color.twentyfour[2] = color & 0xFF;
		color = tems.ts_color_map(bg);
		ca.bg_color.twentyfour[0] = (color >> 16) & 0xFF;
		ca.bg_color.twentyfour[1] = (color >> 8) & 0xFF;
		ca.bg_color.twentyfour[2] = color & 0xFF;
		break;
	}

	ca.action = action;

	tems_cursor(&ca);

	if (action == VIS_GET_CURSOR) {
		tem->tvs_c_cursor.row = 0;
		tem->tvs_c_cursor.col = 0;

		if (ca.row != 0) {
			tem->tvs_c_cursor.row = (ca.row - tems.ts_p_offset.y) /
			    tems.ts_font.height;
		}
		if (ca.col != 0) {
			tem->tvs_c_cursor.col = (ca.col - tems.ts_p_offset.x) /
			    tems.ts_font.width;
		}
	}
}

static void
bit_to_pix4(struct tem_vt_state *tem,
    uint8_t c,
    text_color_t fg_color,
    text_color_t bg_color)
{
	uint8_t *dest = (uint8_t *)tem->tvs_pix_data;
	font_bit_to_pix4(&tems.ts_font, dest, c, fg_color, bg_color);
}

static void
bit_to_pix8(struct tem_vt_state *tem,
    uint8_t c,
    text_color_t fg_color,
    text_color_t bg_color)
{
	uint8_t *dest = (uint8_t *)tem->tvs_pix_data;
	font_bit_to_pix8(&tems.ts_font, dest, c, fg_color, bg_color);
}

static void
bit_to_pix16(struct tem_vt_state *tem,
    uint8_t c,
    text_color_t fg_color4,
    text_color_t bg_color4)
{
	uint16_t fg_color16, bg_color16;
	uint16_t *dest;

	fg_color16 = (uint16_t)tems.ts_color_map(fg_color4);
	bg_color16 = (uint16_t)tems.ts_color_map(bg_color4);

	dest = (uint16_t *)tem->tvs_pix_data;
	font_bit_to_pix16(&tems.ts_font, dest, c, fg_color16, bg_color16);
}

static void
bit_to_pix24(struct tem_vt_state *tem,
    uint8_t c,
    text_color_t fg_color4,
    text_color_t bg_color4)
{
	uint32_t fg_color32, bg_color32;
	uint8_t *dest;

	fg_color32 = tems.ts_color_map(fg_color4);
	bg_color32 = tems.ts_color_map(bg_color4);

	dest = (uint8_t *)tem->tvs_pix_data;
	font_bit_to_pix24(&tems.ts_font, dest, c, fg_color32, bg_color32);
}

static void
bit_to_pix32(struct tem_vt_state *tem,
    uint8_t c,
    text_color_t fg_color4,
    text_color_t bg_color4)
{
	uint32_t fg_color32, bg_color32, *dest;

	fg_color32 = tems.ts_color_map(fg_color4);
	bg_color32 = tems.ts_color_map(bg_color4);

	dest = (uint32_t *)tem->tvs_pix_data;
	font_bit_to_pix32(&tems.ts_font, dest, c, fg_color32, bg_color32);
}

static text_color_t
ansi_bg_to_solaris(struct tem_vt_state *tem, int ansi)
{
	if (tem->tvs_flags & TEM_ATTR_BRIGHT_BG)
		return (brt_xlate[ansi]);
	else
		return (dim_xlate[ansi]);
}

static text_color_t
ansi_fg_to_solaris(struct tem_vt_state *tem, int ansi)
{
	if (tem->tvs_flags & TEM_ATTR_BRIGHT_FG ||
	    tem->tvs_flags & TEM_ATTR_BOLD) {
		return (brt_xlate[ansi]);
	} else {
		return (dim_xlate[ansi]);
	}
}

/*
 * flag: TEM_ATTR_SCREEN_REVERSE or TEM_ATTR_REVERSE
 */
static void
tem_get_color(struct tem_vt_state *tem, text_color_t *fg,
    text_color_t *bg, uint8_t flag)
{
	if (tem->tvs_flags & flag) {
		*fg = ansi_fg_to_solaris(tem,
		    tem->tvs_bg_color);
		*bg = ansi_bg_to_solaris(tem,
		    tem->tvs_fg_color);
	} else {
		*fg = ansi_fg_to_solaris(tem,
		    tem->tvs_fg_color);
		*bg = ansi_bg_to_solaris(tem,
		    tem->tvs_bg_color);
	}
}

void
tem_get_colors(tem_vt_state_t tem_arg, text_color_t *fg, text_color_t *bg)
{
	struct tem_vt_state *tem = (struct tem_vt_state *)tem_arg;

	tem_get_color(tem, fg, bg, TEM_ATTR_REVERSE);
}

/*
 * Clear a rectangle of screen for pixel mode.
 *
 * arguments:
 *    row:	start row#
 *    nrows:	the number of rows to clear
 *    offset_y:	the offset of height in pixels to begin clear
 *    col:	start col#
 *    ncols:	the number of cols to clear
 *    offset_x:	the offset of width in pixels to begin clear
 *    scroll_up: whether this function is called during sroll up,
 *		 which is called only once.
 */
static void
tem_pix_cls_range(struct tem_vt_state *tem,
    screen_pos_t row, int nrows, int offset_y,
    screen_pos_t col, int ncols, int offset_x,
    boolean_t sroll_up)
{
	struct vis_consdisplay da;
	int	i, j;
	int	row_add = 0;
	text_color_t fg_color;
	text_color_t bg_color;

	if (sroll_up)
		row_add = tems.ts_c_dimension.height - 1;

	da.width = tems.ts_font.width;
	da.height = tems.ts_font.height;

	tem_get_color(tem, &fg_color, &bg_color, TEM_ATTR_SCREEN_REVERSE);

	tem_callback_bit2pix(tem, ' ', fg_color, bg_color);
	da.data = (uint8_t *)tem->tvs_pix_data;

	for (i = 0; i < nrows; i++, row++) {
		da.row = (row + row_add) * da.height + offset_y;
		da.col = col * da.width + offset_x;
		for (j = 0; j < ncols; j++) {
			tems_display(&da);
			da.col += da.width;
		}
	}
}

/*
 * virtual screen operations
 */
static void
tem_virtual_display(struct tem_vt_state *tem, unsigned char *string,
    int count, screen_pos_t row, screen_pos_t col,
    text_color_t fg_color, text_color_t bg_color)
{
	int i, width;
	unsigned char *addr;
	text_color_t *pfgcolor;
	text_color_t *pbgcolor;

	if (row < 0 || row >= tems.ts_c_dimension.height ||
	    col < 0 || col >= tems.ts_c_dimension.width ||
	    col + count > tems.ts_c_dimension.width)
		return;

	width = tems.ts_c_dimension.width;
	addr = tem->tvs_screen_buf +  (row * width + col);
	pfgcolor = tem->tvs_fg_buf + (row * width + col);
	pbgcolor = tem->tvs_bg_buf + (row * width + col);
	for (i = 0; i < count; i++) {
		*addr++ = string[i];
		*pfgcolor++ = fg_color;
		*pbgcolor++ = bg_color;
	}
}

static void
i_virtual_copy(unsigned char *base,
    screen_pos_t s_col, screen_pos_t s_row,
    screen_pos_t e_col, screen_pos_t e_row,
    screen_pos_t t_col, screen_pos_t t_row)
{
	unsigned char   *from;
	unsigned char   *to;
	int		cnt;
	screen_size_t chars_per_row;
	unsigned char   *to_row_start;
	unsigned char   *from_row_start;
	screen_size_t   rows_to_move;
	int		cols = tems.ts_c_dimension.width;

	chars_per_row = e_col - s_col + 1;
	rows_to_move = e_row - s_row + 1;

	to_row_start = base + ((t_row * cols) + t_col);
	from_row_start = base + ((s_row * cols) + s_col);

	if (to_row_start < from_row_start) {
		while (rows_to_move-- > 0) {
			to = to_row_start;
			from = from_row_start;
			to_row_start += cols;
			from_row_start += cols;
			for (cnt = chars_per_row; cnt-- > 0; )
				*to++ = *from++;
		}
	} else {
		/*
		 * Offset to the end of the region and copy backwards.
		 */
		cnt = rows_to_move * cols + chars_per_row;
		to_row_start += cnt;
		from_row_start += cnt;

		while (rows_to_move-- > 0) {
			to_row_start -= cols;
			from_row_start -= cols;
			to = to_row_start;
			from = from_row_start;
			for (cnt = chars_per_row; cnt-- > 0; )
				*--to = *--from;
		}
	}
}

static void
tem_virtual_copy(struct tem_vt_state *tem,
    screen_pos_t s_col, screen_pos_t s_row,
    screen_pos_t e_col, screen_pos_t e_row,
    screen_pos_t t_col, screen_pos_t t_row)
{
	screen_size_t chars_per_row;
	screen_size_t   rows_to_move;
	int		rows = tems.ts_c_dimension.height;
	int		cols = tems.ts_c_dimension.width;

	if (s_col < 0 || s_col >= cols ||
	    s_row < 0 || s_row >= rows ||
	    e_col < 0 || e_col >= cols ||
	    e_row < 0 || e_row >= rows ||
	    t_col < 0 || t_col >= cols ||
	    t_row < 0 || t_row >= rows ||
	    s_col > e_col ||
	    s_row > e_row)
		return;

	chars_per_row = e_col - s_col + 1;
	rows_to_move = e_row - s_row + 1;

	/* More sanity checks. */
	if (t_row + rows_to_move > rows ||
	    t_col + chars_per_row > cols)
		return;

	i_virtual_copy(tem->tvs_screen_buf, s_col, s_row,
	    e_col, e_row, t_col, t_row);

	/* text_color_t is the same size as char */
	i_virtual_copy((unsigned char *)tem->tvs_fg_buf,
	    s_col, s_row, e_col, e_row, t_col, t_row);
	i_virtual_copy((unsigned char *)tem->tvs_bg_buf,
	    s_col, s_row, e_col, e_row, t_col, t_row);

}

static void
tem_virtual_cls(struct tem_vt_state *tem,
    int count, screen_pos_t row, screen_pos_t col)
{
	text_color_t fg_color;
	text_color_t bg_color;

	tem_get_color(tem, &fg_color, &bg_color, TEM_ATTR_SCREEN_REVERSE);
	tem_virtual_display(tem, tems.ts_blank_line, count, row, col,
	    fg_color, bg_color);
}

/*
 * only blank screen, not clear our screen buffer
 */
static void
tem_blank_screen(struct tem_vt_state *tem)
{
	int	row;

	if (tems.ts_display_mode == VIS_PIXEL) {
		tem_pix_clear_entire_screen(tem);
		return;
	}

	for (row = 0; row < tems.ts_c_dimension.height; row++) {
		tem_callback_cls(tem, tems.ts_c_dimension.width, row, 0);
	}
}

/*
 * unblank screen with associated tem from its screen buffer
 */
static void
tem_unblank_screen(struct tem_vt_state *tem)
{
	text_color_t fg_color, fg_last = 0;
	text_color_t bg_color, bg_last = 0;
	size_t	tc_size = sizeof (text_color_t);
	int	row, col, count, col_start;
	int	width;
	unsigned char *buf;

	if (tems.ts_display_mode == VIS_PIXEL)
		tem_pix_clear_entire_screen(tem);

	tem_callback_cursor(tem, VIS_HIDE_CURSOR);

	width = tems.ts_c_dimension.width;

	/*
	 * Display data in tvs_screen_buf to the actual framebuffer in a
	 * row by row way.
	 * When dealing with one row, output data with the same foreground
	 * and background color all together.
	 */
	for (row = 0; row < tems.ts_c_dimension.height; row++) {
		buf = tem->tvs_screen_buf + (row * width);
		count = col_start = 0;
		for (col = 0; col < width; col++) {
			fg_color =
			    tem->tvs_fg_buf[(row * width + col) * tc_size];
			bg_color =
			    tem->tvs_bg_buf[(row * width + col) * tc_size];
			if (col == 0) {
				fg_last = fg_color;
				bg_last = bg_color;
			}

			if ((fg_color != fg_last) || (bg_color != bg_last)) {
				/*
				 * Call the primitive to render this data.
				 */
				tem_callback_display(tem,
				    buf, count, row, col_start,
				    fg_last, bg_last);
				buf += count;
				count = 1;
				col_start = col;
				fg_last = fg_color;
				bg_last = bg_color;
			} else {
				count++;
			}
		}

		if (col_start == (width - 1))
			continue;

		/*
		 * Call the primitive to render this data.
		 */
		tem_callback_display(tem,
		    buf, count, row, col_start,
		    fg_last, bg_last);
	}

	tem_callback_cursor(tem, VIS_DISPLAY_CURSOR);
}
