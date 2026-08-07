/*
 * SmazkaVG v1.3.1 Rasterizer
 * ==========================
 * - Per-pixel distance-field curve rendering (no tessellation)
 * - BMP (fast) + WebP (compressed, via external converter, no shell) + SVG + ASCII
 * - Curve types: seg, quad, cubic, rational(conic), catmull-rom
 * - Vertex types: corner, smooth, symmetric, auto (Inkscape-style)
 * - Primitives: v, e, f, s, n, r (arc), z (ellipse)
 * - Namespaces: s (structural), a (assertions), c (constraints), p (paint)
 *
 * v1.3.1 (audit pass):
 *  - Hardened parser: every global ID is bounds-checked before any write.
 *    Crafted files with huge/negative IDs are rejected with a warning instead
 *    of corrupting memory (previously: OOB writes -> crash).
 *  - Edge endpoint vertex IDs and face/stroke references are validated in a
 *    post-parse pass; dangling references are dropped with a warning.
 *  - Face fills use ordered-boundary reconstruction + ear-clipping
 *    triangulation, so concave polygons and curved-edge boundaries render
 *    correctly (previously: naive fan from edge[0].v0 -> broken/overlapping
 *    triangles for non-fan polygons).
 *  - Strokes honour the full taper profile: width is now a function of the
 *    edge parameter t (previously the profile was averaged to a single width).
 *  - Diffusion is a discrete Laplace (Poisson) solve with SOR relaxation:
 *    left/right Dirichlet colors on the curve, region border held at the
 *    artwork, so the field follows curved spines and diffuses smoothly
 *    (v1.3 was a signed-distance brush; v1.1 had only a straight-line
 *    gradient).
 *  - Catmull-Rom edges are converted to cubic Béziers once per frame (the old
 *    per-pixel code re-searched the edge table inside a 65-sample loop).
 *  - WebP export no longer shells out through system(): fork/exec only.
 *  - PNG export is fully self-contained (stored-deflate zlib stream,
 *    CRC-32 + ADLER-32) with no external dependencies.
 *
 * Build: cc -O2 -Wall -Wextra -o smazka-raster src/rasterizer.c -lm
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/wait.h>

/* ─── Limits (documented in SPEC.md §9) ─── */
#define MAX_V   32768   /* v1.6: raised for whole-figure LLM conversion docs */
#define MAX_E   32768
#define MAX_F   1024
#define MAX_S   32768
#define MAX_N   1024
#define MAX_A   256
#define MAX_EL  256
#define MAX_LINE 65536  /* v1.6.2: xpanded faces carry hundreds of edge ids */
#define MAX_W   64
#define MAX_CON 256     /* s / a / c sections */
#define MAX_PCON 128    /* p section */
#define MAX_FE  1024    /* edges per face (v1.6.2: tessellated body loops) */
#define MAX_FPTS (MAX_FE * 9 + 4)   /* max tessellated points per face */

typedef struct { double x, y; } V2;
typedef struct { uint8_t r, g, b, a; } Col;

typedef enum { E_SEG = 0, E_QUAD = 1, E_CUBIC = 2, E_RATIONAL = 3, E_CATMULL = 4 } EType;
typedef enum { V_CORNER = 0, V_SMOOTH = 1, V_SYMMETRIC = 2, V_AUTO = 3 } VType;

