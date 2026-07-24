cmake_minimum_required(VERSION 3.10)

set(PROJECT_NAME_OVERRIDE "MB_DDF_v2" CACHE STRING "Project name")
project(${PROJECT_NAME_OVERRIDE} LANGUAGES CXX VERSION 1.0.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE "Debug" CACHE STRING "Choose build type" FORCE)
endif()
set(CMAKE_CONFIGURATION_TYPES "Debug;Release" CACHE STRING "Configs" FORCE)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_SYSTEM_NAME Linux CACHE STRING "Target system" FORCE)
set(CMAKE_SYSTEM_PROCESSOR aarch64 CACHE STRING "Target architecture" FORCE)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE AND DEFINED ENV{OECORE_CMAKE_TOOLCHAIN_FILE})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{OECORE_CMAKE_TOOLCHAIN_FILE}" CACHE FILEPATH "Toolchain" FORCE)
endif()

set(CROSS_SYSROOT "${CROSS_SYSROOT}" CACHE PATH "Cross sysroot")
set(CROSS_C_COMPILER "${CROSS_C_COMPILER}" CACHE STRING "Cross C compiler")
set(CROSS_CXX_COMPILER "${CROSS_CXX_COMPILER}" CACHE STRING "Cross CXX compiler")

if(NOT CROSS_SYSROOT AND DEFINED ENV{CROSS_SYSROOT})
    set(CROSS_SYSROOT "$ENV{CROSS_SYSROOT}")
endif()
if(NOT CROSS_C_COMPILER AND DEFINED ENV{CROSS_C_COMPILER})
    set(CROSS_C_COMPILER "$ENV{CROSS_C_COMPILER}")
endif()
if(NOT CROSS_CXX_COMPILER AND DEFINED ENV{CROSS_CXX_COMPILER})
    set(CROSS_CXX_COMPILER "$ENV{CROSS_CXX_COMPILER}")
endif()

if(NOT CROSS_CXX_COMPILER OR CROSS_CXX_COMPILER STREQUAL "")
    find_program(CROSS_CXX_COMPILER NAMES aarch64-none-linux-gnu-g++ aarch64-ucas-linux-g++ aarch64-linux-gnu-g++)
endif()
if(NOT CROSS_C_COMPILER OR CROSS_C_COMPILER STREQUAL "")
    find_program(CROSS_C_COMPILER NAMES aarch64-none-linux-gnu-gcc aarch64-ucas-linux-gcc aarch64-linux-gnu-gcc)
endif()

if((NOT CROSS_SYSROOT OR CROSS_SYSROOT STREQUAL "") AND EXISTS "/opt/wanghuo/v2.0.0-rc4/sysroots/armv8a-ucas-linux")
    set(CROSS_SYSROOT "/opt/wanghuo/v2.0.0-rc4/sysroots/armv8a-ucas-linux")
endif()

if((NOT CROSS_SYSROOT OR CROSS_SYSROOT STREQUAL "") AND (NOT CMAKE_SYSROOT OR CMAKE_SYSROOT STREQUAL ""))
    message(FATAL_ERROR "Cross sysroot is not set. Provide -DCROSS_SYSROOT=... or -DCMAKE_SYSROOT=....")
endif()

if((NOT CMAKE_SYSROOT OR CMAKE_SYSROOT STREQUAL "") AND (CROSS_SYSROOT AND NOT CROSS_SYSROOT STREQUAL ""))
    set(CMAKE_SYSROOT "${CROSS_SYSROOT}" CACHE PATH "CMake sysroot" FORCE)
endif()
if((NOT CMAKE_CXX_COMPILER OR CMAKE_CXX_COMPILER STREQUAL "") AND (CROSS_CXX_COMPILER AND NOT CROSS_CXX_COMPILER STREQUAL ""))
    set(CMAKE_CXX_COMPILER "${CROSS_CXX_COMPILER}" CACHE FILEPATH "CMake CXX compiler" FORCE)
endif()
if((NOT CMAKE_C_COMPILER OR CMAKE_C_COMPILER STREQUAL "") AND (CROSS_C_COMPILER AND NOT CROSS_C_COMPILER STREQUAL ""))
    set(CMAKE_C_COMPILER "${CROSS_C_COMPILER}" CACHE FILEPATH "CMake C compiler" FORCE)
