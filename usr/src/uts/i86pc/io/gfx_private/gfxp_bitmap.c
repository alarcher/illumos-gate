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
 * Framebuffer based console support.
 * Note: this is very simplifyed proof of concept code working just with
 * plain console, no X, tested with vmware fusion VM.
 *
 * Missing (no particular order):
 * memory barriers
 * shadow buffering
 * copyin for userspace calls and then polled io split.
 * callbacks for hw blt() and others?
 */
#include <sys/types.h>
#include <sys/visual_io.h>
#include <sys/fbio.h>
#include <sys/ddi.h>
#include <sys/kd.h>
#include <sys/sunddi.h>

#include "gfx_private.h"
#include "gfxp_fb.h"

#define	MYNAME	"gfxp_bitmap"

static ddi_device_acc_attr_t dev_attr = {
	DDI_DEVICE_ATTR_V0,
	DDI_NEVERSWAP_ACC,
	DDI_MERGING_OK_ACC
};

/* default structure for FBIOGATTR ioctl */
static struct fbgattr bitmap_attr =  {
/*	real_type	owner */
	FBTYPE_MEMCOLOR, 0,
/* fbtype: type		h  w  depth cms  size */
	{ FBTYPE_MEMCOLOR, 0, 0, 0, 0, 0 },
/* fbsattr: flags emu_type	dev_specific */
	{ 0, FBTYPE_MEMCOLOR, { 0 } },
/*	emu_types */
	{ -1 }
};

static struct vis_identifier text_ident = { "illumos_fb" };

static int bitmap_kdsetmode(struct gfxp_fb_softc *, int);
static int bitmap_devinit(struct gfxp_fb_softc *, struct vis_devinit *);
static void	bitmap_cons_copy(struct gfxp_fb_softc *, struct vis_conscopy *);
static void	bitmap_cons_display(struct gfxp_fb_softc *,
    struct vis_consdisplay *);
static int	bitmap_cons_clear(struct gfxp_fb_softc *);
static void	bitmap_cons_cursor(struct gfxp_fb_softc *,
    struct vis_conscursor *);
static void	bitmap_polled_copy(struct vis_polledio_arg *,
    struct vis_conscopy *);
static void	bitmap_polled_display(struct vis_polledio_arg *,
    struct vis_consdisplay *);
static void	bitmap_polled_cursor(struct vis_polledio_arg *,
    struct vis_conscursor *);
static int	bitmap_suspend(struct gfxp_fb_softc *softc);
static void	bitmap_resume(struct gfxp_fb_softc *softc);
static int	bitmap_devmap(dev_t dev, devmap_cookie_t dhp, offset_t off,
    size_t len, size_t *maplen, uint_t model, void *ptr);
#if 0
int bitmap_map(devmap_cookie_t dhp, dev_t dev, uint_t flags, offset_t off,
    size_t len, void **pvtp);
int  bitmap_dup(devmap_cookie_t dhp, void *pvtp, devmap_cookie_t new_dhp,
    void **new_pvtp);
void bitmap_unmap(devmap_cookie_t dhp, void *pvtp, offset_t off, size_t len,
    devmap_cookie_t new_dhp1, void **new_pvtp1, devmap_cookie_t new_dhp2,
    void **new_pvtp2);
#endif

static struct devmap_callback_ctl bitmap_cb = {
	.devmap_rev = DEVMAP_OPS_REV,
	.devmap_map = NULL,
	.devmap_access = NULL,
	.devmap_dup = NULL,
	.devmap_unmap = NULL
};

static struct gfxp_ops gfxp_ops = {
	.ident = &text_ident,
	.kdsetmode = bitmap_kdsetmode,
	.devinit = bitmap_devinit,
	.cons_copy = bitmap_cons_copy,
	.cons_display = bitmap_cons_display,
	.cons_cursor = bitmap_cons_cursor,
	.cons_clear = bitmap_cons_clear,
	.suspend = bitmap_suspend,
	.resume = bitmap_resume,
	.devmap = bitmap_devmap
};