static int n_warn;
static void warn(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("smazka: warning: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    n_warn++;
}
static int id_ok(long id, int max) { return id >= 0 && id < max; }

static int n_v;
static struct { V2 p; VType vt; } verts[MAX_V];

static int n_e;
static struct {
    int v0, v1;             /* v0 < 0  => dead edge (invalid refs) */
    EType type;
    V2 cp[4];               /* quad:1, cubic:2, rational:2, catmull:0 */
    double w[2];            /* rational weights */
    int n_cp;
} edges[MAX_E];

static int n_f;
#define MAX_HOLES 4
#define MAX_HOLE_E 64
typedef struct {
    int eids[MAX_FE];            /* outer boundary loop */
    int ne;
    int hole_e[MAX_HOLES][MAX_HOLE_E];   /* hole loops ('|' separated) */
    int hole_len[MAX_HOLES];
    int n_holes;
    uint32_t fill;
} Face;
static Face faces[MAX_F];

static int n_s;
static struct { int eid; Col c; double w[MAX_W]; int nw; int cap; } strokes[MAX_S];  /* cap: 0=round 1=butt 2=square */

static int n_n;
typedef struct { double tx, ty, rot, sx, sy, skew; int cref; } Node;
static Node nodes[MAX_N];

/* ─── Keyframes (animation) ───
   k <id> <node_id> <time> [tx=..] [ty=..] [rot=..] [sx=..] [sy=..] [skew=..]
   A labeled keyframe sets a *partial* pose at time `time` (seconds); fields
   not listed are animated from the node's base (`n` record) values.  Per
   field, the timeline is piecewise-linear over the keyframes that set it,
   clamped outside the keyframe range (or wrapped when --loop is used). */
#define MAX_KF 512
#define KF_TX 1
#define KF_TY 2
#define KF_ROT 4
#define KF_SX 8
#define KF_SY 16
#define KF_SKEW 32
static int n_kf;
static struct {
    int node;          /* target node id */
    double t;          /* time in seconds */
    int mask;          /* which fields are set */
    double v[6];       /* tx, ty, rot, sx, sy, skew */
} kf[MAX_KF];

/* arcs */
static int n_arc;
typedef struct { V2 center; double r, a0, a1; Col c; double lw; } Arc;
static Arc arcs[MAX_A];

/* ellipses */
static int n_ell;
typedef struct { V2 c; double rx, ry, rot; Col fill; Col stroke; double sw; } Ell;
static Ell ells[MAX_EL];

/* Constraints (split namespace) */
static int n_scon, n_acon, n_con, n_pcon;
static struct { int t, a, b; } scon[MAX_CON];
static struct { int t, a, b, c; } acon[MAX_CON];
static struct { int t, a, b; double v; } con[MAX_CON];
static struct { int t, tgt; Col c1, c2; } pcon[MAX_PCON];

/* ─── Color ─── */
static Col col(const char *s) {
    Col c = {0, 0, 0, 255};
    if (!s) return c;
    unsigned v = 0;
    int l = (int)strlen(s);
    if (l >= 8) { if (sscanf(s, "%x", &v) == 1) { c.r=(v>>24)&0xFF; c.g=(v>>16)&0xFF; c.b=(v>>8)&0xFF; c.a=v&0xFF; } }
    else if (l >= 6) { if (sscanf(s, "%x", &v) == 1) { c.r=(v>>16)&0xFF; c.g=(v>>8)&0xFF; c.b=v&0xFF; c.a=255; } }
    else if (l >= 3) { if (sscanf(s, "%x", &v) == 1) { c.r=((v>>8)&0xF)*0x11; c.g=((v>>4)&0xF)*0x11; c.b=(v&0xF)*0x11; c.a=255; } }
    return c;
}
static Col col_from_u32(uint32_t v) {
    Col c; c.r=(v>>16)&0xFF; c.g=(v>>8)&0xFF; c.b=v&0xFF; c.a=255; return c;
}

/* ─── Parser helpers ─── */
static void skip_token(const char **p) {
    while (**p && **p != ' ' && **p != '\t') (*p)++;
    while (**p == ' ' || **p == '\t') (*p)++;
}
static int read_dbl(const char **p, double *out) {
    int n; if (sscanf(*p, "%lf%n", out, &n) == 1 && n > 0) { *p += n; return 1; }
    return 0;
}

/* ─── Main parser ─── */
#include "xauthor.h"

static int parse(const char *path) {
    int xa_errors = 0;
    char *xbuf = xa_read_expand(path, stderr, &xa_errors);
    if (!xbuf) { fprintf(stderr, "smazka: error: cannot open '%s'\n", path); return -1; }
    if (xa_errors)
        fprintf(stderr, "smazka: warning: %d authoring-layer error(s); check '# xa ERROR' lines\n", xa_errors);
    char *cur = xbuf;
    char ln[MAX_LINE];
    int lineno = 0;
    while (xa_line(&cur, ln, sizeof(ln))) {
        lineno++;
        char *nl = strchr(ln, '\n'); if (nl) *nl = 0;
        char *p = ln; while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        char cmd; if (sscanf(p, "%c", &cmd) != 1) continue;

        switch (cmd) {
        case 'v': {
            int id; double x, y; char vtype[16] = "corner";
            int n = sscanf(p, "v %d %lf %lf %15s", &id, &x, &y, vtype);
            if (n < 3) break;
            if (!id_ok(id, MAX_V)) { warn("line %d: v: id %d out of range [0,%d); ignored\n", lineno, id, MAX_V); break; }
            if (id >= n_v) n_v = id + 1;
            verts[id].p.x = x; verts[id].p.y = y;
            if      (strcmp(vtype, "smooth")    == 0) verts[id].vt = V_SMOOTH;
            else if (strcmp(vtype, "symmetric") == 0) verts[id].vt = V_SYMMETRIC;
            else if (strcmp(vtype, "auto")      == 0) verts[id].vt = V_AUTO;
            else                                        verts[id].vt = V_CORNER;
            break;
        }
        case 'e': {
            int id, v0, v1; char etype[32] = "";
            if (sscanf(p, "e %d %d %d %31s", &id, &v0, &v1, etype) < 3) break;
            if (!id_ok(id, MAX_E)) { warn("line %d: e: id %d out of range [0,%d); ignored\n", lineno, id, MAX_E); break; }
            if (id >= n_e) n_e = id + 1;
            edges[id].v0 = v0; edges[id].v1 = v1;
            edges[id].n_cp = 0; edges[id].w[0] = 1; edges[id].w[1] = 1;
            const char *et = etype;
            if (strncmp(et, "type=", 5) == 0) et += 5;
            if      (strcmp(et, "quad")     == 0) edges[id].type = E_QUAD;
            else if (strcmp(et, "cubic")    == 0) edges[id].type = E_CUBIC;
            else if (strcmp(et, "rational") == 0) edges[id].type = E_RATIONAL;
            else if (strcmp(et, "catmull")  == 0) edges[id].type = E_CATMULL;
            else                                   edges[id].type = E_SEG;

            const char *t = p;
            for (int i = 0; i < 4; i++) skip_token(&t);
            int max_cp = (edges[id].type == E_QUAD) ? 1
                       : (edges[id].type == E_CUBIC || edges[id].type == E_RATIONAL) ? 2 : 0;
            double vals[8] = {0};
            int nv = 0;
            while (*t && *t != '#' && nv < 8) {
                double v; if (read_dbl(&t, &v)) vals[nv++] = v; else skip_token(&t);
            }
            int vi = 0;
            for (int i = 0; i < max_cp && vi + 1 < nv; i++) {
                edges[id].cp[i].x = vals[vi++];
                edges[id].cp[i].y = vals[vi++];
            }
            edges[id].n_cp = max_cp;
            if (edges[id].type == E_RATIONAL) {
                for (int i = 0; i < 2 && vi < nv; i++) edges[id].w[i] = vals[vi++];
            }
            if (max_cp > 0 && vi < 2 * max_cp)
                warn("line %d: e %d: expected %d control coordinates, got %d; padding with 0\n",
                     lineno, id, 2 * max_cp, vi);
            break;
        }
        case 'f': {
            int id; if (sscanf(p, "f %d", &id) != 1) break;
            if (!id_ok(id, MAX_F)) { warn("line %d: f: id %d out of range [0,%d); ignored\n", lineno, id, MAX_F); break; }
            if (id >= n_f) n_f = id + 1;
            Face *F = &faces[id];
            memset(F, 0, sizeof(*F));
            const char *t = p;
            for (int i = 0; i < 2; i++) skip_token(&t);
            int cur_loop = -1;   /* -1 = outer loop; >=0 = hole index */
            int bad = 0;
            while (*t && !bad) {
                while (*t == ' ' || *t == '\t') t++;
                if (!*t || *t == '#') break;         /* check AFTER skipping spaces */
                if (*t == '|') {                    /* hole separator */
                    t++;
                    cur_loop++;
                    if (cur_loop >= MAX_HOLES) { warn("line %d: f %d: too many holes (max %d)\n", lineno, id, MAX_HOLES); bad = 1; }
                    continue;
                }
                char tok[64];
                int nread;
                if (sscanf(t, "%63s%n", tok, &nread) != 1) break;
                /* disambiguate edge ids from the trailing hex fill: edge ids are
                   short (< 10000); fill colors are 6/8 hex digits (>= 6 chars) */
                size_t tl = strlen(tok);
                int is_pure = 1;
                for (size_t k = 0; k < tl; k++)
                    if (tok[k] < '0' || tok[k] > '9') { is_pure = 0; break; }
                if (!is_pure || tl >= 6) {           /* fill color (or unknown) */
                    F->fill = (uint32_t)strtoul(tok, NULL, 16);
                    if (tl >= 8) F->fill >>= 8;      /* RRGGBBAA: keep RRGGBB, drop alpha */
                    break;
                }
                int e = atoi(tok);
                if (!id_ok(e, MAX_E)) { warn("line %d: f %d: edge id %d out of range [0,%d); face rejected\n", lineno, id, e, MAX_E); bad = 1; break; }
                if (cur_loop < 0) {
                    if (F->ne < MAX_FE) F->eids[F->ne++] = e;
                    else { warn("line %d: f %d: too many outer edges\n", lineno, id); bad = 1; }
                } else {
                    if (F->hole_len[cur_loop] < MAX_HOLE_E) F->hole_e[cur_loop][F->hole_len[cur_loop]++] = e;
                    else { warn("line %d: f %d: too many hole edges\n", lineno, id); bad = 1; }
                }
                t += nread;
            }
            F->n_holes = (cur_loop >= 0 && !bad) ? cur_loop + 1 : 0;
            if (bad) { F->ne = 0; F->n_holes = 0; }
            break;
        }
        case 's': {
            int id; char ctype[32];
            if (sscanf(p, "s %d %31s", &id, ctype) < 2) break;
            if (strcmp(ctype, "parent") == 0) {
                int a, b;
                if (sscanf(p, "s %*d parent %d %d", &a, &b) == 2) {
                    if (n_scon >= MAX_CON) { warn("line %d: too many 's' records (max %d)\n", lineno, MAX_CON); break; }
                    scon[n_scon].t = 0; scon[n_scon].a = a; scon[n_scon].b = b; n_scon++;
                }
            } else if (strcmp(ctype, "group_id") == 0) {
                int a, b;
                if (sscanf(p, "s %*d group_id %d %d", &a, &b) == 2) {
                    if (n_scon >= MAX_CON) { warn("line %d: too many 's' records (max %d)\n", lineno, MAX_CON); break; }
                    scon[n_scon].t = 1; scon[n_scon].a = a; scon[n_scon].b = b; n_scon++;
                }
            } else {
                int eid; char cstr[32];
                if (sscanf(p, "s %d %d %31s", &id, &eid, cstr) < 3) break;
                if (!id_ok(id, MAX_S)) { warn("line %d: s: id %d out of range [0,%d); ignored\n", lineno, id, MAX_S); break; }
                if (id >= n_s) n_s = id + 1;
                strokes[id].eid = eid; strokes[id].c = col(cstr); strokes[id].nw = 0; strokes[id].cap = 0;
                const char *t = p; for (int i = 0; i < 4; i++) skip_token(&t);
                while (*t && *t != '#') {
                    while (*t == ' ' || *t == '\t') t++;
                    if (strncmp(t, "cap=", 4) == 0) {      /* cap may follow the widths */
                        if (strncmp(t + 4, "butt", 4) == 0) strokes[id].cap = 1;
                        else if (strncmp(t + 4, "square", 6) == 0) strokes[id].cap = 2;
                        else strokes[id].cap = 0;
                        break;
                    }
                    double w; int n;
                    if (sscanf(t, "%lf%n", &w, &n) == 1 && n > 0) {
                        if (strokes[id].nw < MAX_W) strokes[id].w[strokes[id].nw++] = w;
                        else warn("line %d: s %d: more than %d width samples; extra ignored\n", lineno, id, MAX_W);
                        t += n;
                    } else skip_token(&t);
                }
            }
            break;
        }
        case 'a': {
            int id; char at[32];
            if (sscanf(p, "a %d %31s", &id, at) < 2) break;
            if (strcmp(at, "edge_connects") == 0) {
                int a, b, c;
                if (sscanf(p, "a %*d edge_connects %d %d %d", &a, &b, &c) == 3) {
                    if (n_acon >= MAX_CON) { warn("line %d: too many 'a' records (max %d)\n", lineno, MAX_CON); break; }
                    acon[n_acon].t = 0; acon[n_acon].a = a; acon[n_acon].b = b; acon[n_acon].c = c; n_acon++;
                }
            }
            break;
        }
        case 'c': {
            int id; char ct[32];
            if (sscanf(p, "c %d %31s", &id, ct) < 2) break;
            if (strcmp(ct, "bbox_clamp") == 0) {
                int pr; double x0, y0, x1, y1;
                if (sscanf(p, "c %*d bbox_clamp %d %lf %lf %lf %lf", &pr, &x0, &y0, &x1, &y1) == 5) {
                    if (n_con >= MAX_CON) { warn("line %d: too many 'c' records (max %d)\n", lineno, MAX_CON); break; }
                    con[n_con].t = 0; con[n_con].a = pr; con[n_con].v = x0; n_con++;
                }
            } else if (strcmp(ct, "min_dist") == 0) {
                int a, b; double d;
                if (sscanf(p, "c %*d min_dist %d %d %lf", &a, &b, &d) == 3) {
                    if (n_con >= MAX_CON) { warn("line %d: too many 'c' records (max %d)\n", lineno, MAX_CON); break; }
                    con[n_con].t = 1; con[n_con].a = a; con[n_con].b = b; con[n_con].v = d; n_con++;
                }
            }
            break;
        }
        case 'p': {
            int id; char pt[32];
            if (sscanf(p, "p %d %31s", &id, pt) < 2) break;
            if (strcmp(pt, "diffusion") == 0) {
                int eid; char L[4], R[4], lc[32], rc[32];
                if (sscanf(p, "p %*d diffusion %d %3s %31s %3s %31s", &eid, L, lc, R, rc) >= 4) {
                    if (n_pcon >= MAX_PCON) { warn("line %d: too many 'p' records (max %d)\n", lineno, MAX_PCON); break; }
                    pcon[n_pcon].t = 0; pcon[n_pcon].tgt = eid;
                    pcon[n_pcon].c1 = col(lc); pcon[n_pcon].c2 = col(rc); n_pcon++;
                }
            } else if (strcmp(pt, "solid_fill") == 0) {
                int fid; char cstr[32];
                if (sscanf(p, "p %*d solid_fill %d %31s", &fid, cstr) == 2) {
                    if (n_pcon >= MAX_PCON) { warn("line %d: too many 'p' records (max %d)\n", lineno, MAX_PCON); break; }
                    pcon[n_pcon].t = 1; pcon[n_pcon].tgt = fid; pcon[n_pcon].c1 = col(cstr); n_pcon++;
                }
            }
            break;
        }
        case 'n': {
            int id; if (sscanf(p, "n %d", &id) != 1) break;
            if (!id_ok(id, MAX_N)) { warn("line %d: n: id %d out of range [0,%d); ignored\n", lineno, id, MAX_N); break; }
            if (id >= n_n) n_n = id + 1;
            nodes[id].tx = 0; nodes[id].ty = 0; nodes[id].rot = 0;
            nodes[id].sx = 1; nodes[id].sy = 1; nodes[id].skew = 0; nodes[id].cref = -1;
            const char *t = p; for (int i = 0; i < 2; i++) skip_token(&t);
            if (*t && strchr(t, '=')) {
                while (*t) {
                    while (*t == ' ') t++;
                    if (!*t) break;
                    double v; int n;
                    if (strncmp(t, "tx=", 3) == 0 && sscanf(t + 3, "%lf%n", &v, &n) == 1) { nodes[id].tx = v; t += 3 + n; }
                    else if (strncmp(t, "ty=", 3) == 0 && sscanf(t + 3, "%lf%n", &v, &n) == 1) { nodes[id].ty = v; t += 3 + n; }
                    else if (strncmp(t, "rot=", 4) == 0 && sscanf(t + 4, "%lf%n", &v, &n) == 1) { nodes[id].rot = v; t += 4 + n; }
                    else if (strncmp(t, "sx=", 3) == 0 && sscanf(t + 3, "%lf%n", &v, &n) == 1) { nodes[id].sx = v; t += 3 + n; }
                    else if (strncmp(t, "sy=", 3) == 0 && sscanf(t + 3, "%lf%n", &v, &n) == 1) { nodes[id].sy = v; t += 3 + n; }
                    else if (strncmp(t, "skew=", 5) == 0 && sscanf(t + 5, "%lf%n", &v, &n) == 1) { nodes[id].skew = v; t += 5 + n; }
                    else if (strncmp(t, "content=", 8) == 0 && sscanf(t + 8, "%d%n", &nodes[id].cref, &n) == 1) { t += 8 + n; }
                    else skip_token(&t);
                }
            } else {
                sscanf(t, "%lf %lf %lf %lf %lf %lf %d",
                    &nodes[id].tx, &nodes[id].ty, &nodes[id].rot,
                    &nodes[id].sx, &nodes[id].sy, &nodes[id].skew, &nodes[id].cref);
            }
            break;
        }
        case 'r': { /* arc primitive */
            int id; char cstr[32]; double cx, cy, r, a0, a1, lw;
            if (sscanf(p, "r %d %lf %lf %lf %lf %lf %31s %lf", &id, &cx, &cy, &r, &a0, &a1, cstr, &lw) >= 7) {
                if (!id_ok(id, MAX_A)) { warn("line %d: r: id %d out of range [0,%d); ignored\n", lineno, id, MAX_A); break; }
                if (id >= n_arc) n_arc = id + 1;
                arcs[id].center.x = cx; arcs[id].center.y = cy;
                arcs[id].r = r; arcs[id].a0 = a0; arcs[id].a1 = a1;
                arcs[id].c = col(cstr); arcs[id].lw = lw;
            }
            break;
        }
        case 'z': { /* ellipse primitive */
            int id; char fc[32], sc[32] = "00000000"; double cx, cy, rx, ry, rot = 0, sw = 1.5;
            int n = sscanf(p, "z %d %lf %lf %lf %lf %lf %31s %31s %lf", &id, &cx, &cy, &rx, &ry, &rot, fc, sc, &sw);
            if (n >= 7) {
                if (!id_ok(id, MAX_EL)) { warn("line %d: z: id %d out of range [0,%d); ignored\n", lineno, id, MAX_EL); break; }
                if (id >= n_ell) n_ell = id + 1;
                ells[id].c.x = cx; ells[id].c.y = cy;
                ells[id].rx = rx; ells[id].ry = ry; ells[id].rot = rot;
                ells[id].fill = col(fc);
                ells[id].stroke = (n >= 8) ? col(sc) : (Col){0, 0, 0, 255};
                ells[id].sw = (n >= 9) ? sw : 1.5;
            }
            break;
        }
        case 'k': {   /* keyframe: k <id> <node_id> <time> [tx=..] [ty=..] [rot=..] [sx=..] [sy=..] [skew=..] */
            int id, node; double t;
            if (sscanf(p, "k %d %d %lf", &id, &node, &t) < 3) break;
            if (!id_ok(node, MAX_N)) { warn("line %d: k: node %d out of range [0,%d); ignored\n", lineno, node, MAX_N); break; }
            if (t < 0) t = 0;
            if (n_kf >= MAX_KF) { warn("line %d: too many keyframes (max %d)\n", lineno, MAX_KF); break; }
            kf[n_kf].node = node;
            kf[n_kf].t = t;
            kf[n_kf].mask = 0;
            const char *kt = p;
            for (int i = 0; i < 3; i++) skip_token(&kt);
            if (*kt && strchr(kt, '=')) {                 /* labeled form */
                while (*kt) {
                    while (*kt == ' ' || *kt == '\t') kt++;
                    if (!*kt) break;
                    double v; int n;
                    if (strncmp(kt, "tx=", 3) == 0 && sscanf(kt + 3, "%lf%n", &v, &n) == 1) { kf[n_kf].v[0] = v; kf[n_kf].mask |= KF_TX; kt += 3 + n; }
                    else if (strncmp(kt, "ty=", 3) == 0 && sscanf(kt + 3, "%lf%n", &v, &n) == 1) { kf[n_kf].v[1] = v; kf[n_kf].mask |= KF_TY; kt += 3 + n; }
                    else if (strncmp(kt, "rot=", 4) == 0 && sscanf(kt + 4, "%lf%n", &v, &n) == 1) { kf[n_kf].v[2] = v; kf[n_kf].mask |= KF_ROT; kt += 4 + n; }
                    else if (strncmp(kt, "sx=", 3) == 0 && sscanf(kt + 3, "%lf%n", &v, &n) == 1) { kf[n_kf].v[3] = v; kf[n_kf].mask |= KF_SX; kt += 3 + n; }
                    else if (strncmp(kt, "sy=", 3) == 0 && sscanf(kt + 3, "%lf%n", &v, &n) == 1) { kf[n_kf].v[4] = v; kf[n_kf].mask |= KF_SY; kt += 3 + n; }
                    else if (strncmp(kt, "skew=", 5) == 0 && sscanf(kt + 5, "%lf%n", &v, &n) == 1) { kf[n_kf].v[5] = v; kf[n_kf].mask |= KF_SKEW; kt += 5 + n; }
                    else skip_token(&kt);
                }
            } else {                                      /* positional: all six fields */
                double vals[6] = { 0, 0, 0, 1, 1, 0 };
                int got = 0, n2;
                for (int i = 0; i < 6 && sscanf(kt, "%lf%n", &vals[i], &n2) == 1; i++) { kt += n2; got = i + 1; }
                if (got >= 1) { kf[n_kf].v[0] = vals[0]; kf[n_kf].mask |= KF_TX; }
                if (got >= 2) { kf[n_kf].v[1] = vals[1]; kf[n_kf].mask |= KF_TY; }
                if (got >= 3) { kf[n_kf].v[2] = vals[2]; kf[n_kf].mask |= KF_ROT; }
                if (got >= 4) { kf[n_kf].v[3] = vals[3]; kf[n_kf].mask |= KF_SX; }
                if (got >= 5) { kf[n_kf].v[4] = vals[4]; kf[n_kf].mask |= KF_SY; }
                if (got >= 6) { kf[n_kf].v[5] = vals[5]; kf[n_kf].mask |= KF_SKEW; }
            }
            (void)id;
            n_kf++;
            break;
        }
        default:
            warn("line %d: unknown command '%c'\n", lineno, cmd);
            break;
        }
    }
    free(xbuf);
    return 0;
}

/* ─── Post-parse validation ─── */
static void validate_doc(void) {
    for (int i = 0; i < n_e; i++) {
        if (!id_ok(edges[i].v0, n_v) || !id_ok(edges[i].v1, n_v)) {
            warn("e %d references missing vertex (%d,%d); edge dropped\n", i, edges[i].v0, edges[i].v1);
            edges[i].v0 = edges[i].v1 = -1;
        }
    }
    for (int i = 0; i < n_s; i++) {
        if (!id_ok(strokes[i].eid, n_e) || edges[strokes[i].eid].v0 < 0) {
            warn("s %d references invalid edge %d; stroke dropped\n", i, strokes[i].eid);
            strokes[i].nw = 0;
        }
    }
    for (int i = 0; i < n_f; i++) {
        int bad = 0;
        if (faces[i].ne < 3) { warn("f %d has fewer than 3 edges; fill skipped\n", i); faces[i].ne = 0; continue; }
        for (int k = 0; k < faces[i].ne; k++) {
            int e = faces[i].eids[k];
            if (!id_ok(e, n_e) || edges[e].v0 < 0) { warn("f %d references invalid edge %d; fill skipped\n", i, e); bad = 1; break; }
        }
        for (int h = 0; h < faces[i].n_holes && !bad; h++)
            for (int k = 0; k < faces[i].hole_len[h]; k++) {
                int e = faces[i].hole_e[h][k];
                if (!id_ok(e, n_e) || edges[e].v0 < 0) { warn("f %d references invalid hole edge %d; fill skipped\n", i, e); bad = 1; break; }
            }
        if (bad) { faces[i].ne = 0; faces[i].n_holes = 0; }
    }
    for (int i = 0; i < n_n; i++) {
        int cref = nodes[i].cref;
        int ok = (cref >= 0 && cref < n_v) || (cref >= n_v && cref - n_v < n_e);
        if (!ok) {
            warn("n %d references missing content %d\n", i, cref);
            nodes[i].cref = -1;
        }
    }
    for (int i = 0; i < n_pcon; i++) {
        if (pcon[i].t == 0 && (!id_ok(pcon[i].tgt, n_e) || edges[pcon[i].tgt].v0 < 0)) {
            warn("p diffusion references invalid edge %d; dropped\n", pcon[i].tgt); pcon[i].t = -1;
        }
        if (pcon[i].t == 1 && (!id_ok(pcon[i].tgt, n_f) || faces[pcon[i].tgt].ne < 3)) {
            warn("p solid_fill references invalid face %d; dropped\n", pcon[i].tgt); pcon[i].t = -1;
        }
    }
}

/* ── Vertex type constraints (resolved at parse-time) ─── */

/* Apply vertex type constraints to adjust control points.
   For a vertex v shared by edges e_in (ending at v) and e_out (starting at v):
   - corner:    no adjustment
   - smooth:    make handles collinear
   - symmetric: make handles collinear + equal length
   - auto:      recompute handles from neighbors (Inkscape-style) */
static void resolve_vertex_types(void) {
    for (int vi = 0; vi < n_v; vi++) {
        VType vt = verts[vi].vt;
        if (vt == V_CORNER) continue;

        int e_in = -1, e_out = -1;
        for (int ei = 0; ei < n_e; ei++) {
            if (edges[ei].v0 < 0) continue;
            if (edges[ei].v1 == vi) e_in = ei;
            if (edges[ei].v0 == vi && e_out == -1) e_out = ei;
        }
        if (e_in < 0 || e_out < 0) continue;

        V2 v = verts[vi].p;

        if (vt == V_SMOOTH) {
            if (edges[e_out].n_cp >= 1 && edges[e_in].n_cp >= 1) {
                V2 h_out = edges[e_out].cp[0];
                double dx = h_out.x - v.x, dy = h_out.y - v.y;
                int last = edges[e_in].n_cp - 1;
                V2 h_in = edges[e_in].cp[last];
                double k = sqrt((h_in.x - v.x) * (h_in.x - v.x) + (h_in.y - v.y) * (h_in.y - v.y))
                         / sqrt(dx * dx + dy * dy + 1e-20);
                edges[e_in].cp[last].x = v.x - k * dx;
                edges[e_in].cp[last].y = v.y - k * dy;
            }
        } else if (vt == V_SYMMETRIC) {
            if (edges[e_out].n_cp >= 1 && edges[e_in].n_cp >= 1) {
                V2 h_out = edges[e_out].cp[0];
                int last = edges[e_in].n_cp - 1;
                edges[e_in].cp[last].x = 2 * v.x - h_out.x;
                edges[e_in].cp[last].y = 2 * v.y - h_out.y;
            }
        } else if (vt == V_AUTO) {
            int v_prev = -1, v_next = -1;
            if (edges[e_in].n_cp >= 0) v_prev = edges[e_in].v0;
            if (edges[e_out].n_cp >= 0) v_next = edges[e_out].v1;
            if (v_prev >= 0 && v_next >= 0) {
                V2 pp = verts[v_prev].p, pn = verts[v_next].p;
                if (edges[e_out].n_cp >= 1) {
                    edges[e_out].cp[0].x = v.x + (pn.x - pp.x) / 6.0;
                    edges[e_out].cp[0].y = v.y + (pn.y - pp.y) / 6.0;
                }
                int last = edges[e_in].n_cp - 1;
                if (last >= 0) {
                    edges[e_in].cp[last].x = v.x - (pn.x - pp.x) / 6.0;
                    edges[e_in].cp[last].y = v.y - (pn.y - pp.y) / 6.0;
                }
            }
        }
    }
}

/* ─── Node transforms ───
   Each node carries an affine transform (tx, ty, rot, sx, sy, skew) and a
   content reference.  content_ref is resolved against the vertex ID space
   first, then the edge ID space (offset by n_v):
     - vertex v:   transforms verts[v].p
     - edge e:     transforms both endpoint vertices and the edge's control
                   points, so curved edges transform with their geometry.
   Convention (applied per node):  p' = R(rot)·( S(sx,sy)·p + skew-shear ) + t
   Nodes are applied in ID order, so stacked nodes compose deterministically.
   The transforms are baked into the vertex store before vertex-type
   resolution and rendering (the flat model's way of emulating hierarchy). */
static V2 node_xform_p(const Node *nd, V2 p) {
    double c = cos(nd->rot), s = sin(nd->rot);
    double x = p.x * nd->sx;
    double y = p.y * nd->sy;
    x += y * nd->skew;                      /* x-shear after scale */
    return (V2){ c * x - s * y + nd->tx, s * x + c * y + nd->ty };
}

static void apply_node_transforms(void) {
    for (int i = 0; i < n_n; i++) {
        Node *nd = &nodes[i];
        if (nd->cref < 0) continue;
        if (nd->cref < n_v) {
            verts[nd->cref].p = node_xform_p(nd, verts[nd->cref].p);
        } else {
            int eid = nd->cref - n_v;
            if (edges[eid].v0 >= 0) {
                verts[edges[eid].v0].p = node_xform_p(nd, verts[edges[eid].v0].p);
                verts[edges[eid].v1].p = node_xform_p(nd, verts[edges[eid].v1].p);
                for (int k = 0; k < edges[eid].n_cp; k++)
                    edges[eid].cp[k] = node_xform_p(nd, edges[eid].cp[k]);
            }
        }
    }
}

/* base (pre-animation) snapshots — restored before every frame */
static struct { V2 p; VType vt; } verts_base[MAX_V];
static struct { int v0, v1; EType type; V2 cp[4]; double w[2]; int n_cp; } edges_base[MAX_E];
static Node nodes_base[MAX_N];

/* ─── Animation view ───
   With keyframes the auto-fit camera would follow the moving geometry, hiding
   the motion.  Instead, compute ONE view that covers the base geometry plus
   every keyframe pose (the animation's full bounding box) and freeze it. */
/* ─── Animation ───
   Keyframes are evaluated per frame: the node's six transform fields are
   each interpolated piecewise-linearly over the keyframes that set them,
   clamped outside the keyframe range (or wrapped when looping).  Fields
   with no keyframes keep the node's base value, so a keyframe only needs to
   list what changes.  The resulting pose is baked into the vertex/edge
   store exactly like a static node transform. */
static int kf_sorted = 0;
static int kf_order[MAX_KF];

static void anim_snapshot_base(void) {
    memcpy(verts_base, verts, sizeof(verts));
    memcpy(edges_base, edges, sizeof(edges));
    memcpy(nodes_base, nodes, sizeof(nodes));
}
static void anim_restore_base(void) {
    memcpy(verts, verts_base, sizeof(verts));
    memcpy(edges, edges_base, sizeof(edges));
    memcpy(nodes, nodes_base, sizeof(nodes));
}
static int kf_cmp(const void *a, const void *b) {
    int i = *(const int *)a, j = *(const int *)b;
    if (kf[i].t < kf[j].t) return -1;
    if (kf[i].t > kf[j].t) return 1;
    return i - j;
}
/* piecewise-linear value of `field` for `node` at time t; returns 0 if the
   field is not animated (no keyframes set it).  Uses keyframes that set the
   field only, so mixed partial keyframes compose naturally. */
/* map a mask bit (KF_TX..KF_SKEW) to the value index (0..5) */
static int kf_fi(int field) {
    int fi = 0;
    while ((field & 1) == 0) { fi++; field >>= 1; }
    return fi;
}
static int kf_field(double t, int node, int field, double *out) {
    int lo_t = -1, hi_t = -1;      /* nearest keyframe times below / above t */
    int fi = kf_fi(field);
    for (int i = 0; i < n_kf; i++) {
        int idx = kf_order[i];
        if (kf[idx].node != node || !(kf[idx].mask & field)) continue;
        if (kf[idx].t <= t && (lo_t < 0 || kf[idx].t > kf[lo_t].t)) { lo_t = idx; }
        if (kf[idx].t >= t && (hi_t < 0 || kf[idx].t < kf[hi_t].t)) { hi_t = idx; }
    }
    if (lo_t < 0 && hi_t < 0) return 0;                 /* not animated */
    if (lo_t < 0) { *out = kf[hi_t].v[fi]; return 1; }
    if (hi_t < 0) { *out = kf[lo_t].v[fi]; return 1; }
    if (lo_t == hi_t) { *out = kf[lo_t].v[fi]; return 1; }
    double t0 = kf[lo_t].t, t1 = kf[hi_t].t;
    double f = (t1 > t0) ? (t - t0) / (t1 - t0) : 1.0;
    *out = kf[lo_t].v[fi] * (1 - f) + kf[hi_t].v[fi] * f;
    return 1;
}

/* Evaluate all keyframes at time t (seconds) and bake node transforms.
   Call after anim_restore_base(). */
static void apply_anim(double t, int loop) {
    if (n_kf == 0) { apply_node_transforms(); return; }
    if (!kf_sorted) {
        for (int i = 0; i < n_kf; i++) kf_order[i] = i;
        qsort(kf_order, (size_t)n_kf, sizeof(int), kf_cmp);
        kf_sorted = 1;
    }

    if (loop) {
        double tmax = 0;
        for (int i = 0; i < n_kf; i++) if (kf[i].t > tmax) tmax = kf[i].t;
        if (tmax > 0) { t = fmod(t, tmax); if (t < 0) t += tmax; }
    }
    /* nodes[] currently holds the base pose; overwrite animated fields */

    for (int i = 0; i < n_n; i++) {
        double v;
        if (kf_field(t, i, KF_TX, &v)) nodes[i].tx = v;
        if (kf_field(t, i, KF_TY, &v)) nodes[i].ty = v;
        if (kf_field(t, i, KF_ROT, &v)) nodes[i].rot = v;
        if (kf_field(t, i, KF_SX, &v)) nodes[i].sx = v;
        if (kf_field(t, i, KF_SY, &v)) nodes[i].sy = v;
        if (kf_field(t, i, KF_SKEW, &v)) nodes[i].skew = v;
    }
    apply_node_transforms();
}

/* ── Curve evaluation (screen-space) ─── */

static V2 bez3(V2 p0, V2 p1, V2 p2, V2 p3, double t) {
    double u = 1 - t, u2 = u * u, u3 = u2 * u, t2 = t * t, t3 = t2 * t;
    return (V2){ u3*p0.x + 3*u2*t*p1.x + 3*u*t2*p2.x + t3*p3.x,
                 u3*p0.y + 3*u2*t*p1.y + 3*u*t2*p2.y + t3*p3.y };
}
static V2 bez2(V2 p0, V2 p1, V2 p2, double t) {
    double u = 1 - t;
    return (V2){ u*u*p0.x + 2*u*t*p1.x + t*t*p2.x,
                 u*u*p0.y + 2*u*t*p1.y + t*t*p2.y };
}
static V2 rbez(V2 p0, V2 p1, V2 p2, double t, double w0, double w1, double w2) {
    double u = 1 - t;
    double b0 = u * u * w0, b1 = 2 * u * t * w1, b2 = t * t * w2;
    double denom = b0 + b1 + b2;
    if (fabs(denom) < 1e-12) return p1;
    return (V2){ (b0*p0.x + b1*p1.x + b2*p2.x) / denom,
                 (b0*p0.y + b1*p1.y + b2*p2.y) / denom };
}

/* Screen-space copies of geometry (populated by to_screen_geom) */
static V2 ss_verts[MAX_V];
static struct {
    int v0, v1;
    EType type;
    V2 cp[4];
    double w[2];
    int n_cp;
    V2 cat_cp[2];   /* for catmull: equivalent cubic control points */
    int cat_ok;
} ss_edges[MAX_E];

static V2 s2s(V2 p, double ox, double oy, double s) { return (V2){ p.x * s + ox, p.y * s + oy }; }

/* Find the predecessor vertex of v (end of the incoming edge) and the
   successor vertex of v (start of the outgoing edge), for catmull-rom. */
static void catmull_neighbors(int eid, int *vp, int *vn) {
    *vp = -1; *vn = -1;
    for (int k = 0; k < n_e; k++) {
        if (k == eid || ss_edges[k].v0 < 0) continue;
        if (ss_edges[k].v1 == ss_edges[eid].v0 && *vp < 0) *vp = ss_edges[k].v0;
        if (ss_edges[k].v0 == ss_edges[eid].v1 && *vn < 0) *vn = ss_edges[k].v1;
    }
}

static void to_screen_geom(double ox, double oy, double sc) {
    for (int i = 0; i < n_v; i++) ss_verts[i] = s2s(verts[i].p, ox, oy, sc);
    for (int i = 0; i < n_e; i++) {
        ss_edges[i].v0 = edges[i].v0; ss_edges[i].v1 = edges[i].v1;
        ss_edges[i].type = edges[i].type;
        ss_edges[i].n_cp = edges[i].n_cp;
        ss_edges[i].cat_ok = 0;
        for (int j = 0; j < edges[i].n_cp; j++) ss_edges[i].cp[j] = s2s(edges[i].cp[j], ox, oy, sc);
        ss_edges[i].w[0] = edges[i].w[0]; ss_edges[i].w[1] = edges[i].w[1];
        if (edges[i].type == E_CATMULL && edges[i].v0 >= 0) {
            int vp, vn; catmull_neighbors(i, &vp, &vn);
            V2 p0 = ss_verts[edges[i].v0], p3 = ss_verts[edges[i].v1];
            V2 pp = vp >= 0 ? ss_verts[vp] : p0;
            V2 pn = vn >= 0 ? ss_verts[vn] : p3;
            ss_edges[i].cat_cp[0].x = p0.x + (p3.x - pp.x) / 6.0;
            ss_edges[i].cat_cp[0].y = p0.y + (p3.y - pp.y) / 6.0;
            ss_edges[i].cat_cp[1].x = p3.x - (pn.x - p0.x) / 6.0;
            ss_edges[i].cat_cp[1].y = p3.y - (pn.y - p0.y) / 6.0;
            ss_edges[i].cat_ok = 1;
        }
    }
}

/* Evaluate a point on edge eid at parameter t in [0,1] (screen space). */
static V2 eval_edge(int eid, double t) {
    V2 p0 = ss_verts[ss_edges[eid].v0], p3 = ss_verts[ss_edges[eid].v1];
    switch (ss_edges[eid].type) {
    case E_CUBIC:
        if (ss_edges[eid].n_cp >= 2) return bez3(p0, ss_edges[eid].cp[0], ss_edges[eid].cp[1], p3, t);
        break;
    case E_QUAD:
        if (ss_edges[eid].n_cp >= 1) return bez2(p0, ss_edges[eid].cp[0], p3, t);
        break;
    case E_RATIONAL:
        if (ss_edges[eid].n_cp >= 2) return rbez(p0, ss_edges[eid].cp[0], ss_edges[eid].cp[1], t, 1.0, ss_edges[eid].w[0], 1.0);
        break;
    case E_CATMULL:
        if (ss_edges[eid].cat_ok) return bez3(p0, ss_edges[eid].cat_cp[0], ss_edges[eid].cat_cp[1], p3, t);
        break;
    default: break;
    }
    return (V2){ p0.x + t * (p3.x - p0.x), p0.y + t * (p3.y - p0.y) };
}

/* Numeric first/second derivatives along the edge (robust for every type). */
static V2 eval_edge_d1(int eid, double t) {
    double h = 1e-4;
    V2 a = eval_edge(eid, t - h), b = eval_edge(eid, t + h);
    return (V2){ (b.x - a.x) / (2 * h), (b.y - a.y) / (2 * h) };
}
static V2 eval_edge_d2(int eid, double t) {
    double h = 1e-4, h2 = h * h;
    V2 a = eval_edge(eid, t - h), b = eval_edge(eid, t), c = eval_edge(eid, t + h);
    return (V2){ (a.x - 2*b.x + c.x) / h2, (a.y - 2*b.y + c.y) / h2 };
}

/* Distance from P to the curve, with closest-point parameter t.
   Newton iteration from several starts; falls back to the best sample. */
static double dist_to_edge_t(V2 P, int eid, double *tout) {
    if (eid < 0 || eid >= n_e || ss_edges[eid].v0 < 0) { if (tout) *tout = 0; return 1e30; }

    if (ss_edges[eid].type == E_SEG) {
        V2 a = ss_verts[ss_edges[eid].v0], b = ss_verts[ss_edges[eid].v1];
        double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
        double t = len2 < 1e-12 ? 0 : ((P.x - a.x) * dx + (P.y - a.y) * dy) / len2;
        if (t < 0) t = 0;
        if (t > 1) t = 1;
        V2 c = { a.x + t * dx, a.y + t * dy };
        if (tout) *tout = t;
        return sqrt((P.x - c.x) * (P.x - c.x) + (P.y - c.y) * (P.y - c.y));
    }

    double best = 1e30, best_t = 0;
    static const double starts[] = { 0.0, 0.2, 0.4, 0.6, 0.8, 1.0 };
    int nstarts = (ss_edges[eid].type == E_QUAD) ? 3 : 6;
    for (int s = 0; s < nstarts; s++) {
        double t = starts[s];
        for (int iter = 0; iter < 14; iter++) {
            V2 B = eval_edge(eid, t);
            V2 d1 = eval_edge_d1(eid, t);
            V2 d2 = eval_edge_d2(eid, t);
            double dx = P.x - B.x, dy = P.y - B.y;
            /* Solve g(t) = (P-B(t))·B'(t) = 0 by Newton:
               g' = |B'|² - (P-B)·B'' ;  t <- t - g/g'.
               Here f = -g and fp = -g', so dt = f/fp = g/g', hence -dt. */
            double f = -(dx * d1.x + dy * d1.y);
            double fp = d1.x * d1.x + d1.y * d1.y - (dx * d2.x + dy * d2.y);
            if (fabs(fp) < 1e-12) break;
            double dt = -f / fp;
            t += dt;
            if (t < 0) t = 0;
            if (t > 1) t = 1;
            if (fabs(dt) < 1e-10) break;
        }
        V2 B = eval_edge(eid, t);
        double d = sqrt((P.x - B.x) * (P.x - B.x) + (P.y - B.y) * (P.y - B.y));
        if (d < best) { best = d; best_t = t; }
    }
    if (tout) *tout = best_t;
    return best;
}

static double ss_dist_to_edge(V2 P, int eid) { return dist_to_edge_t(P, eid, NULL); }

/* ─── Framebuffer ─── */
static int FW, FH;
static uint8_t *fb; /* RGB */
static int g_debug_overlay = 0; /* --debug-overlay: raw edge guides + red vertex markers */

static void fb_init(int w, int h) {
    FW = w; FH = h; fb = (uint8_t *)malloc((size_t)w * h * 3);
    memset(fb, 255, (size_t)w * h * 3);
}
static void fb_free(void) { free(fb); }
static void fb_clear(void) { if (fb) memset(fb, 255, (size_t)FW * FH * 3); }

static inline void fb_set(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if ((unsigned)x < (unsigned)FW && (unsigned)y < (unsigned)FH) {
        int o = (y * FW + x) * 3; fb[o] = r; fb[o + 1] = g; fb[o + 2] = b;
    }
}
static inline void fb_blend(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if ((unsigned)x >= (unsigned)FW || (unsigned)y >= (unsigned)FH) return;
    if (!a) return;
    int o = (y * FW + x) * 3;
    if (a == 255) { fb[o] = r; fb[o + 1] = g; fb[o + 2] = b; return; }
    uint16_t ia = 255 - a;
    fb[o]     = (uint8_t)((r * a + fb[o]     * ia) / 255);
    fb[o + 1] = (uint8_t)((g * a + fb[o + 1] * ia) / 255);
    fb[o + 2] = (uint8_t)((b * a + fb[o + 2] * ia) / 255);
}

/* ─── Triangle fill (barycentric) ─── */
static void fill_tri(V2 v0, V2 v1, V2 v2, Col c) {
    double mnx = fmin(fmin(v0.x, v1.x), v2.x), mxx = fmax(fmax(v0.x, v1.x), v2.x);
    double mny = fmin(fmin(v0.y, v1.y), v2.y), mxy = fmax(fmax(v0.y, v1.y), v2.y);
    int x0 = (int)floor(mnx), x1 = (int)ceil(mxx), y0 = (int)floor(mny), y1 = (int)ceil(mxy);
    if (x0 < 0) x0 = 0;
    if (x1 >= FW) x1 = FW - 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= FH) y1 = FH - 1;
    double area = (v1.x - v0.x) * (v2.y - v0.y) - (v1.y - v0.y) * (v2.x - v0.x);
    if (fabs(area) < 1e-10) return;
    double ia = 1.0 / area;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) {
            double px = x + 0.5, py = y + 0.5;
            double w0 = ((v1.x - px) * (v2.y - py) - (v1.y - py) * (v2.x - px)) * ia;
            double w1 = ((v2.x - px) * (v0.y - py) - (v2.y - py) * (v0.x - px)) * ia;
            double w2 = 1 - w0 - w1;
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) fb_blend(x, y, c.r, c.g, c.b, c.a);
        }
}

