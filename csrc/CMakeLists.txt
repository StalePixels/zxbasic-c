cmake_minimum_required(VERSION 3.7.2)

# Read version from VERSION file
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" ZXBASIC_C_VERSION)
string(STRIP "${ZXBASIC_C_VERSION}" ZXBASIC_C_VERSION)

project(zxbasic-c VERSION 0.1.0 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warning flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
elseif(MSVC)
    add_compile_options(/W3)
    # Suppress MSVC warnings about fopen, sprintf, etc.
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
endif()

# Size optimisations for Release builds. NextPi deployment is UART-bound
# (~11.5 KB/s), so binary size dominates upload time.
if(CMAKE_BUILD_TYPE STREQUAL "Release" OR
   CMAKE_BUILD_TYPE STREQUAL "MinSizeRel" OR
   "${CMAKE_BUILD_TYPE}" STREQUAL "")
    if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
        # Emit each function/data symbol into its own section so the linker
        # can garbage-collect unreferenced ones at link time.
        add_compile_options(-ffunction-sections -fdata-sections)
        if(APPLE)
            # macOS ld: dead-strip + strip debug + strip non-global symbols.
            set(CMAKE_EXE_LINKER_FLAGS
                "${CMAKE_EXE_LINKER_FLAGS} -Wl,-dead_strip -Wl,-x")
        else()
            # GNU ld / lld: --gc-sections + strip-all.
            set(CMAKE_EXE_LINKER_FLAGS
                "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections -Wl,-s")
        endif()

        # LTO across translation units. Probe with CheckCCompilerFlag —
        # older toolchains (gcc 4.x on ancient distros) may lack it.
        include(CheckCCompilerFlag)
        check_c_compiler_flag(-flto HAVE_FLTO)
        if(HAVE_FLTO)
            add_compile_options(-flto)
            set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -flto")
        endif()
    endif()
endif()

# Flex and bison will be needed for later phases (assembler, compiler).
# The preprocessor uses a hand-written recursive-descent approach.
# find_package(FLEX REQUIRED)
# find_package(BISON REQUIRED)

# Common library (shared utilities)
add_subdirectory(common)

# Pass version to all targets
add_definitions(-DZXBASIC_C_VERSION="${ZXBASIC_C_VERSION}")

# Preprocessor (zxbpp)
add_subdirectory(zxbpp)

# Assembler (zxbasm)
add_subdirectory(zxbasm)

# Compiler (zxbc)
add_subdirectory(zxbc)

# AST dump tool — Phase 5 part B test infrastructure (read-only
# consumer of zxbc_parser_lib; not part of the production toolchain).
add_subdirectory(zxbc-ast-dump)

# Test harness
enable_testing()
add_subdirectory(tests)
