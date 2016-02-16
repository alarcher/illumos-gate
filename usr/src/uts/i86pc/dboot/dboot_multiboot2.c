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
 * dboot module utility functions for multiboot 2 tags processing.
 */

#include <sys/inttypes.h>
#include <sys/param.h>
#include <sys/systm.h>
#ifdef _KERNEL
#include <sys/sysmacros.h>
#endif
#include <sys/multiboot2.h>
#include <sys/multiboot2_impl.h>

/* dboot does not have _KERNEL defined */
#if !defined(offsetof)
#define	offsetof(s, m)	((size_t)(&(((s *)0)->m)))
#endif

typedef boolean_t (*dboot_multiboot2_iterate_cb_t)
    (int index, multiboot_tag_t *tagp, void *data);
struct dboot_multiboot2_iterate_ctx {
	dboot_multiboot2_iterate_cb_t callback;
	int index;				/* item from set */
	uint32_t tag;				/* tag to search */
	multiboot_tag_t *tagp;			/* search result */
};

/*
 * Multiboot2 tag list elements are aligned to MULTIBOOT_TAG_ALIGN,
 * to get next item from the list, we need to use tag size and align it up.
 *
 * Using two macros here, MB2_FIRST_TAG will get address of the first
 * tag pointed by MB2 info header, and MB2_NEXT_TAG will get pointer to
 * next tag in list. The tag list is terminated by MULTIBOOT_TAG_TYPE_END tag.
 */

/*
 * Walk tag list till we hit first instance of given tag or end of the list.
 * MB2_NEXT_TAG() will return NULL on end of list.
 */
static void *
dboot_multiboot2_find_tag_impl(multiboot_tag_t *tagp, uint32_t tag)
{
	while (tagp != NULL && tagp->type != tag) {
		tagp = MB2_NEXT_TAG(tagp);
	}
	return (tagp);
}

/*
 * Walk entire list to find given tag.
 */
void *
dboot_multiboot2_find_tag(multiboot2_info_header_t *mbi, uint32_t tag)
{
	return (dboot_multiboot2_find_tag_impl(MB2_FIRST_TAG(mbi), tag));
}

/*
 * dboot_multiboot2_iterate()
 *
 * While most tags in tag list are unique, the modules are specified
 * one module per tag and therefore we need an mechanism to process
 * tags in set.
 *
 * Arguments:
 *	mbi: multiboot info header
 *	data: callback context.
 *
 * Return value:
 *	Processed item count.
 * Callback returning B_TRUE will terminate the iteration.
 */
int
dboot_multiboot2_iterate(multiboot2_info_header_t *mbi, void *data)
{
	struct dboot_multiboot2_iterate_ctx *ctx = data;
	multiboot_tag_t *tagp;
	uint32_t tag = ctx->tag;
	int index = 0;

	tagp = dboot_multiboot2_find_tag(mbi, tag);
	while (tagp != NULL) {
		if (ctx->callback != NULL) {
			if (ctx->callback(index, tagp, data) == B_TRUE) {
				return (index + 1);
			}
		}
		tagp = dboot_multiboot2_find_tag_impl(MB2_NEXT_TAG(tagp), tag);
		index++;
	}
	return (index);
}

char *
dboot_multiboot2_cmdline(multiboot2_info_header_t *mbi)
{
	multiboot_tag_string_t *tag;

	tag = dboot_multiboot2_find_tag(mbi, MULTIBOOT_TAG_TYPE_CMDLINE);

	if (tag != NULL)
		return (&tag->string[0]);
	else
		return (NULL);
}

/*
 * Simple callback to index item in set.
 * Terminates iteration if the indexed item is found.
 */
static boolean_t dboot_multiboot2_iterate_callback(int index,
    multiboot_tag_t *tagp, void *data)
{
	struct dboot_multiboot2_iterate_ctx *ctx = data;

	if (index == ctx->index) {
		ctx->tagp = tagp;
		return (B_TRUE);
	}
	return (B_FALSE);
}

int
dboot_multiboot2_modcount(multiboot2_info_header_t *mbi)
{
	struct dboot_multiboot2_iterate_ctx ctx = {
		.callback = NULL,
		.index = 0,
		.tag = MULTIBOOT_TAG_TYPE_MODULE,
		.tagp = NULL
	};

	return (dboot_multiboot2_iterate(mbi, &ctx));
}

uint32_t
dboot_multiboot2_modstart(multiboot2_info_header_t *mbi, int index)
{
	multiboot_tag_module_t *tagp;
	struct dboot_multiboot2_iterate_ctx ctx = {
		.callback = dboot_multiboot2_iterate_callback,
		.index = index,
		.tag = MULTIBOOT_TAG_TYPE_MODULE,
		.tagp = NULL
	};

	if (dboot_multiboot2_iterate(mbi, &ctx) != 0) {
		tagp = (multiboot_tag_module_t *) ctx.tagp;

		if (tagp != NULL)
			return (tagp->mod_start);
	}
	return (0);
}

uint32_t
dboot_multiboot2_modend(multiboot2_info_header_t *mbi, int index)
{
	multiboot_tag_module_t *tagp;
	struct dboot_multiboot2_iterate_ctx ctx = {
		.callback = dboot_multiboot2_iterate_callback,
		.index = index,
		.tag = MULTIBOOT_TAG_TYPE_MODULE,
		.tagp = NULL
	};

	if (dboot_multiboot2_iterate(mbi, &ctx) != 0) {
		tagp = (multiboot_tag_module_t *) ctx.tagp;

		if (tagp != NULL)
			return (tagp->mod_end);
	}
	return (0);
}

char *
dboot_multiboot2_modcmdline(multiboot2_info_header_t *mbi, int index)
{
	multiboot_tag_module_t *tagp;
	struct dboot_multiboot2_iterate_ctx ctx = {
		.callback = dboot_multiboot2_iterate_callback,
		.index = index,
		.tag = MULTIBOOT_TAG_TYPE_MODULE,
		.tagp = NULL
	};

	if (dboot_multiboot2_iterate(mbi, &ctx) != 0) {
		tagp = (multiboot_tag_module_t *) ctx.tagp;

		if (tagp != NULL)
			return (&tagp->cmdline[0]);
	}
	return (NULL);
}

multiboot_tag_mmap_t *
dboot_multiboot2_get_mmap_tagp(multiboot2_info_header_t *mbi)
{
	return (dboot_multiboot2_find_tag(mbi, MULTIBOOT_TAG_TYPE_MMAP));
}

int
dboot_multiboot2_basicmeminfo(multiboot2_info_header_t *mbi,
    uint32_t *lower, uint32_t *upper)
{
	multiboot_tag_basic_meminfo_t *mip;

	mip = dboot_multiboot2_find_tag(mbi, MULTIBOOT_TAG_TYPE_BASIC_MEMINFO);
	if (mip != NULL) {
		*lower = mip->mem_lower;
		*upper = mip->mem_upper;
		return (1);
	} else
		return (0);
}

uint32_t
dboot_multiboot2_mmap_get_type(multiboot2_info_header_t *mbi,
    multiboot_tag_mmap_t *mb2_mmap_tagp, int index)
{
	multiboot_mmap_entry_t *mapentp;

	if (mb2_mmap_tagp == NULL)
		mb2_mmap_tagp = dboot_multiboot2_get_mmap_tagp(mbi);

	if (mb2_mmap_tagp != NULL) {
		mapentp = &mb2_mmap_tagp->entries[index];
		return (mapentp->type);
	} else
		return (0);
}

uint64_t
dboot_multiboot2_mmap_get_length(multiboot2_info_header_t *mbi,
    multiboot_tag_mmap_t *mb2_mmap_tagp, int index)
{
	multiboot_mmap_entry_t *mapentp;

	if (mb2_mmap_tagp == NULL)
		mb2_mmap_tagp = dboot_multiboot2_get_mmap_tagp(mbi);

	if (mb2_mmap_tagp != NULL) {
		mapentp = &mb2_mmap_tagp->entries[index];
		return (mapentp->len);
	} else
		return (0);
}

uint64_t
dboot_multiboot2_mmap_get_base(multiboot2_info_header_t *mbi,
    multiboot_tag_mmap_t *mb2_mmap_tagp, int index)
{
	multiboot_mmap_entry_t *mapentp;

	if (mb2_mmap_tagp == NULL)
		mb2_mmap_tagp = dboot_multiboot2_get_mmap_tagp(mbi);

	if (mb2_mmap_tagp != NULL) {
		mapentp = &mb2_mmap_tagp->entries[index];
		return (mapentp->addr);
	} else
		return (0);
}

int
dboot_multiboot2_mmap_entries(multiboot2_info_header_t *mbi,
    multiboot_tag_mmap_t *mb2_mmap_tagp)
{
	if (mb2_mmap_tagp == NULL)
		mb2_mmap_tagp = dboot_multiboot2_get_mmap_tagp(mbi);

	if (mb2_mmap_tagp != NULL)
		return ((mb2_mmap_tagp->size -
		    offsetof(multiboot_tag_mmap_t, entries)) /
		    mb2_mmap_tagp->entry_size);
	else
		return (0);
}

paddr_t
dboot_multiboot2_highest_addr(multiboot2_info_header_t *mbi)
{
	return ((paddr_t)(uintptr_t)mbi + mbi->total_size);
}
