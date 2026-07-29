if(NOT TARGET GTest::gtest_main)
    message(FATAL_ERROR "GTest::gtest_main must be available before tests/app is added")
endif()
if(NOT TARGET GTest::gtest)
    message(FATAL_ERROR "GTest::gtest must be available before tests/app is added")
endif()

add_library(hwtest_app_test_support STATIC
    support/application_snapshot_test_utils.h
    support/mbddf_udp_test_peer.h
    support/mbddf_udp_test_peer.cpp
)

target_compile_features(hwtest_app_test_support PUBLIC cxx_std_17)

target_include_directories(hwtest_app_test_support
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(hwtest_app_test_support
    PUBLIC
        hwtest_algorithm_mbddf
        ${HWTEST_QT_NETWORK_TARGET}
)

if(MSVC)
    target_compile_options(hwtest_app_test_support PRIVATE /utf-8)
endif()

add_executable(hwtest_app_tests
    analysis_result_store_test.cpp
    frontend_launch_options_test.cpp
    post_run_analysis_coordinator_test.cpp
    test_application_controller_test.cpp
    tui_shell_test.cpp
)

file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/configs/mbddf_system_status.testcfg.json"
     HWTEST_APP_TEST_CONFIG)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/configs/mbddf_elec_health.testcfg.json"
     HWTEST_APP_ELEC_HEALTH_CONFIG)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/configs/mbddf_pc_hal.json"
     HWTEST_APP_HAL_CONFIG)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/configs/mbddf_di.testcfg.json"
     HWTEST_APP_DI_CONFIG)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/configs/mbddf_imu_stream.testcfg.json"
     HWTEST_APP_IMU_STREAM_CONFIG)
file(TO_CMAKE_PATH "${PROJECT_SOURCE_DIR}/configs/mbddf_helm_stream.testcfg.json"
     HWTEST_APP_HELM_STREAM_CONFIG)

target_compile_features(hwtest_app_tests PRIVATE cxx_std_17)

