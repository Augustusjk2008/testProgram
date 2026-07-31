add_library(MB_DDF_HW STATIC
    Os/Fd.cpp
    Os/MmapRegion.cpp
    Os/Poll.cpp
    Os/Epoll.cpp
    Transport/NullTransport.cpp
    Transport/XdmaTransport.cpp
    Transport/SpidevTransport.cpp
    Device/PwmDevice.cpp
    Device/Ad7606Device.cpp
    Device/Ads1258Device.cpp
    Device/XadcDevice.cpp
    Device/UpdateImageIpVersionDevice.cpp
    Device/FpgaUpdateStateDevice.cpp
    Device/DidoDevice.cpp
    Device/DhController.cpp
    Device/ComDevice.cpp
    Device/SpiFlashDevice.cpp
)

target_include_directories(MB_DDF_HW PUBLIC
    ${CMAKE_SOURCE_DIR}/src
)

target_compile_features(MB_DDF_HW PUBLIC cxx_std_20)
target_link_libraries(MB_DDF_HW PUBLIC pthread rt)

option(BUILD_HW_DDS_ADAPTER "Build DDS adapter for MB_DDF_HW" ON)
if(BUILD_HW_DDS_ADAPTER)
    add_library(MB_DDF_HW_DDS_Adapter STATIC
        DdsAdapter/ExternalEndpointAdapter.cpp
        DdsAdapter/CallbackExternalEndpoint.cpp
        DdsAdapter/ComExternalEndpoint.cpp
    )
    target_include_directories(MB_DDF_HW_DDS_Adapter PUBLIC ${CMAKE_SOURCE_DIR}/src)
    target_compile_features(MB_DDF_HW_DDS_Adapter PUBLIC cxx_std_20)
    target_link_libraries(MB_DDF_HW_DDS_Adapter PUBLIC MB_DDF_HW)
endif()
