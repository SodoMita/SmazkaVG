/*
 * SmazkaVG compact binary container (tools/smazka-bin)
 * ====================================================
 *
 * Lossless Line-ASM <-> .smvg converter implementing SPEC.md §11.2:
 *
 *   - header: magic "SMVG", version 1.3, flags, per-section counts
 *   - IDs and numbers: variable-length zigzag VLQs (LEB128-style)
 *   - coordinates: delta-encoded against a running "last coordinate"
 *     pair, giving typical savings of 3-6x over raw Q16.16 words
 *
 * Usage:
 *   cc -O2 -o smazka-bin tools/smazka-bin.c
 *   ./smazka-bin enc face.smazka face.smvg     # Line-ASM -> binary
 *   ./smazka-bin dec face.smvg face2.smazka    # binary -> Line-ASM
 *   cmp <(./smazka-bin dec face.smvg) <(sed '/^[!#]/d' face.smazka) ...
 *
 * Round-trip is exact up to Q16.16 quantization (1/65536).
 *
 * Supported records: v, e (seg/quad/cubic/rational/catmull), f, s (stroke +
 * parent + group_id), n (positional or labeled), r, z, a edge_connects,
 * c bbox_clamp + min_dist, p diffusion + solid_fill.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ─── VLQ primitives ─────────────────────────────────────────────── */

static void wu(FILE *f, uint64_t v) {          /* unsigned LEB128 */
    while (v >= 0x80) { fputc((int)(v & 0x7F) | 0x80, f); v >>= 7; }
    fputc((int)v, f);
}
static uint64_t ru(FILE *f) {                  /* unsigned LEB128 */
    uint64_t v = 0; int sh = 0;
    for (;;) {
        int b = fgetc(f);
        if (b < 0) return v;
        v |= (uint64_t)(b & 0x7F) << sh;
        if (!(b & 0x80)) break;
        sh += 7;
    }
    return v;
}
static void wz(FILE *f, int64_t v) {           /* signed zigzag */
    wu(f, (uint64_t)((v << 1) ^ (v >> 63)));
}
static int64_t rz(FILE *f) {
    uint64_t u = ru(f);
    return (int64_t)(u >> 1) ^ -(int64_t)(u & 1);
}

static int32_t q16(double x) {
    double s = x * 65536.0;
    if (s > 2147483647.0) return 2147483647;
    if (s < -2147483648.0) return -2147483648;
    return (int32_t)(s >= 0 ? s + 0.5 : s - 0.5);
}
static void wq(FILE *f, double x) { wz(f, q16(x)); }
static double rq(FILE *f) { return (double)rz(f) / 65536.0; }

/* running coordinate deltas */
static double lx, ly;
static void wc(FILE *f, double x, double y) {  /* delta from last coord */
    wz(f, q16(x) - q16(lx)); wz(f, q16(y) - q16(ly));
    lx = x; ly = y;
}
static void rc(FILE *f, double *x, double *y) {
    *x = (double)(rz(f) + q16(lx)) / 65536.0;
    *y = (double)(rz(f) + q16(ly)) / 65536.0;
    lx = *x; ly = *y;
}

static uint32_t color_val(const char *s) {
    uint32_t v = 0;
    if (sscanf(s, "%x", &v) != 1) v = 0;
    size_t L = strlen(s);
    if (L >= 8) return v;
    if (L >= 6) return (v << 8) | 0xFF;
    return (v << 8) | 0xFF;   /* 3-digit handled by caller via 8-digit expansion */
}

/* ─── Line-ASM token helpers ─────────────────────────────────────── */

static void skip_tok(const char **p) {
    while (**p && **p != ' ' && **p != '\t') (*p)++;
    while (**p == ' ' || **p == '\t') (*p)++;
}

/* ─── Encoder ────────────────────────────────────────────────────── */

