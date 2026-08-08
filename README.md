# SmazkaVG

**A flat, binary-first vector graphics format with embedded LP + convex QP constraint solvers.**

> Version 1.7 — Specification, reference rasterizer, resolver, compact text dialect, and tooling

---

## What is SmazkaVG?

SmazkaVG is a next-generation vector graphics format designed for:

- **Anime/Illustration production** — topology-aware shared edges, Poisson diffusion curves, variable-width strokes with caps/joins, node transforms, hole-aware faces, keyframe animation
- **CAD/Web interchange** — deterministic fixed-point arithmetic, compact binary encoding
- **Ultra-compact text representation** — high-density compact shorthand dialect (`.sg` / `.smz`) providing 50% to 93% text size reductions over raw Line-ASM without losing semantic clarity or precision
- **LLM-friendly editing** — flat textual projections and authoring skin (`xauthor.h`) natively expanded at parse time by all tools
- **LLM-driven vectorization** — measurement + authoring + verify toolkit (`tools/llm`, see [AGENTS.md](AGENTS.md)) that turns raster line art into SmazkaVG with dot-first precision — no trace tools
- **Code golf / minification** — a high-density dialect compiler and minifier (`tools/smazka-golf`)

### Key Design Decisions

| Principle | Implementation |
|---|---|
| **Flat Document Model** | No nested trees or DAGs. All primitives and constraints in flat lists with global IDs. Hierarchy emulated via `parent` constraints. |
| **Fixed-Point Determinism** | Q16.16 (32-bit) or Q32.32 (64-bit) saturating arithmetic. No IEEE 754 in storage. |
| **Decidable Constraints** | Constraint language restricted to **LP + convex QP** (four typed sections `s`/`a`/`c`/`p`). Non-convex QP is a parse error. Termination guaranteed by `MAX_ITER` / `MAX_MS`. |
| **First-Class Curves** | Edges are shared 1D spines: segment, quadratic/cubic/rational Bézier, Catmull-Rom. Strokes are width profiles along the spine. |
| **Compact Text Dialect** | Single-pass shorthand primitives (`P`, `R`, `C`, `+`, `E`, `S`, `F`, `p`, `f`), palette color names, 3/4-hex colors, relative delta coords. |
| **Bounded Computation** | All solver iterations capped by `MAX_ITER` (default 64) and `MAX_MS` (default 50ms). |

## Repository Layout

```
SmazkaVG/
├── README.md                          # This file
├── Makefile                           # make / make test / make solver-test
├── spec/
│   ├── SPEC.md                        # Formal specification (v1.7)
│   ├── CHANGELOG_v1.2.md              # v1.1 -> v1.2 changelog
│   ├── CHANGELOG_v1.3.md              # v1.2 -> v1.3 / v1.3.1 changelog
│   └── CHANGELOG_v1.6.md              # v1.6 changelog
├── src/
│   ├── rasterizer.c                   # Reference rasterizer (BMP/WebP/SVG/ASCII)
│   ├── resolver.c                     # Constraint resolver (LP/QP via psolve)
│   └── xauthor.h                      # Parse-time expansion for authoring skin & compact dialect
├── third_party/
│   └── psolve/                        # LP + convex QP solver (git submodule)
├── tools/
│   ├── smazka-golf.c                  # Code-golf dialect compiler & minifier (-c)
│   ├── smazka-bin.c                   # Compact delta-VLQ binary container
│   └── smazka-solve.c                 # Apply the constraint solver to a file
├── examples/
│   ├── triangle_v1.2.smazka           # Curved-edge face fill
│   ├── eyelash_v1.2.smazka            # Tapered anime lash + diffusion
│   ├── curves_v1.3.smazka             # All curve types + arc + ellipse
│   ├── diffusion_demo.smazka          # Poisson diffusion curves
│   ├── joints_demo.smazka             # Stroke caps & joins
│   ├── donut.smazka                   # Face holes (even-odd fill)
│   ├── nodes_demo.smazka              # Node transforms
│   ├── animation_demo.smazka          # Keyframe animation (frame sequence)
│   ├── solve_demo.smazka              # smazka-solve constraint demo
│   ├── golf_face.sg / golf_face.smazka# Golf dialect demo
│   └── llm_workflow_demo/             # End-to-end LLM dot-first vectorization demo
├── tools/llm/                         # LLM vectorization toolkit (see AGENTS.md)
│   ├── imgscan.py                     #   measure the source raster (runs, ascii, probes)
│   ├── author.py                      #   dot-first strokes/objects -> .smazka / .sg + preview .svg
│   ├── verify.py                      #   precision/coverage metric + red/blue overlays
│   ├── audit.py                       #   per-stroke garbage report: stray/hidden/dup/join/degen + overlay
│   ├── geometry.py                    #   catmull/polyline tessellation, chains, trimming
│   └── selftest.py                    #   toolkit unit tests
└── tests/
    └── run_tests.sh                   # Build + hardening + regression + solver + round-trip + compact tests
```

