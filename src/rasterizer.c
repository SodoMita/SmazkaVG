/*
 * SmazkaVG v1.2 Rasterizer — zero external dependencies
 *
 * Build:  cc -O2 -o smazka-raster src/rasterizer.c -lm
 * Usage:  ./smazka-raster <input.smazka> [width] [height]
 *
 * Outputs: <input>.bmp, <input>.svg, <input>.txt (ascii)
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>

typedef int32_t q16_t;
static inline double q16_to_d(q16_t v)  { return (double)v / 65536.0; }
static inline q16_t  d_to_q16(double x) {
    double s = x * 65536.0;
    if (s >  2147483647.0) return  2147483647;
    if (s < -2147483648.0) return (int32_t)0x80000000;
    return (q16_t)(s >= 0 ? s + 0.5 : s - 0.5);
}

#define MAX_VERTS   4096
#define MAX_EDGES   4096
#define MAX_FACES   4096
#define MAX_STROKES 4096
#define MAX_NODES   1024
#define MAX_LINE    2048
#define MAX_CP      32
#define MAX_WIDTH   64

typedef struct { double x, y; } Vec2;
typedef struct { uint8_t r, g, b, a; } Color;

typedef enum { EDGE_SEG, EDGE_CUBIC, EDGE_BSPLINE, EDGE_SPIRO } EdgeType;

static int      n_verts;
static Vec2     verts[MAX_VERTS];

static int      n_edges;
static struct {
    int v0, v1;
    EdgeType type;
    Vec2 cp[MAX_CP];
    int n_cp;
} edges[MAX_EDGES];

static int      n_faces;
static struct {
    int edge_ids[64]; int n_edges;
    int fill_color;
} faces[MAX_FACES];

static int      n_strokes;
static struct {
    int edge_id;
    Color color;
    double width[MAX_WIDTH];
    int n_width;
} strokes[MAX_STROKES];

static int      n_nodes;
static struct { double tx,ty,rot,sx,sy,skew; int content_prim; } nodes[MAX_NODES];

/* Split namespace constraints */
static int n_struct;
static struct { int type, a, b; } s_struct[256];
static int n_assert;
static struct { int type, a, b, c; } s_assert[256];
static int n_constr;
static struct { int type, a, b; double val; } s_constr[256];
static int n_paint;
static struct { int type, target; Color c1, c2; } s_paint[128];

/* ── Color parsing ─── */

static Color parse_color(const char *s) {
    Color c = {0, 0, 0, 255};
    unsigned int v = 0;
    int len = (int)strlen(s);
    if (len >= 8) {
        sscanf(s, "%x", &v);
        c.r = (v>>24)&0xFF; c.g = (v>>16)&0xFF;
        c.b = (v>>8)&0xFF;  c.a = (v)&0xFF;
    } else if (len >= 6) {
        sscanf(s, "%x", &v);
        c.r = (v>>16)&0xFF; c.g = (v>>8)&0xFF;
        c.b = v&0xFF; c.a = 255;
    }
    return c;
}

/* ─── Edge parser ─── */

static void parse_edge_extras(const char *tok, int eid) {
    edges[eid].type = EDGE_SEG;
    edges[eid].n_cp = 0;

    while (*tok) {
        while (*tok == ' ') tok++;
        if (!*tok) break;

        if (strncmp(tok, "type=", 5) == 0) {
            tok += 5;
            if (strncmp(tok, "cubic", 5) == 0)      { edges[eid].type = EDGE_CUBIC;   tok += 5; }
            else if (strncmp(tok, "bspline", 7) == 0) { edges[eid].type = EDGE_BSPLINE; tok += 7; }
            else if (strncmp(tok, "spiro", 5) == 0)   { edges[eid].type = EDGE_SPIRO;   tok += 5; }
            else { while (*tok && *tok != ' ') tok++; continue; }
            continue;
        }

        /* Read numbers as interleaved x,y pairs for control points */
        double val;
        int nread = 0;
        if (sscanf(tok, "%lf%n", &val, &nread) == 1 && nread > 0) {
            int slot = edges[eid].n_cp;
            if (slot % 2 == 0 && slot/2 < MAX_CP) {
                edges[eid].cp[slot/2].x = val;
            } else if (slot % 2 == 1 && slot/2 < MAX_CP) {
                edges[eid].cp[slot/2].y = val;
            }
            edges[eid].n_cp++;
            tok += nread;
        } else {
            break;
        }
    }
}

/* ─── Main parser ─── */

