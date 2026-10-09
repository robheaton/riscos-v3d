REM > GpuCheck
REM Did the V3D driver actually accelerate anything?
REM
REM It only READS the driver's counters - it never starts or stops anything - so it is
REM safe to run at any time. The counts are everything since the module was loaded, so
REM run it right after the test you care about (reboot, load the module, run the test,
REM then run this). It works with V3DDriver 0.06 and later.
REM
REM Run it by double-clicking it in the Filer, or from a Task Window (*BASIC, then
REM CHAIN "<path>"); it does not draw anything.

ON ERROR PROCfail

PROCmodinfo
PRINT "GpuCheck - module: "; help$
PRINT

SYS "V3D_AccelStats" TO calls%, renders%, fillops%, solid%, fclaimed%, ffailed%
SYS "V3D_AccelCopyStats" TO copyops%, ovlops%, cdirect%, cdfail%, covl%, covlfail%

PRINT "GraphicsV calls seen:        "; calls%
PRINT "  Render calls:              "; renders%
PRINT
PRINT "Fills seen:                  "; fillops%
PRINT "  plain-colour fills:        "; solid%
PRINT "  done by the GPU:           "; fclaimed%
PRINT "  tried but failed:          "; ffailed%
PRINT
PRINT "Copies seen:                 "; copyops%
PRINT "  overlapping (scrolls):     "; ovlops%
PRINT "  copies done by the GPU:    "; cdirect%
PRINT "  copies that failed:        "; cdfail%
PRINT "  scrolls done by the GPU:   "; covl%
PRINT "  scrolls that failed:       "; covlfail%

REM V3D_AccelLimits (skipped counts) and V3D_AccelProfile (health) came in with 0.07, so
REM they are only asked of a newer module - no error trapping is needed.
IF INSTR(help$, "0.06") = 0 THEN
  SYS "V3D_AccelLimits", -1, -1, -1, -1, -1 TO l0%, l1%, l2%, l3%, l4%, skipf%, skipc%
  PRINT
  PRINT "Left to the CPU as too small: "; skipf%; " fills, "; skipc%; " copies"
  SYS "V3D_AccelProfile", 2 TO w%, t%, h2%, h3%, h4%, h5%, h6%, resc%
  PRINT
  IF w% = 0 THEN PRINT "GPU health: fine (no fail-stop), "; t%; " job timeouts, "; resc%; " lost interrupts rescued"
  IF w% <> 0 THEN PRINT "GPU health: *** the driver switched the GPU OFF after a timeout ("; t%; " timeouts) ***"
ELSE
  PRINT
  PRINT "(This is an older module: it does not report the too-small counts or the GPU health.)"
ENDIF

REM Interrupt accounting, screen declines and the details of any lost-interrupt rescue (0.11 and later).
IF INSTR(help$, "0.11") <> 0 OR INSTR(help$, "0.12") <> 0 THEN
  SYS "V3D_AccelProfile", 7 TO i0%, i1%, i2%, i3%, i4%, i5%, i6%, i7%
  PRINT
  PRINT "Interrupts: "; i3%; " bin and "; i4%; " render seen for "; i5%; " jobs started; empty interrupts: "; i2%
  PRINT "  not accounted for: "; i5% - i3%; " bin, "; i5% - i4%; " render (0 or 1 is normal; more means interrupts were lost)"
  IF i6% = 0 THEN
    PRINT "Screen declined for a fill or copy: never"
  ELSE
    PRINT "*** Screen declined for a fill or copy "; i6%; " time(s), last error number "; i7%
  ENDIF
  IF i0% > 0 THEN
    n% = i0% : IF n% > 4 THEN n% = 4
    PRINT "Lost-interrupt rescues so far: "; i0%; " (details of the first "; n%; "):"
    FOR k% = 0 TO n% - 1
      SYS "V3D_AccelProfile", 8, k% TO a%, b%, c%, d%, e%, f%, g%, h%
      w$ = "render" : IF ((a% >> 8) AND 255) = 1 THEN w$ = "bin"
      t$ = "fill" : IF ((a% >> 16) AND 255) <> 0 THEN t$ = "copy"
      late$ = "no" : IF ((g% >> 16) AND 1) = 1 THEN late$ = "YES"
      PRINT "  rescue "; k% + 1; ": waited "; b%; " us for the "; w$; " job of a "; t$
      PRINT "    INT_STS=&"; STR$~(c%); " INT_MSK=&"; STR$~(d%); " hub=&"; STR$~(e%); " thread CS=&"; STR$~(f%)
      PRINT "    frame counter moved by "; g% AND &FFFF; "; the interrupt arrived late anyway: "; late$
      PRINT "    jobs minus interrupts then: bin "; h% AND &FFFF; ", render "; (h% >> 16) AND &FFFF
    NEXT
  ENDIF
ENDIF

PRINT
total% = fclaimed% + cdirect% + covl%
IF total% = 0 THEN
  PRINT "RESULT: the GPU did NOT accelerate anything. This run was not using the GPU."
ELSE
  PRINT "RESULT: the GPU accelerated "; total%; " operations ("; fclaimed%; " fills, "; cdirect%; " copies, "; covl%; " scrolls)."
  IF ffailed% + cdfail% + covlfail% <> 0 THEN PRINT "  Some operations failed and were done by the CPU instead - see above."
ENDIF
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
