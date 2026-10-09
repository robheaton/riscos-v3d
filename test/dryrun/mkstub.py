import os
import re, sys
import sys as _sys
SRC = _sys.argv[2] if len(_sys.argv) > 2 else os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'TuneBench.bas')
OUT = _sys.argv[3] if len(_sys.argv) > 3 else 'stub_tune.bas'
src = open(SRC).read().split('\n')
used = set()
out = []

def split_top(s):
    parts, depth, cur = [], 0, ''
    for ch in s:
        if ch == '(':
            depth += 1
        if ch == ')':
            depth -= 1
        if ch == ',' and depth == 0:
            parts.append(cur); cur = ''
        else:
            cur += ch
    parts.append(cur)
    return parts

for line in src:
    pre_if = ''
    mi = re.match(r'^(\s*IF .* THEN )(SYS\s.*)$', line)
    if mi:
        pre_if, line = mi.group(1), mi.group(2)
    m = re.match(r'^(\s*)SYS\s+(\"[A-Za-z_0-9]+\"|&[0-9A-Fa-f]+)\s*(.*)$', line)
    if not m:
        out.append(line.replace('dur% = 30', 'dur% = 2'))
        continue
    ind, name, rest = m.groups()
    outs = ''
    mt = re.search(r'(^|\s)TO\s(.*)$', rest)
    if mt:
        outs = mt.group(2)
        rest = rest[:mt.start()]
    args = rest.strip()
    if args.startswith(','):
        args = args[1:]
    arglist = [a.strip() for a in split_top(args)] if args.strip() else []
    arglist = [a if a else '0' for a in arglist]
    while len(arglist) < 5:
        arglist.append('0')
    if name == '"OS_Module"':
        pname = 'PROCsys_osmodule'
        arglist = arglist[:0]
    elif name == '&31':
        pname = 'PROCsys_readvdu'
        arglist = []
    elif name == '&45':
        pname = 'PROCsys_plot'
        arglist = arglist[:3]
    else:
        pname = 'PROCsys_' + name.strip('"')
    used.add(pname)
    call = pname + ('(' + ','.join(arglist[:5]) + ')' if arglist else '')
    stmt = pre_if + ind.strip() + call if pre_if else ind + call
    if outs:
        olist = [o.strip() for o in outs.split(',')]
        assigns = []
        for i, o in enumerate(olist):
            if o:
                assigns.append('%s = sr%d%%' % (o, i))
        if assigns:
            stmt += ' : ' + ' : '.join(assigns)
    out.append(stmt)

