REM > ModeChangeTest
REM Does the driver still draw correctly across screen mode changes?   (v3)
REM
REM This tests the v0.07 fix for the "stale geometry" bug. When the screen
REM mode changes, the kernel clears the NEW screen (a full-screen GraphicsV
REM fill - big enough to be sent to the GPU) BEFORE it tells modules the mode
REM changed. Up to v0.06 that clear was aimed using the OLD mode's stride,
REM height and depth: growing from 1024x768 to 1920x1080 made the driver's
REM row arithmetic wrap and aim at memory below the framebuffer.
REM
REM What the script does, for a sequence of modes (growing, shrinking,
REM changing depth):
REM   1. fills the whole current screen with a garbage pattern (&5A5A5A5A), so
REM      a clear that MISSES part of the new screen leaves garbage behind;
REM   2. switches mode with the GPU accelerator ON, with the driver's event
REM      trace armed (V3D_AccelProfile 9) so what the driver did with the
REM      kernel's clear is recorded;
REM   3. samples ~25,000 pixels (every 5th row, every 17th column, plus the
REM      whole last row and last column) and counts any that are not the
REM      background colour - a perfect clear leaves 0 - and says WHERE the
REM      stragglers are (which 64x64 tiles) and what colour they are;
REM   4. verifies a GPU fill, copy and overlapping copy against the CPU's result
REM      in the new mode (so stride, height and depth are all right);
REM   5. checks the driver's health (no GPU timeout, nothing declined).
REM
REM Nothing is printed while the modes change (each change clears the screen);
REM the report is printed at the end, two modes to a page (press a key for the
REM next page), and a copy is saved as ModeChangeLog in the current directory.
REM
REM v3 adds, for the starting mode and again after every mode change, a PLOT
REM PROBE: does OS_Plot / RECTANGLE FILL / VDU 25 draw at all with the GPU
REM stopped, and does the same drawing reach the driver (GraphicsV calls, Render
REM calls, fills seen, fills claimed) with the GPU on. v2 found that after the
REM mode changes the verification drawing never reached the driver at all.
REM
REM Modes your monitor does not offer are skipped.
REM
REM HOW TO RUN IT: double-click it in the Filer (a Run window). It changes the screen
REM mode, so close anything you want to keep first; press SPACE at the end to go
REM back to the desktop. Do NOT type it into a desktop Task Window.
REM (Started from the F12 command line instead - *BASIC then CHAIN "<file>" - the
REM mode changes are still handled correctly, but the verification drawing does not
REM reach the driver at all there, so every verify reads -1; the plot probe shows it.)
REM
REM Before running: reboot, then *RMLoad the built V3DDriver,ffa (version 0.11).

NM% = 6
DIM mname$(NM%), mdepth%(NM%)
FOR i% = 1 TO NM%
  READ mname$(i%), mdepth%(i%)
NEXT
DATA "X1024 Y768 C16M",32, "X1920 Y1080 C16M",32, "X1280 Y720 C16M",32
DATA "X1920 Y1080 C64K",16, "X1024 Y768 C64K",16, "X1920 Y1080 C16M",32

DIM pcol%(9), vin% 80, vout% 80, lim%(4)
FOR i% = 0 TO 9
  READ pcol%(i%)
NEXT
DATA &0000FF00, &00FF0000, &FF000000, &00FFFF00, &FFFF0000, &FF00FF00, &80808000, &0080FF00, &80FF0000, &FF800000

REM Per-mode results, printed at the end.
DIM rrun%(NM%), rleft%(NM%), rtot%(NM%), rvf%(NM%), rvc%(NM%), rvo%(NM%), rok%(NM%)
DIM rntile%(NM%), rtile%(NM%, 3), rpx%(NM%, 3), rbox%(NM%, 3)
DIM rst%(NM%, 1, 7), rview%(NM%, 15), rmsg$(NM%)
DIM rtn%(NM%, 1), rtr%(NM%, 1, 23, 6)
DIM rtmo%(NM%, 15), rtmoflag%(NM%)
DIM pr%(NM%, 2, 7), rgw%(NM%, 9), rlater%(NM%)
DIM stat%(3, 7), tm%(1023), stl%(3), spx%(3)

bg32% = 0 : bg16% = 0 : have32% = FALSE : have16% = FALSE
limsaved% = FALSE : spooled% = FALSE
stage$ = "setup"
ON ERROR PROCfail

PROCmodinfo
PRINT "ModeChangeTest v3 - module: "; help$
IF INSTR(help$, "0.11") = 0 THEN PRINT "This script needs V3DDriver 0.11 (it uses the event trace). Reboot, load 0.11 and try again." : END