## Quick Start

```sh
make && make test

# render an example (PNG + BMP + WebP + SVG + ASCII)
./build/smazka-raster examples/curves_v1.3.smazka 512 512
#   -> curves_v1.3.png/.bmp/.webp/.svg/.txt

# compact text representation minifier: compress Line-ASM down by 50-93%
./build/smazka-golf -c examples/triangle_v1.2.smazka min_triangle.sg
#   1652 bytes -> 117 bytes (93% reduction!)

# render compact shorthand files directly (xauthor.h expands at parse time)
./build/smazka-raster min_triangle.sg 512 512

# golf dialect compiler: shorthand -> Line-ASM
./build/smazka-golf examples/golf_face.sg face.smazka
./build/smazka-raster face.smazka 512 512

# binary container: lossless Line-ASM <-> .smvg
./build/smazka-bin enc face.smazka face.smvg
./build/smazka-bin dec face.smvg face2.smazka

# authoring/debug aids:
#   --view 0 0 1        pin document coords to image pixels (no 50px auto-fit)
#   --debug-overlay     show raw edge guides + red vertex markers
./build/smazka-raster face.smazka 512 512 --view 0 0 1

# authoring skin: symbolic ids, path/fobj/group, seam splices —
# the .smazka itself is the hand-edited artifact (SPEC §7.4, AGENTS.md §8)
./build/smazka-raster tests/dialect.smazka --xpand -      # lint/expand only
./build/smazka-raster tests/dialect.smazka 480 240 --view 0 0 1 --out build/dialect

# LLM dot-first vectorization workflow (requires python3, Pillow, numpy;
# cairosvg for the svg preview path) — full manual in AGENTS.md
make llm-demo   # convert a synthetic line-art source end-to-end
make llm-test   # toolkit unit tests
```

## Compact Text Example (.sg / shorthand)

```
# Ultra-compact text representation (parsed directly by rasterizer)
P 0 0 100 0 50 86 #f0f0ff sw=3
E 0 1 type=cubic 80 30 60 70
S 0 blue 3
S 1 red 1 4 1
```

## Line-ASM Equivalent

```
# Triangle with a curved edge, tapered stroke, and diffusion
v 0 0 0
v 1 100 0
v 2 50 86
e 0 0 1
e 1 1 2 type=cubic 80 30 60 70
e 2 0 2
f 0 0 1 2 F0F0FF          # face with inline fill
s 0 0 0000FF 3 3          # constant stroke
s 1 1 FF0000 1 4 1        # tapered stroke (t=0,0.5,1)
p 0 diffusion 1 L FFE0D0 R 4A3020
```

## Solver Architecture

```
                    ┌──────────────────┐
                    │  Record stream   │
                    │  (s / a / c / p) │
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │   Classifier     │
                    └────────┬─────────┘
                             │
              ┌──────────────┼──────────────┐
              │              │              │
     ┌────────▼──────┐ ┌────▼─────┐ ┌──────▼──────┐
     │ Structural    │ │  LP      │ │ Convex QP   │
     │ + assertions  │ │ (psolve  │ │ (active-set │
     │ (no solver)   │ │  simplex)│ │  over psolve)│
     └───────────────┘ └──────────┘ └─────────────┘
```

The LP/QP backend is [psolve](https://github.com/SodoMita/psolve) (AVX-512
revised-simplex LP + active-set convex QP), vendored as a git submodule
(`third_party/psolve`, `git submodule update --init`).  With `SMZ_HAVE_PSOLVE`
defined the resolver's LP phase solves `min_dist` (sequential LP), `bbox_clamp`,
`linear_*`, and `collision_free` with an L1 least-change objective, and the QP
phase solves `fair_blend` / `min_stretch` via psolve's active-set solver
(bounds as rows).  Without it the resolver compiles as a reference with a
passing self-test.  `make solver-test` builds the psolve-backed binary; the
self-test then runs 10 checks including real LP and QP solves.

## Security notes (v1.3.1)

- Every global ID is bounds-checked before any write; crafted files with huge
  or negative IDs are rejected with warnings instead of crashing.
- Post-parse validation drops dangling edge/face/stroke references.
- WebP export uses `fork`/`exec` (no shell) — no command injection via filenames.
- Document limits are documented in SPEC.md §9 and enforced by the rasterizer.

## Dependencies

- **psolve** — LP solver backend (optional, for `SMZ_HAVE_PSOLVE`): https://github.com/SodoMita/psolve
- **convert / ffmpeg / PIL** — any one of these, used only for WebP export
- **zstd** — optional payload compression (RFC 8878)

## License

TBD

## Specification

Read the full formal specification at [`spec/SPEC.md`](spec/SPEC.md).
