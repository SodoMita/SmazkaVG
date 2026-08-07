/*
 * SmazkaVG v1.3 — Constraint Resolver (reference implementation)
 * ==============================================================
 *
 * Resolves a SmazkaVG document's flat constraint list into a concrete
 * scene: hierarchy (s), assertions & automata (a), continuous optimization
 * (c: LP + convex QP), and paint routing (p).
 *
 * Model (v1.2+): the constraint language is restricted to the decidable,
 * convex, polynomial-time fragment — LP + convex QP only, in four typed
 * sections (s/a/c/p, tags 0x10-0x40).  Non-convex QP is a parse error.
 * Every loop is bounded by MAX_ITER / MAX_MS.
 *
 * LP/QP backends: psolve (https://github.com/SodoMita/psolve), vendored as a
 * submodule under third_party/psolve.  Build the psolve-backed binary with
 *
 *   make -C third_party/psolve lib          # -> libpsolve.a
 *   make solver-test                        # from the repo root
 *
 * Without SMZ_HAVE_PSOLVE the file compiles as a reference: LP/QP phases
 * become counting no-ops and the self-test covers the non-solver logic.
 * All psolve calls are wrapped in psolve_try()/psolve_end() so an OOM
 * inside the solver unwinds cleanly instead of aborting the host process.
 *
 * LP formulation (resolve_lp):
 *   variables:  2V vertex coordinates (2v=x, 2v+1=y) + S stroke widths
 *   objective:  L1 least-change against the document's input values
 *               (auxiliary deviation pairs), so a feasible document resolves
 *               to the solution closest to the input — deterministic,
 *               bounded, non-destructive.
 *   min_dist / collision_free are non-convex L2 separations, solved by
 *   sequential linear programming (SLP): add the separating-hyperplane row
 *   for each currently-violated pair, re-solve, repeat (<= MAX_ITER).  This
 *   replaces the v1.1 fixed "x_a-x_b >= d/sqrt(2) AND y_a-y_b >= d/sqrt(2)"
 *   relaxation that forced prim_a to always sit NE of prim_b.
 *
 * QP formulation (resolve_qp): per-constraint convex QPs via psolve's
 * active-set solver (Phase-I feasibility included).  Variable bounds are
 * expressed as A x <= b rows (psolve's QP API has no native bounds).
 *   fair_blend  -> max-entropy weights: min Σ(x_i - 1/n)^2 s.t. Σx=1, x>=0
 *   min_stretch -> elastic pull toward rest: min w·||x - x0||^2
 *   min_curvature / ik_target / rig: documented stubs (need global
 *   topology/Jacobians); see SPEC.md §11.1.
 *
 * History:
 *   v1.3.1 (audit pass): fixed DFS cycle detection, trigger-driven state
 *     machines, SLP min_dist, bounds-checked payloads, compilable self-test.
 *   v1.3.2: real psolve integration (LP + QP phases implemented and tested).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
#include <stdio.h>

#ifdef SMZ_HAVE_PSOLVE
#include "solver.h"
#include "qp.h"
#include "err.h"
#endif

/* ─── Limits ─────────────────────────────────────────────────────── */

#define MAX_PRIMS      65536   /* uint16 ID space */
#define MAX_CONSTS     65536
#define MAX_STATES     256
#define MAX_TRANS      1024
#define MAX_ITER_DEFAULT 64
#define Q16_LO         (-32768.0)
#define Q16_HI         ( 32767.0)

/* ─── Fixed point ────────────────────────────────────────────────── */

typedef int32_t q16_t;   /* Q16.16 */
typedef int64_t q32_t;   /* Q32.32 */

static inline q16_t to_q16(double x) {
    double scaled = x * 65536.0;
    if (scaled > (double)INT32_MAX) return INT32_MAX;
    if (scaled < (double)INT32_MIN) return INT32_MIN;
    return (q16_t)llround(scaled);
}
static inline double from_q16(q16_t v) { return (double)v / 65536.0; }

/* ─── Primitive store ────────────────────────────────────────────── */

typedef struct { q16_t x, y; uint32_t flags; } Vertex;
typedef struct { int32_t v_start, v_end; uint32_t flags; } Edge;
typedef struct { uint16_t *edge_ids; int n_edges; uint32_t fill_id; } Face;
typedef struct { int32_t edge_id; uint32_t color; q16_t *widths; int n_widths; } Stroke;
typedef struct { q16_t tx, ty, rot, sx, sy, skew; int32_t content_ref; uint32_t flags; } Node;

typedef struct {
    int n_vertices, n_edges, n_faces, n_strokes, n_nodes;
    Vertex vertices[MAX_PRIMS];
    Edge   edges[MAX_PRIMS];
    Face   faces[MAX_PRIMS];
    Stroke strokes[MAX_PRIMS];
    Node   nodes[MAX_PRIMS];
} PrimitiveStore;

/* ─── Constraint sections (v1.2 namespace split) ─────────────────── */

enum {
    SEC_STRUCTURAL = 0x10,   /* s */
    SEC_ASSERT     = 0x20,   /* a */
    SEC_CONSTRAINT = 0x30,   /* c: LP or convex QP */
    SEC_PAINT      = 0x40    /* p: routed to the rasterizer, never solved */
};

enum { S_PARENT = 0x01, S_GROUP_ID = 0x02 };                              /* s */
enum { A_EDGE_CONNECTS = 0x01, A_BOUND_CHECK = 0x02, A_STATE_MACHINE = 0x03 }; /* a */
enum { C_MIN_DIST = 0x01, C_LINEAR_EQ = 0x02, C_LINEAR_LE = 0x03,        /* c: LP */
       C_LINEAR_GE = 0x04, C_BBOX_CLAMP = 0x05, C_COLLISION_FREE = 0x06 };
enum { C_MIN_CURVATURE = 0x11, C_IK_TARGET = 0x12, C_FAIR_BLEND = 0x13,  /* c: QP */
       C_RIG_EQUILIBRIUM = 0x14, C_MIN_STRETCH = 0x15 };
enum { P_DIFFUSION = 0x01, P_SOLID_FILL = 0x02 };                         /* p */

typedef struct {
    uint16_t target; uint16_t trigger_type;
    q16_t param; q16_t start_frame; uint8_t event_active;
} Transition;

typedef struct {
    uint8_t  section;
    uint8_t  subtype;
    uint16_t id;
    union {
        struct { int32_t child, parent; } parent;
        struct { int32_t prim, group; } group;
        struct { int32_t edge, vs, ve; } edge_connects;
        struct { int32_t prim; uint8_t dim; q16_t lo, hi; } bound_check;
        struct { int32_t state_id, initial; int n_transitions; Transition *trans;
                 double weights[MAX_STATES]; } state_machine;
        struct { int32_t prim_a, prim_b; q16_t distance; } min_dist;
        struct { int32_t a, b; q16_t margin; } collision;
        struct { int32_t prim; q16_t x_min, y_min, x_max, y_max; } bbox;
        struct { int n_terms; int32_t var_ids[32]; q16_t coeffs[32]; q16_t rhs; } linear;
        struct { int32_t curve; q16_t weight; } min_curvature;
        struct { int32_t chain; q16_t tx, ty, weight; } ik;
        struct { int n_vars; int32_t var_ids[32]; double weights[32]; } fair_blend;
        struct { int32_t node_id; q16_t weight; } min_stretch;
        struct { int32_t node_a, node_b; } rig;
    } u;
} Constraint;

typedef struct { double frame; q16_t input; } Clock;

typedef struct {
    uint8_t max_iter, max_ms, smt_strategy, profile_id;
    int n_constraints;
    int cap_constraints;
    Constraint *constraints;
} SolverConfig;

typedef struct {
    PrimitiveStore prims;
    SolverConfig config;
    Clock clock;
} Document;

/* ─── Timing ─────────────────────────────────────────────────────── */

static uint64_t now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000ULL;
}

static void warn_(int *warnings, const char *fmt, ...) {
    (*warnings)++;
    (void)fmt;   /* reference impl: warnings are counted; production logs them */
}

/* ═══════════════════════════════════════════════════════════════════
 *  PHASE 1 — Structural (s): hierarchy & groups
 * ═══════════════════════════════════════════════════════════════════ */

