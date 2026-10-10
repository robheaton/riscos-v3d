REM > FillCopyBench
REM Where does GPU acceleration actually beat the CPU, and do the driver's
REM default size limits keep it from ever losing? Every accelerated call
REM pays a fixed GPU round trip (build control lists, submit, wait for the
REM bin and render interrupts) whatever the rectangle size, while the CPU
REM fills or copies a small rectangle in microseconds.
REM
REM Sweeps 13 rectangle sizes for three operations - OS_Plot rectangle
REM fill (PLOT 101), non-overlapping rectangle copy and overlapping
REM rectangle copy (PLOT 190; the overlapping one is what a window scroll
REM does) - the exact calls the kernel routes through GraphicsV. Each size
REM is timed three ways in ONE session at ONE screen mode:
REM   CPU  : V3D_AccelStop (GraphicsV released, kernel does everything)
REM   GPU  : acceleration on, driver size limits set to 0 (everything is
REM          sent to the GPU) - the GPU's raw capability at every size
REM   Dflt : acceleration on with the driver's DEFAULT size limits - what
REM          you actually get; sizes below a limit are left to the CPU, so
REM          this column should never be meaningfully slower than CPU
REM The driver's own stats counters are read around every run to prove how
REM many calls were really accelerated (GPU% should be ~100 in the GPU
REM column; Dflt% is 0 where the limit declined the call).
REM
REM Uses OS_Plot via SYS directly (not VDU/PLOT statements) and
REM ColourTrans_SetGCOL, same as RISCOSmark. It scribbles on the lower-left of
REM your screen; that is harmless, drag a window over it to repaint.
REM
REM HOW TO RUN IT: double-click it in the Filer (a "Run ..." window), or use the
REM command line (F12). Do NOT type *FillCopyBench into a desktop Task Window:
REM a Task Window swallows OS_Plot (TaskWindow claims WrchV and the kernel then
REM sends the plot as bytes), so nothing is drawn and every number is just BASIC
REM overhead. (This header used to claim the opposite. It was wrong: every
REM successful run was started from the Filer.)
REM
REM Needs the module built with V3D_AccelLimits (SWI 30).
REM Before running: *RMLoad the built V3DDriver,ffa yourself.

NS% = 17
NOP% = 3
dur% = 50 : REM measurement time per run, in centiseconds
DIM W%(NS%), H%(NS%)
DIM cpu%(NOP%, NS%), all%(NOP%, NS%), dft%(NOP%, NS%)
DIM acA%(NOP%, NS%), acD%(NOP%, NS%)
DIM ok%(NOP%, NS%), dpx%(NOP%, NS%), dpy%(NOP%, NS%)
DIM lim%(4)
FOR i% = 1 TO NS%
  READ W%(i%), H%(i%)
NEXT
DATA 8,8, 16,16, 32,32, 64,64, 96,96, 128,128, 192,192, 256,256, 384,384, 512,512
DATA 1024,16, 512,32, 16,512
REM Big rectangles (added for large screens, where a job is cut into bands): they are skipped on a screen too
REM small to hold them and their copy destination, like any other size that does not fit.
DATA 768,768, 1024,1024, 1600,700, 1900,1000

limsaved% = FALSE
stage$ = "setup"
ON ERROR PROCfail

SYS "V3D_PowerOn" TO pok%, prate%
IF pok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; pok% : END

DIM vin% 64, vout% 64
vin%!0 = 4 : vin%!4 = 5 : vin%!8 = 9
vin%!12 = 128 : vin%!16 = 129 : vin%!20 = 130 : vin%!24 = 131
vin%!28 = 136 : vin%!32 = 137 : vin%!36 = -1
SYS &31, vin%, vout%
xeig% = vout%!0 : yeig% = vout%!4 : log2bpp% = vout%!8
gwl% = vout%!12 : gwb% = vout%!16 : gwr% = vout%!20 : gwt% = vout%!24
orgx% = vout%!28 : orgy% = vout%!32
REM Identify exactly which script and which module build are running, since
REM files are copied between machines by hand.
PROCmodinfo
PRINT "FillCopyBench v6 (solid-colour fill, big sizes) - module: "; help$
PRINT "Screen depth: "; 1 << log2bpp%; " bits per pixel"
PRINT "Graphics window (pixels): "; gwl%; ","; gwb%; " to "; gwr%; ","; gwt%
PRINT "Graphics origin (OS units): "; orgx%; ","; orgy%
px0% = gwl% + 4
py0% = gwb% + 4

