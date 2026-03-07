/*
 * test_cmdline.c — Command-line option parsing tests
 *
 * Matches:
 *   tests/cmdline/test_zxb.py — value verification (org, optimization_level)
 *   tests/api/test_arg_parser.py — default None semantics (autorun, basic)
 */
#include "test_harness.h"
#include "args.h"
#include "options.h"

#include <string.h>

/* Helper: parse args into a fresh CompilerOptions, return rc */
static int parse(CompilerOptions *opts, int argc, char **argv) {
    compiler_options_init(opts);
    return zxbc_parse_args(argc, argv, opts);
}

/* ---- tests/cmdline/test_zxb.py ---- */

static void test_org_allows_0xnnnn_format(void) {
    CompilerOptions opts;
    char *argv[] = {"zxbc", "--parse-only", "--org", "0xC000", "test.bas"};
    int rc = parse(&opts, 5, argv);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(opts.org, 0xC000);
}

static void test_org_loads_ok_from_config_file(void) {
    CompilerOptions opts;
    char *argv[] = {"zxbc", "--parse-only", "-F",
                    "tests/cmdline/config_sample.ini", "test.bas"};
    int rc = parse(&opts, 5, argv);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(opts.org, 31234);
}

static void test_cmdline_should_override_config_file(void) {
    CompilerOptions opts;
    char *argv[] = {"zxbc", "--parse-only", "-F",
                    "tests/cmdline/config_sample.ini",
                    "--org", "1234", "test.bas"};
    int rc = parse(&opts, 7, argv);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(opts.org, 1234);
    ASSERT_EQ_INT(opts.optimization_level, 3);  /* from config file */
}

static void test_org_cmdline_overrides_config(void) {
    /* When --org is on cmdline AND in config, cmdline wins */
    CompilerOptions opts;
    char *argv[] = {"zxbc", "--parse-only", "--org", "0x8000",
                    "-F", "tests/cmdline/config_sample.ini", "test.bas"};
    int rc = parse(&opts, 7, argv);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(opts.org, 0x8000);  /* cmdline, not config's 31234 */
}

/* ---- tests/api/test_arg_parser.py ---- */

static void test_autorun_defaults_to_none(void) {
    /* Python: options.autorun is None when not specified.
     * C: autorun=false AND OPT_SET_AUTORUN not in cmdline_set */
    CompilerOptions opts;
    char *argv[] = {"zxbc", "--parse-only", "test.bas"};
    int rc = parse(&opts, 3, argv);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_FALSE(opts.autorun);
    ASSERT_EQ_INT(opts.cmdline_set & OPT_SET_AUTORUN, 0);
}

static void test_loader_defaults_to_none(void) {
    /* Python: options.basic is None when not specified */
    CompilerOptions opts;
    char *argv[] = {"zxbc", "--parse-only", "test.bas"};
    int rc = parse(&opts, 3, argv);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_FALSE(opts.use_basic_loader);
    ASSERT_EQ_INT(opts.cmdline_set & OPT_SET_BASIC, 0);
}

static void test_autorun_set_when_specified(void) {
    CompilerOptions opts;
    char *argv[] = {"zxbc", "--parse-only", "--autorun", "test.bas"};
    int rc = parse(&opts, 4, argv);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_TRUE(opts.autorun);
    ASSERT_TRUE(opts.cmdline_set & OPT_SET_AUTORUN);
}

static void test_basic_set_when_specified(void) {
    CompilerOptions opts;
    char *argv[] = {"zxbc", "--parse-only", "--BASIC", "test.bas"};
    int rc = parse(&opts, 4, argv);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_TRUE(opts.use_basic_loader);
    ASSERT_TRUE(opts.cmdline_set & OPT_SET_BASIC);
}

/* ---- Additional value verification ---- */

static void test_optimization_level(void) {
    CompilerOptions opts;
    char *argv[] = {"zxbc", "--parse-only", "-O", "2", "test.bas"};
    int rc = parse(&opts, 5, argv);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(opts.optimization_level, 2);
}

static void test_output_format_tap(void) {
    CompilerOptions opts;
    char *argv[] = {"zxbc", "--parse-only", "--tap", "test.bas"};
    int rc = parse(&opts, 4, argv);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_STR_EQ(opts.output_file_type, "tap");
}

static void test_output_format_tzx(void) {
    CompilerOptions opts;
    char *argv[] = {"zxbc", "--parse-only", "--tzx", "test.bas"};
    int rc = parse(&opts, 4, argv);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_STR_EQ(opts.output_file_type, "tzx");
}

static void test_sinclair_flag(void) {
    CompilerOptions opts;
    char *argv[] = {"zxbc", "--parse-only", "--sinclair", "test.bas"};
    int rc = parse(&opts, 4, argv);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_TRUE(opts.sinclair);
    ASSERT_TRUE(opts.cmdline_set & OPT_SET_SINCLAIR);
}

static void test_heap_size(void) {
    CompilerOptions opts;
    char *argv[] = {"zxbc", "--parse-only", "--heap-size", "8192", "test.bas"};
    int rc = parse(&opts, 5, argv);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(opts.heap_size, 8192);
}

static void test_no_input_file_errors(void) {
    CompilerOptions opts;
    char *argv[] = {"zxbc", "--parse-only"};
    int rc = parse(&opts, 2, argv);
    ASSERT_EQ_INT(rc, 1);  /* error: no input file */
}

static void test_config_no_override_when_cmdline_set(void) {
    /* Config has optimization_level=3. If -O2 is on cmdline, -O2 wins */
    CompilerOptions opts;
    char *argv[] = {"zxbc", "--parse-only", "-O", "2",
                    "-F", "tests/cmdline/config_sample.ini", "test.bas"};
    int rc = parse(&opts, 7, argv);
    ASSERT_EQ_INT(rc, 0);
    ASSERT_EQ_INT(opts.optimization_level, 2);  /* cmdline, not config's 3 */
}

int main(void) {
    RUN_TEST(test_org_allows_0xnnnn_format);
    RUN_TEST(test_org_loads_ok_from_config_file);
    RUN_TEST(test_cmdline_should_override_config_file);
    RUN_TEST(test_org_cmdline_overrides_config);
    RUN_TEST(test_autorun_defaults_to_none);
    RUN_TEST(test_loader_defaults_to_none);
    RUN_TEST(test_autorun_set_when_specified);
    RUN_TEST(test_basic_set_when_specified);
    RUN_TEST(test_optimization_level);
    RUN_TEST(test_output_format_tap);
    RUN_TEST(test_output_format_tzx);
    RUN_TEST(test_sinclair_flag);
    RUN_TEST(test_heap_size);
    RUN_TEST(test_no_input_file_errors);
    RUN_TEST(test_config_no_override_when_cmdline_set);
    REPORT();
}
