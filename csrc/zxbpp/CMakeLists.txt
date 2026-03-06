# zxbpp — ZX BASIC Preprocessor
#
# Using a hand-written recursive-descent preprocessor rather than
# flex/bison. The preprocessor is simple enough that a line-oriented
# approach is more natural and easier to match the Python original's
# exact output format. Flex/bison will be used for the assembler and
# compiler parser in later phases.

add_executable(zxbpp
    main.c
    preproc.c
)

target_include_directories(zxbpp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(zxbpp PRIVATE zxbasic_common)
