/*
 * SmazkaVG solve tool (tools/smazka-solve)
 * ========================================
 *
 * Parses a Line-ASM document into the resolver's in-memory model, runs the
 * constraint solver (smazka_resolve: structural / assertions / automata /
 * LP / convex QP via the psolve backend), and writes the resolved document
 * back as Line-ASM — vertices and node translations updated, everything else
 * preserved verbatim.
 *
 *   make -C third_party/psolve lib          # once
 *   make solve                              # from the repo root
 *   ./build/smazka-solve in.smazka [out.smazka]
 *
 * Example (solve_demo.smazka): two vertices 1 unit apart with a min_dist 50
 * constraint are pushed apart by the LP solver; bbox_clamp and linear_eq
 * constraints move geometry to the nearest feasible configuration (L1
 * least-change objective).
 *
 * The tool intentionally reuses src/resolver.c (SMZ_HAVE_PSOLVE, no
 * standalone main) so the resolver and its tests stay the single source of
 * truth for the solver semantics.
 */

#define SMZ_HAVE_PSOLVE
#include "../src/resolver.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* local constraint-adder (resolver.c's add_c is inside SMZ_STANDALONE) */
static Constraint *add_con(Document *d, uint8_t section, uint8_t subtype) {
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

/* ─── growable verbatim-line buffer for non-vertex records ────────── */
typedef struct { char **lines; int n, cap; } LineBuf;
static void lb_add(LineBuf *b, const char *s) {
    if (b->n >= b->cap) {
        b->cap = b->cap ? b->cap * 2 : 128;
        b->lines = (char **)realloc(b->lines, (size_t)b->cap * sizeof(char *));
    }
    b->lines[b->n++] = strdup(s);
}
static void lb_free(LineBuf *b) {
    for (int i = 0; i < b->n; i++) free(b->lines[i]);
    free(b->lines);
    memset(b, 0, sizeof(*b));
}

static void skip_tok(const char **p) {
    while (**p && **p != ' ' && **p != '\t') (*p)++;
    while (**p == ' ' || **p == '\t') (*p)++;
}

/* ─── Line-ASM → Document ────────────────────────────────────────── */
static int parse_into(Document *doc, const char *path, LineBuf *rest, int *nwarn) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "solve: cannot open %s\n", path); return -1; }
    char ln[2048];
    int lineno = 0;
    while (fgets(ln, sizeof(ln), f)) {
        lineno++;
        char *p = ln;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        char cmd = *p++;
        while (*p == ' ' || *p == '\t') p++;
        char *nl = strchr(p, '\n'); if (nl) *nl = 0;

        switch (cmd) {
        case 'v': {
            int id; double x, y;
            if (sscanf(p, "%d %lf %lf", &id, &x, &y) < 3) break;
            if (id < 0 || id >= MAX_PRIMS) { fprintf(stderr, "solve: line %d: v id %d out of range\n", lineno, id); break; }
            if (id >= doc->prims.n_vertices) doc->prims.n_vertices = id + 1;
            doc->prims.vertices[id].x = to_q16(x);
            doc->prims.vertices[id].y = to_q16(y);
            break;
        }
        case 'e': {
            int id, v0, v1;
            if (sscanf(p, "%d %d %d", &id, &v0, &v1) < 3) break;
            if (id < 0 || id >= MAX_PRIMS) { fprintf(stderr, "solve: line %d: e id %d out of range\n", lineno, id); break; }
            if (id >= doc->prims.n_edges) doc->prims.n_edges = id + 1;
            doc->prims.edges[id].v_start = v0;
            doc->prims.edges[id].v_end = v1;
            lb_add(rest, ln);
            break;
        }
        case 'f': {
            /* faces are not solved; preserve verbatim */
            lb_add(rest, ln);
            break;
        }
        case 's': {
            int id; char k[32];
            if (sscanf(p, "%d %31s", &id, k) < 2) break;
            if (strcmp(k, "parent") == 0) {
                int a, b;
                if (sscanf(p, "%*d parent %d %d", &a, &b) == 2) {
                    Constraint *c = add_con(doc, SEC_STRUCTURAL, S_PARENT);
                    if (c) { c->u.parent.child = a; c->u.parent.parent = b; }
                }
                lb_add(rest, ln);
            } else if (strcmp(k, "group_id") == 0) {
                int a, b;
                if (sscanf(p, "%*d group_id %d %d", &a, &b) == 2) {
                    Constraint *c = add_con(doc, SEC_STRUCTURAL, S_GROUP_ID);
                    if (c) { c->u.group.prim = a; c->u.group.group = b; }
                }
                lb_add(rest, ln);
            } else {
                int eid; char col[16];
                if (sscanf(p, "%d %d %15s", &id, &eid, col) < 3) break;
                if (id < 0 || id >= MAX_PRIMS) break;
                if (id >= doc->prims.n_strokes) doc->prims.n_strokes = id + 1;
                doc->prims.strokes[id].edge_id = eid;
                const char *t = p; for (int i = 0; i < 3; i++) skip_tok(&t);
                double ws[64]; int nw = 0;
                while (*t && *t != '#' && nw < 64) {
                    if (strncmp(t, "cap=", 4) == 0) break;
                    double w;
                    if (sscanf(t, "%lf", &w) == 1) { ws[nw++] = w; skip_tok(&t); }
                    else skip_tok(&t);
                }
                doc->prims.strokes[id].n_widths = nw;
                doc->prims.strokes[id].widths = (q16_t *)malloc((size_t)(nw ? nw : 1) * sizeof(q16_t));
                for (int i = 0; i < nw; i++) doc->prims.strokes[id].widths[i] = to_q16(ws[i]);
                lb_add(rest, ln);
            }
            break;
        }
        case 'n': {
            int id; if (sscanf(p, "%d", &id) != 1) break;
            if (id < 0 || id >= MAX_PRIMS) break;
            if (id >= doc->prims.n_nodes) doc->prims.n_nodes = id + 1;
            double tx=0, ty=0, rot=0, sx=1, sy=1, skew=0; int cref = 0;
            const char *t = p; for (int i = 0; i < 1; i++) skip_tok(&t);
            if (*t && strchr(t, '=')) {
                while (*t) {
                    while (*t == ' ') t++;
                    double v; int n;
                    if (strncmp(t, "tx=", 3) == 0 && sscanf(t + 3, "%lf%n", &v, &n) == 1) { tx = v; t += 3 + n; }
                    else if (strncmp(t, "ty=", 3) == 0 && sscanf(t + 3, "%lf%n", &v, &n) == 1) { ty = v; t += 3 + n; }
                    else if (strncmp(t, "rot=", 4) == 0 && sscanf(t + 4, "%lf%n", &v, &n) == 1) { rot = v; t += 4 + n; }
                    else if (strncmp(t, "sx=", 3) == 0 && sscanf(t + 3, "%lf%n", &v, &n) == 1) { sx = v; t += 3 + n; }
                    else if (strncmp(t, "sy=", 3) == 0 && sscanf(t + 3, "%lf%n", &v, &n) == 1) { sy = v; t += 3 + n; }
                    else if (strncmp(t, "skew=", 5) == 0 && sscanf(t + 5, "%lf%n", &v, &n) == 1) { skew = v; t += 5 + n; }
                    else if (strncmp(t, "content=", 8) == 0 && sscanf(t + 8, "%d%n", &cref, &n) == 1) { t += 8 + n; }
                    else skip_tok(&t);
                }
            } else {
                sscanf(t, "%lf %lf %lf %lf %lf %lf %d", &tx, &ty, &rot, &sx, &sy, &skew, &cref);
            }
            doc->prims.nodes[id].tx = to_q16(tx);
            doc->prims.nodes[id].ty = to_q16(ty);
            doc->prims.nodes[id].rot = to_q16(rot);
            doc->prims.nodes[id].sx = to_q16(sx);
            doc->prims.nodes[id].sy = to_q16(sy);
            doc->prims.nodes[id].skew = to_q16(skew);
            doc->prims.nodes[id].content_ref = cref;
            lb_add(rest, ln);
            break;
        }
        case 'a': {
            int id; char k[32];
            if (sscanf(p, "%d %31s", &id, k) < 2) break;
            if (strcmp(k, "edge_connects") == 0) {
                int a, b, c;
                if (sscanf(p, "%*d edge_connects %d %d %d", &a, &b, &c) == 3) {
                    Constraint *cc = add_con(doc, SEC_ASSERT, A_EDGE_CONNECTS);
                    if (cc) { cc->u.edge_connects.edge = a; cc->u.edge_connects.vs = b; cc->u.edge_connects.ve = c; }
                }
            } else if (strcmp(k, "bound_check") == 0) {
                int pr; char dimc[4]; double lo, hi;
                if (sscanf(p, "%*d bound_check %d %3s %lf %lf", &pr, dimc, &lo, &hi) == 4) {
                    Constraint *cc = add_con(doc, SEC_ASSERT, A_BOUND_CHECK);
                    if (cc) { cc->u.bound_check.prim = pr; cc->u.bound_check.dim = (dimc[0] == 'y'); cc->u.bound_check.lo = to_q16(lo); cc->u.bound_check.hi = to_q16(hi); }
                }
            } else if (strcmp(k, "state_machine") == 0) {
                /* a <id> state_machine <state_id> <initial> <target> <time|event|condition> <param> [start=..] ... */
                Constraint *cc = add_con(doc, SEC_ASSERT, A_STATE_MACHINE);
                if (cc) {
                    const char *t = p;
                    for (int i = 0; i < 2; i++) skip_tok(&t);
                    int sid, initial;
                    if (sscanf(t, "%d %d", &sid, &initial) == 2) {
                        cc->u.state_machine.state_id = sid;
                        cc->u.state_machine.initial = initial;
                        for (int i = 0; i < 2; i++) skip_tok(&t);
                        int ntr = 0;
                        cc->u.state_machine.trans = (Transition *)calloc(MAX_TRANS, sizeof(Transition));
                        while (*t && *t != '#' && ntr < MAX_TRANS) {
                            while (*t == ' ' || *t == '\t') t++;
                            if (!*t || *t == '#') break;
                            int target, n1, n2, n3; char trig[16]; double param;
                            if (sscanf(t, "%d%n %15s%n %lf%n", &target, &n1, trig, &n2, &param, &n3) != 3) break;
                            t += n3;
                            Transition *tr = &cc->u.state_machine.trans[ntr];
                            tr->target = (uint16_t)target;
                            tr->param = to_q16(param);
                            if (strcmp(trig, "time") == 0) tr->trigger_type = 0;
                            else if (strcmp(trig, "event") == 0) tr->trigger_type = 1;
                            else if (strcmp(trig, "condition") == 0) tr->trigger_type = 2;
                            else break;
                            while (*t == ' ' || *t == '\t') t++;
                            if (strncmp(t, "start=", 6) == 0) {
                                double st2; int ns;
                                if (sscanf(t + 6, "%lf%n", &st2, &ns) == 1) { tr->start_frame = to_q16(st2); t += 6 + ns; }
                            }
                            ntr++;
                        }
                        cc->u.state_machine.n_transitions = ntr;
                    }
                }
            }
            lb_add(rest, ln);
            break;
        }
        case 'c': {
            int id; char k[32];
            if (sscanf(p, "%d %31s", &id, k) < 2) break;
            if (strcmp(k, "bbox_clamp") == 0) {
                int pr; double x0, y0, x1, y1;
                if (sscanf(p, "%*d bbox_clamp %d %lf %lf %lf %lf", &pr, &x0, &y0, &x1, &y1) == 5) {
                    Constraint *cc = add_con(doc, SEC_CONSTRAINT, C_BBOX_CLAMP);
                    if (cc) { cc->u.bbox.prim = pr; cc->u.bbox.x_min = to_q16(x0); cc->u.bbox.y_min = to_q16(y0); cc->u.bbox.x_max = to_q16(x1); cc->u.bbox.y_max = to_q16(y1); }
                }
            } else if (strcmp(k, "min_dist") == 0) {
                int a, b; double d;
                if (sscanf(p, "%*d min_dist %d %d %lf", &a, &b, &d) == 3) {
                    Constraint *cc = add_con(doc, SEC_CONSTRAINT, C_MIN_DIST);
                    if (cc) { cc->u.min_dist.prim_a = a; cc->u.min_dist.prim_b = b; cc->u.min_dist.distance = to_q16(d); }
                }
            } else if (strcmp(k, "linear_eq") == 0 || strcmp(k, "linear_le") == 0 || strcmp(k, "linear_ge") == 0) {
                Constraint *cc = add_con(doc, SEC_CONSTRAINT, strcmp(k, "linear_eq") == 0 ? C_LINEAR_EQ : strcmp(k, "linear_le") == 0 ? C_LINEAR_LE : C_LINEAR_GE);
                if (!cc) break;
                const char *t = p; for (int i = 0; i < 2; i++) skip_tok(&t);   /* id kind */
                double rhs; int n;
                if (sscanf(t, "%lf%n", &rhs, &n) != 1) break;
                cc->u.linear.rhs = to_q16(rhs);
                t += n;
                int nterms = 0;
                while (*t && nterms < 32) {
                    int var, n1, n2; double coef;
                    if (sscanf(t, "%d%n %lf%n", &var, &n1, &coef, &n2) == 2 && n2 > 0) {
                        cc->u.linear.var_ids[nterms] = var;
                        cc->u.linear.coeffs[nterms] = to_q16(coef);
                        nterms++;
                        t += n2;
                    } else break;
                }
                cc->u.linear.n_terms = nterms;
            } else if (strcmp(k, "fair_blend") == 0) {
                Constraint *cc = add_con(doc, SEC_CONSTRAINT, C_FAIR_BLEND);
                if (!cc) break;
                const char *t = p; for (int i = 0; i < 2; i++) skip_tok(&t);
                int nv = 0;
                while (*t && nv < 32) {
                    int v;
                    if (sscanf(t, "%d", &v) == 1) { cc->u.fair_blend.var_ids[nv++] = v; skip_tok(&t); }
                    else break;
                }
                cc->u.fair_blend.n_vars = nv;
            }
            lb_add(rest, ln);
            break;
        }
        case 'p': {
            /* paint records are routed to the rasterizer, not solved */
            lb_add(rest, ln);
            break;
        }
        case 'k': {   /* keyframe: k <id> <node> <time> [st=..] [tx=..] [ty=..] [rot=..] [sx=..] [sy=..] [skew=..] */
            int id, node; double t;
            if (sscanf(p, "%d %d %lf", &id, &node, &t) < 3) break;
            if (node < 0 || node >= MAX_PRIMS || doc->n_keyframes >= MAX_KF) break;
            Keyframe *kf = &doc->keyframes[doc->n_keyframes];
            memset(kf, 0, sizeof(*kf));
            kf->node = node; kf->t = to_q16(t); kf->st = -1;
            const char *kt = p;
            for (int i = 0; i < 3; i++) skip_tok(&kt);
            while (*kt) {
                while (*kt == ' ' || *kt == '\t') kt++;
                if (!*kt) break;
                double v; int n;
                if (strncmp(kt, "st=", 3) == 0 && sscanf(kt + 3, "%d%n", &kf->st, &n) == 1) { kt += 3 + n; }
                else if (strncmp(kt, "tx=", 3) == 0 && sscanf(kt + 3, "%lf%n", &v, &n) == 1) { kf->v[0] = to_q16(v); kf->mask |= KF_TX; kt += 3 + n; }
                else if (strncmp(kt, "ty=", 3) == 0 && sscanf(kt + 3, "%lf%n", &v, &n) == 1) { kf->v[1] = to_q16(v); kf->mask |= KF_TY; kt += 3 + n; }
                else if (strncmp(kt, "rot=", 4) == 0 && sscanf(kt + 4, "%lf%n", &v, &n) == 1) { kf->v[2] = to_q16(v); kf->mask |= KF_ROT; kt += 4 + n; }
                else if (strncmp(kt, "sx=", 3) == 0 && sscanf(kt + 3, "%lf%n", &v, &n) == 1) { kf->v[3] = to_q16(v); kf->mask |= KF_SX; kt += 3 + n; }
                else if (strncmp(kt, "sy=", 3) == 0 && sscanf(kt + 3, "%lf%n", &v, &n) == 1) { kf->v[4] = to_q16(v); kf->mask |= KF_SY; kt += 3 + n; }
                else if (strncmp(kt, "skew=", 5) == 0 && sscanf(kt + 5, "%lf%n", &v, &n) == 1) { kf->v[5] = to_q16(v); kf->mask |= KF_SKEW; kt += 5 + n; }
                else skip_tok(&kt);
            }
            doc->n_keyframes++;
            lb_add(rest, ln);
            break;
        }
        default:
            lb_add(rest, ln);   /* preserve unknown records verbatim */
            break;
        }
    }
    fclose(f);
    (void)nwarn;
    return 0;
}

