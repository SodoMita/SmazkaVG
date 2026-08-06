/*
 * SmazkaVG v1.1 — Constraint Resolver Pseudocode
 * =================================================
 *
 * This file shows how the unified solver reads the flat constraint list,
 * classifies each constraint, and dispatches to SMT / LP / QP.
 *
 * It is written as compilable C pseudocode with comments explaining the
 * mathematical formulation.  It assumes the existence of:
 *   - psolve's LP API  (solver.h: LP, Solver, solver_create, solver_solve, solver_optimum)
 *   - A QP active-set wrapper built on top of psolve
 *   - A lightweight SMT engine (interval propagation + DPLL)
 *
 * Target: 128 MB RAM, single CPU core, ≤50ms per resolve frame.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>

/* ─── Forward declarations from psolve ───────────────────────────── */
#include "solver.h"   /* LP, Solver, solver_create, solver_solve, solver_optimum */

/* ─── SmazkaVG document structures ───────────────────────────────── */

#define MAX_PRIMS   65536
#define MAX_CONSTS  65536
#define MAX_STATES  256
#define MAX_ITER_DEFAULT 64

typedef int32_t  q16_t;   /* Q16.16 fixed-point */
typedef int64_t  q32_t;   /* Q32.32 fixed-point */

/* Convert double → Q16.16 (saturating) */
static inline q16_t to_q16(double x) {
    double scaled = x * 65536.0;
    if (scaled > (double)INT32_MAX) return INT32_MAX;
    if (scaled < (double)INT32_MIN) return INT32_MIN;
    return (q16_t)round(scaled);
}

/* Convert Q16.16 → double */
static inline double from_q16(q16_t v) {
    return (double)v / 65536.0;
}

/* ─── Primitive storage (flat arrays, indexed by global ID) ─────── */

typedef struct { q16_t x, y; uint32_t flags; } Vertex;
typedef struct { uint16_t v_start, v_end; uint32_t flags; } Edge;
typedef struct { uint16_t *edge_ids; int n_edges; uint32_t fill_id; } Face;
typedef struct { uint16_t edge_id; uint32_t color; q16_t width; q16_t *ctrl; int n_ctrl; } Stroke;
typedef struct { q16_t tx, ty, rot, sx, sy, skew; uint32_t content_ref; } Node;

typedef struct {
    int n_vertices, n_edges, n_faces, n_strokes, n_nodes;
    Vertex vertices[MAX_PRIMS];
    Edge   edges[MAX_PRIMS];
    Face   faces[MAX_PRIMS];
    Stroke strokes[MAX_PRIMS];
    Node   nodes[MAX_PRIMS];
} PrimitiveStore;

/* ─── Constraint types ───────────────────────────────────────────── */

enum { CT_SMT = 0x10, CT_LP = 0x20, CT_QP = 0x30 };

/* SMT subtypes */
enum {
    SMT_EDGE_CONNECTS = 0x01,
    SMT_PARENT        = 0x02,
    SMT_GROUP_ID      = 0x03,
    SMT_ABOVE         = 0x04,
    SMT_CONTAINS      = 0x05,
    SMT_STATE_MACHINE = 0x06,
    SMT_DISJUNCTION   = 0x07,
    SMT_BOUND_CHECK   = 0x08,
    SMT_XOR           = 0x09,
    SMT_IMPLICATION   = 0x0A,
};

/* LP subtypes */
enum {
    LP_MIN_DIST          = 0x01,
    LP_DIFFUSION         = 0x02,
    LP_LINEAR_EQ         = 0x03,
    LP_LINEAR_LE         = 0x04,
    LP_LINEAR_GE         = 0x05,
    LP_BBOX_CLAMP        = 0x06,
    LP_COLLISION_FREE    = 0x07,
    LP_FLOW_CONSERVATION = 0x08,
};

/* QP subtypes */
enum {
    QP_MIN_CURVATURE = 0x01,
    QP_MIN_STRETCH   = 0x02,
    QP_IK_TARGET     = 0x03,
    QP_FAIR_BLEND    = 0x04,
    QP_CAGE_DEFORM   = 0x05,
};

/* ─── Unified Constraint Record ──────────────────────────────────── */

