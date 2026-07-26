if(NOT NI_DAQMX_INCLUDE_DIR)
    find_path(NI_DAQMX_INCLUDE_DIR
        NIDAQmx.h
        DOC "Directory containing NIDAQmx.h"
    )
endif()
if(NOT NI_DAQMX_LIBRARY)
    find_library(NI_DAQMX_LIBRARY
        NAMES NIDAQmx nicaiu
        DOC "Path to the NI-DAQmx import library"
    )
endif()

if(NOT NI_DAQMX_INCLUDE_DIR OR
   NOT EXISTS "${NI_DAQMX_INCLUDE_DIR}/NIDAQmx.h")
    message(FATAL_ERROR
        "HWTEST_ENABLE_NI_DAQMX=ON requires NI_DAQMX_INCLUDE_DIR containing NIDAQmx.h")
endif()
if(NOT NI_DAQMX_LIBRARY OR NOT EXISTS "${NI_DAQMX_LIBRARY}")
    message(FATAL_ERROR
        "HWTEST_ENABLE_NI_DAQMX=ON requires NI_DAQMX_LIBRARY pointing to NIDAQmx.lib")
endif()

add_library(hwtest_adapter_ni_daqmx SHARED
    ni_daqmx_adapter.cpp
)

target_compile_features(hwtest_adapter_ni_daqmx PRIVATE cxx_std_17)
target_include_directories(hwtest_adapter_ni_daqmx
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/hal/include
        ${NI_DAQMX_INCLUDE_DIR}
)
target_link_libraries(hwtest_adapter_ni_daqmx PRIVATE ${NI_DAQMX_LIBRARY})

set_target_properties(hwtest_adapter_ni_daqmx PROPERTIES
    AUTOMOC OFF
    OUTPUT_NAME hwtest_adapter_ni_daqmx
    WINDOWS_EXPORT_ALL_SYMBOLS ON
)

if(MSVC)
    target_compile_options(hwtest_adapter_ni_daqmx PRIVATE /utf-8)
endif()
