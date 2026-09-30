REM > Format16Test
REM Phase 2 of 16bpp screen mode support - determines empirically which
REM V3D render-target format code matches RISC OS's own real RGB555
REM 16bpp screen layout (R bits 0-4, G bits 5-9, B bits 10-14, bit 15
REM reserved - confirmed against the real PRM Video chapter, not
REM guessed). Tests two candidates from the same reference source this
REM project already trusts for RGBA8=27 (ABGR1555=4, RGBA5551=45)
REM against pure red/green/blue test colours chosen specifically to
REM make a channel-order mistake immediately obvious in the raw hex,
REM not something that needs a screenshot to catch - this project has
REM already been burned once by exactly that kind of silent swap (the
REM Task Manager beacon diagnostic).
REM
REM A format code that gets an exact MATCH on all three colours uses
REM RISC OS's own bit layout directly, no swizzling needed to use it
REM for real 16bpp acceleration. If neither matches on all three,
REM the per-channel differences printed below are enough to work out
REM the real mapping by hand.
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK (clock rate="; rate%; " Hz)"
PRINT

red% = &001F : green% = &03E0 : blue% = &7C00

PRINT "Format 4 (ABGR1555 candidate):"
PROCtest(4, red%, "red  ")
PROCtest(4, green%, "green")
PROCtest(4, blue%, "blue ")

PRINT
PRINT "Format 45 (RGBA5551 candidate):"
PROCtest(45, red%, "red  ")
PROCtest(45, green%, "green")
PROCtest(45, blue%, "blue ")

PRINT
PRINT "A format with MATCH on all three rows uses RISC OS's own RGB555"
PRINT "layout directly - that's the one to use for real 16bpp work."
END

DEF PROCtest(format%, colour%, name$)
  SYS "V3D_16bppFormatTest", format%, colour% TO phys%, readback%
  IF readback% = colour% THEN result$ = "MATCH" ELSE result$ = "differs"
  PRINT "  "; name$; ": sent="; ~colour%; " readback="; ~readback%; "  "; result$
ENDPROC
