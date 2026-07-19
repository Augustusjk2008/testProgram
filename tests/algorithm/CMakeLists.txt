if(NOT TARGET hwtest_algorithm_mbddf)
    message(FATAL_ERROR "hwtest_algorithm_mbddf target must be defined before tests/algorithm is added")
endif()

if(NOT TARGET GTest::gtest_main)
    message(FATAL_ERROR "GTest::gtest_main must be available before tests/algorithm is added")
endif()

if(TARGET Qt5::Core)
    find_package(Qt5 5.15 REQUIRED COMPONENTS Network)
    set(HWTEST_ALGORITHM_QT_NETWORK_TARGET Qt5::Network)
elseif(TARGET Qt6::Core)
    find_package(Qt6 REQUIRED COMPONENTS Network)
    set(HWTEST_ALGORITHM_QT_NETWORK_TARGET Qt6::Network)
else()
    message(FATAL_ERROR "A Qt Core target must be available before tests/algorithm is added")
endif()

add_executable(hwtest_algorithm_tests
    mbddf_protocol_test.cpp
    system_status_executor_test.cpp
)

file(TO_CMAKE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mbddf_system_status.testcfg.json"
     HWTEST_MBDDF_SYSTEM_STATUS_CONFIG)

target_compile_features(hwtest_algorithm_tests PRIVATE cxx_std_17)

target_compile_definitions(hwtest_algorithm_tests
    PRIVATE
        HWTEST_MBDDF_SYSTEM_STATUS_CONFIG="${HWTEST_MBDDF_SYSTEM_STATUS_CONFIG}"
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