static int parse_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *cr = strchr(line, '\r'); if (cr) *cr = 0;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == 0 || *p == '#') continue;

        char cmd;
        if (sscanf(p, "%c", &cmd) != 1) continue;

        switch (cmd) {
        case 'v': {
            int id; double x, y;
            if (sscanf(p, "v %d %lf %lf", &id, &x, &y) >= 3) {
                if (id >= n_verts) n_verts = id + 1;
                verts[id].x = x; verts[id].y = y;
            }
            break;
        }
        case 'e': {
            int id, v0, v1;
            if (sscanf(p, "e %d %d %d", &id, &v0, &v1) < 3) break;
            if (id >= n_edges) n_edges = id + 1;
            edges[id].v0 = v0; edges[id].v1 = v1;
            /* Skip past "e <id> <v0> <v1>" */
            char *tok = p;
            for (int i = 0; i < 3; i++) {
                while (*tok && *tok != ' ') tok++;
                while (*tok == ' ') tok++;
            }
            if (*tok) parse_edge_extras(tok, id);
            break;
        }
        case 'f': {
            int id;
            if (sscanf(p, "f %d", &id) != 1) break;
            if (id >= n_faces) n_faces = id + 1;
            char *tok = p;
            while (*tok && *tok != ' ') tok++;
            while (*tok == ' ') tok++;
            while (*tok && *tok != ' ') tok++;
            while (*tok == ' ') tok++;
            int ne = 0;
            while (*tok && ne < 64) {
                int eid;
                if (sscanf(tok, "%d", &eid) != 1) break;
                faces[id].edge_ids[ne++] = eid;
                while (*tok && *tok != ' ') tok++;
                while (*tok == ' ') tok++;
            }
            faces[id].n_edges = ne;
            if (*tok && (*tok == '0' || (*tok >= '1' && *tok <= '9') || *tok == '#'))
                faces[id].fill_color = (int)strtol(tok, NULL, 16);
            break;
        }
        case 's': {
            int id; char ctype[64];
            if (sscanf(p, "s %d %63s", &id, &ctype) < 2) break;
            if (strcmp(ctype, "parent") == 0) {
                int a, b;
                if (sscanf(p, "s %*d parent %d %d", &a, &b) == 2) {
                    s_struct[n_struct].type = 0; s_struct[n_struct].a = a; s_struct[n_struct].b = b; n_struct++;
                }
            } else if (strcmp(ctype, "group_id") == 0) {
                int a, b;
                if (sscanf(p, "s %*d group_id %d %d", &a, &b) == 2) {
                    s_struct[n_struct].type = 1; s_struct[n_struct].a = a; s_struct[n_struct].b = b; n_struct++;
                }
            } else {
                /* Stroke: s <id> <edge> <color> <w0> <w1> ... */
                int eid; char col[32];
                if (sscanf(p, "s %d %d %31s", &id, &eid, col) >= 3) {
                    if (id >= n_strokes) n_strokes = id + 1;
                    strokes[id].edge_id = eid;
                    strokes[id].color = parse_color(col);
                    strokes[id].n_width = 0;
                    /* Skip past "s <id> <edge> <color>" to reach widths */
                    char *tok = p;
                    for (int i = 0; i < 4; i++) { while (*tok && *tok != ' ') tok++; while (*tok == ' ') tok++; }
                    while (*tok && *tok != '#') {
                        double w; int nread = 0;
                        if (sscanf(tok, "%lf%n", &w, &nread) == 1 && nread > 0) {
                            if (strokes[id].n_width < MAX_WIDTH)
                                strokes[id].width[strokes[id].n_width++] = w;
                            tok += nread;
                        } else break;
                        while (*tok == ' ') tok++;
                    }
                }
            }
            break;
        }
        case 'a': {
            int id; char atype[64];
            if (sscanf(p, "a %d %63s", &id, &atype) < 2) break;
            if (strcmp(atype, "edge_connects") == 0) {
                int a, b, c;
                if (sscanf(p, "a %*d edge_connects %d %d %d", &a, &b, &c) == 3) {
                    s_assert[n_assert].type=0; s_assert[n_assert].a=a; s_assert[n_assert].b=b; s_assert[n_assert].c=c; n_assert++;
                }
            }
            break;
        }
        case 'c': {
            int id; char ctype[64];
            if (sscanf(p, "c %d %63s", &id, &ctype) < 2) break;
            if (strcmp(ctype, "bbox_clamp") == 0) {
                int prim; double x0,y0,x1,y1;
                if (sscanf(p, "c %*d bbox_clamp %d %lf %lf %lf %lf", &prim,&x0,&y0,&x1,&y1) == 5) {
                    s_constr[n_constr].type=0; s_constr[n_constr].a=prim; s_constr[n_constr].val=x0; n_constr++;
                }
            } else if (strcmp(ctype, "min_dist") == 0) {
                int a, b; double d;
                if (sscanf(p, "c %*d min_dist %d %d %lf", &a, &b, &d) == 3) {
                    s_constr[n_constr].type=1; s_constr[n_constr].a=a; s_constr[n_constr].b=b; s_constr[n_constr].val=d; n_constr++;
                }
            }
            break;
        }
        case 'p': {
            int id; char ptype[64];
            if (sscanf(p, "p %d %63s", &id, &ptype) < 2) break;
            if (strcmp(ptype, "diffusion") == 0) {
                int eid; char L[4],R[4],lc[32],rc[32];
                if (sscanf(p, "p %*d diffusion %d %3s %31s %3s %31s", &eid,L,lc,R,rc) >= 4) {
                    s_paint[n_paint].type=0; s_paint[n_paint].target=eid;
                    s_paint[n_paint].c1=parse_color(lc); s_paint[n_paint].c2=parse_color(rc); n_paint++;
                }
            } else if (strcmp(ptype, "solid_fill") == 0) {
                int fid; char col[32];
                if (sscanf(p, "p %*d solid_fill %d %31s", &fid, col) == 2) {
                    s_paint[n_paint].type=1; s_paint[n_paint].target=fid;
                    s_paint[n_paint].c1=parse_color(col); n_paint++;
                }
            }
            break;
        }
        case 'n': {
            int id;
            if (sscanf(p, "n %d", &id) != 1) break;
            if (id >= n_nodes) n_nodes = id + 1;
            nodes[id].tx = 0; nodes[id].ty = 0;
            nodes[id].rot = 0;
            nodes[id].sx = 1; nodes[id].sy = 1;
            nodes[id].skew = 0;
            nodes[id].content_prim = -1;
            char *tok = p;
            while (*tok && *tok != ' ') tok++;
            while (*tok == ' ') tok++;
            while (*tok && *tok != ' ') tok++;
            if (*tok == ' ') tok++;
            if (*tok && strchr(tok, '=')) {
                /* Labeled */
                while (*tok) {
                    while (*tok == ' ') tok++; if (!*tok) break;
                    double v; int nread;
                    if (strncmp(tok, "tx=", 3) == 0 && sscanf(tok+3, "%lf%n", &v, &nread) == 1) { nodes[id].tx = v; tok += 3 + nread; }
                    else if (strncmp(tok, "ty=", 3) == 0 && sscanf(tok+3, "%lf%n", &v, &nread) == 1) { nodes[id].ty = v; tok += 3 + nread; }
                    else if (strncmp(tok, "rot=", 4) == 0 && sscanf(tok+4, "%lf%n", &v, &nread) == 1) { nodes[id].rot = v; tok += 4 + nread; }
                    else if (strncmp(tok, "sx=", 3) == 0 && sscanf(tok+3, "%lf%n", &v, &nread) == 1) { nodes[id].sx = v; tok += 3 + nread; }
                    else if (strncmp(tok, "sy=", 3) == 0 && sscanf(tok+3, "%lf%n", &v, &nread) == 1) { nodes[id].sy = v; tok += 3 + nread; }
                    else if (strncmp(tok, "skew=", 5) == 0 && sscanf(tok+5, "%lf%n", &v, &nread) == 1) { nodes[id].skew = v; tok += 5 + nread; }
                    else if (strncmp(tok, "content=", 8) == 0 && sscanf(tok+8, "%d%n", &nodes[id].content_prim, &nread) == 1) { tok += 8 + nread; }
                    else { while (*tok && *tok != ' ') tok++; }
                }
            } else {
                sscanf(tok, "%lf %lf %lf %lf %lf %lf %d",
                    &nodes[id].tx, &nodes[id].ty, &nodes[id].rot,
                    &nodes[id].sx, &nodes[id].sy, &nodes[id].skew,
                    &nodes[id].content_prim);
            }
            break;
        }
        default: break;
        }
    }
    fclose(f);
    return 0;
}