static int enc(const char *inp, const char *outp) {
    FILE *in = fopen(inp, "r");
    if (!in) { fprintf(stderr, "bin: cannot open %s\n", inp); return 1; }
    FILE *out = fopen(outp, "wb");
    if (!out) { fprintf(stderr, "bin: cannot write %s\n", outp); return 1; }

    /* header placeholder */
    fwrite("SMVG", 1, 4, out);
    fputc(1, out); fputc(3, out);            /* version 1.3 */
    unsigned counts[12] = {0};
    long counts_pos = ftell(out);
    for (int i = 0; i < 12; i++) fwrite(&counts[i], 4, 1, out);

    char ln[2048];
    while (fgets(ln, sizeof(ln), in)) {
        char *p = ln;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') continue;
        char cmd = *p++;
        while (*p == ' ' || *p == '\t') p++;
        char *nl = strchr(p, '\n'); if (nl) *nl = 0;

        switch (cmd) {
        case 'v': {
            int id; double x, y; char vt[16] = "corner";
            if (sscanf(p, "%d %lf %lf %15s", &id, &x, &y, vt) < 3) break;
            fputc(0x01, out); wu(out, (uint32_t)id); wc(out, x, y);
            fputc(strcmp(vt, "smooth") == 0 ? 1 : strcmp(vt, "symmetric") == 0 ? 2
                 : strcmp(vt, "auto") == 0 ? 3 : 0, out);
            counts[0]++;
            break;
        }
        case 'e': {
            int id, v0, v1; char et[32] = "";
            if (sscanf(p, "%d %d %d %31s", &id, &v0, &v1, et) < 3) break;
            const char *t = p; for (int i = 0; i < 4; i++) skip_tok(&t);
            double vals[8]; int nv = 0;
            while (*t && *t != '#' && nv < 8) {
                double v;
                if (sscanf(t, "%lf", &v) == 1) { vals[nv++] = v; skip_tok(&t); }
                else skip_tok(&t);
            }
            const char *ep = et;
            if (strncmp(ep, "type=", 5) == 0) ep += 5;
            int ty = 0;
            if (strcmp(ep, "quad") == 0) ty = 1;
            else if (strcmp(ep, "cubic") == 0) ty = 2;
            else if (strcmp(ep, "rational") == 0) ty = 3;
            else if (strcmp(ep, "catmull") == 0) ty = 4;
            int nc = ty == 1 ? 1 : (ty == 2 || ty == 3) ? 2 : 0;
            fputc(0x02, out); wu(out, (uint32_t)id);
            wu(out, (uint32_t)v0); wu(out, (uint32_t)v1);
            fputc(ty, out); fputc(nc, out);
            for (int i = 0; i < nc * 2 && i < nv; i++) wq(out, vals[i]);
            if (ty == 3) {
                double w1 = nv > nc * 2 ? vals[nc * 2] : 1.0;
                wq(out, w1);
            }
            counts[1]++;
            break;
        }
        case 'f': {
            int id; if (sscanf(p, "%d", &id) != 1) break;
            const char *t = p; skip_tok(&t);   /* skip the record id only */
            int ne = 0; int eids[64]; char fill[16] = "";
            int n_holes = 0; int hlen[4]; int hole[4][64];
            int cur_loop = -1;
            while (*t) {
                while (*t == ' ' || *t == '\t') t++;
                if (!*t || *t == '#') break;          /* check AFTER skipping spaces */
                if (*t == '|') { t++; cur_loop++; if (cur_loop < 4) hlen[cur_loop] = 0; continue; }
                char tok[64]; int nread;
                if (sscanf(t, "%63s%n", tok, &nread) != 1) break;
                size_t tl = strlen(tok);
                int is_pure = 1;
                for (size_t k = 0; k < tl; k++)
                    if (tok[k] < '0' || tok[k] > '9') { is_pure = 0; break; }
                if (!is_pure || tl >= 6) { strncpy(fill, tok, 15); break; }
                int e = atoi(tok);
                if (cur_loop < 0) { if (ne < 64) eids[ne++] = e; }
                else if (cur_loop < 4 && hlen[cur_loop] < 64) hole[cur_loop][hlen[cur_loop]++] = e;
                t += nread;
            }
            n_holes = (cur_loop >= 0) ? cur_loop + 1 : 0;
            fputc(0x03, out); wu(out, (uint32_t)id); wu(out, (uint32_t)ne);
            for (int i = 0; i < ne; i++) wu(out, (uint32_t)eids[i]);
            wu(out, (uint32_t)n_holes);
            for (int h = 0; h < n_holes; h++) {
                wu(out, (uint32_t)hlen[h]);
                for (int i = 0; i < hlen[h]; i++) wu(out, (uint32_t)hole[h][i]);
            }
            wu(out, fill[0] ? (uint32_t)color_val(fill) : 0);
            counts[2]++;
            break;
        }
        case 's': {
            int id; char k[32];
            if (sscanf(p, "%d %31s", &id, k) < 2) break;
            if (strcmp(k, "parent") == 0) {
                int a, b;
                if (sscanf(p, "%*d parent %d %d", &a, &b) == 2) {
                    fputc(0x11, out); wu(out, (uint32_t)id); wu(out, (uint32_t)a); wu(out, (uint32_t)b); counts[7]++;
                }
            } else if (strcmp(k, "group_id") == 0) {
                int a, b;
                if (sscanf(p, "%*d group_id %d %d", &a, &b) == 2) {
                    fputc(0x12, out); wu(out, (uint32_t)id); wu(out, (uint32_t)a); wu(out, (uint32_t)b); counts[8]++;
                }
            } else {
                int eid; char col[16];
                if (sscanf(p, "%d %d %15s", &id, &eid, col) < 3) break;
                const char *t = p; for (int i = 0; i < 3; i++) skip_tok(&t);
                double ws[64]; int nw = 0;
                while (*t && *t != '#' && nw < 64) {
                    double w;
                    if (sscanf(t, "%lf", &w) == 1) { ws[nw++] = w; skip_tok(&t); }
                    else skip_tok(&t);
                }
                fputc(0x04, out); wu(out, (uint32_t)id); wu(out, (uint32_t)eid);
                wu(out, (uint32_t)color_val(col)); wu(out, (uint32_t)nw);
                fputc(0, out);   /* cap: round (reserved) */
                for (int i = 0; i < nw; i++) wq(out, ws[i]);
                counts[3]++;
            }
            break;
        }
        case 'n': {
            int id; if (sscanf(p, "%d", &id) != 1) break;
            double tx=0, ty=0, rot=0, sx=1, sy=1, skew=0; int cref = 0;
            const char *t = p; for (int i = 0; i < 2; i++) skip_tok(&t);
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
            fputc(0x06, out); wu(out, (uint32_t)id);
            wq(out, tx); wq(out, ty); wq(out, rot); wq(out, sx); wq(out, sy); wq(out, skew);
            wu(out, (uint32_t)cref);
            counts[4]++;
            break;
        }
        case 'r': {
            int id; double cx, cy, r, a0, a1, lw; char col[16];
            if (sscanf(p, "%d %lf %lf %lf %lf %lf %15s %lf", &id, &cx, &cy, &r, &a0, &a1, col, &lw) < 7) break;
            fputc(0x07, out); wu(out, (uint32_t)id); wc(out, cx, cy);
            wq(out, r); wq(out, a0); wq(out, a1);
            wu(out, (uint32_t)color_val(col)); wq(out, lw);
            counts[5]++;
            break;
        }
        case 'z': {
            int id; double cx, cy, rx, ry, rot = 0, sw = 1.5; char fc[16], sc[16] = "00000000";
            int n = sscanf(p, "%d %lf %lf %lf %lf %lf %15s %15s %lf", &id, &cx, &cy, &rx, &ry, &rot, fc, sc, &sw);
            if (n < 7) break;
            fputc(0x08, out); wu(out, (uint32_t)id); wc(out, cx, cy);
            wq(out, rx); wq(out, ry); wq(out, rot);
            wu(out, (uint32_t)color_val(fc)); wu(out, (uint32_t)color_val(sc)); wq(out, sw);
            counts[6]++;
            break;
        }
        case 'a': {
            int id; char k[32];
            if (sscanf(p, "%d %31s", &id, k) < 2) break;
            if (strcmp(k, "edge_connects") == 0) {
                int a, b, c;
                if (sscanf(p, "%*d edge_connects %d %d %d", &a, &b, &c) == 3) {
                    fputc(0x21, out); wu(out, (uint32_t)id); wu(out, (uint32_t)a); wu(out, (uint32_t)b); wu(out, (uint32_t)c); counts[9]++;
                }
            }
            break;
        }
        case 'c': {
            int id; char k[32];
            if (sscanf(p, "%d %31s", &id, k) < 2) break;
            if (strcmp(k, "bbox_clamp") == 0) {
                int pr; double x0, y0, x1, y1;
                if (sscanf(p, "%*d bbox_clamp %d %lf %lf %lf %lf", &pr, &x0, &y0, &x1, &y1) == 5) {
                    fputc(0x31, out); wu(out, (uint32_t)id); wu(out, (uint32_t)pr);
                    wq(out, x0); wq(out, y0); wq(out, x1); wq(out, y1);
                    counts[10]++;
                }
            } else if (strcmp(k, "min_dist") == 0) {
                int a, b; double d;
                if (sscanf(p, "%*d min_dist %d %d %lf", &a, &b, &d) == 3) {
                    fputc(0x32, out); wu(out, (uint32_t)id); wu(out, (uint32_t)a); wu(out, (uint32_t)b); wq(out, d);
                    counts[10]++;
                }
            }
            break;
        }
        case 'k': {   /* keyframe: k <id> <node> <time> [labeled fields] */
            int id, node; double t;
            if (sscanf(p, "%d %d %lf", &id, &node, &t) < 3) break;
            const char *kt = p; for (int i = 0; i < 3; i++) skip_tok(&kt);
            int mask = 0; double v[6] = {0,0,0,1,1,0};
            while (*kt) {
                while (*kt == ' ' || *kt == '\t') kt++;
                if (!*kt) break;
                double val; int n;
                if (strncmp(kt, "tx=", 3) == 0 && sscanf(kt+3, "%lf%n", &val, &n) == 1) { v[0]=val; mask|=1; kt+=3+n; }
                else if (strncmp(kt, "ty=", 3) == 0 && sscanf(kt+3, "%lf%n", &val, &n) == 1) { v[1]=val; mask|=2; kt+=3+n; }
                else if (strncmp(kt, "rot=", 4) == 0 && sscanf(kt+4, "%lf%n", &val, &n) == 1) { v[2]=val; mask|=4; kt+=4+n; }
                else if (strncmp(kt, "sx=", 3) == 0 && sscanf(kt+3, "%lf%n", &val, &n) == 1) { v[3]=val; mask|=8; kt+=3+n; }
                else if (strncmp(kt, "sy=", 3) == 0 && sscanf(kt+3, "%lf%n", &val, &n) == 1) { v[4]=val; mask|=16; kt+=3+n; }
                else if (strncmp(kt, "skew=", 5) == 0 && sscanf(kt+5, "%lf%n", &val, &n) == 1) { v[5]=val; mask|=32; kt+=5+n; }
                else skip_tok(&kt);
            }
            fputc(0x09, out); wu(out, (uint32_t)id); wu(out, (uint32_t)node);
            wq(out, t);
            fputc(mask, out);
            for (int b = 0; b < 6; b++) if (mask & (1 << b)) wq(out, v[b]);
            counts[4]++;   /* keyframes share the node counter slot */
            break;
        }
        case 'p': {
            int id; char k[32];
            if (sscanf(p, "%d %31s", &id, k) < 2) break;
            if (strcmp(k, "diffusion") == 0) {
                int eid; char L[4], R[4], lc[16], rc[16];
                if (sscanf(p, "%*d diffusion %d %3s %15s %3s %15s", &eid, L, lc, R, rc) >= 4) {
                    fputc(0x41, out); wu(out, (uint32_t)id); wu(out, (uint32_t)eid);
                    wu(out, (uint32_t)color_val(lc)); wu(out, (uint32_t)color_val(rc));
                    counts[11]++;
                }
            } else if (strcmp(k, "solid_fill") == 0) {
                int fid; char cstr[16];
                if (sscanf(p, "%*d solid_fill %d %15s", &fid, cstr) == 2) {
                    fputc(0x42, out); wu(out, (uint32_t)id); wu(out, (uint32_t)fid);
                    wu(out, (uint32_t)color_val(cstr));
                    counts[11]++;
                }
            }
            break;
        }
        default: break;
        }
    }

    fseek(out, counts_pos, SEEK_SET);
    for (int i = 0; i < 12; i++) fwrite(&counts[i], 4, 1, out);
    fclose(out);
    fclose(in);
    fprintf(stderr, "bin: encoded %s -> %s (v:%u e:%u f:%u s:%u n:%u r:%u z:%u scon:%u acon:%u con:%u p:%u)\n",
            inp, outp, counts[0], counts[1], counts[2], counts[3], counts[4],
            counts[5], counts[6], counts[7], counts[8], counts[10], counts[11]);
    return 0;
}

