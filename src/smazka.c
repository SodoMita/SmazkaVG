#include "smazka.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "solver.h"
#include "mip.h"
#include "err.h"

#define SMZ_MAX_TOKENS 2048
#define SMZ_CANONICAL_TIER 1000000
#define SMZ_EPS 1e-7

typedef struct {
    int *col;
    double *val;
    int n;
    int cap;
    char rel;
    double rhs;
} WorkRow;

typedef struct {
    WorkRow *rows;
    int n_rows;
    int cap_rows;
    int n_cols;
    double *lo;
    double *hi;
    unsigned char *isint;
    int *obj_tier;
    int *obj_col;
    double *obj_coef;
    int n_obj;
    int cap_obj;
} Work;

static void set_error(SmzDoc *doc, int line, const char *message)
{
    if (doc->error[0]) return;
    snprintf(doc->error, sizeof(doc->error), "%s", message);
    doc->error_line = line;
}

static void set_errorf(SmzDoc *doc, int line, const char *prefix, const char *value)
{
    if (doc->error[0]) return;
    snprintf(doc->error, sizeof(doc->error), "%s%s", prefix, value ? value : "");
    doc->error_line = line;
}

void smz_init(SmzDoc *doc)
{
    memset(doc, 0, sizeof(*doc));
    for (int i = 0; i < SMZ_MAX_PROPERTIES; i++)
        for (int k = 0; k < 4; k++) doc->props[i].var[k] = -1;
}

void smz_free(SmzDoc *doc)
{
    if (!doc) return;
    for (int i = 0; i < doc->n_cells; i++) {
        free(doc->cells[i].edge_name);
        free(doc->cells[i].edges);
    }
    memset(doc, 0, sizeof(*doc));
}

static int valid_name(const char *s)
{
    if (!s || (!isalpha((unsigned char)*s) && *s != '_')) return 0;
    for (const char *p = s + 1; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '_') return 0;
    return strlen(s) < SMZ_NAME_MAX;
}

static int tokenize(char *line, char **tok, int cap)
{
    char *hash = strchr(line, '#');
    if (hash) *hash = 0;
    int n = 0;
    char *p = line;
    while (*p) {
        while (isspace((unsigned char)*p)) p++;
        if (!*p) break;
        if (n == cap) return -1;
        tok[n++] = p;
        while (*p && !isspace((unsigned char)*p)) p++;
        if (*p) *p++ = 0;
    }
    return n;
}

