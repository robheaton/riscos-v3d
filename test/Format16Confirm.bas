REM > Format16Confirm
REM Follow-up to Format16Test.bas: confirms the real finding from that
REM run - the clear-colour input is ALWAYS 8-bit-per-channel RGBA
REM (R=byte0/G=byte1/B=byte2/A=byte3), regardless of the target
REM format, with the hardware automatically down-converting each
REM channel to the target's real bit depth. Format 45 (RGBA5551) was
REM found to place channels at the SAME bit positions RISC OS's own
REM RGB555 screen format uses (R bits0-4, G bits5-9, B bits10-14).
REM
REM This sends PROPER 8-bit-per-channel pure R/G/B (0xFF in one byte,
REM 0x00 in the others) - not the pre-packed 5:5:5:1 values the first
REM test used - and predicts an EXACT readback match to RISC OS's own
REM native pure-channel values (0x001F/0x03E0/0x7C00) if the theory is
REM right.
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.

SYS "V3D_PowerOn" TO ok%, rate%
IF ok% <> 1 THEN PRINT "FAIL: V3D_PowerOn returned ok="; ok% : END
PRINT "V3D_PowerOn OK"
PRINT

PROCtest(&000000FF, &001F, "red  ")
PROCtest(&0000FF00, &03E0, "green")
PROCtest(&00FF0000, &7C00, "blue ")

END

DEF PROCtest(colour8%, predicted%, name$)
  SYS "V3D_16bppFormatTest", 45, colour8% TO phys%, readback%
  IF readback% = predicted% THEN result$ = "CONFIRMED" ELSE result$ = "unexpected"
  PRINT name$; ": sent="; ~colour8%; " predicted="; ~predicted%; " readback="; ~readback%; "  "; result$
ENDPROC
