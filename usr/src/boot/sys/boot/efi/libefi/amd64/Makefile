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

MACHINE=	$(MACH64)
ASFLAGS=	-m64

all:		machine x86 libefi.a

include ../Makefile.com

CPPFLAGS +=	-DLOADER_EFI=L\"loader64.efi\"
CFLAGS +=	-m64

CLEANFILES +=	machine x86
