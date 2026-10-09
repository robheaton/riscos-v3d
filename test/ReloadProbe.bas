REM > ReloadProbe
REM What does the V3D look like RIGHT NOW? A short, read-only look, for finding out what is different
REM after a module reload. It needs V3DDriver_bisect5 (the diagnostic build) - the normal module does not
REM have the three register banks it reads. Run it at these moments and photograph each result:
REM   fresh    - just after *RMLoad on a freshly rebooted machine, before anything else
REM   after    - after StressTest has finished (50 iterations OK)
REM   reloaded - right after *RMKill and *RMLoad, BEFORE running StressTest again
REM   failed   - after StressTest has printed FAIL (the machine is still alive then)
REM
REM It prints a line before each step, so if the machine freezes the last line on the screen says which
REM step it was. The steps, in order from the safest to the riskiest:
REM   1. V3D_ClockInfo - asks the firmware about the V3D clock (no V3D register is touched)
REM   2. power side    - PM_GRAFX (is the V3D held in reset?) and the two bus gates (ASB)
REM   3. hub registers - the part of the V3D that is always powered
REM   4. core registers - the part that holds the job queue and the frame counters
REM   5. V3D_PowerOn   - the call StressTest starts with
REM   6, 7. power side and core registers again, to see whether PowerOn changed anything
REM   8. (optional, you are asked) send the V3D clock rate to the firmware again and re-read the clock state
REM A value of &DEADBEEF means the V3D is switched off: that is what a freshly rebooted machine shows before
REM the first PowerOn.
REM The bus gates are shown decoded: REQ_STOP, ACK, EMPTY and FULL. EMPTY means nothing is waiting on
REM that gate; FULL means its queue is full (accesses through it will not complete).
REM
REM Run it by double-clicking it in the Filer (it prints text only).

ON ERROR PROCfail

PROCmodinfo
PRINT "ReloadProbe - module: "; help$
PRINT
INPUT "Which moment is this (fresh, after, reloaded or failed)? " when$
PRINT

PRINT "Step 1: V3D_ClockInfo (firmware mailbox only)..."
SYS "V3D_ClockInfo" TO cs%, cr%, cm%
PRINT "  clock state &"; STR$~(cs%); ", rate "; cr%; " Hz, maximum "; cm%; " Hz"

PROCpower("Step 2: power side (PM_GRAFX and the bus gates)...")
PROChub("Step 3: V3D hub registers...")
PROCcore("Step 4: V3D core registers...")

PRINT "Step 5: V3D_PowerOn..."
SYS "V3D_PowerOn" TO ok%, rate%
PRINT "  ok="; ok%; ", clock "; rate%; " Hz"

PROCpower("Step 6: power side again...")
PROCcore("Step 7: core registers again...")

PRINT
INPUT "Step 8 is optional. Type Y and press Enter to run it, or just press Enter to skip: " yn$
IF yn$ = "Y" OR yn$ = "y" THEN
  PRINT "Step 8: V3D_AccelTune 8,250 (sends the V3D clock rate to the firmware)..."
  SYS "V3D_AccelTune", 8, 250 TO nowclk%, maxclk%
  PRINT "  rate now "; nowclk%; " MHz, firmware maximum "; maxclk%; " MHz"
  SYS "V3D_ClockInfo" TO cs%, cr%, cm%
  PRINT "  clock state now &"; STR$~(cs%); ", rate "; cr%; " Hz"
ENDIF

PRINT
PRINT "Moment: "; when$
PRINT "ReloadProbe finished."
END

DEF PROCpower(title$)
  PRINT title$
  SYS "V3D_RegSnapshot", &51D00001 TO pg%, am%, as%
  IF (pg% AND &40) <> 0 THEN r$ = "released (running)" ELSE r$ = "HELD IN RESET"
  PRINT "  PM_GRAFX = &"; STR$~(pg%); "  V3D reset: "; r$
  PROCasb("ASB master gate", am%)
  PROCasb("ASB slave gate ", as%)
ENDPROC

DEF PROChub(title$)
  PRINT title$
  SYS "V3D_RegSnapshot", &51D00002 TO hid%, hst%, hmk%, mmu%
  PRINT "  hub ident &"; STR$~(hid%); "  interrupt status &"; STR$~(hst%); "  mask &"; STR$~(hmk%); "  MMU &"; STR$~(mmu%)
  IF hid% = &DEADBEEF THEN PRINT "  (&DEADBEEF = the V3D is switched off)"
ENDPROC

DEF PROCcore(title$)
  PRINT title$
  SYS "V3D_RegSnapshot", &51D00003 TO cid%, cst%, cmk%, bfc%, rfc%, cs0%, cs1%, ca0%, ea0%, qts%
  PRINT "  core ident &"; STR$~(cid%); "  interrupt status &"; STR$~(cst%); "  mask &"; STR$~(cmk%)
  PRINT "  frame counters: bin "; bfc%; ", render "; rfc%
  PRINT "  thread 0 (bin) CS=&"; STR$~(cs0%); " CA=&"; STR$~(ca0%); " EA=&"; STR$~(ea0%); " QTS=&"; STR$~(qts%)
  PRINT "  thread 1 (render) CS=&"; STR$~(cs1%)
  IF cid% = &DEADBEEF THEN PRINT "  (&DEADBEEF = the V3D is switched off)"
ENDPROC

DEF PROCasb(name$, v%)
  LOCAL f$
  f$ = ""
  IF (v% AND 1) <> 0 THEN f$ = f$ + " REQ_STOP"
  IF (v% AND 2) <> 0 THEN f$ = f$ + " ACK"
  IF (v% AND 4) <> 0 THEN f$ = f$ + " EMPTY"
  IF (v% AND 8) <> 0 THEN f$ = f$ + " FULL"
  IF f$ = "" THEN f$ = " (no flags)"
  PRINT "  "; name$; " = &"; STR$~(v%); " :"; f$
ENDPROC

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
