# V3DDriver — M0/M1 sketch

A minimal RISC OS module skeleton to validate, on real Pi 4/CM4 hardware,
that a native RISC OS module can power on the VideoCore VI (V3D) GPU
block and read back its identity registers. Deliberately stops short of
control lists, shaders, or anything render-related — that's the next
milestone, once this is proven.

This is an independent implementation, not based on any unpublished
third-party driver. Register offsets and the power-on sequence are
transcribed (with attribution) from the MIT-licensed bare-metal reference
at <https://github.com/Random06457/rpi4-gpu-bare-metal-examples>, which
demonstrates the full pipeline (power-on through a rendered, shaded
triangle) running bare-metal — i.e. reproducible without any OS-level
driver as a dependency, RISC OS included.

## What's solid vs. what needs verification

**High confidence (sourced from working, MIT-licensed reference code):**
- `c/v3d_regs.h` — all register offsets and the PM password/ASB gate
  sequence in `v3d_power_on()`.
- Pi 4's "low peripheral mode" physical base (`0xFE000000`) is the same
  32-bit-reachable mapping RISC OS already relies on elsewhere on this
  board (GPIO/UART/mailbox), so no address translation should be needed.

**Now verified against ROOL's own documentation (previously flagged as guesses):**
- IO mapping (`c/io_map.h`) uses `OS_Memory 14`/`15`, the documented
  public SWI for temporarily mapping a megabyte of device physical
  memory — see
  [OS_Memory 14](https://www.riscosopen.org/wiki/documentation/show/OS_Memory%2014) /
  [15](https://www.riscosopen.org/wiki/documentation/show/OS_Memory%2015)
  on the ROOL wiki. This replaced an earlier `HAL_IOAddress()` guess,
  which was the wrong mechanism. Important consequence found in the
  docs: RISC OS keeps only **one** such mapping alive system-wide, and
  explicitly warns against mapping once at module-init and holding the
  pointer — every access now maps, reads/writes, and releases
  (`io_unmap`) before returning, instead of caching pointers in
  module-global state.
- SWI chunk number is now `0xC0040`, placed in the "user applications"
  group (bits 18-19 = `11`) per
  [ROOL's resource allocation rules](https://www.riscosopen.org/content/allocate)
  and the [SWI number bit layout](https://www.riscosopen.org/wiki/documentation/pages/Introduction+To+SWIs).
  The original placeholder (`0x50EC0`) was actually sitting in the
  reserved "OS modules" group by mistake — safe for private/dev use now,
  but still **not** an official allocation: request one via `!Allocate`
  to `allocate@riscosopen.org` before this is ever shared or released.

**Now build-verified (2026-09-20, against a real local GCCSDK install:
`arm-unknown-riscos-gcc` 4.7.4 / `cmunge` 0.76):**
- The whole thing actually builds and links into `V3DDriver,ffa`. This
  surfaced two real bugs the sketch had gotten wrong, not just things
  needing confirmation:
  - `swi-decoding-table:` syntax was invented (`V3D 2` + indented names)
    instead of cmunge's real flat form, `V3D,PowerOn,ReadIdent` — fixed,
    copied from a working GCCSDK example (`cmunge/Examples/module.cmhg`).
  - `module_final`'s signature was `void`, but cmunge's generated header
    requires `_kernel_oserror *` — fixed, and `module.c` now includes
    that generated header specifically so a mismatch like this is a
    compile error next time, not a silent ABI bug.
  - The Makefile's original two-step "cmunge emits assembly, then
    assemble separately" shape was wrong: `cmunge -o x.o file.cmhg`
    emits a linkable object directly. Flags (`-tgcc -32bit -p`) are
    copied from SysLogD's `Makefile.am`, a real shipping GCCSDK module,
    rather than guessed.
  - Apostrophes in `.cmhg` comments produce (non-fatal) parser warnings
    — avoided in the header now.

**Hardware-verified (2026-09-20, real CM4, RISC OS):** module loads,
`V3D_PowerOn` succeeds, `V3D_ReadIdent` returns:

```
R0 (V3D_HUB_IDENT0) = 0x42554856
R1 (V3D_HUB_IDENT1) = 0x000E1124
R2 (V3D_HUB_IDENT2) = 0x00000100
R3 (V3D_CTL_IDENT0) = 0x04443356
```

Decoded against the real Linux kernel `v3d_regs.h` bitfields (not
guessed):
- `V3D_CTL_IDENT0`: bytes 0-2 spell ASCII `V3D`, byte 3 (VER) = `4`.
- `V3D_HUB_IDENT1`: TVER=4, REV=2 → reports itself as **V3D 4.2**,
  the documented version for the Pi 4/CM4. NCORES=1, WITH_TFU=1,
  WITH_TSY=1 - all correct for this SoC.
- `V3D_HUB_IDENT2`: WITH_MMU=1, L3C=0KB - consistent with the above
  and with the V3D 3.3+ MMU noted in the original feasibility research.

This is the real milestone, not just the build: the power-on sequence
and register reads are genuinely talking to the physical GPU, and the
values are internally consistent and match public Pi 4 specs, not
noise. M0/M1 is done.

## M2 in progress: physical buffer allocation (2026-09-20, build-verified, not yet hardware-tested)

Control lists, tile-allocation memory, and shader/uniform data all need
a buffer whose **physical** address can be handed to the GPU - a
different problem from M0/M1's "map a known device register range"
(`OS_Memory 14`). This needs RISC OS to hand out physically-contiguous
RAM and tell us its physical address, which turns out to be a genuinely
more involved corner of the API: `c/phys_buffer.c` implements it as a
Physical Memory Pool dynamic area, following the exact procedure ROOL
developer Jeffrey Lee laid out on the forum
(<https://www.riscosopen.org/forum/forums/11/topics/9650>), cross-checked
against the PRM page for every SWI/reason-code involved (`OS_DynamicArea
0/21/22`, `OS_Memory 12`, `Dynamic Area ResizePMP`) - not reconstructed
from memory, since that forum thread shows even an experienced RISC OS
developer hit real bugs going through this the first time.

One piece of it (the mandatory `ResizePMP` dynamic-area handler, called
directly by the kernel with its own raw register/flag calling
convention) was verified the same way the M0/M1 fixes were: write a
plausible `generic-veneers` entry in the `.cmhg`, run `cmunge -d` for
real, and let the generated header's prototype comment settle the exact
C signature rather than guess it - see the comment above
`v3d_da_resize_handler` in `phys_buffer.c`.

`V3D_TestBuffer` is a new SWI that exercises the whole chain in
isolation, before any control-list content is built on top of it:
create the pool (once), claim+map one physical page, write a known
32-bit pattern through the resulting logical pointer, read it back, and
return the physical address, the readback value, and the logical
address in R0-R2. This is deliberately staged as its own testable step
- if buffer allocation itself doesn't work, nothing about the control
list encoding would matter anyway.

**Hardware-verified (2026-09-20, real CM4):** `V3D_TestBuffer` returned
readback `0xDEADBEEF` exactly - PMP creation, physical page claim
(`OS_DynamicArea 21`), logical mapping (`OS_DynamicArea 22`), and CPU
read/write through the resulting pointer all work correctly first try.
Example values: phys `0x03049000`, logical `0x91A67000`. Unlike M0/M1,
this one worked without needing a hardware-driven fix round.

## M2 continued: the actual control list (2026-09-20, build-verified, not yet hardware-tested)

`c/cl_clear.c` builds and submits the smallest real V3D job: bin an
empty primitive list, render one 64x64 tile by clearing it to a solid
colour, store the result, and read a pixel back through the CPU-mapped
logical pointer to confirm what actually landed in memory - not just
that the SWI returned without error, which control-list bugs often
don't trigger (wrong/no pixels, or a GPU hang, are much more likely
failure modes than a clean error return).

Every packet's byte layout is transcribed field-by-field from the
reference's C++ struct definitions
(`src/device/v3d/v3d_cl.hpp`) - deliberately **not** using C bitfields
ourselves, since that would mean trusting our AArch32 GCCSDK compiler to
lay out bitfields identically to the reference's AArch64 toolchain.
Instead every packet is built with explicit shifts into named byte
positions (`c/cl_builder.h`), with each field's derivation documented
above its `emit_*` function in `cl_clear.c` and cross-checked against
that struct's own `static_assert(sizeof(...)==N)` line in the source.
The bin/render register submission sequence (`V3D_CLE_CT0QMA/QMS/QTS`,
`CT0/CT1QBA/QEA`, the L2T cache-flush dance) is transcribed the same way
from that reference's `v3d_qpu.cpp` (`executeBin`/`executeRender`).

**Known risk, flagged rather than hidden:** the BCL used here has zero
primitives and skips all viewport/clip/shader-state setup, since nothing
is actually drawn. The reference never exercises this path - its BCL
always sets up a full viewport and shader state because it always draws
a triangle. Whether the V3D CLE tolerates a bare `TileBinningModeCfg` +
`OP_START_TILE_BINNING` + `OP_FLUSH` with nothing else is the single
most likely thing to need a hardware-driven fix, the same way the
`module_final` signature and the SWI chunk group did earlier.

Unlike the reference's infinite `while(...)` completion polls, the bin
and render waits here are bounded (5,000,000 iterations) and return a
timeout error instead of hanging - a module wedging the whole desktop
on a GPU stall would be a worse failure mode than an honest error.

`V3D_ClearTest` (new SWI) takes the clear colour as R0 in, returns the
render target's physical address (R0) and the read-back first pixel
(R1) out. Requires `V3D_PowerOn` first.

**Hardware result, attempt 1 (2026-09-20): `Bad alignment request`.**
Traced this down rather than taking it at face value - it turned out to
be a red herring pointing at a bug in *this project's own error
handling*, not at the real failure. Every `TODO: real error` placeholder
throughout the code (timeouts, "not powered on", bad page counts) was
returning `_kernel_last_oserror()` - which doesn't create an error, it
reads back whatever error happened to be cached *anywhere* in the
system last. A
[ROOL bug thread](https://www.riscosopen.org/forum/forums/4/topics/17507)
shows this exact message ("Bad alignment request", error 279) turning
up in completely unrelated contexts for the same reason. Replaced every
placeholder with a real, distinct, descriptive `_kernel_oserror`, and
fixed the SWI handler's default case to return cmunge's own
`error_BAD_SWI` instead of the same bad pattern.

**Hardware result, attempt 2 (2026-09-20):** with real error reporting
in place: *"timed out waiting for V3D_CLE_BFC to advance - binning did
not complete"* - confirming the exact risk flagged above. Fix: added
back the reference's full always-present viewport/clip/raster
state-setup block (`ClipWindow` through `VcmCacheSize`, ~20 packets,
values copied verbatim from the reference since they're otherwise
geometry-related and inert without any primitives) between
`OP_START_TILE_BINNING` and `OP_FLUSH`.

**Hardware result, attempt 3 (2026-09-20): identical error.** Ruled out
the state-setup block as the cause. Reconsidered something more
fundamental instead of guessing at more BCL content: the completion
poll held a single `OS_Memory 14` mapping through up to 5,000,000 loop
iterations. The PRM is explicit that this is a single, system-wide
*shared* mapping window, and warns against holding it for anything
long-running - if anything else in the system (an interrupt handler,
another driver's own temporary mapping) used the same mechanism during
that busy-wait, our window would be silently repointed at a *different*
device's registers, and the loop would spin against the wrong memory
until timeout regardless of what the GPU actually did. M0/M1's
single-word reads never hit this (in and out in microseconds); this
multi-million-iteration spin is a different animal. Rewrote the poll
(`v3d_poll_counter` in `cl_clear.c`) to remap in short bursts (10,000
reads at a time, 500 bursts, same total ceiling) instead of one
unbroken hold - not yet confirmed this was the actual cause, only that
the old pattern violated the documented contract regardless.

**Hardware result, attempt 4 (2026-09-20): identical error again.**
Ruled out the shared-IO-window theory too. Also discovered a real,
separate bug while re-testing: calling `V3D_ClearTest` twice without
`*RMKill`-ing the module between runs failed with *"memory cannot be
moved"* - `pmp_alloc` always tried to claim fresh physical pages for
the same PMP page range, which fails when that range is still locked
and mapped from the previous call. Fixed (`pmp_release` in
`phys_buffer.c`, called at the start of every `pmp_alloc`, unmaps then
releases any existing pages at that range first - safe even on a
never-used range, since the PRM documents that as a no-op). This makes
repeat testing possible but is unrelated to the BFC timeout itself.

Reconsidered something more fundamental for the timeout: RISC OS runs
with CPU data caches enabled, and our BCL/RCL bytes are written by the
CPU into normal (default-cacheable) RAM via `pmp_alloc`. The V3D GPU is
a separate bus master reading DRAM directly - if those CPU writes were
still sitting in cache rather than having reached physical memory, the
GPU would see stale/garbage data instead of our control list, which
would fully explain binning never producing anything coherent enough to
reach `OP_FLUSH`. RISC OS has a documented API specifically for this
class of problem (`OS_Memory 19`, "Prepare for DMA"), which is itself
confirmation this is a real, commonly-hit issue on this OS - its own
docs describe exactly the CPU-writes-DMA-reads-stale-data scenario.
Rather than take on `OS_Memory 19`'s substantial callback-based
interface, used a simpler fix: `OS_DynamicArea 22`'s page-flags word
already has unused cache-policy bits (4 and 5), and setting both marks
a page **NCNB - VMSA "Strongly-Ordered" memory**: every CPU write goes
straight to physical RAM, nothing to flush, at a performance cost that
doesn't matter for a handful of small buffers. Applied to every page
`pmp_alloc` maps (`PAGE_FLAG_NONCACHEABLE` in `phys_buffer.c`).

**Hardware result, attempt 5 (2026-09-20): identical error, fourth
time in a row.** The memory-cannot-be-moved fix confirmed working;
the BFC timeout itself was completely unmoved by cache coherency
either. Four different, individually well-reasoned fixes in a row
producing byte-for-byte the same failure means continuing to guess at
fixes stopped being a good use of anyone's time - so this round adds
**instrumentation instead of another guess**.

The real Linux v3d driver (`v3d_irq.c`, fetched from kernel.org)
signals bin completion via an interrupt, `V3D_INT_FLDONE`, and
explicitly handles a separate `V3D_INT_OUTOMEM` condition where *the
binner stalls waiting for more tile-allocation memory* until software
feeds it more via `V3D_PTB_BPOA`/`BPOS` - a driver comment states this
plainly: "we need to allocate some new memory and pass it to the
binner so the current job can make progress." A binner stalled on OOM
would look, from `V3D_CLE_BFC` alone, identical to one that never
started - and we've never touched the OOM path at all. This is a real,
plausible mechanism, not a guess dressed up as one, but it needs data
to confirm, not another blind fix.

Added `diagnose_timeout()` (`cl_clear.c`): on a BFC/RFC timeout, it now
reads `V3D_CTL_INT_STS` (the interrupt status register - offset and
bits transcribed from the real kernel header, not guessed) and decodes
it into the returned error message, e.g. whether `OUTOMEM` is set, or
`FLDONE`/`FRDONE` (meaning the hardware *did* think it finished, which
would point at a BFC-reading bug instead), or nothing at all (meaning
the bin genuinely never got anywhere). Whatever comes back next
should tell us which of these it actually is, rather than us
continuing to guess.

**Hardware result, attempt 6 (2026-09-20): `INT_STS=0x00000000 (none)`.**
Real data, and it ruled out three things at once: not OOM (`OUTOMEM`
clear), not a BFC-reading bug (`FLDONE` also clear - the hardware
itself doesn't think it finished either), not a GMP violation. Nothing
happened at all, which points at the submission itself, not anything
downstream of it.

Went looking at the real driver's actual job-submission code next
(`v3d_sched.c`, `v3d_bin_job_run()`) rather than guessing again - it
writes `V3D_CLE_CT0QMA`/`QMS`/`QTS`/`QBA`/`QEA` in exactly the order
this project already does, byte for byte, with the same "writing QEA
starts the job" comment. So the *sequence* isn't the gap. What the
driver's own timeout handler (`v3d_cl_job_timedout`) checks instead is
`V3D_CLE_CTNCA`/`CTNRA` - a "current address"/"return address" pair
that acts as a program counter into the control list, specifically to
tell "never started" apart from "stuck partway through".

Added those registers (`V3D_CLE_CT0CS/EA/CA/RA` and their thread-1
equivalents, offsets from the real kernel header) to `diagnose_timeout()`,
plus a readback of what we actually wrote to `QBA`/`QEA` - if the
readback doesn't match what we wrote, the write itself never reached
the hardware, which would be a different bug entirely from anything
considered so far.

While adding these fields, caught a real bug in the diagnostic code
itself before it shipped: the worst-case message length came to 291
bytes against `_kernel_oserror.errmess`'s 252-byte buffer - a genuine
overflow risk in a kernel module, not cosmetic. Shortened the message
and added hard bounds-checking to `append_str`/`append_hex32`
regardless (252-byte cap enforced in code, not just by keeping the
call sites short), so a future edit that grows the message again fails
safe instead of corrupting memory.

## Layout

```
riscos-v3d/
  cmhg/V3DDriver.cmhg   module header: title, SWI chunk, entry points
  c/v3d_regs.h          register offsets (see attribution above)
  c/io_map.h            OS_Memory 14/15 mapping helper (see above)
  c/phys_buffer.h/.c    physical buffer allocation (M2, see above)
  c/cl_builder.h        byte-packing helper for control-list packets
  c/cl_clear.h/.c       the clear-to-colour control list itself (M2)
  c/module.c            module_init / module_final / SWI handler
  Makefile              GCCSDK build - verified working, see above
```

`cmhg/V3DDriver.h`, `c/*.o`, `cmhg/*.o` and `V3DDriver,ffa` are build
output (gitignored), regenerated by `make`.

## SWIs exposed

- `V3D_PowerOn` (0xC0040) — deassert V3D reset, release ASB gates.
- `V3D_ReadIdent` (0xC0041) — returns V3D_HUB_IDENT0/1/2 and
  V3D_CTL_IDENT0 in R0-R3. A correct read confirms the GPU is powered
  and responding — Mesa's `v3d_get_device_info()` decodes these same
  fields (core count, QPU count, VPM size, HW version) if we want to
  print something human-readable later.
- `V3D_TestBuffer` (0xC0042) — allocates a physically-contiguous page
  (see "M2 in progress" above), writes/reads back a test pattern, and
  returns physical address (R0), the readback value (R1, should be
  `0xDEADBEEF` on success), and the logical address (R2).
- `V3D_ClearTest` (0xC0043) — takes a clear colour in R0, runs the
  clear-to-colour control list (see "M2 continued" above), returns the
  render target's physical address (R0) and the read-back pixel (R1,
  should equal the colour passed in on success). Requires `V3D_PowerOn`.

## Testing on hardware

1. Build with a working GCCSDK RISC OS cross-toolchain.
2. Copy `V3DDriver,ffa` to the Pi 4/CM4, `*RMLoad` it.
3. Test from BASIC, not the `*` prompt — the module has no
   `command-keyword-table`, so its SWIs are not exposed as `*`
   commands (`*V3D_PowerOn` correctly reports "not recognised"; this
   tripped up the first hardware test):
   ```basic
   SYS "V3D_PowerOn"
   SYS "V3D_ReadIdent" TO r0,r1,r2,r3
   PRINT ~r0, ~r1, ~r2, ~r3
   ```
   `IDENT0` should decode to the ASCII bytes for the V3D magic/version
   fields — that's the concrete pass/fail signal for this milestone.
4. M2 buffer test:
   ```basic
   SYS "V3D_TestBuffer" TO r0,r1,r2
   PRINT ~r0, ~r1, ~r2
   ```
   R1 should read back exactly `DEADBEEF`. If this errors, the error
   text will point at which SWI in the chain (`OS_DynamicArea`,
   `OS_Memory`) rejected the call - that's the fastest way back to
   which assumption in `phys_buffer.c` was wrong.
5. M2 control-list test (needs `V3D_PowerOn` done first):
   ```basic
   SYS "V3D_ClearTest", &DEADBEEF TO r0,r1
   PRINT ~r0, ~r1
   ```
   R1 should read back exactly `DEADBEEF` again - if the whole
   bin→render→store pipeline worked, the pixel we asked to clear to is
   the pixel we read back. If this hangs rather than erroring or
   returning, it's most likely a real GPU stall (the bounded poll should
   still recover with a timeout error rather than truly hang forever,
   but this is the first code path that submits real work to the CLE,
   so treat a genuine hang as a live possibility worth testing
   cautiously - e.g. save any open work first).

## Next milestones

- If `V3D_ClearTest` doesn't come back clean, the BCL's lack of
  viewport/shader-state setup (see "M2 continued" above) is the prime
  suspect.
- M3: wrap the rendered buffer as a sprite (`OS_SpriteOp`) and present it
  in a Wimp window - turns "a pixel we can read back" into "something
  visibly on screen".
- Real IRQ handling instead of polling, once the synchronous path works.
- Eventually: actual shaders (the reference's rainbow-triangle QPU
  bytecode is a ready-made, proven starting point) rather than a
  fixed-function clear.

Full technical grounding and effort estimate for the milestones beyond
this sketch: see the research summary from the conversation that
produced this skeleton.
