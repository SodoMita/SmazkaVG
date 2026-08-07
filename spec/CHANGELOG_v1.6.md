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

## Fixed
- **`write_png` block slicing** — stored-deflate blocks are now emitted in
  exact 65535-byte units, so the precomputed `IDAT` length always matches the
  actual stream. The old row-triggered flush produced more blocks than the
  header claimed and corrupted every PNG with a larger frame (e.g. a
  1350×2268 reference image would not decode at all).
- **Polyline tessellation dropped authored knots** (`tools/llm/geometry.py`) —
  every knot is now preserved (phase resets per segment), which is what makes
  exact-coordinate butt seams survivable through tessellation.
- **Raised document caps for whole-figure conversions** (SPEC §9.1):
  V/E/S 4096→32768, F 4096→1024, edges-per-face 64→512. A real hand-authored
  figure (~450 strokes, body loops of hundreds of points) previously fell
  over the parser limits with warnings and dropped records.
- **`tools/llm/author.py`** — `st(..., white=True)` closed white micro-objects
  (iris rings, petals); object loops support filled extras + `w=0` fill-only
  silhouettes; face tessellation is adaptive to stay under the 512-edge cap.

### Examples & build
- **`examples/llm_workflow_demo/`** — synthetic line-art source converted
  end-to-end by the toolkit (`run.sh`); scores tol6 P≈0.98 / C≈0.97 via the
  SVG path and P≈0.98 / C≈0.99 through the C rasterizer (`--view 0 0 1`).
- **`make llm-test`, `make llm-demo`**.

## Changed
- `tests/run_tests.sh` — the node-transform regression (which asserts on the
  red vertex markers) now passes `--debug-overlay`.

---

# v1.6.2 — authoring skin (dialect), lint mode, corruption fixes

The `.smazka` file itself is now the hand-editable artifact: a sugar layer
(SPEC §7.4) that every reader expands deterministically at parse time.

## Added

- **`src/xauthor.h`** — the authoring-skin expander, wired into
  `rasterizer.c` `parse()` and `tools/smazka-bin.c` (enc decodes sugar-free
  containers). Symbolic bare-word ids in all id/reference positions
  (namespaces v/e/f/s/path/group; decimal tokens always numeric);
  `path <name> [closed] [seg|catmull] [w=W] [cap=..] [color=..] items..`;
  `fobj <name> [fill=HEX] [sw=W] [cap=..] [seg|catmull] items..`
  (implicitly closed, `sw=0` fill-only, auto `s group_id` inside `group`);
  items are `x,y` points or splices `use|rev` (shared-spine) / `useg|revg`
  (ghost) of a prior `path`. Seam joins: ≤0.5 px silent, ≤2 px bridge+warn,
  otherwise an expansion error.
- **`--xpand <out.smazka>`** rasterizer mode: expand-only lint pass; errors
  land on stderr and inline as `# xa ERROR` comments.
- **Id-redefinition guard**: every issued definition is tracked per
  namespace; redefining an id (e.g. numeric `v 0` after symbol `v a`)
  is now an expansion error instead of a silent overwrite.
- **`tests/dialect.smazka` fixture + suite block** (xpand lint, record
  counts, collision probe, pixel semantics, bin round-trip raster equality).

## Fixed

- **Xpander output corruption on records > 8 KB**: `xa_put` trusted
  `vsnprintf`'s would-be length, copying bytes never written; and a
  truncated echo swallowed its newline so the *next* emitted record glued
  onto a comment tail and vanished for parsers (a dropped vertex looked
  like a stroke to the origin — the "diagonal from (0,0)" bug).
- **Face fill byte order**: an 8-hex (`RRGGBBAA`) face fill was stored as
  raw u32 and resolved like `RRGGBB`, shifting channels (`FFCCCCFF` →
  `CCCCFF`). Alpha is now dropped at parse.
- **Plain `f` color vs. edge name**: trailing all-hex tokens (`FF6B6B`)
  satisfied the bare-name rule and were looked up as symbolic edges —
  broke old files with hex-letter fills (animation_demo, donut).
- **OOB write on huge numeric vertex ids** (`v 999999 ...`) in the
  expander's tracking arrays (segfault, caught by the evil1 probe).
- **`--out` ignored for single-frame renders** (was frame-sequence-only).
- `MAX_FE` 512 → 1024 (tessellated body loops exceed 512 boundary edges);
  e/f/s numeric heads now bump their namespace counters (latent collision
  with chain-minted ids); test script links `smazka-bin` with `-lm`.

---

# v1.6.3 — audit: constraints that SHOW the garbage (llm-vectorization)

The authoring skin made the document editable; this release makes it
*accountable*. A vectorization document is a claim ("there is ink here,
of this width, owned by this object"); claims need constraints that
rat out violations instead of silently absorbing them.

## Added

- **`tools/llm/audit.py` — the garbage detector.** XPands the document,
  attributes every stroke back to its named record via the `# |` echoes,
  then checks each stroke against the *source image* and the *document's
  own z-order*:
  - **stray** — rendered ink whose distance to the nearest source ink
    exceeds the tolerance (phantom strokes; distance field on the source).
  - **hidden** — strokes fully covered by a later opaque face in the
    document's own paint order (dead ink: authored, paid for, invisible).
  - **dup** — stroke pairs whose resampled polylines shadow each other
    (mean < 1.5 px, max < 4 px): two claims for one line.
  - **join** — stroke endpoints hovering within striking distance
    (0.75–3.5 px) of a vertex they failed to land on: seam drift.
  - **degen** — duplicate consecutive points and other collapsed spans.
  Emits a text report *and* an `overlay.smazka` that paints the findings
  in place (red stray / magenta dup / blue hidden / orange join, over a
  gray ghost of the drawing) — render it and LOOK, don't read tea leaves.
  On the 1350×2268 surfer the first baseline found 66 stray (one ~1400 px
  phantom arm cluster ~61 px off-source), 20 dups and 5 degens; fixing
  what it showed moved tol-6 precision/coverage from .8045/.8371 to
  .8508/.8576.

- **Seam edges are never stroked (ns flags).** `useg|revg` ghost splices
  and auto-bridged seam joints are marked "no-stroke" in `XaChain` and
  skipped by `xa_strokes` — a wrist chord no longer paints across the palm
  when the boundary walks through it.

- **Auto-fit now announces itself.** Rendering without `--view` auto-fits
  with a ~50 px margin; for a document authored in image pixels that is a
  silent ~5 % scale+offset lying to every pixel-distance check (and to
  `verify`). The rasterizer prints the applied transform once per run and
  points at `--view 0 0 1`. (AGENTS.md §1.9 has mandated the pinned view
  since v1.6; now the tool itself says so when you forget.)

## Doctrine notes (consumed by audit, worth restating)

- **Fill-only silhouettes + explicit ink runs.** `fobj … sw=0` plus named
  `path` strokes for exactly the boundary runs the source *inks* — the
  default `sw>0` outline claims ink along the *whole* loop, which is a lie
  on every occluded/merged run. sw=0 + paths = no phantom hugs.
- **A stroke is a claim on the source.** If you cannot point at the source
  run (`imgscan.row_runs/col_runs`) it paints, expect it in next audit's
  stray list. If it is intentionally occluded, expect it in hidden —
  hidden is a *review queue*, not an error.
