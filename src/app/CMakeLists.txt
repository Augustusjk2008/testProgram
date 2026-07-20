set(APP_CORE_PUBLIC_HEADERS
    include/app/test_application_controller.h
    include/app/frontend_launch_options.h
)

set(APP_CORE_SOURCES
    src/test_application_controller.cpp
    src/frontend_launch_options.cpp
)

add_library(hwtest_app_core STATIC
    ${APP_CORE_PUBLIC_HEADERS}
    ${APP_CORE_SOURCES}
)

target_include_directories(hwtest_app_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hwtest_app_core
    PUBLIC
        ${HWTEST_QT_CORE_TARGET}
    PRIVATE
        hwtest_algorithm_mbddf
        hwtest_biz
        hwtest_hal
        hwtest_log
)

target_compile_features(hwtest_app_core PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(hwtest_app_core PRIVATE /utf-8)
endif()

add_library(hwtest_tui_support STATIC
    include/app/tui_shell.h
    src/tui_shell.cpp
)

target_include_directories(hwtest_tui_support
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hwtest_tui_support
    PUBLIC
        hwtest_app_core
        ${HWTEST_QT_CORE_TARGET}
)

target_compile_features(hwtest_tui_support PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(hwtest_tui_support PRIVATE /utf-8)
endif()

add_library(hwtest_gui_support STATIC
    gui/gui_main_window.h
    gui/gui_main_window.cpp
)

target_include_directories(hwtest_gui_support
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/gui
)

target_link_libraries(hwtest_gui_support
    PUBLIC
        hwtest_app_core
        ${HWTEST_QT_WIDGETS_TARGET}
)

target_compile_features(hwtest_gui_support PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(hwtest_gui_support PRIVATE /utf-8)
endif()

add_executable(hwtest_pc_runner
    main.cpp
)

target_link_libraries(hwtest_pc_runner
    PRIVATE
        hwtest_app_core
        ${HWTEST_QT_CORE_TARGET}
)

target_compile_features(hwtest_pc_runner PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(hwtest_pc_runner PRIVATE /utf-8)
endif()

add_executable(hwtest_tui
    tui_main.cpp
)

target_link_libraries(hwtest_tui
    PRIVATE
        hwtest_tui_support
)

target_compile_features(hwtest_tui PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(hwtest_tui PRIVATE /utf-8)
endif()

add_executable(hwtest_gui WIN32
    gui/gui_main.cpp
)

target_link_libraries(hwtest_gui
    PRIVATE
        hwtest_gui_support
)

target_compile_features(hwtest_gui PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(hwtest_gui PRIVATE /utf-8)
endif()
