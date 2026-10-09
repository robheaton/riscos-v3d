REM > TraceDump
REM Shows what the V3D driver saw of the graphics system, event by event (V3DDriver 0.11 or later).
REM
REM The driver can keep a short log of the GraphicsV calls it cares about: a mode being set, the
REM address the VDU is told to write to, and every fill and copy with what it did about it (took
REM it, left it to the CPU, could not use the screen and why). It is off until armed, and then keeps
REM the next 32 events.
REM
REM How to use it:
REM   1. Run it once. It arms the log and says so.
REM   2. Do the thing you want to look at - change the screen mode from the desktop, drag a window,
REM      scroll something, run a program.
REM   3. Run it again. It prints what was recorded and arms the log again for the next time.
REM
REM It only reads and arms; it never changes how the driver works. Double-click it in the Filer.

ON ERROR PROCfail

PROCmodinfo
PRINT "TraceDump - module: "; help$
IF INSTR(help$, "0.11") = 0 AND INSTR(help$, "0.12") = 0 THEN
  PRINT "This needs V3DDriver 0.11 or later."
  END
ENDIF

SYS "V3D_AccelProfile", 9, 0 TO t0%, t1%, t2%, t3%, t4%, t5%, t6%, t7%
stored% = t7% AND &FFFF
armed% = (t7% >> 16) AND 1

IF stored% = 0 AND armed% = 0 THEN
  SYS "V3D_AccelProfile", 9, -1
  PRINT
  PRINT "The log is now ARMED and empty. Do what you want to look at, then run TraceDump again."
  END
ENDIF

IF stored% = 0 THEN
  PRINT
  PRINT "The log is armed but nothing has been recorded yet."
  END
ENDIF

PRINT
PRINT "Events recorded: "; stored%;
IF armed% = 0 THEN PRINT " (the log is full, later events were not kept)" ELSE PRINT
FOR i% = 0 TO stored% - 1
  SYS "V3D_AccelProfile", 9, i% TO t0%, t1%, t2%, t3%, t4%, t5%, t6%, t7%
  kind% = (t0% >> 8) AND 255
  out% = (t0% >> 16) AND 255
  err% = t6% AND &FFFF
  PRINT "  "; i% + 1; ". ";
  IF kind% = 1 THEN PRINT "SetMode"
  IF kind% = 2 THEN PRINT "SetDMAAddress: DAG "; t2%; " address &"; STR$~(t1%)
  IF kind% = 3 THEN
    PRINT "Fill "; t1%; ","; t2%; " to "; t3%; ","; t4%; " : ";
    PROCoutcome(out%, err%)
  ENDIF
  IF kind% = 4 THEN
    PRINT "Copy from "; t1%; ","; t2%; " to "; t3%; ","; t4%; " : ";
    PROCoutcome(out%, err%)
  ENDIF
  IF kind% < 1 OR kind% > 4 THEN PRINT "event kind "; kind%
NEXT

REM What the driver believes about the screen right now.
SYS "V3D_AccelProfile", 6 TO s0%, s1%, s2%, s3%, s4%, s5%, s6%, s7%
SYS "V3D_AccelProfile", 2 TO h0%, h1%, h2%, h3%, h4%, h5%, h6%, h7%
SYS "V3D_AccelProfile", 7 TO c0%, c1%, c2%, c3%, c4%, c5%, c6%, c7%
PRINT
PRINT "The driver's view of the screen now:"
PRINT "  log2bpp "; s2%; ", line length &"; STR$~(s3%); ", XWindLimit "; s4%; ", YWindLimit "; s5%; ", framestore size &"; STR$~(s7%)
PRINT "  write address reported by the kernel: valid "; h3%; ", &"; STR$~(h4%); "; drawing at &"; STR$~(h5%); "; framestore base &"; STR$~(h6%)
IF s0% <> 0 THEN
  PRINT "  *** it is DECLINING this screen, error "; s0%; ": "; $s1%
ELSE
  PRINT "  it is accepting this screen (no decline remembered)"
ENDIF
PRINT "  screens declined for a fill or copy since the module was loaded: "; c6%; ", last error number "; c7%

SYS "V3D_AccelProfile", 9, -1
PRINT
PRINT "(The log has been armed again.)"
END

DEF PROCfail
  ON ERROR OFF
  PRINT
  REPORT
  PRINT " at line "; ERL
  PRINT "If this says the module was not found, V3DDriver is not loaded."
END
ENDPROC

DEF PROCmodinfo
  LOCAL base%, p%, k%, c%
  SYS "OS_Module", 18, "V3DDriver" TO ,,, base%
  p% = base% + base%!&14
  help$ = ""
  k% = 0
  REPEAT
    c% = ?(p% + k%)
    IF c% = 9 THEN c% = 32
    IF c% >= 32 THEN help$ += CHR$(c%)
    k% += 1
  UNTIL c% < 32 OR k% >= 60
ENDPROC

DEF PROCoutcome(o%, e%)
  IF o% = 0 THEN PRINT "CLAIMED by the GPU"
  IF o% = 1 THEN PRINT "left to the CPU (below the size limits)"
  IF o% = 2 THEN PRINT "left to the CPU (fill density rule)"
  IF o% = 3 THEN PRINT "left to the CPU (not a plain colour)"
  IF o% = 4 THEN PRINT "*** SCREEN UNUSABLE, error number "; e%
  IF o% = 5 THEN PRINT "*** GPU job failed, error number "; e%
  IF o% = 6 THEN PRINT "left to the CPU (dithered, small)"
  IF o% = 7 THEN PRINT "*** dithered GPU job failed, error number "; e%
  IF o% = 8 THEN PRINT "left alone (nested call)"
  IF o% > 8 THEN PRINT "no outcome recorded ("; o%; ")"
ENDPROC
