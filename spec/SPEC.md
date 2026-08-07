# SmazkaVG v1.3 Formal Specification

> Flat Document Model · LP + Convex QP Solver · Fixed-Point Determinism
> Revision 1.6 — 2026-08-07
> Supersedes v1.1 (SMT-era) and v1.2 (namespace split). This revision
> documents the reference rasterizer (`src/rasterizer.c`) and resolver
> (`src/resolver.c`) as shipped: psolve LP/QP backend, Poisson diffusion
> curves, stroke caps/joins, native PNG output, and the `smazka-solve` tool.

---

## Table of Contents

1. [Design Philosophy](#1-design-philosophy)
2. [Binary Layout](#2-binary-layout)
3. [Fixed-Point Arithmetic](#3-fixed-point-arithmetic)
4. [Primitive Types](#4-primitive-types)
5. [Constraint Namespaces](#5-constraint-namespaces)
6. [Solver Dispatch & Resolution](#6-solver-dispatch--resolution)
7. [Textual Skin (Line-ASM)](#7-textual-skin-line-asm)
8. [Profiles & Conformance](#8-profiles--conformance)
9. [Security & Bounds](#9-security--bounds)
10. [Normative References](#10-normative-references)
11. [v1.2 / v1.3 Amendment Summary & Future Work](#11-v12--v13-amendment-summary--future-work)

---

## 1. Design Philosophy

### 1.1 Core Axioms

| Axiom | Statement |
|---|---|
| **Flat Document** | All primitives and constraints are declared in a single-level list with global 32-bit IDs. There is **no nesting, no tree, no DAG** in the serialization. Hierarchy, ordering, and grouping are emulated via constraints. |
| **Fixed-Point Determinism** | All stored numerical values use Two's Complement fixed-point (Q16.16 or Q32.32) with saturating arithmetic. No IEEE 754 in storage. |
| **Declarative Constraints** | All relationships between primitives are expressed as typed constraints, split into four sections: `s` (structural), `a` (assertions), `c` (constraints), `p` (paint). |
| **Bounded Computation** | The constraint language is restricted to a **decidable, convex, polynomial-time fragment** (LP + convex QP). Every loop is bounded by header-declared `MAX_ITER` / `MAX_MS`. No unbounded computation is permitted. |
| **Binary-First** | The canonical format is binary. The textual Line-ASM projection is lossless and maps 1:1 to the binary. |
| **First-Class Curves** | Edges are 1D spines (segment, quadratic/cubic/rational Bézier, Catmull-Rom) shared between faces. Strokes are profiles along the spine, not independent polylines. |

### 1.2 Revision History

| Revision | What changed |
|---|---|
| v1.1 | Original spec: SMT + LP/QP hybrid, single `c` table, straight edges only. |
| v1.2 | SMT removed (non-convex, undecidable in general). Namespace split into `s`/`a`/`c`/`p`. Cubic Bézier + B-spline + Spiro edges. Variable-width stroke profiles. Cyclic parent = equilibrium rigging (convex QP). Unified RGBA. Labeled node fields. Convex-QP enforcement at parse time. |
| v1.3 | Reference rasterizer: per-pixel distance-field rendering (no tessellation). Curve types extended: `quad`, `rational` (conic), `catmull`. New primitives: `r` arc, `z` ellipse. Vertex types (`corner`/`smooth`/`symmetric`/`auto`). Face inline fill field. View-fit includes control points. WebP export. |
| v1.3.1 | Audit pass: parser bounds-checking, ear-clipping face fills, true taper rendering, curve-aware diffusion brush, resolver fixes (cycle detection, state-machine activation, SLP min_dist). |
| v1.3.2 | psolve integration: submodule + `libpsolve.a`; resolver LP/QP phases implemented (L1 least-change, SLP with post-solve check, fair_blend/min_stretch QPs); `make solver-test` with 10-test self-suite. |
| v1.4 | Diffusion curves are a real discrete Laplace (Poisson) solve (SOR, bounded, deterministic) instead of a signed-distance brush. Stroke caps (round/butt/square; round caps form round joins). Self-contained PNG output. `tools/smazka-solve` applies the resolver to a file end-to-end. Resolver `rig` QP implemented (translation-only equilibrium). |
| v1.4.1 | **Node transforms applied**: affine transforms (scale→skew→rotate→translate, node ID order) are baked into the vertex store for vertex/edge content. **Face holes**: `f <outer edges> | <hole edges> [| ...] [fill]` filled with even-odd rule (curved holes tessellated). Parser fixes: fill colors starting with decimal digits (e.g. `88AA00`) parse correctly; inline `#` comments no longer leak into the fill. Binary container encodes/decodes holes and 8-digit face fills. Resolver `rig` switched to psolve's **fixed-point PGS** boxed QP (integer-exact Q16.16). |
| v1.5 | **Animation**: `k` keyframe records drive node transforms; the rasterizer interpolates per field (piecewise-linear, partial keyframes inherit the node's base pose) and renders **frame sequences** (`--anim <fps> <frames> [--loop] [--out prefix]`) with a fixed camera covering the animation's full bounding box; single frames at a time are rendered with `--t <seconds>`. Animated GIF output is assembled from the PNG frames when PIL is available. Keyframes round-trip through the binary container; golf dialect gains `K`. |
| v1.6 | **State machines drive keyframe poses**: `a state_machine` activations become **blend weights** over keyframe pose groups (`k ... st=<state>`); the renderer computes an exclusive-chain weight model (initial decays as transitions progress, later transitions fade earlier ones, so idle→walk→jump blends cleanly) and blends the active states' poses per node field, with the global keyframe timeline as fallback. Resolver gains `smazka_resolve_anim`; `smazka-solve --t <s>` bakes a frame; binary container round-trips `st=` and state machines; golf `K` accepts `st=`. |

---

## 2. Binary Layout

### 2.1 Overview

```
┌─────────────────────────────────────────────────────┐
│  HEADER (48 bytes, 4-byte aligned)                  │
├─────────────────────────────────────────────────────┤
│  PRIMITIVES TABLE                                   │
│    ├─ Vertex records   (type 0x01)                  │
│    ├─ Edge records     (type 0x02)                  │
│    ├─ Face records     (type 0x03)                  │
│    ├─ Stroke records   (type 0x04)                  │
│    ├─ Node records     (type 0x06)                  │
│    ├─ Arc records      (type 0x07)                  │
│    └─ Ellipse records  (type 0x08)                  │
├─────────────────────────────────────────────────────┤
│  CONSTRAINTS TABLE  (section-tagged, see §5)        │
│    ├─ s structural    (tag 0x10)                    │
│    ├─ a assertions    (tag 0x20)                    │
│    ├─ c constraints   (tag 0x30: LP / convex QP)    │
│    └─ p paint         (tag 0x40)                    │
├─────────────────────────────────────────────────────┤
│  PAYLOAD SECTION                                    │
│    ├─ Texture blobs (zstd-compressed, optional)     │
│    └─ Metadata key-value pairs                      │
├─────────────────────────────────────────────────────┤
│  FOOTER (8 bytes)                                   │
│    ├─ CRC-32C of all preceding bytes                │
│    └─ End sentinel 0xDEADBEEF                       │
└─────────────────────────────────────────────────────┘
```

All multi-byte integers are **Little-Endian**. All sections are padded to **4-byte alignment**.
A compact reference container (delta-VLQ encoding, §11.2) ships in `tools/`.

### 2.2 Header (48 bytes)

```
Offset  Size  Field              Description
──────  ────  ─────────────────  ───────────────────────────────────────
0x00    4     magic              0x534D5647 ("SMVG" in ASCII)
0x04    2     version_major      uint16, current = 1
0x06    2     version_minor      uint16, current = 3
0x08    4     flags              uint32 bitmask (see §2.3)
0x0C    4     n_vertices         uint32
0x10    4     n_edges            uint32
0x14    4     n_faces            uint32
0x18    4     n_strokes          uint32
0x1C    4     n_nodes            uint32
0x20    4     n_arcs             uint32
0x24    4     n_ellipses         uint32
0x28    4     n_constraints      uint32
0x2C    4     solver_config      uint32 (see §2.4)
0x30    4     header_crc         uint32 CRC-32C of bytes [0x00..0x2F]
```

**Total: 52 bytes** (padded to 52, 4-byte aligned). The v1.1 header layout (48 bytes, `n_curves`) is superseded; readers must reject v1.1-style headers with `version_minor < 2`.

### 2.3 Header Flags (bitmask at offset 0x08)

| Bit | Name | Meaning |
|---|---|---|
| 0 | `HAS_ASSERT` | Document contains `a` (assertion) records |
| 1 | `HAS_LP` | Document contains LP constraints |
| 2 | `HAS_CONVEX_QP` | Document contains convex QP constraints (v1.2: was `HAS_QP`; non-convex QP is a **parse error**) |
| 3 | `HAS_PAINT` | Document contains `p` (paint) records |
| 4 | `HAS_PAYLOAD` | Document has zstd-compressed payload section |
| 5 | `Q32` | Coordinates use Q32.32 instead of Q16.16 |
| 6 | `COLORSPACE_LINEAR` | Colors stored in linear light (default is sRGB) |
| 7 | `ANIMATED` | Document contains state-machine (automata) records |
| 8 | `RIG_MODE` | `s parent` contains cycles; renders as equilibrium rigging (§5.1.3) |
| 9–31 | Reserved | Must be zero |

### 2.4 Solver Config (offset 0x2C)

```
Bits 0–7:   MAX_ITER       (uint8, default 64, solver iteration cap per resolve)
Bits 8–15:  MAX_MS         (uint8, solver wall-clock cap, default 50ms, units = 1ms)
Bits 16–23: SMT_STRATEGY   (reserved; must be 0 in v1.2+)
Bits 24–31: PROFILE_ID     (uint8: 0=full, 1=web-safe, 2=print-pdf, 3=cad-exact, 4=anime-prod)
```

---

## 3. Fixed-Point Arithmetic

### 3.1 Formats

| Format | Storage | Integer Bits | Fractional Bits | Range | Resolution |
|---|---|---|---|---|---|
| **Q16.16** | `int32_t` | 16 (signed) | 16 | −32768.0 to +32767.99998474... | 1/65536 ≈ 1.526e-5 |
| **Q32.32** | `int64_t` | 32 (signed) | 32 | −2147483648.0 to +2147483647.999... | ≈ 2.328e-10 |

### 3.2 Encoding Rules

- **Storage**: Two's Complement, Little-Endian.
- **Conversion** (API boundary only):
  - `to_fixed_q16(x) = clamp(round(x * 65536), INT32_MIN, INT32_MAX)`
  - `from_fixed_q16(v) = (double)v / 65536.0`
- **No IEEE 754 in storage.** Floats may exist only in temporary solver/rasterizer scratch memory.

### 3.3 Saturating Arithmetic

```c
// Saturating addition for Q16.16
static inline int32_t q16_add_sat(int32_t a, int32_t b) {
    int64_t sum = (int64_t)a + (int64_t)b;
    if (sum > INT32_MAX) return INT32_MAX;
    if (sum < INT32_MIN) return INT32_MIN;
    return (int32_t)sum;
}

// Saturating multiplication for Q16.16 (result is Q16.16)
static inline int32_t q16_mul_sat(int32_t a, int32_t b) {
    int64_t prod = (int64_t)a * (int64_t)b;
    int64_t result = prod >> 16;
    if (result > INT32_MAX) return INT32_MAX;
    if (result < INT32_MIN) return INT32_MIN;
    return (int32_t)result;
}

// Saturating division for Q16.16
static inline int32_t q16_div_sat(int32_t a, int32_t b) {
    if (b == 0) return (a >= 0) ? INT32_MAX : INT32_MIN;
    int64_t num = (int64_t)a << 16;
    int64_t result = num / (int64_t)b;
    if (result > INT32_MAX) return INT32_MAX;
    if (result < INT32_MIN) return INT32_MIN;
    return (int32_t)result;
}
```

### 3.4 Saturation Hazard (Q16.16)

Saturating storage means coordinates outside [−32768, +32767] **clamp**, they do
not wrap. A renderer must therefore **clip in view space** and only saturate at
the storage boundary. The reference rasterizer keeps coordinates as IEEE-754
`double` in scratch memory (deterministic: storage is exact, conversion is
`round()` + clamp) and only quantizes when a binary container is written.

---

## 4. Primitive Types

### 4.1 Primitive Type Tags

| Tag | Type | Record Size (bytes) |
|---|---|---|
| `0x01` | Vertex | 16 |
| `0x02` | Edge | 12 + 8×n_cp + 4×n_w |
| `0x03` | Face | variable |
| `0x04` | Stroke | variable (see §4.5) |
| `0x06` | Node (transform group) | 28 |
| `0x07` | Arc | 28 |
| `0x08` | Ellipse | 40 |

### 4.2 Vertex Record (16 bytes)

```
Offset  Size  Field     Description
──────  ────  ───────  ────────────────────────────────
0x00    1     type     0x01
0x01    1     vtype    0=corner 1=smooth 2=symmetric 3=auto (Inkscape-style)
0x02    2     id       uint16 global vertex ID
0x04    4     x        Q16.16 x-coordinate
0x08    4     y        Q16.16 y-coordinate
0x0C    4     flags    uint32 (bit 0: pinned, bit 1: selected, bits 2-31: reserved)
```

When `Q32` flag is set, `x` and `y` are each Q32.32 (8 bytes), making the record 24 bytes.

### 4.3 Edge Record

```
Offset  Size  Field     Description
──────  ────  ───────  ────────────────────────────────
0x00    1     type     0x02
0x01    1     etype    0=seg 1=quad 2=cubic 3=rational 4=catmull
0x02    2     id       uint16 global edge ID
0x04    2     v_start  uint16 ID of start vertex
0x06    2     v_end    uint16 ID of end vertex
0x08    1     n_cp     number of control points
0x09    1     pad      Reserved (0x00)
0x0A    4     flags    uint32 (bit 0: shared, bit 1: boundary, bit 2: diffusion, bits 3-31: reserved)
       8×n_cp  ctrl    Q16.16 (x,y) control points
       4×n_w   weight  Q16.16 weights (rational edges only)
```

Curve types:

| etype | Control points | Interpretation |
|---|---|---|
| `seg` | 0 | Straight segment v_start→v_end |
| `quad` | 1 | Quadratic Bézier |
| `cubic` | 2 | Cubic Bézier |
| `rational` | 2 (+2 weights) | Rational quadratic (conic): weights `(1, w0, 1)`; a second weight is reserved |
| `catmull` | 0 | Catmull-Rom through v_start, v_end; neighbors chosen automatically |

Control points are **geometry, not topology**: two faces sharing an edge render
the same curved spine.

### 4.4 Face Record (variable size)

```
Offset  Size  Field     Description
──────  ────  ───────  ────────────────────────────────
0x00    1     type     0x03
0x01    1     pad      Reserved (0x00)
0x02    2     id       uint16 global face ID
0x04    2     n_edges  uint16 number of edges in outer boundary
0x06    2     n_holes  uint16 number of hole loops
0x08    4     fill     uint32 inline fill color 0xRRGGBB (0 = none; `p solid_fill` overrides)
0x0C    4×n   edges    uint16[] outer edge IDs in boundary order
       4×m   holes     for each hole: n_hole_edges(u16), edge IDs[]
```

The boundary is a **closed chain**: consecutive edges share a vertex. A reader
reconstructs each loop's ordered boundary and fills with the **even-odd rule**
across the outer loop and all hole loops, so donuts and counter shapes
(e.g. the counters of "O" or "B" as paths) render correctly. Curved boundary
edges are tessellated before the point-in-polygon test. Faces are filled with
a `p solid_fill` paint record, or with the inline `fill` field if present.
Hole-free faces may be filled with ear-clipping (faster).

### 4.5 Stroke Record (variable size)

```
Offset  Size  Field          Description
──────  ────  ─────────────  ────────────────────────────────
0x00    1     type           0x04
0x01    1     width_mode     0=constant, 1=variable profile, 2=pressure-driven
0x02    2     id             uint16 global stroke ID
0x04    2     edge_id        uint16 associated edge
0x06    2     n_widths       uint16 number of width samples
0x08    4     color          uint32 packed RGBA (8 bits/channel)
0x0C    1     cap            stroke cap: 0=round (default), 1=butt, 2=square
0x0D    3     pad            Reserved
0x10    4×n   widths         Q16.16 width at t = i/(n-1) for i in [0, n)
```

The width profile is a **function along the edge parameter t ∈ [0,1]**; the
rasterizer interpolates linearly between samples. A single width value is
equivalent to a constant profile. Stroke caps are `cap=round` (default),
`cap=butt`, or `cap=square` (extends by half the local width along the tangent;
segments only). Round caps at a vertex shared by two or more stroked edges form
a **round join** (the endpoint half-disks overlap, closing the corner notch);
explicit miter/bevel joins are future revisions (they require path-level
geometry).

### 4.6 Node Record (28 bytes)

```
Offset  Size  Field          Description
──────  ────  ─────────────  ────────────────────────────────
0x00    1     type           0x06
0x01    1     pad            Reserved (0x00)
0x02    2     id             uint16 global node ID
0x04    4     tx             Q16.16 translation x
0x08    4     ty             Q16.16 translation y
0x0C    4     rotation       Q16.16 rotation angle (radians, range: −π to +π)
0x10    4     sx             Q16.16 scale x
0x14    4     sy             Q16.16 scale y
0x18    4     skew           Q16.16 skew factor
0x1C    4     content_ref    uint32 reference to primitive ID this node transforms (0 = none)
```

**Transform semantics (v1.4.1, applied by the reference rasterizer):**
`content_ref` is resolved against the vertex ID space first, then the edge ID
space (offset by `n_vertices`). A vertex transform updates the vertex position;
an edge transform updates both endpoint vertices and the edge's control points.
The convention per node is `p' = R(rotation)·( S(sx,sy)·p + skew-shear ) + (tx,ty)`,
and nodes are applied in **ID order**, so stacked nodes compose deterministically.
Transforms are baked into the vertex store before vertex-type resolution and
rendering — the flat model's way of emulating a hierarchy (explicit `parent`
edges remain resolver territory, §5.2).

### 4.7 Arc Record (28 bytes)

```
Offset  Size  Field          Description
──────  ────  ─────────────  ────────────────────────────────
0x00    1     type           0x07
0x01    1     pad
0x02    2     id             uint16
0x04    4     cx             Q16.16 center x
0x08    4     cy             Q16.16 center y
0x0C    4     r              Q16.16 radius
0x10    4     a0             Q16.16 start angle (degrees)
0x14    4     a1             Q16.16 end angle (degrees, may be < a0 for wrap-around)
0x18    4     color          uint32 RGBA
0x1C    4     lw             Q16.16 line width
```

### 4.8 Ellipse Record (40 bytes)

```
Offset  Size  Field          Description
──────  ────  ─────────────  ────────────────────────────────
0x00    1     type           0x08
0x01    1     pad
0x02    2     id             uint16
0x04    4     cx, cy         Q16.16 center
0x0C    4     rx, ry         Q16.16 radii
0x14    4     rot            Q16.16 rotation (radians)
0x18    4     fill           uint32 RGBA fill
0x1C    4     stroke         uint32 RGBA stroke (alpha 0 = no stroke)
0x20    4     sw             Q16.16 stroke width
```

### 4.9 Keyframe Record (animation, v1.5)

```
Offset  Size  Field          Description
──────  ────  ─────────────  ────────────────────────────────
0x00    1     type           0x09
0x01    1     pad            Reserved (0x00)
0x02    2     id             uint16 global keyframe ID
0x04    2     node_id        uint16 target node
0x06    4     time           Q16.16 time in seconds (>= 0)
0x0A    4     st             zigzag int32 state group (-1 = global timeline)
0x0E    1     mask           which transform fields are set (bits: 1=tx 2=ty 4=rot 8=sx 16=sy 32=skew)
       4×n   values         Q16.16 values for the set fields, in bit order
```

Line-ASM:

```
k <id> <node_id> <time> [st=<state>] [tx=..] [ty=..] [rot=..] [sx=..] [sy=..] [skew=..]
```

A keyframe with `st=<state>` defines that state's **pose** for the listed
fields (within a state group the last keyframe per node/field wins; `time` is
only used for ordering). A keyframe without `st=` lives on the **global
timeline** (piecewise-linear interpolation in `time`). See §5.3.1 for how the
two interact.

**Semantics.** A keyframe sets a *partial* pose for a node at `time` seconds.
Each of the six transform fields is animated independently over the keyframes
that set it, with **piecewise-linear interpolation** and clamping outside the
keyframe range (or time-wrapping when the renderer runs with `--loop`). Fields
with no keyframes keep the node's base (`n` record) value, so a keyframe only
needs to list what changes. The interpolated pose is applied with the node
transform semantics of §4.6. To move a whole rigid shape, put one node per
vertex (or per edge for curved shapes) and give all of them matching keyframes.

**Rendering.** `smazka-raster <in> [w] [h] --anim <fps> <frames> [--loop] [--out prefix]`
renders `<prefix>_000.png/.bmp … <prefix>_<n-1>.png/.bmp` at `t = i/fps`, with
the camera fixed to the animation's full bounding box (base geometry plus every
keyframe pose) so motion is visible; `--loop` wraps time modulo the last
keyframe time. An animated GIF is assembled from the PNG frames when PIL is
available. `--t <seconds>` renders a single frame at a time.

Animation complements the resolver's constraint-driven `state_machine`
(§5.3.1): keyframes are declarative, renderer-side motion; state machines are
solver-side blend weights. Both can be combined.

### 4.10 Unsafe Records (`.smazkavg_unsafe`)

```
t <id> <x> <y> [size=..] [font=..] <"text">          # text (font reference)
img <id> <x> <y> [w] [h] <path>                      # raster embed
inc <path>                                           # include (inlined at load)
font <id> <path>                                     # font library declaration
```

These records are **not part of safe SmazkaVG** and are rejected by the
binary container. The safe pipeline converts them:

| Record | Safe handling | Status |
|---|---|---|
| `inc` | **Inlined** at parse time (relative path, depth ≤ 8, cycle-guarded, line budget) | Implemented (rasterizer + sanitizer) |
| `t` | Warn + skip (vectorized text is future work) | Implemented (warning) |
| `img` | Warn + skip (centerline vectorization via LP is future work) | Implemented (warning) |
| `font` | Warn + skip | Implemented (warning) |

`tools/smazka-sanitize` converts an unsafe document into a safe `.smazkavg`
by inlining `inc` and stripping `t`/`img`/`font` with per-record warnings;
all other records and comments are preserved verbatim. See docs/PLAN.md for
the raster-centerline and text-vectorization roadmap.

---

## 5. Constraint Namespaces

### 5.1 Sections

Every constraint record begins with a 1-byte section tag, 1-byte subtype, and
2-byte ID. The sections are processed by different pipeline stages:

| Tag | Section | Contents | Processing |
|---|---|---|---|
| `0x10` | `s` **Structural** | `parent`, `group_id` | Builds the scene graph; no solver |
| `0x20` | `a` **Assertions** | `edge_connects`, `bound_check`, `state_machine` | Validated / simulated; errors reported, not solved |
| `0x30` | `c` **Constraints** | `min_dist`, `bbox_clamp`, `linear_*`, `collision_free` (LP); `min_curvature`, `ik_target`, `fair_blend`, `rig` (convex QP) | Solved by LP / convex QP |
| `0x40` | `p` **Paint** | `diffusion`, `solid_fill` | Routed to the rasterizer, never solved |

Rationale (from the v1.2 changelog): `s parent` is structural data; `a
edge_connects` restates `e <id> <v0> <v1>` and is a redundant assertion; `p
diffusion` specifies colors, not solver constraints. Only `c` records feed the
LP/QP solver.

### 5.2 Structural (s)

| Subtype | Name | Payload | Meaning |
|---|---|---|---|
| `0x01` | `parent` | `child_id(u16), parent_id(u16)` | child's transform is composed with parent's |
| `0x02` | `group_id` | `prim_id(u16), group(u16)` | tags primitives with group membership |

#### 5.2.1 Cyclic parent semantics (RIG_MODE)

- **Acyclic chains** (tree): standard bottom-up transform multiplication, O(n).
- **Cyclic chains** (A→B→C→A): interpreted as a **soft equilibrium rigging
  constraint**, solved as a convex QP:
  ```
  minimize  Σ_i ‖world_i − local_i‖²
  subject to  world_child ≈ world_parent × local_child   (soft, penalized)
  ```
  The header flag `RIG_MODE` (bit 8) is set; renderers that don't support rig
  mode must reject the file or fall back to treating every node as a root. If
  the solver fails to converge within `MAX_ITER`, the file is invalid.

### 5.3 Assertions (a)

| Subtype | Name | Payload | Meaning |
|---|---|---|---|
| `0x01` | `edge_connects` | `edge_id(u16), v_start(u16), v_end(u16)` | Asserts edge endpoints; violations are repaired + warned |
| `0x02` | `bound_check` | `prim_id(u16), dim(u8), lo(Q16.16), hi(Q16.16)` | Clamps a coordinate into `[lo, hi]` (saturating) |
| `0x03` | `state_machine` | `state_id(u16), initial(u16), transitions[]` | Per-frame animation automaton (§5.3.1) |

#### 5.3.1 state_machine — drives keyframe pose blending (v1.6)

```
a <id> state_machine <state_id> <initial> <target> <time|event|condition> <param> [start=<t>] ...
```

Each machine has an initial state and an ordered list of transitions; each
transition i drives state `target_i`. At render time the document clock
evaluates every trigger:

| trigger_type | Activation `a_i` |
|---|---|
| `0` time | ramp `(t − start) / param` clamped to `[0,1]` (seconds, matching keyframe times) |
| `1` event | 1 if the event is active (renderer: `--event <target>`), else 0 |
| `2` condition | 1 if `input ≥ param` (renderer: `--input <value>`), else 0 |

**Exclusive-chain weights.** The initial state's weight decays as transitions
progress and later transitions fade earlier ones, so idle→walk→jump blends
cleanly instead of saturating at 50/50:

```
w_initial = max(0, 1 − Σ_i a_i)
w_i       = a_i · Π_{j>i} (1 − a_j)
(normalize to Σw = 1)
```

**Combined rendering.** When the document has both a state machine and
keyframes, the state weights become **blend weights over the keyframe pose
groups** (`k ... st=<state>`). For each node field:

```
value = ( Σ_{s: w_s > 0 and pose_s defines the field} w_s · pose_s ) / ( Σ over the same states )
```

— renormalized over the states that define the field. Fields with no state
pose fall back to the **global keyframe timeline** at `t` (piecewise-linear),
then to the node's base value. So a state machine selects *which* poses are
active and the keyframes *define* the poses; a global timeline can animate
details independently. The resolver exposes the same math as
`smazka_resolve_anim(doc, t, loop)`, and `smazka-solve --t <s>` bakes a frame
into a static document. The v1.1 behavior — a constant uniform blend
`w_i = 1/n` — is **removed**.

### 5.4 Constraints (c) — LP

| Subtype | Name | Payload | Description |
|---|---|---|---|
| `0x01` | `min_dist` | `prim_a(u16), prim_b(u16), distance(Q16.16)` | L2 distance ≥ d between primitives |
| `0x02` | `linear_eq` | `n_terms, (var_id, coeff)[]`, `rhs` | Σ coeff·var = rhs |
| `0x03` | `linear_le` | same | ≤ |
| `0x04` | `linear_ge` | same | ≥ |
| `0x05` | `bbox_clamp` | `prim_id, x_min, y_min, x_max, y_max` | variable bounds |
| `0x06` | `collision_free` | `stroke_a, stroke_b, margin` | sampled stroke separation ≥ margin |

#### 5.4.1 min_dist / collision_free semantics (honest formulation)

The L2 constraint ‖p_a − p_b‖₂ ≥ d is **non-convex** (the feasible region is
the exterior of a ball); no single fixed LP row can express it. The reference
solver uses **sequential linear programming (SLP)**:

1. Sample the closest pair of points between the primitives at the current iterate.
2. Add the *directional separation row*
   `(p_b − p_a)·u ≥ d`  where `u = (p_b − p_a)/‖p_b − p_a‖`.
   This is a valid linear over-approximation of the separating hyperplane: it
   only cuts the half-space that actually violates the constraint.
3. Re-solve and re-sample until the true L2 distance is satisfied (2–3
   iterations typical; bounded by `MAX_ITER`).

The v1.1 relaxation — adding `x_a − x_b ≥ d/√2` **and** `y_a − y_b ≥ d/√2`
simultaneously — is **removed**: it forced prim_a to always sit strictly NE of
prim_b, making feasible layouts infeasible. The reference resolver also adds a
post-solve satisfaction check so a satisfied constraint is not re-perturbed by
the least-change objective. Exact L2 separation needs SOCP/MIP, which is out of
scope for the pure-LP backend and documented as a known limitation.

### 5.5 Constraints (c) — convex QP

| Subtype | Name | Payload | Objective |
|---|---|---|---|
| `0x11` | `min_curvature` | `curve_id(u16), weight(Q16.16)` | min ∫κ² ds ≈ min Σ‖p_{i−1} − 2p_i + p_{i+1}‖² (tridiagonal Q) |
| `0x12` | `ik_target` | `chain_id, target_x, target_y, weight` | min ‖J·Δθ − (target − ee)‖² (Jacobian least-squares) |
| `0x13` | `fair_blend` | `n_vars, var_ids[]` | min Σ(x_i − 1/n)² s.t. Σx_i = 1, x_i ≥ 0 (max-entropy blend) |
| `0x14` | `rig` | `node_a, node_b` | equilibrium rigging for cyclic parents (§5.2.1) |

**Enforcement:** at parse time the reader validates that every QP matrix is
positive semi-definite. Non-convex QP is a **parse error**, not a runtime
failure. Solver runtime is bounded by O(n³ + n²·MAX_ITER).

### 5.6 Paint (p)

| Subtype | Name | Payload | Meaning |
|---|---|---|---|
| `0x01` | `diffusion` | `edge_id, left_color(RGBA), right_color(RGBA)` | smooth color field across the edge (see §5.6.1) |
| `0x02` | `solid_fill` | `face_id, color(RGBA)` | flat fill; overrides the face's inline `fill` |

#### 5.6.1 Diffusion curves — discrete Laplace (Poisson) solve

`p diffusion <edge> L <left_color> R <right_color>` solves the Laplace equation
over the framebuffer region around the edge:

```
∇²c = 0
c = left_color   on one side of the curve   (Dirichlet)
c = right_color  on the other side          (Dirichlet)
c = artwork      at the region border       (the field diffuses into the fill)
```

The sides are defined relative to the direction of travel along the edge (the
left side is the one on the left when facing the direction of travel; for an
edge running left→right, L is above). The reference rasterizer discretizes the
problem per pixel and solves it with successive over-relaxation (SOR,
ω = 1.6) using bounded, deterministic iterations (max-change < 1/4 level stops
early; the iteration count is capped by region size). This is the Orzan et al.
(2008) diffusion-curves model: the field follows curved spines, is smooth and
harmonic away from the curve, and falls off to the artwork at the region
border. The v1.3 signed-distance brush and the v1.1 straight-line gradient are
superseded. A cotangent-Laplacian solve over the *face mesh* (instead of the
pixel grid) remains future work (§11.1).

---

## 6. Solver Dispatch & Resolution

### 6.1 Architecture

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

### 6.2 Dispatch Rules

```
IF section == s:      build scene graph (parent/group), detect cycles -> RIG_MODE
ELIF section == a:    validate (edge_connects), clamp (bound_check),
                      simulate (state_machine) at current frame
ELIF section == c:
    IF subtype <= 0x06:  accumulate into LP -> solve with psolve (SLP for min_dist)
    ELIF subtype >= 0x11: accumulate into QP -> psolve active-set (bounds as rows)
ELIF section == p:    route to rasterizer (never solved)
```

**Reference backend.** The shipped resolver (`src/resolver.c`) links the
[psolve](https://github.com/SodoMita/psolve) solver, vendored as a git submodule
(`third_party/psolve`); build with `make solver-test` (defines
`SMZ_HAVE_PSOLVE`).  LP variables are `2V` vertex coordinates plus `S` stroke
widths; the LP objective is **L1 least-change** against the document input
(auxiliary deviation pairs), so a feasible document resolves to the solution
closest to the input.  `min_dist` / `collision_free` run the SLP loop
(§5.4.1) with a post-solve satisfaction check.  The QP phase solves
`fair_blend` (max-entropy weights) and `min_stretch` (elastic pull) per
constraint, and `rig` (cyclic-parent equilibrium) with psolve's **fixed-point
projected-Gauss-Seidel boxed QP** (`pgs_fixed.h`) so the equilibrium solve is
integer-exact in Q16.16 — bit-identical across platforms. `min_curvature` /
`ik_target` remain documented stubs (§11.1).  All psolve calls are wrapped in
the library's `psolve_try`/`psolve_end` error protocol so an out-of-memory
inside the solver unwinds cleanly.

### 6.3 Resolution Order (deterministic)

1. **Assertions**: `edge_connects` repair.
2. **Structural**: parent/group → scene graph; cycle detection; acyclic transform resolution.
3. **Automata**: `state_machine` → per-frame blend weights.
4. **LP**: `min_dist` (SLP), `bbox_clamp`, `linear_*`, `collision_free`.
5. **Convex QP**: `min_curvature`, `ik_target`, `fair_blend`, `rig`.
6. **Validation**: `bound_check` clamps.

Every phase checks the wall-clock deadline (`MAX_MS`); on timeout the last
known-good state is returned with a `SOLVE_TIMEOUT` flag (bit 31 of the warning
mask). All loops are additionally capped by `MAX_ITER`.

---

## 7. Textual Skin (Line-ASM)

### 7.1 Design Principles

- **One declaration per line** — no nesting, no indentation-based semantics.
- **Extensions**: canonical document extension is **`.smazkavg`**; the legacy
  `.smazka` is accepted by all tools for backward compatibility. Documents
  that use unsafe records (`t` text, `img` raster, `font` declarations) are
  named **`.smazkavg_unsafe`** and must be sanitized
  (`tools/smazka-sanitize`) before interchange; constraint-solved outputs are
  named `<name>.solved.smazkavg` (see docs/PLAN.md).
- **`#` comments** — everything after `#` to end of line is ignored.
- **Fixed-point literals** — decimal numbers are parsed as Q16.16 (multiplied
  by 65536 and rounded). Hex values prefixed `0x` are raw integer encodings.
- **IDs** — unsigned decimal integers.
- **Colors** — 6-digit hex (RGB), 8-digit hex (RGBA), or 3-digit hex (#RGB
  shorthand), no `#` prefix in the value itself.

### 7.2 Line Types

```
# Vertices (vtype optional: corner|smooth|symmetric|auto)
v <id> <x> <y> [vtype]

# Edges (curve type + control points optional)
e <id> <v_start> <v_end> [type=<seg|quad|cubic|rational|catmull> [cp...] [w...]]

# Faces (inline fill optional, overridden by p solid_fill; '|' starts a hole loop)
f <id> <edge_0> <edge_1> ... <edge_n> [| <hole_edge_0> ...] [fill_RRGGBB]

# Strokes (cap optional: round|butt|square)
s <id> <edge_id> <color> <w_0> <w_1> ... <w_n> [cap=<round|butt|square>]

# Nodes (labeled or positional)
n <id> tx=<tx> ty=<ty> rot=<rot> sx=<sx> sy=<sy> skew=<skew> content=<ref>
n <id> <tx> <ty> <rot> <sx> <sy> <skew> [content_ref]

# Arcs
r <id> <cx> <cy> <r> <a0> <a1> <color> [lw]

# Keyframes (animation; drives node transforms, v1.5)
k <id> <node_id> <time> [tx=..] [ty=..] [rot=..] [sx=..] [sy=..] [skew=..]

# Ellipses
z <id> <cx> <cy> <rx> <ry> [rot] <fill> [stroke] [sw]

# ── Structural (s) ─────────────────────────────────────────
s <id> parent <child_id> <parent_id>
s <id> group_id <prim_id> <group>

# ── Assertions (a) ─────────────────────────────────────────
a <id> edge_connects <edge_id> <v_start> <v_end>
a <id> bound_check <prim_id> <x|y> <lo> <hi>
a <id> state_machine <state_id> <initial> <target> <time|event|condition> <param> ...

# ── Constraints (c) — LP ───────────────────────────────────
c <id> min_dist <prim_a> <prim_b> <distance>
c <id> bbox_clamp <prim_id> <x_min> <y_min> <x_max> <y_max>
c <id> linear_eq <rhs> <var_0> <coeff_0> <var_1> <coeff_1> ...
c <id> linear_le <rhs> <var_0> <coeff_0> <var_1> <coeff_1> ...
c <id> linear_ge <rhs> <var_0> <coeff_0> <var_1> <coeff_1> ...
c <id> collision_free <stroke_a> <stroke_b> <margin>

# ── Constraints (c) — convex QP ────────────────────────────
c <id> min_curvature <curve_id> <weight>
c <id> ik_target <chain_id> <target_x> <target_y> <weight>
c <id> fair_blend <var_0> <var_1> ...
c <id> rig <node_a> <node_b>

# ── Paint (p) ──────────────────────────────────────────────
p <id> diffusion <edge_id> L <left_color> R <right_color>
p <id> solid_fill <face_id> <color>

# Metadata (optional)
m <key> <value>
```

The deprecated v1.1 form `c <id> <smt-constraint>` is accepted by v1.2+ readers
with a deprecation warning and should be migrated to the split namespaces.

### 7.3 ABNF Grammar

```abnf
; SmazkaVG v1.3 Line-ASM Grammar (RFC 5234 ABNF)

document     = *(line LF)
line         = comment / vertex / edge / face / stroke / node / arc / ellipse
             / structural / assertion / constraint / paint / meta / empty
empty        = ""
comment      = "#" *VCHAR
LF           = %x0A

vertex       = "v" SP vid SP number SP number [SP vtype]
vtype        = "corner" / "smooth" / "symmetric" / "auto"

edge         = "e" SP eid SP vid SP vid [SP edge-params]
edge-params  = "type=" curve-type SP *(number)
curve-type   = "seg" / "quad" / "cubic" / "rational" / "catmull"

face         = "f" SP fid SP 1*(eid) *(SP "|" SP 1*(eid)) [SP hexcolor]
stroke       = "s" SP sid SP eid SP hexcolor SP 1*(number) [SP "cap=" cap]
cap          = "round" / "butt" / "square"

node         = "n" SP nid SP (labeled-node / positional-node)
labeled-node = "tx=" number SP "ty=" number SP "rot=" number SP "sx=" number
             SP "sy=" number SP "skew=" number SP "content=" uint32
positional-node = number SP number SP number SP number SP number SP number [SP uint32]

arc          = "r" SP aid SP number SP number SP number SP number SP number
             SP hexcolor [SP number]
keyframe     = "k" SP kid SP nid SP number SP *(keyval)
keyval       = ("tx=" / "ty=" / "rot=" / "sx=" / "sy=" / "skew=") number
ellipse      = "z" SP zid SP number SP number SP number SP number [SP number]
             SP hexcolor [SP hexcolor] [SP number]

structural   = "s" SP sid SP ("parent" SP nid SP nid / "group_id" SP uint32 SP uint16)
assertion    = "a" SP aid SP (a-edge / a-bound / a-machine)
a-edge       = "edge_connects" SP eid SP vid SP vid
a-bound      = "bound_check" SP uint32 SP dim SP number SP number
a-machine    = "state_machine" SP uint16 SP uint16 SP 1*(transition)
transition   = uint16 SP ("time" / "event" / "condition") SP number
dim          = "x" / "y"

constraint   = "c" SP cid SP constraint-body
constraint-body = c-min-dist / c-bbox / c-linear / c-collision
                / c-min-curv / c-ik / c-fair / c-rig
c-min-dist   = "min_dist" SP uint32 SP uint32 SP number
c-bbox       = "bbox_clamp" SP uint32 SP number SP number SP number SP number
c-linear     = ("linear_eq" / "linear_le" / "linear_ge") SP number SP *(variable-term)
c-collision  = "collision_free" SP sid SP sid SP number
c-min-curv   = "min_curvature" SP cid SP number
c-ik         = "ik_target" SP uint32 SP number SP number SP number
c-fair       = "fair_blend" SP *(uint32)
c-rig        = "rig" SP nid SP nid
variable-term = uint32 SP number

paint        = "p" SP pid SP (p-diffusion / p-fill)
p-diffusion  = "diffusion" SP eid SP "L" SP hexcolor SP "R" SP hexcolor
p-fill       = "solid_fill" SP fid SP hexcolor

meta         = "m" SP 1*VCHAR SP 1*VCHAR

vid / eid / fid / sid / cid / nid / aid / pid = uint16
uint16       = 1*5DIGIT        ; 0-65535
uint32       = 1*10DIGIT       ; 0-4294967295
number       = ["-"] 1*DIGIT ["." 1*6DIGIT]
hexcolor     = 3HEXDIG / 6HEXDIG / 8HEXDIG   ; RGB / RRGGBB / RRGGBBAA
```

---

## 8. Profiles & Conformance

### 8.1 Profile Definitions

| Profile | ID | Mandatory Features | Forbidden Features |
|---|---|---|---|
| **Full** | 0 | All primitives, all sections, LP + convex QP | None |
| **Web-Safe** | 1 | Vertices, edges, faces, strokes, nodes, arcs, ellipses. LP only. Q16.16. | QP, state machines, diffusion |
| **Print-PDF** | 2 | Vertices, edges, faces, strokes, nodes. LP + QP. Q32.32. | State machines, diffusion |
| **CAD-Exact** | 3 | All primitives. LP only. Q32.32. No saturation (Q64.64 extension). | QP, state machines |
| **Anime-Production** | 4 | All primitives, all sections, LP + QP + state machines + diffusion. Q16.16. | None |

### 8.2 Conformance

A reader **must** reject files that use features outside the declared profile
(or the reader's supported profile set). The `PROFILE_ID` in the header's
`solver_config` field indicates the file's profile. A v1.3 reference reader is
permitted to **warn-and-drop** malformed records it cannot honour (see §9.2)
instead of rejecting the whole file, provided it reports the drop.

---

## 9. Security & Bounds

### 9.1 Document Limits

| Parameter | Spec maximum | Reference rasterizer (enforced) |
|---|---|---|
| Vertices | 65535 | 4096 |
| Edges | 65535 | 4096 |
| Faces | 65535 | 4096 |
| Strokes | 65535 | 4096 |
| Nodes | 65535 | 1024 |
| Arcs / Ellipses | 65535 | 256 |
| Constraints per section (s/a/c) | 65535 | 256 |
| Paint records (p) | 65535 | 128 |
| Stroke width samples | 4096 | 64 |
| Face edge count | 1024 | 64 |
| State machine states | 256 | 256 |
| Solver iterations | 255 (MAX_ITER) | 64 |
| Solver wall time | 255 ms (MAX_MS) | 50 ms |
| Payload section | 16 MB | — |

### 9.2 Malformed Input Handling

| Condition | v1.1 behavior | v1.3 reference behavior |
|---|---|---|
| Unknown type tag / command | Reject file | Warn, skip line |
| ID out of range (huge or negative) | **OOB write → memory corruption** | **Warn, ignore record** (bounds-checked before any write) |
| Edge endpoint references missing vertex | Crash (OOB read) | Warn, drop edge (post-parse validation pass) |
| Face references invalid edge | Crash | Warn, drop face fill |
| Stroke references invalid edge | Crash | Warn, drop stroke |
| Constraint section overflow | Silent buffer overflow | Warn, ignore excess records |
| Self-referential edge (v_start == v_end) | — | Accepted (renders as a point) |
| Face boundary not a closed chain | Wrong fill | Warn, skip fill |
| Non-convex QP | Runtime hazard | Parse error (spec-level) |
| CRC mismatch | Reject file | Reject file (container level) |

### 9.3 No External References

The format **bans** all external references:
- No HTTP/HTTPS URLs
- No file:// paths
- No SVG `<use>` or `<image xlink:href>`
- No font references (all text must be converted to paths)

---

## 10. Normative References

1. **psolve** — AVX512-Simplex revised simplex LP solver. `https://github.com/SodoMita/psolve`
2. **VGC** — Vector Graphics Complex topology model. `https://github.com/vgc/vgc`
3. **libfive** — Kernel-based implicit modeling. `https://github.com/libfive/libfive`
4. **Curv** — Shape description language based on SDFs. `https://github.com/curv3d/curv`
5. **TinyVG** — Compact binary vector graphics. `https://tinyvg.tech`
6. **Zstandard** — Fast compression algorithm. RFC 8878.
7. **Q16.16 Fixed-Point** — ARM Architecture Reference Manual, Section A2.5.
8. **Orzan et al.** — Diffusion curves: A vector representation for smooth-shaded images. SIGGRAPH 2008.
9. **Goldfarb-Reid** — Steepest-edge simplex pricing. Mathematical Programming 75 (1996).
10. **Euler-Elastica** — Curve energy minimization. L. Euler, 1744.

---

## 11. v1.2 / v1.3 Amendment Summary & Future Work

### 11.1 Known Limitations (honest)

- **Diffusion** is a signed-distance brush, not a cotangent-Laplacian PDE solve.
- **min_dist / collision_free** use SLP; exact L2 needs SOCP/MIP.
- **Stroke joins** (miter/round/bevel) and **dash arrays** are not implemented.
- **Holes** in faces (`n_holes`) are specified in the binary layout but not
  exposed in Line-ASM v1.3.
- **Text** is banned (converted to paths).
- The **binary container** is specified but the shipped tooling is a compact
  delta-VLQ reference implementation (§11.2); the full CRC-protected container
  is future work.

### 11.2 Compact Reference Container (tools/)

`tools/smazka-bin` converts Line-ASM ⇄ a compact binary container:

- Header: magic `SMVG`, version 1.3, flags, per-section counts.
- IDs and coordinates are **variable-length zigzag VLQs** (LEB128-style).
- Coordinates are **delta-encoded** relative to the previous record of the same
  type (dx, dy), giving typical savings of 3–6× versus raw Q16.16 words.
- Round-trips losslessly: `smazka-line2bin x.smazkavg > x.smvg && smazka-bin2line x.smvg` reproduces the document.

### 11.2b Solve Pipeline (tools/smazka-solve)

`smazka-solve` parses a Line-ASM document into the resolver's in-memory model,
runs `smazka_resolve` (structural / assertions / automata / LP / convex QP via
psolve), and writes the resolved document back: vertex positions and node
translations updated, everything else preserved verbatim. This closes the loop
between the constraint language and the rendered output:

```
in.smazkavg ──► smazka-solve ──► resolved.smazkavg ──► smazka-raster ──► .png
```

See `examples/solve_demo.smazkavg` (min_dist + bbox_clamp + linear_eq).

### 11.3 Code-Golf Dialect (tools/smazka-golf)

The golf dialect targets byte-starved generative environments (Vyxal, Canvas,
Charcoal). It is a *superset* compiler: every `.sg` file expands to canonical
Line-ASM before rendering.

| Construct | Example | Expands to |
|---|---|---|
| Implicit vertex IDs | `v 10 20` | `v 0 10 20` |
| Relative deltas | `+ 5 0` | `v 1 15 20` |
| Polygon (auto edges+face) | `P 0 0 100 0 50 86` | 3 `v`, 3 `e`, 1 `f` |
| Rectangle | `R 0 0 100 50` | 4 `v`, 4 `e`, 1 `f` |
| Circle (4 cubics) | `C 50 50 30` | 4 `v`, 4 `e type=cubic`, 1 `f` |
| Stroke | `S 0 blue 3` | `s 0 0 0000FF 3` |
| Mirror | `M x 0 1 2` | duplicates vertices with negated x |
| Palette colors | `red`, `#f00` | `FF0000` / `F00` hex |

This addresses the audit findings on golfability: IDs are implicit (45%+ of a
naive file is ID bookkeeping), shape primitives are one token, colors compress
to 1–3 bytes, and symmetry needs no manual duplication.

---

*End of SmazkaVG v1.3 Formal Specification*
