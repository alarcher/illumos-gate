
SRCS +=	multiboot_tramp.S \
	start.S \
	framebuffer.c

OBJS +=	multiboot_tramp.o \
	start.o \
	framebuffer.o

SRCS +=	nullconsole.c \
	spinconsole.c \
	comconsole.c

OBJS +=	nullconsole.o \
	spinconsole.o \
	comconsole.o