SYS "V3D_PowerOn" TO pok%, prate%
IF pok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; pok% : END
SYS "V3D_AccelLimits", -1, -1, -1, -1, -1 TO lim%(0), lim%(1), lim%(2), lim%(3), lim%(4)
limsaved% = TRUE
SYS "V3D_AccelProfile", 2 TO wedged%
IF wedged% = 1 THEN PRINT "The GPU is already switched off (fail-stop). Reboot and try again." : END

REM ---- Phase A: learn what "background" looks like at each depth. The CPU
REM clears these (acceleration stopped), so they are the reference.
PRINT
PRINT "Phase A: reading the background pixel at each depth (GPU stopped)..."
SYS "V3D_AccelStop"
FOR i% = 1 TO NM%
  IF (mdepth%(i%) = 32 AND NOT have32%) OR (mdepth%(i%) = 16 AND NOT have16%) THEN
    PROCsetmode(mname$(i%))
    IF modeok% THEN
      PROCreadvdu
      IF mdepth%(i%) = 32 THEN bg32% = FNpixel(0, 0) : have32% = TRUE
      IF mdepth%(i%) = 16 THEN bg16% = FNpixel(0, 0) : have16% = TRUE
      PRINT "  "; mname$(i%); ": background pixel = &"; STR$~(FNpixel(0, 0))
    ELSE
      PRINT "  "; mname$(i%); ": not offered, skipped"
    ENDIF
  ENDIF
NEXT

REM ---- Phase B: the real test, GPU on.
PRINT
PRINT "Phase B: changing mode with the GPU accelerator ON (the screen goes blank between modes)..."
SYS "V3D_AccelLimits", lim%(0), lim%(1), lim%(2), lim%(3), lim%(4)
SYS "V3D_AccelStart"
nbad% = 0 : nrun% = 0
stage$ = "probe at the start"
PROCprobe(0)
FOR i% = 1 TO NM%
  stage$ = "mode " + mname$(i%)
  rrun%(i%) = 0
  IF (mdepth%(i%) = 32 AND have32%) OR (mdepth%(i%) = 16 AND have16%) THEN
    PROCgarbage
    SYS "V3D_AccelProfile", 9, -1
    PROCgetstats(0)
    PROCsetmode(mname$(i%))
    IF modeok% THEN
      rrun%(i%) = 1
      nrun% += 1
      REM text cursor off: it is drawn into the screen (a white underline at the top left)
      REM and a mode-change test must not mistake it for a clear that missed a pixel
      VDU 23, 1, 0; 0; 0; 0;
      PROCreadvdu
      IF mdepth%(i%) = 32 THEN bg% = bg32% ELSE bg% = bg16%
      PROCsample(bg%)
      PROCgetstats(1)
      PROCgrabtrace(i%, 0)
      SYS "V3D_AccelProfile", 9, -1
      rleft%(i%) = sbad% : rtot%(i%) = nsamp% : rntile%(i%) = sntile%
      FOR k% = 0 TO 3
        rtile%(i%, k%) = stl%(k%) : rpx%(i%, k%) = spx%(k%)
      NEXT
      rbox%(i%, 0) = sminx% : rbox%(i%, 1) = smaxx% : rbox%(i%, 2) = sminy% : rbox%(i%, 3) = smaxy%
      PROCresample(bg%, i%)
      stage$ = "probe in " + mname$(i%)
      PROCprobe(i%)
      PROCgetstats(3)
      SYS "V3D_AccelProfile", 9, -1
      stage$ = "verify in " + mname$(i%)
      rvf%(i%) = FNverifyfill
      rvc%(i%) = FNverifycopy
      rvo%(i%) = FNverifyoverlap
      PROCgetstats(2)
      PROCgrabtrace(i%, 1)
      PROCgetviews(i%)
      FOR k% = 0 TO 7
        rst%(i%, 0, k%) = stat%(1, k%) - stat%(0, k%)
        rst%(i%, 1, k%) = stat%(2, k%) - stat%(3, k%)
      NEXT
      rst%(i%, 0, 6) = stat%(1, 6) : rst%(i%, 0, 7) = stat%(1, 7)
      rst%(i%, 1, 6) = stat%(2, 6) : rst%(i%, 1, 7) = stat%(2, 7)
      ok% = (rleft%(i%) = 0) AND (rlater%(i%) = 0) AND (rvf%(i%) = 0) AND (rvc%(i%) = 0) AND (rvo%(i%) = 0)
      ok% = ok% AND (rview%(i%, 0) = 0) AND (rview%(i%, 7) = 0) AND (stat%(2, 6) = stat%(0, 6))
      rok%(i%) = ok%
      IF NOT ok% THEN nbad% += 1
    ENDIF
  ENDIF
NEXT

