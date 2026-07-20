if(NOT TARGET GTest::gtest_main)
    message(FATAL_ERROR "GTest::gtest_main must be available before tests/app is added")
endif()

add_executable(hwtest_app_tests
    test_application_controller_test.cpp
    tui_shell_test.cpp
)

file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/configs/mbddf_system_status.testcfg.json"
     HWTEST_APP_TEST_CONFIG)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/configs/mbddf_pc_hal.json"
     HWTEST_APP_HAL_CONFIG)

target_compile_features(hwtest_app_tests PRIVATE cxx_std_17)

target_compile_definitions(hwtest_app_tests
    PRIVATE
        HWTEST_APP_TEST_CONFIG="${HWTEST_APP_TEST_CONFIG}"
        HWTEST_APP_HAL_CONFIG="${HWTEST_APP_HAL_CONFIG}"
)

target_include_directories(hwtest_app_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/app/include
)

target_link_libraries(hwtest_app_tests
    PRIVATE
        hwtest_app_core
        hwtest_algorithm_mbddf
        ${HWTEST_QT_NETWORK_TARGET}
        GTest::gtest_main
)

add_dependencies(hwtest_app_tests
    hwtest_pc_runner
    hwtest_tui
)

include(GoogleTest)
gtest_discover_tests(hwtest_app_tests)

add_test(NAME HwtestPcRunnerHelp
    COMMAND $<TARGET_FILE:hwtest_pc_runner> --help
)
set_tests_properties(HwtestPcRunnerHelp PROPERTIES
    PASS_REGULAR_EXPRESSION "Usage:.*hwtest_pc_runner"
    TIMEOUT 5
)

add_test(NAME HwtestTuiHelp
    COMMAND $<TARGET_FILE:hwtest_tui> --help
)
set_tests_properties(HwtestTuiHelp PROPERTIES
    PASS_REGULAR_EXPRESSION "serial-port"
    TIMEOUT 5
)

if(WIN32)
    add_test(NAME HwtestScriptHelp
        COMMAND powershell
            -NoProfile
            -ExecutionPolicy Bypass
            -File "${PROJECT_SOURCE_DIR}/hwtest.ps1"
            help
    )
    set_tests_properties(HwtestScriptHelp PROPERTIES
        PASS_REGULAR_EXPRESSION "hwtest.ps1 <action>"
        TIMEOUT 5
    )
endif()

add_test(NAME HwtestTuiScriptedSession
    COMMAND ${CMAKE_COMMAND}
        "-DTUI_EXE=$<TARGET_FILE:hwtest_tui>"
        "-DTEST_CONFIG=${HWTEST_APP_TEST_CONFIG}"
        "-DHAL_CONFIG=${HWTEST_APP_HAL_CONFIG}"
        "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tui_smoke_input.txt"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_tui_smoke.cmake"
)
set_tests_properties(HwtestTuiScriptedSession PROPERTIES
    TIMEOUT 15
)

add_test(NAME HwtestPcRunnerAsyncError
    COMMAND ${CMAKE_COMMAND}
        "-DRUNNER_EXE=$<TARGET_FILE:hwtest_pc_runner>"
        "-DTEST_CONFIG=${HWTEST_APP_TEST_CONFIG}"
        "-DHAL_CONFIG=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/mbddf_udp_hal.json"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/run_runner_error.cmake"
)
set_tests_properties(HwtestPcRunnerAsyncError PROPERTIES
    TIMEOUT 15
)
