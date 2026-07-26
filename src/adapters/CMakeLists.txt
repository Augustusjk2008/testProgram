if(HWTEST_ENABLE_NI_DAQMX)
    add_subdirectory(ni_daqmx)
endif()

if(BUILD_TESTING)
    add_subdirectory(ni_daqmx/tests)
endif()