/* ─── Curve math ─── */

static Vec2 bezier_cubic(Vec2 p0, Vec2 p1, Vec2 p2, Vec2 p3, double t) {
    double u = 1.0 - t;
    double u2 = u*u, u3 = u2*u, t2 = t*t, t3 = t2*t;
    return (Vec2){
        u3*p0.x + 3*u2*t*p1.x + 3*u*t2*p2.x + t3*p3.x,
        u3*p0.y + 3*u2*t*p1.y + 3*u*t2*p2.y + t3*p3.y
    };
}

static Vec2 edge_point(int eid, double t) {
    if (eid >= n_edges) return (Vec2){0,0};
    Vec2 p0 = verts[edges[eid].v0], p3 = verts[edges[eid].v1];
    if (edges[eid].type == EDGE_CUBIC && edges[eid].n_cp >= 2)
        return bezier_cubic(p0, edges[eid].cp[0], edges[eid].cp[1], p3, t);
    return (Vec2){ p0.x + t*(p3.x-p0.x), p0.y + t*(p3.y-p0.y) };
}

/* ─── Framebuffer ─── */

static int fb_w, fb_h;
static uint8_t *fb;

static void fb_alloc(int w, int h) {
    fb_w = w; fb_h = h;
    fb = (uint8_t *)malloc(w * h * 3);
    memset(fb, 255, w * h * 3);
}
static void fb_free(void) { free(fb); }

