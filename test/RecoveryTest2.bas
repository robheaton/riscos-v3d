REM > RecoveryTest2
REM Part 2 of 2 - run straight after RecoveryTest1.bas, in the same
REM session (don't *RMKill or reload the module in between).
REM
REM This is the actual test. Before the CTRUN-wedge recovery fix, once
REM a V3D_ClearToSprite call timed out, every later call - including
REM this one - failed identically for the rest of the session. If the
REM fix works, this should succeed regardless of what RecoveryTest1 did.

PRINT "Step 3: V3D_ClearTest &DEADBEEF"
SYS "V3D_ClearTest", &DEADBEEF TO phys3%, readback3%
PRINT "  ClearTest returned OK: phys="; ~phys3%; " readback="; ~readback3%
IF readback3% = &DEADBEEF THEN PRINT "  MATCH - recovery worked" ELSE PRINT "  MISMATCH - readback wasn't DEADBEEF"
END
