REM > PlotCensus
REM How much fill and copy work does real use send through GraphicsV, how big is it, and who did it?
REM (V3DDriver 0.12 rev f or later.) The driver counts every rectangle fill, copy and scroll (overlapping copy) by
REM size, and whether the GPU took it or it was left to the CPU. This script starts a recording and prints it.
REM
REM How to use it:
REM   1. Double-click it and type R, Enter. It zeroes the counters and starts the clock.
REM   2. Use the machine as you normally would for a few minutes (browsing, editing, dragging and scrolling windows).
REM   3. Double-click it again and just press Enter. It prints the census. You can print it again later: the
REM      counters keep running until the next R.
REM It only reads counters and zeroes them on R; it never changes how the driver works.

ON ERROR PROCfail

PROCmodinfo
PRINT "PlotCensus - module: "; help$
IF INSTR(help$, "rev f") = 0 THEN PRINT "This needs V3DDriver 0.12 rev f or later." : END
PRINT
INPUT "Type R and Enter to start a new recording, or just Enter to print the census: " a$
IF a$ = "R" OR a$ = "r" THEN
  SYS "V3D_AccelProfile", 10, -1
  PRINT "Recording started. Use the machine normally, then run this again and press Enter."
  END
ENDIF

DIM nm$(2), tg%(2), tc%(2), kg%(2), kc%(2)
nm$(0) = "Fills"
nm$(1) = "Copies (not overlapping)"
nm$(2) = "Scrolls (overlapping copies)"

SYS "V3D_AccelProfile", 10, 0, 0 TO r0%, r1%, r2%, r3%, el%
secs = (el% AND &7FFFFFFF) / 1000000
IF el% < 0 THEN secs = secs + 2147.483648
s% = INT(secs)
PRINT "Recorded for "; s% DIV 60; " min "; s% MOD 60; " s"
IF secs > 4200 THEN PRINT "(the clock wraps at about 71 minutes: the time above may be wrong)"
PRINT

FOR k% = 0 TO 2
  PRINT nm$(k%); ":"
  PRINT "  area up to     GPU calls   CPU calls   GPU kpix   CPU kpix"
  FOR b% = 0 TO 9
    SYS "V3D_AccelProfile", 10, k%, b% TO g%, c%, gk%, ck%
    IF g% + c% > 0 THEN PROCrow(b%, g%, c%, gk%, ck%)
    tg%(k%) += g% : tc%(k%) += c% : kg%(k%) += gk% : kc%(k%) += ck%
  NEXT
  PRINT "  total        ";
  PROCnum(tg%(k%), 9) : PROCnum(tc%(k%), 12) : PROCnum(kg%(k%), 11) : PROCnum(kc%(k%), 11)
  PRINT
  PRINT
NEXT

totc% = 0 : totp% = 0 : gp% = 0
FOR k% = 0 TO 2
  totc% += tg%(k%) + tc%(k%)
  totp% += kg%(k%) + kc%(k%)
  gp% += kg%(k%)
NEXT
PRINT "All: "; totc%; " calls, "; totp%; " kilopixels, of which the GPU did "; gp%; " kilopixels"
IF secs > 0 THEN PRINT "Per second: "; INT(totc% / secs); " calls, "; INT(totp% / secs); " kilopixels"
PRINT
SYS "V3D_AccelProfile", 0, 0 TO fn%, f1%, f2%, f3%, f4%, ft%
SYS "V3D_AccelProfile", 1, 0 TO cn%, c1%, c2%, c3%, c4%, ct%
PRINT "GPU job time (microseconds, includes setup and waiting): fills "; fn%; " jobs, "; ft%; " us; copies "; cn%; " jobs, "; ct%; " us"
END

DEF PROCrow(b%, g%, c%, gk%, ck%)
  LOCAL p%
  p% = b% * 10 + 1
  PRINT "  "; MID$("64 px     256 px    1K px     4K px     16K px    64K px    256K px   1M px     4M px     over 4M   ", p%, 10); "   ";
  PROCnum(g%, 9) : PROCnum(c%, 12) : PROCnum(gk%, 11) : PROCnum(ck%, 11)
  PRINT
ENDPROC

DEF PROCnum(n%, w%)
  LOCAL s$
  s$ = STR$(n%)
  IF LEN(s$) < w% THEN s$ = STRING$(w% - LEN(s$), " ") + s$
  PRINT s$;
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
