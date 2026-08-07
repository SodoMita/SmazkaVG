# AGENTS.md — Vectorizing a raster image with SmazkaVG (dot-first workflow)

This file is the operating manual for an LLM (or human) converting a raster
line-art image into SmazkaVG. It encodes a workflow proven on a real, hard
conversion (anime figure, 1350×2268, hundreds of strokes) and, just as
importantly, the *failure modes* that cost days before they were understood.

No trace tools (potrace/autotrace) are involved anywhere: every coordinate is
*authored* from measurements of the source image and verified against it.

---

## 0. TL;DR loop

```sh
make                       # builds build/smazka-raster (crosscheck renderer)
make llm-demo              # 2-minute end-to-end example of everything below
make llm-test              # toolkit unit tests
```

Per iteration on your image:

1. **measure** a region with `tools/llm/imgscan.py` (ascii / row & col runs)
2. **author/adjust dots** in your build script (`tools/llm/author.py`)
3. run the build script → `preview.svg` + `drawing.smazka`
4. `python3 -m tools.llm.verify source.jpg preview.svg --out cmp`
5. open `cmp_overlay.png` — **red = your ink is wrong, blue = missing ink** —
   and `cmp_sbs.jpg` (side-by-side); fix the worst tile; repeat.

Never iterate without the metric. "Looks better to me" is how weeks get lost.

---

## 1. The doctrine (rules that exist because each was once a disaster)

### 1.1 Dots live INSIDE strokes
Every authored dot must sit inside the ink of the source: probe with
`Source.probe()` or pick centers from `row_scan`/`col_scan` runs — run centers
are inside by construction. `Source.fit_report()` tells you per-dot verdicts
(on/near/off) plus whether the *interpolated curve* wanders off the ink.

### 1.2 Uniform strokes, round caps — no tapered ribbons
Author with a palette of 2–3 constant widths (e.g. 2.0 detail / 2.4 interior /
3.0 outline). Variable-width ribbon profiles (tapered "power strokes") are
nearly impossible to edit later when every segment of a region needs its own
width re-tuning. The renderer supports tapers; the *workflow* forbids them
unless the user explicitly asks.

### 1.3 Objects = ONE closed single-line loop, white fill, black outline
Each visual object (torso, arm, hand, foot, bikini panel, hair mass, board,
bun, flower…) is exactly one closed loop with `fill #FFFFFF`, plus optional
inner detail strokes. Overlaps are resolved **only by painter z-order** (obj()
call order), never by interleaving paths or boolean surgery.

### 1.4 Butt seams share EXACT coordinates
Where two objects meet (arm/hand at wrist, leg/foot at ankle, band/panel),
the two loops must contain the *same* coordinate run — build it once, reuse
it in both (`doc.chain`, `copy_span`, `span_x/span_y`). Identical coordinates
= zero-width seam. "Close enough" seams show up as gray hairlines or doubled
borders at the first 3× zoom.

### 1.5 smooth=True for strands, smooth=False for zigzags
Catmull chains pass through every dot and are lovely for hair/strands.
**Fingers, teeth, toe caps, hatch fans are polylines.** Any smoothing
(Chaikin corner-cutting in particular) eats zigzag valleys: fingers become
nubs, teeth plateaus. This was a multi-day regression in the reference
project; there is a selftest case (`chaikin eats valleys`) so nobody relearns
it the hard way.

### 1.6 Never share a vertex between chains
The rasterizer derives Catmull tangents from **any** edge touching a vertex
(SPEC §7). If two strands share a vertex (a crossing, a "helpful" join), the
tangent of each bleeds into the other and the curve visibly hops sideways at
hatch/crossing zones ("snap-hop"). `author.py` always emits private vertex
chains per stroke/loop; keep it that way. If you hand-edit `.smazka`, give
every chain its own vertices even when coordinates coincide.

### 1.7 Strand endings: stagger, don't curtain
A fan of strands (hair, grass, fringe) whose tips all end at the same height
reads as a plastic curtain. Stagger tip lengths/heights like the source; end
smooth strands *before* they crash into hatch or crossing zones and draw the
hatching as its own short plain strokes.

