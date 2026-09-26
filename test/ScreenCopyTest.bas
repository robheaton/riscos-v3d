REM > ScreenCopyTest
REM Second step of op 1 (copy, see README.md): V3D_ScreenCopyTest
REM copies a rectangle of the REAL screen to another REAL screen
REM location, non-overlapping only (it refuses with a clear error if
REM the two rectangles overlap at all - see cl_copy.h for why overlap
REM isn't handled yet). Same live-desktop caution as ScreenFillTest.bas.
REM
REM What to expect: whatever is currently on screen at the source
REM rectangle (desktop background, icons, whatever's there) should
REM appear duplicated at the destination rectangle. Like ScreenFillTest,
REM this may get painted over by the next Wimp redraw - that's expected.
REM
REM Deliberately small (150x100) and non-overlapping (source and dest
REM are far apart) for a first real-framestore copy test.
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.

src_x% = 50
src_y% = 50
dest_x% = 500
dest_y% = 400
width% = 150
height% = 100

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK (clock rate="; rate%; " Hz)"

PRINT "Copying "; width%; "x"; height%; " from ("; src_x%; ","; src_y%;
PRINT ") to ("; dest_x%; ","; dest_y%; ")..."
PRINT "(this writes directly to the real screen - watch for the source"
PRINT "area's content to appear duplicated at the destination)"

SYS "V3D_ScreenCopyTest", src_x%, src_y%, dest_x%, dest_y%, width%, height% TO tiles_x%, tiles_y%

PRINT "OK - GPU reported completion. Tile grid used: "; tiles_x%; "x"; tiles_y%
PRINT "Did the source area's content appear duplicated at the"
PRINT "destination? If so, real-framestore copy works."
END