static int parse_double(const char *s, double *out)
{
    if (!s || !*s) return 0;
    errno = 0;
    char *end = NULL;
    double v = strtod(s, &end);
    if (errno || !end || *end || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

static int parse_int(const char *s, int *out)
{
    if (!s || !*s) return 0;
    errno = 0;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (errno || !end || *end || v < INT_MIN || v > INT_MAX) return 0;
    *out = (int)v;
    return 1;
}

static double q16_round(double value)
{
    return nearbyint(value * 65536.0) / 65536.0;
}

static int in_q16(double value)
{
    return value >= -32768.0 && value <= 32767.9999847412109375;
}

static int exact_solver_int(double value)
{
    return isfinite(value) && fabs(value) <= 9007199254740991.0 && value == nearbyint(value);
}

static int find_cell(const SmzDoc *doc, const char *name)
{
    for (int i = 0; i < doc->n_cells; i++)
        if (strcmp(doc->cells[i].name, name) == 0) return i;
    return -1;
}

static int find_prop(const SmzDoc *doc, int cell, const char *name)
{
    for (int i = 0; i < doc->n_props; i++)
        if (doc->props[i].cell == cell && strcmp(doc->props[i].name, name) == 0)
            return i;
    return -1;
}

static int parse_cell(SmzDoc *doc, char **t, int nt, int line)
{
    if (nt < 2 || strlen(t[0]) != 2 || t[0][0] != '+') {
        set_error(doc, line, "cell declaration is +V/+E/+F followed by an ID");
        return -1;
    }
    if (!valid_name(t[1])) {
        set_errorf(doc, line, "invalid cell ID: ", t[1]);
        return -1;
    }
    if (find_cell(doc, t[1]) >= 0) {
        set_errorf(doc, line, "duplicate cell: ", t[1]);
        return -1;
    }
    if (doc->n_cells == SMZ_MAX_CELLS) {
        set_error(doc, line, "cell limit exceeded");
        return -1;
    }
    SmzCell *c = &doc->cells[doc->n_cells++];
    memset(c, 0, sizeof(*c));
    c->a = c->b = -1;
    c->line = line;
    snprintf(c->name, sizeof(c->name), "%s", t[1]);
    switch (t[0][1]) {
    case 'V':
        if (nt != 2) { set_error(doc, line, "+V takes only an ID"); return -1; }
        c->type = SMZ_CELL_VERTEX;
        break;
    case 'E':
        if (nt != 4 || !valid_name(t[2]) || !valid_name(t[3])) {
            set_error(doc, line, "+E syntax: +E id start_vertex end_vertex"); return -1;
        }
        c->type = SMZ_CELL_EDGE;
        snprintf(c->a_name, sizeof(c->a_name), "%s", t[2]);
        snprintf(c->b_name, sizeof(c->b_name), "%s", t[3]);
        break;
    case 'F':
        if (nt < 5) { set_error(doc, line, "+F needs at least three edge IDs"); return -1; }
        c->type = SMZ_CELL_FACE;
        c->n_edges = nt - 2;
        if (c->n_edges > SMZ_MAX_FACE_EDGES) {
            set_error(doc, line, "face edge limit exceeded"); return -1;
        }
        c->edge_name = calloc((size_t)c->n_edges, sizeof(*c->edge_name));
        c->edges = malloc((size_t)c->n_edges * sizeof(*c->edges));
        if (!c->edge_name || !c->edges) { set_error(doc, line, "face allocation failed"); return -1; }
        for (int i = 0; i < c->n_edges; i++) {
            if (!valid_name(t[i + 2])) { set_error(doc, line, "invalid face edge ID"); return -1; }
            snprintf(c->edge_name[i], SMZ_NAME_MAX, "%s", t[i + 2]);
            c->edges[i] = -1;
        }
        break;
    default:
        set_error(doc, line, "unknown cell sigil; expected +V, +E, or +F");
        return -1;
    }
    return 0;
}

static int parse_rgba(const char *s, double out[4])
{
    if (!s || strlen(s) != 8) return 0;
    unsigned value = 0;
    for (int i = 0; i < 8; i++) if (!isxdigit((unsigned char)s[i])) return 0;
    if (sscanf(s, "%x", &value) != 1) return 0;
    out[0] = (value >> 24) & 255;
    out[1] = (value >> 16) & 255;
    out[2] = (value >> 8) & 255;
    out[3] = value & 255;
    return 1;
}

static int property_type(const char *s, SmzPropType *type, int *ncomp)
{
    if (strcmp(s, "bool") == 0) { *type = SMZ_PROP_BOOL; *ncomp = 1; return 1; }
    if (strcmp(s, "int") == 0)  { *type = SMZ_PROP_INT; *ncomp = 1; return 1; }
    if (strcmp(s, "q16") == 0)  { *type = SMZ_PROP_Q16; *ncomp = 1; return 1; }
    if (strcmp(s, "vec2") == 0) { *type = SMZ_PROP_VEC2; *ncomp = 2; return 1; }
    if (strcmp(s, "rgba") == 0) { *type = SMZ_PROP_RGBA; *ncomp = 4; return 1; }
    if (strcmp(s, "enum") == 0) { *type = SMZ_PROP_ENUM; *ncomp = 0; return 1; }
    return 0;
}

static int split_property_head(SmzDoc *doc, const char *head, int line,
                               SmzPropMode *mode, char *cell, char *prop,
                               SmzPropType *type, int *ncomp)
{
    *mode = head[0] == '=' ? SMZ_CONST : SMZ_VAR;
    const char *body = head + 1;
    const char *dot = strchr(body, '.');
    const char *colon = strrchr(body, ':');
    if (!dot || !colon || dot >= colon) {
        set_error(doc, line, "property syntax: =cell.name:type or ?cell.name:type");
        return 0;
    }
    size_t nc = (size_t)(dot - body), np = (size_t)(colon - dot - 1);
    if (nc == 0 || nc >= SMZ_NAME_MAX || np == 0 || np >= SMZ_NAME_MAX) {
        set_error(doc, line, "property cell/name is too long"); return 0;
    }
    memcpy(cell, body, nc); cell[nc] = 0;
    memcpy(prop, dot + 1, np); prop[np] = 0;
    if (!valid_name(cell) || !valid_name(prop) || !property_type(colon + 1, type, ncomp)) {
        set_error(doc, line, "invalid property cell, name, or type"); return 0;
    }
    return 1;
}

static int parse_property(SmzDoc *doc, char **t, int nt, int line)
{
    if (doc->n_props == SMZ_MAX_PROPERTIES) {
        set_error(doc, line, "property limit exceeded"); return -1;
    }
    SmzProperty p;
    memset(&p, 0, sizeof(p));
    p.cell = -1;
    p.line = line;
    for (int k = 0; k < 4; k++) p.var[k] = -1;
    if (!split_property_head(doc, t[0], line, &p.mode, p.cell_name, p.name,
                             &p.type, &p.ncomp)) return -1;

    if (p.type == SMZ_PROP_ENUM) {
        if (p.mode != SMZ_CONST || nt != 2 || !valid_name(t[1])) {
            set_error(doc, line, "enum properties are constants: =cell.name:enum value"); return -1;
        }
        snprintf(p.enum_value, sizeof(p.enum_value), "%s", t[1]);
    } else if (p.type == SMZ_PROP_RGBA) {
        if (nt != 2 || !parse_rgba(t[1], p.value)) {
            set_error(doc, line, "rgba syntax requires exactly RRGGBBAA"); return -1;
        }
        for (int k = 0; k < 4; k++) {
            p.seed[k] = p.value[k]; p.lo[k] = 0; p.hi[k] = 255; p.is_integer[k] = 1;
        }
    } else {
        int need_const = 1 + p.ncomp;
        int need_var = 1 + p.ncomp * 3;
        if ((p.mode == SMZ_CONST && nt != need_const) ||
            (p.mode == SMZ_VAR && nt != need_var)) {
            set_error(doc, line, p.mode == SMZ_CONST
                      ? "constant property has the wrong value count"
                      : "variable syntax is ?cell.name:type seed... lo... hi...");
            return -1;
        }
        for (int k = 0; k < p.ncomp; k++) {
            if (!parse_double(t[1 + k], &p.value[k])) {
                set_error(doc, line, "invalid property value/seed"); return -1;
            }
            p.seed[k] = p.value[k];
            if (p.mode == SMZ_VAR) {
                if (!parse_double(t[1 + p.ncomp + 2 * k], &p.lo[k]) ||
                    !parse_double(t[2 + p.ncomp + 2 * k], &p.hi[k]) ||
                    p.lo[k] > p.hi[k] || p.seed[k] < p.lo[k] || p.seed[k] > p.hi[k]) {
                    set_error(doc, line, "invalid variable bounds or seed outside domain"); return -1;
                }
            } else p.lo[k] = p.hi[k] = p.value[k];
        }
        if (p.type == SMZ_PROP_Q16 || p.type == SMZ_PROP_VEC2) {
            for (int k = 0; k < p.ncomp; k++) {
                if (!in_q16(p.seed[k]) || !in_q16(p.lo[k]) || !in_q16(p.hi[k])) {
                    set_error(doc, line, "q16/vec2 value or domain is outside Q16.16 range"); return -1;
                }
                p.seed[k] = p.value[k] = q16_round(p.seed[k]);
                if (p.mode == SMZ_VAR) {
                    p.lo[k] = ceil(p.lo[k] * 65536.0) / 65536.0;
                    p.hi[k] = floor(p.hi[k] * 65536.0) / 65536.0;
                    if (p.lo[k] > p.hi[k] || p.seed[k] < p.lo[k] || p.seed[k] > p.hi[k]) {
                        set_error(doc, line, "Q16.16 quantization empties variable domain"); return -1;
                    }
                } else p.lo[k] = p.hi[k] = p.value[k];
            }
        }
        if (p.type == SMZ_PROP_BOOL) {
            if (p.lo[0] < 0 || p.hi[0] > 1 || !exact_solver_int(p.seed[0]) ||
                !exact_solver_int(p.lo[0]) || !exact_solver_int(p.hi[0])) {
                set_error(doc, line, "bool seed/domain must be integral within [0,1]"); return -1;
            }
            p.is_integer[0] = 1;
        } else if (p.type == SMZ_PROP_INT) {
            if (!exact_solver_int(p.seed[0]) || !exact_solver_int(p.lo[0]) || !exact_solver_int(p.hi[0])) {
                set_error(doc, line, "int seed/domain must be exact integers within solver range"); return -1;
            }
            p.is_integer[0] = 1;
        }
    }
    doc->props[doc->n_props++] = p;
    return 0;
}

static int valid_status(SmzRuleType kind, const char *status)
{
    if (kind == SMZ_RULE_HARD)
        return strcmp(status, "reject") == 0 || strcmp(status, "infeasible") == 0;
    if (kind == SMZ_RULE_SOFT)
        return strcmp(status, "degraded") == 0 || strcmp(status, "optional") == 0;
    return strcmp(status, "optimal") == 0;
}

static int parse_rule_head(SmzDoc *doc, const char *head, int line,
                           SmzRuleType *kind, char *status, int *tier)
{
    *kind = head[0] == '!' ? SMZ_RULE_HARD : head[0] == '?' ? SMZ_RULE_SOFT : SMZ_RULE_OBJECTIVE;
    const char *body = head + 1;
    const char *at = strchr(body, '@');
    size_t ns = at ? (size_t)(at - body) : strlen(body);
    if (ns == 0 || ns >= SMZ_STATUS_MAX) { set_error(doc, line, "missing/long rule status"); return 0; }
    memcpy(status, body, ns); status[ns] = 0;
    *tier = 0;
    if (at && (!parse_int(at + 1, tier) || *tier < 0)) {
        set_error(doc, line, "invalid nonnegative rule tier"); return 0;
    }
    if (*kind == SMZ_RULE_HARD && at) { set_error(doc, line, "hard rules do not take @tier"); return 0; }
    if (*kind != SMZ_RULE_HARD && !at) { set_error(doc, line, "soft/objective rules require @tier"); return 0; }
    if (!valid_status(*kind, status)) { set_error(doc, line, "status is not valid for this rule sigil"); return 0; }
    return 1;
}

static int parse_rule(SmzDoc *doc, char **t, int nt, int line)
{
    if (doc->n_rules == SMZ_MAX_RULES) { set_error(doc, line, "rule limit exceeded"); return -1; }
    SmzRule r;
    memset(&r, 0, sizeof(r));
    r.prop = -1; r.component = -1; r.line = line;
    if (!parse_rule_head(doc, t[0], line, &r.type, r.status, &r.tier)) return -1;
    if (nt < 4 || !valid_name(t[1])) { set_error(doc, line, "rule requires a name and expression"); return -1; }
    snprintf(r.name, sizeof(r.name), "%s", t[1]);
    snprintf(r.ref, sizeof(r.ref), "%s", t[2]);
    if (r.type == SMZ_RULE_HARD) {
        if (nt != 5 || !parse_double(t[4], &r.rhs)) { set_error(doc, line, "hard rule: !status name ref (=|<=|>=) rhs"); return -1; }
        if (strcmp(t[3], "=") == 0) r.op = SMZ_OP_EQ;
        else if (strcmp(t[3], "<=") == 0) r.op = SMZ_OP_LE;
        else if (strcmp(t[3], ">=") == 0) r.op = SMZ_OP_GE;
        else { set_error(doc, line, "unknown hard-rule operator"); return -1; }
    } else if (r.type == SMZ_RULE_SOFT) {
        if (nt != 5 || strcmp(t[3], "~=") != 0 || !parse_double(t[4], &r.rhs)) {
            set_error(doc, line, "soft rule: ?status@tier name ref ~= target"); return -1;
        }
        r.op = SMZ_OP_APPROX;
    } else {
        if (nt != 4) { set_error(doc, line, "objective: ~optimal@tier name ref min|max"); return -1; }
        if (strcmp(t[3], "min") == 0) r.op = SMZ_OP_MIN;
        else if (strcmp(t[3], "max") == 0) r.op = SMZ_OP_MAX;
        else { set_error(doc, line, "objective direction must be min or max"); return -1; }
    }
    doc->rules[doc->n_rules++] = r;
    return 0;
}

static int parse_line(SmzDoc *doc, char *linebuf, int line)
{
    char *t[SMZ_MAX_TOKENS];
    int nt = tokenize(linebuf, t, SMZ_MAX_TOKENS);
    if (nt < 0) { set_error(doc, line, "too many tokens on line"); return -1; }
    if (nt == 0) return 0;
    if (t[0][0] == '+') return parse_cell(doc, t, nt, line);
    if (t[0][0] == '=' || (t[0][0] == '?' && strchr(t[0], '.')))
        return parse_property(doc, t, nt, line);
    if (t[0][0] == '!' || t[0][0] == '?' || t[0][0] == '~')
        return parse_rule(doc, t, nt, line);
    set_errorf(doc, line, "unknown record: ", t[0]);
    return -1;
}

int smz_parse_file(SmzDoc *doc, const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) { set_errorf(doc, 0, "cannot open: ", path); return -1; }
    char line[65536];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        if (!strchr(line, '\n') && !feof(f)) { set_error(doc, lineno, "line exceeds 65535 bytes"); break; }
        if (parse_line(doc, line, lineno) != 0) break;
    }
    if (ferror(f) && !doc->error[0]) set_error(doc, lineno, "read error");
    fclose(f);
    return doc->error[0] ? -1 : 0;
}