SYS "V3D_AccelLimits", -1, -1, -1, -1, -1 TO lim%(0), lim%(1), lim%(2), lim%(3), lim%(4)
limsaved% = TRUE
PRINT "Driver default limits - fill: "; lim%(0); " bytes, "; lim%(1); " rows; copy: "; lim%(2); " bytes, "; lim%(3); " rows; overlap copy: "; lim%(4); " bytes"

REM R3 = 0 means a SOLID colour. (R3 bit 8 = 256 means "generate a dither
REM pattern": ColourTrans may not dither above 16bpp, so that was solid at
REM 32bpp, but at 16bpp it builds a two-colour dither pattern that varies
REM horizontally - the driver correctly declines those, so no fill was
REM accelerated and the 16bpp fill rows of the first 16bpp run were
REM meaningless. R4 = 0 is the GCOL action "store".)
SYS "ColourTrans_SetGCOL", &F3774200,,,0,0

FOR i% = 1 TO NS%
  PROCplace(i%)
NEXT

PRINT
PRINT "Pass 1/3: CPU rendering (V3D_AccelStop)..."
stage$ = "pass 1 (CPU)"
SYS "V3D_AccelStop"
SYS "V3D_AccelStats" TO c0%
PROCpass(1)
SYS "V3D_AccelStats" TO c1%
PRINT "  GraphicsV calls seen by the driver during this pass: "; c1% - c0%; " (should be 0)"

PRINT "Pass 2/3: GPU, size limits set to 0 (everything accelerated)..."
stage$ = "pass 2 (GPU, no limits)"
SYS "V3D_AccelLimits", 0, 0, 0, 0, 0
SYS "V3D_AccelStart"
PROCpass(2)

PRINT "Pass 3/3: GPU with the driver's default size limits..."
stage$ = "pass 3 (GPU, default limits)"
SYS "V3D_AccelLimits", lim%(0), lim%(1), lim%(2), lim%(3), lim%(4)
PROCpass(3)

stage$ = "raw dump"
PRINT
PRINT "RAW COUNTS: operations completed in "; dur% * 10; " ms per run, as cpu/gpu/dflt"
FOR i% = 1 TO NS%
  PRINT STR$(W%(i%)); "x"; STR$(H%(i%));
  PROCraw(1, i%)
  PROCraw(2, i%)
  PROCraw(3, i%)
  PRINT
NEXT

REM The test plots in the lower-left of the screen, which can be underneath
REM this very window, and a long printout scrolls its top lines away. So wait
REM for a key, clear the window (which also wipes any plotting that landed in
REM it), then print the three tables together so they all fit on one screen.
stage$ = "pause"
PRINT
PRINT "Done. Screenshot this page if you like, then press SPACE: the window"
PRINT "is cleared and the three result tables are printed together."
A$ = GET$
CLS

stage$ = "table"
PRINT "FILL (OS_Plot 101), microseconds per operation - "; 1 << log2bpp%; " bits per pixel"
PROCtable(1)
PRINT "COPY, non-overlapping (OS_Plot 190)"
PROCtable(2)
PRINT "COPY, overlapping - a scroll (OS_Plot 190, shifted up a few rows)"
PROCtable(3)
PRINT "Speedup = CPU time / GPU time (below 1.00x the GPU is SLOWER). GPU%/Dflt% ="
PRINT "share of calls actually accelerated. The Dflt columns are what you really get."
END

DEF PROCfail
  ON ERROR OFF
  PRINT
  REPORT
  PRINT " at line "; ERL; " (stage: "; stage$; ")"
  IF limsaved% THEN SYS "V3D_AccelLimits", lim%(0), lim%(1), lim%(2), lim%(3), lim%(4)
  SYS "V3D_AccelStart"
  END
