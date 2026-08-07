# v1.6 — LLM vectorization workflow & clean rendering

Tooling release: no changes to the binary format or the Line-ASM grammar.
Motivation: making SmazkaVG *writable by an LLM looking at a raster image* —
measured on a real, month-long manual conversion (anime figure, 1350×2268,
~450 authored strokes), where the failure modes below were discovered.

## Added

### `tools/llm/` — dot-first authoring toolkit (Python: Pillow + numpy, cairosvg optional)
- **`imgscan.py`** — read ground truth out of the source raster: row/column
  dark-run scans (run centers are guaranteed-inside-ink dot candidates),
  binary/shaded ASCII views for "seeing" fine structure, per-dot `probe()`
  verdicts, `dot_run()` strand backbones, `fit_report()` local curve checks,
  zoomed crop export.
- **`author.py`** — stroke/object registry that emits both Line-ASM
  (`.smazka`) and a WYSIWYG preview SVG. Enforces the workflow doctrine:
  uniform-width round-cap strokes (no tapered ribbons), one closed
  white-filled loop per object, painter z-order only, exact-coordinate butt
  seams (`chain`/`span_x`/`span_y`/`copy_span`), private vertex chains per
  stroke (no cross-chain tangent bleed), `retire()`/`UNPLACED`/`DUP` hygiene
  checks via `validate()`.
- **`verify.py`** — the objective feedback loop: precision/coverage at
  multiple pixel tolerances, red/blue diff overlay, worst-missing tile list,
  side-by-side jpg, zoomed A/B crop pairs.
- **`geometry.py`** — dependency-free Catmull-Rom / polyline tessellation
  (matching the rasterizer's `f` conversion), arc-length resampling,
  Chaikin (with an explicit "eats zigzag valleys" warning), DP simplify,
  end trimming, chain bridging.
- **`selftest.py`** — 25+ unit checks incl. the failure-mode regressions
  (smoothing must not be applied to zigzag loops; seams must be byte-exact;
  chains must never share vertices).
- **`AGENTS.md`** (repo root) — the workflow manual + the trap catalogue.

### Rasterizer CLI
- **`--view <ox> <oy> <sc>`** — pin the view transform. `--view 0 0 1` makes
  document coordinates identical to image pixels; without it the auto-fit
  (50 px margin + centering) silently shifts/scales output, which used to
  make any pixel-exact comparison against a source image meaningless.
- **`--debug-overlay`** — raw 1.5 px edge guides and red vertex markers are
  now opt-in instead of baked into every render/SVG. Clean images by default.

### Examples & build
- **`examples/llm_workflow_demo/`** — synthetic line-art source converted
  end-to-end by the toolkit (`run.sh`); scores tol6 P≈0.98 / C≈0.97 via the
  SVG path and P≈0.98 / C≈0.99 through the C rasterizer (`--view 0 0 1`).
- **`make llm-test`, `make llm-demo`**.

## Changed
- `tests/run_tests.sh` — the node-transform regression (which asserts on the
  red vertex markers) now passes `--debug-overlay`.
