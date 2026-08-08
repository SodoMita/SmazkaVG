/* xauthor.h — SmazkaVG authoring skin & compact dialect parser (v1.7)
 *
 * Provides parse-time expansion of high-level authoring skins and ultra-compact
 * shorthand vector representations (Line-ASM, xauthor dialect, code-golf .sg).
 *
 * Grammar (records; everything else passes through, resolving symbolic IDs
 * in id positions of plain v/e/f/s records):
 *
 *   path <name> [closed] [seg|catmull] [w=W|/W] [cap=round|butt|square]
 *        [color=HEX|c=COLOR] item...
 *        item := x,y | +dx,dy | x y | +dx +dy | use <path> | rev <path> | useg <path> | revg <path>
 *
 *   fobj <name> [fill=COLOR|f=COLOR] [sw=W|s=W|/W] [cap=...] [seg|catmull] item...
 *        Implicitly CLOSED; emits f (+ per-edge uniform strokes unless sw=0).
 *
 *   group <name>  organizational marker; subsequent fobj faces get
 *                 s <auto> group_id <face> <gnum> records.
 *
 *   Compact / Golf shorthand constructs (single-letter commands):
 *   P x1 y1 x2 y2 ... [fill=COLOR] [sw=W]  -- Polygon (implicit closed face + stroke)
 *   R x y w h [fill=COLOR] [sw=W]          -- Rectangle (x y w h)
 *   C cx cy r [fill=COLOR] [sw=W]          -- Circle (4 anchors + 4 cubic Beziers)
 *   + dx dy                                -- Relative vertex
 *   E va vb [cp...]                        -- Edge (0 cp=seg, 2 cp=quad, 4 cp=cubic)
 *   S eid color w0...                      -- Stroke
 *   F eid1 eid2 ... [color]                -- Face
 *   M x|y vid...                           -- Mirror vertices
 *   D dx dy vid...                         -- Translate vertices
 *   K node time tx ty rot [sx sy skew]     -- Keyframe
 */
