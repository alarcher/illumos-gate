#
# This file and its contents are supplied under the terms of the
# Common Development and Distribution License ("CDDL"), version 1.0.
# You may only use this file in accordance with the terms of version
# 1.0 of the CDDL.
#
# A full copy of the text of the CDDL should have accompanied this
# source.  A copy of the CDDL is also available via the Internet at
# http://www.illumos.org/license/CDDL.
#

#
# Copyright 2016 Toomas Soome <tsoome@me.com>
# Copyright 2016 RackTop Systems.
#

include $(SRC)/Makefile.master

CC=	$(GCC_ROOT)/bin/gcc
LD=	$(GNU_ROOT)/bin/gld
OBJCOPY= $(GNU_ROOT)/bin/gobjcopy
OBJDUMP= $(GNU_ROOT)/bin/gobjdump
OBJCOPY= /code/binutils-gdb/binutils/objcopy
OBJDUMP= /code/binutils-gdb/binutils/objdump

PROG=		boot1.sym
MACHINE=$(MACH64)

# need to update this to trigger installboot updates.
BUILDDATE :sh = TZ=UTC date +%Y%m%dT%H%M%SZ
BOOT1_VERSION=	1.1:$(BUILDDATE)

# architecture-specific loader code
SRCS=	boot1.c self_reloc.c start.S ufs_module.c zfs_module.c devopen.c
OBJS=	boot1.o self_reloc.o start.o ufs_module.o zfs_module.o devopen.o

ASFLAGS=-m64 -fPIC
CFLAGS= -O2
CPPFLAGS=	-nostdinc
CPPFLAGS +=	-I.
CPPFLAGS +=	-I./../include
CPPFLAGS +=	-I./../include/${MACHINE}
CPPFLAGS +=	-I./../../../contrib/dev/acpica/include
CPPFLAGS +=	-I./../../../../include
CPPFLAGS +=	-I./../../../sys
CPPFLAGS +=	-I./../../..
CPPFLAGS +=	-I../../../../lib/libstand
CPPFLAGS +=	-DEFI_UFS_BOOT
# CPPFLAGS +=	-DEFI_DEBUG

CPPFLAGS +=	-I./../../zfs/
CPPFLAGS +=	-I./../../../cddl/boot/zfs/
CPPFLAGS +=	-DEFI_ZFS_BOOT

# Always add MI sources and REGULAR efi loader bits
CPPFLAGS +=	-I./../../common

include ../Makefile.inc

FILES=  boot1.efi
FILEMODE=	0555
ROOT_BOOT=	$(ROOT)/boot
ROOTBOOTFILES=$(FILES:%=$(ROOT_BOOT)/%)

LDSCRIPT=	./../loader/arch/${MACHINE}/ldscript.${MACHINE}
LDFLAGS=	-nostdlib --eh-frame-hdr -znocombreloc
LDFLAGS +=	-shared --hash-style=both --enable-new-dtags
LDFLAGS +=	-T${LDSCRIPT} -Bsymbolic

all: boot1.efi

install: all $(ROOTBOOTFILES)

include $(SRC)/Makefile.master

$(OBJS): machine x86

clean clobber:
	$(RM) $(CLEANFILES) $(OBJS)

all	:=	TARGET = all
clean	:=	TARGET = clean
clobber	:=	TARGET = clobber
install	:=	TARGET = install

all clean clobber install: $(SUBDIRS)

$(SUBDIRS): FRC
	@cd $@; pwd; $(MAKE) $(MFLAGS) $(TARGET)

FRC:
