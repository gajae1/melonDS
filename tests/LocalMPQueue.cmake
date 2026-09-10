# Include from the tests project; standalone worker drivers may include this too.
add_executable(LocalMPQueue
    "${CMAKE_CURRENT_LIST_DIR}/LocalMPQueue.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PlatformSync.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/../src/net/LocalMP.cpp")
target_include_directories(LocalMPQueue PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
target_compile_features(LocalMPQueue PRIVATE cxx_std_26)
find_package(Threads REQUIRED)
target_link_libraries(LocalMPQueue PRIVATE Threads::Threads)
foreach(case packet-wrap reply-wrap packet-exact-wrap reply-exact-wrap roundtrip lifecycle new-host-session packet-bounds reply-bounds invalid-input)
    add_test(NAME localmp-${case} COMMAND LocalMPQueue ${case})
    set_tests_properties(localmp-${case} PROPERTIES TIMEOUT 10)
endforeach()

add_executable(LocalMPInterleaving
    "${CMAKE_CURRENT_LIST_DIR}/LocalMPInterleaving.cpp"
    "${CMAKE_CURRENT_LIST_DIR}/PlatformSync.cpp")
target_include_directories(LocalMPInterleaving PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../src")
target_compile_features(LocalMPInterleaving PRIVATE cxx_std_26)
target_link_libraries(LocalMPInterleaving PRIVATE Threads::Threads)
foreach(fifo packet reply)
    add_test(NAME localmp-in-flight-${fifo}-reset COMMAND LocalMPInterleaving ${fifo})
    set_tests_properties(localmp-in-flight-${fifo}-reset PROPERTIES TIMEOUT 10)
endforeach()
