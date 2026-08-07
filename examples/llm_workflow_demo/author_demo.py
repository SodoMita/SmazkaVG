#!/usr/bin/env python3
"""author_demo.py -- vectorize source.png with the dot-first workflow.

Mirrors what an LLM does on real art:
  1. READ   the source with imgscan (ascii + row/col scans -> dot coordinates)
  2. AUTHOR strokes/objects from those dots (every dot inside a dark run)
  3. EMIT   drawing.svg (preview) + drawing.smazka (the real format)
  4. VERIFY with tools.llm.verify (precision/coverage + overlays)
The asserts at the bottom make it a smoke test (also run via `make llm-demo`).
"""
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', '..'))
from tools.llm import imgscan, author, verify            # noqa: E402

SRC = imgscan.Source('source.png')
doc = author.Doc(SRC.W, SRC.H)


def first_run_center(x, y0, y1):
    runs = SRC.col_scan(y0, y1, x, x)[x]
    return (x, runs[0][0]) if runs else None


# -- 1. long strand: col-scan centers (each dot guaranteed inside the stroke)
strand = []
for x in range(35, 361, 20):
    p = first_run_center(x, 190, 265)
    if p:
        strand.append(p)
doc.st('strand', strand, w=3.0, smooth=True)

# -- 2. blob outlines from WINDOWED scans; left/right tips from row extents
# bean1 (front): top band y75-115, bottom band y140-178, tips from rows y115-125
b1_top = [p for x in range(106, 219, 10) if (p := first_run_center(x, 75, 115))]
b1_bot = [p for x in range(112, 214, 10) if (p := first_run_center(x, 140, 178))]
tipL = [(SRC.row_runs(120, 40, 240)[0][0], 120)]          # leftmost ink of row 120
tipR = [(SRC.row_runs(120, 40, 240)[-1][1], 120)]         # rightmost (bean1 tip)
loop1 = doc.chain(tipL, b1_top, tipR, list(reversed(b1_bot)), bridge=12)

# bean2 (back): top band y55-83, bottom band y108-143, tips from rows y95-105
b2_top = [p for x in range(196, 346, 10) if (p := first_run_center(x, 55, 83))]
b2_bot = [p for x in range(198, 344, 10) if (p := first_run_center(x, 108, 143))]
runs = [r for y in (95, 100, 105) for r in SRC.row_runs(y, 180, 380)]
t2L = (min(a for a, _ in runs), 100)
t2R = (max(b for _, b in runs), 100)
loop2 = doc.chain([t2L], b2_top, [t2R], list(reversed(b2_bot)), bridge=12)

# -- 3. zigzag: prove the reading with binary ascii, then author SEEN dots
print('zigzag zone exactly as the scanner sees it:')
print(SRC.ascii_binary(138, 106, 218, 134))
zig = [(140, 130), (155, 110), (170, 130), (185, 110), (200, 130), (215, 112)]
bad = [p for p, v in SRC.probe(zig) if v == 'off']
assert not bad, f'zigzag dots off the source: {bad}'
doc.st('zig', zig, w=2.0, smooth=False)                  # polyline: NO smoothing

# -- 4. hatch accents: straight 2-dot strokes
doc.st('h1', [(285, 95), (300, 123)], w=2.0, smooth=False)
doc.st('h2', [(305, 91), (320, 119)], w=2.0, smooth=False)

# -- assemble: back blob, front blob, then the open strand on top (z-order)
doc.obj('bean2', [(loop2, 3.0, True)], inner=['h1', 'h2'])
doc.obj('bean1', [(loop1, 3.0, True)], inner=['zig'])
doc.obj('front_strand', inner=['strand'])
doc.validate(verbose=True)

doc.emit_svg('preview.svg')
doc.emit_smazka('drawing.smazka')
print('emitted preview.svg + drawing.smazka')

# -- 5. verify loop -------------------------------------------------------
png, sbs, tiles = verify.run('source.png', 'preview.svg', out_prefix='cmp')
scores, _, _ = verify.compare('source.png', png, tols=(6,))
p6, c6 = scores[6]
assert p6 >= 0.75 and c6 >= 0.75, \
    f'demo regressed: tol6 P={p6:.2f} C={c6:.2f} (inspect cmp_overlay.png)'
print(f'DEMO OK (svg preview): tol6 precision {p6:.3f} coverage {c6:.3f}')

# -- 6. crosscheck: the real .smazka through the C rasterizer -------------
# IMPORTANT: --view 0 0 1 pins document coords to source pixels (else the
# rasterizer auto-fits with a 50px margin and nothing lines up).
BIN = os.path.join('..', '..', 'build', 'smazka-raster')
if os.path.exists(BIN):
    import subprocess
    subprocess.run([BIN, 'drawing.smazka', str(SRC.W), str(SRC.H),
                    '--view', '0', '0', '1'], check=True,
                   capture_output=True)
    s2, _, _ = verify.compare('source.png', 'drawing.png', tols=(6,))
    p2, c2 = s2[6]
    print(f'DEMO OK (C rasterizer): tol6 precision {p2:.3f} coverage {c2:.3f}')
    assert p2 >= 0.75 and c2 >= 0.75, 'smazka round-trip regressed'
else:
    print('  SKIP: build/smazka-raster not found (run make)')
