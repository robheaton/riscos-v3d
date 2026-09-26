REM > FillTest
REM Step 2 of the planned GraphicsV Render accelerator (see README.md):
REM exercises V3D_FillTest's multi-tile fill, still into a synthetic PMP
REM test buffer (not the real screen - that's step 3).
REM
REM Deliberately picks a rectangle that is NOT a whole number of 64x64
REM tiles in either dimension (250x180 -> 4x3 tiles, last column/row
REM partial) specifically to exercise partial edge tiles, which the
REM single-tile-only M2 code never had to handle. Also deliberately
REM stays under pmp_alloc's 64-page/call cap (see cl_fill.c) - this is
REM a limit of THIS TEST HARNESS's synthetic render buffer, not of the
REM fill algorithm (the real GraphicsV feature never PMP-allocates its
REM render target - see step 3).
REM
REM Samples multiple points across the tile grid (all four corners,
REM the centre, and a point in the partial last row/column) rather than
REM just pixel 0 - a multi-tile bug is exactly the kind of thing that
REM would leave tile (0,0) correct while others are wrong.
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.
REM V3D_PowerOn is called below - idempotent, same as StressTest.bas's
REM own defensive every-call style, so this is self-contained.
REM
REM PROCcheck is defined AFTER the final END, not inline here - BBC
REM BASIC does NOT auto-skip a DEF PROC body during ordinary top-to-
REM bottom execution (unlike some other BASIC dialects); falling
REM through into one with its parameters never bound is exactly what
REM produced this script's first real bug ("Unknown or missing
REM variable" on px%/py%, immediately after the DEF line, before the
REM procedure was ever actually called).

width% = 250
height% = 180
REM alpha=FF, arbitrary distinctive B/G/R bytes
colour% = &FF3C9BD6

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK (clock rate="; rate%; " Hz)"

PRINT "Requesting "; width%; "x"; height%; " fill, colour "; ~colour%; "..."
SYS "V3D_FillTest", width%, height%, colour% TO phys%, stride%, tiles_x%, tiles_y%, log%

PRINT "OK: phys="; ~phys%; " stride="; stride%; " tiles="; tiles_x%; "x"; tiles_y%; " log="; ~log%
PRINT

fail% = FALSE

REM DIAGNOSTIC ROUND (2026-09-26): first run showed ONLY pixel (0,0)
REM correct, every other sample point reading back plausible leftover-
REM garbage values (the render buffer is deliberately never zeroed, so
REM an untouched pixel keeps whatever was in that recycled physical
REM page before - see cl_fill.c). These extra points narrow down
REM whether that's a within-tile stride bug (tile 0 itself only
REM partially stored) or a per-tile-loop bug (only tile (0,0) ever got
REM processed at all) before changing any code.
PROCcheck(0, 0, "Tile0 origin   ")
PROCcheck(63, 0, "Tile0 TR corner")
PROCcheck(0, 63, "Tile0 BL corner")
PROCcheck(63, 63, "Tile0 BR corner")
PROCcheck(32, 32, "Tile0 mid      ")
PROCcheck(64, 0, "Tile1 origin   ")
PROCcheck(0, 64, "Tile(0,1) orig ")
PROCcheck(128, 0, "Tile2 origin   ")
PROCcheck(192, 0, "Tile3 origin   ")

PROCcheck(width%-1, 0, "Top-right      ")
PROCcheck(0, height%-1, "Bottom-left    ")
PROCcheck(width%-1, height%-1, "Bottom-right   ")
PROCcheck(width% DIV 2, height% DIV 2, "Centre         ")
REM a point inside the partial last tile column (col 3: pixels 192-249)
PROCcheck(240, height% DIV 2, "Partial col    ")
REM a point inside the partial last tile row (row 2: pixels 128-179)
PROCcheck(width% DIV 2, 170, "Partial row    ")
REM the corner of the partial-partial tile (bottom-right-most tile)
PROCcheck(width%-1, height%-1, "Partial corner ")

PRINT
IF fail% THEN PRINT "FAIL - one or more sample points did not match." : END
PRINT "All sample points matched - multi-tile fill correct across the "; tiles_x%; "x"; tiles_y%; " tile grid."
END

DEF PROCcheck(px%, py%, label$)
  addr% = log% + py% * stride% + px% * 4
  val% = !addr%
  PRINT label$; " ("; px%; ","; py%; "): "; ~val%;
  IF val% = colour% THEN PRINT " OK" ELSE PRINT " MISMATCH (wanted "; ~colour%; ")" : fail% = TRUE
ENDPROC