### 1.8 Don't re-snap authored dots across busy zones
Blindly snapping dots to "nearest dark pixel" makes chains hop between
parallel lines (hatching, braid X-weave) and produces shingle artifacts.
Author from scan runs (position is data), trim with `trim=` to marry
junctions, and use `fit_report` locally instead of global re-snapping.

### 1.9 Coordinates are source-image pixels — pin the view
`author.Doc(W, H)` uses the source image's pixel grid 1:1. When rendering
`.smazka` with the C rasterizer, always pass `--view 0 0 1`, otherwise it
auto-fits with a 50 px margin and your comparison metric lies to you —
a full-figure document at native canvas size comes out ~5 % small and
~40 px off, halving a tol-6 score while *looking* fine (v1.6.3+ prints the
applied auto-fit transform once per run; heed it).
(When authoring interactively with debug guides, `--debug-overlay` re-enables
raw edge guides + red vertex markers; those marks used to be baked into every
output, which is why old example PNGs look dotty.)

---

## 2. Measuring: how an LLM "sees" the source (`imgscan`)

```python
from tools.llm import imgscan
SRC = imgscan.Source('original.jpg', thr=128)

SRC.row_runs(y=383, x0=600, x1=800)      # [(664,684),(712,748)] dark runs on a row
SRC.col_scan(360, 420, 700, 760)         # {x: [(yc, y0, y1), ...]} run centers per column
SRC.ascii_binary(640, 340, 780, 410)     # 1 char/px; *the* eye for faces/hands
SRC.ascii(300, 500, 700, 900, cw=3, ch=6)# shaded overview of a big region
SRC.crop(640, 340, 780, 410, '/tmp/z.png', scale=3)   # zoomed reference image
SRC.probe([(730, 383), (731, 390)])      # [((730,383),'on'), ((731,390),'off')]
SRC.dot_run(383, 660, 750, n=8)          # 8 dots across the row's main dark run
```

Practical notes from the field:

- **Face**: work from `ascii_binary` at 1px/char. Bold eyebrow bands, iris
  outlines, nose hook, mouth lens, blush ticks (light gray! probe with
  `thr=200`) are all measurably separate structures; author them as separate
  short strokes.
- **Iris/pupil**: match the source *exactly* (if the source irises are white
  circles with no pupils, draw white circles — do not "fix" the art).
- **Hands**: measure knuckle/finger strands with col scans; expect 2–3 px
  strand spacing. Author the loop as one polyline with deep valleys between
  fingers (see 1.5).
- **Feet/toes**: smooth outline + small cap bumps *on* the bottom line;
  separators are short interior strokes; toenails are tiny curves inside caps.
- **Thresholds**: line art core is `<128`; soft gray accents (blush, shading)
  live at `<210`. Probe with the right threshold or you will author ghosts.

---

## 3. Authoring (`author`)

```python
from tools.llm import author
doc = author.Doc(1350, 2268)

# plain inner strokes ------------------------------------------------
doc.st('eye_low',  [(706,403),(726,405),(746,403),(757,399)], w=2.2)
doc.st('fingers',  [(1168,1130), ... 44 measured dots ...], w=2.4, smooth=False)
doc.st('strand',   [(x,y), ...], w=2.4, trim=6)   # snip 6px off both ends

# object: closed white loop + inner details, z-order = call order ----
seam = [(1168,1130),(1222,1132)]                    # wrist, EXACT coords
doc.st('wrist', seam)
arm  = doc.chain([(1002,760)],  ..., seam)          # outer loop incl. seam
hand = doc.chain('wrist', [(1165,1200)], ..., [(1168,1130)])
doc.obj('arm',  [(arm,  3.0, True)])                # loop, width, smooth
doc.obj('hand', [(hand, 3.0, False)])               # polyline loop (fingers!)

doc.retire('wrist')       # coordinates reusable, never drawn standalone
doc.validate()            # warns: DUP names, UNPLACED strokes, degenerates

doc.emit_svg('preview.svg')
doc.emit_smazka('drawing.smazka')
```

