# V3DDriver

GPU-accelerated rectangle fills and copies for the RISC OS desktop on the Raspberry Pi 4, using the V3D GPU.

An independent RISC OS module that drives the Pi 4's VideoCore VI (V3D 4.2) directly. It hooks the kernel's
`GraphicsV` Render call, so window dragging, scrolling and large fills are done by the GPU with no change to any
application. **Version 0.12 rev d. Experimental, and not an official ROOL project.**

## What it does

When RISC OS draws a rectangle fill or copy (`OS_Plot`, window scrolls, backdrop tiling), the kernel offers it to
`GraphicsV` reason 13. This module takes the ones worth taking and runs them as V3D jobs; everything else is left to
the CPU as usual.

- Rectangle **fills** (plain colours, and dither patterns that vary only from row to row), **copies**, and
  **overlapping copies** (scrolls, in either direction).
- **32 and 16 bits per pixel** (16bpp is the Pi's RGB565).
- **Any screen size.** One GPU job holds at most 510 tiles of 64x64 pixels, so bigger rectangles are cut into bands
  of whole tile rows. Works on a 3440x1440 screen on real hardware; true 3840x2160 is untested (see below).
- Notices desktop **mode changes** and copes with them.
- **Reload without a reboot** (`*RMKill` then `*RMLoad`) is safe.

Not accelerated: small operations (a GPU call has a fixed cost of about 119 microseconds, so the driver leaves
anything small to the CPU), patterns that vary along a row, indexed colour depths (8bpp and below), text, icons,
sprites and the Draw module (they do not go through `GraphicsV`).

## How fast, and is it right?

- **RISCOSmark** (an independent benchmark, 1080p, 32bpp): Rectangle Copy went from 1,333 to 3,288;
  every other test was unchanged (about 2.5x).
- **Big rectangles, 3440x1440** (`test/FillCopyBench`, CPU time against GPU time): copies 4 to 9x faster, scrolls 4
  to 8x, large fills 1.3 to 4x. The desktop mostly draws small things, so the gain is most visible when dragging or
  scrolling large windows.
- **Correctness:** `test/VerifySuite` draws hundreds of fills, copies and scrolls on the GPU and again on the CPU
  and compares every pixel. 450 of 450 identical, at 16 and 32bpp, at 1080p, 1920x1200 and 3440x1440, including runs
  that force the job-cutting.

## Requirements

- Raspberry Pi 4B, Pi 400 or Compute Module 4 (BCM2711). **Not** the Pi 5 or earlier Pis.
- A recent RISC OS 5 build, in a 32bpp or 16bpp screen mode.
- For very large screens, raise `gpu_mem` in `config.txt` (128 or more; untested at 4K).

## Using it

```
*RMLoad V3DDriver                  accelerates straight away
*RMLoad V3DDriver NoAutoStart      load it but leave acceleration off until you ask
*RMKill V3DDriver                  unload; reloading without a reboot works
```

Run `test/GpuCheck` (double-click it in the Filer) to see what the GPU has done: operations accelerated, failures,
timeouts, interrupts and whether any screen was declined.

The module's SWIs are in the "user applications" chunk `&C0040`. **That number is not an official allocation** and
has to be replaced before any wider release.

| SWI | Purpose |
|---|---|
| `V3D_AccelStart` / `V3D_AccelStop` | claim or release `GraphicsV` |
| `V3D_AccelStats`, `V3D_AccelCopyStats`, `V3D_AccelProfile` | counters, health, interrupts, timings |
| `V3D_AccelLimits` | the size limits below which an operation is left to the CPU |
| `V3D_AccelTune` | tuning switches (job size, clock, scroll mode), a re-arm after a fail-stop |

The other SWIs (`V3D_ClearTest`, `V3D_FillTest`, `V3D_RegSnapshot` and so on) are development and test tools, not a
stable interface.

If a GPU job ever times out, the driver switches acceleration off (a "fail-stop") and the desktop carries on on the
CPU; reboot, or re-arm with `V3D_AccelTune 6`.

## Tests and tools

All in `test/` (RISC OS BASIC; double-click them in the Filer - **never type them into a Task Window**, which
swallows the drawing they test).

| | |
|---|---|
| `VerifySuite` | GPU against CPU, pixel by pixel; includes forced job-cutting |
| `GpuCheck` | what the GPU has done so far |
| `FillCopyBench` | times fills, copies and scrolls of 17 sizes: CPU, GPU, and the default rules |
| `ModeChangeTest` | desktop mode changes at both depths, up to 1920x1200 |
| `TuneBench`, `TraceDump` | tuning measurements; the driver's event log |
| `StressTest`, `ReloadProbe` | the old test path under repetition; the V3D's state around a reload |

`make hosttest` runs the host-side tests (job-cutting against a model screen, scroll ordering, the screen-base rule).
`make beacons` builds a diagnostic module that can draw numbered progress marks on screen, for locating a hard
freeze; it is never part of the normal build.

## Building

GCCSDK's `arm-unknown-riscos-gcc` (4.7.4) and `cmunge`, on any host:

```
make            builds V3DDriver,ffa
make hosttest
```

## Known limitations

- Pi 4 family only; 32 and 16bpp only.
- **True 4K (3840x2160) has not been run.** The job-cutting is proven on real hardware at 3440x1440 (1,242 tiles) and
  against a model at 4K.
- The fail-stop after a timeout has not been provoked on hardware.
- The test SWIs are not a stable API, and the SWI chunk number is unofficial.

## How it works

The V3D runs a *binning* job and a *render* job (control lists) per operation, working in 64x64 tiles: a fill
clears a tile buffer and stores it; a copy loads tiles and stores them elsewhere; an overlapping copy is one ordered
pass whose tile order equals that of one big job, proven against `memmove` on 148,000 cases. Jobs are submitted
through the V3D registers directly and completed by its interrupt. Key files: `c/module.c` (SWIs, `GraphicsV`
handling), `c/cl_fill.c` and `c/cl_copy.c` (the jobs), `c/cl_common.c` (submission, bands, scratch memory),
`c/v3d_irq.c` (interrupt), `c/gfxv_accel.c` (`GraphicsV` decoding).

The full story - every hardware round trip, the bugs found, and the long hunt for the reload freeze (whose cause was a
firmware clock flag that always reads "off", so every first power-on reset-pulsed a running V3D) - is in
[`docs/DEVELOPMENT-LOG.md`](docs/DEVELOPMENT-LOG.md).

## Credits

Register offsets and the power-on sequence are taken, with attribution, from the MIT-licensed bare-metal reference
[rpi4-gpu-bare-metal-examples](https://github.com/Random06457/rpi4-gpu-bare-metal-examples). Behaviour was
cross-checked against the Linux `v3d` driver and the RISC OS kernel and `BCMSupport`/`BCMVideo` sources.
