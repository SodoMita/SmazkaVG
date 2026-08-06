/*
 * SmazkaVG Rasterizer — zero external dependencies
 *
 * Reads Line-ASM (.smazka), renders to framebuffer, outputs:
 *   1. BMP   (raw, no libs — fast iteration)
 *   2. SVG   (vector projection — LLM-friendly)
 *   3. ASCII art (terminal preview)
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

/* ═══════════════════════════════════════════════════════════════════
 *  FIXED-POINT (Q16.16)
 * ═══════════════════════════════════════════════════════════════════ */

typedef int32_t q16_t;

static inline double q16_to_d(q16_t v)  { return (double)v / 65536.0; }
static inline q16_t  d_to_q16(double x) {
    double s = x * 65536.0;
    if (s >  2147483647.0) return  2147483647;
    if (s < -2147483648.0) return (int32_t)0x80000000;
    return (q16_t)(s >= 0 ? s + 0.5 : s - 0.5);
}

/* ═══════════════════════════════════════════════════════════════════
 *  SCENE DATA (flat, matching spec)
 * ═══════════════════════════════════════════════════════════════════ */

#define MAX_VERTS   4096
#define MAX_EDGES   4096
#define MAX_FACES   4096
#define MAX_STROKES 4096
#define MAX_NODES   1024
#define MAX_LINE    1024

typedef struct { double x, y; } Vec2;
typedef struct { uint8_t r, g, b, a; } Color;

/* Parsed primitives */
static int      n_verts;
static Vec2     verts[MAX_VERTS];
static int      vert_pinned[MAX_VERTS];

static int      n_edges;
static struct { int v0, v1; } edges[MAX_EDGES];

static int      n_faces;
static struct {
    int edge_ids[64]; int n_edges;
    int fill_color;    /* 0 = none, else packed RGBA */
} faces[MAX_FACES];

static int      n_strokes;
static struct {
    int edge_id;
    Color color;
    double width;
} strokes[MAX_STROKES];

/* Nodes (transform hierarchy) */
static int      n_nodes;
static struct {
    double tx, ty, rot, sx, sy, skew;
    int content_prim;   /* what this node transforms (-1 = none) */
    int content_type;   /* 0=none, 1=face, 2=stroke */
} nodes[MAX_NODES];

/* Constraints — we resolve a subset at parse time */
static int parent_of[MAX_NODES]; /* parent_of[i] = parent node id, -1 = root */
static int above_a[256], above_b[256]; /* above ordering pairs */
static int n_above;

/* Diffusion curves */
static int n_diffusion;
static struct {
    int edge_id;
    Color left, right;
} diffusion[64];

/* ═══════════════════════════════════════════════════════════════════
 *  LINE-ASM PARSER
 * ═══════════════════════════════════════════════════════════════════ */

static Color parse_color(const char *s) {
    Color c = {0, 0, 0, 255};
    unsigned int v = 0;
    if (strlen(s) >= 8) {
        sscanf(s, "%x", &v);
        c.r = (v >> 24) & 0xFF;
        c.g = (v >> 16) & 0xFF;
        c.b = (v >>  8) & 0xFF;
        c.a = (v      ) & 0xFF;
    } else if (strlen(s) >= 6) {
        sscanf(s, "%x", &v);
        c.r = (v >> 16) & 0xFF;
        c.g = (v >>  8) & 0xFF;
        c.b = (v      ) & 0xFF;
        c.a = 255;
    }
    return c;
}

