REM > AccelTest
REM Step 4 of the planned GraphicsV Render accelerator (see README.md):
REM claims GraphicsV for real (V3D_AccelStart) and reports what actually
REM happened via V3D_AccelStats - real instrumentation, not guesswork,
REM since this handler now sits in the LIVE Wimp/VDU graphics pipeline
REM for as long as it's claimed. This is the highest-stakes test in the
REM whole project so far: while claimed, EVERY GraphicsV call on the
REM system passes through this driver's handler, not just calls this
REM script makes itself.
REM
REM What this script does NOT do: it does not itself try to synthesise
REM a fake GCOL fill call. A BASIC program running in a Task Window
REM plots into the window's own backing store, not necessarily via the
REM same direct-to-screen GraphicsV path a real Wimp redraw uses - so a
REM self-generated test here could easily prove nothing either way.
REM Instead: claim the vector, then YOU interact with the real desktop
REM normally for a bit (drag a window, resize one, scroll something,
REM move icons) to generate genuine Wimp redraw traffic, then come back
REM here and press a key to see the stats and release the vector again.
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK (clock rate="; rate%; " Hz)"

PRINT
PRINT "About to claim GraphicsV (V3D_AccelStart)."
PRINT "From this point, this driver intercepts EVERY GraphicsV Render"
PRINT "call system-wide until V3D_AccelStop is called."
PRINT

SYS "V3D_AccelStart"
PRINT "Claimed. Now click away from this window and interact with the"
PRINT "desktop for a bit:"
PRINT "  - drag a window around"
PRINT "  - resize a window"
PRINT "  - scroll something"
PRINT "  - open/close a directory display"
PRINT "Then click back into this window and press any key."
PRINT
A$ = GET$

SYS "V3D_AccelStats" TO calls%, renders%, fills%, solids%, claimed%, errors%

PRINT
PRINT "GraphicsV handler invocations:     "; calls%
PRINT "  of which reason 13 (Render):     "; renders%
PRINT "  of which op 2 (fill):            "; fills%
PRINT "  of which a genuine solid ECF:    "; solids%
PRINT "  actually accelerated by the GPU: "; claimed%
PRINT "  attempted but failed:            "; errors%
PRINT

SYS "V3D_AccelStop"
PRINT "Released (V3D_AccelStop) - back to normal software rendering."
PRINT
IF calls% = 0 THEN PRINT "No GraphicsV traffic seen at all - try more desktop interaction, or this may indicate something isn't reaching this vector." : END
IF claimed% > 0 THEN PRINT "At least one real accelerated fill happened - check the desktop looked correct throughout." ELSE PRINT "GraphicsV traffic was seen, but nothing matched (reason 13 + op 2 + solid ECF) - could be genuinely correct (no plain solid fills happened to occur) or could mean the ECF/coordinate decoding needs a closer look."
END
