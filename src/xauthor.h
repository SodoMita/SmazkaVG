/* xauthor.h — SmazkaVG authoring skin: parse-time expansion (v1.6.2)
 *
 * The problem this solves: raw Line-ASM forces hand authors (LLM or human)
 * into nameless numeric ID soup with one edge record per segment, no
 * grouping, and no way to reuse coordinate runs ("butt seams") — which is
 * why authoring kept leaking out into Python.  This header is a
 * deterministic *authoring layer* that expands to plain v/e/f/s records at
 * parse time; downstream renderers/solvers only ever see normal records.
 *
 * Grammar (records; everything else passes through, resolving symbolic IDs
 * in id positions of plain v/e/f/s records):
 *
 *   path <name> [closed] [seg|catmull] [w=W] [cap=round|butt|square]
 *        [color=HEX] item...
 *        item := x,y | use <path> | rev <path> | useg <path> | revg <path>
 *        - one vertex + edge record per x,y; w= emits one uniform stroke
 *          record per resulting edge; 'closed' adds the closing edge
 *        - use/rev   splice the named path's EDGE LIST (shared spine, true
 *                    topology reuse — mind catmull tangent bleed, AGENTS.md §1.6)
 *        - useg/revg ghost splice: re-emit the coordinates as fresh seg
 *                    edges (zero-width seam, no shared vertices; the default
 *                    seam mechanism, AGENTS.md §1.4)
 *        Endpoint mismatch across a splice: <=0.5px joins silently,
 *        <=2px bridges with a seg edge + warning, otherwise a loud error.
 *
 *   fobj <name> [fill=HEX=FFFFFF] [sw=W=3.0] [cap=...] [seg|catmull] item...
 *        Implicitly CLOSED; emits f (+ per-edge uniform strokes unless
 *        sw=0).  'seg' boundaries are straight-edge polygons (tessellate
 *        smooth loops upstream); 'catmull' boundaries keep authored knots
 *        and let the renderer tessellate the shared-vertex chain.
 *
 *   group <name>  organizational marker; subsequent fobj faces get
 *                 s <auto> group_id <face> <gnum> records.
 *
 * Symbolic IDs: bare identifiers [A-Za-z_][A-Za-z0-9_]* anywhere an id is
 * expected in plain v/e/f/s records.  Define-before-use; numbers and names
 * may be mixed (auto-assignment stays above the largest numeric id seen).
 * A purely decimal token is always a number, never a name.
 * Errors are loud: undefined names / duplicate definitions / splice
 * mismatches go to stderr AND are echoed as '# xa ERROR' lines.
 */
#ifndef SMAZKA_XAUTHOR_H
#define SMAZKA_XAUTHOR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>

#define XA_MAX_V 32768
#define XA_MAX_E 32768
#define XA_NAME   96

enum { XA_V = 0, XA_E, XA_F, XA_S, XA_PTH, XA_GRP };
#define XA_NNS 6
static const char *XA_NSNAME[] = { "v", "e", "f", "s", "path", "group" };

typedef struct {
    char name[XA_NAME];
    int ns, id;
    int *elist, ne;                 /* PATH only */
    int v_first, v_last;
    double x0, y0, x1, y1;
} XaSym;

typedef struct {
    XaSym *syms; int nsyms, cap;
    int cnt[XA_NNS];
    double vx[XA_MAX_V], vy[XA_MAX_V];
    int e0[XA_MAX_E], e1[XA_MAX_E];
    char *out; size_t olen, ocap;
    unsigned char *issued;          /* XA_NNS bitmaps of 4096 bytes: which ids are defined */
    int errors, group, group_cnt, lineno;
    FILE *err;
} Xa;

