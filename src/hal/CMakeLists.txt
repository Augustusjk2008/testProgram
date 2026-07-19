set(HAL_PUBLIC_HEADERS
    include/hal/hal_global.h
    include/hal/hal_types.h
    include/hal/hal_adapter_abi.h
    include/hal/i_hal_service.h
    include/hal/i_hal_device.h
    include/hal/i_analog_io.h
    include/hal/i_digital_io.h
    include/hal/i_serial_bus.h
    include/hal/i_canfd_bus.h
    include/hal/i_control_channel.h
    include/hal/hal_factory.h
)

set(HAL_PRIVATE_HEADERS
    src/hardware_adapter.h
    src/mock_adapter.h
    src/c_abi_adapter.h
    src/adapter_loader.h
    src/control_io_provider.h
    src/qt_serial_control_provider.h
    src/qt_udp_control_provider.h
    src/control_channel_manager.h
    src/resource_mapper.h
    src/safety_guard.h
    src/hal_device.h
    src/hal_service.h
)

set(HAL_SOURCES
    src/hal_types.cpp
    src/hal_error_mapper.cpp
    src/qt_serial_control_provider.cpp
    src/qt_udp_control_provider.cpp
    src/control_channel_manager.cpp
    src/resource_mapper.cpp
    src/safety_guard.cpp
    src/mock_adapter.cpp
    src/c_abi_adapter.cpp
    src/adapter_loader.cpp
    src/hal_device.cpp
    src/hal_service.cpp
    src/hal_factory.cpp
)

add_library(hwtest_hal STATIC
    ${HAL_PUBLIC_HEADERS}
    ${HAL_PRIVATE_HEADERS}
    ${HAL_SOURCES}
)

set_target_properties(hwtest_hal PROPERTIES
    OUTPUT_NAME hwtest_hal
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(hwtest_hal
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(hwtest_hal
    PUBLIC
        ${HAL_QT_CORE_TARGET}
    PRIVATE
        ${HAL_QT_NETWORK_TARGET}
        ${HAL_QT_SERIALPORT_TARGET}
)

target_compile_features(hwtest_hal PUBLIC cxx_std_17)

target_compile_definitions(hwtest_hal
    PUBLIC
        HWTEST_HAL_STATIC
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)
