REM > ScreenStripedFillTest
REM Real-framestore counterpart of ScreenFillTest.bas, for the new
REM dithered-fill mechanism (see README.md's "Dithered/patterned fills"
REM section) - StripedFillTest.bas already proved the mechanism against
REM a synthetic buffer (all 24 sample points matched, real hardware,
REM first attempt); this writes DIRECTLY into the real, live screen
REM framestore instead.
REM
REM Same visible-effect caveat as ScreenFillTest.bas: the Wimp may
REM immediately redraw over the rectangle (expected, not a bug - this
REM driver isn't synchronised with the Wimp's own redraw cycle). A
REM rectangle with visible horizontal stripes, in roughly the right
REM place and size, cycling through the 4 colours below every 4 rows,
REM is success for this test.
REM
REM Deliberately a small, conservative rectangle (150x100, matching
REM ScreenFillTest.bas's own size) placed away from screen edges, same
REM reasoning as that script (small blast radius if something is
REM subtly wrong).
REM
REM row_phase% is passed as 0 here - this test SWI, unlike the live
REM accelerator's own v3d_accel_render_striped_fill, does not screen-
REM anchor the pattern itself (see module.c's V3D_ScreenStripedFillTest
REM case comment) - there's nothing for a one-shot manual test to stay
REM anchored against. That just means the rectangle's own top row is
REM colours[0], counting down the screen from there.
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.
REM V3D_PowerOn is called below - idempotent, same as other tests.

x% = 400
y% = 300
width% = 150
height% = 100
row_phase% = 0

REM alpha=FF, four distinct, easily-distinguishable colours
pattern0% = &FFE01010
pattern1% = &FF10E010
pattern2% = &FF1010E0
pattern3% = &FFE0E010

DIM colours% 31
!(colours%+0)  = pattern0%
!(colours%+4)  = pattern1%
!(colours%+8)  = pattern2%
!(colours%+12) = pattern3%
!(colours%+16) = pattern0%
!(colours%+20) = pattern1%
!(colours%+24) = pattern2%
!(colours%+28) = pattern3%

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK (clock rate="; rate%; " Hz)"

PRINT "Striped-filling screen rectangle at ("; x%; ","; y%; ") size "; width%; "x"; height%; "..."
PRINT "(this writes directly to the real screen - watch for horizontal stripes)"

SYS "V3D_ScreenStripedFillTest", x%, y%, width%, height%, row_phase%, colours%

PRINT "OK - GPU reported completion."
PRINT "Did a rectangle with 4 repeating horizontal colour stripes appear on screen?"
PRINT "If so, the dithered-fill mechanism works against the real framestore."
END
