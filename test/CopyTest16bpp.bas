REM > CopyTest16bpp
REM 16bpp screen mode support, Phase 3: isolated synthetic-buffer proof
REM for the copy path's LOAD side at 16-bit width, before the real
REM screen framestore is ever touched at this depth. Phase 2 only
REM proved the STORE side (a clear, never a load); the Load packet was
REM already flagged elsewhere in this project as genuinely new
REM territory even at the proven 32bpp width, and has never been
REM tested at 16-bit granularity until this.
REM
REM Mirrors CopyTest.bas exactly (same sample-point structure), just
REM 2-byte pixels instead of 4, read via two ?-byte indirections
REM combined by hand rather than a single word read (BASIC's `!`
REM reads a 4-byte word - there's no well-established single-operator
REM 16-bit equivalent this project has already verified, so this
REM sticks to the one indirection operator (?) already proven
REM elsewhere rather than guess at another one).
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.

width% = 100
height% = 80

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK (clock rate="; rate%; " Hz)"

PRINT "Requesting "; width%; "x"; height%; " 16bpp copy..."
SYS "V3D_CopyTest16bpp", width%, height% TO src%, dest%, stride%, tiles_x%, tiles_y%

PRINT "OK: src="; ~src%; " dest="; ~dest%; " stride="; stride%; " tiles="; tiles_x%; "x"; tiles_y%
PRINT

fail% = FALSE

PROCcheck(0, 0, "Top-left       ")
PROCcheck(width%-1, 0, "Top-right      ")
PROCcheck(0, height%-1, "Bottom-left    ")
PROCcheck(width%-1, height%-1, "Bottom-right   ")
PROCcheck(width% DIV 2, height% DIV 2, "Centre         ")
REM a point inside the partial last tile column/row (100x80 -> 2x2
REM tiles of 64, both columns and rows have a partial second one)
PROCcheck(70, height% DIV 2, "Partial col    ")
PROCcheck(width% DIV 2, 70, "Partial row    ")

PRINT
IF fail% THEN PRINT "FAIL - one or more sample points did not match." : END
PRINT "All sample points matched - 16bpp copy correct across the "; tiles_x%; "x"; tiles_y%; " tile grid."
END

DEF PROCcheck(px%, py%, label$)
  offset% = py% * stride% + px% * 2
  src_val% = ?(src% + offset%) OR (?(src% + offset% + 1) << 8)
  dest_val% = ?(dest% + offset%) OR (?(dest% + offset% + 1) << 8)
  PRINT label$; " ("; px%; ","; py%; "): src="; ~src_val%; " dest="; ~dest_val%;
  IF src_val% = dest_val% THEN PRINT " OK" ELSE PRINT " MISMATCH" : fail% = TRUE
ENDPROC