stage$ = "finish"
SYS "V3D_AccelLimits", lim%(0), lim%(1), lim%(2), lim%(3), lim%(4)
SYS "V3D_AccelStart"
PROCspoolon
PROCreport
PROCspooloff
END

DEF PROCfail
  ON ERROR OFF
  PRINT
  REPORT
  PRINT " at line "; ERL; " (stage: "; stage$; ")"
  IF limsaved% THEN SYS "V3D_AccelLimits", lim%(0), lim%(1), lim%(2), lim%(3), lim%(4)
  SYS "V3D_AccelStart"
  PRINT "Type *Desktop to return to the desktop."
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

REM Try to change mode; modeok% says whether the monitor/driver accepted it.
DEF PROCsetmode(m$)
  LOCAL ERROR
  ON ERROR LOCAL modeok% = FALSE : ENDPROC
  modeok% = FALSE
  MODE m$
  modeok% = TRUE
ENDPROC

REM Copy everything the report prints to a file as well (best effort).
DEF PROCspoolon
  LOCAL ERROR
  ON ERROR LOCAL spooled% = FALSE : ENDPROC
  OSCLI "SPOOL ModeChangeLog"
  spooled% = TRUE
ENDPROC

DEF PROCspooloff
  LOCAL ERROR
  ON ERROR LOCAL ENDPROC
  IF spooled% THEN OSCLI "SPOOL"
ENDPROC

REM Current screen's variables, re-read after every mode change.
DEF PROCreadvdu
  vin%!0 = 4 : vin%!4 = 5 : vin%!8 = 9 : vin%!12 = 6 : vin%!16 = 12 : vin%!20 = 11 : vin%!24 = 148
  vin%!28 = 136 : vin%!32 = 137 : vin%!36 = 128 : vin%!40 = 129 : vin%!44 = 130 : vin%!48 = 131
  vin%!52 = 0 : vin%!56 = 10 : vin%!60 = -1
  SYS &31, vin%, vout%
  xeig% = vout%!0 : yeig% = vout%!4 : log2bpp% = vout%!8
  ll% = vout%!12 : ylim% = vout%!16 : xlim% = vout%!20 : ss% = vout%!24
  orgx% = vout%!28 : orgy% = vout%!32
  gwl% = vout%!36 : gwb% = vout%!40 : gwr% = vout%!44 : gwt% = vout%!48
  mflags% = vout%!52 : log2bpc% = vout%!56
  bpp% = (1 << log2bpp%) DIV 8
ENDPROC

REM Absolute pixel (x%,y%), y counted up from the bottom of the screen.
DEF FNpixel(x%, y%)
  LOCAL a%
  a% = ss% + (ylim% - y%) * ll% + x% * bpp%
  IF bpp% = 4 THEN = !a%
= ?a% + 256 * a%?1

REM Fill the whole visible screen with garbage, so a missed clear is visible.
DEF PROCgarbage
  LOCAL p%, e%
  PROCreadvdu
  e% = ss% + ll% * (ylim% + 1) - 4
  FOR p% = ss% TO e% STEP 4
    !p% = &5A5A5A5A
  NEXT
ENDPROC

REM Count sampled pixels that are NOT the background after a mode change, and
REM remember where (which 64x64 tiles, counted from the top-left of the screen),
REM what colour, and the bounding box (x across, y down from the top).
DEF PROCsample(bg%)
  LOCAL x%, y%, k%
  sbad% = 0 : nsamp% = 0 : sntile% = 0
  sminx% = 99999 : smaxx% = -1 : sminy% = 99999 : smaxy% = -1
  FOR k% = 0 TO 1023
    tm%(k%) = 0
  NEXT
  FOR k% = 0 TO 3
    stl%(k%) = -1 : spx%(k%) = 0
  NEXT
  FOR y% = 0 TO ylim% STEP 5
    FOR x% = 0 TO xlim% STEP 17
      PROCpix(x%, y%, bg%)
    NEXT
  NEXT
  FOR x% = 0 TO xlim%
    PROCpix(x%, 0, bg%)
    PROCpix(x%, ylim%, bg%)
  NEXT
  FOR y% = 0 TO ylim%
    PROCpix(0, y%, bg%)
    PROCpix(xlim%, y%, bg%)
  NEXT
ENDPROC

DEF PROCpix(x%, y%, bg%)
  LOCAL v%, t%, yt%
  nsamp% += 1
  v% = FNpixel(x%, y%)
  IF v% <> bg% THEN
    yt% = ylim% - y%
    IF sbad% < 4 THEN spx%(sbad%) = v%
    sbad% += 1
    IF x% < sminx% THEN sminx% = x%
    IF x% > smaxx% THEN smaxx% = x%
    IF yt% < sminy% THEN sminy% = yt%
    IF yt% > smaxy% THEN smaxy% = yt%
    t% = (yt% DIV 64) * 32 + (x% DIV 64)
    IF tm%(t%) = 0 THEN
      tm%(t%) = 1
      IF sntile% < 4 THEN stl%(sntile%) = t%
      sntile% += 1
    ENDIF
  ENDIF
