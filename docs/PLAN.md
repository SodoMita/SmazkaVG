# SmazkaVG Roadmap — Safe/Unsafe Split, Interchange & Vectorization

> Status: living plan. Items marked **[done]** are implemented on `main`;
> the rest are ordered by impact. Revision of this document: 2026-08-07.

---

## 1. Motivation

Vector formats are powerful precisely because they are *resolution-independent
and self-contained*. Three features that are extremely useful in practice
partially defeat that goal:

| Feature | Why it is useful | Why it is "unsafe" for a pure VG format |
|---|---|---|
| **Raster embed** (`img`) | Photos, textures, scanned line art | Not vector; pulls in pixel data, file references, MIME handling |
| **Text with fonts** (`t`) | Real copy, accessibility, editability | Requires font references; rendering varies by system/font |
| **Include** (`inc`) | Reuse, multi-file authoring, libraries | External references break self-containment |

**Decision:** SmazkaVG keeps a strict *safe* core, and these live in a clearly
separated **`smazkavg_unsafe`** superset. Any unsafe document can be converted
to a safe one:

- raster images → **stripped** (or optionally **vectorized** to paths, §6)
- text → **vectorized** to outline paths (§7)
- includes → **inlined** ([done] `smazka-sanitize` inlines them)

This mirrors how PDFs/EPUBs define "tagged" vs "raw" and how Web builds
separate dev bundles from production. The unsafe format is allowed to carry
whatever a user wants; the safe format is the interchange/archive format.

---

## 2. Extension & naming conventions

- **[done]** Canonical extension: **`.smazkavg`** (renamed from `.smazka`).
  Legacy `.smazka` inputs are still accepted by every tool (verified by test);
  we keep the old extension working for at least one minor version.
- **Unsafe documents**: `.smazkavg_unsafe` (e.g. `art.smazkavg_unsafe`).
- **Solved documents**: `smazka-solve` writes the constraint-solved result to
  `<name>.solved.smazkavg` (safe) or `<name>.solved.smazkavg_unsafe` (if the
  input was unsafe and was not sanitized first).
- **Sanitized documents**: `smazka-sanitize` writes `<name>.smazkavg`
  (stripping the `_unsafe` suffix if given a `.smazkavg_unsafe` input).

Naming rules are enforced by tools, not by the grammar (the grammar only
distinguishes the *records*).

---

## 3. Unsafe record set (`smazkavg_unsafe`)

Grammar additions (all marked unsafe):

```
# Text (UNSAFE: font reference)
t <id> <x> <y> [size=..] [font=..] [weight=..] [align=..] <"quoted text">

# Raster embed (UNSAFE: external image)
img <id> <x> <y> [w=..] [h=..] [rot=..] [opacity=..] <path.png|jpg|webp>

# Include (UNSAFE until inlined; safe once inlined)
inc <path.smazkavg|path.smazkavg_unsafe>

# Font library declaration (UNSAFE)
font <id> <path.ttf|otf> [name=..]
```

Status:
- **[done]** `inc` — parsed and **inlined at parse time** in the rasterizer
  (relative paths, depth-limited to 8, cycle-guarded, global line budget);
  `smazka-sanitize` inlines them for output.
- **[done]** `t` / `img` — the safe rasterizer **warns and skips** them;
  `smazka-sanitize` strips them from output.
- **TODO** `font` record — parse + warn (text vectorization work, §7).

A file is "safe" iff it contains no `t`/`img`/`font` records (includes are
fine because they are inlined). `smazka-sanitize` prints a summary of what it
stripped/inlined.

---

## 4. Safe/unsafe conversion pipeline

```
 .smazkavg_unsafe ─┐
                   ├─► smazka-sanitize ─► .smazkavg (self-contained, safe)
 .smazkavg (unsafe records) ─┘         │
                                        ├─ (future) --vectorize: replace img
                                        │   with centerline paths (§6), t
                                        │   with outline paths (§7)
                                        ▼
                       smazka-raster / smazka-bin / smazka-golf
```

Tool contract:
- **`smazka-raster`** — safe renderer; rejects nothing but warns+skips unsafe
  records so an unsafe file still previews.
- **`smazka-sanitize`** — [done] inlines `inc`, strips `t`/`img`/`font`
  (warn per record), copies everything else verbatim (comments preserved),
  writes a header line. Future: `--vectorize-text`, `--vectorize-img`,
  `--check` (exit nonzero if any unsafe record remains).
- **`smazka-bin`** — binary container is *safe-only*: encoding an unsafe
  document fails with a clear error pointing at `smazka-sanitize`. (Decision:
  the binary container is the interchange format; it should never smuggle
  external references. `inc` becomes an inline subtree in binary; `t`/`img`
  are rejected.)
