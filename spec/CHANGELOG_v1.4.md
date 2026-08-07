# SmazkaVG v1.4 — Changelog (from v1.3.2)

> Feature pass on top of the v1.3.x audit-fix series.  Every entry maps to a
> weakness identified in review or a gap in the v1.3 reference implementation.

---

## 1. Diffusion curves: real Poisson solve (was: signed-distance brush)

**Problem:** v1.3 replaced the v1.1 straight-line gradient with a
signed-distance curve brush, but the SPEC's headline promise — Orzan et al.
diffusion curves, a smooth PDE field that follows curved spines — was still
unimplemented. The brush was local and did not solve a field equation.

**Fix:** `p diffusion` now solves the discrete Laplace equation over the
framebuffer region around the edge:

- **Dirichlet conditions**: `left_color` / `right_color` painted onto pixels
  within ~2.5px of the curve, classified by the sign of the tangent cross
  product (sides relative to the direction of travel).
- **Region border** held at the existing artwork, so the field diffuses
  smoothly into the fill.
- Solved by **SOR** (ω = 1.6) with bounded, deterministic iterations
  (max-change < 1/4 level early exit; count capped by region size).

Verified: boundary colors match L/R exactly, the gradient is smooth and
harmonic (uniform slope, monotone), the field follows a curved cubic spine
(the 50/50 midline tracks the curve within ±3 world units vs ±43 for the old
brush), falls off to the background far from the curve, is byte-identical
across reruns, and renders in 0.1–0.4 s.

## 2. Stroke caps & joins (was: raw distance-field ends)

**Problem:** strokes ended as flat cuts perpendicular to the tangent; corners
where two stroked edges met left an unfilled notch (SVG `stroke-linecap` /
`stroke-linejoin` equivalents were missing).

**Fix:**
- `cap=round` (default): endpoint half-disks via distance-to-endpoint-point.
- `cap=butt`: flat cut (nothing beyond the endpoint).
- `cap=square`: rectangle extending half the local width along the tangent
  (segments; curves fall back to butt).
- Because round caps are drawn per stroke, a vertex shared by ≥2 stroked edges
  automatically forms a **round join** (the half-disks overlap and close the
  notch — verified: round join fills 438 px of the corner where butt leaves 0).

Miter/bevel joins are documented future work (they need path-level geometry).

## 3. Native PNG output (was: BMP + external WebP only)

**Problem:** the only self-contained raster output was 24-bit BMP (3 bytes per
pixel, no compression); WebP required an external converter.

**Fix:** `write_png` is fully self-contained — CRC-32 chunks, a zlib stream
built from stored (uncompressed) deflate blocks, and ADLER-32 — no external
dependencies. PNG decodes and matches the BMP pixel-for-pixel (verified at 256
and 512, including the multi-block path). `smazka-raster` now emits
`.png .bmp .webp .svg .txt`.

## 4. Solve pipeline: tools/smazka-solve (was: resolver never applied to files)

**Problem:** the resolver solved in-memory documents, but no tool connected
Line-ASM files to it, so constraints (`min_dist`, `bbox_clamp`, `linear_*`)
never actually moved geometry in a document.

**Fix:** `tools/smazka-solve` parses a Line-ASM document into the resolver's
model, runs `smazka_resolve` (structural / assertions / automata / LP / QP via
psolve), and writes the resolved document back (vertices and node translations
updated, everything else verbatim). Verified on `examples/solve_demo.smazka`:
`min_dist ≥ 50`, `bbox_clamp`, and `linear_eq` all enforced in the output.

## 5. Resolver: `rig` QP implemented (was: stub)

**Problem:** `C_RIG_EQUILIBRIUM` (cyclic-parent equilibrium, v1.2 semantics)
was a documented stub.

**Fix:** translation-only equilibrium QP over the cycle members' world
translations: `min Σ‖x − x0‖² + λ Σ‖x_child − x_parent − t_child‖²` (λ = 1e6),
solved with psolve's active-set solver. Cycle membership is marked by
`resolve_hierarchy` (node flags bit 0). Verified (solver test 11): a consistent
3-node cycle A→B→C→A with local offsets (10, 5, −15) resolves to world offsets
satisfying every edge within 1e-2. Rotation/scale rigs remain future work.

