REM > StressTest
REM Repeatedly exercises V3D_PowerOn + V3D_ClearTest + V3D_ClearToSprite
REM in one session, without reloading the module or rebooting between
REM iterations, to check whether the firmware clock-gate fix (
REM V3D_PowerOn now enables the clock before every PM_GRAFX reset
REM pulse) makes a real hardware reset - and so full recovery - happen
REM on every call, not just the first one after a reboot.
REM
REM Stops immediately on the first failure (wrong readback, or any
REM SWI error/timeout) with full detail. A clean run to completion is
REM the pass condition; if it ever fails partway through, the printed
REM iteration number and colour are enough to reproduce it directly
REM (SYS "V3D_ClearTest"/"V3D_ClearToSprite" with that same value).
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.

i% = 0
ON ERROR PRINT "FAIL at iteration "; i%; ": "; REPORT$; " (error "; ERR; ")" : END

N% = 50

FOR i% = 1 TO N%
  SYS "V3D_PowerOn" TO ok%, rate%
  IF ok% <> 1 THEN PRINT "FAIL at iteration "; i%; ": V3D_PowerOn returned ok="; ok% : END

  REM vary B/G/R each iteration (i% repeated in every byte, i%<=50 so
  REM no cross-byte carry), force alpha=FF so type-6 TBGR stays opaque
  colour% = (i% * &01010101) OR &FF000000

  SYS "V3D_ClearTest", colour% TO phys%, readback%
  IF readback% <> colour% THEN PRINT "FAIL at iteration "; i%; " (ClearTest): wanted "; ~colour%; " got "; ~readback% : END

  SYS "V3D_ClearToSprite", colour% TO sprite%, readback2%, physaddr%
  IF readback2% <> colour% THEN PRINT "FAIL at iteration "; i%; " (ClearToSprite): wanted "; ~colour%; " got "; ~readback2% : END

  IF (i% MOD 5) = 0 THEN PRINT "iteration "; i%; " OK (clock rate="; rate%; " Hz)"
NEXT i%

PRINT
PRINT N%; " iterations completed with no failures."
PRINT "Confirms V3D_PowerOn's reset+clock-enable recovers cleanly call"
PRINT "after call in one session, with no reboot needed between tests."
END
