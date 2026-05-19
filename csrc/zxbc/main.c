/*
 * main.c — ZX BASIC Compiler entry point
 *
 * Ported from src/zxbc/zxbc.py and src/zxbc/args_parser.py
 */
#include "zxbc.h"
#include "args.h"
#include "parser.h"
#include "errmsg.h"
#include "visitor.h"
#include "codegen.h"
#include "zxbpp.h"
#include "cwalk.h"
#include "utils.h"

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

    /* Built-in include path (zxbpp.set_include_path / get_include_path,
     * src/zxbpp/zxbpp.py:142-164) then the -I dirs (zxbpp.py:193). The
     * built-in pair is anchored to the running executable's own
     * directory — the C analogue of get_include_path()'s
     * os.path.dirname(__file__) (CWD-INDEPENDENT, zxbpp.py:144-152) —
     * and realpath()'d (mirrors os.path.realpath). Pushed
     * UNCONDITIONALLY (set_include_path sets INCLUDE_MAP for every arch
     * with no existence check; only per-file lookup skips missing dirs,
     * zxbpp.py:158-205) — no access() gate, matching Python. The -I
     * value is colon-SPLIT and appended AFTER the built-ins (zxbpp.py:193
     * i_path.extend(OPTIONS.include_path.split(":"))), empty segments
     * skipped, order preserved. This block is BYTE-IDENTICAL to the
     * second-pass block in codegen.c (codegen.c:531-532 mandate). */
    {
        const char *arch = cs.opts.architecture ? cs.opts.architecture : "zx48k";
        char exe_dir[PATH_MAX];
        char raw_path[PATH_MAX];
        char real_path[PATH_MAX];

        if (get_executable_dir(argv[0], exe_dir, sizeof(exe_dir))) {
            snprintf(raw_path, sizeof(raw_path),
                     "%s/../../../src/lib/arch/%s/stdlib", exe_dir, arch);
            if (realpath(raw_path, real_path))
                vec_push(pp.include_paths, arena_strdup(&pp.arena, real_path));
            snprintf(raw_path, sizeof(raw_path),
                     "%s/../../../src/lib/arch/%s/runtime", exe_dir, arch);
            if (realpath(raw_path, real_path))
                vec_push(pp.include_paths, arena_strdup(&pp.arena, real_path));
        }
    }

    if (cs.opts.include_path && cs.opts.include_path[0]) {
        char ipbuf[PATH_MAX];
        if (strlen(cs.opts.include_path) < sizeof(ipbuf)) {
            strcpy(ipbuf, cs.opts.include_path);
            char *save = NULL;
            for (char *seg = strtok_r(ipbuf, ":", &save);
                 seg != NULL;
                 seg = strtok_r(NULL, ":", &save)) {
                if (seg[0])
                    vec_push(pp.include_paths, arena_strdup(&pp.arena, seg));
            }
        }
    }

    int pp_rc = preproc_file(&pp, cs.opts.input_filename);
    if (pp_rc != 0 || pp.error_count > 0) {
        /* Python prints no extra trailer on preprocess failure — the
         * preproc error itself was already emitted (zxbpp.py path). */
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

    /* Post-parse validation: check GOTO/GOSUB label targets and pending calls */
    if (rc == 0 && ast) {
        check_pending_labels(&cs, ast);
        check_pending_calls(&cs);

        /* Check READ without DATA (matches Python translator check) */
        if (cs.data_is_used && cs.datas.len == 0) {
            zxbc_error(&cs, 0, "No DATA defined");
        }

        /* Faithful post-parse error gate (src/zxbc/zxbc.py:108).
         * Python returns 1 IMMEDIATELY after parser.parse() if
         * gl.has_errors, BEFORE running any visitor pass. The C
         * check_pending_labels / check_pending_calls / No-DATA checks
         * above are the C analogue of the work Python's p_start +
         * parse() complete before that gate; if any of them set an
         * error, Python's first `if gl.has_errors: return 1`
         * (zxbc.py:108) short-circuits and the OptimizerVisitor (which
         * would emit e.g. the "Useless empty IF ignored" [W140]
         * warning) never runs. Mirror that here: when an error is
         * already present, set rc=1 and skip visitor_run_passes so the
         * C does not emit post-error visitor warnings Python suppresses.
         * This only short-circuits an already-error state — it can
         * never add a rejection, only suppress post-error output to
         * match Python (FALSE_POS-safe by construction). The no-error
         * path is unchanged. */
        if (cs.error_count > 0) {
            rc = 1;
        } else {
            /* Post-parse semantic passes (Python order: Unreachable ->
             * FunctionGraph -> Optimizer; src/zxbc/zxbc.py:107-141).
             * S2.1 groundwork: inert no-op until the passes land in
             * S2.2-S2.4. A pass signals an error via zxbc_error() ->
             * cs.error_count, which the re-check below maps to rc = 1
             * (mirrors Python gl.has_errors, zxbc.py:139-141). */
            visitor_run_passes(&cs, ast);

            if (cs.error_count > 0)
                rc = 1;
        }
    }

    if (rc == 0 && cs.opts.parse_only) {
        if (cs.opts.debug_level > 0)
            zxbc_info(&cs, "Parse OK (%d top-level statements)", ast->child_count);
    } else if (rc == 0) {
        /* Code generation (zxbc.py:125-214, output_file_type==ASM path). */
        rc = codegen_emit(&cs, ast);
        if (cs.error_count > 0)
            rc = 1;
    }

    /* Cleanup */
    if (cs.opts.stderr_f)
        fclose(cs.opts.stderr_f);

    compiler_destroy(&cs);
    return rc;
}
