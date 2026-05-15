# zxbc-ast-dump — Sprint 8 / Phase 5 part B.
#
# Read-only consumer of zxbc_parser_lib (exposed by csrc/zxbc/
# CMakeLists.txt). Parses a .bas, walks the AST, emits JSON to stdout
# in the schema documented by csrc/tests/dump_python_ast.py.

add_executable(zxbc-ast-dump
    main.c
)

target_include_directories(zxbc-ast-dump PRIVATE
    ${CMAKE_SOURCE_DIR}/zxbc
    ${CMAKE_SOURCE_DIR}/common
)

target_link_libraries(zxbc-ast-dump PRIVATE zxbc_parser_lib)
