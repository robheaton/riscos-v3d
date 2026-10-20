REM > ReloadFreezeDiag
REM Diagnostic for the still-unresolved reload-without-reboot freeze
REM (see README.md's "Known limitations" and "Real IRQ handling"
REM sections). Four fix attempts have already been tried and ruled
REM out (dangling IRQ vector claim, reset-pulsing an already-warm
REM core, leaked PMP Dynamic Area pages, and a diagnostic bisection
REM build) - do NOT re-suggest any of those four as a fix; this
REM script exists to gather NEW evidence instead, not to test another
REM guess.
REM
REM The reproducible signature (from the README's own history): after
REM a reload (no intervening reboot), StressTest.bas gets through
REM PowerOn -> ClearTest -> ClearToSprite (iteration 1) and PowerOn
REM (iteration 2) cleanly, then freezes on the SECOND ClearTest call -
REM consistently, across every fix combination tried. This is a HARD
REM MACHINE FREEZE, not a SWI timeout, so cl_clear.c's own
REM diagnose_timeout() never gets a chance to run - nobody has actually
REM seen V3D register state in the moments leading up to this freeze.
REM
REM This script calls the new V3D_RegSnapshot SWI (read-only, no
REM V3D_PowerOn required) and PRINTs the register values BEFORE every
REM risky call, not just on failure. If the machine freezes again,
REM whatever printed last on screen is real forensic data - the exact
REM hardware state immediately before whatever goes wrong - rather
REM than another guess about C-level resource leaks.
REM
REM EXACT REPRO STEPS:
REM   1. Make sure some real GPU work has already happened THIS boot
REM      (e.g. you've already run AccelTest.bas/AutoStartTest.bas, or
REM      just used the desktop normally with the accelerator active -
REM      auto-start means this happens automatically once the module
REM      is loaded and you interact with the desktop at all).
REM   2. *RMKill the module, then *RMLoad it again - NO reboot.
REM   3. Run this script (*ReloadFreezeDiag).
REM   4. If it freezes, note the LAST few lines printed on screen
REM      before it happened - that's the data this exists to capture.

i% = 0
ON ERROR PRINT "FAIL at iteration "; i%; ": "; REPORT$; " (error &"; STR$~(ERR); ")" : END

N% = 5

FOR i% = 1 TO N%
  PROCsnapshot("before PowerOn, iter " + STR$(i%))
  SYS "V3D_PowerOn" TO ok%, rate%
  IF ok% <> 1 THEN PRINT "FAIL at iteration "; i%; ": V3D_PowerOn returned ok="; ok% : END

  colour% = (i% * &01010101) OR &FF000000

  PROCsnapshot("before ClearTest, iter " + STR$(i%))
  SYS "V3D_ClearTest", colour% TO phys%, readback%, bin_irq%, render_irq%
  IF readback% <> colour% THEN PRINT "FAIL at iteration "; i%; " (ClearTest): wanted "; ~colour%; " got "; ~readback% : END

  PROCsnapshot("before ClearToSprite, iter " + STR$(i%))
  SYS "V3D_ClearToSprite", colour% TO sprite%, readback2%, physaddr%, sprite_irq%
  IF readback2% <> colour% THEN PRINT "FAIL at iteration "; i%; " (ClearToSprite): wanted "; ~colour%; " got "; ~readback2% : END

  PRINT "iter "; i%; " OK"
NEXT i%

PRINT
PRINT N%; " iterations completed with no freeze."
END

DEF PROCsnapshot(label$)
  SYS "V3D_RegSnapshot" TO intsts%, hubsts%, ct0cs%, ct1cs%, ct0ca%, ct0ea%, ct1ca%, ct1ea%, asbm%, asbs%
  PRINT label$; ": INTSTS="; ~intsts%; " HUBSTS="; ~hubsts%; " CT0CS="; ~ct0cs%; " CT1CS="; ~ct1cs%
  PRINT "  CT0CA="; ~ct0ca%; " CT0EA="; ~ct0ea%; " CT1CA="; ~ct1ca%; " CT1EA="; ~ct1ea%
  REM ASBM/ASBS bit0=REQ_STOP, bit1=ACK - see module.c's V3D_RegSnapshot
  REM comment for why these were added (2026-09-28 reload-freeze forensics)
  PRINT "  ASBM="; ~asbm%; " ASBS="; ~asbs%
ENDPROC
