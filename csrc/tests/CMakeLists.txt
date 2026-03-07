# Test harness for all test suites

# ---- Functional tests (existing) ----

# zxbpp preprocessor tests
add_test(
    NAME zxbpp_prepro_tests
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_zxbpp_tests.sh
        $<TARGET_FILE:zxbpp>
        ${CMAKE_SOURCE_DIR}/../tests/functional/zxbpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/../tests/functional/zxbpp
)

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
)
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
)
add_test(NAME symboltable_tests COMMAND test_symboltable)

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

# ---- Command-line tests (shell scripts) ----

# cmdline tests — matches tests/cmdline/test_zxb.py
add_test(
    NAME cmdline_tests
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_cmdline_tests.sh
        $<TARGET_FILE:zxbc>
        ${CMAKE_SOURCE_DIR}/../tests/cmdline
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..
)
