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

OBJS     := c/module.o c/phys_buffer.o c/cl_clear.o c/cl_common.o c/cl_fill.o c/cl_copy.o c/sprite.o c/bcmsupport.o c/memsplit.o c/v3d_clock.o c/v3d_irq.o c/screen_info.o c/gfxv_accel.o cmhg/V3DDriver.o

all: V3DDriver,ffa

# -d generates the C header (SWI numbers + entry-point prototypes),
# -o generates the linkable object - both from the same .cmhg source.
cmhg/V3DDriver.h: cmhg/V3DDriver.cmhg
	$(CMUNGE) $(CMUNGEFLAGS) -d $@ $<

cmhg/V3DDriver.o: cmhg/V3DDriver.cmhg
	$(CMUNGE) $(CMUNGEFLAGS) -o $@ $<

# All .c files include the generated header, so it must exist first.
c/module.o: c/module.c c/beacon.h c/screen_base.h c/accel_api.h c/cl_common.h c/cl_bands.h c/v3d_regs.h c/io_map.h c/phys_buffer.h c/cl_clear.h c/cl_fill.h c/cl_copy.h c/memsplit.h c/v3d_clock.h c/v3d_irq.h c/screen_info.h c/gfxv_accel.h cmhg/V3DDriver.h
	$(CC) $(CFLAGS) -c c/module.c -o c/module.o

c/cl_copy.o: c/cl_copy.c c/cl_copy.h c/cl_common.h c/cl_bands.h c/cl_builder.h c/cl_packets.h c/v3d_regs.h c/io_map.h c/phys_buffer.h c/v3d_irq.h
	$(CC) $(CFLAGS) -c c/cl_copy.c -o c/cl_copy.o

c/gfxv_accel.o: c/gfxv_accel.c c/gfxv_accel.h c/accel_api.h cmhg/V3DDriver.h
	$(CC) $(CFLAGS) -c c/gfxv_accel.c -o c/gfxv_accel.o

c/screen_info.o: c/screen_info.c c/screen_info.h
	$(CC) $(CFLAGS) -c c/screen_info.c -o c/screen_info.o

c/phys_buffer.o: c/phys_buffer.c c/phys_buffer.h c/cl_common.h c/cl_bands.h cmhg/V3DDriver.h
	$(CC) $(CFLAGS) -c c/phys_buffer.c -o c/phys_buffer.o

c/cl_clear.o: c/cl_clear.c c/beacon.h c/cl_clear.h c/cl_builder.h c/cl_packets.h c/v3d_regs.h c/io_map.h c/phys_buffer.h c/sprite.h c/v3d_irq.h
	$(CC) $(CFLAGS) -c c/cl_clear.c -o c/cl_clear.o

c/cl_common.o: c/cl_common.c c/cl_common.h c/cl_bands.h c/cl_builder.h c/cl_packets.h c/v3d_regs.h c/io_map.h c/phys_buffer.h c/v3d_irq.h
	$(CC) $(CFLAGS) -c c/cl_common.c -o c/cl_common.o

c/cl_fill.o: c/cl_fill.c c/cl_fill.h c/cl_common.h c/cl_bands.h c/cl_builder.h c/cl_packets.h c/v3d_regs.h c/io_map.h c/phys_buffer.h c/v3d_irq.h
	$(CC) $(CFLAGS) -c c/cl_fill.c -o c/cl_fill.o

c/sprite.o: c/sprite.c c/sprite.h
	$(CC) $(CFLAGS) -c c/sprite.c -o c/sprite.o

c/bcmsupport.o: c/bcmsupport.c c/beacon.h c/bcmsupport.h
	$(CC) $(CFLAGS) -c c/bcmsupport.c -o c/bcmsupport.o

c/memsplit.o: c/memsplit.c c/memsplit.h c/bcmsupport.h
	$(CC) $(CFLAGS) -c c/memsplit.c -o c/memsplit.o

c/v3d_clock.o: c/v3d_clock.c c/beacon.h c/v3d_clock.h c/bcmsupport.h
	$(CC) $(CFLAGS) -c c/v3d_clock.c -o c/v3d_clock.o

c/v3d_irq.o: c/v3d_irq.c c/v3d_irq.h c/cl_common.h c/cl_bands.h c/io_map.h c/v3d_regs.h cmhg/V3DDriver.h
	$(CC) $(CFLAGS) -c c/v3d_irq.c -o c/v3d_irq.o

V3DDriver,ffa: $(OBJS)
	$(CC) -mmodule -o $@ $(OBJS)

# ---------------------------------------------------------------------------------------------
# Diagnostic build for hard-freeze hunting (reload freeze, attempt 8): the same sources compiled with
# -DV3D_BEACONS, so every step on the suspect path lights a numbered block on screen (see c/beacon.h).
# Objects go to beacons/, so the normal build is never disturbed, and the help string says BEACONS so
# `*Help Modules` shows which one is loaded. Not part of `all` - never ship this one.
BEACON_DIR  := beacons
BEACON_OBJS := $(patsubst c/%.o,$(BEACON_DIR)/%.o,$(filter c/%.o,$(OBJS))) $(BEACON_DIR)/beacon.o $(BEACON_DIR)/V3DDriver.o

