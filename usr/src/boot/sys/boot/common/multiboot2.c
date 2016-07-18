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
 * This module adds support for loading and booting illumos multiboot2
 * kernel. This code is only built to support illumos kernel, as available
 * xen is supporting multiboot1.
 * The assumed module order is multiboot2 followed by multiboot1, allowing
 * fallback for multiboot1 only kernel.
 */
#include <sys/cdefs.h>

#include <sys/param.h>
#include <sys/exec.h>
#include <sys/linker.h>
#include <sys/module.h>
#include <sys/stdint.h>
#include <sys/multiboot2.h>
#include <stand.h>
#include "libzfs.h"

#include "bootstrap.h"

#include <machine/metadata.h>
#include <machine/pc/bios.h>

#if !defined(EFI)
#include "../i386/libi386/libi386.h"
#include "../i386/btx/lib/btxv86.h"
#include "pxe.h"

extern BOOTPLAYER bootplayer;	/* dhcp info */
extern void multiboot_tramp();
#else
#include <efi.h>
#include <efilib.h>
#include "loader_efi.h"

extern int efi_getdev(void **vdev, const char *devspec, const char **path);
extern void multiboot_tramp(uint32_t magic, struct relocator *mbi,
    uint64_t entry);
extern void efi_addsmapdata(struct preloaded_file *kfp);
static void (*trampoline)(uint32_t magic, struct relocator *relocator,
    uint64_t entry);
#endif

#include "platform/acfreebsd.h"
#include "acconfig.h"
#define ACPI_SYSTEM_XFACE
#include "actypes.h"
#include "actbl.h"

extern ACPI_TABLE_RSDP *rsdp;

/* MB data heap pointer */
static vm_offset_t last_addr;
extern char bootprog_name[];

static int multiboot2_loadfile(char *, u_int64_t, struct preloaded_file **);
static int multiboot2_exec(struct preloaded_file *);

struct file_format multiboot2 = { multiboot2_loadfile, multiboot2_exec };
static int keep_bs;
static vm_offset_t load_addr;
static vm_offset_t entry_addr;

/* validate tags in info request */
static int
is_info_request_valid(multiboot_header_tag_information_request_t *rtag)
{
	int i;

	/* don't really care about optional tag */
	if (rtag->flags & MULTIBOOT_HEADER_TAG_OPTIONAL)
		return (1);

	for (i = 0; i < (rtag->size - sizeof (*rtag)) /
	    sizeof (rtag->requests[0]); i++)
		switch (rtag->requests[i]) {
		case MULTIBOOT_TAG_TYPE_END:
		case MULTIBOOT_TAG_TYPE_CMDLINE:
		case MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME:
		case MULTIBOOT_TAG_TYPE_MODULE:
		case MULTIBOOT_TAG_TYPE_BASIC_MEMINFO:
		case MULTIBOOT_TAG_TYPE_BOOTDEV:
		case MULTIBOOT_TAG_TYPE_MMAP:
		case MULTIBOOT_TAG_TYPE_FRAMEBUFFER:
		case MULTIBOOT_TAG_TYPE_VBE:
		case MULTIBOOT_TAG_TYPE_ELF_SECTIONS:
		case MULTIBOOT_TAG_TYPE_APM:
		case MULTIBOOT_TAG_TYPE_EFI32:
		case MULTIBOOT_TAG_TYPE_EFI64:
		case MULTIBOOT_TAG_TYPE_ACPI_OLD:
		case MULTIBOOT_TAG_TYPE_ACPI_NEW:
		case MULTIBOOT_TAG_TYPE_NETWORK:
		case MULTIBOOT_TAG_TYPE_EFI_MMAP:
		case MULTIBOOT_TAG_TYPE_EFI_BS:
			break;
		default:
			printf("unsupported information tag: 0x%x\n",
			    rtag->requests[i]);
			return (0);
		}
	return (1);
}

static int
multiboot2_loadfile(char *filename, u_int64_t dest,
    struct preloaded_file **result)
{
	int i, fd, error;
	struct stat st;
	caddr_t header_search;
	ssize_t search_size;
	multiboot2_header_t *header;
	multiboot_header_tag_t *tag;
	multiboot_header_tag_address_t *addr_tag = NULL;
	multiboot_header_tag_entry_address_t *entry_tag = NULL;
	multiboot_header_tag_framebuffer_t *fb_tag = NULL;
	struct preloaded_file *fp;

	/* this allows to check other methods */
	error = EFTYPE;
	if (filename == NULL)
		return (error);

	/* is kernel already loaded? */
	fp = file_findfile(NULL, NULL);
	if (fp != NULL)
		return (error);

	if ((fd = open(filename, O_RDONLY)) == -1)
		return (errno);

	/*
	 * Read MULTIBOOT_SEARCH size in order to search for the
	 * multiboot magic header.
	 */
	header_search = malloc(MULTIBOOT_SEARCH);
	if (header_search == NULL) {
		close(fd);
		return (ENOMEM);
	}

	search_size = read(fd, header_search, MULTIBOOT_SEARCH);
	if (search_size != MULTIBOOT_SEARCH)
		goto out;

	header = NULL;
	search_size = 0;
	for (i = 0; i < (MULTIBOOT_SEARCH - sizeof(multiboot2_header_t));
	    i += MULTIBOOT_HEADER_ALIGN) {
		header = (multiboot2_header_t *)(header_search + i);
		search_size = header->header_length;
		if (header->magic == MULTIBOOT2_HEADER_MAGIC)
			break;
	}

	if (search_size == 0 || i + search_size > MULTIBOOT_SEARCH)
		goto out;

	/* Valid multiboot header has been found, validate checksum */
	if (header->magic + header->architecture + header->header_length +
	    header->checksum != 0) {
		printf("Multiboot2 checksum failed, magic: 0x%x "
		    "architecture: 0x%x header_length: 0x%x checksum: 0x%x\n",
		    header->magic, header->architecture,
		    header->header_length, header->checksum);
		goto out;
	}

	for (tag = header->tags; tag->type != MULTIBOOT_TAG_TYPE_END;
	    tag = (multiboot_header_tag_t *)
	    ((uint32_t *)tag + roundup(tag->size, MULTIBOOT_TAG_ALIGN) / 4))
		switch (tag->type) {
		case MULTIBOOT_HEADER_TAG_INFORMATION_REQUEST:
			if (!is_info_request_valid((void*)tag))
				goto out;
		break;
		case MULTIBOOT_HEADER_TAG_ADDRESS:
			addr_tag = (multiboot_header_tag_address_t *) tag;
		break;
		case MULTIBOOT_HEADER_TAG_ENTRY_ADDRESS:
			entry_tag =
			    (multiboot_header_tag_entry_address_t *) tag;
		break;
		case MULTIBOOT_HEADER_TAG_CONSOLE_FLAGS:
		break;
		case MULTIBOOT_HEADER_TAG_FRAMEBUFFER:
			fb_tag = (multiboot_header_tag_framebuffer_t *) tag;
		break;
		case MULTIBOOT_HEADER_TAG_MODULE_ALIGN:
			/* we always align modules */
		break;
		case MULTIBOOT_HEADER_TAG_EFI_BS:
			keep_bs = 1;
		break;
		default:
			if (! (tag->flags & MULTIBOOT_HEADER_TAG_OPTIONAL)) {
				printf("unsupported tag: 0x%x\n", tag->type);
				goto out;
			}
		}

	/*
	 * if we are missing data, let next module to have chance.
	 */
	if (addr_tag == NULL)
		goto out;

	if (addr_tag != NULL && entry_tag != NULL) {
		fp = file_alloc();
		if (fp == NULL) {
			error = ENOMEM;
			goto out;
		}
		if (lseek(fd, 0, SEEK_SET) == -1) {
			printf("lseek failed\n");
			error = EIO;
			file_discard(fp);
			goto out;
		}
		if (fstat(fd, &st) < 0) {
			printf("lseek failed\n");
			error = EIO;
			file_discard(fp);
			goto out;
		}

		load_addr = addr_tag->load_addr;
		entry_addr = entry_tag->entry_addr;
		fp->f_addr = archsw.arch_loadaddr(LOAD_KERN, filename,
		    addr_tag->load_addr);
		if (fp->f_addr == 0) {
			error = ENOMEM;
			file_discard(fp);
			goto out;
		}
		fp->f_size = archsw.arch_readin(fd, fp->f_addr, st.st_size);

		if (fp->f_size != st.st_size) {
			printf("error reading: %s", strerror(errno));
			file_discard(fp);
			error = EIO;
			goto out;
		}

		fp->f_name = strdup(filename);
		fp->f_type = strdup("aout multiboot2 kernel");
		fp->f_metadata = NULL;
#if defined(EFI)
		efi_addsmapdata(fp);
#else
		bios_addsmapdata(fp);
#endif
		*result = fp;
		setenv("kernelname", fp->f_name, 1);
		error = 0;
	}

out:
	free(header_search);
	close(fd);
	return (error);
}

