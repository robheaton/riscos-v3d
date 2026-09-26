REM > AutoStartTest
REM Verifies the new auto-start behaviour (see README.md's "Auto-start
REM at boot" section): that GraphicsV is claimed automatically in
REM module_init, WITHOUT this script ever calling V3D_AccelStart
REM itself. Deliberately does NOT call V3D_AccelStart or V3D_PowerOn -
REM the whole point is to prove the module is already accelerating
REM before this script does anything at all.
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself with a
REM PLAIN *RMLoad (no "NoAutoStart" on the command tail) - that's the
REM condition this test is checking.

PRINT "This test does NOT call V3D_AccelStart or V3D_PowerOn."
PRINT "If auto-start is working, GraphicsV traffic should already"
PRINT "have been intercepted since the module loaded."
PRINT
PRINT "Click away from this window and interact with the desktop for"
PRINT "a bit (drag/resize a window, scroll something), then click back"
PRINT "and press any key."
PRINT
A$ = GET$

SYS "V3D_AccelStats" TO calls%, renders%, fills%, solids%, claimed%, errors%
SYS "V3D_AccelCopyStats" TO copy_ops%, copy_overlap%, copy_claimed%, copy_errors%, copy_overlap_claimed%, copy_overlap_errors%, copy_errnum%

PRINT
PRINT "GraphicsV handler invocations:     "; calls%
PRINT "  actually accelerated (fill):     "; claimed%
PRINT "  actually accelerated (copy):     "; copy_claimed% + copy_overlap_claimed%
PRINT

IF calls% > 0 THEN PRINT "PASS - GraphicsV traffic was intercepted" : PRINT "without this script ever calling V3D_AccelStart." : PRINT "Auto-start is working." : END

PRINT "INCONCLUSIVE - no GraphicsV traffic seen at all. Either"
PRINT "auto-start isn't working, or genuinely no Render calls"
PRINT "happened during the interaction (try more/different desktop"
PRINT "activity, or confirm the module was loaded WITHOUT"
PRINT "NoAutoStart on its command tail)."
END
