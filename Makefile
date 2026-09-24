# Makefile - builds V3DDriver,ffa against the GCCSDK RISC OS cross-toolchain.
#
# VERIFIED: every command below was run for real against the
# arm-unknown-riscos-gcc 4.7.4 / cmunge 0.76 toolchain and produced a
# working link. Flags (-tgcc -32bit -p for cmunge) are copied from
# SysLogD's Makefile.am, a real shipping GCCSDK module - the two-step
# "cmunge to .s then assemble separately" shape in an earlier draft of
# this file was wrong; cmunge emits a linkable .o directly.

CC       := arm-unknown-riscos-gcc
CMUNGE   := cmunge
CMUNGEFLAGS := -tgcc -32bit -p

CFLAGS   := -mmodule -O2 -Wall -Wextra -Ic -Icmhg

OBJS     := c/module.o c/phys_buffer.o c/cl_clear.o c/sprite.o cmhg/V3DDriver.o

all: V3DDriver,ffa

# -d generates the C header (SWI numbers + entry-point prototypes),
# -o generates the linkable object - both from the same .cmhg source.
cmhg/V3DDriver.h: cmhg/V3DDriver.cmhg
	$(CMUNGE) $(CMUNGEFLAGS) -d $@ $<

cmhg/V3DDriver.o: cmhg/V3DDriver.cmhg
	$(CMUNGE) $(CMUNGEFLAGS) -o $@ $<

# All .c files include the generated header, so it must exist first.
c/module.o: c/module.c c/v3d_regs.h c/io_map.h c/phys_buffer.h c/cl_clear.h cmhg/V3DDriver.h
	$(CC) $(CFLAGS) -c c/module.c -o c/module.o

c/phys_buffer.o: c/phys_buffer.c c/phys_buffer.h cmhg/V3DDriver.h
	$(CC) $(CFLAGS) -c c/phys_buffer.c -o c/phys_buffer.o

c/cl_clear.o: c/cl_clear.c c/cl_clear.h c/cl_builder.h c/v3d_regs.h c/io_map.h c/phys_buffer.h c/sprite.h
	$(CC) $(CFLAGS) -c c/cl_clear.c -o c/cl_clear.o

c/sprite.o: c/sprite.c c/sprite.h
	$(CC) $(CFLAGS) -c c/sprite.c -o c/sprite.o

V3DDriver,ffa: $(OBJS)
	$(CC) -mmodule -o $@ $(OBJS)

clean:
	rm -f c/module.o c/phys_buffer.o c/cl_clear.o c/sprite.o cmhg/V3DDriver.o cmhg/V3DDriver.h "V3DDriver,ffa"

.PHONY: all clean
