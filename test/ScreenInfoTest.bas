REM > ScreenInfoTest
REM Exercises V3D_ScreenInfo - step 1 of the planned GraphicsV Render
REM accelerator (see README.md). Purely read-only: no V3D_PowerOn
REM needed first, no hardware touched. Prints the OS's own live screen
REM mode state and sanity-checks it against BASIC's own *DISPLAYMODE-style
REM view where an obvious cross-check is available.
REM
REM Before running: *RMLoad the built V3DDriver,ffa yourself.

SYS "V3D_ScreenInfo" TO mode_flags%, log2bpp%, line_length%, xwind%, ywind%, screen_start%, display_start%, total_size%

PRINT "ModeFlags:       "; ~mode_flags%
PRINT "Log2BPP:         "; log2bpp%; " ("; (2^log2bpp%); " bits/pixel)"
PRINT "LineLength:      "; line_length%; " bytes/row"
PRINT "XWindLimit:      "; xwind%; " (width = "; xwind%+1; " px)"
PRINT "YWindLimit:      "; ywind%; " (height = "; ywind%+1; " px)"
PRINT "ScreenStart:     "; ~screen_start%
PRINT "DisplayStart:    "; ~display_start%
PRINT "TotalScreenSize: "; ~total_size%; " ("; total_size%; " bytes)"
PRINT

REM Cross-check: LineLength * (width in px) should be >= width*bytes-per-pixel,
REM and for the common case (no padding) exactly equal.
bytes_per_pixel% = 2^log2bpp% / 8
expected_min% = (xwind%+1) * bytes_per_pixel%
PRINT "Expected minimum LineLength for "; (xwind%+1); " px @ "; bytes_per_pixel%; " bytes/px: "; expected_min%
IF line_length% >= expected_min% THEN PRINT "OK - LineLength covers the visible width." ELSE PRINT "UNEXPECTED - LineLength is narrower than the visible width!"

REM Cross-check: TotalScreenSize should be >= the visible area's own
REM size (LineLength * visible height) - it can be larger (e.g. extra
REM banks for hardware double-buffering) but not smaller.
visible_bytes% = line_length% * (ywind%+1)
PRINT "Visible area (LineLength x height): "; visible_bytes%; " bytes"
IF total_size% >= visible_bytes% THEN PRINT "OK - TotalScreenSize covers the visible area." ELSE PRINT "UNEXPECTED - TotalScreenSize is SMALLER than the visible area!"
END