#ifndef SMAZKA_XAUTHOR_H
#define SMAZKA_XAUTHOR_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <ctype.h>
#include <strings.h>

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
    size_t have = (size_t)n;
    if (have >= sizeof(tmp)) {
        have = sizeof(tmp) - 1;
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

static int xa_norm_color(const char *s, char *out, size_t cap) {
    static const struct { const char *n; const char *c; } pal[] = {
        { "black", "000000FF" }, { "white", "FFFFFF" }, { "red", "FF0000FF" },
        { "green", "00FF00FF" }, { "blue", "0000FFFF" }, { "yellow", "FFFF00FF" },
        { "cyan", "00FFFFFF" }, { "magenta", "FF00FFFF" }, { "orange", "FFA500FF" },
        { "pink", "FFC0CBFF" }, { "purple", "800080FF" }, { "brown", "A52A2AFF" },
        { "gray", "808080FF" }, { "grey", "808080FF" }, { "silver", "C0C0C0FF" },
        { "maroon", "800000FF" }, { "lime", "00FF00FF" }, { "navy", "000080FF" },
        { "teal", "008080FF" }, { "gold", "FFD700FF" }, { "skin", "FFE0D0FF" },
        { NULL, NULL }
    };
    if (!s || !*s) { snprintf(out, cap, "000000FF"); return 0; }
    const char *p = s;
    if (*p == '#') p++;
    if (strchr(p, ':')) p = strchr(p, ':') + 1;
    if (strchr(p, '=')) p = strchr(p, '=') + 1;
    for (int i = 0; pal[i].n; i++) {
        if (strcasecmp(p, pal[i].n) == 0) {
            snprintf(out, cap, "%s", pal[i].c);
            return 1;
        }
    }
    size_t len = strlen(p);
    unsigned val = 0;
    if (sscanf(p, "%x", &val) == 1) {
        if (len == 3) {
            unsigned r = ((val >> 8) & 0xF) * 0x11;
            unsigned g = ((val >> 4) & 0xF) * 0x11;
            unsigned b = (val & 0xF) * 0x11;
            snprintf(out, cap, "%02X%02X%02X", r, g, b);
            return 1;
        } else if (len == 4) {
            unsigned r = ((val >> 12) & 0xF) * 0x11;
            unsigned g = ((val >> 8) & 0xF) * 0x11;
            unsigned b = ((val >> 4) & 0xF) * 0x11;
            unsigned a = (val & 0xF) * 0x11;
            snprintf(out, cap, "%02X%02X%02X%02X", r, g, b, a);
            return 1;
        } else if (len == 6) {
            snprintf(out, cap, "%06X", val);
            return 1;
        } else if (len == 8) {
            snprintf(out, cap, "%08X", val);
            return 1;
        }
    }
    snprintf(out, cap, "%s", p);
    return 0;
}

static int xa_is_num(const char *t) {           /* whole-token decimal int */
    if (*t == '-' || *t == '+') t++;
    if (!*t) return 0;
    while (*t) { if (!isdigit((unsigned char)*t)) return 0; t++; }
    return 1;
}
static int xa_is_float(const char *t) {
    if (*t == '-' || *t == '+') t++;
    if (!*t) return 0;
    char *end;
    strtod(t, &end);
    return end != t && *end == 0;
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
    xa_put(xa, "v %d %.4f %.4f\n", id, x, y);
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

static int xa_is_hexcol(const char *t) {
    size_t n = strlen(t);
    if (n != 3 && n != 4 && n != 6 && n != 8) return 0;
    for (const char *p = t; *p; p++) if (!isxdigit((unsigned char)*p)) return 0;
    return 1;
}

static int xa_tok(char *ln, char **tv, int cap) {
    int n = 0;
    char *p = ln;
    while (*p && n < cap) {
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (!*p || *p == '#' || *p == '!') break;
        tv[n++] = p;
        while (*p && *p != ' ' && *p != '\t' && *p != '\r') p++;
        if (*p) { *p = 0; p++; }
    }
    return n;
}

static void xa_parse_coord(const char *t, double cur, double *out) {
    int rel = (*t == '+' || *t == '-');
    double val = strtod(t, NULL);
    *out = rel ? cur + val : val;
}

typedef struct {
    int *e; unsigned char *ns; int n, cap;
    int started, v_first, v_last;
    double x0, y0, x1, y1;
} XaChain;

static void xa_pushm(XaChain *c, int eid, int nstk) {
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 32;
        c->e = realloc(c->e, (size_t)c->cap * sizeof(int));
        c->ns = realloc(c->ns, (size_t)c->cap * sizeof(unsigned char));
    }
    c->e[c->n] = eid; c->ns[c->n] = (unsigned char)nstk; c->n++;
}

static void xa_push(XaChain *c, int eid) { xa_pushm(c, eid, 0); }

static void xa_adopt(XaChain *c, int v, double x, double y) {
    c->started = 1; c->v_first = v; c->x0 = x; c->y0 = y;
    c->v_last = v; c->x1 = x; c->y1 = y;
}

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
            if (mode <= 2) {
                if (xa_join(xa, c, sx, sy, sv) != 0) continue;
                for (int k = 0; k < sp->ne; k++) {
                    int e = mode == 1 ? sp->elist[k] : sp->elist[sp->ne - 1 - k];
                    xa_pushm(c, e, 1);
                }
                c->v_last = mode == 1 ? sp->v_last : sp->v_first;
                c->x1 = mode == 1 ? sp->x1 : sp->x0;
                c->y1 = mode == 1 ? sp->y1 : sp->y0;
            } else {
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
                        xa_pushm(c, xa_new_edge(xa, c->v_last, nv, "seg"), 1);
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
                        c->x1 = fx; c->y1 = fy;
                        continue;
                    }
                    int nv = xa_new_vert(xa, fx, fy);
                    xa_pushm(c, xa_new_edge(xa, c->v_last, nv, "seg"), 1);
                    c->v_last = nv; c->x1 = fx; c->y1 = fy;
                }
            }
            continue;
        }

        double x = 0, y = 0;
        int got_pt = 0;
        const char *comma = strchr(t, ',');
        if (comma) {
            char sx[64], sy[64];
            size_t lx = comma - t;
            if (lx < sizeof(sx) && strlen(comma + 1) < sizeof(sy)) {
                memcpy(sx, t, lx); sx[lx] = 0;
                snprintf(sy, sizeof(sy), "%s", comma + 1);
                xa_parse_coord(sx, c->started ? c->x1 : 0, &x);
                xa_parse_coord(sy, c->started ? c->y1 : 0, &y);
                got_pt = 1;
            }
        } else if (xa_is_float(t) && i + 1 < nit && xa_is_float(it[i + 1])) {
            xa_parse_coord(t, c->started ? c->x1 : 0, &x);
            xa_parse_coord(it[i + 1], c->started ? c->y1 : 0, &y);
            i++;
            got_pt = 1;
        }

        if (!got_pt) {
            xa_err(xa, "bad item '%s' (want x,y or x y or use|rev|useg|revg <path>)", t);
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
    char norm_col[64];
    xa_norm_color(color, norm_col, sizeof(norm_col));
    for (int i = 0; i < c->n; i++) {
        if (c->ns && c->ns[i]) continue;
        int id = xa->cnt[XA_S]++;
        xa_mark(xa, XA_S, id);
        xa_put(xa, "s %d %d %s %.4f %.4f cap=%s\n", id, c->e[i],
               norm_col, w, w, cap);
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
    int start_i = 1;
    char auto_name[XA_NAME];
    if (nt >= 2 && xa_is_name(tv[1]) && strcmp(tv[1], "closed") != 0 &&
        strcmp(tv[1], "seg") != 0 && strcmp(tv[1], "catmull") != 0 &&
        strncmp(tv[1], "w=", 2) != 0 && strncmp(tv[1], "sw=", 3) != 0 &&
        strncmp(tv[1], "cap=", 4) != 0 && strncmp(tv[1], "color=", 6) != 0 &&
        strncmp(tv[1], "c=", 2) != 0) {
        snprintf(auto_name, XA_NAME, "%s", tv[1]);
        start_i = 2;
    } else {
        snprintf(auto_name, XA_NAME, "path_%d", xa->cnt[XA_PTH]);
    }

    int closed = 0;
    const char *etype = "seg";
    double w = 0;
    char color[64] = "000000", cap[16] = "round";
    char *pts_tv[2048]; int npts = 0;

    for (int i = start_i; i < nt; i++) {
        const char *t = tv[i];
        if      (strcmp(t, "closed") == 0) closed = 1;
        else if (strcmp(t, "seg") == 0 || strcmp(t, "catmull") == 0) etype = t;
        else if (strncmp(t, "w=", 2) == 0) w = atof(t + 2);
        else if (strncmp(t, "sw=", 3) == 0) w = atof(t + 3);
        else if (t[0] == '/' && xa_is_float(t + 1)) w = atof(t + 1);
        else if (strncmp(t, "cap=", 4) == 0) snprintf(cap, 16, "%s", t + 4);
        else if (strncmp(t, "color=", 6) == 0) snprintf(color, sizeof(color), "%s", t + 6);
        else if (strncmp(t, "c=", 2) == 0) snprintf(color, sizeof(color), "%s", t + 2);
        else {
            pts_tv[npts++] = tv[i];
        }
    }
    XaChain c = {0};
    xa_chain(xa, pts_tv, npts, etype, closed, &c);
    xa_register_path(xa, auto_name, &c);
    if (w > 0) xa_strokes(xa, &c, w, color, cap);
    free(c.ns);
    xa_put(xa, "# path %s (edges %d%s)\n", auto_name, c.n, closed ? ", closed" : "");
}

static void xa_record_fobj(Xa *xa, char **tv, int nt) {
    int start_i = 1;
    char auto_name[XA_NAME];
    if (nt >= 2 && xa_is_name(tv[1]) && strncmp(tv[1], "fill=", 5) != 0 &&
        strncmp(tv[1], "f=", 2) != 0 && strncmp(tv[1], "sw=", 3) != 0 &&
        strncmp(tv[1], "s=", 2) != 0 && strncmp(tv[1], "cap=", 4) != 0 &&
        strcmp(tv[1], "seg") != 0 && strcmp(tv[1], "catmull") != 0) {
        snprintf(auto_name, XA_NAME, "%s", tv[1]);
        start_i = 2;
    } else {
        snprintf(auto_name, XA_NAME, "fobj_%d", xa->cnt[XA_F]);
    }

    double sw = 3.0;
    const char *etype = "seg";
    char fill[64] = "FFFFFF", cap[16] = "round";
    char *pts_tv[2048]; int npts = 0;

    for (int i = start_i; i < nt; i++) {
        const char *t = tv[i];
        if      (strncmp(t, "fill=", 5) == 0) snprintf(fill, sizeof(fill), "%s", t + 5);
        else if (strncmp(t, "f=", 2) == 0) snprintf(fill, sizeof(fill), "%s", t + 2);
        else if (strncmp(t, "sw=", 3) == 0) sw = atof(t + 3);
        else if (strncmp(t, "s=", 2) == 0) sw = atof(t + 2);
        else if (t[0] == '/' && xa_is_float(t + 1)) sw = atof(t + 1);
        else if (strncmp(t, "cap=", 4) == 0) snprintf(cap, 16, "%s", t + 4);
        else if (strcmp(t, "seg") == 0 || strcmp(t, "catmull") == 0) etype = t;
        else {
            pts_tv[npts++] = tv[i];
        }
    }
    XaChain c = {0};
    xa_chain(xa, pts_tv, npts, etype, 1, &c);
    if (c.n >= 3) {
        int fid = xa->cnt[XA_F]++;
        xa_mark(xa, XA_F, fid);
        char norm_fill[64];
        xa_norm_color(fill, norm_fill, sizeof(norm_fill));
        xa_put(xa, "f %d", fid);
        for (int k = 0; k < c.n; k++) xa_put(xa, " %d", c.e[k]);
        xa_put(xa, " %s\n", norm_fill);
        xa_register_path(xa, auto_name, &c);
        if (xa->group >= 0) {
            int gid = xa->cnt[XA_S]++;
            xa_mark(xa, XA_S, gid);
            xa_put(xa, "s %d group_id %d %d\n", gid, fid, xa->group);
        }
        if (sw > 0) { char col[16] = "000000"; xa_strokes(xa, &c, sw, col, cap); }
        free(c.ns);
    } else {
        xa_err(xa, "fobj '%s': fewer than 3 edges, no face emitted", auto_name);
        free(c.e); free(c.ns);
    }
    xa_put(xa, "# fobj %s\n", auto_name);
}

/* ---------------- golf / compact shorthand handlers ---------------- */
static void xa_record_golf(Xa *xa, char **tv, int nt) {
    char cmd = tv[0][0];
    if (cmd == 'P' || cmd == 'R' || cmd == 'C') {
        double vals[128]; int nv = 0;
        char fill[64] = "FFFFFF";
        double sw = 0;
        for (int i = 1; i < nt; i++) {
            const char *t = tv[i];
            if (strncmp(t, "fill=", 5) == 0) snprintf(fill, sizeof(fill), "%s", t + 5);
            else if (strncmp(t, "f=", 2) == 0) snprintf(fill, sizeof(fill), "%s", t + 2);
            else if (strncmp(t, "sw=", 3) == 0) { sw = atof(t + 3); }
            else if (strncmp(t, "s=", 2) == 0) { sw = atof(t + 2); }
            else if (t[0] == '/' && xa_is_float(t + 1)) { sw = atof(t + 1); }
            else if (strchr(t, ',')) {
                char sx[64], sy[64];
                char *comma = strchr(t, ',');
                size_t lx = comma - t;
                if (lx < sizeof(sx) && strlen(comma + 1) < sizeof(sy)) {
                    memcpy(sx, t, lx); sx[lx] = 0;
                    snprintf(sy, sizeof(sy), "%s", comma + 1);
                    vals[nv++] = atof(sx); vals[nv++] = atof(sy);
                }
            } else if (xa_is_float(t)) {
                vals[nv++] = atof(t);
            } else {
                snprintf(fill, sizeof(fill), "%s", t);
            }
        }

        if (cmd == 'R') {
            if (nv < 4) { xa_err(xa, "R needs x y w h"); return; }
            double x = vals[0], y = vals[1], w = vals[2], h = vals[3];
            int v0 = xa_new_vert(xa, x, y);
            int v1 = xa_new_vert(xa, x + w, y);
            int v2 = xa_new_vert(xa, x + w, y + h);
            int v3 = xa_new_vert(xa, x, y + h);
            int e0 = xa_new_edge(xa, v0, v1, "seg");
            int e1 = xa_new_edge(xa, v1, v2, "seg");
            int e2 = xa_new_edge(xa, v2, v3, "seg");
            int e3 = xa_new_edge(xa, v3, v0, "seg");
            int fid = xa->cnt[XA_F]++;
            xa_mark(xa, XA_F, fid);
            char norm_fill[64];
            xa_norm_color(fill, norm_fill, sizeof(norm_fill));
            xa_put(xa, "f %d %d %d %d %d %s\n", fid, e0, e1, e2, e3, norm_fill);
            if (sw > 0) {
                XaChain c = {0};
                xa_push(&c, e0); xa_push(&c, e1); xa_push(&c, e2); xa_push(&c, e3);
                xa_strokes(xa, &c, sw, "000000", "round");
                free(c.e); free(c.ns);
            }
        } else if (cmd == 'C') {
            if (nv < 3) { xa_err(xa, "C needs cx cy r"); return; }
            double cx = vals[0], cy = vals[1], r = vals[2];
            const double k = 0.5522847498307936;
            int a0 = xa_new_vert(xa, cx + r, cy);
            int a1 = xa_new_vert(xa, cx, cy + r);
            int a2 = xa_new_vert(xa, cx - r, cy);
            int a3 = xa_new_vert(xa, cx, cy - r);
            double cps[4][4] = {
                { cx + r, cy + k * r, cx + k * r, cy + r },
                { cx - k * r, cy + r, cx - r, cy + k * r },
                { cx - r, cy - k * r, cx - k * r, cy - r },
                { cx + k * r, cy - r, cx + r, cy - k * r }
            };
            int fe[4];
            for (int i = 0; i < 4; i++) {
                int a_curr = (i == 0) ? a0 : (i == 1) ? a1 : (i == 2) ? a2 : a3;
                int a_next = (i == 0) ? a1 : (i == 1) ? a2 : (i == 2) ? a3 : a0;
                int eid = xa->cnt[XA_E]++;
                xa_mark(xa, XA_E, eid);
                xa_put(xa, "e %d %d %d type=cubic %.4f %.4f %.4f %.4f\n",
                       eid, a_curr, a_next, cps[i][0], cps[i][1], cps[i][2], cps[i][3]);
                fe[i] = eid;
            }
            int fid = xa->cnt[XA_F]++;
            xa_mark(xa, XA_F, fid);
            char norm_fill[64];
            xa_norm_color(fill, norm_fill, sizeof(norm_fill));
            xa_put(xa, "f %d %d %d %d %d %s\n", fid, fe[0], fe[1], fe[2], fe[3], norm_fill);
            if (sw > 0) {
                XaChain c = {0};
                for (int i = 0; i < 4; i++) xa_push(&c, fe[i]);
                xa_strokes(xa, &c, sw, "000000", "round");
                free(c.e); free(c.ns);
            }
        } else if (cmd == 'P') {
            if (nv < 6 || nv % 2 != 0) { xa_err(xa, "P needs x1 y1 x2 y2 ..."); return; }
            int m = nv / 2;
            int first_v = xa->cnt[XA_V];
            for (int i = 0; i < m; i++) xa_new_vert(xa, vals[2 * i], vals[2 * i + 1]);
            XaChain c = {0};
            for (int i = 0; i < m; i++) {
                int v0 = first_v + i;
                int v1 = first_v + ((i + 1) % m);
                xa_push(&c, xa_new_edge(xa, v0, v1, "seg"));
            }
            int fid = xa->cnt[XA_F]++;
            xa_mark(xa, XA_F, fid);
            char norm_fill[64];
            xa_norm_color(fill, norm_fill, sizeof(norm_fill));
            xa_put(xa, "f %d", fid);
            for (int i = 0; i < m; i++) xa_put(xa, " %d", c.e[i]);
            xa_put(xa, " %s\n", norm_fill);
            if (sw > 0) xa_strokes(xa, &c, sw, "000000", "round");
            free(c.e); free(c.ns);
        }
    } else if (cmd == '+') {
        double dx = 0, dy = 0;
        if (nt >= 2 && strchr(tv[1], ',')) {
            sscanf(tv[1], "%lf,%lf", &dx, &dy);
        } else if (nt >= 3) {
            dx = atof(tv[1]); dy = atof(tv[2]);
        }
        int last_v = xa->cnt[XA_V] - 1;
        double lx = (last_v >= 0) ? xa->vx[last_v] : 0;
        double ly = (last_v >= 0) ? xa->vy[last_v] : 0;
        xa_new_vert(xa, lx + dx, ly + dy);
    } else if (cmd == 'E') {
        if (nt < 3) { xa_err(xa, "E needs va vb"); return; }
        int va = xa_id(xa, XA_V, tv[1], 0);
        int vb = xa_id(xa, XA_V, tv[2], 0);
        double cps[8]; int nc = 0;
        for (int i = 3; i < nt && nc < 8; i++) {
            if (xa_is_float(tv[i])) cps[nc++] = atof(tv[i]);
        }
        int eid = xa->cnt[XA_E]++;
        xa_mark(xa, XA_E, eid);
        if (nc == 2) xa_put(xa, "e %d %d %d type=quad %.4f %.4f\n", eid, va, vb, cps[0], cps[1]);
        else if (nc >= 4) xa_put(xa, "e %d %d %d type=cubic %.4f %.4f %.4f %.4f\n", eid, va, vb, cps[0], cps[1], cps[2], cps[3]);
        else xa_put(xa, "e %d %d %d\n", eid, va, vb);
    } else if (cmd == 'S') {
        if (nt < 3) { xa_err(xa, "S needs eid color"); return; }
        int eid = xa_id(xa, XA_E, tv[1], 0);
        char norm_col[64];
        xa_norm_color(tv[2], norm_col, sizeof(norm_col));
        int sid = xa->cnt[XA_S]++;
        xa_mark(xa, XA_S, sid);
        xa_put(xa, "s %d %d %s", sid, eid, norm_col);
        for (int i = 3; i < nt; i++) {
            if (xa_is_float(tv[i])) xa_put(xa, " %.4f", atof(tv[i]));
            else if (strncmp(tv[i], "cap=", 4) == 0) xa_put(xa, " %s", tv[i]);
        }
        xa_put(xa, "\n");
    } else if (cmd == 'F') {
        if (nt < 2) { xa_err(xa, "F needs edge ids"); return; }
        int fid = xa->cnt[XA_F]++;
        xa_mark(xa, XA_F, fid);
        xa_put(xa, "f %d", fid);
        for (int i = 1; i < nt; i++) {
            if (xa_is_name(tv[i]) || xa_is_num(tv[i])) {
                if (i == nt - 1 && (xa_is_hexcol(tv[i]) || strchr(tv[i], '#') || xa_is_name(tv[i]))) {
                    char norm_fill[64];
                    if (xa_norm_color(tv[i], norm_fill, sizeof(norm_fill))) {
                        xa_put(xa, " %s", norm_fill);
                        continue;
                    }
                }
                int eid = xa_id(xa, XA_E, tv[i], 0);
                if (eid >= 0) xa_put(xa, " %d", eid);
            }
        }
        xa_put(xa, "\n");
    } else if (cmd == 'M' || cmd == 'D') {
        char axis = 'x'; double dx = 0, dy = 0;
        int i = 1;
        if (cmd == 'M') {
            if (i < nt) { axis = tv[i][0]; i++; }
        } else {
            if (i + 1 < nt) { dx = atof(tv[i]); dy = atof(tv[i+1]); i += 2; }
        }
        for (; i < nt; i++) {
            int vid = xa_id(xa, XA_V, tv[i], 0);
            if (vid >= 0 && vid < xa->cnt[XA_V]) {
                double nx = xa->vx[vid], ny = xa->vy[vid];
                if (cmd == 'M') {
                    if (axis == 'x') ny = -ny;
                    else nx = -nx;
                } else { nx += dx; ny += dy; }
                xa_new_vert(xa, nx, ny);
            }
        }
    } else if (cmd == 'K') {
        if (nt < 6) { xa_err(xa, "K needs node time tx ty rot"); return; }
        int kfid = xa->cnt[XA_S]++;
        int node = atoi(tv[1]);
        double time = atof(tv[2]), tx = atof(tv[3]), ty = atof(tv[4]), rot = atof(tv[5]);
        xa_put(xa, "k %d %d %.4f %.4f %.4f %.4f", kfid, node, time, tx, ty, rot);
        if (nt >= 7) xa_put(xa, " %.4f", atof(tv[6]));
        if (nt >= 8) xa_put(xa, " %.4f", atof(tv[7]));
        if (nt >= 9) xa_put(xa, " %.4f", atof(tv[8]));
        xa_put(xa, "\n");
    }
}

static int xa_is_kw(const char *t) {
    static const char *KWS =
        "|parent|group_id|min_dist|bbox_clamp|linear_eq|linear_le|linear_ge|"
        "collision_free|fair_blend|min_stretch|state_machine|edge_connects|"
        "diffusion|solid_fill|corner|smooth|symmetric|auto|seg|quad|cubic|"
        "rational|catmull|";
    if (strchr(t, '=')) return 1;
    char key[128]; snprintf(key, sizeof(key), "|%s|", t);
    return strstr(KWS, key) != NULL;
}

static void xa_rewrite_plain(Xa *xa, const char *line0) {
    char buf[65536]; snprintf(buf, sizeof(buf), "%s", line0);
    char *tv[2048];
    static char pool[2048][128]; static int pi;
    int nt = xa_tok(buf, tv, 2048);
    if (nt == 0) { xa_put(xa, "\n"); return; }
    char cmd = tv[0][0];
    if (cmd == 'v' && nt >= 3) {
        if (nt >= 4) {
            if (xa_is_name(tv[1])) {
                int id = xa_id(xa, XA_V, tv[1], 1);
                double x = strtod(tv[2], NULL), y = strtod(tv[3], NULL);
                if (id >= 0 && id < XA_MAX_V) { xa->vx[id] = x; xa->vy[id] = y; }
                snprintf(pool[pi & 2047], sizeof(pool[0]), "%d", id); tv[1] = pool[pi++ & 2047];
            } else if (xa_is_num(tv[1])) {
                int id = atoi(tv[1]);
                if (xa_claim(xa, XA_V, id)) xa_err(xa, "id %d redefined (collides with issued v id)", id);
                xa_mark(xa, XA_V, id);
                double x = strtod(tv[2], NULL), y = strtod(tv[3], NULL);
                if (id >= 0 && id < XA_MAX_V) { xa->vx[id] = x; xa->vy[id] = y; }
                if (id >= xa->cnt[XA_V]) xa->cnt[XA_V] = id + 1;
            }
        } else {
            double x = strtod(tv[1], NULL), y = strtod(tv[2], NULL);
            xa_new_vert(xa, x, y);
            return;
        }
    } else if (cmd == 'e' && nt >= 4) {
        int eid = -1;
        if (xa_is_name(tv[1])) {
            eid = xa_id(xa, XA_E, tv[1], 1);
            snprintf(pool[pi & 2047], sizeof(pool[0]), "%d", eid); tv[1] = pool[pi++ & 2047];
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
                    snprintf(pool[pi & 2047], sizeof(pool[0]), "%d", id);
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
            snprintf(pool[pi & 2047], sizeof(pool[0]), "%d", id); tv[1] = pool[pi++ & 2047];
        } else if (xa_is_num(tv[1])) {
            int id = atoi(tv[1]);
            if (xa_claim(xa, XA_F, id)) xa_err(xa, "id %d redefined (collides with issued f id)", id);
            xa_mark(xa, XA_F, id);
            if (id >= xa->cnt[XA_F]) xa->cnt[XA_F] = id + 1;
        }
        for (int i = 2; i < nt; i++) {
            if (!xa_is_name(tv[i]) || xa_is_kw(tv[i])) continue;
            if (i == nt - 1 && xa_is_hexcol(tv[i])) {
                char norm_fill[64];
                xa_norm_color(tv[i], norm_fill, sizeof(norm_fill));
                snprintf(pool[pi & 2047], sizeof(pool[0]), "%s", norm_fill);
                tv[i] = pool[pi++ & 2047];
                continue;
            }
            int id = xa_id(xa, XA_E, tv[i], 0);
            if (id >= 0) {
                snprintf(pool[pi & 2047], sizeof(pool[0]), "%d", id);
                tv[i] = pool[pi++ & 2047];
            }
        }
    } else if (cmd == 's' && nt >= 3 && !xa_is_kw(tv[2])) {
        if (xa_is_name(tv[1])) {
            int id = xa_id(xa, XA_S, tv[1], 1);
            snprintf(pool[pi & 2047], sizeof(pool[0]), "%d", id); tv[1] = pool[pi++ & 2047];
        } else if (xa_is_num(tv[1])) {
            int id = atoi(tv[1]);
            if (xa_claim(xa, XA_S, id)) xa_err(xa, "id %d redefined (collides with issued s id)", id);
            xa_mark(xa, XA_S, id);
            if (id >= xa->cnt[XA_S]) xa->cnt[XA_S] = id + 1;
        }
        if (xa_is_name(tv[2])) {
            int id = xa_id(xa, XA_E, tv[2], 0);
            if (id >= 0) {
                snprintf(pool[pi & 2047], sizeof(pool[0]), "%d", id);
                tv[2] = pool[pi++ & 2047];
            }
        }
        if (nt >= 4) {
            char norm_col[64];
            xa_norm_color(tv[3], norm_col, sizeof(norm_col));
            snprintf(pool[pi & 2047], sizeof(pool[0]), "%s", norm_col);
            tv[3] = pool[pi++ & 2047];
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
    xa_put(&xa, "# xpanded by xauthor.h (authoring skin v1.7)\n");
    const char *p = src;
    while (p && *p) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len > 0) {
            char *line = strndup(p, len);
            xa.lineno++;
            char *s = line; while (*s == ' ' || *s == '\t') s++;
            if (!*s || *s == '#' || *s == '!') {
                /* skip comment */
            } else if (strncmp(s, "path ", 5) == 0 || strncmp(s, "path\t", 5) == 0) {
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
            } else if (strncmp(s, "group ", 6) == 0 || strncmp(s, "group\t", 6) == 0 ||
                       strncmp(s, "g ", 2) == 0 || strncmp(s, "g\t", 2) == 0) {
                char *copy = strdup(s);
                char *tv[8]; xa_tok(copy, tv, 8);
                if (tv[1]) {
                    XaSym *g = xa_find(&xa, XA_GRP, tv[1]);
                    xa.group = g ? g->id : (xa_define(&xa, XA_GRP, tv[1], 0)->id = xa.group_cnt++);
                    xa_put(&xa, "# group %s\n", tv[1]);
                }
                free(copy);
            } else if (strchr("PRC+ESFMDK", s[0]) && (s[1] == ' ' || s[1] == '\t' || s[1] == 0)) {
                char *copy = strdup(s);
                char *tv[2048]; int nt = xa_tok(copy, tv, 2048);
                xa_record_golf(&xa, tv, nt);
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
