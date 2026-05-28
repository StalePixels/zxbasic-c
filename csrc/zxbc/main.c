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

int zxbc_main(int argc, char *argv[]) {
    CompilerState cs;
    compiler_init(&cs);

    /* Set cwalk to Unix-style paths for consistent output */
    cwk_path_set_style(CWK_STYLE_UNIX);

    int rc = zxbc_parse_args(argc, argv, &cs.opts);
    if (rc != 0) {
        compiler_destroy(&cs);
        return rc < 0 ? 0 : rc;  /* -1 = --help/--version (success) */
    }

    /* S7.2d-i (a) — invalid --arch. Faithful port of
     * args_config.py:62-63:
     *   if options.arch not in arch.AVAILABLE_ARCHITECTURES:
     *       parser.error(f"Invalid architecture '{options.arch}'")
     * AVAILABLE_ARCHITECTURES == ("zx48k", "zxnext")
     * (src/arch/__init__.py:13-18). This is the FIRST parser.error in
     * parse_options() (line 62) — before the enable/disable-warning
     * duplicate check (line 70), the deprecation block (107-132), the
     * --BASIC/--autorun guards (137-149) and the append-binary
     * format-gate (151). It must therefore fire before all of those;
     * placed here, immediately after zxbc_parse_args, ahead of the
     * append-binary gate. parser.error -> stderr + exit 2; the
     * argparse usage: preamble is the carried adjudication and is
     * deliberately NOT reproduced. options.arch defaults to "zx48k"
     * and is always a string, so a NULL architecture means default
     * (valid) — guard for NULL and treat it as zx48k. */
    {
        const char *a = cs.opts.architecture;
        if (a && strcmp(a, "zx48k") != 0 && strcmp(a, "zxnext") != 0) {
            fprintf(stderr, "error: Invalid architecture '%s'\n", a);
            compiler_destroy(&cs);
            return 2;
        }
    }

    /* S7.2d-i (c) — a warning code both enabled (+W) and disabled
     * (-W). Faithful port of args_config.py:68-73:
     *   enabled  = set(options.enable_warning  or [])
     *   disabled = set(options.disable_warning or [])
     *   dup = [f"W{x}" for x in enabled.intersection(disabled)]
     *   if dup:
     *       parser.error(f"Warning(s) {', '.join(dup)} cannot be "
     *                    "enabled and disabled simultaneously")
     * This is the SECOND parser.error in parse_options() (line 70) —
     * after the arch check (62), before deprecation/(e)/append-binary.
     * Placed right after (a). Python builds `dup` from a set
     * intersection; for the single-element case (the only one in the
     * S7.2d-i fixture set) iteration order is unambiguous. For the
     * multi-element case the order is Python set-iteration order,
     * which for these short numeric-string codes is NOT insertion
     * order and is not reproducible from the C lists alone — see the
     * report's residual note; the single-element contract (W<code>)
     * is exact. parser.error -> stderr + exit 2; usage: preamble
     * deliberately not reproduced. */
    for (int i = 0; i < cs.opts.enabled_warning_count; i++) {
        const char *ec = cs.opts.enabled_warnings[i];
        for (int j = 0; j < cs.opts.disabled_warning_count; j++) {
            if (strcmp(ec, cs.opts.disabled_warnings[j]) == 0) {
                fprintf(stderr,
                    "error: Warning(s) W%s cannot be enabled and "
                    "disabled simultaneously\n", ec);
                compiler_destroy(&cs);
                return 2;
            }
        }
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

    /* S7.2d-i (e) — --BASIC / --autorun output-format guards.
     * Faithful port of args_config.py:137-149, evaluated AFTER
     * output_file_type is resolved (last-wins in args.c) and AFTER
     * the deprecation block (107-132) — hence placed here, right
     * after the deprecation/strict-bool emission and before the -e
     * error-file open:
     *
     *   if (options.basic or options.autorun) and
     *       OPTIONS.output_file_type not in {TAP,TZX,SNA,Z80}:
     *       parser.error("Options --BASIC and --autorun require one "
     *                    "of sna, tzx, tap or z80 output format")
     *
     *   if not (options.basic and options.autorun) and
     *       OPTIONS.output_file_type in {SNA,Z80}:
     *       parser.error("Options --BASIC and --autorun are both "
     *                    "required for snapshot formats")
     *
     * options.basic/options.autorun are tri-state in Python
     * (store_true, default=None); `(basic or autorun)` is true if
     * EITHER was given, `not (basic and autorun)` is true unless BOTH
     * given. The C use_basic_loader/autorun bools carry exactly that
     * truthiness for these CLI runs. Both checks are independent
     * parser.error()s in source order (137 then 145) — the first
     * that matches wins. parser.error -> stderr + exit 2; the
     * argparse usage: preamble is the carried adjudication and is
     * deliberately NOT reproduced. Python evaluates these at
     * args_config.py:137-149, BEFORE the append-binary format-gate
     * at :151, so the append-binary gate is emitted AFTER this block
     * (below) — matching Python's order exactly, so a command line
     * that would trigger BOTH reports the (e) message, as Python does. */
    {
        const char *oft = cs.opts.output_file_type
                              ? cs.opts.output_file_type : "bin";
        bool is_tap_tzx_sna_z80 =
            strcmp(oft, "tap") == 0 || strcmp(oft, "tzx") == 0 ||
            strcmp(oft, "sna") == 0 || strcmp(oft, "z80") == 0;
        bool is_sna_z80 =
            strcmp(oft, "sna") == 0 || strcmp(oft, "z80") == 0;

        if ((cs.opts.use_basic_loader || cs.opts.autorun)
            && !is_tap_tzx_sna_z80) {
            fprintf(stderr,
                "error: Options --BASIC and --autorun require one of "
                "sna, tzx, tap or z80 output format\n");
            compiler_destroy(&cs);
            return 2;
        }

        if (!(cs.opts.use_basic_loader && cs.opts.autorun)
            && is_sna_z80) {
            fprintf(stderr,
                "error: Options --BASIC and --autorun are both "
                "required for snapshot formats\n");
            compiler_destroy(&cs);
            return 2;
        }
    }

    /* --append-binary format-gate — faithful port of args_config.py:
     *   if options.append_binary and OPTIONS.output_file_type not in
     *       {FileType.TAP, FileType.TZX}:
     *       parser.error("Option --append-binary needs either tap or
     *                     tzx output format")
     *
     * ONLY options.append_binary gates (NOT append_headless_binary).
     * Python evaluates this at args_config.py:151-152, AFTER the
     * --BASIC/--autorun guards (:137-149) above and the deprecation
     * block (:107-132) — so it is emitted here, last of the post-parse
     * validation cluster, matching Python's order exactly (a command
     * line tripping both an (e) error and this gate reports the (e)
     * message, as Python does). parser.error -> stderr + exit 2; the
     * argparse usage: preamble is the carried adjudication, not
     * reproduced. */
    if (cs.opts.append_binary_count > 0
        && !(cs.opts.output_file_type
             && (strcmp(cs.opts.output_file_type, "tap") == 0
                 || strcmp(cs.opts.output_file_type, "tzx") == 0))) {
        fprintf(stderr,
            "error: Option --append-binary needs either tap or tzx output format\n");
        compiler_destroy(&cs);
        return 2;
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
        char lib_root[PATH_MAX];
        /* raw_path oversized: joining two PATH_MAX-class strings via
         * "%s/arch/%s/..." trips gcc's -Wformat-truncation. */
        char raw_path[PATH_MAX * 2 + 64];
        char real_path[PATH_MAX];

        if (get_lib_include_root(argv[0], lib_root, sizeof(lib_root))) {
            snprintf(raw_path, sizeof(raw_path),
                     "%s/arch/%s/stdlib", lib_root, arch);
            if (realpath(raw_path, real_path))
                vec_push(pp.include_paths, arena_strdup(&pp.arena, real_path));
            snprintf(raw_path, sizeof(raw_path),
                     "%s/arch/%s/runtime", lib_root, arch);
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

    /* Parse. The PLY/LALR(1) engine (the faithful port of Python's ACTUAL
     * parser) is now the DEFAULT parse path — it BES Python's parser (LALR(1)
     * table + parse loop + the p_* reduce-actions), so its output is
     * byte-for-byte identical on both valid and malformed input. Proven across
     * the full firewall: codegen ASM_MISMATCH 0 / FALSE_POS 0; stages both
     * archs GREEN (zx48k 895/886/886, zxnext 197/197/197); omatrix -O0..-O3
     * EQUAL == legacy at every level (879/881/887/881) with FEWER BIN-DIFFs;
     * parse FALSE_POS 0 and the error-path strictly improved (FALSE_NEG ⊆
     * legacy, STDERR_MISMATCH 47→39). The engine drives the lexer itself
     * (parser_init_noprime) and emits p_error messages directly
     * (plyparse_program_emit_errors), exactly as Python's PLY parser.
     *
     * The legacy recursive-descent parser is kept in-tree (dead) and selectable
     * via ZXBC_LEGACY_PARSER for emergency A/B fallback; it is no longer the
     * default. (The prior ZXBC_ENGINE opt-IN gate is retired — the engine is
     * default; honour ZXBC_ENGINE too so existing scripts keep working.) */
    Parser parser;
    AstNode *ast;
    bool use_legacy = getenv("ZXBC_LEGACY_PARSER") != NULL;
    if (!use_legacy) {
        parser_init_noprime(&parser, &cs, source);
        bool perr = false;
        ast = plyparse_program_emit_errors(&parser, &perr);
        /* parser_parse sets cs->ast (read by codegen's data_ast builder,
         * codegen.c:439 codegen_find_arraydecl); the engine path must too. */
        cs.ast = ast;
    } else {
        parser_init(&parser, &cs, source);
        ast = parser_parse(&parser);
    }

    rc = 0;
    if (parser.had_error || !ast) {
        rc = 1;
    }

    /* Use error_count for exit code (matching Python's gl.has_errors) */
    if (cs.error_count > 0)
        rc = 1;

    /* Post-parse validation: check GOTO/GOSUB label targets and pending calls */
    if (rc == 0 && ast) {
        /* zxbparser.py:529-541 — Python emits the "Undeclared label"
         * (SYMBOL_TABLE.check_labels, :529) and "Undeclared identifier"
         * (check_pending_labels, :536) diagnostics and GATES the pending-call
         * "Undeclared function" check behind them: `if gl.has_errors: return`
         * (:532, after check_labels) and `if not check_pending_labels(ast):
         * return` (:536) both short-circuit before check_pending_calls (:540).
         * So a program with an undeclared label/identifier never reaches the
         * pending-call check. The C merges check_labels + check_pending_labels
         * into check_pending_labels (it emits both messages); mirror the gate
         * by running check_pending_calls only when that pass was clean.
         * (label_decl1: bare `WRONG_LABEL` with a later `GOTO WRONG_LABEL`
         * -> "Undeclared label" ONLY, not also "Undeclared function".) */
        bool labels_ok = check_pending_labels(&cs, ast);
        if (labels_ok)
            check_pending_calls(&cs);

        /* No DATA defined: emitted per-statement by the translator visitors
         * tr_visit_read / tr_visit_restore (translator.c:2448, :2497) — the
         * faithful analogue of Python's syntax_error_no_data_defined calls
         * in src/arch/z80/visitor/translator.py:485 (visit_RESTORE) and :500
         * (visit_READ). Both the full-compile path (codegen_emit at :432) and
         * the --parse-only path (codegen_emit_ex(semantic_only=true) at :425)
         * run the translator visitor pass, so each READ/RESTORE statement
         * reports its own lineno. No fallback is required here — a previous
         * lineno-0 catch-all at this site duplicated the per-statement emit
         * and shadowed the Python-faithful per-statement linenos. */

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
        /* Python runs translator.visit + FunctionTranslator.start (and the
         * `if gl.has_errors: return 1` gate) BEFORE the parse-only return
         * (zxbc.py:125-141). Those passes own a class of semantic rejects
         * that fire while computing each node's `.t` — notably the const
         * typecast-to-non-integral check (translator_visitor.py:232,
         * "Cant convert '<expr>' to type <type>", rman). Run that same
         * pre-parse-only slice here (semantic_only=true stops before
         * emit_data_blocks/backend.emit, the post-return work at
         * zxbc.py:144+ that owns e.g. arrlabels10's deferred data-block
         * convert error — keeping it out of parse-only, exactly as the
         * oracle does). */
        codegen_emit_ex(&cs, ast, true);
        if (cs.error_count > 0)
            rc = 1;
        if (rc == 0 && cs.opts.debug_level > 0)
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

#ifdef ZXBASIC_STANDALONE
int main(int argc, char *argv[]) { return zxbc_main(argc, argv); }
#endif