static int parse_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }

    /* Init parent map */
    for (int i = 0; i < MAX_NODES; i++) parent_of[i] = -1;

    char line[MAX_LINE];
    while (fgets(line, sizeof(line), f)) {
        /* Strip newline */
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        char *cr = strchr(line, '\r'); if (cr) *cr = 0;

        /* Skip empty / comments */
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
                verts[id].x = x;
                verts[id].y = y;
            }
            break;
        }

        case 'e': {
            int id, v0, v1;
            if (sscanf(p, "e %d %d %d", &id, &v0, &v1) >= 3) {
                if (id >= n_edges) n_edges = id + 1;
                edges[id].v0 = v0;
                edges[id].v1 = v1;
            }
            break;
        }

        case 'f': {
            int id;
            if (sscanf(p, "f %d", &id) != 1) break;
            if (id >= n_faces) n_faces = id + 1;
            /* Parse edge list: f <id> <e0> <e1> ... [fill] */
            char *tok = p;
            /* skip "f <id>" */
            while (*tok && *tok != ' ') tok++;  /* skip 'f' */
            while (*tok == ' ') tok++;
            while (*tok && *tok != ' ') tok++;  /* skip id */
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
            /* Check for fill color at end */
            if (*tok) {
                faces[id].fill_color = (int)strtol(tok, NULL, 16);
            }
            break;
        }

        case 's': {
            int id, eid; char col_str[32]; double w;
            if (sscanf(p, "s %d %d %31s %lf", &id, &eid, col_str, &w) >= 4) {
                if (id >= n_strokes) n_strokes = id + 1;
                strokes[id].edge_id = eid;
                strokes[id].color = parse_color(col_str);
                strokes[id].width = w;
            }
            break;
        }

        case 'n': {
            int id; double tx, ty, rot, sx, sy, sk; int cref = -1;
            int n = sscanf(p, "n %d %lf %lf %lf %lf %lf %lf %d",
                           &id, &tx, &ty, &rot, &sx, &sy, &sk, &cref);
            if (n >= 7) {
                if (id >= n_nodes) n_nodes = id + 1;
                nodes[id].tx = tx; nodes[id].ty = ty;
                nodes[id].rot = rot;
                nodes[id].sx = sx; nodes[id].sy = sy;
                nodes[id].skew = sk;
                nodes[id].content_prim = (n >= 8) ? cref : -1;
                nodes[id].content_type = 0;
            }
            break;
        }

        case 'c': {
            int cid; char ctype[64];
            if (sscanf(p, "c %d %63s", &cid, ctype) < 2) break;

            if (strcmp(ctype, "parent") == 0) {
                int child, par;
                if (sscanf(p, "c %*d parent %d %d", &child, &par) == 2)
                    parent_of[child] = par;
            }
            else if (strcmp(ctype, "diffusion") == 0) {
                int eid; char ls[32], rs[32], L[4], R[4];
                if (sscanf(p, "c %*d diffusion %d %3s %31s %3s %31s",
                           &eid, L, ls, R, rs) >= 4) {
                    if (n_diffusion < 64) {
                        diffusion[n_diffusion].edge_id = eid;
                        diffusion[n_diffusion].left  = parse_color(ls);
                        diffusion[n_diffusion].right = parse_color(rs);
                        n_diffusion++;
                    }
                }
            }
            else if (strcmp(ctype, "above") == 0) {
                int a, b;
                if (sscanf(p, "c %*d above %d %d", &a, &b) == 2 && n_above < 256) {
                    above_a[n_above] = a;
                    above_b[n_above] = b;
                    n_above++;
                }
            }
            /* Other constraints are resolved by the solver (resolver.c);
               for rasterization we use the geometry as-declared. */
            break;
        }

        default:
            break; /* skip 'm' metadata, unknown, etc. */
        }
    }
    fclose(f);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  TRANSFORM RESOLUTION (hierarchy via parent constraints)
 * ═══════════════════════════════════════════════════════════════════ */

/* 2D affine: [a b tx; c d ty] stored as {a,b,c,d,tx,ty} */
typedef struct { double a,b,c,d,tx,ty; } Affine;

static Affine affine_identity(void) {
    return (Affine){1,0,0,1,0,0};
}

static Affine affine_mul(Affine p, Affine l) {
    return (Affine){
        p.a*l.a + p.b*l.c,   p.a*l.b + p.b*l.d,
        p.c*l.a + p.d*l.c,   p.c*l.b + p.d*l.d,
        p.a*l.tx + p.b*l.ty + p.tx,
        p.c*l.tx + p.d*l.ty + p.ty
    };
}

static Affine node_local(int i) {
    double cr = cos(nodes[i].rot), sr = sin(nodes[i].rot);
    return (Affine){
        nodes[i].sx * cr,  -nodes[i].sy * sr,
        nodes[i].sx * sr,   nodes[i].sy * cr,
        nodes[i].tx,        nodes[i].ty
    };
}

/* Compute world transform for node i (with cycle protection) */
static Affine world_xform[MAX_NODES];
static int    world_valid[MAX_NODES];

