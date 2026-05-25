/*
 * ply_astcmp_harness.c — Phase D parallel-validation harness.
 *
 * For one .bas: parse it BOTH ways from the same CompilerState model —
 *   (1) the production recursive-descent parser (parser_parse), and
 *   (2) the ported PLY LALR(1) engine + reduce-actions (plyparse_program) —
 * and deep-compare the resulting ASTs. Reports per-file:
 *   WIRED      — the engine reduced the whole program with no unported action
 *   UNWIRED:N  — hit not-yet-ported production N (or p_error if N<0)
 *   EQUAL      — WIRED and the engine AST deep-equals the production AST
 *   DIFF       — WIRED but the ASTs differ (prints the first divergence)
 *
 * This is the Phase-D meter: grow EQUAL across the valid corpus as actions are
 * wired. Additive — does not touch the production path.
 *
 * Usage: ply-astcmp [--verbose] <file.bas>
 *   exit 0 = EQUAL, 1 = DIFF, 2 = UNWIRED, 3 = parse/setup error
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "zxbc.h"
#include "parser.h"

void compiler_init(CompilerState *cs);

static int g_verbose = 0;

/* ---- deep AST comparator. Compares tag, lineno-independent structure, and
 * the per-tag semantic fields that distinguish nodes. Returns true if equal;
 * on first diff, writes a description to `why`. ---- */
static bool streq(const char *a, const char *b) {
    if (a == b) return true;
    if (!a || !b) return false;
    return strcmp(a, b) == 0;
}

static bool ast_equal(const AstNode *a, const AstNode *b, char *why, size_t wn,
                      const char *path) {
    if (a == NULL && b == NULL) return true;
    if (a == NULL || b == NULL) {
        snprintf(why, wn, "%s: one side NULL (%s vs %s)", path,
                 a ? ast_tag_name(a->tag) : "NULL",
                 b ? ast_tag_name(b->tag) : "NULL");
        return false;
    }
    if (a->tag != b->tag) {
        snprintf(why, wn, "%s: tag %s vs %s", path,
                 ast_tag_name(a->tag), ast_tag_name(b->tag));
        return false;
    }
    /* per-tag fields */
    switch (a->tag) {
        case AST_NUMBER:
            if (a->u.number.value != b->u.number.value) {
                snprintf(why, wn, "%s: NUMBER %g vs %g", path,
                         a->u.number.value, b->u.number.value);
                return false;
            }
            break;
        case AST_STRING:
            if (!streq(a->u.string.value, b->u.string.value)) {
                snprintf(why, wn, "%s: STRING '%s' vs '%s'", path,
                         a->u.string.value, b->u.string.value);
                return false;
            }
            break;
        case AST_BINARY:
            if (!streq(a->u.binary.operator, b->u.binary.operator)) {
                snprintf(why, wn, "%s: BINARY op '%s' vs '%s'", path,
                         a->u.binary.operator, b->u.binary.operator);
                return false;
            }
            break;
        case AST_UNARY:
            if (!streq(a->u.unary.operator, b->u.unary.operator)) {
                snprintf(why, wn, "%s: UNARY op '%s' vs '%s'", path,
                         a->u.unary.operator, b->u.unary.operator);
                return false;
            }
            break;
        case AST_ID:
            if (!streq(a->u.id.name, b->u.id.name)) {
                snprintf(why, wn, "%s: ID '%s' vs '%s'", path,
                         a->u.id.name, b->u.id.name);
                return false;
            }
            break;
        case AST_SENTENCE:
            if (!streq(a->u.sentence.kind, b->u.sentence.kind)) {
                snprintf(why, wn, "%s: SENTENCE '%s' vs '%s'", path,
                         a->u.sentence.kind, b->u.sentence.kind);
                return false;
            }
            break;
        case AST_BUILTIN:
            if (!streq(a->u.builtin.fname, b->u.builtin.fname)) {
                snprintf(why, wn, "%s: BUILTIN '%s' vs '%s'", path,
                         a->u.builtin.fname, b->u.builtin.fname);
                return false;
            }
            break;
        default:
            break;
    }
    /* type tag/name (a coarse type check — exact type identity is structural) */
    if ((a->type_ == NULL) != (b->type_ == NULL)) {
        snprintf(why, wn, "%s: type_ presence differs (%s)", path,
                 ast_tag_name(a->tag));
        return false;
    }
    if (a->type_ && b->type_ && a->type_->basic_type != b->type_->basic_type) {
        snprintf(why, wn, "%s: basic_type %d vs %d", path,
                 a->type_->basic_type, b->type_->basic_type);
        return false;
    }
    /* children */
    if (a->child_count != b->child_count) {
        snprintf(why, wn, "%s <%s>: child_count %d vs %d", path,
                 ast_tag_name(a->tag), a->child_count, b->child_count);
        return false;
    }
    for (int i = 0; i < a->child_count; i++) {
        char sub[256];
        snprintf(sub, sizeof(sub), "%s/%s[%d]", path, ast_tag_name(a->tag), i);
        if (!ast_equal(a->children[i], b->children[i], why, wn, sub))
            return false;
    }
    return true;
}

