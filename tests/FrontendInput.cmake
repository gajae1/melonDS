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

set(touch_methods)
foreach(pair IN ITEMS "touchEvent|void ScreenPanel::touchEvent(QTouchEvent* event)"
        "releaseTouch|void ScreenPanel::releaseTouch()"
        "touchPanelEvent|bool ScreenPanel::event(QEvent* event)"
        "touchMousePress|void ScreenPanel::mousePressEvent(QMouseEvent* event)"
        "touchMouseRelease|void ScreenPanel::mouseReleaseEvent(QMouseEvent* event)"
        "touchMouseMove|void ScreenPanel::mouseMoveEvent(QMouseEvent* event)"
        "touchTablet|void ScreenPanel::tabletEvent(QTabletEvent* event)"
        "touchAppState|void MainWindow::onAppStateChanged(Qt::ApplicationState state)")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 method)
    list(GET parts 1 signature)
    if (method STREQUAL "touchAppState")
        set(source Window.cpp)
    else()
        set(source Screen.cpp)
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/${source}" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" "${source}" VERBATIM)
    list(APPEND touch_methods "${output}")
endforeach()
add_executable(FrontendTouch "${CMAKE_SOURCE_DIR}/tests/FrontendTouch.cpp"
    ../ScreenLayout.cpp ${touch_methods} "${focus_method}")
target_include_directories(FrontendTouch PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_BINARY_DIR}")
if (USE_QT6)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    target_link_libraries(FrontendTouch PRIVATE Qt6::Widgets Qt6::Test)
else()
    find_package(Qt5 REQUIRED COMPONENTS Test)
    target_link_libraries(FrontendTouch PRIVATE Qt5::Widgets Qt5::Test)
endif()
foreach(case IN ITEMS drag cancel end-paused focus app-inactive mouse tablet)
    add_test(NAME frontend-touch-${case} COMMAND FrontendTouch ${case})
    set_tests_properties(frontend-touch-${case} PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()

set(touch_publication_methods)
foreach(pair IN ITEMS "touchPublish|void EmuInstance::touchScreen(int x, int y)"
        "touchRelease|void EmuInstance::releaseScreen()"
        "touchRead|bool EmuInstance::inputGetTouch(u16& x, u16& y)")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 method)
    list(GET parts 1 signature)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstanceInput.cpp" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstanceInput.cpp VERBATIM)
    list(APPEND touch_publication_methods "${output}")
endforeach()
add_executable(TouchPublication "${CMAKE_SOURCE_DIR}/tests/TouchPublication.cpp" ${touch_publication_methods})
target_include_directories(TouchPublication PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(TouchPublication PRIVATE Threads::Threads)
add_test(NAME frontend-touch-publication COMMAND TouchPublication)
set_tests_properties(frontend-touch-publication PROPERTIES TIMEOUT 15)

set(joystick_methods)
foreach(pair IN ITEMS "joystickSet|void EmuInstance::setJoystick(int id)"
        "joystickOpen|void EmuInstance::openJoystick()"
        "joystickClose|void EmuInstance::closeJoystick()"
        "joystickButton|bool EmuInstance::joystickButtonDown(int val)"
        "joystickProcess|void EmuInstance::inputProcess()"
        "joystickRumbleStart|void EmuInstance::inputRumbleStart(melonDS::u32 len_ms)"
        "joystickRumbleStop|void EmuInstance::inputRumbleStop()")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 method)
    list(GET parts 1 signature)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstanceInput.cpp" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstanceInput.cpp VERBATIM)
    list(APPEND joystick_methods "${output}")
endforeach()
add_executable(FrontendJoystick "${CMAKE_SOURCE_DIR}/tests/FrontendJoystick.cpp" ${joystick_methods})
target_include_directories(FrontendJoystick PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(FrontendJoystick PRIVATE PkgConfig::SDL2 Threads::Threads)
foreach(case IN ITEMS controls transition capabilities detach open-failure close)
    add_test(NAME frontend-joystick-${case} COMMAND FrontendJoystick ${case})
    set_tests_properties(frontend-joystick-${case} PROPERTIES TIMEOUT 15 SKIP_RETURN_CODE 77)
endforeach()

set(mic_methods)
foreach(pair IN ITEMS "micOpen|void EmuInstance::micOpen()"
        "micGetNumSamplesIn|int EmuInstance::micGetNumSamplesIn(int inlen)"
        "micResample|void EmuInstance::micResample(s16* inbuf, int inlen)"
        "micReadInput|int EmuInstance::micReadInput(s16* data, int maxlength)"
        "micCallback|void EmuInstance::micCallback(void* data, Uint8* stream, int len)")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 method)
    list(GET parts 1 signature)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstanceAudio.cpp" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstanceAudio.cpp VERBATIM)
    list(APPEND mic_methods "${output}")