endif()

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

set(TARGET_NAME "${PROJECT_NAME}")

option(BUILD_LIBRARY "Build library instead of executable" OFF)
option(BUILD_HARDWARE_LAYER "Build MB_DDF hardware layer" OFF)
option(BUILD_HW_DDS_ADAPTER "Build DDS adapter for MB_DDF_HW" ON)
option(ENABLE_TESTS "Enable test build (requires Google Test)" OFF)
foreach(_option BUILD_HARDWARE_LAYER BUILD_HW_DDS_ADAPTER)
    if(DEFINED ENV{${_option}})
        set(${_option} $ENV{${_option}})
        message(STATUS "${_option} set from environment: ${${_option}}")
    endif()
endforeach()

if(NOT ENABLE_TESTS AND DEFINED ENV{ENABLE_TESTS})
    set(ENABLE_TESTS $ENV{ENABLE_TESTS})
    message(STATUS "ENABLE_TESTS set from environment: ${ENABLE_TESTS}")
endif()

set(MB_DDF_APP_MODE "ECHO" CACHE STRING "Application image: ECHO, HW_TEST, or DEMO")
string(TOUPPER "${MB_DDF_APP_MODE}" MB_DDF_APP_MODE)
set_property(CACHE MB_DDF_APP_MODE PROPERTY STRINGS ECHO HW_TEST DEMO)
set(_valid_app_modes ECHO HW_TEST DEMO)
if(NOT MB_DDF_APP_MODE IN_LIST _valid_app_modes)
    message(FATAL_ERROR
        "Invalid MB_DDF_APP_MODE='${MB_DDF_APP_MODE}'. Expected ECHO, HW_TEST, or DEMO.")
endif()

# Executable images own their complete hardware composition. Library builds remain DDS-only.
if(NOT BUILD_LIBRARY)
    set(BUILD_HARDWARE_LAYER ON CACHE BOOL "Build MB_DDF hardware layer" FORCE)
    if(MB_DDF_APP_MODE STREQUAL "DEMO" OR ENABLE_TESTS)
        set(BUILD_HW_DDS_ADAPTER ON CACHE BOOL "Build DDS adapter for MB_DDF_HW" FORCE)
    else()
        set(BUILD_HW_DDS_ADAPTER OFF CACHE BOOL "Build DDS adapter for MB_DDF_HW" FORCE)
    endif()
endif()

set(DDS_CORE_SOURCES
    src/MB_DDF/DDS/DDSCore.cpp
    src/MB_DDF/DDS/Gateway/DdsGatewayLocalBus.cpp
    src/MB_DDF/DDS/Gateway/DomainGateway.cpp
    src/MB_DDF/DDS/Gateway/GatewayEnvelope.cpp
    src/MB_DDF/DDS/ExternalPort.cpp
    src/MB_DDF/DDS/ProcessSharedSync.cpp
    src/MB_DDF/DDS/Publisher.cpp
    src/MB_DDF/DDS/RingBuffer.cpp
    src/MB_DDF/DDS/SharedMemory.cpp
    src/MB_DDF/DDS/Subscriber.cpp
    src/MB_DDF/DDS/TopicRegistry.cpp
)

set(DDS_PUBLIC_HEADERS
    src/MB_DDF/Debug/Logger.h
    src/MB_DDF/DDS/DDSCore.h
    src/MB_DDF/DDS/ExternalEndpoint.h
    src/MB_DDF/DDS/ExternalPort.h
    src/MB_DDF/DDS/Gateway/DdsGatewayLocalBus.h
    src/MB_DDF/DDS/Gateway/DomainGateway.h
    src/MB_DDF/DDS/Gateway/GatewayEnvelope.h
    src/MB_DDF/DDS/Gateway/GatewayLocalBus.h
    src/MB_DDF/DDS/Message.h
    src/MB_DDF/DDS/ProcessSharedSync.h
    src/MB_DDF/DDS/Publisher.h
    src/MB_DDF/DDS/RingBuffer.h
    src/MB_DDF/DDS/SemaphoreGuard.h
    src/MB_DDF/DDS/SharedMemory.h
    src/MB_DDF/DDS/Subscriber.h
    src/MB_DDF/DDS/TopicRegistry.h
)