target_compile_definitions(hwtest_app_tests
    PRIVATE
        HWTEST_APP_TEST_CONFIG="${HWTEST_APP_TEST_CONFIG}"
        HWTEST_APP_ELEC_HEALTH_CONFIG="${HWTEST_APP_ELEC_HEALTH_CONFIG}"
        HWTEST_APP_HAL_CONFIG="${HWTEST_APP_HAL_CONFIG}"
        HWTEST_APP_DI_CONFIG="${HWTEST_APP_DI_CONFIG}"
        HWTEST_APP_IMU_STREAM_CONFIG="${HWTEST_APP_IMU_STREAM_CONFIG}"
        HWTEST_APP_HELM_STREAM_CONFIG="${HWTEST_APP_HELM_STREAM_CONFIG}"
        HAL_TEST_DIGITAL_ADAPTER_FIXTURE_PATH="$<TARGET_FILE:hal_adapter_digital_fixture>"
        HWTEST_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_include_directories(hwtest_app_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/app/include
        ${PROJECT_SOURCE_DIR}/src/app/src
)

target_link_libraries(hwtest_app_tests
    PRIVATE
        hwtest_app_core
        hwtest_tui_support
        hwtest_app_test_support
        GTest::gtest_main
)

add_dependencies(hwtest_app_tests
    hal_adapter_digital_fixture
    hwtest_pc_runner
    hwtest_tui
)

include(GoogleTest)
gtest_discover_tests(hwtest_app_tests)

add_executable(hwtest_gui_tests
    gui_test_main.cpp
    gui_main_window_test.cpp
)

target_compile_features(hwtest_gui_tests PRIVATE cxx_std_17)

target_compile_definitions(hwtest_gui_tests
    PRIVATE
        HWTEST_APP_TEST_CONFIG="${HWTEST_APP_TEST_CONFIG}"
        HWTEST_APP_HAL_CONFIG="${HWTEST_APP_HAL_CONFIG}"
        HWTEST_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_link_libraries(hwtest_gui_tests
    PRIVATE
        hwtest_gui_support
        hwtest_tui_support
        hwtest_app_test_support
        GTest::gtest
)

add_dependencies(hwtest_gui_tests
    hwtest_gui
    hwtest_tui
)

if(MSVC)
    target_compile_options(hwtest_gui_tests PRIVATE /utf-8)
endif()

gtest_discover_tests(hwtest_gui_tests)

add_executable(hwtest_web_tests
    web_test_main.cpp
    web_protocol_test.cpp
    web_telemetry_batcher_test.cpp
    web_socket_frontend_server_test.cpp
    web_controller_integration_test.cpp
    web_architecture_test.cpp
    support/websocket_test_client.h
    support/websocket_test_client.cpp
)

target_compile_features(hwtest_web_tests PRIVATE cxx_std_17)

target_compile_definitions(hwtest_web_tests
    PRIVATE
        HWTEST_APP_TEST_CONFIG="${HWTEST_APP_TEST_CONFIG}"
        HWTEST_APP_HAL_CONFIG="${HWTEST_APP_HAL_CONFIG}"
        HWTEST_APP_HELM_STREAM_CONFIG="${HWTEST_APP_HELM_STREAM_CONFIG}"
        HWTEST_PROJECT_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

target_include_directories(hwtest_web_tests
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/app/web
)

target_link_libraries(hwtest_web_tests
    PRIVATE
        hwtest_web_support
        hwtest_tui_support
        hwtest_app_test_support
        ${HWTEST_QT_WEBSOCKETS_TARGET}
        GTest::gtest
)

add_dependencies(hwtest_web_tests hwtest_web)

if(MSVC)
    target_compile_options(hwtest_web_tests PRIVATE /utf-8)
endif()

gtest_discover_tests(hwtest_web_tests
    PROPERTIES
        LABELS "app_websocket"
        TIMEOUT 20
)

add_test(NAME HwtestWebHelp
    COMMAND $<TARGET_FILE:hwtest_web> --help
)
set_tests_properties(HwtestWebHelp PROPERTIES
    PASS_REGULAR_EXPRESSION "web-port.*WebSocket"
    TIMEOUT 5
)

add_test(NAME HwtestWebStartsLoopback
    COMMAND $<TARGET_FILE:hwtest_web> --smoke-test
)
set_tests_properties(HwtestWebStartsLoopback PROPERTIES
    PASS_REGULAR_EXPRESSION "ready wsUrl=ws://127\\.0\\.0\\.1:[0-9]+/ws"
    TIMEOUT 5
)

set_tests_properties(
    HwtestWebHelp
    HwtestWebStartsLoopback
    PROPERTIES
        LABELS "app;websocket;process"
)

add_test(NAME HwtestGuiStartsOffscreen
    COMMAND $<TARGET_FILE:hwtest_gui> --smoke-test
)
set_tests_properties(HwtestGuiStartsOffscreen PROPERTIES
    ENVIRONMENT "QT_QPA_PLATFORM=offscreen"
    TIMEOUT 5
)

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
        PASS_REGULAR_EXPRESSION "WebSocket backend"
        TIMEOUT 5
    )

    add_test(NAME HwtestScriptRejectsInvalidUi
        COMMAND ${CMAKE_COMMAND}
            "-DSCRIPT=${PROJECT_SOURCE_DIR}/hwtest.ps1"
            -DMODE=reject_invalid_ui
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_hwtest_script_test.cmake"
    )
    set_tests_properties(HwtestScriptRejectsInvalidUi PROPERTIES TIMEOUT 15)

    add_test(NAME HwtestScriptSelectsTui
        COMMAND ${CMAKE_COMMAND}
            "-DSCRIPT=${PROJECT_SOURCE_DIR}/hwtest.ps1"
            -DMODE=select_tui
            "-DBUILD_DIR=${PROJECT_BINARY_DIR}"
            "-DCONFIGURATION=$<CONFIG>"
            "-DINPUT_FILE=${CMAKE_CURRENT_SOURCE_DIR}/tui_smoke_input.txt"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/run_hwtest_script_test.cmake"
    )
    set_tests_properties(HwtestScriptSelectsTui PROPERTIES TIMEOUT 20)
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
