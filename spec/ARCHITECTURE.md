# SmazkaVG architecture

SmazkaVG is a nonsequential, constraint-first vector-graphics format.

Primary representation reference: Dalstein, Ronfard, and van de Panne, [Vector Graphics Complexes](https://www.borisdalstein.com/research/vgc/).

Mandatory optimization backend: pinned [`psolve`](https://github.com/SodoMita/psolve) submodule.

## Decisions

1. Source records are unordered declarations. Parser, hash, insertion, matching, simplex, and branch-discovery order have no scene semantics.
2. Vertices, edges, and faces are VGC-style cells in an incidence graph.
3. Every cell has one explicit, unique, absolute integral `z`.
4. Every render-affecting property is a typed constant or bounded solver variable.
5. Geometry, color, width, transforms, depth, and existence all use that property system.
6. Constants reduce the optimization domain and specialize painting kernels.
7. Variables lower to psolve LP, QP, MIP, or fixed PGS according to type and rule form.
8. Rendering receives one immutable solved scene. There is no direct-source production paint path.
9. Hard failure, infeasibility, degraded soft residual, optional-cell removal, bounded-search exhaustion, ambiguity, and optimal completion are distinct statuses.
10. The source language uses compact ASCII sigils and explicit named statuses.

## Pipeline

```text
bounded source
  -> parse all records
  -> resolve all symbols
  -> build cell incidence graph and property registry
  -> validate types/constants/domains
  -> compact variable IDs
  -> lower rules to mandatory psolve
  -> solve lexicographic tiers
  -> quantize and validate resolved properties
  -> verify unique absolute Z
  -> immutable solved scene
  -> type-specific painting sorted by Z
```

## Cell model

- `+V` declares a vertex cell.
- `+E` declares an edge cell incident to two vertices.
- `+F` declares a face cell bounded by an oriented cycle of edges.

Shared geometry is shared incidence, not duplicate coordinates.

Special shapes are restricted cell geometry types or authoring sugar lowered to cells. They do not get independent parser or renderer arrays.

## Typed properties

A property is:

```text
const <type> <value>
var   <type> <seed> <finite-domain>
```

Implemented types:

- `bool`: existence and binary feature selection; MIP when variable;
- `int`: absolute Z and integer state; MIP when variable;
- `q16`: bounded scalar geometry/style;
- `vec2`: bounded 2D geometry;
- `rgba`: four bounded integer channels;
- `enum`: constant kernel/type selection.

Constants are validated and removed from the solver vector. Solved variables become constants in the immutable output.

## Absolute Z

Every cell, including vertices, has exactly one resolved `z:int`.

- Larger Z paints later.
- Duplicate or missing Z is fatal.
- Source order is never an inferred fallback.
- Variable Z requires MIP and must resolve uniquely.
- Relative depth syntax, if added, is only sugar over absolute integer variables and may not leave a partial order.

## Constraint and failure levels

ASCII sigils are paired with named statuses:

- `!reject` / `!infeasible`: hard rules;
- `?degraded@N` / `?optional@N`: soft tier `N`;
- `~optimal@N`: explicit objective tier `N`.

Tiers are lexicographic. The optimum of each tier is frozen before the next tier. Seed deviation is the final canonical objective.

A render-affecting free variable is invalid. Future uniqueness checking will min/max each resolved visual variable under the frozen optimum and report `ambiguous` if its range is nonzero.

## Specialized painting

Optimization does not imply one universal slow renderer.

Examples:

- `curve=seg` dispatches the segment kernel;
- a segment-bounded face dispatches the polygon kernel;
- constant colors avoid variable-color branches;
- `exists=0` removes the cell before dispatch;
- future rational, diffusion, and transform types get their own validated kernels.

All dispatch happens after solving.

## Current implementation boundary

The new core currently provides:

- unordered parsing and forward references;
- V/E/F incidence;
- typed constants and variables;
- hard single-property linear rules;
- soft L1 target rules;
- explicit linear min/max objectives;
- lexicographic tier freezing;
- automatic canonical seed objective;
- psolve LP and MIP lowering;
- Q16.16 post-solve quantization;
- required existence and unique absolute Z validation;
- immutable canonical resolved output;
- Z-sorted segment and polygon SVG kernels.

Next implementation steps:

1. sparse linear expressions;
2. convex QP rule forms;
3. fixed PGS property groups;
4. variable enum one-hot lowering;
5. curve geometry types beyond segments;
6. face holes and non-manifold validation;
7. uniqueness certificates;
8. binary encoding of the same IR;
9. raster output from the same solved scene.

## Invariants tested now

- random record permutation preserves resolved and SVG bytes;
- LP and MIP paths are both exercised;
- hard and soft symbolic rules resolve expected values;
- all output properties are constants;
- duplicate absolute Z fails;
- invalid named failure status fails;
- painting follows absolute Z.