/*ARGSUSED*/
int gfxp_bm_attach(dev_info_t *devi, ddi_attach_cmd_t cmd,
    struct gfxp_fb_softc *softc)
{
	softc->polledio.display = bitmap_polled_display;
	softc->polledio.copy = bitmap_polled_copy;
	softc->polledio.cursor = bitmap_polled_cursor;
	softc->gfxp_ops = &gfxp_ops;
	softc->fbgattr = &bitmap_attr;
	softc->mode = KD_GRAPHICS;

	return (DDI_SUCCESS);
}

/*ARGSUSED*/
int gfxp_bm_detach(dev_info_t *devi, ddi_detach_cmd_t cmd,
    struct gfxp_fb_softc *softc)
{
	if (softc->console.fb.fb_size != 0) {
		gfxp_unmap_kernel_space((gfxp_kva_t)softc->console.fb.fb,
		    softc->console.fb.fb_size);
		fb_info.fb = NULL;
		kmem_free(softc->console.fb.shadow_fb,
		    softc->console.fb.fb_size);
		softc->console.fb.shadow_fb = NULL;
	}
	kmem_free(softc->console.fb.cursor.data, softc->console.fb.cursor.size);
	kmem_free(softc->console.fb.cursor.buf, softc->console.fb.cursor.size);
	boot_fb_set(&fb_info);
	return (DDI_SUCCESS);
}

/*ARGSUSED*/
static int
bitmap_suspend(struct gfxp_fb_softc *softc)
{
	return (DDI_SUCCESS);
}

/*ARGSUSED*/
static void
bitmap_resume(struct gfxp_fb_softc *softc)
{
}

/*ARGSUSED*/
static int
bitmap_kdsetmode(struct gfxp_fb_softc *softc, int mode)
{
	return (ENOTSUP);
}

/*
 * Copy fb_info from early boot and set up the FB
 */
static int
bitmap_setup_fb(struct gfxp_fb_softc *softc)
{
	size_t size;

	softc->console.fb.paddr = fb_info.paddr;
	softc->console.fb.pitch = fb_info.pitch;
	softc->console.fb.bpp = fb_info.bpp;
	softc->console.fb.depth = fb_info.depth;
	softc->console.fb.rgb = fb_info.rgb;
	softc->console.fb.screen.x = fb_info.screen.x;
	softc->console.fb.screen.y = fb_info.screen.y;
	softc->console.fb.terminal_origin.x = fb_info.terminal_origin.x;
	softc->console.fb.terminal_origin.y = fb_info.terminal_origin.y;
	softc->console.fb.terminal.x = fb_info.terminal.x;
	softc->console.fb.terminal.y = fb_info.terminal.y;
	softc->console.fb.cursor.visible = fb_info.cursor.visible;
	softc->console.fb.cursor.origin.x = fb_info.cursor.origin.x;
	softc->console.fb.cursor.origin.y = fb_info.cursor.origin.y;
	softc->console.fb.cursor.pos.x = fb_info.cursor.pos.x;
	softc->console.fb.cursor.pos.y = fb_info.cursor.pos.y;
	softc->console.fb.font_width = fb_info.font_width;
	softc->console.fb.font_height = fb_info.font_height;

	size = fb_info.screen.y * fb_info.pitch;
	softc->console.fb.fb_size = ptob(btopr(size));
	softc->console.fb.fb = (uint8_t *)gfxp_map_kernel_space(fb_info.paddr,
	    softc->console.fb.fb_size, GFXP_MEMORY_WRITECOMBINED);
	if (softc->console.fb.fb == NULL) {
		softc->console.fb.fb_size = 0;
		return (DDI_FAILURE);
	}
	softc->console.fb.shadow_fb = kmem_zalloc(softc->console.fb.fb_size,
	    KM_SLEEP);
	softc->console.fb.cursor.size = fb_info.font_width *
	    fb_info.font_height * fb_info.bpp;
	softc->console.fb.cursor.data =
	    kmem_zalloc(softc->console.fb.cursor.size, KM_SLEEP);
	softc->console.fb.cursor.buf =
	    kmem_zalloc(softc->console.fb.cursor.size, KM_SLEEP);

	bitmap_attr.fbtype.fb_height = fb_info.screen.y;
	bitmap_attr.fbtype.fb_width = fb_info.screen.x;
	bitmap_attr.fbtype.fb_depth = fb_info.bpp;
	bitmap_attr.fbtype.fb_size = size;
	if (fb_info.depth == 32)
		bitmap_attr.fbtype.fb_cmsize = 1 << 24;
	else
		bitmap_attr.fbtype.fb_cmsize = 1 << fb_info.depth;

	/* this will enable boot_fb functions to access framebuffer */
	boot_fb_set(&softc->console.fb);
	return (DDI_SUCCESS);
}

