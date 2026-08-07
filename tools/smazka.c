#include "smazka.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(const char *argv0)
{
    fprintf(stderr,
        "SmazkaVG constraint-first core\n"
        "usage: %s <document.smazka> [--resolved path|-] [--svg path W H] [--check]\n",
        argv0);
}

int main(int argc, char **argv)
{
    if (argc < 2) { usage(argv[0]); return 2; }
    const char *input = argv[1];
    const char *resolved = NULL;
    const char *svg = NULL;
    int width = 512, height = 512;
    int check_only = 0;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--resolved") == 0 && i + 1 < argc) resolved = argv[++i];
        else if (strcmp(argv[i], "--svg") == 0 && i + 3 < argc) {
            svg = argv[++i]; width = atoi(argv[++i]); height = atoi(argv[++i]);
            if (width < 1 || width > 16384 || height < 1 || height > 16384) {
                fprintf(stderr, "smazka: SVG dimensions must be in [1,16384]\n"); return 2;
            }
        } else if (strcmp(argv[i], "--check") == 0) check_only = 1;
        else { fprintf(stderr, "smazka: unknown/incomplete option: %s\n", argv[i]); usage(argv[0]); return 2; }
    }

    SmzDoc *doc = calloc(1, sizeof(*doc));
    if (!doc) { fprintf(stderr, "smazka: allocation failed\n"); return 2; }
    smz_init(doc);
    if (smz_parse_file(doc, input) != 0 || smz_resolve(doc) != 0) {
        if (doc->error_line)
            fprintf(stderr, "smazka:%d: %s\n", doc->error_line, doc->error);
        else fprintf(stderr, "smazka: %s\n", doc->error[0] ? doc->error : "unknown failure");
        smz_free(doc); free(doc); return 1;
    }
    fprintf(stderr, "smazka: %d cells, %d properties, %d rules; solved by %s; unique absolute Z\n",
            doc->n_cells, doc->n_props, doc->n_rules, smz_solve_kind_name(doc->solve_kind));
    for (int i = 0; i < doc->n_rules; i++) {
        const SmzRule *rule = &doc->rules[i];
        const SmzProperty *prop = &doc->props[rule->prop];
        double value = prop->value[rule->component];
        if (rule->type == SMZ_RULE_SOFT) {
            double residual = value - rule->rhs;
            if (residual < 0) residual = -residual;
            fprintf(stderr, "status=%s tier=%d rule=%s residual=%.10g\n",
                    rule->status, rule->tier, rule->name, residual);
        } else if (rule->type == SMZ_RULE_OBJECTIVE) {
            fprintf(stderr, "status=%s tier=%d rule=%s value=%.10g\n",
                    rule->status, rule->tier, rule->name, value);
        }
    }

    if (resolved && smz_write_resolved(doc, strcmp(resolved, "-") == 0 ? NULL : resolved) != 0) {
        fprintf(stderr, "smazka: cannot write resolved document\n"); smz_free(doc); free(doc); return 1;
    }
    if (svg && smz_write_svg(doc, svg, width, height) != 0) {
        fprintf(stderr, "smazka: cannot write SVG (invalid boundary or I/O failure)\n"); smz_free(doc); free(doc); return 1;
    }
    if (!check_only && !resolved && !svg) smz_write_resolved(doc, NULL);
    smz_free(doc);
    free(doc);
    return 0;
}