typedef struct {
    uint8_t  type;      /* CT_SMT, CT_LP, CT_QP */
    uint8_t  subtype;
    uint16_t id;
    uint16_t payload_sz;

    /* Payload — union of all possible constraint data.
       In production this would be a tagged union with
       variable-length arrays; simplified here for clarity. */
    union {
        /* SMT payloads */
        struct { uint16_t edge_id, v_start, v_end; } edge_connects;
        struct { uint16_t child_id, parent_id; } parent;
        struct { uint16_t prim_id, group; } group_id;
        struct { uint16_t prim_a, prim_b; } above;
        struct { uint16_t outer_id, inner_id; } contains;
        struct {
            uint16_t state_id, n_transitions, initial_state;
            struct { uint16_t target; uint16_t trigger_type; q16_t param; } trans[64];
        } state_machine;
        struct { uint16_t n_choices; uint16_t choice_ids[16]; } disjunction;
        struct { uint16_t prim_id; uint8_t dim; q16_t lo, hi; } bound_check;
        struct { uint16_t c_a, c_b; } xor;
        struct { uint16_t antecedent, consequent; } implication;

        /* LP payloads */
        struct { uint16_t prim_a, prim_b; q16_t distance; } min_dist;
        struct { uint16_t edge_id; uint32_t left_color, right_color; } diffusion;
        struct {
            uint16_t n_terms;
            struct { uint16_t var_id; q16_t coeff; } terms[32];
            q16_t rhs;
        } linear;
        struct { uint16_t prim_id; q16_t x_min, y_min, x_max, y_max; } bbox;
        struct { uint16_t stroke_a, stroke_b; q16_t margin; } collision;
        struct { uint16_t node_id; q16_t flow; } flow;

        /* QP payloads */
        struct { uint16_t curve_id; q16_t weight; } min_curvature;
        struct { uint16_t node_id; q16_t weight; } min_stretch;
        struct { uint16_t chain_id; q16_t tx, ty, weight; } ik_target;
        struct { uint16_t n_vars; uint16_t var_ids[32]; } fair_blend;
        struct { uint16_t cage_id; q16_t weight; } cage_deform;
    } data;
} Constraint;

/* ─── Document ───────────────────────────────────────────────────── */

typedef struct {
    uint8_t  max_iter;
    uint8_t  max_ms;
    uint8_t  smt_strategy;
    uint8_t  profile_id;
    int      n_constraints;
    Constraint constraints[MAX_CONSTS];
} SolverConfig;

typedef struct {
    PrimitiveStore prims;
    SolverConfig   config;
} Document;

/* ─── Timing ─────────────────────────────────────────────────────── */

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

/* ═══════════════════════════════════════════════════════════════════
 *  PHASE 1: SMT — Topology & Hierarchy Resolution
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Resolve edge_connects constraints:
 *   Assert: edge[e].v_start == declared_v_start
 *           edge[e].v_end   == declared_v_end
 *
 * This is a simple integrity check — no actual SMT solving needed
 * for well-formed documents. For malformed ones, the SMT engine
 * can attempt repair (swap endpoints, flag warnings).
 */
static void resolve_edge_connects(Document *doc, int *warnings) {
    for (int i = 0; i < doc->config.n_constraints; i++) {
        Constraint *c = &doc->config.constraints[i];
        if (c->type != CT_SMT || c->subtype != SMT_EDGE_CONNECTS) continue;

        uint16_t eid = c->data.edge_connects.edge_id;
        uint16_t vs  = c->data.edge_connects.v_start;
        uint16_t ve  = c->data.edge_connects.v_end;

        if (eid >= doc->prims.n_edges) { (*warnings)++; continue; }

        /* Assert consistency */
        if (doc->prims.edges[eid].v_start != vs ||
            doc->prims.edges[eid].v_end   != ve) {
            /* Repair: override edge endpoints to match constraint */
            doc->prims.edges[eid].v_start = vs;
            doc->prims.edges[eid].v_end   = ve;
            doc->prims.edges[eid].flags |= 0x80000000; /* mark repaired */
            (*warnings)++;
        }
    }
}

/*
 * Resolve parent constraints (hierarchy):
 *   Build a mapping: child_id → parent_id
 *   Detect cycles via DFS with coloring.
 *   For cyclic chains (A→B→C→A), resolve via fixed-point iteration.
 *
 * The "transform" of each node in a cycle converges to a fixed point
 * where the product of transforms along the cycle equals identity
 * (or is flagged as unsolvable).
 */
