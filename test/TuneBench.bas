REM > TuneBench
REM How fast is each GPU setting, and does raising the V3D clock help the big
REM operations? (V3D_AccelTune, driver 0.10)
REM
REM For every row in the table (three driver settings, then the V3D clock at 400
REM and 500 MHz) this script
REM   1. applies the settings,
REM   2. times 8 representative operations - small/medium/large fills, copies
REM      and overlapping copies (a window scroll) - with every one forced onto
REM      the GPU (size limits 0), and reads the driver's own per-phase timer
REM      (V3D_AccelProfile) so the time can be split into: setup (building the
REM      control lists, zeroing scratch), bin (binning job), mid (flush +
REM      render kick) and render (render job),
REM   3. VERIFIES THE PIXELS: the same fill / copy / overlapping copy is drawn
REM      once by the CPU (acceleration stopped) and once by the GPU, and the
REM      two results are compared pixel by pixel. A setting that is fast but
REM      wrong is worth nothing, so a FAIL here matters more than any timing.
REM
REM Rows: "baseline06" = the old 0.06 settings (strongly-ordered scratch, full
REM zeroing, two-pass scrolls); "ncb+zero" = 0.08's; "default" = 0.10's, at the clock
REM the firmware left (normally 250 MHz); "def@400" and "def@500" = the same with
REM the V3D clock requested at 400 / 500 MHz (500 is the stock Pi 4 maximum and is
REM clamped to whatever the firmware reports). The large operations looked clock-bound
REM at 250 MHz, so those two rows are the experiment: if render time per tile falls
REM and every pixel check still passes, the clock goes into the driver. The clock is
REM changed only between configs, with the GPU idle, and put back at the end. The
REM name of each row is printed BEFORE it runs, so if the machine does hang the last
REM name on screen is the cause (the clock rows run last).
REM
REM HOW TO RUN IT: double-click it in the Filer (a "Run ..." window), or use the
REM command line (F12). DO NOT type *TuneBench into a desktop Task Window: a Task
REM Window swallows OS_Plot (TaskWindow claims WrchV, and the kernel then sends the
REM plot as bytes instead of drawing it), so nothing is drawn, nothing reaches the
REM GPU and every timing is just BASIC overhead. That is exactly what happened to the
REM first run on 2026-10-04; the script now checks (below) and stops with a message.
REM It draws in the RIGHT-hand part of the screen so this window's text stays
REM readable; drag a window over the area afterwards to repaint it.
REM
REM Before running: reboot, then *RMLoad the built V3DDriver,ffa (version 0.10).

NSH% = 8
NCFG% = 5
dur% = 30 : REM measurement time per run, in centiseconds
DIM mop%(NSH%), mw%(NSH%), mh%(NSH%)
DIM mok%(NSH%), msx%(NSH%), msy%(NSH%), mdx%(NSH%), mdy%(NSH%)
DIM cname$(NCFG%), cz%(NCFG%), cat%(NCFG%), cov%(NCFG%), cmhz%(NCFG%)
DIM us%(NCFG%, NSH%), jn%(NCFG%, NSH%), jp%(NCFG%, NSH%, 5), ops%(NCFG%, NSH%)
DIM vf%(NCFG%), vc%(NCFG%), vo%(NCFG%), ran%(NCFG%), cclk%(NCFG%)
DIM lim%(4), pcol%(9)

FOR i% = 1 TO NSH%
  READ mop%(i%), mw%(i%), mh%(i%)
NEXT
REM op: 1 = fill (PLOT 101), 2 = copy (PLOT 190), 3 = overlapping copy
DATA 1,16,16, 1,128,128, 1,512,512, 2,32,32, 2,128,128, 2,512,512, 3,128,128, 3,256,256

FOR i% = 1 TO NCFG%
  READ cname$(i%), cz%(i%), cat%(i%), cov%(i%), cmhz%(i%)
NEXT
REM name, zero_mode, scratch_attr, overlap_mode, V3D clock in MHz (0 = leave as found)
DATA "baseline06",0,0,0,0
DATA "ncb+zero",1,1,0,0
DATA "default",1,1,1,0
DATA "def@400",1,1,1,400
DATA "def@500",1,1,1,500

FOR i% = 0 TO 9
  READ pcol%(i%)
