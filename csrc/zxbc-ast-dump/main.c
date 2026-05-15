/*
 * zxbc-ast-dump — Sprint 8 (Phase 5 part B).
 *
 * Read-only test tool: parse a .bas via the zxbc_parser_lib and emit
 * the resulting AST as JSON to stdout, in the same schema produced by
 * csrc/tests/dump_python_ast.py. Sprint 9's harness diffs the two
 * dumps to detect AST-level port drift.
 *
 * Schema:
 *   {"tag": str, "line": int|null, "attrs": {"name": str?, "type": str?},
 *    "children": [...]}
 *
 * On parse failure: exit non-zero, emit nothing to stdout. The harness
 * wrapping us classifies that as SKIP — C-parse-error.
 *
 * No production .c source is modified — this is a pure consumer of
 * the parser library exposed by csrc/zxbc/CMakeLists.txt.
 */

#include "zxbc.h"
#include "parser.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Map an AstTag to the string tag the Python side emits. AST_SENTENCE
 * is special-cased at the call site — it carries its own kind in the
 * union and is emitted by the per-instance string. */
static const char *dump_tag_name(AstTag tag) {
    switch (tag) {
        case AST_NOP:          return "NOP";
        case AST_NUMBER:       return "NUMBER";
        case AST_STRING:       return "STRING";
        case AST_BINARY:       return "BINARY";
        case AST_UNARY:        return "UNARY";
        case AST_ID:           return "VAR";
        case AST_TYPECAST:     return "TYPECAST";
        case AST_BUILTIN:      return "BUILTIN";
        case AST_CALL:         return "CALL";
        case AST_FUNCCALL:     return "FUNCCALL";
        case AST_FUNCDECL:     return "FUNCDECL";
        case AST_VARDECL:      return "VARDECL";
        case AST_ARRAYDECL:    return "ARRAYDECL";
        case AST_ARRAYACCESS:  return "ARRAYACCESS";
        case AST_ARRAYLOAD:    return "ARRAYLOAD";
        case AST_ARGUMENT:     return "ARGUMENT";
        case AST_ARGLIST:      return "ARGLIST";
        case AST_PARAMLIST:    return "PARAMLIST";
        case AST_BLOCK:        return "BLOCK";
        case AST_SENTENCE:     return "SENTENCE";
        case AST_BOUND:        return "BOUND";
        case AST_BOUNDLIST:    return "BOUNDLIST";
        case AST_ASM:          return "ASM";
        case AST_CONSTEXPR:    return "CONSTEXPR";
        case AST_STRSLICE:     return "STRSLICE";
        case AST_ARRAYINIT:    return "ARRAYINIT";
        case AST_TYPE:         return "TYPE";
        case AST_BASICTYPE:    return "BASICTYPE";
        case AST_TYPEALIAS:    return "TYPEALIAS";
        default:               return "UNKNOWN";
    }
}

static void emit_indent(FILE *out, int depth) {
    for (int i = 0; i < depth; i++) fputs("  ", out);
}

static void emit_json_string(FILE *out, const char *s) {
    fputc('"', out);
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '"' || c == '\\') { fputc('\\', out); fputc(c, out); }
        else if (c == '\n') fputs("\\n", out);
        else if (c == '\r') fputs("\\r", out);
        else if (c == '\t') fputs("\\t", out);
        else if (c < 0x20)  fprintf(out, "\\u%04x", c);
        else                fputc(c, out);
    }
    fputc('"', out);
}

/* The schema's attrs.name comes from per-tag fields:
 *   AST_ID         → u.id.name
 *   AST_VARDECL    → first child (an AST_ID) carries the name
 *   AST_FUNCDECL   → same
 *   AST_SENTENCE   → u.sentence.kind (used as the JSON tag, not the name)
 * For Sprint 8 we extract just the AST_ID case; richer extraction can
 * be added in Round 1 without changing the schema.
 */
static const char *ast_node_name(const AstNode *n) {
    if (!n) return NULL;
    if (n->tag == AST_ID && n->u.id.name) return n->u.id.name;
    return NULL;
}

static const char *ast_node_typename(const AstNode *n) {
    if (!n || !n->type_) return NULL;
    return n->type_->name;
}

static void emit_node(FILE *out, const AstNode *n, int depth) {
    if (!n) { fputs("null", out); return; }

    /* AST_SENTENCE uses its u.sentence.kind as the visible tag (LET, PRINT, FOR…). */
    const char *tag = (n->tag == AST_SENTENCE && n->u.sentence.kind)
                      ? n->u.sentence.kind
                      : dump_tag_name(n->tag);

    fputs("{\n", out);

    emit_indent(out, depth + 1);
    fputs("\"attrs\": {", out);
    bool first_attr = true;
    const char *name = ast_node_name(n);
    if (name) {
        fputs("\"name\": ", out);
        emit_json_string(out, name);
        first_attr = false;
    }
    const char *tname = ast_node_typename(n);
    if (tname) {
        if (!first_attr) fputs(", ", out);
        fputs("\"type\": ", out);
        emit_json_string(out, tname);
    }
    fputs("},\n", out);

    emit_indent(out, depth + 1);
    fputs("\"children\": [", out);
    if (n->child_count == 0) {
        fputs("],\n", out);
    } else {
        fputc('\n', out);
        for (int i = 0; i < n->child_count; i++) {
            emit_indent(out, depth + 2);
            emit_node(out, n->children[i], depth + 2);
            if (i < n->child_count - 1) fputc(',', out);
            fputc('\n', out);
        }
        emit_indent(out, depth + 1);
        fputs("],\n", out);
    }

    emit_indent(out, depth + 1);
    if (n->lineno > 0) fprintf(out, "\"line\": %d,\n", n->lineno);
    else               fputs("\"line\": null,\n", out);

    emit_indent(out, depth + 1);
    fputs("\"tag\": ", out);
    emit_json_string(out, tag);
    fputc('\n', out);

    emit_indent(out, depth);
    fputc('}', out);
}

static char *slurp_file(const char *path) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    rewind(fp);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)sz, fp) != (size_t)sz) { free(buf); fclose(fp); return NULL; }
    buf[sz] = '\0';
    fclose(fp);
    return buf;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: zxbc-ast-dump <path/to/file.bas>\n");
        return 2;
    }

    char *src = slurp_file(argv[1]);
    if (!src) {
        fprintf(stderr, "zxbc-ast-dump: cannot read %s\n", argv[1]);
        return 2;
    }

    CompilerState cs;
    compiler_init(&cs);
    cs.current_file = argv[1];

    Parser parser;
    parser_init(&parser, &cs, src);
    AstNode *root = parser_parse(&parser);

    int rc = 0;
    if (!root || parser.had_error || cs.error_count > 0) {
        /* Empty stdout on parse failure — harness classifies as SKIP — C-parse-error. */
        rc = 1;
    } else {
        emit_node(stdout, root, 0);
        fputc('\n', stdout);
    }

    compiler_destroy(&cs);
    free(src);
    return rc;
}