ENDPROC

REM The driver's counters: calls, Render calls, fill ops, solid fills, claimed, errors,
REM then screens declined and the last decline's error number.
DEF PROCgetstats(n%)
  LOCAL a0%, a1%, a2%, a3%, a4%, a5%, d6%, d7%
  SYS "V3D_AccelStats" TO a0%, a1%, a2%, a3%, a4%, a5%
  SYS "V3D_AccelProfile", 7 TO ,,,,,, d6%, d7%
  stat%(n%, 0) = a0% : stat%(n%, 1) = a1% : stat%(n%, 2) = a2% : stat%(n%, 3) = a3%
  stat%(n%, 4) = a4% : stat%(n%, 5) = a5% : stat%(n%, 6) = d6% : stat%(n%, 7) = d7%
ENDPROC

REM Read out the driver's event log into the per-mode tables (phase ph%: 0 = the
REM mode change, 1 = the verification).
DEF PROCgrabtrace(m%, ph%)
  LOCAL i%, n%, t0%, t1%, t2%, t3%, t4%, t5%, t6%, t7%
  SYS "V3D_AccelProfile", 9, 0 TO t0%, t1%, t2%, t3%, t4%, t5%, t6%, t7%
  n% = t7% AND &FFFF
  IF n% > 24 THEN n% = 24
  rtn%(m%, ph%) = n%
  FOR i% = 0 TO n% - 1
    SYS "V3D_AccelProfile", 9, i% TO t0%, t1%, t2%, t3%, t4%, t5%, t6%, t7%
    rtr%(m%, ph%, i%, 0) = t0% : rtr%(m%, ph%, i%, 1) = t1% : rtr%(m%, ph%, i%, 2) = t2%
    rtr%(m%, ph%, i%, 3) = t3% : rtr%(m%, ph%, i%, 4) = t4% : rtr%(m%, ph%, i%, 5) = t5%
    rtr%(m%, ph%, i%, 6) = t6%
  NEXT
ENDPROC

REM Health and screen views, after the verification.
DEF PROCgetviews(m%)
  LOCAL v0%, v1%, v2%, v3%, v4%, v5%, v6%, v7%
  SYS "V3D_AccelProfile", 2 TO v0%, v1%, v2%, v3%, v4%, v5%, v6%, v7%
  rview%(m%, 0) = v0% : rview%(m%, 1) = v1% : rview%(m%, 2) = v3% : rview%(m%, 3) = v4%
  rview%(m%, 4) = v5% : rview%(m%, 5) = v6% : rview%(m%, 6) = v7%
  rtmoflag%(m%) = 0
  IF v0% <> 0 OR v1% <> 0 THEN
    rtmoflag%(m%) = 1
    SYS "V3D_AccelProfile", 3 TO v0%, v1%, v2%, v3%, v4%, v5%, v6%, v7%
    rtmo%(m%, 0) = v0% : rtmo%(m%, 1) = v1% : rtmo%(m%, 2) = v2% : rtmo%(m%, 3) = v3%
    rtmo%(m%, 4) = v4% : rtmo%(m%, 5) = v5% : rtmo%(m%, 6) = v6% : rtmo%(m%, 7) = v7%
    SYS "V3D_AccelProfile", 4 TO v0%, v1%, v2%, v3%, v4%, v5%, v6%, v7%
    rtmo%(m%, 8) = v0% : rtmo%(m%, 9) = v1% : rtmo%(m%, 10) = v2% : rtmo%(m%, 11) = v3%
    rtmo%(m%, 12) = v4% : rtmo%(m%, 13) = v5% : rtmo%(m%, 14) = v6% : rtmo%(m%, 15) = v7%
  ENDIF
  SYS "V3D_AccelProfile", 6 TO v0%, v1%, v2%, v3%, v4%, v5%, v6%, v7%
  rview%(m%, 7) = v0% : rview%(m%, 8) = v2% : rview%(m%, 9) = v3% : rview%(m%, 10) = v4%
  rview%(m%, 11) = v5% : rview%(m%, 12) = v6% : rview%(m%, 13) = v7%
  rmsg$(m%) = ""
  IF v0% <> 0 THEN rmsg$(m%) = $v1%
ENDPROC

