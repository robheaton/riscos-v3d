REM > RecoveryTest1
REM Part 1 of 2 (see RecoveryTest2.bas). Run this first, then run
REM RecoveryTest2 straight after IN THE SAME SESSION (don't *RMKill or
REM reload the module in between) - the whole point is testing whether
REM a second call recovers after this one, whatever this one does.
REM
REM No error handling here on purpose: if V3D_ClearToSprite times out,
REM BASIC's own default error report is exactly what we want to see -
REM just copy back whatever it prints.

PRINT "Step 1: V3D_PowerOn"
SYS "V3D_PowerOn"
PRINT "  ok"
PRINT

PRINT "Step 2: V3D_ClearToSprite &FF00FF00"
SYS "V3D_ClearToSprite", &FF00FF00 TO area%, readback2%, phys2%
PRINT "  ClearToSprite returned OK: area="; ~area%; " readback="; ~readback2%; " phys="; ~phys2%
PRINT
PRINT "Now run RecoveryTest2.bas, without reloading the module."
END
