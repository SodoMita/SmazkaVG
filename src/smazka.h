#ifndef SMAZKA_H
#define SMAZKA_H

#include <stddef.h>
#include <stdio.h>

#define SMZ_MAX_CELLS       2048
#define SMZ_MAX_PROPERTIES  8192
#define SMZ_MAX_RULES       8192
#define SMZ_MAX_FACE_EDGES  1024
#define SMZ_NAME_MAX        64
#define SMZ_STATUS_MAX      24
#define SMZ_ERROR_MAX       512

typedef enum {
    SMZ_CELL_VERTEX,
    SMZ_CELL_EDGE,
    SMZ_CELL_FACE
} SmzCellType;

typedef enum {
    SMZ_PROP_BOOL,
    SMZ_PROP_INT,
    SMZ_PROP_Q16,
    SMZ_PROP_VEC2,
    SMZ_PROP_RGBA,
    SMZ_PROP_ENUM
} SmzPropType;

typedef enum {
    SMZ_CONST,
    SMZ_VAR
} SmzPropMode;

typedef enum {
    SMZ_RULE_HARD,
    SMZ_RULE_SOFT,
    SMZ_RULE_OBJECTIVE
} SmzRuleType;

typedef enum {
    SMZ_OP_EQ,
    SMZ_OP_LE,
    SMZ_OP_GE,
    SMZ_OP_APPROX,
    SMZ_OP_MIN,
    SMZ_OP_MAX
} SmzRuleOp;

typedef enum {
    SMZ_SOLVE_NONE,
    SMZ_SOLVE_LP,
    SMZ_SOLVE_MIP
} SmzSolveKind;

typedef struct {
    char name[SMZ_NAME_MAX];
    SmzCellType type;
    char a_name[SMZ_NAME_MAX];
    char b_name[SMZ_NAME_MAX];
    int a, b;
    char (*edge_name)[SMZ_NAME_MAX];
    int *edges;
    int n_edges;
    int line;
} SmzCell;

typedef struct {
    char cell_name[SMZ_NAME_MAX];
    char name[SMZ_NAME_MAX];
    SmzPropType type;
    SmzPropMode mode;
    int ncomp;
    double value[4];
    double seed[4];
    double lo[4];
    double hi[4];
    int var[4];
    int is_integer[4];
    char enum_value[SMZ_NAME_MAX];
    int cell;
    int line;
} SmzProperty;

typedef struct {
    SmzRuleType type;
    SmzRuleOp op;
    char status[SMZ_STATUS_MAX];
    char name[SMZ_NAME_MAX];
    int tier;
    char ref[SMZ_NAME_MAX * 2];
    double rhs;
    int prop;
    int component;
    int line;
} SmzRule;

typedef struct {
    SmzCell cells[SMZ_MAX_CELLS];
    int n_cells;
    SmzProperty props[SMZ_MAX_PROPERTIES];
    int n_props;
    SmzRule rules[SMZ_MAX_RULES];
    int n_rules;
    int n_base_vars;
    SmzSolveKind solve_kind;
    char error[SMZ_ERROR_MAX];
    int error_line;
} SmzDoc;

void smz_init(SmzDoc *doc);
void smz_free(SmzDoc *doc);
int smz_parse_file(SmzDoc *doc, const char *path);
int smz_resolve(SmzDoc *doc);
int smz_validate(const SmzDoc *doc, char *error, size_t error_cap);
int smz_write_resolved(const SmzDoc *doc, const char *path);
int smz_write_svg(const SmzDoc *doc, const char *path, int width, int height);
const char *smz_solve_kind_name(SmzSolveKind kind);

#endif
