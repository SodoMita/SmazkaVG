# SmazkaVG v1.2 — Changelog from v1.1

> Critique-driven revisions. Every change maps to a specific weakness identified in review.

---

## Critical Fixes

### 1. Termination Guarantee (was: SMT bomb)

**Problem:** v1.1 mixed general SMT + LP + QP. General SMT is NP-hard to undecidable. Non-convex QP is NP-hard. A file could exist that no renderer can open in bounded time.

**Fix:** Constraint language is restricted to a **decidable, convex, polynomial-time fragment**:

| Theory | v1.1 | v1.2 | Complexity |
|---|---|---|---|
| Boolean | Full SMT | Bounded unit propagation (domain ≤ 65536) | O(n·d) where d = domain size |
| Integer arithmetic | Full SMT | Bounded enumeration + LP relaxation | O(n·d) |
| Linear real arithmetic | LP | LP (unchanged) | O(n³) worst case (simplex) |
| Quadratic real arithmetic | QP (general) | **Convex QP only** (PSD Q matrices) | O(n²·k) where k = active-set iterations |
| Non-linear | SMT interval prop | **Removed** | — |

**Enforcement:** At parse time, the renderer validates that all QP constraint matrices are positive semi-definite. Non-convex QP is a **parse error**, not a runtime failure. The solver is guaranteed to terminate in O(n³ + n²·MAX_ITER) time.

**Header flag:** Bit 2 now means `HAS_CONVEX_QP` (not general QP). Non-convex QP files are rejected.

### 2. Namespace Split (was: `c` list is four things in one hat)

**Problem:** The `c` (constraint) list mixed redundant assertions, structural data, real geometric constraints, and paint properties. Tooling couldn't decide what to send to the solver vs. renderer vs. validator.

**Fix:** Split into four typed sections, each with a distinct letter prefix:

| Prefix | Section | Contents | Processing |
|---|---|---|---|
| `s` | **Structural** | `parent`, `group_id` | Builds scene graph, no solver |
| `a` | **Assertions** | `edge_connects`, `bound_check` | Validated, errors reported, not solved |
| `c` | **Constraints** | `min_dist`, `bbox_clamp`, `linear_eq`, `collision_free`, `min_curvature` | Sent to LP/convex-QP solver |
| `p` | **Paint** | `diffusion`, `solid_fill`, `gradient`, `pattern` | Sent to rasterizer, not solver |

**Rationale:**
- `s parent` is **not** a constraint — it's structural data. It belongs in `s`.
- `a edge_connects` is a **redundant assertion** — it restates `e <id> <v0> <v1>`. It belongs in `a` (validation), not `c` (solver input).
- `p diffusion` is **paint data** — it specifies colors, not something a solver satisfies. It belongs in `p`.
- Only `c min_dist`, `c bbox_clamp`, etc. are **real constraints** that the LP/QP solver operates on.

**Binary encoding:** The constraint type tag byte now distinguishes sections:
- `0x10` = structural (`s`)
- `0x20` = assertion (`a`)
- `0x30` = constraint (`c`) — LP/QP
- `0x40` = paint (`p`)

### 3. Curves (was: straight segments only — worse than SVG)

**Problem:** Every edge was a straight line between two vertices. For anime line art (hair curls, eye shapes, jawlines), this is disqualifying.

**Fix:** Edges now carry a **curve type** and optional control points:

```
e <id> <v0> <v1> [type=<seg|cubic|bspline>] [cp1x cp1y cp2x cp2y]
```

**Curve types:**

| Type | Meaning | Control Points | Storage |
|---|---|---|---|
| `seg` | Straight segment (v0→v1) | None | 12 bytes (v1.1 compatible) |
| `cubic` | Cubic Bézier | cp1, cp2 (4 numbers) | 28 bytes |
| `bspline` | Uniform cubic B-spline | cp1, cp2, ..., cpn (2n numbers) | 12 + 4n bytes |
| `spiro` | Euler spiral (Spiro) | curvature samples (2n numbers) | 12 + 4n bytes |

**Default:** If no type is specified, the edge is a straight segment (backward-compatible with v1.1).

**Rasterization:** Cubic Bézier edges are tessellated into line segments using **forward differencing** (incremental evaluation, no per-segment `cos`/`sin`). The tessellation density is adaptive: subdivide until the flatness criterion `‖P0 - 2P1 + P2‖ < 0.5` pixels is met.

**Topology:** The edge still has exactly two endpoint vertices (v0, v1). Control points are **geometry**, not topology. Two faces sharing edge `e1` still share the same spine — they just both render the curved version.

