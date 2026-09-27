# V3DDriver — real desktop fills and copies now GPU-accelerated via GraphicsV, hardware-verified

An independent RISC OS module driving the Raspberry Pi 4/CM4's
VideoCore VI (V3D) GPU directly at the register level. As of
2026-09-24, this actually works: `V3D_ClearTest` submits a real
control list to the GPU, the GPU executes it (bin + render), clears a
tile to a requested colour, stores it to memory, and the CPU reads
back exactly that colour through a physical buffer the GPU wrote to.
Confirmed on real hardware, not just build-verified.

> **Known limitation**: do not `*RMKill`/`*RMLoad` this module without
> a full reboot in between - it reliably freezes the machine. Always
> reboot before loading a freshly-built module. See "Known
> limitations" further down for the full story.

- **M0/M1** — power on the V3D block, read its identity registers back
  correctly (confirms it reports itself as V3D 4.2, matching the known
  Pi 4/CM4 spec).
- **M2** — allocate physically-addressable, GPU-shared buffers; build
  and submit an actual binning + rendering control list; read back a
  GPU-written pixel that matches what was asked for.
- **M3** — wrap the same output as a valid RISC OS sprite and display
  it in a Wimp window via a small companion BASIC program. Build-
  verified, not yet confirmed on hardware.

Getting M2 working took thirteen hardware round-trips and genuinely
found bugs at nearly every layer - a misused error-reporting function,
a page-reuse bug, missing cache-coherency handling, a GPU thread wedged
by a bad early submission with no real reset pulse to clear it, an
undocumented-by-us restart race, and a control-list opcode used without
its prerequisite. The full log of what was tried, what the hardware
actually said, and what each fix turned out to be is kept below rather
than summarized away - it's the most concrete evidence in this repo of
what this kind of bring-up work actually looks like, and worth reading
before repeating any of it.

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

**Hardware result, attempt 7 (2026-09-20) - the actual finding:**
`CA=0x0304A000` while our actual submission was at `wQBA=0x03089000`/
`rQBA=0x03089000` (write and readback matching, so the write itself
was never in question). `0x0304A000` is suspiciously close to
`0x03049000` - the physical address `V3D_TestBuffer` reported *several
test attempts ago, near the start of this debugging session*. The
CLE's "current position in the control list" register was pointing at
a stale address from a job submitted long before the current one, not
our actual submission - meaning **the bin thread has been wedged since
early in this session**, most likely from the very first, genuinely
malformed control list (before the BCL state-setup block existed), and
every fix since then was tested against a thread that was never
listening in the first place. That explains why four individually
well-reasoned, unrelated fixes all produced byte-for-byte the same
result: none of them addressed *un-wedging it*.

Root cause of why nothing since has un-wedged it: `v3d_power_on()` only
ever set `PM_GRAFX`'s reset-deassert bit, copied faithfully from the
bare-metal reference - which is correct there, since it runs once on a
chip fresh from power-on reset where the bit starts clear, so their one
"set" call is the only real reset transition that ever happens for
them. This project calls `V3D_PowerOn` repeatedly across a whole
debugging session with no reboot in between, so after the first call
the bit was already set and every subsequent call was a complete
no-op - it never actually reset anything, wedged CLE included. Fixed:
`v3d_power_on()` now explicitly clears the bit (asserts reset) before
setting it again (deasserts), forcing a genuine pulse on every call,
with a volatile-counted delay loop between the two (checked in the
actual disassembly that `-O2` didn't eliminate it, since a delay loop
with no other side effect is exactly the kind of thing an optimiser is
entitled to remove).

**Hardware result, attempt 8 (2026-09-20): different failure this
time, and it's genuine progress.** `wQBA`/`rQBA` and `wQEA`/`rQEA` no
longer matched each other at all - a new symptom, not the same result
again. Rather than treat that as noise, found the actual, authoritative
register documentation instead of continuing to infer from names:
Broadcom's own **"VideoCore IV 3D Architecture Reference Guide"**
(docs.broadcom.com/doc/12358545, found in the very first research pass
of this whole project and never actually opened until now), Table 49,
gives the real bit-level meaning of `V3D_CTnCS`:

- bit 5 `CTRUN`: 1 = running (writing 1 *stops* a thread - never write
  it to start one)
- bit 4 `CTSUBS`: writing 1 clears a "stopped at halt" condition and
  restarts the thread if `CA != EA`
- bit 3 `CTERR`: read-only, set when stopped *with an error*

Decoded against this: the *previous* attempt's `CS=0x58` meant
`CTRUN=0, CTERR=1` - **the thread was halted with an error**, which is
exactly consistent with "wedged since early in the session" and
explains why four earlier fixes all produced the identical result. The
*latest* attempt's `CS=0x20` means `CTRUN=1` - genuinely running, no
error - confirming the reset-pulse fix really did clear that error
state. Progress, not a new dead end: `CA` had advanced roughly 32KB
past our submitted 107-byte control list without stopping at
`OP_FLUSH`, so the thread is now actually executing, just not
completing cleanly - a different, narrower problem than "wedged".

Note on this document's scope: it's the *older* VC4 generation, and
its description of `CTnCA`/`CTnEA` as the direct submission registers
does **not** apply to the modern V3D used here (the real V3D driver,
proven correct by the bare-metal reference actually rendering on real
Pi4 hardware, submits via `CT0QBA`/`QEA` instead) - only the `CTnCS`
bit layout and general thread-state model were treated as still
authoritative, since those are lower-level architectural concepts that
carried forward and this is the only place they're documented at the
bit level at all.

Added the documented restart step - write `CTSUBS` (bit 4) to
`CT0CS`/`CT1CS` before every submission, clearing any leftover
halt/error state via the official mechanism rather than relying only
on the block-level `PM_GRAFX` reset pulse to reach it - plus decoded
`CS` (`RUN`/`STOP`/`ERR`) into the diagnostic message alongside the
existing fields. Not yet re-tested on hardware.

If this still doesn't produce a clean completion, worth trying an
actual power cycle of the CM4 too - software reset pulses don't always
reach everything a hardware reset does, and we now have concrete
evidence (the stuck CT0CA) that *something* was left in a bad state
from early in this session that ordinary software paths didn't reach
on their own.

**Hardware result, attempt 9 (2026-09-20):** `CS=0x50` -
`CTRUN=0, CTSUBS=1, CTERR=0`: stopped at halt again, but *no error*
this time. `CA=0x00000000, EA=0x00000000` - both zeroed. And `rQBA`/
`rQEA` this run exactly matched the *previous* run's written values,
not this run's - the new job was never latched at all.

Read together: writing `CTSUBS` to restart very likely clears `CA`/`EA`
to zero as part of leaving halt - which means the documented restart
condition ("starts running if `CA != EA`") can never be true in that
same instant (0 == 0), so the thread just re-stops immediately. We
then wrote the new job's `QMA`/`QMS`/`QTS`/`QBA`/`QEA` with zero delay
after that restart trigger - exactly the shape of a write-after-write
race against a hardware state machine still mid-transition, and the
readback confirms those writes never landed.

Added `v3d_restart_thread()` (`cl_clear.c`): issues the `CTSUBS`
restart write, then a fixed settle delay (same volatile-counted-loop
technique as the `PM_GRAFX` reset pulse in `module.c`, same reasoning
for why the counter must be volatile) before any further register
writes. This isn't "wait for an async op to finish" - there's nothing
meaningful to poll for until new work is actually queued - just room
for the hardware to finish processing one control write before the
next one arrives.

**Hardware result, attempt 10 (2026-09-24) - best state seen yet:**
`CS=0x20` → `CTRUN=1`, no error, no halt - the CLE thread is genuinely
**running**, for the first time across ten attempts. `CA=0x00000010`
(suspiciously tiny - none of our real buffers are anywhere near this)
and `EA=0x030A9000`, which matched neither `wQBA` nor `wQEA` from this
submission. Rather than pattern-match `EA` against a *guessed* buffer
address via page-layout arithmetic (exactly the kind of guess this
project has repeatedly gotten burned trusting), extended the
diagnostic to log `tile_alloc`/`render_target`/`tile_state`'s actual
physical addresses alongside everything else
(`diag_bufs_t` in `cl_clear.c`) so the next result answers this with
data instead of arithmetic.

The thread being confirmed alive and running (not wedged, not errored)
for a full 5,000,000-iteration timeout without ever hitting `OP_FLUSH`
narrows things meaningfully: this now looks like a genuine control-list
*content* or infinite-loop/wait issue, not a setup/reset/infrastructure
one - a different, narrower class of problem than anything the last
nine attempts were chasing.

**Hardware result, attempt 11 (2026-09-24) - binning is confirmed
fully working.** `STS=0x00000002` decodes to `V3D_INT_FLDONE` (bin
done) - `V3D_CLE_BFC` genuinely advanced this time, and the timeout
moved to `RFC` (render) instead. Real, unambiguous progress: the whole
BCL, buffer setup, restart sequence, and bin submission are correct.

The render-side hang was sharply diagnostic: `CT1CA=0x00000000` with
`CT1CS` showing `CTRTSD=1` (one level into a nested sub-list) and
`CTRUN=1` (genuinely running). Comparing against the reference's real
RCL: it always pairs `SupertileCoordinates` with a prior
`StartAddressOfGenericTileList`, which points the hardware at a small
"indirect" list of its own - we called `SupertileCoordinates` without
ever writing that indirect-list address, leaving the branch target at
its zero default. `CA` landing on exactly `0x00000000` while genuinely
mid-execution matches that precisely. The logged buffer addresses from
the previous commit confirmed it wasn't any of our own buffers either
- ruling that specific guess in with data, not arithmetic.

Fixed: removed `SupertileCoordinates` (and the now-dead
`emit_supertile_coordinates`) entirely. Both `TileCoordinates` blocks
already do our real work (clear + store) inline; we have no supertiles
to walk and don't need generic-tile-list indirection for one hardcoded
tile.

**Hardware result, attempt 12 (2026-09-24): binning failed again, and
it's the *same* failure mode as before the settle-delay fix.** `CS=0x50`
(stopped at halt, no error) with `CA=EA=0` and `QBA`/`QEA` readback not
matching this run's submission - identical to what attempt 9 diagnosed,
which the fixed-delay version of `v3d_restart_thread` was supposed to
have already fixed, and which *did* work for the immediately preceding
(successful) test. A fix that works once and then reproduces the exact
pre-fix symptom on a later run is the signature of timing that
sometimes isn't long enough, not a real fix - the original version
reasoned there was "nothing meaningful to poll for" after the restart
write and used a fixed-count delay loop instead. That conflated two
different things: a job completing (genuinely nothing to wait for
yet) and the hardware finishing its own response to the restart write
itself (`CTSUBS` clearing back to 0, which the PRM documents happens
once the thread actually leaves "stopped at halt"). Replaced the fixed
delay with a bounded poll on that actual condition.

**Hardware result, attempt 13 (2026-09-24): success.**
`SYS "V3D_ClearTest", &DEADBEEF TO r0,r1` returned `r0=0x0304A000`
(the render target's physical address) and `r1=DEADBEEF` - the exact
colour requested, read back from the physical memory the GPU wrote it
to. Binning completes, rendering completes, the clear+store actually
happens, and the CPU sees the correct result through a non-cacheable
mapping of GPU-written memory. **M2 is done and hardware-confirmed.**

## Layout

```
riscos-v3d/
  cmhg/V3DDriver.cmhg   module header: title, SWI chunk, entry points
  c/v3d_regs.h          register offsets (see attribution above)
  c/io_map.h            OS_Memory 14/15 mapping helper (see above)
  c/phys_buffer.h/.c    physical buffer allocation (M2, see above)
  c/cl_builder.h        byte-packing helper for control-list packets
  c/cl_clear.h/.c       the clear-to-colour control list itself (M2/M3)
  c/sprite.h/.c         RISC OS sprite area/header format (M3)
  c/memsplit.h/.c       VideoCore firmware mailbox ARM/GPU memory query
  c/module.c            module_init / module_final / SWI handler
  Makefile              GCCSDK build - verified working, see above
  test/ShowSprite.bas   M3 companion app - displays the GPU output in
                         a Wimp window (see "M3" below for why this
                         isn't part of the module itself)
```

`cmhg/V3DDriver.h`, `c/*.o`, `cmhg/*.o` and `V3DDriver,ffa` are build
output (gitignored), regenerated by `make`.

## SWIs exposed

- `V3D_PowerOn` (0xC0040) — deassert V3D reset, release ASB gates.
- `V3D_ReadIdent` (0xC0041) — returns V3D_HUB_IDENT0/1/2 and
  V3D_CTL_IDENT0 in R0-R3. A correct read confirms the GPU is powered
  and responding — Mesa's `v3d_get_device_info()` decodes these same
  fields (core count, QPU count, VPM size, HW version) if we want to
  print something human-readable later. Also now returns `CT0CS`/`CT1CS`
  in R4/R5 (added to check a freshly-reset thread's actual state
  *before* any submission ever happens - see "M3, attempt 2" below).
- `V3D_TestBuffer` (0xC0042) — allocates a physically-contiguous page
  (see "M2 in progress" above), writes/reads back a test pattern, and
  returns physical address (R0), the readback value (R1, should be
  `0xDEADBEEF` on success), and the logical address (R2).
- `V3D_ClearTest` (0xC0043) — takes a clear colour in R0, runs the
  clear-to-colour control list (see "M2 continued" above), returns the
  render target's physical address (R0) and the read-back pixel (R1,
  should equal the colour passed in on success). Requires `V3D_PowerOn`.
- `V3D_ClearToSprite` (0xC0044) — same clear-to-colour job as
  `V3D_ClearTest`, but the render target is prefixed with a valid RISC
  OS sprite area+header (see "M3" below) so the GPU writes straight
  into sprite-ready memory. Takes a clear colour in R0, returns the
  sprite area's *logical* address (R0 - pass directly to `OS_SpriteOp`
  from any task), the read-back pixel (R1), and the physical address
  (R2, diagnostic only). Requires `V3D_PowerOn`.
