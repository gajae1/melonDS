# Compile the real mapping widget and configuration code; use the existing
# extraction helper to exercise input handlers without starting emulation threads.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(input_methods)
foreach(method IN ITEMS onKeyPress onKeyRelease keyReleaseAll)
    if (method STREQUAL "keyReleaseAll")
        set(signature "void EmuInstance::${method}()")
    else()
        set(signature "void EmuInstance::${method}(QKeyEvent* event)")
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstanceInput.cpp" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstanceInput.cpp VERBATIM)
    list(APPEND input_methods "${output}")
endforeach()
set(focus_method "${CMAKE_CURRENT_BINARY_DIR}/onFocusOut.inc")
add_custom_command(OUTPUT "${focus_method}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/Window.cpp" "void MainWindow::onFocusOut()" "${focus_method}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" Window.cpp VERBATIM)
add_executable(FrontendInput
    "${CMAKE_SOURCE_DIR}/tests/FrontendInput.cpp"
    "${CMAKE_SOURCE_DIR}/tests/PlatformSync.cpp"
    "${CMAKE_SOURCE_DIR}/tests/PlatformHeadless.cpp"
    Config.cpp KeyboardInput.cpp InputConfig/KeyMapButton.h
    ${input_methods} "${focus_method}")
target_include_directories(FrontendInput PRIVATE
    "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/net" "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(FrontendInput PRIVATE ${QT_LINK_LIBS} PkgConfig::SDL2 Threads::Threads)
add_test(NAME qt-keyboard-mapping-input COMMAND FrontendInput)
set_tests_properties(qt-keyboard-mapping-input PROPERTIES
    TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