NEXT
DATA &0000FF00, &00FF0000, &FF000000, &00FFFF00, &FFFF0000, &FF00FF00, &80808000, &0080FF00, &80FF0000, &FF800000

limsaved% = FALSE
tuned% = FALSE
clk0% = 0
stage$ = "setup"
ON ERROR PROCfail

PROCmodinfo
PRINT "TuneBench v1 - module: "; help$
IF INSTR(help$, "0.10") = 0 AND INSTR(help$, "0.11") = 0 THEN PRINT "WARNING: this script needs V3DDriver 0.10 or later (V3D_AccelTune 8 = clock)."

SYS "V3D_PowerOn" TO pok%, prate%
IF pok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; pok% : END

DIM vin% 80, vout% 80
vin%!0 = 4 : vin%!4 = 5 : vin%!8 = 9
vin%!12 = 128 : vin%!16 = 129 : vin%!20 = 130 : vin%!24 = 131
vin%!28 = 136 : vin%!32 = 137 : vin%!36 = 6 : vin%!40 = 12 : vin%!44 = 148 : vin%!48 = -1
SYS &31, vin%, vout%
xeig% = vout%!0 : yeig% = vout%!4 : log2bpp% = vout%!8
gwl% = vout%!12 : gwb% = vout%!16 : gwr% = vout%!20 : gwt% = vout%!24
orgx% = vout%!28 : orgy% = vout%!32
ll% = vout%!36 : ylim% = vout%!40 : ss% = vout%!44
bpp% = (1 << log2bpp%) DIV 8
PRINT "Screen depth: "; 1 << log2bpp%; " bits per pixel, line length "; ll%; ", top row "; ylim%
IF bpp% <> 4 AND bpp% <> 2 THEN PRINT "This script needs a 16 or 32 bits-per-pixel mode." : END

SYS "V3D_AccelProfile", 2 TO wedged%, wcount%, fine%, vrv%, vrp%, pbase%, fbase%, resc%
PRINT "Timer in use: "; : IF fine% = 1 THEN PRINT "1MHz system timer" ELSE PRINT "10ms fallback (phase times will be rough)"
PRINT "Write-bank address seen: "; : IF vrv% = 1 THEN PRINT "yes, &"; STR$~(vrp%); " (rendering base &"; STR$~(pbase%); ", framestore base &"; STR$~(fbase%); ")" ELSE PRINT "not yet (rendering base &"; STR$~(pbase%); ")"
IF wedged% = 1 THEN PRINT "The GPU is already switched off (fail-stop). Reboot and try again." : END

SYS "V3D_AccelTune", 8, -1 TO clk0%, clkmax%
PRINT "V3D clock: "; clk0%; " MHz now, firmware maximum "; clkmax%; " MHz"
PRINT
PRINT "The last two rows ask the firmware to raise the V3D clock (to 400 MHz and to the"
PRINT "maximum), with the GPU idle, then put it back. Press SPACE to go ahead, any other key to quit."
k$ = GET$
IF k$ <> " " THEN END

SYS "V3D_AccelLimits", -1, -1, -1, -1, -1 TO lim%(0), lim%(1), lim%(2), lim%(3), lim%(4)
limsaved% = TRUE
SYS "V3D_AccelLimits", 0, 0, 0, 0, 0
tuned% = TRUE

REM plotting area: right-hand part of the screen, so this window stays readable
px0% = gwr% - 1060
IF px0% < gwl% + 4 THEN px0% = gwl% + 4
py0% = gwb% + 4
REM verification area, above the largest timed rectangle
vx% = px0% : vy% = py0% + 530
IF vy% + 300 > gwt% THEN PRINT "Screen too short for the verification area (need ~830 rows)." : PROCrestore : END

FOR i% = 1 TO NSH%
  PROCplace(i%)
NEXT

REM first the baseline numbers are proven to be sane
SYS "ColourTrans_SetGCOL", &F3774200,,,0,0
SYS "V3D_AccelStart"

REM ---- SELF-CHECK: does OS_Plot draw here at all? Fill a patch of the screen with a
REM sentinel written straight to memory, plot over it, and read it back.
stage$ = "self-check"
sent% = &5A5A5A5A : IF bpp% = 2 THEN sent% = &5A5A
FOR r% = 0 TO 39
  a% = ss% + (ylim% - (vy% + r%)) * ll% + vx% * bpp%
  IF bpp% = 4 THEN
    FOR q% = 0 TO 39 : a%!(q% * 4) = &5A5A5A5A : NEXT
  ELSE
    FOR q% = 0 TO 39 : a%?(q% * 2) = &5A : a%?(q% * 2 + 1) = &5A : NEXT
  ENDIF