/* ─── Face boundary reconstruction + ear-clipping fill ─── */

static double cross2(V2 o, V2 a, V2 b) {
    return (a.x - o.x) * (b.y - o.y) - (a.y - o.y) * (b.x - o.x);
}
static double poly_area2(V2 *p, int n) { /* 2x signed area */
    double s = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        s += p[i].x * p[j].y - p[j].x * p[i].y;
    }
    return s;
}

/* Reconstruct the ordered chain of boundary vertex IDs for an edge loop.
   Returns vertex count (>=3) or 0 if the edge list is not a closed chain. */
static int chain_from_edges(const int *eids, int n, int *vout, int cap) {
    if (n < 3 || n > MAX_FE) return 0;
    int a[MAX_FE], b[MAX_FE];
    for (int k = 0; k < n; k++) {
        a[k] = ss_edges[eids[k]].v0;
        b[k] = ss_edges[eids[k]].v1;
    }
    /* try both orientations of the first edge (the list may run CW or CCW) */
    for (int orient = 0; orient < 2; orient++) {
        int used[MAX_FE] = {0};
        int v[MAX_FE + 1];
        v[0] = orient == 0 ? a[0] : b[0];
        int cur = orient == 0 ? b[0] : a[0];
        used[0] = 1;
        int ok = 1;
        for (int step = 1; step < n; step++) {
            int found = -1, nxt = -1;
            for (int k = 1; k < n; k++) {
                if (used[k]) continue;
                if (a[k] == cur) { found = k; nxt = b[k]; break; }
                if (b[k] == cur) { found = k; nxt = a[k]; break; }
            }
            if (found < 0) { ok = 0; break; }
            used[found] = 1; v[step] = nxt; cur = nxt;
        }
        if (!ok || cur != v[0]) continue;
        if (n + 1 > cap) return 0;
        memcpy(vout, v, (n + 1) * sizeof(int));
        return n + 1;
    }
    return 0;
}

