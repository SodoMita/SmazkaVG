# SmazkaVG

**A flat, binary-first vector graphics format with embedded LP + convex QP constraint solvers.**

> Version 1.3.1 — Specification, reference rasterizer, resolver, and tooling

---

## What is SmazkaVG?

SmazkaVG is a next-generation vector graphics format designed for:

- **Anime/Illustration production** — topology-aware shared edges, diffusion curves, variable-width strokes
- **CAD/Web interchange** — deterministic fixed-point arithmetic, compact binary encoding
- **LLM-friendly editing** — flat Line-ASM textual projection (~50% fewer tokens than JSON/XML)
- **Code golf / generative art** — a byte-starved dialect compiler (`tools/smazka-golf`)

### Key Design Decisions

| Principle | Implementation |
|---|---|
| **Flat Document Model** | No nested trees or DAGs. All primitives and constraints in flat lists with global IDs. Hierarchy emulated via `parent` constraints. |
| **Fixed-Point Determinism** | Q16.16 (32-bit) or Q32.32 (64-bit) saturating arithmetic. No IEEE 754 in storage. |
| **Decidable Constraints** | Constraint language restricted to **LP + convex QP** (four typed sections `s`/`a`/`c`/`p`). Non-convex QP is a parse error. Termination guaranteed by `MAX_ITER` / `MAX_MS`. |
| **First-Class Curves** | Edges are shared 1D spines: segment, quadratic/cubic/rational Bézier, Catmull-Rom. Strokes are width profiles along the spine. |
| **Bounded Computation** | All solver iterations capped by `MAX_ITER` (default 64) and `MAX_MS` (default 50ms). |

## Repository Layout

```
SmazkaVG/
├── README.md                          # This file
├── Makefile                           # make / make test
├── spec/
│   ├── SPEC.md                        # Formal specification (v1.3)
│   ├── CHANGELOG_v1.2.md              # v1.1 -> v1.2 changelog
│   └── CHANGELOG_v1.3.md              # v1.2 -> v1.3 / v1.3.1 changelog
├── src/
│   ├── rasterizer.c                   # Reference rasterizer (BMP/WebP/SVG/ASCII)
│   └── resolver.c                     # Constraint resolver (compilable, self-test)
├── tools/
│   ├── smazka-golf.c                  # Code-golf dialect compiler
│   └── smazka-bin.c                   # Compact delta-VLQ binary container
├── examples/
│   ├── triangle_v1.2.smazka           # Curved-edge face fill
│   ├── eyelash_v1.2.smazka            # Tapered anime lash + diffusion
│   ├── curves_v1.3.smazka             # All curve types + arc + ellipse
│   └── golf_face.sg / golf_face.smazka# Golf dialect demo
└── tests/
    └── run_tests.sh                   # Build + hardening + regression + round-trip
```

## Quick Start

```sh
make && make test

# render an example
./build/smazka-raster examples/curves_v1.3.smazka 512 512
#   -> curves_v1.3.bmp/.webp/.svg/.txt

# resolver self-test
./build/resolver-test

# golf dialect: one-token shapes, implicit IDs
./build/smazka-golf examples/golf_face.sg face.smazka
./build/smazka-raster face.smazka 512 512

# binary container: lossless Line-ASM <-> .smvg
./build/smazka-bin enc face.smazka face.smvg
./build/smazka-bin dec face.smvg face2.smazka
```

## Example (Line-ASM)

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

The LP solver is provided by [psolve](https://github.com/SodoMita/psolve) (AVX-512 revised simplex) when
`SMZ_HAVE_PSOLVE` is defined; without it the resolver compiles as a reference with a passing self-test.

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
