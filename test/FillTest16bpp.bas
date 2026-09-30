REM > FillTest16bpp
REM 16bpp screen mode support, Phase 4: isolated synthetic-buffer proof
REM for the fill/store path at 16-bit width, before the real screen
REM framestore is ever touched at this depth. Mirrors CopyTest16bpp.bas's
REM approach (isolated buffer first, real framestore/live desktop next)
REM and Format16Confirm.bas's confirmed 8bpc-in/RGB555-out relationship
REM (colour is sent as 8-bit-per-channel RGBA, R=byte0/G=byte1/B=byte2 -
REM same convention as every 32bpp fill - and read back as RISC OS's own
REM native RGB555 value: R bits0-4, G bits5-9, B bits10-14).
REM
REM Reads back 16-bit values via two ?-byte indirections combined by
REM hand, same as CopyTest16bpp.bas, for the same reason (no established
REM single-operator 16-bit read in this project yet).
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.

width% = 100
height% = 80
colour8% = &000000FF : REM pure red, 8-bit-per-channel RGBA
predicted% = &001F   : REM RISC OS RGB555 pure red (confirmed, Format16Confirm.bas)

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK (clock rate="; rate%; " Hz)"

PRINT "Requesting "; width%; "x"; height%; " 16bpp fill, colour="; ~colour8%; "..."
SYS "V3D_FillTest16bpp", width%, height%, colour8% TO phys%, stride%, tiles_x%, tiles_y%, log%

PRINT "OK: phys="; ~phys%; " log="; ~log%; " stride="; stride%; " tiles="; tiles_x%; "x"; tiles_y%
PRINT "Predicted raw pixel value: "; ~predicted%
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
PRINT "All sample points matched - 16bpp fill correct across the "; tiles_x%; "x"; tiles_y%; " tile grid."
END

DEF PROCcheck(px%, py%, label$)
  offset% = py% * stride% + px% * 2
  val% = ?(log% + offset%) OR (?(log% + offset% + 1) << 8)
  PRINT label$; " ("; px%; ","; py%; "): readback="; ~val%;
  IF val% = predicted% THEN PRINT " OK" ELSE PRINT " MISMATCH" : fail% = TRUE
ENDPROC