NEXT
PROCfillrect(vx% + 4, vy% + 4, 20, 20)
a% = ss% + (ylim% - (vy% + 10)) * ll% + (vx% + 10) * bpp%
IF bpp% = 4 THEN pv% = !a% ELSE pv% = ?a% + 256 * a%?1
IF pv% = sent% THEN
  PRINT
  PRINT "*** OS_Plot did NOT draw anything. If you typed *TuneBench into a desktop Task"
  PRINT "    Window, that is why: TaskWindow swallows the plot bytes. Close it and"
  PRINT "    double-click the script in the Filer instead (a Run window), or use the"
  PRINT "    command line (F12). Nothing was changed; no timings would be meaningful."
  PROCrestore
  END
ENDIF
PRINT "Self-check: OS_Plot draws here."

PRINT
nrun% = 0
wedgedflag% = FALSE
FOR c% = 1 TO NCFG%
  IF NOT wedgedflag% THEN
    nrun% += 1
    PROCconfig(c%)
  ENDIF
NEXT

stage$ = "restore"
PROCrestore

stage$ = "raw dump"
PRINT
PRINT "RAW (cfg:shape ops jobs setup bin mid render total, us per job summed/averaged):"
FOR c% = 1 TO NCFG%
  IF ran%(c%) THEN
    PRINT cname$(c%); " verify F"; vf%(c%); " C"; vc%(c%); " O"; vo%(c%)
    FOR m% = 1 TO NSH%
      IF mok%(m%) THEN PRINT "  "; m%; ":"; ops%(c%, m%); " "; jn%(c%, m%); " "; jp%(c%, m%, 1); " "; jp%(c%, m%, 2); " "; jp%(c%, m%, 3); " "; jp%(c%, m%, 4); " "; jp%(c%, m%, 5)
    NEXT
  ENDIF
NEXT

stage$ = "pause"
PRINT
PRINT "Done. Press SPACE: the window is cleared and the result tables printed."
A$ = GET$
CLS

stage$ = "table"
PROCmatrix
PROCclocks
PROCphases(1, "FILL 16x16")
PROCphases(4, "COPY 32x32")
PRINT "us/op is wall time per OS_Plot call incl. BASIC/SWI overhead (~10-20us). The"
PRINT "phase tables are the DRIVER's own per-job clock: setup+bin+mid+render = total."
PRINT "V = pixels verified identical to the CPU's result (P pass, F FAIL, - not run)."
PRINT
PRINT "Press SPACE for the large-operation tables (where a faster clock should show)."
A$ = GET$
CLS
PROCphases(3, "FILL 512x512")
PROCphases(6, "COPY 512x512")
PROCphases(7, "SCROLL 128x128")
PROCclocks
END

DEF PROCfail
  ON ERROR OFF
  PRINT
  REPORT
  PRINT " at line "; ERL; " (stage: "; stage$; ")"
  PROCrestore
  END
ENDPROC

REM Puts the driver back as it was: default tuning, default size limits,
REM acceleration running.
DEF PROCrestore
  ON ERROR OFF
  REM Limits first (cannot fail), then the fallible tuning and clock calls: after a GPU
  REM timeout the driver refuses key 5 and key 8, and everything after a refusal would
  REM otherwise be skipped. The clock is restored before the reset for the same reason.
  IF limsaved% THEN SYS "V3D_AccelLimits", lim%(0), lim%(1), lim%(2), lim%(3), lim%(4)
  SYS "V3D_AccelStart"
  IF tuned% THEN
    IF clk0% > 0 THEN SYS "V3D_AccelTune", 8, clk0%
    SYS "V3D_AccelTune", 0, 1
    SYS "V3D_AccelTune", 4, 1
    SYS "V3D_AccelTune", 7, 1
    SYS "V3D_AccelTune", 5, 0 TO rr%
  ENDIF
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

DEF PROCcol(s$, w%)
  LOCAL k%
  k% = w% - LEN(s$)
  IF k% > 0 THEN PRINT STRING$(k%, " ");
  PRINT s$;
