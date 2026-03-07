# zxbc — ZX BASIC Compiler

add_executable(zxbc
    main.c
    args.c
    compiler.c
    ast.c
    errmsg.c
    options.c
    lexer.c
    parser.c
)

target_include_directories(zxbc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/common
)

target_link_libraries(zxbc PRIVATE zxbpp_lib)

# Link math library on Unix
if(UNIX)
    target_link_libraries(zxbc PRIVATE m)
endif()
