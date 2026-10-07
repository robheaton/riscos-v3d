import os
import re, sys
SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'ModeChangeTest.bas')
BUG = sys.argv[1] if len(sys.argv) > 1 else '0'
src = open(SRC).read().split('\n')
out = []
used = set()

def split_top(s):
    parts, depth, cur = [], 0, ''
    for ch in s:
        if ch == '(': depth += 1
        if ch == ')': depth -= 1
        if ch == ',' and depth == 0:
            parts.append(cur); cur = ''
        else: cur += ch
    parts.append(cur)
    return parts

for line in src:
    mm = re.match(r'^(\s*)MODE\s+(\w+\$)\s*$', line)
    if mm:
        out.append('%sPROCstub_mode(%s)' % (mm.group(1), mm.group(2)))
        continue
    if re.match(r'^\s*VDU 23,', line):
        out.append('REM ' + line.strip())
        continue
    mr = re.match(r'^(\s*)RECTANGLE FILL (.*)$', line)
    if mr:
        a = [x.strip() for x in split_top(mr.group(2))]
        out.append('%sPROCstub_rectfill(%s)' % (mr.group(1), ','.join(a[:4])))
        continue
    mv = re.match(r'^(\s*)VDU 25, ?(\d+), ?(\w+%); ?(\w+%);\s*$', line)
    if mv:
        out.append('%sPROCsys_plot(%s,%s,%s)' % (mv.group(1), mv.group(2), mv.group(3), mv.group(4)))
        continue
    pre_if = ''
    mi = re.match(r'^(\s*IF .* THEN )(SYS\s.*)$', line)
    if mi:
        pre_if, line = mi.group(1), mi.group(2)
    m = re.match(r'^(\s*)SYS\s+(\"[A-Za-z_0-9]+\"|&[0-9A-Fa-f]+)\s*(.*)$', line)
    if not m:
        out.append(line)
        continue
    ind, name, rest = m.groups()
    outs = ''
    mt = re.search(r'(^|\s)TO\s(.*)$', rest)
    if mt:
        outs = mt.group(2); rest = rest[:mt.start()]
    args = rest.strip()
    if args.startswith(','): args = args[1:]
    arglist = [a.strip() for a in split_top(args)] if args.strip() else []
    arglist = [a if a else '0' for a in arglist]
    while len(arglist) < 5: arglist.append('0')
    if name == '"OS_Module"': pname = 'PROCsys_osmodule'; arglist = []
    elif name == '&31': pname = 'PROCsys_readvdu'; arglist = []
    elif name == '&45': pname = 'PROCsys_plot'; arglist = arglist[:3]
    else: pname = 'PROCsys_' + name.strip('"')
    used.add(pname)
    call = pname + ('(' + ','.join(arglist[:5]) + ')' if arglist else '')
    stmt = (pre_if + ind.strip() + call) if pre_if else (ind + call)
    if outs:
        olist = [o.strip() for o in outs.split(',')]
        assigns = ['%s = sr%d%%' % (o, i) for i, o in enumerate(olist) if o]
        if assigns: stmt += ' : ' + ' : '.join(assigns)
    out.append(stmt)

VARS = r'''
DIM scr% 9000000
DIM jobs%(1)
DIM modmem% 200
DIM tmp% 200000
DIM pts%(3)
sr0% = 0 : sr1% = 0 : sr2% = 0 : sr3% = 0 : sr4% = 0 : sr5% = 0 : sr6% = 0 : sr7% = 0
accel% = 0 : gcol% = 0 : nfill% = 0 : ncopy% = 0 : novl% = 0
sxl% = 1023 : syl% = 767 : sbpp% = 4 : sll% = 4096 : bugmode% = BUGFLAG
$(modmem% + 32) = "V3DDriver" + CHR$(9) + "0.11 (stub)"
modmem%!&14 = 32
$(modmem% + 80) = "V3DDriver: stub declined this screen"
tracen% = 0 : tracearm% = 0 : ncalls% = 0 : nrender% = 0 : nfops% = 0 : nsolid% = 0
'''.replace('BUGFLAG', BUG)

