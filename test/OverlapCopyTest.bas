REM > OverlapCopyTest
REM Tests the new overlap-safe copy path (see README.md's "Overlapping
REM copy" section) against a synthetic buffer - genuinely new territory
REM (a scratch-buffer two-pass copy), so tested in isolation before
REM ever trusting it against the real screen or the live desktop.
REM
REM Deliberately uses a SUB-TILE shift (20,15 - neither a multiple of
REM 64) between source (at 0,0) and destination (at shift_x,shift_y)
REM within ONE buffer - exactly mirrors a real scroll, and exactly the
REM case a naive direct copy gets wrong (see cl_copy.h for why).
REM
REM The buffer is filled with a per-pixel pattern before the copy runs,
REM so the EXPECTED value at any destination pixel (shift_x+x,shift_y+y)
REM is computable directly from the ORIGINAL source pixel's own formula
REM at (x,y) - not by re-reading the source region after the copy,
REM since part of it may itself have been overwritten by the
REM destination write where the two rectangles overlap.
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.

width% = 150
height% = 100
shift_x% = 20
shift_y% = 15

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK (clock rate="; rate%; " Hz)"

PRINT "Requesting "; width%; "x"; height%; " overlapping copy, shift (";
PRINT shift_x%; ","; shift_y%; ")..."
SYS "V3D_OverlapCopyTest", width%, height%, shift_x%, shift_y% TO buf%, stride%, tiles_x%, tiles_y%

PRINT "OK: buf="; ~buf%; " stride="; stride%; " tiles="; tiles_x%; "x"; tiles_y%
PRINT

fail% = FALSE

PROCcheck(0, 0, "Dest top-left        ")
PROCcheck(width%-1, 0, "Dest top-right       ")
PROCcheck(0, height%-1, "Dest bottom-left     ")
PROCcheck(width%-1, height%-1, "Dest bottom-right    ")
PROCcheck(width% DIV 2, height% DIV 2, "Dest centre          ")
REM points specifically inside the overlap region between source and
REM dest rectangles - the highest-risk area for this whole mechanism
PROCcheck(10, 10, "Overlap region pt 1  ")
PROCcheck(50, 40, "Overlap region pt 2  ")
PROCcheck(width%-shift_x%-1, height%-shift_y%-1, "Overlap region corner")

PRINT
IF fail% THEN PRINT "FAIL - one or more sample points did not match." : END
PRINT "All sample points matched - overlap-safe copy correct, shift ("; shift_x%; ","; shift_y%; ")."
END

DEF PROCcheck(x%, y%, label$)
  dest_addr% = buf% + (y%+shift_y%) * stride% + (x%+shift_x%) * 4
  actual% = !dest_addr%
  expected% = &FF000000 OR (x% << 12) OR y%
  PRINT label$; " src("; x%; ","; y%; ") -> dest("; x%+shift_x%; ","; y%+shift_y%; "): ";
  PRINT "actual="; ~actual%; " expected="; ~expected%;
  IF actual% = expected% THEN PRINT " OK" ELSE PRINT " MISMATCH" : fail% = TRUE
ENDPROC