ENDPROC

REM OS_Module 18 looks the module up (R3 = its code address); the module
REM header holds an offset to the zero-terminated help string at +&14, which
REM has a tab between the name and the version. Read it byte by byte.
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
  UNTIL c% < 32 OR k% >= 48
ENDPROC

DEF PROCcol(s$, w%)
  LOCAL k%
  k% = w% - LEN(s$)
  IF k% > 0 THEN PRINT STRING$(k%, " ");
  PRINT s$;
ENDPROC

DEF PROCplace(i%)
  LOCAL w%, h%
  w% = W%(i%) : h% = H%(i%)
  REM fill: the rectangle itself must fit inside the graphics window
  ok%(1, i%) = (px0% + w% - 1 <= gwr%) AND (py0% + h% - 1 <= gwt%)
  dpx%(1, i%) = px0%
  dpy%(1, i%) = py0%
  REM copy: destination beside the source if it fits, otherwise above it
  ok%(2, i%) = FALSE
  IF px0% + 2 * w% + 16 <= gwr% AND py0% + h% - 1 <= gwt% THEN
    dpx%(2, i%) = px0% + w% + 16
    dpy%(2, i%) = py0%
    ok%(2, i%) = TRUE
  ELSE
    IF px0% + w% - 1 <= gwr% AND py0% + 2 * h% + 16 <= gwt% THEN
      dpx%(2, i%) = px0%
      dpy%(2, i%) = py0% + h% + 16
      ok%(2, i%) = TRUE
    ENDIF
  ENDIF
  REM overlapping copy: destination shifted up by a quarter of the height
  REM (plus one row), so it always overlaps the source like a scroll
  dpx%(3, i%) = px0%
  dpy%(3, i%) = py0% + h% DIV 4 + 1
  ok%(3, i%) = (px0% + w% - 1 <= gwr%) AND (dpy%(3, i%) + h% - 1 <= gwt%)
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

DEF PROCpass(which%)
  LOCAL i%, op%, n%, a0%, a1%, pct%
  FOR i% = 1 TO NS%
    FOR op% = 1 TO NOP%
      IF ok%(op%, i%) THEN
        a0% = FNaccelcount(op%)
        n% = FNrun(op%, i%)
        a1% = FNaccelcount(op%)
        pct% = (a1% - a0%) * 100 DIV (n% + 1)
        IF which% = 1 THEN cpu%(op%, i%) = n%
        IF which% = 2 THEN all%(op%, i%) = n% : acA%(op%, i%) = pct%
        IF which% = 3 THEN dft%(op%, i%) = n% : acD%(op%, i%) = pct%
      ENDIF
    NEXT
  NEXT
ENDPROC

DEF FNrun(op%, i%)
  LOCAL w%, h%, x1%, y1%, x2%, y2%, dx%, dy%, n%, t0%
  w% = W%(i%) : h% = H%(i%)
  x1% = (px0% << xeig%) - orgx%
  y1% = (py0% << yeig%) - orgy%
  x2% = ((px0% + w% - 1) << xeig%) - orgx%
  y2% = ((py0% + h% - 1) << yeig%) - orgy%
  dx% = (dpx%(op%, i%) << xeig%) - orgx%
  dy% = (dpy%(op%, i%) << yeig%) - orgy%
  n% = 0
  IF op% = 1 THEN
    REM one untimed warm-up, so any one-off driver initialisation is not timed
    SYS &45, 4, x1%, y1%
    SYS &45, 101, x2%, y2%
    t0% = TIME
    REPEAT
      SYS &45, 4, x1%, y1%
      SYS &45, 101, x2%, y2%
      n% += 1
    UNTIL TIME - t0% >= dur%
  ELSE
    SYS &45, 4, x1%, y1%
    SYS &45, 4, x2%, y2%
    SYS &45, 190, dx%, dy%
    t0% = TIME
    REPEAT
      SYS &45, 4, x1%, y1%
      SYS &45, 4, x2%, y2%
      SYS &45, 190, dx%, dy%
      n% += 1
    UNTIL TIME - t0% >= dur%
  ENDIF
