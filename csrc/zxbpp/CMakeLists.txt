# Preprocessor lexer (flex)
FLEX_TARGET(zxbpp_lexer lexer.l ${CMAKE_CURRENT_BINARY_DIR}/lexer.c
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/lexer.h)

# Preprocessor parser (bison)
BISON_TARGET(zxbpp_parser parser.y ${CMAKE_CURRENT_BINARY_DIR}/parser.c
    DEFINES_FILE ${CMAKE_CURRENT_BINARY_DIR}/parser.h
    COMPILE_FLAGS "-v")

ADD_FLEX_BISON_DEPENDENCY(zxbpp_lexer zxbpp_parser)

add_executable(zxbpp
    main.c
    ${FLEX_zxbpp_lexer_OUTPUTS}
    ${BISON_zxbpp_parser_OUTPUTS}
)

target_include_directories(zxbpp PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(zxbpp PRIVATE zxbasic_common)