- `V3D_MemSplit` (0xC0045) — queries the VideoCore firmware mailbox
  (property tags `0x00010005`/`0x00010006`, "Get ARM memory"/"Get VC
  memory") for the ARM/GPU physical memory split, to check directly
  whether there's an address ceiling the GPU can't reach (see "M3,
  attempt 7" below). Returns ARM base (R0), ARM size (R1), VC base
  (R2), VC size (R3). Does not require `V3D_PowerOn` - this is
  firmware-level, unrelated to the V3D block's own power state.
- `V3D_ScreenInfo` (0xC0048) — step 1 of the planned GraphicsV Render
  accelerator (see "GraphicsV Render accelerator" below): reads the
  OS's own live screen-mode state via `OS_ReadVduVariables`, nothing
  V3D-specific, no hardware touched, no `V3D_PowerOn` required. Returns
  ModeFlags (R0), Log2BPP (R1), LineLength in bytes (R2), XWindLimit
  (R3, rightmost pixel index), YWindLimit (R4, topmost pixel index),
  ScreenStart (R5, logical address of the currently-displayed bank),
  DisplayStart (R6, logical address of the bank being written to),
  TotalScreenSize (R7, the OS's own authoritative size of the
  genuinely-allocated screen memory in bytes - added after step 3's
  framestore-contiguity check hit "Address not recognised" using its
  own derived `LineLength*(YWindLimit+1)` estimate instead, see below).
  See `test/ScreenInfoTest.bas`.
- `V3D_FillTest` (0xC0049) — step 2 of the planned GraphicsV Render
  accelerator (see "GraphicsV Render accelerator" below): a multi-tile
  solid-colour fill into a synthetic PMP test buffer (not the real
  screen yet). Takes width (R0), height (R1), colour (R2). Returns
  physical address (R0), row stride in bytes (R1, may exceed
  `width`*4 - the buffer is padded to a whole tile grid), tiles_x (R2),
  tiles_y (R3), and a *logical* address (R4) for direct sampling.
  Requires `V3D_PowerOn`. See `test/FillTest.bas`.
- `V3D_ScreenFillTest` (0xC004A) — step 3 of the planned GraphicsV
  Render accelerator (see "GraphicsV Render accelerator" below): the
  same multi-tile fill, but writing DIRECTLY into the real, live screen
  framestore - the first SWI in this project with a visible effect on
  the actual desktop. Takes x (R0), y (R1), width (R2), height (R3),
  colour (R4) in screen pixel coordinates (row 0 = whatever's first in
  memory at `ScreenStart` - not yet reconciled with GraphicsV's own
  bottom-left-origin convention, see cl_fill.h). Returns tiles_x (R0),
  tiles_y (R1). Requires `V3D_PowerOn`; only supports 32bpp modes; the
  first call verifies the whole screen framestore is physically
  contiguous (see screen_info.h) and caches the result. See
  `test/ScreenFillTest.bas`.
- `V3D_AccelStart` (0xC004B) — step 4 of the planned GraphicsV Render
  accelerator (see "GraphicsV Render accelerator" below): claims
  `GraphicsV` (vector 42) for real, via `OS_Claim`. **From this point
  this driver intercepts every `GraphicsV` call system-wide**, not just
  calls this module's own test SWIs make - the highest-stakes SWI in
  this project so far. Deliberately explicit opt-in, not auto-claimed
  on module load. Idempotent (safe to call again if already claimed).
  No arguments, no return values.
- `V3D_AccelStop` (0xC004C) — releases `GraphicsV` again (`OS_Release`),
  returning to normal software rendering. Idempotent. Also called
  automatically, unconditionally, from `module_final`.
- `V3D_AccelStats` (0xC004D) — real instrumentation for the claimed
  accelerator (see gfxv_accel.c): total handler invocations (R0), how
  many were reason 13/Render (R1), how many were op 2/fill (R2), how
  many had a genuine solid ECF pattern (R3), how many were actually
  accelerated by the GPU (R4), how many were attempted but failed (R5).
  See `test/AccelTest.bas`.
- `V3D_AccelFailInfo` (0xC004E) — added after a first live-desktop test
  showed a high failure rate, to get real diagnostic data instead of
  guessing at the cause (see "Step 4" below for what it found). Last
  failure's error number (R0) and exact rectangle - left/bottom/right/
  top (R1-R4) - plus min/max pixel area seen among failures (R5/R6)
  and successes (R7/R8); `0xFFFFFFFF` for a min means "none seen".
- `V3D_CopyTest` (0xC004F) — first step of op 1 (copy, see "Op 1:
  rectangle copy" below): copies a `width` (R0) x `height` (R1)
  rectangle from a synthetic source buffer (filled with a per-pixel
  test pattern) to a synthetic dest buffer via the GPU, not the real
  screen yet. Returns source logical address (R0), dest logical
  address (R1), shared row stride in bytes (R2), tiles_x (R3), tiles_y
  (R4). Requires `V3D_PowerOn`. See `test/CopyTest.bas`.
- `V3D_ScreenCopyTest` (0xC0050) — second step of op 1 (copy, see
  "Copy step 2" below): copies a `width` (R4) x `height` (R5)
  rectangle from `(src_x, src_y)` (R0, R1) to `(dest_x, dest_y)` (R2,
  R3) directly on the real screen framestore. Refuses with a clear
  error if the two rectangles overlap. Returns tiles_x (R0), tiles_y
  (R1). Requires `V3D_PowerOn`. See `test/ScreenCopyTest.bas`.
- `V3D_AccelCopyStats` (0xC0051) — op 1 instrumentation for the claimed
  accelerator (see "Copy step 3"/"Overlapping copy" below): total op-1
  calls seen (R0), how many were overlapping (R1, now accelerated via
  the scratch-buffer path rather than always skipped), how many
  non-overlapping copies were accelerated directly (R2) and how many
  of those failed (R3), how many overlapping copies were accelerated
  via the scratch-buffer path (R4) and how many of those failed or
  were too large (R5), and the last failure's error number from either
  path (R6). See `test/AccelTest.bas`.
- `V3D_OverlapCopyTest` (0xC0052) — isolated test for the overlap-safe
  copy path (see "Overlapping copy" below): a single synthetic buffer
  with a `width` (R0) x `height` (R1) source rectangle at `(0,0)` and a
  destination rectangle at a deliberately sub-tile `(shift_x, shift_y)`
  (R2, R3) - not the real screen yet. Returns the buffer's logical
  address (R0), row stride (R1), tiles_x (R2), tiles_y (R3). Requires
  `V3D_PowerOn`. See `test/OverlapCopyTest.bas`.
- `V3D_ScreenCopyOverlapTest` (0xC0053) — real-framestore counterpart
  (see "Overlapping copy" below): R0=src_x, R1=src_y, R2=dest_x,
  R3=dest_y, R4=width, R5=height, deliberately allowed to overlap
  (unlike `V3D_ScreenCopyTest`, which refuses). Returns tiles_x (R0),
  tiles_y (R1). Requires `V3D_PowerOn`. See
  `test/ScreenCopyOverlapTest.bas`.

## M3: displaying it (2026-09-24, build-verified, not yet hardware-tested)

A kernel module can't easily run its own Wimp event loop (`Wimp_Poll`
expects a normal foreground task, not something living inside the
kernel), so M3 splits across two pieces: the module (`V3D_ClearToSprite`)
produces a buffer that's already a valid sprite, and a separate BASIC
program (`test/ShowSprite.bas`) does the Wimp/window side using
ordinary, decades-stable RISC OS APIs.

That split works cleanly because of something specific to how M2's
buffers are allocated: `V3D_ClearToSprite` returns a *logical* address
from a Physical Memory Pool dynamic area (see `phys_buffer.h`), and
dynamic areas live in RISC OS's shared logical address space rather
than a per-task Application Space - so the pointer means the same
thing in `ShowSprite.bas`'s task context as it does inside the module.
No copy between address spaces needed.

The sprite format itself (`c/sprite.h/.c`) is different territory from
the V3D register work above - old, stable, thoroughly documented RISC
OS API rather than exotic hardware bring-up - but the same "verify,
don't infer from memory" discipline was applied anyway: every field
(sprite area header, sprite header, the RISC OS 3.5-style mode word,
window block layout, window/icon flag bit positions, the redraw-loop
rectangle data block, `OS_SpriteOp`'s area-value encoding) was checked
against ROOL's own PRM pages rather than recalled, and the sprite
header struct's byte layout is enforced with a compile-time size
assertion rather than just trusted.

**Hardware result, attempt 1 (2026-09-24): binning timeout again** -
`CS=0x50` (stopped at halt, no error), the same symptom the M2
restart-poll fix (commit a29ac06) was meant to have already solved.
The difference this time: it happened on a **freshly-loaded module**,
where the CLE thread should never have been halted to begin with.

That distinction was the actual lead: `v3d_restart_thread` was writing
the `CTSUBS` restart trigger *unconditionally* before every submission,
not only when the thread actually needed restarting. The PRM only
documents `CTSUBS` as clearing `CA`/`EA` when a thread genuinely leaves
"stopped at halt" - writing it to a thread that was never halted (a
freshly-reset, idle CT0) is outside what's documented, and evidently
doesn't leave `CA`/`EA` alone either. So the fix that solved one race
was, on a clean run, *creating the same race from scratch* by
disturbing a thread that didn't need touching. Fixed: `CT0CS`/`CT1CS`
are now read first, and the restart write only happens when the thread
is actually stopped with `CTSUBS` or `CTERR` set - a running or
cleanly-idle thread is left alone entirely. (Separately: the BASIC-side
infinite error-handler recursion bug that obscured this result on the
first hardware attempt is fixed - see `test/ShowSprite.bas`'s history.)

**Hardware result, attempt 2 (2026-09-24): binning timeout again, but a
genuinely new symptom.** `CS=0x20` (`RUN`, no error/halt - the
conditional-restart fix worked, this specific race is gone) with
`CA≈0x1` (nowhere near any of our buffers) and `QEA` reading back
`0xFFFFFFFF` - a fault-like value never seen before. Raised a question
that had never actually been tested in isolation: does a freshly-reset
CT0 default to `CTRUN=1` for reasons unrelated to any real submission?
If so, `v3d_restart_thread`'s "skip restart if already `CTRUN`" logic
would wrongly treat a fresh, `CA`/`EA`-still-garbage thread as
"already fine" and write new `QBA`/`QEA` onto it without ever properly
initialising it - which would explain both symptoms (garbage `CA`,
faulting `EA` readback) at once.

Rather than patch this on another guess, extended `V3D_ReadIdent` to
also return `CT0CS`/`CT1CS` (R4/R5) - a way to check the post-reset
thread state directly, using an already-proven-working SWI, without
ever attempting a submission.

**Hardware result, attempt 3 (2026-09-24): `CT0CS=0`/`CT1CS=0` right
after power-on.** Both threads cleanly idle - no `CTRUN`, no halt, no
error. This directly disproved the "fresh thread defaults to running"
theory: the conditional-restart logic was correctly reading a clean
state and correctly skipping the restart write. That path was never
the bug.

**Hardware result, attempt 4 (2026-09-24): reran `V3D_ClearToSprite`
on the same clean session - identical `CA≈1`/`QEA=0xFFFFFFFF`
symptom, reproducibly.** Not session-dependent noise - something
structural about this specific path causes it, every time. That
pointed at a gap in the diagnostics rather than another guess at the
fix: every timeout so far had only ever checked `V3D_CTL_INT_STS`, the
*per-core* interrupt status - which has no MMU-related bits at all.
MMU violations are reported separately, at the *hub* level
(`V3D_HUB_INT_STS`'s `MMU_WRV`/`MMU_PTI`/`MMU_CAP` bits, confirmed from
the real Linux kernel header). The bare-metal reference never sets up
the V3D MMU and assumes physical addresses pass straight through to
the GPU untranslated - true for it, since it boots from scratch with
nothing else touching the chip, but not necessarily true here: if RISC
OS's own firmware/GPU init left the MMU enabled with a page table that
doesn't cover arbitrary system RAM our PMP hands out, a perfectly
valid CPU-side physical address could be one the GPU simply can't
reach - which would produce exactly this symptom (thread genuinely
running, but unable to fetch anything coherent).

Added `V3D_MMU_CTL` (bit 0 says definitively whether the MMU is even
active - the first and most important new fact), plus
`V3D_HUB_INT_STS`/`V3D_MMU_VIO_ID`/`V3D_MMU_VIO_ADDR` (only read when
the MMU is actually enabled, to keep the common case short) to
`diagnose_timeout()`.

**Hardware result, attempt 5 (2026-09-24): `MMU=0x00000000` - ruled
out too.** MMU genuinely disabled, so not an address-translation
problem either.

**Hardware result, attempt 6 (2026-09-24) - same call, run a second
time without reloading the module: the most informative result yet.**
`CA=0x03070000` and `EA=0x0307006B` - both matching `wQBA`/`wQEA`
*exactly*. `QBA`/`QEA` write and readback also match exactly. No stale
garbage, no zeroed registers, no fault pattern - the submission
mechanics are completely correct, the thread is confirmed `RUN`ning,
and it *still* never completes, with `CA` stuck precisely at the
starting address the whole time - not advanced at all, not wandered
off into unrelated memory. That rules out everything about *how* the
job is submitted and points squarely at what the binner does once it
starts reading the list.

Found a real, previously-unused pair of variables while looking:
`tile_alloc_log`/`tile_state_log` were fetched from `pmp_alloc` and
then explicitly discarded (`(void) tile_alloc_log;`) - these scratch
buffers were never actually zeroed before being handed to the GPU. The
bare-metal reference gets away with this because its memory comes
straight from a fresh boot, pristine zero from power-on; RISC OS
instead hands out recycled physical pages via `OS_Memory 12`, which
can carry arbitrary leftover content from whatever last used them. If
the binner's own bookkeeping in that scratch space is sensitive to its
initial content, that would explain a hang whose presence depends on
*which* physical pages happen to get allocated - consistent with this
never reproducing on `V3D_ClearTest`'s page range, only
`V3D_ClearToSprite`'s. Zeroed both buffers before submission.

**Hardware result, attempt 6 (2026-09-24): identical symptom after
zeroing - ruled out too.** Same exact-match `CA`/`EA`/`QBA`/`QEA`
pattern, thread confirmed running, still stuck precisely at the start
address. Garbage scratch-buffer content wasn't it either.

**Hardware result, attempt 7 (2026-09-24) - the decisive one.** Rather
than guess a fourth time, tested something structural instead: reran
`V3D_ClearTest` - previously 100% reliable, never once failed - in the
*same session*, after enough testing that its next allocation would
also land at a higher physical address than its earlier successful
runs. It failed identically to `V3D_ClearToSprite`. That's conclusive:
this was never about which SWI or which control-list content - it's
address-range dependent. Physical addresses climb over a session
because every distinct SWI claims its own physical pages and never
releases them across calls to *other* SWIs, so the longer a session
runs, the higher up in RAM new allocations land - and somewhere around
here, the GPU stops being able to reach them.

That's classic Broadcom "GPU memory split" territory - VideoCore
firmware has historically reserved a specific, separate window of
physical RAM for its own/GPU use, with the rest belonging exclusively
to the ARM side. Added `V3D_MemSplit`, which queries the firmware
mailbox directly (property tags `0x00010005`/`0x00010006`, "Get ARM
memory"/"Get VC memory" - standard, well-documented Raspberry Pi
firmware protocol, message format is empty request → base+size
response) for the actual split, rather than continuing to infer a
boundary empirically through trial and error. Not yet tested on
hardware - this is the answer we've been chasing indirectly for seven
attempts, so worth getting real numbers before changing any more code.

**Hardware result, attempt 8 (2026-09-24): `V3D_MemSplit` itself hung -
"mailbox stayed empty waiting for memory-split reply".** Our own raw
mailbox request got no reply at all - a third, independent problem
(the mailbox protocol implementation itself) surfaced by trying to
diagnose the second one. Root-caused by reading RISC OS's own
`BCMSupport` module source (real code, fetched from
gitlab.riscosopen.org/RiscOS/Sources/HWSupport/BCMSupport 2026-09-24,
not guessed): `BCMSupport` already claims this mailbox's hardware IRQ
at boot (`c/mailbox.c`, `OS_ClaimDeviceVector` + `HAL_IRQEnable`), and
its interrupt handler drains *every* reply out of the read FIFO the
instant the GPU firmware raises the interrupt - silently discarding
any reply it doesn't recognise, i.e. anything not sent through its own
API. A second poller reading the same registers directly cannot win
this race (IRQ latency is far below any poll granularity), so our
request's reply was being eaten by `BCMSupport` before our own polling
loop ever ran. This confirms and finally explains the pre-existing
caveat comment that was already sitting in `v3d_regs.h` ("RISC OS's own
Firmware module already implements this channel... prefer calling into
it"). It also means the direction, not just the symptom, was wrong:
raw mailbox access from a second module is fundamentally unsafe here,
not merely mistimed.

**Fix**: `BCMSupport` exports a SWI for exactly this,
`BCMSupport_SendTempPropertyBuffer` (same source: `c/BCMSupport.c`,
`case SWINO(SendTempPropertyBuffer)`) - R0/R1 = request/response buffer
pointers (standard property-tag message format, word-aligned), R2 = 0.
It copies the request into its own buffer, sends it, blocks on its
*own* IRQ-driven response queue (so it's guaranteed to see the reply,
unlike us), copies the result back, and frees the buffer. Rewrote
`memsplit.c` to look up this SWI at runtime via
`OS_SWINumberFromString` (verified against OSLib's own
`xos_swi_number_from_string` binding rather than guessed) instead of
touching `VIDEOCORE_MBOX` registers at all - no physical/NCNB buffer of
our own is needed any more, so `memsplit.c` no longer depends on
`phys_buffer.h`. Not yet tested on hardware.

(Also fixed, while investigating: the raw mailbox code, before it was
deleted, had been polling the wrong status register - `MAIL0_STA`
before writing, when the send side is `MAIL1_STA` at a different
offset - confirmed against the Linux kernel's own
`drivers/mailbox/bcm2835-mailbox.c`. Moot now that nothing touches the
mailbox directly, but left documented in `v3d_regs.h` in case anything
here ever needs raw mailbox access again.)

**Hardware result, attempt 9 (2026-09-26): `V3D_MemSplit` worked -
`ARM base=0 size=0x3C000000, VC base=0x3C000000 size=0x4000000`.** Real
numbers at last: a 1GB board, ARM gets the bottom 960MB, VideoCore
firmware reserves the top 64MB from `0x3C000000` up. This is the
answer attempt 7 was chasing - and it disproves attempt 7's own theory.
Every hang logged so far (attempts 1-6, and the decisive attempt-7
retest) happened with `CA`/`EA`/`QBA`/`QEA` around `0x03000000`-
`0x030C0000` - roughly 48-50MB in. That's nowhere near the 960MB
ARM/VC boundary; a physical-address-climbs-into-GPU-territory theory
cannot explain a hang at 1/20th of that boundary. The correlation
attempt 7 found (later calls in a session fail, earlier ones don't) was
real, but the "why" was wrong.

Re-reading `v3d_restart_thread()` (`c/cl_clear.c`) against the actual
evidence points at a better explanation: it deliberately does nothing
when `CS` shows `CTRUN` set, treating that as "already running, leave
it alone" (added earlier after writing CTSUBS unconditionally
reproduced the same hang on an already-fine thread). But `CTRUN=1`
just means "running" - it doesn't distinguish "genuinely busy" from
"hung mid-job", and a `BFC`/`RFC` poll timeout is exactly the case
where `CTRUN=1` almost certainly means the latter. Nothing anywhere
in this codebase ever cleared that condition, so once *any* submission
timed out with `CTRUN` still set, the CLE thread stayed wedged in that
state for the rest of the session: every later submission's
`restart_thread()` call keeps declining to touch it (any SWI, any
buffer address), which is consistent with everything logged, including
a previously-100%-reliable `V3D_ClearTest` failing identically once run
after `V3D_ClearToSprite` had already wedged the thread earlier in the
same session.

**Fix**: `diagnose_timeout()` now attempts recovery, not just
diagnosis. If `CS` shows `CTRUN` set at the moment of a timeout, it
writes `V3D_CT_CS_CTRUN` back (per its own documented R/W meaning:
writing 1 *stops* the thread, never starts one - see the bit's comment
in `v3d_regs.h`) and polls briefly for it to actually clear, so the
*next* submission's `restart_thread()` finds a genuinely stopped thread
(`CTSUBS`/`CTERR` set) instead of a permanently wedged `CTRUN=1` it
will never act on. The outcome (`REC=stopped` / `REC=timeout`) is
folded into the existing diagnostic message. Not yet tested on
hardware - if this is right, a `V3D_ClearTest` run immediately after a
timeout should now succeed instead of failing identically.

**Hardware result, attempt 10 (2026-09-26): the CTRUN-recovery fix got
its first real test, and the result rules out the theory behind it
too.** `V3D_ClearToSprite &FF00FF00` on a *freshly-loaded module's
first-ever GPU call this session* (no prior invocation could have
wedged anything) timed out on binning:
`CS=0x00000020(RUN) REC=timeout MMU=0x00000000 CA=0x03049000
EA=0x0304906B QBA=w0x03049000/r0x03049000 QEA=w0x0304906B/r0x0304906B
tAl=0x0304F000 rTg=0x0304A000 tSt=0x03052000`. Two things this rules
out directly:

- `CA`/`EA` **exactly match** the `QBA`/`QEA` just written - this is
  not the stale-old-job symptom the recovery fix targeted (that would
  show `CA`/`EA` from a *previous* run, not this one). The thread
  picked up the fresh job and immediately stalled on it.
- This was the first GPU call since `*RMLoad`, so there is no "earlier
  timeout left it wedged" history to recover from - the session-wedge
  theory from attempt 9 cannot explain a hang on the very first call.
- `REC=timeout`: the stop-request itself didn't take within its poll
  bound. Whatever this is, it isn't a soft leftover CS flag - it's a
  core that isn't even servicing a stop request, consistent with a
  genuine stall rather than an idle-but-mislabelled thread.

That last point matches a gap this project's own diagnostics already
flagged but never actually wired up: `diagnose_timeout()` was reading
`V3D_CTL_INT_STS` into `sts` the whole time, then silently discarding
it (`(void) sts;`) instead of printing it - and this file's own
pre-existing comment on `V3D_INT_OUTOMEM` (source: the real Linux
kernel driver) says exactly this: *"a bin stalled on OOM looks
identical from BFC's perspective to one that never started at all"* -
i.e. exactly `CA==QBA`, `BFC` never advancing. That's a real,
previously-unread piece of hardware-reported evidence sitting right
there, not a new guess. Fixed `diagnose_timeout()` to actually print
`STS=` (with `(OOM)`/`(GMPV)` decoded) instead of throwing it away. Not
yet tested on hardware - next timeout's message will say whether the
binner is out of tile-allocation memory or something else entirely.

**Hardware result, attempt 11 (2026-09-26) - bisection against the
exact M2-verified commit.** Attempt 10's `V3D_ClearTest` hang (not just
`ClearToSprite`) meant this was no longer sprite-specific - a real
regression somewhere, or something environmental. Rather than keep
reading code, built and shipped the *exact* binary from commit
`200aa5d` ("M2 confirmed working on hardware... verified correct") -
before any of the later `v3d_restart_thread`/`diagnose_timeout`
iterations existed - and ran it fresh on the same hardware. **It hung
too**, with its own (older, simpler) diagnostic:
`STS=0x00000000 CS=0x00000050 (STOP) CA=0x00000000 EA=0x00000000
QBA=w0x03066000/r0x00003000 QEA=w0x0306606B/r0x03065002
tAlloc=0x0306B000 rTgt=0x03067000 tState=0x0306E000`. This rules out
every code change made since M2 as the cause - the identical bytes
that were "byte-exact, 100% reliable" on this project's own earlier
hardware session no longer work today, on the same board. Whatever
this is, it isn't in `cl_clear.c`'s restart/recovery logic.

That reframes the question: what differs about *this session's
hardware/firmware state* versus whatever M2's success depended on?
One real candidate, not yet checked at all: nothing in this driver has
ever asked the firmware to turn the V3D core's clock on.
`v3d_power_on()` only pulses `PM_GRAFX`'s reset and releases the ASB
gates - a different, lower-level thing than the Raspberry Pi
firmware's own independent clock-gating per hardware block. A V3D
block with no clock running would still answer plain register
reads/writes correctly (explaining why `V3D_ReadIdent` and
`V3D_TestBuffer` have always worked) while never advancing a state
machine that depends on that clock ticking - which looks exactly like
what's been observed: `CTRUN` sets, `STS` never reports an error,
nothing ever completes. M2 may simply have run at a moment when the
firmware happened to already have the V3D clock on for unrelated
reasons; nothing here has ever controlled it directly.

Added `V3D_ClockInfo` (0xC0046) to check this - queries
`GetClockState`/`GetClockRate`/`GetMaxClockRate` for `ClockID::V3D`
(=5) via `BCMSupport_SendTempPropertyBuffer` (tag numbers and clock ID
cross-checked against both the bare-metal reference project and
BCMSupport's own real header, in agreement). Diagnostic only - it does
not yet force the clock on, deliberately, so the first hardware result
is real data rather than a fix layered on a guess. Refactored the
mailbox-call helper out of `memsplit.c` into `bcmsupport.c` so both
SWIs share it. Not yet tested on hardware.

**Hardware result, attempt 12 (2026-09-26) - confirmed.**
`V3D_ClockInfo` returned `state=0 rate=250000000 max_rate=500000000` -
`state=0` means the firmware has V3D's clock gated *off* right now;
`rate`/`max_rate` are just remembered/available values, not evidence
of anything running. This is a clean, direct confirmation of attempt
11's theory, not an inference: the GPU register block that has always
answered plain reads/writes correctly (`V3D_ReadIdent`,
`V3D_TestBuffer`) simply has no clock driving its CLE state machine,
which explains every hang on record without needing address ranges,
session history, or control-list content to be wrong at all.

**Fix**: `v3d_power_on()` now calls `v3d_ensure_clock_on()`
(`v3d_clock.c`) *before* the `PM_GRAFX` reset pulse - turning the clock
on is a prerequisite for everything else this function does, not a
follow-up. It requests `SetClockState(V3D, 1)`, then only calls
`SetClockRate` if the firmware has no rate remembered at all (this
hardware already had one - `250000000` - so that path won't even fire
here, but it's there for a from-cold-boot firmware that hasn't been
asked for a V3D rate yet). `V3D_PowerOn` now also returns the
confirmed running clock rate in R1. Not yet tested on hardware - if
this is right, `V3D_ClearTest` should now succeed on the very first
call after `V3D_PowerOn`, on this exact board, without touching
anything else.

**Hardware result, attempt 13 (2026-09-26) - CONFIRMED FIXED.** Fresh
reboot, `V3D_PowerOn` (returned `ok=1 rate=250000000`), then
`V3D_ClearTest &DEADBEEF` straight after with nothing else run first:
`phys=0x0304A000 readback=DEADBEEF` - exact match. This closes out the
entire hang investigation that ran across attempts 1-13: the root
cause was never address ranges, session history, control-list content,
or the restart/recovery logic added and iterated on along the way - it
was a firmware clock gate that this driver had simply never asked the
firmware to open, present from the very first hardware test and
intermittent only in the sense that whatever state the firmware
happened to leave that clock in varied across boots/sessions. M3
(sprite display) can now resume on solid ground.

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
6. M3 - actually see it: from the desktop, load `test/ShowSprite.bas`
   into BASIC (`LOAD "ShowSprite"` then `RUN`, with `V3DDriver,ffa`
   already `*RMLoad`ed) - see "M3" above for what to expect.

**Hardware result, attempt 14 (2026-09-26) - M3 CONFIRMED.** Ran
`ShowSprite.bas` by double-clicking it from the Filer (not from a Task
Window - see below for why that distinction matters). A window opened
showing a solid yellow 64x64 area, and closed cleanly with no error.
Decoded the colour to make sure it was actually correct rather than a
bug: the sprite is type 6 (32bpp **T-B-G-R**, per `sprite.c`) - alpha
in bits 31-24, blue 23-16, green 15-8, red 7-0. `&FF00FFFF` in that
layout is alpha=FF, blue=00, green=FF, red=FF - full red and green, no
blue - which is exactly yellow. The GPU wrote precisely what was
asked for and the Wimp displayed it correctly; the earlier
expectation of a different colour was just an unmatched assumption
about hex-to-channel mapping, not a bug. Full pipeline confirmed
end-to-end on real hardware: `V3D_PowerOn` (clock-enable + reset) ->
`V3D_ClearToSprite` -> `OS_SpriteOp` plot in a redraw handler ->
correct on-screen colour -> clean window close.

**One real gotcha found along the way, not a driver bug**: an earlier
attempt to run this same script from inside a Task Window failed with
"Window Manager is currently in use" (RISC OS error, sourced to
`TaskWindow`'s own veto of `Service_WimpCloseDown` for what it
considers its own child task - confirmed by reading
`TaskWindow`'s real source, not guessed). Cross-checked against RISC
OS's own documentation: Task Windows are explicitly for programs that
*don't* call `Wimp_Poll`/`Wimp_Initialise` - a real Wimp application
like `ShowSprite.bas` needs to run as an independent task (e.g. via
the Filer, as done here, or `Wimp_StartTask`), not inside a Task
Window. Noted here so this doesn't get re-investigated as a code bug
in a future session.

**Hardware result, attempt 15 (2026-09-26) - reboot-free reliability:
regressed, then re-diagnosed.** `StressTest.bas` failed on its very
first iteration - immediately after the just-successful `ShowSprite`
run, in the same session, no reboot in between. Narrowed with a
minimal, isolated repro (fresh reboot, just two plain `PowerOn`+
`ClearTest` pairs back to back): the first call always works; the
second, immediately after, reliably hangs - `CS=(RUN)`, `BFC` frozen,
`STS=0` (no OOM/GMPV), `REC=timeout` (the stop-request recovery
doesn't take), and this time `CA`/`EA` correctly matched *this*
submission's own freshly-written addresses (not a stale leftover job -
ruled out separately), so the CLE is picking up a fresh, correctly-
addressed job and simply never starting it.

Checked whether the reset itself was the problem first, since that's
the cheaper thing to rule out: `V3D_ReadIdent` right after a "second"
`V3D_PowerOn` (before touching `ClearTest` at all) reported
`CT0CS=0 CT1CS=0` - a genuinely clean reset. So the reset pulse itself
is fine on repeat calls; something about the *submission* after a
repeat power-on is different.

The one concrete difference between call 1 and call 2: on call 1 the
firmware clock was confirmed off, so `v3d_ensure_clock_on()`'s
`SetClockState(V3D, 1)` was a real, meaningful transition; on call 2
the clock is already on, and the code re-issued that same "turn on"
request unconditionally anyway. If the firmware treats a redundant
enable as a brief re-init of an already-stable clock rather than a
true no-op, a hardware reset pulse landing moments later on a
momentarily-disturbed clock would produce exactly this symptom, while
leaving every other register (including the reset check above) looking
completely normal. Fixed `v3d_ensure_clock_on()` to check
`GetClockState` first and only call `SetClockState` when the clock is
actually confirmed off. Not yet tested on hardware.

**Hardware result, attempt 16 (2026-09-26) - both clock theories ruled
out; found the real bug.** The idempotent-clock-enable fix made no
difference - byte-for-byte identical failure, identical addresses.
Then tested with the second call using entirely different, never-
before-used physical pages (`ClearToSprite`'s page range instead of
`ClearTest`'s): still hung, but this time with `CA`/`EA` showing the
*first* call's own addresses, not the second call's freshly-written
`QBA`/`QEA` at all. That's decisive: the CLE thread was never picking
up the new job - it was still sitting exactly where the first job left
it, regardless of which physical pages the new job used.

Root cause: `v3d_restart_thread()`'s guard treats "CS reads
`0x00000000`" as "nothing to do," but `CTRUN` clears itself the moment
a job completes normally - a job that just finished leaves `CS` reading
identically to a thread that has never run anything at all, while `CA`
sits at that completed job's own address rather than 0. The guard
can't tell the two states apart from `CS` alone, and picked the wrong
one: it left a completed-but-stale thread alone, so the next
submission's `QBA`/`QEA` write was never latched into `CA` at all.

Fixed using a discriminant `CS` genuinely doesn't have but `CA` does:
`V3D_ReadIdent` already showed `CA` reads exactly 0 on a truly virgin
thread. `v3d_restart_thread()` now only skips the kick when `CA==0`
*and* nothing is already latched in `CS` (preserving the historical v2
finding that unconditional `CTSUBS` breaks a genuinely fresh thread) -
any thread that has run a job before, however it stopped, always gets
the kick regardless of which `CS` bits happen to be set. Not yet
tested on hardware.

**Hardware result, attempt 17 (2026-09-26) - that fix made zero
difference, byte-for-byte identical failure (confirmed the newly-sent
module was actually the one running, then re-verified with an explicit
`V3D_ReadIdent` check between the two calls: `CT0CS=0` right before the
second submission - so the guard's logic really was being exercised as
designed, and still didn't help).** That ruled out the *condition*
being wrong and pointed at *when* it runs instead. `V3D_CT_CS_CTSUBS`
is documented to restart the thread "if `CA != EA`" - and right after a
job completes normally, `CA` and `EA` both already sit at that same
job's own end address: equal. Calling the kick *before* writing the
new `QBA`/`QEA` (as every version so far did, attempt 16's fix
included) means `CTSUBS`'s one-shot check evaluates two equal, stale
addresses and correctly does nothing - the new addresses arrive
moments later, but the restart condition already failed to fire.

Moved the kick to run *after* the new `QBA`/`QEA` are written instead:
by then `EA` holds the new job's end address while `CA` is still stuck
on the previous job's - genuinely different - so `CTSUBS`'s documented
condition actually holds when it's evaluated. The virgin-thread check
(`CA==0`, still needed - see v2's history) is now taken by the caller
before the job-setup writes, since that's the only point where "has
this thread ever run before" is still knowable; the kick itself moved
to `v3d_kick_thread()`, called only for non-virgin threads, after
`QBA`/`QEA`. Not yet tested on hardware.

**Hardware result, attempt 18 (2026-09-26) - the reorder fix also made
zero difference, byte-for-byte identical failure again.** Four attempts
at this mechanism (fixed-delay, CS-gated, CA-gated, reordered) had now
all failed identically. Noticed something in the diagnostic that hadn't
been weighed properly: `QBA`'s own *readback* didn't match what was
just written, even in the very first failing test, well before any
reordering existed - a sign the problem wasn't really about ordering or
gating logic at all, but about the whole mechanism.

Went back to the primary source properly this time instead of reasoning
from this project's own prior comments about it. Fetched Broadcom's
"VideoCore IV 3D Architecture Reference Guide" PDF directly
(docs.broadcom.com/doc/12358545) and read the actual `V3D_CTnCS`/
`CTnEA`/`CTnCA` register tables: that document has **no `QBA`/`QEA`/
`QMA`/`QMS` registers at all** - it's for an older V3D generation that
submits jobs by directly writing `CTnCA`/`CTnEA`, where `CTSUBS`
genuinely means what this project's comments assumed. The Pi 4/CM4
(V3D 4.2) is a later generation with a different, queue-based
submission model - `CTnCA`/`CTnEA` are plain progress-monitoring
registers there, and `QBA`/`QEA`/`QMA`/`QMS` (a completely different
set of registers, different offsets) are what you actually write to
submit. Every version of the restart/kick logic had been applying an
older architecture's documented semantics to registers that don't work
that way on this hardware - explaining why no version of it, however
carefully reasoned, ever changed the outcome.

Confirmed the correct model by reading the actual, shipping Linux
kernel `v3d` driver for this exact hardware generation
(`drivers/gpu/drm/v3d/v3d_sched.c`, `v3d_bin_job_run`/
`v3d_render_job_run`, fetched from
raw.githubusercontent.com/torvalds/linux 2026-09-26): it submits every
job - first, second, thousandth, no exceptions - with nothing but
`QMA`/`QMS`/`QTS` (if present) then `QBA` then `QEA`. No `CS` read, no
`CTSUBS`, no state check, no restart logic of any kind. Its own
comment: *"Writing the end register is what starts the job."* Removed
the entire kick mechanism (`v3d_kick_thread` and everything before it)
and the virgin-thread `CA` check - `v3d_clear_run` now does exactly
what the real driver does, nothing more. Not yet tested on hardware.

**Hardware result, attempt 19 (2026-09-26) - even with the restart
mechanism fully removed, identical failure. Also ruled out the SWI
identity mattering**: tested `ClearToSprite` twice in a row (no
`ClearTest` involved at all) - first call succeeds exactly as always,
second call hangs identically. So it's not "`ClearTest` specifically as
job 1", not the restart/kick logic (gone entirely now), not addresses,
not the clock - purely "any second real GPU submission in a session,
regardless of which SWI either call was."

Looked again at the real Linux v3d driver's `v3d_bin_job_run()` for
anything this driver doesn't do that it does, unconditionally, for
every job. Found `V3D_PTB_BPOS` (`0x30c` from the core base, confirmed
against the real kernel header) - written to 0 before *anything else*
in the bin submission, including the cache flush, with the driver's
own comment: *"Clear out the overflow allocation, so we don't reuse the
overflow attached to a previous job."* This driver never wrote this
register at all - not once, ever. "The overflow attached to a previous
job" is exactly the scenario stuck on for nineteen attempts now, and
this is the first concrete, named mechanism in the real driver that's
specifically about job-to-job state, rather than per-job setup. Added
the same unconditional `BPOS=0` write in the same place (before the
cache flush, before `QMA`/`QMS`/`QTS`/`QBA`/`QEA`). Not yet tested on
hardware.

**Hardware result, attempt 20 (2026-09-26) - `BPOS` made no difference
either. Then found the real bug, and it wasn't in the submission path
at all.** Every test across all nineteen attempts had followed the same
pattern: `V3D_PowerOn` → submit → `V3D_PowerOn` again → submit - always
re-calling `PowerOn` before the second submission, out of habit (and
because it's genuinely required before the *first* one). Tested,
finally, the one thing that had never actually been isolated:
`V3D_PowerOn` once, then two submissions with **no** second `PowerOn`
in between. Both succeeded, byte-exact, on entirely different physical
addresses. That's the whole answer: calling `V3D_PowerOn` a *second*
time - re-pulsing a hardware reset on an already-warm, already-clocked,
already-working V3D core - is itself what was breaking everything.
Every one of attempts 9 through 20 was a well-grounded fix for a
symptom of a trigger reintroduced by the test methodology itself, not
a bug in the code under test.

The irony: the exact assert-then-deassert reset-pulse-on-every-call
behavior in `v3d_power_on()` was originally added (long before the
clock-gate investigation) specifically to *fix* a wedged-thread problem
by forcing a "genuine pulse every call" - reasonable at the time, wrong
in hindsight now that the real cause of early hangs (the firmware
clock gate) is understood and fixed independently. Re-pulsing reset on
an already-good core doesn't recover anything; it's what breaks a
previously-working one.

**Fix**: `V3D_PowerOn` is now idempotent. The full reset/gate/clock
sequence runs only on the genuinely first call in a boot; every call
after that is a no-op that just reports back the already-confirmed
clock rate. Real callers only need to call it once anyway, but
defensively calling it again - completely reasonable for real code to
do - must not silently break the GPU. `BPOS`-clearing is kept (harmless,
and matches the real driver unconditionally), even though it wasn't
the actual fix. Not yet tested on hardware.

**Hardware result, attempt 21 (2026-09-26) - CONFIRMED FIXED, and
stress-tested.** `StressTest.bas` (50 iterations of `V3D_PowerOn` +
`V3D_ClearTest` + `V3D_ClearToSprite`, exact readback checked every
time, varying colour, no reboot) completed all 50 with no failures.
This closes out the entire "second call always hangs" investigation
(attempts 9-21): the fix holds under real repetition, not just a
one-off pair of calls. `V3D_PowerOn` is now genuinely safe to call
defensively as many times as real code likes, in addition to being
required before the first submission.

## Real IRQ handling (started 2026-09-26)

The bin/render waits (`v3d_poll_counter`) are currently bounded busy-
polls - fine for a test SWI, not for a real driver. Moving to real
interrupt-driven waiting, staged deliberately: this is a materially
different risk class from everything before it. Every bug so far has
been recoverable with `*RMKill` or a reboot; a genuinely broken
low-level interrupt/register handler risks destabilising the machine
more seriously. Researched thoroughly before writing any code (see
`c/v3d_irq.h`'s own comment for full sourcing) rather than guessing at
IRQ-claiming mechanics the way earlier register-level work sometimes
started from an educated guess:

- CMHG directive is `vector-handlers` (not `generic-veneers`, which is
  for a different, simpler C-callable-from-assembly convention) -
  confirmed against BCMSupport, a real shipped ROOL module using this
  exact directive for its own mailbox IRQ handler. Handler signature
  `int v3d_irq_handler(_kernel_swi_regs *r, void *pw)`, return 0 to
  claim / 1 to pass on (`VECTOR_CLAIM`/`VECTOR_PASSON`, confirmed from
  cmunge's own codegen source, not the PRM prose alone).
- Device number **10** (`iDev_GPU_VC3D`) for `OS_ClaimDeviceVector`.
  Not available through any documented enumeration API - RISC OS
  Open's own "List of HAL devices" page lists only Video/Audio/
  SysPeri/Comms/ExpCtl/HID categories, no 3D/GPU one - so this comes
  from `HAL_BCM2835`'s own private header, and is trusted only because
  it's cross-checked two independent ways: that header's own comment
  chain, and `HAL_BCM2835`'s GIC-400 driver mapping device 10 to GIC
  SPI 74, which matches the real upstream Linux device tree's `v3d`
  node exactly.
- The real, important finding: the PRM's own `OS_ClaimDeviceVector`
  page explicitly warns an interrupt handler must "strictly avoid the
  use of non re-entrant SWIs", because a normal SWI call from IRQ mode
  risks corrupting `R14_svc` if it interrupted other SVC-mode code
  mid-SWI. So `HAL_IRQClear` (needed *inside* the handler once real
  bin/render completion is wired up) can't be called via a plain SWI
  from there - it needs the routine's raw address plus a "static base"
  value (`R9`) resolved once via `OS_Hardware 1` outside interrupt
  context, called with `R9` set - hand-written register-level code,
  not yet implemented.

**Stage 1 (this commit)**: prove the claim/enable/disable plumbing is
safe before touching anything else. `V3D_IRQTest` claims the vector,
calls `HAL_IRQEnable`/`HAL_IRQDisable` via the safe `OS_Hardware 0`
SWI path (fine from ordinary SWI-handler context, deliberately *not*
used from inside the handler itself), releases the vector again, and
reports how many times the handler was actually invoked (`R0` - should
read 0, since no `V3D_CTL_INT_MSK_CLR` bit is unmasked anywhere in
this stage, so V3D cannot physically raise this device's IRQ line
yet). A nonzero count would mean a device-number collision or some
other surprise, to be understood before going further. Not yet tested
on hardware.

**Hardware result (2026-09-26) - CONFIRMED CLEAN.** Fresh reboot,
`V3D_PowerOn` then `V3D_IRQTest`: no error, `spurious count=0`. Vector
claimed, `HAL_IRQEnable`/`HAL_IRQDisable` both worked, released again,
handler never invoked (as expected - nothing unmasks a real interrupt
in this stage). Device number 10 is confirmed correct and the claim/
enable/disable/release plumbing is safe. Stage 2 next: the raw-pointer
+ static-base call for `HAL_IRQClear` from inside the handler, plus
unmasking `V3D_CTL_INT_MSK_CLR` and actually wiring bin/render
completion to the interrupt instead of the busy-poll.

**Stage 2a (this commit)**: everything needed for the handler to
service a real interrupt, built and testable while `V3D_CTL_INT_MSK_CLR`
is still never written anywhere - so nothing here can be exercised by a
genuine hardware interrupt yet, same safety margin as stage 1.
`v3d_irq_resolve_clear()` looks up `HAL_IRQClear`'s raw address and
static-base (`R9`) value via `OS_Hardware 1`, and establishes a
*permanent* IO mapping (`OS_Memory 13`, not the shared/temporary
`OS_Memory 14` window `v3d_poll_counter` already warns is unsafe to
hold across anything, and definitely unsafe to call as a SWI from IRQ
mode at all) for the handler's own register access. The raw call
itself (`call_hal_routine`, GCC inline assembly: `R9`=static base,
`blx` to the resolved address, `R0`=device number) was disassembled
and checked by hand against the documented convention before shipping,
not just built and hoped - the generated code is a textbook-exact
match (`mov r0,#10` / `ldr r9,[...]` / `blx r4`, with `r4/r8/r9/lr`
correctly saved and restored around it). The handler itself now has
real logic (check `V3D_CTL_INT_STS`, acknowledge + claim if
`FLDONE`/`FRDONE` is set, otherwise pass on) but still cannot run for
a genuine reason yet. `V3D_IRQTest` extended to also resolve and
directly exercise the raw `HAL_IRQClear` call from safe, non-interrupt
context, proving the address/static-base/branch sequence itself
doesn't crash before it's ever used for real - reports both the
claimed and spurious handler-invocation counts (`R0`/`R1`), both
expected to read 0. Not yet tested on hardware.

**Hardware result (2026-09-26) - CONFIRMED CLEAN.** `V3D_PowerOn` then
`V3D_IRQTest`: `claimed=0 spurious=0`, no error. The raw `HAL_IRQClear`
call (resolved address + `R9` static-base + `blx`, hand-disassembled
and verified before shipping) ran for real on hardware for the first
time, from the safe non-interrupt test path, without incident. This
was the highest-risk untested piece of this whole feature; it's
confirmed working. Stage 2b next: unmask `V3D_CTL_INT_MSK_CLR` and wire
real bin/render completion to it - the step where a genuine hardware
interrupt fires for the first time.

**Stage 2b (this commit)**: turns the interrupt on, but as a hybrid,
not a full swap. `v3d_irq_ensure_ready()` (lazy, idempotent, called
from `V3D_ClearTest`/`V3D_ClearToSprite` alongside the existing
`ensure_pmp()`) claims the vector, resolves `HAL_IRQClear`, unmasks
`V3D_CTL_INT_MSK_CLR` for `FLDONE`/`FRDONE` (mask everything first,
then selectively unmask - matches the real Linux v3d driver's own
`v3d_irq_enable()` pattern, not guessed), and calls `HAL_IRQEnable`.
The handler now tracks separate `g_v3d_irq_bin_done_count`/
`g_v3d_irq_render_done_count`. Deliberately **not** the only way
completion is detected yet: `v3d_poll_counter()` now checks both the
IRQ-driven counter and the original `BFC`/`RFC` register in the same
loop, either one satisfying it - so if anything about the interrupt
path is subtly wrong, reliability doesn't regress, it just falls back
to exactly what already worked, while still being observable whether
the interrupt fired correctly. `V3D_ClearTest` now also reports (`R2`/
`R3`) how many times *this specific call's* completion was actually
seen by the interrupt handler - temporary, stage-2b-only
observability, to be removed once trust is established and the
register-polling half can be dropped entirely. Not yet tested on
hardware.

**Hardware result (2026-09-26) - MACHINE FROZE. Real incident, root-
caused, fixed.** Fresh reboot, `V3D_PowerOn` then `V3D_ClearTest`: the
machine froze outright with `Internal error: abort on data transfer at
&20F09D7C` - a genuine ARM data abort, unrecoverable, requiring a
physical reset. This is exactly the risk class this whole feature was
staged to catch early and safely; here it wasn't caught early enough,
so it's recorded here in full rather than glossed over.

Root cause, found by reading the real Linux v3d driver's own IRQ init
code (`drivers/gpu/drm/v3d/v3d_irq.c`, `v3d_irq_init()`), not guessed:
on V3D 4.2 (this hardware), hub and core interrupts can be wired to a
**single shared GIC line** rather than two separate ones -
`v3d->single_irq_line` is set whenever the device tree provides only
one IRQ entry for the device, and BCM2711's real device tree
(`arch/arm/boot/dts/broadcom/bcm2711.dtsi`) gives the `v3d` node
exactly one: `interrupts = <GIC_SPI 74 IRQ_TYPE_LEVEL_HIGH>`. That's a
**level-triggered** line. Our handler only ever read/acknowledged
`V3D_CTL_INT_STS` (core) - it never touched `V3D_HUB_INT_STS` at all.
If anything at the hub level was ever set and unacknowledged, the GIC
would re-assert the same line again the instant it was acknowledged at
the controller level (`HAL_IRQClear`), since the peripheral's output
was still physically high - an interrupt storm, entered the moment
`HAL_IRQEnable` ran. That fits the symptom exactly: not a crash inside
our own submission logic (which never got the chance to run first -
this froze immediately on the very first `V3D_ClearTest` after
`PowerOn`, before any meaningful GPU work), but a runaway low-level
condition consistent with repeated re-entry corrupting the IRQ stack.

**Fix, two-pronged** (defence in depth, not just the more "obviously
correct" half alone): (1) `v3d_irq_unmask()` now explicitly masks off
*every* hub-level interrupt source (`V3D_HUB_INT_MSK_SET = 0xFFFFFFFF`)
- nothing at the hub level is needed for a simple clear-to-colour job,
so structurally prevent it from ever being the trigger at all; (2) the
handler unconditionally reads and acknowledges `V3D_HUB_INT_STS`
regardless, matching the real driver's own `v3d_hub_irq()` fallback for
exactly this `single_irq_line` configuration - belt and braces, in
case something is ever unmasked from outside this driver's control.

Given a real freeze already happened once, added one more layer of
caution before testing again rather than going straight back to full
`V3D_ClearTest`: `V3D_IRQTest` now exercises the actual live-fire path
in complete isolation - unmask, `HAL_IRQEnable`, genuinely wait (not
just enable-then-immediately-disable, which never gave real hardware
time to latch and deliver anything - the previous "clean" `V3D_IRQTest`
result likely never actually tested this at all), watch the handler's
counters, `HAL_IRQDisable`, re-mask - without ever submitting a GPU
job, so `FLDONE`/`FRDONE` cannot legitimately fire and *any* handler
invocation during the wait is exactly the class of problem this exists
to catch safely, before real submission code ever unmasks for real.
Not yet tested on hardware.

**Hardware result (2026-09-26) - CONFIRMED SAFE.** `V3D_PowerOn` then
`V3D_IRQTest`: `claimed=0 spurious=0`, no freeze, no error. A genuine
~10 million-iteration wait with the interrupt actually enabled and
nothing at all fired spuriously - the hub-masking fix holds up
directly in isolation, with no GPU job involved. Next: the real test -
`V3D_ClearTest` with the interrupt live, actually submitting a job so
`FLDONE`/`FRDONE` should legitimately fire this time.

**Hardware result (2026-09-26) - SECOND FREEZE, same crash address,
real root cause found.** `V3D_ClearTest` froze the machine again:
identical `Internal error: abort on data transfer at &20F09D7C` -
byte-for-byte the same address as before. That determinism, plus
`V3D_IRQTest`'s wait having just proven clean with the interrupt
genuinely live but no job submitted, narrowed this precisely: the
handler had never actually run for a *real* reason before this moment
(nothing legitimately fires `FLDONE`/`FRDONE` without a submitted job)
- this was its first genuine invocation, and it crashed immediately,
every time, at the same address.

Root cause: `v3d_irq_claim()` was passing `R2=0` to
`OS_ClaimDeviceVector`. The PRM documents that this value becomes `R12`
whenever the hardware vector later invokes the handler - and R12 is
exactly the register RISC OS/GCCSDK module code uses to locate its own
global/static data. Every *other* entry point into this module (SWI
handler, `module_init`) receives the correct value automatically from
the kernel; a raw hardware vector doesn't know to provide it - passing
the right value is the claiming code's own responsibility, and it
never did. Confirmed at the disassembly level, not just inferred: the
`vector-handlers` veneer (`cmhg/V3DDriver.o`, `v3d_irq_entry`) takes
`R12` and dereferences it as a pointer chain (`ldr ip,[ip]` / `ldmib
ip,{fp,ip}`) to bootstrap the module's static-data-area pointers
*before* ever calling our C handler - with `R2=0`, that's a null-
pointer dereference chain landing on a garbage address, which is
exactly what a "data transfer abort" at a fixed, deterministic address
looks like. The same mechanism was independently confirmed for
`module_init` (its own `pw` parameter comes from `_clib_initialise
module`'s return value) and for the SWI-handler veneer (`mov r2, ip` -
its `pw` comes straight from the kernel-provided `R12`), proving both
that this is the same "private word" concept throughout the module and
that a genuinely correct value was available the whole time, just
never captured or passed on.

**Fix**: `module_init` now captures its own `pw` parameter (previously
discarded) via `v3d_irq_set_private_word()`, and `v3d_irq_claim()`/
`v3d_irq_release()` pass that real value as `R2` instead of `0`. Not
yet tested on hardware - the hub-masking fix from the previous freeze
is unrelated and still correct/needed (it addresses a different,
real hazard - a shared GIC line with unacknowledged hub-level sources
- and stays in place), this is a second, independent bug that
happened to be masked by the first one until it was fixed.

**Hardware result (2026-09-26) - CONFIRMED WORKING END TO END.** Fresh
reboot, `V3D_IRQTest` clean (`claimed=0 spurious=0`), then
`V3D_ClearTest &DEADBEEF`: no freeze, `readback=DEADBEEF` as always,
and **`bin_irq=1 render_irq=1`** - the handler genuinely fired and
correctly detected both bin and render completion for this exact job,
not a polling fallback. This closes out real IRQ handling's live-fire
milestone: the private-word fix and the hub-masking fix together
resolve both freezes, and the interrupt-driven completion path now
works correctly on real hardware.

**Hardware result (2026-09-26) - repetition confirmed, but not yet
proven IRQ-specific.** `StressTest.bas` ran 50 clean iterations of
`V3D_PowerOn`+`V3D_ClearTest`+`V3D_ClearToSprite` with exact readback
checks throughout - but the script predated `V3D_ClearTest`'s IRQ
observability fields, so it only confirmed overall (hybrid) correctness,
not specifically that the interrupt fired on every call rather than the
register-polling fallback occasionally winning the race. Extended it to
check `bin_irq`/`render_irq` (and `V3D_ClearToSprite`'s new packed IRQ
flag, `R3`) strictly on every iteration, and to summarise how many of
the 50 calls were genuinely IRQ-driven. Not yet re-run on hardware.
Once confirmed 50/50, the register-polling half of `v3d_poll_counter()`
can be dropped, now that the interrupt path is trusted.

**Hardware result (2026-09-26) - CONFIRMED, 50/50.** `StressTest.bas`:
50 clean iterations, `V3D_ClearTest` IRQ-driven on 50/50 calls,
`V3D_ClearToSprite` IRQ-driven on 50/50 calls - zero fallback to
register polling, on either SWI, across the whole run. Real interrupt-
driven GPU completion is now proven reliable under sustained
repetition, not just a one-off. Dropped the register-polling half of
`v3d_poll_counter()` - the interrupt path is what this driver actually
relies on now.

**Hardware result (2026-09-26) - a third freeze, this time on
`StressTest.bas` after a plain `*RMKill`/`*RMLoad` reload rather than a
fresh reboot; a full reboot fixed it.** Root cause: `module_final` did
nothing at all - never released the interrupt vector or disabled the
interrupt. `OS_ClaimDeviceVector` only recognises a later claim as
*replacing* an earlier one when the device number, handler address and
`R12` all match exactly - a reloaded module gets a fresh RMA address
for its handler every time, so it's never recognised as "the same
driver." Without releasing on unload, every reload left the previous
instance's claim permanently in the vector's chain of claimants,
pointing at code and data that no longer existed once that instance
was unloaded - a dangling entry a real interrupt could still reach
later (e.g. if the handler ever passed on an unrecognised interrupt),
with unpredictable results. Matches BCMSupport's own real module, which
does the equivalent release in its `mailbox_shutdown()`.

**Fix**: added `v3d_irq_shutdown()` (mask everything off, `HAL_IRQDisable`,
release the vector - a safe no-op if IRQ handling was never used this
session) and call it unconditionally from `module_final`.

**Hardware result (2026-09-26) - the vector-release fix alone was not
enough. The machine froze again at the exact same test step, reload
then `StressTest.bas`.** That ruled the dangling-vector theory out (or
at least out as the *whole* explanation) and pointed at something more
familiar: `g_powered_on` is per-module-instance C state, and a reload
resets it to 0 even though the underlying V3D core is still warm from
the previous instance's use - meaning the freshly-loaded instance's
first `V3D_PowerOn` call reset-pulses an already-working core all over
again. That is the *exact* confirmed cause of the original "second call
always hangs" saga (attempts 9-21) - just triggered by module reload
this time instead of a second call within one instance, so the earlier
idempotency fix (scoped to `g_powered_on`, which can't survive a
reload) never saw it coming.

**Real fix**: `v3d_ensure_clock_on()` now reports whether the firmware
clock was *already* on before it touched anything - real, instance-
independent hardware truth, unlike `g_powered_on`. `v3d_power_on()` now
skips the entire reset/gate sequence whenever the clock was already on,
regardless of this instance's own history, not just when its own
`g_powered_on` happens to be set. Not yet tested on hardware.

**Real IRQ handling: mechanically complete, reload-safety not yet
reconfirmed.** The bin/render wait no longer touches any V3D hardware
register directly - it purely watches a memory counter the interrupt
handler maintains, woken by a real `OS_ClaimDeviceVector`-claimed GIC
interrupt, acknowledged correctly at both the hub and core level (the
shared-GIC-line freeze), with the module's own private word correctly
threaded through to the handler (the null-pointer-chain freeze). Reload
without a reboot has now caused two further freezes on top of that
(dangling vector claim, then reset-pulsing an already-warm core) -
both fixed, neither yet reconfirmed on hardware.

**Hardware result (2026-09-26) - the reset-skip fix alone was also not
enough. A third freeze at the exact same test step**, confirmed via a
control experiment first: running `StressTest.bas` twice in the *same*
session with no reload at all works perfectly cleanly, ruling out
"running the 50-iteration battery more than once" as inherently
unsafe and confirming this is genuinely specific to reloading. The
freeze happened before any output at all - with the old every-5th-
iteration print, that only narrows it to "somewhere in iterations
1-4", not precise enough to diagnose further, so `StressTest.bas` now
prints a marker before every single risky call (not just every 5th
iteration) - if this happens again, the last line on screen will
pinpoint the exact call in flight.

Found a third system-wide OS resource this module was leaking across
reloads, this time by direct analogy to the two already found (the
interrupt vector, the reset-pulse tracking): the PMP Dynamic Area
itself. Like a claimed device vector, a Dynamic Area is system-wide OS
state that outlives the module that created it unless its physical
pages are explicitly released first - `module_final` never did this
either. A reloaded module's `OS_Memory 12` calls could be recommended
physical pages a still-existing, still-locked zombie area from the
previous instance already owns - a double-claim hazard matching the
timing exactly (the very first real GPU memory use after reload, which
is the first `pmp_alloc()` call). Exposed `pmp_release()` (previously
file-local to `phys_buffer.c`) and called it unconditionally from
`module_final` for the whole PMP area's page range.

**Hardware result (2026-09-26) - CONCLUSIVE NEGATIVE, investigation
paused.** Built and tested a diagnostic variant with *only* the
`pmp_release()` addition reverted (vector-release and reset-skip fixes
both still present): froze at the exact same step regardless. That
isolates `pmp_release()` as not the cause - but since *every* variant
tried (with any combination of the three fixes present or absent) has
frozen identically, none of the three theories investigated (dangling
vector claim, reset-pulsing a warm core, leaked PMP pages) has been
confirmed as the actual root cause, despite each being individually
well-grounded and directly analogous to real bugs already found and
fixed elsewhere in this project. Four hardware round-trips (each
costing a full physical reboot) spent on this one scenario without a
resolution.

**Decision: documented as a known limitation rather than continuing to
guess.** This does not affect the module's actual functionality -
confirmed reliable within a single boot/instance (50/50 real
interrupt-driven completions under sustained repetition, real GPU
rendering, all working correctly). It only affects *reloading* the
module without an intervening reboot, which is a development-time
convenience, not something real usage depends on (a real driver loads
once at boot and stays loaded). See "Known limitations" below.
Revisiting this is possible but would need a fundamentally different
diagnostic approach, not another targeted guess in the same vein as
the last four.

Five separate bugs across twenty-plus hardware round-trips to get
here, four of them genuine machine freezes, not just wrong answers -
three of those four (vector claim leak, reset-pulse tracking, PMP
Dynamic Area leak) were investigated but the actual reload freeze
remains unresolved. Next milestones below.

## Known limitations

- **Do not `*RMKill`/`*RMLoad` this module without a full reboot in
  between.** Doing so reliably freezes the machine outright (a hard,
  unrecoverable freeze requiring a physical reset) the moment any real
  GPU work is exercised afterward - confirmed reproducible across many
  hardware tests. Root cause not found despite dedicated investigation
  (see "Real IRQ handling" above for the full history) - several
  well-grounded theories were tested and fixed, none resolved it.
  **Always reboot before loading a freshly-built module for testing.**
  This does not affect the module's actual operation once loaded - only
  development-time reloading. **Now applies on every ordinary load, not
  just ones where `V3D_AccelStart` was manually run** - see "Auto-start
  at boot" below, since the module now claims `GraphicsV` automatically
  on load, adding a second (really third, alongside the IRQ vector)
  system vector's release path to whatever isn't currently understood
  about this freeze.

## Auto-start at boot (2026-09-27, hardware-verified)

The module now claims `GraphicsV` automatically in `module_init` - the
moment it's loaded, acceleration is active, no manual `V3D_AccelStart`
needed. Test scripts (`AccelTest.bas` etc.) still work unchanged:
`V3D_AccelStart`/`V3D_AccelStop` are idempotent, so calling
`V3D_AccelStart` when auto-start already claimed the vector is a safe
no-op.

**Deliberately does NOT power on the GPU/PMP/IRQ machinery at boot** -
only the `GraphicsV` claim itself happens in `module_init`. The actual
GPU initialisation (firmware mailbox calls, clock enable, ASB gates)
stays exactly as lazy as it already was, deferred to the first real
accelerated call (`v3d_accel_render_fill`/`v3d_accel_render_copy`
already self-initialise this way). `module_init` runs very early in
the boot sequence, before the system may be fully settled, and there's
no proven need for this module's own hardware bring-up to happen that
early - claiming a software vector is cheap and inert until `GraphicsV`
actually delivers a Render call.

**Opt-out**: `*RMLoad V3DDriver NoAutoStart` skips the automatic claim,
leaving the module in the original manual-opt-in mode (`V3D_AccelStart`
still works normally) - a safety valve for testing/isolating this
module given its history of real bugs that only surfaced under live
desktop traffic (see the "GraphicsV Render accelerator" sections
above), and given every additional claimed system vector is one more
thing to reason about for the still-unresolved reload freeze above.

A failed auto-claim (e.g. `OS_Claim` itself erroring) does not fail
`module_init` - the module still loads and its test SWIs still work
either way; only the automatic acceleration is unavailable, recoverable
later via a manual `V3D_AccelStart`.

**Confirmed on real hardware 2026-09-27**: `AutoStartTest.bas` - which
deliberately never calls `V3D_AccelStart` or `V3D_PowerOn` itself -
after a plain `*RMLoad` (no `NoAutoStart`), reported 3803 `GraphicsV`
handler invocations with 309 fills and 246 copies genuinely
accelerated, all before the script ever called anything. Auto-start
works: loading the module is now sufficient on its own, no manual
opt-in required.

## Next milestones

M2 (clear-to-colour) and M3 (display in a Wimp window) are both done
and hardware-verified. Not yet attempted:

- ~~**Robustness**~~ - done: `StressTest.bas` confirmed 50 repeated
  `V3D_PowerOn`/`V3D_ClearTest`/`V3D_ClearToSprite` calls in one session
  with exact readback checks and no failures (attempt 21).
- Real IRQ handling instead of polling - the current bin/render waits
  are bounded busy-polls, fine for a test SWI, not for a real driver.
- Eventually: actual shaders (the reference's rainbow-triangle QPU
  bytecode is a ready-made, proven starting point) rather than a
  fixed-function clear - this is real primitive rendering, not just a
  full-tile clear, and would need `GlShaderState`/`VertexArrayPrims`
  and the shader-state-record machinery this project has deliberately
  avoided so far.

Full technical grounding and effort estimate for the milestones beyond
this sketch: see the research summary from the conversation that
produced this skeleton.

## GraphicsV Render accelerator (started 2026-09-26)

Reframing the end goal from "generic 3D driver" to "accelerate the Wimp
desktop", researched against the real RISC OS PRM (not just precedent
projects): `GraphicsV` (vector 42) is the real, current, RISC OS 5
mechanism the kernel uses to talk to any video driver, and reason code
13 ("Render") is the hardware-accelerated rectangle fill/copy hook used
for window clears and scrolls - confirmed against both a modern working
implementation (RPCEmu-extended's `docs/gfxcard.md`) and the authoritative
PRM pages (`GraphicsV`, `GraphicsV 13`/`13 1`/`13 2`, `HAL Video API`,
`HAL_VideoRender`). Key findings that shape the plan:

- `GraphicsV` is a standard `OS_Claim`-chained vector (`R4`=0 signals
  "claimed", same convention as `WrchV`/`UpCallV`) - a small module can
  claim it *in addition to* the kernel's own internal HAL-bridge
  claimant (which is what actually services Render calls today via
  `HAL_VideoRender`, confirmed via the `HAL Video API` page), intercept
  reason 13 only, and leave everything else (and any reason-13 case it
  can't handle) unclaimed to fall through - no mode-setting/EDID/
  framestore-ownership needed, genuinely additive.
- Reason 13 (Render) is always called **foreground, SVC mode only**
  (per the PRM's calling-context table) - never IRQ/background, unlike
  most other GraphicsV reasons. That means the handler can safely block
  and reuse this driver's existing synchronous submit-and-wait path
  (`v3d_clear_run`) with none of the re-entrancy/non-blocking
  constraints a BG-context call would impose.
- Op 2 (fill) parameter block's word 4 is a pointer to an **ECF
  (Extended Colour Fill) block** - a legacy 8-byte replicated-pattern
  block, not a plain RGBA colour. Plan is to detect the common
  solid-colour case (all 8 pattern bytes equal) and service only that,
  leaving genuine patterned fills unclaimed.
- Op 1 (copy) rectangles "may be overlapping" per the PRM - exactly
  what a window-scroll copy is. A tile-based GPU sampling and writing
  the same underlying memory in one pass has no general correctness
  guarantee for overlap, so the plan is to detect overlap and leave
  those calls unclaimed until a direction-aware approach is worked out;
  non-overlapping copies (e.g. window-to-window drags) get accelerated
  first.
- Both "synchronise" flags in R0 are satisfied for free: every
  submission in this codebase already blocks synchronously on the
  IRQ-driven completion counters before returning, so there's no
  in-flight-op state to track (no pipelining benefit yet, but also no
  correctness gap).
- Claiming a *second* system vector (on top of the IRQ vector) means
  the unresolved reload-without-reboot freeze (see "Known limitations")
  gets a second release path to reason about - treat "no reload without
  reboot" as even more load-bearing once this lands, until that's
  either fixed or disproven as a factor here too.

Planned order: (1) read-only mode/VDU variable groundwork - **done**,
see `V3D_ScreenInfo` below; (2) multi-tile fill into an arbitrary
physical address, verified via a PMP test buffer before ever touching
real screen memory - **done**, see `V3D_FillTest` below; (3) same fill
targeting the real framestore at a real rectangle, verified visually -
**done, hardware-verified**, see `V3D_ScreenFillTest` below; (4) claim
`GraphicsV`, handle op 2 (fill) only, ECF-solid-only, behind an
explicit opt-in (not auto-claimed on module load, given a silently
wrong fill corrupts visible desktop state, which is worse than a
crash) - **done, hardware-verified** (2028/2028 real desktop fills
accelerated, zero failures, desktop confirmed visually correct
throughout), see `V3D_AccelStart`/`V3D_AccelStop`/`V3D_AccelStats`
below; (5) op 1 (copy), non-overlapping only - **done, hardware-
verified** (2641/3533 real desktop copies accelerated, 892 correctly
rejected as overlapping, zero failures). See "Op 1: rectangle copy"
below. **All 5 planned steps complete.**

### Step 1: screen-mode groundwork (2026-09-26, hardware-verified)

`V3D_ScreenInfo` reads `OS_ReadVduVariables` for `ModeFlags`, `Log2BPP`,
`LineLength`, `XWindLimit`, `YWindLimit`, `ScreenStart`, `DisplayStart` -
see `c/screen_info.c`/`.h`. Deliberately has nothing to do with V3D
itself: no hardware touched, no `V3D_PowerOn` required, pure OS state
read. VDU variable numbers transcribed from a real, shipped GCCSDK
header (`env/include/oslib/os.h`'s `os_VDUVAR_*` enum), not guessed.
Test with `test/ScreenInfoTest.bas`, which also sanity-checks
`LineLength` against the visible width for the obvious no-padding case.

**Confirmed on real hardware**: 1920x1080, `Log2BPP`=5 (32bpp),
`LineLength`=7680 bytes/row (exactly width*4 - no padding),
`ScreenStart`==`DisplayStart` (no hardware double-buffering active on
this setup). This is the exact mode step 3 needs to target.

### Step 2: multi-tile fill into a synthetic buffer (2026-09-26, hardware-verified)

Generalises the M2/M3 single-64x64-tile clear+store to an arbitrary
`tiles_x` x `tiles_y` grid - `V3D_FillTest` (R0=width, R1=height,
R2=colour in; physical address, row stride, tiles_x, tiles_y, and a
*logical* address for direct sampling out). Still writes into a
freshly-allocated PMP test buffer, not the real screen - that's step 3.

Refactor first: the ~30 tiny packet-emitter functions (`TileBinningModeCfg`,
`ClipWindow`, `StoreTileBufferRT0`, etc.) were pulled out of `cl_clear.c`
into a shared `c/cl_packets.h` verbatim (byte-for-byte identical
emission, confirmed by clean rebuild) so the multi-tile code in the new
`c/cl_fill.c` doesn't duplicate them by hand. `cl_clear.c`'s own M2/M3
logic (the proven single-tile path, its diagnostics, its history
comments) is untouched.

Key details, all transcribed from the same MIT-licensed reference this
whole project verifies against (github.com/Random06457/rpi4-gpu-bare-
metal-examples, `gl_rainbow_triangle.cpp`, fetched and read directly
2026-09-26, not guessed or extrapolated from the single-tile case):

- **Tile/tile_state sizing formulas**: `tile_alloc_size = align(tiles_x
  * tiles_y * 64, 4096) + 64*64*2` (that `+64*64*2` is a literal 8192-byte
  constant, not scaled by tile count - confirmed by reading the source,
  not inferred, since the single-tile case (N=1) couldn't have
  distinguished the two readings). `tile_state_size = tiles * 256` bytes
  (V3D>=40 TSDA size, matching the constant `cl_clear.c` already used).
- **Supertile selection** (`get_super_tiles` in `cl_fill.c`): picks the
  smallest supertile grid keeping cell count under 256, transcribed from
  the reference's `getSuperTiles()`. `MulticoreTileListSetBase`/
  `MulticoreSupertileCfg` still need real (non-1x1) values even though...
- **...`SupertileCoordinates` is deliberately NOT used.** The reference
  uses it (plus a generic/indirect tile list with `BranchToImplicitTileList`)
  to keep the RCL compact when walking thousands of tiles for real
  *primitive* rendering. A fill has no primitives and needs none of that
  machinery - this just unrolls one explicit `TileCoordinates`+clear+store
  pair per tile directly in the RCL (~40 bytes/tile - about 20KB for a
  full 1920x1080 screen, trivial). Confirmed via `TileBinningModeCfg`'s
  own packet layout that supertile grouping is purely an RCL/render-side
  addressing detail, not something the binner needs told separately.
- **Partial edge tiles**: the test buffer is deliberately padded up to a
  whole tile grid (not just `width`x`height`) rather than assuming
  real hardware clips a partial edge tile's full-tile store exactly to
  the declared frame size - that assumption is untested and would be a
  real risk for step 3's actual framestore target (writing 64 rows into
  a 1080-row screen's bottom edge tile could scribble past the buffer
  if wrong). Flagged as an open question for step 3, not resolved yet.
- **`pmp_alloc`'s 64-page/call cap** (a fixed-size stack array in
  `phys_buffer.c`, not a hardware limit) bounds how big a rectangle this
  *test harness* can exercise via its synthetic PMP render buffer - a
  full 1920x1088 padded buffer alone is ~2040 pages. This does not bound
  the real feature: step 3 writes directly into the already-existing
  screen framestore (no PMP allocation for the render target at all),
  and `tile_alloc`/`tile_state`/the RCL all stay comfortably under 64
  pages even at full screen size (32/3/5 pages respectively). Caught
  with a specific, clear error in `v3d_fill_test` rather than a
  confusing one from `pmp_alloc` itself.

Test with `test/FillTest.bas`: deliberately uses a 250x180 rectangle
(4x3 tiles, non-tile-aligned on purpose, to exercise partial edge
tiles) and samples 16 points across the grid - all four corners of tile
(0,0) plus its centre, the origins of tiles (1,0)/(2,0)/(3,0)/(0,1),
every corner of the whole rectangle, its centre, and points inside the
partial last row/column - rather than just pixel 0.

**Real bug found and fixed by hardware testing, not guessed**: the
first hardware run showed exactly ONE sample point correct (tile
(0,0)'s origin) and everything else reading back untouched leftover
memory - looking at first like "only the first tile works". A second,
more targeted diagnostic round (probing all four corners of tile (0,0)
itself, plus the exact origins of tiles (1,0)/(2,0)/(3,0)/(0,1))
showed tile (0,0) was in fact *fully* correct (all corners, not just
its origin), and so was tile (2,0) and tile (0,1) - but tiles (1,0) and
(3,0) were not. The pattern (byte offsets that are multiples of 512
correct, offsets of 256/768 wrong) only makes sense if the CLE derives
each tile's column offset internally from the current `TileCoordinates`
command and adds it to whatever `addr` the store command is given - so
this driver's own manual `render_phys + row*64*stride + col*64*4`
address computation was compounding with the hardware's own, and a
"correct"-looking readback at some positions was actually a different,
wrongly-doubled tile's write landing there by coincidence (every tile
writes the same solid colour, so a collision reads as "correct" either
way). Confirmed against the reference project's own literal usage
(`gl_rainbow_triangle.cpp`'s single shared store command, executed once
per tile via the generic/indirect tile list, always passes the plain
`render_target.paddr()` with no manual offset at all, every time) -
**fix**: pass the same plain base address for every tile's store,
trusting the CLE to add the per-tile offset itself from the preceding
`TileCoordinates` command, exactly like the reference does. Confirmed
fixed on hardware 2026-09-26: all 16 sample points correct across the
4x3 grid, including every partial edge tile.

This also answers step 3's flagged open question about partial edge
tiles: real hardware did NOT corrupt the padding area for the
partial last row/column (readback at the true, non-padded rectangle
edges - e.g. `(249,179)`, the actual bottom-right pixel - came back
correct), consistent with the declared frame width/height in
`TileRenderingModeCfgCommon`/`ClipWindow` genuinely clipping each edge
tile's store. Still worth deliberate caution the first time step 3
targets the real framestore (no padding to fall back on there), but
this is real supporting evidence, not just an assumption.

### Step 3: fill into the real screen framestore (2026-09-26, hardware-verified)

Same multi-tile fill as step 2, but targeting the real, live screen
framestore instead of a synthetic PMP buffer - `V3D_ScreenFillTest`.
This is the first SWI in the whole project that writes somewhere with
a visible effect on the real desktop, not an isolated test buffer, so
it gets a correspondingly more careful writeup below.

**Refactor**: `cl_fill.c`'s single `v3d_fill_test` function was split
into a shared `run_fill()` core (builds/submits the control lists
against whatever `target_phys`/`target_stride` it's given) plus two
thin wrappers - `v3d_fill_test` (step 2, PMP-allocates a padded render
buffer first) and `v3d_fill_screen` (step 3, no render-target PMP
allocation at all - the screen memory already exists). Both share the
same tiling/supertile/submission logic, so a fix to one applies to both.

**New groundwork, in `screen_info.h`/`.c`** (kept there rather than a
separate file since it's the same class of "read OS state about the
screen" as `V3D_ScreenInfo`):
- `v3d_logical_to_physical()` - `OS_Memory 0` (reason 0, "General page
  block operations") with flags `GIVEN_LOG_ADDR|RETURN_PHYS_ADDR`
  (`0x200|0x2000`). Confirmed against the real PRM (`OS_Memory 0` and
  `OS_Memory 0 Flags`, fetched 2026-09-26) rather than the older, MEMC-
  only `OS_ReadMemMapEntries` (SWI `&52`), which doesn't apply to this
  hardware's MMU-based memory model. Cross-referenced against that PRM
  page's own "linked from" list, which includes `GraphicsV 6` (SetDAG)
  - i.e. this is the same mechanism a real GraphicsV driver uses to get
  a framestore's physical address, not something improvised for this
  project.
- `v3d_verify_framestore_contiguous()` - a Dynamic Area's logical window
  has no *general* guarantee of physically contiguous backing, and this
  driver's whole approach (one physical base address + stride, matching
  how V3D's tile store addressing works) silently breaks if that's
  wrong for even one page in range - it would write GPU output to the
  wrong physical memory entirely, on the *real* framestore this time.
  So this checks it for real: translates every single page boundary
  across the whole screen's byte range and confirms each consecutive
  pair's physical addresses differ by exactly one page. In practice
  this is expected to hold (display scanout hardware generally needs a
  contiguous framestore too), but "expected" is exactly the kind of
  assumption this project has been burned by before (see the M3
  power-on/reset history) - checked once, cached, not just trusted.

**`module.c`**: `ensure_screen_framestore()` lazily runs the above once
(mirrors `ensure_pmp`'s pattern), also rejecting any mode that isn't
32bpp (`Log2BPP`=5) - the pixel-format gate flagged in the original
scoping writeup, since this driver only knows RGBA8 output. Scratch
buffers (CL/tile_alloc/tile_state - never the render target) come from
the same `g_pmp_fill` area `V3D_FillTest` uses, at a fixed high page
offset (`SCREEN_FILL_BASE_PAGE`=3000) well clear of `V3D_FillTest`'s own
usage (which can reach ~2040 pages for a full-screen-sized synthetic
buffer).

**Deliberately NOT addressed yet**: `V3D_ScreenFillTest`'s (x,y) is
just "offset from `ScreenStart`, row 0 = whatever's first in memory" -
it does not attempt to reconcile that with `GraphicsV 13`'s own
documented bottom-left-origin, positive-Y-up coordinate convention.
Getting that mapping right (and right relative to whatever `YEigFactor`
implies about the actual scan direction) is real, necessary work before
step 4 can wire this into `GraphicsV` correctly, but this step's own
goal - "can this driver write a correctly-shaped rectangle directly
into real screen memory and have it appear on the real display at all"
- needs none of that yet, so it's deferred rather than guessed at.

Test with `test/ScreenFillTest.bas`: a deliberately small (150x100),
away-from-the-edges rectangle, not the full screen, for a conservative
first real-framestore test - if the contiguity check or address math
is somehow subtly wrong despite the care above, the blast radius of a
mistake should stay small and recoverable (a garbled desktop area until
the next redraw, at worst a reboot - not a hang, since this reuses the
exact same hardware-proven submit/IRQ-wait path as every previous
fill). The script also explicitly warns that the Wimp may immediately
paint over the result - that's expected (this driver isn't synchronised
with the Wimp's redraw cycle yet, that's step 4+ territory), not a bug.

**Real bug found on the very first hardware run, fixed before any
visible-corruption risk was actually taken**: `V3D_ScreenFillTest`
failed immediately with `"Address not recognised"` - `OS_Memory 0`'s
own error for a logical address it doesn't consider backed by real
RAM. Root cause: `v3d_verify_framestore_contiguous` checked
`LineLength*(YWindLimit+1)` bytes (this driver's own *derived estimate*
of the visible area), not the OS's own authoritative
`TotalScreenSize` VDU variable (0x96) - the two aren't guaranteed to
match, and evidently didn't on this setup. **Fix**: added
`TotalScreenSize` to `V3D_ScreenInfo`'s output (R7) and switched the
contiguity check to use it instead. Also wrapped the OS's error (and
any future discontiguity found) with the exact byte offset that failed
- the original error gave no clue *where* in the range it happened,
which cost real debugging time working out that the range itself, not
a specific page, was the actual problem.

**Second hardware round, same fix's real target found to be wrong at a
more fundamental level**: with the `TotalScreenSize` fix applied,
`V3D_ScreenFillTest` failed identically - `"Address not recognised"`,
and (per the added diagnostic) with no offset prefix at all, meaning it
failed on the very *first* translation attempt (`ScreenStart` itself,
before the range even mattered). This ruled out "wrong range" as the
problem and pointed at something more fundamental: `OS_Memory 0`
apparently cannot translate the Pi's screen framestore's logical
address to a physical one at all - its own PRM text explains why ("the
addresses must correspond to pages which the OS has assigned a valid
physical page number to"), implying this framestore isn't ordinary
OS_Memory-0-trackable RAM, plausibly a HAL-managed region locked down
outside normal page-allocation bookkeeping.

**Fix**: abandoned `OS_Memory 0` for this purpose entirely, in favour
of `GraphicsV 9` ("Framestore information") - the actual, direct,
PRM-documented mechanism for exactly this question, returning the
framestore's physical base address and size directly from the active
display driver, no generic address translation involved. Called via
`OS_CallAVector` (SWI `&34`) as a genuine vector call (not a claim) -
this driver asking the currently active driver directly, the same way
the kernel itself would. This also happens to settle the contiguity
question more cleanly than the original page-walk approach ever did: a
driver reporting one `(base, size)` pair is implicitly asserting that's
one contiguous region, so there's nothing left to separately verify.
`v3d_verify_framestore_contiguous` was renamed to `v3d_get_framestore_phys`
to match what it actually does now. One real risk carried over from the
PRM's own text, not yet observed: the call only succeeds if the active
driver indicates (via `GraphicsV 8`) that it has a *separate*
framestore - if the Pi's driver doesn't, this fails with a clear,
specific error (`R4` still nonzero on return) rather than silently
trusting stale register contents.

**Confirmed working on real hardware 2026-09-26**: `GraphicsV 9`
answered correctly on the very first real attempt - the Pi's active
display driver does report a separate framestore this way. Running
`ScreenFillTest.bas` (150x100 rectangle at (400,300)) produced a
solid, correctly-sized (3x2 tile grid, as expected for 150x100) purple
rectangle that appeared near the top-left of the real desktop and
stayed on screen (an empty background area, so nothing redrew over
it). This confirms the whole step-3 chain end to end: `GraphicsV 9`'s
reported physical address is correct, the multi-tile fill machinery
(unchanged from step 2, just pointed at a different address) works
identically against real screen memory as it did against a synthetic
buffer, and V3D output is genuinely visible on the real display - not
just readable back by the CPU. Useful data point for step 4: this
driver's own row-0-at-`ScreenStart` convention places increasing `y`
further from the top of the visible display (i.e. `ScreenStart` is
the top of the screen in memory, not the bottom) - real evidence to
reconcile against `GraphicsV`'s own bottom-left-origin convention when
that work starts.

### Step 4: claim GraphicsV for real (2026-09-26, hardware-verified)

`V3D_AccelStart`/`V3D_AccelStop`/`V3D_AccelStats` - new `c/gfxv_accel.h`/
`.c`. This is the first code in the project that, once claimed, sits in
the live Wimp/VDU graphics pipeline for every `GraphicsV` call system-
wide, not just calls this module's own SWIs make - a materially
different risk level from every prior test, and it stays behind an
explicit opt-in for exactly that reason (see the original scoping
writeup above).

**ECF (fill pattern) decoding - the real format, not the guessed one.**
Researched properly before writing any code that touches live desktop
rendering: the actual PRM page (`ECF Block`, fetched 2026-09-26) shows
a 64-byte, 64-byte-aligned block of 8 lines, each an (OR mask, EOR mask)
pair - `output = (existing_pixel OR or_mask) EOR eor_mask` - not the "8
bytes, one per pattern row" guess an old forum thread suggested (that
turned out to be the pre-32bpp VIDC1-era format, wrong for this
hardware). The classic idiom for a genuine solid fill (independent of
whatever pixel was already there) is `or_mask=0xFFFFFFFF` (forces every
bit to 1 regardless of existing content) + `eor_mask=~colour` (flips
down to the exact desired value) - `colour = ~eor_mask` follows
algebraically. Detected in `ecf_decode_solid()` by checking every one
of the 8 lines has `or_mask==0xFFFFFFFF` and an *identical* `eor_mask`
(a genuine dithered/patterned fill would vary it per line); anything
else is left unclaimed, not guessed at.

**Coordinate convention resolved.** `GraphicsV 13 2`'s own doc: `(0,0)`
is bottom-left, positive Y is up, all four rectangle edges inclusive.
Step 3's hardware test already established this driver's own internal
convention is top-down from `ScreenStart`. `v3d_accel_render_fill`
(in `module.c`, exposed to `gfxv_accel.c`) is the one place the
conversion happens: `y_top_down = YWindLimit - top`. `gfxv_accel.c`
itself deliberately knows nothing about this - it only speaks
GraphicsV's own coordinate language.

**Claim protocol**: `OS_Claim`/`OS_Release` (SWI `&1F`/`&20`, confirmed
via a real, shipped GCCSDK header rather than guessed) on vector 42,
using the same `vector-handlers` veneer mechanism already proven
correct for the hardware IRQ vector - `cmunge` only allows one
`vector-handlers` field per module (found the hard way, a build error),
so both pairs (`v3d_irq_entry`/`v3d_irq_handler`,
`v3d_gfxv_entry`/`v3d_gfxv_handler`) go on one comma-separated line.
The handler always returns `1` (`VECTOR_PASSON`) regardless of whether
it served the call - claiming is signalled purely via `R4=0`, per
`GraphicsV`'s own documented protocol, deliberately not using the
generic vector-chain-stopping mechanism at all. This matters: a
well-behaved further claimant (the kernel's own HAL-bridge claimant,
confirmed to exist by the original `GraphicsV`/`HAL Video API`
research) checks `R4` itself and does nothing once it's already 0, so
always continuing the chain is both correct and the simplest way to
guarantee this driver can never accidentally swallow a call meant for
someone else.

**Fail-safe on error**: if an accelerated fill is attempted but the GPU
submission itself fails (e.g. a timeout), the call is left *unclaimed*
(`R4` untouched) rather than claimed-but-broken, so the OS's own CPU
fallback still draws the correct pixels. A partially-GPU-drawn-then-
CPU-overdrawn rectangle is visually correct in the end; a claimed call
that silently failed would not be.

**Real instrumentation before real testing.** Because this handler now
runs inside the live graphics pipeline, "did it work" can't be judged
from visual effects alone - `V3D_AccelStats` tracks real invocation
counts (total calls, how many were reason 13, how many were op 2, how
many had a genuine solid ECF, how many were actually accelerated, how
many failed), mirroring the IRQ handler's own `claimed`/`spurious`
counter pattern. `test/AccelTest.bas` claims the vector, asks the user
to interact with the real desktop for a bit (drag/resize/scroll -
BASIC running in a Task Window plots into the window's own backing
store, not necessarily via the same path a real Wimp redraw uses, so a
self-generated test call here could prove nothing either way), then
reports the stats and releases.

**First hardware round (2026-09-26): fail-safe design confirmed
working, but a real bug found underneath it.** `AccelTest.bas` while
interacting with the real desktop: 13620 handler invocations, 4863
genuine solid-ECF fills attempted, only 58 (1.2%) actually accelerated.
Critically, **nothing on screen looked wrong** - confirming the
fail-safe design's core property (a failed GPU attempt correctly falls
through to normal CPU rendering) held up under real, heavy, unplanned
load, not just the isolated test cases that exercised it before.

`V3D_AccelFailInfo`'s new diagnostics found the actual cause
immediately: the last failure's rectangle printed as `top=22
bottom=45` - i.e. `top < bottom`, impossible for a genuine rectangle
in `GraphicsV`'s bottom-left-origin convention (a real top edge must be
`>= ` the bottom edge). That's the exact signature of a **word1/word3
swap in the parameter block decode** - `gfxv_accel.c` had read word1 as
`bottom` and word3 as `top`, backwards from the real PRM layout
(`GraphicsV 13 2`: word1=Top, word3=Bottom - re-fetched and double-
checked after finding this, not just trusted from memory a second
time). With the swap, `top - bottom + 1` computed as unsigned
arithmetic wraps around into a huge, nonsensical height for almost
every real rectangle, cascading into garbage tile counts - explaining
both the near-100% failure rate and why it varied by rectangle (any
call where the real top and bottom coordinates happened to differ by
only 1, i.e. a single-pixel-tall row, would coincidentally read the
same swapped value either way and slip through - consistent with the
successes skewing toward very small areas, 2 to 960 sq px).

**Fixed**: swapped the decode to `top = params[1]`, `bottom =
params[3]`.

**Confirmed fixed on hardware, 2026-09-26**: same `AccelTest.bas`
exercise (drag/resize/scroll windows, open/close a directory display) -
**2028/2028 (100%) of genuine solid-ECF fills were accelerated by the
GPU, zero failures**, and the desktop looked completely normal
throughout (explicitly confirmed, not just assumed from the absence of
an obvious glitch). This is the milestone the whole "GraphicsV Render
accelerator" effort was aimed at: real V3D GPU hardware is now
transparently accelerating real Wimp desktop window-fill operations,
indistinguishably from normal CPU rendering, entirely through the
documented, sanctioned `GraphicsV` mechanism - no Wimp/WindowManager
patching, no undocumented hooks.

## Op 1: rectangle copy (2026-09-26, hardware-verified)

The last piece of the original 5-step plan: `GraphicsV` op 1 (rectangle
copy), needed for window-scroll acceleration - arguably more visible in
everyday use than window clears (scrolling a directory listing, a text
document, a terminal).

**Simpler mechanism than originally scoped.** The original scoping
writeup assumed a copy would need "a texture-sample-into-render-target
control list... nothing in `cl_clear.c` samples a texture today." Before
writing any code, checked the reference project's own `v3d_cl.hpp` for
`LoadTileBufferGeneral` - a real RCL packet (opcode 30) that's the exact
mirror of `StoreTileBufferGeneral` (opcode 29, already proven by the
fill path): it loads pixel data FROM memory INTO the on-chip tile
buffer, rather than dumping the tile buffer TO memory. A copy is then
just **Load(source) + Store(dest) per tile - no shader, no texture
unit, no primitive rendering at all**, reusing almost all of the fill
path's proven scaffolding (tile/tile_state sizing, supertile selection,
BCL, submission/IRQ-wait). Genuinely new territory though: the
reference project defines this packet but never actually uses it in
its own example code, so its semantics (does it derive per-tile
addressing from `TileCoordinates` the same automatic way `Store`
does? does `height=0` mean "derive from `TileRenderingModeCfgCommon`"
the same way it does for `Store`?) are inferences from its structural
similarity to `Store`, not independently confirmed - exactly what
`V3D_CopyTest` exists to check before trusting it for anything real.

**Overlap: deliberately deferred, not solved.** `GraphicsV 13 1`'s own
doc explicitly allows overlapping source/destination rectangles ("they
may be overlapping" - exactly what a window scroll is). Because this
driver controls the exact per-tile processing order itself (it emits
every RCL instruction), a classic `memmove`-style direction choice
(process tiles in the order that guarantees no tile's `Store` ever
overwrites a not-yet-`Load`ed region) is a real, principled way to make
overlapping copies safe - genuinely more subtle than it first looks
once **sub-tile-granularity pixel shifts** are considered: a real
scroll is rarely a whole multiple of 64 pixels, so a single tile's
`Load` window can straddle the boundary between already-overwritten
and not-yet-touched memory even with correct macro-level tile
ordering. Matching the original plan's own scoping ("op 1 (copy),
non-overlapping only, as a later pass"), this is deliberately deferred
rather than half-solved - `cl_copy.c` only handles non-overlapping
copies for now, and callers (eventually `gfxv_accel.c`) must reject
overlapping rectangles themselves.

**New**: `c/cl_copy.h`/`.c` (a separate file from `cl_fill.c`, same
reasoning as `cl_fill.c` being separate from `cl_clear.c` - keeps a bug
here from regressing the proven fill path), `emit_load_tile_buffer_rt0`
in `cl_packets.h`, and `V3D_CopyTest`. Test with `test/CopyTest.bas`: a
deliberately non-tile-aligned 200x150 rectangle, source filled with a
per-pixel test pattern (not a flat colour - can't distinguish
"genuinely copied" from "coincidentally already there"), sampling 7
points including partial edge tiles.

**Confirmed on real hardware 2026-09-26, first attempt**: all 7 sample
points matched (source vs. dest, byte-for-byte) across the 4x3
non-tile-aligned grid, including both partial edge tiles. This confirms
the inference held: the CLE derives each tile's addressing from
`TileCoordinates` automatically for `Load` exactly as it already does
for `Store` (see step 2's own hard-won lesson above), and `height=0`
("derive from `TileRenderingModeCfgCommon`") works for `Load` the same
way it does for `Store`. `LoadTileBufferGeneral` - untested even in the
reference project's own code - is now proven, real, working
infrastructure for this driver.

### Copy step 2: real framestore copy (2026-09-26, hardware-verified)

Same Load+Store copy, but targeting the real screen framestore
directly (`V3D_ScreenCopyTest`, `v3d_copy_screen` in `cl_copy.c`) -
same progression as fill's step 2 -> step 3. Non-overlapping only:
the SWI handler itself refuses with a clear error (an AABB overlap
test) if the source and destination rectangles intersect at all,
rather than trusting the caller - same fail-safe philosophy as every
other real-framestore-touching SWI in this module, and the stakes are
the same (a bug here writes to the live, visible desktop). Test with
`test/ScreenCopyTest.bas`: a small (150x100), non-overlapping copy -
whatever's currently on screen at the source rectangle should appear
duplicated at the destination.

**Confirmed on real hardware 2026-09-26, first attempt**: the source
rectangle's on-screen content (part of the desktop wallpaper) appeared
correctly duplicated at the destination rectangle, screenshot-verified.
Same Load+Store mechanism, now proven against real screen memory as
well as a synthetic buffer.

### Copy step 3: wired into the live handler (2026-09-26, hardware-verified)

`gfxv_accel.c`'s `v3d_gfxv_handler` now dispatches on op (1=copy,
2=fill) via two new static helpers (`handle_render_fill`,
`handle_render_copy`), both behind the same `V3D_AccelStart` opt-in -
op 1 needed no separate claim/opt-in mechanism of its own. The overlap
check happens at the protocol layer (`handle_render_copy`), decoding
straight from `GraphicsV 13 1`'s own parameter block (word0=Source
left, word1=Source bottom, word2=Dest left, word3=Dest bottom,
word4=Width-1, word5=Height-1) before ever calling into `module.c`'s
new `v3d_accel_render_copy` - which applies the coordinate-flip
conversion independently to *both* rectangles (each has its own bottom
edge; they're not assumed to share a common y-origin).

Added real instrumentation from the start this time, having learned
from op 2's own live-test history (a real bug and a real reliability
question both only became visible once actual invocation counts were
captured) - `V3D_AccelCopyStats`: total op-1 calls seen, how many were
rejected for overlapping, how many were actually accelerated, how many
failed, and the last failure's error number. `AccelTest.bas` now
reports both fill and copy stats from the same live-desktop session.

**Confirmed on real hardware 2026-09-26**: `AccelTest.bas` during real
drag/resize/scroll/directory-browse interaction - **996/996 (100%) of
genuine solid-ECF fills accelerated, 2641/3533 op-1 calls accelerated
(the remaining 892 correctly rejected as overlapping - a real,
frequent case in ordinary use, e.g. scrolling), zero failures on
either op**, desktop confirmed visually correct throughout, including
during scrolling (exactly where overlapping copies get skipped and
fall back to normal rendering).

**This completes the original 5-step GraphicsV Render accelerator
plan, hardware-verified end to end.** Real V3D GPU hardware now
transparently accelerates real Wimp desktop rendering - both window
fills and non-overlapping copies - entirely through the documented
`GraphicsV` mechanism, with a proven fail-safe design that has now
been exercised under real, heavy, unplanned desktop load without a
single visible glitch across two separate live test sessions.

## Overlapping copy (2026-09-26, hardware-verified)

Extends op 1 to handle overlapping rectangles - the common real-world
case this driver couldn't accelerate yet (window scrolling is almost
always an overlapping copy: same buffer, content shifted).

**Confirmed the tile-ordering hazard before choosing an approach.** A
`memmove`-style direction choice (process tiles in whichever order
guarantees no `Store` ever overwrites a not-yet-`Load`ed region) looks
appealing since this driver already controls per-tile processing order
itself - but worked through it concretely and found it's NOT
sufficient once the shift is smaller than one tile (64px) in either
dimension, which is the common case (most scrolls are by a modest
number of pixels, not a whole number of tiles). Concretely: with
source tile (0,0) at `[src_x, src_x+64)` and a 20px shift, destination
tile (0,0) at `[src_x+20, src_x+84)` overlaps the FIRST 20 columns of
SOURCE tile (1,0)'s own region `[src_x+64, src_x+128)` - so processing
tile (0,0) first and writing its `Store` can corrupt part of what tile
(1,0)'s `Load` still needs, regardless of macro-level row/column
ordering.

**Chosen approach: route through a scratch buffer.** `source -> scratch`
then `scratch -> dest`, both passes using the exact same, unmodified,
already-proven `run_copy` core (source/dest strides were generalised
to be independent, a small change, to support the scratch buffer's own
different stride). Both passes are individually non-overlapping by
construction (scratch is fresh driver memory nothing else touches), so
this needs zero new tile-ordering logic to get wrong - correctness
comes from the structure of the approach, not from careful reasoning
about edge cases. The tradeoff is performance (two full copy passes
instead of one) and a size limit - the scratch buffer itself is
bounded to `pmp_alloc`'s 64-page/call cap, so very large overlapping
copies (e.g. a full-screen scroll) are refused with a clear error and
fall back to normal CPU rendering, same fail-safe philosophy as
everywhere else in this driver.

**New**: `v3d_copy_screen_overlapping` in `cl_copy.c`, wired into
`gfxv_accel.c`'s `handle_render_copy` - overlapping calls, previously
always skipped, now go through this path instead. Isolated test first
(`V3D_OverlapCopyTest`/`v3d_copy_overlap_test`): a single synthetic
buffer with source at `(0,0)` and destination at a deliberately
sub-tile shift (not a multiple of 64px - exactly the case a direct
copy gets wrong), mirroring a real scroll far more directly than two
separate buffers would. Expected post-copy values are computed
analytically from the source pixel's own test-pattern formula, not by
re-reading the source region after the copy - part of it may itself
have been overwritten where the two rectangles overlap, so "does dest
match src now" would be circular. `test/OverlapCopyTest.bas` samples
points specifically inside the overlap region, the highest-risk area.

Instrumentation extended (`V3D_AccelCopyStats` now also reports
overlap-path accelerated/failed counts, distinct from the direct
path's own) rather than a new SWI, since it's the same underlying
question (op 1 outcomes) just with one more path to distinguish.

**Confirmed on real hardware 2026-09-26, first attempt**: all 8 sample
points matched (source's original value vs. destination's actual
value), including every point specifically inside the overlap region -
the highest-risk area for this whole mechanism. The scratch-buffer
approach's core claim held: because both passes are individually
non-overlapping by construction, no tile-ordering reasoning was needed
to get this right, and none was - it worked first try.

**One gap closed retroactively**: `handle_render_copy` already called
`v3d_copy_screen_overlapping` for real desktop traffic as soon as the
previous commit landed, before a real-framestore-specific isolated
test existed - unlike every other real-screen feature in this driver,
which got its own dedicated real-framestore step before going live.
Added `V3D_ScreenCopyOverlapTest`/`test/ScreenCopyOverlapTest.bas` to
close that gap: a genuinely overlapping, sub-tile-shifted copy (mimics
a real scroll - same rectangle shifted by `(30,20)` pixels) directly
against the real screen.

**Confirmed on real hardware 2026-09-26**: the source area's content
appeared correctly shifted at the destination, with no visible
corruption in the overlap region - visually confirmed. Overlapping
copy is now hardware-verified end to end: synthetic buffer, real
framestore, and (from the moment it was wired in) the live desktop
handler. This closes the last gap in the original 5-step plan - window
scrolling, the most common real-world copy case, is now genuinely
GPU-accelerable, not just non-overlapping copies.

## Scratch-buffer size cap raised, and a real bug found along the way (2026-09-26, hardware-verified)

A live `AccelTest.bas` run with the overlap-copy path active showed
**651/778 (84%) of real window-scroll copies too large for the
scratch buffer's 64-page (256KB) cap**, falling back to normal CPU
rendering - safely (the fail-safe design worked exactly as intended),
but with far less acceleration coverage than hoped (only 16% of real
scrolls fit). A modest window scroll alone (800x600) needs ~470 pages;
64 was never going to cover realistic cases.

**Raised `pmp_alloc`'s per-call cap from 64 to 2048 pages (256KB to
8MB)** - comfortably covers even a full 1920x1080 screen-sized scroll
(~2026 pages) - now exposed as `PMP_MAX_PAGES_PER_CALL` in
`phys_buffer.h` rather than hardcoded `64u` checks scattered across
`cl_fill.c`/`cl_copy.c` (which is exactly how the next problem below
went unnoticed for as long as it did).

**Not a safe change to make by just bumping the constant.** The two
arrays `pmp_alloc()` fills in (`claim_list`/`map_list`) were local
stack variables - at 2048 entries x 12 bytes each, that's 24KB of
SVC-mode kernel stack per array, and RISC OS's SVC stack is a
genuinely limited, easy-to-overflow resource. A stack overflow in a
kernel module crashes the whole machine - a far worse failure mode
than anything else in this project. Fixed by moving `claim_list`/
`map_list` (and `pmp_release`'s own `list`) from stack-local to
function-static arrays instead - static/BSS workspace isn't remotely
constrained the way SVC stack is, and these arrays are only ever
touched from ordinary foreground SVC-mode SWI calls (both functions do
real SWI calls throughout, never safe from IRQ context anyway), so
there's no reentrancy hazard from making them static.

**While making that change, found a real, separate, pre-existing bug**:
`pmp_release()` had no bounds check on `num_pages` at all, unlike
`pmp_alloc()`. `module_final` calls it with a whole area's `max_pages`
in one go - 4096 for `g_pmp_fill` - which already exceeded even the
OLD 64-page array bound, every time a session that had created
`g_pmp_fill` was unloaded. Fixed by making `pmp_release()` chunk
internally in `PMP_MAX_PAGES_PER_CALL`-sized batches, so its own
contract ("works for any `num_pages`") is actually upheld regardless
of how big or small that cap is set to - a correctness fix independent
of the cap-raising work that surfaced it.

**Also had to rework the base-page layout** in `module.c`: several
reservations that used to need only ~41-105 pages of scratch space can
now need up to ~4137 (two `PMP_MAX_PAGES_PER_CALL`-sized buffers plus
a small scratch - e.g. `V3D_CopyTest`'s source+dest, or
`V3D_OverlapCopyTest`'s bounding buffer + its own scratch-buffer pass).
The old hand-computed gaps (200-500 pages) would have silently
collided. Replaced with a uniform scheme instead of more careful
arithmetic: every SWI/accel path gets its own full block
(`block_index * PMP_FILL_BLOCK_PAGES`), trivially non-overlapping
without needing to verify each gap by hand.

**First hardware round found a real, separate bug**: `V3D_OverlapCopyTest`
failed immediately with `"Bad page number"` - a genuine OS error, not
one of this driver's own. `OVERLAP_COPY_TEST_BASE_PAGE` was exactly
`32768` (2^15) with the block size initially set to 8192 pages (49152
total) - and every base at or above it failed identically, while every
base below it (used by fills and direct copies, both still at 100%
success in the same live test run) kept working. Two explanations are
both plausible - an internal limit on PMP page indices, or
`OS_DynamicArea 0` silently clamping the requested `max_pages` below
what was asked for (`pmp_create`'s own comment already flagged this as
possible; never actually checked) - but the fix is the same either
way: **shrunk the block size from 8192 (a padded ~2x-worst-case guess)
to 4608 (the real worst-case need of ~4137 pages plus ~470 pages of
actual margin)**, bringing the total window from 49152 pages down to
27648, comfortably clear of the boundary that failed.

**Confirmed fixed on real hardware, 2026-09-26**: isolated synthetic
test (`V3D_OverlapCopyTest`) - all 8 sample points matched. Real
framestore test (`V3D_ScreenCopyOverlapTest`) - visually confirmed
correct. Live desktop test (`AccelTest.bas`) - **1705/1705 fills,
1211/1211 direct copies, and 224/224 (100%) overlap-path copies
accelerated, zero failures across all three paths**, desktop visually
confirmed correct throughout including scrolling. Real, working,
GPU-accelerated window scrolling, confirmed clean at every level from
an isolated unit test up to sustained real desktop load.