ENDPROC

REM Where each timed shape is drawn, and whether it fits the graphics window.
DEF PROCplace(i%)
  LOCAL w%, h%
  w% = mw%(i%) : h% = mh%(i%)
  mok%(i%) = FALSE
  msx%(i%) = px0% : msy%(i%) = py0%
  IF mop%(i%) = 1 THEN
    mok%(i%) = (px0% + w% - 1 <= gwr%) AND (py0% + h% - 1 <= gwt%)
  ENDIF
  IF mop%(i%) = 2 THEN
    IF px0% + 2 * w% + 16 <= gwr% AND py0% + h% - 1 <= gwt% THEN
      mdx%(i%) = px0% + w% + 16 : mdy%(i%) = py0% : mok%(i%) = TRUE
    ENDIF
  ENDIF
  IF mop%(i%) = 3 THEN
    mdx%(i%) = px0% : mdy%(i%) = py0% + h% DIV 4 + 1
    mok%(i%) = (px0% + w% - 1 <= gwr%) AND (mdy%(i%) + h% - 1 <= gwt%)
  ENDIF
ENDPROC

REM Fill the rectangle with bottom-left at absolute pixel (x%,y%).
DEF PROCfillrect(x%, y%, w%, h%)
  LOCAL x1%, y1%, x2%, y2%
  x1% = (x% << xeig%) - orgx% : y1% = (y% << yeig%) - orgy%
  x2% = ((x% + w% - 1) << xeig%) - orgx% : y2% = ((y% + h% - 1) << yeig%) - orgy%
  SYS &45, 4, x1%, y1%
  SYS &45, 101, x2%, y2%
ENDPROC

REM Copy the w% x h% rectangle at (sx%,sy%) so its bottom-left lands on (dx%,dy%).
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

REM Apply a configuration, time every shape, verify the pixels.
DEF PROCconfig(c%)
  LOCAL m%, n%
  stage$ = "config " + cname$(c%)
  PRINT "Running config "; nrun%; ": "; cname$(c%); " (zero="; cz%(c%); " ncb="; cat%(c%); " overlap1pass="; cov%(c%); " clock request="; cmhz%(c%); " MHz)"
  SYS "V3D_AccelTune", 0, cz%(c%)
  SYS "V3D_AccelTune", 4, cat%(c%)
  SYS "V3D_AccelTune", 7, cov%(c%)
  SYS "V3D_AccelTune", 5, 0 TO rr%
  REM ask for the clock this row wants (or the one found at the start), but only if it
  REM is not already in force, so rows that do not change it never touch the firmware
  IF cmhz%(c%) > 0 THEN want% = cmhz%(c%) ELSE want% = clk0%
  SYS "V3D_AccelTune", 8, -1 TO clkbefore%, clkmx%
  IF want% > 0 AND want% <> clkbefore% THEN SYS "V3D_AccelTune", 8, want%
  SYS "V3D_AccelTune", 8, -1 TO clknow%, clkmx%
  PRINT "   V3D clock now "; clknow%; " MHz"
  cclk%(c%) = clknow%
  SYS "V3D_AccelStart"
  ran%(c%) = TRUE
  wedgedflag% = FALSE

  FOR m% = 1 TO NSH%
    IF mok%(m%) THEN
      stage$ = "config " + cname$(c%) + " timing shape " + STR$(m%)
      PROCmeasure(c%, m%)
      SYS "V3D_AccelProfile", 2 TO w%
      IF w% = 1 THEN
        PROCwedged
        ENDPROC
      ENDIF
    ENDIF
  NEXT

  stage$ = "config " + cname$(c%) + " verify"
  vf%(c%) = FNverifyfill
  vc%(c%) = FNverifycopy
  vo%(c%) = FNverifyoverlap
  REM the firmware owns the clock policy; make sure it did not move the clock under us
  SYS "V3D_AccelTune", 8, -1 TO clkend%, clkmx%
  IF clkend% <> clknow% THEN PRINT "   *** the V3D clock CHANGED during this row: "; clknow%; " -> "; clkend%; " MHz (the timings above are suspect)" : cclk%(c%) = clkend%
  SYS "V3D_AccelProfile", 2 TO w%
  IF w% = 1 THEN PROCwedged
  PRINT "   verify: fill "; vf%(c%); " copy "; vc%(c%); " overlap "; vo%(c%); "  (0 = identical to the CPU, -1 = not accelerated)"
