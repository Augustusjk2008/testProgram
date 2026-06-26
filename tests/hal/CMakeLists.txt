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

add_executable(hwtest_hal_tests
    hal_error_mapper_test.cpp
    hal_service_test.cpp
    resource_mapper_test.cpp
    safety_guard_test.cpp
)

target_include_directories(hwtest_hal_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src/hal/include
        ${PROJECT_SOURCE_DIR}/src/hal/src
)

target_link_libraries(hwtest_hal_tests
    PRIVATE
        hwtest_hal
        GTest::gtest_main
)

include(GoogleTest)
gtest_discover_tests(hwtest_hal_tests)