static int link_ref(SmzDoc *doc, SmzRule *r)
{
    char buf[SMZ_NAME_MAX * 2];
    snprintf(buf, sizeof(buf), "%s", r->ref);
    char *first = strchr(buf, '.');
    if (!first) { set_error(doc, r->line, "property reference needs cell.property"); return 0; }
    *first++ = 0;
    int cell = find_cell(doc, buf);
    if (cell < 0) { set_errorf(doc, r->line, "unknown rule cell: ", buf); return 0; }
    char *component = strchr(first, '.');
    if (component) *component++ = 0;
    int prop = find_prop(doc, cell, first);
    if (prop < 0) { set_errorf(doc, r->line, "unknown rule property: ", first); return 0; }
    SmzProperty *p = &doc->props[prop];
    int comp = 0;
    if (p->ncomp > 1) {
        if (!component) { set_error(doc, r->line, "vector/color rule reference needs a component"); return 0; }
        if (p->type == SMZ_PROP_VEC2)
            comp = strcmp(component, "x") == 0 ? 0 : strcmp(component, "y") == 0 ? 1 : -1;
        else if (p->type == SMZ_PROP_RGBA)
            comp = strcmp(component, "r") == 0 ? 0 : strcmp(component, "g") == 0 ? 1 :
                   strcmp(component, "b") == 0 ? 2 : strcmp(component, "a") == 0 ? 3 : -1;
        else comp = -1;
        if (comp < 0) { set_error(doc, r->line, "invalid property component"); return 0; }
    } else if (component) { set_error(doc, r->line, "scalar property has no component"); return 0; }
    r->prop = prop; r->component = comp;
    return 1;
}