ENDPROC

DEF PROCwedged
  LOCAL a%, b%, c%, d%, e%, f%, g%, h%
  wedgedflag% = TRUE
  PRINT
  PRINT "*** A GPU JOB TIMED OUT in "; stage$
  PRINT "The driver has switched acceleration off. Last-timeout registers:"
  SYS "V3D_AccelProfile", 3 TO a%, b%, c%, d%, e%, f%, g%, h%
  PRINT "  which|kind|valid=&"; STR$~(a%); " INT_STS=&"; STR$~(b%); " INT_MSK=&"; STR$~(c%)
  PRINT "  CT0CS=&"; STR$~(d%); " CT0CA=&"; STR$~(e%); " CT0EA=&"; STR$~(f%); " CT1CS=&"; STR$~(g%); " CT1CA=&"; STR$~(h%)
  SYS "V3D_AccelProfile", 4 TO a%, b%, c%, d%, e%, f%, g%, h%
  PRINT "  CT1EA=&"; STR$~(a%); " BFC=&"; STR$~(b%); " RFC=&"; STR$~(c%); " BFC0=&"; STR$~(d%); " RFC0=&"; STR$~(e%)
  PRINT "  IRQs: bin "; f%; " render "; g%; " hub INT_STS=&"; STR$~(h%)
  PRINT "Stopping here. Please photograph/copy this and reboot before doing anything else."
ENDPROC

REM Time one shape: op%/w/h from the table, ops per dur% centiseconds, plus the
REM driver's own per-job phase clock over exactly the timed loop.
DEF PROCmeasure(c%, m%)
  LOCAL n%, view%, jobs%, js%, jb%, jm%, jr%, jt%, jx%, w%, h%
  w% = mw%(m%) : h% = mh%(m%)
  IF mop%(m%) = 1 THEN view% = 0 ELSE view% = 1
  n% = FNrun(m%, view%)
  SYS "V3D_AccelProfile", view%, 0 TO jobs%, js%, jb%, jm%, jr%, jt%, jx%
  ops%(c%, m%) = n%
  us%(c%, m%) = dur% * 10000 DIV (n% + 1)
  jn%(c%, m%) = jobs%
  IF jobs% > 0 THEN
    jp%(c%, m%, 1) = js% DIV jobs%
    jp%(c%, m%, 2) = jb% DIV jobs%
    jp%(c%, m%, 3) = jm% DIV jobs%
    jp%(c%, m%, 4) = jr% DIV jobs%
    jp%(c%, m%, 5) = jt% DIV jobs%
  ENDIF
ENDPROC

DEF FNrun(m%, view%)
  LOCAL op%, w%, h%, n%, t0%, j%, a%, b%, c%, d%, e%, f%, g%
  op% = mop%(m%) : w% = mw%(m%) : h% = mh%(m%)
  REM one untimed warm-up, then zero the driver's phase counters, so neither
  REM one-off initialisation nor the warm-up is in the average
  PROConeop(m%)
  SYS "V3D_AccelProfile", view%, 1 TO a%, b%, c%, d%, e%, f%, g%
  n% = 0
  t0% = TIME
  REPEAT
    PROConeop(m%)
    n% += 1
  UNTIL TIME - t0% >= dur%
= n%

DEF PROConeop(m%)
  IF mop%(m%) = 1 THEN
    PROCfillrect(msx%(m%), msy%(m%), mw%(m%), mh%(m%))
  ELSE
    PROCcopyrect(msx%(m%), msy%(m%), mw%(m%), mh%(m%), mdx%(m%), mdy%(m%))
  ENDIF
ENDPROC

REM The source pattern for the copy tests: 10 vertical bars plus 8 small
REM rectangles, so any shift, tile-edge or channel error shows up.
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

REM Count differing pixels between two w% x h% areas (bottom-left corners given).
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
  PROCfillrect(vx%, vy%, 100, 80)
  SYS "V3D_AccelStart"
  a0% = FNaccelcount(1)
  PROCfillrect(vx% + 120, vy%, 100, 80)
  a1% = FNaccelcount(1)
  IF a1% = a0% THEN = -1
= FNcmp(vx%, vy%, vx% + 120, vy%, 100, 80)

DEF FNverifycopy
  LOCAL a0%, a1%
  SYS "V3D_AccelStop"
  PROCpattern(vx%, vy% + 100)
  PROCcopyrect(vx%, vy% + 100, 100, 80, vx% + 120, vy% + 100)
  SYS "V3D_AccelStart"
  a0% = FNaccelcount(2)
  PROCcopyrect(vx%, vy% + 100, 100, 80, vx% + 240, vy% + 100)
  a1% = FNaccelcount(2)
  IF a1% = a0% THEN = -1
  REM the GPU copy must equal the CPU copy, AND equal the source (so a copy
  REM that did nothing cannot pass)
  IF FNcmp(vx% + 120, vy% + 100, vx% + 240, vy% + 100, 100, 80) <> 0 THEN = FNcmp(vx% + 120, vy% + 100, vx% + 240, vy% + 100, 100, 80)
= FNcmp(vx%, vy% + 100, vx% + 240, vy% + 100, 100, 80)

DEF FNverifyoverlap
  LOCAL a0%, a1%
  SYS "V3D_AccelStop"
  PROCpattern(vx%, vy% + 200)
  PROCpattern(vx% + 240, vy% + 200)
  PROCcopyrect(vx%, vy% + 200, 100, 80, vx%, vy% + 221)
  SYS "V3D_AccelStart"
  a0% = FNaccelcount(3)
  PROCcopyrect(vx% + 240, vy% + 200, 100, 80, vx% + 240, vy% + 221)
  a1% = FNaccelcount(3)
  IF a1% = a0% THEN = -1
= FNcmp(vx%, vy% + 200, vx% + 240, vy% + 200, 100, 101)

REM ---- result tables ----
DEF PROCmatrix
  LOCAL c%, m%, v$
  PRINT "TIME PER OPERATION in microseconds (everything forced onto the GPU) - "; 1 << log2bpp%; " bpp"
  PROCcol("config", 12)
  PROCcol("F16", 6) : PROCcol("F128", 6) : PROCcol("F512", 6)
  PROCcol("C32", 6) : PROCcol("C128", 6) : PROCcol("C512", 6)
  PROCcol("O128", 6) : PROCcol("O256", 6)
  PRINT "  V"
  FOR c% = 1 TO NCFG%
    IF ran%(c%) THEN
      PROCcol(cname$(c%), 12)
      FOR m% = 1 TO NSH%
        IF mok%(m%) THEN PROCcol(STR$(us%(c%, m%)), 6) ELSE PROCcol("-", 6)
      NEXT
      PRINT "  ";
      PROCvflag(vf%(c%))
      PROCvflag(vc%(c%))
      PROCvflag(vo%(c%))
      PRINT
    ENDIF
  NEXT
ENDPROC

DEF PROCclocks
  LOCAL c%
  PRINT "V3D clock in force for each row (MHz):";
  FOR c% = 1 TO NCFG%
    IF ran%(c%) THEN PRINT " "; cname$(c%); "="; cclk%(c%);
  NEXT
  PRINT
ENDPROC

DEF PROCvflag(v%)
  IF v% = 0 THEN
    PRINT "P";
  ELSE
    IF v% = -1 THEN
      PRINT "-";
    ELSE
      PRINT "F";
    ENDIF
  ENDIF
ENDPROC

DEF PROCphases(m%, title$)
  LOCAL c%
  IF NOT mok%(m%) THEN ENDPROC
  PRINT
  PRINT "WHERE THE TIME GOES per GPU job, microseconds: "; title$
  PROCcol("config", 12) : PROCcol("setup", 7) : PROCcol("bin", 6) : PROCcol("mid", 6) : PROCcol("render", 7) : PROCcol("total", 7)
  PRINT
  FOR c% = 1 TO NCFG%
    IF ran%(c%) THEN
      PROCcol(cname$(c%), 12)
      PROCcol(STR$(jp%(c%, m%, 1)), 7) : PROCcol(STR$(jp%(c%, m%, 2)), 6) : PROCcol(STR$(jp%(c%, m%, 3)), 6)
      PROCcol(STR$(jp%(c%, m%, 4)), 7) : PROCcol(STR$(jp%(c%, m%, 5)), 7)
      PRINT
    ENDIF
  NEXT
ENDPROC