static void dump_ast(const AstNode *n, int depth, const char *which) {
    if (!n) { printf("%*s%s<NULL>\n", depth * 2, "", which); return; }
    printf("%*s%s%s", depth * 2, "", which, ast_tag_name(n->tag));
    if (n->tag == AST_SENTENCE && n->u.sentence.kind) printf(" '%s'", n->u.sentence.kind);
    else if (n->tag == AST_ID && n->u.id.name) printf(" '%s'", n->u.id.name);
    else if (n->tag == AST_NUMBER) printf(" %g", n->u.number.value);
    else if (n->tag == AST_BINARY && n->u.binary.operator) printf(" '%s'", n->u.binary.operator);
    printf("  (ch=%d)\n", n->child_count);
    for (int i = 0; i < n->child_count; i++) dump_ast(n->children[i], depth + 1, "");
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = malloc(n + 1);
    if (fread(s, 1, n, f) != (size_t)n) { /* best effort */ }
    s[n] = 0;
    fclose(f);
    return s;
}

int main(int argc, char **argv) {
    int argi = 1;
    int do_dump = 0;
    while (argi < argc && argv[argi][0] == '-') {
        if (strcmp(argv[argi], "--verbose") == 0) g_verbose = 1;
        else if (strcmp(argv[argi], "--dump") == 0) do_dump = 1;
        else break;
        argi++;
    }
    if (argi >= argc) { fprintf(stderr, "usage: %s [--verbose] <file.bas>\n", argv[0]); return 3; }
    const char *path = argv[argi];
    char *src = read_file(path);
    if (!src) { perror("open"); return 3; }

    /* (1) production parser */
    CompilerState cs1;
    compiler_init(&cs1);
    cs1.current_file = (char *)path;
    Parser p1;
    parser_init(&p1, &cs1, src);
    AstNode *prod_ast = parser_parse(&p1);

    /* (2) PLY engine parser (separate CompilerState so symbol tables don't
     * interfere). */
    CompilerState cs2;
    compiler_init(&cs2);
    cs2.current_file = (char *)path;
    Parser p2;
    parser_init_noprime(&p2, &cs2, src);
    bool unwired = false; int unwired_prod = 0;
    AstNode *eng_ast = plyparse_program(&p2, &unwired, &unwired_prod);

    if (do_dump) {
        printf("=== PRODUCTION AST ===\n"); dump_ast(prod_ast, 0, "");
        printf("=== ENGINE AST (unwired=%d:%d) ===\n", unwired, unwired_prod);
        dump_ast(eng_ast, 0, "");
    }
    if (unwired) {
        printf("UNWIRED:%d\n", unwired_prod);
        return 2;
    }
    if (!prod_ast || !eng_ast) {
        printf("UNWIRED:parse-null (prod=%p eng=%p)\n",
               (void *)prod_ast, (void *)eng_ast);
        return 2;
    }
    char why[512] = "";
    bool eq = ast_equal(prod_ast, eng_ast, why, sizeof(why), "");
    if (eq) {
        printf("EQUAL\n");
        free(src);
        return 0;
    }
    printf("DIFF\t%s\n", why);
    free(src);
    return 1;
}
