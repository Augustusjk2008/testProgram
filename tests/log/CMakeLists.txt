add_executable(hwtest_log_tests
    log_service_test.cpp
    log_file_sink_test.cpp
    hal_log_bridge_test.cpp
)

target_include_directories(hwtest_log_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src/logging/include
        ${PROJECT_SOURCE_DIR}/src/hal/include
)

target_link_libraries(hwtest_log_tests
    PRIVATE
        hwtest_log
        hwtest_hal
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(hwtest_log_tests)