static int link_doc(SmzDoc *doc)
{
    for (int i = 0; i < doc->n_cells; i++) {
        SmzCell *c = &doc->cells[i];
        if (c->type == SMZ_CELL_EDGE) {
            c->a = find_cell(doc, c->a_name); c->b = find_cell(doc, c->b_name);
            if (c->a < 0 || c->b < 0 || doc->cells[c->a].type != SMZ_CELL_VERTEX ||
                doc->cells[c->b].type != SMZ_CELL_VERTEX) {
                set_error(doc, c->line, "edge endpoints must name vertex cells"); return 0;
            }
        } else if (c->type == SMZ_CELL_FACE) {
            for (int k = 0; k < c->n_edges; k++) {
                c->edges[k] = find_cell(doc, c->edge_name[k]);
                if (c->edges[k] < 0 || doc->cells[c->edges[k]].type != SMZ_CELL_EDGE) {
                    set_error(doc, c->line, "face boundary must name edge cells"); return 0;
                }
            }
        }
    }
    for (int i = 0; i < doc->n_props; i++) {
        SmzProperty *p = &doc->props[i];
        p->cell = find_cell(doc, p->cell_name);
        if (p->cell < 0) { set_errorf(doc, p->line, "unknown property cell: ", p->cell_name); return 0; }
        for (int j = 0; j < i; j++)
            if (doc->props[j].cell == p->cell && strcmp(doc->props[j].name, p->name) == 0) {
                set_error(doc, p->line, "duplicate property"); return 0;
            }
    }
    for (int i = 0; i < doc->n_rules; i++) if (!link_ref(doc, &doc->rules[i])) return 0;
    return 1;
}

static int scalar_property(const SmzProperty *p)
{
    return p->type != SMZ_PROP_ENUM;
}

static int assign_variables(SmzDoc *doc)
{
    int n = 0;
    for (int i = 0; i < doc->n_props; i++) {
        SmzProperty *p = &doc->props[i];
        if (p->mode == SMZ_VAR) {
            if (!scalar_property(p)) { set_error(doc, p->line, "variable enum is not implemented; use a constant enum"); return 0; }
            for (int k = 0; k < p->ncomp; k++) p->var[k] = n++;
        }
    }
    doc->n_base_vars = n;
    return 1;
}

static int work_row(Work *w, char rel, double rhs)
{
    if (w->n_rows == w->cap_rows) {
        int nc = w->cap_rows ? w->cap_rows * 2 : 64;
        WorkRow *nr = realloc(w->rows, (size_t)nc * sizeof(*nr));
        if (!nr) return -1;
        w->rows = nr; w->cap_rows = nc;
    }
    WorkRow *r = &w->rows[w->n_rows++];
    memset(r, 0, sizeof(*r)); r->rel = rel; r->rhs = rhs;
    return w->n_rows - 1;
}

static int work_ent(Work *w, int row, int col, double val)
{
    WorkRow *r = &w->rows[row];
    if (r->n == r->cap) {
        int nc = r->cap ? r->cap * 2 : 4;
        int *ncol = malloc((size_t)nc * sizeof(*ncol));
        double *nval = malloc((size_t)nc * sizeof(*nval));
        if (!ncol || !nval) { free(ncol); free(nval); return 0; }
        if (r->n) {
            memcpy(ncol, r->col, (size_t)r->n * sizeof(*ncol));
            memcpy(nval, r->val, (size_t)r->n * sizeof(*nval));
        }
        free(r->col); free(r->val);
        r->col = ncol; r->val = nval; r->cap = nc;
    }
    r->col[r->n] = col; r->val[r->n] = val; r->n++;
    return 1;
}

static int work_obj(Work *w, int tier, int col, double coef)
{
    if (w->n_obj == w->cap_obj) {
        int nc = w->cap_obj ? w->cap_obj * 2 : 64;
        int *nt = malloc((size_t)nc * sizeof(*nt));
        int *ncol = malloc((size_t)nc * sizeof(*ncol));
        double *ncoef = malloc((size_t)nc * sizeof(*ncoef));
        if (!nt || !ncol || !ncoef) { free(nt); free(ncol); free(ncoef); return 0; }
        if (w->n_obj) {
            memcpy(nt, w->obj_tier, (size_t)w->n_obj * sizeof(*nt));
            memcpy(ncol, w->obj_col, (size_t)w->n_obj * sizeof(*ncol));
            memcpy(ncoef, w->obj_coef, (size_t)w->n_obj * sizeof(*ncoef));
        }
        free(w->obj_tier); free(w->obj_col); free(w->obj_coef);
        w->obj_tier = nt; w->obj_col = ncol; w->obj_coef = ncoef; w->cap_obj = nc;
    }
    w->obj_tier[w->n_obj] = tier;
    w->obj_col[w->n_obj] = col;
    w->obj_coef[w->n_obj] = coef;
    w->n_obj++;
    return 1;
}

static void free_work(Work *w)
{
    for (int i = 0; i < w->n_rows; i++) { free(w->rows[i].col); free(w->rows[i].val); }
    free(w->rows); free(w->lo); free(w->hi); free(w->isint);
    free(w->obj_tier); free(w->obj_col); free(w->obj_coef);
    memset(w, 0, sizeof(*w));
}

static int add_abs_target(Work *w, int base_col, double target, int tier)
{
    int d = w->n_cols, nn = w->n_cols + 1;
    double *lo = malloc((size_t)nn * sizeof(*lo));
    double *hi = malloc((size_t)nn * sizeof(*hi));
    unsigned char *ii = malloc((size_t)nn);
    if (!lo || !hi || !ii) { free(lo); free(hi); free(ii); return 0; }
    if (d) {
        memcpy(lo, w->lo, (size_t)d * sizeof(*lo));
        memcpy(hi, w->hi, (size_t)d * sizeof(*hi));
        memcpy(ii, w->isint, (size_t)d);
    }
    free(w->lo); free(w->hi); free(w->isint);
    w->lo = lo; w->hi = hi; w->isint = ii; w->n_cols = nn;
    w->lo[d] = 0; w->hi[d] = LP_INF; w->isint[d] = 0;
    int r = work_row(w, '<', target);
    if (r < 0 || !work_ent(w, r, base_col, 1) || !work_ent(w, r, d, -1)) return 0;
    r = work_row(w, '<', -target);
    if (r < 0 || !work_ent(w, r, base_col, -1) || !work_ent(w, r, d, -1)) return 0;
    return work_obj(w, tier, d, 1);
}

