cmake_minimum_required(VERSION 3.16)

project(testProgram LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

if (MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
    add_compile_definitions($<$<CONFIG:Debug>:_ITERATOR_DEBUG_LEVEL=0>)
endif()

include(CTest)

find_package(Qt5 5.15 QUIET COMPONENTS Core)
if (Qt5_FOUND)
    set(HWTEST_QT_CORE_TARGET Qt5::Core)
else()
    find_package(Qt6 REQUIRED COMPONENTS Core)
    set(HWTEST_QT_CORE_TARGET Qt6::Core)
endif()

# Compatibility variable for the independently buildable HAL module.
set(HAL_QT_CORE_TARGET ${HWTEST_QT_CORE_TARGET})

add_subdirectory(src/hal)
add_subdirectory(src/logging)
add_subdirectory(src/biz)

if (BUILD_TESTING)
    add_subdirectory(tests)
endif()