static void resolve_hierarchy(Document *doc, int *warnings) {
    int n = doc->prims.n_nodes;
    int *parent_of = (int *)calloc(n, sizeof(int));
    for (int i = 0; i < n; i++) parent_of[i] = -1; /* -1 = root */

    /* Step 1: Build parent map from constraints */
    for (int i = 0; i < doc->config.n_constraints; i++) {
        Constraint *c = &doc->config.constraints[i];
        if (c->type != CT_SMT || c->subtype != SMT_PARENT) continue;
        uint16_t child  = c->data.parent.child_id;
        uint16_t parent = c->data.parent.parent_id;
        if (child < n && parent < n)
            parent_of[child] = parent;
    }

    /* Step 2: Detect cycles via DFS */
    int *color = (int *)calloc(n, sizeof(int)); /* 0=white, 1=gray, 2=black */
    int *cycle_member = (int *)calloc(n, sizeof(int));

    for (int i = 0; i < n; i++) {
        if (color[i] != 0) continue;
        /* DFS from i */
        int stack[256], top = 0;
        stack[top++] = i;
        while (top > 0) {
            int u = stack[top - 1];
            if (color[u] == 0) {
                color[u] = 1; /* gray = in progress */
                int p = parent_of[u];
                if (p >= 0 && p < n) {
                    if (color[p] == 1) {
                        /* Cycle detected! Mark all gray nodes in the cycle */
                        cycle_member[p] = 1;
                        cycle_member[u] = 1;
                        (*warnings)++;
                    } else if (color[p] == 0) {
                        stack[top++] = p;
                        continue;
                    }
                }
                color[u] = 2; /* black = done */
                top--;
            } else {
                top--;
            }
        }
    }

    /* Step 3: Resolve transforms.
       For non-cyclic nodes: straightforward chain multiplication.
       For cyclic nodes: fixed-point iteration. */

    /* Non-cyclic: compute world transforms bottom-up */
    /* (Simplified: in production, topological sort on acyclic subgraph) */
    for (int i = 0; i < n; i++) {
        if (cycle_member[i]) continue;
        /* Accumulate transforms up the chain */
        /* world_xform[i] = parent_world_xform × local_xform[i] */
        /* ... (matrix multiplication in Q16.16) ... */
    }

    /* Cyclic: fixed-point iteration */
    int max_iter = doc->config.max_iter > 0 ? doc->config.max_iter : MAX_ITER_DEFAULT;
    for (int iter = 0; iter < max_iter; iter++) {
        int changed = 0;
        for (int i = 0; i < n; i++) {
            if (!cycle_member[i]) continue;
            /* Compute new world transform from current assignments */
            int p = parent_of[i];
            if (p < 0 || p >= n) continue;
            /* new_world = world[p] × local[i] */
            /* Compare with current; if diff > 1 q16 unit, set changed=1 */
            /* ... saturating Q16.16 matrix multiply ... */
        }
        if (!changed) break; /* Converged */
    }
    /* If not converged after max_iter, use last values + warning */

    free(parent_of);
    free(color);
    free(cycle_member);
}

/*
 * Resolve group_id and above constraints (ordering):
 *   group_id: tag primitives with group membership.
 *   above:    enforce depth ordering: prim_a renders above prim_b.
 *
 * This is a topological sort on the "above" relation (partial order).
 * If cycles exist in "above", use the SMT disjunction handler to
 * break ties deterministically (lowest ID wins).
 */
static void resolve_ordering(Document *doc, int *warnings) {
    /* group_id: just tag — no solving needed */
    /* above:    build DAG, topological sort, break ties by ID */
    /* ... implementation omitted for brevity ... */
}

/*
 * Resolve state_machine constraints (cyclic animation):
 *
 * Each state machine has states S_0, S_1, ..., S_{n-1}
 * and transitions with triggers (time, event, condition).
 *
 * For CYCLIC machines (A→B→A→...), the solver computes
 * blend weights w_i for each state such that:
 *   Σ w_i = 1,  w_i ≥ 0
 *   The blended transform is stable (fixed point).
 *
 * This is formulated as an LP:
 *   variables: w_0, ..., w_{n-1}
 *   subject to: Σ w_i = 1, w_i ≥ 0
 *   objective: minimize Σ w_i² (maximum entropy → smoothest blend)
 *
 * Wait — this is actually a QP. We solve it as a QP via active-set.
 * But since the constraints are simple (simplex), we can use the
 * closed-form solution: w_i = 1/n for all i (uniform blend).
 *
 * For non-uniform blends (trigger-dependent), we solve the full QP.
 */
