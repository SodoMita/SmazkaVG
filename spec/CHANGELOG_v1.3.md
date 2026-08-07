# SmazkaVG v1.3 — Changelog (from v1.2)

> Covers the v1.3 rasterizer (distance-field rendering + new primitives) and
> the v1.3.1 audit pass (hardening + algorithm fixes). Each entry maps to a
> weakness identified in review.

---

## v1.3 — Distance-Field Rasterizer & Primitive Expansion

### 1. Rasterization: tessellation-free distance fields

**Problem:** v1.2 rasterized curves by tessellation (adaptive subdivision with
forward differencing). Flatness criteria were fragile, and thin/tapered anime
lines looked stepped.

**Fix:** The rasterizer now renders per-pixel from the **distance field** of
each curve: for every pixel inside the curve's hull, Newton iteration on the
closest-point equation `(P−B(t))·B′(t) = 0` yields the distance and the edge
parameter `t`. Strokes are anti-aliased bands of the distance field; no
tessellation is involved.

### 2. Curve types: quad, rational, catmull

**Problem:** v1.2 shipped cubic Béziers, B-splines and Spiro; the reference
rasterizer implemented only segments and cubics.

**Fix:** v1.3 adds `type=quad` (quadratic Bézier), `type=rational` (conic with
a center weight), and `type=catmull` (Catmull-Rom through the endpoints, with
automatic neighbor selection). Vertex types (`smooth`, `symmetric`, `auto`)
adjust handles at parse time, Inkscape-style.

### 3. New primitives: arc (`r`) and ellipse (`z`)

**Problem:** circles and rounded corners required 4 cubic Béziers each.

**Fix:** two first-class primitives:
- `r <id> <cx> <cy> <r> <a0> <a1> <color> [lw]` — circular arc
- `z <id> <cx> <cy> <rx> <ry> [rot] <fill> [stroke] [sw]` — rotated ellipse

### 4. View fit includes control points

**Problem:** curved geometry was clipped when control points extended beyond
the vertex bounding box (e.g. an S-curve with outlying handles).

**Fix:** the auto-fit view now includes edge control points, arc extents and
ellipse extents.

### 5. Face inline fill

**Problem:** fills required a separate `p solid_fill` record even for trivial
cases.

**Fix:** faces accept an optional inline `RRGGBB` fill field; `p solid_fill`
still overrides it.

---

## v1.3.1 — Audit Pass (hardening & algorithm fixes)

### 1. Memory safety: every ID is bounds-checked (was: OOB writes → crash)

**Problem (critical):** `v 999999 0 0` or a dangling edge reference crashed
the rasterizer (confirmed: SIGSEGV) because IDs indexed fixed arrays without
bounds checks. Negative IDs silently corrupted memory. The s/a/c/p sections
wrote past their fixed arrays without capacity checks.

**Fix:**
- All primitive IDs (v/e/f/s/n/r/z) are validated against their array
  capacities **before any write**; out-of-range records are ignored with a
  warning.
- Constraint sections (`s`/`a`/`c`, 256 each; `p`, 128) enforce capacity.
- A post-parse validation pass drops edges/faces/strokes that reference
  missing primitives (edges may legally reference later declarations, so
  cross-references are validated after the whole file is read).

### 2. Face fills: ordered boundary + ear clipping (was: broken fan)

**Problem:** faces were filled with a triangle fan rooted at
`edges[face.eids[0]].v0`. For any polygon whose first edge's start vertex was
not connected to every other boundary vertex — and for every concave polygon
(L-shapes, crescents, stars) — the fan produced overlapping triangles and
missing areas. Curved edges were filled as straight chords, leaving gaps
between the stroke and the face interior.

**Fix:**
- The face's boundary vertex chain is reconstructed from its edge list (both
  windings tried), then **ear-clipped** (O(n²)) so concave polygons fill
  exactly (verified: 100% fill precision on an L-shape).
- Curved boundary edges are tessellated into the fill polygon, so fills hug
  the true curve (verified: an outward-bulging cubic face fills ~11k px more
  than the straight-chord version; an inward bulge stops short of the chord,
  both correct).

### 3. Strokes: taper profiles actually taper (was: averaged width)

**Problem:** `s 0 0 0000FF 1.0 4.0 6.0 4.0 1.0` rendered as a constant-width
stroke because the renderer averaged the profile into a single width.

**Fix:** the distance-field stroke evaluates the width profile at the pixel's
closest-point parameter `t`, so tapers and power strokes render correctly.

### 4. Distance-field Newton sign (was: cubic/quad strokes invisible)

**Problem:** the closest-point Newton iteration had an inverted step
(`dt = f/fp` instead of `-f/fp`), so the per-pixel distance for cubic and
quadratic edges was wrong — strokes and outlines on those edges silently
failed to render. Only segments (analytic) and Catmull-Rom (accidentally via
dense sampling) rendered.

**Fix:** corrected Newton step. Cubic/quad strokes, outlines, and diffusion
now render.

### 5. Diffusion: curve-aware signed-distance brush (was: straight-line-only)

**Problem:** the diffusion "curve" painted a linear gradient perpendicular to
the straight chord between edge endpoints — it did not follow curved spines,
contradicting the v1.1 spec's PDE claims.

**Fix:** the brush now blends across the signed distance from the curve's
closest point (tangent cross product), so it follows cubic/quad/rational
spines. SPEC.md §5.6.1 documents this honestly: it is an approximation of
Orzan-style diffusion curves; the cotangent-Laplacian PDE solver is future
work.

