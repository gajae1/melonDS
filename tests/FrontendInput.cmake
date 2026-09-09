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

set(rom_decompressor "${CMAKE_CURRENT_BINARY_DIR}/decompressROM.inc")
add_custom_command(OUTPUT "${rom_decompressor}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstance.cpp"
        "u32 EmuInstance::decompressROM(const u8* inContent, const u32 inSize, unique_ptr<u8[]>& outContent)"
        "${rom_decompressor}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstance.cpp VERBATIM)
add_executable(ROMDecompression "${CMAKE_SOURCE_DIR}/tests/ROMDecompression.cpp" "${rom_decompressor}")
target_include_directories(ROMDecompression PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(ROMDecompression PRIVATE PkgConfig::Zstd)
foreach(case IN ITEMS lengths completion framing trailing empty size-limit allocation)
    add_test(NAME rom-zstd-decompression-${case} COMMAND ROMDecompression ${case})
    set_tests_properties(rom-zstd-decompression-${case} PROPERTIES TIMEOUT 30)
endforeach()

set(audio_callback "${CMAKE_CURRENT_BINARY_DIR}/audioCallback.inc")
add_custom_command(OUTPUT "${audio_callback}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstanceAudio.cpp"
        "void EmuInstance::audioCallback(void* data, Uint8* stream, int len)" "${audio_callback}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstanceAudio.cpp VERBATIM)
add_executable(FrontendAudio "${CMAKE_SOURCE_DIR}/tests/FrontendAudio.cpp" "${audio_callback}")
target_include_directories(FrontendAudio PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(FrontendAudio PRIVATE PkgConfig::SDL2)
melonds_configure_audio_kernels(FrontendAudio)
add_test(NAME audio-callback-buffer COMMAND FrontendAudio)
set_tests_properties(audio-callback-buffer PROPERTIES TIMEOUT 30)

add_executable(SaveManagerIO "${CMAKE_SOURCE_DIR}/tests/SaveManagerIO.cpp" SaveManager.cpp SaveManager.h)
target_include_directories(SaveManagerIO PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
if (USE_QT6)
    target_link_libraries(SaveManagerIO PRIVATE Qt6::Core)
else()
    target_link_libraries(SaveManagerIO PRIVATE Qt5::Core)
endif()
foreach(case IN ITEMS replace retry-open retry-rename retry-worker path-during-flush reload-partial buffer-resize)
    add_test(NAME save-manager-${case} COMMAND SaveManagerIO ${case})
    set_tests_properties(save-manager-${case} PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)
endforeach()

set(state_writer "${CMAKE_CURRENT_BINARY_DIR}/saveState.inc")
add_custom_command(OUTPUT "${state_writer}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstance.cpp"
        "bool EmuInstance::saveState(const std::string& filename)" "${state_writer}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstance.cpp VERBATIM)
foreach(method IN ITEMS loadState undoStateLoad applyState)
    if (method STREQUAL "loadState")
        set(state_signature "StateLoadResult EmuInstance::loadState(const std::string& filename)")
    elseif (method STREQUAL "applyState")
        set(state_signature "StateLoadResult EmuInstance::applyState(Savestate& state, bool undo)")
    else()
        set(state_signature "StateLoadResult EmuInstance::undoStateLoad()")
    endif()
    add_custom_command(
        OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${method}.inc"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstance.cpp" "${state_signature}"
            "${CMAKE_CURRENT_BINARY_DIR}/${method}.inc"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstance.cpp VERBATIM)
endforeach()
find_package(Threads REQUIRED)
add_executable(SavestateLoad "${CMAKE_SOURCE_DIR}/tests/SavestateLoad.cpp"
    "${CMAKE_SOURCE_DIR}/tests/PlatformSync.cpp" "${CMAKE_SOURCE_DIR}/tests/PlatformHeadless.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/loadState.inc" "${CMAKE_CURRENT_BINARY_DIR}/undoStateLoad.inc"
    "${CMAKE_CURRENT_BINARY_DIR}/applyState.inc")
target_include_directories(SavestateLoad PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(SavestateLoad PRIVATE core Threads::Threads)
if (USE_QT6)
    target_link_libraries(SavestateLoad PRIVATE Qt6::Core)
else()
    target_link_libraries(SavestateLoad PRIVATE Qt5::Core)
endif()
foreach(case IN ITEMS success late-section load-error load-oom backup-error header short-read read-error oversize undo-error rollback-error)
    add_test(NAME savestate-load-${case} COMMAND SavestateLoad ${case} interpreter)
    set_tests_properties(savestate-load-${case} PROPERTIES TIMEOUT 30)
endforeach()
if (ENABLE_JIT)
    foreach(mode IN ITEMS jit fastmem)
        add_test(NAME savestate-rollback-${mode} COMMAND SavestateLoad late-section ${mode})
        set_tests_properties(savestate-rollback-${mode} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    endforeach()
endif()

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/stateMessages.inc" "${CMAKE_CURRENT_BINARY_DIR}/stateThreadConstructor.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuThread.cpp" "void EmuThread::handleMessages()"
        "${CMAKE_CURRENT_BINARY_DIR}/stateMessages.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuThread.cpp" "EmuThread::EmuThread(EmuInstance* inst, QObject* parent) : QThread(parent)"
        "${CMAKE_CURRENT_BINARY_DIR}/stateThreadConstructor.inc"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuThread.cpp VERBATIM)
add_executable(StateLoadMessages "${CMAKE_SOURCE_DIR}/tests/StateLoadMessages.cpp" EmuThread.h
    "${CMAKE_SOURCE_DIR}/tests/PlatformSync.cpp" "${CMAKE_SOURCE_DIR}/tests/PlatformHeadless.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/stateMessages.inc" "${CMAKE_CURRENT_BINARY_DIR}/stateThreadConstructor.inc")
target_include_directories(StateLoadMessages PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(StateLoadMessages PRIVATE core Threads::Threads)
if (USE_QT6)
    target_link_libraries(StateLoadMessages PRIVATE Qt6::Core)
else()
    target_link_libraries(StateLoadMessages PRIVATE Qt5::Core)
endif()
add_test(NAME savestate-message-recovery COMMAND StateLoadMessages)
set_tests_properties(savestate-message-recovery PROPERTIES TIMEOUT 10)

add_executable(SavestateFileIO "${CMAKE_SOURCE_DIR}/tests/SavestateFileIO.cpp"
    "${CMAKE_SOURCE_DIR}/src/Savestate.cpp" "${state_writer}")
target_include_directories(SavestateFileIO PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}")
if (USE_QT6)
    target_link_libraries(SavestateFileIO PRIVATE Qt6::Core)
else()
    target_link_libraries(SavestateFileIO PRIVATE Qt5::Core)
endif()
foreach(case IN ITEMS roundtrip serialize-error serialize-reject short-write write-error commit-failure)
    add_test(NAME savestate-file-${case} COMMAND SavestateFileIO ${case})
    set_tests_properties(savestate-file-${case} PROPERTIES TIMEOUT 10 SKIP_RETURN_CODE 77)
endforeach()

set(firmware_override "${CMAKE_CURRENT_BINARY_DIR}/customizeFirmware.inc")
set(firmware_mac "${CMAKE_CURRENT_BINARY_DIR}/parseMacAddress.inc")
add_custom_command(OUTPUT "${firmware_override}" "${firmware_mac}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstance.cpp"
        "void EmuInstance::customizeFirmware(Firmware& firmware, bool overridesettings) noexcept"
        "${firmware_override}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstance.cpp"
        "bool EmuInstance::parseMacAddress(void* data)" "${firmware_mac}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstance.cpp VERBATIM)
add_executable(FirmwareProfile "${CMAKE_SOURCE_DIR}/tests/FirmwareProfile.cpp"
    "${CMAKE_SOURCE_DIR}/tests/PlatformSync.cpp"
    "${CMAKE_SOURCE_DIR}/tests/PlatformHeadless.cpp" Config.cpp "${firmware_override}" "${firmware_mac}")
target_include_directories(FirmwareProfile PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_SOURCE_DIR}/src/net" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(FirmwareProfile PRIVATE core ${QT_LINK_LIBS} Threads::Threads)
add_test(NAME firmware-profile-direct-boot COMMAND FirmwareProfile)
set_tests_properties(firmware-profile-direct-boot PROPERTIES TIMEOUT 30)
