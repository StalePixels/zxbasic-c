# Multicall ("zxbasic-suite") — single binary, applet selected by argv[0].
#
# Three symlinks (zxbpp / zxbasm / zxbc) each point to this binary; the
# dispatcher in main.c looks at argv[0]'s basename and calls the matching
# per-tool entry point. One shared text/rodata segment instead of three
# nearly-identical executables.

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

# Post-build: create the three applet symlinks next to the suite binary,
# so the build tree alone is a runnable layout (tests don't need install).
# On Windows there is no portable equivalent — copy the .exe under each
# applet name so the same name-dispatch path works.
#
# Skipped when ZXBASIC_BUILD_STANDALONE=ON, because the standalone build
# already produces bin/zxbpp etc. as real executables under those names.
set(ZXBASIC_APPLETS zxbpp zxbasm zxbc)

if(NOT ZXBASIC_BUILD_STANDALONE)
    if(WIN32)
        foreach(applet ${ZXBASIC_APPLETS})
            add_custom_command(TARGET zxbasic-suite POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy
                        $<TARGET_FILE:zxbasic-suite>
                        $<TARGET_FILE_DIR:zxbasic-suite>/${applet}.exe
                VERBATIM)
        endforeach()
    else()
        foreach(applet ${ZXBASIC_APPLETS})
            add_custom_command(TARGET zxbasic-suite POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E remove -f
                        $<TARGET_FILE_DIR:zxbasic-suite>/${applet}
                COMMAND ${CMAKE_COMMAND} -E create_symlink
                        zxbasic-suite
                        $<TARGET_FILE_DIR:zxbasic-suite>/${applet}
                VERBATIM)
        endforeach()
    endif()
endif()

# Install rules — same layout as the build tree: one real binary plus
# three applet entries pointing at it.
include(GNUInstallDirs)
install(TARGETS zxbasic-suite
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

if(WIN32)
    foreach(applet ${ZXBASIC_APPLETS})
        install(FILES $<TARGET_FILE_DIR:zxbasic-suite>/${applet}.exe
                DESTINATION ${CMAKE_INSTALL_BINDIR})
    endforeach()
else()
    foreach(applet ${ZXBASIC_APPLETS})
        install(CODE "
            execute_process(COMMAND \"\${CMAKE_COMMAND}\" -E create_symlink
                zxbasic-suite
                \"\$ENV{DESTDIR}\${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_BINDIR}/${applet}\")
        ")
    endforeach()
endif()