static inline void fb_set(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if ((unsigned)x < (unsigned)fb_w && (unsigned)y < (unsigned)fb_h) {
        int off = (y * fb_w + x) * 3;
        fb[off] = r; fb[off+1] = g; fb[off+2] = b;
    }
}
static inline void fb_blend(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if ((unsigned)x >= (unsigned)fb_w || (unsigned)y >= (unsigned)fb_h) return;
    if (a == 0) return;
    int off = (y * fb_w + x) * 3;
    if (a == 255) { fb[off]=r; fb[off+1]=g; fb[off+2]=b; return; }
    uint16_t ia = 255 - a;
    fb[off]   = (uint8_t)((r*a + fb[off]*ia)/255);
    fb[off+1] = (uint8_t)((g*a + fb[off+1]*ia)/255);
    fb[off+2] = (uint8_t)((b*a + fb[off+2]*ia)/255);
}

/* ─── Rasterization ─── */

static void draw_triangle(Vec2 v0, Vec2 v1, Vec2 v2, Color col) {
    double minx = fmin(fmin(v0.x,v1.x),v2.x), maxx = fmax(fmax(v0.x,v1.x),v2.x);
    double miny = fmin(fmin(v0.y,v1.y),v2.y), maxy = fmax(fmax(v0.y,v1.y),v2.y);
    int x0 = (int)floor(minx), x1 = (int)ceil(maxx);
    int y0 = (int)floor(miny), y1 = (int)ceil(maxy);
    if (x0 < 0) x0 = 0; if (x1 >= fb_w) x1 = fb_w-1;
    if (y0 < 0) y0 = 0; if (y1 >= fb_h) y1 = fb_h-1;

    double area = (v1.x-v0.x)*(v2.y-v0.y) - (v1.y-v0.y)*(v2.x-v0.x);
    if (fabs(area) < 1e-10) return;
    double ia = 1.0 / area;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            double px = x+0.5, py = y+0.5;
            double w0 = ((v1.x-px)*(v2.y-py) - (v1.y-py)*(v2.x-px)) * ia;
            double w1 = ((v2.x-px)*(v0.y-py) - (v2.y-py)*(v0.x-px)) * ia;
            double w2 = 1.0 - w0 - w1;
            if (w0 >= 0 && w1 >= 0 && w2 >= 0)
                fb_blend(x, y, col.r, col.g, col.b, col.a);
        }
    }
}

static void draw_thick_line(Vec2 a, Vec2 b, double width, Color col) {
    double dx = b.x-a.x, dy = b.y-a.y;
    double len = sqrt(dx*dx + dy*dy);
    if (len < 1e-6) return;
    double hw = width * 0.5;
    int x0 = (int)floor(fmin(a.x,b.x) - hw - 1);
    int x1 = (int)ceil( fmax(a.x,b.x) + hw + 1);
    int y0 = (int)floor(fmin(a.y,b.y) - hw - 1);
    int y1 = (int)ceil( fmax(a.y,b.y) + hw + 1);
    if (x0 < 0) x0 = 0; if (x1 >= fb_w) x1 = fb_w-1;
    if (y0 < 0) y0 = 0; if (y1 >= fb_h) y1 = fb_h-1;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            double px = x+0.5 - a.x, py = y+0.5 - a.y;
            double t = (px*dx + py*dy) / (len*len);
            if (t < 0) t = 0; if (t > 1) t = 1;
            double cx = a.x + t*dx, cy = a.y + t*dy;
            double dist = sqrt((x+0.5-cx)*(x+0.5-cx) + (y+0.5-cy)*(y+0.5-cy));
            if (dist <= hw + 0.5) {
                double alpha = (dist <= hw - 0.5) ? 1.0 : 1.0 - (dist - hw + 0.5);
                if (alpha < 0) alpha = 0;
                fb_blend(x, y, col.r, col.g, col.b, (uint8_t)(alpha * col.a));
            }
        }
    }
}

