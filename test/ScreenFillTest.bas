REM > ScreenFillTest
REM Step 3 of the planned GraphicsV Render accelerator (see README.md):
REM V3D_ScreenFillTest writes DIRECTLY into the real, live screen
REM framestore - unlike every previous test in this project, this has
REM a visible effect on your actual desktop the moment it succeeds.
REM
REM What to expect: a small solid-colour rectangle should appear
REM somewhere on screen. It may get immediately painted over by the
REM next Wimp redraw (e.g. if the mouse moves, a window updates, the
REM clock ticks) - that's expected, not a bug: this driver isn't
REM synchronised with the Wimp's own redraw cycle yet (that's step 4+).
REM A rectangle that appears at all, in roughly the right place and
REM size, is success for this step.
REM
REM We don't yet know whether row 0 (offset 0 from ScreenStart, this
REM script's y=0) corresponds to the TOP or BOTTOM of the visible
REM display, or how it relates to GraphicsV's own bottom-left-origin
REM coordinate convention - reconciling that is deliberately deferred
REM to step 4 (see cl_fill.h). Wherever it appears is fine for this test.
REM
REM Deliberately a small, conservative rectangle (150x100) placed away
REM from screen edges for a first real-framestore test, not the full
REM screen - if something is subtly wrong (e.g. the framestore
REM contiguity check below is somehow wrong on this specific setup),
REM the blast radius of a mistake should be small and recoverable
REM (worst case: a garbled desktop area until the next full redraw or
REM a *right-click-and-redraw / *WindowManager refresh; a reboot always
REM recovers regardless, same as any other display glitch).
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.
REM V3D_PowerOn is called below - idempotent, same as other tests.

x% = 400
y% = 300
width% = 150
height% = 100
REM alpha=FF, arbitrary distinctive colour - easy to spot against a
REM typical desktop background/icon colours
colour% = &FFE010B0

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK (clock rate="; rate%; " Hz)"

PRINT "Filling screen rectangle at ("; x%; ","; y%; ") size "; width%; "x"; height%; ", colour "; ~colour%; "..."
PRINT "(this writes directly to the real screen - watch for a rectangle to appear)"

SYS "V3D_ScreenFillTest", x%, y%, width%, height%, colour% TO tiles_x%, tiles_y%

PRINT "OK - GPU reported completion. Tile grid used: "; tiles_x%; "x"; tiles_y%
PRINT "Did a "; colour% AND &FFFFFF; "-ish coloured rectangle appear on screen? If so, step 3 works."
END
