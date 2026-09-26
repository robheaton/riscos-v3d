REM > RecoveryTest
REM Proves (or disproves) the CTRUN-wedge recovery fix in
REM diagnose_timeout() (see README.md, "attempt 9"). Theory: once a
REM submission timed out with CTRUN still set on the CLE thread, it
REM stayed wedged for the rest of the session, so every later call
REM failed identically regardless of which SWI or which buffer - a
REM previously-100%-reliable V3D_ClearTest included. The fix makes
REM a timeout request a proper stop instead of leaving the thread
REM wedged, so the *next* submission should recover cleanly.
REM
REM Before running: *RMLoad the freshly built V3DDriver,ffa yourself.
REM This program does not load it for you.
REM
REM What to expect:
REM   Step 2 (ClearToSprite) may well time out - that's fine, it's the
REM   call that used to wedge the thread. Whatever it prints, the
REM   script carries on to step 3 regardless.
REM   Step 3 (ClearTest) is the actual test. Before this fix, once
REM   step 2 had wedged the thread, this failed identically every time
REM   for the rest of the session. If the fix works, it should now
REM   print "ClearTest returned OK" and "MATCH - recovery worked".
REM
REM Please copy back exactly what steps 2 and 3 print.

step% = 0
ON ERROR PRINT "  Error "; ERR; ": "; REPORT$; " (step "; step%; ")" : ON ERROR OFF : IF step% = 2 THEN GOTO recover ELSE GOTO finished

PRINT "Step 1: V3D_PowerOn"
step% = 1
SYS "V3D_PowerOn"
PRINT "  ok"
PRINT

PRINT "Step 2: V3D_ClearToSprite &FF00FF00 (may time out - that's fine)"
step% = 2
SYS "V3D_ClearToSprite", &FF00FF00 TO area%, readback2%, phys2%
PRINT "  ClearToSprite returned OK (no timeout this time): area="; ~area%; " readback="; ~readback2%; " phys="; ~phys2%

.recover
ON ERROR PRINT "  Error "; ERR; ": "; REPORT$; " (step "; step%; ")" : ON ERROR OFF : GOTO finished
PRINT
PRINT "Step 3: V3D_ClearTest &DEADBEEF (the real test - should succeed even if step 2 timed out)"
step% = 3
SYS "V3D_ClearTest", &DEADBEEF TO phys3%, readback3%
PRINT "  ClearTest returned OK: phys="; ~phys3%; " readback="; ~readback3%
IF readback3% = &DEADBEEF THEN PRINT "  MATCH - recovery worked" ELSE PRINT "  MISMATCH - readback wasn't DEADBEEF"

.finished
PRINT
PRINT "Done."
END
