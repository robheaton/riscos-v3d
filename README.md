# V3DDriver

GPU-accelerated rectangle fills and copies for the RISC OS desktop on the Raspberry Pi 4, using the V3D GPU.

An independent RISC OS module that drives the Pi 4's VideoCore VI (V3D 4.2) directly. It hooks the kernel's
`GraphicsV` Render call, so window dragging, scrolling and large fills are done by the GPU with no change to any
application. **Version 0.12 rev g. Experimental, and not an official ROOL project.**

## What it does

When RISC OS draws a rectangle fill or copy (`OS_Plot`, window scrolls, backdrop tiling), the kernel offers it to
`GraphicsV`. This module runs the worthwhile ones as V3D jobs and leaves the rest to the CPU.

- **Fills** (plain colours, and dither patterns that vary only from row to row), **copies** and **scrolls**.
- **32 and 16 bits per pixel** (16bpp is the Pi's RGB565), at any screen size: bigger rectangles are cut into
  bands. Tested on real hardware up to 3440x1440.
- Copes with desktop **mode changes**, and can be **reloaded without a reboot** (`*RMKill` then `*RMLoad`).

Not accelerated: small operations (a GPU call costs about 119 microseconds, so those stay on the CPU), patterns that
vary along a row, 8bpp and below, and text, icons, sprites and Draw (they do not go through `GraphicsV`).

## How fast, and is it right?

- **RISCOSmark** (1080p, 32bpp): Rectangle Copy 1,333 to 3,288 (about 2.5x); every other test unchanged.
- **Big rectangles, 3440x1440** (`test/FillCopyBench`): copies 4 to 9x faster, scrolls 4 to 8x, large fills 1.3 to
  4x. The desktop mostly draws small things, so the gain shows when dragging or scrolling large windows.
- **Correctness:** `test/VerifySuite` draws hundreds of operations on the GPU and the CPU and compares every pixel:
  450 of 450 identical, at both depths and three screen sizes.

## Requirements

- Raspberry Pi 4B, Pi 400 or Compute Module 4. **Not** the Pi 5 or earlier Pis.
- A recent RISC OS 5 build, in a 32bpp or 16bpp mode. For very large screens, set `gpu_mem` to 128 or more.

## Using it

```
*RMLoad V3DDriver                  accelerates straight away
*RMLoad V3DDriver NoAutoStart      load it but leave acceleration off until you ask
*RMKill V3DDriver                  unload
```

Double-click `test/GpuCheck` in the Filer to see what the GPU has done: operations accelerated, failures, timeouts.

If a GPU job ever times out, the driver switches acceleration off (a "fail-stop") and the desktop carries on on the
CPU; reboot, or re-arm with `V3D_AccelTune 6`.

The SWIs (prefix `V3D`, chunk `&5AE00`) and errors (block `&823100`) are documented in
[`docs/SWIs.md`](docs/SWIs.md). Call SWIs by name; the test SWIs are development tools, not a stable API.

## Tests

The BASIC programs in `test/` must be **double-clicked in the Filer, never typed into a Task Window**, which
swallows the drawing they test. The main ones are `VerifySuite` (GPU against CPU, pixel by pixel), `FillCopyBench`
(timings), `ModeChangeTest` and `GpuCheck`. `make hosttest` runs the host-side tests, and CI runs them on every push.

## Building

```
make                  GCCSDK 4.7.4 (arm-unknown-riscos-gcc, cmunge): V3DDriver,ffa, the smaller module (about 83 KB)
make gcc16            GCC 16 (github.com/robheaton/riscos-gcc16): V3DDriver_gcc16,ffa (about 147 KB)
make hosttest         the host-side tests
```

Both builds come from the same sources and pass the same hardware tests; use the GCCSDK one unless you want GCC 16.
`make gcc16` looks for the tool chain in `~/riscos-gcc16/riscos-gcc16-cross-16.2.0-24-x86_64-linux/bin`; set
`GCC16_BIN` to use another. `tools/mkrelease.py` builds the release zip with RISC OS file types.

## Known limitations

- True 4K (3840x2160) has not been run; the job-cutting is proven at 3440x1440 and against a model at 4K.
- The fail-stop after a timeout has not been provoked on hardware.

## How it works

Each operation is a *binning* job plus a *render* job (V3D control lists) working in 64x64 tiles: a fill clears a
tile buffer and stores it; a copy loads tiles and stores them elsewhere. Jobs are submitted through the V3D registers
and completed by its interrupt. Start with `c/gfxv_accel.c` (`GraphicsV` decoding), `c/cl_fill.c` and `c/cl_copy.c`
(the jobs), `c/cl_common.c` (submission, bands) and `c/module.c` (SWIs). The full history, including the long hunt
for the reload freeze, is in [`docs/DEVELOPMENT-LOG.md`](docs/DEVELOPMENT-LOG.md).

## Licence and credits

GNU General Public License, version 2 or later, the same as the Linux V3D driver this one follows: see
[`LICENSE`](LICENSE) and [`NOTICE`](NOTICE). Register offsets and the power-on sequence come, with attribution, from
the MIT-licensed [rpi4-gpu-bare-metal-examples](https://github.com/Random06457/rpi4-gpu-bare-metal-examples),
cross-checked against the Linux `v3d` driver and the RISC OS kernel and `BCMSupport`/`BCMVideo` sources.
