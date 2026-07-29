cmake_minimum_required(VERSION 3.10)

set(TEST_TARGET_NAME "MB_DDF_v2_Tests")
set(HARDWARE_TEST_TARGET_NAME "MB_DDF_v2_HardwareTests")
option(ENABLE_HW_UNIT_TESTS "Enable hardware layer unit tests" OFF)
option(ENABLE_HW_SMOKE_TESTS "Enable hardware layer smoke tests" OFF)
foreach(_option ENABLE_HW_UNIT_TESTS ENABLE_HW_SMOKE_TESTS)
    if(DEFINED ENV{${_option}})
        set(${_option} $ENV{${_option}})
        message(STATUS "${_option} set from environment: ${${_option}}")
    endif()
endforeach()

set(TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_dds_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_domain_gateway.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_external_endpoint.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_gateway_envelope.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_message.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_publisher_subscriber.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_ringbuffer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_semaphore_guard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_shared_memory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/unit/test_topic_registry.cpp
)

set(HARDWARE_TEST_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware/test_hardware_smoke.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware/test_hardware_ipc.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware/test_hardware_stress.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware/test_hardware_performance.cpp
)

set(DDS_TESTED_SOURCES
    ${CMAKE_SOURCE_DIR}/src/MB_DDF/DDS/DDSCore.cpp
    ${CMAKE_SOURCE_DIR}/src/MB_DDF/DDS/Gateway/DdsGatewayLocalBus.cpp
    ${CMAKE_SOURCE_DIR}/src/MB_DDF/DDS/Gateway/DomainGateway.cpp
    ${CMAKE_SOURCE_DIR}/src/MB_DDF/DDS/Gateway/GatewayEnvelope.cpp
    ${CMAKE_SOURCE_DIR}/src/MB_DDF/DDS/ExternalPort.cpp
    ${CMAKE_SOURCE_DIR}/src/MB_DDF/DDS/ProcessSharedSync.cpp
    ${CMAKE_SOURCE_DIR}/src/MB_DDF/DDS/Publisher.cpp
    ${CMAKE_SOURCE_DIR}/src/MB_DDF/DDS/RingBuffer.cpp
    ${CMAKE_SOURCE_DIR}/src/MB_DDF/DDS/SharedMemory.cpp
    ${CMAKE_SOURCE_DIR}/src/MB_DDF/DDS/Subscriber.cpp
    ${CMAKE_SOURCE_DIR}/src/MB_DDF/DDS/TopicRegistry.cpp
)

function(configure_mb_ddf_test_target target_name)
    target_include_directories(${target_name} PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src
    )

    target_link_libraries(${target_name} PRIVATE
        gtest_main
        gmock
        pthread
        rt
    )

    target_compile_features(${target_name} PRIVATE cxx_std_20)
    target_compile_definitions(${target_name} PRIVATE
        MB_DDF_TEST_BUILD=1
    )

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${target_name} PRIVATE
            -g
            -O0
            -Wall
            -Wextra
            --coverage
        )
        target_link_options(${target_name} PRIVATE --coverage)
    else()
        target_compile_options(${target_name} PRIVATE -O2)
    endif()

    target_link_options(${target_name} PRIVATE
        -static-libstdc++
        -static-libgcc
    )
endfunction()

add_executable(${TEST_TARGET_NAME} ${TEST_SOURCES} ${DDS_TESTED_SOURCES})
configure_mb_ddf_test_target(${TEST_TARGET_NAME})

add_executable(${HARDWARE_TEST_TARGET_NAME} ${HARDWARE_TEST_SOURCES} ${DDS_TESTED_SOURCES})
configure_mb_ddf_test_target(${HARDWARE_TEST_TARGET_NAME})

enable_testing()
add_test(NAME ${TEST_TARGET_NAME} COMMAND ${TEST_TARGET_NAME})
add_test(NAME ${HARDWARE_TEST_TARGET_NAME} COMMAND ${HARDWARE_TEST_TARGET_NAME})

if(ENABLE_HW_UNIT_TESTS AND TARGET MB_DDF_HW)
    add_executable(MB_DDF_HW_Tests
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_hw_os.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_hw_transport.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_pwm_device.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_ad7606_device.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_ads1258_device.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_xadc_device.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_dido_device.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_dh_controller.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_com_device.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_com_loopback_workflow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_flash_device.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_spi_flash_device.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_spi_flash_workflow.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_dds_adapter.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_product_protocol.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_hardware_test_service.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_system_test_provider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_hardware_test_provider.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_helm_dds_bridge.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/hw_unit/test_helm_pwm_lifecycle.cpp
        ${CMAKE_SOURCE_DIR}/src/MB_DDF_HW_Test/ProductProtocol.cpp
        ${CMAKE_SOURCE_DIR}/src/MB_DDF_HW_Test/HardwareTestService.cpp
        ${CMAKE_SOURCE_DIR}/src/MB_DDF_HW_Test/SystemTestProvider.cpp
        ${CMAKE_SOURCE_DIR}/src/MB_DDF_HW_Test/HardwareTestProvider.cpp
        ${CMAKE_SOURCE_DIR}/src/MB_DDF_HW_Test/HelmDdsTestBridge.cpp
        ${CMAKE_SOURCE_DIR}/src/HelmControl/HelmPwmLifecycle.cpp
        ${CMAKE_SOURCE_DIR}/src/MB_DDF_Demo/HardwareExamples.cpp
        ${DDS_TESTED_SOURCES}
    )
    configure_mb_ddf_test_target(MB_DDF_HW_Tests)
    target_include_directories(MB_DDF_HW_Tests PRIVATE ${PRODUCT_PROTOCOL_GENERATED_DIR})
    add_dependencies(MB_DDF_HW_Tests generate_product_protocol)
    target_link_libraries(MB_DDF_HW_Tests PRIVATE MB_DDF_HW)
    target_compile_definitions(MB_DDF_HW_Tests PRIVATE MB_DDF_DEMO_WITH_HARDWARE=1)
    if(TARGET MB_DDF_HW_DDS_Adapter)
        target_link_libraries(MB_DDF_HW_Tests PRIVATE MB_DDF_HW_DDS_Adapter)
    endif()
    add_test(NAME MB_DDF_HW_Tests COMMAND MB_DDF_HW_Tests)
endif()

if(ENABLE_HW_SMOKE_TESTS AND TARGET MB_DDF_HW)
    add_executable(MB_DDF_HW_Smoke ${CMAKE_CURRENT_SOURCE_DIR}/hardware/test_mb_ddf_hw_smoke.cpp)
    target_include_directories(MB_DDF_HW_Smoke PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(MB_DDF_HW_Smoke PRIVATE cxx_std_20)
    target_link_libraries(MB_DDF_HW_Smoke PRIVATE MB_DDF_HW pthread rt)
    if(TARGET MB_DDF_HW_DDS_Adapter)
        target_link_libraries(MB_DDF_HW_Smoke PRIVATE MB_DDF_HW_DDS_Adapter)
        target_compile_definitions(MB_DDF_HW_Smoke PRIVATE MB_DDF_HW_SMOKE_WITH_ADAPTER=1)
    endif()
    target_link_options(MB_DDF_HW_Smoke PRIVATE -static-libstdc++ -static-libgcc)
endif()