static int
bitmap_devinit(struct gfxp_fb_softc *softc, struct vis_devinit *data)
{
	if (bitmap_setup_fb(softc) == DDI_FAILURE)
		return (1);

	/* initialize console instance */
	data->version = VIS_CONS_REV;
	data->width = softc->console.fb.screen.x;
	data->height = softc->console.fb.screen.y;
	data->linebytes = softc->console.fb.pitch;
	data->depth = softc->console.fb.depth;
	data->mode = VIS_PIXEL;
	data->polledio = &softc->polledio;
	data->modechg_cb = NULL;
	data->modechg_arg = NULL;
	return (0);
}

static void
bitmap_cons_copy(struct gfxp_fb_softc *softc, struct vis_conscopy *ma)
{
	uint32_t soffset, toffset;
	uint32_t width, height;
	uint8_t *src, *dst, *sdst;
	int i;

	soffset = ma->s_col * softc->console.fb.bpp +
	    ma->s_row * softc->console.fb.pitch;
	toffset = ma->t_col * softc->console.fb.bpp +
	    ma->t_row * softc->console.fb.pitch;
	src = softc->console.fb.shadow_fb + soffset;
	dst = softc->console.fb.fb + toffset;
	sdst = softc->console.fb.shadow_fb + toffset;
	width = (ma->e_col - ma->s_col + 1) * softc->console.fb.bpp;
	height = ma->e_row - ma->s_row + 1;

	if (toffset <= soffset) {
		for (i = 0; i < height; i++) {
			uint32_t increment = i * softc->console.fb.pitch;
			(void) memmove(dst + increment, src + increment, width);
			(void) memmove(sdst + increment, src + increment,
			    width);
		}
	} else {
		for (i = height - 1; i >= 0; i--) {
			uint32_t increment = i * softc->console.fb.pitch;
			(void) memmove(dst + increment, src + increment, width);
			(void) memmove(sdst + increment, src + increment,
			    width);
		}
	}
}

static void
bitmap_cons_display(struct gfxp_fb_softc *softc, struct vis_consdisplay *da)
{
	uint32_t size;		/* write size per scanline */
	uint8_t *fbp, *sfbp;	/* fb + calculated offset */
	int i;

	/* make sure we will not write past FB */
	if (da->col >= softc->console.fb.screen.x ||
	    da->row >= softc->console.fb.screen.y ||
	    da->col + da->width > softc->console.fb.screen.x ||
	    da->row + da->height > softc->console.fb.screen.y)
		return;

	size = da->width * softc->console.fb.bpp;
	fbp = softc->console.fb.fb + da->col * softc->console.fb.bpp +
	    da->row * softc->console.fb.pitch;
	sfbp = softc->console.fb.shadow_fb + da->col * softc->console.fb.bpp +
	    da->row * softc->console.fb.pitch;

	/* write all scanlines in rectangle */
	for (i = 0; i < da->height; i++) {
		uint8_t *dest = fbp + i * softc->console.fb.pitch;
		uint8_t *src = da->data + i * size;
		(void) memcpy(dest, src, size);
		dest = sfbp + i * softc->console.fb.pitch;
		(void) memcpy(dest, src, size);
	}
}

