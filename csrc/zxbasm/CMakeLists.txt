# zxbasm — ZX BASIC Assembler (C port)
#
# Hand-written recursive-descent parser matching the Python PLY grammar.
# Links against zxbpp for preprocessing and common utilities.
#
# S6.2a: the assembler core is exposed as a static library `zxbasm_lib`
# so the zxbc compiler can link it for the .bin/container output path
# (the zxbc.py asmparse.assemble + generate_binary seam). The standalone
# `zxbasm` binary still links the same objects via the lib — its
# behaviour is unchanged. `preproc.c` is consumed via the shared
# `zxbpp_lib` (exactly as zxbc does) rather than compiled in directly,
# so the preprocessor translation unit is not duplicated when zxbc
# links zxbasm_lib. The zxbasm-side parser entry point is named
# `asm_parser_parse` to avoid the hard symbol collision with the zxbc
# parser's `parser_parse` (architecturally honest — the Python original
# keeps two separate PLY parsers, asmparse.parser vs zxbparser).

add_library(zxbasm_lib STATIC
    asm_core.c
    asm_instr.c
    expr.c
    lexer.c
    memory.c
    outfmt_tap.c
    parser.c
    z80_opcodes.c
)

target_include_directories(zxbasm_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/zxbpp
)

target_link_libraries(zxbasm_lib PUBLIC zxbasic_common zxbpp_lib)

# Standalone assembler binary — entry point only; links the lib.
add_executable(zxbasm
    main.c
)

target_link_libraries(zxbasm PRIVATE zxbasm_lib)