REM ---- the plot probe ----
REM Fill a 40x40 patch with a sentinel by direct memory writes, then draw a rectangle over
REM it three ways. First with the GPU stopped (does the drawing happen at all?), then with
REM the GPU on and every size limit 0 (does it reach the driver? is it drawn?).
DEF PROCsentinel(px%, py%, w%, h%, sent%)
  LOCAL x%, y%, a%
  FOR y% = py% TO py% + h% - 1
    FOR x% = px% TO px% + w% - 1
      a% = ss% + (ylim% - y%) * ll% + x% * bpp%
      IF bpp% = 4 THEN
        !a% = sent%
      ELSE
        ?a% = sent% AND 255
        a%?1 = (sent% >> 8) AND 255
      ENDIF
    NEXT
  NEXT
ENDPROC

DEF PROCdraw(k%, x%, y%, w%, h%)
  LOCAL x1%, y1%, x2%, y2%
  x1% = (x% << xeig%) - orgx% : y1% = (y% << yeig%) - orgy%
  x2% = ((x% + w% - 1) << xeig%) - orgx% : y2% = ((y% + h% - 1) << yeig%) - orgy%
  CASE k% OF
    WHEN 0
      SYS &45, 4, x1%, y1%
      SYS &45, 101, x2%, y2%
    WHEN 1
      RECTANGLE FILL x1%, y1%, x2% - x1%, y2% - y1%
    WHEN 2
      VDU 25, 4, x1%; y1%;
      VDU 25, 101, x2%; y2%;
  ENDCASE
ENDPROC

DEF PROCprobe(m%)
  LOCAL k%, px%, py%, sent%, c0%, c1%, c2%, c3%, c4%, c5%, d0%, d1%, d2%, d3%, d4%, d5%
  PROCreadvdu
  rgw%(m%, 0) = gwl% : rgw%(m%, 1) = gwb% : rgw%(m%, 2) = gwr% : rgw%(m%, 3) = gwt%
  rgw%(m%, 4) = orgx% : rgw%(m%, 5) = orgy% : rgw%(m%, 6) = mflags% : rgw%(m%, 7) = log2bpc%
  rgw%(m%, 8) = xeig% : rgw%(m%, 9) = yeig%
  px% = 20 : py% = 20
  sent% = &5A5A5A5A : IF bpp% = 2 THEN sent% = &5A5A
  SYS "ColourTrans_SetGCOL", &F3774200,,,0,0
  FOR k% = 0 TO 2
    SYS "V3D_AccelStop"
    PROCsentinel(px%, py%, 40, 40, sent%)
    PROCdraw(k%, px% + 4, py% + 4, 20, 20)
    IF FNpixel(px% + 10, py% + 10) <> sent% THEN pr%(m%, k%, 0) = 1 ELSE pr%(m%, k%, 0) = 0
    SYS "V3D_AccelLimits", 0, 0, 0, 0, 0
    SYS "V3D_AccelStart"
    PROCsentinel(px%, py%, 40, 40, sent%)
    SYS "V3D_AccelStats" TO c0%, c1%, c2%, c3%, c4%, c5%
    PROCdraw(k%, px% + 4, py% + 4, 20, 20)
    SYS "V3D_AccelStats" TO d0%, d1%, d2%, d3%, d4%, d5%
    SYS "V3D_AccelLimits", lim%(0), lim%(1), lim%(2), lim%(3), lim%(4)
    pr%(m%, k%, 1) = d0% - c0% : pr%(m%, k%, 2) = d1% - c1% : pr%(m%, k%, 3) = d2% - c2% : pr%(m%, k%, 4) = d4% - c4%
    IF FNpixel(px% + 10, py% + 10) <> sent% THEN pr%(m%, k%, 5) = 1 ELSE pr%(m%, k%, 5) = 0
  NEXT
ENDPROC

REM A second look at the cleared screen a little later than the first, to see whether
REM anything changes (a clear that was still landing, or something drawing afterwards).
DEF PROCresample(bg%, m%)
  PROCsample(bg%)
  rlater%(m%) = sbad%
ENDPROC

REM ---- the report ----
DEF PROCreport
  LOCAL m%, k%, npage%
  PRINT
  PRINT "==== ModeChangeTest v3 report - "; help$; " ===="
  PRINT "background pixel: 32bpp &"; STR$~(bg32%); "  16bpp &"; STR$~(bg16%)
  PROCreportprobe(0)
  npage% = 0
  FOR m% = 1 TO NM%
    IF rrun%(m%) THEN
      PROCreportmode(m%)
      npage% += 1
      IF npage% MOD 2 = 0 AND m% < NM% THEN
        PRINT
        PRINT "---- press any key for the next page ----"
        k% = GET
      ENDIF
    ENDIF
  NEXT
  PRINT
  IF nrun% = 0 THEN
    PRINT "No mode could be set - nothing was tested."
  ELSE
    IF nbad% = 0 THEN PRINT "ALL "; nrun%; " MODE CHANGES PASSED." ELSE PRINT nbad%; " of "; nrun%; " mode changes FAILED."
  ENDIF
  PRINT "Type *Desktop to return to the desktop."
