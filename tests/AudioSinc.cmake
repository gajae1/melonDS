if(TARGET core)
    add_executable(AudioClockCorrection AudioClockCorrection.cpp PlatformSync.cpp PlatformHeadless.cpp)
    target_include_directories(AudioClockCorrection PRIVATE "${CMAKE_SOURCE_DIR}/src/frontend/qt_sdl")
    target_link_libraries(AudioClockCorrection PRIVATE core Threads::Threads)
    add_test(NAME audio-clock-correction COMMAND AudioClockCorrection)
    # Exercises 14 long virtual-clock trajectories with the real SPU/Sinc path.
    set_tests_properties(audio-clock-correction PROPERTIES TIMEOUT 600)

    add_executable(AudioSinc AudioSinc.cpp PlatformSync.cpp PlatformHeadless.cpp)
    target_link_libraries(AudioSinc PRIVATE core Threads::Threads)
    add_test(NAME audio-sinc-clock-correction COMMAND AudioSinc --clock-correction)
    set_tests_properties(audio-sinc-clock-correction PROPERTIES TIMEOUT 30)
    add_test(NAME audio-sinc-history COMMAND AudioSinc --history)
    set_tests_properties(audio-sinc-history PROPERTIES TIMEOUT 30)
    add_test(NAME audio-sinc-history-scalar COMMAND AudioSinc --history)
    set_tests_properties(audio-sinc-history-scalar PROPERTIES TIMEOUT 30
        ENVIRONMENT "MELONDS_INTERPOLATION_SCALAR=1")
    add_test(NAME audio-sinc-spectrum-capture COMMAND AudioSinc "${CMAKE_CURRENT_BINARY_DIR}/sinc-spectrum" 4 5 6)
    set_tests_properties(audio-sinc-spectrum-capture PROPERTIES TIMEOUT 180 FIXTURES_SETUP sinc-spectrum)
    add_test(NAME audio-sinc-spectrum COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_CURRENT_SOURCE_DIR}/AudioSincSpectrum.py" "${CMAKE_CURRENT_BINARY_DIR}/sinc-spectrum"
        --report "${CMAKE_CURRENT_BINARY_DIR}/sinc-spectrum.json")
    set_tests_properties(audio-sinc-spectrum PROPERTIES TIMEOUT 60 FIXTURES_REQUIRED sinc-spectrum)
endif()
