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
 */
/*	Copyright (c) 1990, 1991 UNIX System Laboratories, Inc.	*/

/*	Copyright (c) 1984, 1986, 1987, 1988, 1989, 1990 AT&T	*/
/*	  All Rights Reserved  	*/

#ifndef	_SYS_TEM_H
#define	_SYS_TEM_H

#ifdef __cplusplus
extern "C" {
#endif

#include <sys/types.h>
#include <sys/visual_io.h>
#include <sys/font.h>
#include <sys/list.h>
#include <bootstrap.h>

/*
 * definitions for ANSI x3.64 terminal control language parser
 */

#define	TEM_MAXPARAMS	5	/* maximum number of ANSI paramters */
#define	TEM_MAXTAB	40	/* maximum number of tab stops */
#define	TEM_MAXFKEY	30	/* max length of function key with <ESC>Q */
#define	MAX_TEM		2	/* max number of loadable terminal emulators */

#define	TEM_SCROLL_UP		0
#define	TEM_SCROLL_DOWN		1
#define	TEM_SHIFT_LEFT		0
#define	TEM_SHIFT_RIGHT		1

#define	TEM_ATTR_NORMAL		0x0000
#define	TEM_ATTR_REVERSE	0x0001
#define	TEM_ATTR_BOLD		0x0002
#define	TEM_ATTR_BLINK		0x0004
#define	TEM_ATTR_TRANSPARENT	0x0008
#define	TEM_ATTR_SCREEN_REVERSE	0x0010
#define	TEM_ATTR_BRIGHT_FG	0x0020
#define	TEM_ATTR_BRIGHT_BG	0x0040

#define	ANSI_COLOR_BLACK	0
#define	ANSI_COLOR_WHITE	7

#define	TEM_TEXT_WHITE		0
#define	TEM_TEXT_BLACK		1
#define	TEM_TEXT_BLACK24_RED	0x00
#define	TEM_TEXT_BLACK24_GREEN	0x00
#define	TEM_TEXT_BLACK24_BLUE	0x00
#define	TEM_TEXT_WHITE24_RED	0xff
#define	TEM_TEXT_WHITE24_GREEN	0xff
#define	TEM_TEXT_WHITE24_BLUE	0xff

#define	A_STATE_START			0
#define	A_STATE_ESC			1
#define	A_STATE_CSI			2
#define	A_STATE_CSI_QMARK		3
#define	A_STATE_CSI_EQUAL		4

/*
 * Default number of rows and columns
 */
#define	TEM_DEFAULT_ROWS	34
#define	TEM_DEFAULT_COLS	80

/*
 * Default foreground/background color
 */

#define	DEFAULT_ANSI_FOREGROUND	ANSI_COLOR_BLACK
#define	DEFAULT_ANSI_BACKGROUND	ANSI_COLOR_WHITE


#define	BUF_LEN		160 /* Two lines of data can be processed at a time */

typedef uint8_t text_color_t;

typedef struct tem_color {
	text_color_t	fg_color;
	text_color_t	bg_color;
	unsigned short	a_flags;
} tem_color_t;

struct tem_pix_pos {
	screen_pos_t	x;
	screen_pos_t	y;
};

struct tem_char_pos {
	screen_pos_t	col;
	screen_pos_t	row;
};

struct tem_size {
	screen_size_t	width;
	screen_size_t	height;
};

typedef struct {
	uint8_t red[16];
	uint8_t green[16];
	uint8_t blue[16];
} text_cmap_t;

extern text_cmap_t cmap4_to_24;

/*
 * State structure for each virtual terminal emulator
 */
struct tem_vt_state {
	uint8_t		tvs_fbmode;	/* framebuffer mode */
	unsigned short	tvs_flags;	/* flags for this x3.64 terminal */
	int		tvs_state;	/* state in output esc seq processing */
	boolean_t	tvs_gotparam;	/* does output esc seq have a param */

	int	tvs_curparam;	/* current param # of output esc seq */
	int	tvs_paramval;	/* value of current param */
	int	tvs_params[TEM_MAXPARAMS];  /* parameters of output esc seq */
	screen_pos_t	tvs_tabs[TEM_MAXTAB];	/* tab stops */
	int	tvs_ntabs;		/* number of tabs used */
	int	tvs_nscroll;		/* number of lines to scroll */

	struct tem_char_pos tvs_s_cursor;	/* start cursor position */
	struct tem_char_pos tvs_c_cursor;	/* current cursor position */
	struct tem_char_pos tvs_r_cursor;	/* remembered cursor position */

	unsigned char	*tvs_outbuf;	/* place to keep incomplete lines */
	int		tvs_outbuf_size;
	int		tvs_outindex;	/* index into a_outbuf */
	void   		*tvs_pix_data;	/* pointer to tmp bitmap area */
	int		tvs_pix_data_size;
	text_color_t	tvs_fg_color;
	text_color_t	tvs_bg_color;
	int		tvs_first_line;	/* kernel console output begins */

	unsigned char	*tvs_screen_buf;	/* whole screen buffer */
	int		tvs_screen_buf_size;
	text_color_t	*tvs_fg_buf;	/* fg_color attribute cache */
	text_color_t	*tvs_bg_buf;	/* bg_color attribute cache */
	int		tvs_color_buf_size;

	boolean_t	tvs_isactive;
	int		tvs_initialized;	/* initialization flag */

	list_node_t	tvs_list_node;
};

typedef struct tem_callbacks {
	void (*tsc_display)(struct tem_vt_state *, unsigned char *, int,
	    screen_pos_t, screen_pos_t, unsigned char, unsigned char);
	void (*tsc_copy)(struct tem_vt_state *,
	    screen_pos_t, screen_pos_t, screen_pos_t, screen_pos_t,
	    screen_pos_t, screen_pos_t);
	void (*tsc_cursor)(struct tem_vt_state *, short);
	void (*tsc_bit2pix)(struct tem_vt_state *, unsigned char,
	    unsigned char, unsigned char);
	void (*tsc_cls)(struct tem_vt_state *, int, screen_pos_t, screen_pos_t);
} tem_callbacks_t;

typedef struct __tem_modechg_cb_arg *tem_modechg_cb_arg_t;
typedef void (*tem_modechg_cb_t) (tem_modechg_cb_arg_t arg);
typedef	struct __tem_vt_state *tem_vt_state_t;

/*
 * common term soft state structure shared by all virtual terminal emulators
 */
typedef struct tem_state {
	struct console	*ts_hdl;	/* Framework handle for dev */
	screen_size_t	ts_linebytes;	/* Layered on bytes per scan line */

	int	ts_display_mode;	/* What mode we are in */

	struct tem_size ts_c_dimension;	/* window dimensions in characters */
	struct tem_size ts_p_dimension;	/* screen dimensions in pixels */
	struct tem_pix_pos ts_p_offset;	/* pix offset to center the display */

	int	ts_pix_data_size;	/* size of bitmap data areas */
	int	ts_pdepth;		/* pixel depth */
	struct font	ts_font;	/* font table */

	unsigned char	*ts_blank_line;	/* a blank line for scrolling */
	tem_callbacks_t	*ts_callbacks;	/* internal output functions */

	int	ts_initialized;		/* initialization flag */

	tem_modechg_cb_t	ts_modechg_cb;
	tem_modechg_cb_arg_t	ts_modechg_arg;

	color_map_fn_t	ts_color_map;

	tem_color_t	ts_init_color; /* initial color and attributes */

	struct tem_vt_state	*ts_active;
	list_t		ts_list;	/* chain of all tems */
} tem_state_t;

extern tem_state_t tems;
/*
 * tems_* fuctions mean that they just operate on the common soft state
 * (tem_state_t), and tem_* functions mean that they operate on the
 * per-tem structure (tem_vt_state). All "safe" interfaces are in tem_safe.c.
 */
int	tems_cls(struct vis_consclear *);
void	tems_display(struct vis_consdisplay *);
void	tems_copy(struct vis_conscopy *);
void	tems_cursor(struct vis_conscursor *);

int	tem_initialized(tem_vt_state_t);

tem_vt_state_t tem_init(void);

void	tem_destroy(tem_vt_state_t);
int	tem_info_init(struct console *);
void	tem_write(tem_vt_state_t, uint8_t *, ssize_t);
void	tem_get_size(uint16_t *, uint16_t *, uint16_t *, uint16_t *);
void	tem_save_state(void);
void	tem_register_modechg_cb(tem_modechg_cb_t, tem_modechg_cb_arg_t);
void	tem_activate(tem_vt_state_t, boolean_t);
void	tem_switch(tem_vt_state_t, tem_vt_state_t);
uint8_t	tem_get_fbmode(tem_vt_state_t);
void	tem_set_fbmode(tem_vt_state_t, uint8_t);
void	tem_get_colors(tem_vt_state_t, text_color_t *, text_color_t *);

#ifdef __cplusplus
}
#endif

#endif /* _SYS_TEM_H */