ENDPROC

DEF PROCreportprobe(m%)
  LOCAL k%
  IF m% = 0 THEN PRINT "plot probe before any mode change (the mode you started in):" ELSE PRINT "  plot probe:"
  PRINT "    vdu     : graphics window ";  rgw%(m%, 0);  ",";  rgw%(m%, 1);  " to ";  rgw%(m%, 2);
  PRINT ",";  rgw%(m%, 3);  "  origin ";  rgw%(m%, 4);  ",";  rgw%(m%, 5);  "  ModeFlags &";
  PRINT STR$~(rgw%(m%, 6));  "  Log2BPC ";  rgw%(m%, 7);  "  eig ";  rgw%(m%, 8);  ",";  rgw%(m%, 9)
  FOR k% = 0 TO 2
    CASE k% OF
      WHEN 0
        PRINT "    OS_Plot         : ";
      WHEN 1
        PRINT "    RECTANGLE FILL  : ";
      WHEN 2
        PRINT "    VDU 25          : ";
    ENDCASE
    PRINT "CPU path draws ";  pr%(m%, k%, 0);  "  | GPU on: GraphicsV calls ";  pr%(m%, k%, 1);
    PRINT " Render ";  pr%(m%, k%, 2);  " fills seen ";  pr%(m%, k%, 3);  " CLAIMED ";  pr%(m%, k%, 4);
    PRINT " pixels drawn ";  pr%(m%, k%, 5)
  NEXT
ENDPROC

DEF PROCreportmode(m%)
  LOCAL k%, t%
  PRINT
  IF rok%(m%) THEN PRINT m%; ". "; mname$(m%); "   PASS" ELSE PRINT m%; ". "; mname$(m%); "   *** FAIL ***"
  PRINT "  clear : ";  rleft%(m%);  " of ";  rtot%(m%);
  PRINT " sampled pixels were not background straight after the mode change, ";  rlater%(m%);
  PRINT " on a second look, in ";  rntile%(m%);  " tile(s)"
  IF rleft%(m%) <> 0 THEN
    PRINT "          first colours:";
    FOR k% = 0 TO 3
      IF k% < rleft%(m%) THEN PRINT " &"; STR$~(rpx%(m%, k%));
    NEXT
    PRINT
    PRINT "          tiles (column,row from top-left):";
    FOR k% = 0 TO 3
      t% = rtile%(m%, k%)
      IF t% >= 0 THEN PRINT " ("; t% MOD 32; ","; t% DIV 32; ")";
    NEXT
    PRINT
    PRINT "          box: x "; rbox%(m%, 0); "-"; rbox%(m%, 1); "  y from top "; rbox%(m%, 2); "-"; rbox%(m%, 3)
  ENDIF
  PRINT "  verify: fill ";  rvf%(m%);  " copy ";  rvc%(m%);  " overlap ";  rvo%(m%);
  PRINT "   (-1 = the GPU did not take it, >0 = wrong pixels)"
  PRINT "  at the mode change : GraphicsV calls ";  rst%(m%, 0, 0);  " Render calls ";  rst%(m%, 0, 1);
  PRINT " fill ops ";  rst%(m%, 0, 2);  " solid ";  rst%(m%, 0, 3);  " CLAIMED ";  rst%(m%, 0, 4);
  PRINT " errors ";  rst%(m%, 0, 5)
  PRINT "  during verification: GraphicsV calls ";  rst%(m%, 1, 0);  " Render calls ";  rst%(m%, 1, 1);
  PRINT " fill ops ";  rst%(m%, 1, 2);  " solid ";  rst%(m%, 1, 3);  " CLAIMED ";  rst%(m%, 1, 4);
  PRINT " errors ";  rst%(m%, 1, 5)
  PRINT "  screen declines so far ";  rst%(m%, 1, 6);  " (last error number ";  rst%(m%, 1, 7);
  PRINT ")   cached decline now: ";  rview%(m%, 7);
  IF rview%(m%, 7) <> 0 THEN PRINT " - "; rmsg$(m%) ELSE PRINT
  PRINT "  health : wedged "; rview%(m%, 0); " timeouts "; rview%(m%, 1); " rescues "; rview%(m%, 6)
  IF rtmoflag%(m%) THEN
    PRINT "  last GPU timeout (INT_STS, INT_MSK, CT0CS, CT0CA, CT0EA, CT1CS, CT1CA, CT1EA, BFC, RFC...):"
    PRINT "    &";  STR$~(rtmo%(m%, 0));  " &";  STR$~(rtmo%(m%, 1));  " &";  STR$~(rtmo%(m%, 2));  " &";
    PRINT STR$~(rtmo%(m%, 3));  " &";  STR$~(rtmo%(m%, 4));  " &";  STR$~(rtmo%(m%, 5));  " &";
    PRINT STR$~(rtmo%(m%, 6));  " &";  STR$~(rtmo%(m%, 7))
    PRINT "    &";  STR$~(rtmo%(m%, 8));  " &";  STR$~(rtmo%(m%, 9));  " &";  STR$~(rtmo%(m%, 10));
    PRINT " &";  STR$~(rtmo%(m%, 11));  " &";  STR$~(rtmo%(m%, 12));  " &";  STR$~(rtmo%(m%, 13));
    PRINT " &";  STR$~(rtmo%(m%, 14));  " &";  STR$~(rtmo%(m%, 15))
  ENDIF
  PRINT "  write address: valid ";  rview%(m%, 2);  " &";  STR$~(rview%(m%, 3));  "  used base &";
  PRINT STR$~(rview%(m%, 4));  "  framestore &";  STR$~(rview%(m%, 5))
  PRINT "  screen : log2bpp ";  rview%(m%, 8);  " line &";  STR$~(rview%(m%, 9));  " xlimit ";
  PRINT rview%(m%, 10);  " ylimit ";  rview%(m%, 11);  " size &";  STR$~(rview%(m%, 12));
  PRINT " framestore size &";  STR$~(rview%(m%, 13))
  PROCreportprobe(m%)
  PRINT "  driver events at the mode change:"
  PROCprintevents(m%, 0)
  PRINT "  driver events during verification:"
  PROCprintevents(m%, 1)