endforeach()
add_executable(FrontendMicrophone "${CMAKE_SOURCE_DIR}/tests/FrontendMicrophone.cpp" ${mic_methods})
target_include_directories(FrontendMicrophone PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(FrontendMicrophone PRIVATE PkgConfig::SDL2 Threads::Threads)
foreach(case IN ITEMS resample-control resample-window resample-guard resample-tiny producer-consumer reopen-preserve)
    add_test(NAME frontend-microphone-${case} COMMAND FrontendMicrophone ${case})
    set_tests_properties(frontend-microphone-${case} PROPERTIES TIMEOUT 15)
endforeach()

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

add_executable(ArchiveIO "${CMAKE_SOURCE_DIR}/tests/ArchiveIO.cpp")
target_include_directories(ArchiveIO PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ArchiveIO PRIVATE PkgConfig::LibArchive)
if (USE_QT6)
    target_link_libraries(ArchiveIO PRIVATE Qt6::Core)
else()
    target_link_libraries(ArchiveIO PRIVATE Qt5::Core)
endif()
foreach(case IN ITEMS formats locale missing empty corrupt-header truncated-data crc member-type
        chunked-read early-eof read-error limits invalid-name null-name allocation)
    add_test(NAME archive-io-${case} COMMAND ArchiveIO ${case})
    set_tests_properties(archive-io-${case} PROPERTIES TIMEOUT 15)
endforeach()

set(file_methods)
foreach(method IN ITEMS LastSep LoadROMData LoadRTC SaveRTC)
    if (method STREQUAL "LastSep")
        set(signature "int EmuInstance::lastSep(const std::string& path)")
    elseif (method STREQUAL "LoadROMData")
        set(signature "bool EmuInstance::loadROMData(const QStringList& filepath, std::unique_ptr<u8[]>& filedata, u32& filelen, string& basepath, string& romname) noexcept")
    elseif (method STREQUAL "LoadRTC")
        set(signature "void EmuInstance::loadRTCData()")
    else()
        set(signature "void EmuInstance::saveRTCData()")
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/file${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstance.cpp" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstance.cpp VERBATIM)
    list(APPEND file_methods "${output}")
endforeach()
add_executable(FrontendFileIO "${CMAKE_SOURCE_DIR}/tests/FrontendFileIO.cpp" ArchiveUtil.cpp
    "${CMAKE_SOURCE_DIR}/tests/PlatformSync.cpp" "${CMAKE_SOURCE_DIR}/tests/PlatformHeadless.cpp"
    ${file_methods} "${rom_decompressor}")
target_include_directories(FrontendFileIO PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(FrontendFileIO PRIVATE core PkgConfig::LibArchive PkgConfig::Zstd Threads::Threads)
if (USE_QT6)
    target_link_libraries(FrontendFileIO PRIVATE Qt6::Core)
else()
    target_link_libraries(FrontendFileIO PRIVATE Qt5::Core)
endif()
foreach(case IN ITEMS rom-normal rom-relative rom-zstd rom-zstd-invalid rom-empty rom-missing
        rom-short rom-error rom-oversize rom-wrapped rom-allocation rom-archive rom-archive-relative rom-archive-missing
        rtc-roundtrip rtc-truncated rtc-oversize rtc-short rtc-error rtc-missing rtc-write rtc-commit)
    add_test(NAME frontend-file-${case} COMMAND FrontendFileIO ${case})
    set_tests_properties(frontend-file-${case} PROPERTIES TIMEOUT 15)
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
foreach(case IN ITEMS replace retry-open retry-rename retry-worker path-during-flush buffer-resize
        unpublished-pending memory-copy-pending flush-latest recovery-copy same-copy-path copy-commit-failure
        relocation-pending)
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
        "${CMAKE_CURRENT_BINARY_DIR}/importSaveWrapper.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuThread.cpp" "void EmuThread::handleMessages()"
        "${CMAKE_CURRENT_BINARY_DIR}/stateMessages.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuThread.cpp" "EmuThread::EmuThread(EmuInstance* inst, QObject* parent) : QThread(parent)"
        "${CMAKE_CURRENT_BINARY_DIR}/stateThreadConstructor.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuThread.cpp" "int EmuThread::importSavefile(const QString& filename)"
        "${CMAKE_CURRENT_BINARY_DIR}/importSaveWrapper.inc"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuThread.cpp VERBATIM)
add_executable(StateLoadMessages "${CMAKE_SOURCE_DIR}/tests/StateLoadMessages.cpp" EmuThread.h
    "${CMAKE_SOURCE_DIR}/tests/PlatformSync.cpp" "${CMAKE_SOURCE_DIR}/tests/PlatformHeadless.cpp"
    "${CMAKE_CURRENT_BINARY_DIR}/stateMessages.inc" "${CMAKE_CURRENT_BINARY_DIR}/stateThreadConstructor.inc"
    "${CMAKE_CURRENT_BINARY_DIR}/importSaveWrapper.inc")
target_include_directories(StateLoadMessages PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(StateLoadMessages PRIVATE core Threads::Threads)
if (USE_QT6)
    target_link_libraries(StateLoadMessages PRIVATE Qt6::Core)
else()
    target_link_libraries(StateLoadMessages PRIVATE Qt5::Core)
endif()
add_test(NAME savestate-message-recovery COMMAND StateLoadMessages)
set_tests_properties(savestate-message-recovery PROPERTIES TIMEOUT 10)
foreach(case IN ITEMS normal missing empty short error oversize allocation reset-failure paused-failure no-cart)
    add_test(NAME save-import-${case} COMMAND StateLoadMessages ${case})
    set_tests_properties(save-import-${case} PROPERTIES TIMEOUT 10)
endforeach()

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/closeEvent.inc"
        "${CMAKE_CURRENT_BINARY_DIR}/prepareClose.inc" "${CMAKE_CURRENT_BINARY_DIR}/closeSaveManagers.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/Window.cpp" "void MainWindow::closeEvent(QCloseEvent* event)"
        "${CMAKE_CURRENT_BINARY_DIR}/closeEvent.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/Window.cpp" "bool MainWindow::prepareClose()"
        "${CMAKE_CURRENT_BINARY_DIR}/prepareClose.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/Window.cpp" "bool MainWindow::flushSaveManagers(EmuInstance* instance)"
        "${CMAKE_CURRENT_BINARY_DIR}/closeSaveManagers.inc"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" Window.cpp VERBATIM)
add_executable(FrontendClose "${CMAKE_SOURCE_DIR}/tests/FrontendClose.cpp" "${CMAKE_CURRENT_BINARY_DIR}/closeEvent.inc"
    "${CMAKE_CURRENT_BINARY_DIR}/prepareClose.inc" "${CMAKE_CURRENT_BINARY_DIR}/closeSaveManagers.inc")
# moc reads these includes while parsing the fixture's Q_OBJECT class. CMake
# does not recognize .inc files as C++ sources, so order their generation first.
set_property(TARGET FrontendClose PROPERTY AUTOGEN_TARGET_DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/closeEvent.inc"
    "${CMAKE_CURRENT_BINARY_DIR}/prepareClose.inc"
    "${CMAKE_CURRENT_BINARY_DIR}/closeSaveManagers.inc")
target_include_directories(FrontendClose PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(FrontendClose PRIVATE ${QT_LINK_LIBS})
foreach(case IN ITEMS cancel-ds cancel-gba cancel-firmware child-cancel clean secondary retry recovery recovery-cancel recovery-failure)
    add_test(NAME frontend-close-${case} COMMAND FrontendClose ${case})
    set_tests_properties(frontend-close-${case} PROPERTIES TIMEOUT 10 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()

set(cart_methods)
foreach(method IN ITEMS BuildPath FlushSave FlushAll AssetPath SaveError ReadSave LoadROM LoadGBA UpdateConsole Reset)
    if (method STREQUAL "AssetPath")
        set(signature "string EmuInstance::getAssetPath(bool gba, const string& configpath, const string& ext, const string& file = \"\")")
    elseif (method STREQUAL "BuildPath")
        set(signature "static string AssetPath(const string& directory, const string& name, const string& ext)")
    elseif (method STREQUAL "FlushSave")
        set(signature "static bool FlushSave(SaveManager* save, QString& errorstr)")
    elseif (method STREQUAL "FlushAll")
        set(signature "bool EmuInstance::flushSaveData(QString& errorstr)")
    elseif (method STREQUAL "ReadSave")
        set(signature "bool EmuInstance::loadSaveRAM(string path, string original, bool gba, unique_ptr<u8[]>& data, u32& length, QString& errorstr)")
    elseif (method STREQUAL "SaveError")
        set(signature "QString EmuInstance::getSavErrorString(std::string& filepath, bool gba)")
    elseif (method STREQUAL "LoadROM")
        set(signature "bool EmuInstance::loadROM(QStringList filepath, bool reset, QString& errorstr, const AssetIdentity::Selection& assets)")
    elseif (method STREQUAL "LoadGBA")
        set(signature "bool EmuInstance::loadGBAROM(QStringList filepath, QString& errorstr, const AssetIdentity::Selection& assets)")
    elseif (method STREQUAL "Reset")
        set(signature "bool EmuInstance::reset(const AssetIdentity::Selection& dsAssets, const AssetIdentity::Selection& gbaAssets)")
    else()
        set(signature "bool EmuInstance::updateConsole() noexcept")
    endif()
    set(output "${CMAKE_CURRENT_BINARY_DIR}/cart${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstance.cpp" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstance.cpp VERBATIM)
    list(APPEND cart_methods "${output}")
endforeach()
add_executable(CartReplacement "${CMAKE_SOURCE_DIR}/tests/CartReplacement.cpp" SaveManager.h
    "${CMAKE_SOURCE_DIR}/tests/PlatformSync.cpp" "${CMAKE_SOURCE_DIR}/tests/PlatformHeadless.cpp" ${cart_methods})
target_include_directories(CartReplacement PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(CartReplacement PRIVATE MELONDS_TEST_CART_SAVE)
target_link_libraries(CartReplacement PRIVATE core Threads::Threads)
if (USE_QT6)
    target_link_libraries(CartReplacement PRIVATE Qt6::Core)
else()
    target_link_libraries(CartReplacement PRIVATE Qt5::Core)
endif()
foreach(case IN ITEMS ds-invalid gba-invalid ds-writable gba-writable ds-existing-writable gba-existing-writable
        ds-console-failure console-retain ds-queued-failure ds-success ds-reset-success gba-success gba-queued
        ds-pending-failure gba-pending-failure ds-same-save read-short read-error read-oversize read-denied ds-import-partial
        ds-asset-path gba-asset-path asset-reset asset-reset-failure)
    add_test(NAME cart-replacement-${case} COMMAND CartReplacement ${case})
    set_tests_properties(cart-replacement-${case} PROPERTIES TIMEOUT 20)
endforeach()

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


add_executable(CheatImportUI "${CMAKE_SOURCE_DIR}/tests/CheatImportUI.cpp"
    CheatImportDialog.cpp CheatImportDialog.h CheatImportDialog.ui)
target_include_directories(CheatImportUI PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}" "${CMAKE_SOURCE_DIR}/src")
if (USE_QT6)
    target_link_libraries(CheatImportUI PRIVATE Qt6::Widgets)
else()
    target_link_libraries(CheatImportUI PRIVATE Qt5::Widgets)
endif()
add_test(NAME cheat-import-selection COMMAND CheatImportUI)
set_tests_properties(cheat-import-selection PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen" TIMEOUT 10)

add_executable(AssetIdentityTest "${CMAKE_SOURCE_DIR}/tests/AssetIdentity.cpp" AssetIdentity.cpp)
target_include_directories(AssetIdentityTest PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
if (USE_QT6)
    target_link_libraries(AssetIdentityTest PRIVATE Qt6::Core)
else()
    target_link_libraries(AssetIdentityTest PRIVATE Qt5::Core)
endif()
foreach(case IN ITEMS normal collision gba archive case-name legacy legacy-separate relocation metadata locked record-type registry-failure folders unchanged alias)
    add_test(NAME asset-identity-${case} COMMAND AssetIdentityTest ${case})
    set_tests_properties(asset-identity-${case} PROPERTIES TIMEOUT 15)
endforeach()

set(asset_ui_methods)
foreach(pair IN ITEMS
        "assetPrepareUI|bool EmuThread::prepareAssets(const QStringList& source, bool gba, bool allowExisting, AssetIdentity::Selection& selection, QString& error)"
        "assetBootUI|int EmuThread::bootROM(const QStringList& filename, QString& errorstr)"
        "assetInsertUI|int EmuThread::insertCart(const QStringList& filename, bool gba, QString& errorstr)"
        "assetResetUI|void EmuThread::emuReset()")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 method)
    list(GET parts 1 signature)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/EmuThread.cpp" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuThread.cpp VERBATIM)
    list(APPEND asset_ui_methods "${output}")
endforeach()
add_executable(AssetIdentityUI "${CMAKE_SOURCE_DIR}/tests/AssetIdentityUI.cpp" AssetIdentity.cpp EmuThread.h
    ${asset_ui_methods} "${CMAKE_CURRENT_BINARY_DIR}/stateThreadConstructor.inc")
target_include_directories(AssetIdentityUI PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
if (USE_QT6)
    target_link_libraries(AssetIdentityUI PRIVATE Qt6::Widgets)
else()
    target_link_libraries(AssetIdentityUI PRIVATE Qt5::Widgets)
endif()
foreach(case IN ITEMS cancel existing separate other reset worker-reset)
    add_test(NAME asset-ui-${case} COMMAND AssetIdentityUI ${case})
    set_tests_properties(asset-ui-${case} PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()

set(cheat_message_methods)
foreach(pair IN ITEMS "cheatSendMessage|void EmuThread::sendMessage(Message msg)"
        "cheatWaitMessage|void EmuThread::waitMessage(int num)"
        "cheatStopToken|std::stop_token EmuThread::cheatStopToken()")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 method)
    list(GET parts 1 signature)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/EmuThread.cpp" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuThread.cpp VERBATIM)
    list(APPEND cheat_message_methods "${output}")
endforeach()
add_executable(CheatCancellation "${CMAKE_SOURCE_DIR}/tests/CheatCancellation.cpp" EmuThread.h
    "${CMAKE_SOURCE_DIR}/tests/PlatformSync.cpp" "${CMAKE_SOURCE_DIR}/tests/PlatformHeadless.cpp"
    ${cheat_message_methods} "${CMAKE_CURRENT_BINARY_DIR}/stateThreadConstructor.inc")
target_include_directories(CheatCancellation PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(CheatCancellation PRIVATE core Threads::Threads)
if (USE_QT6)
    target_link_libraries(CheatCancellation PRIVATE Qt6::Core)
else()
    target_link_libraries(CheatCancellation PRIVATE Qt5::Core)
endif()
foreach(case IN ITEMS pause stop exit pending)
    add_test(NAME cheat-message-${case} COMMAND CheatCancellation ${case})
    set_tests_properties(cheat-message-${case} PROPERTIES TIMEOUT 15)
endforeach()

set(ar_file_methods)
foreach(pair IN ITEMS "CloseFile|bool CloseFile(FileHandle* file)"
        "IsEndOfFile|bool IsEndOfFile(FileHandle* file)"
        "FileSeek|bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)"
        "FilePosition|u64 FilePosition(FileHandle* file)"
        "FileRead|u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)"
        "FileLength|u64 FileLength(FileHandle* file)")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 method)
    list(GET parts 1 signature)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/ar${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/Platform.cpp" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" Platform.cpp VERBATIM)
    list(APPEND ar_file_methods "${output}")
endforeach()
add_executable(ARDatabaseInput "${CMAKE_SOURCE_DIR}/tests/ARDatabaseInput.cpp"
    "${CMAKE_SOURCE_DIR}/src/ARDatabaseDAT.cpp" ${ar_file_methods})
target_include_directories(ARDatabaseInput PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}")
if (USE_QT6)
    target_link_libraries(ARDatabaseInput PRIVATE Qt6::Core)
else()
    target_link_libraries(ARDatabaseInput PRIVATE Qt5::Core)
endif()
foreach(case IN ITEMS controls header-index strings category codes entry-span partial io-read io-seek no-progress parents)
    add_test(NAME ar-database-${case} COMMAND ARDatabaseInput ${case})
    set_tests_properties(ar-database-${case} PROPERTIES TIMEOUT 15)
endforeach()
