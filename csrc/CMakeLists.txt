cmake_minimum_required(VERSION 3.16)

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
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
endif()

# Flex and bison will be needed for later phases (assembler, compiler).
# The preprocessor uses a hand-written recursive-descent approach.
# find_package(FLEX REQUIRED)
# find_package(BISON REQUIRED)

# Common library (shared utilities)
add_subdirectory(common)

# Pass version to all targets
add_compile_definitions(ZXBASIC_C_VERSION="${ZXBASIC_C_VERSION}")

# Preprocessor (zxbpp)
add_subdirectory(zxbpp)

# Assembler (zxbasm)
add_subdirectory(zxbasm)

# Test harness
enable_testing()
add_subdirectory(tests)
