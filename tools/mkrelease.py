#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""
mkrelease.py - make the RISC OS release zip of a built module, the way zip tools on RISC OS do.

    tools/mkrelease.py OUTPUT.zip MODULE [--toolchain TEXT]

Names in the zip are plain ("V3DDriver", not "V3DDriver,ffa"); the RISC OS file type, load and exec addresses and
attributes travel in Acorn's "ARC0" extra field (header id 0x4341) that SparkFS and Info-ZIP on RISC OS write and
read, so any RISC OS unzip puts the file types back. The zip holds the module, a ReadMe (the README, with a line
saying which tool chain built the module), the licence and notices, and the test scripts in Tests.
Run it from the top of the repository. Python's zipfile only; no zip(1).
"""
import argparse, os, struct, sys, time, zipfile

ARC0_TAG = 0x4341
RISCOS_SYSTEM = 13
EPOCH_1900_TO_1970 = 2208988800
TESTS = ["VerifySuite", "GpuCheck", "FillCopyBench", "ModeChangeTest", "TuneBench", "TraceDump", "StressTest",
         "PlotCensus"]


def arc0(filetype, ro_time, attr=0x13):
    load = 0xFFF00000 | (filetype << 8) | (ro_time >> 32)
    return struct.pack("<HH4sIII4x", ARC0_TAG, 20, b"ARC0", load, ro_time & 0xFFFFFFFF, attr)


def entry(name, filetype, is_dir, now):
    ro_time = int((now + EPOCH_1900_TO_1970) * 100)
    t = time.localtime(now)
    info = zipfile.ZipInfo(name, (t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec & ~1))
    info.create_system = RISCOS_SYSTEM
    info.extra = arc0(filetype, ro_time)
    if is_dir:
        info.compress_type = zipfile.ZIP_STORED
        info.external_attr = 0x41ED0010
    else:
        info.compress_type = zipfile.ZIP_DEFLATED
        info.external_attr = 0x81A40000
    return info


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("module")
    ap.add_argument("--toolchain", default="")
    a = ap.parse_args()
    now = time.time()
    readme = open("README.md", "rb").read()
    if a.toolchain:
        readme = (b"Built with: " + a.toolchain.encode() + b"\n\n") + readme
    with zipfile.ZipFile(a.out, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr(entry("V3DDriver/", 0xFFD, True, now), b"")
        z.writestr(entry("V3DDriver/V3DDriver", 0xFFA, False, now), open(a.module, "rb").read())
        z.writestr(entry("V3DDriver/ReadMe", 0xFFF, False, now), readme)
        z.writestr(entry("V3DDriver/Licence", 0xFFF, False, now), open("LICENSE", "rb").read())
        z.writestr(entry("V3DDriver/NOTICE", 0xFFF, False, now), open("NOTICE", "rb").read())
        z.writestr(entry("V3DDriver/Tests/", 0xFFD, True, now), b"")
        for t in TESTS:
            z.writestr(entry("V3DDriver/Tests/" + t, 0xFFB, False, now), open("test/%s.bas" % t, "rb").read())
    print("wrote", a.out)


if __name__ == "__main__":
    main()