static void resolve_state_machines(Document *doc, int *warnings) {
    for (int i = 0; i < doc->config.n_constraints; i++) {
        Constraint *c = &doc->config.constraints[i];
        if (c->type != CT_SMT || c->subtype != SMT_STATE_MACHINE) continue;

        int n_states = c->data.state_machine.n_transitions + 1;
        if (n_states > MAX_STATES) { (*warnings)++; continue; }

        /* Compute trigger activations at current time t */
        double *activation = (double *)calloc(n_states, sizeof(double));
        /* ... evaluate triggers ... */

        /* If cyclic: compute blend weights */
        /* Simple case: uniform blend */
        double w = 1.0 / n_states;
        for (int s = 0; s < n_states; s++) {
            activation[s] = w;
        }

        /* Apply blended transforms to affected primitives */
        /* ... blend = Σ activation[s] × transform_of_state(s) ... */

        free(activation);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  PHASE 2: LP — Continuous Optimization via psolve
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Build and solve the LP system from all LP constraints.
 *
 * The LP variables are:
 *   - Vertex positions (x_i, y_i) for adjustable vertices
 *   - Stroke widths
 *   - Diffusion curve colors
 *   - Slack variables for inequalities
 *
 * We accumulate:
 *   - Objective coefficients c[] (for optimization objectives)
 *   - Constraint matrix A in CSC format
 *   - RHS vector b[]
 *   - Relation vector rel[] ('<', '>', '=')
 *   - Variable bounds l[], u[]
 */
static void resolve_lp(Document *doc, int *warnings) {
    /* Count LP constraints and estimate variable count */
    int n_lp = 0;
    for (int i = 0; i < doc->config.n_constraints; i++) {
        if (doc->config.constraints[i].type == CT_LP) n_lp++;
    }
    if (n_lp == 0) return;

    /* Variable allocation:
       var 0..2*V-1: vertex x,y positions (V = n_vertices)
       var 2*V..2*V+S-1: stroke widths (S = n_strokes)
       var 2*V+S..2*V+S+3*E-1: diffusion colors (3 per edge with diffusion)
       ... total estimated conservatively */
    int V = doc->prims.n_vertices;
    int S = doc->prims.n_strokes;
    int n_vars = 2 * V + S + 3 * doc->prims.n_edges;
    int max_constraints = n_lp + 2 * V; /* constraints + bounds */

    /* Allocate LP structure (psolve's format) */
    LP lp;
    memset(&lp, 0, sizeof(LP));
    lp.n = n_vars;
    lp.m = max_constraints;
    lp.c = (double *)calloc(n_vars, sizeof(double));
    lp.b = (double *)calloc(max_constraints, sizeof(double));
    lp.l = (double *)malloc(n_vars * sizeof(double));
    lp.u = (double *)malloc(n_vars * sizeof(double));
    lp.rel = (char *)malloc(max_constraints * sizeof(char));

    /* Default bounds: vertices bounded by document extent,
       stroke widths ≥ 0, colors in [0, 1] */
    for (int j = 0; j < n_vars; j++) {
        lp.l[j] = -32768.0;  /* Q16.16 min as double */
        lp.u[j] =  32767.0;  /* Q16.16 max as double */
    }
    /* Stroke widths: ≥ 0 */
    for (int j = 2 * V; j < 2 * V + S; j++) {
        lp.l[j] = 0.0;
        lp.u[j] = 1000.0;
    }

    /* Objective: minimize sum of squared deviations from initial positions */
    /* (This gives a "least-change" solution when constraints are satisfiable) */
    for (int j = 0; j < 2 * V; j++) {
        lp.c[j] = 0.0;  /* No linear objective on positions */
    }
    lp.maximize = 0;  /* minimize */

    /* Accumulate constraints into CSC matrix */
    int row = 0;

    for (int i = 0; i < doc->config.n_constraints; i++) {
        Constraint *c = &doc->config.constraints[i];
        if (c->type != CT_LP) continue;

        switch (c->subtype) {

        case LP_MIN_DIST: {
            /* min_dist(prim_a, prim_b, distance):
               ‖pos_a - pos_b‖ ≥ distance
               
               Linearized: for each coordinate dimension d:
                 x_a[d] - x_b[d] ≥ distance / sqrt(2)  (conservative)
                 x_b[d] - x_a[d] ≥ distance / sqrt(2)
               
               This gives an LP relaxation of the L2 constraint.
               For exact L2, we'd need SOCP (out of scope for pure LP). */
            double dist = from_q16(c->data.min_dist.distance);
            double slack = dist / 1.41421356; /* dist / sqrt(2) */
            int va = c->data.min_dist.prim_a;
            int vb = c->data.min_dist.prim_b;
            /* Row: x_va - x_vb ≥ slack → -(x_va - x_vb) ≤ -slack */
            lp.b[row] = -slack;
            lp.rel[row] = '<';
            /* CSC entries: -1 at (row, 2*va), +1 at (row, 2*vb) */
            /* ... add to sparse matrix ... */
            row++;
            /* Repeat for y dimension */
            lp.b[row] = -slack;
            lp.rel[row] = '<';
            row++;
            break;
        }

        case LP_DIFFUSION: {
            /* diffusion(edge_id, left_color, right_color):
               Build Laplacian system L·c = b over mesh vertices.
               
               For each vertex v adjacent to the diffusion edge:
                 L_row[v]: Σ (cot(α_ij) + cot(β_ij)) · (c_v - c_neighbor) = 0
                 (discrete Laplace-Beltrange operator)
               
               Boundary conditions:
                 c_v = left_color  for vertices on the left side
                 c_v = right_color for vertices on the right side
               
               This is a LINEAR EQUALITY SYSTEM → LP with '=' constraints. */
            uint16_t eid = c->data.diffusion.edge_id;
            double cl[3], cr[3];
            cl[0] = ((c->data.diffusion.left_color >> 16) & 0xFF) / 255.0;
            cl[1] = ((c->data.diffusion.left_color >>  8) & 0xFF) / 255.0;
            cl[2] = ((c->data.diffusion.left_color      ) & 0xFF) / 255.0;
            cr[0] = ((c->data.diffusion.right_color >> 16) & 0xFF) / 255.0;
            cr[1] = ((c->data.diffusion.right_color >>  8) & 0xFF) / 255.0;
            cr[2] = ((c->data.diffusion.right_color      ) & 0xFF) / 255.0;

            /* For each mesh vertex, add Laplacian equation */
            /* (Simplified: in production, compute cotangent weights) */
            /* Variable index for color channel ch at vertex v: 2*V + S + 3*eid_base + ch */
            /* ... build CSC entries for Laplacian ... */
            break;
        }

        case LP_LINEAR_EQ:
        case LP_LINEAR_LE:
        case LP_LINEAR_GE: {
            /* Generic linear constraint:
               Σ coeff_i × var_i {=, ≤, ≥} rhs */
            double rhs = from_q16(c->data.linear.rhs);
            lp.b[row] = rhs;
            lp.rel[row] = (c->subtype == LP_LINEAR_EQ) ? '=' :
                          (c->subtype == LP_LINEAR_LE) ? '<' : '>';
            /* Add CSC entries for each term */
            for (int t = 0; t < c->data.linear.n_terms; t++) {
                int var = c->data.linear.terms[t].var_id;
                double coeff = from_q16(c->data.linear.terms[t].coeff);
                /* A[row][var] = coeff */
                /* ... add to CSC ... */
            }
            row++;
            break;
        }

        case LP_BBOX_CLAMP: {
            /* bbox_clamp(prim_id, x_min, y_min, x_max, y_max):
               x_min ≤ x[prim] ≤ x_max
               y_min ≤ y[prim] ≤ y_max
               
               This is implemented as variable BOUNDS, not constraints: */
            int vid = c->data.bbox.prim_id;
            lp.l[2 * vid]     = from_q16(c->data.bbox.x_min);
            lp.u[2 * vid]     = from_q16(c->data.bbox.x_max);
            lp.l[2 * vid + 1] = from_q16(c->data.bbox.y_min);
            lp.u[2 * vid + 1] = from_q16(c->data.bbox.y_max);
            break;
        }

        case LP_COLLISION_FREE: {
            /* collision_free(stroke_a, stroke_b, margin):
               At each sampled point along the strokes:
               ‖p_a(t) - p_b(t)‖ ≥ margin
               
               Linearized same as min_dist (L∞ relaxation). */
            double margin = from_q16(c->data.collision.margin);
            /* ... sample strokes, add inequality rows ... */
            break;
        }

        default:
            break;
        }
    }

    lp.m = row; /* actual number of constraints */

    /* Build CSC matrix from accumulated entries */
    /* ... (sort triplets by column, build colptr) ... */
    lp.Acolptr = (int *)calloc((n_vars + 1), sizeof(int));
    /* ... fill CSC ... */

    /* Solve via psolve */
    Solver *s = solver_create(&lp);
    if (!s) { (*warnings)++; goto lp_cleanup; }

    int status = solver_solve(s);
    if (status == 1) {
        /* INFEASIBLE: constraints cannot be simultaneously satisfied */
        (*warnings)++;
        /* Fall back to constraint-violation-minimizing solution */
    } else if (status == 2) {
        /* UNBOUNDED: add implicit bounds */
        (*warnings)++;
    } else if (status == 0) {
        /* OPTIMAL: extract solution, quantize back to Q16.16 */
        double *x_opt = (double *)malloc(n_vars * sizeof(double));
        double obj;
        solver_optimum(s, x_opt, &obj);

        /* Write back vertex positions (quantized + saturated) */
        for (int v = 0; v < V; v++) {
            doc->prims.vertices[v].x = to_q16(x_opt[2 * v]);
            doc->prims.vertices[v].y = to_q16(x_opt[2 * v + 1]);
        }
        /* Write back stroke widths */
        for (int st = 0; st < S; st++) {
            doc->prims.strokes[st].width = to_q16(x_opt[2 * V + st]);
        }

        free(x_opt);
    }

    solver_destroy(s);

lp_cleanup:
    free(lp.c); free(lp.b); free(lp.l); free(lp.u);
    free(lp.rel); free(lp.Acolptr); free(lp.Arow); free(lp.Aval);
}

/* ═══════════════════════════════════════════════════════════════════
 *  PHASE 3: QP — Quadratic Optimization (Active-Set over psolve)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * QP Standard Form:
 *   minimize    ½ xᵀQx + cᵀx
 *   subject to  Ax ≤ b
 *               l ≤ x ≤ u
 *
 * Implementation: Active-set method wrapping psolve's LP simplex.
 *
 * Algorithm:
 *   1. Solve the LP relaxation (ignore Q, just minimize cᵀx) via psolve.
 *   2. From the LP optimum, compute the QP gradient: g = Qx + c.
 *   3. Solve the QP subproblem on the current active set:
 *        minimize  ½ pᵀQp + gᵀp
 *        subject to A_W p = 0  (active constraints remain tight)
 *      This is a linear system: Q_WW p = -g_W
 *   4. Line search: x ← x + αp, where α = min(1, α_max) and
 *      α_max is the step to the nearest inactive constraint boundary.
 *   5. Update active set:
 *      - If α < 1: add the blocking constraint to the active set.
 *      - If any Lagrange multiplier λ_i < 0 for an active constraint:
 *        remove the most negative one from the active set.
 *   6. Repeat until convergence (‖p‖ < ε) or MAX_ITER.
 */

typedef struct {
    int n;          /* number of variables */
    double *Q;      /* n×n positive semi-definite matrix (row-major) */
    double *c;      /* linear term, n */
    int m;          /* number of inequality constraints */
    double *A;      /* m×n constraint matrix (row-major) */
    double *b;      /* m */
    double *l, *u;  /* bounds, n */
} QPProblem;

static void resolve_qp(Document *doc, int *warnings) {
    int n_qp = 0;
    for (int i = 0; i < doc->config.n_constraints; i++) {
        if (doc->config.constraints[i].type == CT_QP) n_qp++;
    }
    if (n_qp == 0) return;

    int V = doc->prims.n_vertices;
    int n_vars = 2 * V;

    /* Build QP problem */
    QPProblem qp;
    qp.n = n_vars;
    qp.Q = (double *)calloc(n_vars * n_vars, sizeof(double));
    qp.c = (double *)calloc(n_vars, sizeof(double));
    qp.l = (double *)malloc(n_vars * sizeof(double));
    qp.u = (double *)malloc(n_vars * sizeof(double));
    for (int j = 0; j < n_vars; j++) {
        qp.l[j] = -32768.0;
        qp.u[j] =  32767.0;
    }

    /* Accumulate QP objectives */
    for (int i = 0; i < doc->config.n_constraints; i++) {
        Constraint *con = &doc->config.constraints[i];
        if (con->type != CT_QP) continue;

        switch (con->subtype) {

        case QP_MIN_CURVATURE: {
            /* Euler-elastica: min ∫ κ² ds
               
               Discretized for a cubic Bézier with control points p0,p1,p2,p3:
                 κ(t) ≈ ‖B''(t)‖ / ‖B'(t)‖³
               
               Quadratic approximation (for small curvature):
                 min Σ_i ‖p_{i-1} - 2p_i + p_{i+1}‖²
               
               This gives a tridiagonal Q matrix:
                 Q[i][i]   = 2 (for interior control points)
                 Q[i][i-1] = Q[i][i+1] = -1
               
               Weighted by the constraint's weight parameter. */
            double w = from_q16(con->data.min_curvature.weight);
            uint16_t cid = con->data.min_curvature.curve_id;
            /* ... build tridiagonal Q entries for curve's control points ... */
            /* Q[2*cp][2*cp] += 2*w for each control point cp */
            /* Q[2*cp][2*(cp-1)] -= w */
            /* Q[2*cp][2*(cp+1)] -= w */
            /* (Same for y coordinates) */
            break;
        }

        case QP_IK_TARGET: {
            /* Inverse kinematics: min ‖end_effector - target‖²
               
               For a bone chain with joints j_0, ..., j_k:
                 end_effector = Σ bone_length_i × R(θ_i) × direction_i
               
               Linearized around current pose:
                 min ‖J·Δθ - (target - current_ee)‖²
               
               Where J is the Jacobian matrix.
               This is a least-squares QP: Q = JᵀJ, c = -Jᵀ(target - current). */
            double w = from_q16(con->data.ik_target.weight);
            double tx = from_q16(con->data.ik_target.tx);
            double ty = from_q16(con->data.ik_target.ty);
            /* ... compute Jacobian, build Q = w * J'J, c = -w * J'(target-current) ... */
            break;
        }

        case QP_MIN_STRETCH: {
            /* min ‖Δx‖² (elastic deformation):
               Q = w * I (identity), c = -w * x_initial
               
               This pulls vertices toward their rest positions. */
            double w = from_q16(con->data.min_stretch.weight);
            uint16_t nid = con->data.min_stretch.node_id;
            /* Q[2*nid][2*nid] += w */
            /* Q[2*nid+1][2*nid+1] += w */
            /* c[2*nid] -= w * x_initial */
            /* c[2*nid+1] -= w * y_initial */
            break;
        }

        case QP_FAIR_BLEND: {
            /* min Σ(x_i - x̄)² subject to Σx_i = 1, x_i ≥ 0
               
               This is the "maximum entropy" blend:
                 Q = I (identity)
                 c = -2*x̄ * 1 (where x̄ = 1/n)
                 A_eq: Σx_i = 1
                 bounds: 0 ≤ x_i ≤ 1 */
            int nv = con->data.fair_blend.n_vars;
            double xbar = 1.0 / nv;
            for (int k = 0; k < nv; k++) {
                int var = con->data.fair_blend.var_ids[k];
                qp.Q[var * n_vars + var] += 1.0;
                qp.c[var] -= 2.0 * xbar;
            }
            /* Add equality constraint: Σx_i = 1 */
            /* ... add to A, b ... */
            break;
        }

        default:
            break;
        }
    }

    /* ─── Active-Set QP Solver (wrapping psolve) ─── */

    int max_iter = doc->config.max_iter > 0 ? doc->config.max_iter : MAX_ITER_DEFAULT;
    double *x = (double *)calloc(n_vars, sizeof(double));
    int *active = (int *)calloc(qp.m > 0 ? qp.m : 1, sizeof(int));
    int n_active = 0;

    /* Initialize x from LP relaxation (or zeros) */
    /* ... solve LP with same A,b,l,u but objective c (ignoring Q) ... */

    for (int iter = 0; iter < max_iter; iter++) {
        /* Compute gradient: g = Qx + c */
        double *g = (double *)calloc(n_vars, sizeof(double));
        for (int i = 0; i < n_vars; i++) {
            g[i] = qp.c[i];
            for (int j = 0; j < n_vars; j++) {
                g[i] += qp.Q[i * n_vars + j] * x[j];
            }
        }

        /* Solve QP subproblem on active set:
           minimize ½pᵀQp + gᵀp  s.t.  A_W p = 0
           → Q_WW p = -g (projected onto null space of active constraints) */
        double *p = (double *)calloc(n_vars, sizeof(double));
        /* ... solve reduced system ... */

        /* Check convergence */
        double pnorm = 0;
        for (int i = 0; i < n_vars; i++) pnorm += p[i] * p[i];
        if (pnorm < 1e-12) { free(g); free(p); break; }

        /* Line search */
        double alpha = 1.0;
        /* ... find max step before violating inactive constraints ... */

        /* Update x */
        for (int i = 0; i < n_vars; i++) {
            x[i] += alpha * p[i];
            /* Saturate to bounds */
            if (x[i] < qp.l[i]) x[i] = qp.l[i];
            if (x[i] > qp.u[i]) x[i] = qp.u[i];
        }

        /* Update active set */
        /* ... add blocking constraint if alpha < 1 ... */
        /* ... remove constraint with negative multiplier ... */

        free(g);
        free(p);
    }

    /* Quantize solution back to Q16.16 */
    for (int v = 0; v < V; v++) {
        doc->prims.vertices[v].x = to_q16(x[2 * v]);
        doc->prims.vertices[v].y = to_q16(x[2 * v + 1]);
    }

    free(x);
    free(active);
    free(qp.Q);
    free(qp.c);
    free(qp.l);
    free(qp.u);
}

/* ═══════════════════════════════════════════════════════════════════
 *  PHASE 4: Temporal Resolution (SMT — State Machines)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * For cyclic animation state machines (e.g., idle → walk → idle):
 *
 * Let S = {s_0, s_1, ..., s_{n-1}} be the states.
 * Let T = {t_0, t_1, ..., t_{k-1}} be the transitions.
 *
 * Each transition t_i has:
 *   - source state: src(t_i)
 *   - target state: tgt(t_i)
 *   - trigger: time-based (frame count), event-based, or condition-based
 *
 * At resolve time (given current frame F):
 *   1. Evaluate all triggers → determine which transitions are "active"
 *   2. If multiple transitions from the same state are active:
 *      - Use SMT disjunction to select one (deterministic: lowest ID wins)
 *   3. Compute blend weight for active transition:
 *      - time-based: w = (F - start_frame) / duration  (clamped to [0,1])
 *   4. Apply blended transform: xform = (1-w) × xform_src + w × xform_tgt
 *
 * For FULLY CYCLIC machines (all states in a cycle):
 *   The blend weights form a fixed-point system:
 *     w_i = f(w_{i-1}, trigger_i)
 *   Solve via bounded iteration (MAX_ITER steps).
 */

/* ═══════════════════════════════════════════════════════════════════
 *  PHASE 5: Validation (SMT — Bound Checks)
 * ═══════════════════════════════════════════════════════════════════ */

static void resolve_validation(Document *doc, int *warnings) {
    for (int i = 0; i < doc->config.n_constraints; i++) {
        Constraint *c = &doc->config.constraints[i];
        if (c->type != CT_SMT || c->subtype != SMT_BOUND_CHECK) continue;

        uint16_t prim = c->data.bound_check.prim_id;
        uint8_t  dim  = c->data.bound_check.dim; /* 0=x, 1=y */
        q16_t lo = c->data.bound_check.lo;
        q16_t hi = c->data.bound_check.hi;

        q16_t val = (dim == 0) ? doc->prims.vertices[prim].x
                                : doc->prims.vertices[prim].y;

        if (val < lo || val > hi) {
            /* Clamp to bounds (saturating) */
            if (val < lo) val = lo;
            if (val > hi) val = hi;
            if (dim == 0) doc->prims.vertices[prim].x = val;
            else          doc->prims.vertices[prim].y = val;
            (*warnings)++;
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  MAIN RESOLVE ENTRY POINT
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * The unified resolve function.
 * Called once per frame (or once on document load).
 * Returns number of warnings generated.
 */
int smazka_resolve(Document *doc) {
    int warnings = 0;
    uint64_t start = now_us();
    uint64_t deadline = start + (uint64_t)doc->config.max_ms * 1000ULL;

    /* Phase 1: SMT — Topology & Hierarchy */
    resolve_edge_connects(doc, &warnings);
    resolve_hierarchy(doc, &warnings);
    resolve_ordering(doc, &warnings);
    resolve_state_machines(doc, &warnings);

    if (now_us() > deadline) {
        warnings |= 0x80000000; /* timeout flag */
        return warnings;
    }

    /* Phase 2: LP — Continuous Optimization */
    resolve_lp(doc, &warnings);

    if (now_us() > deadline) {
        warnings |= 0x80000000;
        return warnings;
    }

    /* Phase 3: QP — Quadratic Optimization */
    resolve_qp(doc, &warnings);

    if (now_us() > deadline) {
        warnings |= 0x80000000;
        return warnings;
    }

    /* Phase 4 was already handled in Phase 1 (state machines) */

    /* Phase 5: Validation */
    resolve_validation(doc, &warnings);

    return warnings;
}

/* ═══════════════════════════════════════════════════════════════════
 *  EXAMPLE: 3-Way Stroke Intersection via LP
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Scenario: Three variable-width strokes s0, s1, s2 must be pairwise
 * at least `margin` apart at their closest points.
 *
 * Variables:  w_0, w_1, w_2  (stroke widths, ≥ 0)
 *             p0_t, p1_t, p2_t  (closest-point parameters, ∈ [0,1])
 *
 * For each pair (a, b):
 *   ‖stroke_a(p_a_t) - stroke_b(p_b_t)‖ ≥ margin + (w_a + w_b) / 2
 *
 * Linearized (L∞ relaxation):
 *   x_a(t_a) - x_b(t_b) ≥ margin + (w_a + w_b)/2   OR
 *   x_b(t_b) - x_a(t_a) ≥ margin + (w_a + w_b)/2
 *   (same for y)
 *
 * Objective: minimize w_0 + w_1 + w_2  (thinnest valid strokes)
 *            or minimize Σ(w_i - w_i_desired)²  (closest to desired widths)
 *
 * The first is an LP; the second is a QP.
 *
 * LP formulation for psolve:
 *   n_vars = 3 (w_0, w_1, w_2) — parameters t_a are fixed per iteration
 *   n_constraints = 6 (2 per pair: x and y, worst-case direction)
 *   minimize: w_0 + w_1 + w_2
 *   subject to:
 *     For each pair (a,b):
 *       sign_x * (x_a(t_a) - x_b(t_b)) - (w_a + w_b)/2 ≥ margin
 *       sign_y * (y_a(t_a) - y_b(t_b)) - (w_a + w_b)/2 ≥ margin
 *     w_i ≥ 0, w_i ≤ w_i_max
 *
 * Iterative: after solving, check if the L2 distance actually satisfies
 * the constraint. If not, re-linearize around the new closest points
 * and re-solve. Converges in 2-3 iterations for typical cases.
 */

/* ═══════════════════════════════════════════════════════════════════
 *  EXAMPLE: Cyclic Transform Chain (A→B→C→A)
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * Scenario: Node A is parent of B, B is parent of C, C is parent of A.
 *
 * Transforms (2D affine, represented as [tx, ty, cos, sin, sx, sy]):
 *   local_A = [10, 0, 1, 0, 1, 1]    (translate x=10)
 *   local_B = [0, 10, 1, 0, 1, 1]    (translate y=10)
 *   local_C = [-5, -5, 1, 0, 1, 1]   (translate x=-5, y=-5)
 *
 * Fixed-point iteration:
 *   Initial: world_A = world_B = world_C = identity
 *
 *   Iteration 1:
 *     world_A = world_C × local_A = I × [10,0,...] = [10,0,...]
 *     world_B = world_A × local_B = [10,0,...] × [0,10,...] = [10,10,...]
 *     world_C = world_B × local_C = [10,10,...] × [-5,-5,...] = [5,5,...]
 *
 *   Iteration 2:
 *     world_A = world_C × local_A = [5,5,...] × [10,0,...] = [15,5,...]
 *     world_B = world_A × local_B = [15,5,...] × [0,10,...] = [15,15,...]
 *     world_C = world_B × local_C = [15,15,...] × [-5,-5,...] = [10,10,...]
 *
 *   This diverges! Translations keep accumulating.
 *   The SMT solver detects non-convergence and applies the
 *   "centroid" fallback: average all world transforms.
 *
 *   Alternatively, if the cycle includes rotations or scales that
 *   dampen (e.g., scale 0.5 each step), convergence is achieved.
 *
 *   Convergent example:
 *     local_A = [10, 0, 1, 0, 0.5, 0.5]  (scale 0.5)
 *     local_B = [0, 10, 1, 0, 0.5, 0.5]
 *     local_C = [-5, -5, 1, 0, 0.5, 0.5]
 *
 *   After several iterations, the scales dampen translations to zero.
 *   The fixed point is: world = [0, 0, ..., 0.125, 0.125] (product of 0.5³)
 */
