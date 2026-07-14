if(NOT TARGET hwtest_biz)
    message(FATAL_ERROR "hwtest_biz target must be defined before tests/biz is added")
endif()

if(NOT TARGET GTest::gtest_main)
    message(FATAL_ERROR "GTest::gtest_main must be available before tests/biz is added")
endif()

add_executable(hwtest_biz_tests
    algorithm_executor_contract_test.cpp
    architecture_test.cpp
    report_generator_test.cpp
    test_config_manager_test.cpp
    test_plan_builder_test.cpp
    test_run_service_test.cpp
)

file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/src/biz" HWTEST_BIZ_SOURCE_DIR)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}" HWTEST_BIZ_TEST_SOURCE_DIR)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/tmp/hwtest_BIZ/configs/sample_product.testcfg"
     HWTEST_BIZ_ATTACHMENT_SAMPLE_PATH)

target_compile_features(hwtest_biz_tests PRIVATE cxx_std_17)

target_compile_definitions(hwtest_biz_tests
    PRIVATE
        HWTEST_BIZ_SOURCE_DIR="${HWTEST_BIZ_SOURCE_DIR}"
        HWTEST_BIZ_TEST_SOURCE_DIR="${HWTEST_BIZ_TEST_SOURCE_DIR}"
        HWTEST_BIZ_ATTACHMENT_SAMPLE_PATH="${HWTEST_BIZ_ATTACHMENT_SAMPLE_PATH}"
)

target_include_directories(hwtest_biz_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(hwtest_biz_tests
    PRIVATE
        hwtest_biz
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(hwtest_biz_tests)
