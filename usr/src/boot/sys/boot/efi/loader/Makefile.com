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
#

include $(SRC)/Makefile.master

CC=		$(GCC_ROOT)/bin/gcc
LD=		$(GNU_ROOT)/bin/gld
OBJCOPY=	$(GNU_ROOT)/bin/gobjcopy
OBJDUMP=	$(GNU_ROOT)/bin/gobjdump

PROG=		loader.sym

VGATEXT_FONT	= 8x16
VGATEXT_FONT_SRC= 8859-1
VGATEXT_FONT_DIR= $(SRC)/uts/common/io/font

# architecture-specific loader code
SRCS=	autoload.c bootinfo.c conf.c copy.c devicename.c main.c self_reloc.c \
	smbios.c vers.c memmap.c multiboot2.c font.c 12x22.c 6x10.c 7x14.c \
	8x16.c list.c tem.c

OBJS= autoload.o bootinfo.o conf.o copy.o devicename.o main.o self_reloc.o \
	smbios.o vers.o memmap.o multiboot2.o font.o 12x22.o 6x10.o 7x14.o \
	8x16.o list.o tem.o

CFLAGS=	-Os
CPPFLAGS= -nostdinc -I../../../../../include -I../../..../
CPPFLAGS += -I../../../../../lib/libstand
CPPFLAGS += -I../../../../../lib/libz

include ../../Makefile.inc

include ../arch/${MACHINE}/Makefile.inc

CPPFLAGS +=	-I. -I..
CPPFLAGS +=	-I../arch/common
CPPFLAGS +=	-I../../include
CPPFLAGS +=	-I../../include/${MACHINE}
CPPFLAGS +=	-I../../../..
CPPFLAGS +=	-I../../../i386/libi386
CPPFLAGS +=	-I../../../zfs
CPPFLAGS +=	-I../../../../cddl/boot/zfs
CPPFLAGS +=	-I../../../../contrib/dev/acpica/include
CPPFLAGS +=	-DEFI_ZFS_BOOT
CPPFLAGS +=	-DNO_PCI -DEFI

# Export serial numbers, UUID, and asset tag from loader.
CPPFLAGS += -DSMBIOS_SERIAL_NUMBERS
# Use little-endian UUID format as defined in SMBIOS 2.6.
CPPFLAGS += -DSMBIOS_LITTLE_ENDIAN_UUID
# Use network-endian UUID format for backward compatibility.
#CPPFLAGS += -DSMBIOS_NETWORK_ENDIAN_UUID

LIBSTAND=	../../libstand/${MACHINE}/libstand.a

BOOT_FORTH=	yes
CPPFLAGS +=	-DBOOT_FORTH -DSTAND
CPPFLAGS +=	-I$(SRC)/common/ficl
CPPFLAGS +=	-I../../libficl
LIBFICL=	../../libficl/${MACHINE}/libficl.a

CPPFLAGS +=	-I../../../zfs
LIBZFSBOOT=	../../libzfsboot/${MACHINE}/libzfsboot.a

#LOADER_FDT_SUPPORT?=	no
#.if ${MK_FDT} != "no" && ${LOADER_FDT_SUPPORT} != "no"
#CFLAGS+=	-I${.CURDIR}/../../fdt
#CFLAGS+=	-I${.OBJDIR}/../../fdt
#CFLAGS+=	-DLOADER_FDT_SUPPORT
#LIBEFI_FDT=	${.OBJDIR}/../../efi/fdt/libefi_fdt.a
#LIBFDT=		${.OBJDIR}/../../fdt/libfdt.a
#.endif

# Always add MI sources
include	../Makefile.common
CPPFLAGS +=	-I../../../common

FILES=		${EFIPROG}
FILEMODE=	0555
ROOT_BOOT=	$(ROOT)/boot
ROOTBOOTFILES=$(FILES:%=$(ROOT_BOOT)/%)

LDSCRIPT=	../arch/${MACHINE}/ldscript.${MACHINE}
LDFLAGS =	-nostdlib --eh-frame-hdr
LDFLAGS +=	-shared --hash-style=both --enable-new-dtags
LDFLAGS +=	-T${LDSCRIPT} -Bsymbolic

CLEANFILES=	8x16.c vers.c

NEWVERSWHAT=	"EFI loader" ${MACHINE}

install: all $(ROOTBOOTFILES)

vers.c:	../../../common/newvers.sh ../version
	sh ../../../common/newvers.sh ../version ${NEWVERSWHAT}

${EFIPROG}: loader.sym
	if [ `${OBJDUMP} -t loader.sym | fgrep '*UND*' | wc -l` != 0 ]; then \
		${OBJDUMP} -t loader.sym | fgrep '*UND*'; \
		exit 1; \
	fi
	${OBJCOPY} --readonly-text -j .peheader -j .text -j .sdata -j .data \
		-j .dynamic -j .dynsym -j .rel.dyn \
		-j .rela.dyn -j .reloc -j .eh_frame -j set_Xcommand_set \
		--output-target=${EFI_TARGET} --subsystem efi-app loader.sym $@

LIBEFI=		../../libefi/${MACHINE}/libefi.a

DPADD=		${LIBFICL} ${LIBZFSBOOT} ${LIBEFI} ${LIBFDT} ${LIBEFI_FDT} \
		${LIBSTAND} ${LDSCRIPT}
LDADD=		${LIBFICL} ${LIBZFSBOOT} ${LIBEFI} ${LIBFDT} ${LIBEFI_FDT} \
		${LIBSTAND}


loader.sym:	$(OBJS) $(DPADD)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LDADD)

machine:
	$(RM) machine
	$(SYMLINK) ../../../../${MACHINE}/include machine

x86:
	$(RM) x86
	$(SYMLINK) ../../../../x86/include x86

clean clobber:
	$(RM) $(CLEANFILES) $(OBJS) loader.sym

%.o:	../%.c
	$(COMPILE.c) $<

%.o:	../arch/${MACHINE}/%.c
	$(COMPILE.c) $<

%.o:	../arch/common/%.c
	$(COMPILE.c) $<

#
# using -W to silence gas here, as for 32bit build, it will generate warning
# for start.S
#
%.o:	../arch/${MACHINE}/%.S
	$(COMPILE.S) -Wa,-W $<

%.o:	../../../common/%.c
	$(COMPILE.c) $<

%.o:	../../../common/linenoise/%.c
	$(COMPILE.c) $<

%.o: ../../../i386/libi386/%.c
	$(COMPILE.c) $<

%.o: $(SRC)/common/list/%.c
	$(COMPILE.c) -DNDEBUG $<

%.o: $(SRC)/uts/common/io/font/%.c
	$(COMPILE.c) $<

$(VGATEXT_FONT).c: $(VGATEXT_FONT_DIR)/$(VGATEXT_FONT_SRC).bdf \
	$(VGATEXT_FONT_DIR)/bdf_to_c.awk
	$(AWK) -f $(VGATEXT_FONT_DIR)/bdf_to_c.awk	\
		$(VGATEXT_FONT_DIR)/$(VGATEXT_FONT_SRC).bdf > $@

$(ROOT_BOOT)/%: %
	$(INS.file)
