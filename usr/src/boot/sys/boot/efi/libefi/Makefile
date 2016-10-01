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
# Copyright 2015 Toomas Soome <tsoome@me.com>
# Copyright 2016 RackTop Systems.
#

include $(SRC)/Makefile.master

CC=     $(GCC_ROOT)/bin/gcc

LIB=	efi

all:	lib$(LIB).a

install:

SRCS=	delay.c efi_console.c efinet.c efipart.c errno.c handles.c \
	libefi.c time.c

include $(SRC)/Makefile.master

SUBDIRS = $(MACH) $(MACH64)

all	:=	TARGET = all
clean	:=	TARGET = clean
clobber	:=	TARGET = clobber
install	:=	TARGET = install

all clean clobber: $(SUBDIRS)

install: all

$(SUBDIRS): FRC
	@cd $@; pwd; $(MAKE) $(MFLAGS) $(TARGET)

x86:
	$(RM) x86
	$(SYMLINK) ../../../x86/include x86

$(OBJS): machine x86