/*
 * Since for now we have no way to pass the environment to the kernel other than
 * through arguments, we need to take care of console setup.
 *
 * If the console is in mirror mode, set the kernel console from $os_console.
 * If it's unset, use first item from $console.
 * If $console is "ttyX", also pass $ttyX-mode, since it may have been set by
 * the user.
 *
 * In case of memory allocation errors, just return original command line,
 * so we have chance of booting.
 *
 * On success, cl will be freed and a new, allocated command line string is
 * returned.
 */
static char *
update_cmdline(char *cl)
{
	char *os_console = getenv("os_console");
	char *ttymode = NULL;
	char mode[10];
	char *tmp;
	int len;

	if (os_console == NULL) {
		tmp = strdup(getenv("console"));
		os_console = strsep(&tmp, ", ");
	} else
		os_console = strdup(os_console);

	if (os_console == NULL)
		return (cl);

	if (strncmp(os_console, "tty", 3) == 0) {
		snprintf(mode, sizeof (mode), "%s-mode", os_console);
		ttymode = getenv(mode);	/* never NULL */
	}

	if (strstr(cl, "-B") != NULL) {
		len = strlen(cl) + 1;
		/*
		 * if console is not present, add it
		 * if console is ttyX, add ttymode
		 */
		tmp = strstr(cl, "console");
		if (tmp == NULL) {
			len += 12;	/* " -B console=" */
			len += strlen(os_console);
			if (ttymode != NULL) {
				len += 13;	/* ",ttyX-mode=\"\"" */
				len += strlen(ttymode);
			}
			tmp = malloc(len);
			if (tmp == NULL) {
				free(os_console);
				return (cl);
			}
			if (ttymode != NULL)
				sprintf(tmp,
				    "%s -B console=%s,%s-mode=\"%s\"",
				    cl, os_console, os_console, ttymode);
			else
				sprintf(tmp, "%s -B console=%s",
				    cl, os_console);
		} else {
			/* console is set, do we need tty mode? */
			tmp += 8;
			if (strstr(tmp, "tty") == tmp) {
				strncpy(mode, tmp, 4);
				mode[4] = '\0';
				strcat(mode, "-mode");
				ttymode = getenv(mode);	/* never NULL */
			} else { /* nope */
				free(os_console);
				return (cl);
			}
			len = strlen(cl) + 1;
			len += 13;	/* ",ttyX-mode=\"\"" */
			len += strlen(ttymode);
			tmp = malloc(len);
			if (tmp == NULL) {
				free(os_console);
				return (cl);
			}
			sprintf(tmp, "%s,%s=\"%s\"", cl, mode, ttymode);
		}
	} else {
		/*
		 * no -B, so we need to add " -B console=%s[,ttyX-mode=\"%s\"]"
		 */
		len = strlen(cl) + 1;
		len += 12;		/* " -B console=" */
		len += strlen(os_console);
		if (ttymode != NULL) {
			len += 13;	/* ",ttyX-mode=\"\"" */
			len += strlen(ttymode);
		}
		tmp = malloc(len);
		if (tmp == NULL) {
			free(os_console);
			return (cl);
		}
		if (ttymode != NULL)
			sprintf(tmp, "%s -B console=%s,%s-mode=\"%s\"", cl,
			    os_console, os_console, ttymode);
		else
			sprintf(tmp, "%s -B console=%s", cl, os_console);
	}
	free(os_console);
	free(cl);
	return (tmp);
}

/*
 * build the kernel command line. Shared function between MB1 and MB2.
 */