static Affine resolve_world(int i, int depth) {
    if (world_valid[i]) return world_xform[i];
    if (depth > 64) return affine_identity(); /* cycle breaker */

    Affine local = node_local(i);
    if (parent_of[i] >= 0 && parent_of[i] < n_nodes) {
        Affine pw = resolve_world(parent_of[i], depth + 1);
        world_xform[i] = affine_mul(pw, local);
    } else {
        world_xform[i] = local;
    }
    world_valid[i] = 1;
    return world_xform[i];
}

static Vec2 transform_point(Affine xf, Vec2 p) {
    return (Vec2){ xf.a*p.x + xf.b*p.y + xf.tx,
                   xf.c*p.x + xf.d*p.y + xf.ty };
}

/* ═══════════════════════════════════════════════════════════════════
 *  FRAMEBUFFER
 * ═══════════════════════════════════════════════════════════════════ */

static int fb_w, fb_h;
static uint8_t *fb; /* RGB, 3 bytes/pixel, row-major, top-down */

static void fb_alloc(int w, int h) {
    fb_w = w; fb_h = h;
    fb = (uint8_t *)calloc(w * h * 3, 1);
    /* Init to white */
    memset(fb, 255, w * h * 3);
}

static void fb_free(void) { free(fb); }

static inline void fb_set(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
    if (x < 0 || x >= fb_w || y < 0 || y >= fb_h) return;
    int off = (y * fb_w + x) * 3;
    fb[off] = r; fb[off+1] = g; fb[off+2] = b;
}

static inline void fb_blend(int x, int y, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    if (x < 0 || x >= fb_w || y < 0 || y >= fb_h) return;
    if (a == 0) return;
    int off = (y * fb_w + x) * 3;
    if (a == 255) {
        fb[off] = r; fb[off+1] = g; fb[off+2] = b;
        return;
    }
    uint16_t ia = 255 - a;
    fb[off]   = (uint8_t)((r * a + fb[off]   * ia) / 255);
    fb[off+1] = (uint8_t)((g * a + fb[off+1] * ia) / 255);
    fb[off+2] = (uint8_t)((b * a + fb[off+2] * ia) / 255);
}

/* ═══════════════════════════════════════════════════════════════════
 *  RASTERIZATION PRIMITIVES
 * ═══════════════════════════════════════════════════════════════════ */

/* Edge function for triangle fill (barycentric) */
static inline double edge_fn(Vec2 a, Vec2 b, Vec2 p) {
    return (p.x - a.x) * (b.y - a.y) - (p.y - a.y) * (b.x - a.x);
}

static void draw_triangle(Vec2 v0, Vec2 v1, Vec2 v2, Color col) {
    /* Bounding box */
    double minx = fmin(fmin(v0.x, v1.x), v2.x);
    double maxx = fmax(fmax(v0.x, v1.x), v2.x);
    double miny = fmin(fmin(v0.y, v1.y), v2.y);
    double maxy = fmax(fmax(v0.y, v1.y), v2.y);

    int x0 = (int)floor(minx), x1 = (int)ceil(maxx);
    int y0 = (int)floor(miny), y1 = (int)ceil(maxy);
    if (x0 < 0) x0 = 0; if (x1 >= fb_w) x1 = fb_w - 1;
    if (y0 < 0) y0 = 0; if (y1 >= fb_h) y1 = fb_h - 1;

    double area = edge_fn(v0, v1, v2);
    if (fabs(area) < 1e-10) return;
    double inv_area = 1.0 / area;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            Vec2 p = {x + 0.5, y + 0.5};
            double w0 = edge_fn(v1, v2, p) * inv_area;
            double w1 = edge_fn(v2, v0, p) * inv_area;
            double w2 = edge_fn(v0, v1, p) * inv_area;
            if (w0 >= 0 && w1 >= 0 && w2 >= 0) {
                fb_blend(x, y, col.r, col.g, col.b, col.a);
            }
        }
    }
}

