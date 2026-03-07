# zxbpp — ZX BASIC Preprocessor
#
# Using a hand-written recursive-descent preprocessor rather than
# flex/bison. The preprocessor is simple enough that a line-oriented
# approach is more natural and easier to match the Python original's
# exact output format.

# Preprocessor core as a static library (shared by zxbpp binary and zxbc)
add_library(zxbpp_lib STATIC
    preproc.c
)

target_include_directories(zxbpp_lib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(zxbpp_lib PUBLIC zxbasic_common)

# Standalone preprocessor binary
add_executable(zxbpp
    main.c
)

target_link_libraries(zxbpp PRIVATE zxbpp_lib)