---

## Summary

| Aspect | v1.3.2 | v1.4 |
|---|---|---|
| Diffusion | Signed-distance brush | **Discrete Laplace (Poisson) solve, SOR, deterministic** |
| Stroke caps | None (raw ends) | **round / butt / square** |
| Stroke joins | Notch at corners | **Round joins** (miter/bevel future) |
| Raster output | BMP (+ external WebP) | **Self-contained PNG** + BMP/WebP/SVG/ASCII |
| Resolver applied to files | Not possible | **tools/smazka-solve** |
| Resolver `rig` | Stub | **Translation-only equilibrium QP** |
| Tests | 24 | **37** (PNG, caps/joins, diffusion, solve pipeline, rig) |

---

## v1.4.1 — Node transforms, face holes, fixed-point PGS, parser fixes

### 1. Node transforms are applied (was: parsed, never used)

**Problem:** the rasterizer parsed `n` records (tx/ty/rot/sx/sy/skew/content)
but never applied them — a core pillar of the flat model ("hierarchy emulated
via transforms") was dead code.

**Fix:** `apply_node_transforms()` bakes each node's affine transform into the
vertex store before rendering: `content_ref` resolves to a vertex (transform
the position) or an edge (transform both endpoints + control points, so curved
edges move with their geometry). Convention per node:
`p' = R(rot)·(S(sx,sy)·p + skew) + t`; nodes apply in ID order (deterministic
composition). Verified: a translated vertex lands at the expected pixel and a
90°-rotated vertex lands at its rotated position (examples/nodes_demo.smazka).

### 2. Face holes (donuts / counters)

**Problem:** faces had a single boundary — no holes, so donuts and letter
counters were impossible.

**Fix:** Line-ASM `f <id> <outer edges...> | <hole edges...> [| ...] [fill]`;
holes fill with the **even-odd rule** across all loops (curved hole edges are
tessellated). Hole-free faces keep the fast ear-clipping path. The SVG
projection emits `fill-rule="evenodd"` subpaths. Verified: a square-with-square-
hole donut leaves the hole empty and fills the ring; a curved (cubic) hole
behaves identically (examples/donut.smazka).

### 3. Resolver rig on fixed-point PGS

The `rig` equilibrium QP now uses psolve's **fixed-point projected
Gauss-Seidel** solver (`pgs_fixed.h`): `min Σ‖x−x0‖² + λΣ‖x_c−x_p−t‖²` built
as an integer boxed QP at Q16.16 scale (A raw, b/lo/hi/x scaled), plain GS,
deterministic full budget scaled by system size. The solve is integer-exact —
bit-identical across platforms, matching the format's fixed-point core.
Solver test 11 still passes (deltas 5/−15/10 exact, ~36 ms).

### 4. Parser fixes (found while adding holes)

- **Digit-leading fill colors**: a face fill like `88AA00` (or any color whose
  first two hex digits are decimal digits) was parsed as an edge id — a
  latent bug in both the rasterizer and the binary encoder. Face parsing is
  now token-based: edge ids are pure, short decimal tokens; fill colors are
  6/8 hex tokens.
- **Inline comments**: `f 0 0 1 2 # note` leaked `#` into the fill field.
  Tokens are checked after skipping whitespace, so `#` ends the record.
- **Binary container**: face records now encode/decode hole loops, and face
  fills round-trip as full 8-digit RRGGBBAA (a 6-digit face fill with a
  nonzero high byte previously lost its top byte, e.g. `FFAA00` → `AA00FF`).

---

## Summary (v1.4.1)

| Aspect | v1.4 | v1.4.1 |
|---|---|---|
| Node transforms | Parsed, never applied | **Applied (vertex/edge content, ID-order composition)** |
| Face holes | None | **`|` hole loops, even-odd fill (curved holes ok)** |
| Rig solver | Dense active-set QP | **Fixed-point PGS (integer-exact Q16.16)** |
| Face parser | Digit-leading fills broken | **Token-based, comment-safe** |
| Binary container | No holes; 6-digit fills lossy | **Holes + 8-digit fills** |
| Tests | 37 | **44** (holes, node transforms, digit-fill, round-trips) |