static void draw_variable_stroke(int eid, double *widths, int nw, Color col) {
    if (eid >= n_edges || nw < 2) return;
    int steps = 48;
    for (int i = 0; i < steps; i++) {
        double t0 = (double)i / steps;
        double t1 = (double)(i+1) / steps;
        Vec2 p0 = edge_point(eid, t0);
        Vec2 p1 = edge_point(eid, t1);
        double wi = t0 * (nw - 1);
        int w0i = (int)wi, w1i = w0i + 1;
        if (w1i >= nw) w1i = nw - 1;
        double wf = wi - w0i;
        double w = widths[w0i] * (1-wf) + widths[w1i] * wf;
        draw_thick_line(p0, p1, w, col);
    }
}

static void draw_diffusion(int eid, Color lc, Color rc, double radius) {
    if (eid >= n_edges) return;
    Vec2 a = verts[edges[eid].v0], b = verts[edges[eid].v1];
    double dx = b.x-a.x, dy = b.y-a.y;
    double len = sqrt(dx*dx+dy*dy);
    if (len < 1e-6) return;
    double nx = -dy/len, ny = dx/len;
    int x0 = (int)floor(fmin(a.x,b.x)-radius-1), x1 = (int)ceil(fmax(a.x,b.x)+radius+1);
    int y0 = (int)floor(fmin(a.y,b.y)-radius-1), y1 = (int)ceil(fmax(a.y,b.y)+radius+1);
    if (x0 < 0) x0 = 0; if (x1 >= fb_w) x1 = fb_w-1;
    if (y0 < 0) y0 = 0; if (y1 >= fb_h) y1 = fb_h-1;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            double px = x+0.5, py = y+0.5;
            double side = (px-a.x)*ny - (py-a.y)*nx;
            double t = ((px-a.x)*dx + (py-a.y)*dy) / (len*len);
            if (t < -0.05 || t > 1.05) continue;
            double d = fabs(side);
            if (d > radius) continue;
            double blend = 0.5 + side / (2.0*radius);
            if (blend < 0) blend = 0; if (blend > 1) blend = 1;
            double falloff = 1.0 - d/radius;
            uint8_t r = (uint8_t)(lc.r*(1-blend) + rc.r*blend);
            uint8_t g = (uint8_t)(lc.g*(1-blend) + rc.g*blend);
            uint8_t b = (uint8_t)(lc.b*(1-blend) + rc.b*blend);
            fb_blend(x, y, r, g, b, (uint8_t)(falloff * 180));
        }
    }
}

/* ─── View & Render ─── */

static void compute_view(double *ox, double *oy, double *scale) {
    if (n_verts == 0) { *ox = *oy = 0; *scale = 1; return; }
    double mn_x=1e30, mx_x=-1e30, mn_y=1e30, mx_y=-1e30;
    for (int i = 0; i < n_verts; i++) {
        if (verts[i].x < mn_x) mn_x = verts[i].x;
        if (verts[i].x > mx_x) mx_x = verts[i].x;
        if (verts[i].y < mn_y) mn_y = verts[i].y;
        if (verts[i].y > mx_y) mx_y = verts[i].y;
    }
    double sw = mx_x - mn_x; if (sw < 1) sw = 1;
    double sh = mx_y - mn_y; if (sh < 1) sh = 1;
    double margin = 50.0;
    double sx = (fb_w - 2*margin) / sw, sy = (fb_h - 2*margin) / sh;
    *scale = fmin(sx, sy);
    *ox = margin - mn_x*(*scale) + ((fb_w - 2*margin) - sw*(*scale)) * 0.5;
    *oy = margin - mn_y*(*scale) + ((fb_h - 2*margin) - sh*(*scale)) * 0.5;
}

static Vec2 s2s(Vec2 p, double ox, double oy, double s) {
    return (Vec2){ p.x*s + ox, p.y*s + oy };
}

