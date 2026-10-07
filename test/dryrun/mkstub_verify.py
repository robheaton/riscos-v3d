import os, re, sys
# usage: mkstub_verify.py <bpp 4|2> <bug 0..3> [out.bas]
#   bug 0 = GPU emulation exact; 1 = GPU fill of width 64 drops its last column;
#   2 = GPU scroll copies naively in place (wrong for overlapping shifts);
#   3 = GPU fill also writes one pixel just outside its rectangle.
BPP = sys.argv[1] if len(sys.argv) > 1 else '4'
BUG = sys.argv[2] if len(sys.argv) > 2 else '0'
OUT = sys.argv[3] if len(sys.argv) > 3 else 'stub_verify.bas'
SRC = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'VerifySuite.bas')
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
    line = re.sub(r'^NFILL% = 150 : NCOPY% = 150 : NOVL% = 150', 'NFILL% = 14 : NCOPY% = 10 : NOVL% = 24', line)
    pre_if = ''
    mi = re.match(r'^(\s*IF .* THEN )(SYS\s.*)$', line)
    if mi:
        pre_if, line = mi.group(1), mi.group(2)
    m = re.match(r'^(\s*)SYS\s+(\"[A-Za-z_0-9]+\"|&[0-9A-Fa-f]+)\s*(.*)$', line)
    if not m:
        out.append(pre_if + line if pre_if else line)
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
    stmt = (pre_if + call) if pre_if else (ind + call)
    if outs:
        olist = [o.strip() for o in outs.split(',')]
        assigns = ['%s = sr%d%%' % (o, i) for i, o in enumerate(olist) if o]
        if assigns: stmt += ' : ' + ' : '.join(assigns)
    out.append(stmt)

VARS = r'''
DIM scr% 4000000
DIM modmem% 100
DIM tmp% 400000
DIM pts%(3)
sr0% = 0 : sr1% = 0 : sr2% = 0 : sr3% = 0 : sr4% = 0 : sr5% = 0 : sr6% = 0 : sr7% = 0
accel% = 0 : gcol% = 0 : nfillc% = 0 : ncopyc% = 0 : novlc% = 0
lim0% = 16384 : lim1% = 32 : lim2% = 2048 : lim3% = 16 : lim4% = 2048
sxl% = 1299 : syl% = 699 : sbpp% = SBPP : sll% = 1300 * sbpp% : bugmode% = BUGFLAG
$(modmem% + 32) = "V3DDriver" + CHR$(9) + "0.07 (stub)"
modmem%!&14 = 32
'''.replace('SBPP', BPP).replace('BUGFLAG', BUG)