char *
mb_kernel_cmdline(struct preloaded_file *fp, struct devdesc *rootdev)
{
	char *cmdline = NULL;
	size_t len;

	if (fp->f_args == NULL)
		fp->f_args = getenv("boot-args");

	len = strlen(fp->f_name) + 1;

	if (fp->f_args != NULL)
		len += strlen(fp->f_args) + 1;

	if (rootdev->d_type == DEVT_ZFS)
		len += 3 + strlen(zfs_bootfs(rootdev)) + 1;

	cmdline = malloc(len);
	if (cmdline == NULL)
		return (cmdline);

	if (rootdev->d_type == DEVT_ZFS) {
		if (fp->f_args != NULL)
			snprintf(cmdline, len, "%s %s -B %s", fp->f_name,
			    fp->f_args, zfs_bootfs(rootdev));
		else
			snprintf(cmdline, len, "%s -B %s", fp->f_name,
			    zfs_bootfs(rootdev));
	} else if (fp->f_args != NULL)
		snprintf(cmdline, len, "%s %s", fp->f_name, fp->f_args);
	else
		snprintf(cmdline, len, "%s", fp->f_name);

	return (update_cmdline(cmdline));
}

/*
 * returns allocated virtual address from MB info area
 */
static vm_offset_t
mb_malloc(size_t n)
{
	vm_offset_t ptr = last_addr;
	last_addr = roundup(last_addr + n, MULTIBOOT_TAG_ALIGN);
	return (ptr);
}

/* calculate size for module taglist */
static int
module_size(struct preloaded_file *fp)
{
	int len, size;
	struct preloaded_file *mfp;

	size = 0;
	for (mfp = fp->f_next; mfp != NULL; mfp = mfp->f_next) {
		len = strlen(mfp->f_name) + 1;
		len += strlen(mfp->f_type) + 5 + 1;
		if (mfp->f_args != NULL)
			len += strlen(mfp->f_args) + 1;
		size += sizeof (multiboot_tag_module_t) + len;
		size = roundup(size, MULTIBOOT_TAG_ALIGN);
	}
	return (size);
}

#if defined (EFI)
/* calculate size for UEFI memory map tag */
static int
efimemmap_size(void)
{
	UINTN size, cur_size, desc_size;
	EFI_MEMORY_DESCRIPTOR *mmap;
	EFI_STATUS ret;

	size = 1 << 12;		/* start with 4k */
	while (1) {
		cur_size = size;
		mmap = malloc(cur_size);
		if (mmap == NULL)
			return (0);
		ret = BS->GetMemoryMap(&cur_size, mmap, NULL, &desc_size, NULL);
		free(mmap);
		if (ret == EFI_SUCCESS)
			break;
		if (ret == EFI_BUFFER_TOO_SMALL) {
			if (size < cur_size)
				size = cur_size;
			size += (1 << 12);
		} else
			return (0);
	}

	/* EFI MMAP will grow when we allocate MBI, set some buffer */
	size += (3 << 12);
	size = roundup(size, desc_size);
	return (sizeof (multiboot_tag_efi_mmap_t) + size);
}
#endif

/* calculate size for bios smap tag */
static int
biossmap_size(struct preloaded_file *fp)
{
	int num;
	struct file_metadata *md;
	struct bios_smap *smap;

	md = file_findmetadata(fp, MODINFOMD_SMAP);
	if (md == NULL)
		return (0);

	smap = (struct bios_smap *)md->md_data;
	num = md->md_size / sizeof(struct bios_smap); /* number of entries */
	return (sizeof (multiboot_tag_mmap_t) +
	    num * sizeof (multiboot_mmap_entry_t));
}

static int
mbi_size(struct preloaded_file *fp, char *cmdline)
{
	int size;

	size = sizeof (uint32_t) * 2; /* first 2 fields from MBI header */
	size += sizeof (multiboot_tag_string_t) + strlen(cmdline) + 1;
	size = roundup2(size, MULTIBOOT_TAG_ALIGN);
	size += sizeof (multiboot_tag_string_t) + strlen(bootprog_name) + 1;
	size = roundup2(size, MULTIBOOT_TAG_ALIGN);
#if !defined (EFI)
	size += sizeof (multiboot_tag_basic_meminfo_t);
	size = roundup2(size, MULTIBOOT_TAG_ALIGN);
#endif
	size += module_size(fp);
	size = roundup2(size, MULTIBOOT_TAG_ALIGN);
#if defined (EFI)
	size += sizeof (multiboot_tag_efi64_t);
	size = roundup2(size, MULTIBOOT_TAG_ALIGN);
	size += efimemmap_size();

	size += sizeof (multiboot_tag_framebuffer_t);
	size = roundup2(size, MULTIBOOT_TAG_ALIGN);
#endif
	size += biossmap_size(fp);
	size = roundup2(size, MULTIBOOT_TAG_ALIGN);

#if !defined (EFI)
	if (strstr(getenv("loaddev"), "pxe") != NULL) {
		size += sizeof(multiboot_tag_network_t) + sizeof (BOOTPLAYER);
		size = roundup2(size, MULTIBOOT_TAG_ALIGN);
	}
#endif

	if (rsdp != NULL) {
		if (rsdp->Revision == 0)
			size += sizeof (multiboot_tag_old_acpi_t) +
			    sizeof(ACPI_RSDP_COMMON);
		else
			size += sizeof (multiboot_tag_new_acpi_t) +
			    rsdp->Length;
		size = roundup2(size, MULTIBOOT_TAG_ALIGN);
	}
	size += sizeof(multiboot_tag_t);

	return (size);
}

