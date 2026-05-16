# zxbc — ZX BASIC Compiler
#
# Sprint 8 (Phase 5 part B) refactor: the parser machinery is exposed
# as a static library `zxbc_parser_lib` so the new csrc/zxbc-ast-dump/
# binary can consume it without duplicating sources.
#
# Plan deviation, recorded in the Sprint 8 tracker: the plan listed
# {parser.c, lexer.c, ast.c, errmsg.c} as the lib's contents, with
# compiler.c + options.c staying in the binary. In practice
# parser_init needs CompilerState (set up by compiler_init in
# compiler.c) and CompilerOptions (initialised by options.c), so the
# lib also contains those two files. The production zxbc binary still
# links the same .o set as before — production behaviour is unchanged.

add_library(zxbc_parser_lib STATIC
    parser.c
    lexer.c
    ast.c
    errmsg.c
    compiler.c
    options.c
    visitor.c
    passes/functiongraph.c
    passes/unreachable.c
    passes/optimizer.c
    ic.c
)

target_include_directories(zxbc_parser_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
)

target_link_libraries(zxbc_parser_lib PUBLIC zxbpp_lib)

if(UNIX)
    target_link_libraries(zxbc_parser_lib PUBLIC m)
endif()

# Production zxbc binary — leftover sources after the lib extraction.
add_executable(zxbc
    main.c
    args.c
)

target_link_libraries(zxbc PRIVATE zxbc_parser_lib)