PROCS = r'''
DEF PROCstub_mode(m$)
  LOCAL i%, x%, y%, n%
  i% = INSTR(m$, "X") : sxl% = VAL(MID$(m$, i% + 1)) - 1
  i% = INSTR(m$, "Y") : syl% = VAL(MID$(m$, i% + 1)) - 1
  IF INSTR(m$, "C64K") <> 0 THEN sbpp% = 2 ELSE sbpp% = 4
  sll% = (sxl% + 1) * sbpp%
  REM the CLS the kernel does for the new mode: clear to background (0)
  n% = sll% * (syl% + 1)
  FOR x% = 0 TO n% - 4 STEP 4
    scr%!x% = 0
  NEXT
  REM -BUG emulation: the clear misses the last 40 rows (as a stale-geometry clear would)
  IF bugmode% = 1 AND accel% = 1 THEN
    FOR x% = n% - 40 * sll% TO n% - 4 STEP 4
      scr%!x% = &5A5A5A5A
    NEXT
  ENDIF
  REM -BUG 2 emulation: one 64x64 tile (column 3, row 2 from the top-left) is never written
  IF bugmode% = 2 AND accel% = 1 AND sxl% > 255 AND syl% > 191 THEN
    FOR y% = 128 TO 191
      FOR x% = 192 TO 255
        scr%!(y% * sll% + x% * sbpp%) = &5A5A5A5A
      NEXT
    NEXT
  ENDIF
  IF tracearm% = 1 THEN tracen% = 3
ENDPROC
DEF PROCsys_V3D_PowerOn(a%,b%,c%,d%,e%)
  sr0% = 1 : sr1% = 500000000
ENDPROC
DEF PROCsys_osmodule
  sr3% = modmem%
ENDPROC
DEF PROCsys_readvdu
  vout%!0 = 0 : vout%!4 = 0
  IF sbpp% = 4 THEN vout%!8 = 5 ELSE vout%!8 = 4
  vout%!12 = sll% : vout%!16 = syl% : vout%!20 = sxl% : vout%!24 = scr% : vout%!28 = 0 : vout%!32 = 0
ENDPROC
DEF PROCsys_V3D_AccelProfile(a%,b%,c%,d%,e%)
  sr0% = 0 : sr1% = 0 : sr2% = 0 : sr3% = 0 : sr4% = 0 : sr5% = 0 : sr6% = 0 : sr7% = 0
  IF a% = 6 AND bugmode% = 3 THEN sr0% = 35 : sr1% = modmem% + 80
  IF a% = 7 AND bugmode% = 3 THEN sr6% = 5 : sr7% = 35
  IF a% = 9 THEN
    IF b% = -1 THEN
      tracen% = 0 : tracearm% = 1
    ELSE
      sr7% = tracen% + 65536 * tracearm%
      IF b% < tracen% THEN
        IF b% = 0 THEN sr0% = 1 + 256 * 1
        IF b% = 1 THEN sr0% = 1 + 256 * 2 : sr1% = &80000000 : sr2% = 3
        IF b% >= 2 THEN
          sr0% = 1 + 256 * 3 : sr3% = sxl% : sr4% = syl%
          IF bugmode% = 3 THEN sr0% += 4 * 65536 : sr6% = 35
        ENDIF
        sr6% += b% * 65536
      ENDIF
    ENDIF
  ENDIF
ENDPROC
DEF PROCsys_V3D_AccelStart(a%,b%,c%,d%,e%)
  accel% = 1
ENDPROC
DEF PROCsys_V3D_AccelStop(a%,b%,c%,d%,e%)
  accel% = 0
ENDPROC
DEF PROCsys_V3D_AccelLimits(a%,b%,c%,d%,e%)
  sr0% = 600000 : sr1% = 64 : sr2% = 16384 : sr3% = 48 : sr4% = 65536
ENDPROC
DEF PROCsys_V3D_AccelStats(a%,b%,c%,d%,e%)
  sr0% = ncalls% : sr1% = nrender% : sr2% = nfops% : sr3% = nsolid% : sr4% = nfill% : sr5% = 0
ENDPROC
DEF PROCstub_rectfill(x%, y%, w%, h%)
  PROCsys_plot(4, x%, y%)
  PROCsys_plot(101, x% + w%, y% + h%)
ENDPROC
DEF PROCsys_V3D_AccelCopyStats(a%,b%,c%,d%,e%)
  sr2% = ncopy% : sr4% = novl%
ENDPROC
DEF PROCsys_ColourTrans_SetGCOL(a%,b%,c%,d%,e%)
  gcol% = a% >> 8
ENDPROC
DEF PROCput(x%, y%, v%)
  LOCAL a%
  a% = (syl% - y%) * sll% + x% * sbpp%
  IF sbpp% = 4 THEN
    scr%!a% = v%
  ELSE
    scr%?a% = v% AND 255 : scr%?(a% + 1) = (v% >> 8) AND 255
  ENDIF
ENDPROC
DEF FNget(x%, y%)
  LOCAL a%
  a% = (syl% - y%) * sll% + x% * sbpp%
  IF sbpp% = 4 THEN = scr%!a%
= scr%?a% + 256 * scr%?(a% + 1)
DEF PROCsys_plot(a%,b%,c%)
  LOCAL x1%, y1%, x2%, y2%, w%, h%, x%, y%, dx%, dy%, ovl%
  IF a% = 4 THEN
    pts%(0) = pts%(2) : pts%(1) = pts%(3) : pts%(2) = b% : pts%(3) = c%
  ENDIF
  IF a% = 101 THEN
    x1% = pts%(2) : y1% = pts%(3) : x2% = b% : y2% = c%
    w% = x2% - x1% + 1 : h% = y2% - y1% + 1
    IF accel% = 1 AND bugmode% <> 4 THEN
      ncalls% += 1 : nrender% += 1 : nfops% += 1 : nsolid% += 1
      IF bugmode% <> 3 THEN nfill% += 1
      IF tracearm% = 1 AND tracen% < 24 THEN tracen% += 1
    ENDIF
    FOR y% = y1% TO y2%
      FOR x% = x1% TO x2%
        PROCput(x%, y%, gcol%)
      NEXT
    NEXT
  ENDIF
  IF a% = 190 THEN
    x1% = pts%(0) : y1% = pts%(1) : x2% = pts%(2) : y2% = pts%(3)
    w% = x2% - x1% + 1 : h% = y2% - y1% + 1
    dx% = b% : dy% = c%
    ovl% = (x1% < dx% + w%) AND (dx% < x1% + w%) AND (y1% < dy% + h%) AND (dy% < y1% + h%)
    IF accel% = 1 THEN
      IF ovl% THEN novl% += 1 ELSE ncopy% += 1
    ENDIF
    FOR y% = 0 TO h% - 1
      FOR x% = 0 TO w% - 1
        tmp%!((y% * w% + x%) * 4) = FNget(x1% + x%, y1% + y%)
      NEXT
    NEXT
    FOR y% = 0 TO h% - 1
      FOR x% = 0 TO w% - 1
        PROCput(dx% + x%, dy% + y%, tmp%!((y% * w% + x%) * 4))
      NEXT
    NEXT
  ENDIF
ENDPROC
'''
defined = set(re.findall(r'DEF (PROCsys_\w+)', PROCS))
extra = ''
for n in sorted(used - defined):
    extra += 'DEF %s(a%%,b%%,c%%,d%%,e%%)\n  sr0%% = 0 : sr1%% = 0 : sr2%% = 0 : sr3%% = 0 : sr4%% = 0 : sr5%% = 0 : sr6%% = 0 : sr7%% = 0\nENDPROC\n' % n
open('stub_mode.bas', 'w').write(VARS + '\n'.join(out) + '\n' + PROCS + '\n' + extra)
