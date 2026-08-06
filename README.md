# SmazkaVG

**A flat, binary-first vector graphics format with embedded SMT + LP/QP constraint solvers.**

> Version 1.1 — Specification & Reference Implementation

---

## What is SmazkaVG?

SmazkaVG is a next-generation vector graphics format designed for:

- **Anime/Illustration production** — topology-aware shared edges, diffusion curves, deformation rigging
- **CAD/Web interchange** — deterministic fixed-point arithmetic, compact binary encoding
- **LLM-friendly editing** — flat Line-ASM textual projection (~50% fewer tokens than JSON/XML)

### Key Design Decisions

| Principle | Implementation |
|---|---|
| **Flat Document Model** | No nested trees or DAGs. All primitives and constraints in flat lists with global IDs. Hierarchy emulated via `parent` constraints. |
| **Fixed-Point Determinism** | Q16.16 (32-bit) or Q32.32 (64-bit) saturating arithmetic. No IEEE 754 in storage. |
| **SMT + LP/QP Hybrid Solver** | SMT handles discrete logic (topology, hierarchy, cycles). LP handles continuous constraints (collision-free strokes, diffusion). QP handles optimization (curvature minimization, IK). |
| **Bounded Computation** | All solver iterations capped by `MAX_ITER` (default 64) and `MAX_MS` (default 50ms). No unbounded computation. |

## Repository Layout

```
SmazkaVG/
├── README.md                          # This file
├── spec/
│   └── SPEC.md                        # Formal specification (binary layout, constraint types, solver dispatch)
├── src/
│   └── resolver.c                     # Constraint resolver pseudocode (SMT + LP/QP dispatch)
└── examples/
    ├── cyclic_hierarchy.smazka        # Line-ASM example: cyclic parent chain + shared edges + diffusion
    └── hexdump_annotated.txt          # Annotated binary hexdump of the example
```

## Solver Architecture

```
                    ┌──────────────────┐
                    │  Constraint List │
                    │  (flat, typed)   │
                    └────────┬─────────┘
                             │
                    ┌────────▼─────────┐
                    │   Classifier     │
                    │  (dispatch logic)│
                    └────────┬─────────┘
                             │
              ┌──────────────┼──────────────┐
              │              │              │
     ┌────────▼──────┐ ┌────▼─────┐ ┌──────▼──────┐
     │  SMT Solver   │ │LP Solver │ │ QP Solver   │
     │ (interval +   │ │(psolve   │ │(active-set  │
     │  fixed-point) │ │ simplex) │ │ over psolve)│
     └───────────────┘ └──────────┘ └─────────────┘
```

The LP solver is provided by [psolve](https://github.com/SodoMita/psolve) — an AVX-512 vectorized revised simplex implementation. The QP solver is an active-set method wrapping psolve. The SMT solver is a custom bounded decision procedure using interval propagation and fixed-point iteration.

## Quick Example (Line-ASM)

```
# Triangle with shared edge and diffusion curve
v 0 0 0
v 1 100 0
v 2 50 86
e 0 0 1
e 1 1 2
e 2 0 2
f 0 0 1 2
c 0 edge_connects e1 v1 v2
c 1 diffusion e1 L FF0000 R 0000FF
c 2 min_dist e0 e1 3.0
```

## Dependencies

- **psolve** — LP solver backend: https://github.com/SodoMita/psolve
- **zstd** — Optional payload compression (RFC 8878)

## License

TBD

## Specification

Read the full formal specification at [`spec/SPEC.md`](spec/SPEC.md).