/* Thick line via distance field */
static void draw_thick_line(Vec2 a, Vec2 b, double width, Color col) {
    double dx = b.x - a.x, dy = b.y - a.y;
    double len = sqrt(dx*dx + dy*dy);
    if (len < 1e-6) return;

    double half_w = width * 0.5;
    /* Bounding box with padding */
    double pad = half_w + 1.0;
    int x0 = (int)floor(fmin(a.x, b.x) - pad);
    int x1 = (int)ceil( fmax(a.x, b.x) + pad);
    int y0 = (int)floor(fmin(a.y, b.y) - pad);
    int y1 = (int)ceil( fmax(a.y, b.y) + pad);
    if (x0 < 0) x0 = 0; if (x1 >= fb_w) x1 = fb_w - 1;
    if (y0 < 0) y0 = 0; if (y1 >= fb_h) y1 = fb_h - 1;

    /* Perpendicular distance from point to line segment */
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            double px = x + 0.5 - a.x;
            double py = y + 0.5 - a.y;
            double t = (px * dx + py * dy) / (len * len);
            if (t < 0.0) t = 0.0;
            if (t > 1.0) t = 1.0;
            double cx = a.x + t * dx;
            double cy = a.y + t * dy;
            double dist = sqrt((x+0.5 - cx)*(x+0.5 - cx) + (y+0.5 - cy)*(y+0.5 - cy));

            if (dist <= half_w + 0.5) {
                /* Anti-alias: smoothstep in [half_w-0.5, half_w+0.5] */
                double alpha;
                if (dist <= half_w - 0.5)
                    alpha = 1.0;
                else
                    alpha = 1.0 - (dist - half_w + 0.5);
                if (alpha < 0) alpha = 0;
                uint8_t a8 = (uint8_t)(alpha * col.a);
                fb_blend(x, y, col.r, col.g, col.b, a8);
            }
        }
    }
}

/* Diffusion curve: color region near edge with gradient perpendicular to edge */
static void draw_diffusion(int edge_id, Color left_col, Color right_col, double radius) {
    if (edge_id >= n_edges) return;
    Vec2 a = verts[edges[edge_id].v0];
    Vec2 b = verts[edges[edge_id].v1];
    double dx = b.x - a.x, dy = b.y - a.y;
    double len = sqrt(dx*dx + dy*dy);
    if (len < 1e-6) return;

    /* Perpendicular direction (left side) */
    double nx = -dy / len, ny = dx / len;

    int x0 = (int)floor(fmin(a.x, b.x) - radius - 1);
    int x1 = (int)ceil( fmax(a.x, b.x) + radius + 1);
    int y0 = (int)floor(fmin(a.y, b.y) - radius - 1);
    int y1 = (int)ceil( fmax(a.y, b.y) + radius + 1);
    if (x0 < 0) x0 = 0; if (x1 >= fb_w) x1 = fb_w - 1;
    if (y0 < 0) y0 = 0; if (y1 >= fb_h) y1 = fb_h - 1;

    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            double px = x + 0.5, py = y + 0.5;
            /* Signed distance from edge line (positive = left of direction a→b) */
            double side = ((px - a.x) * ny - (py - a.y) * nx);
            /* Also need to be near the edge segment */
            double t = ((px - a.x) * dx + (py - a.y) * dy) / (len * len);
            if (t < -0.05 || t > 1.05) continue;

            double d = fabs(side);
            if (d > radius) continue;

            /* Blend factor: 0 = left, 1 = right */
            double blend = 0.5 + side / (2.0 * radius);
            if (blend < 0) blend = 0;
            if (blend > 1) blend = 1;

            /* Falloff at edges of radius */
            double falloff = 1.0 - d / radius;
            uint8_t r = (uint8_t)(left_col.r * (1-blend) + right_col.r * blend);
            uint8_t g = (uint8_t)(left_col.g * (1-blend) + right_col.g * blend);
            uint8_t b = (uint8_t)(left_col.b * (1-blend) + right_col.b * blend);
            uint8_t a = (uint8_t)(falloff * 200); /* semi-transparent blend */
            fb_blend(x, y, r, g, b, a);
        }
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  SCENE RENDER
 * ═══════════════════════════════════════════════════════════════════ */

/* Auto-fit scene to framebuffer */
static void compute_view(double *ox, double *oy, double *scale) {
    if (n_verts == 0) { *ox = 0; *oy = 0; *scale = 1; return; }
    double minx = 1e30, maxx = -1e30, miny = 1e30, maxy = -1e30;
    for (int i = 0; i < n_verts; i++) {
        if (verts[i].x < minx) minx = verts[i].x;
        if (verts[i].x > maxx) maxx = verts[i].x;
        if (verts[i].y < miny) miny = verts[i].y;
        if (verts[i].y > maxy) maxy = verts[i].y;
    }
    double sw = maxx - minx, sh = maxy - miny;
    if (sw < 1) sw = 1;
    if (sh < 1) sh = 1;
    double margin = 40.0;
    double sx = (fb_w - 2*margin) / sw;
    double sy = (fb_h - 2*margin) / sh;
    *scale = fmin(sx, sy);
    *ox = margin - minx * (*scale) + ((fb_w - 2*margin) - sw * (*scale)) * 0.5;
    *oy = margin - miny * (*scale) + ((fb_h - 2*margin) - sh * (*scale)) * 0.5;
}