static int compare_value(double lhs, SmzRuleOp op, double rhs)
{
    if (op == SMZ_OP_EQ) return fabs(lhs - rhs) <= SMZ_EPS;
    if (op == SMZ_OP_LE) return lhs <= rhs + SMZ_EPS;
    return lhs >= rhs - SMZ_EPS;
}

static int build_work(SmzDoc *doc, Work *w)
{
    memset(w, 0, sizeof(*w));
    w->n_cols = doc->n_base_vars;
    if (w->n_cols) {
        w->lo = malloc((size_t)w->n_cols * sizeof(*w->lo));
        w->hi = malloc((size_t)w->n_cols * sizeof(*w->hi));
        w->isint = calloc((size_t)w->n_cols, 1);
        if (!w->lo || !w->hi || !w->isint) return 0;
    }
    for (int i = 0; i < doc->n_props; i++) {
        SmzProperty *p = &doc->props[i];
        if (p->mode != SMZ_VAR) continue;
        for (int k = 0; k < p->ncomp; k++) {
            int v = p->var[k]; w->lo[v] = p->lo[k]; w->hi[v] = p->hi[k];
            w->isint[v] = (unsigned char)p->is_integer[k];
            if (!add_abs_target(w, v, p->seed[k], SMZ_CANONICAL_TIER)) return 0;
        }
    }
    for (int i = 0; i < doc->n_rules; i++) {
        SmzRule *r = &doc->rules[i];
        SmzProperty *p = &doc->props[r->prop];
        int k = r->component;
        if (r->type == SMZ_RULE_HARD) {
            if (p->mode == SMZ_CONST) {
                if (!compare_value(p->value[k], r->op, r->rhs)) {
                    char msg[SMZ_ERROR_MAX];
                    snprintf(msg, sizeof(msg), "%s [%s]: constant hard rule failed", r->name, r->status);
                    set_error(doc, r->line, msg); return 0;
                }
            } else {
                char rel = r->op == SMZ_OP_EQ ? '=' : r->op == SMZ_OP_LE ? '<' : '>';
                int row = work_row(w, rel, r->rhs);
                if (row < 0 || !work_ent(w, row, p->var[k], 1)) return 0;
            }
        } else if (r->type == SMZ_RULE_SOFT) {
            if (p->mode == SMZ_VAR && !add_abs_target(w, p->var[k], r->rhs, r->tier)) return 0;
        } else if (p->mode == SMZ_VAR) {
            double coef = r->op == SMZ_OP_MIN ? 1 : -1;
            if (!work_obj(w, r->tier, p->var[k], coef)) return 0;
        }
    }
    if (w->n_rows == 0 && w->n_cols > 0) {
        if (work_row(w, '<', 0) < 0) return 0;
    }
    return 1;
}

static int build_lp(const Work *w, const double *objective, LP *lp)
{
    memset(lp, 0, sizeof(*lp));
    int n = w->n_cols, m = w->n_rows;
    int *count = calloc((size_t)n + 1, sizeof(*count));
    if (!count) return 0;
    long nnz = 0;
    for (int i = 0; i < m; i++) for (int k = 0; k < w->rows[i].n; k++) {
        int c = w->rows[i].col[k]; if (c < 0 || c >= n) { free(count); return 0; }
        count[c + 1]++; nnz++;
    }
    for (int c = 0; c < n; c++) count[c + 1] += count[c];
    lp->Acolptr = malloc(((size_t)n + 1) * sizeof(int));
    lp->Arow = malloc((size_t)(nnz ? nnz : 1) * sizeof(int));
    lp->Aval = malloc((size_t)(nnz ? nnz : 1) * sizeof(double));
    int *pos = malloc(((size_t)n + 1) * sizeof(int));
    lp->rel = malloc((size_t)(m ? m : 1));
    lp->b = malloc((size_t)(m ? m : 1) * sizeof(double));
    lp->c = malloc((size_t)(n ? n : 1) * sizeof(double));
    lp->l = malloc((size_t)(n ? n : 1) * sizeof(double));
    lp->u = malloc((size_t)(n ? n : 1) * sizeof(double));
    if (!lp->Acolptr || !lp->Arow || !lp->Aval || !pos || !lp->rel ||
        !lp->b || !lp->c || !lp->l || !lp->u) {
        free(count); free(pos);
        free(lp->Acolptr); free(lp->Arow); free(lp->Aval); free(lp->rel);
        free(lp->b); free(lp->c); free(lp->l); free(lp->u);
        memset(lp, 0, sizeof(*lp));
        return 0;
    }
    memcpy(lp->Acolptr, count, ((size_t)n + 1) * sizeof(int));
    memcpy(pos, count, ((size_t)n + 1) * sizeof(int));
    for (int i = 0; i < m; i++) {
        lp->rel[i] = w->rows[i].rel; lp->b[i] = w->rows[i].rhs;
        for (int k = 0; k < w->rows[i].n; k++) {
            int c = w->rows[i].col[k], at = pos[c]++;
            lp->Arow[at] = i; lp->Aval[at] = w->rows[i].val[k];
        }
    }
    for (int c = 0; c < n; c++) {
        lp->c[c] = objective[c]; lp->l[c] = w->lo[c]; lp->u[c] = w->hi[c];
    }
    lp->n = n; lp->m = m; lp->maximize = 0;
    free(count); free(pos);
    return 1;
}

static void free_lp(LP *lp)
{
    free(lp->Acolptr); free(lp->Arow); free(lp->Aval); free(lp->rel);
    free(lp->b); free(lp->c); free(lp->l); free(lp->u);
    memset(lp, 0, sizeof(*lp));
}

