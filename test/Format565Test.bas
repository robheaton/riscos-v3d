REM > Format565Test
REM 16bpp screen mode support - CORRECTION (2026-09-30): this hardware's
REM real 16bpp screen mode is genuine RGB565 (5-6-5, all 16 bits used for
REM colour), NOT the RGB555 (5-5-5 + 1 reserved bit) Phase 1-4 originally
REM assumed from the classic RISC OS PRM sprite-format doc. Confirmed
REM directly from the real Raspberry Pi BCMVideo HAL driver source
REM (RiscOS/Sources/Video/HWSupport/BCMVideo/s/GraphicsV): "16bpp is
REM RGB565, i.e. RRRRRGGGGGGBBBBB" - and the underlying firmware
REM framebuffer (dispmanx) is HARDWIRED to 565, which is why "32 thousand
REM colours" is greyed out on this hardware - it was never available.
REM
REM The V3D render-target format enum has exactly ONE 565-shaped entry:
REM BGR565 (code 7) - no plain "RGB565" alternative exists. This project
REM has already been burned once by assuming a V3D format's NAME
REM describes its real channel order (format 4 was named "ABGR1555" but
REM actually placed channels R/B-swapped relative to what the name
REM suggests) - so this must be settled empirically again, not assumed
REM either way.
REM
REM This reuses the EXISTING V3D_16bppFormatTest SWI (already generic
REM over format_code, built for Phase 2's 555 investigation) - no new
REM module build needed for this step. Sends proper 8-bit-per-channel
REM pure R/G/B inputs (not pre-packed) per Phase 2's own confirmed
REM finding that the clear-colour packet always takes 8bpc RGBA input
REM regardless of target depth, and predicts the CANONICAL, universally-
REM known RGB565 bit masks (0xF800/0x07E0/0x001F) against BOTH possible
REM channel orderings so the result is self-interpreting either way.
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself (any recent
REM build has V3D_16bppFormatTest - no rebuild was needed for this test).

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK"
PRINT

PRINT "Testing V3D format code 7 (BGR565) with pure 8bpc R/G/B inputs."
PRINT "Hypothesis A (matches BCMVideo's own RRRRRGGGGGGBBBBB comment,"
PRINT "  name read low-bit-to-high-bit, same surprise as format 4 before):"
PRINT "  R bits0-4, G bits5-10, B bits11-15 -> red=&001F green=&07E0 blue=&F800"
PRINT "Hypothesis B (name taken at face value, R/B swapped from A):"
PRINT "  B bits0-4, G bits5-10, R bits11-15 -> red=&F800 green=&07E0 blue=&001F"
PRINT

PROCtest(&000000FF, "red  ")
PROCtest(&0000FF00, "green")
PROCtest(&00FF0000, "blue ")

END

DEF PROCtest(colour8%, name$)
  SYS "V3D_16bppFormatTest", 7, colour8% TO phys%, readback%
  hypA$ = "?" : hypB$ = "?"
  IF name$ = "red  " THEN IF readback% = &001F THEN hypA$="MATCH" ELSE hypA$="no"
  IF name$ = "red  " THEN IF readback% = &F800 THEN hypB$="MATCH" ELSE hypB$="no"
  IF name$ = "green" THEN IF readback% = &07E0 THEN hypA$="MATCH" : hypB$="MATCH"
  IF name$ = "blue " THEN IF readback% = &F800 THEN hypA$="MATCH" ELSE hypA$="no"
  IF name$ = "blue " THEN IF readback% = &001F THEN hypB$="MATCH" ELSE hypB$="no"
  PRINT name$; ": sent="; ~colour8%; " readback="; ~readback%; "  hypA="; hypA$; " hypB="; hypB$
ENDPROC
