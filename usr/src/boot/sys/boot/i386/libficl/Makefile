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

SUBDIRS=	softcore
all	:=	TARGET = all
install	:=	TARGET = install
clean	:=	TARGET = clean
clobber	:=	TARGET = clobber

MACHINE=	$(MACH)
CC=		$(GCC_ROOT)/bin/gcc
FICLDIR=	$(SRC)/common/ficl
PNGLITE=	$(SRC)/common/pnglite

all install: $(SUBDIRS) machine x86 libficl.a

CPPFLAGS=	-DSTAND -I. -I../../../../include
CPPFLAGS +=	-I../../../../lib/libstand
CPPFLAGS +=	-I../../..  -I$(FICLDIR) -I../../common -I$(PNGLITE)

CFLAGS=		-Os -Wall -nostdinc
CFLAGS +=	-m32 -march=i386 -mpreferred-stack-boundary=2
CFLAGS +=	-fno-reorder-functions
CFLAGS +=	-ffreestanding
CFLAGS +=	-mno-mmx -mno-3dnow -mno-sse -mno-sse2 -mno-sse3 -msoft-float
CFLAGS +=	-std=gnu99

OBJECTS= dictionary.o system.o fileaccess.o float.o double.o prefix.o search.o
OBJECTS += softcore.o stack.o tools.o vm.o primitives.o unix.o utility.o
OBJECTS += hash.o callback.o word.o loader.o
HEADERS= $(FICLDIR)/ficl.h $(FICLDIR)/ficlplatform/unix.h ficllocal.h
#

MAJOR = 4
MINOR = 1.0

# static library build
libficl.a: $(OBJECTS)
	$(AR) $(ARFLAGS) libficl.a $(OBJECTS)

machine:
	$(RM) machine
	$(SYMLINK) ../../../$(MACHINE)/include machine

x86:
	$(RM) x86
	$(SYMLINK) ../../../x86/include x86

%.o:	softcore/%.c $(HEADERS)
	$(COMPILE.c) $<

%.o:	$(FICLDIR)/%.c $(HEADERS)
	$(COMPILE.c) $<

%.o:	$(FICLDIR)/ficlplatform/%.c $(HEADERS)
	$(COMPILE.c) $<

#
#       generic cleanup code
#
clobber clean: $(SUBDIRS) FRC
	$(RM) *.o *.a libficl.* ficl machine x86

$(SUBDIRS): FRC
	@cd $@; pwd; $(MAKE) $(MFLAGS) $(TARGET)

FRC:
