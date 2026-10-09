REM > VerifySuite
REM Differential test: does the GPU draw EXACTLY the same pixels as the CPU?
REM
REM Draws hundreds of random fills, copies and overlapping copies (scrolls) -
REM every one forced onto the GPU (size limits 0) - and compares the result
REM pixel by pixel with the same operation drawn by the kernel's own CPU code
REM (acceleration stopped). Each rectangle is drawn inside a PADDED area that was
REM first filled with a sentinel colour, and the whole padded area is compared,
REM so a GPU write that lands OUTSIDE the requested rectangle is caught too.
REM
REM It deliberately includes the awkward cases: 1-pixel sizes, sizes either side
REM of the 64-pixel tile (63/64/65, 127/128/129...), odd X offsets (at 16bpp that
REM puts a rectangle on a half-word boundary), and for scrolls every sign
REM combination of shift, shifts smaller and larger than a tile.
REM
REM HOW TO RUN IT: double-click it in the Filer (that opens a "Run ..." window), or
REM use the command line after F12. DO NOT type *VerifySuite into a desktop Task
REM Window: a Task Window swallows OS_Plot (the kernel sends the plot bytes through
REM WrchV, which TaskWindow claims, so nothing is drawn at all - found from the
REM kernel source and TaskWindow source on 2026-10-07). The script checks for this
REM and says so. It takes a few minutes and scribbles on the right-hand part of
REM the screen.
REM
REM Before running: reboot, then *RMLoad V3DDriver,ffa (version 0.10 or later; options 5 and 6 need 0.12).

NFILL% = 150 : NCOPY% = 150 : NOVL% = 150
REM sizes that are most likely to expose tile-edge and packing bugs
DIM edge%(21)
FOR i% = 0 TO 21
  READ edge%(i%)
NEXT
DATA 1,2,3,4,7,8,15,16,17,31,32,33,63,64,65,100,127,128,129,191,192,193

DIM vin% 80, vout% 80, lim%(4)
DIM prx%(15), pry%(15), prw%(15), prh%(15), prc%(15)
limsaved% = FALSE : tuned% = FALSE : banded% = FALSE : clk0% = 0
stage$ = "setup"
ON ERROR PROCfail

PROCmodinfo
PRINT "VerifySuite v1 - module: "; help$
PRINT
SYS "V3D_PowerOn" TO pok%, prate%
IF pok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; pok% : END

PROCreadvdu
PRINT "Screen: "; 1 << log2bpp%; " bpp, line length "; ll%; ", rows "; ylim% + 1; ", columns "; xlim% + 1
PRINT "Graphics window (pixels): "; gwl%; ","; gwb%; " to "; gwr%; ","; gwt%; "   origin (OS units): "; orgx%; ","; orgy%
IF bpp% <> 4 AND bpp% <> 2 THEN PRINT "Needs a 16 or 32 bits-per-pixel mode." : END
SYS "V3D_AccelProfile", 2 TO wedged%
IF wedged% = 1 THEN PRINT "The GPU is already switched off (fail-stop). Reboot and try again." : END

IF INSTR(help$, "0.10") = 0 AND INSTR(help$, "0.11") = 0 AND INSTR(help$, "0.12") = 0 THEN PRINT "WARNING: this script needs V3DDriver 0.10 or later (V3D_AccelTune 8 = clock)."
PRINT
PRINT "Which settings should be tested?"
PRINT "  1 = the 0.10 defaults: scratch_attr=1, zero_mode=1, ONE-PASS scrolls, clock as found"
PRINT "  2 = the same but with the older TWO-PASS scrolls (overlap_mode=0)"
PRINT "  3 = the old 0.06-style settings (strongly-ordered scratch, full zeroing, two-pass scrolls)"
PRINT "  4 = the 0.10 defaults with the V3D clock raised to the firmware's maximum (experiment)"
PRINT "  5 = BANDING (needs 0.12): the defaults, but ONE TILE ROW (64 pixel rows) per GPU job, so every"
PRINT "      rectangle taller than 64 rows is split into several jobs - the way a screen taller"
PRINT "      than 1088 rows is handled; fills, copies and scrolls all banded"
PRINT "  6 = BANDING with 2 tiles per job: bands of 2 tile rows for the narrowest rectangles, 1 row otherwise"
REPEAT
  k$ = GET$
UNTIL k$ = "1" OR k$ = "2" OR k$ = "3" OR k$ = "4" OR k$ = "5" OR k$ = "6"
IF (k$ = "5" OR k$ = "6") AND INSTR(help$, "0.12") = 0 THEN PRINT "Options 5 and 6 need V3DDriver 0.12 (V3D_AccelTune 9)." : END
SYS "V3D_AccelLimits", -1, -1, -1, -1, -1 TO lim%(0), lim%(1), lim%(2), lim%(3), lim%(4)
limsaved% = TRUE
tuned% = TRUE
SYS "V3D_AccelTune", 8, -1 TO clk0%, clkmax%
PRINT "V3D clock: "; clk0%; " MHz now, firmware maximum "; clkmax%; " MHz"
IF k$ = "1" THEN PROCapply(1, 1, 1) : PRINT "Testing: defaults (zero_mode=1, scratch_attr=1, overlap_mode=1 = one-pass scrolls)"
IF k$ = "2" THEN PROCapply(1, 1, 0) : PRINT "Testing: zero_mode=1, scratch_attr=1, overlap_mode=0 (two-pass scrolls)"
IF k$ = "3" THEN PROCapply(0, 0, 0) : PRINT "Testing: zero_mode=0, scratch_attr=0, overlap_mode=0 (0.06-style)"
IF k$ = "5" OR k$ = "6" THEN
  PROCapply(1, 1, 1)
  lim9% = 1 : IF k$ = "6" THEN lim9% = 2
  banded% = TRUE
  SYS "V3D_AccelTune", 9, lim9% TO oldmax%
  PRINT "Testing: defaults with a job limit of "; lim9%; " tile(s) (was "; oldmax%; "): big rectangles are BANDED"
ENDIF
IF k$ = "4" THEN
  PROCapply(1, 1, 1)
  SYS "V3D_AccelTune", 8, clkmax% TO clknow%
  PRINT "Testing: defaults with the V3D clock at "; clknow%; " MHz"
ENDIF
SYS "V3D_AccelLimits", 0, 0, 0, 0, 0
SYS "ColourTrans_SetGCOL", &F3774200,,,0,0

REM ---- screen layout: six 400x300 slots in the right-hand part of the screen
px0% = gwr% - 1230
IF px0% < gwl% + 4 THEN px0% = gwl% + 4
py0% = gwb% + 4
IF px0% + 1220 > gwr% OR py0% + 620 > gwt% THEN PRINT "The graphics window is too small for the test area (need 1220x620)." : PROCrestore : END

REM ---- sanity checks: do plots draw at all, and do they reach the GPU?
stage$ = "sanity"
PRINT
PRINT "Sanity check: does OS_Plot draw here?"
SYS "V3D_AccelStop"
PROCsentinel(px0%, py0%, 40, 40)
PROCfillrect(px0% + 4, py0% + 4, 20, 20)
sent% = &5A5A5A5A : IF bpp% = 2 THEN sent% = &5A5A
IF FNpixel(px0% + 10, py0% + 10) = sent% THEN
  PRINT "*** OS_Plot did NOT draw anything. If this was typed into a desktop Task Window,"
  PRINT "    that is the cause: TaskWindow swallows the plot bytes. Close it and double-click"
  PRINT "    the script in the Filer instead (a Run window), or use the command line (F12)."
  PRINT "    Now: window "; gwl%; ","; gwb%; " to "; gwr%; ","; gwt%
  PROCreadvdu
  PRINT "    Re-read: window "; gwl%; ","; gwb%; " to "; gwr%; ","; gwt%; "  origin "; orgx%; ","; orgy%
  PROCrestore
  END
ENDIF
PRINT "  yes - the CPU path draws."
SYS "V3D_AccelStart"
a0% = FNaccelcount(1)
PROCfillrect(px0% + 4, py0% + 40, 20, 20)
IF FNaccelcount(1) = a0% THEN
  PRINT "*** The fill drew but did NOT reach the GPU although the size limits are 0."
  SYS "V3D_AccelProfile", 6 TO serr%, smsg%
  IF serr% <> 0 THEN PRINT "    The driver declined this screen: "; $smsg%
  PROCrestore
  END
ENDIF
PRINT "  yes - and the GPU path takes it."

seed% = RND(-20261004)
nfill% = 0 : bfill% = 0 : ncopy% = 0 : bcopy% = 0 : novl% = 0 : bovl% = 0 : nskip% = 0 : nshown% = 0

REM ================= FILLS =================
stage$ = "fills"
PRINT
PRINT "Fills ("; NFILL%; " cases)..."
FOR c% = 1 TO NFILL%
  w% = FNsize(300) : h% = FNsize(200)
  xo% = RND(4) - 1 : yo% = RND(4) - 1
  col% = RND(&7FFFFFFF) << 8
  PROCfilltest(w%, h%, xo%, yo%, col%)
  IF c% MOD 25 = 0 THEN PRINT "  "; c%; " done, "; bfill%; " bad"
NEXT

REM ================= COPIES =================
stage$ = "copies"
PRINT
PRINT "Copies ("; NCOPY%; " cases)..."
FOR c% = 1 TO NCOPY%
  w% = FNsize(300) : h% = FNsize(200)
  sx% = RND(360 - w% + 1) - 1 : sy% = RND(260 - h% + 1) - 1
  xo% = RND(4) - 1 : yo% = RND(4) - 1
  PROCcopytest(w%, h%, sx%, sy%, xo%, yo%)
  IF c% MOD 25 = 0 THEN PRINT "  "; c%; " done, "; bcopy%; " bad"
NEXT

REM ================= OVERLAPPING COPIES =================
stage$ = "overlaps"
PRINT
PRINT "Overlapping copies / scrolls ("; NOVL%; " cases)..."
REM Count the GPU jobs the scrolls cost: one pass = one job per scroll, two-pass
REM scrolls cost two jobs each when the driver sees the rectangles overlap.
SYS "V3D_AccelProfile", 1, 1 TO jj%
SYS "V3D_AccelCopyStats" TO ,ov0%
FOR c% = 1 TO NOVL%
  w% = FNsize(200) : h% = FNsize(130)
  REM a shift of up to 70 either way, different from none, in each axis independently
  dx% = RND(141) - 71 : dy% = RND(141) - 71
  IF c% MOD 5 = 0 THEN dx% = 0 : dy% = RND(141) - 71
  IF c% MOD 7 = 0 THEN dy% = 0 : dx% = RND(141) - 71
  IF dx% = 0 AND dy% = 0 THEN dx% = 1
  PROCovltest(w%, h%, dx%, dy%)
  IF c% MOD 25 = 0 THEN PRINT "  "; c%; " done, "; bovl%; " bad"
NEXT

stage$ = "finish"
SYS "V3D_AccelProfile", 1, 0 TO jobs%
SYS "V3D_AccelCopyStats" TO ,ov1%
PROCrestore
PRINT
PRINT "Scrolls: "; NOVL%; " GPU scrolls cost "; jobs%; " GPU jobs; the driver saw "; ov1% - ov0%; " of them as overlapping."
PRINT "  (one-pass scrolls: jobs = "; NOVL%; "; two-pass: jobs = "; NOVL% + ov1% - ov0%; ")"
nbandfail% = 0
IF banded% THEN
  PRINT "  (banded: a scroll taller than its bands costs several jobs, so more jobs than scrolls is expected)"
  IF jobs% < NOVL% + NOVL% DIV 4 THEN
    PRINT "*** the scrolls were NOT split into bands as intended ("; jobs% - NOVL%; " extra jobs) - this run did not exercise the banding"
    nbandfail% = 1
  ENDIF
ENDIF
PRINT "RESULT fills "; nfill% - bfill%; "/"; nfill%; " ok;  copies "; ncopy% - bcopy%; "/"; ncopy%; " ok;  scrolls "; novl% - bovl%; "/"; novl%; " ok;  not accelerated "; nskip%
IF bfill% + bcopy% + bovl% + nskip% + nbandfail% = 0 THEN
  PRINT "ALL "; nfill% + ncopy% + novl%; " CASES IDENTICAL TO THE CPU."
ELSE
  IF bfill% + bcopy% + bovl% + nskip% = 0 THEN
    PRINT "The comparisons found no differences, but the banding was not exercised - do not count this run."
  ELSE
    PRINT "*** DIFFERENCES FOUND - see the FAIL lines above."
  ENDIF
ENDIF
END

DEF PROCfail
  ON ERROR OFF
  PRINT
  REPORT
  PRINT " at line "; ERL; " (stage: "; stage$; ")"
  PROCrestore
  END
ENDPROC

REM Apply a setting combination and re-claim the scratch buffers with it.
DEF PROCapply(z%, a%, o%)
  SYS "V3D_AccelTune", 0, z%
  SYS "V3D_AccelTune", 4, a%
  SYS "V3D_AccelTune", 7, o%
  SYS "V3D_AccelTune", 5, 0 TO rr%
ENDPROC

REM Back to the module as it was: 0.10 default tuning, default limits, accelerator on.
DEF PROCrestore
  ON ERROR OFF
  REM Limits first (cannot fail), then the fallible tuning and clock calls: after a GPU
  REM timeout the driver refuses them, and anything after a refusal would be skipped.
  IF limsaved% THEN SYS "V3D_AccelLimits", lim%(0), lim%(1), lim%(2), lim%(3), lim%(4)
  REM the job limit next: setting 9 never fails, and if it stayed at 6 after a timeout every
  REM real operation wider than 6 tiles would be refused until the next reboot
  IF banded% THEN SYS "V3D_AccelTune", 9, 510
  SYS "V3D_AccelStart"
  IF clk0% > 0 THEN SYS "V3D_AccelTune", 8, clk0%
  IF tuned% THEN PROCapply(1, 1, 1)
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

DEF PROCreadvdu
  vin%!0 = 4 : vin%!4 = 5 : vin%!8 = 9 : vin%!12 = 128 : vin%!16 = 129 : vin%!20 = 130 : vin%!24 = 131
  vin%!28 = 136 : vin%!32 = 137 : vin%!36 = 6 : vin%!40 = 12 : vin%!44 = 148 : vin%!48 = 11 : vin%!52 = -1
  SYS &31, vin%, vout%
  xeig% = vout%!0 : yeig% = vout%!4 : log2bpp% = vout%!8
  gwl% = vout%!12 : gwb% = vout%!16 : gwr% = vout%!20 : gwt% = vout%!24
  orgx% = vout%!28 : orgy% = vout%!32
  ll% = vout%!36 : ylim% = vout%!40 : ss% = vout%!44 : xlim% = vout%!48
  bpp% = (1 << log2bpp%) DIV 8
ENDPROC

REM A size biased towards the awkward values: half the time one of the tile-edge
REM sizes (clamped to the maximum), otherwise uniform 1..max.
DEF FNsize(max%)
  LOCAL v%
  IF RND(2) = 1 THEN
    v% = edge%(RND(22) - 1)
    IF v% > max% THEN v% = max%
  ELSE
    v% = RND(max%)
  ENDIF
= v%

DEF FNpixel(x%, y%)
  LOCAL a%
  a% = ss% + (ylim% - y%) * ll% + x% * bpp%
  IF bpp% = 4 THEN = !a%
= ?a% + 256 * a%?1

REM Fill a w% x h% area with a CPU-written sentinel, bypassing OS_Plot entirely.
DEF PROCsentinel(x%, y%, w%, h%)
  LOCAL r%, c%, a%
  FOR r% = 0 TO h% - 1
    a% = ss% + (ylim% - (y% + r%)) * ll% + x% * bpp%
    IF bpp% = 4 THEN
      FOR c% = 0 TO w% - 1
        a%!(c% * 4) = &5A5A5A5A
      NEXT
    ELSE
      FOR c% = 0 TO w% - 1
        a%?(c% * 2) = &5A : a%?(c% * 2 + 1) = &5A
      NEXT
    ENDIF
  NEXT
ENDPROC

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

REM Number of differing pixels between two areas; the first difference is left
REM in fx%/fy% (offset within the area) for the FAIL line.
DEF FNcmp(ax%, ay%, bx%, by%, w%, h%)
  LOCAL x%, y%, pa%, pb%, bad%
  bad% = 0 : fx% = -1 : fy% = -1
  FOR y% = 0 TO h% - 1
    pa% = ss% + (ylim% - (ay% + y%)) * ll% + ax% * bpp%
    pb% = ss% + (ylim% - (by% + y%)) * ll% + bx% * bpp%
    IF bpp% = 4 THEN
      FOR x% = 0 TO w% - 1
        IF pa%!(x% * 4) <> pb%!(x% * 4) THEN
          bad% += 1
          IF fx% < 0 THEN fx% = x% : fy% = y%
        ENDIF
      NEXT
    ELSE
      FOR x% = 0 TO w% - 1
        IF pa%?(x% * 2) <> pb%?(x% * 2) OR pa%?(x% * 2 + 1) <> pb%?(x% * 2 + 1) THEN
          bad% += 1
          IF fx% < 0 THEN fx% = x% : fy% = y%
        ENDIF
      NEXT
    ENDIF
  NEXT
= bad%

REM A random pattern of 14 coloured rectangles, generated once and then drawn
REM identically wherever it is needed.
DEF PROCgenpattern
  LOCAL k%
  FOR k% = 0 TO 13
    prw%(k%) = 10 + RND(120) : prh%(k%) = 10 + RND(90)
    prx%(k%) = RND(360 - prw%(k%)) - 1 : pry%(k%) = RND(260 - prh%(k%)) - 1
    prc%(k%) = RND(&7FFFFFFF) << 8
  NEXT
ENDPROC

REM Draws the pattern (CPU path) in a 400x300 slot whose bottom-left is (x%,y%); the
REM coloured rectangles themselves lie within its lower-left 360x260.
DEF PROCdrawpattern(x%, y%)
  LOCAL k%
  SYS "ColourTrans_SetGCOL", &C0C0C000,,,0,0
  PROCfillrect(x%, y%, 400, 300)
  FOR k% = 0 TO 13
    SYS "ColourTrans_SetGCOL", prc%(k%),,,0,0
    PROCfillrect(x% + prx%(k%), y% + pry%(k%), prw%(k%), prh%(k%))
  NEXT
ENDPROC

DEF PROCreport(kind$, detail$, bad%)
  IF nshown% < 25 THEN
    PRINT "FAIL "; kind$; " "; detail$; ": "; bad%; " pixels differ, first at +"; fx%; ","; fy%
    nshown% += 1
  ENDIF
ENDPROC

REM ---- one fill: CPU reference in slot 1, GPU in slot 2, padded by 8 pixels
DEF PROCfilltest(w%, h%, xo%, yo%, col%)
  LOCAL ax%, ay%, bx%, by%, a0%, bad%
  ax% = px0% : ay% = py0% : bx% = px0% + 420 : by% = py0%
  SYS "V3D_AccelStop"
  PROCsentinel(ax%, ay%, w% + 16 + xo%, h% + 16 + yo%)
  PROCsentinel(bx%, by%, w% + 16 + xo%, h% + 16 + yo%)
  SYS "ColourTrans_SetGCOL", col%,,,0,0
  PROCfillrect(ax% + 8 + xo%, ay% + 8 + yo%, w%, h%)
  SYS "V3D_AccelStart"
  a0% = FNaccelcount(1)
  PROCfillrect(bx% + 8 + xo%, by% + 8 + yo%, w%, h%)
  nfill% += 1
  IF FNaccelcount(1) = a0% THEN
    nskip% += 1
    IF nshown% < 25 THEN PRINT "NOT ACCELERATED fill "; w%; "x"; h%; " x+"; xo% : nshown% += 1
  ELSE
    bad% = FNcmp(ax%, ay%, bx%, by%, w% + 16 + xo%, h% + 16 + yo%)
    IF bad% <> 0 THEN bfill% += 1 : PROCreport("fill", STR$(w%) + "x" + STR$(h%) + " xo=" + STR$(xo%) + " yo=" + STR$(yo%), bad%)
  ENDIF
ENDPROC

REM ---- one non-overlapping copy: pattern in slot 3, CPU copy to slot 4, GPU copy to slot 5
DEF PROCcopytest(w%, h%, sx%, sy%, xo%, yo%)
  LOCAL px%, py%, ax%, ay%, bx%, by%, a0%, bad%
  px% = px0% + 840 : py% = py0%
  ax% = px0% : ay% = py0% + 320 : bx% = px0% + 420 : by% = py0% + 320
  SYS "V3D_AccelStop"
  PROCgenpattern
  PROCdrawpattern(px%, py%)
  PROCsentinel(ax%, ay%, w% + 16 + xo%, h% + 16 + yo%)
  PROCsentinel(bx%, by%, w% + 16 + xo%, h% + 16 + yo%)
  PROCcopyrect(px% + sx%, py% + sy%, w%, h%, ax% + 8 + xo%, ay% + 8 + yo%)
  SYS "V3D_AccelStart"
  a0% = FNaccelcount(2)
  PROCcopyrect(px% + sx%, py% + sy%, w%, h%, bx% + 8 + xo%, by% + 8 + yo%)
  ncopy% += 1
  IF FNaccelcount(2) = a0% THEN
    nskip% += 1
    IF nshown% < 25 THEN PRINT "NOT ACCELERATED copy "; w%; "x"; h% : nshown% += 1
  ELSE
    bad% = FNcmp(ax%, ay%, bx%, by%, w% + 16 + xo%, h% + 16 + yo%)
    IF bad% <> 0 THEN bcopy% += 1 : PROCreport("copy", STR$(w%) + "x" + STR$(h%) + " from " + STR$(sx%) + "," + STR$(sy%) + " xo=" + STR$(xo%) + " yo=" + STR$(yo%), bad%)
  ENDIF
ENDPROC

REM ---- one scroll: identical pattern in slots 1 and 2, CPU scroll in slot 1, GPU scroll in slot 2
DEF PROCovltest(w%, h%, dx%, dy%)
  LOCAL ax%, ay%, bx%, by%, ox%, oy%, a0%, bad%, cw%, ch%, cx%, cy%
  ax% = px0% : ay% = py0% : bx% = px0% + 420 : by% = py0%
  ox% = 80 : oy% = 80
  SYS "V3D_AccelStop"
  PROCgenpattern
  PROCdrawpattern(ax%, ay%)
  PROCdrawpattern(bx%, by%)
  PROCcopyrect(ax% + ox%, ay% + oy%, w%, h%, ax% + ox% + dx%, ay% + oy% + dy%)
  SYS "V3D_AccelStart"
  a0% = FNaccelcount(3)
  PROCcopyrect(bx% + ox%, by% + oy%, w%, h%, bx% + ox% + dx%, by% + oy% + dy%)
  novl% += 1
  IF FNaccelcount(3) = a0% THEN
    nskip% += 1
    IF nshown% < 25 THEN PRINT "NOT ACCELERATED scroll "; w%; "x"; h%; " by "; dx%; ","; dy% : nshown% += 1
  ELSE
    REM compare the box covering source and destination, plus an 8 pixel margin
    cx% = ox% - 8 : IF dx% < 0 THEN cx% = ox% + dx% - 8
    cy% = oy% - 8 : IF dy% < 0 THEN cy% = oy% + dy% - 8
    cw% = w% + ABS(dx%) + 16 : ch% = h% + ABS(dy%) + 16
    bad% = FNcmp(ax% + cx%, ay% + cy%, bx% + cx%, by% + cy%, cw%, ch%)
    IF bad% <> 0 THEN bovl% += 1 : PROCreport("scroll", STR$(w%) + "x" + STR$(h%) + " by " + STR$(dx%) + "," + STR$(dy%), bad%)
  ENDIF
ENDPROC
