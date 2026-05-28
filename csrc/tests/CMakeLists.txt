# Test harness for all test suites

# ---- Functional tests (existing) ----

# zxbpp preprocessor tests — invoked via the bin/ zxbpp entry, which is
# the multicall zxbasic-suite binary under the applet name (symlink on
# Unix, copy on Windows). The harness is name-agnostic; it just takes a
# binary path as argv[1]. Bash-only harness, so skipped on Windows
# (the #line directive path differences were the original skip reason
# too — see the c-build.yml comment).
if(NOT WIN32)
    add_test(
        NAME zxbpp_prepro_tests
        COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_zxbpp_tests.sh
            $<TARGET_FILE_DIR:zxbasic-suite>/zxbpp
            ${CMAKE_SOURCE_DIR}/../tests/functional/zxbpp
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/../tests/functional/zxbpp
    )
endif()

# ---- Unit tests (C programs) ----

# test_utils — parse_int and utility functions (matches tests/api/test_utils.py)
add_executable(test_utils test_utils.c)
target_link_libraries(test_utils zxbasic_common)
add_test(NAME api_utils_tests COMMAND test_utils)

# test_config — CompilerOptions defaults and config loading (matches tests/api/test_config.py)
add_executable(test_config test_config.c)
target_include_directories(test_config PRIVATE ${CMAKE_SOURCE_DIR}/zxbc)
target_link_libraries(test_config zxbasic_common)
# test_config needs options.c from zxbc
target_sources(test_config PRIVATE ${CMAKE_SOURCE_DIR}/zxbc/options.c)
add_test(NAME api_config_tests COMMAND test_config)

# test_types — Type system predicates (matches tests/symbols/test_symbolBASICTYPE.py)
add_executable(test_types test_types.c)
target_include_directories(test_types PRIVATE ${CMAKE_SOURCE_DIR}/zxbc)
target_link_libraries(test_types zxbasic_common)
add_test(NAME types_tests COMMAND test_types)

# test_ast — AST node creation and type operations (matches tests/symbols/)
add_executable(test_ast test_ast.c)
target_include_directories(test_ast PRIVATE ${CMAKE_SOURCE_DIR}/zxbc)
target_link_libraries(test_ast zxbasic_common)
target_sources(test_ast PRIVATE
    ${CMAKE_SOURCE_DIR}/zxbc/ast.c
    ${CMAKE_SOURCE_DIR}/zxbc/compiler.c
    ${CMAKE_SOURCE_DIR}/zxbc/options.c
    ${CMAKE_SOURCE_DIR}/zxbc/errmsg.c
    ${CMAKE_SOURCE_DIR}/zxbc/pyfloat.c
)
if(UNIX)
    target_link_libraries(test_ast m)
endif()
add_test(NAME ast_tests COMMAND test_ast)

# test_symboltable — Symbol table operations (matches tests/api/test_symbolTable.py)
add_executable(test_symboltable test_symboltable.c)
target_include_directories(test_symboltable PRIVATE ${CMAKE_SOURCE_DIR}/zxbc)
target_link_libraries(test_symboltable zxbasic_common)
target_sources(test_symboltable PRIVATE
    ${CMAKE_SOURCE_DIR}/zxbc/ast.c
    ${CMAKE_SOURCE_DIR}/zxbc/compiler.c
    ${CMAKE_SOURCE_DIR}/zxbc/options.c
    ${CMAKE_SOURCE_DIR}/zxbc/errmsg.c
    ${CMAKE_SOURCE_DIR}/zxbc/pyfloat.c
)
if(UNIX)
    target_link_libraries(test_symboltable m)
endif()
add_test(NAME symboltable_tests COMMAND test_symboltable)

# test_check — api/check functions (matches tests/api/test_check.py)
add_executable(test_check test_check.c)
target_include_directories(test_check PRIVATE ${CMAKE_SOURCE_DIR}/zxbc)
target_link_libraries(test_check zxbasic_common)
target_sources(test_check PRIVATE
    ${CMAKE_SOURCE_DIR}/zxbc/ast.c
    ${CMAKE_SOURCE_DIR}/zxbc/compiler.c
    ${CMAKE_SOURCE_DIR}/zxbc/options.c
    ${CMAKE_SOURCE_DIR}/zxbc/errmsg.c
    ${CMAKE_SOURCE_DIR}/zxbc/pyfloat.c
)
if(UNIX)
    target_link_libraries(test_check m)
endif()
add_test(NAME check_tests COMMAND test_check)

# test_cmdline — value-verifying option parsing tests
# (matches tests/cmdline/test_zxb.py + tests/api/test_arg_parser.py)
add_executable(test_cmdline test_cmdline.c)
target_include_directories(test_cmdline PRIVATE ${CMAKE_SOURCE_DIR}/zxbc)
target_link_libraries(test_cmdline zxbasic_common)
target_sources(test_cmdline PRIVATE
    ${CMAKE_SOURCE_DIR}/zxbc/args.c
    ${CMAKE_SOURCE_DIR}/zxbc/options.c
)
add_test(
    NAME cmdline_value_tests
    COMMAND test_cmdline
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..
)

# NOTE: Round 0 (Sprint 11) deleted csrc/tests/run_cmdline_tests.sh
# along with the other fuzzy harnesses — the 4 shell-driven cmdline
# tests it ran were syntax-only flag-acceptance checks (the fuzzy
# harness audit at docs/captures/zxbasic-c/test-infra-trust.md flagged
# them as "test_compile_only passes if zxbc is replaced with a do-
# nothing script"). The C test_cmdline binary's 15 value-verifying
# tests (registered above as cmdline_value_tests) are the real
# replacement.