/* Tessellate one edge into a polyline (start..end inclusive). */
static int edge_tess(int eid, V2 *out, int cap) {
    int n = 2;
    if (ss_edges[eid].type != E_SEG) n = 9;   /* 8 segments per curved edge */
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) out[i] = eval_edge(eid, (double)i / (n - 1));
    return n;
}

/* Build the screen-space boundary polygon of an edge loop (curved edges
   tessellated).  Returns point count (>=3) or 0. */
static int loop_pts_from_edges(const int *eids, int n, V2 *out, int cap) {
    int vchain[MAX_FE + 1];
    int nv = chain_from_edges(eids, n, vchain, MAX_FE + 1);
    if (!nv) return 0;
    V2 tmp[MAX_FPTS];
    int nt = 0;
    for (int k = 0; k < nv; k++) {
        int eid = eids[k];
        int pn = edge_tess(eid, tmp + nt, (int)(MAX_FPTS - nt) - 1);
        nt += pn;
        if (nt >= MAX_FPTS - 2) return 0;
    }
    /* de-duplicate consecutive points (shared endpoints) */
    int m = 0;
    for (int i = 0; i < nt; i++) {
        if (m > 0 && fabs(tmp[i].x - out[m - 1].x) < 1e-9 && fabs(tmp[i].y - out[m - 1].y) < 1e-9) continue;
        if (m >= cap) return 0;
        out[m++] = tmp[i];
    }
    /* ensure the loop closes */
    if (m > 0 && (fabs(out[0].x - out[m - 1].x) > 1e-9 || fabs(out[0].y - out[m - 1].y) > 1e-9)) {
        if (m < cap) out[m++] = out[0];
    }
    return m;
}

static int face_pts(int fid, V2 *out, int cap) {
    return loop_pts_from_edges(faces[fid].eids, faces[fid].ne, out, cap);
}

static int pt_in_tri(V2 p, V2 a, V2 b, V2 c) {
    /* a,b,c CCW */
    double d1 = cross2(a, b, p), d2 = cross2(b, c, p), d3 = cross2(c, a, p);
    return d1 >= -1e-9 && d2 >= -1e-9 && d3 >= -1e-9;
}

/* Ear-clipping triangulation; falls back to a fan if no ear is found. */
static void fill_poly(V2 *pts, int n, Col c) {
    if (n < 3) return;
    if (n == 3) { fill_tri(pts[0], pts[1], pts[2], c); return; }
    if (poly_area2(pts, n) < 0) {           /* make CCW */
        for (int i = 0, j = n - 1; i < j; i++, j--) { V2 t = pts[i]; pts[i] = pts[j]; pts[j] = t; }
    }
    int *idx = (int *)malloc((size_t)n * sizeof(int));
    if (!idx) return;
    for (int i = 0; i < n; i++) idx[i] = i;
    int m = n;
    while (m > 3) {
        int clipped = 0;
        for (int i = 0; i < m && !clipped; i++) {
            int i0 = idx[(i + m - 1) % m], i1 = idx[i], i2 = idx[(i + 1) % m];
            if (cross2(pts[i0], pts[i1], pts[i2]) <= 1e-9) continue;  /* not convex (or degenerate) */
            int empty = 1;
            for (int j = 0; j < m; j++) {
                int vj = idx[j];
                if (vj == i0 || vj == i1 || vj == i2) continue;
                if (pt_in_tri(pts[vj], pts[i0], pts[i1], pts[i2])) { empty = 0; break; }
            }
            if (!empty) continue;
            fill_tri(pts[i0], pts[i1], pts[i2], c);
            memmove(idx + i, idx + i + 1, (size_t)(m - i - 1) * sizeof(int));
            m--; clipped = 1;
        }
        if (!clipped) {  /* degenerate polygon: conservative fan */
            for (int j = 1; j + 1 < m; j++) fill_tri(pts[idx[0]], pts[idx[j]], pts[idx[j + 1]], c);
            break;
        }
    }
    if (m == 3) fill_tri(pts[idx[0]], pts[idx[1]], pts[idx[2]], c);
    free(idx);
}

/* Resolve a face's fill color: explicit p solid_fill wins, else the face's
   inline fill field (RRGGBB hex). Returns 0 if the face is unfilled. */
static int face_fill_color(int fid, Col *out) {
    for (int pi = 0; pi < n_pcon; pi++) {
        if (pcon[pi].t == 1 && pcon[pi].tgt == fid) { *out = pcon[pi].c1; return 1; }
    }
    if (faces[fid].fill) { *out = col_from_u32(faces[fid].fill); return 1; }
    return 0;
}

/* Ray-cast point-in-polygon (works for concave and non-self-intersecting
   polygons; the loop must be closed, i.e. p[0] == p[n-1] or implied). */
static int point_in_loop(V2 P, const V2 *p, int n) {
    int inside = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        if (((p[i].y > P.y) != (p[j].y > P.y)) &&
            (P.x < (p[j].x - p[i].x) * (P.y - p[i].y) / (p[j].y - p[i].y + 1e-30) + p[i].x))
            inside = !inside;
    }
    return inside;
}