#define XA_IDBITS 4096              /* bytes per ns bitmap -> 32768 ids */
static int xa_claim(Xa *xa, int ns, int id) {   /* 1 if id already defined */
    if (!xa->issued || ns < 0 || ns >= XA_NNS || id < 0 || id >= XA_IDBITS * 8) return 0;
    return (xa->issued[ns * XA_IDBITS + id / 8] >> (id % 8)) & 1;
}
static void xa_mark(Xa *xa, int ns, int id) {
    if (ns < 0 || ns >= XA_NNS || id < 0 || id >= XA_IDBITS * 8) return;
    if (!xa->issued) xa->issued = calloc(XA_NNS, XA_IDBITS);
    xa->issued[ns * XA_IDBITS + id / 8] |= (unsigned char)(1u << (id % 8));
}

static void xa_put(Xa *xa, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[8192];
    int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    /* vsnprintf returns the would-be length; if it exceeded tmp the string
       was truncated -- only the bytes actually written may be copied. */
    size_t have = (size_t)n;
    if (have >= sizeof(tmp)) {
        have = sizeof(tmp) - 1;
        /* truncation must not swallow a record's line ending: a newline-
           terminated write keeps its newline (otherwise the next record
           glues onto a comment tail and becomes invisible to parsers) */
        size_t fl = strlen(fmt);
        if (fl && fmt[fl - 1] == '\n') tmp[have - 1] = '\n';
    }
    if (xa->olen + have + 1 >= xa->ocap) {
        while (xa->olen + have + 1 >= xa->ocap) xa->ocap *= 2;
        xa->out = realloc(xa->out, xa->ocap);
    }
    memcpy(xa->out + xa->olen, tmp, have);
    xa->olen += have;
    xa->out[xa->olen] = 0;
}

static void xa_err(Xa *xa, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    char tmp[1024];
    vsnprintf(tmp, sizeof(tmp), fmt, ap); va_end(ap);
    xa->errors++;
    fprintf(xa->err, "xa: line %d: %s\n", xa->lineno, tmp);
    xa_put(xa, "# xa ERROR: %s\n", tmp);
}

static int xa_is_num(const char *t) {           /* whole-token decimal int */
    if (*t == '-' || *t == '+') t++;
    if (!*t) return 0;
    while (*t) { if (!isdigit((unsigned char)*t)) return 0; t++; }
    return 1;
}
static int xa_is_name(const char *t) {
    if (!isalpha((unsigned char)*t) && *t != '_') return 0;
    for (const char *p = t; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_') return 0;
    return 1;
}

static XaSym *xa_find(Xa *xa, int ns, const char *name) {
    for (int i = xa->nsyms - 1; i >= 0; i--)
        if (xa->syms[i].ns == ns && strcmp(xa->syms[i].name, name) == 0)
            return &xa->syms[i];
    return NULL;
}

static XaSym *xa_define(Xa *xa, int ns, const char *name, int id) {
    XaSym *dup = xa_find(xa, ns, name);
    if (dup) { xa_err(xa, "duplicate %s id '%s'", XA_NSNAME[ns], name); return dup; }
    if (xa->nsyms == xa->cap) {
        xa->cap = xa->cap ? xa->cap * 2 : 256;
        xa->syms = realloc(xa->syms, (size_t)xa->cap * sizeof(XaSym));
    }
    XaSym *s = &xa->syms[xa->nsyms++];
    memset(s, 0, sizeof(*s));
    snprintf(s->name, XA_NAME, "%s", name);
    s->ns = ns; s->id = id;
    return s;
}

/* id token -> numeric id; define=1 on record definition positions */
static int xa_id(Xa *xa, int ns, const char *tok, int define) {
    if (xa_is_num(tok)) {
        int d = atoi(tok);
        if (define) {
            if (xa_claim(xa, ns, d)) {
                xa_err(xa, "id %d redefined (collides with issued %s id)", d, XA_NSNAME[ns]);
            }
            xa_mark(xa, ns, d);
        }
        if (d >= xa->cnt[ns]) xa->cnt[ns] = d + 1;
        return d;
    }
    if (!xa_is_name(tok)) return -1;
    XaSym *s = xa_find(xa, ns, tok);
    if (s) {
        if (define) xa_err(xa, "duplicate id '%s'", tok);
        return s->id;
    }
    if (!define) { xa_err(xa, "undefined name '%s'", tok); return -1; }
    s = xa_define(xa, ns, tok, xa->cnt[ns]++);
    xa_mark(xa, ns, s->id);
    return s->id;
}

static int xa_new_vert(Xa *xa, double x, double y) {
    int id = xa->cnt[XA_V]++;
    if (id >= XA_MAX_V) { xa_err(xa, "vertex space exhausted"); return 0; }
    xa_put(xa, "v %d %.2f %.2f\n", id, x, y);
    xa->vx[id] = x; xa->vy[id] = y;
    xa_mark(xa, XA_V, id);
    return id;
}
static int xa_new_edge(Xa *xa, int v0, int v1, const char *type) {
    int id = xa->cnt[XA_E]++;
    if (id >= XA_MAX_E) { xa_err(xa, "edge space exhausted"); return 0; }
    xa_put(xa, "e %d %d %d type=%s\n", id, v0, v1, type);
    xa->e0[id] = v0; xa->e1[id] = v1;
    xa_mark(xa, XA_E, id);
    return id;
}

static int xa_is_hexcol(const char *t) {        /* 6/8 hex digits = RRGGBB[AA] */
    size_t n = strlen(t);
    if (n != 6 && n != 8) return 0;
    for (const char *p = t; *p; p++) if (!isxdigit((unsigned char)*p)) return 0;
    return 1;
}

static int xa_tok(char *ln, char **tv, int cap) {
    int n = 0;
    char *p = ln;
    while (*p && n < cap) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p || *p == '#') break;
        tv[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r') p++;
        if (*p) { *p = 0; p++; }
    }
    return n;
}

