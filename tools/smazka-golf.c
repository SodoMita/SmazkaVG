/*
 * SmazkaVG code-golf dialect compiler & minifier (tools/smazka-golf)
 * =================================================================
 *
 * 1. Compiles a byte-starved shorthand (.sg) into canonical Line-ASM.
 * 2. Minifies/compresses any Line-ASM or xauthor document into ultra-compact
 *    text representation (-c / --compact).
 *
 * Usage:
 *   smazka-golf face.sg face.smazka         # compile shorthand to Line-ASM
 *   smazka-golf -c face.smazka face.sg      # minify Line-ASM to compact .sg
 *
 * Dialect summary
 * ---------------
 *   ! comment (also #)
 *   v x y                vertex (auto-ID)
 *   + dx dy              vertex relative to previous
 *   P x1 y1 x2 y2 ...    polygon: vertices + edges + closed face (auto-ID)
 *   R x y w h            rectangle (auto-ID)
 *   C cx cy r            circle (4 anchor vertices + 4 cubic Beziers)
 *   E va vb [cp...]      edge between vertex ids
 *   S eid color w0...    stroke on edge
 *   F eid... [color]     face (optional inline fill)
 *   M x|y vid...         mirror copies of vertices across axis
 *   D dx dy vid...       translated copies
 *   K node t tx ty rot   keyframe
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <strings.h>

#include "../src/xauthor.h"

#define MAX_PTS 32768
#define MAX_EDGES 32768
#define MAX_FACES 1024
#define MAX_STROKES 32768

static FILE *g_out;
static int nv, ne, nf, ns, nkf;
static double vx[MAX_PTS], vy[MAX_PTS];

static void emit(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_out, fmt, ap);
    va_end(ap);
}

static int addv(double x, double y) {
    if (nv >= MAX_PTS) { fprintf(stderr, "golf: too many vertices\n"); exit(1); }
    vx[nv] = x; vy[nv] = y;
    emit("v %d %.4f %.4f\n", nv, x, y);
    return nv++;
}

static unsigned parse_color(const char *s) {
    char norm[64];
    xa_norm_color(s, norm, sizeof(norm));
    unsigned val = 0xFFFFFFFF;
    if (sscanf(norm, "%x", &val) == 1) {
        if (strlen(norm) == 6) val = (val << 8) | 0xFF;
    }
    return val;
}

static void format_color(const char *s, char *out, size_t cap) {
    static const struct { const char *n; const char *c; } pal[] = {
        { "black", "000000FF" }, { "white", "FFFFFF" }, { "white", "FFFFFFFF" },
        { "red", "FF0000FF" }, { "green", "00FF00FF" }, { "blue", "0000FFFF" },
        { "yellow", "FFFF00FF" }, { "cyan", "00FFFFFF" }, { "magenta", "FF00FFFF" },
        { "orange", "FFA500FF" }, { "pink", "FFC0CBFF" }, { "purple", "800080FF" },
        { "brown", "A52A2AFF" }, { "gray", "808080FF" }, { "silver", "C0C0C0FF" },
        { "skin", "FFE0D0FF" }, { NULL, NULL }
    };
    char norm[64];
    xa_norm_color(s, norm, sizeof(norm));
    for (int i = 0; pal[i].n; i++) {
        if (strcasecmp(norm, pal[i].c) == 0 ||
            (strlen(norm) == 6 && strncasecmp(pal[i].c, norm, 6) == 0)) {
            snprintf(out, cap, "%s", pal[i].n);
            return;
        }
    }
    if (strlen(norm) == 6 || (strlen(norm) == 8 && strcasecmp(norm + 6, "FF") == 0)) {
        if (norm[0] == norm[1] && norm[2] == norm[3] && norm[4] == norm[5]) {
            snprintf(out, cap, "#%c%c%c", norm[0], norm[2], norm[4]);
            return;
        }
        snprintf(out, cap, "#%.6s", norm);
        return;
    }
    snprintf(out, cap, "#%s", norm);
}

static void fmt_num(double v, char *out, size_t cap) {
    if (fabs(v - round(v)) < 1e-5) {
        snprintf(out, cap, "%.0f", v);
    } else {
        snprintf(out, cap, "%.4f", v);
        char *p = out + strlen(out) - 1;
        while (p > out && *p == '0') { *p = 0; p--; }
        if (p > out && *p == '.') *p = 0;
    }
}

/* ---------------- minifier (-c mode) ---------------- */
typedef struct { double x, y; } Pt;
typedef struct { int v0, v1; char type[16]; double cps[8]; int nc; } EdgeData;
typedef struct { int eids[64]; int ne; char fill[64]; } FaceData;
typedef struct { int eid; char col[64]; double w[16]; int nw; char cap[16]; } StrokeData;