/* Even-odd fill over an outer loop plus hole loops: a pixel is filled when it
   is inside an odd number of loops.  Handles concave boundaries, curved
   edges (tessellated) and holes. */
static void fill_face_evenodd(V2 **loops, int *lens, int nloops, Col c) {
    double mnx = 1e30, mxx = -1e30, mny = 1e30, mxy = -1e30;
    for (int l = 0; l < nloops; l++)
        for (int i = 0; i < lens[l]; i++) {
            double x = loops[l][i].x, y = loops[l][i].y;
            if (x < mnx) mnx = x;
            if (x > mxx) mxx = x;
            if (y < mny) mny = y;
            if (y > mxy) mxy = y;
        }
    int x0 = (int)floor(mnx), x1 = (int)ceil(mxx);
    int y0 = (int)floor(mny), y1 = (int)ceil(mxy);
    if (x0 < 0) x0 = 0;
    if (x1 >= FW) x1 = FW - 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= FH) y1 = FH - 1;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            V2 P = { x + 0.5, y + 0.5 };
            int cnt = 0;
            for (int l = 0; l < nloops; l++) cnt += point_in_loop(P, loops[l], lens[l]);
            if (cnt & 1) fb_blend(x, y, c.r, c.g, c.b, c.a);
        }
    }
}

static void fill_face(int fid) {
    Col fc;
    if (!face_fill_color(fid, &fc)) return;
    if (faces[fid].ne < 3) return;
    if (faces[fid].n_holes > 0) {
        V2 outer[MAX_FPTS];
        V2 holes[MAX_HOLES][MAX_FPTS / 2];
        int n_outer = face_pts(fid, outer, MAX_FPTS);
        if (n_outer < 3) return;
        V2 *loops[1 + MAX_HOLES];
        int lens[1 + MAX_HOLES];
        loops[0] = outer; lens[0] = n_outer;
        int nloops = 1;
        for (int h = 0; h < faces[fid].n_holes; h++) {
            int n = loop_pts_from_edges(faces[fid].hole_e[h], faces[fid].hole_len[h], holes[h], MAX_FPTS / 2);
            if (n >= 3) { loops[nloops] = holes[h]; lens[nloops] = n; nloops++; }
        }
        if (nloops > 1) { fill_face_evenodd(loops, lens, nloops, fc); return; }
    }
    V2 pts[MAX_FPTS];
    int n = face_pts(fid, pts, MAX_FPTS);
    if (n >= 3) fill_poly(pts, n, fc);
}

/* ─── Per-pixel curve stroke (distance field, tapered) ─── */

static double width_at(double *w, int nw, double t) {
    if (nw <= 0) return 2.0;
    if (nw == 1) return w[0];
    double x = t * (nw - 1);
    int i0 = (int)x;
    if (i0 < 0) i0 = 0;
    if (i0 >= nw - 1) return w[nw - 1];
    double f = x - i0;
    return w[i0] * (1 - f) + w[i0 + 1] * f;
}

static void stroke_edge_perpixel(int eid, double *widths, int nw, Col c, double scale, int cap) {
    if (!id_ok(eid, n_e) || ss_edges[eid].v0 < 0) return;

    double maxw = 2.0;
    for (int i = 0; i < nw; i++) if (widths[i] > maxw) maxw = widths[i];
    double pad = maxw * scale * 0.5 + 3.0;

    V2 p0 = ss_verts[ss_edges[eid].v0], p3 = ss_verts[ss_edges[eid].v1];
    int x0 = (int)floor(fmin(p0.x, p3.x) - pad), x1 = (int)ceil(fmax(p0.x, p3.x) + pad);
    int y0 = (int)floor(fmin(p0.y, p3.y) - pad), y1 = (int)ceil(fmax(p0.y, p3.y) + pad);
    for (int i = 0; i < ss_edges[eid].n_cp; i++) {
        V2 cp = ss_edges[eid].cp[i];
        if (cp.x - pad < x0) x0 = (int)floor(cp.x - pad);
        if (cp.x + pad > x1) x1 = (int)ceil(cp.x + pad);
        if (cp.y - pad < y0) y0 = (int)floor(cp.y - pad);
        if (cp.y + pad > y1) y1 = (int)ceil(cp.y + pad);
    }
    if (x0 < 0) x0 = 0;
    if (x1 >= FW) x1 = FW - 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= FH) y1 = FH - 1;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            V2 P = { x + 0.5, y + 0.5 };
            double t = 0;
            double d = dist_to_edge_t(P, eid, &t);   /* distance, t clamped */
            double tc = t; if (tc < 0) tc = 0; if (tc > 1) tc = 1;
            double hw = width_at(widths, nw, tc) * scale * 0.5;

            /* Cap handling. dist_to_edge_t clamps t, so a pixel "beyond" an
               endpoint is detected by projecting onto the endpoint tangents.
               - round:  distance to the endpoint point (half-disk)
               - butt:   nothing beyond the endpoint (flat cut)
               - square: rectangle of width 2*hw_e extending hw_e along the
                         tangent (perpendicular distance, capped tangentially) */
            V2 T0 = eval_edge_d1(eid, 0.0);
            V2 T1 = eval_edge_d1(eid, 1.0);
            double l0 = sqrt(T0.x * T0.x + T0.y * T0.y);
            double l1 = sqrt(T1.x * T1.x + T1.y * T1.y);
            int beyond0 = (l0 > 1e-9) && (T0.x * (P.x - p0.x) + T0.y * (P.y - p0.y)) / l0 < 0;
            int beyond1 = (l1 > 1e-9) && (T1.x * (P.x - p3.x) + T1.y * (P.y - p3.y)) / l1 > 0;
            if (beyond0 || beyond1) {
                int is_start = beyond0;
                V2 E = is_start ? p0 : p3;
                V2 T = is_start ? T0 : T1;
                double tl = sqrt(T.x * T.x + T.y * T.y);
                double hw_e = width_at(widths, nw, is_start ? 0.0 : 1.0) * scale * 0.5;
                if (tl < 1e-9) {   /* degenerate endpoint: round disk */
                    d = sqrt((P.x - E.x) * (P.x - E.x) + (P.y - E.y) * (P.y - E.y));
                    hw = hw_e;
                } else {
                    double dtan = is_start
                        ? -((P.x - E.x) * T.x + (P.y - E.y) * T.y) / tl
                        :  ((P.x - E.x) * T.x + (P.y - E.y) * T.y) / tl;
                    double dperp = fabs(((P.x - E.x) * T.y - (P.y - E.y) * T.x)) / tl;
                    if (cap == 0) {
                        d = sqrt((P.x - E.x) * (P.x - E.x) + (P.y - E.y) * (P.y - E.y));
                        hw = hw_e;
                    } else if (cap == 1) {
                        d = 1e30;              /* butt: nothing beyond */
                    } else {                   /* square */
                        if (dtan >= 0 && dtan <= hw_e) { d = dperp; hw = hw_e; }
                        else d = 1e30;
                    }
                }
            }

            if (d <= hw + 0.75) {
                double alpha;
                if (d <= hw - 0.75) alpha = 1.0;
                else alpha = 1.0 - (d - hw + 0.75) / 1.5;
                if (alpha < 0) alpha = 0;
                fb_blend(x, y, c.r, c.g, c.b, (uint8_t)(alpha * c.a));
            }
        }
    }
}
/* ─── Arc rendering ─── */
static void draw_arc(int ai) {
    Arc *a = &arcs[ai];
    double pad = a->lw + 2;
    int x0 = (int)floor(a->center.x - a->r - pad), x1 = (int)ceil(a->center.x + a->r + pad);
    int y0 = (int)floor(a->center.y - a->r - pad), y1 = (int)ceil(a->center.y + a->r + pad);
    if (x0 < 0) x0 = 0;
    if (x1 >= FW) x1 = FW - 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= FH) y1 = FH - 1;
    double hw = a->lw * 0.5;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            double dx = x + 0.5 - a->center.x, dy = y + 0.5 - a->center.y;
            double dist = sqrt(dx * dx + dy * dy);
            double angle = atan2(dy, dx) * 180.0 / M_PI;
            if (angle < 0) angle += 360;
            double a0 = fmod(a->a0, 360), a1 = fmod(a->a1, 360);
            int in_arc;
            if (a0 <= a1) in_arc = (angle >= a0 && angle <= a1);
            else          in_arc = (angle >= a0 || angle <= a1);
            if (in_arc && fabs(dist - a->r) <= hw + 0.5) {
                double alpha = (fabs(dist - a->r) <= hw - 0.5) ? 1.0 : 1.0 - (fabs(dist - a->r) - hw + 0.5);
                if (alpha < 0) alpha = 0;
                fb_blend(x, y, a->c.r, a->c.g, a->c.b, (uint8_t)(alpha * a->c.a));
            }
        }
    }
}

/* ─── Ellipse rendering ─── */
static void draw_ellipse(int ei) {
    Ell *e = &ells[ei];
    double cr = cos(e->rot), sr = sin(e->rot);
    double pad = fmax(e->rx, e->ry) + e->sw + 2;
    int x0 = (int)floor(e->c.x - pad), x1 = (int)ceil(e->c.x + pad);
    int y0 = (int)floor(e->c.y - pad), y1 = (int)ceil(e->c.y + pad);
    if (x0 < 0) x0 = 0;
    if (x1 >= FW) x1 = FW - 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= FH) y1 = FH - 1;
    double hw = e->sw * 0.5;
    double rx2 = e->rx * e->rx, ry2 = e->ry * e->ry;
    if (rx2 < 1e-12 || ry2 < 1e-12) return;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            double dx = x + 0.5 - e->c.x, dy = y + 0.5 - e->c.y;
            double ex = dx * cr + dy * sr, ey = -dx * sr + dy * cr;
            double val = (ex * ex) / rx2 + (ey * ey) / ry2;
            if (val <= 1.0)
                fb_blend(x, y, e->fill.r, e->fill.g, e->fill.b, e->fill.a);
            if (e->stroke.a > 0) {
                double avg_r = (e->rx + e->ry) * 0.5;
                double d = fabs(sqrt(fmax(val, 0.0)) - 1.0) * avg_r;
                if (d <= hw + 0.5) {
                    double alpha = (d <= hw - 0.5) ? 1.0 : 1.0 - (d - hw + 0.5);
                    if (alpha < 0) alpha = 0;
                    fb_blend(x, y, e->stroke.r, e->stroke.g, e->stroke.b, (uint8_t)(alpha * e->stroke.a));
                }
            }
        }
    }
}

/* ── View fit ─── */
static int view_fixed = 0;
static double fix_ox, fix_oy, fix_sc;