VARS = r'''
DIM scr% 4000000
DIM jobs%(1)
DIM modmem% 100
DIM tmp% 80200
DIM pts%(3)
sr0% = 0 : sr1% = 0 : sr2% = 0 : sr3% = 0 : sr4% = 0 : sr5% = 0 : sr6% = 0 : sr7% = 0 : sr8% = 0 : sr9% = 0
accel% = 0 : corrupt% = CORRUPT : gcol% = 0 : nfill% = 0 : ncopy% = 0 : novl% = 0 : npts% = 0
$(modmem% + 32) = "V3DDriver" + CHR$(9) + "0.07 (stub)"
modmem%!&14 = 32
'''
PROCS = r'''DEF PROCsys_V3D_PowerOn(a%,b%,c%,d%,e%)
  sr0% = 1 : sr1% = 500000000
ENDPROC
DEF PROCsys_osmodule
  sr3% = modmem%
ENDPROC
DEF PROCsys_readvdu
  vout%!0 = 0 : vout%!4 = 0 : vout%!8 = 5
  vout%!12 = 0 : vout%!16 = 0 : vout%!20 = 1099 : vout%!24 = 899
  vout%!28 = 0 : vout%!32 = 0 : vout%!36 = 4400 : vout%!40 = 899 : vout%!44 = scr%
ENDPROC
DEF PROCsys_V3D_AccelProfile(a%,b%,c%,d%,e%)
  LOCAL k%
  FOR k% = 0 TO 7 : sr0% = 0 : NEXT
  sr0% = 0 : sr1% = 0 : sr2% = 0 : sr3% = 0 : sr4% = 0 : sr5% = 0 : sr6% = 0 : sr7% = 0 : sr8% = 0 : sr9% = 0
  IF a% = 0 OR a% = 1 THEN
    sr0% = jobs%(a%) : sr1% = jobs%(a%) * 10 : sr2% = jobs%(a%) * 20 : sr3% = jobs%(a%) * 5
    sr4% = jobs%(a%) * 30 : sr5% = jobs%(a%) * 65 : sr6% = 80
    IF b% = 1 THEN jobs%(a%) = 0
  ENDIF
  IF a% = 2 THEN
    sr2% = 1 : sr3% = 1 : sr4% = &1000000 : sr5% = &1000000 : sr6% = &1000000
  ENDIF
ENDPROC
DEF PROCsys_V3D_AccelTune(a%,b%,c%,d%,e%)
  sr0% = 0
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
  sr4% = nfill%
ENDPROC
DEF PROCsys_V3D_AccelCopyStats(a%,b%,c%,d%,e%)
  sr2% = ncopy% : sr4% = novl%
ENDPROC
DEF PROCsys_ColourTrans_SetGCOL(a%,b%,c%,d%,e%)
  gcol% = a% >> 8
ENDPROC
DEF PROCsys_plot(a%,b%,c%)
  LOCAL x1%, y1%, x2%, y2%, w%, h%, x%, y%, dx%, dy%, ovl%, src%, dst%
  IF a% = 4 THEN
    pts%(0) = pts%(2) : pts%(1) = pts%(3) : pts%(2) = b% : pts%(3) = c%
  ENDIF
  IF a% = 101 THEN
    x1% = pts%(2) : y1% = pts%(3) : x2% = b% : y2% = c%
    w% = x2% - x1% + 1 : h% = y2% - y1% + 1
    IF accel% = 1 THEN nfill% += 1 : jobs%(0) += 1
    IF w% * h% <= 20000 THEN
      FOR y% = y1% TO y2%
        FOR x% = x1% TO x2%
          scr%!((899 - y%) * 4400 + x% * 4) = gcol%
        NEXT
      NEXT
      IF accel% = 1 AND corrupt% = 1 THEN scr%!((899 - y2%) * 4400 + x2% * 4) = gcol% EOR 1
    ENDIF
  ENDIF
  IF a% = 190 THEN
    x1% = pts%(0) : y1% = pts%(1) : x2% = pts%(2) : y2% = pts%(3)
    w% = x2% - x1% + 1 : h% = y2% - y1% + 1
    dx% = b% : dy% = c%
    ovl% = (x1% < dx% + w%) AND (dx% < x1% + w%) AND (y1% < dy% + h%) AND (dy% < y1% + h%)
    IF accel% = 1 THEN
      IF ovl% THEN novl% += 1 : jobs%(1) += 2 ELSE ncopy% += 1 : jobs%(1) += 1
    ENDIF
    IF w% * h% <= 20000 THEN
      REM correct overlap handling: read the whole source first (bottom-up copy order not needed in a stub)
      FOR y% = 0 TO h% - 1
        FOR x% = 0 TO w% - 1
          tmp%!((y% * w% + x%) * 4) = scr%!((899 - (y1% + y%)) * 4400 + (x1% + x%) * 4)
        NEXT
      NEXT
      FOR y% = 0 TO h% - 1
        FOR x% = 0 TO w% - 1
          scr%!((899 - (dy% + y%)) * 4400 + (dx% + x%) * 4) = tmp%!((y% * w% + x%) * 4)
        NEXT
      NEXT
    ENDIF
  ENDIF
ENDPROC
'''

# the DEF PROCs must come AFTER the main program ends; BBC BASIC allows DEF anywhere, GOTO skips over them
import sys as _s
CORR = _s.argv[1] if len(_s.argv) > 1 else '0'
defined = set(re.findall(r'DEF (PROCsys_\w+)', PROCS))
extra = ''
for n in sorted(used - defined):
    extra += 'DEF %s(a%%,b%%,c%%,d%%,e%%)\n  sr0%% = 0 : sr1%% = 0 : sr2%% = 0 : sr3%% = 0 : sr4%% = 0 : sr5%% = 0 : sr6%% = 0 : sr7%% = 0 : sr8%% = 0 : sr9%% = 0\nENDPROC\n' % n
open(OUT, 'w').write(VARS.replace('CORRUPT', CORR) + '\n'.join(out) + '\n' + PROCS + '\n' + extra)
