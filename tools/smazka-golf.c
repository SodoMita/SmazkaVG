/*
 * SmazkaVG code-golf dialect compiler (tools/smazka-golf)
 * ========================================================
 *
 * Compiles a byte-starved shorthand (.sg) into canonical Line-ASM (v1.3).
 * Every .sg construct expands 1:1; the output renders with the standard
 * rasterizer.
 *
 *   cc -O2 -o smazka-golf tools/smazka-golf.c
 *   ./smazka-golf face.sg > face.smazka
 *   ./smazka-raster face.smazka
 *
 * Dialect summary
 * ---------------
 *   ! comment (also #)
 *   v 10 20              vertex, auto-ID
 *   + 5 0                vertex relative to the previous one, auto-ID
 *   P x1 y1 x2 y2 ...    polygon: vertices + edges + closed face, auto-ID
 *   R x y w h            rectangle, auto-ID
 *   C cx cy r            circle (4 anchor vertices + 4 cubic Beziers), auto-ID
 *   E va vb [cp...]      edge between existing vertex ids, auto-ID
 *                        (type inferred: 0 cp=seg, 2 cp=quad, 4 cp=cubic)
 *   S eid color w0..     stroke on edge (color: name, #rgb, rrggbb, rrggbbaa)
 *   F eid... [color]     face (optional inline fill)
 *   M x vid...           mirror copies of vertices across the x axis (auto-ID)
 *   M y vid...           mirror copies across the y axis
 *   D dx dy vid...       translated copies (auto-ID)
 *
 * Palette: red green blue black white yellow cyan magenta orange pink
 *          purple brown gray silver maroon lime navy teal gold skin
 *
 * This addresses the golfability audit: ~45% of a naive file is manual
 * global-ID bookkeeping; here IDs are implicit; shapes are one token;
 * colors are 1-4 bytes; symmetry needs one command instead of duplicated
 * geometry.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <strings.h>

#define MAX_PTS 4096

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
    static const struct { const char *n; unsigned c; } pal[] = {
        { "black", 0x000000FF }, { "white", 0xFFFFFFFF }, { "red", 0xFF0000FF },
        { "green", 0x00FF00FF }, { "blue", 0x0000FFFF }, { "yellow", 0xFFFF00FF },
        { "cyan", 0x00FFFFFF }, { "magenta", 0xFF00FFFF }, { "orange", 0xFFA500FF },
        { "pink", 0xFFC0CBFF }, { "purple", 0x800080FF }, { "brown", 0xA52A2AFF },
        { "gray", 0x808080FF }, { "grey", 0x808080FF }, { "silver", 0xC0C0C0FF },
        { "maroon", 0x800000FF }, { "lime", 0x00FF00FF }, { "navy", 0x000080FF },
        { "teal", 0x008080FF }, { "gold", 0xFFD700FF }, { "skin", 0xFFE0D0FF },
        { NULL, 0 }
    };
    const char *p = s;
    if (*p == '#') p++;
    if (strchr(p, ':')) p = strchr(p, ':') + 1;      /* allow "color:name" */
    if (strchr(p, '=')) p = strchr(p, '=') + 1;      /* allow "color=name" */
    for (int i = 0; pal[i].n; i++)
        if (strcasecmp(p, pal[i].n) == 0) return pal[i].c;
    unsigned v = 0;
    if (sscanf(p, "%x", &v) != 1) { fprintf(stderr, "golf: bad color '%s'\n", s); v = 0xFF0000FF; }
    switch (strlen(p)) {
    case 3: return ((((v >> 8) & 0xF) * 0x11) << 24) | ((((v >> 4) & 0xF) * 0x11) << 16)
                  | (((v & 0xF) * 0x11) << 8) | 0xFF;
    case 6: return (v << 8) | 0xFF;
    default: return v;
    }
}