static void view(double *ox, double *oy, double *sc) {
    if (view_fixed) { *ox = fix_ox; *oy = fix_oy; *sc = fix_sc; return; }
    double mnx = 1e30, mxx = -1e30, mny = 1e30, mxy = -1e30;
    int any = 0;
    for (int i = 0; i < n_v; i++) {
        if (verts[i].p.x < mnx) mnx = verts[i].p.x;
        if (verts[i].p.x > mxx) mxx = verts[i].p.x;
        if (verts[i].p.y < mny) mny = verts[i].p.y;
        if (verts[i].p.y > mxy) mxy = verts[i].p.y;
        any = 1;
    }
    /* include curve control points so curved geometry is not clipped */
    for (int i = 0; i < n_e; i++) {
        for (int j = 0; j < edges[i].n_cp; j++) {
            if (edges[i].cp[j].x < mnx) mnx = edges[i].cp[j].x;
            if (edges[i].cp[j].x > mxx) mxx = edges[i].cp[j].x;
            if (edges[i].cp[j].y < mny) mny = edges[i].cp[j].y;
            if (edges[i].cp[j].y > mxy) mxy = edges[i].cp[j].y;
            any = 1;
        }
    }
    for (int i = 0; i < n_arc; i++) {
        if (arcs[i].center.x - arcs[i].r < mnx) mnx = arcs[i].center.x - arcs[i].r;
        if (arcs[i].center.x + arcs[i].r > mxx) mxx = arcs[i].center.x + arcs[i].r;
        if (arcs[i].center.y - arcs[i].r < mny) mny = arcs[i].center.y - arcs[i].r;
        if (arcs[i].center.y + arcs[i].r > mxy) mxy = arcs[i].center.y + arcs[i].r;
        any = 1;
    }
    for (int i = 0; i < n_ell; i++) {
        if (ells[i].c.x - ells[i].rx < mnx) mnx = ells[i].c.x - ells[i].rx;
        if (ells[i].c.x + ells[i].rx > mxx) mxx = ells[i].c.x + ells[i].rx;
        if (ells[i].c.y - ells[i].ry < mny) mny = ells[i].c.y - ells[i].ry;
        if (ells[i].c.y + ells[i].ry > mxy) mxy = ells[i].c.y + ells[i].ry;
        any = 1;
    }
    if (!any) { *ox = *oy = 0; *sc = 1; return; }
    double sw = mxx - mnx; if (sw < 1) sw = 1;
    double sh = mxy - mny; if (sh < 1) sh = 1;
    double m = 50;
    double sx = (FW - 2 * m) / sw, sy = (FH - 2 * m) / sh;
    *sc = fmin(sx, sy);
    *ox = m - mnx * (*sc) + ((FW - 2 * m) - sw * (*sc)) * 0.5;
    *oy = m - mny * (*sc) + ((FH - 2 * m) - sh * (*sc)) * 0.5;
}
static void bbox_pt(V2 p, double *mnx, double *mny, double *mxx, double *mxy) {
    if (p.x < *mnx) *mnx = p.x;
    if (p.x > *mxx) *mxx = p.x;
    if (p.y < *mny) *mny = p.y;
    if (p.y > *mxy) *mxy = p.y;
}
static void node_bbox_at_pose(const Node *pose,
                              double *mnx, double *mny, double *mxx, double *mxy) {
    int cref = pose->cref;
    if (cref < 0) return;
    if (cref < n_v) {
        bbox_pt(node_xform_p(pose, verts_base[cref].p), mnx, mny, mxx, mxy);
    } else {
        int eid = cref - n_v;
        if (eid < 0 || eid >= n_e || edges_base[eid].v0 < 0) return;
        bbox_pt(node_xform_p(pose, verts_base[edges_base[eid].v0].p), mnx, mny, mxx, mxy);
        bbox_pt(node_xform_p(pose, verts_base[edges_base[eid].v1].p), mnx, mny, mxx, mxy);
        for (int k = 0; k < edges_base[eid].n_cp; k++)
            bbox_pt(node_xform_p(pose, edges_base[eid].cp[k]), mnx, mny, mxx, mxy);
    }
}
static void anim_compute_view(void) {
    double mnx = 1e30, mny = 1e30, mxx = -1e30, mxy = -1e30;
    int any = 0;
    for (int i = 0; i < n_v; i++) { bbox_pt(verts_base[i].p, &mnx, &mny, &mxx, &mxy); any = 1; }
    for (int i = 0; i < n_e; i++)
        for (int j = 0; j < edges_base[i].n_cp; j++) { bbox_pt(edges_base[i].cp[j], &mnx, &mny, &mxx, &mxy); any = 1; }
    for (int i = 0; i < n_arc; i++) {
        bbox_pt((V2){ arcs[i].center.x - arcs[i].r, arcs[i].center.y }, &mnx, &mny, &mxx, &mxy);
        bbox_pt((V2){ arcs[i].center.x + arcs[i].r, arcs[i].center.y }, &mnx, &mny, &mxx, &mxy);
        bbox_pt((V2){ arcs[i].center.x, arcs[i].center.y - arcs[i].r }, &mnx, &mny, &mxx, &mxy);
        bbox_pt((V2){ arcs[i].center.x, arcs[i].center.y + arcs[i].r }, &mnx, &mny, &mxx, &mxy);
        any = 1;
    }
    for (int i = 0; i < n_ell; i++) {
        bbox_pt((V2){ ells[i].c.x - ells[i].rx, ells[i].c.y }, &mnx, &mny, &mxx, &mxy);
        bbox_pt((V2){ ells[i].c.x + ells[i].rx, ells[i].c.y }, &mnx, &mny, &mxx, &mxy);
        bbox_pt((V2){ ells[i].c.x, ells[i].c.y - ells[i].ry }, &mnx, &mny, &mxx, &mxy);
        bbox_pt((V2){ ells[i].c.x, ells[i].c.y + ells[i].ry }, &mnx, &mny, &mxx, &mxy);
        any = 1;
    }
    /* every keyframe pose is an extreme */
    for (int k = 0; k < n_kf; k++) {
        Node pose = nodes_base[kf[k].node];
        if (kf[k].mask & KF_TX)   pose.tx = kf[k].v[0];
        if (kf[k].mask & KF_TY)   pose.ty = kf[k].v[1];
        if (kf[k].mask & KF_ROT)  pose.rot = kf[k].v[2];
        if (kf[k].mask & KF_SX)   pose.sx = kf[k].v[3];
        if (kf[k].mask & KF_SY)   pose.sy = kf[k].v[4];
        if (kf[k].mask & KF_SKEW) pose.skew = kf[k].v[5];
        node_bbox_at_pose(&pose, &mnx, &mny, &mxx, &mxy);
        any = 1;
    }
    if (!any) { fix_ox = fix_oy = 0; fix_sc = 1; }
    else {
        double sw = mxx - mnx; if (sw < 1) sw = 1;
        double sh = mxy - mny; if (sh < 1) sh = 1;
        double m = 50;
        double sx = (FW - 2 * m) / sw, sy = (FH - 2 * m) / sh;
        fix_sc = fmin(sx, sy);
        fix_ox = m - mnx * fix_sc + ((FW - 2 * m) - sw * fix_sc) * 0.5;
        fix_oy = m - mny * fix_sc + ((FH - 2 * m) - sh * fix_sc) * 0.5;
    }
    view_fixed = 1;
}


/* ─── Diffusion curves (discrete Laplace / Poisson solve) ───
   For each `p diffusion` record we solve the Laplace equation over the
   framebuffer region around the curve:
       ∇²c = 0
   with Dirichlet boundary conditions c = left_color on one side of the
   curve and c = right_color on the other, and the region border held at the
   existing framebuffer content, so the field diffuses smoothly into the
   artwork.  Solved by successive over-relaxation (SOR) with bounded,
   deterministic iterations (SPEC §5.6.1).  This is the Orzan et al.
   diffusion-curves model discretized per pixel: it follows curved spines
   and produces smooth fields, replacing the v1.3 perpendicular-gradient
   brush. */
static void draw_diffusion(int pi) {
    int eid = pcon[pi].tgt;
    if (!id_ok(eid, n_e) || ss_edges[eid].v0 < 0) return;
    V2 a = ss_verts[ss_edges[eid].v0], b = ss_verts[ss_edges[eid].v1];
    double rad = 60 * ((double)FW / 512.0);   /* ~60px at 512-wide view */
    if (rad > 150) rad = 150;                  /* bound the solve region */
    double pad = rad + 2;
    int x0 = (int)floor(fmin(a.x, b.x) - pad), x1 = (int)ceil(fmax(a.x, b.x) + pad);
    int y0 = (int)floor(fmin(a.y, b.y) - pad), y1 = (int)ceil(fmax(a.y, b.y) + pad);
    for (int i = 0; i < ss_edges[eid].n_cp; i++) {
        V2 cp = ss_edges[eid].cp[i];
        if (cp.x - pad < x0) x0 = (int)floor(cp.x - pad);
        if (cp.x + pad > x1) x1 = (int)ceil(cp.x + pad);
        if (cp.y - pad < y0) y0 = (int)floor(cp.y - pad);
        if (cp.y + pad > y1) y1 = (int)ceil(cp.y + pad);
    }
    if (x0 < 0) x0 = 0;
    if (x1 >= FW) x1 = FW - 1;
    if (y0 < 0) y0 = 0;
    if (y1 >= FH) y1 = FH - 1;
    int W = x1 - x0 + 1, H = y1 - y0 + 1;
    if (W < 3 || H < 3) return;
    long area = (long)W * H;
    if (area > 900000L) return;               /* bounded computation */

    Col lc = pcon[pi].c1, rc = pcon[pi].c2;
    float *R = (float *)malloc((size_t)area * sizeof(float));
    float *G = (float *)malloc((size_t)area * sizeof(float));
    float *B = (float *)malloc((size_t)area * sizeof(float));
    uint8_t *fixed = (uint8_t *)malloc((size_t)area);
    if (!R || !G || !B || !fixed) { free(R); free(G); free(B); free(fixed); return; }

    /* init from the framebuffer (region border = artwork = Neumann-free BC) */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int o = ((y0 + y) * FW + (x0 + x)) * 3;
            int idx = y * W + x;
            R[idx] = fb[o];
            G[idx] = fb[o + 1];
            B[idx] = fb[o + 2];
        }
    }
    memset(fixed, 0, (size_t)area);

    /* Dirichlet: paint left/right colors onto pixels within ~2.5px of the
       curve, classified by the sign of the tangent cross product */
    {
        int n_samp = 256;
        for (int s = 0; s <= n_samp; s++) {
            double tt = (double)s / n_samp;
            V2 Q = eval_edge(eid, tt);
            V2 T = eval_edge_d1(eid, tt);
            double tl = sqrt(T.x * T.x + T.y * T.y);
            if (tl < 1e-9) continue;
            T.x /= tl; T.y /= tl;
            int cx = (int)floor(Q.x), cy = (int)floor(Q.y);
            for (int dy = -2; dy <= 2; dy++) {
                for (int dx = -2; dx <= 2; dx++) {
                    int px = cx + dx, py = cy + dy;
                    if (px < x0 || px > x1 || py < y0 || py > y1) continue;
                    double ex = px + 0.5 - Q.x, ey = py + 0.5 - Q.y;
                    if (ex * ex + ey * ey > 2.5 * 2.5) continue;
                    double side = ex * T.y - ey * T.x;   /* + = left side */
                    int idx = (py - y0) * W + (px - x0);
                    fixed[idx] = 1;
                    if (side >= 0) { R[idx] = lc.r; G[idx] = lc.g; B[idx] = lc.b; }
                    else           { R[idx] = rc.r; G[idx] = rc.g; B[idx] = rc.b; }
                }
            }
        }
    }

    /* SOR relaxation, bounded iterations (deterministic: same input, same
       trajectory; max-change threshold breaks early) */
    int maxiter = (area > 400000L) ? 300 : 600;
    const double omega = 1.6;
    for (int it = 0; it < maxiter; it++) {
        double maxch = 0.0;
        for (int y = 1; y < H - 1; y++) {
            for (int x = 1; x < W - 1; x++) {
                int idx = y * W + x;
                if (fixed[idx]) continue;
                double nvR = 0.25 * (R[idx - 1] + R[idx + 1] + R[idx - W] + R[idx + W]);
                double chR = omega * (nvR - R[idx]);
                R[idx] += chR;
                if (fabs(chR) > maxch) maxch = fabs(chR);
                double nvG = 0.25 * (G[idx - 1] + G[idx + 1] + G[idx - W] + G[idx + W]);
                double chG = omega * (nvG - G[idx]);
                G[idx] += chG;
                if (fabs(chG) > maxch) maxch = fabs(chG);
                double nvB = 0.25 * (B[idx - 1] + B[idx + 1] + B[idx - W] + B[idx + W]);
                double chB = omega * (nvB - B[idx]);
                B[idx] += chB;
                if (fabs(chB) > maxch) maxch = fabs(chB);
            }
        }
        if (maxch < 0.25 / 255.0) break;   /* converged below 1/4 of a level */
    }

    /* write the solved field back (region border == background, so the
       transition to the surrounding artwork is continuous) */
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            int idx = y * W + x;
            int o = ((y0 + y) * FW + (x0 + x)) * 3;
            fb[o]     = (uint8_t)(R[idx] < 0 ? 0 : R[idx] > 255 ? 255 : R[idx]);
            fb[o + 1] = (uint8_t)(G[idx] < 0 ? 0 : G[idx] > 255 ? 255 : G[idx]);
            fb[o + 2] = (uint8_t)(B[idx] < 0 ? 0 : B[idx] > 255 ? 255 : B[idx]);
        }
    }

    free(R); free(G); free(B); free(fixed);
}

/* ─── Render ─── */
static void render(void) {
    double ox, oy, sc; view(&ox, &oy, &sc);
    to_screen_geom(ox, oy, sc);

    /* Ellipses — transform in place, render, restore */
    V2 ell_c_save[MAX_EL]; double ell_rx_save[MAX_EL], ell_ry_save[MAX_EL], ell_sw_save[MAX_EL];
    for (int i = 0; i < n_ell; i++) {
        ell_c_save[i] = ells[i].c; ell_rx_save[i] = ells[i].rx;
        ell_ry_save[i] = ells[i].ry; ell_sw_save[i] = ells[i].sw;
        ells[i].c = s2s(ells[i].c, ox, oy, sc);
        ells[i].rx *= sc; ells[i].ry *= sc; ells[i].sw *= sc;
    }
    for (int i = 0; i < n_ell; i++) draw_ellipse(i);
    for (int i = 0; i < n_ell; i++) {
        ells[i].c = ell_c_save[i]; ells[i].rx = ell_rx_save[i];
        ells[i].ry = ell_ry_save[i]; ells[i].sw = ell_sw_save[i];
    }

    /* Solid face fills (ear-clipping, concave + curved safe) */
    for (int fi = 0; fi < n_f; fi++)
        if (faces[fi].ne >= 3) fill_face(fi);

    /* Diffusion */
    for (int pi = 0; pi < n_pcon; pi++)
        if (pcon[pi].t == 0) draw_diffusion(pi);

    /* Edge outlines (per-pixel, thin) — debug overlay only */
    if (g_debug_overlay)
    for (int ei = 0; ei < n_e; ei++) {
        if (ss_edges[ei].v0 < 0) continue;
        V2 p0 = ss_verts[ss_edges[ei].v0], p3 = ss_verts[ss_edges[ei].v1];
        double w = 1.5, pad = 3;
        int x0 = (int)floor(fmin(p0.x, p3.x) - pad), x1 = (int)ceil(fmax(p0.x, p3.x) + pad);
        int y0 = (int)floor(fmin(p0.y, p3.y) - pad), y1 = (int)ceil(fmax(p0.y, p3.y) + pad);
        for (int i = 0; i < ss_edges[ei].n_cp; i++) {
            V2 cp = ss_edges[ei].cp[i];
            if (cp.x - pad < x0) x0 = (int)floor(cp.x - pad);
            if (cp.x + pad > x1) x1 = (int)ceil(cp.x + pad);
            if (cp.y - pad < y0) y0 = (int)floor(cp.y - pad);
            if (cp.y + pad > y1) y1 = (int)ceil(cp.y + pad);
        }
        if (x0 < 0) x0 = 0;
        if (x1 >= FW) x1 = FW - 1;
        if (y0 < 0) y0 = 0;
        if (y1 >= FH) y1 = FH - 1;
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                V2 P = { x + 0.5, y + 0.5 };
                double d = ss_dist_to_edge(P, ei);
                double hw = w * 0.5;
                if (d <= hw + 0.5) {
                    double al = (d <= hw - 0.5) ? 1.0 : 1.0 - (d - hw + 0.5);
                    if (al < 0) al = 0;
                    fb_blend(x, y, 40, 40, 40, (uint8_t)(al * 255));
                }
            }
    }

    /* Strokes (per-pixel, variable width along t) */
    for (int si = 0; si < n_s; si++)
        if (strokes[si].nw > 0)
            stroke_edge_perpixel(strokes[si].eid, strokes[si].w, strokes[si].nw, strokes[si].c, sc, strokes[si].cap);

    /* Arcs */
    for (int i = 0; i < n_arc; i++) {
        arcs[i].center = s2s(arcs[i].center, ox, oy, sc);
        arcs[i].r *= sc; arcs[i].lw *= sc;
        draw_arc(i);
    }

    /* Vertices — debug overlay only */
    if (g_debug_overlay)
    for (int vi = 0; vi < n_v; vi++) {
        V2 sp = ss_verts[vi];
        int cx = (int)sp.x, cy = (int)sp.y;
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
                if (dx * dx + dy * dy <= 5) fb_set(cx + dx, cy + dy, 220, 50, 50);
    }
}

