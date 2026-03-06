# Test harness for zxbpp preprocessor tests
# Tests are run via a shell script that compares output against expected files

# Register the test harness script
add_test(
    NAME zxbpp_prepro_tests
    COMMAND ${CMAKE_CURRENT_SOURCE_DIR}/run_zxbpp_tests.sh
        $<TARGET_FILE:zxbpp>
        ${CMAKE_SOURCE_DIR}/../tests/functional/zxbpp
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/../tests/functional/zxbpp
)
