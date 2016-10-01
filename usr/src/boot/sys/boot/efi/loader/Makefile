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

.KEEP_STATE:

NEWVERSWHAT=	"EFI loader" ${MACHINE}

all: loader.efi

install: all $(ROOTBOOTFILES)

vers.c:	../../common/newvers.sh ../../efi/loader/version
	$(SH) ../../common/newvers.sh version ${NEWVERSWHAT}

EFI_TARGET=	pei-x86-64

loader.efi: loader.sym
	if [ `${OBJDUMP} -t loader.sym | fgrep '*UND*' | wc -l` != 0 ]; then \
		${OBJDUMP} -t loader.sym | fgrep '*UND*'; \
		exit 1; \
	fi
	${OBJCOPY} --readonly-text -j .peheader -j .text -j .sdata -j .data \
		-j .dynamic -j .dynsym -j .rel.dyn \
		-j .rela.dyn -j .reloc -j .eh_frame -j set_Xcommand_set \
		--output-target=${EFI_TARGET} --subsystem efi-app loader.sym $@

LIBEFI=		../libefi/libefi.a

DPADD=		${LIBFICL} ${LIBZFSBOOT} ${LIBEFI} ${LIBFDT} ${LIBEFI_FDT} \
		${LIBSTAND} ${LDSCRIPT}
LDADD=		${LIBFICL} ${LIBZFSBOOT} ${LIBEFI} ${LIBFDT} ${LIBEFI_FDT} \
		${LIBSTAND}


loader.sym:	$(OBJS) $(DPADD)
	$(LD) $(LDFLAGS) -o $@ $(OBJS) $(LDADD)

CLEANFILES +=   machine x86

machine:
	$(RM) machine
	$(SYMLINK) ../../../${MACHINE}/include machine

x86:
	$(RM) x86
	$(SYMLINK) ../../../x86/include x86

$(OBJS): machine x86

clean clobber:
	$(RM) $(CLEANFILES) $(OBJS) loader.sym loader.efi

%.o:	%.c
	$(COMPILE.c) $<

%.o:	./arch/${MACHINE}/%.c
	$(COMPILE.c) $<

SUBDIRS = $(MACH) $(MACH64)

all	:=	TARGET = all
clean	:=	TARGET = clean
clobber	:=	TARGET = clobber
install	:=	TARGET = install

all clean clobber install: $(SUBDIRS)

$(SUBDIRS): FRC
	@cd $@; pwd; $(MAKE) $(MFLAGS) $(TARGET)

FRC:
