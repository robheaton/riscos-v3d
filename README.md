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

**Needs verification against ROOL's actual source before relying on it:**
- `HAL_IOAddress()` as the mechanism to turn a device physical address
  into a usable logical pointer inside a module. This is the standard
  pattern on RISC OS HAL ports, but the exact signature/behaviour should
  be confirmed against an existing hardware module in ROOL's tree (e.g.
  grep for `HAL_IOAddress` under an existing Pi-specific driver) rather
  than trusted blindly from this sketch.
- SWI chunk number `0x50EC0` in `cmhg/V3DDriver.cmhg` is a placeholder —
  request a real allocation from ROOL before this touches any machine
  that might run other modules using a colliding chunk.
- The Makefile's exact GCCSDK tool invocations (`cmunge` name/flags) —
  shape is standard, but confirm against whatever GCCSDK build is
  actually installed.

## Layout

```
riscos-v3d/
  cmhg/V3DDriver.cmhg   module header: title, SWI chunk, entry points
  c/v3d_regs.h          register offsets (see attribution above)
  c/module.c            module_init / module_final / SWI handler
  Makefile              GCCSDK build (needs local toolchain confirmation)
```

## SWIs exposed

- `V3D_PowerOn` (0x50EC0) — deassert V3D reset, release ASB gates.
- `V3D_ReadIdent` (0x50EC1) — returns V3D_HUB_IDENT0/1/2 and
  V3D_CTL_IDENT0 in R0-R3. A correct read confirms the GPU is powered
  and responding — Mesa's `v3d_get_device_info()` decodes these same
  fields (core count, QPU count, VPM size, HW version) if we want to
  print something human-readable later.

## Testing on hardware

1. Build with a working GCCSDK RISC OS cross-toolchain.
2. Copy `V3DDriver,ffa` to the Pi 4/CM4, `*RMLoad` it.
3. `*V3D_PowerOn` — should return without error.
4. Something like a tiny BASIC test:
   ```basic
   SYS "V3D_ReadIdent" TO r0,r1,r2,r3
   PRINT ~r0, ~r1, ~r2, ~r3
   ```
   `IDENT0` should decode to the ASCII bytes for the V3D magic/version
   fields — that's the concrete pass/fail signal for this milestone.

## Next milestones (not attempted here)

- M2: allocate a physically-addressed buffer, build a trivial binning +
  rendering control list (clear-to-colour, no shaders), submit via
  `V3D_CLE_CT0QBA`/`CT1QBA`, poll `V3D_CLE_BFC`/`RFC` for completion.
- M3: wrap the rendered buffer as a sprite (`OS_SpriteOp`) and present it
  in a Wimp window.
- Real IRQ handling instead of polling, once the synchronous path works.

Full technical grounding and effort estimate for the milestones beyond
this sketch: see the research summary from the conversation that
produced this skeleton.