= n%

DEF PROCraw(op%, i%)
  IF ok%(op%, i%) THEN
    PRINT "  "; MID$("FCO", op%, 1); " "; STR$(cpu%(op%, i%)); "/"; STR$(all%(op%, i%)); "/"; STR$(dft%(op%, i%));
  ELSE
    PRINT "  "; MID$("FCO", op%, 1); " n/a";
  ENDIF
ENDPROC

DEF PROCtable(op%)
  LOCAL i%, cu%, gu%, du%, s1%, s2%, shape$, mark$, firstwin%, worst%, worsti%
  PROCcol("Shape", 10) : PROCcol("Pixels", 9) : PROCcol("CPU us", 8) : PROCcol("GPU us", 8) : PROCcol("Speedup", 9)
  PROCcol("GPU%", 5) : PROCcol("Dflt us", 8) : PROCcol("Speedup", 9) : PROCcol("Dflt%", 6)
  PRINT
  firstwin% = 0
  worst% = 100000
  worsti% = 0
  FOR i% = 1 TO NS%
    shape$ = STR$(W%(i%)) + "x" + STR$(H%(i%))
    PROCcol(shape$, 10) : PROCcol(STR$(W%(i%) * H%(i%)), 9)
    IF ok%(op%, i%) = 0 OR cpu%(op%, i%) = 0 OR all%(op%, i%) = 0 OR dft%(op%, i%) = 0 THEN
      PRINT "   (did not fit the graphics window)"
    ELSE
      cu% = dur% * 10000 DIV cpu%(op%, i%)
      gu% = dur% * 10000 DIV all%(op%, i%)
      du% = dur% * 10000 DIV dft%(op%, i%)
      IF cu% < 1 THEN cu% = 1
      IF gu% < 1 THEN gu% = 1
      IF du% < 1 THEN du% = 1
      REM speedups from the raw operation counts, not from the rounded microseconds:
      REM (time ratio = inverse count ratio) - at 2-3 us per op the rounding alone used
      REM to make a harmless 0.94x print as 0.66x and trip the DEFAULT LOSES mark
      s1% = all%(op%, i%) * 100 DIV cpu%(op%, i%)
      s2% = dft%(op%, i%) * 100 DIV cpu%(op%, i%)
      mark$ = ""
      IF s1% < 100 THEN mark$ = "  GPU slower"
      IF s2% < 90 THEN mark$ = mark$ + "  DEFAULT LOSES"
      IF s1% >= 100 AND firstwin% = 0 AND i% <= 10 THEN firstwin% = i%
      IF s2% < worst% THEN worst% = s2% : worsti% = i%
      PROCcol(STR$(cu%), 8) : PROCcol(STR$(gu%), 8)
      PROCcol(STR$(s1% DIV 100) + "." + RIGHT$("0" + STR$(s1% MOD 100), 2) + "x", 9)
      PROCcol(STR$(acA%(op%, i%)), 5)
      PROCcol(STR$(du%), 8)
      PROCcol(STR$(s2% DIV 100) + "." + RIGHT$("0" + STR$(s2% MOD 100), 2) + "x", 9)
      PROCcol(STR$(acD%(op%, i%)), 6)
      PRINT mark$
    ENDIF
  NEXT
  IF firstwin% > 0 THEN
    PRINT "First square size where the GPU wins (no limits): "; W%(firstwin%); "x"; H%(firstwin%); " ("; W%(firstwin%) * H%(firstwin%); " pixels)"
  ELSE
    PRINT "The GPU did not win at any square size tested."
  ENDIF
  IF worsti% > 0 THEN
    PRINT "Default limits, worst result vs CPU: "; STR$(worst% DIV 100); "."; RIGHT$("0" + STR$(worst% MOD 100), 2); "x at "; W%(worsti%); "x"; H%(worsti%)
  ENDIF
ENDPROC
