# zxbasm — ZX BASIC Assembler (C port)
#
# Hand-written recursive-descent parser matching the Python PLY grammar.
# Links against zxbpp for preprocessing and common utilities.

add_executable(zxbasm
    main.c
    asm_core.c
    asm_instr.c
    expr.c
    lexer.c
    memory.c
    parser.c
    z80_opcodes.c
)

target_include_directories(zxbasm PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/zxbpp
)

target_link_libraries(zxbasm PRIVATE zxbasic_common)

# Link zxbpp as a library — we need the preprocessor functions.
# For now, compile zxbpp's preproc.c directly into zxbasm.
target_sources(zxbasm PRIVATE
    ${CMAKE_SOURCE_DIR}/zxbpp/preproc.c
)
