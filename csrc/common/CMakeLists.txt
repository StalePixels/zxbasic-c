add_library(zxbasic_common STATIC
    arena.c
    strbuf.c
    hashmap.c
)

target_include_directories(zxbasic_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