static int minify(const char *in_path, const char *out_path) {
    int nerr = 0;
    char *expanded = xa_read_expand(in_path, stderr, &nerr);
    if (!expanded) { fprintf(stderr, "golf: cannot read %s\n", in_path); return 1; }

    FILE *out = stdout;
    if (out_path && strcmp(out_path, "-") != 0) {
        out = fopen(out_path, "w");
        if (!out) {
            fprintf(stderr, "golf: cannot write %s\n", out_path);
            free(expanded);
            return 1;
        }
    }

    Pt *verts = calloc(MAX_PTS, sizeof(Pt));
    EdgeData *edges = calloc(MAX_EDGES, sizeof(EdgeData));
    FaceData *faces = calloc(MAX_FACES, sizeof(FaceData));
    StrokeData *strokes = calloc(MAX_STROKES, sizeof(StrokeData));
    if (!verts || !edges || !faces || !strokes) {
        fprintf(stderr, "golf: out of memory\n");
        free(expanded);
        if (verts) free(verts);
        if (edges) free(edges);
        if (faces) free(faces);
        if (strokes) free(strokes);
        return 1;
    }

    int num_v = 0, num_e = 0, num_f = 0, num_s = 0;

    char *cur = expanded;
    char line[1024];
    while (xa_line(&cur, line, sizeof(line))) {
        char *copy = strdup(line);
        char *tv[128];
        int nt = xa_tok(copy, tv, 128);
        if (nt == 0) { free(copy); continue; }

        char cmd = tv[0][0];
        if (cmd == 'v' && nt >= 4) {
            int id = atoi(tv[1]);
            if (id >= 0 && id < MAX_PTS) {
                verts[id].x = atof(tv[2]);
                verts[id].y = atof(tv[3]);
                if (id >= num_v) num_v = id + 1;
            }
        } else if (cmd == 'e' && nt >= 4) {
            int id = atoi(tv[1]);
            if (id >= 0 && id < MAX_EDGES) {
                edges[id].v0 = atoi(tv[2]);
                edges[id].v1 = atoi(tv[3]);
                snprintf(edges[id].type, sizeof(edges[id].type), "seg");
                edges[id].nc = 0;
                for (int i = 4; i < nt; i++) {
                    if (strncmp(tv[i], "type=", 5) == 0) {
                        snprintf(edges[id].type, sizeof(edges[id].type), "%s", tv[i] + 5);
                    } else if (xa_is_float(tv[i]) && edges[id].nc < 8) {
                        edges[id].cps[edges[id].nc++] = atof(tv[i]);
                    }
                }
                if (id >= num_e) num_e = id + 1;
            }
        } else if (cmd == 'f' && nt >= 2) {
            int id = atoi(tv[1]);
            if (id >= 0 && id < MAX_FACES) {
                faces[id].ne = 0;
                snprintf(faces[id].fill, sizeof(faces[id].fill), "FFFFFF");
                for (int i = 2; i < nt; i++) {
                    if (xa_is_num(tv[i])) {
                        if (faces[id].ne < 64) faces[id].eids[faces[id].ne++] = atoi(tv[i]);
                    } else {
                        snprintf(faces[id].fill, sizeof(faces[id].fill), "%s", tv[i]);
                    }
                }
                if (id >= num_f) num_f = id + 1;
            }
        } else if (cmd == 's' && nt >= 3) {
            int id = atoi(tv[1]);
            if (id >= 0 && id < MAX_STROKES) {
                strokes[id].eid = atoi(tv[2]);
                snprintf(strokes[id].col, sizeof(strokes[id].col), "%s", nt >= 4 ? tv[3] : "000000FF");
                snprintf(strokes[id].cap, sizeof(strokes[id].cap), "round");
                strokes[id].nw = 0;
                for (int i = 4; i < nt; i++) {
                    if (strncmp(tv[i], "cap=", 4) == 0) {
                        snprintf(strokes[id].cap, sizeof(strokes[id].cap), "%s", tv[i] + 4);
                    } else if (xa_is_float(tv[i]) && strokes[id].nw < 16) {
                        strokes[id].w[strokes[id].nw++] = atof(tv[i]);
                    }
                }
                if (id >= num_s) num_s = id + 1;
            }
        } else {
            fprintf(out, "%s\n", line);
        }
        free(copy);
    }

    /* Track which edges have strokes */
    int stroke_for_e[MAX_EDGES];
    memset(stroke_for_e, -1, sizeof(stroke_for_e));
    for (int i = 0; i < num_s; i++) {
        if (strokes[i].eid >= 0 && strokes[i].eid < MAX_EDGES) {
            stroke_for_e[strokes[i].eid] = i;
        }
    }

    unsigned char claimed_v[MAX_PTS] = {0};
    unsigned char claimed_e[MAX_EDGES] = {0};

    /* Shape detection on faces */
    for (int i = 0; i < num_f; i++) {
        FaceData *fd = &faces[i];
        if (fd->ne == 0) continue;
        char col_str[64];
        format_color(fd->fill, col_str, sizeof(col_str));

        /* Check stroke width & color on face edges */
        double face_sw = -1;
        int uniform_strokes = (fd->ne > 0);
        char face_scol[64] = "";
        for (int k = 0; k < fd->ne; k++) {
            int sid = stroke_for_e[fd->eids[k]];
            if (sid < 0) { uniform_strokes = 0; break; }
            double w = (strokes[sid].nw > 0) ? strokes[sid].w[0] : 0;
            if (face_sw < 0) {
                face_sw = w;
                snprintf(face_scol, sizeof(face_scol), "%s", strokes[sid].col);
            } else if (fabs(face_sw - w) > 1e-4 || strcasecmp(face_scol, strokes[sid].col) != 0) {
                uniform_strokes = 0; break;
            }
        }

        /* Check for Rectangle: 4 straight edges forming axis-aligned rect */
        if (fd->ne == 4) {
            int e0 = fd->eids[0], e1 = fd->eids[1], e2 = fd->eids[2], e3 = fd->eids[3];
            if (edges[e0].nc == 0 && edges[e1].nc == 0 && edges[e2].nc == 0 && edges[e3].nc == 0) {
                int v0 = edges[e0].v0, v1 = edges[e0].v1, v2 = edges[e1].v1, v3 = edges[e2].v1;
                double x0 = verts[v0].x, y0 = verts[v0].y;
                double x1 = verts[v1].x, y1 = verts[v1].y;
                double x2 = verts[v2].x, y2 = verts[v2].y;
                double x3 = verts[v3].x, y3 = verts[v3].y;
                if (fabs(y0 - y1) < 1e-4 && fabs(x1 - x2) < 1e-4 && fabs(y2 - y3) < 1e-4 && fabs(x3 - x0) < 1e-4) {
                    char sx0[32], sy0[32], sw[32], sh[32];
                    fmt_num(x0, sx0, sizeof(sx0)); fmt_num(y0, sy0, sizeof(sy0));
                    fmt_num(x1 - x0, sw, sizeof(sw)); fmt_num(y2 - y1, sh, sizeof(sh));
                    fprintf(out, "R %s %s %s %s %s", sx0, sy0, sw, sh, col_str);
                    if (uniform_strokes && face_sw > 0) {
                        char ssw[32]; fmt_num(face_sw, ssw, sizeof(ssw));
                        fprintf(out, " sw=%s", ssw);
                        for (int k = 0; k < 4; k++) claimed_e[fd->eids[k]] = 1;
                    }
                    fprintf(out, "\n");
                    claimed_v[v0] = claimed_v[v1] = claimed_v[v2] = claimed_v[v3] = 1;
                    continue;
                }
            }
        }

        /* Check for Circle: 4 cubic Bezier edges forming a circle */
        if (fd->ne == 4) {
            int e0 = fd->eids[0], e1 = fd->eids[1], e2 = fd->eids[2], e3 = fd->eids[3];
            if (edges[e0].nc == 4 && edges[e1].nc == 4 && edges[e2].nc == 4 && edges[e3].nc == 4) {
                int v0 = edges[e0].v0, v1 = edges[e0].v1, v2 = edges[e1].v1, v3 = edges[e2].v1;
                double x0 = verts[v0].x, y0 = verts[v0].y;
                double x1 = verts[v1].x, y1 = verts[v1].y;
                double x2 = verts[v2].x, y2 = verts[v2].y;
                double cx = (x0 + x2) / 2.0;
                double cy = (y0 + y2) / 2.0;
                double r0 = hypot(x0 - cx, y0 - cy);
                double r1 = hypot(x1 - cx, y1 - cy);
                if (fabs(r0 - r1) < 1e-2 && r0 > 0.5) {
                    char scx[32], scy[32], sr[32];
                    fmt_num(cx, scx, sizeof(scx));
                    fmt_num(cy, scy, sizeof(scy));
                    fmt_num(r0, sr, sizeof(sr));
                    fprintf(out, "C %s %s %s %s", scx, scy, sr, col_str);
                    if (uniform_strokes && face_sw > 0) {
                        char ssw[32]; fmt_num(face_sw, ssw, sizeof(ssw));
                        fprintf(out, " sw=%s", ssw);
                        for (int k = 0; k < 4; k++) claimed_e[fd->eids[k]] = 1;
                    }
                    fprintf(out, "\n");
                    claimed_v[v0] = claimed_v[v1] = claimed_v[v2] = claimed_v[v3] = 1;
                    continue;
                }
            }
        }

        /* Polygon: arbitrary closed loop */
        int straight = 1;
        for (int k = 0; k < fd->ne; k++) if (edges[fd->eids[k]].nc != 0) { straight = 0; break; }
        if (straight && fd->ne >= 3) {
            fprintf(out, "P");
            for (int k = 0; k < fd->ne; k++) {
                int eid = fd->eids[k];
                int vid = edges[eid].v0;
                char sx[32], sy[32];
                fmt_num(verts[vid].x, sx, sizeof(sx));
                fmt_num(verts[vid].y, sy, sizeof(sy));
                fprintf(out, " %s %s", sx, sy);
                claimed_v[vid] = 1;
            }
            fprintf(out, " %s", col_str);
            if (uniform_strokes && face_sw > 0) {
                char ssw[32]; fmt_num(face_sw, ssw, sizeof(ssw));
                fprintf(out, " sw=%s", ssw);
                for (int k = 0; k < fd->ne; k++) claimed_e[fd->eids[k]] = 1;
            }
            fprintf(out, "\n");
            continue;
        }

        /* Fallback: F line */
        fprintf(out, "F");
        for (int k = 0; k < fd->ne; k++) fprintf(out, " %d", fd->eids[k]);
        fprintf(out, " %s\n", col_str);
    }

    /* Unclaimed standalone vertices */
    for (int i = 0; i < num_v; i++) {
        if (!claimed_v[i]) {
            char sx[32], sy[32];
            fmt_num(verts[i].x, sx, sizeof(sx));
            fmt_num(verts[i].y, sy, sizeof(sy));
            fprintf(out, "v %s %s\n", sx, sy);
        }
    }

    /* Unclaimed standalone edges */
    for (int i = 0; i < num_e; i++) {
        if (!claimed_e[i]) {
            EdgeData *ed = &edges[i];
            fprintf(out, "E %d %d", ed->v0, ed->v1);
            for (int k = 0; k < ed->nc; k++) {
                char scp[32]; fmt_num(ed->cps[k], scp, sizeof(scp));
                fprintf(out, " %s", scp);
            }
            fprintf(out, "\n");
        }
    }

    /* Strokes on unclaimed edges */
    for (int i = 0; i < num_s; i++) {
        StrokeData *sd = &strokes[i];
        if (sd->eid >= 0 && sd->eid < MAX_EDGES && claimed_e[sd->eid]) continue;
        char col_str[64];
        format_color(sd->col, col_str, sizeof(col_str));
        fprintf(out, "S %d %s", sd->eid, col_str);
        for (int k = 0; k < sd->nw; k++) {
            char sw[32]; fmt_num(sd->w[k], sw, sizeof(sw));
            fprintf(out, " %s", sw);
        }
        if (strcmp(sd->cap, "round") != 0) fprintf(out, " cap=%s", sd->cap);
        fprintf(out, "\n");
    }

    free(expanded);
    free(verts); free(edges); free(faces); free(strokes);
    if (out != stdout) fclose(out);
    return 0;
}

