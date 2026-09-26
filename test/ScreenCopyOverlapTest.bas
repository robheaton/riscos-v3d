REM > ScreenCopyOverlapTest
REM Real-framestore counterpart to OverlapCopyTest.bas - same escalation
REM discipline as every other real-screen feature in this driver
REM (synthetic buffer first, then the real framestore, before trusting
REM it live). Writes DIRECTLY into the real, live screen framestore -
REM same caution as ScreenFillTest.bas/ScreenCopyTest.bas.
REM
REM Deliberately overlapping and sub-tile-shifted, mimicking a real
REM window scroll: source and destination are the SAME rectangle
REM shifted by (30,20) pixels - neither a multiple of 64.
REM
REM What to expect: whatever is on screen at the source rectangle
REM should appear shifted by (30,20) pixels at the destination,
REM correctly, without visible corruption in the overlap region.
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.

src_x% = 300
src_y% = 300
shift_x% = 30
shift_y% = 20
width% = 200
height% = 150

dest_x% = src_x% + shift_x%
dest_y% = src_y% + shift_y%

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK (clock rate="; rate%; " Hz)"

PRINT "Overlapping copy: "; width%; "x"; height%; " from ("; src_x%; ",";
PRINT src_y%; ") to ("; dest_x%; ","; dest_y%; ") - shift ("; shift_x%;
PRINT ","; shift_y%; ")"
PRINT "(this writes directly to the real screen - watch for the source"
PRINT "area's content to appear shifted at the destination, correctly)"

SYS "V3D_ScreenCopyOverlapTest", src_x%, src_y%, dest_x%, dest_y%, width%, height% TO tiles_x%, tiles_y%

PRINT "OK - GPU reported completion. Tile grid used: "; tiles_x%; "x"; tiles_y%
PRINT "Did the source area's content appear correctly shifted at the"
PRINT "destination, with no visible corruption in the overlap area?"
END