$(BEACON_DIR)/V3DDriver.cmhg: cmhg/V3DDriver.cmhg
	@mkdir -p $(BEACON_DIR)
	sed 's/^help-string:.*/help-string:              V3DDriver 0.12 BEACONS (diagnostic build, not for normal use)/' $< > $@

$(BEACON_DIR)/V3DDriver.o: $(BEACON_DIR)/V3DDriver.cmhg
	$(CMUNGE) $(CMUNGEFLAGS) -o $@ $<

$(BEACON_DIR)/%.o: c/%.c $(wildcard c/*.h) cmhg/V3DDriver.h
	@mkdir -p $(BEACON_DIR)
	$(CC) $(CFLAGS) -DV3D_BEACONS -c $< -o $@

V3DDriver_beacons,ffa: $(BEACON_OBJS)
	$(CC) -mmodule -o $@ $(BEACON_OBJS)

beacons: V3DDriver_beacons,ffa

# ---------------------------------------------------------------------------------------------
# Build with the GCC 16 tool chain (https://github.com/robheaton/riscos-gcc16, the Linux cross compiler), into gcc16/
# so that its objects never mix with the GCCSDK 4.7.4 ones. GCC16_BIN is that tool chain's bin directory. Its module
# kit is ARMv6, soft-float and position-dependent (the module relocates itself at start); the sources are the same.
GCC16_BIN  ?= $(HOME)/riscos-gcc16/riscos-gcc16-cross-16.2.0-24-x86_64-linux/bin
GCC16_DIR  := gcc16
GCC16_OBJS := $(patsubst c/%.o,$(GCC16_DIR)/%.o,$(filter c/%.o,$(OBJS))) $(GCC16_DIR)/V3DDriver.o

# cmunge writes the header and the object together; the C files need the header, so they depend on the object.
$(GCC16_DIR)/V3DDriver.o: cmhg/V3DDriver.cmhg
	@mkdir -p $(GCC16_DIR)
	$(GCC16_BIN)/cmunge -tgcc -32bit -p -d $(GCC16_DIR)/V3DDriver.h -o $@ $<

$(GCC16_DIR)/%.o: c/%.c $(wildcard c/*.h) $(GCC16_DIR)/V3DDriver.o
	$(GCC16_BIN)/arm-riscos-gnueabihf-gcc -mmodule -O2 -Wall -Wextra -Ic -I$(GCC16_DIR) -c $< -o $@

V3DDriver_gcc16,ffa: $(GCC16_OBJS)
	$(GCC16_BIN)/arm-riscos-gnueabihf-gcc -mmodule -o $@ $(GCC16_OBJS)

gcc16: V3DDriver_gcc16,ffa

clean:
	rm -rf $(GCC16_DIR) "V3DDriver_gcc16,ffa"
	rm -rf $(BEACON_DIR) "V3DDriver_beacons,ffa"
	rm -f c/module.o c/phys_buffer.o c/cl_clear.o c/cl_common.o c/cl_fill.o c/cl_copy.o c/sprite.o c/bcmsupport.o c/memsplit.o c/v3d_clock.o c/v3d_irq.o c/screen_info.o c/gfxv_accel.o cmhg/V3DDriver.o cmhg/V3DDriver.h "V3DDriver,ffa"

# Host-side tests (run on the development machine, not RISC OS): the scratch
# cache / control-list code built against a mock allocator, and the integer
# floating-point replacement checked against the float original, and (0.12) the REAL fill and copy
# code - band splitting included - run against a mock job runner that plays the render control
# list back on a model screen (fillsim, copysim).
hosttest:
	gcc -O1 -Wall -Itest/host/shim -Ic -o test/host/scratch_test test/host/scratch_test.c c/cl_common.c
	./test/host/scratch_test
	gcc -O0 -o test/host/fpcheck test/host/fpcheck.c
	./test/host/fpcheck
	gcc -O2 -Wall -o test/host/overlap_order_test test/host/overlap_order_test.c
	./test/host/overlap_order_test
	gcc -O1 -Wno-int-to-pointer-cast -Itest/host/shim -Ic -Dv3d_job_run=v3d_job_run_real -c -o test/host/cl_common_sim.o c/cl_common.c
	gcc -O1 -Wno-int-to-pointer-cast -Itest/host/shim -Ic -o test/host/fillsim test/host/fillsim.c c/cl_fill.c test/host/cl_common_sim.o
	./test/host/fillsim
	gcc -O1 -Wno-int-to-pointer-cast -Itest/host/shim -Ic -o test/host/copysim test/host/copysim.c c/cl_copy.c test/host/cl_common_sim.o
	./test/host/copysim
	$(MAKE) beacontest

# The beacon bar's drawing code against a fake framebuffer (no stray writes, step N in block N, one red
# block); also writes small PPM pictures to test/host/ to look at. 32-bit-address tricks as in the sims.
beacontest:
	gcc -O1 -Wall -no-pie -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast -DV3D_BEACONS '-DBEACON_DSB()=((void)0)' -Itest/host/shim -Ic -o test/host/beacon_test test/host/beacon_test.c c/beacon.c
	cd test/host && ./beacon_test

.PHONY: all clean hosttest beacontest beacons gcc16
