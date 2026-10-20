REM > StressTest
REM Repeatedly exercises V3D_PowerOn + V3D_ClearTest + V3D_ClearToSprite
REM in one session, without reloading the module or rebooting between
REM iterations. V3D_PowerOn is now idempotent (the real bug behind a
REM long "second call always hangs" investigation turned out to be
REM V3D_PowerOn itself: calling it more than once per boot used to
REM re-pulse a hardware reset on an already-working V3D core, which is
REM what broke things - not anything about the submissions between
REM calls). This deliberately calls it every iteration anyway, since
REM real code doing that defensively must not break the GPU either.
REM
REM Also checks V3D_ClearTest's bin_irq/render_irq fields and
REM V3D_ClearToSprite's packed IRQ flag on every iteration - not just
REM overall readback correctness - to confirm real interrupt-driven
REM completion (not the register-polling fallback still sitting
REM alongside it in v3d_poll_counter) holds up under sustained
REM repetition, not just a one-off call. Prints a summary of how many
REM iterations were genuinely IRQ-driven at the end.
REM
REM Prints a marker before every single risky step (not just every 5th
REM iteration) - added after a real hardware freeze that left no clue
REM which of PowerOn/ClearTest/ClearToSprite was in flight when it hit,
REM since the old every-5th-iteration summary print meant "froze before
REM printing anything" could mean anywhere in iterations 1-4. If it
REM freezes again, the last line on screen now pinpoints the exact call.
REM
REM Stops immediately on the first failure (wrong readback, or any
REM SWI error/timeout) with full detail. A clean run to completion is
REM the pass condition; if it ever fails partway through, the printed
REM iteration number and colour are enough to reproduce it directly
REM (SYS "V3D_ClearTest"/"V3D_ClearToSprite" with that same value).
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.

i% = 0
ON ERROR PRINT "FAIL at iteration "; i%; ": "; REPORT$; " (error &"; STR$~(ERR); ")" : END

N% = 50
irq_clear_count% = 0
irq_sprite_count% = 0

FOR i% = 1 TO N%
  PRINT "iter "; i%; ": PowerOn..."
  SYS "V3D_PowerOn" TO ok%, rate%
  IF ok% <> 1 THEN PRINT "FAIL at iteration "; i%; ": V3D_PowerOn returned ok="; ok% : END

  REM vary B/G/R each iteration (i% repeated in every byte, i%<=50 so
  REM no cross-byte carry), force alpha=FF so type-6 TBGR stays opaque
  colour% = (i% * &01010101) OR &FF000000

  PRINT "iter "; i%; ": ClearTest..."
  SYS "V3D_ClearTest", colour% TO phys%, readback%, bin_irq%, render_irq%
  IF readback% <> colour% THEN PRINT "FAIL at iteration "; i%; " (ClearTest): wanted "; ~colour%; " got "; ~readback% : END
  IF bin_irq% = 1 AND render_irq% = 1 THEN irq_clear_count% = irq_clear_count% + 1

  PRINT "iter "; i%; ": ClearToSprite..."
  SYS "V3D_ClearToSprite", colour% TO sprite%, readback2%, physaddr%, sprite_irq%
  IF readback2% <> colour% THEN PRINT "FAIL at iteration "; i%; " (ClearToSprite): wanted "; ~colour%; " got "; ~readback2% : END
  IF sprite_irq% = 1 THEN irq_sprite_count% = irq_sprite_count% + 1

  PRINT "iter "; i%; " OK (clock rate="; rate%; " Hz, ClearTest IRQ="; bin_irq%; "/"; render_irq%; " ClearToSprite IRQ="; sprite_irq%; ")"
NEXT i%

PRINT
PRINT N%; " iterations completed with no failures."
PRINT "V3D_ClearTest was IRQ-driven (both bin+render) on "; irq_clear_count%; " / "; N%; " calls."
PRINT "V3D_ClearToSprite was IRQ-driven on "; irq_sprite_count%; " / "; N%; " calls."
IF irq_clear_count% = N% AND irq_sprite_count% = N% THEN PRINT "Real interrupt-driven completion held up on every single call." ELSE PRINT "Some calls fell back to register polling - not a failure, but worth understanding why."
END