static int
bitmap_cons_clear(struct gfxp_fb_softc *softc)
{
	uint32_t size;
	size = softc->console.fb.screen.x * softc->console.fb.screen.y *
	    softc->console.fb.bpp;

	(void) memset(softc->console.fb.fb, 0, size);

	return (0);
}

static void
bitmap_hide_cursor(struct gfxp_fb_softc *softc, struct vis_conscursor *ca)
{
	struct vis_consdisplay cd;

	softc->console.fb.cursor.visible = B_FALSE;
	cd.row = ca->row;
	cd.col = ca->col;
	cd.width = ca->width;
	cd.height = ca->height;
	cd.data = softc->console.fb.cursor.buf;
	bitmap_cons_display(softc, &cd);
}

static void
bitmap_cursor_create(struct gfxp_fb_softc *softc, struct vis_conscursor *ca,
    struct vis_consdisplay *cur)
{
	uint32_t offset;
	uint32_t bpp = softc->console.fb.bpp;
	uint8_t *src, *dst;
	uint32_t size = cur->width * bpp;
	uint32_t data, *cursor, *glyph;
	uint8_t *cursor8, *glyph8;
	int i;

	data = ca->bg_color.twentyfour[0] << softc->console.fb.rgb.red.pos;
	data |= ca->bg_color.twentyfour[1] << softc->console.fb.rgb.green.pos;
	data |= ca->bg_color.twentyfour[2] << softc->console.fb.rgb.blue.pos;

	/* save data under the cursor to buffer */

	offset = cur->col * bpp + cur->row * softc->console.fb.pitch;
	for (i = 0; i < cur->height; i++) {
		src = softc->console.fb.fb + offset +
		    i * softc->console.fb.pitch;
		dst = softc->console.fb.cursor.buf + i * size;
		(void) memcpy(dst, src, size);
	}

	/* set cursor buffer */
	switch (softc->console.fb.depth) {
	case 24:
		cursor8 = softc->console.fb.cursor.data;
		glyph8 = softc->console.fb.cursor.buf;
		for (i = 0; i < cur->height * size; i += 3) {
			cursor8[i] = glyph8[i] ^ ((data >> 16) & 0xff);
			cursor8[i+1] = glyph8[i+1] ^ ((data >> 8) & 0xff);
			cursor8[i+2] = glyph8[i+2] ^ (data & 0xff);
		}
		break;
	case 32:
		cursor = (uint32_t *)softc->console.fb.cursor.data;
		glyph = (uint32_t *)softc->console.fb.cursor.buf;
		for (i = 0; i < cur->height * cur->width; i++) {
			cursor[i] = glyph[i] ^ data;
		}
		break;
	}
	cur->data = softc->console.fb.cursor.data;
}

static void
bitmap_show_cursor(struct gfxp_fb_softc *softc, struct vis_conscursor *ca)
{
	struct vis_consdisplay cd;
	softc->console.fb.cursor.visible = B_TRUE;

	cd.row = ca->row;
	cd.col = ca->col;
	cd.width = ca->width;
	cd.height = ca->height;
	bitmap_cursor_create(softc, ca, &cd);
	bitmap_cons_display(softc, &cd);
}

