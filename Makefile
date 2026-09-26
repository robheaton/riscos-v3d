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

OBJS     := c/module.o c/phys_buffer.o c/cl_clear.o c/cl_fill.o c/sprite.o c/bcmsupport.o c/memsplit.o c/v3d_clock.o c/v3d_irq.o c/screen_info.o cmhg/V3DDriver.o

all: V3DDriver,ffa

# -d generates the C header (SWI numbers + entry-point prototypes),
# -o generates the linkable object - both from the same .cmhg source.
cmhg/V3DDriver.h: cmhg/V3DDriver.cmhg
	$(CMUNGE) $(CMUNGEFLAGS) -d $@ $<

cmhg/V3DDriver.o: cmhg/V3DDriver.cmhg
	$(CMUNGE) $(CMUNGEFLAGS) -o $@ $<

# All .c files include the generated header, so it must exist first.
c/module.o: c/module.c c/v3d_regs.h c/io_map.h c/phys_buffer.h c/cl_clear.h c/cl_fill.h c/memsplit.h c/v3d_clock.h c/v3d_irq.h c/screen_info.h cmhg/V3DDriver.h
	$(CC) $(CFLAGS) -c c/module.c -o c/module.o

c/screen_info.o: c/screen_info.c c/screen_info.h
	$(CC) $(CFLAGS) -c c/screen_info.c -o c/screen_info.o

c/phys_buffer.o: c/phys_buffer.c c/phys_buffer.h cmhg/V3DDriver.h
	$(CC) $(CFLAGS) -c c/phys_buffer.c -o c/phys_buffer.o

c/cl_clear.o: c/cl_clear.c c/cl_clear.h c/cl_builder.h c/cl_packets.h c/v3d_regs.h c/io_map.h c/phys_buffer.h c/sprite.h c/v3d_irq.h
	$(CC) $(CFLAGS) -c c/cl_clear.c -o c/cl_clear.o

c/cl_fill.o: c/cl_fill.c c/cl_fill.h c/cl_builder.h c/cl_packets.h c/v3d_regs.h c/io_map.h c/phys_buffer.h c/v3d_irq.h
	$(CC) $(CFLAGS) -c c/cl_fill.c -o c/cl_fill.o

c/sprite.o: c/sprite.c c/sprite.h
	$(CC) $(CFLAGS) -c c/sprite.c -o c/sprite.o

c/bcmsupport.o: c/bcmsupport.c c/bcmsupport.h
	$(CC) $(CFLAGS) -c c/bcmsupport.c -o c/bcmsupport.o

c/memsplit.o: c/memsplit.c c/memsplit.h c/bcmsupport.h
	$(CC) $(CFLAGS) -c c/memsplit.c -o c/memsplit.o

c/v3d_clock.o: c/v3d_clock.c c/v3d_clock.h c/bcmsupport.h
	$(CC) $(CFLAGS) -c c/v3d_clock.c -o c/v3d_clock.o

c/v3d_irq.o: c/v3d_irq.c c/v3d_irq.h c/io_map.h c/v3d_regs.h cmhg/V3DDriver.h
	$(CC) $(CFLAGS) -c c/v3d_irq.c -o c/v3d_irq.o

V3DDriver,ffa: $(OBJS)
	$(CC) -mmodule -o $@ $(OBJS)

clean:
	rm -f c/module.o c/phys_buffer.o c/cl_clear.o c/cl_fill.o c/sprite.o c/bcmsupport.o c/memsplit.o c/v3d_clock.o c/v3d_irq.o c/screen_info.o cmhg/V3DDriver.o cmhg/V3DDriver.h "V3DDriver,ffa"

.PHONY: all clean