# 教学示例只进入可执行程序，不进入 MB_DDF 静态库或动态库。
set(DDS_DEMO_SOURCES
    src/MB_DDF_Demo/DdsExamples.cpp
    src/MB_DDF_Demo/DemoRunner.cpp
    src/MB_DDF_Demo/HardwareExamples.cpp
)

set(HW_TEST_SOURCES
    src/MB_DDF_HW_Test/ProductProtocol.cpp
    src/MB_DDF_HW_Test/HardwareTestService.cpp
    src/MB_DDF_HW_Test/SystemTestProvider.cpp
    src/MB_DDF_HW_Test/HardwareTestProvider.cpp
)

if((NOT BUILD_LIBRARY AND MB_DDF_APP_MODE STREQUAL "HW_TEST") OR
   (ENABLE_TESTS AND ENABLE_HW_UNIT_TESTS))
    set(PRODUCT_PROTOCOL_CSV_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/docs/design/product_protocol_csv")
    file(GLOB PRODUCT_PROTOCOL_CSV_FILES CONFIGURE_DEPENDS
        "${PRODUCT_PROTOCOL_CSV_DIR}/*.csv")
    set(PRODUCT_PROTOCOL_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    set(PRODUCT_PROTOCOL_GENERATED_HEADER
        "${PRODUCT_PROTOCOL_GENERATED_DIR}/ProductProtocolGenerated.h")
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_custom_command(
        OUTPUT "${PRODUCT_PROTOCOL_GENERATED_HEADER}"
        COMMAND "${Python3_EXECUTABLE}"
                "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_product_protocol.py"
                --output "${PRODUCT_PROTOCOL_GENERATED_HEADER}"
                "${PRODUCT_PROTOCOL_CSV_DIR}"
        DEPENDS
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/generate_product_protocol.py"
            ${PRODUCT_PROTOCOL_CSV_FILES}
        COMMENT "Validating product protocol CSV and generating ProductProtocolGenerated.h"
        VERBATIM
    )
    add_custom_target(generate_product_protocol
        DEPENDS "${PRODUCT_PROTOCOL_GENERATED_HEADER}")
endif()

function(configure_dds_target target_name visibility)
    target_include_directories(${target_name} ${visibility}
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>
        $<INSTALL_INTERFACE:include>
    )
    target_compile_features(${target_name} PUBLIC cxx_std_20)
    target_link_libraries(${target_name} PUBLIC pthread rt)

    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        target_compile_options(${target_name} PRIVATE -g -O0 -Wall -Wextra -pedantic)
        if(${visibility} STREQUAL "PRIVATE")
            target_link_options(${target_name} PRIVATE -g)
        endif()
    else()
        target_compile_options(${target_name} PRIVATE -O3 -DNDEBUG -Werror)
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
            target_link_options(${target_name} PRIVATE -flto=auto)
        elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
            target_link_options(${target_name} PRIVATE -flto=thin)
        endif()
        if(${visibility} STREQUAL "PRIVATE")
            target_link_options(${target_name} PRIVATE -s)
        endif()
    endif()
endfunction()

if(BUILD_LIBRARY)
    add_library(${TARGET_NAME}_static STATIC ${DDS_CORE_SOURCES})
    set_target_properties(${TARGET_NAME}_static PROPERTIES
        OUTPUT_NAME ${PROJECT_NAME}
        POSITION_INDEPENDENT_CODE ON
    )
    configure_dds_target(${TARGET_NAME}_static PUBLIC)

    add_library(${TARGET_NAME}_shared SHARED ${DDS_CORE_SOURCES})
    set_target_properties(${TARGET_NAME}_shared PROPERTIES
        OUTPUT_NAME ${PROJECT_NAME}
        VERSION ${PROJECT_VERSION}
        SOVERSION 1
        POSITION_INDEPENDENT_CODE ON
    )
    configure_dds_target(${TARGET_NAME}_shared PUBLIC)
else()
    set(APP_SOURCES src/main.cpp)
    if(MB_DDF_APP_MODE STREQUAL "ECHO")
        list(APPEND APP_SOURCES src/MB_DDF_HW_Test/ComEchoRunner.cpp)
    elseif(MB_DDF_APP_MODE STREQUAL "HW_TEST")
        list(APPEND APP_SOURCES ${HW_TEST_SOURCES})
    elseif(MB_DDF_APP_MODE STREQUAL "DEMO")
        list(APPEND APP_SOURCES ${DDS_CORE_SOURCES} ${DDS_DEMO_SOURCES})
    endif()
    add_executable(${TARGET_NAME} ${APP_SOURCES})
    configure_dds_target(${TARGET_NAME} PRIVATE)
    if(MB_DDF_APP_MODE STREQUAL "ECHO")
        target_compile_definitions(${TARGET_NAME} PRIVATE MB_DDF_APP_ECHO=1)
    elseif(MB_DDF_APP_MODE STREQUAL "HW_TEST")
        target_compile_definitions(${TARGET_NAME} PRIVATE MB_DDF_APP_HW_TEST=1)
        target_include_directories(${TARGET_NAME} PRIVATE "${PRODUCT_PROTOCOL_GENERATED_DIR}")
        add_dependencies(${TARGET_NAME} generate_product_protocol)
    elseif(MB_DDF_APP_MODE STREQUAL "DEMO")
        target_compile_definitions(${TARGET_NAME} PRIVATE MB_DDF_APP_DEMO=1)
    endif()
endif()

if(BUILD_HARDWARE_LAYER)
    add_subdirectory(src/MB_DDF_HW)

    # 仅可执行程序需要运行硬件示例；库构建继续只导出 DDS 核心。
    if(NOT BUILD_LIBRARY)
        target_link_libraries(${TARGET_NAME} PRIVATE MB_DDF_HW)
        if(MB_DDF_APP_MODE STREQUAL "DEMO")
            target_compile_definitions(${TARGET_NAME} PRIVATE MB_DDF_DEMO_WITH_HARDWARE=1)
        endif()
        if(MB_DDF_APP_MODE STREQUAL "DEMO" AND BUILD_HW_DDS_ADAPTER)
            target_compile_definitions(${TARGET_NAME} PRIVATE MB_DDF_DEMO_WITH_HW_DDS_ADAPTER=1)
            target_link_libraries(${TARGET_NAME} PRIVATE MB_DDF_HW_DDS_Adapter)
        endif()
    endif()
endif()

if(ENABLE_TESTS)
    include(FetchContent)

    set(FETCHCONTENT_BASE_DIR "${CMAKE_SOURCE_DIR}/.deps" CACHE PATH "FetchContent base directory" FORCE)
    set(FETCHCONTENT_QUIET OFF)

    if(DEFINED ENV{GTEST_GIT_MIRROR})
        set(GTEST_GIT_REPO $ENV{GTEST_GIT_MIRROR})
    else()
        set(GTEST_GIT_REPO "https://gitee.com/mirrors/googletest.git")
    endif()

    FetchContent_Declare(
        googletest
        GIT_REPOSITORY ${GTEST_GIT_REPO}
        GIT_TAG v1.15.2
        GIT_SHALLOW TRUE
        GIT_PROGRESS TRUE
        TIMEOUT 300
    )

    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(BUILD_GMOCK ON CACHE BOOL "" FORCE)

    FetchContent_MakeAvailable(googletest)
    add_subdirectory(tests)
endif()

if(BUILD_LIBRARY)
    install(TARGETS ${TARGET_NAME}_static ${TARGET_NAME}_shared
        EXPORT ${PROJECT_NAME}Targets
        ARCHIVE DESTINATION libs
        LIBRARY DESTINATION libs
    )

    foreach(_header ${DDS_PUBLIC_HEADERS})
        file(RELATIVE_PATH _rel_path "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/${_header}")
        get_filename_component(_dir "${_rel_path}" DIRECTORY)
        install(FILES "${_header}" DESTINATION "include/${_dir}")
    endforeach()

    install(EXPORT ${PROJECT_NAME}Targets
        FILE ${PROJECT_NAME}Targets.cmake
        NAMESPACE ${PROJECT_NAME}::
        DESTINATION cmake
    )
endif()
