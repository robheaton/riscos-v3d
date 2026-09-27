REM > StripedFillTest
REM Dithered/patterned fill support (see README.md's "Dithered/patterned
REM fills" section, cl_fill.c's v3d_striped_fill_raw/v3d_striped_fill_test,
REM and gfxv_accel.c's ecf_decode_striped). Same "prove it against a
REM synthetic buffer before touching the real screen" methodology as
REM FillTest.bas - this exercises V3D_StripedFillTest directly, never the
REM live GraphicsV handler.
REM
REM At 32bpp an ECF pattern can only vary PER ROW (never within a row),
REM cycling every 8 rows - see gfxv_accel.h's top comment. This test uses
REM a genuine 4-colour repeating pattern (not just 2, to exercise more
REM than a simple alternation) placed at colours[0..3], with colours[4..7]
REM set to repeat the same 4 values - so with row_phase=0, row r's expected
REM colour is pattern[r MOD 4].
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself, and make
REM sure V3D_PowerOn has been called at least once this boot (this script
REM calls it itself, idempotent, same as every other test script here).
REM
REM PROCcheck is defined AFTER the final END, not inline - see FillTest.bas
REM for why (BBC BASIC does not auto-skip DEF PROC bodies).

width% = 200
height% = 67
REM alpha=FF, four distinct arbitrary B/G/R patterns
pattern0% = &FFE01010
pattern1% = &FF10E010
pattern2% = &FF1010E0
pattern3% = &FFB0B010

DIM colours% 31
!(colours%+0)  = pattern0%
!(colours%+4)  = pattern1%
!(colours%+8)  = pattern2%
!(colours%+12) = pattern3%
!(colours%+16) = pattern0%
!(colours%+20) = pattern1%
!(colours%+24) = pattern2%
!(colours%+28) = pattern3%

row_phase% = 0

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK (clock rate="; rate%; " Hz)"

PRINT "Requesting "; width%; "x"; height%; " striped fill, row_phase="; row_phase%; "..."
SYS "V3D_StripedFillTest", width%, height%, row_phase%, colours% TO phys%, stride%, log%

PRINT "OK: phys="; ~phys%; " stride="; stride%; " log="; ~log%
PRINT

fail% = FALSE

REM Sample a column of rows spanning more than one 8-row cycle (rows 0
REM through 20, covering two full cycles plus a partial third), plus a
REM couple of horizontal points on one row to confirm a single row really
REM is one uniform colour (no unwanted horizontal variation).
FOR row% = 0 TO 20
  PROCcheck(width% DIV 2, row%, "row " + STR$(row%))
NEXT row%

PROCcheck(0, 5, "row5 left      ")
PROCcheck(width%-1, 5, "row5 right     ")
PROCcheck(width% DIV 2, height%-1, "last row       ")

PRINT
IF fail% THEN PRINT "FAIL - one or more sample points did not match." : END
PRINT "All sample points matched - striped fill correct."
END

DEF PROCcheck(px%, py%, label$)
  addr% = log% + py% * stride% + px% * 4
  val% = !addr%
  expected_idx% = (row_phase% + py%) MOD 8
  expected% = !(colours% + expected_idx% * 4)
  PRINT label$; " ("; px%; ","; py%; "): "; ~val%;
  IF val% = expected% THEN PRINT " OK" ELSE PRINT " MISMATCH (wanted "; ~expected%; ")" : fail% = TRUE
ENDPROC