static void
bitmap_cons_cursor(struct gfxp_fb_softc *softc, struct vis_conscursor *ca)
{
	switch (ca->action) {
	case VIS_HIDE_CURSOR:
		if (softc->console.fb.cursor.visible)
			bitmap_hide_cursor(softc, ca);
		break;
	case VIS_DISPLAY_CURSOR:
		/* keep track of cursor position for polled mode */
		softc->console.fb.cursor.pos.x =
		    (ca->col - softc->console.fb.terminal_origin.x) /
		    softc->console.fb.font_width;
		softc->console.fb.cursor.pos.y =
		    (ca->row - softc->console.fb.terminal_origin.y) /
		    softc->console.fb.font_height;
		softc->console.fb.cursor.origin.x = ca->col;
		softc->console.fb.cursor.origin.y = ca->row;
		bitmap_show_cursor(softc, ca);
		break;
	case VIS_GET_CURSOR:
		ca->row = softc->console.fb.cursor.origin.y;
		ca->col = softc->console.fb.cursor.origin.x;
		break;
	}
}

static void
bitmap_polled_copy(struct vis_polledio_arg *arg, struct vis_conscopy *ca)
{
	struct gfxp_fb_softc *softc = (struct gfxp_fb_softc *)arg;
	bitmap_cons_copy(softc, ca);
}

static void
bitmap_polled_display(struct vis_polledio_arg *arg, struct vis_consdisplay *da)
{
	struct gfxp_fb_softc *softc = (struct gfxp_fb_softc *)arg;
	bitmap_cons_display(softc, da);
}

static void
bitmap_polled_cursor(struct vis_polledio_arg *arg, struct vis_conscursor *ca)
{
	struct gfxp_fb_softc *softc = (struct gfxp_fb_softc *)arg;
	bitmap_cons_cursor(softc, ca);
}

/*
 * Device mapping support. Should be possible to mmmap frame buffer
 * to user space. Currently not working, mmap will receive -1 as pointer.
 */
/*ARGSUSED*/
static int
bitmap_devmap(dev_t dev, devmap_cookie_t dhp, offset_t off,
    size_t len, size_t *maplen, uint_t model, void *ptr)
{
	struct gfxp_fb_softc *softc = (struct gfxp_fb_softc *)ptr;
	ddi_umem_cookie_t cookie;
	int err;
	size_t length;

	if (softc == NULL) {
		cmn_err(CE_WARN, "bitmap: Can't find softstate");
		return (-1);
	}

	if (off >= softc->console.fb.fb_size) {
		cmn_err(CE_WARN, "bitmap: Can't map offset 0x%llx", off);
		return (-1);
	}

	if (off + len > softc->console.fb.fb_size)
		length = softc->console.fb.fb_size - off;
	else
		length = len;

	cookie = gfxp_umem_cookie_init((caddr_t)softc->console.fb.fb,
	    softc->console.fb.fb_size);
	if (cookie == NULL)
		return (-1);
	err = gfxp_devmap_umem_setup(dhp, softc->devi, &bitmap_cb, cookie,
	    off, length, PROT_ALL, 0, &dev_attr);
	if (err < 0)
		gfxp_umem_cookie_destroy(cookie);

	*maplen = length;

	return (err);
}

#if 0
int bitmap_map(devmap_cookie_t dhp, dev_t dev, uint_t flags, offset_t off,
    size_t len, void **pvtp)
{
	struct ddi_umem_cookie *cp;
	devmap_handle_t *dhp;
	struct gfxp_fb_softc *softc = (struct gfxp_fb_softc *)ptr;
	int instance;

	instance = getminor(dev);
	softc = ddi_get_soft_state(xsvc_statep, instance);
	if (state == NULL) {
		return (ENXIO);
	}

	dhp = (devmap_handle_t *)dhc;

	cp = (struct ddi_umem_cookie *)dhp->dh_cookie;
	cp->cook_refcnt = 1;

	*pvtp = state;
	return (0);
}

int  bitmap_dup(devmap_cookie_t dhp, void *pvtp, devmap_cookie_t new_dhp,
    void **new_pvtp)
{
}

void bitmap_unmap(devmap_cookie_t dhp, void *pvtp, offset_t off, size_t len,
    devmap_cookie_t new_dhp1, void **new_pvtp1, devmap_cookie_t new_dhp2,
    void **new_pvtp2)
{
}
#endif