ENDPROC

DEF PROCprintevents(m%, ph%)
  LOCAL i%
  IF rtn%(m%, ph%) = 0 THEN
    PRINT "    (none)"
  ELSE
    FOR i% = 0 TO rtn%(m%, ph%) - 1
      PROCprintevent(m%, ph%, i%)
    NEXT
  ENDIF
ENDPROC

DEF PROCprintevent(m%, ph%, i%)
  LOCAL kind%, out%, err%
  kind% = (rtr%(m%, ph%, i%, 0) >> 8) AND 255
  out% = (rtr%(m%, ph%, i%, 0) >> 16) AND 255
  err% = rtr%(m%, ph%, i%, 6) AND &FFFF
  CASE kind% OF
    WHEN 1
      PRINT "    SetMode"
    WHEN 2
      PRINT "    SetDMAAddress: DAG "; rtr%(m%, ph%, i%, 2); " address &"; STR$~(rtr%(m%, ph%, i%, 1))
    WHEN 3
      PRINT "    Fill ";  rtr%(m%, ph%, i%, 1);  ",";  rtr%(m%, ph%, i%, 2);  " to ";
      PRINT rtr%(m%, ph%, i%, 3);  ",";  rtr%(m%, ph%, i%, 4);  " : ";
      PROCoutcome(out%, err%)
    WHEN 4
      PRINT "    Copy from ";  rtr%(m%, ph%, i%, 1);  ",";  rtr%(m%, ph%, i%, 2);  " to ";
      PRINT rtr%(m%, ph%, i%, 3);  ",";  rtr%(m%, ph%, i%, 4);  " : ";
      PROCoutcome(out%, err%)
    OTHERWISE
      PRINT "    event kind "; kind%
  ENDCASE
ENDPROC

DEF PROCoutcome(o%, e%)
  CASE o% OF
    WHEN 0
      PRINT "CLAIMED by the GPU"
    WHEN 1
      PRINT "left to the CPU (below the size limits)"
    WHEN 2
      PRINT "left to the CPU (fill density rule)"
    WHEN 3
      PRINT "left to the CPU (not a plain colour)"
    WHEN 4
      PRINT "*** SCREEN UNUSABLE, error number "; e%
    WHEN 5
      PRINT "*** GPU job failed, error number "; e%
    WHEN 6
      PRINT "left to the CPU (dithered, small)"
    WHEN 7
      PRINT "*** dithered GPU job failed, error number "; e%
    WHEN 8
      PRINT "left alone (nested call)"
    OTHERWISE
      PRINT "no outcome recorded ("; o%; ")"
  ENDCASE
ENDPROC

REM ---- GPU-vs-CPU verification, as in TuneBench ----
DEF PROCfillrect(x%, y%, w%, h%)
  LOCAL x1%, y1%, x2%, y2%
  x1% = (x% << xeig%) - orgx% : y1% = (y% << yeig%) - orgy%
  x2% = ((x% + w% - 1) << xeig%) - orgx% : y2% = ((y% + h% - 1) << yeig%) - orgy%
  SYS &45, 4, x1%, y1%
  SYS &45, 101, x2%, y2%
ENDPROC

