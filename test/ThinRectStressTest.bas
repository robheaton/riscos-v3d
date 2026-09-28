REM > ThinRectStressTest
REM Regression/stress test: hammers V3D_ScreenFillTest/V3D_ScreenCopyTest
REM with degenerate 1-pixel-wide/tall and tightly-adjacent rectangles,
REM 10,000 real GPU submissions total. Originated from the Task Manager
REM freeze investigation (see README.md's "Task Manager freeze" section)
REM - Task Manager's own Dynamic Areas list draws many extremely thin
REM bars (a 1-pixel-wide "|" tick mark for near-zero values) that no
REM other test in this project ever exercised. Kept as a permanent
REM regression test for thin/degenerate shapes and sustained high call
REM volume, now that the actual freeze has been root-caused and fixed
REM elsewhere (a pmp_alloc/Service_MemoryMoved reentrancy issue, unrelated
REM to rectangle shape).
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK (clock rate="; rate%; " Hz)"
PRINT

iterations% = 2000
colour% = &FFB0E010

PRINT "Phase 1: "; iterations%; " x 1-pixel-WIDE tall fills..."
FOR i% = 1 TO iterations%
  x% = 50 + (i% MOD 100)
  SYS "V3D_ScreenFillTest", x%, 50, 1, 200, colour%
  IF (i% MOD 200) = 0 THEN PRINT "  "; i%; " / "; iterations%
NEXT i%
PRINT "Phase 1 complete - no hang."
PRINT

PRINT "Phase 2: "; iterations%; " x 1-pixel-TALL wide fills..."
FOR i% = 1 TO iterations%
  y% = 50 + (i% MOD 100)
  SYS "V3D_ScreenFillTest", 300, y%, 200, 1, colour%
  IF (i% MOD 200) = 0 THEN PRINT "  "; i%; " / "; iterations%
NEXT i%
PRINT "Phase 2 complete - no hang."
PRINT

PRINT "Phase 3: "; iterations%; " x 1-pixel-WIDE column copies..."
FOR i% = 1 TO iterations%
  y% = 50 + (i% MOD 100)
  SYS "V3D_ScreenCopyTest", 50, y%, 700, y%, 1, 200
  IF (i% MOD 200) = 0 THEN PRINT "  "; i%; " / "; iterations%
NEXT i%
PRINT "Phase 3 complete - no hang."
PRINT

PRINT "Phase 4: "; iterations%; " x 1-pixel-TALL row copies..."
FOR i% = 1 TO iterations%
  x% = 50 + (i% MOD 100)
  SYS "V3D_ScreenCopyTest", x%, 300, x%, 600, 200, 1
  IF (i% MOD 200) = 0 THEN PRINT "  "; i%; " / "; iterations%
NEXT i%
PRINT "Phase 4 complete - no hang."
PRINT

PRINT "Phase 5: tightly-packed ADJACENT thin fills (edge/overlap"
PRINT "boundary stress - Task Manager stacks many bars right next to"
PRINT "each other) - "; iterations%; " iterations..."
FOR i% = 1 TO iterations%
  y% = 50 + ((i% MOD 40) * 2)
  SYS "V3D_ScreenFillTest", 900, y%, 150, 1, colour%
  SYS "V3D_ScreenFillTest", 900, y% + 1, 150, 1, colour% EOR &00FFFFFF
  IF (i% MOD 200) = 0 THEN PRINT "  "; i%; " / "; iterations%
NEXT i%
PRINT "Phase 5 complete - no hang."
PRINT

PRINT "All phases completed with no hang or error."
PRINT "Thin/degenerate rectangle shapes do NOT reproduce the freeze"
PRINT "on their own, in isolation from Task Manager."
END
