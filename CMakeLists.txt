cmake_minimum_required(VERSION 3.16)

project(testProgram LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_AUTOMOC ON)

find_package(Qt5 5.15 QUIET COMPONENTS Core)
if (Qt5_FOUND)
    set(HAL_QT_CORE_TARGET Qt5::Core)
else()
    find_package(Qt6 REQUIRED COMPONENTS Core)
    set(HAL_QT_CORE_TARGET Qt6::Core)
endif()

add_subdirectory(src/hal)