- **`smazka-solve`** — operates on the resolved document model; writes
  `.solved.smazkavg` and preserves sanitized state.

---

## 5. Solved-output variants

The constraint solver (`smazka_resolve`, LP + convex QP via psolve, plus the
combined animation resolve `smazka_resolve_anim`) computes concrete geometry
from constraints. Two output modes are planned:

1. **Bake-in-place** **[done for animation]**: `smazka-solve in.smazkavg
   out.smazkavg --t <seconds>` replaces node transforms with the resolved
   pose and drops the `k`/`a state_machine` records (frame baking).
2. **Solved document**: `smazka-solve in.smazkavg out.solved.smazkavg`
   keeps the constraint records but *appends* a `# solved` section carrying
   the solver's concrete values (vertex positions, node transforms,
   diffusion colors), so a renderer can skip re-solving or verify
   determinism. The same document re-solved must reproduce byte-identical
   output (fixed-point determinism).
3. **Derived naming**: `.solved.smazkavg` for safe, `.solved.smazkavg_unsafe`
   when the source was unsafe and not sanitized.

TODO: implement (2) as a `smazka-solve --solved` mode; add a determinism test
(solve twice → diff clean).

---

## 6. Raster → path centerline vectorization (LP-driven)

The interesting part: instead of just *stripping* `img` records, convert them
to vector paths automatically using the constraint solver.

**Core idea.** Treat the raster as a cost/occupancy field. A path from point A
to point B is built so that it follows the *centerline of dark pixels* by
solving a sequence of linear programs:

1. **Thinning / medial axis**: compute a pixel-skeleton (morphological
   thinning or a distance-transform ridge) of the alpha/ink channel; each
   skeleton branch becomes a candidate polyline.
2. **Graph extraction**: junctions become vertices, skeleton branches become
   edges — this maps 1:1 onto SmazkaVG's `v`/`e` records (shared edges!).
3. **Refinement as an LP/QP** (the solver-specific part): for each branch,
   place the control points so the polyline stays within the ink band and
   minimizes deviation from the skeleton. Concretely:
   - variables: vertex positions `(x_i, y_i)` and (for cubics) control points;
   - constraints: `bbox_clamp` per vertex to the local ink band;
     `linear_eq` to pin endpoints to junctions; `min_dist` between non-adjacent
     branches to keep them separated;
   - objective (QP): minimize curvature energy
     `Σ ‖p_{i-1} − 2p_i + p_{i+1}‖²` (already available as `min_curvature`),
     which yields smooth centerlines from noisy skeletons.
4. **Strokes**: the ink's local thickness per sample becomes the stroke width
   profile `s <id> <eid> <color> <w0> ... <wn>` — a variable-width power stroke
   reproduces calligraphic rasters.

This is exactly the "constraints from raster images → paths" the user
described: `img` → (skeleton) → `v`/`e`/`s` records, with the LP/QP solver
doing the geometric fitting. It also gives SmazkaVG a *differentiator*:
constraint-native auto-tracing (vs. plain threshold tracing in Illustrator/
Inkscape).

**Milestones:**
- M1: `tools/imgscan.py`-style skeleton extraction (borrow the approach from
  the (now-removed) `llm-vectorization` branch's `geometry.py` if recoverable;
  else reimplement: distance transform + ridge following).
- M2: map skeleton to `v`/`e`/`s` records; emit a `.smazkavg_unsafe`-free
  `.smazkavg`.
- M3: LP/QP refinement pass (reuse `smazka-solve` machinery + `min_curvature`).
- M4: `smazka-sanitize --vectorize-img` integration + golden tests on simple
  shapes (circle, star, handwritten stroke) with pixel-tolerance assertions.

---

## 7. Text support plan

`t` records are unsafe because they reference fonts. Plan (in priority order):

1. **Vectorize at authoring time (recommended path)**: a
   `smazka-text` tool converts `t <id> <x> <y> ... "text"` + a `font` record
   into `v`/`e`/`f` outline paths (glyph outlines via FreeType → cubic
   Béziers, converted to SmazkaVG edges + even-odd faces). The result is a
   fully safe document. Accessibility metadata can be preserved as `m`-key
   records (`m text_content <id> "..."`).
2. **Built-in minimal glyph set**: ship a tiny hand-authored vector font
   (A–Z, 0–9, basic punctuation) so `t` renders *without* external fonts in
   the safe rasterizer when the text is ASCII and `font=` is omitted.
   This makes `t` "safe-ish" for a restricted character set.