static int solve_once(SmzDoc *doc, Work *w, const double *objective,
                      double *solution, double *obj)
{
    LP lp;
    if (!build_lp(w, objective, &lp)) { set_error(doc, 0, "solver model allocation failed"); return 0; }
    volatile int has_int = 0;
    for (int i = 0; i < w->n_cols; i++) has_int |= w->isint[i];
    int jump = 0;
    if (!psolve_try()) jump = setjmp(psolve_env);
    if (jump) {
        psolve_end(); free_lp(&lp); set_error(doc, 0, "psolve internal/allocation failure"); return 0;
    }
    int status = -1;
    if (has_int) {
        MIP mip;
        memset(&mip, 0, sizeof(mip));
        mip.n = lp.n; mip.m = lp.m; mip.c = lp.c; mip.Acolptr = lp.Acolptr;
        mip.Arow = lp.Arow; mip.Aval = lp.Aval; mip.rel = lp.rel; mip.b = lp.b;
        mip.l = lp.l; mip.u = lp.u; mip.isint = w->isint; mip.mip_gap = 0;
        mip.node_limit = 100000; mip.lp_iter_limit = 1000000;
        MIPResult result;
        memset(&result, 0, sizeof(result));
        mip_solve(&mip, &result);
        status = result.status;
        if (status == 0) {
            memcpy(solution, result.x, (size_t)lp.n * sizeof(double));
            *obj = result.obj; doc->solve_kind = SMZ_SOLVE_MIP;
        }
        mip_result_free(&result);
    } else {
        Solver *solver = solver_create(&lp);
        if (solver) {
            solver->iteration_limit = 1000000;
            status = solver_solve(solver);
            if (status == 0) {
                solver_optimum(solver, solution, obj);
                doc->solve_kind = SMZ_SOLVE_LP;
            }
            solver_destroy(solver);
        }
    }
    psolve_end(); free_lp(&lp);
    if (status != 0) {
        if (status == 1) {
            const SmzRule *cause = NULL;
            for (int i = 0; i < doc->n_rules; i++)
                if (doc->rules[i].type == SMZ_RULE_HARD) { cause = &doc->rules[i]; break; }
            if (cause) {
                char message[SMZ_ERROR_MAX];
                snprintf(message, sizeof(message), "%s [%s]: psolve infeasible", cause->name, cause->status);
                set_error(doc, cause->line, message);
            } else set_error(doc, 0, "psolve: infeasible");
        } else set_error(doc, 0, status == 2 ? "psolve: unbounded" : "psolve: solver limit/error");
        return 0;
    }
    return 1;
}