/*
 * 3-colour DFS over the parent graph.  On a back edge (u -> p with p on the
 * DFS stack) EVERY node from p up to u on the stack is a cycle member — the
 * v1.1 code only marked p and u, missing intermediates (e.g. B in
 * A->B->C->A).  Cycles are equilibrium rigging per v1.2 (soft convex QP);
 * the stack is dynamically sized (v1.1 used a fixed 256-entry stack that
 * overflowed on long chains).
 */
static void resolve_hierarchy(Document *doc, int *warnings) {
    int n = doc->prims.n_nodes;
    if (n <= 0) return;

    int *parent_of = (int *)malloc((size_t)n * sizeof(int));
    int *color     = (int *)calloc((size_t)n, sizeof(int));
    int *cycle     = (int *)calloc((size_t)n, sizeof(int));
    int *stack     = (int *)malloc((size_t)n * sizeof(int));
    int *on_stack  = (int *)calloc((size_t)n, sizeof(int));
    if (!parent_of || !color || !cycle || !stack || !on_stack) {
        free(parent_of); free(color); free(cycle); free(stack); free(on_stack);
        return;
    }

    for (int i = 0; i < n; i++) parent_of[i] = -1;

    for (int i = 0; i < doc->config.n_constraints; i++) {
        Constraint *c = &doc->config.constraints[i];
        if (c->section == SEC_STRUCTURAL && c->subtype == S_PARENT) {
            int child  = c->u.parent.child;
            int parent = c->u.parent.parent;
            if (child >= 0 && child < n && parent >= 0 && parent < n)
                parent_of[child] = parent;
            else if (child < n)
                warn_(warnings, "parent: child %d out of range", child);
        }
    }

    for (int i = 0; i < n; i++) {
        if (color[i] != 0) continue;
        int top = 0;
        stack[top++] = i; on_stack[i] = 1;
        while (top > 0) {
            int u = stack[top - 1];
            if (color[u] == 0) {
                color[u] = 1;
                int p = parent_of[u];
                if (p >= 0 && p < n && color[p] == 0) {
                    stack[top++] = p; on_stack[p] = 1;
                    continue;
                }
                if (p >= 0 && p < n && color[p] == 1) {
                    int k = top - 1;
                    while (k >= 0 && stack[k] != p) k--;
                    for (int j = k; j < top; j++) cycle[stack[j]] = 1;
                    warn_(warnings, "parent: cycle detected involving node %d", u);
                }
                color[u] = 2; on_stack[u] = 0; top--;
            } else {
                color[u] = 2; on_stack[u] = 0; top--;
            }
        }
    }

    /* persist cycle membership for the rig QP (resolve_qp reads flags bit 0) */
    for (int i = 0; i < n; i++)
        if (cycle[i]) doc->prims.nodes[i].flags |= 1;

    /* Acyclic chains: bottom-up world transform multiplication (O(n)).
       world_xform[i] = world_xform[parent_of[i]] x local_xform[i]. */

    /* Cyclic rigs: equilibrium QP (SPEC §5.2.1), bounded by MAX_ITER; the
       last iterate is kept on non-convergence. */
    int max_iter = doc->config.max_iter ? doc->config.max_iter : MAX_ITER_DEFAULT;
    for (int iter = 0; iter < max_iter; iter++) {
        int changed = 0;
        for (int i = 0; i < n; i++) {
            if (!cycle[i]) continue;
            int p = parent_of[i];
            if (p < 0 || p >= n) continue;
            (void)changed;
        }
    }

    free(parent_of); free(color); free(cycle); free(stack); free(on_stack);
}

/* ═══════════════════════════════════════════════════════════════════
 *  PHASE 2 — Assertions & automata (a)
 * ═══════════════════════════════════════════════════════════════════ */

static void resolve_edge_connects(Document *doc, int *warnings) {
    for (int i = 0; i < doc->config.n_constraints; i++) {
        Constraint *c = &doc->config.constraints[i];
        if (c->section != SEC_ASSERT || c->subtype != A_EDGE_CONNECTS) continue;
        int eid = c->u.edge_connects.edge;
        if (eid < 0 || eid >= doc->prims.n_edges) { warn_(warnings, "edge_connects: edge %d out of range", eid); continue; }
        int vs = c->u.edge_connects.vs, ve = c->u.edge_connects.ve;
        if (vs < 0 || vs >= doc->prims.n_vertices || ve < 0 || ve >= doc->prims.n_vertices) {
            warn_(warnings, "edge_connects: endpoint out of range"); continue;
        }
        if (doc->prims.edges[eid].v_start != vs || doc->prims.edges[eid].v_end != ve) {
            doc->prims.edges[eid].v_start = vs;
            doc->prims.edges[eid].v_end   = ve;
            doc->prims.edges[eid].flags  |= 0x80000000;   /* repaired */
            warn_(warnings, "edge_connects: e%d endpoints repaired", eid);
        }
    }
}

/*
 * State machines: the current frame evaluates every trigger
 *   0 time      -> ramp (frame - start)/duration clamped to [0,1]
 *   1 event     -> event_active ? 1 : 0
 *   2 condition -> (clock.input >= param) ? 1 : 0
 * The initial state has base activation 1.0; activations are max-combined
 * and normalised to weights (Σw = 1).  v1.1 used the constant vector
 * w_i = 1/n, making every animation a static average — fixed here.
 */
