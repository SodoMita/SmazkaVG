"""verify.py -- the objective feedback loop.

Renders the preview SVG, binarizes both images, and scores:
  precision (share of drawn pixels that land on/near source strokes) and
  coverage (share of source-stroke pixels that are covered by the drawing)
at several tolerances. Emits a red/blue overlay + missing-region tiles +
a side-by-side jpg for eyeballing (yours or a VLM's).

Workflow per iteration (this is the loop that replaces "vibes"):
  1. python3 build_drawing.py            # author -> drawing.svg + drawing.smazka
  2. python3 -m tools.llm.verify original.jpg drawing.svg --out cmp
  3. inspect cmp_overlay.png: RED = your ink with no source under it (wrong),
     BLUE = source ink you have not drawn yet (missing). Fix the worst region.
  4. goto 1.

cairosvg is used for rendering when available; falls back to the SmazkaVG
rasterizer binary via --smazka-bin (then only the .smazka path is scored).
"""
import argparse
import os
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image


def render_svg(svg_path, W, H, out_png=None):
    import cairosvg
    out_png = out_png or svg_path.rsplit('.', 1)[0] + '_render.png'
    cairosvg.svg2png(url=svg_path, write_to=out_png,
                     output_width=W, output_height=H)
    return out_png


def _dilate(mask, r):
    """Cheap disk-ish dilation via coordinate shifts (no scipy needed)."""
    h, w = mask.shape
    out = np.zeros_like(mask)
    rr = int(r)
    for dy in range(-rr, rr + 1):
        for dx in range(-rr, rr + 1):
            if dx * dx + dy * dy <= r * r:
                ys = slice(max(0, dy), h + min(0, dy))
                xs = slice(max(0, dx), w + min(0, dx))
                yt = slice(max(0, -dy), h + min(0, -dy))
                xt = slice(max(0, -dx), w + min(0, -dx))
                out[ys, xs] |= mask[yt, xt]
    return out


def compare(src_path, render_path, tols=(3, 6, 10), src_thr=128, rdr_thr=200):
    src = np.array(Image.open(src_path).convert('L')) < src_thr
    rdr = np.array(Image.open(render_path).convert('L').resize(
        (src.shape[1], src.shape[0]))) < rdr_thr
    scores = {}
    for t in tols:
        src_d = _dilate(src, t)     # source grown: what render may touch
        rdr_d = _dilate(rdr, t)     # render grown: what it manages to cover
        prec = float((rdr & src_d).sum()) / max(1, rdr.sum())
        cov = float((src & rdr_d).sum()) / max(1, src.sum())
        scores[t] = (prec, cov)
    return scores, src, rdr


def overlay(src, rdr, tol=6):
    """BLACK = agreement, RED = drawn-not-in-source, BLUE = missing."""
    src_d = _dilate(src, tol)
    rdr_d = _dilate(rdr, tol)
    ov = np.full(src.shape + (3,), 255, np.uint8)
    both = rdr & src_d
    ov[both & src] = (0, 0, 0)
    ov[rdr & ~src_d] = (255, 40, 40)
    ov[src & ~rdr_d] = (40, 110, 255)
    return ov


def worst_tiles(src, rdr, tile=378, tol=6, n=6):
    """Tiles with the most BLUE (missing) ink -- your work list, sorted."""
    src_d = _dilate(src, tol)
    rdr_d = _dilate(rdr, tol)
    missing = src & ~rdr_d
    h, w = src.shape
    out = []
    for y0 in range(0, h, tile):
        for x0 in range(0, w, tile):
            m = missing[y0:y0 + tile, x0:x0 + tile].sum()
            out.append((int(m), x0, y0))
    out.sort(reverse=True)
    return out[:n]


def side_by_side(src_path, render_path, out_jpg, scale=2):
    o = Image.open(src_path).convert('L')
    r = Image.open(render_path).convert('L').resize(o.size)
    ow, oh = o.width // scale, o.height // scale
    sb = Image.new('L', (ow * 2 + 16, oh), 208)
    sb.paste(o.resize((ow, oh)), (0, 0))
    sb.paste(r.resize((ow, oh)), (ow + 16, 0))
    sb.save(out_jpg, quality=90)
    return out_jpg


def zcrop_pair(src_path, render_path, x0, y0, x1, y1, out_png, scale=3):
    o = Image.open(src_path).convert('L').crop((x0, y0, x1, y1))
    r = Image.open(render_path).convert('L').crop((x0, y0, x1, y1))
    w, h2 = o.width * scale, o.height * scale
    pair = Image.new('L', (w * 2 + 12, h2), 210)
    pair.paste(o.resize((w, h2), Image.NEAREST), (0, 0))
    pair.paste(r.resize((w, h2), Image.NEAREST), (w + 12, 0))
    pair.save(out_png)
    return out_png


def run(src_path, svg_path, out_prefix='cmp', tols=(3, 6, 10), sbs_scale=2):
    W = Image.open(src_path).width
    H = Image.open(src_path).height
    png = render_svg(svg_path, W, H)
    scores, src, rdr = compare(src_path, png, tols)
    for t in sorted(scores):
        p, c = scores[t]
        print(f'tol{t}px  precision {p:.3f}  coverage {c:.3f}')
    Image.fromarray(overlay(src, rdr, 6)).save(f'{out_prefix}_overlay.png')
    sb = side_by_side(src_path, png, f'{out_prefix}_sbs.jpg', sbs_scale)
    tiles = worst_tiles(src, rdr)
    for i, (m, x0, y0) in enumerate(tiles):
        if m == 0:
            continue
        print(f'  missing #{i}: px={m} at ({x0},{y0})..'
              f'({min(x0 + 378, src.shape[1])},{min(y0 + 378, src.shape[0])})')
    return png, sb, tiles


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='SmazkaVG LLM verify loop')
    ap.add_argument('source')
    ap.add_argument('svg')
    ap.add_argument('--out', default='cmp')
    ap.add_argument('--scale', type=int, default=2)
    args = ap.parse_args()
    run(args.source, args.svg, args.out, sbs_scale=args.scale)