static int xa_pt(const char *t, double *x, double *y) {
    const char *c = strchr(t, ',');
    if (!c) return 0;
    *x = strtod(t, NULL); *y = strtod(c + 1, NULL);
    return 1;
}

/* ---------------- chain builder (path/fobj) ---------------- */
typedef struct {
    int *e; int n, cap;
    int started, v_first, v_last;
    double x0, y0, x1, y1;
} XaChain;

static void xa_push(XaChain *c, int eid) {
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        c->e = realloc(c->e, (size_t)c->cap * sizeof(int));
    }
    c->e[c->n++] = eid;
}

static void xa_adopt(XaChain *c, int v, double x, double y) {
    c->started = 1; c->v_first = v; c->x0 = x; c->y0 = y;
    c->v_last = v; c->x1 = x; c->y1 = y;
}

/* validate move to (x,y) starting at vertex vn; bridges small gaps */
static int xa_join(Xa *xa, XaChain *c, double x, double y, int vn) {
    if (!c->started) { xa_adopt(c, vn, x, y); return 0; }
    double d = hypot(c->x1 - x, c->y1 - y);
    if (d <= 0.5) return 0;
    if (d <= 2.0) {
        fprintf(xa->err, "xa: line %d: warning: seam gap %.2fpx bridged at (%.0f,%.0f)\n",
                xa->lineno, d, x, y);
        xa_push(c, xa_new_edge(xa, c->v_last, vn, "seg"));
        return 0;
    }
    xa_err(xa, "splice seam gap %.2fpx at (%.0f,%.0f)->(%.0f,%.0f): refusing",
           d, c->x1, c->y1, x, y);
    return -1;
}

