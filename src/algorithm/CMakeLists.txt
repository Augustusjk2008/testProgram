set(ALGORITHM_PUBLIC_HEADERS
    include/algorithm/board_test_executor.h
    include/algorithm/bus_echo_transport.h
    include/algorithm/di_stimulus_controller.h
    include/algorithm/dh_ignite_stream_executor.h
    include/algorithm/elec_health_status_executor.h
    include/algorithm/imu_stream_executor.h
    include/algorithm/helm_stream_executor.h
    include/algorithm/mbddf_exchange_executor.h
    include/algorithm/mbddf_protocol.h
    include/algorithm/mbddf_transport.h
    include/algorithm/post_run_analysis.h
    include/algorithm/run_parameter_schema.h
    include/algorithm/serial_test_executor.h
    include/algorithm/system_status_executor.h
)

set(ALGORITHM_SOURCES
    src/board_test_executor.cpp
    src/bus_echo_transport.cpp
    src/di_stimulus_controller.cpp
    src/dh_ignite_stream_executor.cpp
    src/elec_health_status_executor.cpp
    src/imu_stream_executor.cpp
    src/helm_stream_executor.cpp
    src/mbddf_protocol.cpp
    src/mbddf_transport.cpp
    src/post_run_analysis_factory.cpp
    src/helm_analysis_capture.cpp
    src/helm_performance_analyzer.cpp
    src/helm_waveform_analyzers.cpp
    src/helm_sweep_estimator.cpp
    src/analysis_result_json.cpp
    src/run_parameter_schema.cpp
    src/serial_test_executor.cpp
    src/system_status_executor.cpp
)

if(NOT TARGET hwtest_biz)
    message(FATAL_ERROR "hwtest_algorithm_mbddf requires the hwtest_biz target")
endif()

if(NOT TARGET hwtest_hal)
    message(FATAL_ERROR "hwtest_algorithm_mbddf requires the hwtest_hal target")
endif()

if(NOT DEFINED HWTEST_QT_CORE_TARGET OR NOT TARGET "${HWTEST_QT_CORE_TARGET}")
    message(FATAL_ERROR "hwtest_algorithm_mbddf requires HWTEST_QT_CORE_TARGET")
endif()

add_library(hwtest_algorithm_mbddf STATIC
    ${ALGORITHM_PUBLIC_HEADERS}
    ${ALGORITHM_SOURCES}
)

set_target_properties(hwtest_algorithm_mbddf PROPERTIES
    OUTPUT_NAME hwtest_algorithm_mbddf
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(hwtest_algorithm_mbddf
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hwtest_algorithm_mbddf
    PUBLIC
        hwtest_biz
        hwtest_hal
        ${HWTEST_QT_CORE_TARGET}
)

target_compile_features(hwtest_algorithm_mbddf PUBLIC cxx_std_17)

target_compile_definitions(hwtest_algorithm_mbddf
    PUBLIC
        HWTEST_ALGORITHM_MBDDF_STATIC
)

if(MSVC)
    target_compile_options(hwtest_algorithm_mbddf PUBLIC /utf-8)
endif()
