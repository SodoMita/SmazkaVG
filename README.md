# SmazkaVG

A compact, nonsequential, constraint-first vector-graphics format.

SmazkaVG models a drawing as VGC-style cells with typed properties. The pinned `psolve` backend is mandatory. Source record order has no meaning; every cell has a unique absolute Z after resolution.

## Core ideas

- **VGC-style topology:** vertices, edges, and faces form an incidence graph.
- **Constraint-first properties:** geometry, color, width, existence, depth, and future transforms are typed constants or bounded variables.
- **Mandatory psolve:** continuous properties use LP/QP/PGS forms; integer, enum, existence, and variable depth use MIP forms.
- **Compact ASCII sigils:** `+`, `=`, `?`, `!`, and `~` encode cells, properties, hard constraints, soft tiers, and objectives.
- **Explicit failures:** symbols are paired with names such as `reject`, `infeasible`, `degraded`, `optional`, and `optimal`.
- **Absolute depth:** each cell has a unique integral `z`; larger values paint later.
- **Solved-scene rendering:** type-specific painters consume only immutable resolved properties.

## Build

```sh
git submodule update --init --recursive
make
make test
```

This produces `build/smazka`.

## Example

```text
+V a
+V b
+E line a b

=a.exists:bool 1
=a.z:int 10
?a.xy:vec2 12 10 0 100 0 100

=b.exists:bool 1
=b.z:int 11
=b.xy:vec2 90 20

=line.exists:bool 1
=line.z:int 20
=line.curve:enum seg
=line.width:q16 3
=line.color:rgba 202020FF

!infeasible anchor_x a.xy.x = 10
?degraded@20 prefer_y a.xy.y ~= 20
```

Records may appear in any order.

## CLI

```sh
# Parse, solve, and validate
build/smazka examples/triangle.smazka --check

# Emit an immutable solved document
build/smazka examples/triangle.smazka --resolved solved.smazka

# Render through the solved, Z-sorted SVG kernels
build/smazka examples/triangle.smazka --svg triangle.svg 100 100
```

## Sigil summary

| Form | Meaning |
|---|---|
| `+V`, `+E`, `+F` | declare cells and incidence |
| `=cell.prop:type ...` | constant property |
| `?cell.prop:type ...` | bounded variable with seed |
| `!reject` / `!infeasible` | hard rule |
| `?degraded@N` / `?optional@N` | soft rule at tier N |
| `~optimal@N` | objective at tier N |

See [`spec/FORMAT.md`](spec/FORMAT.md) for exact implemented syntax and [`spec/ARCHITECTURE.md`](spec/ARCHITECTURE.md) for the design.

## Repository

```text
src/smazka.h, src/smazka.c  parser, cell/property IR, psolve lowering, solved exporters
tools/smazka.c              command-line frontend
spec/FORMAT.md              implemented grammar and semantics
spec/ARCHITECTURE.md        architectural invariants and roadmap
examples/triangle.smazka    LP/MIP + absolute-Z example
tests/run_core_tests.sh     permutation, solver, status, and depth tests
docs/REFERENCES.md          primary papers, standards, and source references
third_party/psolve          mandatory pinned LP/QP/MIP/PGS backend
```

## Current scope

Implemented now:

- unordered declarations and forward references;
- vertex/edge/face cells;
- constant and variable `bool`, `int`, `q16`, `vec2`, and `rgba` properties;
- constant enum properties;
- hard scalar linear constraints;
- soft scalar L1 targets;
- scalar min/max objectives;
- lexicographic tier solving;
- LP and MIP lowering through psolve;
- required existence and unique absolute Z;
- canonical immutable solved output;
- segment and polygon SVG kernels sorted by Z.

The roadmap adds sparse expressions, QP/PGS rule forms, more geometry kernels, holes, uniqueness certificates, binary encoding, and raster output. These extensions must use the same cell/property/solver pipeline.

## License

No license has been selected yet. This is a release blocker.
