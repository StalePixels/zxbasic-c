/*
 * main.c — ZX BASIC Compiler entry point
 *
 * Ported from src/zxbc/zxbc.py and src/zxbc/args_parser.py
 */
#include "zxbc.h"
#include "args.h"
#include "parser.h"
#include "errmsg.h"
#include "zxbpp.h"
#include "cwalk.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(int argc, char *argv[]) {
    CompilerState cs;
    compiler_init(&cs);

    /* Set cwalk to Unix-style paths for consistent output */
    cwk_path_set_style(CWK_STYLE_UNIX);

    int rc = zxbc_parse_args(argc, argv, &cs.opts);
    if (rc != 0) {
        compiler_destroy(&cs);
        return rc < 0 ? 0 : rc;  /* -1 = --help/--version (success) */
    }

    cs.current_file = cs.opts.input_filename;

    /* Open error file if specified */
    if (cs.opts.stderr_filename) {
        cs.opts.stderr_f = fopen(cs.opts.stderr_filename, "w");
        if (!cs.opts.stderr_f) {
            fprintf(stderr, "Error: cannot open error file '%s'\n", cs.opts.stderr_filename);
            compiler_destroy(&cs);
            return 1;
        }
    }

    if (cs.opts.debug_level > 0) {
        zxbc_info(&cs, "Input file: %s", cs.opts.input_filename);
        zxbc_info(&cs, "Output format: %s", cs.opts.output_file_type);
        zxbc_info(&cs, "Optimization level: %d", cs.opts.optimization_level);
    }

    /* Run preprocessor */
    PreprocState pp;
    preproc_init(&pp);
    pp.debug_level = cs.opts.debug_level;

    {
        const char *arch = cs.opts.architecture ? cs.opts.architecture : "zx48k";
        char stdlib_path[PATH_MAX];
        char runtime_path[PATH_MAX];

        snprintf(stdlib_path, sizeof(stdlib_path), "src/lib/arch/%s/stdlib", arch);
        snprintf(runtime_path, sizeof(runtime_path), "src/lib/arch/%s/runtime", arch);
        if (access(stdlib_path, R_OK) == 0) {
            vec_push(pp.include_paths, arena_strdup(&pp.arena, stdlib_path));
            vec_push(pp.include_paths, arena_strdup(&pp.arena, runtime_path));
        }
    }

    if (cs.opts.include_path)
        vec_push(pp.include_paths, cs.opts.include_path);

    int pp_rc = preproc_file(&pp, cs.opts.input_filename);
    if (pp_rc != 0 || pp.error_count > 0) {
        fprintf(stderr, "zxbc: preprocessing failed\n");
        preproc_destroy(&pp);
        compiler_destroy(&cs);
        return 1;
    }

    char *source = arena_strdup(&cs.arena, strbuf_cstr(&pp.output));
    preproc_destroy(&pp);

    if (cs.opts.debug_level > 1) {
        zxbc_info(&cs, "Preprocessed source (%zu bytes)", strlen(source));
    }

    /* Parse */
    Parser parser;
    parser_init(&parser, &cs, source);
    AstNode *ast = parser_parse(&parser);

    rc = 0;
    if (parser.had_error || !ast) {
        rc = 1;
    }

    /* Use error_count for exit code (matching Python's gl.has_errors) */
    if (cs.error_count > 0)
        rc = 1;

    /* Post-parse validation: check GOTO/GOSUB label targets */
    if (rc == 0 && ast) {
        check_pending_labels(&cs, ast);
        if (cs.error_count > 0)
            rc = 1;
    }

    if (rc == 0 && cs.opts.parse_only) {
        if (cs.opts.debug_level > 0)
            zxbc_info(&cs, "Parse OK (%d top-level statements)", ast->child_count);
    } else if (rc == 0) {
        /* TODO: code generation */
        fprintf(stderr, "zxbc: code generation not yet implemented (Phase 3 in progress)\n");
        rc = 1;
    }

    /* Cleanup */
    if (cs.opts.stderr_f)
        fclose(cs.opts.stderr_f);

    compiler_destroy(&cs);
    return rc;
}