/* ─── BMP output ─── */
static void write_bmp(const char *path) {
    FILE *f = fopen(path, "wb"); if (!f) return;
    int rb = FW * 3, pad = (4 - rb % 4) % 4, isz = (rb + pad) * FH, fsz = 54 + isz;
    uint8_t fh[14] = {0}; fh[0] = 'B'; fh[1] = 'M';
    fh[2] = fsz & 0xFF; fh[3] = (fsz >> 8) & 0xFF; fh[4] = (fsz >> 16) & 0xFF; fh[5] = (fsz >> 24) & 0xFF;
    fh[10] = 54; fwrite(fh, 1, 14, f);
    uint8_t dh[40] = {0}; dh[0] = 40;
    dh[4] = FW & 0xFF; dh[5] = (FW >> 8) & 0xFF; dh[6] = (FW >> 16) & 0xFF; dh[7] = (FW >> 24) & 0xFF;
    dh[8] = FH & 0xFF; dh[9] = (FH >> 8) & 0xFF; dh[10] = (FH >> 16) & 0xFF; dh[11] = (FH >> 24) & 0xFF;
    dh[12] = 1; dh[14] = 24;
    dh[20] = isz & 0xFF; dh[21] = (isz >> 8) & 0xFF; dh[22] = (isz >> 16) & 0xFF; dh[23] = (isz >> 24) & 0xFF;
    fwrite(dh, 1, 40, f);
    uint8_t pb[3] = {0};
    for (int y = FH - 1; y >= 0; y--) {
        for (int x = 0; x < FW; x++) {
            int o = (y * FW + x) * 3;
            uint8_t bgr[3] = { fb[o + 2], fb[o + 1], fb[o] };
            fwrite(bgr, 1, 3, f);
        }
        if (pad) fwrite(pb, 1, pad, f);
    }
    fclose(f);
}

/* ── PNG output (self-contained: stored-deflate zlib stream, filter 0) ── */
static uint32_t crc_tab[256];
static void crc_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_tab[i] = c;
    }
}
/* raw (running) CRC-32 update: no complement on entry/exit, so calls chain */
static uint32_t crc32_raw(uint32_t c, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; i++) c = crc_tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}
static void png_chunk(FILE *f, const char *type, const uint8_t *data, uint32_t len) {
    uint8_t hdr[8] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len,
                       (uint8_t)type[0], (uint8_t)type[1], (uint8_t)type[2], (uint8_t)type[3] };
    fwrite(hdr, 1, 8, f);
    if (len) fwrite(data, 1, len, f);
    uint32_t c = 0xFFFFFFFFu;
    c = crc32_raw(c, (const uint8_t *)type, 4);
    if (len) c = crc32_raw(c, data, len);
    c ^= 0xFFFFFFFFu;
    uint8_t cb[4] = { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
    fwrite(cb, 1, 4, f);
}

static void write_png(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13] = { 0 };
    ihdr[0] = FW >> 24; ihdr[1] = FW >> 16; ihdr[2] = FW >> 8; ihdr[3] = FW;
    ihdr[4] = FH >> 24; ihdr[5] = FH >> 16; ihdr[6] = FH >> 8; ihdr[7] = FH;
    ihdr[8] = 8;   /* bit depth */
    ihdr[9] = 2;   /* color type: RGB */
    png_chunk(f, "IHDR", ihdr, 13);

    /* IDAT: zlib stream (header 0x78 0x01) with stored deflate blocks and
       filter-0 scanlines.  The chunk length is known up front, so we can
       stream the blocks and update the CRC incrementally. */
    size_t raw_len = (1 + (size_t)FW * 3) * (size_t)FH;   /* filter + RGB per row */
    int nblocks = (int)((raw_len + 65534) / 65535);
    uint32_t idat_len = (uint32_t)(2 + 4 + raw_len + 5 * (size_t)nblocks);
    uint8_t hdr8[8] = { (uint8_t)(idat_len >> 24), (uint8_t)(idat_len >> 16),
                        (uint8_t)(idat_len >> 8), (uint8_t)idat_len,
                        'I', 'D', 'A', 'T' };
    fwrite(hdr8, 1, 8, f);
    uint32_t c = 0xFFFFFFFFu;
    c = crc32_raw(c, (const uint8_t *)"IDAT", 4);

    uint8_t zl[2] = { 0x78, 0x01 };
    fwrite(zl, 1, 2, f);
    c = crc32_raw(c, zl, 2);

    /* v1.6 fix: emit stored blocks of EXACTLY 65535 bytes (last one shorter)
       so the precomputed nblocks/idat_len always match the actual stream.
       The old row-triggered flush sliced blocks at buffer boundaries, which
       produced more blocks than idat_len claimed on large frames (broken
       PNG when (1+FW*3)*FH got big, e.g. a 1350x2268 reference frame). */
    size_t rowb = (size_t)1 + (size_t)FW * 3;
    size_t cap = (size_t)65535 + rowb + 8;
    uint8_t *tmp = (uint8_t *)malloc(cap);
    if (!tmp) { fclose(f); return; }
    size_t used = 0;
    uint32_t adler_a = 1, adler_b = 0;   /* adler32 of the raw (filtered) stream */
    uint64_t written = 0;                /* bytes of raw stream emitted so far */
    for (int y = 0; y < FH; y++) {
        tmp[used++] = 0;                 /* filter: None */
        adler_a = (adler_a + 0) % 65521;
        adler_b = (adler_b + adler_a) % 65521;
        long long o = (long long)y * FW * 3;
        for (int x = 0; x < FW; x++) {
            tmp[used++] = fb[o + x * 3];
            adler_a = (adler_a + fb[o + x * 3]) % 65521;
            adler_b = (adler_b + adler_a) % 65521;
            tmp[used++] = fb[o + x * 3 + 1];
            adler_a = (adler_a + fb[o + x * 3 + 1]) % 65521;
            adler_b = (adler_b + adler_a) % 65521;
            tmp[used++] = fb[o + x * 3 + 2];
            adler_a = (adler_a + fb[o + x * 3 + 2]) % 65521;
            adler_b = (adler_b + adler_a) % 65521;
        }
        while (used >= 65535 || (y == FH - 1 && used > 0)) {
            size_t chunk = used >= 65535 ? 65535 : used;
            int final = (written + chunk == (uint64_t)raw_len) ? 1 : 0;
            uint8_t bh[5] = { (uint8_t)final,
                              (uint8_t)(chunk & 0xFF), (uint8_t)((chunk >> 8) & 0xFF),
                              (uint8_t)((~chunk) & 0xFF), (uint8_t)((~(chunk >> 8)) & 0xFF) };
            fwrite(bh, 1, 5, f);
            c = crc32_raw(c, bh, 5);
            fwrite(tmp, 1, chunk, f);
            c = crc32_raw(c, tmp, chunk);
            written += chunk;
            used -= chunk;
            if (used) memmove(tmp, tmp + chunk, used);
        }
    }
    free(tmp);
    uint32_t adler = (adler_b << 16) | adler_a;
    uint8_t tr[4] = { (uint8_t)(adler >> 24), (uint8_t)(adler >> 16), (uint8_t)(adler >> 8), (uint8_t)adler };
    fwrite(tr, 1, 4, f);
    c = crc32_raw(c, tr, 4);
    c ^= 0xFFFFFFFFu;
    uint8_t cb[4] = { (uint8_t)(c >> 24), (uint8_t)(c >> 16), (uint8_t)(c >> 8), (uint8_t)c };
    fwrite(cb, 1, 4, f);

    png_chunk(f, "IEND", NULL, 0);
    fclose(f);
}

/* ── WebP output via external converter (fork/exec only — no shell) ── */
static int try_exec(char *const argv[]) {
    pid_t pid = fork();
    if (pid == 0) { execvp(argv[0], argv); _exit(127); }
    if (pid > 0) {
        int st = 0;
        if (waitpid(pid, &st, 0) == pid)
            return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
    }
    return -1;
}
static void write_webp(const char *bmp_path, const char *webp_path) {
    char *img[] = { "convert", (char *)bmp_path, (char *)webp_path, NULL };
    if (try_exec(img) == 0) return;
    char *ff[] = { "ffmpeg", "-y", "-i", (char *)bmp_path, "-c:v", "libwebp",
                   "-lossless", "1", "-q:v", "90", (char *)webp_path, NULL };
    if (try_exec(ff) == 0) return;
    char *py[] = { "python3", "-c",
                   "from PIL import Image; import sys; Image.open(sys.argv[1]).save(sys.argv[2])",
                   (char *)bmp_path, (char *)webp_path, NULL };
    try_exec(py);
}

/* ── SVG output ── */
static void write_svg(const char *path) {
    FILE *f = fopen(path, "w"); if (!f) return;
    double ox, oy, sc; view(&ox, &oy, &sc);
    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\" width=\"%d\" height=\"%d\">\n", FW, FH, FW, FH);
    fprintf(f, "  <!-- SmazkaVG v1.4 SVG projection -->\n");
    fprintf(f, "  <rect width=\"%d\" height=\"%d\" fill=\"white\"/>\n", FW, FH);

    /* Ellipses */
    for (int i = 0; i < n_ell; i++) {
        V2 c = s2s(ells[i].c, ox, oy, sc);
        fprintf(f, "  <ellipse cx=\"%.2f\" cy=\"%.2f\" rx=\"%.2f\" ry=\"%.2f\"", c.x, c.y, ells[i].rx * sc, ells[i].ry * sc);
        fprintf(f, " fill=\"rgba(%d,%d,%d,%.2f)\"", ells[i].fill.r, ells[i].fill.g, ells[i].fill.b, ells[i].fill.a / 255.0);
        fprintf(f, " stroke=\"rgba(%d,%d,%d,%.2f)\" stroke-width=\"%.2f\"/>\n",
                ells[i].stroke.r, ells[i].stroke.g, ells[i].stroke.b, ells[i].stroke.a / 255.0, ells[i].sw * sc);
    }

    /* Arcs */
    for (int i = 0; i < n_arc; i++) {
        V2 c = s2s(arcs[i].center, ox, oy, sc); double r = arcs[i].r * sc;
        double a0 = arcs[i].a0 * M_PI / 180, a1 = arcs[i].a1 * M_PI / 180;
        double x0 = c.x + r * cos(a0), y0 = c.y + r * sin(a0), x1 = c.x + r * cos(a1), y1 = c.y + r * sin(a1);
        int large = (fabs(a1 - a0) > M_PI) ? 1 : 0;
        fprintf(f, "  <path d=\"M %.2f,%.2f A %.2f,%.2f 0 %d 1 %.2f,%.2f\"", x0, y0, r, r, large, x1, y1);
        fprintf(f, " stroke=\"rgba(%d,%d,%d,%.2f)\" stroke-width=\"%.2f\" fill=\"none\"/>\n",
                arcs[i].c.r, arcs[i].c.g, arcs[i].c.b, arcs[i].c.a / 255.0, arcs[i].lw * sc);
    }

    /* Faces (tessellated boundary polygon(s), same as the raster fill; holes
       become even-odd subpaths) */
    for (int fi = 0; fi < n_f; fi++) {
        if (faces[fi].ne < 3) continue;
        Col fc;
        if (!face_fill_color(fi, &fc)) continue;
        V2 pts[MAX_FPTS];
        V2 holes[MAX_HOLES][MAX_FPTS / 2];
        int n = face_pts(fi, pts, MAX_FPTS);
        if (n < 3) continue;
        fprintf(f, "  <path d=\"M ");
        for (int k = 0; k < n; k++) {
            if (k == 0) fprintf(f, "%.2f,%.2f", pts[k].x, pts[k].y);
            else fprintf(f, " L %.2f,%.2f", pts[k].x, pts[k].y);
        }
        fprintf(f, " Z");
        if (faces[fi].n_holes > 0) {
            for (int h = 0; h < faces[fi].n_holes; h++) {
                int hn = loop_pts_from_edges(faces[fi].hole_e[h], faces[fi].hole_len[h], holes[h], MAX_FPTS / 2);
                if (hn < 3) continue;
                for (int k = 0; k < hn; k++) {
                    if (k == 0) fprintf(f, " M %.2f,%.2f", holes[h][k].x, holes[h][k].y);
                    else fprintf(f, " L %.2f,%.2f", holes[h][k].x, holes[h][k].y);
                }
                fprintf(f, " Z");
            }
            fprintf(f, "\" fill=\"rgba(%d,%d,%d,%.2f)\" fill-rule=\"evenodd\"/>\n", fc.r, fc.g, fc.b, fc.a / 255.0);
        } else {
            fprintf(f, "\" fill=\"rgba(%d,%d,%d,%.2f)\"/>\n", fc.r, fc.g, fc.b, fc.a / 255.0);
        }
    }

    /* Diffusion (linear gradient along the spine, for SVG renderers) */
    for (int pi = 0; pi < n_pcon; pi++) {
        if (pcon[pi].t != 0) continue;
        int eid = pcon[pi].tgt; if (!id_ok(eid, n_e)) continue;
        V2 a = s2s(verts[edges[eid].v0].p, ox, oy, sc), b = s2s(verts[edges[eid].v1].p, ox, oy, sc);
        Col lc = pcon[pi].c1, rc = pcon[pi].c2;
        fprintf(f, "  <defs><linearGradient id=\"d%d\" x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\">", pi, a.x, a.y, b.x, b.y);
        fprintf(f, "<stop offset=\"0%%\" stop-color=\"rgb(%d,%d,%d)\"/>", lc.r, lc.g, lc.b);
        fprintf(f, "<stop offset=\"100%%\" stop-color=\"rgb(%d,%d,%d)\"/></linearGradient></defs>\n", rc.r, rc.g, rc.b);
        fprintf(f, "  <line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"url(#d%d)\" stroke-width=\"8\" opacity=\"0.6\"/>\n", a.x, a.y, b.x, b.y, pi);
    }

    /* Edges — debug overlay only */
    if (g_debug_overlay)
    for (int ei = 0; ei < n_e; ei++) {
        if (edges[ei].v0 < 0) continue;
        V2 sa = s2s(verts[edges[ei].v0].p, ox, oy, sc), sb = s2s(verts[edges[ei].v1].p, ox, oy, sc);
        if (edges[ei].type == E_CUBIC && edges[ei].n_cp >= 2) {
            V2 c1 = s2s(edges[ei].cp[0], ox, oy, sc), c2 = s2s(edges[ei].cp[1], ox, oy, sc);
            fprintf(f, "  <path id=\"e%d\" d=\"M %.2f,%.2f C %.2f,%.2f %.2f,%.2f %.2f,%.2f\" stroke=\"#282828\" stroke-width=\"1.5\" fill=\"none\"/>\n", ei, sa.x, sa.y, c1.x, c1.y, c2.x, c2.y, sb.x, sb.y);
        } else if (edges[ei].type == E_QUAD && edges[ei].n_cp >= 1) {
            V2 c1 = s2s(edges[ei].cp[0], ox, oy, sc);
            fprintf(f, "  <path id=\"e%d\" d=\"M %.2f,%.2f Q %.2f,%.2f %.2f,%.2f\" stroke=\"#282828\" stroke-width=\"1.5\" fill=\"none\"/>\n", ei, sa.x, sa.y, c1.x, c1.y, sb.x, sb.y);
        } else {
            fprintf(f, "  <line id=\"e%d\" x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"#282828\" stroke-width=\"1.5\"/>\n", ei, sa.x, sa.y, sb.x, sb.y);
        }
    }

    /* Strokes */
    for (int si = 0; si < n_s; si++) {
        int eid = strokes[si].eid; if (!id_ok(eid, n_e) || strokes[si].nw == 0) continue;
        V2 sa = s2s(verts[edges[eid].v0].p, ox, oy, sc), sb = s2s(verts[edges[eid].v1].p, ox, oy, sc);
        Col c = strokes[si].c;
        double aw = 0; for (int w = 0; w < strokes[si].nw; w++) aw += strokes[si].w[w];
        aw /= strokes[si].nw;
        const char *cap = strokes[si].cap == 1 ? "butt" : strokes[si].cap == 2 ? "square" : "round";
        if (edges[eid].type == E_CUBIC && edges[eid].n_cp >= 2) {
            V2 c1 = s2s(edges[eid].cp[0], ox, oy, sc), c2 = s2s(edges[eid].cp[1], ox, oy, sc);
            fprintf(f, "  <path id=\"s%d\" d=\"M %.2f,%.2f C %.2f,%.2f %.2f,%.2f %.2f,%.2f\" stroke=\"rgba(%d,%d,%d,%.2f)\" stroke-width=\"%.2f\" stroke-linecap=\"%s\" fill=\"none\"/>\n",
                    si, sa.x, sa.y, c1.x, c1.y, c2.x, c2.y, sb.x, sb.y, c.r, c.g, c.b, c.a / 255.0, aw * sc, cap);
        } else if (edges[eid].type == E_QUAD && edges[eid].n_cp >= 1) {
            V2 c1 = s2s(edges[eid].cp[0], ox, oy, sc);
            fprintf(f, "  <path id=\"s%d\" d=\"M %.2f,%.2f Q %.2f,%.2f %.2f,%.2f\" stroke=\"rgba(%d,%d,%d,%.2f)\" stroke-width=\"%.2f\" stroke-linecap=\"%s\" fill=\"none\"/>\n",
                    si, sa.x, sa.y, c1.x, c1.y, sb.x, sb.y, c.r, c.g, c.b, c.a / 255.0, aw * sc, cap);
        } else {
            fprintf(f, "  <line id=\"s%d\" x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"rgba(%d,%d,%d,%.2f)\" stroke-width=\"%.2f\" stroke-linecap=\"%s\"/>\n",
                    si, sa.x, sa.y, sb.x, sb.y, c.r, c.g, c.b, c.a / 255.0, aw * sc, cap);
        }
    }

    /* Vertices — debug overlay only */
    if (g_debug_overlay)
    for (int vi = 0; vi < n_v; vi++) {
        V2 sp = s2s(verts[vi].p, ox, oy, sc);
        const char *vt = "corner";
        if (verts[vi].vt == V_SMOOTH) vt = "smooth";
        else if (verts[vi].vt == V_SYMMETRIC) vt = "symmetric";
        else if (verts[vi].vt == V_AUTO) vt = "auto";
        fprintf(f, "  <circle id=\"v%d\" cx=\"%.2f\" cy=\"%.2f\" r=\"3\" fill=\"#dc3232\" data-type=\"%s\"/>\n", vi, sp.x, sp.y, vt);
    }
    fprintf(f, "</svg>\n");
    fclose(f);
}

/* ── ASCII ─── */
static void write_ascii(const char *path) {
    FILE *f = fopen(path, "w"); if (!f) return;
    const char *ramp = " .:-=+*#%@"; int rl = (int)strlen(ramp);
    int cw = 4, ch = 7, cols = FW / cw, rows = FH / ch;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;
    if (cols > 160) cols = 160;
    if (rows > 80) rows = 80;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            double s = 0; int n = 0;
            for (int dy = 0; dy < ch; dy++) {
                int py = r * ch + dy; if (py >= FH) break;
                for (int dx = 0; dx < cw; dx++) {
                    int px = c * cw + dx; if (px >= FW) break;
                    int o = (py * FW + px) * 3;
                    s += 0.299 * fb[o] + 0.587 * fb[o + 1] + 0.114 * fb[o + 2]; n++;
                }
            }
            double l = n ? s / n : 255;
            int idx = rl - 1 - (int)(l / 256 * rl);
            if (idx < 0) idx = 0;
            if (idx >= rl) idx = rl - 1;
            fputc(ramp[idx], f);
        }
        fputc('\n', f);
    }
    fclose(f);
}

