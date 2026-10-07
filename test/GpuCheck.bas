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
