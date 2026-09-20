# Makefile - builds V3DDriver against the GCCSDK RISC OS cross-toolchain.
#
# NOT VERIFIED end-to-end: exact tool names/paths vary a bit between
# GCCSDK releases. This is the standard shape (compile C, munge the
# .cmhg header into linkable glue, link as a relocatable module) but
# check `arm-unknown-riscos-gcc --version` / GCCSDK's own docs and
# adjust CC/CMHG/LD below before trusting this blindly.
#
# Install toolchain: https://github.com/riscos/riscos-gnu-toolchain

PREFIX  ?= arm-unknown-riscos
CC      := $(PREFIX)-gcc
CMHG    := $(PREFIX)-cmunge
LD      := $(PREFIX)-gcc

CFLAGS  := -mmodule -O2 -Wall -Wextra -Ic

OBJS    := c/module.o cmhg/V3DDriver_head.o

all: V3DDriver,ffa

cmhg/V3DDriver_head.s: cmhg/V3DDriver.cmhg
	$(CMHG) $< -o $@ -d cmhg/V3DDriver_head.d

cmhg/V3DDriver_head.o: cmhg/V3DDriver_head.s
	$(CC) -c $< -o $@

c/module.o: c/module.c c/v3d_regs.h
	$(CC) $(CFLAGS) -c c/module.c -o c/module.o

V3DDriver,ffa: $(OBJS)
	$(LD) -mmodule -o $@ $(OBJS)

clean:
	rm -f c/module.o cmhg/V3DDriver_head.s cmhg/V3DDriver_head.o cmhg/V3DDriver_head.d "V3DDriver,ffa"

.PHONY: all clean
