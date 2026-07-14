set(LOG_PUBLIC_HEADERS
    include/logging/log_global.h
    include/logging/log_types.h
    include/logging/i_log_service.h
    include/logging/log_service.h
    include/logging/log_formatter.h
    include/logging/log_file_sink.h
    include/logging/hal_log_bridge.h
)

set(LOG_SOURCES
    src/log_service.cpp
    src/log_formatter.cpp
    src/log_file_sink.cpp
    src/hal_log_bridge.cpp
)

# HAL-free boundary for layers that only exchange LogEvent values.  It owns the
# type conversion and Qt metatype functions without pulling in the log service
# or the HAL bridge.
add_library(hwtest_log_types STATIC
    include/logging/log_global.h
    include/logging/log_types.h
    src/log_types.cpp
)

set_target_properties(hwtest_log_types PROPERTIES
    OUTPUT_NAME hwtest_log_types
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(hwtest_log_types
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hwtest_log_types
    PUBLIC
        ${HWTEST_QT_CORE_TARGET}
)

target_compile_features(hwtest_log_types PUBLIC cxx_std_17)
target_compile_definitions(hwtest_log_types
    PUBLIC
        HWTEST_LOG_STATIC
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)

add_library(hwtest_log STATIC
    ${LOG_PUBLIC_HEADERS}
    ${LOG_SOURCES}
)

set_target_properties(hwtest_log PROPERTIES
    OUTPUT_NAME hwtest_log
    POSITION_INDEPENDENT_CODE ON
)

target_include_directories(hwtest_log
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hwtest_log
    PUBLIC
        ${HAL_QT_CORE_TARGET}
        hwtest_hal
        hwtest_log_types
)

target_compile_features(hwtest_log PUBLIC cxx_std_17)

target_compile_definitions(hwtest_log
    PUBLIC
        HWTEST_LOG_STATIC
    PRIVATE
        QT_NO_CAST_FROM_ASCII
        QT_NO_CAST_TO_ASCII
)