int main(int argc, char **argv) {
    if (argc >= 2 && (strcmp(argv[1], "-c") == 0 || strcmp(argv[1], "--compact") == 0)) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s -c <in.smazka> [out.sg]\n", argv[0]);
            return 1;
        }
        return minify(argv[2], argc >= 4 ? argv[3] : "-");
    }

    if (argc < 2) {
        fprintf(stderr, "SmazkaVG golf dialect compiler & minifier\nUsage:\n  %s <in.sg> [out.smazka]\n  %s -c <in.smazka> [out.sg]\n", argv[0], argv[0]);
        return 1;
    }
    FILE *f = fopen(argv[1], "r");
    if (!f) { fprintf(stderr, "golf: cannot open %s\n", argv[1]); return 1; }
    g_out = stdout;
    if (argc >= 3) {
        g_out = fopen(argv[2], "w");
        if (!g_out) { fprintf(stderr, "golf: cannot write %s\n", argv[2]); return 1; }
    }

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '\n' || *p == '\r' || *p == '!') continue;
        char cmd = *p++;
        while (*p == ' ' || *p == '\t') p++;
        char *nl = strchr(p, '\n'); if (nl) *nl = 0;
        for (char *q = p; *q; q++) if (*q == '!') { *q = 0; break; }

        if (cmd == 'v') {
            double x, y;
            if (sscanf(p, "%lf %lf", &x, &y) == 2) addv(x, y);
        } else if (cmd == '+') {
            double dx, dy;
            if (nv == 0) { fprintf(stderr, "golf: '+' before any 'v'\n"); continue; }
            if (sscanf(p, "%lf %lf", &dx, &dy) == 2) addv(vx[nv - 1] + dx, vy[nv - 1] + dy);
        } else if (cmd == 'P' || cmd == 'R' || cmd == 'C') {
            double vals[64]; int n = 0;
            char *t = p;
            while (*t && n < 64) {
                double v;
                if (sscanf(t, "%lf", &v) == 1) {
                    vals[n++] = v;
                    while (*t && *t != ' ' && *t != '\t') t++;
                    while (*t == ' ' || *t == '\t') t++;
                } else break;
            }
            int first = nv;
            if (cmd == 'R') {
                if (n < 4) { fprintf(stderr, "golf: R needs x y w h\n"); continue; }
                double x = vals[0], y = vals[1], w = vals[2], h = vals[3];
                addv(x, y); addv(x + w, y); addv(x + w, y + h); addv(x, y + h);
            } else if (cmd == 'C') {
                if (n < 3) { fprintf(stderr, "golf: C needs cx cy r\n"); continue; }
                double cx = vals[0], cy = vals[1], r = vals[2];
                const double k = 0.5522847498307936;
                int a[4];
                a[0] = addv(cx + r, cy);
                a[1] = addv(cx, cy + r);
                a[2] = addv(cx - r, cy);
                a[3] = addv(cx, cy - r);
                double cps[4][4] = {
                    { cx + r, cy + k * r, cx + k * r, cy + r },
                    { cx - k * r, cy + r, cx - r, cy + k * r },
                    { cx - r, cy - k * r, cx - k * r, cy - r },
                    { cx + k * r, cy - r, cx + r, cy - k * r }
                };
                int fe[4];
                for (int i = 0; i < 4; i++) {
                    int eid = ne++;
                    emit("e %d %d %d type=cubic %.4f %.4f %.4f %.4f\n",
                         eid, a[i], a[(i + 1) % 4], cps[i][0], cps[i][1], cps[i][2], cps[i][3]);
                    fe[i] = eid;
                }
                emit("f %d %d %d %d %d\n", nf++, fe[0], fe[1], fe[2], fe[3]);
                continue;
            } else {
                if (n < 6 || n % 2 != 0) { fprintf(stderr, "golf: P needs x1 y1 x2 y2 ...\n"); continue; }
                int m = n / 2;
                for (int i = 0; i < m; i++) addv(vals[2 * i], vals[2 * i + 1]);
            }
            int cnt = nv - first;
            if (cnt < 3) continue;
            int fe[64];
            for (int i = 0; i < cnt; i++) {
                fe[i] = ne++;
                emit("e %d %d %d\n", fe[i], first + i, first + ((i + 1) % cnt));
            }
            emit("f %d", nf++);
            for (int i = 0; i < cnt; i++) emit(" %d", fe[i]);
            emit("\n");
        } else if (cmd == 'E') {
            int va, vb;
            double cps[8]; int nc = 0;
            char *t = p;
            if (sscanf(t, "%d %d", &va, &vb) != 2) { fprintf(stderr, "golf: E needs va vb\n"); continue; }
            for (int i = 0; i < 2; i++) { while (*t && *t != ' ' && *t != '\t') t++; while (*t == ' ' || *t == '\t') t++; }
            while (*t && nc < 8) {
                while (*t == ' ' || *t == '\t') t++;
                if (!*t) break;
                double v;
                if (sscanf(t, "%lf", &v) == 1) { cps[nc++] = v; while (*t && *t != ' ' && *t != '\t') t++; }
                else break;
            }
            int eid = ne++;
            if (nc == 2) emit("e %d %d %d type=quad %.4f %.4f\n", eid, va, vb, cps[0], cps[1]);
            else if (nc >= 4) emit("e %d %d %d type=cubic %.4f %.4f %.4f %.4f\n", eid, va, vb, cps[0], cps[1], cps[2], cps[3]);
            else emit("e %d %d %d\n", eid, va, vb);
        } else if (cmd == 'S') {
            int eid; char col[64];
            if (sscanf(p, "%d %63s", &eid, col) != 2) { fprintf(stderr, "golf: S needs eid color\n"); continue; }
            char *t = p;
            for (int i = 0; i < 2; i++) { while (*t && *t != ' ' && *t != '\t') t++; while (*t == ' ' || *t == '\t') t++; }
            unsigned c = parse_color(col);
            emit("s %d %d %08X", ns++, eid, c);
            while (*t) {
                while (*t == ' ' || *t == '\t') t++;
                if (!*t) break;
                double w; int n;
                if (sscanf(t, "%lf%n", &w, &n) == 1 && n > 0) { emit(" %.4f", w); t += n; } else break;
            }
            emit("\n");
        } else if (cmd == 'F') {
            int ids[128]; int n = 0;
            char *t = p;
            while (*t && n < 128) {
                while (*t == ' ' || *t == '\t') t++;
                if (!*t) break;
                int v;
                if (sscanf(t, "%d", &v) == 1) {
                    ids[n++] = v;
                    while (*t && *t != ' ' && *t != '\t') t++;
                } else break;
            }
            if (n < 3) { fprintf(stderr, "golf: F needs >=3 edge ids\n"); continue; }
            char col[64] = "";
            char *t2 = p;
            int nt = 0;
            while (*t2) {
                while (*t2 == ' ' || *t2 == '\t') t2++;
                if (!*t2) break;
                char tok[64];
                if (sscanf(t2, "%63s", tok) == 1) {
                    if (nt >= n && (xa_is_hexcol(tok) || *tok == '#')) snprintf(col, sizeof(col), "%s", tok);
                    nt++;
                    while (*t2 && *t2 != ' ' && *t2 != '\t') t2++;
                } else break;
            }
            emit("f %d", nf++);
            for (int i = 0; i < n; i++) emit(" %d", ids[i]);
            if (*col) {
                unsigned c = parse_color(col);
                size_t L = strlen(col);
                if (L == 8) emit(" %08X", c);
                else if (L == 6) emit(" %06X", c >> 8);
                else emit(" %03X", (c >> 24) & 0xFF);
            }
            emit("\n");
        } else if (cmd == 'M' || cmd == 'D') {
            char axis = 't'; double offx = 0, offy = 0;
            char *t = p;
            if (cmd == 'M') {
                if (sscanf(t, "%c", &axis) != 1 || (axis != 'x' && axis != 'y')) {
                    fprintf(stderr, "golf: M needs x|y\n"); continue;
                }
                while (*t && *t != ' ' && *t != '\t') t++;
            } else {
                if (sscanf(t, "%lf %lf", &offx, &offy) != 2) { fprintf(stderr, "golf: D needs dx dy\n"); continue; }
                for (int i = 0; i < 2; i++) { while (*t && *t != ' ' && *t != '\t') t++; while (*t == ' ' || *t == '\t') t++; }
            }
            while (*t) {
                while (*t == ' ' || *t == '\t') t++;
                if (!*t) break;
                int vid, nread;
                if (sscanf(t, "%d%n", &vid, &nread) == 1) {
                    if (vid < 0 || vid >= nv) { fprintf(stderr, "golf: vertex %d unknown\n", vid); break; }
                    double nx = vx[vid], ny = vy[vid];
                    if (cmd == 'M') {
                        if (axis == 'x') ny = -ny;
                        else nx = -nx;
                    } else { nx += offx; ny += offy; }
                    addv(nx, ny);
                    t += nread;
                } else break;
            }
        } else if (cmd == 'K') {
            int node; double t, vals[6] = {0,0,0,1,1,0};
            char *tkn = p;
            int got = 0;
            if (sscanf(tkn, "%d %lf", &node, &t) != 2) { fprintf(stderr, "golf: K needs node time\n"); continue; }
            while (*tkn && *tkn != ' ' && *tkn != '\t') tkn++;
            while (*tkn == ' ' || *tkn == '\t') tkn++;
            while (*tkn && *tkn != ' ' && *tkn != '\t') tkn++;
            while (*tkn == ' ' || *tkn == '\t') tkn++;
            while (*tkn && got < 6) {
                double v; int n;
                if (sscanf(tkn, "%lf%n", &v, &n) == 1 && n > 0) { vals[got++] = v; tkn += n; while (*tkn == ' ' || *tkn == '\t') tkn++; }
                else break;
            }
            if (got < 3) { fprintf(stderr, "golf: K needs tx ty rot\n"); continue; }
            emit("k %d %d %.4f %.4f %.4f %.4f", nkf++, node, t, vals[0], vals[1], vals[2]);
            if (got >= 4) emit(" %.4f", vals[3]);
            if (got >= 5) emit(" %.4f", vals[4]);
            if (got >= 6) emit(" %.4f", vals[5]);
            emit("\n");
        } else {
            fprintf(stderr, "golf: unknown command '%c'\n", cmd);
        }
    }
    fclose(f);
    if (g_out != stdout) fclose(g_out);
    return 0;
}
