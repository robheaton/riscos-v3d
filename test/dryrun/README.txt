Dry-running the BASIC test scripts without a RISC OS machine
============================================================

A bug in a test script costs a reboot-and-test cycle on the real hardware, so
every script is run first under Matrix Brandy with the hardware calls stubbed.
It validates syntax, arrays, control flow, the pixel-verification logic and the
output formatting; it cannot validate the hardware calls themselves.

Setup (no root, nothing installed system-wide):

    mkdir brandy && cd brandy
    apt-get download brandy && dpkg-deb -x brandy_*.deb root

Generate and run (from this directory, with Brandy at ./brandy/root):

    python3 mkstub.py 0 ../TuneBench.bas stub_tune.bas     # 0 = GPU emulation correct, 1 = corrupt a pixel
    printf '1\n 1 ' | brandy/root/usr/bin/sbrandy -size 32M -quit stub_tune.bas

    python3 mkstub.py 0 ../AccelTest.bas stub_accel.bas
    printf ' ' | brandy/root/usr/bin/sbrandy -size 32M -quit stub_accel.bas

    python3 mkstub_mode.py 0            # ModeChangeTest; 1 = the fake clear misses 40 rows; 2 = one 64x64 tile missed;
                                        # 3 = screen declined; 4 = plots never reach the driver; 5 = a screen over 1088
                                        # rows loses everything below row 1088 (a dropped band - must FAIL)
    brandy/root/usr/bin/sbrandy -size 64M -quit stub_mode.bas

mkstub*.py turn every `SYS` into a call to a stub procedure, emulate OS_Plot
fill/copy on a fake screen (so the GPU-vs-CPU comparison really runs), and give
any SWI they do not know a generic stub. Pass 1 as the corruption flag to prove
the verification step actually catches wrong pixels.

Gotchas found this way: `HEX$` does not exist in BASIC V (use `STR$~`); do not
use user-defined string functions (`DEF FNxxx$`); `ON ERROR LOCAL` needs a
`LOCAL ERROR` first; keywords are upper-case only, so lower-case names such as
`nrun%` and `wcount%` are safe.

Line length: real BASIC V refuses to load a text file that has a line longer than about
250 characters ("Line too long") - Brandy does not mind, so the dry run cannot see it.
Check every script before it goes to the NAS:

    awk 'length($0) > 150 { print FILENAME ": " NR ": " length($0) }' ../*.bas

and split any PRINT that long into several PRINT statements ending in ";" (they print
on one output line). ModeChangeTest v3 first went out with a 259-character PRINT and
failed to load on the Pi for this reason.