static void render_scene(void) {
    double ox, oy, sc;
    compute_view(&ox, &oy, &sc);
    Color col;

    /* 1. Solid fills (paint section) */
    for (int pi = 0; pi < n_paint; pi++) {
        if (s_paint[pi].type != 1) continue;
        int fid = s_paint[pi].target;
        if (fid >= n_faces || faces[fid].n_edges < 3) continue;
        col = s_paint[pi].c1;
        int fv = edges[faces[fid].edge_ids[0]].v0;
        Vec2 sv0 = s2s(verts[fv], ox, oy, sc);
        for (int ei = 1; ei + 1 < faces[fid].n_edges; ei++) {
            int v1 = edges[faces[fid].edge_ids[ei]].v0 == fv ?
                     edges[faces[fid].edge_ids[ei]].v1 : edges[faces[fid].edge_ids[ei]].v0;
            int v2 = edges[faces[fid].edge_ids[ei+1]].v0 == fv ?
                     edges[faces[fid].edge_ids[ei+1]].v1 : edges[faces[fid].edge_ids[ei+1]].v0;
            draw_triangle(sv0, s2s(verts[v1],ox,oy,sc), s2s(verts[v2],ox,oy,sc), col);
        }
    }

    /* 2. Diffusion (paint section) */
    for (int pi = 0; pi < n_paint; pi++) {
        if (s_paint[pi].type != 0) continue;
        draw_diffusion(s_paint[pi].target, s_paint[pi].c1, s_paint[pi].c2, 50.0 * sc / 100.0);
    }

    /* 3. Edge outlines (thin black, tessellated for curves) */
    for (int ei = 0; ei < n_edges; ei++) {
        if (edges[ei].type == EDGE_CUBIC && edges[ei].n_cp >= 2) {
            Vec2 p0 = s2s(verts[edges[ei].v0], ox, oy, sc);
            Vec2 p3 = s2s(verts[edges[ei].v1], ox, oy, sc);
            int steps = 32;
            for (int i = 0; i < steps; i++) {
                double t0 = (double)i / steps;
                double t1 = (double)(i+1) / steps;
                Vec2 a = bezier_cubic(p0, s2s(edges[ei].cp[0],ox,oy,sc), s2s(edges[ei].cp[1],ox,oy,sc), p3, t0);
                Vec2 b = bezier_cubic(p0, s2s(edges[ei].cp[0],ox,oy,sc), s2s(edges[ei].cp[1],ox,oy,sc), p3, t1);
                draw_thick_line(a, b, 1.5, (Color){40,40,40,255});
            }
        } else {
            draw_thick_line(
                s2s(verts[edges[ei].v0], ox, oy, sc),
                s2s(verts[edges[ei].v1], ox, oy, sc),
                1.5, (Color){40,40,40,255});
        }
    }

    /* 4. Strokes (variable-width, colored) */
    for (int si = 0; si < n_strokes; si++) {
        if (strokes[si].n_width > 0)
            draw_variable_stroke(strokes[si].edge_id, strokes[si].width, strokes[si].n_width, strokes[si].color);
    }

    /* 5. Vertex dots */
    for (int vi = 0; vi < n_verts; vi++) {
        Vec2 sp = s2s(verts[vi], ox, oy, sc);
        int cx = (int)sp.x, cy = (int)sp.y;
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
                if (dx*dx+dy*dy <= 5)
                    fb_set(cx+dx, cy+dy, 220, 50, 50);
    }
}

/* ── BMP output ─── */

static void write_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    int rb = fb_w * 3, pad = (4 - rb%4) % 4;
    int isz = (rb + pad) * fb_h, fsz = 54 + isz;
    uint8_t fh[14] = {0};
    fh[0]='B'; fh[1]='M';
    fh[2]=fsz&0xFF; fh[3]=(fsz>>8)&0xFF; fh[4]=(fsz>>16)&0xFF; fh[5]=(fsz>>24)&0xFF;
    fh[10]=54;
    fwrite(fh, 1, 14, f);
    uint8_t dh[40] = {0};
    dh[0]=40;
    dh[4]=fb_w&0xFF; dh[5]=(fb_w>>8)&0xFF; dh[6]=(fb_w>>16)&0xFF; dh[7]=(fb_w>>24)&0xFF;
    dh[8]=fb_h&0xFF; dh[9]=(fb_h>>8)&0xFF; dh[10]=(fb_h>>16)&0xFF; dh[11]=(fb_h>>24)&0xFF;
    dh[12]=1; dh[14]=24;
    dh[20]=isz&0xFF; dh[21]=(isz>>8)&0xFF; dh[22]=(isz>>16)&0xFF; dh[23]=(isz>>24)&0xFF;
    fwrite(dh, 1, 40, f);
    uint8_t padb[3] = {0};
    for (int y = fb_h-1; y >= 0; y--) {
        for (int x = 0; x < fb_w; x++) {
            int o = (y*fb_w+x)*3;
            uint8_t bgr[3] = { fb[o+2], fb[o+1], fb[o] };
            fwrite(bgr, 1, 3, f);
        }
        if (pad) fwrite(padb, 1, pad, f);
    }
    fclose(f);
}

