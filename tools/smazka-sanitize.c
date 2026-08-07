/*
 * SmazkaVG sanitizer (tools/smazka-sanitize)
 * ==========================================
 *
 * Converts an UNSAFE SmazkaVG document (.smazkavg_unsafe, or legacy
 * .smazka) into a SAFE document (.smazkavg):
 *
 *   - `inc <path>`   : INLINED (recursively, depth-limited, cycle-guarded).
 *                      Once inlined an include is safe.
 *   - `t` (text)     : STRIPPED with a warning — text references fonts,
 *                      which the safe pipeline bans (vectorized text is
 *                      future work, see docs/PLAN.md).
 *   - `img` (raster) : STRIPPED with a warning — embedded rasters are not
 *                      part of a pure vector format (centerline
 *                      vectorization via the LP solver is future work).
 *   - everything else is copied VERBATIM (comments preserved).
 *
 * Usage:
 *   make sanitize
 *   ./build/smazka-sanitize in.smazkavg_unsafe out.smazkavg
 *   ./build/smazka-sanitize in.smazkavg_unsafe            # -> stdout
 *
 * A document that contains no `t`/`img` records is already safe; the tool
 * still inlines any `inc` records and reports the result.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#define MAX_INC_DEPTH 8
#define MAX_LINE 4096
#define MAX_BUDGET 2000000

static long budget = 0;
static int  warned_unsafe = 0;
static int  n_inc = 0;

static void note(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("sanitize: ", stderr);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

static void path_join(const char *dir, const char *name, char *out, int cap) {
    if (dir && dir[0] && strchr(name, '/') == NULL && strchr(name, '\\') == NULL)
        snprintf(out, cap, "%s/%s", dir, name);
    else
        snprintf(out, cap, "%s", name);
}
static void dir_of(const char *path, char *out, int cap) {
    snprintf(out, cap, "%s", path);
    char *slash = strrchr(out, '/');
#ifdef _WIN32
    if (!slash) slash = strrchr(out, '\\');
#endif
    if (slash) *slash = 0;
    else out[0] = 0;
}

static int sanitize_stream(FILE *f, const char *dir, int depth, FILE *out) {
    if (depth > MAX_INC_DEPTH) { note("include depth exceeds %d; cycle?\n", MAX_INC_DEPTH); return -1; }
    char ln[MAX_LINE];
    int lineno = 0;
    while (fgets(ln, sizeof(ln), f)) {
        if (++budget > MAX_BUDGET) { note("line budget exceeded\n"); return -1; }
        lineno++;
        char *p = ln;
        while (*p == ' ' || *p == '\t') p++;
        if (!*p || *p == '#') { fputs(ln, out); continue; }

        if (strncmp(p, "inc ", 4) == 0) {
            char path[1024];
            if (sscanf(p, "inc %1023s", path) == 1) {
                char joined[1200], fdir[1200];
                path_join(dir, path, joined, sizeof(joined));
                dir_of(joined, fdir, sizeof(fdir));
                FILE *incf = fopen(joined, "r");
                if (!incf) { note("cannot open include '%s'\n", joined); continue; }
                n_inc++;
                sanitize_stream(incf, fdir, depth + 1, out);
                fclose(incf);
                continue;
            }
        }
        if (strncmp(p, "t ", 2) == 0) {
            note("line %d: stripped unsafe text record (fonts are not part of safe SmazkaVG)\n", lineno);
            warned_unsafe = 1;
            continue;
        }
        if (strncmp(p, "font ", 5) == 0) {
            note("line %d: stripped unsafe font declaration\n", lineno);
            warned_unsafe = 1;
            continue;
        }
        if (strncmp(p, "img ", 4) == 0) {
            note("line %d: stripped unsafe raster record (embedding is not part of safe SmazkaVG)\n", lineno);
            warned_unsafe = 1;
            continue;
        }
        fputs(ln, out);
    }
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "SmazkaVG sanitizer (unsafe -> safe)\n"
                        "Usage: %s <in.smazkavg_unsafe> [out.smazkavg]\n", argv[0]);
        return 1;
    }
    FILE *in = fopen(argv[1], "r");
    if (!in) { fprintf(stderr, "sanitize: cannot open %s\n", argv[1]); return 1; }
    FILE *out = stdout;
    if (argc == 3) {
        out = fopen(argv[2], "w");
        if (!out) { fprintf(stderr, "sanitize: cannot write %s\n", argv[2]); return 1; }
    }

    char fdir[1200];
    dir_of(argv[1], fdir, sizeof(fdir));
    fprintf(out, "# sanitized by smazka-sanitize (safe SmazkaVG; unsafe records stripped/inlined)\n");
    int rc = sanitize_stream(in, fdir, 0, out);
    fclose(in);
    if (out != stdout) fclose(out);

    fprintf(stderr, "sanitize: %d include(s) inlined, %s\n", n_inc,
            warned_unsafe ? "unsafe records stripped (see warnings above)"
                          : "document already safe (no text/raster records)");
    return rc == 0 ? 0 : 1;
}