/* ─── Document → Line-ASM (vertices with resolved positions) ─────── */
static void emit_doc(Document *doc, LineBuf *rest, FILE *out, int bake_anim) {
    fprintf(out, "# SmazkaVG resolved by smazka-solve\n");
    for (int v = 0; v < doc->prims.n_vertices; v++) {
        fprintf(out, "v %d %.6f %.6f\n", v,
                from_q16(doc->prims.vertices[v].x),
                from_q16(doc->prims.vertices[v].y));
    }
    if (bake_anim) {
        for (int n = 0; n < doc->prims.n_nodes; n++) {
            Node *nd = &doc->prims.nodes[n];
            fprintf(out, "n %d tx=%.6f ty=%.6f rot=%.6f sx=%.6f sy=%.6f skew=%.6f content=%d\n",
                    n, from_q16(nd->tx), from_q16(nd->ty), from_q16(nd->rot),
                    from_q16(nd->sx), from_q16(nd->sy), from_q16(nd->skew), nd->content_ref);
        }
    }
    for (int i = 0; i < rest->n; i++) {
        if (bake_anim) {
            char c = rest->lines[i][0];
            if (c == 'n' || c == 'k') continue;
            const char *s = rest->lines[i];
            if (c == 'a' && strstr(s, "state_machine")) continue;
        }
        fputs(rest->lines[i], out);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "SmazkaVG solve tool\nUsage: %s <in.smazka> [out.smazka]\n", argv[0]);
        return 1;
    }
    if (argc < 2) {
        fprintf(stderr, "SmazkaVG solve tool\nUsage: %s <in.smazka> [out.smazka] [--t <seconds>]\n", argv[0]);
        return 1;
    }
    Document doc;
    memset(&doc, 0, sizeof(doc));
    doc.config.max_iter = MAX_ITER_DEFAULT;
    doc.config.max_ms = 200;

    double bake_t = -1.0;
    const char *outp = NULL;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--t") == 0 && i + 1 < argc) { bake_t = atof(argv[i + 1]); i++; }
        else if (outp == NULL && argv[i][0] != '-') outp = argv[i];
        else fprintf(stderr, "solve: ignoring unknown option '%s'\n", argv[i]);
    }

    LineBuf rest;
    memset(&rest, 0, sizeof(rest));
    if (parse_into(&doc, argv[1], &rest, NULL) != 0) return 1;

    int w = smazka_resolve(&doc);
    fprintf(stderr, "solve: %d vertices, %d constraints, %d warnings%s\n",
            doc.prims.n_vertices, doc.config.n_constraints, w & 0x7FFFFFFF,
            (w & 0x80000000) ? " (timeout)" : "");

    if (bake_t >= 0) {
        smazka_resolve_anim(&doc, bake_t, 1);
        fprintf(stderr, "solve: baked animation pose at t = %.3f s\n", bake_t);
    }

    FILE *out = stdout;
    if (outp) {
        out = fopen(outp, "w");
        if (!out) { fprintf(stderr, "solve: cannot write %s\n", outp); return 1; }
    }
    emit_doc(&doc, &rest, out, bake_t >= 0);
    if (out != stdout) fclose(out);

    /* free strokes' width arrays */
    for (int s = 0; s < doc.prims.n_strokes; s++) free(doc.prims.strokes[s].widths);
    lb_free(&rest);
    free(doc.config.constraints);
    return 0;
}