/* ─── SVG output ─── */

static void write_svg(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    double ox, oy, sc;
    compute_view(&ox, &oy, &sc);

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 %d %d\" width=\"%d\" height=\"%d\">\n", fb_w,fb_h,fb_w,fb_h);
    fprintf(f, "  <!-- SmazkaVG v1.2 SVG projection -->\n");
    fprintf(f, "  <rect width=\"%d\" height=\"%d\" fill=\"white\"/>\n", fb_w, fb_h);

    /* Faces (solid fill paint) */
    for (int pi = 0; pi < n_paint; pi++) {
        if (s_paint[pi].type != 1) continue;
        int fid = s_paint[pi].target;
        if (fid >= n_faces) continue;
        Color fc = s_paint[pi].c1;
        fprintf(f, "  <g id=\"face-%d\"><polygon points=\"", fid);
        int fv = edges[faces[fid].edge_ids[0]].v0, vis[64]={0}, pv[64], npv=0;
        for (int ei = 0; ei < faces[fid].n_edges; ei++) {
            int eid = faces[fid].edge_ids[ei];
            int va = edges[eid].v0, vb = edges[eid].v1;
            int v = (npv==0) ? va : (vis[va] ? vb : va);
            if (!vis[v] && npv < 64) { pv[npv++] = v; vis[v] = 1; }
        }
        for (int k = 0; k < npv; k++) {
            Vec2 sp = s2s(verts[pv[k]], ox, oy, sc);
            fprintf(f, "%.2f,%.2f ", sp.x, sp.y);
        }
        fprintf(f, "\" fill=\"rgba(%d,%d,%d,%.2f)\"/></g>\n", fc.r,fc.g,fc.b,fc.a/255.0);
    }

    /* Diffusion */
    for (int pi = 0; pi < n_paint; pi++) {
        if (s_paint[pi].type != 0) continue;
        int eid = s_paint[pi].target;
        if (eid >= n_edges) continue;
        Vec2 sa = s2s(verts[edges[eid].v0],ox,oy,sc), sb = s2s(verts[edges[eid].v1],ox,oy,sc);
        Color lc = s_paint[pi].c1, rc = s_paint[pi].c2;
        fprintf(f, "  <defs><linearGradient id=\"d%d\">\n", pi);
        fprintf(f, "    <stop offset=\"0%%\" stop-color=\"rgb(%d,%d,%d)\"/>\n", lc.r,lc.g,lc.b);
        fprintf(f, "    <stop offset=\"100%%\" stop-color=\"rgb(%d,%d,%d)\"/>\n", rc.r,rc.g,rc.b);
        fprintf(f, "  </linearGradient></defs>\n");
        fprintf(f, "  <line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"url(#d%d)\" stroke-width=\"8\" opacity=\"0.6\"/>\n", sa.x,sa.y,sb.x,sb.y,pi);
    }

    /* Edges (curved if cubic Bézier) */
    for (int ei = 0; ei < n_edges; ei++) {
        Vec2 sa = s2s(verts[edges[ei].v0],ox,oy,sc), sb = s2s(verts[edges[ei].v1],ox,oy,sc);
        if (edges[ei].type == EDGE_CUBIC && edges[ei].n_cp >= 2) {
            Vec2 c1 = s2s(edges[ei].cp[0],ox,oy,sc), c2 = s2s(edges[ei].cp[1],ox,oy,sc);
            fprintf(f, "  <path id=\"e%d\" d=\"M %.2f,%.2f C %.2f,%.2f %.2f,%.2f %.2f,%.2f\" stroke=\"#282828\" stroke-width=\"1.5\" fill=\"none\"/>\n",
                    ei, sa.x,sa.y, c1.x,c1.y, c2.x,c2.y, sb.x,sb.y);
        } else {
            fprintf(f, "  <line id=\"e%d\" x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"#282828\" stroke-width=\"1.5\"/>\n", ei,sa.x,sa.y,sb.x,sb.y);
        }
    }

    /* Strokes */
    for (int si = 0; si < n_strokes; si++) {
        int eid = strokes[si].edge_id;
        if (eid >= n_edges) continue;
        Vec2 sa = s2s(verts[edges[eid].v0],ox,oy,sc), sb = s2s(verts[edges[eid].v1],ox,oy,sc);
        Color c = strokes[si].color;
        double aw = 0; for (int w = 0; w < strokes[si].n_width; w++) aw += strokes[si].width[w];
        if (strokes[si].n_width > 0) aw /= strokes[si].n_width;
        if (edges[eid].type == EDGE_CUBIC && edges[eid].n_cp >= 2) {
            Vec2 c1 = s2s(edges[eid].cp[0],ox,oy,sc), c2 = s2s(edges[eid].cp[1],ox,oy,sc);
            fprintf(f, "  <path id=\"s%d\" d=\"M %.2f,%.2f C %.2f,%.2f %.2f,%.2f %.2f,%.2f\" stroke=\"rgba(%d,%d,%d,%.2f)\" stroke-width=\"%.2f\" stroke-linecap=\"round\" fill=\"none\"/>\n",
                    si, sa.x,sa.y, c1.x,c1.y, c2.x,c2.y, sb.x,sb.y, c.r,c.g,c.b,c.a/255.0,aw*sc);
        } else {
            fprintf(f, "  <line id=\"s%d\" x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" stroke=\"rgba(%d,%d,%d,%.2f)\" stroke-width=\"%.2f\" stroke-linecap=\"round\"/>\n",
                    si,sa.x,sa.y,sb.x,sb.y,c.r,c.g,c.b,c.a/255.0,aw*sc);
        }
    }

    /* Vertices */
    for (int vi = 0; vi < n_verts; vi++) {
        Vec2 sp = s2s(verts[vi], ox, oy, sc);
        fprintf(f, "  <circle id=\"v%d\" cx=\"%.2f\" cy=\"%.2f\" r=\"3\" fill=\"#dc3232\"/>\n", vi,sp.x,sp.y);
    }

    fprintf(f, "</svg>\n");
    fclose(f);
}

/* ─── ASCII output ─── */

static void write_ascii(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    const char *ramp = " .:-=+*#%@";
    int rl = (int)strlen(ramp);
    int cw = 4, ch = 7;
    int cols = fb_w / cw, rows = fb_h / ch;
    if (cols < 1) cols = 1; if (rows < 1) rows = 1;
    if (cols > 160) cols = 160; if (rows > 80) rows = 80;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            double sum = 0; int cnt = 0;
            for (int dy = 0; dy < ch; dy++) {
                int py = r*ch + dy; if (py >= fb_h) break;
                for (int dx = 0; dx < cw; dx++) {
                    int px = c*cw + dx; if (px >= fb_w) break;
                    int o = (py*fb_w+px)*3;
                    sum += 0.299*fb[o] + 0.587*fb[o+1] + 0.114*fb[o+2];
                    cnt++;
                }
            }
            double lum = cnt ? sum/cnt : 255.0;
            int idx = rl - 1 - (int)(lum / 256.0 * rl);
            if (idx < 0) idx = 0; if (idx >= rl) idx = rl-1;
            fputc(ramp[idx], f);
        }
        fputc('\n', f);
    }
    fclose(f);
}