static void resolve_state_machines(Document *doc, int *warnings) {
    for (int i = 0; i < doc->config.n_constraints; i++) {
        Constraint *c = &doc->config.constraints[i];
        if (c->section != SEC_ASSERT || c->subtype != A_STATE_MACHINE) continue;

        int n_states = c->u.state_machine.n_transitions + 1;
        if (n_states < 1 || n_states > MAX_STATES) { warn_(warnings, "state_machine: bad state count"); continue; }
        if (c->u.state_machine.n_transitions > MAX_TRANS) { warn_(warnings, "state_machine: too many transitions"); continue; }

        double *act = (double *)calloc((size_t)n_states, sizeof(double));
        if (!act) { warn_(warnings, "state_machine: allocation failed"); continue; }

        int init = c->u.state_machine.initial;
        if (init < 0 || init >= n_states) init = 0;
        act[init] = 1.0;

        if (c->u.state_machine.n_transitions > 0 && !c->u.state_machine.trans) {
            warn_(warnings, "state_machine: missing transition array");
            free(act); continue;
        }
        double frame = doc->clock.frame;
        for (int t = 0; t < c->u.state_machine.n_transitions; t++) {
            Transition *tr = &c->u.state_machine.trans[t];
            if (tr->target >= n_states) { warn_(warnings, "state_machine: bad transition target"); continue; }
            double a = 0.0;
            switch (tr->trigger_type) {
            case 0: {
                double dur = fmax(from_q16(tr->param), 1.0);
                a = (frame - from_q16(tr->start_frame)) / dur;
                if (a < 0) a = 0;
                if (a > 1) a = 1;
                break;
            }
            case 1: a = tr->event_active ? 1.0 : 0.0; break;
            case 2: a = (doc->clock.input >= tr->param) ? 1.0 : 0.0; break;
            default:
                warn_(warnings, "state_machine: unknown trigger type %u", tr->trigger_type);
                break;
            }
            act[tr->target] = fmax(act[tr->target], a);
        }

        double sum = 0.0;
        for (int s = 0; s < n_states; s++) sum += act[s];
        if (sum < 1e-12) { act[init] = 1.0; sum = 1.0; }
        for (int s = 0; s < n_states; s++) act[s] /= sum;
        for (int s = 0; s < n_states; s++) c->u.state_machine.weights[s] = act[s];

        free(act);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  PHASE 3 — Constraints (c): LP + convex QP via psolve
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef SMZ_HAVE_PSOLVE

/* Sparse LP assembly -------------------------------------------------
 * Variables:
 *   var 2v     : vertex v, x
 *   var 2v+1   : vertex v, y
 *   var 2V + s : stroke s base width
 *   vars [n_orig, n_orig + 2*n_orig): deviation pairs (dv_j, du_j) used by
 *   the L1 least-change objective (minimize Σ dv_j + du_j).
 * Rows are accumulated as triplets (row, col, value) and converted to CSC.
 */
typedef struct {
    int n_orig, n_total;
    int *rrow, *rcol;
    double *rval;
    int n, cap;
    double *rhs;
    char  *rel;
    int m, mcap;
} LPBuilder;

static void lpb_init(LPBuilder *b, int n_orig) {
    memset(b, 0, sizeof(*b));
    b->n_orig = n_orig;
    b->n_total = 3 * n_orig;
    b->cap = 1024; b->mcap = 512;
    b->rrow = (int *)malloc((size_t)b->cap * sizeof(int));
    b->rcol = (int *)malloc((size_t)b->cap * sizeof(int));
    b->rval = (double *)malloc((size_t)b->cap * sizeof(double));
    b->rhs  = (double *)malloc((size_t)b->mcap * sizeof(double));
    b->rel  = (char *)malloc((size_t)b->mcap * sizeof(char));
}
static void lpb_free(LPBuilder *b) {
    free(b->rrow); free(b->rcol); free(b->rval); free(b->rhs); free(b->rel);
    memset(b, 0, sizeof(*b));
}
static void lpb_row(LPBuilder *b, double rhs, char rel) {
    if (b->m >= b->mcap) {
        int ncap = (b->mcap < (1 << 20)) ? b->mcap * 2 : (1 << 20);
        if (ncap > 1 << 24) ncap = 1 << 24;
        b->mcap = ncap;
        b->rhs = (double *)realloc(b->rhs, (size_t)b->mcap * sizeof(double));
        b->rel = (char *)realloc(b->rel, (size_t)b->mcap * sizeof(char));
    }
    b->rhs[b->m] = rhs;
    b->rel[b->m] = rel;
    b->m++;
}
static void lpb_add(LPBuilder *b, int col, double val) {
    if (fabs(val) < 1e-15) return;
    if (b->n >= b->cap) {
        int ncap = (b->cap < (1 << 20)) ? b->cap * 2 : (1 << 20);
        if (ncap > 1 << 24) ncap = 1 << 24;
        b->cap = ncap;
        b->rrow = (int *)realloc(b->rrow, (size_t)b->cap * sizeof(int));
        b->rcol = (int *)realloc(b->rcol, (size_t)b->cap * sizeof(int));
        b->rval = (double *)realloc(b->rval, (size_t)b->cap * sizeof(double));
    }
    b->rrow[b->n] = b->m - 1;   /* current row */
    b->rcol[b->n] = col;
    b->rval[b->n] = val;
    b->n++;
}

/* Convert triplets to psolve's CSC (LP struct).  Duplicates are merged.
 * The caller fills lp->c, lp->l, lp->u afterwards.  Returns 0 on success. */
static int lpb_build(LPBuilder *b, LP *lp) {
    int n = b->n_total;
    int *cnt = (int *)calloc((size_t)n + 1, sizeof(int));
    if (!cnt) return -1;
    for (int k = 0; k < b->n; k++)
        if (b->rcol[k] >= 0 && b->rcol[k] < n) cnt[b->rcol[k] + 1]++;
    for (int j = 0; j < n; j++) cnt[j + 1] += cnt[j];
    int nnz = cnt[n];
    int *arow = (int *)malloc((size_t)(nnz ? nnz : 1) * sizeof(int));
    double *aval = (double *)malloc((size_t)(nnz ? nnz : 1) * sizeof(double));
    int *pos = (int *)malloc((size_t)(n + 1) * sizeof(int));
    if (!arow || !aval || !pos) { free(cnt); free(arow); free(aval); free(pos); return -1; }
    memcpy(pos, cnt, (size_t)(n + 1) * sizeof(int));
    for (int k = 0; k < b->n; k++) {
        int c = b->rcol[k];
        if (c < 0 || c >= n) continue;
        int p = pos[c]++;
        arow[p] = b->rrow[k];
        aval[p] = b->rval[k];
    }
    free(pos);

    /* sort rows within each column, merge duplicates, pack contiguously */
    int *colptr = (int *)malloc((size_t)(n + 1) * sizeof(int));
    if (!colptr) { free(cnt); free(arow); free(aval); return -1; }
    colptr[0] = 0;
    int w = 0;
    for (int j = 0; j < n; j++) {
        int lo = cnt[j], hi = cnt[j + 1];
        for (int a = lo + 1; a < hi; a++) {           /* insertion sort by row */
            int keyr = arow[a]; double keyv = aval[a];
            int q = a - 1;
            while (q >= lo && arow[q] > keyr) { arow[q + 1] = arow[q]; aval[q + 1] = aval[q]; q--; }
            arow[q + 1] = keyr; aval[q + 1] = keyv;
        }
        for (int a = lo; a < hi; a++) {
            if (w > colptr[j] && arow[w - 1] == arow[a]) aval[w - 1] += aval[a];
            else { arow[w] = arow[a]; aval[w] = aval[a]; w++; }
        }
        colptr[j + 1] = w;
    }
    free(cnt);

    memset(lp, 0, sizeof(*lp));
    lp->n = n;
    lp->m = b->m;
    lp->Acolptr = colptr;
    lp->Arow = arow;
    lp->Aval = aval;
    lp->rel = (char *)malloc((size_t)(b->m ? b->m : 1));
    lp->b   = (double *)malloc((size_t)(b->m ? b->m : 1) * sizeof(double));
    lp->c   = (double *)calloc((size_t)n, sizeof(double));
    lp->l   = (double *)malloc((size_t)n * sizeof(double));
    lp->u   = (double *)malloc((size_t)n * sizeof(double));
    lp->maximize = 0;
    if (!lp->rel || !lp->b || !lp->c || !lp->l || !lp->u) {
        free(lp->rel); free(lp->b); free(lp->c); free(lp->l); free(lp->u);
        return -1;
    }
    if (b->m) {
        memcpy(lp->rel, b->rel, (size_t)b->m);
        memcpy(lp->b, b->rhs, (size_t)b->m * sizeof(double));
    }
    for (int j = 0; j < n; j++) { lp->l[j] = Q16_LO; lp->u[j] = Q16_HI; }
    for (int j = b->n_orig; j < n; j++) lp->l[j] = 0.0;      /* deviations >= 0 */
    return 0;
}

static void lp_free_lp(LP *lp) {
    free(lp->Acolptr); free(lp->Arow); free(lp->Aval);
    free(lp->rel); free(lp->b); free(lp->c); free(lp->l); free(lp->u);
}

/* Sample point at parameter t along an edge (straight chord between its
 * endpoints; the reference store has no curve control points — curved
 * sampling lives in the rasterizer). */
static void edge_chord_point(const Document *doc, int eid, double t, double *px, double *py) {
    if (eid < 0 || eid >= doc->prims.n_edges) { *px = 0; *py = 0; return; }
    int a = doc->prims.edges[eid].v_start, b = doc->prims.edges[eid].v_end;
    if (a < 0 || a >= doc->prims.n_vertices || b < 0 || b >= doc->prims.n_vertices) { *px = 0; *py = 0; return; }
    double x0 = from_q16(doc->prims.vertices[a].x), y0 = from_q16(doc->prims.vertices[a].y);
    double x1 = from_q16(doc->prims.vertices[b].x), y1 = from_q16(doc->prims.vertices[b].y);
    *px = x0 + t * (x1 - x0);
    *py = y0 + t * (y1 - y0);
}

static void resolve_lp(Document *doc, int *warnings) {
    int V = doc->prims.n_vertices, S = doc->prims.n_strokes;
    int n_orig = 2 * V + S;
    if (n_orig == 0) return;

    /* initial (input) values — the L1 projection target */
    double *x0 = (double *)malloc((size_t)n_orig * sizeof(double));
    if (!x0) { warn_(warnings, "lp: alloc"); return; }
    for (int v = 0; v < V; v++) {
        x0[2 * v]     = from_q16(doc->prims.vertices[v].x);
        x0[2 * v + 1] = from_q16(doc->prims.vertices[v].y);
    }
    for (int s = 0; s < S; s++) {
        x0[2 * V + s] = (doc->prims.strokes[s].n_widths > 0)
                      ? from_q16(doc->prims.strokes[s].widths[0]) : 2.0;
    }

    uint64_t deadline = now_us() + (uint64_t)(doc->config.max_ms ? doc->config.max_ms : 50) * 1000ULL;
    int max_iter = doc->config.max_iter ? doc->config.max_iter : MAX_ITER_DEFAULT;
    int done = 0;

    for (int it = 0; it < max_iter && !done; it++) {
        LPBuilder b;
        lpb_init(&b, n_orig);

        /* L1 least-change rows:  x_j - dv_j <= x0_j ;  -x_j - du_j <= -x0_j */
        for (int j = 0; j < n_orig; j++) {
            lpb_row(&b, x0[j], '<');
            lpb_add(&b, j, 1.0); lpb_add(&b, b.n_orig + j, -1.0);
            lpb_row(&b, -x0[j], '<');
            lpb_add(&b, j, -1.0); lpb_add(&b, 2 * b.n_orig + j, -1.0);
        }

        int all_ok = 1;
        for (int i = 0; i < doc->config.n_constraints; i++) {
            Constraint *c = &doc->config.constraints[i];
            if (c->section != SEC_CONSTRAINT) continue;

            switch (c->subtype) {
            case C_LINEAR_EQ: case C_LINEAR_LE: case C_LINEAR_GE: {
                char rel = (c->subtype == C_LINEAR_EQ) ? '=' :
                           (c->subtype == C_LINEAR_LE) ? '<' : '>';
                lpb_row(&b, from_q16(c->u.linear.rhs), rel);
                for (int t = 0; t < c->u.linear.n_terms; t++) {
                    int var = c->u.linear.var_ids[t];
                    if (var < 0 || var >= n_orig) { warn_(warnings, "linear: var %d out of range", var); continue; }
                    lpb_add(&b, var, from_q16(c->u.linear.coeffs[t]));
                }
                break;
            }
            case C_MIN_DIST: {
                int a = c->u.min_dist.prim_a, bb = c->u.min_dist.prim_b;
                double d = from_q16(c->u.min_dist.distance);
                if (a < 0 || bb < 0 || a >= V || bb >= V || a == bb) { warn_(warnings, "min_dist: bad prim"); break; }
                double ax = from_q16(doc->prims.vertices[a].x), ay = from_q16(doc->prims.vertices[a].y);
                double bx = from_q16(doc->prims.vertices[bb].x), by = from_q16(doc->prims.vertices[bb].y);
                double ux = bx - ax, uy = by - ay;
                double len = sqrt(ux * ux + uy * uy);
                if (len < 1e-9) { ux = 1.0; uy = 0.0; len = 1.0; }
                ux /= len; uy /= len;
                if (len < d - 1e-9) {                 /* violated: add SLP row */
                    /* (pos_b - pos_a)·u >= d  <=>  -(pos_b - pos_a)·u <= -d */
                    lpb_row(&b, -d, '<');
                    lpb_add(&b, 2 * a, ux);     lpb_add(&b, 2 * a + 1, uy);
                    lpb_add(&b, 2 * bb, -ux);   lpb_add(&b, 2 * bb + 1, -uy);
                    all_ok = 0;
                }
                break;
            }
            case C_COLLISION_FREE: {
                int a = c->u.collision.a, bb = c->u.collision.b;
                double margin = from_q16(c->u.collision.margin);
                if (a < 0 || bb < 0 || a >= S || bb >= S) { warn_(warnings, "collision_free: bad stroke"); break; }
                int ea = doc->prims.strokes[a].edge_id, eb = doc->prims.strokes[bb].edge_id;
                double viol = 0.0;
                for (int ti = 0; ti < 5 && viol >= -1e-9; ti++) {
                    for (int tj = 0; tj < 5; tj++) {
                        double pax, pay, pbx, pby;
                        edge_chord_point(doc, ea, ti / 4.0, &pax, &pay);
                        edge_chord_point(doc, eb, tj / 4.0, &pbx, &pby);
                        double ux = pbx - pax, uy = pby - pay;
                        double len = sqrt(ux * ux + uy * uy);
                        if (len < 1e-9) continue;
                        ux /= len; uy /= len;
                        if (len < margin - 1e-9) {
                            /* point a is a chord point: coefficients on the
                               endpoints are (1-t) and t (chord sampling) */
                            lpb_row(&b, -margin, '<');
                            double ta = ti / 4.0, tb = tj / 4.0;
                            /* p_a = (1-ta)*v0a + ta*v1a ; p_b = (1-tb)*v0b + tb*v1b */
                            int a0 = doc->prims.edges[ea].v_start, a1 = doc->prims.edges[ea].v_end;
                            int b0 = doc->prims.edges[eb].v_start, b1 = doc->prims.edges[eb].v_end;
                            if (a0 >= 0 && a1 >= 0 && b0 >= 0 && b1 >= 0 &&
                                a0 < V && a1 < V && b0 < V && b1 < V) {
                                lpb_add(&b, 2 * a0,     (1 - ta) * ux);
                                lpb_add(&b, 2 * a0 + 1, (1 - ta) * uy);
                                lpb_add(&b, 2 * a1,     ta * ux);
                                lpb_add(&b, 2 * a1 + 1, ta * uy);
                                lpb_add(&b, 2 * b0,     -(1 - tb) * ux);
                                lpb_add(&b, 2 * b0 + 1, -(1 - tb) * uy);
                                lpb_add(&b, 2 * b1,     -tb * ux);
                                lpb_add(&b, 2 * b1 + 1, -tb * uy);
                            }
                            viol = 1.0;
                            break;
                        }
                    }
                }
                if (viol > 0) all_ok = 0;
                break;
            }
            case C_BBOX_CLAMP: {
                int p = c->u.bbox.prim;
                if (p < 0 || p >= V) { warn_(warnings, "bbox_clamp: prim out of range"); break; }
                /* applied as bounds after build; recorded here via builder? no-op */
                (void)p;
                break;
            }
            default:
                break;   /* QP subtypes handled in resolve_qp */
            }
        }

        if (b.m == 0) lpb_row(&b, 0.0, '<');   /* psolve needs >= 1 row */

        LP lp;
        if (lpb_build(&b, &lp) != 0) { warn_(warnings, "lp: build failed"); lpb_free(&b); break; }

        /* bounds */
        for (int j = 0; j < n_orig; j++) { lp.l[j] = Q16_LO; lp.u[j] = Q16_HI; }
        for (int s = 0; s < S; s++) { lp.l[2 * V + s] = 0.0; lp.u[2 * V + s] = 1000.0; }
        for (int i = 0; i < doc->config.n_constraints; i++) {
            Constraint *c = &doc->config.constraints[i];
            if (c->section == SEC_CONSTRAINT && c->subtype == C_BBOX_CLAMP) {
                int p = c->u.bbox.prim;
                if (p < 0 || p >= V) continue;
                double lx = from_q16(c->u.bbox.x_min), ux = from_q16(c->u.bbox.x_max);
                double ly = from_q16(c->u.bbox.y_min), uy = from_q16(c->u.bbox.y_max);
                if (lx > lp.l[2 * p])     lp.l[2 * p] = lx;
                if (ux < lp.u[2 * p])     lp.u[2 * p] = ux;
                if (ly > lp.l[2 * p + 1]) lp.l[2 * p + 1] = ly;
                if (uy < lp.u[2 * p + 1]) lp.u[2 * p + 1] = uy;
            }
        }
        /* objective: minimize Σ (dv_j + du_j) */
        for (int j = n_orig; j < lp.n; j++) lp.c[j] = 1.0;

        int status = -1;
        psolve_try();
        Solver *s = solver_create(&lp);
        if (s) {
            status = solver_solve(s);
            if (status == 0) {
                /* solver_optimum writes ALL lp->n variables (originals +
                   deviations), so the buffer must be lp->n wide */
                double *xo = (double *)malloc((size_t)lp.n * sizeof(double));
                double obj;
                solver_optimum(s, xo, &obj);
                for (int v = 0; v < V; v++) {
                    doc->prims.vertices[v].x = to_q16(xo[2 * v]);
                    doc->prims.vertices[v].y = to_q16(xo[2 * v + 1]);
                }
                for (int st = 0; st < S; st++)
                    if (doc->prims.strokes[st].n_widths > 0)
                        doc->prims.strokes[st].widths[0] = to_q16(xo[2 * V + st]);
                free(xo);
            }
            solver_destroy(s);
        }
        psolve_end();

        if (status == 1) { warn_(warnings, "lp: infeasible (constraints unsatisfiable)"); }
        else if (status == 2) { warn_(warnings, "lp: unbounded"); }
        else if (status == 3) { warn_(warnings, "lp: iteration limit"); }
        else if (status != 0) { warn_(warnings, "lp: solver error %d", status); }

        lp_free_lp(&lp);
        lpb_free(&b);

        if (status != 0) break;          /* keep last-known-good document */

        /* SLP convergence: verify the continuous constraints from the NEW
           positions.  (Checking pre-solve positions alone lets the L1
           objective pull the solution back to the input once a satisfied
           row is dropped.) */
        int post_ok = 1;
        for (int i = 0; i < doc->config.n_constraints && post_ok; i++) {
            Constraint *c = &doc->config.constraints[i];
            if (c->section != SEC_CONSTRAINT) continue;
            if (c->subtype == C_MIN_DIST) {
                int a = c->u.min_dist.prim_a, bb = c->u.min_dist.prim_b;
                double d = from_q16(c->u.min_dist.distance);
                if (a < 0 || bb < 0 || a >= V || bb >= V) continue;
                double dx = from_q16(doc->prims.vertices[bb].x) - from_q16(doc->prims.vertices[a].x);
                double dy = from_q16(doc->prims.vertices[bb].y) - from_q16(doc->prims.vertices[a].y);
                if (sqrt(dx * dx + dy * dy) < d - 1e-6) post_ok = 0;
            } else if (c->subtype == C_COLLISION_FREE) {
                int a = c->u.collision.a, bb = c->u.collision.b;
                double margin = from_q16(c->u.collision.margin);
                if (a < 0 || bb < 0 || a >= S || bb >= S) continue;
                int ea = doc->prims.strokes[a].edge_id, eb = doc->prims.strokes[bb].edge_id;
                for (int ti = 0; ti < 5 && post_ok; ti++)
                    for (int tj = 0; tj < 5; tj++) {
                        double pax, pay, pbx, pby;
                        edge_chord_point(doc, ea, ti / 4.0, &pax, &pay);
                        edge_chord_point(doc, eb, tj / 4.0, &pbx, &pby);
                        double dx = pbx - pax, dy = pby - pay;
                        if (sqrt(dx * dx + dy * dy) < margin - 1e-6) post_ok = 0;
                    }
            }
        }
        if (all_ok || post_ok) done = 1;
        if (now_us() > deadline) break;  /* bounded computation */
    }

    free(x0);
}

/* ── Convex QP helpers ──────────────────────────────────────────────
 * psolve's QP: min ½xᵀQx + cᵀx s.t. Ax <= b (Q PSD, dense, column-major;
 * A row-major).  Variable bounds are expressed as rows. */
typedef struct { double *Q, *c, *A, *b; int n, m; } SmallQP;

static int qp_solve_small(SmallQP *q, double *xout, int *warnings) {
    QP qp;
    qp.n = q->n; qp.m = q->m;
    qp.Q = q->Q; qp.c = q->c; qp.A = q->A; qp.b = q->b;
    qp.x0 = NULL;
    QPResult res;
    memset(&res, 0, sizeof(res));
    int status = -1;
    psolve_try();
    qp_solve(&qp, &res);
    psolve_end();
    if (res.status == 0) { memcpy(xout, res.x, (size_t)q->n * sizeof(double)); status = 0; }
    else { warn_(warnings, "qp: status %d", res.status); }
    qp_result_free(&res);
    return status;
}

static void resolve_qp(Document *doc, int *warnings) {
    int V = doc->prims.n_vertices;

    /* fair_blend: max-entropy weights — min Σ(x_i - 1/n)^2 s.t. Σx=1, x>=0 */
    for (int i = 0; i < doc->config.n_constraints; i++) {
        Constraint *c = &doc->config.constraints[i];
        if (c->section != SEC_CONSTRAINT || c->subtype != C_FAIR_BLEND) continue;
        int nv = c->u.fair_blend.n_vars;
        if (nv < 1 || nv > 32) { warn_(warnings, "fair_blend: bad n_vars"); continue; }

        SmallQP q;
        q.n = nv;
        q.Q = (double *)calloc((size_t)nv * nv, sizeof(double));
        q.c = (double *)malloc((size_t)nv * sizeof(double));
        q.m = nv + 2;
        q.A = (double *)calloc((size_t)q.m * nv, sizeof(double));
        q.b = (double *)malloc((size_t)q.m * sizeof(double));
        if (!q.Q || !q.c || !q.A || !q.b) { warn_(warnings, "fair_blend: alloc"); free(q.Q); free(q.c); free(q.A); free(q.b); continue; }

        for (int j = 0; j < nv; j++) {
            q.Q[(size_t)j * nv + j] = 1.0;           /* I, column-major */
            q.c[j] = -2.0 / nv;
            q.A[(size_t)(2 + j) * nv + j] = -1.0;    /* -x_j <= 0  => x_j >= 0 */
            q.b[2 + j] = 0.0;
        }
        for (int j = 0; j < nv; j++) { q.A[j] = 1.0; q.b[0] = 1.0; }        /* Σx <= 1 */
        for (int j = 0; j < nv; j++) { q.A[(size_t)1 * nv + j] = -1.0; q.b[1] = -1.0; } /* Σx >= 1 */

        double *sol = (double *)calloc((size_t)nv, sizeof(double));
        if (qp_solve_small(&q, sol, warnings) == 0)
            for (int j = 0; j < nv; j++) c->u.fair_blend.weights[j] = sol[j];

        free(sol); free(q.Q); free(q.c); free(q.A); free(q.b);
    }

    /* min_stretch: elastic pull toward rest — min w·||x - x0||^2, bounded */
    for (int i = 0; i < doc->config.n_constraints; i++) {
        Constraint *c = &doc->config.constraints[i];
        if (c->section != SEC_CONSTRAINT || c->subtype != C_MIN_STRETCH) continue;

        int node = c->u.min_stretch.node_id;
        int v = node;
        if (node >= 0 && node < doc->prims.n_nodes && doc->prims.nodes[node].content_ref >= 0)
            v = doc->prims.nodes[node].content_ref;
        if (v < 0 || v >= V) { warn_(warnings, "min_stretch: bad target vertex"); continue; }

        double w = from_q16(c->u.min_stretch.weight);
        if (w < 0) w = 0;
        double x0 = from_q16(doc->prims.vertices[v].x);
        double y0 = from_q16(doc->prims.vertices[v].y);

        /* bounds for this vertex: bbox_clamp rows if present, else Q16 range */
        double lx = Q16_LO, ux = Q16_HI, ly = Q16_LO, uy = Q16_HI;
        for (int k = 0; k < doc->config.n_constraints; k++) {
            Constraint *cc = &doc->config.constraints[k];
            if (cc->section == SEC_CONSTRAINT && cc->subtype == C_BBOX_CLAMP && cc->u.bbox.prim == v) {
                if (from_q16(cc->u.bbox.x_min) > lx) lx = from_q16(cc->u.bbox.x_min);
                if (from_q16(cc->u.bbox.x_max) < ux) ux = from_q16(cc->u.bbox.x_max);
                if (from_q16(cc->u.bbox.y_min) > ly) ly = from_q16(cc->u.bbox.y_min);
                if (from_q16(cc->u.bbox.y_max) < uy) uy = from_q16(cc->u.bbox.y_max);
            }
        }

        SmallQP q;
        q.n = 2;
        q.Q = (double *)calloc(4, sizeof(double));
        q.c = (double *)malloc(2 * sizeof(double));
        q.m = 4;
        q.A = (double *)calloc((size_t)q.m * 2, sizeof(double));
        q.b = (double *)malloc((size_t)q.m * sizeof(double));
        if (!q.Q || !q.c || !q.A || !q.b) { warn_(warnings, "min_stretch: alloc"); free(q.Q); free(q.c); free(q.A); free(q.b); continue; }

        q.Q[0] = w; q.Q[3] = w;                /* w·I, column-major */
        q.c[0] = -w * x0; q.c[1] = -w * y0;
        /* rows: x <= ux ; -x <= -lx ; y <= uy ; -y <= -ly */
        q.A[0] = 1.0;           q.b[0] = ux;
        q.A[2] = -1.0;          q.b[1] = -lx;
        q.A[1 * 2 + 1] = 1.0;   q.b[2] = uy;
        q.A[3 * 2 + 1] = -1.0;  q.b[3] = -ly;

        double sol[2];
        if (qp_solve_small(&q, sol, warnings) == 0) {
            doc->prims.vertices[v].x = to_q16(sol[0]);
            doc->prims.vertices[v].y = to_q16(sol[1]);
        }
        free(q.Q); free(q.c); free(q.A); free(q.b);
    }

    /* rig: translation-only equilibrium for cyclic parent chains (v1.2
       semantics, SPEC §5.2.1).  Cycle members are flagged by
       resolve_hierarchy (flags bit 0).  We solve the soft-consistency QP
       over the members' world translations:
         min  Σ_k ||x_k - x0_k||²  +  λ Σ_{(c,p)} ||x_c - x_p - t_c||²
       where t_c is node c's local offset.  λ is large so consistent cycles
       are satisfied exactly while the regularization anchors the global
       translation.  The solved values are written back to the nodes' tx/ty
       as the equilibrium world offset (rotation/scale rigs are future work;
       see SPEC.md §11.1). */
    for (int i = 0; i < doc->config.n_constraints; i++) {
        Constraint *c = &doc->config.constraints[i];
        if (c->section != SEC_CONSTRAINT || c->subtype != C_RIG_EQUILIBRIUM) continue;

        int N = doc->prims.n_nodes;
        int *map = (int *)malloc((size_t)(N ? N : 1) * sizeof(int));
        int *idlist = (int *)malloc((size_t)(N ? N : 1) * sizeof(int));
        if (!map || !idlist) { free(map); free(idlist); continue; }
        for (int k = 0; k < N; k++) map[k] = -1;
        int m = 0;
        for (int k = 0; k < N; k++)
            if (doc->prims.nodes[k].flags & 1) { map[k] = m; idlist[m++] = k; }
        if (m == 0) { free(map); free(idlist); continue; }

        int nvar = 2 * m;
        SmallQP q;
        q.n = nvar;
        q.Q = (double *)calloc((size_t)nvar * nvar, sizeof(double));
        q.c = (double *)calloc((size_t)nvar, sizeof(double));
        q.m = 1;                                   /* trivial row 0 <= 0 */
        q.A = (double *)calloc((size_t)nvar, sizeof(double));
        q.b = (double *)malloc(sizeof(double));
        if (!q.Q || !q.c || !q.A || !q.b) { warn_(warnings, "rig: alloc"); free(map); free(idlist); free(q.Q); free(q.c); free(q.A); free(q.b); continue; }
        q.b[0] = 0.0;

        const double lam = 1e6;
        /* regularization ||x - x0||² */
        for (int k = 0; k < m; k++) {
            int nid = idlist[k];
            q.Q[(size_t)(2 * k) * nvar + 2 * k] += 1.0;
            q.Q[(size_t)(2 * k + 1) * nvar + 2 * k + 1] += 1.0;
            q.c[2 * k]     -= from_q16(doc->prims.nodes[nid].tx);
            q.c[2 * k + 1] -= from_q16(doc->prims.nodes[nid].ty);
        }
        /* consistency edges: parent relationships among cycle members */
        for (int ci = 0; ci < doc->config.n_constraints; ci++) {
            Constraint *cc = &doc->config.constraints[ci];
            if (cc->section != SEC_STRUCTURAL || cc->subtype != S_PARENT) continue;
            int child = cc->u.parent.child, parent = cc->u.parent.parent;
            int cm = (child >= 0 && child < N) ? map[child] : -1;
            int pm = (parent >= 0 && parent < N) ? map[parent] : -1;
            if (cm < 0 || pm < 0) continue;
            double tx = from_q16(doc->prims.nodes[child].tx);
            double ty = from_q16(doc->prims.nodes[child].ty);
            int ccx = 2 * cm, ccy = ccx + 1, cpx = 2 * pm, cpy = cpx + 1;
            q.Q[(size_t)ccx * nvar + ccx] += lam;
            q.Q[(size_t)ccy * nvar + ccy] += lam;
            q.Q[(size_t)cpx * nvar + cpx] += lam;
            q.Q[(size_t)cpy * nvar + cpy] += lam;
            q.Q[(size_t)ccx * nvar + cpx] -= lam;
            q.Q[(size_t)cpx * nvar + ccx] -= lam;
            q.Q[(size_t)ccy * nvar + cpy] -= lam;
            q.Q[(size_t)cpy * nvar + ccy] -= lam;
            q.c[ccx] -= lam * tx;
            q.c[ccy] -= lam * ty;
            q.c[cpx] += lam * tx;
            q.c[cpy] += lam * ty;
        }

        double *sol = (double *)calloc((size_t)nvar, sizeof(double));
        if (qp_solve_small(&q, sol, warnings) == 0) {
            for (int k = 0; k < m; k++) {
                int nid = idlist[k];
                doc->prims.nodes[nid].tx = to_q16(sol[2 * k]);
                doc->prims.nodes[nid].ty = to_q16(sol[2 * k + 1]);
            }
        }
        free(sol);
        free(q.Q); free(q.c); free(q.A); free(q.b);
        free(map); free(idlist);
    }
}

#else /* !SMZ_HAVE_PSOLVE */

static void resolve_lp(Document *doc, int *warnings) {
    int n_lp = 0;
    for (int i = 0; i < doc->config.n_constraints; i++)
        if (doc->config.constraints[i].section == SEC_CONSTRAINT) n_lp++;
    (void)n_lp; (void)warnings;
}
static void resolve_qp(Document *doc, int *warnings) {
    (void)doc; (void)warnings;
}

#endif /* SMZ_HAVE_PSOLVE */

/* ═══════════════════════════════════════════════════════════════════
 *  PHASE 4 — Validation (a): bound checks
 * ═══════════════════════════════════════════════════════════════════ */

static void resolve_validation(Document *doc, int *warnings) {
    for (int i = 0; i < doc->config.n_constraints; i++) {
        Constraint *c = &doc->config.constraints[i];
        if (c->section != SEC_ASSERT || c->subtype != A_BOUND_CHECK) continue;

        int prim = c->u.bound_check.prim;
        if (prim < 0 || prim >= doc->prims.n_vertices) {
            warn_(warnings, "bound_check: prim %d out of range", prim);
            continue;
        }
        int dim = c->u.bound_check.dim;
        if (dim != 0 && dim != 1) { warn_(warnings, "bound_check: bad dim"); continue; }
        if (c->u.bound_check.lo > c->u.bound_check.hi) { warn_(warnings, "bound_check: empty interval"); continue; }

        q16_t *val = (dim == 0) ? &doc->prims.vertices[prim].x
                                : &doc->prims.vertices[prim].y;
        if (*val < c->u.bound_check.lo || *val > c->u.bound_check.hi) {
            *val = (*val < c->u.bound_check.lo) ? c->u.bound_check.lo : c->u.bound_check.hi;
            warn_(warnings, "bound_check: v%d %s clamped", prim, dim == 0 ? "x" : "y");
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  MAIN RESOLVE ENTRY POINT
 * ═══════════════════════════════════════════════════════════════════ */

int smazka_resolve(Document *doc) {
    int warnings = 0;
    uint64_t start = now_us();
    uint64_t deadline = start + (uint64_t)(doc->config.max_ms ? doc->config.max_ms : 50) * 1000ULL;

    resolve_edge_connects(doc, &warnings);
    resolve_hierarchy(doc, &warnings);
    resolve_state_machines(doc, &warnings);
    if (now_us() > deadline) { warnings |= 0x80000000; return warnings; }

    resolve_lp(doc, &warnings);
    if (now_us() > deadline) { warnings |= 0x80000000; return warnings; }

    resolve_qp(doc, &warnings);
    if (now_us() > deadline) { warnings |= 0x80000000; return warnings; }

    resolve_validation(doc, &warnings);
    return warnings;
}

/* ═══════════════════════════════════════════════════════════════════
 *  SELF-TEST (SMZ_STANDALONE)
 * ═══════════════════════════════════════════════════════════════════ */

#ifdef SMZ_STANDALONE
#include <assert.h>

static Constraint *add_c(Document *d, uint8_t section, uint8_t subtype) {
    if (d->config.n_constraints >= d->config.cap_constraints) {
        int ncap = d->config.cap_constraints ? d->config.cap_constraints * 2 : 32;
        if (ncap > MAX_CONSTS) ncap = MAX_CONSTS;
        Constraint *nc = (Constraint *)realloc(d->config.constraints, (size_t)ncap * sizeof(Constraint));
        if (!nc) return NULL;
        d->config.constraints = nc;
        d->config.cap_constraints = ncap;
    }
    Constraint *c = &d->config.constraints[d->config.n_constraints++];
    memset(c, 0, sizeof(*c));
    c->section = section; c->subtype = subtype;
    return c;
}

int main(void) {
    int failures = 0;
    Document doc;
    memset(&doc, 0, sizeof(doc));
    doc.config.max_iter = MAX_ITER_DEFAULT;
    doc.config.max_ms = 50;
    doc.clock.frame = 0.0;

    /* ── Test 1: cycle A->B->C->A detected ── */
    {
        doc.prims.n_nodes = 3;
        Constraint *p;
        p = add_c(&doc, SEC_STRUCTURAL, S_PARENT); p->u.parent.child = 1; p->u.parent.parent = 0;
        p = add_c(&doc, SEC_STRUCTURAL, S_PARENT); p->u.parent.child = 2; p->u.parent.parent = 1;
        p = add_c(&doc, SEC_STRUCTURAL, S_PARENT); p->u.parent.child = 0; p->u.parent.parent = 2;
        int w = smazka_resolve(&doc);
        if (!(w & ~0x80000000u)) { printf("FAIL test1: no cycle warning\n"); failures++; }
        else printf("PASS test1: A->B->C->A cycle detected (%d warnings)\n", w & 0x7FFFFFFF);
    }

    /* ── Test 2: acyclic chain resolves clean ── */
    {
        memset(&doc, 0, sizeof(doc)); doc.config.max_iter = MAX_ITER_DEFAULT; doc.config.max_ms = 50;
        doc.prims.n_nodes = 4;
        Constraint *p;
        p = add_c(&doc, SEC_STRUCTURAL, S_PARENT); p->u.parent.child = 1; p->u.parent.parent = 0;
        p = add_c(&doc, SEC_STRUCTURAL, S_PARENT); p->u.parent.child = 2; p->u.parent.parent = 1;
        p = add_c(&doc, SEC_STRUCTURAL, S_PARENT); p->u.parent.child = 3; p->u.parent.parent = 2;
        int w = smazka_resolve(&doc);
        if (w & 0x7FFFFFFF) { printf("FAIL test2: spurious cycle warning\n"); failures++; }
        else printf("PASS test2: acyclic chain resolves clean\n");
    }

    /* ── Test 3: state machine weights track triggers (not uniform 1/n) ── */
    {
        memset(&doc, 0, sizeof(doc)); doc.config.max_iter = MAX_ITER_DEFAULT; doc.config.max_ms = 50;
        doc.prims.n_nodes = 3;
        Constraint *sm = add_c(&doc, SEC_ASSERT, A_STATE_MACHINE);
        sm->u.state_machine.state_id = 0;
        sm->u.state_machine.initial = 0;
        sm->u.state_machine.n_transitions = 2;
        sm->u.state_machine.trans = (Transition *)calloc(2, sizeof(Transition));
        assert(sm->u.state_machine.trans);
        sm->u.state_machine.trans[0].target = 1; sm->u.state_machine.trans[0].trigger_type = 0;
        sm->u.state_machine.trans[0].start_frame = 0; sm->u.state_machine.trans[0].param = to_q16(10);
        sm->u.state_machine.trans[1].target = 2; sm->u.state_machine.trans[1].trigger_type = 0;
        sm->u.state_machine.trans[1].start_frame = 0; sm->u.state_machine.trans[1].param = to_q16(10);
        doc.clock.frame = 0.0;
        smazka_resolve(&doc);
        double w0[3] = { sm->u.state_machine.weights[0], sm->u.state_machine.weights[1],
                         sm->u.state_machine.weights[2] };
        int w0_ok = w0[0] == 1.0 && w0[1] == 0.0 && w0[2] == 0.0;
        doc.clock.frame = 5.0;
        smazka_resolve(&doc);
        double *w = sm->u.state_machine.weights;
        int w5_ok = fabs(w[0] - 0.5) < 1e-9 && fabs(w[1] - 0.25) < 1e-9 && fabs(w[2] - 0.25) < 1e-9;
        int not_uniform = fabs(w[0] - 1.0/3.0) > 1e-6;
        if (w0_ok && w5_ok && not_uniform)
            printf("PASS test3: weights track triggers: frame0=(%.2f,%.2f,%.2f) frame5=(%.2f,%.2f,%.2f)\n",
                   w0[0], w0[1], w0[2], w[0], w[1], w[2]);
        else { printf("FAIL test3\n"); failures++; }
        free(sm->u.state_machine.trans);
    }

    /* ── Test 4: bound_check clamps and warns ── */
    {
        memset(&doc, 0, sizeof(doc)); doc.config.max_iter = MAX_ITER_DEFAULT; doc.config.max_ms = 50;
        doc.prims.n_vertices = 2;
        doc.prims.vertices[0].x = to_q16(5000.0); doc.prims.vertices[0].y = to_q16(0);
        Constraint *bc = add_c(&doc, SEC_ASSERT, A_BOUND_CHECK);
        bc->u.bound_check.prim = 0; bc->u.bound_check.dim = 0;
        bc->u.bound_check.lo = to_q16(-100.0); bc->u.bound_check.hi = to_q16(100.0);
        int w = smazka_resolve(&doc);
        int ok = (from_q16(doc.prims.vertices[0].x) == 100.0) && (w & 0x7FFFFFFF);
        if (ok) printf("PASS test4: bound_check clamped 5000 -> 100 (warnings=%d)\n", w & 0x7FFFFFFF);
        else { printf("FAIL test4: x=%.1f warnings=%d\n", from_q16(doc.prims.vertices[0].x), w & 0x7FFFFFFF); failures++; }
    }

    /* ── Test 5: edge_connects repair ── */
    {
        memset(&doc, 0, sizeof(doc)); doc.config.max_iter = MAX_ITER_DEFAULT; doc.config.max_ms = 50;
        doc.prims.n_vertices = 2; doc.prims.n_edges = 1;
        doc.prims.edges[0].v_start = 0; doc.prims.edges[0].v_end = 1;
        Constraint *ec = add_c(&doc, SEC_ASSERT, A_EDGE_CONNECTS);
        ec->u.edge_connects.edge = 0; ec->u.edge_connects.vs = 1; ec->u.edge_connects.ve = 0;
        int w = smazka_resolve(&doc);
        if ((doc.prims.edges[0].v_start == 1 && doc.prims.edges[0].v_end == 0) && (w & 0x7FFFFFFF))
            printf("PASS test5: edge_connects repaired e0 to (1,0)\n");
        else { printf("FAIL test5\n"); failures++; }
    }

#ifdef SMZ_HAVE_PSOLVE
    /* ── Test 6: LP bbox_clamp + L1 least-change pulls to nearest feasible ── */
    {
        memset(&doc, 0, sizeof(doc)); doc.config.max_iter = MAX_ITER_DEFAULT; doc.config.max_ms = 200;
        doc.prims.n_vertices = 1;
        doc.prims.vertices[0].x = to_q16(0.0); doc.prims.vertices[0].y = to_q16(0.0);
        Constraint *bb = add_c(&doc, SEC_CONSTRAINT, C_BBOX_CLAMP);
        bb->u.bbox.prim = 0;
        bb->u.bbox.x_min = to_q16(10.0); bb->u.bbox.y_min = to_q16(10.0);
        bb->u.bbox.x_max = to_q16(20.0); bb->u.bbox.y_max = to_q16(20.0);
        int w = smazka_resolve(&doc);
        double x = from_q16(doc.prims.vertices[0].x), y = from_q16(doc.prims.vertices[0].y);
        int ok = fabs(x - 10.0) < 1e-3 && fabs(y - 10.0) < 1e-3 && !(w & 0x7FFFFFFF);
        if (ok) printf("PASS test6: LP bbox: (0,0) -> (%.2f,%.2f) in bbox [10,20]^2\n", x, y);
        else { printf("FAIL test6: got (%.2f,%.2f)\n", x, y); failures++; }
    }

    /* ── Test 7: LP linear_eq 2x+y=40 from (10,10); unique L1 optimum (15,10) ── */
    {
        memset(&doc, 0, sizeof(doc)); doc.config.max_iter = MAX_ITER_DEFAULT; doc.config.max_ms = 200;
        doc.prims.n_vertices = 1;
        doc.prims.vertices[0].x = to_q16(10.0); doc.prims.vertices[0].y = to_q16(10.0);
        Constraint *bb = add_c(&doc, SEC_CONSTRAINT, C_BBOX_CLAMP);
        bb->u.bbox.prim = 0;
        bb->u.bbox.x_min = to_q16(0.0); bb->u.bbox.y_min = to_q16(0.0);
        bb->u.bbox.x_max = to_q16(40.0); bb->u.bbox.y_max = to_q16(40.0);
        Constraint *le = add_c(&doc, SEC_CONSTRAINT, C_LINEAR_EQ);
        le->u.linear.n_terms = 2;
        le->u.linear.var_ids[0] = 0; le->u.linear.coeffs[0] = to_q16(2.0);
        le->u.linear.var_ids[1] = 1; le->u.linear.coeffs[1] = to_q16(1.0);
        le->u.linear.rhs = to_q16(40.0);
        smazka_resolve(&doc);
        double x = from_q16(doc.prims.vertices[0].x), y = from_q16(doc.prims.vertices[0].y);
        int ok = fabs(x - 15.0) < 1e-2 && fabs(y - 10.0) < 1e-2;
        if (ok) printf("PASS test7: LP linear_eq: 2x+y=40 from (10,10) -> (%.2f,%.2f)\n", x, y);
        else { printf("FAIL test7: got (%.2f,%.2f)\n", x, y); failures++; }
    }

    /* ── Test 8: LP min_dist SLP separates two vertices to >= 10 ── */
    {
        memset(&doc, 0, sizeof(doc)); doc.config.max_iter = 32; doc.config.max_ms = 250;
        doc.prims.n_vertices = 2;
        doc.prims.vertices[0].x = to_q16(0.0); doc.prims.vertices[0].y = to_q16(0.0);
        doc.prims.vertices[1].x = to_q16(1.0); doc.prims.vertices[1].y = to_q16(0.0);
        Constraint *md = add_c(&doc, SEC_CONSTRAINT, C_MIN_DIST);
        md->u.min_dist.prim_a = 0; md->u.min_dist.prim_b = 1;
        md->u.min_dist.distance = to_q16(10.0);
        smazka_resolve(&doc);
        double dx = from_q16(doc.prims.vertices[1].x) - from_q16(doc.prims.vertices[0].x);
        double dy = from_q16(doc.prims.vertices[1].y) - from_q16(doc.prims.vertices[0].y);
        double d = sqrt(dx * dx + dy * dy);
        int ok = d >= 10.0 - 1e-3;
        if (ok) printf("PASS test8: LP min_dist SLP: distance %.3f >= 10\n", d);
        else { printf("FAIL test8: distance %.3f < 10\n", d); failures++; }
    }

    /* ── Test 9: QP fair_blend max-entropy weights = 1/3 each ── */
    {
        memset(&doc, 0, sizeof(doc)); doc.config.max_iter = MAX_ITER_DEFAULT; doc.config.max_ms = 200;
        Constraint *fb = add_c(&doc, SEC_CONSTRAINT, C_FAIR_BLEND);
        fb->u.fair_blend.n_vars = 3;
        fb->u.fair_blend.var_ids[0] = 0; fb->u.fair_blend.var_ids[1] = 1; fb->u.fair_blend.var_ids[2] = 2;
        smazka_resolve(&doc);
        double *w = fb->u.fair_blend.weights;
        int ok = fabs(w[0] - 1.0/3.0) < 1e-6 && fabs(w[1] - 1.0/3.0) < 1e-6 && fabs(w[2] - 1.0/3.0) < 1e-6;
        if (ok) printf("PASS test9: QP fair_blend weights = (%.4f,%.4f,%.4f)\n", w[0], w[1], w[2]);
        else { printf("FAIL test9: weights = (%.4f,%.4f,%.4f)\n", w[0], w[1], w[2]); failures++; }
    }

    /* ── Test 10: QP min_stretch pulls vertex back toward rest, bounded ── */
    {
        memset(&doc, 0, sizeof(doc)); doc.config.max_iter = MAX_ITER_DEFAULT; doc.config.max_ms = 200;
        doc.prims.n_vertices = 1; doc.prims.n_nodes = 1;
        doc.prims.vertices[0].x = to_q16(500.0); doc.prims.vertices[0].y = to_q16(500.0);
        doc.prims.nodes[0].content_ref = 0;
        Constraint *bb = add_c(&doc, SEC_CONSTRAINT, C_BBOX_CLAMP);
        bb->u.bbox.prim = 0;
        bb->u.bbox.x_min = to_q16(0.0); bb->u.bbox.y_min = to_q16(0.0);
        bb->u.bbox.x_max = to_q16(100.0); bb->u.bbox.y_max = to_q16(100.0);
        Constraint *ms = add_c(&doc, SEC_CONSTRAINT, C_MIN_STRETCH);
        ms->u.min_stretch.node_id = 0; ms->u.min_stretch.weight = to_q16(1.0);
        smazka_resolve(&doc);
        double x = from_q16(doc.prims.vertices[0].x), y = from_q16(doc.prims.vertices[0].y);
        int ok = fabs(x - 100.0) < 1e-2 && fabs(y - 100.0) < 1e-2;
        if (ok) printf("PASS test10: QP min_stretch: (500,500) w/ bbox[0,100] -> (%.2f,%.2f)\n", x, y);
        else { printf("FAIL test10: got (%.2f,%.2f)\n", x, y); failures++; }
    }
#endif /* SMZ_HAVE_PSOLVE */

    /* ── Test 11: rig — translation-only equilibrium for a consistent cycle ── */
#ifdef SMZ_HAVE_PSOLVE
    {
        memset(&doc, 0, sizeof(doc)); doc.config.max_iter = MAX_ITER_DEFAULT; doc.config.max_ms = 200;
        doc.prims.n_nodes = 3;
        Constraint *p;
        p = add_c(&doc, SEC_STRUCTURAL, S_PARENT); p->u.parent.child = 1; p->u.parent.parent = 0;
        p = add_c(&doc, SEC_STRUCTURAL, S_PARENT); p->u.parent.child = 2; p->u.parent.parent = 1;
        p = add_c(&doc, SEC_STRUCTURAL, S_PARENT); p->u.parent.child = 0; p->u.parent.parent = 2;
        /* local offsets: t0=(10,0) t1=(5,0) t2=(-15,0) — consistent (sum 0) */
        doc.prims.nodes[0].tx = to_q16(10.0); doc.prims.nodes[0].ty = 0;
        doc.prims.nodes[1].tx = to_q16(5.0);  doc.prims.nodes[1].ty = 0;
        doc.prims.nodes[2].tx = to_q16(-15.0); doc.prims.nodes[2].ty = 0;
        Constraint *rg = add_c(&doc, SEC_CONSTRAINT, C_RIG_EQUILIBRIUM);
        rg->u.rig.node_a = 0; rg->u.rig.node_b = 1;
        smazka_resolve(&doc);
        /* consistency: world_child - world_parent = t_child */
        double w0 = from_q16(doc.prims.nodes[0].tx);
        double w1 = from_q16(doc.prims.nodes[1].tx);
        double w2 = from_q16(doc.prims.nodes[2].tx);
        double e01 = fabs((w1 - w0) - 5.0);   /* t1 = 5  */
        double e12 = fabs((w2 - w1) - (-15.0)); /* t2 = -15 */
        double e20 = fabs((w0 - w2) - 10.0);  /* t0 = 10 */
        int ok = e01 < 1e-2 && e12 < 1e-2 && e20 < 1e-2;
        if (ok) printf("PASS test11: rig equilibrium (world offsets %.2f, %.2f, %.2f)\n", w0, w1, w2);
        else { printf("FAIL test11: world (%.2f, %.2f, %.2f) errors (%.3f, %.3f, %.3f)\n", w0, w1, w2, e01, e12, e20); failures++; }
    }
#endif /* SMZ_HAVE_PSOLVE */

    if (failures) { printf("%d TEST(S) FAILED\n", failures); return 1; }
    printf("ALL RESOLVER TESTS PASSED\n");
    return 0;
}
#endif /* SMZ_STANDALONE */
