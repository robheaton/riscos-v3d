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

IF errors% > 0 THEN PROCfailinfo

REM V3D_AccelCopyStats: R0=total op-1 calls, R1=overlapping calls seen
REM (now ATTEMPTED via the scratch-buffer path, not just rejected),
REM R2=non-overlapping accelerated, R3=non-overlapping failed,
REM R4=overlapping accelerated (scratch-buffer path), R5=overlapping
REM failed/too-large, R6=last failure's error number (either path).
SYS "V3D_AccelCopyStats" TO copy_ops%, copy_overlap%, copy_claimed%, copy_errors%, copy_overlap_claimed%, copy_overlap_errors%, copy_errnum%

PRINT "GraphicsV op 1 (copy) calls seen:      "; copy_ops%
PRINT "Direct (non-overlapping) path:"
PRINT "  actually accelerated by the GPU:    "; copy_claimed%
PRINT "  attempted but failed:               "; copy_errors%
PRINT "Overlap (scratch-buffer) path:"
PRINT "  overlapping calls seen:             "; copy_overlap%
PRINT "  actually accelerated by the GPU:    "; copy_overlap_claimed%
PRINT "  attempted but failed/too large:     "; copy_overlap_errors%
IF copy_errors% > 0 OR copy_overlap_errors% > 0 THEN PROCcopyerrorinfo
PRINT

SYS "V3D_AccelStop"
PRINT "Released (V3D_AccelStop) - back to normal software rendering."
PRINT
IF calls% = 0 THEN PRINT "No GraphicsV traffic seen at all - try more" : PRINT "desktop interaction, or this may indicate something" : PRINT "isn't reaching this vector." : END

IF claimed% > 0 OR copy_claimed% > 0 OR copy_overlap_claimed% > 0 THEN PRINT "At least one real accelerated fill or copy happened - check the desktop looked correct throughout." : END

PRINT "GraphicsV traffic was seen, but nothing was accelerated."
PRINT "Could be genuinely correct (no plain solid fills or"
PRINT "non-overlapping copies happened to occur), or could mean"
PRINT "the decoding needs a closer look."
END

DEF PROCfailinfo
  SYS "V3D_AccelFailInfo" TO errnum%, left%, bottom%, right%, top%, failmin%, failmax%, okmin%, okmax%
  PRINT "Failure diagnostics:"
  PRINT "  Last failure error number: "; errnum%
  PRINT "  Last failure rectangle: left="; left%; " bottom="; bottom%; " right="; right%; " top="; top%
  PRINT "  Last failure size: "; (right%-left%+1); "x"; (top%-bottom%+1)
  IF failmax% > 0 THEN PRINT "  Failed fill area range: "; failmin%; " to "; failmax%; " sq px" ELSE PRINT "  No failed fill area recorded"
  IF okmax% > 0 THEN PRINT "  Succeeded fill area range: "; okmin%; " to "; okmax%; " sq px" ELSE PRINT "  No succeeded fill area recorded"
  PRINT
ENDPROC

DEF PROCcopyerrorinfo
  SYS "V3D_AccelCopyLastError" TO msgaddr%
  PRINT "  Last copy failure error number: "; copy_errnum%
  PRINT "  Last copy failure message: "; $msgaddr%
ENDPROC
