# V3DDriver SWI reference

Version 0.12 rev g. The module's SWIs are in the chunk **`&5AE00`** with the prefix **`V3D`**, and its errors in the
block **`&823100`**, both allocated by ROOL. Builds before the rev g release used a temporary chunk (`&C0040`): call the SWIs
**by name** (`SYS "V3D_AccelTune"` in BASIC, `OS_SWINumberFromString` in C) and the number never matters.

Registers are written R0, R1 ... "in" is what you pass, "out" what comes back; a register not listed is left as it was.
Every SWI returns an error block on failure (see [Errors](#errors)); the `X` forms (`XV3D_...`) return it instead of
raising it, as usual.

## What the module does by itself

Loading the module is enough. Unless it was loaded with `NoAutoStart`, it claims `GraphicsV` (`V3D_AccelStart` is done
for you), powers the V3D on the first time it is needed, and from then on takes the fills, copies and scrolls the
kernel offers it (see the [README](../README.md)). **None of the SWIs below has to be called for that to work.** They
exist to control it, to measure it, and to test it.

| Group | SWIs |
|---|---|
| [Control](#control) | `V3D_AccelStart`, `V3D_AccelStop`, `V3D_AccelLimits`, `V3D_AccelTune`, `V3D_PowerOn` |
| [Statistics](#statistics) | `V3D_AccelStats`, `V3D_AccelCopyStats`, `V3D_AccelStripedStats`, `V3D_AccelFailInfo`, `V3D_AccelCopyLastError`, `V3D_AccelReentrantCount`, `V3D_AccelTimingStats`, `V3D_AccelProfile` |
| [Diagnostics](#diagnostics) | `V3D_ReadIdent`, `V3D_MemSplit`, `V3D_ClockInfo`, `V3D_ScreenInfo`, `V3D_RegSnapshot` |
| [Development tests](#development-tests) | `V3D_TestBuffer`, `V3D_ClearTest`, `V3D_ClearToSprite`, `V3D_IRQTest`, `V3D_FillTest`, `V3D_FillTest16bpp`, `V3D_StripedFillTest`, `V3D_CopyTest`, `V3D_CopyTest16bpp`, `V3D_OverlapCopyTest`, `V3D_ScreenFillTest`, `V3D_ScreenStripedFillTest`, `V3D_ScreenCopyTest`, `V3D_ScreenCopyOverlapTest`, `V3D_16bppFormatTest` |

The control, statistics and diagnostic SWIs are the interface; the development tests are how the driver was brought up
and are kept for testing. **They are not a stable API** and may change or go.

### SWI numbers


| SWI | Number | | SWI | Number |
|---|---|---|---|---|
| `V3D_PowerOn` | `&5AE00` | | `V3D_AccelCopyStats` | `&5AE11` |
| `V3D_ReadIdent` | `&5AE01` | | `V3D_OverlapCopyTest` | `&5AE12` |
| `V3D_TestBuffer` | `&5AE02` | | `V3D_ScreenCopyOverlapTest` | `&5AE13` |
| `V3D_ClearTest` | `&5AE03` | | `V3D_AccelCopyLastError` | `&5AE14` |
| `V3D_ClearToSprite` | `&5AE04` | | `V3D_RegSnapshot` | `&5AE15` |
| `V3D_MemSplit` | `&5AE05` | | `V3D_StripedFillTest` | `&5AE16` |
| `V3D_ClockInfo` | `&5AE06` | | `V3D_AccelStripedStats` | `&5AE17` |
| `V3D_IRQTest` | `&5AE07` | | `V3D_ScreenStripedFillTest` | `&5AE18` |
| `V3D_ScreenInfo` | `&5AE08` | | `V3D_AccelReentrantCount` | `&5AE19` |
| `V3D_FillTest` | `&5AE09` | | `V3D_AccelTimingStats` | `&5AE1A` |
| `V3D_ScreenFillTest` | `&5AE0A` | | `V3D_16bppFormatTest` | `&5AE1B` |
| `V3D_AccelStart` | `&5AE0B` | | `V3D_CopyTest16bpp` | `&5AE1C` |
| `V3D_AccelStop` | `&5AE0C` | | `V3D_FillTest16bpp` | `&5AE1D` |
| `V3D_AccelStats` | `&5AE0D` | | `V3D_AccelLimits` | `&5AE1E` |
| `V3D_AccelFailInfo` | `&5AE0E` | | `V3D_AccelProfile` | `&5AE1F` |
| `V3D_CopyTest` | `&5AE0F` | | `V3D_AccelTune` | `&5AE20` |
| `V3D_ScreenCopyTest` | `&5AE10` | |  |  |

## Control

#### `V3D_AccelStart` (&5AE0B)

Claim the `GraphicsV` vector so the module starts accelerating. In: nothing. Out: nothing. Does nothing if it is
already claimed. Normally done at load; needed only after `V3D_AccelStop`, or after loading with `NoAutoStart`.

#### `V3D_AccelStop` (&5AE0C)

Release `GraphicsV`: the kernel's own code does everything again. In: nothing. Out: nothing. The module stays loaded and
the V3D stays powered. Used by `FillCopyBench` to time the CPU.

#### `V3D_AccelLimits` (&5AE1E)

The size below which a fill or copy is left to the CPU, because every GPU call has a fixed cost of about 119 microseconds
(see the README). Sizes are in bytes of pixel data (width x height x bytes per pixel), so they scale with the screen depth.

| | In | Out |
|---|---|---|
| R0 | minimum fill bytes, or -1 to leave unchanged | the value before this call |
| R1 | minimum fill rows (or -1) | the value before |
| R2 | minimum copy bytes (or -1) | the value before |
| R3 | minimum copy rows (or -1) | the value before |
| R4 | minimum overlapping-copy (scroll) bytes (or -1) | the value before |
| R5 | | fills declined as too small, since load |
| R6 | | copies declined as too small, since load |

Defaults: fill 16,384 bytes and 32 rows (64x64 at 32bpp); copy 2,048 bytes and 16 rows; scroll 2,048 bytes. All five set
to 0 sends everything to the GPU (used to test). All -1 just reads the values and the counts.

#### `V3D_AccelTune` (&5AE20)

Run-time switches. In: R0 = the setting, R1 = the new value, or -1 to just read it. Out: R0 = the value before the call
(for setting 8, see below). An unknown setting or a value out of range gives error 47.

| R0 | Setting | R1 |
|---|---|---|
| 0 | `zero_mode`: how much of the scratch memory is cleared before a job | 0 or 1 (default 1) |
| 4 | `scratch_attr`: memory type of the scratch pages, applied after a reset (setting 5) | 0 or 1 (default 1) |
| 5 | reset the scratch memory: releases the cached pages and claims the three production slots again | ignored; returns R0 = 0 on success |
| 6 | **re-arm after a fail-stop**: clears the latch set by a GPU timeout. Only do this if you are sure the GPU is idle | ignored; returns R0 = the previous latch state (1 = it was set) |
| 7 | `overlap_mode`: 1 = a scroll is one ordered pass (default), 0 = two passes through a bounce buffer | 0 or 1 |
| 8 | the V3D clock, in MHz | MHz to set (clamped to the firmware's maximum; 200 to 1000), or -1 to read. Out: R0 = the rate now in force, R1 = the firmware's maximum. Refused while a job is running |
| 9 | `max_job_tiles`: the most 64x64 tiles one GPU job may hold; a bigger rectangle is cut into bands of whole tile rows | 1 to 510 (default 510), or -1 to read |

Settings 1 to 3 existed in 0.07 to 0.09 and were removed. Setting 0, 4 and 7 to 0 and then 5 restores the 0.06 behaviour.

#### `V3D_PowerOn` (&5AE00)

Make sure the V3D is powered, out of reset and its bus gates open. In: nothing. Out: R0 = 1, R1 = the V3D clock rate in Hz.
Idempotent: later calls in the same module instance return at once. If the hardware is already up (after a reload, say),
nothing is reset; on a freshly booted machine it runs the full power-on sequence. The accelerated path powers on by itself;
you need this only before the [development tests](#development-tests).

## Statistics

Counters since the module was loaded unless a SWI says otherwise. `GpuCheck` and `PlotCensus` in `Tests` print them.

#### `V3D_AccelStats` (&5AE0D)

Out: R0 = `GraphicsV` calls seen (all reasons), R1 = Render calls (reason 13), R2 = fill operations, R3 = fills that were a genuine
solid colour, R4 = fills the GPU did, R5 = fills the GPU tried and failed (the CPU then did them).

#### `V3D_AccelCopyStats` (&5AE11)

Out: R0 = copy operations seen, R1 = of those, overlapping (scrolls), R2 = non-overlapping copies the GPU did, R3 = of
those, failed, R4 = scrolls the GPU did, R5 = scrolls that failed or were too large, R6 = the last failure's error number
(the number within the block, see [Errors](#errors)).

#### `V3D_AccelStripedStats` (&5AE17)

Dither fills, which vary only from row to row. Out: R0 = such fills seen, R1 = done by the GPU, R2 = tried and failed,
R3 = the last failure's error number.

#### `V3D_AccelFailInfo` (&5AE0E)

Where fills failed. Out: R0 = the last failure's error number, R1 to R4 = its rectangle (left, bottom, right, top),
R5 and R6 = the smallest and largest area (in pixels) among failures, R7 and R8 = the smallest and largest among successes.
A smallest value of `&FFFFFFFF` means none was seen.

#### `V3D_AccelCopyLastError` (&5AE14)

Out: R0 = the address of a zero-terminated string, the last copy failure's message (an empty string if there has been none).
In BASIC, read it with `$R0%`.

#### `V3D_AccelReentrantCount` (&5AE19)

Out: R0 = how many times a second fill or copy arrived while one was still in flight and was skipped (the CPU did it). Normally 0.

#### `V3D_AccelTimingStats` (&5AE1A)

Out: R0 = the worst single accelerated call, in centiseconds, R1 = the highest rate of accelerated calls in any second,
R2 = the worst call in microseconds, R3 = 1 if the microsecond timer is real.

#### `V3D_AccelProfile` (&5AE1F)

Several views of the accelerator's state. In: R0 = the view, R1 = a view-specific argument. Out: as listed.

| R0 | View |
|---|---|
| 0, 1 | job times of fills (0) and copies (1); every job counts, so a scroll in two passes counts twice. Out: R0 = jobs, R1 to R5 = sums of microseconds (setup, binning, mid, render, total), R6 = the worst job. **R1 in = 1** zeroes these counters afterwards |
| 2 | health. Out: R0 = 1 if the fail-stop latch is set, R1 = timeouts, R2 = 1 if the microsecond timer is in use, R3 = a write-bank address has been seen, R4 = that address, R5 = the base address rendering uses, R6 = the framestore base, R7 = lost interrupts rescued |
| 3 | the last timeout, part 1. Out: R0 = which job (1 bin, 2 render) + kind << 8 (0 fill, 1 copy) + valid << 16, R1 = interrupt status, R2 = interrupt mask, R3 = thread 0 CS, R4 = thread 0 CA, R5 = thread 0 EA, R6 = thread 1 CS, R7 = thread 1 CA |
| 4 | the last timeout, part 2. Out: R0 = thread 1 EA, R1 = bin frame count, R2 = render frame count, R3 and R4 = the same two before the job, R5 = bin interrupts, R6 = render interrupts, R7 = hub interrupt status |
| 5 | zero the job-time counters of views 0 and 1. No outputs |
| 6 | the screen the driver works from. Out: R0 = the error number (n) of the last reason it declined the screen (0 = none), R1 = the address of that message (`$R1%` in BASIC), R2 = log2 bits per pixel, R3 = line length, R4 = XWindLimit, R5 = YWindLimit, R6 = TotalScreenSize, R7 = the framestore size `GraphicsV` reported |
| 7 | interrupt accounting, never reset. Out: R0 = lost interrupts rescued, R1 = interrupts claimed, R2 = empty interrupts, R3 = bin interrupts, R4 = render interrupts, R5 = jobs started (so R5-R3 and R5-R4 should be 0 or 1 when idle), R6 = times the screen could not be used for a fill or copy, R7 = the error number of the last such decline |
| 8 | one recorded lost-interrupt rescue; R1 in = 0 to 3. Out: R0 = valid, which job (1 bin, 2 render) and kind, R1 = microseconds waited, R2 to R5 = interrupt and thread state, R6 = frame-counter change and whether the interrupt arrived late, R7 = jobs minus interrupts then |
| 9 | the event trace of what `GraphicsV` sent: **R1 in = -1** clears and arms it (it then keeps the next 32 events); R1 = 0 to 31 reads event R1. Out: R0 = 1 + kind << 8 + outcome << 16 (0 = no such event; kinds 1 SetMode, 2 SetDMAAddress, 3 fill, 4 copy; outcomes 0 taken by the GPU, 1 left to the CPU as too small, 2 left for density, 3 not a plain colour, 4 screen unusable, 5 GPU job failed, 6 dithered and small, 7 dithered job failed, 8 nested call), R1 to R5 = the call's parameters, R6 = error number and sequence, R7 = events stored and armed. `TraceDump` prints it |
| 10 | the **census** of fills and copies by size: **R1 in = -1** zeroes it and the job-time sums and starts the clock; otherwise R1 = kind (0 fill, 1 copy, 2 scroll) and R2 = size bucket 0 to 9 (pixel area up to 64, 256, 1K, 4K, 16K, 64K, 256K, 1M, 4M, above). Out: R0 = calls the GPU took, R1 = left to the CPU, R2 = kilopixels the GPU did, R3 = kilopixels left to the CPU, R4 = microseconds since the reset. `PlotCensus` prints it |

An unknown view, or a view-8 index above 3, gives a "bad SWI" error.

## Diagnostics

Read-only, and safe to call at any time.

#### `V3D_ReadIdent` (&5AE01)

Out: R0, R1, R2 = the hub identity registers (`&42554856` is "VHUB"), R3 = the core identity (`&04443356`, "V3D" version 4), R4 and R5 = the status of the two
control-list threads. A V3D that is switched off answers `&DEADBEEF` to every register.

#### `V3D_MemSplit` (&5AE05)

Out: R0 = ARM memory base, R1 = ARM size, R2 = the VideoCore's base, R3 = its size, from the firmware (through `BCMSupport`).

#### `V3D_ClockInfo` (&5AE06)

Asks the firmware about the V3D clock. Out: R0 = the clock state, R1 = its rate in Hz, R2 = its maximum in Hz.
**On a Pi 4 the state reads 0 ("off") even while the V3D is running**, so do not use it to decide whether the V3D is up;
the module itself asks the hardware.

#### `V3D_ScreenInfo` (&5AE08)

The kernel's live screen-mode variables. Out: R0 = mode flags, R1 = log2 bits per pixel, R2 = line length in bytes, R3 = XWindLimit,
R4 = YWindLimit, R5 = ScreenStart, R6 = DisplayStart, R7 = TotalScreenSize.

#### `V3D_RegSnapshot` (&5AE15)

A read-only dump of V3D registers, **without** requiring `V3D_PowerOn`, so a hang can be located. In: R0 selects the bank.

| R0 in | Out |
|---|---|
| anything else (the original form) | R0 = core interrupt status, R1 = hub interrupt status, R2 = thread 0 CS, R3 = thread 1 CS, R4 = thread 0 CA, R5 = thread 0 EA, R6 = thread 1 CA, R7 = thread 1 EA, R8 = ASB master gate, R9 = ASB slave gate |
| `&51D00001` | the power side: R0 = `PM_GRAFX` (bit 6 set = out of reset), R1 = ASB master gate, R2 = ASB slave gate (bit 0 stop request, bit 1 acknowledge, bit 2 empty, bit 3 full) |
| `&51D00002` | the hub bank: R0 = identity, R1 = interrupt status, R2 = interrupt mask, R3 = MMU control |
| `&51D00003` | the core bank: R0 = identity, R1 = interrupt status, R2 = interrupt mask, R3 = bin frame count, R4 = render frame count, R5 = thread 0 CS, R6 = thread 1 CS, R7 = thread 0 CA, R8 = thread 0 EA, R9 = thread 0 tile-state address |

Each bank is a separate call so that, if one hangs the machine, the earlier ones are already on the screen. `ReloadProbe` in the
source tree uses it.

## Development tests

**These write to memory and to the V3D, some of them to the live screen.** Except where it says otherwise they need `V3D_PowerOn` first (error "called before V3D_PowerOn") and a working V3D. Colours are one word with bytes R, G, B, A (byte 0 = R). A "test buffer" is a fresh area in a Dynamic Area of the module (`V3DBuf` or `V3DFill`); the screen tests draw straight into the real framestore, and the Wimp may redraw over the result at once. `y` is counted down from the top of the screen memory.

#### `V3D_TestBuffer` (&5AE02)

Allocates a page of the `V3DBuf` area and writes `&DEADBEEF` into it. Out: R0 = its physical address, R1 = the value read back, R2 = its logical address. No `V3D_PowerOn` needed.

#### `V3D_ClearTest` (&5AE03)

Clears one 64x64 tile of a fresh buffer to a colour with a hand-built control list and waits for the interrupts. In: R0 = colour. Out: R0 = the render target's physical address, R1 = the pixel read back, R2 = bin interrupts seen during the call, R3 = render interrupts seen. The original proof that the GPU works; `StressTest` repeats it. A timeout latches the fail-stop.

#### `V3D_ClearToSprite` (&5AE04)

As `V3D_ClearTest`, but the target is a valid RISC OS sprite. In: R0 = colour. Out: R0 = the sprite area's logical address (pass it to `OS_SpriteOp`), R1 = the pixel read back, R2 = its physical address, R3 = 1 if both completions were interrupt-driven.

#### `V3D_IRQTest` (&5AE07)

Claims the V3D interrupt, exercises the raw HAL clear call, runs an isolated live wait with the interrupt enabled and no job, and releases it. Out: R0 = interrupts claimed during the wait, R1 = spurious ones (both should be 0). Refuses (error 44) once the accelerator has set interrupts up: reboot, or load with `NoAutoStart`.

#### `V3D_FillTest` (&5AE09)

Fills a multi-tile rectangle in a fresh buffer. In: R0 = width, R1 = height, R2 = colour. Out: R0 = the buffer's physical address, R1 = row stride in bytes, R2 = tiles across, R3 = tiles down, R4 = its logical address (sample pixels with `!` in BASIC).

#### `V3D_FillTest16bpp` (&5AE1D)

As `V3D_FillTest`, for a 16 bits per pixel (RGB565) target. Same registers; the colour is still R, G, B bytes.

#### `V3D_StripedFillTest` (&5AE16)

Fills with a pattern that repeats down the rows. In: R0 = width, R1 = height, R2 = the row phase, R3 = a pointer to 8 colour words. Out: R0 = physical address, R1 = row stride, R2 = logical address.

#### `V3D_CopyTest` (&5AE0F)

Copies between two fresh buffers, the source filled with a per-pixel pattern. In: R0 = width, R1 = height. Out: R0 = the source's logical address, R1 = the destination's, R2 = stride in bytes, R3 = tiles across, R4 = tiles down. Compare the two buffers pixel by pixel.

#### `V3D_CopyTest16bpp` (&5AE1C)

As `V3D_CopyTest`, at 16 bits per pixel. Same registers.

#### `V3D_OverlapCopyTest` (&5AE12)

A scroll inside one fresh buffer, through the same code the desktop uses. In: R0 = width, R1 = height, R2 = shift in x, R3 = shift in y (need not be tile-aligned). Out: R0 = the buffer's logical address, R1 = stride, R2 = tiles across, R3 = tiles down. Compare against a CPU copy.

#### `V3D_ScreenFillTest` (&5AE0A)

Fills a rectangle **on the real screen**. In: R0 = x, R1 = y, R2 = width, R3 = height, R4 = colour. Out: R0 = tiles across, R1 = tiles down. Error 18 if the rectangle is outside the mode.

#### `V3D_ScreenStripedFillTest` (&5AE18)

Fills a patterned rectangle on the real screen. In: R0 = x, R1 = y, R2 = width, R3 = height, R4 = the row phase, R5 = a pointer to 8 colour words. No output.

#### `V3D_ScreenCopyTest` (&5AE10)

Copies a rectangle on the real screen. In: R0 = source x, R1 = source y, R2 = destination x, R3 = destination y, R4 = width, R5 = height. Out: R0 = tiles across, R1 = tiles down. Refuses overlapping rectangles.

#### `V3D_ScreenCopyOverlapTest` (&5AE13)

As `V3D_ScreenCopyTest`, but the rectangles may overlap: a scroll on the real screen. Same registers.

#### `V3D_16bppFormatTest` (&5AE1B)

Finds which render-target format matches the 16-bit screen. In: R0 = the V3D format code, R1 = the colour. Out: R0 = the target's physical address, R1 = the raw 16-bit value read back. Format 7 (BGR565) is the one that matches the Pi's RGB565 screen.

## Errors

Every error this module makes is the number **`&823100` plus n**, with the text below. Errors that come from the OS are passed on unchanged.
What the driver *reports* for diagnosis (the "last error number" in `V3D_AccelProfile`, `V3D_AccelCopyStats` and the event trace)
is the plain n, not the full number. In BASIC, after an error, `PRINT ~ERR` shows the full number: `V3D_AccelTune` with an unknown setting gives `82312F`.

Some numbers are shared on purpose by the test SWIs (10 = bad width or height, 14 = rectangle too large, 11 and 12 = timeouts of a fill or a copy); a few are shared by accident (2, 32): the message says which one it was.

| n | Number | Message |
|---|---|---|
| 1 | `&823101` | V3DDriver: HAL_IRQClear not resolved yet - call v3d_irq_resolve_clear first |
| 2 | `&823102` | V3DDriver: timed out waiting for ASB master gate ACK to clear during V3D power-on<br>V3DDriver: v3d_irq_unmask called before registers mapped |
| 3 | `&823103` | V3DDriver: timed out waiting for ASB slave gate ACK to clear during V3D power-on |
| 4 | `&823104` | V3DDriver: V3D_ReadIdent called before V3D_PowerOn |
| 5 | `&823105` | V3DDriver: V3D_ClearTest called before V3D_PowerOn |
| 6 | `&823106` | V3DDriver: pmp_alloc requested with an invalid page count |
| 7 | `&823107` |  |
| 8 | `&823108` |  |
| 9 | `&823109` | V3DDriver: V3D_ClearToSprite called before V3D_PowerOn |
| 10 | `&82310A` | V3DDriver: rectangle width/height must be non-zero and no larger than 16384<br>V3DDriver: V3D_CopyTest width/height must be non-zero<br>V3DDriver: V3D_CopyTest16bpp width/height must be non-zero<br>V3DDriver: overlapping copy width/height must be non-zero<br>V3DDriver: V3D_OverlapCopyTest width/height must be non-zero<br>V3DDriver: V3D_FillTest width/height must be non-zero<br>V3DDriver: V3D_FillTest16bpp width/height must be non-zero<br>V3DDriver: V3D_StripedFillTest width/height must be non-zero |
| 11 | `&82310B` | V3DDriver: fill timed out waiting for binning<br>V3DDriver: copy timed out waiting for binning |
| 12 | `&82310C` | V3DDriver: fill timed out waiting for rendering<br>V3DDriver: copy timed out waiting for rendering |
| 13 | `&82310D` | V3DDriver: V3D_FillTest called before V3D_PowerOn<br>V3DDriver: V3D_StripedFillTest called before V3D_PowerOn |
| 14 | `&82310E` | V3DDriver: rectangle too large for the cached scratch buffers<br>V3DDriver: V3D_CopyTest rectangle too large for this test harness (pmp_alloc's pmp_alloc's per-call page cap (PMP_MAX_PAGES_PER_CALL))<br>V3DDriver: V3D_CopyTest16bpp rectangle too large for its dedicated test PMP slot - try a smaller width/height<br>V3DDriver: overlapping copy too large for a scratch-buffer pass (the bounce buffer's page cap, PMP_MAX_PAGES_PER_CALL)<br>V3DDriver: V3D_OverlapCopyTest rectangle+shift too large for this test harness (pmp_alloc's pmp_alloc's per-call page cap (PMP_MAX_PAGES_PER_CALL))<br>V3DDriver: V3D_FillTest rectangle too large for this test harness (pmp_alloc's pmp_alloc's per-call page cap (PMP_MAX_PAGES_PER_CALL)) - try a smaller width/height. Not a limit of the fill algorithm itself.<br>V3DDriver: V3D_FillTest16bpp rectangle too large for its dedicated test PMP slot - try a smaller width/height.<br>V3DDriver: V3D_StripedFillTest rectangle too large for this test harness (pmp_alloc's per-call page cap) - try a smaller width/height. |
| 15 | `&82310F` | V3DDriver: no free scratch slot (too many distinct PMP bases in use) |
| 16 | `&823110` | V3DDriver: only 32bpp and 16bpp screen modes are supported |
| 17 | `&823111` | V3DDriver: V3D_ScreenFillTest called before V3D_PowerOn<br>V3DDriver: V3D_ScreenStripedFillTest called before V3D_PowerOn |
| 18 | `&823112` | V3DDriver: V3D_ScreenFillTest rectangle is outside the current screen mode's bounds<br>V3DDriver: V3D_ScreenStripedFillTest rectangle is outside the current screen mode's bounds |
| 20 | `&823114` | V3DDriver: GraphicsV 9 (framestore information) was not claimed by the active display driver - it may not report a separate framestore this way |
| 21 | `&823115` |  |
| 22 | `&823116` | V3DDriver: V3D_CopyTest called before V3D_PowerOn |
| 23 | `&823117` | V3DDriver: V3D_ScreenCopyTest called before V3D_PowerOn |
| 24 | `&823118` | V3DDriver: V3D_ScreenCopyTest rectangle is outside the current screen mode's bounds<br>V3DDriver: V3D_ScreenCopyOverlapTest rectangle is outside the current screen mode's bounds |
| 25 | `&823119` | V3DDriver: V3D_ScreenCopyTest source and destination rectangles overlap - not supported yet |
| 26 | `&82311A` | V3DDriver: V3D_OverlapCopyTest called before V3D_PowerOn |
| 27 | `&82311B` | V3DDriver: V3D_ScreenCopyOverlapTest called before V3D_PowerOn |
| 28 | `&82311C` | V3DDriver: V3D_16bppFormatTest called before V3D_PowerOn |
| 29 | `&82311D` | V3DDriver: unsupported screen depth for accelerated copy/fill |
| 30 | `&82311E` | V3DDriver: V3D_CopyTest16bpp called before V3D_PowerOn |
| 32 | `&823120` | V3DDriver: creating the V3DFill Dynamic Area failed earlier - acceleration unavailable until the module is reloaded<br>V3DDriver: V3D_FillTest16bpp called before V3D_PowerOn |
| 33 | `&823121` | V3DDriver: the screen mode's geometry is inconsistent (line length / height exceed the framestore) - declined |
| 34 | `&823122` | V3DDriver: the VDU write bank differs from the display bank and its address has not been seen yet - declined |
| 35 | `&823123` | V3DDriver: the VDU write address lies outside the framestore - declined |
| 40 | `&823128` | V3DDriver: GPU acceleration is switched off because a GPU job timed out earlier - reboot, or use V3D_AccelTune 6 to switch it back on |
| 42 | `&82312A` | V3DDriver: internal error - a control list outgrew its buffer; job not submitted |
| 43 | `&82312B` | V3DDriver: rectangle lies outside the screen - declined |
| 44 | `&82312C` | V3DDriver: V3D_IRQTest cannot run once the accelerator has set up interrupts - it would switch them off for good. Reboot first, or load with NoAutoStart |
| 45 | `&82312D` | V3DDriver: the V3D clock is on but its bus gates are not open (a previous power-on did not complete) - reboot |
| 46 | `&82312E` | V3DDriver: the GPU is switched off after a timeout - re-arm (V3D_AccelTune 6) only if you are sure it is idle |
| 47 | `&82312F` | V3DDriver: V3D_AccelTune: unknown setting, or value out of range |
| 48 | `&823130` | V3DDriver: the GPU is still running a job (or timed out and may still be) - not unloading. Reboot to clear it |
| 49 | `&823131` | V3DDriver: the firmware did not report a maximum V3D clock rate |
| 50 | `&823132` | V3DDriver: the firmware did not report the V3D clock rate after the request |
| 51 | `&823133` | V3DDriver: the V3D clock is only changed with the GPU idle |
| 52 | `&823134` | V3DDriver: a V3D bus gate did not stop, so the V3D is not being reset - reboot |