### 4. Cyclic Parent Semantics (was: undefined, diverges under composition)

**Problem:** A→B→C→A under normal transform composition diverges. Presented as a "feature" without semantics, it's a footgun.

**Fix:** Cycles in `s parent` are now **explicitly defined as equilibrium rigging constraints**:

- **Acyclic parent chains** (tree): resolved by standard bottom-up transform multiplication. Deterministic, O(n).
- **Cyclic parent chains**: interpreted as a **soft rigging constraint** — the solver finds an equilibrium transform assignment that minimizes:

  ```
  minimize  Σ_i ‖world_i - local_i‖²
  subject to  world_child ≈ world_parent × local_child  (soft, penalized)
  ```

  This is a **convex QP** (PSD quadratic objective, linear constraints). It always converges.

- **Header flag:** If any cycle exists in `s parent`, the `RIG_MODE` flag (bit 7) is set. Renderers that don't support rig mode can reject the file or fall back to ignoring cycles (treating each node as root).

- **No ambiguity:** The spec now states: "A cycle in `s parent` is NOT a hard structural relationship. It is a soft equilibrium constraint solved as convex QP. If the solver fails to converge within MAX_ITER, the file is invalid."

### 5. Variable-Width Stroke Profiles (was: scalar width only)

**Problem:** `s 0 0 FF0000FF 2.0` — width `2.0` is a single number. Anime inking requires tapered "power strokes."

**Fix:** Stroke width is now a **function along the edge parameter**:

```
s <id> <edge_id> <color> <w0> <w1> ... <wn>
```

Where `w0..wn` are width values at **uniform parameter intervals** along the edge. For a cubic Bézier edge with parameter `t ∈ [0,1]`, `w_i` is the width at `t = i/(n-1)`.

**Minimum:** 2 samples (start and end width). More samples = finer taper control.

**Pressure-driven shorthand:** If the stroke has `width_mode=2` (pressure), the width profile is generated from a pressure envelope (array of pressure values at uniform intervals). The rasterizer maps pressure → width via a configurable curve (default: linear).

**Backward compatibility:** A single width value is equivalent to `w0 = w1 = ... = w_n = w` (constant width).

### 6. Color Consistency (was: RGBA for strokes, RGB for diffusion)

**Problem:** Stroke colors were RGBA, diffusion colors were RGB. No color-space tag.

**Fix:** All colors are now **RGBA (32-bit packed)**. Diffusion colors include an alpha channel. The header's `COLORSPACE_LINEAR` flag (bit 6) indicates whether colors are in linear light or sRGB. Default is sRGB.

### 7. Named Fields in Node Definition (was: 7 bare numbers)

**Problem:** `n 2 -5.0 -5.0 0.0 0.5 0.5 0.0 1` — seven positional fields. One misplaced column silently corrupts the transform.

**Fix:** Node fields are now **labeled** (but positional order is still accepted for compactness):

```
# Labeled (recommended for hand-edited files)
n 2 tx=-5.0 ty=-5.0 rot=0.0 sx=0.5 sy=0.5 skew=0.0 content=1

# Positional (compact, for generated files)
n 2 -5.0 -5.0 0.0 0.5 0.5 0.0 1
```

The parser accepts both forms. If the first field after the ID is `key=value`, all fields must be labeled. Otherwise, all fields are positional.

---

## Summary: v1.1 → v1.2

| Aspect | v1.1 | v1.2 |
|---|---|---|
| Solver | SMT + LP + QP (general) | **LP + convex QP only** (decidable, polynomial) |
| Constraint namespace | Single `c` list | **Four sections**: `s` (structural), `a` (assertions), `c` (constraints), `p` (paint) |
| Edge geometry | Straight segments only | **Cubic Bézier, B-spline, Spiro** (plus segments) |
| Stroke width | Scalar | **Array (taper profile)** |
| Cyclic parents | Undefined (diverges) | **Equilibrium rigging (convex QP)** |
| Color format | Inconsistent (RGBA vs RGB) | **Unified RGBA** |
| Node syntax | Positional only | **Labeled or positional** |
| Termination | Not guaranteed | **Guaranteed** (convex LP/QP, bounded iteration) |

---

## Migration from v1.1

v1.1 files are **backward-compatible** with v1.2 readers:
- Edges without a `type=` field are treated as `seg` (straight).
- Strokes with a single width value are treated as constant-width.
- The old `c` prefix is accepted but deprecated; readers should warn and suggest migration.
- The old color formats (RGB for diffusion) are accepted but deprecated.

v1.2 files with new features (curves, taper profiles, labeled nodes) are **not** backward-compatible with v1.1 readers.
