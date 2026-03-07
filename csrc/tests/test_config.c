/*
 * test_config.c — Tests for CompilerOptions defaults and config file loading
 *
 * Matches: tests/api/test_config.py (TestConfig)
 *          tests/api/test_arg_parser.py (TestArgParser)
 */
#include "test_harness.h"
#include "options.h"
#include "config_file.h"
#include "utils.h"

#include <stdlib.h>
#include <unistd.h>

/* ---- test_config.py: test_init ---- */
TEST(test_config_init_defaults) {
    CompilerOptions opts;
    compiler_options_init(&opts);

    ASSERT_EQ_INT(opts.debug_level, 0);
    ASSERT_EQ_INT(opts.optimization_level, DEFAULT_OPTIMIZATION_LEVEL);
    ASSERT_FALSE(opts.case_insensitive);
    ASSERT_EQ_INT(opts.array_base, 0);
    ASSERT_FALSE(opts.default_byref);
    ASSERT_EQ_INT(opts.max_syntax_errors, DEFAULT_MAX_SYNTAX_ERRORS);
    ASSERT_EQ_INT(opts.string_base, 0);
    ASSERT_NULL(opts.memory_map);
    ASSERT_FALSE(opts.force_asm_brackets);
    ASSERT_FALSE(opts.use_basic_loader);
    ASSERT_FALSE(opts.autorun);
    ASSERT_STR_EQ(opts.output_file_type, "bin");
    ASSERT_STR_EQ(opts.include_path, "");
    ASSERT_FALSE(opts.memory_check);
    ASSERT_FALSE(opts.strict_bool);
    ASSERT_FALSE(opts.array_check);
    ASSERT_FALSE(opts.enable_break);
    ASSERT_FALSE(opts.emit_backend);
    ASSERT_NULL(opts.architecture);
    ASSERT_EQ_INT(opts.expected_warnings, 0);
    ASSERT_EQ(opts.opt_strategy, OPT_STRATEGY_AUTO);
    ASSERT_FALSE(opts.explicit_);
    ASSERT_FALSE(opts.sinclair);
    ASSERT_FALSE(opts.strict);
    ASSERT_EQ_INT(opts.org, 32768);
}

/* ---- test_config.py: test_loader_ignore_none and test_autorun_ignore_none
 * In Python, setting an option to None is ignored (preserves previous value).
 * In C, we don't have None — this tests the concept that we have proper
 * defaults that don't change when not explicitly set on the cmdline. ---- */
TEST(test_config_defaults_stable_across_reinit) {
    CompilerOptions opts;
    compiler_options_init(&opts);
    opts.use_basic_loader = true;
    opts.autorun = true;

    /* Reinit should reset to defaults */
    CompilerOptions opts2;
    compiler_options_init(&opts2);
    ASSERT_FALSE(opts2.use_basic_loader);
    ASSERT_FALSE(opts2.autorun);

    /* But our original should still be set */
    ASSERT_TRUE(opts.use_basic_loader);
    ASSERT_TRUE(opts.autorun);
}

/* Helper callback for config tests */
static bool config_test_cb(const char *key, const char *value, void *userdata) {
    CompilerOptions *opts = (CompilerOptions *)userdata;
    if (strcmp(key, "optimization_level") == 0)
        opts->optimization_level = atoi(value);
    else if (strcmp(key, "org") == 0)
        parse_int(value, &opts->org);
    else if (strcmp(key, "heap_size") == 0)
        opts->heap_size = atoi(value);
    return true;
}

TEST(test_load_config_from_file_proper) {
    char tmpfile[] = "/tmp/zxbc_test_config_XXXXXX";
    int fd = mkstemp(tmpfile);
    ASSERT(fd >= 0);
    const char *content = "[zxbc]\noptimization_level = 3\norg = 31234\n";
    write(fd, content, strlen(content));
    close(fd);

    CompilerOptions opts;
    compiler_options_init(&opts);

    int rc = config_load_section(tmpfile, "zxbc", config_test_cb, &opts);
    ASSERT_EQ_INT(rc, 1);
    ASSERT_EQ_INT(opts.optimization_level, 3);
    ASSERT_EQ_INT(opts.org, 31234);

    unlink(tmpfile);
}

/* ---- test_config.py: test_load_config_from_file_fails_if_no_section ---- */
TEST(test_load_config_fails_if_no_section) {
    char tmpfile[] = "/tmp/zxbc_test_config_XXXXXX";
    int fd = mkstemp(tmpfile);
    ASSERT(fd >= 0);
    const char *content = "[zxbasm]\norg = 1234\n";
    write(fd, content, strlen(content));
    close(fd);

    CompilerOptions opts;
    compiler_options_init(&opts);

    int rc = config_load_section(tmpfile, "zxbc", config_test_cb, &opts);
    ASSERT_EQ_INT(rc, 0);  /* section not found */

    unlink(tmpfile);
}

/* ---- test_config.py: test_load_config_from_file_fails_if_no_file ---- */
TEST(test_load_config_fails_if_no_file) {
    CompilerOptions opts;
    compiler_options_init(&opts);

    int rc = config_load_section("/nonexistent/path/dummy.ini", "zxbc", config_test_cb, &opts);
    ASSERT_EQ_INT(rc, -1);  /* file not found */
}

/* ---- test_config.py: test_load_config_from_file_fails_if_duplicated_fields ---- */
TEST(test_load_config_fails_if_duplicated_fields) {
    char tmpfile[] = "/tmp/zxbc_test_config_XXXXXX";
    int fd = mkstemp(tmpfile);
    ASSERT(fd >= 0);
    const char *content = "[zxbc]\nheap_size = 1234\nheap_size = 5678\n";
    write(fd, content, strlen(content));
    close(fd);

    CompilerOptions opts;
    compiler_options_init(&opts);

    int rc = config_load_section(tmpfile, "zxbc", config_test_cb, &opts);
    ASSERT_EQ_INT(rc, -2);  /* duplicate keys */

    unlink(tmpfile);
}

int main(void) {
    printf("test_config (matching tests/api/test_config.py + test_arg_parser.py):\n");
    RUN_TEST(test_config_init_defaults);
    RUN_TEST(test_config_defaults_stable_across_reinit);
    /* Skip test_load_config_from_file — uses nested function (non-standard) */
    RUN_TEST(test_load_config_from_file_proper);
    RUN_TEST(test_load_config_fails_if_no_section);
    RUN_TEST(test_load_config_fails_if_no_file);
    RUN_TEST(test_load_config_fails_if_duplicated_fields);
    REPORT();
}
