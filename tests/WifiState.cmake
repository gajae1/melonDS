# Include after the real core target; shared test registration belongs to the parent.
if (TARGET core)
    find_package(Threads REQUIRED)
    add_executable(WifiState "${CMAKE_CURRENT_LIST_DIR}/WifiState.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/PlatformSync.cpp")
    target_compile_features(WifiState PRIVATE cxx_std_26)
    target_link_libraries(WifiState PRIVATE core Threads::Threads)
    foreach(case IN ITEMS connect-refusal connected-refusal connect-payload scan-refusal)
        add_test(NAME wifi-state-${case} COMMAND WifiState ${case})
        set_tests_properties(wifi-state-${case} PROPERTIES TIMEOUT 15)
    endforeach()
endif()
