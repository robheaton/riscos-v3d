REM > CopyTest
REM First step of GraphicsV Render op 1 (copy), for eventual window-
REM scroll acceleration (see README.md). Exercises the new
REM LoadTileBufferGeneral+StoreTileBufferGeneral round trip - genuinely
REM new territory, never used in the reference project's own example
REM code, so this is the first real test of whether it works at all.
REM
REM V3D_CopyTest fills a synthetic source buffer with a per-pixel test
REM pattern (not a flat colour - see cl_copy.c for why: a flat colour
REM can't distinguish "genuinely copied" from "happened to already be
REM there"), copies it to a separate destination buffer via the GPU,
REM and this script samples several points in BOTH buffers directly to
REM confirm the copy matched pixel-for-pixel.
REM
REM Deliberately a non-tile-aligned size (200x150 -> 4x3 tiles, last
REM column/row partial) to exercise partial edge tiles for copy too,
REM same reasoning as FillTest.bas.
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.

width% = 200
height% = 150

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK (clock rate="; rate%; " Hz)"

PRINT "Requesting "; width%; "x"; height%; " copy..."
SYS "V3D_CopyTest", width%, height% TO src%, dest%, stride%, tiles_x%, tiles_y%

PRINT "OK: src="; ~src%; " dest="; ~dest%; " stride="; stride%; " tiles="; tiles_x%; "x"; tiles_y%
PRINT

fail% = FALSE

PROCcheck(0, 0, "Top-left       ")
PROCcheck(width%-1, 0, "Top-right      ")
PROCcheck(0, height%-1, "Bottom-left    ")
PROCcheck(width%-1, height%-1, "Bottom-right   ")
PROCcheck(width% DIV 2, height% DIV 2, "Centre         ")
REM a point inside the partial last tile column (col 3: pixels 192-199)
PROCcheck(196, height% DIV 2, "Partial col    ")
REM a point inside the partial last tile row (row 2: pixels 128-149)
PROCcheck(width% DIV 2, 140, "Partial row    ")

PRINT
IF fail% THEN PRINT "FAIL - one or more sample points did not match." : END
PRINT "All sample points matched - copy correct across the "; tiles_x%; "x"; tiles_y%; " tile grid."
END

DEF PROCcheck(px%, py%, label$)
  offset% = py% * stride% + px% * 4
  src_val% = !(src% + offset%)
  dest_val% = !(dest% + offset%)
  PRINT label$; " ("; px%; ","; py%; "): src="; ~src_val%; " dest="; ~dest_val%;
  IF src_val% = dest_val% THEN PRINT " OK" ELSE PRINT " MISMATCH" : fail% = TRUE
ENDPROC
