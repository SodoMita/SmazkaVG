#!/usr/bin/env python3
"""selftest.py -- smoke tests for the LLM authoring toolkit.

Run:  python3 -m tools.llm.selftest        (from the repo root)
Environment knobs:
  SMAZKA_BIN   path to smazka-raster (default: build/smazka-raster); the
               smazka round-trip check is skipped if the binary is missing.
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))

from tools.llm import geometry, imgscan, author   # noqa: E402

FAILED = []


def check(name, cond):
    print(('  PASS: ' if cond else '  FAIL: ') + name)
    if not cond:
        FAILED.append(name)


# ------------------------------------------------------------------ geometry
t = geometry.tessellate([(0, 0), (50, 40), (100, 0)], smooth=True, closed=False, step=6)
check('tessellate passes through endpoints',
      abs(t[0][0]) < 1e-6 and abs(t[-1][0] - 100) < 1e-6)
check('tessellate densifies', len(t) > 10)
zig = [(0, 0), (10, 20), (20, 0), (30, 20), (40, 0)]
tz = geometry.tessellate(zig, smooth=False, step=4)
deepest = max(p[1] for p in tz)
check('polyline keeps zigzag valleys', deepest > 17)
ts = geometry.tessellate(zig, smooth=True, step=4)
check('catmull also keeps amplitude (uniform, authored-as-is)',
      max(p[1] for p in ts) > 15)
ck = geometry.chaikin(zig, passes=2)
check('chaikin eats valleys (warning demo)', max(p[1] for p in ck) < 17)
sp = geometry.dp_simplify([(0, 0), (50, 1), (100, 0)], eps=2.0)
check('dp_simplify drops collinear', len(sp) == 2)
tr = geometry.trim_ends([(0, 0), (10, 0), (20, 0)], r=6)
check('trim_ends snips both sides', tr[0][0] >= 5 and tr[-1][0] <= 15)
ch = geometry.chain([(0, 0), (10, 0)], [(60, 0)], bridge=8)
check('chain bridges big gaps explicitly', ch == [(0, 0), (10, 0), (60, 0)])

# ------------------------------------------------------------------ imgscan
from PIL import Image, ImageDraw
os.makedirs('build/t', exist_ok=True)
im = Image.new('L', (120, 80), 255)
d = ImageDraw.Draw(im)
d.line([(10, 40), (110, 40)], fill=0, width=4)
d.ellipse([20, 10, 60, 30], outline=0, width=2)
im.save('build/t/selftest_src.png')
src = imgscan.Source('build/t/selftest_src.png')
runs = src.row_runs(40, 0, 119)
check('row_runs finds the 4px band', any(b - a >= 3 for a, b in runs))
verd = dict((p, v) for p, v in src.probe([(60, 40), (60, 44), (0, 0)]))
check('probe on/near/off', verd[(60, 40)] == 'on' and verd[(0, 0)] == 'off')
dr = src.dot_run(40, 0, 119, n=6)
check('dot_run spans the band', len(dr) == 6 and dr[-1][0] - dr[0][0] > 80)
ab = src.ascii_binary(8, 36, 30, 44)
check('ascii_binary shows ink', '#' in ab)
rp = src.fit_report([(10, 40), (60, 40), (110, 40)])
check('fit_report clean on a good line', rp['n_off'] == 0)

# ------------------------------------------------------------------ author
doc = author.Doc(120, 80)
doc.st('top_arc', [(10, 10), (60, 4), (110, 10)], w=2.4)
doc.st('seamL', [(10, 20), (10, 50)], w=2.2)
doc.st('seamR', [(110, 20), (110, 50)], w=2.2)
doc.st('bottom', [(110, 50), (60, 60), (10, 50)], w=2.2)
loopA = doc.chain('top_arc', 'seamR', 'bottom', ('rev', 'seamL'))
doc.st('sash_top', [(8, 28), (112, 28)], w=2.6)
ribbon = doc.chain([(8, 28)], 'sash_top', [(112, 28), (112, 36), (8, 36)])
doc.obj('blob', [(loopA, 3.0, True)], inner=['seamL'])
doc.retire('seamL', 'seamR', 'bottom', 'top_arc', 'sash_top')
doc.obj('ribbon', [(ribbon, 2.6, False)])
w = doc.validate(verbose=False)
check('no UNPLACED after retire', not any('UNPLACED' in x for x in w))

svg = doc.emit_svg()
check('svg has two groups', svg.count('<g id=') == 2)
check('svg white fill objects', svg.count('fill="#FFFFFF"') == 2)
check('svg uniform caps', svg.count('round') >= 4)

smz = doc.emit_smazka()
n_f = sum(1 for ln in smz.splitlines() if ln.startswith('f '))
n_e = sum(1 for ln in smz.splitlines() if ln.startswith('e '))
n_s = sum(1 for ln in smz.splitlines() if ln.startswith('s '))
check('smazka: 2 faces', n_f == 2)
check('smazka: stroke per edge', n_s >= n_e)
check('smazka: round caps everywhere', 'cap=round' in smz and 'cap=butt' not in smz)
check('smazka: no vertex is shared between chains',
      len(set(ln.split()[1] for ln in smz.splitlines() if ln.startswith('v ')))
      == len([ln for ln in smz.splitlines() if ln.startswith('v ')]))

cmp_sg = doc.emit_compact()
check('emit_compact emits shorthand polygon lines', 'P ' in cmp_sg)
check('emit_compact uses short palette names/hex', 'white' in cmp_sg or '#fff' in cmp_sg)

# seam exactness: the two loops carry identical seam coordinates
doc2 = author.Doc(100, 100)
doc2.st('wrist', [(40, 80), (60, 80)], w=2.0)
arm = doc2.chain([(20, 0)], [(20, 60)], [(40, 80)], 'wrist', [(80, 0)])
hand = doc2.chain('wrist', [(80, 95), (70, 100), (45, 100), (40, 80)])
doc2.obj('arm', [(arm, 3.0, True)])
doc2.obj('hand', [(hand, 3.0, False)])
s2 = doc2.emit_svg()
check('seam coordinates shared verbatim', s2.count('40.00 80.00') >= 2
      and s2.count('60.00 80.00') >= 2)

# ------------------------------------------------------- smazka -> raster bin
bin_path = os.environ.get('SMAZKA_BIN', 'build/smazka-raster')
if os.path.exists(bin_path):
    with open('build/t/selftest.smazka', 'w') as fh:
        fh.write(smz)
    rc = os.system(f'{bin_path} build/t/selftest.smazka 120 86 '
                   f'>build/t/selftest_raster.log 2>&1')
    ok = rc == 0 and os.path.exists('build/t/selftest.png')
    check('emitted .smazka renders with the C rasterizer', ok)
    if ok:
        px = Image.open('build/t/selftest.png').convert('RGB').load()
        check('clean render: no red vertex markers by default',
              not any(px[x, y][0] > 200 and px[x, y][1] < 90
                      for x in range(0, 120, 3) for y in range(0, 86, 3)))

    with open('build/t/selftest_compact.sg', 'w') as fh:
        fh.write(cmp_sg)
    rc_compact = os.system(f'{bin_path} build/t/selftest_compact.sg 120 86 '
                           f'>build/t/selftest_compact_raster.log 2>&1')
    check('emitted compact .sg renders with the C rasterizer', rc_compact == 0)
else:
    print('  SKIP: rasterizer binary not found (run make first)')

# ------------------------------------------------------------------ verify
try:
    import cairosvg  # noqa
    have_svg = True
except Exception:
    have_svg = False
if have_svg:
    from tools.llm import verify
    doc.emit_svg(path='build/t/selftest.svg')
    png, sb, tiles = verify.run('build/t/selftest_src.png', 'build/t/selftest.svg',
                                out_prefix='build/t/selftest_cmp')
    check('verify runs end-to-end', os.path.exists(png) and os.path.exists(sb))
else:
    print('  SKIP: cairosvg unavailable; verify loop not exercised')

print()
if FAILED:
    print(f'LLM toolkit selftest: {len(FAILED)} FAILED')
    sys.exit(1)
print('LLM toolkit selftest: ALL PASSED')
