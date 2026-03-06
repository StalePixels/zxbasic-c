cmake_minimum_required(VERSION 3.16)
project(zxbasic-c C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Warning flags
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# Flex and bison will be needed for later phases (assembler, compiler).
# The preprocessor uses a hand-written recursive-descent approach.
# find_package(FLEX REQUIRED)
# find_package(BISON REQUIRED)

# Common library (shared utilities)
add_subdirectory(common)

# Preprocessor (zxbpp)
add_subdirectory(zxbpp)

# Test harness
enable_testing()
add_subdirectory(tests)
