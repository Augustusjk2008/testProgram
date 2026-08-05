if(NOT TARGET hwtest_algorithm_mbddf)
    message(FATAL_ERROR "hwtest_algorithm_mbddf target must be defined before tests/algorithm is added")
endif()

if(NOT TARGET GTest::gtest_main)
    message(FATAL_ERROR "GTest::gtest_main must be available before tests/algorithm is added")
endif()

if(TARGET Qt5::Core)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Network)
    set(HWTEST_ALGORITHM_QT_NETWORK_TARGET Qt5::Network)
    set(HWTEST_ALGORITHM_QT_CORE_TARGET Qt5::Core)
elseif(TARGET Qt6::Core)
    find_package(Qt6 REQUIRED COMPONENTS Network)
    set(HWTEST_ALGORITHM_QT_NETWORK_TARGET Qt6::Network)
    set(HWTEST_ALGORITHM_QT_CORE_TARGET Qt6::Core)
else()
    message(FATAL_ERROR "A Qt Core target must be available before tests/algorithm is added")
endif()

add_executable(hwtest_algorithm_tests
    board_test_executor_test.cpp
    di_stimulus_controller_test.cpp
    dh_ignite_stream_executor_test.cpp
    imu_stream_executor_test.cpp
    helm_stream_executor_test.cpp
    post_run_analysis_contract_test.cpp
    helm_waveform_analyzers_test.cpp
    mbddf_protocol_test.cpp
    run_parameter_schema_test.cpp
    serial_test_executor_test.cpp
    system_status_executor_test.cpp
)

# Compiles the public header without the algorithm library's BIZ/HAL linkage,
# proving this boundary remains Qt-Core-only.
add_library(hwtest_post_run_analysis_public_api_check OBJECT
    post_run_analysis_public_api_check.cpp
)
target_compile_features(hwtest_post_run_analysis_public_api_check PRIVATE cxx_std_17)
target_include_directories(hwtest_post_run_analysis_public_api_check
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/algorithm/include
)
target_link_libraries(hwtest_post_run_analysis_public_api_check
    PRIVATE
        ${HWTEST_ALGORITHM_QT_CORE_TARGET}
)

file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mbddf_system_status.testcfg.json"
     HWTEST_MBDDF_SYSTEM_STATUS_CONFIG)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/configs/mbddf_elec_health.testcfg.json"
     HWTEST_MBDDF_ELEC_HEALTH_CONFIG)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/configs/mbddf_timer_jitter.testcfg.json"
     HWTEST_MBDDF_TIMER_JITTER_CONFIG)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/configs/mbddf_di.testcfg.json"
     HWTEST_MBDDF_DI_CONFIG)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/configs/mbddf_serial_test.testcfg.json"
     HWTEST_MBDDF_SERIAL_TEST_CONFIG)
file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/helm_performance"
     HWTEST_HELM_PERFORMANCE_FIXTURE_DIR)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/dut/docs/design/product_protocol_csv"
     HWTEST_MBDDF_PROTOCOL_CATALOG_DIR)

target_compile_features(hwtest_algorithm_tests PRIVATE cxx_std_17)

target_compile_definitions(hwtest_algorithm_tests
    PRIVATE
        HWTEST_MBDDF_SYSTEM_STATUS_CONFIG="${HWTEST_MBDDF_SYSTEM_STATUS_CONFIG}"
        HWTEST_MBDDF_ELEC_HEALTH_CONFIG="${HWTEST_MBDDF_ELEC_HEALTH_CONFIG}"
        HWTEST_MBDDF_TIMER_JITTER_CONFIG="${HWTEST_MBDDF_TIMER_JITTER_CONFIG}"
        HWTEST_MBDDF_DI_CONFIG="${HWTEST_MBDDF_DI_CONFIG}"
        HWTEST_MBDDF_SERIAL_TEST_CONFIG="${HWTEST_MBDDF_SERIAL_TEST_CONFIG}"
        HWTEST_HELM_PERFORMANCE_FIXTURE_DIR="${HWTEST_HELM_PERFORMANCE_FIXTURE_DIR}"
        HWTEST_MBDDF_PROTOCOL_CATALOG_DIR="${HWTEST_MBDDF_PROTOCOL_CATALOG_DIR}"
)

target_link_libraries(hwtest_algorithm_tests
    PRIVATE
        hwtest_algorithm_mbddf
        hwtest_biz
        hwtest_hal
        ${HWTEST_ALGORITHM_QT_NETWORK_TARGET}
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(hwtest_algorithm_tests)