static int
multiboot2_exec(struct preloaded_file *fp)
{
	struct preloaded_file *mfp;
	multiboot2_info_header_t *mbi;
	char *cmdline = NULL;
	struct devdesc *rootdev;
	struct file_metadata *md;
	int i, error, num, size;
	int rootfs = 0;
	struct bios_smap *smap;
	vm_offset_t tmp;
#if defined (EFI)
	multiboot_tag_module_t *module;
	EFI_MEMORY_DESCRIPTOR *map;
	struct relocator *relocator;
	struct chunk *chunk;

	efi_getdev((void **)(&rootdev), NULL, NULL);
#else
	i386_getdev((void **)(&rootdev), NULL, NULL);
#endif

	error = EINVAL;
	if (rootdev == NULL) {
		printf("can't determine root device\n");
		goto error;
	}

	/*
	 * Set the image command line.
	 */
	if (fp->f_args == NULL) {
		cmdline = getenv("boot-args");
		if (cmdline != NULL) {
			fp->f_args = strdup(cmdline);
			if (fp->f_args == NULL) {
				error = ENOMEM;
				goto error;
			}
		}
	}

	cmdline = mb_kernel_cmdline(fp, rootdev);
	if (cmdline == NULL) {
		error = ENOMEM;
		goto error;
	}

	size = mbi_size(fp, cmdline);	/* get the size for MBI */

	/* set up base for mb_malloc */
	for (mfp = fp; mfp->f_next != NULL; mfp = mfp->f_next);

#if defined (EFI)
	last_addr = efi_loadaddr(LOAD_MEM, &size, mfp->f_addr + mfp->f_size);
	mbi = (multiboot2_info_header_t *)last_addr;
	if (mbi == NULL) {
		error = ENOMEM;
		goto error;
	}
	last_addr = (vm_offset_t)mbi->tags;
#else
	/* start info block from new page */
	last_addr = roundup(mfp->f_addr + mfp->f_size, MULTIBOOT_MOD_ALIGN);
	mbi = (multiboot2_info_header_t *)PTOV(last_addr);
	last_addr = (vm_offset_t)mbi->tags;
#endif

	{
		multiboot_tag_string_t *tag;
		i = sizeof (multiboot_tag_string_t) + strlen(cmdline) + 1;
		tag = (multiboot_tag_string_t *) mb_malloc(i);

		tag->type = MULTIBOOT_TAG_TYPE_CMDLINE;
		tag->size = i;
		memcpy(tag->string, cmdline, strlen(cmdline) + 1);
		free(cmdline);
		cmdline = NULL;
	}

	{
		multiboot_tag_string_t *tag;
		i = sizeof (multiboot_tag_string_t) + strlen(bootprog_name) + 1;
		tag = (multiboot_tag_string_t *) mb_malloc(i);

		tag->type = MULTIBOOT_TAG_TYPE_BOOT_LOADER_NAME;
		tag->size = i;
		memcpy(tag->string, bootprog_name, strlen(bootprog_name) + 1);
	}

#if !defined (EFI)
	/* Only set in case of BIOS */
	{
		multiboot_tag_basic_meminfo_t *tag;
		tag = (multiboot_tag_basic_meminfo_t *)
		    mb_malloc(sizeof (*tag));

		tag->type = MULTIBOOT_TAG_TYPE_BASIC_MEMINFO;
		tag->size = sizeof (*tag);
		tag->mem_lower = bios_basemem / 1024;
		tag->mem_upper = bios_extmem / 1024;
	}
#endif

	num = 0;
	for (mfp = fp->f_next; mfp != NULL; mfp = mfp->f_next) {
		num++;
		if (mfp->f_type != NULL && strcmp(mfp->f_type, "rootfs") == 0)
			rootfs++;
	}

	if (num == 0 || rootfs == 0) {
		/* need at least one module - rootfs */
		printf("No rootfs module provided, aborting\n");
		error = EINVAL;
		goto error;
	}

	/*
	 * set the stage for physical memory layout:
	 * kernel load_addr
	 * modules aligned to page boundary
	 * MBI aligned to page boundary
	 * so the tmp points now on first module physical address.
	 * tmp != mfp->f_addr only in case of EFI.
	 */
	tmp = roundup2(load_addr + fp->f_size, MULTIBOOT_MOD_ALIGN);
#if defined (EFI)
	module = (multiboot_tag_module_t *)last_addr;
#endif

	for (mfp = fp->f_next; mfp != NULL; mfp = mfp->f_next) {
		multiboot_tag_module_t *tag;

		num = strlen(mfp->f_name) + 1;
		num += strlen(mfp->f_type) + 5 + 1;
		if (mfp->f_args != NULL) {
			num += strlen(mfp->f_args) + 1;
		}
		cmdline = malloc(num);
		if (cmdline == NULL) {
			error = ENOMEM;
			goto error;
		}

		if (mfp->f_args != NULL)
			snprintf(cmdline, num, "%s type=%s %s",
			    mfp->f_name, mfp->f_type, mfp->f_args);
		else
			snprintf(cmdline, num, "%s type=%s",
			    mfp->f_name, mfp->f_type);

		tag = (multiboot_tag_module_t *)mb_malloc(sizeof (*tag) + num);

		tag->type = MULTIBOOT_TAG_TYPE_MODULE;
		tag->size = sizeof (*tag) + num;
		tag->mod_start = tmp;
		tag->mod_end = tmp + mfp->f_size;
		tmp = roundup2(tag->mod_end, MULTIBOOT_MOD_ALIGN);
		memcpy(tag->cmdline, cmdline, num);
		free(cmdline);
		cmdline = NULL;
	}

	md = file_findmetadata(fp, MODINFOMD_SMAP);
	if (md == NULL) {
		printf("no memory smap\n");
		error = EINVAL;
		goto error;
	}

	smap = (struct bios_smap *)md->md_data;
	num = md->md_size / sizeof(struct bios_smap); /* number of entries */

	{
		multiboot_tag_mmap_t *tag;
		multiboot_mmap_entry_t *mmap_entry;

		tag = (multiboot_tag_mmap_t *)
		    mb_malloc(sizeof (*tag) +
		    num * sizeof (multiboot_mmap_entry_t));

		tag->type = MULTIBOOT_TAG_TYPE_MMAP;
		tag->size = sizeof (*tag) +
		    num * sizeof (multiboot_mmap_entry_t);
		tag->entry_size = sizeof (multiboot_mmap_entry_t);
		tag->entry_version = 0;
		mmap_entry = tag->entries;

		for (i = 0; i < num; i++) {
			mmap_entry[i].addr = smap[i].base;
			mmap_entry[i].len = smap[i].length;
			mmap_entry[i].type = smap[i].type;
			mmap_entry[i].zero = 0;
		}
	}

#if !defined (EFI)
	if (strstr(getenv("loaddev"), "pxe") != NULL) {
		multiboot_tag_network_t *tag;
		tag = (multiboot_tag_network_t *)
		    mb_malloc(sizeof(*tag) + sizeof (BOOTPLAYER));

		tag->type = MULTIBOOT_TAG_TYPE_NETWORK;
		tag->size = sizeof(*tag) + sizeof (BOOTPLAYER);
		memcpy(tag->dhcpack, &bootplayer, sizeof (BOOTPLAYER));
	}
#endif

	if (rsdp != NULL) {
		multiboot_tag_new_acpi_t *ntag;
		multiboot_tag_old_acpi_t *otag;
		int size;

		if (rsdp->Revision == 0) {
			size = sizeof (*otag) + rsdp->Length;
			otag = (multiboot_tag_old_acpi_t *)mb_malloc(size);
			otag->type = MULTIBOOT_TAG_TYPE_ACPI_OLD;
			otag->size = size;
			memcpy(otag->rsdp, rsdp, sizeof (ACPI_RSDP_COMMON));
		} else {
			size = sizeof (*ntag) + rsdp->Length;
			ntag = (multiboot_tag_new_acpi_t *)mb_malloc(size);
			ntag->type = MULTIBOOT_TAG_TYPE_ACPI_NEW;
			ntag->size = size;
			memcpy(ntag->rsdp, rsdp, rsdp->Length);
		}
	}

#if defined (EFI)
#ifdef  __LP64__
	{
		multiboot_tag_efi64_t *tag;
		tag = (multiboot_tag_efi64_t *)
		    mb_malloc(sizeof (*tag));

		tag->type = MULTIBOOT_TAG_TYPE_EFI64;
		tag->size = sizeof (*tag);
		tag->pointer = (uint64_t)ST;
	}
#else
	{
		multiboot_tag_efi32_t *tag;
		tag = (multiboot_tag_efi32_t *)
		    mb_malloc(sizeof (*tag));

		tag->type = MULTIBOOT_TAG_TYPE_EFI32;
		tag->size = sizeof (*tag);
		tag->pointer = (uint32_t)ST;
	}
#endif /* __LP64__ */

	{
		multiboot_tag_framebuffer_t *tag;
		int bpp;
		struct efi_fb fb;
		extern int efi_find_framebuffer(struct efi_fb *efifb);

		if (efi_find_framebuffer(&fb) == 0) {
			tag = (multiboot_tag_framebuffer_t *)
			    mb_malloc(sizeof (*tag));
			bpp = fls(fb.fb_mask_red | fb.fb_mask_green |
			    fb.fb_mask_blue | fb.fb_mask_reserved);

			tag->common.type = MULTIBOOT_TAG_TYPE_FRAMEBUFFER;
			tag->common.size = sizeof (multiboot_tag_framebuffer_t);
			tag->common.framebuffer_addr = fb.fb_addr;
			tag->common.framebuffer_width = fb.fb_width;
			tag->common.framebuffer_height = fb.fb_height;
			tag->common.framebuffer_bpp = bpp;
			tag->common.framebuffer_pitch =
			    fb.fb_stride * (bpp / 8);
			tag->common.framebuffer_type =
			    MULTIBOOT_FRAMEBUFFER_TYPE_RGB;
			tag->common.reserved = 0;
			if (fb.fb_mask_red & 0x000000ff) {
				tag->u.fb2.framebuffer_red_field_position = 0;
				tag->u.fb2.framebuffer_blue_field_position = 16;
			} else {
				tag->u.fb2.framebuffer_red_field_position = 16;
				tag->u.fb2.framebuffer_blue_field_position = 0;
			}
			tag->u.fb2.framebuffer_red_mask_size = 8;
			tag->u.fb2.framebuffer_green_field_position = 8;
			tag->u.fb2.framebuffer_green_mask_size = 8;
			tag->u.fb2.framebuffer_blue_mask_size = 8;
		}
	}

	/* Leave EFI memmap last as we will also switch off BS */
	{
		multiboot_tag_efi_mmap_t *tag;
		UINTN size, desc_size, key;
		EFI_STATUS status;

		tag = (multiboot_tag_efi_mmap_t *)
		    mb_malloc(sizeof (*tag));

		size = 0;
		status = BS->GetMemoryMap(&size,
		    (EFI_MEMORY_DESCRIPTOR *)tag->efi_mmap, &key,
		    &desc_size, &tag->descr_vers);
		if (status != EFI_BUFFER_TOO_SMALL) {
			error = EINVAL;
			goto error;
		}
		status = BS->GetMemoryMap(&size,
		    (EFI_MEMORY_DESCRIPTOR *)tag->efi_mmap, &key,
		    &desc_size, &tag->descr_vers);
		if (EFI_ERROR(status)) {
			error = EINVAL;
			goto error;
		}
		tag->type = MULTIBOOT_TAG_TYPE_EFI_MMAP;
		tag->size = sizeof (*tag) + size;
		tag->descr_size = (uint32_t) desc_size;

		/*
		 * UEFI32 in qemu and vbox has issue that memory
		 * data is stored in high part of the uint64 field.
		 * Since Attribute can not be 0, set shift value based on
		 * following test by 32 bits.
		 */
		map = (EFI_MEMORY_DESCRIPTOR *)tag->efi_mmap;
		if ((map->Attribute & 0xffffffff) == 0) {
			for (i = 0; i < size / desc_size;
			    i++, map = NextMemoryDescriptor(map, desc_size)) {
				map->PhysicalStart >>= 32;
				map->VirtualStart >>= 32;
				map->NumberOfPages >>= 32;
				map->Attribute >>= 32;
			}
			map = (EFI_MEMORY_DESCRIPTOR *)tag->efi_mmap;
		}

		/*
		 * Find relocater pages. We assume we have free pages
		 * below kernel load address.
		 * In this version we are using 5 pages:
		 * relocator data, trampoline, copy, memmove, stack
		 */
		for (i = 0; i < size / desc_size;
		    i++, map = NextMemoryDescriptor(map, desc_size)) {
			if (map->PhysicalStart == 0)
				continue;
			if (map->Type != EfiConventionalMemory)
				continue;
			if (map->PhysicalStart < load_addr &&
			    map->NumberOfPages > 5)
				break;
		}

		if (keep_bs == 0)
			status = BS->ExitBootServices(IH, key);

		last_addr += size;
		last_addr = roundup2(last_addr, MULTIBOOT_TAG_ALIGN);
	}
#endif

	/*
	 * MB tag list end marker
	 */
	{
		multiboot_tag_t *tag = (multiboot_tag_t *)
		    mb_malloc(sizeof(*tag));
		tag->type = MULTIBOOT_TAG_TYPE_END;
		tag->size = sizeof(*tag);
	}

	mbi->total_size = last_addr - (vm_offset_t)mbi;
	mbi->reserved = 0;

#if defined (EFI)
	/* at this point we have load_addr pointing to kernel load
	 * address, module list in MBI having physical addresses,
	 * module list in fp having logical addresses and tmp pointing to
	 * physical address for MBI
	 * now we must move all pieces to place and start the kernel.
	 */
	relocator = (struct relocator *)(uintptr_t)map->PhysicalStart;
	relocator->stqh_first = NULL;
	relocator->stqh_last = &relocator->stqh_first;

	i = 0;
	chunk = &relocator->chunklist[i++];
	chunk->vaddr = fp->f_addr;
	chunk->paddr = load_addr;
	chunk->size = fp->f_size;

	STAILQ_INSERT_TAIL(relocator, chunk, next);

	for (mfp = fp->f_next; mfp != NULL; mfp = mfp->f_next) {
		chunk = &relocator->chunklist[i++];
		chunk->vaddr = mfp->f_addr;
		chunk->paddr = module->mod_start;
		chunk->size = mfp->f_size;
		STAILQ_INSERT_TAIL(relocator, chunk, next);

		module = (multiboot_tag_module_t *)
		    roundup2((uintptr_t)module + module->size,
		    MULTIBOOT_TAG_ALIGN);
	}
	chunk = &relocator->chunklist[i++];
	chunk->vaddr = (EFI_VIRTUAL_ADDRESS)(uintptr_t)mbi;
	chunk->paddr = tmp;
	chunk->size = mbi->total_size;
	STAILQ_INSERT_TAIL(relocator, chunk, next);

	trampoline = (void *)(uintptr_t)relocator + EFI_PAGE_SIZE;
	memmove(trampoline, multiboot_tramp, EFI_PAGE_SIZE);

	relocator->copy = (uintptr_t)trampoline + EFI_PAGE_SIZE;
	memmove((void *)relocator->copy, efi_copy_finish, EFI_PAGE_SIZE);

	relocator->memmove = (uintptr_t)relocator->copy + EFI_PAGE_SIZE;
	memmove((void *)relocator->memmove, memmove, EFI_PAGE_SIZE);
	relocator->stack = relocator->memmove + EFI_PAGE_SIZE - 8;

	trampoline(MULTIBOOT2_BOOTLOADER_MAGIC, relocator, entry_addr);
#else
	dev_cleanup();
	__exec((void *)VTOP(multiboot_tramp), MULTIBOOT2_BOOTLOADER_MAGIC,
	    (void *)entry_addr, (void *)VTOP(mbi));
#endif
	panic("exec returned");

error:
	if (cmdline != NULL)
		free(cmdline);
#if defined (EFI)
	if (mbi != NULL)
		efi_free_loadaddr((vm_offset_t)mbi, size >> 12);
#endif
	return (error);
}
