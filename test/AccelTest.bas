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
REM To also exercise the newer dithered-fill path (V3D_AccelStripedStats
REM below - see README's "Dithered/patterned fills" section), try to
REM trigger some GREYED-OUT/disabled UI during this interaction period
REM too - e.g. open a dialogue box and look at a disabled button/writable
REM field, or open a menu with a greyed-out entry, or drag a "shaded"
REM (write-protected/locked) Filer icon - RISC OS conventionally dithers
REM these rather than using a flat grey, and that's exactly the ECF
REM pattern shape this new path targets. A zero count there is still a
REM perfectly valid result if nothing dithered happened to get redrawn.
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
PRINT "  of which a solid ECF, above the size limit: "; solids%
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

REM V3D_AccelStripedStats: R0=striped (non-solid) fill calls decoded,
REM R1=actually accelerated, R2=attempted but failed, R3=last failure's
REM error number - see README's "Dithered/patterned fills" section.
REM Real desktop dithered fills are rarer than solid ones (e.g. some
REM greyed-out icon backgrounds) - a small/zero count here is plausible
REM even after real interaction, not necessarily a sign of a problem.
SYS "V3D_AccelStripedStats" TO striped%, striped_claimed%, striped_errors%, striped_errnum%

PRINT "GraphicsV striped (dithered) fill calls seen: "; striped%
PRINT "  actually accelerated by the GPU:            "; striped_claimed%
PRINT "  attempted but failed:                        "; striped_errors%
IF striped_errors% > 0 THEN PRINT "  last failure error number:                   "; striped_errnum%
PRINT

REM V3D_AccelReentrantCount: non-zero means a second GraphicsV Render
REM call landed while a previous one was still being processed, and
REM was safely skipped rather than risking corruption of shared
REM scratch state (see gfxv_accel.c's reentrancy guard). Should always
REM read 0 in normal use.
SYS "V3D_AccelReentrantCount" TO reentrant%
PRINT "GraphicsV calls skipped as reentrant (diagnostic): "; reentrant%
PRINT

REM V3D_AccelLimits (all -1 = read only): the size limits below which a
REM fill or copy is deliberately left to the CPU, because a GPU round trip
REM costs ~119us whatever the size (see gfxv_accel.c's SIZE LIMITS note
REM and test/FillCopyBench.bas). Such calls are NOT failures - they show
REM up here instead of in the "accelerated" or "failed" counts above.
SYS "V3D_AccelLimits", -1, -1, -1, -1, -1 TO lim_fb%, lim_fr%, lim_cb%, lim_cr%, lim_ob%, skip_fills%, skip_copies%
PRINT "Size limits - fill: "; lim_fb%; " bytes, "; lim_fr%; " rows; copy: "; lim_cb%; " bytes, "; lim_cr%; " rows; overlap copy: "; lim_ob%; " bytes"
PRINT "Left to the CPU as too small to benefit: "; skip_fills%; " fills, "; skip_copies%; " copies"
PRINT

REM V3D_AccelTimingStats: R0=worst single accelerated call's duration
REM ever seen (centiseconds), R1=highest accelerated-call rate seen in
REM any 1-second window all session.
SYS "V3D_AccelTimingStats" TO max_dur_cs%, peak_rate%, max_dur_us%, fine%
IF fine% = 1 THEN
  PRINT "Worst single accelerated call duration: "; max_dur_us%; " us"
ELSE
  PRINT "Worst single accelerated call duration: "; max_dur_cs%*10; " ms (10ms timer - the 1MHz timer was not available)"
ENDIF
PRINT "Peak accelerated calls in any 1-second window: "; peak_rate%
PRINT

REM V3D_AccelProfile (new in 0.07): the GPU path's health, and where the time
REM of an accelerated job goes. View 2 = health; views 0/1 = per-job phase
REM sums for fills / copies; view 6 = why the driver last declined the screen.
SYS "V3D_AccelProfile", 2 TO wedged%, wcount%, fine%, vrv%, vrp%, pbase%, fbase%, rescued%
PRINT "GPU fail-stop latch: "; wedged%; "   job timeouts so far: "; wcount%; "   lost interrupts rescued: "; rescued%
IF wedged% = 1 THEN PROCwedged
IF vrv% = 1 THEN PRINT "Write-bank address seen from GraphicsV: &"; STR$~(vrp%); "  (rendering base &"; STR$~(pbase%); ", framestore base &"; STR$~(fbase%); ")" ELSE PRINT "No write-bank address seen yet (rendering base &"; STR$~(pbase%); ")"
PROCjobtimes(0, "Fill")
PROCjobtimes(1, "Copy")
SYS "V3D_AccelProfile", 7 TO resc%, iclaim%, ispur%, ibin%, irend%, jobsrun%
PRINT "Interrupts: "; ibin%; " bin and "; irend%; " render seen for "; jobsrun%; " jobs started"
PRINT "  not accounted for: "; jobsrun% - ibin%; " bin, "; jobsrun% - irend%; " render (0-1 is normal; more = interrupts really lost); empty interrupts: "; ispur%
IF resc% > 0 THEN PROCrescues(resc%)
SYS "V3D_AccelProfile", 6 TO serr%, smsg%, sbpp%, sll%, sxl%, syl%, stot%, sfs%
IF serr% <> 0 THEN
  PRINT "*** The driver is DECLINING this screen (error "; serr%; "): "; $smsg%
  PRINT "    log2bpp "; sbpp%; ", line length "; sll%; ", XWindLimit "; sxl%; ", YWindLimit "; syl%; ", TotalScreenSize "; stot%; ", framestore "; sfs%
ENDIF
PRINT

SYS "V3D_AccelStop"
PRINT "Released (V3D_AccelStop) - back to normal software rendering."
PRINT
IF calls% = 0 THEN PRINT "No GraphicsV traffic seen at all - try more" : PRINT "desktop interaction, or this may indicate something" : PRINT "isn't reaching this vector." : END

IF claimed% > 0 OR copy_claimed% > 0 OR copy_overlap_claimed% > 0 OR striped_claimed% > 0 THEN PRINT "At least one real accelerated fill or copy happened - check the desktop looked correct throughout." : END

PRINT "GraphicsV traffic was seen, but nothing was accelerated."
PRINT "Could be genuinely correct (no plain solid fills or"
PRINT "non-overlapping copies happened to occur), or could mean"
PRINT "the decoding needs a closer look."
END

REM The details the driver recorded for the first few lost-interrupt rescues (see
REM V3D_AccelProfile view 8): was the interrupt status bit still set (raised but never
REM delivered), did it arrive late, and how many earlier interrupts were missing?
DEF PROCrescues(n%)
  LOCAL i%, a%, b%, c%, d%, e%, f%, g%, h%
  IF n% > 4 THEN n% = 4
  FOR i% = 0 TO n% - 1
    SYS "V3D_AccelProfile", 8, i% TO a%, b%, c%, d%, e%, f%, g%, h%
    PRINT "  rescue "; i% + 1; ": waiting for the ";
    IF ((a% >> 8) AND 255) = 1 THEN PRINT "bin"; ELSE PRINT "render";
    PRINT " job of a ";
    IF ((a% >> 16) AND 255) = 0 THEN PRINT "fill"; ELSE PRINT "copy";
    PRINT ", waited "; b%; " us"
    PRINT "     INT_STS=&"; STR$~(c%); " INT_MSK=&"; STR$~(d%); " hub=&"; STR$~(e%); " thread CS=&"; STR$~(f%); " frames advanced "; g% AND &FFFF; ", interrupt then arrived late: ";
    IF ((g% >> 16) AND 1) = 1 THEN PRINT "YES" ELSE PRINT "no"
    PRINT "     jobs-minus-interrupts at that moment: bin "; h% AND &FFFF; ", render "; (h% >> 16) AND &FFFF
  NEXT
ENDPROC

DEF PROCjobtimes(view%, name$)
  LOCAL jn%, js%, jb%, jm%, jr%, jt%, jx%
  SYS "V3D_AccelProfile", view%, 0 TO jn%, js%, jb%, jm%, jr%, jt%, jx%
  IF jn% = 0 THEN
    PRINT name$; " jobs timed: 0"
  ELSE
    PRINT name$; " jobs timed: "; jn%; "  average us per job - setup "; js% DIV jn%; ", bin "; jb% DIV jn%; ", mid "; jm% DIV jn%; ", render "; jr% DIV jn%; ", total "; jt% DIV jn%; " (worst "; jx%; ")"
  ENDIF
ENDPROC

DEF PROCwedged
  LOCAL a%, b%, c%, d%, e%, f%, g%, h%
  PRINT "*** A GPU JOB TIMED OUT and acceleration has been switched off. Registers:"
  SYS "V3D_AccelProfile", 3 TO a%, b%, c%, d%, e%, f%, g%, h%
  PRINT "  which|kind|valid=&"; STR$~(a%); " INT_STS=&"; STR$~(b%); " INT_MSK=&"; STR$~(c%)
  PRINT "  CT0CS=&"; STR$~(d%); " CT0CA=&"; STR$~(e%); " CT0EA=&"; STR$~(f%); " CT1CS=&"; STR$~(g%); " CT1CA=&"; STR$~(h%)
  SYS "V3D_AccelProfile", 4 TO a%, b%, c%, d%, e%, f%, g%, h%
  PRINT "  CT1EA=&"; STR$~(a%); " BFC=&"; STR$~(b%); " RFC=&"; STR$~(c%); " BFC0=&"; STR$~(d%); " RFC0=&"; STR$~(e%)
  PRINT "  IRQs: bin "; f%; " render "; g%; " hub INT_STS=&"; STR$~(h%)
ENDPROC

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
