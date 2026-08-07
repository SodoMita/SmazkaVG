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
