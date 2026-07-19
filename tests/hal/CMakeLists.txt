include(FetchContent)

set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)

FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(googletest)

add_library(hal_adapter_fixture SHARED
    fixtures/adapter_loader_fixture.cpp
)

set_target_properties(hal_adapter_fixture PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

target_include_directories(hal_adapter_fixture
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/hal/include
)

target_link_libraries(hal_adapter_fixture
    PRIVATE
        ${HAL_QT_CORE_TARGET}
)

add_library(hal_adapter_missing_symbol_fixture SHARED
    fixtures/adapter_loader_missing_symbol_fixture.cpp
)

set_target_properties(hal_adapter_missing_symbol_fixture PROPERTIES
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

target_include_directories(hal_adapter_missing_symbol_fixture
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/hal/include
)

target_link_libraries(hal_adapter_missing_symbol_fixture
    PRIVATE
        ${HAL_QT_CORE_TARGET}
)

add_executable(hwtest_hal_tests
    adapter_loader_test.cpp
    control_channel_test.cpp
    hal_device_test.cpp
    hal_error_mapper_test.cpp
    hal_service_test.cpp
    hal_types_test.cpp
    mock_adapter_test.cpp
    resource_mapper_test.cpp
    safety_guard_test.cpp
)

target_include_directories(hwtest_hal_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src/hal/include
        ${PROJECT_SOURCE_DIR}/src/hal/src
)

target_compile_definitions(hwtest_hal_tests
    PRIVATE
        HAL_TEST_ADAPTER_FIXTURE_PATH="$<TARGET_FILE:hal_adapter_fixture>"
        HAL_TEST_ADAPTER_MISSING_SYMBOL_FIXTURE_PATH="$<TARGET_FILE:hal_adapter_missing_symbol_fixture>"
)

target_link_libraries(hwtest_hal_tests
    PRIVATE
        hwtest_hal
        ${HAL_QT_NETWORK_TARGET}
        GTest::gtest_main
)

add_dependencies(hwtest_hal_tests
    hal_adapter_fixture
    hal_adapter_missing_symbol_fixture
)

include(GoogleTest)
gtest_discover_tests(hwtest_hal_tests)
