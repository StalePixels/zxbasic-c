# CMake generated Testfile for 
# Source directory: /Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests
# Build directory: /Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/build-gcc/tests
# 
# This file includes the relevant testing commands required for 
# testing this directory and lists subdirectories to be tested as well.
add_test(zxbpp_prepro_tests "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/run_zxbpp_tests.sh" "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/build-gcc/zxbpp/zxbpp" "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/../tests/functional/zxbpp")
set_tests_properties(zxbpp_prepro_tests PROPERTIES  WORKING_DIRECTORY "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/../tests/functional/zxbpp" _BACKTRACE_TRIPLES "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;6;add_test;/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;0;")
add_test(api_utils_tests "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/build-gcc/tests/test_utils")
set_tests_properties(api_utils_tests PROPERTIES  _BACKTRACE_TRIPLES "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;19;add_test;/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;0;")
add_test(api_config_tests "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/build-gcc/tests/test_config")
set_tests_properties(api_config_tests PROPERTIES  _BACKTRACE_TRIPLES "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;27;add_test;/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;0;")
add_test(types_tests "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/build-gcc/tests/test_types")
set_tests_properties(types_tests PROPERTIES  _BACKTRACE_TRIPLES "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;33;add_test;/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;0;")
add_test(ast_tests "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/build-gcc/tests/test_ast")
set_tests_properties(ast_tests PROPERTIES  _BACKTRACE_TRIPLES "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;49;add_test;/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;0;")
add_test(symboltable_tests "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/build-gcc/tests/test_symboltable")
set_tests_properties(symboltable_tests PROPERTIES  _BACKTRACE_TRIPLES "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;65;add_test;/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;0;")
add_test(check_tests "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/build-gcc/tests/test_check")
set_tests_properties(check_tests PROPERTIES  _BACKTRACE_TRIPLES "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;81;add_test;/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;0;")
add_test(cmdline_value_tests "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/build-gcc/tests/test_cmdline")
set_tests_properties(cmdline_value_tests PROPERTIES  WORKING_DIRECTORY "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/.." _BACKTRACE_TRIPLES "/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;92;add_test;/Volumes/McFiver/u/GIT/ZX/NextPi/zxbasic-c/csrc/tests/CMakeLists.txt;0;")