static int int_cmp(const void *a, const void *b)
{
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int add_freeze_rows(Work *w, const double *objective, double optimum)
{
    int r = work_row(w, '<', optimum + SMZ_EPS);
    if (r < 0) return 0;
    for (int i = 0; i < w->n_cols; i++)
        if (objective[i] && !work_ent(w, r, i, objective[i])) return 0;
    r = work_row(w, '>', optimum - SMZ_EPS);
    if (r < 0) return 0;
    for (int i = 0; i < w->n_cols; i++)
        if (objective[i] && !work_ent(w, r, i, objective[i])) return 0;
    return 1;
}

static int solve_work(SmzDoc *doc, Work *w, double *solution)
{
    if (w->n_cols == 0) { doc->solve_kind = SMZ_SOLVE_NONE; return 1; }
    int *tiers = malloc((size_t)(w->n_obj ? w->n_obj : 1) * sizeof(int));
    double *objective = calloc((size_t)w->n_cols, sizeof(double));
    if (!tiers || !objective) { free(tiers); free(objective); return 0; }
    int nt = 0;
    for (int i = 0; i < w->n_obj; i++) tiers[nt++] = w->obj_tier[i];
    qsort(tiers, (size_t)nt, sizeof(int), int_cmp);
    int out = 0;
    for (int i = 0; i < nt; i++) if (i == 0 || tiers[i] != tiers[i - 1]) tiers[out++] = tiers[i];
    nt = out;
    for (int ti = 0; ti < nt; ti++) {
        memset(objective, 0, (size_t)w->n_cols * sizeof(double));
        for (int j = 0; j < w->n_obj; j++)
            if (w->obj_tier[j] == tiers[ti]) objective[w->obj_col[j]] += w->obj_coef[j];
        double optimum = 0;
        if (!solve_once(doc, w, objective, solution, &optimum)) { free(tiers); free(objective); return 0; }
        if (ti + 1 < nt && !add_freeze_rows(w, objective, optimum)) {
            set_error(doc, 0, "cannot freeze lexicographic objective tier");
            free(tiers); free(objective); return 0;
        }
    }
    free(tiers); free(objective);
    return 1;
}

static int prop_index(const SmzDoc *doc, int cell, const char *name)
{
    return find_prop(doc, cell, name);
}

static int required_prop(const SmzDoc *doc, int cell, const char *name, SmzPropType type,
                         char *error, size_t cap)
{
    int p = prop_index(doc, cell, name);
    if (p < 0 || doc->props[p].type != type) {
        snprintf(error, cap, "%s requires %s:%s", doc->cells[cell].name, name,
                 type == SMZ_PROP_BOOL ? "bool" : type == SMZ_PROP_INT ? "int" :
                 type == SMZ_PROP_VEC2 ? "vec2" : type == SMZ_PROP_RGBA ? "rgba" :
                 type == SMZ_PROP_Q16 ? "q16" : "enum");
        return -1;
    }
    return p;
}

int smz_validate(const SmzDoc *doc, char *error, size_t error_cap)
{
    if (doc->n_cells == 0) { snprintf(error, error_cap, "document has no cells"); return -1; }
    for (int i = 0; i < doc->n_cells; i++) {
        int pz = required_prop(doc, i, "z", SMZ_PROP_INT, error, error_cap);
        int pe = required_prop(doc, i, "exists", SMZ_PROP_BOOL, error, error_cap);
        if (pz < 0 || pe < 0) return -1;
        double z = doc->props[pz].value[0];
        if (fabs(z - nearbyint(z)) > SMZ_EPS) { snprintf(error, error_cap, "%s.z is not integral", doc->cells[i].name); return -1; }
        double ex = doc->props[pe].value[0];
        if (fabs(ex - nearbyint(ex)) > SMZ_EPS || ex < 0 || ex > 1) {
            snprintf(error, error_cap, "%s.exists is not boolean", doc->cells[i].name); return -1;
        }
        for (int j = 0; j < i; j++) {
            int qz = prop_index(doc, j, "z");
            if (qz >= 0 && llround(doc->props[qz].value[0]) == llround(z)) {
                snprintf(error, error_cap, "duplicate absolute z for %s and %s", doc->cells[j].name, doc->cells[i].name);
                return -1;
            }
        }
        if (doc->cells[i].type == SMZ_CELL_VERTEX &&
            required_prop(doc, i, "xy", SMZ_PROP_VEC2, error, error_cap) < 0) return -1;
        if (doc->cells[i].type == SMZ_CELL_EDGE) {
            if (ex >= 0.5) {
                int ea = prop_index(doc, doc->cells[i].a, "exists");
                int eb = prop_index(doc, doc->cells[i].b, "exists");
                if (ea < 0 || eb < 0 || doc->props[ea].value[0] < 0.5 || doc->props[eb].value[0] < 0.5) {
                    snprintf(error, error_cap, "%s exists but an endpoint does not", doc->cells[i].name); return -1;
                }
            }
            if (required_prop(doc, i, "width", SMZ_PROP_Q16, error, error_cap) < 0 ||
                required_prop(doc, i, "color", SMZ_PROP_RGBA, error, error_cap) < 0 ||
                required_prop(doc, i, "curve", SMZ_PROP_ENUM, error, error_cap) < 0) return -1;
            int pc = prop_index(doc, i, "curve");
            if (strcmp(doc->props[pc].enum_value, "seg") != 0) {
                snprintf(error, error_cap, "%s curve kernel is not implemented (only seg)", doc->cells[i].name);
                return -1;
            }
        }
        if (doc->cells[i].type == SMZ_CELL_FACE) {
            if (required_prop(doc, i, "fill", SMZ_PROP_RGBA, error, error_cap) < 0) return -1;
            const SmzCell *face = &doc->cells[i];
            if (ex >= 0.5) {
                for (int k = 0; k < face->n_edges; k++) {
                    int ee = prop_index(doc, face->edges[k], "exists");
                    if (ee < 0 || doc->props[ee].value[0] < 0.5) {
                        snprintf(error, error_cap, "%s exists but a boundary edge does not", face->name); return -1;
                    }
                }
            }
            const SmzCell *first = &doc->cells[face->edges[0]];
            int start = first->a, current = first->b;
            for (int k = 1; k < face->n_edges; k++) {
                const SmzCell *edge = &doc->cells[face->edges[k]];
                if (edge->a == current) current = edge->b;
                else if (edge->b == current) current = edge->a;
                else { snprintf(error, error_cap, "%s boundary is not a consecutive edge cycle", face->name); return -1; }
            }
            if (current != start) { snprintf(error, error_cap, "%s boundary is not closed", face->name); return -1; }
        }
    }
    return 0;
}

int smz_resolve(SmzDoc *doc)
{
    if (!link_doc(doc) || !assign_variables(doc)) return -1;
    Work work;
    if (!build_work(doc, &work)) {
        if (!doc->error[0]) set_error(doc, 0, "cannot lower properties/rules to psolve");
        free_work(&work); return -1;
    }
    double *solution = work.n_cols ? calloc((size_t)work.n_cols, sizeof(double)) : NULL;
    if (work.n_cols && !solution) { set_error(doc, 0, "solution allocation failed"); free_work(&work); return -1; }
    if (!solve_work(doc, &work, solution)) { free(solution); free_work(&work); return -1; }
    for (int i = 0; i < doc->n_props; i++) {
        SmzProperty *p = &doc->props[i];
        if (p->mode != SMZ_VAR) continue;
        for (int k = 0; k < p->ncomp; k++) {
            double v = solution[p->var[k]];
            if (p->type == SMZ_PROP_Q16 || p->type == SMZ_PROP_VEC2)
                v = nearbyint(v * 65536.0) / 65536.0;
            else if (p->is_integer[k]) v = nearbyint(v);
            p->value[k] = v;
        }
    }
    free(solution); free_work(&work);
    char error[SMZ_ERROR_MAX] = {0};
    if (smz_validate(doc, error, sizeof(error)) != 0) { set_error(doc, 0, error); return -1; }
    return 0;
}

const char *smz_solve_kind_name(SmzSolveKind kind)
{
    return kind == SMZ_SOLVE_LP ? "LP" : kind == SMZ_SOLVE_MIP ? "MIP" : "constants";
}

static const char *cell_sigil(SmzCellType t)
{
    return t == SMZ_CELL_VERTEX ? "+V" : t == SMZ_CELL_EDGE ? "+E" : "+F";
}

static const char *prop_type_name(SmzPropType t)
{
    return t == SMZ_PROP_BOOL ? "bool" : t == SMZ_PROP_INT ? "int" :
           t == SMZ_PROP_Q16 ? "q16" : t == SMZ_PROP_VEC2 ? "vec2" :
           t == SMZ_PROP_RGBA ? "rgba" : "enum";
}

static int index_name_cmp_doc(const void *a, const void *b, void *ctx)
{
    const SmzDoc *doc = ctx;
    int ia = *(const int *)a, ib = *(const int *)b;
    return strcmp(doc->cells[ia].name, doc->cells[ib].name);
}

static void sort_cell_indices(const SmzDoc *doc, int *idx)
{
    for (int i = 0; i < doc->n_cells; i++) idx[i] = i;
    for (int i = 1; i < doc->n_cells; i++) {
        int key = idx[i], j = i - 1;
        while (j >= 0 && index_name_cmp_doc(&idx[j], &key, (void *)doc) > 0) {
            idx[j + 1] = idx[j]; j--;
        }
        idx[j + 1] = key;
    }
}

static void rgba_hex(const double v[4], char out[9])
{
    snprintf(out, 9, "%02X%02X%02X%02X", (unsigned)llround(v[0]),
             (unsigned)llround(v[1]), (unsigned)llround(v[2]), (unsigned)llround(v[3]));
}

int smz_write_resolved(const SmzDoc *doc, const char *path)
{
    FILE *f = path ? fopen(path, "w") : stdout;
    if (!f) return -1;
    fputs("# smazkavg solved; source-order=irrelevant\n", f);
    int *idx = malloc((size_t)doc->n_cells * sizeof(int));
    if (!idx) { if (f != stdout) fclose(f); return -1; }
    sort_cell_indices(doc, idx);
    for (int q = 0; q < doc->n_cells; q++) {
        const SmzCell *c = &doc->cells[idx[q]];
        fprintf(f, "%s %s", cell_sigil(c->type), c->name);
        if (c->type == SMZ_CELL_EDGE) fprintf(f, " %s %s", c->a_name, c->b_name);
        else if (c->type == SMZ_CELL_FACE)
            for (int k = 0; k < c->n_edges; k++) fprintf(f, " %s", c->edge_name[k]);
        fputc('\n', f);
        int *pi = malloc((size_t)doc->n_props * sizeof(int));
        if (!pi) { free(idx); if (f != stdout) fclose(f); return -1; }
        int np = 0;
        for (int i = 0; i < doc->n_props; i++) if (doc->props[i].cell == idx[q]) pi[np++] = i;
        for (int i = 1; i < np; i++) {
            int key = pi[i], j = i - 1;
            while (j >= 0 && strcmp(doc->props[pi[j]].name, doc->props[key].name) > 0) {
                pi[j + 1] = pi[j]; j--;
            }
            pi[j + 1] = key;
        }
        for (int ii = 0; ii < np; ii++) {
            const SmzProperty *p = &doc->props[pi[ii]];
            fprintf(f, "=%s.%s:%s", c->name, p->name, prop_type_name(p->type));
            if (p->type == SMZ_PROP_ENUM) fprintf(f, " %s", p->enum_value);
            else if (p->type == SMZ_PROP_RGBA) { char hex[9]; rgba_hex(p->value, hex); fprintf(f, " %s", hex); }
            else for (int k = 0; k < p->ncomp; k++) fprintf(f, " %.10g", p->value[k]);
            fputc('\n', f);
        }
        free(pi);
    }
    free(idx);
    int ok = !ferror(f);
    if (f != stdout && fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

static int cell_exists(const SmzDoc *doc, int cell)
{
    int p = prop_index(doc, cell, "exists");
    return p >= 0 && doc->props[p].value[0] >= 0.5;
}

static long long cell_z(const SmzDoc *doc, int cell)
{
    int p = prop_index(doc, cell, "z");
    return p >= 0 ? llround(doc->props[p].value[0]) : LLONG_MIN;
}

static int z_cmp(const SmzDoc *doc, int a, int b)
{
    long long za = cell_z(doc, a), zb = cell_z(doc, b);
    if (za != zb) return za < zb ? -1 : 1;
    return strcmp(doc->cells[a].name, doc->cells[b].name);
}

static void sort_z(const SmzDoc *doc, int *idx)
{
    for (int i = 0; i < doc->n_cells; i++) idx[i] = i;
    for (int i = 1; i < doc->n_cells; i++) {
        int key = idx[i], j = i - 1;
        while (j >= 0 && z_cmp(doc, idx[j], key) > 0) { idx[j + 1] = idx[j]; j--; }
        idx[j + 1] = key;
    }
}

static int vertex_xy(const SmzDoc *doc, int cell, double *x, double *y)
{
    int p = prop_index(doc, cell, "xy");
    if (p < 0) return 0;
    *x = doc->props[p].value[0]; *y = doc->props[p].value[1]; return 1;
}

static void svg_rgba(FILE *f, const double c[4])
{
    fprintf(f, "rgba(%u,%u,%u,%.6f)", (unsigned)llround(c[0]),
            (unsigned)llround(c[1]), (unsigned)llround(c[2]), c[3] / 255.0);
}

static int write_face_svg(const SmzDoc *doc, FILE *f, int ci)
{
    const SmzCell *face = &doc->cells[ci];
    int verts[SMZ_MAX_FACE_EDGES + 1];
    for (int k = 0; k < face->n_edges; k++)
        if (!cell_exists(doc, face->edges[k])) return 0;
    const SmzCell *e0 = &doc->cells[face->edges[0]];
    if (!cell_exists(doc, e0->a) || !cell_exists(doc, e0->b)) return 0;
    verts[0] = e0->a; verts[1] = e0->b;
    int current = e0->b;
    for (int k = 1; k < face->n_edges; k++) {
        const SmzCell *e = &doc->cells[face->edges[k]];
        if (!cell_exists(doc, e->a) || !cell_exists(doc, e->b)) return 0;
        if (e->a == current) current = e->b;
        else if (e->b == current) current = e->a;
        else return 0;
        verts[k + 1] = current;
    }
    if (current != verts[0]) return 0;
    int fill = prop_index(doc, ci, "fill");
    fprintf(f, "  <polygon id=\"%s\" points=\"", face->name);
    for (int k = 0; k < face->n_edges; k++) {
        double x, y; if (!vertex_xy(doc, verts[k], &x, &y)) return 0;
        fprintf(f, "%s%.6f,%.6f", k ? " " : "", x, y);
    }
    fputs("\" fill=\"", f); svg_rgba(f, doc->props[fill].value); fputs("\"/>\n", f);
    return 1;
}

static int write_edge_svg(const SmzDoc *doc, FILE *f, int ci)
{
    const SmzCell *e = &doc->cells[ci];
    if (!cell_exists(doc, e->a) || !cell_exists(doc, e->b)) return 0;
    double x0, y0, x1, y1;
    if (!vertex_xy(doc, e->a, &x0, &y0) || !vertex_xy(doc, e->b, &x1, &y1)) return 0;
    int width = prop_index(doc, ci, "width"), color = prop_index(doc, ci, "color");
    fprintf(f, "  <line id=\"%s\" x1=\"%.6f\" y1=\"%.6f\" x2=\"%.6f\" y2=\"%.6f\" stroke=\"",
            e->name, x0, y0, x1, y1);
    svg_rgba(f, doc->props[color].value);
    fprintf(f, "\" stroke-width=\"%.6f\" stroke-linecap=\"round\"/>\n",
            doc->props[width].value[0]);
    return 1;
}

int smz_write_svg(const SmzDoc *doc, const char *path, int width, int height)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%d\" height=\"%d\" viewBox=\"0 0 %d %d\">\n", width, height, width, height);
    fprintf(f, "  <rect width=\"100%%\" height=\"100%%\" fill=\"white\"/>\n");
    int *idx = malloc((size_t)doc->n_cells * sizeof(int));
    if (!idx) { fclose(f); return -1; }
    sort_z(doc, idx);
    int ok = 1;
    for (int i = 0; i < doc->n_cells && ok; i++) {
        int c = idx[i];
        if (!cell_exists(doc, c)) continue;
        if (doc->cells[c].type == SMZ_CELL_EDGE) ok = write_edge_svg(doc, f, c);
        else if (doc->cells[c].type == SMZ_CELL_FACE) ok = write_face_svg(doc, f, c);
    }
    free(idx);
    fputs("</svg>\n", f);
    if (ferror(f) || fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}