PROCS = r'''
DEF PROCsys_V3D_PowerOn(a%,b%,c%,d%,e%)
  sr0% = 1 : sr1% = 250000000
ENDPROC
DEF PROCsys_osmodule
  sr3% = modmem%
ENDPROC
DEF PROCsys_readvdu
  vout%!0 = 0 : vout%!4 = 0
  IF sbpp% = 4 THEN vout%!8 = 5 ELSE vout%!8 = 4
  vout%!12 = 0 : vout%!16 = 0 : vout%!20 = sxl% : vout%!24 = syl% : vout%!28 = 0 : vout%!32 = 0
  vout%!36 = sll% : vout%!40 = syl% : vout%!44 = scr% : vout%!48 = sxl%
ENDPROC
DEF PROCsys_V3D_AccelProfile(a%,b%,c%,d%,e%)
  sr0% = 0 : sr1% = 0 : sr2% = 0 : sr3% = 0 : sr4% = 0 : sr5% = 0 : sr6% = 0 : sr7% = 0
ENDPROC
DEF PROCsys_V3D_AccelStart(a%,b%,c%,d%,e%)
  accel% = 1
ENDPROC
DEF PROCsys_V3D_AccelStop(a%,b%,c%,d%,e%)
  accel% = 0
ENDPROC
REM Models the driver's size limits (a limit of -1 = leave unchanged; all zero = everything
REM goes to the GPU) so a script that relies on "limits 0 = everything" is really tested.
DEF PROCsys_V3D_AccelLimits(a%,b%,c%,d%,e%)
  sr0% = lim0% : sr1% = lim1% : sr2% = lim2% : sr3% = lim3% : sr4% = lim4%
  IF a% <> -1 THEN lim0% = a%
  IF b% <> -1 THEN lim1% = b%
  IF c% <> -1 THEN lim2% = c%
  IF d% <> -1 THEN lim3% = d%
  IF e% <> -1 THEN lim4% = e%
ENDPROC
DEF FNgpufill(w%, h%)
  LOCAL by%, t%
  IF accel% = 0 THEN = FALSE
  by% = w% * h% * sbpp%
  IF by% < lim0% OR h% < lim1% THEN = FALSE
  REM the fill density rule (skipped when both fill limits are zero)
  IF lim0% <> 0 OR lim1% <> 0 THEN
    t% = ((w% + 63) DIV 64) * ((h% + 63) DIV 64)
    IF by% + 15 * h% < t% * 4500 THEN = FALSE
  ENDIF
= TRUE
DEF FNgpucopy(w%, h%, ov%)
  LOCAL by%
  IF accel% = 0 THEN = FALSE
  by% = w% * h% * sbpp%
  IF ov% THEN
    IF by% < lim4% OR h% < lim3% THEN = FALSE
  ELSE
    IF by% < lim2% OR h% < lim3% THEN = FALSE
  ENDIF
= TRUE
DEF PROCsys_V3D_AccelStats(a%,b%,c%,d%,e%)
  sr4% = nfillc%
ENDPROC
DEF PROCsys_V3D_AccelCopyStats(a%,b%,c%,d%,e%)
  sr2% = ncopyc% : sr4% = novlc%
ENDPROC
DEF PROCsys_ColourTrans_SetGCOL(a%,b%,c%,d%,e%)
  gcol% = (a% >> 8) AND &FFFFFF
ENDPROC
DEF PROCput(x%, y%, v%)
  LOCAL a%
  IF x% < 0 OR y% < 0 OR x% > sxl% OR y% > syl% THEN ENDPROC
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
  LOCAL x1%, y1%, x2%, y2%, w%, h%, x%, y%, dx%, dy%, ovl%, gpu%
  IF a% = 4 THEN
    pts%(0) = pts%(2) : pts%(1) = pts%(3) : pts%(2) = b% : pts%(3) = c%
  ENDIF
  IF a% = 101 THEN
    x1% = pts%(2) : y1% = pts%(3) : x2% = b% : y2% = c%
    w% = x2% - x1% + 1 : h% = y2% - y1% + 1
    gpu% = FNgpufill(w%, h%)
    IF gpu% THEN nfillc% += 1
    FOR y% = y1% TO y2%
      FOR x% = x1% TO x2%
        IF NOT (gpu% AND bugmode% = 1 AND w% = 64 AND x% = x2%) THEN PROCput(x%, y%, gcol%)
      NEXT
    NEXT
    IF gpu% AND bugmode% = 3 THEN PROCput(x2% + 1, y2% + 1, gcol%)
  ENDIF
  IF a% = 190 THEN
    x1% = pts%(0) : y1% = pts%(1) : x2% = pts%(2) : y2% = pts%(3)
    w% = x2% - x1% + 1 : h% = y2% - y1% + 1
    dx% = b% : dy% = c%
    ovl% = (x1% < dx% + w%) AND (dx% < x1% + w%) AND (y1% < dy% + h%) AND (dy% < y1% + h%)
    gpu% = FNgpucopy(w%, h%, ovl%)
    IF gpu% THEN
      IF ovl% THEN novlc% += 1 ELSE ncopyc% += 1
    ENDIF
    IF gpu% AND bugmode% = 2 AND ovl% THEN
      REM naive in-place copy, rows then columns in increasing order: wrong for some shifts
      FOR y% = 0 TO h% - 1
        FOR x% = 0 TO w% - 1
          PROCput(dx% + x%, dy% + y%, FNget(x1% + x%, y1% + y%))
        NEXT
      NEXT
    ELSE
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
  ENDIF
ENDPROC
'''
defined = set(re.findall(r'DEF (PROCsys_\w+)', PROCS))
extra = ''
for n in sorted(used - defined):
    extra += 'DEF %s(a%%,b%%,c%%,d%%,e%%)\n  sr0%% = 0 : sr1%% = 0 : sr2%% = 0 : sr3%% = 0 : sr4%% = 0 : sr5%% = 0 : sr6%% = 0 : sr7%% = 0\nENDPROC\n' % n
open(OUT, 'w').write(VARS + '\n'.join(out) + '\n' + PROCS + '\n' + extra)
