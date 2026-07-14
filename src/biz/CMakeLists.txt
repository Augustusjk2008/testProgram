set(BIZ_PUBLIC_HEADERS
    include/biz/biz_global.h
    include/biz/biz_types.h
    include/biz/i_algorithm_executor.h
    include/biz/i_test_run_service.h
    include/biz/i_report_generator.h
    include/biz/biz_factory.h
    include/biz/test_config_manager.h
    include/biz/test_plan_builder.h
)

set(BIZ_SOURCES
    src/biz_types.cpp
    src/test_config_manager.cpp
    src/test_plan_builder.cpp
    src/report_generator.cpp
    src/test_run_service.cpp
    src/biz_factory.cpp
)

if(NOT TARGET hwtest_log_types)
    message(FATAL_ERROR "hwtest_biz requires the hwtest_log_types target")
endif()

if(NOT DEFINED HWTEST_QT_CORE_TARGET OR NOT TARGET "${HWTEST_QT_CORE_TARGET}")
    message(FATAL_ERROR "hwtest_biz requires HWTEST_QT_CORE_TARGET to name a Qt Core target")
endif()

add_library(hwtest_biz STATIC
    ${BIZ_PUBLIC_HEADERS}
    ${BIZ_SOURCES}
)

set_target_properties(hwtest_biz PROPERTIES
    OUTPUT_NAME hwtest_biz
    POSITION_INDEPENDENT_CODE ON
    AUTOMOC ON
)

target_include_directories(hwtest_biz
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hwtest_biz
    PUBLIC
        hwtest_log_types
        ${HWTEST_QT_CORE_TARGET}
)

target_compile_features(hwtest_biz PUBLIC cxx_std_17)

target_compile_definitions(hwtest_biz
    PUBLIC
        HWTEST_BIZ_STATIC
)

if(MSVC)
    target_compile_options(hwtest_biz PUBLIC /utf-8)
endif()
