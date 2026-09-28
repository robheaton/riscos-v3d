REM > BenchmarkTest
REM Proper GPU-vs-CPU throughput benchmark for the V3DDriver GraphicsV
REM Render accelerator (see README.md). Deliberately does NOT rely on
REM live Wimp desktop redraw timing (too variable/unrepeatable) -
REM instead drives the same underlying mechanism directly and
REM repeatably via V3D_ScreenFillTest/V3D_ScreenCopyTest (writes
REM straight to the real screen framestore, the same v3d_fill_screen/
REM v3d_copy_screen core the live GraphicsV handler itself uses once
REM V3D_AccelStart is claimed), timed against genuine equivalent CPU
REM software rendering (BASIC's own RECTANGLE FILL / PLOT 190 rectangle
REM copy - the same VDU/OS_Plot mechanism the Wimp itself falls back to
REM when this driver doesn't claim a call).
REM
REM PLOT 190 (&BE) is the real, PRM-documented "Move rectangle" plot
REM action in COPY mode (bit1 set) - confirmed directly against RISC
REM OS's own kernel VDU driver source (Kernel/s/vdu/vdugrafd,
REM BlockCopyMove routine), not guessed from memory.
REM
REM This writes directly to the real screen repeatedly - expect visible
REM flicker/redrawing in the benchmark rectangle area while it runs.
REM That's expected, not a bug (see ScreenFillTest.bas/ScreenCopyTest.bas
REM for the same caution) - the desktop will look normal again once the
REM Wimp's own next redraw happens.
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK (clock rate="; rate%; " Hz)"
PRINT

REM alpha=FF, arbitrary distinctive colour for the fill benchmark
fill_colour% = &FFE010B0
gcol_r% = 224 : gcol_g% = 16 : gcol_b% = 176

iterations% = 20

PRINT "=== Fill benchmark (GPU V3D_ScreenFillTest vs CPU RECTANGLE FILL) ==="
PRINT "Size            GPU ms/op    CPU ms/op    Speedup"
PROCfillbench(64, 64)
PROCfillbench(256, 256)
PROCfillbench(800, 600)
PRINT

PRINT "=== Copy benchmark (GPU V3D_ScreenCopyTest vs CPU PLOT 190) ==="
PRINT "Size            GPU ms/op    CPU ms/op    Speedup"
PROCcopybench(64, 64)
PROCcopybench(256, 256)
PROCcopybench(800, 300)
PRINT

PRINT "Benchmark complete."
END

DEF PROCfillbench(width%, height%)
  x% = 50 : y% = 50
  t0% = TIME
  FOR i% = 1 TO iterations%
    SYS "V3D_ScreenFillTest", x%, y%, width%, height%, fill_colour%
  NEXT i%
  gpu_ms% = ((TIME - t0%) * 10) DIV iterations%

  t0% = TIME
  GCOL 0, gcol_r%, gcol_g%, gcol_b%
  FOR i% = 1 TO iterations%
    RECTANGLE FILL x%, y%, width%, height%
  NEXT i%
  cpu_ms% = ((TIME - t0%) * 10) DIV iterations%

  PROCprintrow(width%, height%, gpu_ms%, cpu_ms%)
ENDPROC

DEF PROCcopybench(width%, height%)
  src_x% = 50 : src_y% = 50
  dest_x% = src_x% + width% + 50 : dest_y% = src_y%

  t0% = TIME
  FOR i% = 1 TO iterations%
    SYS "V3D_ScreenCopyTest", src_x%, src_y%, dest_x%, dest_y%, width%, height%
  NEXT i%
  gpu_ms% = ((TIME - t0%) * 10) DIV iterations%

  t0% = TIME
  FOR i% = 1 TO iterations%
    MOVE src_x%, src_y%
    MOVE src_x% + width%, src_y% + height%
    PLOT 190, dest_x%, dest_y%
  NEXT i%
  cpu_ms% = ((TIME - t0%) * 10) DIV iterations%

  PROCprintrow(width%, height%, gpu_ms%, cpu_ms%)
ENDPROC

DEF PROCprintrow(width%, height%, gpu_ms%, cpu_ms%)
  size$ = STR$(width%) + "x" + STR$(height%)
  PRINT size$; STRING$(16 - LEN(size$), " ");
  PRINT gpu_ms%; STRING$(13 - LEN(STR$(gpu_ms%)), " ");
  PRINT cpu_ms%;
  IF gpu_ms% > 0 THEN PRINT STRING$(13 - LEN(STR$(cpu_ms%)), " "); (cpu_ms% / gpu_ms%); "x" ELSE PRINT "  (GPU too fast to measure at cs resolution)"
ENDPROC