/* ─── Main ─── */
static void strip_ext(const char *in, char *out, int sz) {
    strncpy(out, in, sz - 1); out[sz - 1] = 0;
    char *d = strrchr(out, '.'); if (d) *d = 0;
}

/* ── Animated GIF from the PNG frames (best-effort, via PIL) ── */
static void write_animated_gif(const char *prefix, int nframes, int fps) {
    char *av[1024]; int n = 0;
    av[n++] = "python3"; av[n++] = "-c";
    av[n++] = "from PIL import Image; import sys; fs=sys.argv[1:-2]; imgs=[Image.open(f) for f in fs]; imgs[0].save(sys.argv[-1], save_all=True, append_images=imgs[1:], duration=1000//int(sys.argv[-2]), loop=0)";
    char *tmpnames = (char *)malloc((size_t)nframes * 64);
    for (int i = 0; i < nframes; i++) {
        snprintf(tmpnames + (size_t)i * 64, 64, "%s_%03d.png", prefix, i);
        av[n++] = tmpnames + (size_t)i * 64;
    }
    char fpsbuf[16], outbuf[560];
    snprintf(fpsbuf, sizeof(fpsbuf), "%d", fps);
    snprintf(outbuf, sizeof(outbuf), "%s.gif", prefix);
    av[n++] = fpsbuf; av[n++] = outbuf; av[n] = NULL;
    if (try_exec(av) == 0) fprintf(stderr, "GIF:  %s\n", outbuf);
    free(tmpnames);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "SmazkaVG v1.5 Rasterizer\n"
                        "Usage: %s <in.smazka> [w] [h] [options]\n"
                        "Options:\n"
                        "  --anim <fps> <frames>   render a frame sequence (PNG+BMP per frame, +GIF if PIL present)\n"
                        "  --t <seconds>           render a single frame at time t\n"
                        "  --out <prefix>          output prefix for the frame sequence\n"
                        "  --loop                  wrap time modulo the animation duration\n"
                        "  --debug-overlay         draw raw edge guides + red vertex markers (authoring aid)\n"
                        "  --xpand <out.smazka>     expand the authoring skin (path/fobj/group/symbolic ids)\n"
                        "                        into plain Line-ASM records and exit (use '-' for stdout)\n"
                        "  --view <ox> <oy> <sc>   pin the view transform (pixel-exact mapping; 0 0 1 =\n"
                        "                        document coords are image pixels, no auto-fit margin)\n", argv[0]);
        return 1;
    }
    const char *inp = argv[1];
    int w = 512, h = 512;
    int anim = 0, fps = 12, nframes = 24, loop = 0;
    double t_single = -1.0;
    char out_prefix[560] = "";
    const char *xpand_out = NULL;
    /* parse [w] [h] positionals then flags */
    int pos = 2;
    if (pos < argc && argv[pos][0] != '-') { w = atoi(argv[pos++]); }
    if (pos < argc && argv[pos][0] != '-') { h = atoi(argv[pos++]); }
    for (int i = pos; i < argc; i++) {
        if (strcmp(argv[i], "--anim") == 0 && i + 2 < argc) { fps = atoi(argv[i + 1]); nframes = atoi(argv[i + 2]); anim = 1; i += 2; }
        else if (strcmp(argv[i], "--t") == 0 && i + 1 < argc) { t_single = atof(argv[i + 1]); i += 1; }
        else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) { snprintf(out_prefix, sizeof(out_prefix), "%s", argv[i + 1]); i += 1; }
        else if (strcmp(argv[i], "--loop") == 0) { loop = 1; }
        else if (strcmp(argv[i], "--debug-overlay") == 0) { g_debug_overlay = 1; }
        else if (strcmp(argv[i], "--xpand") == 0 && i + 1 < argc) { xpand_out = argv[i + 1]; i += 1; }
        else if (strcmp(argv[i], "--view") == 0 && i + 3 < argc) {
            fix_ox = atof(argv[i + 1]); fix_oy = atof(argv[i + 2]);
            fix_sc = atof(argv[i + 3]); view_fixed = 1; i += 3;
        }
        else fprintf(stderr, "smazka: ignoring unknown option '%s'\n", argv[i]);
    }
    if (xpand_out) {                 /* expand authoring skin -> plain Line-ASM, exit */
        int nerr = 0;
        char *x = xa_read_expand(inp, stderr, &nerr);
        if (!x) { fprintf(stderr, "smazka: xpand: cannot read '%s'\n", inp); return 1; }
        FILE *of = strcmp(xpand_out, "-") == 0 ? stdout : fopen(xpand_out, "w");
        if (!of) { fprintf(stderr, "smazka: xpand: cannot write '%s'\n", xpand_out); free(x); return 1; }
        fwrite(x, 1, strlen(x), of);
        if (of != stdout) fclose(of);
        free(x);
        fprintf(stderr, "xpand: %s -> %s (%d error(s))\n", inp, xpand_out, nerr);
        return nerr ? 2 : 0;
    }
    if (w < 64) w = 64;
    if (h < 64) h = 64;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;

    if (parse(inp) != 0) return 1;
    validate_doc();
    if (n_kf > 0) anim_snapshot_base();
    else apply_node_transforms();
    resolve_vertex_types();

    int nq = 0, nc = 0, nr = 0, nm = 0;
    for (int i = 0; i < n_e; i++) {
        if (edges[i].type == E_QUAD) nq++;
        else if (edges[i].type == E_CUBIC) nc++;
        else if (edges[i].type == E_RATIONAL) nr++;
        else if (edges[i].type == E_CATMULL) nm++;
    }
    fprintf(stderr, "v1.5: %d verts, %d edges (seg:%d quad:%d cubic:%d rat:%d cat:%d), %d faces, %d strokes, %d arcs, %d ellipses, %d keyframes, %d warnings\n",
            n_v, n_e, n_e - nq - nc - nr - nm, nq, nc, nr, nm, n_f, n_s, n_arc, n_ell, n_kf, n_warn);

    char base[512]; strip_ext(inp, base, sizeof(base));
    crc_init();
    fb_init(w, h);

    if (anim) {
        /* frame sequence */
        if (!out_prefix[0]) snprintf(out_prefix, sizeof(out_prefix), "%s", base);
        fprintf(stderr, "anim: %d frames @ %d fps (t = 0 .. %.3f s), loop=%d\n", nframes, fps, (double)(nframes - 1) / fps, loop);
        anim_compute_view();
        for (int f = 0; f < nframes; f++) {
            double t = (double)f / fps;
            fb_clear();
            if (n_kf > 0) { anim_restore_base(); apply_anim(t, loop); }
            render();
            char png_p[600], bmp_p[600];
            snprintf(png_p, sizeof(png_p), "%s_%03d.png", out_prefix, f);
            snprintf(bmp_p, sizeof(bmp_p), "%s_%03d.bmp", out_prefix, f);
            write_png(png_p);
            write_bmp(bmp_p);
            fprintf(stderr, "frame %3d: t=%6.3f  %s\n", f, t, png_p);
        }
        write_animated_gif(out_prefix, nframes, fps);
        return 0;
    }

    /* single frame (optionally at time t) */
    if (n_kf > 0) {
        double t = (t_single >= 0) ? t_single : 0.0;
        anim_restore_base();
        anim_compute_view();
        apply_anim(t, loop);
        if (t_single >= 0) fprintf(stderr, "anim: frame at t = %.3f s\n", t);
    }

    render();

    /* --out pins the single-frame output prefix too (defaults to input name) */
    if (!out_prefix[0]) snprintf(out_prefix, sizeof(out_prefix), "%s", base);
    char bmp_p[560], webp_p[560], png_p[560], svg_p[560], txt_p[560];
    snprintf(bmp_p, sizeof(bmp_p), "%s.bmp", out_prefix);
    snprintf(webp_p, sizeof(webp_p), "%s.webp", out_prefix);
    snprintf(png_p, sizeof(png_p), "%s.png", out_prefix);
    snprintf(svg_p, sizeof(svg_p), "%s.svg", out_prefix);
    snprintf(txt_p, sizeof(txt_p), "%s.txt", out_prefix);

    write_png(png_p);   fprintf(stderr, "PNG:  %s (%dx%d)\n", png_p, w, h);
    write_bmp(bmp_p);   fprintf(stderr, "BMP:  %s (%dx%d, %d KB)\n", bmp_p, w, h, (54 + w * h * 3) / 1024);
    write_webp(bmp_p, webp_p); fprintf(stderr, "WebP: %s\n", webp_p);
    write_svg(svg_p);   fprintf(stderr, "SVG:  %s\n", svg_p);
    write_ascii(txt_p); fprintf(stderr, "ASCII: %s\n", txt_p);

    fb_free();
    return 0;
}