static int xa_chain(Xa *xa, char **it, int nit, const char *etype, int closed,
                    XaChain *c) {
    for (int i = 0; i < nit; i++) {
        const char *t = it[i];
        int mode = 0;
        if (i + 1 < nit) {
            if      (strcmp(t, "use") == 0)  mode = 1;
            else if (strcmp(t, "rev") == 0)  mode = 2;
            else if (strcmp(t, "useg") == 0) mode = 3;
            else if (strcmp(t, "revg") == 0) mode = 4;
        }
        if (mode) {
            XaSym *sp = xa_find(xa, XA_PTH, it[++i]);
            if (!sp || !sp->elist) { xa_err(xa, "%s of unknown path '%s'", t, it[i]); continue; }
            double sx = (mode & 1) ? sp->x0 : sp->x1;
            double sy = (mode & 1) ? sp->y0 : sp->y1;
            int    sv = (mode & 1) ? sp->v_first : sp->v_last;
            if (mode <= 2) {                    /* shared spine splice */
                if (xa_join(xa, c, sx, sy, sv) != 0) continue;
                for (int k = 0; k < sp->ne; k++) {
                    int e = mode == 1 ? sp->elist[k] : sp->elist[sp->ne - 1 - k];
                    xa_push(c, e);
                }
                c->v_last = mode == 1 ? sp->v_last : sp->v_first;
                c->x1 = mode == 1 ? sp->x1 : sp->x0;
                c->y1 = mode == 1 ? sp->y1 : sp->y0;
            } else {                            /* ghost splice: fresh coords */
                if (c->started) {
                    double d = hypot(c->x1 - sx, c->y1 - sy);
                    if (d > 2.0) {
                        xa_err(xa, "ghost seam gap %.2fpx: refusing", d);
                        continue;
                    }
                    if (d > 0.5) {
                        fprintf(xa->err, "xa: line %d: warning: ghost seam gap %.2fpx bridged\n",
                                xa->lineno, d);
                        int nv = xa_new_vert(xa, sx, sy);
                        xa_push(c, xa_new_edge(xa, c->v_last, nv, "seg"));
                        c->v_last = nv; c->x1 = sx; c->y1 = sy;
                    }
                }
                for (int k = 0; k < sp->ne; k++) {
                    int e = mode == 3 ? sp->elist[k] : sp->elist[sp->ne - 1 - k];
                    int near_v = mode == 3 ? xa->e0[e] : xa->e1[e];
                    int far_v  = mode == 3 ? xa->e1[e] : xa->e0[e];
                    if (!c->started)
                        xa_adopt(c, xa_new_vert(xa, xa->vx[near_v], xa->vy[near_v]),
                                 xa->vx[near_v], xa->vy[near_v]);
                    double fx = xa->vx[far_v], fy = xa->vy[far_v];
                    if (hypot(c->x1 - fx, c->y1 - fy) <= 0.5) {
                        /* zero-length piece: don't emit a degenerate edge,
                           but DO advance tracking if this is the last piece */
                        c->x1 = fx; c->y1 = fy;
                        continue;
                    }
                    int nv = xa_new_vert(xa, fx, fy);
                    xa_push(c, xa_new_edge(xa, c->v_last, nv, "seg"));
                    c->v_last = nv; c->x1 = fx; c->y1 = fy;
                }
            }
            continue;
        }
        double x, y;
        if (!xa_pt(t, &x, &y)) {
            xa_err(xa, "bad item '%s' (want x,y or use|rev|useg|revg <path>)", t);
            continue;
        }
        if (!c->started) { xa_adopt(c, xa_new_vert(xa, x, y), x, y); continue; }
        int nv = xa_new_vert(xa, x, y);
        xa_push(c, xa_new_edge(xa, c->v_last, nv, etype));
        c->v_last = nv; c->x1 = x; c->y1 = y;
    }
    if (closed && c->started && c->n >= 1 &&
        hypot(c->x1 - c->x0, c->y1 - c->y0) > 0.5)
        xa_push(c, xa_new_edge(xa, c->v_last, c->v_first, etype));
    return c->n;
}

static void xa_strokes(Xa *xa, XaChain *c, double w, const char *color,
                       const char *cap) {
    for (int i = 0; i < c->n; i++) {
        int id = xa->cnt[XA_S]++;
        xa_mark(xa, XA_S, id);
        xa_put(xa, "s %d %d %s %.2f %.2f cap=%s\n", id, c->e[i],
               color, w, w, cap);
    }
}

static void xa_register_path(Xa *xa, const char *name, XaChain *c) {
    XaSym *s = xa_define(xa, XA_PTH, name, 0);
    if (s && !s->elist) {
        s->elist = c->e; s->ne = c->n;
        s->v_first = c->v_first; s->v_last = c->v_last;
        s->x0 = c->x0; s->y0 = c->y0; s->x1 = c->x1; s->y1 = c->y1;
    } else free(c->e);
}