DEF PROCcopyrect(sx%, sy%, w%, h%, dx%, dy%)
  LOCAL x1%, y1%, x2%, y2%, gx%, gy%
  x1% = (sx% << xeig%) - orgx% : y1% = (sy% << yeig%) - orgy%
  x2% = ((sx% + w% - 1) << xeig%) - orgx% : y2% = ((sy% + h% - 1) << yeig%) - orgy%
  gx% = (dx% << xeig%) - orgx% : gy% = (dy% << yeig%) - orgy%
  SYS &45, 4, x1%, y1%
  SYS &45, 4, x2%, y2%
  SYS &45, 190, gx%, gy%
ENDPROC

DEF FNaccelcount(op%)
  LOCAL a%, d%, o%
  IF op% = 1 THEN
    SYS "V3D_AccelStats" TO ,,,,a%
  ELSE
    SYS "V3D_AccelCopyStats" TO ,,d%,,o%
    a% = d% + o%
  ENDIF
= a%

DEF PROCpattern(x%, y%)
  LOCAL k%
  FOR k% = 0 TO 9
    SYS "ColourTrans_SetGCOL", pcol%(k%),,,0,0
    PROCfillrect(x% + k% * 10, y%, 10, 80)
  NEXT
  FOR k% = 0 TO 7
    SYS "ColourTrans_SetGCOL", pcol%((k% + 3) MOD 10),,,0,0
    PROCfillrect(x% + 2 + k% * 11, y% + k% * 9, 20, 4)
  NEXT
  SYS "ColourTrans_SetGCOL", &F3774200,,,0,0
ENDPROC

DEF FNcmp(ax%, ay%, bx%, by%, w%, h%)
  LOCAL x%, y%, pa%, pb%, bad%
  bad% = 0
  FOR y% = 0 TO h% - 1
    pa% = ss% + (ylim% - (ay% + y%)) * ll% + ax% * bpp%
    pb% = ss% + (ylim% - (by% + y%)) * ll% + bx% * bpp%
    IF bpp% = 4 THEN
      FOR x% = 0 TO w% - 1
        IF pa%!(x% * 4) <> pb%!(x% * 4) THEN bad% += 1
      NEXT
    ELSE
      FOR x% = 0 TO w% - 1
        IF pa%?(x% * 2) <> pb%?(x% * 2) OR pa%?(x% * 2 + 1) <> pb%?(x% * 2 + 1) THEN bad% += 1
      NEXT
    ENDIF
  NEXT
= bad%

DEF FNverifyfill
  LOCAL a0%, a1%
  SYS "ColourTrans_SetGCOL", &F3774200,,,0,0
  SYS "V3D_AccelStop"
  PROCfillrect(20, 20, 100, 80)
  SYS "V3D_AccelLimits", 0, 0, 0, 0, 0
  SYS "V3D_AccelStart"
  a0% = FNaccelcount(1)
  PROCfillrect(140, 20, 100, 80)
  a1% = FNaccelcount(1)
  SYS "V3D_AccelLimits", lim%(0), lim%(1), lim%(2), lim%(3), lim%(4)
  IF a1% = a0% THEN = -1
= FNcmp(20, 20, 140, 20, 100, 80)

DEF FNverifycopy
  LOCAL a0%, a1%
  SYS "V3D_AccelStop"
  PROCpattern(20, 120)
  PROCcopyrect(20, 120, 100, 80, 140, 120)
  SYS "V3D_AccelLimits", 0, 0, 0, 0, 0
  SYS "V3D_AccelStart"
  a0% = FNaccelcount(2)
  PROCcopyrect(20, 120, 100, 80, 260, 120)
  a1% = FNaccelcount(2)
  SYS "V3D_AccelLimits", lim%(0), lim%(1), lim%(2), lim%(3), lim%(4)
  IF a1% = a0% THEN = -1
  IF FNcmp(140, 120, 260, 120, 100, 80) <> 0 THEN = FNcmp(140, 120, 260, 120, 100, 80)
= FNcmp(20, 120, 260, 120, 100, 80)

DEF FNverifyoverlap
  LOCAL a0%, a1%
  SYS "V3D_AccelStop"
  PROCpattern(20, 220)
  PROCpattern(260, 220)
  PROCcopyrect(20, 220, 100, 80, 20, 241)
  SYS "V3D_AccelLimits", 0, 0, 0, 0, 0
  SYS "V3D_AccelStart"
  a0% = FNaccelcount(3)
  PROCcopyrect(260, 220, 100, 80, 260, 241)
  a1% = FNaccelcount(3)
  SYS "V3D_AccelLimits", lim%(0), lim%(1), lim%(2), lim%(3), lim%(4)
  IF a1% = a0% THEN = -1
= FNcmp(20, 220, 260, 220, 100, 101)
