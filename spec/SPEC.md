# SmazkaVG v1.1 Formal Specification

> Flat Document Model · SMT + LP/QP Hybrid Solver · Fixed-Point Determinism  
> Revision 1.1.0 — 2026-08-07

---

## Table of Contents

1. [Design Philosophy](#1-design-philosophy)
2. [Binary Layout](#2-binary-layout)
3. [Fixed-Point Arithmetic](#3-fixed-point-arithmetic)
4. [Primitive Types](#4-primitive-types)
5. [Constraint Types (SMT / LP / QP)](#5-constraint-types-smt--lp--qp)
6. [Solver Dispatch & Resolution](#6-solver-dispatch--resolution)
7. [Textual Skin (Line-ASM)](#7-textual-skin-line-asm)
8. [Profiles & Conformance](#8-profiles--conformance)
9. [Security & Bounds](#9-security--bounds)
10. [Normative References](#10-normative-references)

---

## 1. Design Philosophy

### 1.1 Core Axioms

| Axiom | Statement |
|---|---|
| **Flat Document** | All primitives and constraints are declared in a single-level list with global 32-bit IDs. There is **no nesting, no tree, no DAG** in the serialization. Hierarchy, ordering, and grouping are emulated via constraints. |
| **Fixed-Point Determinism** | All stored numerical values use Two's Complement fixed-point (Q16.16 or Q32.32) with saturating arithmetic. No IEEE 754 in storage. |
| **Declarative Constraints** | All relationships between primitives are expressed as typed constraints. The solver (SMT for discrete/logical, LP/QP for continuous optimization) resolves them into a concrete scene graph. |
| **Bounded Computation** | Every solver iteration, loop, and recursion has a compile-time or header-declared upper bound. No unbounded computation is permitted. |
| **Binary-First** | The canonical format is binary. The textual Line-ASM projection is lossless and maps 1:1 to the binary. |

### 1.2 Comparison with Prior Formats

| Feature | SVG | TinyVG | VGC | SmazkaVG v1.1 |
|---|---|---|---|---|
| Document model | Nested XML tree | Flat binary records | Topological VGC | **Flat binary + constraint graph** |
| Arithmetic | IEEE 754 float | Q16.16 fixed | IEEE 754 float | **Q16.16 / Q32.32 saturating** |
| Solver | None | None | Constraint (LP) | **SMT + LP/QP hybrid** |
| Hierarchy | DOM tree | Group IDs | Edge/face trees | **Flat + `parent` constraints** |
| Animation | SMIL/CSS | None | Keyframes | **Constraint-based state machines** |
| Topology | Paths (duplicated) | Paths | Half-edge mesh | **Half-edge + SMT topology rules** |

---

## 2. Binary Layout

### 2.1 Overview

```
┌─────────────────────────────────────────────────────┐
│  HEADER (32 bytes, 4-byte aligned)                  │
├─────────────────────────────────────────────────────┤
│  PRIMITIVES TABLE                                   │
│    ├─ Vertex records   (variable count)             │
│    ├─ Edge records     (variable count)             │
│    ├─ Face records     (variable count)             │
│    ├─ Stroke records   (variable count)             │
│    ├─ Curve records    (variable count)             │
│    └─ Node records     (variable count)             │
├─────────────────────────────────────────────────────┤
│  CONSTRAINTS TABLE                                  │
│    ├─ SMT constraints  (discrete/logical)           │
│    ├─ LP constraints   (linear inequalities)        │
│    └─ QP constraints   (quadratic objectives)       │
├─────────────────────────────────────────────────────┤
│  PAYLOAD SECTION                                    │
│    ├─ AST blobs (analytic functions)                │
│    ├─ Texture blobs (zstd-compressed)               │
│    └─ Metadata key-value pairs                      │
├─────────────────────────────────────────────────────┤
│  FOOTER (8 bytes)                                   │
│    ├─ CRC-32C of all preceding bytes                │
│    └─ End sentinel 0xDEADBEEF                       │
└─────────────────────────────────────────────────────┘
```

All multi-byte integers are **Little-Endian**. All sections are padded to **4-byte alignment**.

### 2.2 Header (32 bytes)

```
Offset  Size  Field              Description
──────  ────  ─────────────────  ───────────────────────────────────────
0x00    4     magic              0x534D5647 ("SMVG" in ASCII)
0x04    2     version_major      uint16, current = 1
0x06    2     version_minor      uint16, current = 1
0x08    4     flags              uint32 bitmask (see §2.3)
0x0C    4     n_vertices         uint32
0x10    4     n_edges            uint32
0x14    4     n_faces            uint32
0x18    4     n_strokes          uint32
0x1C    4     n_curves           uint32
0x20    4     n_nodes            uint32
0x24    4     n_constraints      uint32
0x28    4     solver_config      uint32 (see §2.4)
0x2C    4     header_crc         uint32 CRC-32C of bytes [0x00..0x2B]
```

**Total: 48 bytes** (padded to 48, which is 4-byte aligned).

> Note: The original design used magic "ANIV"; this revision uses "SMVG" (0x534D5647) for brand clarity. The magic is checked first during parsing.

### 2.3 Header Flags (bitmask at offset 0x08)

| Bit | Name | Meaning |
|---|---|---|
| 0 | `HAS_SMT` | Document contains SMT (logical/discrete) constraints |
| 1 | `HAS_LP` | Document contains LP (linear programming) constraints |
| 2 | `HAS_QP` | Document contains QP (quadratic programming) constraints |
| 3 | `HAS_AST` | Document contains analytic AST blobs in payload |
| 4 | `HAS_PAYLOAD` | Document has zstd-compressed payload section |
| 5 | `Q32` | Coordinates use Q32.32 instead of Q16.16 |
| 6 | `COLORSPACE_LINEAR` | Colors stored in linear light (default is sRGB) |
| 7 | `ANIMATED` | Document contains temporal/animation constraints |
| 8–31 | Reserved | Must be zero |

### 2.4 Solver Config (offset 0x28)

```
Bits 0–7:   MAX_ITER       (uint8, default 64, solver iteration cap per resolve)
Bits 8–15:  MAX_MS         (uint8, solver wall-clock cap, default 50ms, units = 1ms)
Bits 16–23: SMT_STRATEGY   (uint8: 0=basic, 1=interval-propagation, 2=bit-vector)
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
- **No IEEE 754 in storage.** Floats may exist only in temporary solver scratch memory.

### 3.3 Saturating Arithmetic

All arithmetic on fixed-point values uses **saturating (clamped) operations**:

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
    int64_t result = prod >> 16;  // shift back to Q16.16
    if (result > INT32_MAX) return INT32_MAX;
    if (result < INT32_MIN) return INT32_MIN;
    return (int32_t)result;
}

// Saturating division for Q16.16
static inline int32_t q16_div_sat(int32_t a, int32_t b) {
    if (b == 0) return (a >= 0) ? INT32_MAX : INT32_MIN;
    int64_t num = (int64_t)a << 16;  // promote to Q32.16 intermediate
    int64_t result = num / (int64_t)b;
    if (result > INT32_MAX) return INT32_MAX;
    if (result < INT32_MIN) return INT32_MIN;
    return (int32_t)result;
}
```

### 3.4 Overflow Semantics

| Operation | Overflow Behavior |
|---|---|
| Add | Saturate to `INT32_MAX` or `INT32_MIN` |
| Subtract | Saturate to `INT32_MAX` or `INT32_MIN` |
| Multiply | Saturate to `INT32_MAX` or `INT32_MIN` |
| Divide by zero | Return `INT32_MAX` (if numerator ≥ 0) or `INT32_MIN` |
| Negate of `INT32_MIN` | Returns `INT32_MAX` |

### 3.5 Solver-Side Arithmetic

The LP/QP solver (psolve) operates internally on `double` for numerical stability. Fixed-point values are **promoted to double** at the solver API boundary. The solver's output is **quantized back** to fixed-point before storage:

```
fixed → double → [solver computes] → double → fixed (round + saturate)
```

This promotion is deterministic because:
1. The input fixed-point values are exact (no floating-point representation ambiguity).
2. The solver is seeded deterministically (no random perturbation).
3. The output is rounded via `round()` (not truncation) and saturated.

---

## 4. Primitive Types

### 4.1 Primitive Type Tags

Each primitive record begins with a 1-byte type tag:

| Tag | Type | Record Size (bytes) |
|---|---|---|
| `0x01` | Vertex | 16 |
| `0x02` | Edge | 12 |
| `0x03` | Face | variable (4 + 4×n_vertices) |
| `0x04` | Stroke | variable (see §4.5) |
| `0x05` | AnalyticCurve | variable (see §4.6) |
| `0x06` | Node (transform group) | 28 |

### 4.2 Vertex Record (16 bytes)

```
Offset  Size  Field     Description
──────  ────  ───────  ────────────────────────────────
0x00    1     type     0x01
0x01    1     pad      Reserved (0x00)
0x02    2     id       uint16 global vertex ID
0x04    4     x        Q16.16 x-coordinate
0x08    4     y        Q16.16 y-coordinate
0x0C    4     flags    uint32 (bit 0: pinned, bit 1: selected, bits 2-31: reserved)
```

When `Q32` flag is set, `x` and `y` are each Q32.32 (8 bytes), making the record 24 bytes.

### 4.3 Edge Record (12 bytes)

```
Offset  Size  Field     Description
──────  ────  ───────  ────────────────────────────────
0x00    1     type     0x02
0x01    1     pad      Reserved (0x00)
0x02    2     id       uint16 global edge ID
0x04    2     v_start  uint16 ID of start vertex
0x06    2     v_end    uint16 ID of end vertex
0x08    4     flags    uint32 (bit 0: shared, bit 1: boundary, bit 2: diffusion, bits 3-31: reserved)
```

Edges are **first-class shared primitives** (VGC-derived). Multiple faces can reference the same edge.

### 4.4 Face Record (variable size)

```
Offset  Size  Field     Description
──────  ────  ───────  ────────────────────────────────
0x00    1     type     0x03
0x01    1     pad      Reserved (0x00)
0x02    2     id       uint16 global face ID
0x04    2     n_edges  uint16 number of edges in face boundary
0x06    2     n_holes  uint16 number of hole loops
0x08    4     fill_id  uint32 fill reference (0 = none)
0x0C    4×n   edges    uint16[] edge IDs (winding: positive = same direction, negative = reversed)
```

The face record size is `12 + 4×n_edges + 4×n_holes + 4×n_holes_edges` bytes, padded to 4-byte alignment.

### 4.5 Stroke Record (variable size)

```
Offset  Size  Field          Description
──────  ────  ─────────────  ────────────────────────────────
0x00    1     type           0x04
0x01    1     width_mode     0=constant, 1=variable, 2=pressure-driven
0x02    2     id             uint16 global stroke ID
0x04    2     n_ctrl_pts     uint16 number of control points
0x06    2     edge_id        uint16 associated edge (or 0xFFFF = none)
0x08    4     color          uint32 packed RGBA (8 bits/channel)
0x0C    4     width_base     Q16.16 base stroke width
0x10    8×n   ctrl_points    Q16.16 (x,y) pairs for cubic Bézier control points
```

Variable-width strokes additionally carry a width profile (array of Q16.16 values at uniform parameter intervals).

### 4.6 AnalyticCurve Record (variable size)

```
Offset  Size  Field          Description
──────  ────  ─────────────  ────────────────────────────────
0x00    1     type           0x05
0x01    1     curve_kind     0=Bézier, 1=B-spline, 2=spiral, 3=SDF-contour
0x02    2     id             uint16 global curve ID
0x04    2     ast_offset     uint16 offset into payload section for AST blob
0x06    2     ast_size       uint16 size of AST blob in bytes
0x08    4     flags          uint32 (bit 0: closed, bit 1: diffusion-left, bit 2: diffusion-right, bits 3-31: reserved)
```

### 4.7 Node Record (28 bytes)

Nodes are the **transform hierarchy emulators**. They carry a local transform and reference a parent via constraints (not nesting).

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

---

## 5. Constraint Types (SMT / LP / QP)

### 5.1 Constraint Record Header

Every constraint record begins with:

```
Offset  Size  Field       Description
──────  ────  ──────────  ────────────────────────────────────
0x00    1     type        0x10 (SMT), 0x20 (LP), 0x30 (QP)
0x01    1     subtype     uint8, see tables below
0x02    2     id          uint16 global constraint ID
0x04    2     payload_sz  uint16 size of constraint-specific payload
```

### 5.2 SMT Constraint Subtypes (type = 0x10)

SMT constraints handle **discrete logic, topology rules, and non-linear reasoning**.

| Subtype | Name | Payload Format | SMT Theory |
|---|---|---|---|
| `0x01` | `edge_connects` | `edge_id(u16), v_start(u16), v_end(u16)` | Uninterpreted Functions |
| `0x02` | `parent` | `child_id(u16), parent_id(u16)` | Integer Arithmetic |
| `0x03` | `group_id` | `prim_id(u16), group(u16)` | Integer Arithmetic |
| `0x04` | `above` | `prim_a(u16), prim_b(u16)` | Integer Arithmetic (ordering) |
| `0x05` | `contains` | `outer_id(u16), inner_id(u16)` | Real Arithmetic + SDF |
| `0x06` | `state_machine` | `state_id(u16), n_transitions(u16), transitions[]` | Bit-vector / Fixed-point |
| `0x07` | `disjunction` | `n_choices(u16), choice_constraint_ids[]` | Boolean |
| `0x08` | `bound_check` | `prim_id(u16), dim(u8), lo(Q16.16), hi(Q16.16)` | Bit-vector (overflow) |
| `0x09` | `xor` | `constraint_a(u16), constraint_b(u16)` | Boolean |
| `0x0A` | `implication` | `antecedent(u16), consequent(u16)` | Boolean |

#### 5.2.1 `state_machine` Payload Detail

```
Offset  Size  Field             Description
──────  ────  ────────────────  ────────────────────────────────
0x00    2     state_id          uint16
0x02    2     n_transitions     uint16
0x04    2     initial_state     uint16
0x06    2     pad               Reserved
0x08    8×n   transitions[]     Each transition:
                                   - target_state (u16)
                                   - trigger_type (u16: 0=time, 1=event, 2=condition)
                                   - trigger_param (Q16.16: time/frame/threshold)
```

Cycles are permitted (e.g., A→B→A). The solver resolves via **fixed-point iteration** over blend weights.

### 5.3 LP Constraint Subtypes (type = 0x20)

LP constraints express **linear inequalities and equalities** for continuous optimization.

| Subtype | Name | Payload Format | Description |
|---|---|---|---|
| `0x01` | `min_dist` | `prim_a(u16), prim_b(u16), distance(Q16.16)` | Distance ≥ d between primitives |
| `0x02` | `diffusion` | `edge_id(u16), left_color(u32), right_color(u32)` | Poisson equation for color diffusion |
| `0x03` | `linear_eq` | Variable-length: `n_terms(u16), (var_id(u16), coeff(Q16.16))[]` | Σ(coeff_i × var_i) = rhs |
| `0x04` | `linear_le` | Same as `linear_eq` but inequality ≤ | Σ(coeff_i × var_i) ≤ rhs |
| `0x05` | `linear_ge` | Same as `linear_eq` but inequality ≥ | Σ(coeff_i × var_i) ≥ rhs |
| `0x06` | `bbox_clamp` | `prim_id(u16), x_min(Q16.16), y_min(Q16.16), x_max(Q16.16), y_max(Q16.16)` | Variable bounded in rectangle |
| `0x07` | `collision_free` | `stroke_a(u16), stroke_b(u16), margin(Q16.16)` | Strokes do not overlap |
| `0x08` | `flow_conservation` | `node_id(u16), flow_var(Q16.16)` | Network flow balance |

#### 5.3.1 Diffusion Curve Resolution

The `diffusion` constraint generates a **Laplacian linear system** over the mesh:

```
L · c = b
```

Where:
- `L` is the discrete Laplacian matrix (cotangent weights, computed from vertex positions)
- `c` is the vector of unknown colors at each vertex
- `b` encodes boundary conditions: vertices adjacent to the diffusion edge get `left_color` on one side and `right_color` on the other

This is solved via the LP solver (linear system = LP with equality constraints).

### 5.4 QP Constraint Subtypes (type = 0x30)

QP constraints express **quadratic objectives** subject to linear constraints.

| Subtype | Name | Payload Format | Objective |
|---|---|---|---|
| `0x01` | `min_curvature` | `curve_id(u16), weight(Q16.16)` | min ∫ κ² ds (Euler-elastica) |
| `0x02` | `min_stretch` | `node_id(u16), weight(Q16.16)` | min ‖Δx‖² (elastic deformation) |
| `0x03` | `ik_target` | `bone_chain_id(u16), target_x(Q16.16), target_y(Q16.16), weight(Q16.16)` | min Σ w_i ‖p_i − target‖² |
| `0x04` | `fair_blend` | `n_vars(u16), var_ids[]` | min Σ(x_i − x̄)² subject to Σx_i = 1 |
| `0x05` | `cage_deform` | `cage_id(u16), weight(Q16.16)` | min ‖M·x − b‖² (mean-value coordinates) |

#### 5.4.1 QP Standard Form

All QP constraints are reduced to the standard form for the solver:

```
minimize    ½ xᵀQx + cᵀx
subject to  Ax ≤ b
            l ≤ x ≤ u
```

Where `Q` is a positive semi-definite matrix, `A` is a linear constraint matrix, and `l, u` are variable bounds.

**Building QP from psolve's LP core:**

The QP solver is implemented as an **active-set method** that wraps the LP simplex:
1. Start from the LP-feasible vertex (solved via psolve).
2. Identify the active set of inequality constraints.
3. Compute the QP search direction by solving a linear system (Newton step on the quadratic objective restricted to the active manifold).
4. Perform a line search along the direction, respecting bounds.
5. Update the active set (add violated constraints, drop constraints with negative multipliers).
6. Repeat until convergence or `MAX_ITER`.

---

## 6. Solver Dispatch & Resolution

### 6.1 Architecture

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

### 6.2 Dispatch Rules

The classifier inspects each constraint's type and subtype:

```
IF constraint.type == SMT (0x10):
    → Route to SMT solver
    
ELIF constraint.type == LP (0x20):
    IF subtype IN {min_dist, collision_free, bbox_clamp, linear_eq, linear_le, linear_ge, flow_conservation}:
        → Accumulate into LP system → solve with psolve
    ELIF subtype == diffusion:
        → Build Laplacian system → solve as LP (linear equations)
        
ELIF constraint.type == QP (0x30):
    → Accumulate into QP system → solve with active-set over psolve
```

### 6.3 Resolution Order

The solver processes constraints in this fixed order (to ensure deterministic results):

1. **Phase 1: Topology (SMT)** — Resolve `edge_connects`, `parent`, `group_id`, `above` constraints. Build the effective scene hierarchy and adjacency.
2. **Phase 2: Continuous Optimization (LP)** — Solve `min_dist`, `collision_free`, `diffusion`, `bbox_clamp` constraints. Compute vertex positions, stroke widths, diffusion colors.
3. **Phase 3: Quadratic Optimization (QP)** — Solve `min_curvature`, `ik_target`, `fair_blend`. Compute spline shapes, bone poses, blend weights.
4. **Phase 4: Temporal (SMT)** — Resolve `state_machine` constraints. Compute animation blend weights via fixed-point iteration.
5. **Phase 5: Validation (SMT)** — Run `bound_check` constraints. Verify no fixed-point overflow occurred.

### 6.4 SMT Solving via Interval Propagation + Fixed-Point Iteration

The SMT solver does **not** use a full SMT-LIB backend (too heavy for embedded use). Instead, it implements a **bounded decision procedure**:

**For Boolean/Integer theories (parent, group_id, above, disjunction, xor, implication):**
- Enumerate small domains (IDs are uint16, so at most 65536 values).
- Use **unit propagation + backtracking** (DPLL-style) with a clause database.
- Bounded by `MAX_ITER` backtracks.

**For Real Arithmetic theories (contains, bound_check):**
- Use **interval arithmetic** over Q16.16 values.
- Each variable has an interval `[lo, hi]` in Q16.16.
- Constraint propagation narrows intervals.
- If an interval becomes empty → UNSAT.
- If all intervals are singletons → SAT (exact assignment).
- Otherwise → UNKNOWN (conservative: flag a warning, use midpoint values).

**For Fixed-Point Iteration (cyclic state machines, cyclic transforms):**
```
Initialize all unknowns to default values.
Repeat up to MAX_ITER times:
    For each cyclic constraint:
        Compute new value from current assignments.
        Clamp to Q16.16 range (saturating).
    IF no value changed by more than 1 Q16.16 unit:
        Converged → DONE.
IF not converged:
    Use last computed values + flag warning.
```

### 6.5 Timeout & Fallback

Every solver invocation checks a tick counter. If wall-clock time exceeds `MAX_MS` (from header `solver_config`):

1. Halt the current solver.
2. Return the last known-good state (from the previous successful resolve, or the default state for the first resolve).
3. Set a `SOLVE_TIMEOUT` warning flag in the document's runtime metadata.

---

## 7. Textual Skin (Line-ASM)

### 7.1 Design Principles

- **One declaration per line** — no nesting, no indentation-based semantics.
- **`#` comments** — everything after `#` to end of line is ignored.
- **Fixed-point literals** — decimal numbers are parsed as Q16.16 (multiplied by 65536 and rounded). Hex values prefixed `0x` are raw integer encodings.
- **IDs** — unsigned decimal integers.
- **Colors** — 6-digit hex (RGB) or 8-digit hex (RGBA), no `#` prefix in the value itself (the field position implies color).

### 7.2 Line Types

```
# Vertices
v <id> <x> <y> [flags]

# Edges  
e <id> <v_start> <v_end> [flags]

# Faces
f <id> <edge_0> <edge_1> ... <edge_n> [fill_id]

# Strokes
s <id> <edge_id> <color> <width> [ctrl_x0 ctrl_y0 ctrl_x1 ctrl_y1 ...]

# Analytic Curves
d <id> <kind> <ast_offset> <ast_size> [flags]

# Nodes (transform groups)
n <id> <tx> <ty> <rot> <sx> <sy> <skew> [content_ref]

# SMT Constraints
c <id> edge_connects <edge_id> <v_start> <v_end>
c <id> parent <child_id> <parent_id>
c <id> group_id <prim_id> <group>
c <id> above <prim_a> <prim_b>
c <id> contains <outer_id> <inner_id>
c <id> state_machine <state_id> <initial> <target_0> <trigger_0> <param_0> ...
c <id> disjunction <c_id_0> <c_id_1> ...
c <id> bound_check <prim_id> <dim> <lo> <hi>
c <id> xor <c_id_a> <c_id_b>
c <id> implication <antecedent_id> <consequent_id>

# LP Constraints
c <id> min_dist <prim_a> <prim_b> <distance>
c <id> diffusion <edge_id> L <left_color> R <right_color>
c <id> linear_eq <rhs> <var_0> <coeff_0> <var_1> <coeff_1> ...
c <id> linear_le <rhs> <var_0> <coeff_0> <var_1> <coeff_1> ...
c <id> linear_ge <rhs> <var_0> <coeff_0> <var_1> <coeff_1> ...
c <id> bbox_clamp <prim_id> <x_min> <y_min> <x_max> <y_max>
c <id> collision_free <stroke_a> <stroke_b> <margin>
c <id> flow_conservation <node_id> <flow>

# QP Constraints
c <id> min_curvature <curve_id> <weight>
c <id> min_stretch <node_id> <weight>
c <id> ik_target <chain_id> <target_x> <target_y> <weight>
c <id> fair_blend <var_0> <var_1> ...
c <id> cage_deform <cage_id> <weight>

# Metadata (optional)
m <key> <value>
```

### 7.3 ABNF Grammar

```abnf
; SmazkaVG Line-ASM Grammar (RFC 5234 ABNF)

document     = *(line LF)
line         = comment / vertex / edge / face / stroke / curve / node / constraint / meta / empty
empty        = ""
comment      = "#" *VCHAR
LF           = %x0A

; ── Primitives ──────────────────────────────────────────────────────

vertex       = "v" SP vid SP number SP number [SP flags]
edge         = "e" SP eid SP vid SP vid [SP flags]
face         = "f" SP fid SP 1*(eid) [SP fid]
stroke       = "s" SP sid SP eid SP hexcolor SP number *(SP number)
curve        = "d" SP cid SP curve-kind SP uint16 SP uint16 [SP flags]
node         = "n" SP nid SP number SP number SP number SP number SP number SP number SP number [SP uint32]

curve-kind   = "bezier" / "bspline" / "spiral" / "sdf"

; ── Constraints ─────────────────────────────────────────────────────

constraint   = "c" SP cid SP constraint-body

constraint-body = smt-constraint / lp-constraint / qp-constraint

; SMT
smt-constraint = c-edge-connects / c-parent / c-group-id / c-above
               / c-contains / c-state-machine / c-disjunction
               / c-bound-check / c-xor / c-implication

c-edge-connects = "edge_connects" SP eid SP vid SP vid
c-parent        = "parent" SP nid SP nid
c-group-id      = "group_id" SP uint32 SP uint16
c-above         = "above" SP uint32 SP uint32
c-contains      = "contains" SP uint32 SP uint32
c-state-machine = "state_machine" SP uint16 SP uint16 SP *(transition)
transition      = uint16 SP trigger-type SP number
trigger-type    = "time" / "event" / "condition"
c-disjunction   = "disjunction" SP 2*(cid)
c-bound-check   = "bound_check" SP uint32 SP dim SP number SP number
c-xor           = "xor" SP cid SP cid
c-implication   = "implication" SP cid SP cid
dim             = "x" / "y"

; LP
lp-constraint   = c-min-dist / c-diffusion / c-linear-eq / c-linear-le
                / c-linear-ge / c-bbox-clamp / c-collision-free
                / c-flow-conservation

c-min-dist        = "min_dist" SP uint32 SP uint32 SP number
c-diffusion       = "diffusion" SP eid SP "L" SP hexcolor SP "R" SP hexcolor
c-linear-eq       = "linear_eq" SP number SP *(variable-term)
c-linear-le       = "linear_le" SP number SP *(variable-term)
c-linear-ge       = "linear_ge" SP number SP *(variable-term)
variable-term     = uint32 SP number    ; variable_id coefficient
c-bbox-clamp      = "bbox_clamp" SP uint32 SP number SP number SP number SP number
c-collision-free  = "collision_free" SP sid SP sid SP number
c-flow-conservation = "flow_conservation" SP uint32 SP number

; QP
qp-constraint   = c-min-curvature / c-min-stretch / c-ik-target
                / c-fair-blend / c-cage-deform

c-min-curvature = "min_curvature" SP cid SP number
c-min-stretch   = "min_stretch" SP nid SP number
c-ik-target     = "ik_target" SP uint32 SP number SP number SP number
c-fair-blend    = "fair_blend" SP *(uint32)
c-cage-deform   = "cage_deform" SP uint32 SP number

; ── Metadata ────────────────────────────────────────────────────────

meta           = "m" SP 1*VCHAR SP 1*VCHAR

; ── Terminals ───────────────────────────────────────────────────────

vid            = uint16          ; vertex ID
eid            = uint16          ; edge ID
fid            = uint16          ; face ID
sid            = uint16          ; stroke ID
cid            = uint16          ; curve ID or constraint ID
nid            = uint16          ; node ID
flags          = uint32
uint16         = 1*5DIGIT        ; 0-65535
uint32         = 1*10DIGIT       ; 0-4294967295
number         = ["-"] 1*DIGIT ["." 1*6DIGIT]   ; decimal fixed-point literal
hexcolor       = 6*8HEXDIG       ; RRGGBB or RRGGBBAA

SP             = " "
DIGIT          = %x30-39
HEXDIG         = DIGIT / "A" / "B" / "C" / "D" / "E" / "F"
                  / "a" / "b" / "c" / "d" / "e" / "f"
VCHAR          = %x21-7E
```

---

## 8. Profiles & Conformance

### 8.1 Profile Definitions

| Profile | ID | Mandatory Features | Forbidden Features |
|---|---|---|---|
| **Full** | 0 | All primitives, all constraint types, SMT+LP+QP | None |
| **Web-Safe** | 1 | Vertices, edges, faces, strokes, nodes. LP constraints only. Q16.16. | SMT, QP, analytic AST, diffusion curves |
| **Print-PDF** | 2 | Vertices, edges, faces, strokes. LP + QP. Q32.32. | Animation, state machines, analytic AST |
| **CAD-Exact** | 3 | All primitives. SMT + LP. Q32.32. No saturation (exact arithmetic via Q64.64 extension). | Animation, QP, analytic AST |
| **Anime-Production** | 4 | All primitives. All solvers. Q16.16. Full animation + rigging. | None |

### 8.2 Conformance

A reader **must** reject files that use features outside the declared profile (or the reader's supported profile set). The `PROFILE_ID` in the header's `solver_config` field indicates the file's profile.

---

## 9. Security & Bounds

### 9.1 Document Limits

| Parameter | Maximum | Enforced By |
|---|---|---|
| Vertices | 65535 | uint16 ID space |
| Edges | 65535 | uint16 ID space |
| Faces | 65535 | uint16 ID space |
| Strokes | 65535 | uint16 ID space |
| Constraints | 65535 | uint16 ID space |
| AST depth | 32 | Parser rejection |
| AST nodes | 256 per blob | Parser rejection |
| State machine states | 256 | Parser rejection |
| State machine transitions | 1024 | Parser rejection |
| Payload section | 16 MB | Header-declared limit |
| Solver iterations | 255 (MAX_ITER) | Header `solver_config` |
| Solver wall time | 255 ms (MAX_MS) | Header `solver_config` |
| Face edge count | 1024 | Parser rejection |
| Stroke control points | 4096 | Parser rejection |

### 9.2 Malformed Input Handling

| Condition | Action |
|---|---|
| Unknown type tag | Reject file (fatal parse error) |
| ID out of range | Reject file |
| Self-referential edge (v_start == v_end) | Warn, skip edge |
| Cyclic parent with no convergence after MAX_ITER | Use last values, flag warning |
| CRC mismatch | Reject file |
| Payload size exceeds 16 MB | Reject file |
| AST depth exceeds 32 | Reject file |

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
8. **SMT-LIB v2** — Standard for SMT solvers. `http://smt-lib.org`
9. **Goldfarb-Reid** — Steepest-edge simplex pricing. Mathematical Programming 75 (1996).
10. **Euler-Elastica** — Curve energy minimization. L. Euler, *Methodus Inveniendi Lineas Curvas*, 1744.

---

*End of SmazkaVG v1.1 Formal Specification*
