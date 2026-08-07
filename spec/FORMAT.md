# SmazkaVG constraint-first format

**Status:** implementation draft matching `src/smazka.c`

SmazkaVG is a nonsequential, constraint-resolved vector-graphics format based on VGC-style cells. The pinned psolve backend is mandatory.

## 1. Processing model

```text
unordered declarations
  -> VGC-style cell/incidence graph
  -> typed constant/variable properties
  -> hard, soft, and objective rules
  -> psolve LP/MIP (QP and fixed PGS are planned typed lowerings)
  -> immutable resolved properties
  -> cells sorted by unique absolute Z
  -> type-specific painting kernels
```

Reordering source records cannot change the graph, solved properties, canonical output, or render.

## 2. ASCII sigils

| Sigil | Meaning |
|---|---|
| `+` | cell declaration |
| `=` | typed constant property |
| `?` followed by `cell.property:type` | typed variable property |
| `!status` | hard rule with named failure status |
| `?status@tier` | soft approximation rule with named degraded status |
| `~status@tier` | lexicographic objective with named completion status |
| `#` | comment to end of line |

The canonical language is ASCII. Whitespace separates tokens. Source lines are unordered.

## 3. Cells

```text
+V vertex_id
+E edge_id start_vertex end_vertex
+F face_id boundary_edge_0 boundary_edge_1 boundary_edge_2 ...
```

- IDs match `[A-Za-z_][A-Za-z0-9_]*` and are globally unique.
- `+E` incidence must reference vertex cells.
- `+F` is an oriented boundary cycle of at least three edge cells.
- Shared topology uses the same cell ID; duplicate near-equal geometry is not topology.

## 4. Properties

### Constants

```text
=cell.property:bool 0|1
=cell.property:int integer
=cell.property:q16 number
=cell.property:vec2 x y
=cell.property:rgba RRGGBBAA
=cell.property:enum identifier
```

### Variables

A scalar variable gives `seed lo hi`. A vector gives all seeds followed by a `lo hi` pair per component.

```text
?cell.property:bool seed lo hi
?cell.property:int seed lo hi
?cell.property:q16 seed lo hi
?cell.property:vec2 seed_x seed_y lo_x hi_x lo_y hi_y
?cell.property:rgba RRGGBBAA
```

- `bool`, `int`, and variable RGBA channels lower to psolve MIP integer variables.
- `q16` and `vec2` lower to bounded continuous variables and are quantized to Q16.16 after solve.
- Constants are validated and removed from the optimization domain.
- Variable enums are not accepted yet; use constant enums until the one-hot MIP lowering is implemented.

### Required core properties

Every cell:

```text
cell.exists : bool
cell.z      : int
```

Every resolved cell has a unique integral absolute `z`, including vertices. Larger `z` paints later.

Additional required properties:

| Cell | Properties |
|---|---|
| vertex | `xy:vec2` |
| edge | `curve:enum`, `width:q16`, `color:rgba` |
| face | `fill:rgba` |

The implemented edge kernel currently accepts `curve=seg`. Other curve types will be added as typed geometry kernels, not parser/render bypasses.

## 5. Rules and named failure semantics

### Hard

```text
!reject rule_name cell.property.component (=|<=|>=) value
!infeasible rule_name cell.property.component (=|<=|>=) value
```

- `reject` is intended for domain/structural conditions that should have been constant.
- `infeasible` is a solver hard constraint.
- A failed constant hard rule reports its name and status.

### Soft

```text
?degraded@tier rule_name cell.property.component ~= target
?optional@tier rule_name cell.property.component ~= target
```

The absolute residual is minimized at the given nonnegative lexicographic tier.

### Objective

```text
~optimal@tier rule_name cell.property.component min|max
```

Tiers are solved in ascending order. Each tier optimum is frozen before the next tier. Seed deviation is automatically minimized at the final canonical tier, so solver discovery order is never a tie-break rule.

### Components

- scalar: `cell.width`, `cell.z`, `cell.exists`
- vector: `cell.xy.x`, `cell.xy.y`
- color: `cell.color.r`, `.g`, `.b`, `.a`

The current implementation supports one referenced scalar component per rule. General sparse linear expressions are the next parser/lowering extension.

## 6. Existence

`exists` is explicit. Record presence does not imply existence.

```text
=eye.exists:bool 1
?detail.exists:bool 1 0 1
```

Variable existence is an integer MIP decision. A painter never infers existence from opacity, malformed geometry, or record order.

## 7. Painting

Only the immutable solved scene is paintable. Cells are sorted by resolved absolute `z`; source order is ignored.

Case-specific dispatch currently includes:

- segment edge -> SVG `<line>` kernel;
- face with segment-edge cycle -> SVG `<polygon>` kernel;
- `exists=0` -> no paint.

Constants specialize kernels; solved variables become concrete values before dispatch.

## 8. Canonical resolved form

`build/smazka --resolved` emits:

- cells sorted lexicographically by ID;
- properties sorted lexicographically within each cell;
- every property converted to `=` constant form;
- no source rules, seeds, or parser-order artifacts.

This is an immutable solved scene, not a second route around psolve.

## 9. CLI

```sh
build/smazka drawing.smazka --check
build/smazka drawing.smazka --resolved solved.smazka
build/smazka drawing.smazka --svg drawing.svg 512 512
```

## 10. Required invariants

- psolve is present and linked in the default build.
- every ID and property is unique;
- all references resolve after full-file parsing;
- every cell has explicit `exists` and unique absolute `z`;
- every variable has finite bounds and a seed;
- all hard rules hold;
- all objective tiers terminate with an accepted psolve status;
- output is unchanged by source-record permutation;
- renderers consume only solved properties.