static Vec2 scene_to_screen(Vec2 p, double ox, double oy, double scale) {
    return (Vec2){ p.x * scale + ox, p.y * scale + oy };
}

static void render_scene(void) {
    double ox, oy, scale;
    compute_view(&ox, &oy, &scale);

    /* Transform all vertices by node hierarchy (if any nodes exist) */
    /* For now, nodes apply to content_prim. If no nodes, use raw verts. */

    /* 1. Draw faces (triangulated from edge lists) */
    for (int fi = 0; fi < n_faces; fi++) {
        if (faces[fi].n_edges < 3) continue;
        Color fc;
        if (faces[fi].fill_color) {
            unsigned int v = faces[fi].fill_color;
            fc.r = (v >> 24) & 0xFF; fc.g = (v >> 16) & 0xFF;
            fc.b = (v >>  8) & 0xFF; fc.a = (v      ) & 0xFF;
        } else {
            fc = (Color){200, 220, 240, 180}; /* default light blue fill */
        }
        /* Fan triangulate from first vertex of first edge */
        int first_vid = edges[faces[fi].edge_ids[0]].v0;
        Vec2 sv0 = scene_to_screen(verts[first_vid], ox, oy, scale);
        for (int ei = 1; ei + 1 < faces[fi].n_edges; ei++) {
            int e1 = faces[fi].edge_ids[ei];
            int e2 = faces[fi].edge_ids[ei + 1];
            /* Pick the two "other" vertices of these edges */
            int v1 = (edges[e1].v0 != first_vid) ? edges[e1].v0 : edges[e1].v1;
            int v2 = (edges[e2].v0 != first_vid) ? edges[e2].v0 : edges[e2].v1;
            Vec2 sv1 = scene_to_screen(verts[v1], ox, oy, scale);
            Vec2 sv2 = scene_to_screen(verts[v2], ox, oy, scale);
            draw_triangle(sv0, sv1, sv2, fc);
        }
    }

    /* 2. Draw diffusion curves */
    for (int d = 0; d < n_diffusion; d++) {
        int eid = diffusion[d].edge_id;
        if (eid >= n_edges) continue;
        Vec2 sa = scene_to_screen(verts[edges[eid].v0], ox, oy, scale);
        Vec2 sb = scene_to_screen(verts[edges[eid].v1], ox, oy, scale);
        draw_diffusion(eid, diffusion[d].left, diffusion[d].right, 60.0 * scale / 100.0);
    }

    /* 3. Draw edges as thin lines */
    for (int ei = 0; ei < n_edges; ei++) {
        Vec2 sa = scene_to_screen(verts[edges[ei].v0], ox, oy, scale);
        Vec2 sb = scene_to_screen(verts[edges[ei].v1], ox, oy, scale);
        draw_thick_line(sa, sb, 1.5, (Color){40, 40, 40, 255});
    }

    /* 4. Draw strokes (thick, colored) */
    for (int si = 0; si < n_strokes; si++) {
        int eid = strokes[si].edge_id;
        if (eid >= n_edges) continue;
        Vec2 sa = scene_to_screen(verts[edges[eid].v0], ox, oy, scale);
        Vec2 sb = scene_to_screen(verts[edges[eid].v1], ox, oy, scale);
        draw_thick_line(sa, sb, strokes[si].width * scale, strokes[si].color);
    }

    /* 5. Draw vertices as dots */
    for (int vi = 0; vi < n_verts; vi++) {
        Vec2 sp = scene_to_screen(verts[vi], ox, oy, scale);
        int cx = (int)sp.x, cy = (int)sp.y;
        for (int dy = -2; dy <= 2; dy++)
            for (int dx = -2; dx <= 2; dx++)
                if (dx*dx + dy*dy <= 5)
                    fb_set(cx+dx, cy+dy, 220, 50, 50);
    }
}