Z-ordering strategy that works (reference figure): background props →
back hair mass → body → back foot → front foot → bottoms → top → arm →
hand → head/face → front hair → headdress → flower. Front-most last.

Authoring volumes: the reference figure is ~450 authored strokes / ~90
objects-scale zones. Budget dots: 10–40 px spacing on curves, tighter in
bends; 20–44 dots per contour-rich object loop.

---

## 4. Verifying (`verify`)

```sh
python3 -m tools.llm.verify original.jpg preview.svg --out cmp
# tol3px  precision 0.86  coverage 0.81
# tol6px  precision 0.92  coverage 0.88
# tol10px precision 0.96  coverage 0.93
#   missing #0: px=1281 at (360,378)..(738,756)      <- work list, sorted
```

- **precision** = drawn pixels that have source ink within tol (anti-hallucination)
- **coverage**  = source ink pixels covered within tol (anti-laziness)
- `cmp_overlay.png` — black agreement / red wrong ink / blue missing ink
- `cmp_sbs.jpg` — half-scale original|render, the artifact to eyeball or to
  hand to a VLM (see §5)
- `verify.zcrop_pair()` for 3× zoomed A/B crops of a region
- Reference numbers on the hard project: tol6 P≈0.92 C≈0.88 with a fully
  hand-authored result; expect 0.97+/0.97+ on simpler art (the demo hits it).

Use the C rasterizer as an independent second pair of eyes (catches
emission bugs the SVG path can't):

```sh
build/smazka-raster drawing.smazka 1350 2268 --view 0 0 1
python3 -c "from tools.llm import verify; \
print(verify.compare('original.jpg','drawing.png',(6,))[0])"
```

---

## 5. Using a VLM as a critic (optional but strong)

Render `cmp_sbs.jpg` (or a zoom pair) and ask an image model for an
**annotated copy**: *"Take this comparison image and draw a copy of it with
red circles around every place where the right side deviates from the left,
label each with a short note."*

⚠️ Ask for an *image back*, not an essay: endpoints that only return images
discard text replies (you get "Response contains no images"). Feed the
annotation back into your next measurement pass. Also known-good prompts:
"circle the region where the eye sits relative to the original", "draw an
arrow where this strand should end".

---

## 6. SmazkaVG cheatsheet (the subset this workflow emits)

```
v <id> <x> <y>                     # vertex (pixel coords with --view 0 0 1)
e <id> <v0> <v1> type=catmull      # smooth edge (author emits private chains)
e <id> <v0> <v1> type=seg          # polyline edge (zigzag detail)
f <id> <e0 e1 ... en> FFFFFF       # closed face (white object loop)
s <id> <e> 000000FF <w> <w> cap=round   # uniform round-cap stroke on an edge
```

Width constant across an edge ⇒ two equal samples (`w w`). Faces fill exactly
inside their edges, so the outline stroke sits half-in/half-out — visually a
butt seam when the neighbor shares the coordinates (§1.4).

Binary/golf: once the drawing is final, `./build/smazka-bin enc` shrinks it
losslessly; `./build/smazka-golf` exists if byte-golfing is the goal.

---

## 7. Iteration order that converges

1. Silhouette loops of the largest objects (metric will already jump).
2. Largest inner structures (bikini band, hair masses, board rails).
3. Face — always measure, never draw from imagination (§2). A face authored
   from vibe is the #1 "doesn't look like her" cause.
4. Hands/feet — polylines with authored valleys (§1.5); measure knuckles.
5. Detail: hatches, blush, braid weave, knots, petals (closed mini-loops).
6. Strand-tip staggering + seam audit (§1.7/§1.4) at 3× zoom pairs.
7. Final: metric at 3 tolerances, C-rasterizer crosscheck, VLM critique pass.

Fix the **worst missing tile first** (`verify.run` prints them sorted), one
region per iteration. Do not "polish globally" before the metric is green;
global vibes-polish is how previously-correct regions regress unnoticed.

---

## 8. Author directly in `.smazka` (dialect v1.6.2)

The deliverable is the `.smazka` file itself, edited as text — **not** a
Python script that regenerates it. Python is for *measurement* (`imgscan`)
and *metrics* (`verify`); authoring happens in the document.

Use the authoring skin (SPEC §7.4):

- **Name things.** `v`/`e`/`f`/`s` take symbolic ids; `path`/`fobj`/`group`
  bundle geometry under names (`fobj hair_back fill=FFFFFF sw=3 seg …`).
  Names are what makes an edit diff reviewable.
- **One `fobj` per closed object**, plain points on one line items. Small
  smooth loops: `fobj … catmull` through sparse knots. Long tessellated
  loops: plain `seg` points (~6 px spacing) — face fills chew ≤1024 edges.
- **Paths are inner strokes**: `path swoosh catmull w=2.2 cap=round …` —
  no manual `v/e/s` triplet bookkeeping per curve.
- **Seams are splice references, not retyped coordinates.** Define the seam
  once (`path wrist seg 1245,1120 1170,1122`) and reference it from both
  sides: `fobj arm … useg wrist …` / `fobj hand … revg wrist …`. `use/rev`
  shares the spline's vertices; `useg/revg` ghosts fresh copies. A join of
  ≤0.5 px is silent, ≤2 px auto-bridges with a warning, more is an
  expansion error — by design: drifted seams are *louder* than float noise.
- **Lint before rendering**: `smazka-raster file.smazka --xpand /dev/null` —
  exit code 0 and no `# xa ERROR` lines means well-formed. Errors are also
  written inline in the xpanded output, at the position they occurred.
- **Don't share vertices between catmull chains** (tangents derive from any
  touching edge — SPEC/AGENTS §1); ghosts (`useg`) exist precisely for that.

