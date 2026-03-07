add_library(zxbasic_common STATIC
    arena.c
    strbuf.c
    hashmap.c
    ya_getopt.c
    cwalk.c
    utils.c
    config_file.c
)

target_include_directories(zxbasic_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