/* ═══════════════════════════════════════════════════════════════════
 *  BMP OUTPUT (uncompressed, 24-bit, zero dependencies)
 * ═══════════════════════════════════════════════════════════════════ */

static int write_bmp(const char *path) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;

    int row_bytes = fb_w * 3;
    int pad = (4 - row_bytes % 4) % 4;
    int img_size = (row_bytes + pad) * fb_h;
    int file_size = 14 + 40 + img_size;

    /* File header (14 bytes) */
    uint8_t fh[14] = {0};
    fh[0] = 'B'; fh[1] = 'M';
    fh[2] = file_size & 0xFF;
    fh[3] = (file_size >> 8) & 0xFF;
    fh[4] = (file_size >> 16) & 0xFF;
    fh[5] = (file_size >> 24) & 0xFF;
    /* offset to pixel data = 54 */
    fh[10] = 54;
    fwrite(fh, 1, 14, f);

    /* DIB header (40 bytes) — BITMAPINFOHEADER */
    uint8_t dh[40] = {0};
    dh[0] = 40; /* header size */
    dh[4] = fb_w & 0xFF;         dh[5] = (fb_w >> 8) & 0xFF;
    dh[6] = (fb_w >> 16) & 0xFF; dh[7] = (fb_w >> 24) & 0xFF;
    dh[8] = fb_h & 0xFF;         dh[9] = (fb_h >> 8) & 0xFF;
    dh[10] = (fb_h >> 16) & 0xFF; dh[11] = (fb_h >> 24) & 0xFF;
    dh[12] = 1;  /* planes */
    dh[14] = 24; /* bits per pixel */
    /* compression = 0 (BI_RGB) */
    dh[20] = img_size & 0xFF;
    dh[21] = (img_size >> 8) & 0xFF;
    dh[22] = (img_size >> 16) & 0xFF;
    dh[23] = (img_size >> 24) & 0xFF;
    fwrite(dh, 1, 40, f);

    /* Pixel data: BMP is bottom-up, BGR */
    uint8_t pad_bytes[3] = {0};
    for (int y = fb_h - 1; y >= 0; y--) {
        for (int x = 0; x < fb_w; x++) {
            int off = (y * fb_w + x) * 3;
            uint8_t bgr[3] = { fb[off+2], fb[off+1], fb[off] };
            fwrite(bgr, 1, 3, f);
        }
        if (pad > 0) fwrite(pad_bytes, 1, pad, f);
    }

    fclose(f);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  SVG OUTPUT (vector projection — LLM-friendly)
 * ═══════════════════════════════════════════════════════════════════ */