static void xa_record_path(Xa *xa, char **tv, int nt) {
    if (nt < 2) { xa_err(xa, "path needs a name"); return; }
    if (!xa_is_name(tv[1])) { xa_err(xa, "path name '%s' is not a bare identifier", tv[1]); return; }
    int closed = 0, i = 2;
    const char *etype = "seg";
    double w = 0;
    char color[16] = "000000", cap[16] = "round";
    for (; i < nt; i++) {
        const char *t = tv[i];
        if      (strcmp(t, "closed") == 0) closed = 1;
        else if (strcmp(t, "seg") == 0 || strcmp(t, "catmull") == 0) etype = t;
        else if (strncmp(t, "w=", 2) == 0) w = atof(t + 2);
        else if (strncmp(t, "cap=", 4) == 0) snprintf(cap, 16, "%s", t + 4);
        else if (strncmp(t, "color=", 6) == 0) snprintf(color, 16, "%s", t + 6);
        else break;
    }
    XaChain c = {0};
    xa_chain(xa, tv + i, nt - i, etype, closed, &c);
    xa_register_path(xa, tv[1], &c);
    if (w > 0) xa_strokes(xa, &c, w, color, cap);
    xa_put(xa, "# path %s (edges %d%s)\n", tv[1], c.n, closed ? ", closed" : "");
}

static void xa_record_fobj(Xa *xa, char **tv, int nt) {
    if (nt < 2) { xa_err(xa, "fobj needs a name"); return; }
    if (!xa_is_name(tv[1])) { xa_err(xa, "fobj name '%s' is not a bare identifier", tv[1]); return; }
    int i = 2;
    double sw = 3.0;
    const char *etype = "seg";
    char fill[16] = "FFFFFF", cap[16] = "round";
    for (; i < nt; i++) {
        const char *t = tv[i];
        if      (strncmp(t, "fill=", 5) == 0) snprintf(fill, 16, "%s", t + 5);
        else if (strncmp(t, "sw=", 3) == 0) sw = atof(t + 3);
        else if (strncmp(t, "cap=", 4) == 0) snprintf(cap, 16, "%s", t + 4);
        else if (strcmp(t, "seg") == 0 || strcmp(t, "catmull") == 0) etype = t;
        else break;
    }
    XaChain c = {0};
    xa_chain(xa, tv + i, nt - i, etype, 1, &c);
    if (c.n >= 3) {
        int fid = xa->cnt[XA_F]++;
        xa_mark(xa, XA_F, fid);
        xa_put(xa, "f %d", fid);
        for (int k = 0; k < c.n; k++) xa_put(xa, " %d", c.e[k]);
        xa_put(xa, " %s\n", fill);
        xa_register_path(xa, tv[1], &c);
        if (xa->group >= 0) {
            int gid = xa->cnt[XA_S]++;
            xa_mark(xa, XA_S, gid);
            xa_put(xa, "s %d group_id %d %d\n", gid, fid, xa->group);
        }
        if (sw > 0) { char col[16] = "000000"; xa_strokes(xa, &c, sw, col, cap); }
    } else {
        xa_err(xa, "fobj '%s': fewer than 3 edges, no face emitted", tv[1]);
        free(c.e);
    }
    xa_put(xa, "# fobj %s\n", tv[1]);
}

/* ---------------- plain-record symbolic-id resolution ---------------- */
static int xa_is_kw(const char *t) {
    static const char *KWS =
        "|parent|group_id|min_dist|bbox_clamp|linear_eq|linear_le|linear_ge|"
        "collision_free|fair_blend|min_stretch|state_machine|edge_connects|"
        "diffusion|solid_fill|corner|smooth|symmetric|auto|seg|quad|cubic|"
        "rational|catmull|";
    if (strchr(t, '=')) return 1;               /* key=value never a name */
    char key[128]; snprintf(key, sizeof(key), "|%s|", t);
    return strstr(KWS, key) != NULL;
}

