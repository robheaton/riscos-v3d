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
  tiles_y (R1). Requires `V3D_PowerOn`; supports 32bpp and 16bpp modes; the
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
  many had a genuine solid ECF pattern and were at or above the size
  limit (R3; since V3DDriver 0.06 smaller fills are declined before the ECF
  is decoded and only appear in `V3D_AccelLimits`' declined count), how many
  were actually accelerated by the GPU (R4), how many were attempted but
  failed (R5). Counters are cumulative from module load and never reset.
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
- `V3D_AccelCopyLastError` (0xC0054) — returns (R0) the address of a
  null-terminated string holding the OS error text of the last copy
  failure on either path (empty if none); read it in BASIC with `$addr%`.
- `V3D_RegSnapshot` (0xC0055) — raw V3D register dump for the paused
  reload-freeze investigation; does not need `V3D_PowerOn`. R0-R7 =
  CTL_INT_STS, HUB_INT_STS, CT0CS, CT1CS, CT0CA, CT0EA, CT1CA, CT1EA;
  R8/R9 = ASB master/slave control.
- `V3D_StripedFillTest` (0xC0056) — isolated synthetic-buffer test of
  dithered fills (see "Dithered/patterned fills"): R0=width, R1=height,
  R2=row_phase, R3=pointer to 8 colour words. Returns physical address
  (R0), row stride (R1), logical address (R2).
- `V3D_AccelStripedStats` (0xC0057) — dithered-fill counters: decoded
  (R0), accelerated (R1), failed (R2), last failure error number (R3).
- `V3D_ScreenStripedFillTest` (0xC0058) — real-framestore dithered
  fill: R0=x, R1=y, R2=width, R3=height, R4=row_phase, R5=pointer to 8
  colour words. 32bpp and 16bpp.
- `V3D_AccelReentrantCount` (0xC0059) — R0 = GraphicsV Render calls
  skipped because one was already in flight (should always be 0).
- `V3D_AccelTimingStats` (0xC005A) — R0 = longest single accelerated
  call (centiseconds), R1 = most accelerated calls in any 1-second window.
- `V3D_16bppFormatTest` (0xC005B), `V3D_CopyTest16bpp` (0xC005C),
  `V3D_FillTest16bpp` (0xC005D) — isolated synthetic-buffer diagnostics
  from the 16bpp work (see "16bpp screen mode support"):
  `V3D_16bppFormatTest` takes a V3D format code (R0) and an 8-bit-per-channel
  colour (R1) and returns the physical address (R0) and the raw 16-bit
  readback (R1); `V3D_CopyTest16bpp` takes width/height (R0/R1) and returns
  source/dest logical addresses (R0/R1), stride (R2), tiles_x/y (R3/R4);
  `V3D_FillTest16bpp` takes width/height/colour (R0-R2) and returns physical
  address (R0), stride (R1), tiles_x/y (R2/R3), logical address (R4).
- `V3D_AccelLimits` (0xC005E) — read or set the size limits below which a
  `GraphicsV` fill or copy is left unclaimed so the CPU does it (see
  "Performance: where the GPU wins" below). In, each `-1` = leave unchanged
  (so five `-1`s just read): R0 = minimum fill bytes, R1 = minimum fill
  rows, R2 = minimum direct-copy bytes, R3 = minimum copy rows (also used
  for overlapping copies), R4 = minimum overlapping-copy bytes, where bytes
  = width x height x bytes per pixel. Out: R0-R4 = the values before this
  call, R5 = fills declined as too small, R6 = copies declined as too small.
  Setting all five to 0 sends everything to the GPU, which is how
  `test/FillCopyBench.bas` measures raw GPU capability. Defaults: fill
  600,000 B / 64 rows, copy 16,384 B / 48 rows, overlapping copy 65,536 B.
- `V3D_AccelProfile` (0xC005F, new in 0.07) — where the time of an accelerated
  job goes, and the health of the GPU path. R0 = view (R1 = 1 to zero the
  counters after reading, views 0/1 only): 0 = fills, 1 = copies (R0 jobs,
  R1-R5 sums of microseconds for setup / bin / mid / render / total, R6 worst
  job); 2 = health (R0 fail-stop latch, R1 timeouts, R2 microsecond timer in
  use, R3/R4 write-bank address seen, R5 base rendering uses, R6 framestore
  base, R7 lost interrupts rescued); 3 and 4 = the registers captured at the
  last GPU timeout; 5 = zero everything; 6 = the screen the driver is working
  from and why it last declined it (R1 points at the message text); 7 (0.11) =
  interrupt accounting (R0 rescues, R1 claimed, R2 empty, R3 bin, R4 render, R5 jobs
  started, R6 screens declined for a fill or copy, R7 error number of the last one);
  8 (0.11) = one recorded lost-interrupt rescue (R1 = 0-3); 9 (0.11) = the event trace
  (R1 = -1 clears and arms it; R1 = 0-31 reads event R1: SetMode, SetDMAAddress and
  each fill/copy Render with what became of it).
- `V3D_AccelTune` (0xC0060, new in 0.07) — run-time settings. R0 = setting, R1 =
  new value or -1 to read; returns the previous value in R0. 0 zero_mode, 4
  scratch_attr, 7 overlap_mode (values 0 or 1; all three default to 1 from 0.10, see
  "0.07"-"0.10" below; 0.06's behaviour is all three at 0), 5 reset the scratch
  buffers (applies scratch_attr; claims them again in foreground code), 6 re-arm the
  GPU after a timeout, 8 the V3D clock: R1 = MHz (200-1000, clamped to the
  firmware's maximum, only with the GPU idle) or -1 to read; returns R0 = the rate
  in force in MHz and R1 = the firmware's maximum in MHz. The clock is never changed
  by default. Settings 1-3 (cl_mode, flush_mode, render_only) existed in 0.07-0.09 and
  were removed in 0.10 because they saved nothing. `V3D_AccelTimingStats` now also
  returns the worst call in microseconds (R2) and whether the 1 MHz timer is in use
  (R3).

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

### Revisited (2026-09-27): proactive register instrumentation, not another fix attempt

Per the note above, this needed a fundamentally different diagnostic
approach - all four prior attempts were *fixes* aimed at a candidate
resource leak (vector, clock state, PMP). None of them were actual
*instrumentation*: this is a hard machine freeze, not a SWI timeout,
so `cl_clear.c`'s own `diagnose_timeout()` register dump never gets a
chance to run - nobody has actually seen real V3D hardware register
state in the moments leading up to this specific freeze.

**New**: `V3D_RegSnapshot` - a plain, read-only dump of
`V3D_CTL_INT_STS`, `V3D_HUB_INT_STS`, `CT0CS`/`CT1CS`, `CT0CA`/`CT0EA`,
`CT1CA`/`CT1EA` (no `V3D_PowerOn` required - plain register reads have
always worked regardless of clock/power state, see M0/M1's own
findings). `test/ReloadFreezeDiag.bas` mirrors `StressTest.bas`'s
exact structure (the scenario that's reproduced this freeze
consistently before) but calls this SWI and prints the results BEFORE
every risky call, not just on failure - if the freeze happens again,
whatever printed last is real forensic data about the exact hardware
state immediately beforehand, rather than another guess.

Not yet run on hardware.

### Revisited again (2026-09-28/29): real register forensics, `module_final` confirmed running, two real cache bugs fixed - still unresolved

`test/ReloadFreezeDiag.bas` finally ran on hardware. First result: froze
with zero output, before even its first line - earlier than the
established "iteration 2 ClearTest" signature. Bisecting the same calls
by hand (typed individually, then all on one line with no gaps) never
reproduced it at all, matching this bug's established non-determinism
(slow/human-paced calls never froze in any of this project's testing;
fast, back-to-back calls did).

**First-ever bounded reproduction.** A shortened repro (register
snapshot, `PowerOn`, `ClearTest`, all on one line) finally hit
`cl_clear.c`'s own `diagnose_timeout()` instead of a hard freeze - real
V3D register state, captured for the first time in this bug's whole
history: `CS=0x00000000(STOP) STS=0x00000000 MMU=0x00000000
CA=0xFFFFFFFF EA=0xFFFFFFFF QBA=w.../r0xFFFFFFFF QEA=w.../r0xFFFFFFFF`.
Within the same register-mapping session, `CS`/`STS`/`MMU` read clean
while `CA`/`EA`/`QBA`/`QEA` (the bin thread's address/queue registers)
all read the classic "nothing answered this read" pattern - genuinely
CT0-specific, not a whole-block failure (confirmed separately: the
`V3D_HUB_IDENT0/1/2`/`V3D_CTL_IDENT0` block reads back the exact known-
good values, and CT1 - untouched this session - reads a clean `0, 0`,
not `0xFFFFFFFF`, ruling out "that's just what an untouched register
looks like").

**ASB bus gate theory, tested and ruled out.** `v3d_ensure_clock_on`
only clears the ASB master/slave bus gate when the firmware's own
clock-already-on flag is false - meaning a second/reload `PowerOn`
skips that clear entirely, even if the gate had independently ended up
re-stopped. A plausible mechanism for exactly this symptom - directly
tested by extending `V3D_RegSnapshot` and `diagnose_timeout` to also
read `ASB_V3D_M_CTRL`/`S_CTRL`. Result: both read clear, not stopped,
at the exact moment `CA`/`EA` were reading garbage. Cleanly falsified,
not left dangling.

**`module_final` confirmed to genuinely run on every `*RMKill`.** Added
`V3DDriver$InitRan`/`$FinalRan` system-variable markers, checkable via
`*Show` independent of any V3D hardware - both fire as expected on real
hardware. Rules out "cleanup silently never executes" as the
explanation for any of this bug's failed fix attempts. (Real
methodology stumble worth recording: the first version of this
diagnostic used SWI `0x20` believing it was `OS_SetVarVal` - it's
actually `OS_Release`, caught only because returning its error instead
of discarding it surfaced "Bad vector release" instead of a sensible
failure. Correct number is `0x24`. A reminder to verify SWI numbers
against a real source rather than trust memory, same lesson `io_map.h`
already documents for `OS_Memory` 14/15.)

**Two real, independently-verified hardware bugs found** by reading the
upstream Linux `drivers/gpu/drm/v3d` driver source directly (not
guessed), each its own fix attempt on this freeze, hardware-tested,
neither of which turned out to be it:

- **GFXH-1897**, a named, Broadcom-acknowledged V3D silicon erratum on
  V3D < 7.1 (this hardware is 4.2): writing `V3D_CTL_L2TCACTL` to start
  a new L2T flush while a previous flush's `L2TFLS` bit is still set can
  clobber `L2TCACTL` and corrupt cache state. `v3d_flush_caches` now
  polls `L2TFLS` with a 100ms wall-clock bound before every flush,
  matching upstream exactly.
- **`V3D_CTL_SLCACTL` reserved bits**: `v3d_flush_caches` wrote a blind
  `0xFFFFFFFF` into this register on every flush (twice per fill/copy/
  clear call). Only 16 of its 32 bits are real, documented fields
  (`TVCCS`/`TDCCS`/`UCC`/`ICC`) - the other 16 are genuinely reserved,
  never touched by the real driver across any version. Now writes only
  the four real fields (`V3D_SLCACTL_ALL4()`). (Checked and ruled out
  the same fetch's other two steps, `v3d_flush_l3`/`v3d_invalidate_l2c`
  - both are version-gated no-ops on V3D 4.2, not a real gap here.)

**Neither fix resolves the freeze**, alone or together - hardware-
tested, including an isolation test with the GFXH-1897 guard
temporarily disabled to check whether its new register read explained
the freeze moving earlier (it didn't - the zero-output symptom
persisted either way). **Seven failed fix attempts now.** Both cache
fixes are kept regardless (real, correct, independent of whether they
explain this bug), and the `V3DDriver$*Ran` markers and `V3D_RegSnapshot`
`ASBM`/`ASBS` fields stay as permanent, low-cost diagnostics.

**Paused, Rob's own call, not abandoned.** Ten-plus hardware round-trips
in this round alone, on top of the twenty-plus before it. The three
remaining patches in the same upstream "Fix RPi 4 system hangs" series
(flush MMU TLB/cache during runtime resume, clean caches before
suspend, reduce autosuspend delay) are PM-runtime-specific and this
driver has no equivalent framework - not yet actually checked in
detail, the most concrete lead if this is resumed.

## Known limitations

- **Do not `*RMKill`/`*RMLoad` this module without a full reboot in
  between.** Doing so reliably freezes the machine outright (a hard,
  unrecoverable freeze requiring a physical reset) the moment any real
  GPU work is exercised afterward - confirmed reproducible across many
  hardware tests. **Always reboot before loading a freshly-built module
  for testing.** This does not affect the module's actual operation
  once loaded - only development-time reloading. Since auto-start is
  back on by default (see below), this now bites on every ordinary
  reload, not just ones where `V3D_AccelStart` was manually run.

  STILL UNRESOLVED as of 2026-09-29, now **PAUSED after seven failed
  fix attempts** (dangling vector claim, reset-pulsing a warm core, a
  leaked PMP Dynamic Area, a diagnostic bisection build, `cl_clear.c`
  scratch-page caching, a GFXH-1897 hardware-erratum guard, and a
  `V3D_CTL_SLCACTL` reserved-bits fix) - see "Revisited again
  (2026-09-28/29)" above for the full writeup of the last two, both
  real, independently-verified bugs against the real hardware
  erratum/register documentation, neither of which turned out to be
  the cause. Real V3D register forensics were finally captured (first
  time in this bug's whole history) and the ASB bus gate theory was
  directly tested and ruled out. `module_final` is now confirmed,
  directly, to genuinely run on every `*RMKill` - ruling out "cleanup
  silently never executes" as the explanation for any of the seven
  attempts. Paused at Rob's own call given the hardware-cycle cost so
  far, not abandoned - the three still-unchecked patches from the same
  upstream erratum series (PM-runtime cache/MMU handling) are the most
  concrete lead if this is picked back up.

- **A second, separate freeze, ROOT-CAUSED AND FIXED**: opening Task
  Manager while `GraphicsV` acceleration was active used to hard-freeze
  the machine 100% of the time, confirmed on a fresh boot with no
  reload involved at all (unlike the freeze above, which needs a
  reload first). See "Task Manager freeze: ROOT-CAUSED AND FIXED FOR
  REAL" below for the full writeup - this is fixed, and auto-start is
  back on by default as a result.

## Auto-start at boot (2026-09-27, hardware-verified; briefly disabled by default after a freeze was found - root-caused, fixed, and RE-ENABLED as of 2026-09-28, see "Task Manager freeze: ROOT-CAUSED AND FIXED FOR REAL" below)

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

**UPDATE (2026-09-27): this section describes the ORIGINAL (now
reverted) auto-start behaviour, kept for history.** A separate, more
serious freeze (Task Manager, see "URGENT: Task Manager freeze" above)
means auto-start is now OFF by default again - the condition is
inverted from what's described here: `GraphicsV` stays opt-in
(`V3D_AccelStart`) unless `AutoStart` (not `NoAutoStart`) is explicitly
given on the command tail. The auto-start MECHANISM itself (the
`cmd_tail_has_word` check, the idempotent `v3d_gfxv_start` call in
`module_init`) is unchanged and still fully hardware-verified below -
only the DEFAULT polarity changed.

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

## Dithered/patterned fills (2026-09-27, build-verified, not yet hardware-tested)

The GraphicsV Render accelerator only claimed op 2 (fill) when the ECF
block decoded as a genuine SOLID colour (all 8 lines' OR mask
`0xFFFFFFFF`, all 8 EOR masks identical). Everything else - including
RISC OS's own dithered/patterned desktop fills (e.g. some greyed-out
icon backgrounds) - fell through unclaimed to the CPU path. Investigated
whether this could be extended, expecting to need shader/texture-
sampling support (this project has none, and neither does the reference
project it verifies against - confirmed via `grep` turning up zero
texture/TMU code in either).

**Turned out not to be needed.** At 32bpp, each ECF line's OR/EOR mask
is a full 32-bit word - exactly ONE pixel wide - so the fixed-function
ECF mechanism can only vary the fill colour VERTICALLY (per scanline,
cycling every 8 rows), never horizontally within a row. A "dithered
fill" at this pixel depth is therefore never a true 2D texture - it's
always expressible as up to 8 distinct row-colours repeating every 8
rows. That means it can be built entirely from the *existing*,
hardware-proven fixed-function fill machinery: up to 8 separate GPU
submissions, one per distinct row colour, each one filling only "every
8th row" of the target rectangle via a store stride inflated to 8x the
real row stride (each submission's own tile grid only covers
`ceil(rows_needing_this_colour / 1)` rows at that inflated stride - see
`cl_fill.c`'s `v3d_striped_fill_raw`).

`ecf_decode_solid` (`gfxv_accel.c`) generalised into `ecf_decode_striped`:
same `OR==0xFFFFFFFF` check on all 8 lines, but no longer also requiring
an identical EOR mask - it now decodes up to 8 independent colours
(`colours_out[line] = ~eor_mask`). A genuine solid fill is simply the
case where all 8 decoded colours happen to be equal - `handle_render_fill`
checks for that itself and keeps using the cheaper single-submission
path when so, only falling through to the up-to-8-submission striped
path otherwise.

**Screen-anchored, not rectangle-anchored**: `row_phase` (which
`colours[]` index applies to the rectangle's own first row) is passed
as the rectangle's absolute on-screen top-down row number, modulo 8
(`v3d_accel_render_striped_fill` in `module.c`) - not 0/rectangle-
relative - so the pattern stays locked to the screen as content
scrolls or redraws, matching RISC OS's own conventional dither-pattern
behaviour (avoiding a "marching ants" effect). This is standard RISC OS
convention, not itself confirmed against the PRM text for this specific
mechanism - low risk either way, since getting it wrong would only be a
visual-consistency bug, not a hang or corruption.

**Known, deliberate tradeoff**: up to 8x the GPU submissions of a solid
fill for the same rectangle. Given this project's own earlier finding
that even solid-fill acceleration's benefit is easy to not notice on a
fast modern ARM core (see the "is there anything else..." discussion
above), a small dithered rectangle could plausibly be a net loss versus
just leaving it unclaimed for the CPU path. Not optimised for yet -
first priority is proving correctness in isolation (this milestone),
matching this project's established methodology throughout.

New isolated test SWI `V3D_StripedFillTest` (see `test/StripedFillTest.bas`)
exercises `v3d_striped_fill_test` against a synthetic PMP buffer only -
deliberately not wired into any real GraphicsV traffic yet at test time,
same "prove it standalone first" pattern as every other feature above.
New live-path instrumentation `V3D_AccelStripedStats` mirrors
`V3D_AccelStats`, tracking real desktop invocations of the new path
separately from the existing solid-fill counters.

**Confirmed on real hardware 2026-09-27, first attempt**: `StripedFillTest.bas`
against a 200x67 rectangle with a genuine 4-colour repeating pattern -
all 24 sample points matched (rows 0-20 spanning more than 2 full 8-row
cycles, plus left/right edge samples confirming no unwanted horizontal
variation within a row, plus the last row of a non-tile-aligned
rectangle). The mechanism is proven correct against a synthetic buffer.

**Real-framestore test (2026-09-27, hardware-verified, second attempt
in a row with zero bugs found)**: new SWI `V3D_ScreenStripedFillTest`
(mirroring `V3D_ScreenFillTest`, real framestore instead of a PMP test
buffer, no screen-anchoring since a one-shot manual test has nothing to
stay anchored against - `row_phase` is caller-supplied) and
`test/ScreenStripedFillTest.bas`. A 150x100 rectangle at (400,300) with
a 4-colour repeating pattern appeared correctly on the real desktop -
clean horizontal stripes, correct position and size, no corruption.

**Live-desktop test (2026-09-27) - wiring confirmed harmless, but the
striped path itself was never actually exercised by real traffic.**
`AccelTest.bas` (now also reporting `V3D_AccelStripedStats`) was run
twice with genuine desktop interaction (dragging/resizing/scrolling
windows, and on the second round deliberately opening greyed-out
Filer/menu items to try to trigger a dithered redraw). Both rounds:
**existing solid-fill and copy traffic stayed 100% accelerated with
zero errors** (811/811 then 1246/1246 fills, 717 then 836 copies
including all overlap-path ones) - the new striped-dispatch code causes
no regression to the paths already proven live. **But `V3D_AccelStripedStats`
reported 0 striped calls in both rounds** - the live handler was never
even OFFERED a non-solid ECF fill to accelerate, so the striped path
itself remains hardware-proven only in isolation (`V3D_StripedFillTest`)
and via direct real-framestore injection (`V3D_ScreenStripedFillTest`),
never via genuine Wimp-generated traffic.

**Working theory for why, not yet independently confirmed**: dithered
ECF fills are primarily a workaround for LOW colour-depth screen modes
(256-colour, 16-colour), where the OS dithers two palette colours
together to approximate an RGB value with no exact palette match. This
driver's screen (confirmed back in Step 1) is 1920x1080 at **32bpp**,
where every colour is exactly representable already - so RISC OS's own
fill code plausibly has no reason to ever emit a non-solid ECF pattern
on this specific hardware/mode at all, independent of what kind of UI
element is being redrawn. Separately, greyed-out/disabled menu text
specifically was a bad choice of trigger regardless of colour depth -
RISC OS dims disabled icon TEXT via font/glyph rendering, not a filled
rectangle, so it was never going to route through `GraphicsV` op 2 in
the first place (consistent with this project's earlier, separate
finding that text/icons/sprites can't be accelerated via `GraphicsV` at
all - see "is it possible to add more..." discussion history).
Confirming the colour-depth theory directly would mean testing at a
lower `Log2BPP`, which `ensure_screen_framestore` currently outright
rejects (32bpp-only) and would be a separate, riskier piece of work
(this driver has no screen-mode-change handling at all yet) - not
pursued further for now.

**Status at the time: treated as complete** - the mechanism is hardware-
proven correct (isolated buffer, first attempt; real framestore, first
attempt) and the live dispatch wiring was believed harmless based on
two real-desktop test rounds (ordinary drag/resize/scroll interaction).
**Superseded almost immediately - see the next section**: a usage
pattern those two rounds never happened to exercise (an unattended,
timer-driven redraw of a small-percentage-bar list) turned out to
freeze the machine hard.

## Task Manager freeze: root-caused and fixed (2026-09-27)

**Root cause found.** Real timing instrumentation (`V3D_AccelTimingStats`,
added specifically to test this) showed a normal accelerated GraphicsV
call completing in ~10ms, with ordinary desktop interaction already
producing bursts of up to 176 accelerated calls/second. Under Task
Manager's own redraw traffic, a screen-beacon diagnostic captured at
the moment of freeze showed one accelerated call's wall-clock duration
had hit `cl_fill.c`/`cl_copy.c`'s `wait_for_irq` spin-loop's old
5,000,000-iteration bound - a ~300x slowdown from the 10ms baseline.
That loop runs entirely in SVC mode, so a multi-second stall there
blocks the WHOLE machine for its entire duration - matching the
freeze's exact signature (only the hardware cursor, DMA/interrupt-
driven and decoupled from the CPU, kept moving; everything else was
genuinely unresponsive because the CPU was stuck in this one spin loop,
not because anything had actually crashed).

**Why Task Manager specifically**: real RISC OS source (Switcher's own
`s/Switcher`, `calc_slotsize`/`calc_rmasize`, called unconditionally on
EVERY Wimp Null event while the Task window is open) shows Task
Manager re-checks memory/dynamic-area sizes and force-redraws on any
change, every single poll - a plausible feedback loop, since this
driver's own PMP scratch allocations (used by every accelerated
fill/copy) change exactly the RMA/Dynamic Area sizes Task Manager is
watching, potentially triggering yet more redraws in response to the
very acceleration meant to speed them up. This likely pushed the real
accelerated-call rate far above the 176/sec baseline measured during
ordinary dragging, backlogging the GPU/interrupt pipeline until
individual calls' wall-clock latency spiked into the seconds.

**The actual bug**: `wait_for_irq`'s timeout was a raw spin-COUNT
(5,000,000 iterations), not a real-time bound. That protects against a
tight loop that itself runs slowly, but does nothing against a
genuinely backlogged GPU/interrupt pipeline, where the loop runs at
normal speed but the IRQ it's waiting for is simply delayed for a long
real-world time - iteration count alone can't distinguish "this will
never happen" from "this is coming, just slowly," and wasn't bounding
real elapsed time at all.

**The fix**: `wait_for_irq` (duplicated independently in `cl_fill.c`
and `cl_copy.c`, per this project's established convention of not
touching the separately-proven `cl_clear.c` path) now bounds real
wall-clock time via `v3d_read_centiseconds` (the same
`OS_ReadMonotonicTime` wrapper proven safe by the timing-stats work
above), checked every 4096 inner iterations rather than every single
one (a SWI call is far more expensive than a plain memory read, so
checking every iteration would itself throttle the poll). Bailing out
after `CIRCUIT_BREAKER_TIMEOUT_CS` (30 centiseconds = 300ms - a ~30x
margin above the measured 10ms normal case, generous enough for
legitimately larger operations, while cutting a multi-second stall
short roughly 10x faster) is always safe: the caller's existing
fail-safe handling (`gfxv_accel.c`) already leaves any failed
GraphicsV call unclaimed, falling back to normal CPU rendering - a
fast bailout is strictly better than a slow one, never worse, since
"gave up and let the CPU draw it instead" was always the safe outcome
this driver falls back to on any error.

**Diagnostic methodology that got here** (worth remembering for future
hangs): a reentrancy guard ruled out concurrent-call corruption first;
a single unmistakable red/green "is a call in flight" screen beacon
(bypassing the GPU entirely, so it survives even a CPU lockup) ruled
out a permanently wedged GPU submission; real timing counters
(readable safely via an ordinary, non-freezing test run) established a
normal baseline; only then did a targeted screen-beacon reading confirm
the specific call that stalled. Several earlier attempts at fine-
grained, colour/position-coded screen beacons proved genuinely hard to
read back reliably from a phone photo (JPEG colour grading, cropping,
and small pixel gaps all introduced real ambiguity) - the large,
binary-state (red/green) signal and the plain numeric counters were
what actually resolved this, not the detailed bar charts. Reading the
actual RISC OS Task Manager source (Switcher) directly, rather than
inferring its behaviour from the outside, was what explained *why*
Task Manager specifically triggers this and nothing else tested so far
does.

**UPDATE: hardware-tested, did NOT fix the freeze.** The circuit
breaker is real and correct (a raw spin-count timeout genuinely doesn't
bound real elapsed time under load, and this fix stands on its own
merits), but the freeze still happened afterward, identically. The
big-flag diagnostic (see above) was re-checked at this freeze and came
back GREEN - meaning the last accelerated call had ALREADY finished
cleanly, contradicting the "one call stalls for seconds" theory this
fix targeted. In hindsight, the earlier duration-beacon reading showing
~3 seconds was very likely a real but non-fatal slow call captured
SOMEWHERE during that test run, not the actual freeze event itself -
a red herring, not the cause.

**Second theory tested and also ruled out**: this handler always
returns `VECTOR_PASSON`, even after successfully claiming a call,
meaning the kernel's own downstream GraphicsV claimant (the HAL video
bridge) still runs afterward for the same event - theorised that some
edge case in that downstream interaction, not this driver's own work,
could be the real cause. Tested directly: after a successful claim,
return `VECTOR_CLAIM` instead, stopping the chain right there. **Still
froze, big-flag still GREEN.** This rules out the downstream-claimant
interaction as well.

**Third theory tested: mere claiming/init, independent of real GPU work
("dry run").** Built a version that still runs every setup step (GPU
power-on, PMP Dynamic Area, IRQ vector claim) but deliberately skips
the actual `v3d_fill_screen`/`v3d_copy_screen`/etc call, falling back
to CPU rendering every time. **Did NOT freeze.** This isolates the bug
to real GPU-drawn content actually appearing on screen - ruling out
"just being an active GraphicsV claimant" as sufficient on its own.

**Fourth theory tested: CPU/GPU cache coherency.** Added a genuine
CPU-side readback check (`check_fill_cache_coherency`) right after
every live accelerated solid fill, comparing the actual pixel value
against what was requested - 597 checks, 0 mismatches during ordinary
use. Cross-checked by reading `phys_buffer.c` directly: every PMP
scratch buffer is already explicitly mapped non-cacheable (a real fix
from earlier in this project), and `pmp_alloc` always releases the
previous claim at a base page before reclaiming it (no leak from
reusing the same base page on every call, confirmed by reading the
code, not assumed). **Ruled out.**

**Fifth theory tested: high-frequency Dynamic Area churn on its own.**
New `V3D_PMPChurnTest` SWI does 5000 rapid `pmp_alloc` calls (real
`OS_DynamicArea`/`OS_Memory` SWIs) with ZERO V3D hardware interaction -
no GPU submission, nothing rendered at all - run while Task Manager was
open. Completed cleanly in 190ms. **Ruled out.**

**Where this leaves things**: every hypothesis specific to this
driver's own code has now been tested and ruled out on real hardware -
reentrancy, IRQ-wait timeout, downstream vector-chain interaction, mere
claiming/init, CPU/GPU cache coherency, PMP memory leak, and
high-frequency Dynamic Area churn in isolation. The only confirmed
fact remaining: dry-run (no real GPU-drawn content) never freezes;
real acceleration (actual GPU register writes, rendering, and real
pixel output) eventually does, while this driver's own instrumentation
shows every individual call it can see completing cleanly every time.
This is a genuinely deep, elusive bug at this point - possibly
something cumulative across many real submissions rather than any
single call's fault, or a GraphicsV call type this driver's own "busy"
instrumentation doesn't cover, or an interaction not yet visible to
anything tried so far. The circuit-breaker fix and reentrancy guard
both remain in place as real, defensible improvements on their own
merits, even though neither was the root cause.

**Practical status**: acceleration remains reliable for ordinary
desktop use (thousands of successful, correct accelerated calls in
every test that didn't involve Task Manager) - this is now a narrow,
specific, documented limitation (Task Manager only) rather than a
general reliability problem. Auto-start stays OFF by default until
this is properly root-caused. Continuing to narrow this down further
would need either a fundamentally different diagnostic angle (much
richer real-time V3D register capture, or a way to make the freeze
reproducible via script rather than needing manual Task Manager
interaction each time) or a decision to accept this as a known
limitation for now.

## URGENT: Task Manager freeze - auto-start disabled by default (2026-09-27; RESOLVED 2026-09-28, see "Task Manager freeze: ROOT-CAUSED AND FIXED FOR REAL" below - auto-start is back on by default)

Real-world use immediately after the above (opening Task Manager, no
special action beyond that) produced a **100% reproducible hard machine
freeze** - confirmed on a completely fresh boot, first module load, no
reload involved (a DIFFERENT, more serious bug than the pre-existing
reload-without-reboot freeze documented above - that one needs a reload
first; this one doesn't). Symptom: only the mouse cursor still moves
(hardware cursor, IRQ/DMA-driven, decoupled from the rest of the
system); everything else is completely unresponsive, requiring a power
cycle. Happens every time, specifically when `GraphicsV` acceleration
is active (`V3D_AccelStart` claimed) and Task Manager's window is
opened.

A screen-recording of the freeze, examined frame by frame, shows real
visual corruption in the moments leading up to it, not just a sudden
stop: Task Manager's "Dynamic areas" pane (a list of every RISC OS
dynamic area with a small percentage-used bar next to each, too many
entries to fit at once, so the OS periodically auto-scrolls/redraws it
on a timer with **no user input involved at all**) alternates between
fully correct and visibly garbled - text labels vanish while the
coloured bar fills persist - across several redraw cycles, then the
system locks up.

**This usage pattern - unattended, timer-driven, high-frequency small
redraws with no user interaction - is genuinely different from
everything `AccelTest.bas`'s manual drag/resize/scroll testing has ever
exercised**, and is a strong candidate root cause independent of which
specific accelerated path (fill or copy) turns out to be responsible.
Not yet root-caused - this needs dedicated investigation, likely
starting from `V3D_RegSnapshot`-style register instrumentation captured
right before/during a Task Manager open, same methodology already
built for the (separate) reload freeze.

**Immediate safety action taken**: auto-start is now OFF by default
again (module version bumped to 0.03). Previously, unless `NoAutoStart`
was on the command tail, `module_init` claimed `GraphicsV`
unconditionally - meaning, after the previous milestone, EVERY ordinary
boot was exposed to this freeze the moment anyone opened Task Manager,
a completely routine action. The condition is now inverted: `GraphicsV`
stays fully opt-in (`V3D_AccelStart`, exactly as it was before
auto-start was ever added) unless `AutoStart` is explicitly given on
the command tail (`*RMLoad V3DDriver,ffa AutoStart`), for anyone who
understands the risk and wants to keep testing with it live. **Do not
flip the default back to auto-start-on until this freeze is root-caused
and fixed, not merely worked around** - unlike the reload freeze (which
only bites during development-time reloading), this bites on ordinary
end-user desktop use.

## Task Manager freeze: ROOT-CAUSED AND FIXED FOR REAL (2026-09-28)

The investigation above (circuit breaker, VECTOR_CLAIM, cache
coherency, thin rectangles, PMP churn in isolation) ruled out a long
list of real, well-reasoned hypotheses one at a time, but none of them
were the actual cause - each fix or test that looked promising still
froze identically, or failed to reproduce anything at all. The real
answer needed reading RISC OS's own kernel source directly, not more
guessing from outside.

**The actual mechanism**: `pmp_alloc`'s physical-page claim step
(`OS_DynamicArea` reason 21) unconditionally broadcasts
`Service_MemoryMoved` - confirmed directly in the real kernel source
(`Kernel/s/ChangeDyn`, `DynArea_PMP_PhysOp` → `PMPMemoryMoved` →
`Issue_Service`, no exceptions, every single call). Service calls in
RISC OS are genuinely synchronous - the kernel calls directly into
every registered handler, in the same call stack, immediately. **The
Wimp module itself has a real, non-trivial handler for this exact
service call** - confirmed synchronous free-pool/page-table bookkeeping
in SVC mode, with interrupts deliberately re-enabled mid-handler (an
old documented MOS-bug workaround).

Every accelerated fill or copy used to call `pmp_alloc` up to four
separate times (one each for the CL, RCL, tile_alloc, and tile_state
scratch buffers) - meaning up to four `Service_MemoryMoved` broadcasts
per operation, each one reaching back into the Wimp module's own
internal bookkeeping. Critically, this always happened from **inside**
a `GraphicsV` call the Wimp itself had just dispatched to us - meaning
the Wimp's own reentrant bookkeeping ran nested inside a redraw it was
still in the middle of issuing. This is a fundamentally different, far
nastier kind of reentrancy than anything tested earlier in this
investigation (which all focused on this driver's own vector-handler
reentrancy, already proven not to be the issue) - it's the *Wimp*
re-entering *itself*, indirectly, via our driver's own memory
management calls.

This also explains, precisely, why neither `V3D_PMPChurnTest` nor
`ThinRectStressTest` (isolated, no-Task-Manager-needed stress tests
built earlier in this investigation) ever reproduced it, despite both
doing thousands of real, rapid page claims: **both call this driver
directly via SWI, bypassing `GraphicsV` entirely** - neither one ever
triggers a `pmp_alloc` from *inside* a live Wimp-dispatched redraw, so
neither could ever expose this specific hazard. Only genuine
`GraphicsV`-routed traffic (ordinary dragging, or Task Manager) ever
does that - and Task Manager's own Dynamic Areas list (confirmed via a
real screen recording to be the ONLY part of its window that
continuously redraws/flickers, everything else static) directly
displays this driver's own "V3DFill" dynamic area, watching exactly
the memory churn our own accelerated calls generate.

**The fix**: claim scratch pages ONCE per distinct base page
(generously, sized for a worst-case full-screen operation - 30x17
tiles), then reuse the same already-claimed pages on every subsequent
call, skipping `pmp_alloc` - and therefore the `Service_MemoryMoved`
broadcast - entirely after the first use. Implemented independently in
both `cl_fill.c` (`find_or_create_fill_scratch`) and `cl_copy.c`
(`find_or_create_copy_scratch`, plus a matching cache for the overlap-
copy path's own scratch pixel buffer), following this project's
existing convention of keeping the two files independently diffable.
Safe by construction: the CL/RCL are always rebuilt from scratch on
every call regardless, and tile_alloc/tile_state are always explicitly
zeroed before use - only the *source* of the physical/logical
addresses changes from "always freshly claimed" to "claimed once, then
reused"; none of the control-list-building or GPU-submission logic
changes at all. A request bigger than the cached worst-case size (an
unusually large screen) falls back to the original uncached behaviour
for that one call rather than trying to grow the cache.

**Confirmed on real hardware 2026-09-28**: the exact same test that
had reliably frozen the machine on every previous build - `*RMLoad`,
`V3D_AccelStart`, open Task Manager - ran cleanly with substantial
interaction (scrolling the Dynamic Areas list, resizing the window,
dragging other windows, left open for an extended period). Immediately
followed by a full `AccelTest.bas` run *with Task Manager still open*:
**14,852 GraphicsV handler invocations, 3,216 real accelerated
operations (1164 fills + 1968 direct copies + 84 overlap copies), zero
failures, zero cache-coherency mismatches.** The bug is fixed, not
just worked around - the actual `Service_MemoryMoved` reentrancy
hazard is eliminated from the hot path, not merely made less likely to
trigger.

**Status**: auto-start has been RE-ENABLED by default (module version
0.04) now that the root cause is understood and fixed - a plain
`*RMLoad` is once again sufficient for live desktop acceleration, no
manual `V3D_AccelStart` needed (opt out via `NoAutoStart` on the
command tail if you want manual control instead). The temporary
diagnostic instrumentation accumulated during this investigation
(screen beacons, the dry-run toggle, the per-pixel cache-coherency
check, `V3D_PMPChurnTest`) has been removed - the real, permanent
fixes (scratch-page caching, the wall-clock circuit breaker, the
reentrancy guard, `V3D_AccelReentrantCount`/`V3D_AccelTimingStats` as
lightweight ongoing diagnostics) remain in place.

**Methodology note worth remembering for future hard-to-reproduce
bugs on this project**: real kernel source access
(`~/Development/BCM2835/RiscOS/Sources`) was decisive here, twice -
once to rule out Switcher's own Service-call handler as trivial
(saving time chasing a dead end), and once to find the Wimp module's
own handler as the real hazard. Guessing from outside the black box
(spin-count timeouts, vector-chain protocol details, cache flags) took
many builds and found real, worthwhile improvements along the way, but
never found the actual root cause - reading the actual code that
mattered did, on the very first look at the right file.

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
a self-generated test call here would not have been a safe substitute;
the real reason a script cannot plot from a desktop Task Window was only
found on 2026-10-07: see the "0.08" section below), then reports the stats
and releases.

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

## 16bpp screen mode support (2026-09-30, hardware-verified)

Everything above only ever ran at 32bpp - `ensure_screen_framestore`
hard-rejected any other depth, and the code openly admitted screen
geometry was cached once and "isn't expected to change without a
module reload." Extended to real 16bpp acceleration (not the much
harder indexed/palette depths - the GPU render target can't produce
palette output directly, that would need a whole unbuilt
RGBA8-to-palette conversion pass touching the already-unproven
`emit_load_tile_buffer_rt0` path) across four phases, each
hardware-verified before the next, following this project's usual
isolated-buffer -> real-framestore -> live-desktop methodology.

**Phase 1 - live mode-change safety.** Added a `Service_ModeChange`
(`&46`) handler (`v3d_service_handler`, `module.c`, plus a new
`service-call-handler:` CMHG directive - no precedent for this
directive existed in this project before) that clears
`g_screen_framestore_verified` when the screen mode changes, so the
next accelerated call re-derives geometry from scratch instead of
trusting a stale cache. Two real CMHG/cmunge lessons found against the
actual toolchain, not docs: `service-call-handler:` takes a
space-separated filter number, not a comma (secondhand docs said
comma, cmunge disagreed with a Serious Error); the generated handler
signature is plain `void`, not `void *`. Hardware-confirmed: switching
resolution while accelerating kept targeting the right geometry
afterward.

**Phase 2 - screen pixel format, got a real surprise partway through.**
RISC OS's "16bpp" sprite format is documented (PRM Video chapter) as
15bpp-in-16-bit (RGB555: R bits0-4/G bits5-9/B bits10-14, bit15
reserved) - that's what this phase was originally built and confirmed
against, using an isolated V3D format test (`V3D_16bppFormatTest`)
that settled a genuine ambiguity in the V3D render-target format enum
(two 5:5:5:1-shaped candidates, `ABGR1555=4` and `RGBA5551=45` -
confirmed format 45 matches RGB555's channel positions, format 4 is
R/B-swapped from what its name suggests).

That result was correct about the V3D hardware and wrong about this
hardware's actual screen. Discovered when "32 thousand colours" turned
out to be greyed out and unavailable in the mode chooser - checked the
real Raspberry Pi RISC OS video driver source directly
(`RiscOS/Sources/Video/HWSupport/BCMVideo/s/GraphicsV`): `"16bpp is
RGB565, i.e. RRRRRGGGGGGBBBBB"`, and the firmware's dispmanx
framebuffer "is limited to 16 bit palette entries (565 format)" - this
hardware's HAL driver never offers genuine RGB555 at all; the classic
PRM sprite format describes older VIDC-era hardware this Pi doesn't
have. Checked the real V3D format enum for a 565-shaped candidate:
exactly one exists, `BGR565=7`. Built a fresh isolated test
(`test/Format565Test.bas`, reusing the existing generic
`V3D_16bppFormatTest` SWI, no rebuild needed) rather than trust either
a literal reading of `BGR565`'s name or BCMVideo's own comment string -
**confirmed on real hardware: R sits in the HIGH 5 bits (11-15), G in
the middle 6 (5-10), B in the LOW 5 bits (0-4)**, the standard textbook
RGB565 layout. Worth remembering: two different naming sources (a
vendor driver's own source comment vs. a GPU vendor's format enum) can
use opposite bit-order conventions - verify each independently, a
convention that explains one doesn't necessarily predict the other.

**Phase 3 - 16bpp copy acceleration.** `cl_copy.c`'s `run_copy`/
`v3d_copy_screen`/`v3d_copy_screen_overlapping` gained explicit
`bytes_per_pixel`/`format_code` parameters instead of hardcoding
4-bytes/RGBA8, selected via a new `screen_bpp_params()` helper
(`module.c`). Copy turned out to be entirely unaffected by the later
RGB565 correction: a V3D Load+Store round trip through the *same*
declared format code is bit-exact regardless of whether that format's
bit layout matches the real screen's semantic meaning - already
hardware-proven with a non-colour test pattern
(`V3D_CopyTest16bpp`/`test/CopyTest16bpp.bas`, all 7 sample points
matched exactly). Hardware-confirmed at all three levels: isolated
buffer, real framestore (`V3D_ScreenCopyTest`), and live desktop -
**412 direct + 174 overlap = 586 accelerated copies, zero failures**
(and, in the same live run, 0 accelerated / 361 correctly-rejected
fills, confirming fill's deliberate 32bpp-only guard worked exactly as
designed before Phase 4 existed).

**Phase 4 - 16bpp fill acceleration, plus the correction.**
Mechanically generalized `cl_fill.c`/`module.c` the same way Phase 3
generalized copy. The genuinely new piece: the live GraphicsV fill
accelerator's ECF solid-fill detection (`gfxv_accel.c`) is inherently
32bpp-shaped - each 32-bit OR/EOR mask word is exactly one pixel wide
at 32bpp, but packs *two* horizontally-adjacent sub-pixels at 16bpp.
`ecf_decode_striped` gained a `log2_bpp` parameter: at 16bpp it
requires both packed sub-pixels to decode identically or the whole ECF
block is rejected (a real 2-pixel horizontal repeat this driver's
single-colour-per-row fill can't express - same fail-safe "leave
unclaimed" philosophy as everywhere else in this driver). A new
`widen_565_to_8bit_rgba` helper (`round(v*255/31)` for the 5-bit R/B
fields, `round(v*255/63)` for the 6-bit G field) converts the decoded
raw pixel back to this driver's 8-bit-per-channel convention before
handing it to the unchanged `v3d_accel_render_fill`/`_striped_fill`.

An adversarial code-review pass (three independent reviewers - bit-math,
control-flow, integration lenses - each finding adversarially
re-verified by three more) came back clean on the new decode logic
itself, but caught a real bug: the two small dedicated PMP slots added
for isolated 16bpp testing were only bounds-checked against the
generic per-call page cap, not their own actual ~100-page reservation,
so a moderately large manual test call could have silently overflowed
into the neighbouring slot and corrupted it. Fixed with a real
bounds check against each slot's own real budget before it ever shipped.

**Hardware-confirmed at all three levels with the corrected RGB565
format**: isolated synthetic buffer (`V3D_FillTest16bpp`/
`test/FillTest16bpp.bas`, 7/7 sample points exact); real framestore
(`V3D_ScreenFillTest`, a clean vivid purple rectangle, no colour
distortion); live desktop (`AccelTest.bas`) - **1107/1107 (100%) solid
fills and 818/818 (100%) copies accelerated (488 direct + 330
overlap), zero failures**, comparable throughput to this project's best
historical 32bpp runs, desktop visually confirmed correct throughout.

**All four phases are now complete.** 16bpp direct-colour screen modes
work end to end - live mode switching, and both fill and copy
acceleration - on this hardware's real RGB565 format. Indexed/palette
depths (8bpp/4bpp/1bpp) remain out of scope, for the reason given at
the top of this section.

## 0.07: full code review, hardening, and a tuning harness (2026-10-04, build-verified and host-tested, NOT yet hardware-tested)

After the 16bpp work and the benchmark validation, the whole driver was reviewed
by eight independent read-only reviewers (GPU submission core, memory ordering,
GraphicsV protocol, lifecycle and reload, IRQ and concurrency, performance,
`module.c` and build hygiene, and a roadmap pass). Each claim below was then
checked against the code and the RISC OS kernel/HAL sources before anything was
changed; claims that did not survive are not listed. 0.07 is the result.

**Defects found and fixed**

- *Scratch-buffer overrun.* `find_or_create_*_scratch` returned a cached slot
  before checking the request fitted it. The slot is sized for 30x17 tiles
  (1920x1088); once any small operation had created it, a later operation over
  512 tiles zeroed and wrote past it (data abort in SVC mode, or the binner writing
  into memory the module does not own). Every mode above 1920x1088 was exposed,
  and 1920x1200, 2560x1440 and 4K all exceed it. Now the fit is checked on every
  use, from the exact page counts, and a request that does not fit is declined.
  There is no per-call allocation fallback any more (that was the Task Manager
  freeze trigger).
- *Stale geometry during a mode change.* `ModeChangeSub` clears the new screen
  (`BL FF`, a full-screen GraphicsV fill) BEFORE it issues Service_ModeChange, so
  that clear reached the accelerator with the old mode's cached stride, height
  and depth. 1280x720 to 1920x1080 wrapped the Y flip and aimed an 8MB fill ~1.8MB
  below the framebuffer. The kernel does issue `GraphicsV 6` (SetDMAAddress, R0=3)
  before that clear, so the driver now watches GraphicsV 2 and 6 go past (never
  claiming them) and rebuilds its cache from the already-updated VDU variables;
  Service_PreModeChange (&4D) invalidates too.
- *No bounds checks.* A comment in `gfxv_accel.c` said "downstream validation
  rejects" absurd rectangles; nothing did. Every accelerated rectangle is now
  validated against the verified geometry (`span_ok`, wrap-safe), and the cached
  geometry itself is sanity-checked. The four `V3D_Screen*Test` SWIs had bounds
  checks that wrapped (`x + width` overflow); fixed the same way.
- *Timeout handling.* The "300 ms" circuit breaker was really a 5,000,000-spin cap
  (~10-25 ms), so big jobs could time out falsely, and after a timeout nothing
  stopped the next call re-writing the control lists under a possibly-running
  job while that job's late interrupt satisfied the new job's wait. Now the wait is
  bounded by the 1 MHz timer, a timeout latches acceleration OFF (fail-stop; the
  kernel's CPU/DMA path carries on drawing), the CLE and interrupt registers are
  captured for diagnosis (`V3D_AccelProfile` views 3/4), and a lost interrupt is
  rescued if the frame counter has advanced and the thread has stopped.
- *IRQ handler.* The vector is claimed unshared, so "pass on" falls to the
  kernel's NOIRQ, which disables device 10 permanently. The handler passed on
  whenever it found nothing to service. It now always claims and always
  acknowledges, with a breaker for a genuinely stuck line. Pending status is
  cleared before unmasking and after masking (as Linux does), and a half-built
  `ensure_ready` is undone.
- *Memory ordering.* v0.05's permanent register mapping removed the barriers that
  OS_Memory 14/15 used to supply, and nothing replaced them. A `DSB` is now
  issued before the doorbell write and a `DMB` after completion is seen.
  Architecturally the CPU's write-buffered screen stores and the Strongly-ordered
  doorbell are not ordered against each other.
- *Zombie Dynamic Areas.* Each reload left two (`V3DBuf`, `V3DFill`), because the
  code believed removing a PMP area calls its (refusing) resize handler. The
  kernel source shows it does not (DynArea_Remove takes the DAR_PMP path straight
  to DAR_delink). Areas are now removed in `module_final` and in the stale-area
  cleanup. This is NOT shown to be related to the reload freeze.
- *Unload order.* `module_final` now checks the GPU is idle first and refuses to
  unload otherwise (unless fatal), releases GraphicsV before the interrupt, and
  never frees pages a possibly-running job owns.
- *Other.* A failed `V3DFill` area creation is latched instead of leaking another
  area per call; `V3D_IRQTest` refuses to run once the accelerator has set up
  interrupts (it would switch them off); a "clock already on" power-on now
  verifies the ASB gates are open instead of trusting the firmware flag; calls
  addressed to another GraphicsV driver or head are no longer claimed; a screen
  whose write bank differs from its display bank is declined until the write
  address has been seen (and the observed `GraphicsV 6` address is used as the
  rendering base, as BCMVideo's own Render does); scratch-cache entries are
  dropped whenever their pages are released (the test SWIs used to leave stale
  entries); an unsupported (8bpp) mode is remembered instead of costing a SWI per
  call; control-list writes are bounds-checked.

**The fixed cost, and why it is probably CPU work**

The ~116 us per accelerated call was never shown to be GPU time. Reading the code,
a one-tile operation does: ~4,096 word stores zeroing the tile scratch, ~200 byte
stores building the control lists, and ~22 emulated floating-point instructions
(the viewport maths in `float`, trapping into the FPEmulator in SVC mode), all into
or around Strongly-ordered memory where every store is a separate bus transaction.
0.07 removes the floating point outright (integer maths, checked identical for
every dimension 1-65535; the object files now contain zero FPA instructions where
0.06's `cl_fill.o` had 22), replaces two `OS_ReadMonotonicTime` SWIs per call with
reads of the 1 MHz system timer, and adds run-time switches (`V3D_AccelTune`) to
test the rest on the real hardware without a reboot:

| setting | 0 (default) | alternatives |
|---|---|---|
| zero_mode | zero all tile scratch every call | 1 = only what the request can touch; 2 = once per slot; 3 = never (unproven) |
| cl_mode | build control lists directly in the scratch pages | 1 = build in cached memory, copy with word stores |
| flush_mode | flush GPU caches before both jobs | 1 = before the binning job only |
| render_only | bin job then render job | 1 = fills skip the binning job (unproven: the fill RCL never reads the tile lists) |
| scratch_attr | scratch pages strongly-ordered | 1 = Normal non-cacheable bufferable (word stores merge; needs the DSB) |

`V3D_AccelProfile` reports, per job, where the time goes (setup / bin / mid /
render). `test/TuneBench.bas` runs the combinations and, for each one, VERIFIES the
pixels: a fill, copy and overlapping copy drawn by the GPU are compared with the
same operations drawn by the CPU. A faster setting that is wrong is worthless.

**Structure.** `cl_fill.c` and `cl_copy.c` each carried their own copy of the
scratch cache, wait loop and submission code, so every defect existed twice. That
code is now one place, `c/cl_common.c`, which is also where the host test lives
(it builds the real file against a mock page allocator and checks the cache
invariant "a slot never outlives its pages", the capacity boundary at 512/513
tiles, failure latching, the zero modes and byte-identical control lists in both
build modes). The functions `module.c` exports are declared once, in
`c/accel_api.h`, so a signature mismatch is a compile error.

**Not verified on hardware yet:** everything above. The reload freeze is NOT
fixed and no reviewer found its cause; the clean unload path is new and should be
tested once before it is trusted. Large screens (above 1920x1088) are declined
rather than accelerated; supporting them means sizing the slot from the mode.

## 0.08: first hardware results, the Task Window trap, and a 14x cheaper GPU job (2026-10-07, build-verified and host-tested; hardware verification of 0.08 itself pending)

**0.07 on the real hardware.** `AccelTest.bas`: 34,013 GraphicsV calls, 0 failures,
0 timeouts, 0 declined screens, 0 lost interrupts; 1,222 direct and 1,000 overlapping
copies accelerated (3,222 GPU jobs, averaging setup 294 us / bin 13 / render 464 /
total 771 us, i.e. ~80 tiles a job). `TuneBench.bas`, SAFE set, run from a Filer "Run"
window: **every one of the eight setting combinations produced pixels identical to the
CPU's** for a fill, a copy and an overlapping copy, and a tiny GPU job fell from
**111 us to 8 us** (setup 108 -> 1 us, bin 3 -> 2, render 4 unchanged):

| setting (V3D_AccelTune) | setup | job total | note |
|---|---|---|---|
| baseline (0.06 behaviour) | 103-108 | 111-125 | strongly-ordered scratch, zero every page |
| zero_mode 1 (zero only what is used) | 6-16 | 13-34 | |
| cl_mode 1 (build lists in cached memory) | 100-103 | 107-119 | ~5 us; nothing once scratch_attr is 1 |
| flush_mode 1 | 103-107 | 111-125 | below the timer's resolution |
| scratch_attr 1 ("ncb": Normal non-cacheable bufferable) | 6-8 | 13-25 | |
| scratch_attr 1 + zero_mode 1 | 1-2 | 8-19 | **the 0.08 default** |

Per operation through OS_Plot (microseconds, baseline -> scratch_attr 1 + zero_mode 1):
16x16 fill 120 -> 15, 128x128 fill 132 -> 25, 512x512 fill 473 -> 246; 32x32 copy
122 -> 15, 128x128 copy 143 -> 39, 512x512 copy 643 -> 426; 128x128 scroll 279 -> 68,
256x256 scroll 440 -> 214. The ~116 us "fixed GPU cost" of the 0.06 benchmarks was
about 86% CPU work (word stores into strongly-ordered memory, one bus transaction each,
plus the byte-wise control-list build and the floating-point traps), not GPU time. What
is left is bandwidth-bound: ~3.4 us a tile for a fill, ~6.3 us for a copy (~5 GB/s).
`flush_mode`, `cl_mode`, `zero_mode` 2/3 and `render_only` could save at most a few of
the remaining 8 us, so they are not worth their risk (the EXPERIMENTAL set was never
run, and need not be).

**The Task Window trap (found by reading the kernel and TaskWindow source).** The first
TuneBench run, typed as `*tunebench` into a desktop Task Window, reported every
verification as "not accelerated" and a flat ~80 us per fill and ~118 us per copy
whatever the size. Nothing had been drawn. `OS_Plot` (`Kernel/s/vdu/vduswis` `SWIPlot`)
only calls the VDU plot code directly when nothing but the kernel is on `WrchV`; when
`TaskWindow` has claimed `WrchV` (it does for the life of any Task Window child), the
plot is sent as `VDU 25,code,x,y` through `OS_WriteC`, and `TaskWindow`'s `MyWrchV`
swallows the five bytes. No plot, no `RectangleFill`, no GraphicsV call. The flat cost
was ~8 SWIs per swallowed byte. `FillCopyBench.bas` had worked because it was always
started from the Filer (a "Run ..." window is a `Wimp_CommandWindow`, an ordinary Wimp
task); the comments in the scripts and an earlier README passage claiming "OS_Plot works
from a Task Window" were wrong. `TuneBench.bas` and `VerifySuite.bas` now draw a patch,
read it back from screen memory and stop with an explanation if nothing was drawn.
The ~130 tiny fill jobs a run was recording were other desktop tasks drawing while the
Task Window polled the Wimp; they happened to be a clean measurement of a 4-tile job.

**Verified in the kernel source (not just by the result):** in the page flags word of
`OS_DynamicArea 22`, bit 4 is NotBufferable and bit 5 is NotCacheable; with CP = 0 the
`XCBTableVMSAv6` row maps neither bit to Normal write-back cacheable, bit 4 alone and
bits 4|5 to Strongly-ordered, and **bit 5 alone to Normal non-cacheable (TEX=100, C=0,
B=0)**, never to cacheable. Re-claiming pages that were strongly-ordered with the new
attribute is handled (release returns them to the free pool and every claim/map rewrites
the flags). `Kernel/Docs/PageFlags` describes the bits with the opposite polarity and is
wrong. The `DSB SY` before each doorbell write and the `DMB SY` after the completion
interrupt are the matching barriers.

**Defaults changed in 0.08:** `zero_mode` 1 and `scratch_attr` 1 (the verified fastest
combination). Everything else is as 0.07. Setting `V3D_AccelTune` 0 and 4 back to 0 and
then 5 (reset) restores the 0.06 behaviour exactly.

**Size limits recalibrated.** With a job costing ~8 us instead of ~116 us the break-even
sizes drop by roughly an order of magnitude. From the CPU figures in the table below
(8x8 fill 2.8 us, 64x64 24 us, 128x128 93 us; 8x8 copy 6.5 us, 64x64 145 us) against the
new GPU figures, a fill breaks even near 55x55 pixels and a copy near 16 rows. New
defaults: fill 16,384 B / 32 rows (was 600,000 / 64), copy 2,048 B / 16 rows (was
16,384 / 48), overlapping copy 8,192 B (was 65,536). These sit a little above break-even
and are a first calibration, to be confirmed or tightened from the "Dflt" columns of
`test/FillCopyBench.bas`, which reports CPU, GPU-with-no-limits and default-limits side
by side.

**One-pass scrolling (`V3D_AccelTune 7`, off by default).** Overlapping copies took two
GPU passes through an 8 MB bounce buffer, because a single pass was thought unsafe: with
a fixed forward tile order, a tile's store can overwrite the source of a tile not yet
loaded once the shift is under 64 pixels. That is true of a fixed order, and it is all
the old counterexample showed. If the tiles are visited AHEAD of the shift first -
columns last-to-first when the destination is right of the source, rows last-to-first
when it is lower in memory - every store lands only on source tiles that have already
been loaded, which is memmove's choice of direction made per axis. `test/host/
overlap_order_test.c` models the hardware (full unclipped 64x64 loads, clipped stores,
strictly sequential tiles) over 148,059 random rectangles and shifts (74,210 genuinely
overlapping): **0 failures in sign order, 45,151 in forward order.** A scroll then costs
one job instead of two and needs no bounce buffer. It stays OFF until
`test/VerifySuite.bas` (option 2) passes on the hardware, because the one assumption
the simulation cannot check is the hardware's own: that a single-core CLE really runs the
tiles one after another in the order emitted.

**New test tooling.** `test/VerifySuite.bas` draws hundreds of random fills, copies and
scrolls (1-pixel sizes, 63/64/65-pixel sizes, odd offsets, every shift sign, shifts
over a tile) with the GPU and with the CPU and compares a padded area pixel by pixel, so
a stray write outside the rectangle is caught too. Its dry-run harness
(`test/dryrun/mkstub_verify.py`) can inject GPU bugs (a dropped column, a naive in-place
scroll, a one-pixel overspill) and every one is flagged, at 32bpp and 16bpp.
`test/TuneBench.bas` gained the draw self-check and a row for one-pass scrolls.

**0.08 verified on the hardware, and 0.09 (2026-10-07).** `VerifySuite` ran on the real Pi
at 1920x1080 in both depths: with the 0.08 defaults (`scratch_attr` 1, `zero_mode` 1,
two-pass scrolls) **450 of 450** random cases were pixel-identical to the CPU at
32bpp and at 16bpp (150 fills, 150 copies, 150 scrolls each, 1-pixel to 300-pixel
sizes, odd offsets, a guard margin that would expose any stray write). With
`overlap_mode` 1 (one-pass scrolls) it again passed 450 of 450 at 32bpp and at 16bpp,
and at 16bpp the 150 scrolls cost exactly **150 GPU jobs against 228 in two-pass mode**
(150 plus the 78 the driver saw as overlapping), which is direct proof the single-pass
path was the one running. **0.09 makes `overlap_mode` 1 the default** and changes
nothing else. The size limits are still the first calibration; they are to be tightened
from `test/FillCopyBench.bas` run on 0.09, whose scroll rows now measure one-pass
scrolls.

**Still open.** Hardware confirmation of 0.10 (VerifySuite, FillCopyBench, the clock rows of TuneBench); the reload freeze (untouched); large screens (above 1920x1088 are
declined, not accelerated); removing the now-dead tuning switches once 0.08 is proven.

## 0.10: calibrated from hardware, the switches that did nothing removed, and a clock experiment (2026-10-07, build-verified and host-tested; hardware verification of 0.10 pending)

**0.09 on the hardware.** `test/FillCopyBench.bas` at 1920x1080, 32bpp and 16bpp, with the
0.09 defaults (microseconds per operation, CPU against GPU, from the raw counts; the GPU
column is with every size limit set to 0):

| shape | fill 32bpp | copy 32bpp | scroll 32bpp | fill 16bpp | copy 16bpp |
|---|---|---|---|---|---|
| 8x8 | 2.9 / 11.8 | 6.5 / 12.3 | 6.7 / 12.2 | 2.5 / 11.4 | 5.8 / 11.6 |
| 16x16 | 3.4 / 11.8 | 13.4 / 12.4 | 13.4 / 12.5 | 2.8 / 11.5 | 10.1 / 11.7 |
| 32x32 | 7.5 / 11.8 | 39.8 / 12.8 | 41.0 / 12.7 | 4.8 / 11.4 | 24.9 / 12.0 |
| 64x64 | 24.2 / 13.1 | 144 / 15.9 | 166 / 15.9 | 13.4 / 11.6 | 79.8 / 14.0 |
| 128x128 | 92.5 / 22.5 | 300 / 33.8 | 305 / 34.2 | 45.3 / 20.3 | 290 / 28.5 |
| 256x256 | 214 / 64.1 | 640 / 106 | 641 / 106 | 176 / 55.8 | 608 / 87.2 |
| 512x512 | 618 / 231 | 1984 / 396 | 1984 / 401 | 422 / 197 | 1302 / 322 |
| 1024x16 | 33.7 / 56.0 | 111 / 61.2 | 112 / 61.0 | 19.6 / 55.5 | 66.2 / 59.1 |
| 512x32 | 40.7 / 32.6 | 128 / 40.3 | 132 / 40.3 | 27.1 / 31.5 | 84.8 / 38.0 |
| 16x512 | 42.6 / 32.2 | 294 / 39.9 | 294 / 39.9 | 28.7 / 31.6 | 208 / 35.3 |

A one-pass scroll costs the same as a copy (128x128: 34.2 against 33.8 us), which is
the direct measurement of what the single pass bought: in 0.08 the scroll rows were
twice the copy rows. The GPU wins a fill from 64x64 (1.84x at 32bpp), a copy from 16x16
(1.08x) and by 3x at 32x32 and 9x at 64x64, and large copies by 5x. Only two things were
wrong with the 0.09 limits: a 32x32 scroll was left to the CPU although the GPU does it
3.4x faster (the scroll limit, 8,192 bytes, dated from two-pass scrolls), and fills of two
awkward 16bpp shapes (512x32, 16x512), which touch 8 tiles but carry little data per
tile, ran 10-13% slower than the CPU.

**What 0.10 does about it.** The scroll limit is now the copy limit (2,048 bytes). Fills
also need `bytes + 15 x rows >= 4,500 x tiles` (`GFXV_FILL_MIN_BYTES_PER_TILE`,
`GFXV_FILL_ROW_WEIGHT`; the rows term is there because the CPU is slow on tall
rectangles - a bytes-only rule would have dropped the 32bpp 16x512 fill, which the GPU wins
by 1.31x, and was caught in review),
which removes exactly the two 16bpp losers and keeps every measured winner at both
depths. A full cost model
(CPU = a + b*bytes + c*rows, GPU = g0 + g1*tiles, fitted to all 52 measurements:
GPU fill about 8.6 us + 3.07 us per tile, GPU copy 8.2 + 4.4 per tile) was tried as an
alternative and gives the same overall gain as the limits (1.61x geometric mean over the
26 fill shapes, 3.85x over the copy shapes) with a worst case of 1.00x instead of 0.86x,
which the one extra rule achieves without a model. Checked against all 26 measured fill
shapes (13 at each depth) as a decision table: worst case 1.00x against the CPU, geometric-mean
gain 1.62x, one marginal win (16bpp 64x64, 1.16x) left to the CPU. The rule is skipped when
both fill limits are 0, because "all limits 0 = everything goes to the GPU" is what the
benchmarks and VerifySuite depend on (the first version of the rule ignored that and would
have stopped VerifySuite at its sanity check; a review caught it, and the dry-run stub now
models the size limits so this class of bug shows up there).

**The tuning switches that did nothing were removed.** Building the control lists in
cached memory (`cl_mode`), skipping the second cache flush (`flush_mode`), zeroing never or
once (`zero_mode` 2/3) and skipping the binning job (`render_only`) either saved nothing
once `scratch_attr` was 1 or could only save 2-3 us of the ~8 us a tiny job costs now.
`V3D_AccelTune` keeps `zero_mode`, `scratch_attr` and `overlap_mode` (now all 1 by default) and
gains setting 8.

**A clock experiment (`V3D_AccelTune 8`).** The large-operation costs, ~3.4 us per 64x64 tile
for a fill and ~6.3 us for a copy, are what a tile buffer moving about four pixels per clock at
the 250 MHz the firmware leaves the V3D at would give, i.e. they look clock-bound, and the
firmware reports a 500 MHz maximum (the stock Pi 4 V3D rate). `V3D_AccelTune 8,<MHz>` asks
the firmware for a new rate (clamped to its maximum, GPU idle, foreground only) and returns
the rate in force and the maximum; `8,-1` reads. Nothing changes the clock by default.
`test/TuneBench.bas` has rows `def@400` and `def@500` that set it and run the same eight timed
shapes with a pixel check against the CPU, and `test/VerifySuite.bas` option 4 runs the
450-case fuzz test at the maximum. If large operations speed up and every check passes, the
driver will request the higher clock itself.

**Clock experiment result (2026-10-07, hardware): no effect, so the driver leaves the clock
alone.** `test/TuneBench.bas` on 0.10 set the V3D clock to 400 and 500 MHz (read back as
exactly that, and re-read after each row: unchanged during the runs). Every row passed the
pixel check (fill, copy, scroll all identical to the CPU) with no timeouts, and the render
time per job did not change by even a microsecond: 512x512 fill 220 us at 250, 400 and 500
MHz, 512x512 copy 400-401 us, 128x128 scroll 25-26 us. The large operations are bound by
memory bandwidth (about 4.8 GB/s written by a fill, about 5.2 GB/s moved by a copy), not by
the V3D core clock, which is what the per-tile arithmetic had wrongly suggested. The setting
(`V3D_AccelTune 8`) stays as a documented, off-by-default diagnostic; nothing requests a
higher clock. Per-operation wall time, 0.06-style settings against the 0.10 defaults
(microseconds): 16x16 fill 120 -> 15, 128x128 fill 131 -> 25, 512x512 fill 476 -> 247;
32x32 copy 119 -> 16, 128x128 copy 144 -> 39, 512x512 copy 643 -> 429; 128x128 scroll
279 -> 39, 256x256 scroll 449 -> 117.

**Review.** Three independent read-only reviews of the 0.10 change set checked: the removal
of the switches (a function-by-function disassembly comparison against 0.09 found the job
submission path unchanged: same register writes in the same order, same flushes, same
barrier position), the clock request (tags, request and response layout verified against
BCMSupport, the firmware mailbox documentation and Linux's `clk-raspberrypi.c`, where the V3D
clock is `.maximize = true` and is raised to the firmware maximum when prepared), and the
limits and scripts. They found, and 0.10 fixes: the limits-0 blocker above; the two
failure cases of the clock request (a zero maximum or zero read-back is now an error, and
the clock is only changed with the GPU idle and within 200-1000 MHz); the benchmark
scripts' restore order (limits first, so a refused tuning call cannot leave the limits at 0)
and two early exits that skipped the restore; the clock now re-read after each benchmark row
in case the firmware moved it; and the 8 MB scroll bounce buffer no longer claimed by a scratch
reset when one-pass scrolls are selected. Not verifiable without hardware: whether the firmware
honours a V3D rate request exactly or reverts it later; TuneBench prints the rate before and
after each row so it will show.

## 0.11: forensics for the two things 0.10 could not explain (2026-10-07, hardware-verified)

**ModeChangeTest on 0.10 failed in all six mode changes.** This was the first time the
mode-change handling (the v0.07 fix for aiming the kernel's clear at the NEW screen) had run on
the hardware. Two separate things were wrong in the first run's output: every GPU
verification fill/copy/overlap after the change reported -1 (the GPU never took the
operation), and in four of the six changes a few sampled pixels of the new screen were not
the background colour after the clear. The leftover counts were not random: 52, 144, 39,
and 144 samples, where one whole 64x64 tile holds exactly 52 samples (4 columns x 13 rows) or 48
or 39 depending on its alignment, so the stragglers are whole tiles (one, three, one, three).
Whether a GPU clear missed those tiles, something drew over the screen afterwards, or the test was
fooled (a text cursor or pointer, say), cannot be told from counts, and 0.10 recorded nothing that could
distinguish them; and a screen the driver could not use was declined silently (no counter, and
the cached-error view only shows errors that are remembered, not the transient ones).

What 0.11 adds, none of it changing what the accelerator claims or does:

- **A silent decline is now counted** (`V3D_AccelProfile 7`, R6/R7).
- **An event trace** (`V3D_AccelProfile 9`): off until armed, then the next 32 events are kept -
  GraphicsV SetMode, SetDMAAddress (with the address), and every fill and copy Render with its
  rectangle and what the driver did with it: claimed, left to the CPU (small, density rule, not
  a plain colour), screen unusable (with the error number), GPU job failed (with the error
  number), or nested. The kernel's clear at a mode change is one full-screen fill; the
  trace shows whether the driver was even asked, whether it took it, and why not.
- **Lost-interrupt forensics** (`V3D_AccelProfile 7`, `8`): per-job interrupt accounting and, for each
  of the first four rescued interrupts, what the hardware looked like at that moment (which
  job, how long it had waited, INT_STS/INT_MSK/hub status, thread state, frame-counter
  change, whether the interrupt turned up late), to decide whether the three lost
  interrupts in 2.3 million jobs are a masking race, a missed edge or something else.
- **`test/ModeChangeTest.bas` v2** prints one report at the end (the screen is cleared at each
  mode change): where the leftover pixels are (which tiles, what colour, bounding box), the
  driver's counters and trace for the mode change and for the verification, the screen
  geometry and write address the driver used, and the health flags. A copy is saved as
  `ModeChangeLog`. `test/dryrun/mkstub_mode.py` models a clear that misses a whole tile (bug mode 2)
  and a driver that declines the screen (bug mode 3), and the report identifies both.

**0.11 on the hardware (2026-10-07): the mode-change handling works.**
`test/ModeChangeTest.bas` v3, six mode changes (1024x768, 1920x1080, 1280x720 at 32bpp; 1920x1080
and 1024x768 at 16bpp; back to 1920x1080 32bpp), every one with the GPU on:

- The kernel's clear arrives as exactly one full-screen GraphicsV fill, after GraphicsV 2 (SetMode)
  and GraphicsV 6 (SetDMAAddress) with DAG 2, 1, 3, 0 in that order; the driver rebuilds its
  screen cache from the DAG 3 address, claims the fill, and the whole new screen is background
  (0 of 12,978 to 30,408 sampled pixels wrong, straight after and on a second look).
- Afterwards a GPU fill, copy and overlapping copy each reach the driver, are claimed, and are
  pixel-identical to the CPU's (verify 0/0/0) in every mode and both depths; no screen declined,
  no timeout, no rescue. **ALL 6 MODE CHANGES PASSED.** The plot probe shows OS_Plot, BASIC
  RECTANGLE FILL and VDU 25 all draw and all reach the driver.
- Two things that looked like failures were the test, not the driver: the v2 run from the F12
  command line reported verify -1 everywhere (none of the verification drawing reached GraphicsV
  at all, cause not found; v3 run from the Filer and from F12 did not reproduce it), and one
  white pixel at the top left of the 1920x1080 16bpp screen was the text cursor (the report now
  turns the cursor off before sampling). The 0.10 run's whole-tile "leftovers" were not
  reproduced either; the 0.10 data can no longer be explained and is treated as a test artefact.
- `AccelTest` on 0.11 in the desktop: 2,509 jobs, 2,509 bin and 2,509 render interrupts, no lost
  interrupts, no failures. `FillCopyBench` at 1920x1080 16bpp: the fill density rule works
  (512x32 0.99x, 16x512 1.00x, 1024x16 0.97x against the CPU); the GPU wins fills from 64x64
  (1.13x) and copies and scrolls from 32x32 (2.1x); the slowest default result is 0.92x at a
  16x16 fill and 0.95-0.96x at 8x8 copies, i.e. the cost of declining a call (about 0.1-0.25 us).

**RISCOSmark, 0.06 against 0.11 (2026-10-07, 1920x1080 16M, one reboot per module).** Rectangle
Copy, the one RISCOSmark test that reaches GraphicsV, went from 3,306 (1,366% of the RiscPC
baseline) on 0.06 to 7,750 (3,202%) on 0.11: 2.34x, and 5.8x the 1,333 of the CPU alone. Every
other test is flat within run-to-run noise (Memory 55,567 -> 55,910; Icon Plotting 55,178 ->
55,742; Draw Path 14,488 -> 14,448; Draw Fill 2,635 -> 2,633; Draw Render 5,263 -> 5,257), so the
gain is the driver's and not the machine's. Counters after each run (test/GpuCheck.bas): 0.11 took
6,746 of 6,865 copies and 1,390 of 1,451 scrolls on the GPU (98%, none failed), against 2,869 of
3,098 and 526 of 759 on 0.06 (88%); the benchmark ran 2.2x as many copies in the same time. The
desktop tests (Draw Fill, Draw Render, Icon Plotting) do not reach the accelerator at all, so a
faster driver cannot move them.

## Performance: where the GPU actually wins, and why small operations are left to the CPU (2026-10-04, hardware-verified)

With 16bpp done, the question was whether anything could raise performance
further. Answering it properly meant measuring against something independent
of this project's own counters, then finding out what the GPU path really
costs.

### Independent validation with RISCOSmark

RISCOSmark 2.04 (Richard Spencer, 2003) is a real, pre-existing RISC OS
benchmark, so it cannot flatter this driver. The first two comparisons were
inconclusive (one run had a stale module session, the next changed screen
mode between runs, which also moved tests this driver never touches such as
Icon Plotting). A clean A/B at one mode (32bpp, driver not loaded vs
freshly loaded) was decisive: **Rectangle Copy went from 1,333 (550% of the
RiscPC SA baseline) to 3,159 (1,305%), and to 3,288 (1,358%) with the
current build - 2.47x - while every other test stayed flat** (Processor,
Memory, Icon Plotting, Draw Path/Fill/Render, HD and FS all within noise),
which is the control that makes the result trustworthy.

Whether RISCOSmark's tests even reach this driver was settled from the real
kernel source rather than guessed:

- Its Rectangle Copy calls `OS_Plot` with plot code 190. The kernel's
  `BlockCopyMove` (`Kernel/s/vdu/vdugrafd`) and `RectangleFill`
  (`vdugrafa`) both try `GraphicsV` reason 13 (`GVRender_CopyRectangle` /
  `GVRender_FillRectangle`) before falling back to CPU code, gated by
  `CheckAcceleration` (`vdugrafv`: `Log2BPC` must equal `Log2BPP`, and no
  sprite redirection). That is exactly the mechanism this driver hooks.
- Draw Path / Draw Fill / Draw Render cannot be accelerated this way. The
  Draw module (`Video/Render/DrawMod/s/Draw`) fetches `VduExt_HLineAddr`, a
  raw pointer to the kernel's per-scanline `HLine`, and calls it directly,
  bypassing `RectangleFill` entirely; and a curved polygon is not a
  rectangle, which is all `GraphicsV` can express. Icon Plotting is
  `OS_SpriteOp`, also not routed through `GraphicsV`. Accelerating those
  would be a different, much larger project (V3D geometry submission plus
  replacing DrawMod's renderer), not an extension of this one.
- `GraphicsV` Render defines exactly three operations (NOP, copy rectangle,
  fill rectangle) and this driver already handles both real ones, so there
  is nothing further to hook there. Tiled sprites (`OS_SpriteOp 65`, e.g.
  the desktop backdrop) already use copy-rectangle on this platform because
  BCMVideo reports `CopyRectangleIsFast`.

### The catch: every GPU call has a fixed cost

The driver used to claim every solid fill and every copy, of any size. The
RISCOSmark rectangle is large (301x237), so it hid a problem: each
accelerated call builds control lists, submits to the binner, waits for an
interrupt, submits to the renderer and waits for another. `test/FillCopyBench.bas`
measures this directly: 13 rectangle sizes for fill, non-overlapping copy
and overlapping copy (a scroll), each timed three ways in one session at
one mode - CPU (`V3D_AccelStop`), GPU with every size limit set to 0, and
GPU with the driver's default limits - while reading the driver's own
counters to prove the GPU runs really were accelerated.

Microseconds per operation at 32bpp (operations completed in 500 ms,
500000 / count; every GPU run 100% accelerated):

| Operation | CPU | GPU | GPU speedup |
|---|---|---|---|
| Fill 8x8 | 2.8 | 116 | 0.02x |
| Fill 64x64 | 24 | 117 | 0.21x |
| Fill 128x128 | 93 | 129 | 0.72x |
| Fill 256x256 | 214 | 179 | 1.19x |
| Fill 512x512 | 618 | 460 | 1.34x |
| Fill 1024x16 | 34 | 171 | 0.20x |
| Copy 8x8 | 6.5 | 117 | 0.06x |
| Copy 64x64 | 145 | 120 | 1.20x |
| Copy 128x128 | 300 | 140 | 2.14x |
| Copy 256x256 | 640 | 220 | 2.91x |
| Copy 384x384 | 1,416 | 403 | 3.52x |
| Copy 512x512 | 1,984 | 614 | 3.23x |
| Copy 1024x16 | 111 | 174 | 0.64x |
| Overlap copy 64x64 | 166 | 236 | 0.70x |
| Overlap copy 128x128 | 305 | 274 | 1.11x |
| Overlap copy 256x256 | 640 | 432 | 1.48x |
| Overlap copy 384x384 | 1,433 | 794 | 1.81x |

What the numbers say:

- **A fixed ~116 us per GPU call** (112 us at 16bpp) however small the
  rectangle, and ~230 us for an overlapping copy, which needs two GPU
  passes. The CPU does an 8x8 fill in under 3 us, so claiming everything
  made small operations up to ~60x slower and only large ones faster.
- **Large operations are bandwidth-bound, not overhead-bound**: a copy
  costs ~6-8 us per 64x64 tile (32KB moved, about 4-5 GB/s, roughly what
  the Pi 4's memory can deliver), so the big-copy wins are already near the
  hardware limit; only the fixed cost is improvable, which mainly widens
  the win in the 64-200 pixel range.
- **Fills are a marginal win at best** (1.2-1.34x, only from 256x256), and
  scrolls only win from 128x128 up.
- **Shape matters**: the GPU pays per 64x64 tile touched, the CPU pays per
  pixel and per row, so wide-and-short rectangles (1024x16, 512x32) favour
  the CPU. The CPU's copy cost is dominated by a large per-row overhead
  (about 2.2-2.5 us per row for 256-1000 byte rows), which is why even 64x64
  copies already favour the GPU.
- **16bpp (RGB565)**: the CPU writes half as many bytes, so GPU wins shrink.
  Copy: 1.32x at 96x96, 3.08x at 256x256, 2.45x at 512x512 (CPU 512x512
  copy 1,302 us vs 1,984 us at 32bpp). Solid fills barely win at all (1.05x
  at 256x256, 1.09x at 384x384, 1.02x at 512x512).

### Size limits

`gfxv_accel.c` now leaves a call **unclaimed** when it is too small to
benefit, so the kernel's own CPU code does it - the same fail-safe path every
error already took, so it cannot produce a wrong picture. The limits are in
**bytes of pixel data** (width x height x bytes per pixel, so they follow
the screen depth) plus a minimum height (short rectangles waste most of each
64-row tile):

| | Minimum bytes | Minimum rows | Break-even it came from |
|---|---|---|---|
| Fill | 600,000 | 64 | GPU wins only 1.2-1.34x at 32bpp and ~1.0x at 16bpp; set just above 16bpp 512x512 (524,288 B) |
| Copy | 16,384 | 48 | 64x64 at 32bpp, 96x96 at 16bpp |
| Overlapping copy | 65,536 | 48 | 128x128 (two GPU passes) |

A dithered fill is up to 8 GPU submissions, so it needs 8x the fill limit.
The byte-based copy limit sits correctly at both depths: at 16bpp a 64x64
copy (8,192 B, GPU 0.69x) is declined while 96x96 (18,432 B, GPU 1.32x) is
accelerated. The limits are tunable at run time with `V3D_AccelLimits`,
which also reports how many calls were declined (see its entry above); all
zeros sends everything to the GPU, which is how the benchmark measures raw
capability. `AccelTest.bas` now prints the limits and the declined counts.

### Trimming the cost of declining, and of GPU calls

- **Reject before decoding.** A first 16bpp run showed a *declined* 8x8 fill
  costing 3.8 us against 2.5 us for the CPU fill itself (0.67x), while
  declined copies cost only ~0.15 us. The size check ran after the ECF block
  was decoded, and the 16bpp decode widened all 8 lines with three software
  divisions each. The check now comes first, and the widening uses
  `(v*527+23)>>6` / `(v*259+33)>>6`, brute-force verified identical to the
  old divisions for every 5-bit and 6-bit input. Declined-fill overhead fell
  to ~0.14-0.23 us (8x8 0.67x -> 0.95x, 16x16 0.66x -> 0.93x, 32x32 0.78x ->
  0.96x, 64x64 0.90x -> 0.98x, 128x128 and up ~1.00x).
- **Permanent register mapping.** Every accelerated call used to map the V3D
  register megabyte with a temporary `OS_Memory 14` mapping and unmap it
  (`OS_Memory 15`) twice, although the interrupt code already holds a
  permanent mapping of the same megabyte. The hot paths now use that one
  (`v3d_regs_map`/`v3d_regs_unmap` in `v3d_irq.h`; the temporary mapping is
  the fallback), and the wait loops read the timeout clock far less often.
  The measured gain was only ~3 us per GPU call (119 -> 116 us) - much less
  than guessed - but it also removes the shared, system-wide, steal-able
  temporary mapping from the hot path.

### A benchmark bug worth recording

The first 16bpp fill rows showed `GPU% = 0`: nothing was accelerated. The
benchmark had copied RISCOSmark's `ColourTrans_SetGCOL` flags (`R3 = 256`),
but in the real ColourTrans source (`Video/Render/Colours/s/MainSWIs`) **R3
bit 8 means "generate a dither pattern"**. Dithering is refused above
16bpp, so the colour was solid at 32bpp, but at 16bpp ColourTrans builds a
two-colour pattern that varies horizontally, which `ecf_decode_striped`
correctly declines. The fix is `R3 = 0` (solid). The practical consequence is
real: at 16bpp any fill that asks ColourTrans for a dither pattern falls back
to the CPU by design; the Wimp's own fills are solid and were 100%
accelerated in the live test.

### What real desktop use looks like

A clean `AccelTest.bas` run (fresh boot, nothing else run first, 32bpp, a
couple of minutes of ordinary use) saw 13,923 Render calls (4,612 fills,
9,311 copies), and **87% of them were small rectangles left to the CPU**:

| | Calls | Accelerated | Left to the CPU |
|---|---|---|---|
| Fills | 4,612 | 12 (0.3%) | 4,600 (99.7%) |
| Direct copies | 7,628 | 622 (8%) | 7,006 (92%) |
| Overlapping copies (scrolls) | 1,683 | 1,191 (71%) | 492 (29%) |

with no failures, no reentrant calls, a peak of 284 accelerated calls per
second (the benchmark reaches 8,597) and a worst accelerated call of 10 ms
(it was 30 ms before the limits). The old claim-everything behaviour would
have spent at least ~1.5 s of the CPU waiting on GPU round trips for those
declined calls in that one session, and far more under heavier redraws.
The real GPU value on the desktop is in scrolls and large copies, not fills.

One lesson from getting that run: the statistics counters are cumulative
from module load and are never reset, so a first `AccelTest` run in the same
boot as the benchmark reported ~1 million calls that were almost entirely the
benchmark's own traffic (its 44,854 accelerated fills match the benchmark's
~44,820 almost exactly). A clean desktop measurement needs a fresh boot with
`AccelTest` run alone.

### Test tooling

`test/FillCopyBench.bas` (v4) prints a banner with the script version and
the loaded module's help string, because files move between machines by
hand. It pauses after the raw counts and clears its window before printing
the three tables together, since the benchmark plots in the lower-left of
the screen (possibly underneath its own window) and a long printout scrolls
its top away. Test scripts are dry-run on the development machine through a
BBC BASIC interpreter (Matrix Brandy, unpacked locally from the Ubuntu
package) with the hardware calls stubbed out. That reproduced the failure
`FillCopyBench`'s first version had hit on the real machine (a BASIC error
at its first user-defined string function), so the fix could be verified
before another hardware run. User-defined string functions (`DEF FNxxx$`)
failed both in the interpreter and on the real machine, so the scripts print
through `PROC`s instead.

### Still open

- (Update: 0.07 adds the profiling timer, `V3D_AccelProfile`, and the run-time
  switches described in the "0.07" section above; results pending.) The
  remaining ~116 us fixed cost is two GPU jobs with two interrupt round
  trips, plus control-list building and clearing at least 16KB of scratch even
  for a one-tile operation. It should be profiled before it is changed: the
  real HAL times everything with the BCM GPU system timer (`ST_CLO`, 1 MHz,
  peripheral base + `0x3004`, physical `0xFE003004` on the Pi 4), which is
  read-only and could be mapped once to timestamp each phase. Candidate
  experiments, each a hardware-behaviour change that should be a run-time
  toggle given this project's freeze history: a render-only job (skip the
  bin job, which Linux's v3d driver permits), one tile visit per tile for
  fills instead of the current two (a clear-and-"store none" visit, then the
  real store, copied from the reference's race-condition workaround), and
  zeroing less scratch.
- Fills cost ~4.8-5.3 us per tile against 4-6.5 us for a copy that moves four
  times the data, which is what points at that per-tile overhead.