static int write_svg(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    /* Compute view bounds to set viewBox */
    double ox, oy, scale;
    compute_view(&ox, &oy, &scale);

    fprintf(f, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n");
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" "
               "viewBox=\"0 0 %d %d\" width=\"%d\" height=\"%d\">\n",
            fb_w, fb_h, fb_w, fb_h);
    fprintf(f, "  <!-- SmazkaVG v1.1 SVG projection -->\n");
    fprintf(f, "  <!-- Auto-generated: %d verts, %d edges, %d faces, %d strokes -->\n",
            n_verts, n_edges, n_faces, n_strokes);

    /* White background */
    fprintf(f, "  <rect width=\"%d\" height=\"%d\" fill=\"white\"/>\n", fb_w, fb_h);

    /* Faces */
    for (int fi = 0; fi < n_faces; fi++) {
        if (faces[fi].n_edges < 3) continue;
        fprintf(f, "  <g id=\"face-%d\">\n", fi);

        /* Build polygon from edge chain */
        fprintf(f, "    <polygon points=\"");
        int first_vid = edges[faces[fi].edge_ids[0]].v0;
        /* Collect unique vertices in order */
        int visited[64] = {0};
        int poly_verts[64], npv = 0;
        for (int ei = 0; ei < faces[fi].n_edges; ei++) {
            int eid = faces[fi].edge_ids[ei];
            int va = edges[eid].v0, vb = edges[eid].v1;
            int v = (npv == 0) ? va :
                    (visited[va] ? vb : va);
            if (!visited[v] && npv < 64) {
                poly_verts[npv++] = v;
                visited[v] = 1;
            }
        }
        for (int k = 0; k < npv; k++) {
            Vec2 sp = scene_to_screen(verts[poly_verts[k]], ox, oy, scale);
            fprintf(f, "%.2f,%.2f ", sp.x, sp.y);
        }
        unsigned int fc = faces[fi].fill_color;
        if (fc) {
            fprintf(f, "\" fill=\"rgba(%d,%d,%d,%.2f)\"/>\n",
                    (fc>>24)&0xFF, (fc>>16)&0xFF, (fc>>8)&0xFF,
                    ((fc)&0xFF) / 255.0);
        } else {
            fprintf(f, "\" fill=\"rgba(200,220,240,0.7)\"/>\n");
        }
        fprintf(f, "  </g>\n");
    }

    /* Diffusion curves (as gradient lines with annotation) */
    for (int d = 0; d < n_diffusion; d++) {
        int eid = diffusion[d].edge_id;
        if (eid >= n_edges) continue;
        Vec2 sa = scene_to_screen(verts[edges[eid].v0], ox, oy, scale);
        Vec2 sb = scene_to_screen(verts[edges[eid].v1], ox, oy, scale);
        Color lc = diffusion[d].left, rc = diffusion[d].right;
        fprintf(f, "  <defs>\n");
        fprintf(f, "    <linearGradient id=\"diff-%d\" x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\">\n",
                d, sa.x, sa.y, sb.x, sb.y);
        fprintf(f, "      <stop offset=\"0%%\" stop-color=\"rgb(%d,%d,%d)\"/>\n", lc.r, lc.g, lc.b);
        fprintf(f, "      <stop offset=\"100%%\" stop-color=\"rgb(%d,%d,%d)\"/>\n", rc.r, rc.g, rc.b);
        fprintf(f, "    </linearGradient>\n");
        fprintf(f, "  </defs>\n");
        fprintf(f, "  <line x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" "
                   "stroke=\"url(#diff-%d)\" stroke-width=\"12\" opacity=\"0.6\"/>\n",
                sa.x, sa.y, sb.x, sb.y, d);
    }

    /* Edges (thin black lines) */
    for (int ei = 0; ei < n_edges; ei++) {
        Vec2 sa = scene_to_screen(verts[edges[ei].v0], ox, oy, scale);
        Vec2 sb = scene_to_screen(verts[edges[ei].v1], ox, oy, scale);
        fprintf(f, "  <line id=\"edge-%d\" x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" "
                   "stroke=\"#282828\" stroke-width=\"1.5\"/>\n",
                ei, sa.x, sa.y, sb.x, sb.y);
    }

    /* Strokes */
    for (int si = 0; si < n_strokes; si++) {
        int eid = strokes[si].edge_id;
        if (eid >= n_edges) continue;
        Vec2 sa = scene_to_screen(verts[edges[eid].v0], ox, oy, scale);
        Vec2 sb = scene_to_screen(verts[edges[eid].v1], ox, oy, scale);
        Color c = strokes[si].color;
        fprintf(f, "  <line id=\"stroke-%d\" x1=\"%.2f\" y1=\"%.2f\" x2=\"%.2f\" y2=\"%.2f\" "
                   "stroke=\"rgba(%d,%d,%d,%.2f)\" stroke-width=\"%.2f\" stroke-linecap=\"round\"/>\n",
                si, sa.x, sa.y, sb.x, sb.y,
                c.r, c.g, c.b, c.a/255.0, strokes[si].width * scale);
    }

    /* Vertices (red dots) */
    for (int vi = 0; vi < n_verts; vi++) {
        Vec2 sp = scene_to_screen(verts[vi], ox, oy, scale);
        fprintf(f, "  <circle id=\"vert-%d\" cx=\"%.2f\" cy=\"%.2f\" r=\"3\" fill=\"#dc3232\"/>\n",
                vi, sp.x, sp.y);
        /* Label */
        fprintf(f, "  <text x=\"%.2f\" y=\"%.2f\" font-size=\"10\" fill=\"#333\">v%d (%.0f,%.0f)</text>\n",
                sp.x + 5, sp.y - 5, vi, verts[vi].x, verts[vi].y);
    }

    fprintf(f, "</svg>\n");
    fclose(f);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  ASCII ART OUTPUT
 * ═══════════════════════════════════════════════════════════════════ */

static int write_ascii(const char *path) {
    FILE *f = fopen(path, "w");
    if (!f) return -1;

    /* Character ramp: dark → light */
    const char *ramp = " .:-=+*#%@";
    int ramp_len = (int)strlen(ramp);

    /* Downsample framebuffer to character grid.
       Each character cell covers ~4×7 pixels (typical terminal aspect).
       We sample by averaging luminance over each cell. */
    int cell_w = 4, cell_h = 7;
    int cols = fb_w / cell_w;
    int rows = fb_h / cell_h;
    if (cols < 1) cols = 1;
    if (rows < 1) rows = 1;

    /* Cap to reasonable terminal size */
    if (cols > 160) cols = 160;
    if (rows > 80)  rows = 80;

    for (int row = 0; row < rows; row++) {
        for (int col = 0; col < cols; col++) {
            /* Average luminance over cell */
            double lum_sum = 0;
            int count = 0;
            for (int dy = 0; dy < cell_h; dy++) {
                int py = row * cell_h + dy;
                if (py >= fb_h) break;
                for (int dx = 0; dx < cell_w; dx++) {
                    int px = col * cell_w + dx;
                    if (px >= fb_w) break;
                    int off = (py * fb_w + px) * 3;
                    /* ITU-R BT.601 luminance */
                    double l = 0.299 * fb[off] + 0.587 * fb[off+1] + 0.114 * fb[off+2];
                    lum_sum += l;
                    count++;
                }
            }
            double lum = (count > 0) ? lum_sum / count : 255.0;
            /* Map luminance [0..255] → ramp index [0..ramp_len-1]
               (dark pixel → high index = dense char) */
            int idx = ramp_len - 1 - (int)(lum / 256.0 * ramp_len);
            if (idx < 0) idx = 0;
            if (idx >= ramp_len) idx = ramp_len - 1;
            fputc(ramp[idx], f);
        }
        fputc('\n', f);
    }

    fclose(f);
    return 0;
}

/* ═══════════════════════════════════════════════════════════════════
 *  MAIN
 * ═══════════════════════════════════════════════════════════════════ */

static void strip_ext(const char *in, char *out, int out_sz) {
    strncpy(out, in, out_sz - 1);
    out[out_sz - 1] = 0;
    char *dot = strrchr(out, '.');
    if (dot) *dot = 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
            "SmazkaVG Rasterizer v1.1\n"
            "Usage: %s <input.smazka> [width] [height]\n"
            "Outputs: <input>.bmp  <input>.svg  <input>.txt\n",
            argv[0]);
        return 1;
    }

    const char *inpath = argv[1];
    int w = (argc >= 3) ? atoi(argv[2]) : 512;
    int h = (argc >= 4) ? atoi(argv[3]) : 512;
    if (w < 64)  w = 64;
    if (h < 64)  h = 64;
    if (w > 4096) w = 4096;
    if (h > 4096) h = 4096;

    /* Parse */
    if (parse_file(inpath) != 0) return 1;
    fprintf(stderr, "Parsed: %d verts, %d edges, %d faces, %d strokes, %d nodes, %d diffusion curves\n",
            n_verts, n_edges, n_faces, n_strokes, n_nodes, n_diffusion);

    /* Resolve hierarchy */
    memset(world_valid, 0, sizeof(world_valid));
    for (int i = 0; i < n_nodes; i++)
        resolve_world(i, 0);

    /* Render */
    fb_alloc(w, h);
    render_scene();

    /* Build output paths */
    char base[512];
    strip_ext(inpath, base, sizeof(base));

    char bmp_path[560], svg_path[560], txt_path[560];
    snprintf(bmp_path, sizeof(bmp_path), "%s.bmp", base);
    snprintf(svg_path, sizeof(svg_path), "%s.svg", base);
    snprintf(txt_path, sizeof(txt_path), "%s.txt", base);

    /* Write outputs */
    if (write_bmp(bmp_path) == 0)
        fprintf(stderr, "BMP:  %s (%dx%d, %d bytes)\n", bmp_path, w, h,
                54 + w * h * 3);
    else
        fprintf(stderr, "ERROR writing BMP\n");

    if (write_svg(svg_path) == 0)
        fprintf(stderr, "SVG:  %s\n", svg_path);
    else
        fprintf(stderr, "ERROR writing SVG\n");

    if (write_ascii(txt_path) == 0)
        fprintf(stderr, "ASCII: %s\n", txt_path);
    else
        fprintf(stderr, "ERROR writing ASCII\n");

    fb_free();
    return 0;
}