static void xa_rewrite_plain(Xa *xa, const char *line0) {
    char buf[65536]; snprintf(buf, sizeof(buf), "%s", line0);
    char *tv[2048];
    static char pool[2048][32]; static int pi;
    int nt = xa_tok(buf, tv, 2048);
    if (nt == 0) { xa_put(xa, "\n"); return; }
    char cmd = tv[0][0];
    if (cmd == 'v' && nt >= 4) {
        if (xa_is_name(tv[1])) {
            int id = xa_id(xa, XA_V, tv[1], 1);
            double x = strtod(tv[2], NULL), y = strtod(tv[3], NULL);
            if (id >= 0 && id < XA_MAX_V) { xa->vx[id] = x; xa->vy[id] = y; }
            snprintf(pool[pi & 2047], 31, "%d", id); tv[1] = pool[pi++ & 2047];
        } else if (xa_is_num(tv[1])) {
            int id = atoi(tv[1]);
            if (xa_claim(xa, XA_V, id)) xa_err(xa, "id %d redefined (collides with issued v id)", id);
            xa_mark(xa, XA_V, id);
            double x = strtod(tv[2], NULL), y = strtod(tv[3], NULL);
            if (id >= 0 && id < XA_MAX_V) { xa->vx[id] = x; xa->vy[id] = y; }
            if (id >= xa->cnt[XA_V]) xa->cnt[XA_V] = id + 1;
        }
    } else if (cmd == 'e' && nt >= 4) {
        int eid = -1;
        if (xa_is_name(tv[1])) {
            eid = xa_id(xa, XA_E, tv[1], 1);
            snprintf(pool[pi & 2047], 31, "%d", eid); tv[1] = pool[pi++ & 2047];
        } else if (xa_is_num(tv[1])) {
            eid = atoi(tv[1]);
            if (xa_claim(xa, XA_E, eid)) xa_err(xa, "id %d redefined (collides with issued e id)", eid);
            xa_mark(xa, XA_E, eid);
            if (eid >= xa->cnt[XA_E]) xa->cnt[XA_E] = eid + 1;
        }
        int vid[2] = { -1, -1 };
        for (int k = 2; k <= 3; k++) {
            if (xa_is_name(tv[k])) {
                int id = xa_id(xa, XA_V, tv[k], 0);
                vid[k - 2] = id;
                if (id >= 0) {
                    snprintf(pool[pi & 2047], 31, "%d", id);
                    tv[k] = pool[pi++ & 2047];
                }
            } else if (xa_is_num(tv[k])) vid[k - 2] = atoi(tv[k]);
        }
        if (eid >= 0 && eid < XA_MAX_E && vid[0] >= 0 && vid[1] >= 0) {
            xa->e0[eid] = vid[0]; xa->e1[eid] = vid[1];
        }
    } else if (cmd == 'f' && nt >= 2) {
        if (xa_is_name(tv[1])) {
            int id = xa_id(xa, XA_F, tv[1], 1);
            snprintf(pool[pi & 2047], 31, "%d", id); tv[1] = pool[pi++ & 2047];
        } else if (xa_is_num(tv[1])) {
            int id = atoi(tv[1]);
            if (xa_claim(xa, XA_F, id)) xa_err(xa, "id %d redefined (collides with issued f id)", id);
            xa_mark(xa, XA_F, id);
            if (id >= xa->cnt[XA_F]) xa->cnt[XA_F] = id + 1;
        }
        for (int i = 2; i < nt; i++) {
            if (!xa_is_name(tv[i]) || xa_is_kw(tv[i])) continue;
            if (i == nt - 1 && xa_is_hexcol(tv[i])) continue;   /* fill color, not an edge name */
            int id = xa_id(xa, XA_E, tv[i], 0);
            if (id >= 0) {
                snprintf(pool[pi & 2047], 31, "%d", id);
                tv[i] = pool[pi++ & 2047];
            }
        }
    } else if (cmd == 's' && nt >= 3 && !xa_is_kw(tv[2])) {
        if (xa_is_name(tv[1])) {
            int id = xa_id(xa, XA_S, tv[1], 1);
            snprintf(pool[pi & 2047], 31, "%d", id); tv[1] = pool[pi++ & 2047];
        } else if (xa_is_num(tv[1])) {
            int id = atoi(tv[1]);
            if (xa_claim(xa, XA_S, id)) xa_err(xa, "id %d redefined (collides with issued s id)", id);
            xa_mark(xa, XA_S, id);
            if (id >= xa->cnt[XA_S]) xa->cnt[XA_S] = id + 1;
        }
        if (xa_is_name(tv[2])) {
            int id = xa_id(xa, XA_E, tv[2], 0);
            if (id >= 0) {
                snprintf(pool[pi & 2047], 31, "%d", id);
                tv[2] = pool[pi++ & 2047];
            }
        }
    }
    xa_put(xa, "%s", tv[0]);
    for (int i = 1; i < nt; i++) xa_put(xa, " %s", tv[i]);
    xa_put(xa, "\n");
}

