/*-
 * Copyright (c) 2013 The FreeBSD Foundation
 * All rights reserved.
 *
 * This software was developed by Benno Rice under sponsorship from
 * the FreeBSD Foundation.
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
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * $FreeBSD$
 */

#ifndef	_LOADER_EFI_COPY_H_
#define	_LOADER_EFI_COPY_H_

#include <stand.h>
#include <efi.h>
#include <efilib.h>
#include <sys/multiboot2.h>
#include <sys/queue.h>

struct chunk {
	EFI_VIRTUAL_ADDRESS vaddr;
	EFI_PHYSICAL_ADDRESS paddr;
	size_t size;
	STAILQ_ENTRY(chunk) next;
};

struct relocator {
	vm_offset_t stack;
	vm_offset_t copy;
	vm_offset_t memmove;
	struct chunk *stqh_first;
	struct chunk **stqh_last;
	struct chunk chunklist[1];
};

int	efi_autoload(void);

int	efi_getdev(void **vdev, const char *devspec, const char **path);
char	*efi_fmtdev(void *vdev);
int	efi_setcurrdev(struct env_var *ev, int flags, const void *value);

ssize_t	efi_copyin(const void *src, vm_offset_t dest, const size_t len);
ssize_t	efi_copyout(const vm_offset_t src, void *dest, const size_t len);
ssize_t	efi_readin(const int fd, vm_offset_t dest, const size_t len);
vm_offset_t efi_loadaddr(u_int type, void *data, vm_offset_t addr);
void efi_free_loadaddr(vm_offset_t addr, size_t pages);
void * efi_translate(vm_offset_t ptr);
void bi_isadir(void);

multiboot2_info_header_t *efi_copy_finish(struct relocator *);

#endif	/* _LOADER_EFI_COPY_H_ */