Edit loop per fix: measure zone (`imgscan.shade/probe`) → edit the named
record in the `.smazka` → `--xpand` lint → render `--view 0 0 1` →
`verify.run` → next worst tile. Numeric ids are fine to mix in (supplement
automation), just keep them above the name counters; redefinitions are now
hard errors, not silent overwrites.

---

## 9. Audit: constraints that show the garbage (v1.6.3)

`verify` answers "how close is the render" (one number); **audit** answers
"which records are lying" (a per-stroke charge sheet):

```sh
PYTHONPATH=tools python3 -m llm.audit drawing.smazka \
    --src original.jpg --tol 6 \
    --overlay audit_overlay.smazka --report audit.txt
build/smazka-raster audit_overlay.smazka 1350 2268 --view 0 0 1
```

Charge sheet:

- **stray** (red) — ink with no source under it within `--tol`. Re-anchor
  it from `imgscan` runs or retire it. A stray stroke is authored
  imagination; it is also how +2 pp of false metric gets cleaned.
- **hidden** (blue) — every inked pixel covered by a later opaque face in
  your own z-order. Sometimes doctrine (occluded board edge under an arm
  fill); sometimes a z-order bug. It is a review queue, not a conviction.
- **dup** (magenta) — two records painting the same line. Merge them;
  dups render as fake-bold.
- **join** (orange) — endpoints hovering near vertices they missed. Fix by
  retyping the exact coordinate or by splicing with `use/rev` (§8), never
  by nudging "close enough".
- **degen** — collapsed spans (duplicate consecutive points).

Rules of engagement:

1. **Don't argue with stray.** Measure the zone (`§2`), and either the
   source run exists (re-anchor, exact coords) or it doesn't (delete).
2. **Overlay first, then edit.** The overlay is a `.smazka` of the
   findings themselves — render it pinned (`--view 0 0 1`) and read the
   colors off the source, crop-zoomed. The `.txt` is for ordering
   (worst-first), the overlay is for *seeing*.
3. **Audit after every batch of edits**, before committing. Regression
   direction is more important than the absolute count: claims deleted
   should never come back silently.
4. Pair with `fobj … sw=0` + explicit `path` boundary strokes (§8): the
   default "stroke the whole loop" paints phantom ink wherever the source
   merges two objects into one line (an arm crossing a board edge, scarf
   over sleeve). sw=0 makes the fill honest; paths claim only real ink.