/* ---------------- driver ---------------- */
static int xa_expand(const char *src, FILE *err, char **outp) {
    Xa xa; memset(&xa, 0, sizeof(xa));
    xa.ocap = 1 << 20; xa.out = malloc(xa.ocap); xa.out[0] = 0;
    xa.err = err ? err : stderr; xa.group = -1;
    xa_put(&xa, "# xpanded by xauthor.h (authoring skin v1.6.2)\n");
    const char *p = src;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            char *line = strndup(p, len);
            xa.lineno++;
            char *s = line; while (*s == ' ' || *s == '\t') s++;
            if (!*s || *s == '#') xa_put(&xa, "%s\n", s);
            else if (strncmp(s, "path ", 5) == 0 || strncmp(s, "path\t", 5) == 0) {
                char *copy = strdup(s);
                char *tv[2048]; int nt = xa_tok(copy, tv, 2048);
                xa_put(&xa, "# | %s\n", s);
                xa_record_path(&xa, tv, nt);
                free(copy);
            } else if (strncmp(s, "fobj ", 5) == 0 || strncmp(s, "fobj\t", 5) == 0) {
                char *copy = strdup(s);
                char *tv[2048]; int nt = xa_tok(copy, tv, 2048);
                xa_put(&xa, "# | %s\n", s);
                xa_record_fobj(&xa, tv, nt);
                free(copy);
            } else if (strncmp(s, "group ", 6) == 0 || strncmp(s, "group\t", 6) == 0) {
                char *copy = strdup(s);
                char *tv[8]; xa_tok(copy, tv, 8);
                if (tv[1]) {
                    XaSym *g = xa_find(&xa, XA_GRP, tv[1]);
                    xa.group = g ? g->id : (xa_define(&xa, XA_GRP, tv[1], 0)->id = xa.group_cnt++);
                    xa_put(&xa, "# group %s\n", tv[1]);
                }
                free(copy);
            } else xa_rewrite_plain(&xa, s);
            free(line);
        } else xa_put(&xa, "\n");
        p = nl ? nl + 1 : NULL;
    }
    if (xa.errors) fprintf(xa.err, "xa: %d error(s) during expansion\n", xa.errors);
    *outp = xa.out;
    free(xa.syms);
    free(xa.issued);
    return xa.errors;
}

static char *xa_read_expand(const char *path, FILE *err, int *nerr) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f); free(buf); return NULL;
    }
    buf[sz] = 0; fclose(f);
    char *out = NULL;
    int e = xa_expand(buf, err, &out);
    free(buf);
    if (nerr) *nerr = e;
    return out;
}

/* line iterator over an expanded buffer (drop-in for fgets loops) */
static char *xa_line(char **cur, char *dst, size_t cap) {
    if (!*cur || !**cur) return NULL;
    const char *nl = strchr(*cur, '\n');
    size_t len = nl ? (size_t)(nl - *cur) : strlen(*cur);
    if (len >= cap) len = cap - 1;
    memcpy(dst, *cur, len); dst[len] = 0;
    *cur = nl ? (char *)nl + 1 : NULL;
    return dst;
}

#endif /* SMAZKA_XAUTHOR_H */
