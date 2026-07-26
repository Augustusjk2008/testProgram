add_library(hwtest_fake_nidaqmx STATIC
    fake_nidaqmx.cpp
)
set_target_properties(hwtest_fake_nidaqmx PROPERTIES AUTOMOC OFF)
set_target_properties(hwtest_fake_nidaqmx PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_compile_features(hwtest_fake_nidaqmx PRIVATE cxx_std_17)
target_include_directories(hwtest_fake_nidaqmx
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/fake
        ${CMAKE_CURRENT_SOURCE_DIR}
)

add_executable(hwtest_ni_daqmx_adapter_fake_tests
    ni_daqmx_adapter_fake_test.cpp
    ../ni_daqmx_adapter.cpp
)
set_target_properties(hwtest_ni_daqmx_adapter_fake_tests PROPERTIES AUTOMOC OFF)

target_compile_features(hwtest_ni_daqmx_adapter_fake_tests PRIVATE cxx_std_17)
target_include_directories(hwtest_ni_daqmx_adapter_fake_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/fake
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${PROJECT_SOURCE_DIR}/src/hal/include
)
target_link_libraries(hwtest_ni_daqmx_adapter_fake_tests
    PRIVATE hwtest_fake_nidaqmx
)

if(MSVC)
    target_compile_options(hwtest_ni_daqmx_adapter_fake_tests PRIVATE /utf-8)
endif()

add_test(NAME NiDaqmxAdapterFakeTest
    COMMAND $<TARGET_FILE:hwtest_ni_daqmx_adapter_fake_tests>
)
set_tests_properties(NiDaqmxAdapterFakeTest PROPERTIES
    LABELS "hal;adapter;ni_daqmx;fake"
    TIMEOUT 10
)
