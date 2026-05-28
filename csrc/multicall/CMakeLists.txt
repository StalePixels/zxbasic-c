# Multicall ("zxbasic-suite") — single binary, applet selected by argv[0].
#
# Three symlinks at install time (zxbpp / zxbasm / zxbc) each point to this
# binary; the dispatcher in main.c looks at argv[0]'s basename and calls the
# matching per-tool entry point. One shared text/rodata segment instead of
# three nearly-identical executables.

add_executable(zxbasic-suite
    main.c
    ${CMAKE_SOURCE_DIR}/zxbpp/main.c
    ${CMAKE_SOURCE_DIR}/zxbasm/main.c
    ${CMAKE_SOURCE_DIR}/zxbc/main.c
    ${CMAKE_SOURCE_DIR}/zxbc/args.c
)

target_include_directories(zxbasic-suite PRIVATE
    ${CMAKE_SOURCE_DIR}/zxbpp
    ${CMAKE_SOURCE_DIR}/zxbasm
    ${CMAKE_SOURCE_DIR}/zxbc
)

target_link_libraries(zxbasic-suite PRIVATE
    zxbpp_lib
    zxbasm_lib
    zxbc_parser_lib
    zxbasic_common
)

if(UNIX)
    target_link_libraries(zxbasic-suite PRIVATE m)
endif()