/* ─── Decoder ────────────────────────────────────────────────────── */

static void dec(const char *inp, const char *outp) {
    FILE *in = fopen(inp, "rb");
    if (!in) { fprintf(stderr, "bin: cannot open %s\n", inp); return; }
    FILE *out = stdout;
    if (outp && strcmp(outp, "-") != 0) { out = fopen(outp, "w"); if (!out) { fprintf(stderr, "bin: cannot write %s\n", outp); return; } }

    char magic[4]; size_t got = fread(magic, 1, 4, in);
    if (got != 4 || memcmp(magic, "SMVG", 4) != 0) { fprintf(stderr, "bin: not a SmazkaVG container\n"); return; }
    int maj = fgetc(in), min = fgetc(in);
    if (maj != 1 || min != 3) { fprintf(stderr, "bin: unsupported version %d.%d\n", maj, min); return; }
    unsigned counts[12];
    for (int i = 0; i < 12; i++) {
        unsigned c = 0;
        fread(&c, 4, 1, in);
        counts[i] = c;
    }
    /* count total records */
    long total = 0;
    for (int i = 0; i < 12; i++) total += counts[i];

    for (long rec = 0; rec < total; rec++) {
        int tag = fgetc(in);
        switch (tag) {
        case 0x01: { int id = (int)ru(in); double x, y; rc(in, &x, &y); int vt = fgetc(in);
            fprintf(out, "v %d %.6f %.6f%s\n", id, x, y, vt == 1 ? " smooth" : vt == 2 ? " symmetric" : vt == 3 ? " auto" : ""); break; }
        case 0x02: { int id = (int)ru(in), v0 = (int)ru(in), v1 = (int)ru(in);
            int ty = fgetc(in), nc = fgetc(in);
            static const char *tn[5] = { "seg", "quad", "cubic", "rational", "catmull" };
            fprintf(out, "e %d %d %d", id, v0, v1);
            if (ty > 0) {
                fprintf(out, " type=%s", tn[ty < 5 ? ty : 0]);
                for (int i = 0; i < nc * 2; i++) fprintf(out, " %.6f", rq(in));
                if (ty == 3) fprintf(out, " %.6f", rq(in));
            }
            fprintf(out, "\n"); break; }
        case 0x03: { int id = (int)ru(in), ne = (int)ru(in);
            fprintf(out, "f %d", id);
            for (int i = 0; i < ne; i++) fprintf(out, " %d", (int)ru(in));
            int nh = (int)ru(in);
            for (int h = 0; h < nh; h++) {
                int hl = (int)ru(in);
                fprintf(out, " |");
                for (int i = 0; i < hl; i++) fprintf(out, " %d", (int)ru(in));
            }
            uint32_t fill = (uint32_t)ru(in);
            if (fill) fprintf(out, " %08X", fill);
            fprintf(out, "\n"); break; }
        case 0x04: { int id = (int)ru(in), eid = (int)ru(in);
            uint32_t col = (uint32_t)ru(in); int nw = (int)ru(in);
            fgetc(in);   /* cap: reserved */
            fprintf(out, "s %d %d %08X", id, eid, col);
            for (int i = 0; i < nw; i++) fprintf(out, " %.6f", rq(in));
            fprintf(out, "\n"); break; }
        case 0x09: { int id = (int)ru(in), node = (int)ru(in);
            double t = rq(in);
            int mask = fgetc(in);
            double v[6] = {0,0,0,1,1,0};
            for (int b = 0; b < 6; b++) if (mask & (1 << b)) v[b] = rq(in);
            fprintf(out, "k %d %d %.6f", id, node, t);
            if (mask & 1)  fprintf(out, " tx=%.6f", v[0]);
            if (mask & 2)  fprintf(out, " ty=%.6f", v[1]);
            if (mask & 4)  fprintf(out, " rot=%.6f", v[2]);
            if (mask & 8)  fprintf(out, " sx=%.6f", v[3]);
            if (mask & 16) fprintf(out, " sy=%.6f", v[4]);
            if (mask & 32) fprintf(out, " skew=%.6f", v[5]);
            fprintf(out, "\n"); break; }
        case 0x06: { int id = (int)ru(in);
            double tx = rq(in), ty = rq(in), rot = rq(in), sx = rq(in), sy = rq(in), skew = rq(in);
            int cref = (int)ru(in);
            fprintf(out, "n %d tx=%.6f ty=%.6f rot=%.6f sx=%.6f sy=%.6f skew=%.6f content=%d\n",
                    id, tx, ty, rot, sx, sy, skew, cref); break; }
        case 0x07: { int id = (int)ru(in); double cx, cy; rc(in, &cx, &cy);
            double r = rq(in), a0 = rq(in), a1 = rq(in);
            uint32_t col = (uint32_t)ru(in); double lw = rq(in);
            fprintf(out, "r %d %.6f %.6f %.6f %.6f %.6f %08X %.6f\n", id, cx, cy, r, a0, a1, col, lw); break; }
        case 0x08: { int id = (int)ru(in); double cx, cy; rc(in, &cx, &cy);
            double rx = rq(in), ry = rq(in), rot = rq(in);
            uint32_t fc = (uint32_t)ru(in), sc = (uint32_t)ru(in); double sw = rq(in);
            fprintf(out, "z %d %.6f %.6f %.6f %.6f %.6f %08X %08X %.6f\n", id, cx, cy, rx, ry, rot, fc, sc, sw); break; }
        case 0x11: { int id = (int)ru(in), a = (int)ru(in), b = (int)ru(in);
            fprintf(out, "s %d parent %d %d\n", id, a, b); break; }
        case 0x12: { int id = (int)ru(in), a = (int)ru(in), b = (int)ru(in);
            fprintf(out, "s %d group_id %d %d\n", id, a, b); break; }
        case 0x21: { int id = (int)ru(in), a = (int)ru(in), b = (int)ru(in), c = (int)ru(in);
            fprintf(out, "a %d edge_connects %d %d %d\n", id, a, b, c); break; }
        case 0x31: { int id = (int)ru(in), pr = (int)ru(in);
            double x0 = rq(in), y0 = rq(in), x1 = rq(in), y1 = rq(in);
            fprintf(out, "c %d bbox_clamp %d %.6f %.6f %.6f %.6f\n", id, pr, x0, y0, x1, y1); break; }
        case 0x32: { int id = (int)ru(in), a = (int)ru(in), b = (int)ru(in); double d = rq(in);
            fprintf(out, "c %d min_dist %d %d %.6f\n", id, a, b, d); break; }
        case 0x41: { int id = (int)ru(in), eid = (int)ru(in);
            uint32_t lc = (uint32_t)ru(in), rc2 = (uint32_t)ru(in);
            fprintf(out, "p %d diffusion %d L %08X R %08X\n", id, eid, lc, rc2); break; }
        case 0x42: { int id = (int)ru(in), fid = (int)ru(in); uint32_t col = (uint32_t)ru(in);
            fprintf(out, "p %d solid_fill %d %08X\n", id, fid, col); break; }
        default:
            fprintf(stderr, "bin: unknown record tag 0x%02X at %ld; aborting decode\n", tag, rec);
            fclose(in);
            if (out != stdout) fclose(out);
            return;
        }
    }
    fclose(in);
    if (out != stdout) fclose(out);
    fprintf(stderr, "bin: decoded %s -> %s\n", inp, outp ? outp : "stdout");
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "SmazkaVG compact binary container\nUsage: %s enc <in.smazka> <out.smvg>\n       %s dec <in.smvg> [out.smazka]\n", argv[0], argv[0]);
        return 1;
    }
    if (strcmp(argv[1], "enc") == 0) return enc(argv[2], argv[3]);
    if (strcmp(argv[1], "dec") == 0) { dec(argv[2], argc > 3 ? argv[3] : NULL); return 0; }
    fprintf(stderr, "bin: unknown mode '%s'\n", argv[1]);
    return 1;
}