3. **Explicit `font` record** semantics: `font <id> <path>` is only honored
   by unsafe-capable renderers; safe rendering substitutes the built-in set
   (glyph coverage) or warns.

Milestone: M1 built-in glyph set (a few hundred records in
`data/fonts/basic.smazkavg` included via `inc`), M2 `smazka-text` FreeType
path (optional dependency, like the WebP converters), M3 `t` handling in
`sanitize --vectorize-text`.

---

## 8. Review-driven fixes (from the two-branch audit)

1. **Version drift** **[done]**: banner, stats line, SVG comment, README all
   say v1.6 now; spec revision is 1.6. Add a CI-style test that greps the
   binary banner against `spec/SPEC.md`'s revision so it cannot drift again.
2. **`llm-vectorization` branch** — no longer present on the remote; its
   useful pieces should be re-implemented on `main`:
   - `--view <ox> <oy> <sc>` exact camera control (authoring/regression);
   - `--debug-overlay` opt-in guides/vertex markers (markers are currently
     always drawn in renders and SVG — make them opt-in);
   - raise limits: `MAX_V/MAX_E/MAX_S` 4096 → 32768, `MAX_FE` 64 → 512
     (review notes production drawings need more; keep fixed arrays with
     documented caps, or move to dynamic storage);
   - confirm the stored-PNG block path is correct at large frames (ours
     computes the IDAT length up front and streams 65535-byte stored
     blocks — add a 1024×1024+ PNG round-trip test to prove it).
3. **Baseline VG features** (spec already documents them as future):
   - stroke **joins**: miter/bevel (round exists via caps);
   - **dash arrays**: `dash=<len, gap, ...>` on strokes (renderer-side);
   - **gradients/patterns** as `p` paint records (linear/radial/conic);
   - **clipping/masking**: `clip` record referencing a face + optional
     alpha mask; **blend modes** on strokes/fills;
   - **reusable symbols/instances**: a `sym`/`use` pair (safe: defined
     in-document; `use` is a reference — treat like `inc`, i.e. inline at
     load time to stay safe);
   - **pages/artboards**: a `page` record with `w h [x0 y0]` and per-page
     viewport selection for multi-artboard output.
4. **Interchange/container**: finalize the binary container — header CRC +
   full-document CRC-32C, section versioning, reserved fields, and a
   conformance test suite (round-trip + reject-malformed). The container is
   the place where "safe" is enforced most strictly (§4).
5. **License**: still `TBD` — decide (e.g. MIT for the reference
   implementation + CC0 or Apache-2.0 for the spec) before promoting the
   format; add `LICENSE` + `NOTICE`.

---

## 9. Repository / process

- Single-source-of-truth on `main`; feature work in short-lived branches that
  are rebased onto `main` (the review's "split-brain" risk is avoided by
  merging any `llm-vectorization`-style work back quickly).
- Version policy: one revision number (`spec/SPEC.md` revision) must match
  `rasterizer` banner + README; enforced by a test (§8.1).
- CI: `make test` is the gate (currently 63 checks); add the banner-vs-spec
  check and the large-PNG check.

---

## 10. Milestone summary

| # | Milestone | Scope | Status |
|---|---|---|---|
| 0 | `.smazkavg` rename + legacy compat | all tools/tests/docs | **[done]** |
| 0.5 | Version drift fix | banner/stats/README/spec | **[done]** |
| 1 | `inc` include (inline, depth/cycle guarded) | rasterizer, sanitize | **[done]** |
| 2 | `smazka-sanitize` (inline/strip, safe output) | new tool | **[done]** |
| 3 | `t`/`img`/`font` records in grammar + warnings | rasterizer | **partial** (`t`,`img` warn; `font` TODO) |
| 4 | `--view`, `--debug-overlay`, raised limits | rasterizer | TODO (from llm-vectorization review) |
| 5 | Solved-document output (`--solved`, determinism test) | smazka-solve | TODO |
| 6 | Built-in vector glyph set + `smazka-text` | text (§7) | TODO |
| 7 | Raster→centerline vectorization (skeleton + LP/QP fit) | tools/ + solver (§6) | TODO |
| 8 | Baseline VG features (joins, dashes, gradients, clip/mask, symbols, pages) | rasterizer/spec (§8.3) | TODO |
| 9 | Finalized container (CRCs, conformance suite) | binio (§8.4) | TODO |
| 10 | License decision + `LICENSE`/`NOTICE` | repo | TODO |

**Next small steps (suggested):** (a) `font` record + warnings; (b)
`smazka-solve --solved` with a determinism test; (c) `--debug-overlay` +
`--view`; (d) limit raises + large-PNG test.
