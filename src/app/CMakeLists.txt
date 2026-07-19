add_executable(hwtest_pc_runner
    main.cpp
)

target_link_libraries(hwtest_pc_runner
    PRIVATE
        hwtest_algorithm_mbddf
        hwtest_biz
        hwtest_hal
        hwtest_log
        ${HWTEST_QT_CORE_TARGET}
)

target_compile_features(hwtest_pc_runner PRIVATE cxx_std_17)

if(MSVC)
    target_compile_options(hwtest_pc_runner PRIVATE /utf-8)
endif()
