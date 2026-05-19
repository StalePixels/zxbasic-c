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
#include "config_file.h"

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

    /* Format-gate — faithful port of args_config.py:151-152:
     *
     *   if options.append_binary and OPTIONS.output_file_type not in
     *       {FileType.TAP, FileType.TZX}:
     *       parser.error("Option --append-binary needs either tap or
     *                     tzx output format")
     *
     * ONLY options.append_binary gates (NOT append_headless_binary —
     * Python only checks append_binary). parser.error prints to stderr
     * and exits 2. The C parser.error-analogue convention (see
     * zxbasm/main.c:138, the args_config.py:158-159 "No such file or
     * directory" parser.error port) is: fprintf(stderr, "error: <msg>")
     * then exit 2. Fires after args are parsed and output_file_type is
     * resolved (last-wins in args.c), before any compilation/output.
     * Scope: this gate emits exit-2 + the one error: line ONLY — the
     * argparse usage: preamble is S7.2d, deliberately not reproduced
     * here. */
    if (cs.opts.append_binary_count > 0
        && !(cs.opts.output_file_type
             && (strcmp(cs.opts.output_file_type, "tap") == 0
                 || strcmp(cs.opts.output_file_type, "tzx") == 0))) {
        fprintf(stderr,
            "error: Option --append-binary needs either tap or tzx output format\n");
        compiler_destroy(&cs);
        return 2;
    }

    cs.current_file = cs.opts.input_filename;

    /* Command-line flag deprecation warnings (args_config.py:107-132).
     * Python emits these in args_config (BEFORE OPTIONS.stderr is
     * rebound to the -e file at args_config.py:171-172), and
     * OPTIONS.stderr defaults to sys.stderr (config.py:202). So these
     * warnings ALWAYS go to the real stderr, even with -e FILE — the
     * -e file never carries a WARNING: line. We replicate that ordering
     * here: cs.error_cache is already initialised (compiler.c:1520, via
     * compiler_init at main.c:22) and cs.opts.stderr_f is still NULL
     * (the -e fopen block below has not run yet), so err_stream() in
     * zxbc_msg_output resolves to the real stderr — Python-faithful.
     *
     * Scope: warning emission + the --strict-bool force-reset only.
     * output_file_type resolution stays last-wins (set in args.c) — out
     * of S7.2b scope. The elif chain is mandatory: -f/--output-format
     * SUPPRESSES all four format-flag deprecations, and only ONE ever
     * fires. The strict-bool block is independent and can fire
     * alongside one of the four. */
    if (cs.opts.opt_seen_output_format) {
        /* -f / --output-format given: NO deprecation warning. */
    } else if (cs.opts.opt_seen_tzx) {
        zxbc_warning_command_line_flag_deprecation(
            &cs, "--tzx (use -f tzx or --output-format=tzx instead)");
    } else if (cs.opts.opt_seen_tap) {
        zxbc_warning_command_line_flag_deprecation(
            &cs, "--tap (use -f tap or --output-format=tap instead)");
    } else if (cs.opts.opt_seen_asm) {
        zxbc_warning_command_line_flag_deprecation(
            &cs, "--asm (use -f asm or --output-format=asm instead)");
    } else if (cs.opts.opt_seen_emit_backend) {
        zxbc_warning_command_line_flag_deprecation(
            &cs, "--emit-backend (use -f ir or --output-format=ir instead)");
    }

    if (cs.opts.strict_bool) {
        cs.opts.strict_bool = false;
        zxbc_warning_command_line_flag_deprecation(
            &cs, "--strict-bool is deprecated (no longer needed)");
    }

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

    /* User -D/--define defines seeded into the BASIC first pass.
     * Python: args_config.py:91-96 populates OPTIONS.__DEFINES, which
     * zxbpp's reset_id_table() (zxbpp.py:106-113) seeds into ID_TABLE
     * for the first (BASIC) pass. Split on the FIRST '=' (i.split("=",1)).
     * Byte-critical: gates BASIC-side #ifdef/#ifndef. */
    for (int di = 0; di < cs.opts.defines_count; di++) {
        const char *raw = cs.opts.defines[di];
        /* Heap scratch sized to the arg: Python puts no length cap on
         * -D, so neither do we (a fixed buffer would silently drop a
         * long define — the silent-divergence footgun). preproc_define
         * arena_strdup's name+body, so scratch need only live across
         * the call. */
        char *scratch = malloc(strlen(raw) + 1);
        const char *dname, *dval;
        if (scratch) {
            compiler_split_define(raw, scratch, &dname, &dval);
            preproc_define(&pp, dname, dval, 0, NULL);
            free(scratch);
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

    /* --save-config FILE (faithful port of zxbc.py:235 save_config()).
     *
     * Python zxbc.py:71-73:
     *   def save_config(options):
     *       if not gl.has_errors and options.save_config:
     *           src.api.config.save_config_into_file(
     *               options.save_config, ConfigSections.ZXBC)
     *
     * It is called at TWO sites, both AFTER the work for that path is
     * done and IMMEDIATELY before `return gl.has_errors`:
     *   - parse-only:   zxbc.py:139-141  (after FunctionTranslator)
     *   - full compile: zxbc.py:235      (after generate_binary / mmap)
     * Both sites are only reached when `not gl.has_errors`, and
     * save_config() re-asserts `not gl.has_errors`. The C `rc == 0`
     * here is exactly that no-errors gate: rc is forced to 1 on any
     * parser / cs.error_count / post-parse error and to >0 on a
     * codegen/assembly failure (a Python `return 1`/`return 5` that
     * never reaches save_config). Placed after BOTH the parse-only and
     * codegen branches and before cleanup — the C analogue of
     * save_config()'s position right before `return gl.has_errors`.
     * Section name is the literal "zxbc" (ConfigSections.ZXBC).
     *
     * emit-backend exclusion: Python's emit_backend block
     * (zxbc.py:156-168) `if OPTIONS.emit_backend: ...; return 0`
     * returns BEFORE the full-compile save_config() at zxbc.py:235, so
     * a non-parse-only `-E` run does NOT write the config. The
     * parse-only save (zxbc.py:139-141) is checked EARLIER than the
     * emit_backend block, so `--parse-only --save-config` still writes
     * even with -E. Faithful gate: parse_only || !emit_backend.
     *
     * The emitted [zxbc] key set / ORDER mirrors Python's ordered
     * OPTIONS() dict at save time (config.py init() order, then the
     * arch backend's ADD_IF_NOT_DEFINED org/heap_size/heap_address/
     * headerless, then zxbpp's debug_zxbpp), with config.py:168 skip
     * rules: __-prefixed / value-None / OPTIONS_NOT_SAVED skipped,
     * bool -> str(v).lower() ("true"/"false"), opt_strategy ->
     * "size"/"speed"/"auto", ints decimal, strings raw. Verified
     * byte-identical against the Python reference. */
    if (rc == 0 && cs.opts.save_config
        && (cs.opts.parse_only || !cs.opts.emit_backend)) {
        ConfigWriter *w = config_writer_new();
        char nbuf[32];

        /* 1. stderr_filename  str  ignore_none -> emit only if set */
        if (cs.opts.stderr_filename)
            config_writer_add(w, "stderr_filename", cs.opts.stderr_filename);
        /* 2. debug_level  int */
        snprintf(nbuf, sizeof(nbuf), "%d", cs.opts.debug_level);
        config_writer_add(w, "debug_level", nbuf);
        /* 3. optimization_level  int */
        snprintf(nbuf, sizeof(nbuf), "%d", cs.opts.optimization_level);
        config_writer_add(w, "optimization_level", nbuf);
        /* 4. case_insensitive  bool */
        config_writer_add(w, "case_insensitive", cs.opts.case_insensitive ? "true" : "false");
        /* 5. array_base  int */
        snprintf(nbuf, sizeof(nbuf), "%d", cs.opts.array_base);
        config_writer_add(w, "array_base", nbuf);
        /* 6. default_byref  bool (no CLI flag — always default false) */
        config_writer_add(w, "default_byref", cs.opts.default_byref ? "true" : "false");
        /* 7. max_syntax_errors  int */
        snprintf(nbuf, sizeof(nbuf), "%d", cs.opts.max_syntax_errors);
        config_writer_add(w, "max_syntax_errors", nbuf);
        /* 8. string_base  int */
        snprintf(nbuf, sizeof(nbuf), "%d", cs.opts.string_base);
        config_writer_add(w, "string_base", nbuf);
        /* 9. memory_map  str  ignore_none -> emit only if set */
        if (cs.opts.memory_map)
            config_writer_add(w, "memory_map", cs.opts.memory_map);
        /* 10. force_asm_brackets  bool */
        config_writer_add(w, "force_asm_brackets", cs.opts.force_asm_brackets ? "true" : "false");
        /* 11. use_basic_loader  bool */
        config_writer_add(w, "use_basic_loader", cs.opts.use_basic_loader ? "true" : "false");
        /* 12. autorun  bool */
        config_writer_add(w, "autorun", cs.opts.autorun ? "true" : "false");
        /* 13. output_file_type  str */
        config_writer_add(w, "output_file_type",
                          cs.opts.output_file_type ? cs.opts.output_file_type : "bin");
        /* 14. include_path  str  (default "" — emitted, value empty) */
        config_writer_add(w, "include_path",
                          cs.opts.include_path ? cs.opts.include_path : "");
        /* 15. memory_check  bool */
        config_writer_add(w, "memory_check", cs.opts.memory_check ? "true" : "false");
        /* 16. strict_bool  bool */
        config_writer_add(w, "strict_bool", cs.opts.strict_bool ? "true" : "false");
        /* 17. array_check  bool */
        config_writer_add(w, "array_check", cs.opts.array_check ? "true" : "false");
        /* 18. enable_break  bool */
        config_writer_add(w, "enable_break", cs.opts.enable_break ? "true" : "false");
        /* 19. emit_backend  bool */
        config_writer_add(w, "emit_backend", cs.opts.emit_backend ? "true" : "false");
        /* 20. explicit  bool */
        config_writer_add(w, "explicit", cs.opts.explicit_ ? "true" : "false");
        /* 21. strict  bool */
        config_writer_add(w, "strict", cs.opts.strict ? "true" : "false");
        /* 22. zxnext  bool */
        config_writer_add(w, "zxnext", cs.opts.zxnext ? "true" : "false");
        /* 23. architecture  str  (options.arch default "zx48k" — never
         *     None; OPTIONS.architecture = options.arch always sets it) */
        config_writer_add(w, "architecture",
                          cs.opts.architecture ? cs.opts.architecture : "zx48k");
        /* 24. expected_warnings  int */
        snprintf(nbuf, sizeof(nbuf), "%d", cs.opts.expected_warnings);
        config_writer_add(w, "expected_warnings", nbuf);
        /* 25. hide_warning_codes  bool */
        config_writer_add(w, "hide_warning_codes", cs.opts.hide_warning_codes ? "true" : "false");
        /* 26. opt_strategy  OptimizationStrategy -> "size"/"speed"/"auto" */
        config_writer_add(w, "opt_strategy",
                          cs.opts.opt_strategy == OPT_STRATEGY_SIZE ? "size"
                          : cs.opts.opt_strategy == OPT_STRATEGY_SPEED ? "speed"
                          : "auto");
        /* 27. org  int  (arch backend ADD_IF_NOT_DEFINED, default 32768) */
        snprintf(nbuf, sizeof(nbuf), "%d", cs.opts.org);
        config_writer_add(w, "org", nbuf);
        /* 28. heap_size  int  (arch backend, default 4768) */
        snprintf(nbuf, sizeof(nbuf), "%d", cs.opts.heap_size);
        config_writer_add(w, "heap_size", nbuf);
        /* 29. heap_address  int  (arch backend default None,
         *     ignore_none=False but value stays None when unset ->
         *     skipped; C -1 == "auto"/None -> skip, else emit) */
        if (cs.opts.heap_address != -1) {
            snprintf(nbuf, sizeof(nbuf), "%d", cs.opts.heap_address);
            config_writer_add(w, "heap_address", nbuf);
        }
        /* 30. headerless  bool  (arch backend, default false) */
        config_writer_add(w, "headerless", cs.opts.headerless ? "true" : "false");
        /* 31. debug_zxbpp  bool  (zxbpp ADD_IF_NOT_DEFINED default
         *     False; zxbpp.py:1030 only updates it AFTER save_config in
         *     the call chain, so it is always false at save time) */
        config_writer_add(w, "debug_zxbpp", "false");

        int wrc = config_writer_save(w, cs.opts.save_config, "zxbc");
        config_writer_free(w);

        if (wrc == -2) {
            /* config.py:160-164 DuplicateSectionError/DuplicateOptionError */
            char msg[1024];
            snprintf(msg, sizeof(msg),
                     "Invalid config file '%s': it has duplicated fields",
                     cs.opts.save_config);
            zxbc_msg_output(&cs, msg);
            if (cs.opts.stderr_f)
                fclose(cs.opts.stderr_f);
            compiler_destroy(&cs);
            return 1;
        }
        if (wrc == -1) {
            /* config.py:180-184 IOError */
            char msg[1024];
            snprintf(msg, sizeof(msg),
                     "Can't write config file '%s'", cs.opts.save_config);
            zxbc_msg_output(&cs, msg);
            if (cs.opts.stderr_f)
                fclose(cs.opts.stderr_f);
            compiler_destroy(&cs);
            return 1;
        }
    }

    /* Cleanup */
    if (cs.opts.stderr_f)
        fclose(cs.opts.stderr_f);

    compiler_destroy(&cs);
    return rc;
}