/* ─── Main ─── */

static void strip_ext(const char *in, char *out, int sz) {
    strncpy(out, in, sz-1); out[sz-1] = 0;
    char *d = strrchr(out, '.'); if (d) *d = 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "SmazkaVG v1.2 Rasterizer\nUsage: %s <input.smazka> [w] [h]\n", argv[0]);
        return 1;
    }
    const char *inpath = argv[1];
    int w = (argc >= 3) ? atoi(argv[2]) : 512;
    int h = (argc >= 4) ? atoi(argv[3]) : 512;
    if (w < 64) w = 64; if (h < 64) h = 64;
    if (w > 4096) w = 4096; if (h > 4096) h = 4096;

    if (parse_file(inpath) != 0) return 1;

    int n_cubic = 0;
    for (int i = 0; i < n_edges; i++) if (edges[i].type == EDGE_CUBIC) n_cubic++;
    fprintf(stderr, "v1.2: %d verts, %d edges (%d cubic), %d faces, %d strokes, %d paint\n",
            n_verts, n_edges, n_cubic, n_faces, n_strokes, n_paint);

    fb_alloc(w, h);
    render_scene();

    char base[512];
    strip_ext(inpath, base, sizeof(base));

    char bmp_p[560], svg_p[560], txt_p[560];
    snprintf(bmp_p, sizeof(bmp_p), "%s.bmp", base);
    snprintf(svg_p, sizeof(svg_p), "%s.svg", base);
    snprintf(txt_p, sizeof(txt_p), "%s.txt", base);

    write_bmp(bmp_p); fprintf(stderr, "BMP:  %s (%dx%d)\n", bmp_p, w, h);
    write_svg(svg_p); fprintf(stderr, "SVG:  %s\n", svg_p);
    write_ascii(txt_p); fprintf(stderr, "ASCII: %s\n", txt_p);

    fb_free();
    return 0;
}