static int is_hex_color(const char *t) {
    size_t L = strlen(t);
    if (L != 3 && L != 6 && L != 8) return 0;
    for (size_t i = 0; i < L; i++)
        if (!((t[i] >= '0' && t[i] <= '9') || (t[i] >= 'a' && t[i] <= 'f') || (t[i] >= 'A' && t[i] <= 'F')))
            return 0;
    return 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "SmazkaVG golf dialect compiler\nUsage: %s <in.sg> [out.smazka]\n", argv[0]);
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
        for (char *q = p; *q; q++) if (*q == '!') { *q = 0; break; }   /* '!' is the only comment marker; '#' is a color prefix */

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
                /* 4 anchors at 0/90/180/270 degrees */
                int a[4];
                a[0] = addv(cx + r, cy);
                a[1] = addv(cx, cy + r);
                a[2] = addv(cx - r, cy);
                a[3] = addv(cx, cy - r);
                /* one cubic per quadrant, 2 control points each */
                double cps[4][4] = {
                    { cx + r, cy + k * r, cx + k * r, cy + r },   /* a0 -> a1 */
                    { cx - k * r, cy + r, cx - r, cy + k * r },   /* a1 -> a2 */
                    { cx - r, cy - k * r, cx - k * r, cy - r },   /* a2 -> a3 */
                    { cx + k * r, cy - r, cx + r, cy - k * r }    /* a3 -> a0 */
                };
                int fe[4];
                for (int i = 0; i < 4; i++) {
                    int eid = ne++;
                    emit("e %d %d %d type=cubic %.4f %.4f %.4f %.4f\n",
                         eid, a[i], a[(i + 1) % 4], cps[i][0], cps[i][1], cps[i][2], cps[i][3]);
                    fe[i] = eid;
                }
                emit("f %d %d %d %d %d\n", nf++, fe[0], fe[1], fe[2], fe[3]);
                continue;   /* already emitted edges + face */
            } else { /* P */
                if (n < 6 || n % 2 != 0) { fprintf(stderr, "golf: P needs x1 y1 x2 y2 ...\n"); continue; }
                int m = n / 2;
                for (int i = 0; i < m; i++) addv(vals[2 * i], vals[2 * i + 1]);
            }
            /* shared polygon close: edges + face */
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
            /* optional trailing fill color */
            char col[64] = "";
            char *t2 = p;
            int nt = 0;
            while (*t2) {
                while (*t2 == ' ' || *t2 == '\t') t2++;
                if (!*t2) break;
                char tok[64];
                if (sscanf(t2, "%63s", tok) == 1) {
                    if (nt >= n && is_hex_color(tok)) strncpy(col, tok, 63);
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
        } else if (cmd == 'K') {   /* keyframe: K <node> <time> [st=<n>] <tx> <ty> <rot> [sx] [sy] [skew] */
            int node, st = -1; double t, vals[6] = {0,0,0,1,1,0};
            char *tkn = p;
            int got = 0;
            if (sscanf(tkn, "%d %lf", &node, &t) != 2) { fprintf(stderr, "golf: K needs node time\n"); continue; }
            while (*tkn && *tkn != ' ' && *tkn != '\t') tkn++;
            while (*tkn == ' ' || *tkn == '\t') tkn++;
            while (*tkn && *tkn != ' ' && *tkn != '\t') tkn++;
            while (*tkn == ' ' || *tkn == '\t') tkn++;
            if (strncmp(tkn, "st=", 3) == 0) { int ns; if (sscanf(tkn + 3, "%d%n", &st, &ns) == 1) tkn += 3 + ns; while (*tkn == ' ' || *tkn == '\t') tkn++; }
            while (*tkn && got < 6) {
                double v; int n;
                if (sscanf(tkn, "%lf%n", &v, &n) == 1 && n > 0) { vals[got++] = v; tkn += n; while (*tkn == ' ' || *tkn == '\t') tkn++; }
                else break;
            }
            if (got < 3) { fprintf(stderr, "golf: K needs tx ty rot\n"); continue; }
            if (st >= 0) emit("k %d %d %.4f st=%d %.4f %.4f %.4f", nkf++, node, t, st, vals[0], vals[1], vals[2]);
            else emit("k %d %d %.4f %.4f %.4f %.4f", nkf++, node, t, vals[0], vals[1], vals[2]);
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