### 6. Catmull-Rom performance (was: O(E·65) per pixel)

**Problem:** the Catmull-Rom distance path re-searched the edge table to find
neighbor vertices inside a 65-sample loop — O(E·65) per pixel, seconds per
frame.

**Fix:** Catmull-Rom edges are converted to equivalent cubic Béziers **once per
frame**; distance evaluation is O(1). Catmull-heavy scenes render in tens of
milliseconds.

### 7. WebP export: no shell (was: command injection)

**Problem:** `write_webp` built a `system()` command line from the input path;
a file name containing shell metacharacters could execute arbitrary commands.

**Fix:** the converter is invoked via `fork`/`execvp` (convert → ffmpeg → PIL),
with no shell involved.

### 8. Resolver fixes (src/resolver.c)

**Problem:** the v1.1 resolver pseudocode had (a) a DFS that marked only the
two endpoints of a back edge, missing intermediate cycle nodes (A→B→C→A left B
unmarked); (b) a fixed stack[256] that overflowed on long parent chains; (c)
state machines that assigned the uniform blend `w = 1/n` to every state,
making all animation a static average; (d) a `min_dist` LP relaxation
(`x_a−x_b ≥ d/√2` **and** `y_a−y_b ≥ d/√2`) that forced prim_a to always sit
NE of prim_b, over-constraining feasible layouts.

**Fix:** correct DFS with dynamic stack marking every cycle member; state
machines evaluate real triggers (time ramp / event / condition) and normalize
activations per frame; `min_dist`/`collision_free` use sequential linear
programming with directional separation rows. The file now **compiles
standalone** (`cc -DSMZ_STANDALONE src/resolver.c`) with a passing self-test
(cycle detection, acyclic chains, trigger-driven blend weights, bound
clamping, edge_connects repair). LP/QP kernels remain behind `SMZ_HAVE_PSOLVE`.

### 9. Spec synchronization (was: SPEC.md stuck at v1.1)

**Problem:** SPEC.md still described v1.1: a single `c` table, general SMT,
non-convex intervals. The v1.2 changelog (namespace split, convex-only) was
never folded in.

**Fix:** SPEC.md is now v1.3: s/a/c/p sections with binary tags 0x10–0x40,
convex-only solvers with termination guarantees, v1.3 primitives (arcs,
ellipses, curve types, vertex types), enforced limits, honest diffusion and
min_dist semantics, and the tools (§11.2 binary container, §11.3 golf dialect).

### 10. Tools

- `tools/smazka-golf` — code-golf dialect compiler: implicit IDs, relative
  deltas, `P`/`R`/`C` shape primitives, `M` mirror, palette colors. Addresses
  the audit's golfability findings (45% of a naive file is ID bookkeeping; no
  geometric built-ins; verbose keywords).
- `tools/smazka-bin` — compact delta-VLQ binary container with lossless
  round-trip (Line-ASM ⇄ .smvg).

---

---

## v1.3.2 — psolve integration (the LP/QP backend is now real)

**Problem:** SPEC.md and the resolver claimed psolve as the solver backend,
but the LP/QP phases in `src/resolver.c` were pseudocode stubs: nothing was
ever linked or solved. psolve also had no library build target, so there was
no artifact to link against.

**Fix:**
- psolve's Makefile gained `make lib` / `liblp` / `libqp` (static archives;
  `libpsolve.a` covers LP + QP + MIP).
- psolve is vendored as a git submodule (`third_party/psolve`,
  `git submodule update --init`).
- `src/resolver.c` now implements the LP phase against psolve's sparse-CSC
  API: variables are 2V vertex coordinates + S stroke widths; the objective
  is **L1 least-change** (auxiliary deviation pairs); `min_dist` and
  `collision_free` run sequential linear programming with a post-solve
  satisfaction check; `bbox_clamp` maps to variable bounds; `linear_*` map to
  rows. The QP phase solves `fair_blend` (max-entropy weights) and
  `min_stretch` (elastic pull) per constraint with psolve's active-set solver,
  expressing bounds as `Ax ≤ b` rows.
- All psolve calls are wrapped in `psolve_try()`/`psolve_end()` (the library
  aborts on OOM unless a handler is installed).
- `make solver-test` builds the psolve-backed binary; the self-test grows to
  10 checks, including real solves: LP bbox (L1 projection), LP linear
  equality (unique L1 optimum), LP min_dist SLP separation, QP fair_blend
  weights, QP min_stretch with bounds.
- SPEC.md §6 documents the reference backend and the honest stubs
  (`min_curvature` / `ik_target` / `rig`).

---

## Summary

| Aspect | v1.2 | v1.3 / v1.3.1 |
|---|---|---|
| Rasterization | Tessellation | **Per-pixel distance fields** |
| Curve types | seg, cubic (spec: bspline, spiro) | seg, quad, cubic, rational, catmull |
| Primitives | v, e, f, s, n | + **arc, ellipse** |
| Face fills | Fan triangulation (broken) | **Boundary reconstruction + ear clipping** |
| Stroke width | Profile parsed, averaged | **Profile evaluated per t (real taper)** |
| Diffusion | Straight-line brush | **Curve-aware signed-distance brush** |
| Parser safety | OOB writes (crashes) | **Fully bounds-checked** |
| Resolver | Non-compiling pseudocode | **Compilable reference + self-test** |
| Spec | v1.1 drift | **v1.3 (namespaces, convex-only, tools)** |
