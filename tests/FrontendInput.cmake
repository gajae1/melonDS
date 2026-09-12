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
target_compile_definitions(FrontendInput PRIVATE MELONDS_TEST_FILE_EXISTS)
add_test(NAME qt-keyboard-mapping-input COMMAND FrontendInput)
set_tests_properties(qt-keyboard-mapping-input PROPERTIES
    TIMEOUT 30 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")

set(input_dialog_methods)
foreach(pair IN ITEMS "inputLoadConfig|void EmuInstance::inputLoadConfig()"
        "joystickDeInit|void EmuInstance::inputDeInit()"
        "joystickSaveConfig|void EmuInstance::saveJoystickConfig()"
        "inputButtonNames|const char* EmuInstance::buttonNames[12] ="
        "inputHotkeyNames|const char* EmuInstance::hotkeyNames[HK_MAX] =")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 method)
    list(GET parts 1 signature)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstanceInput.cpp" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstanceInput.cpp VERBATIM)
    list(APPEND input_dialog_methods "${output}")
endforeach()
add_executable(InputConfigUI "${CMAKE_SOURCE_DIR}/tests/InputConfigUI.cpp"
    "${CMAKE_SOURCE_DIR}/tests/PlatformSync.cpp"
    Config.cpp KeyboardInput.cpp InputConfig/InputConfigDialog.h InputConfig/InputConfigDialog.ui
    InputConfig/MapButton.h InputConfig/KeyMapButton.h InputConfig/resources/ds.qrc
    ${input_dialog_methods} ${input_methods})
target_include_directories(InputConfigUI PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/net"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/.."
    "${CMAKE_CURRENT_SOURCE_DIR}/InputConfig" "${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(InputConfigUI PROPERTIES AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}/InputConfig")
target_compile_definitions(InputConfigUI PRIVATE MELONDS_TEST_FILE_EXISTS)
target_link_libraries(InputConfigUI PRIVATE ${QT_LINK_LIBS} PkgConfig::SDL2 Threads::Threads)
if (USE_QT6)
    find_package(Qt6 REQUIRED COMPONENTS Test)
    target_link_libraries(InputConfigUI PRIVATE Qt6::Test)
else()
    find_package(Qt5 REQUIRED COMPONENTS Test)
    target_link_libraries(InputConfigUI PRIVATE Qt5::Test)
endif()
foreach(case IN ITEMS letter controller space return tab hotkey escape unbind cancel missing-selection
        selection-reorder selection-migration selection-draft selection-ambiguous selection-stale
        selection-no-device selection-malformed selection-shutdown save-retry save-continue)
    add_test(NAME input-dialog-${case} COMMAND InputConfigUI ${case})
    set_tests_properties(input-dialog-${case} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()

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
        "joystickRestore|void EmuInstance::setJoystickSelection(const JoystickSelection& selection)"
        "joystickGetSelection|JoystickSelection EmuInstance::getJoystickSelection()"
        "joystickOpen|void EmuInstance::openJoystick()"
        "joystickClose|void EmuInstance::closeJoystick()"
        "joystickButton|bool EmuInstance::joystickButtonDown(int val)"
        "joystickProcess|void EmuInstance::inputProcess()"
        "joystickRumbleStart|void EmuInstance::inputRumbleStart(melonDS::u32 len_ms)"
        "joystickMotion|float EmuInstance::inputMotionQuery(melonDS::Platform::MotionQueryType type)"
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
target_sources(InputConfigUI PRIVATE ${joystick_methods})
target_include_directories(FrontendJoystick PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(FrontendJoystick PRIVATE PkgConfig::SDL2 Threads::Threads)
foreach(case IN ITEMS controls transition capabilities detach open-failure close reorder ambiguous serial-reconnect duplicate-serial)
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
        "${CMAKE_CURRENT_SOURCE_DIR}/ROMPreparation.cpp"
        "u32 ROMPreparation::Decompress(const u8* inContent, const u32 inSize, unique_ptr<u8[]>& outContent, std::stop_token stop)"
        "${rom_decompressor}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" ROMPreparation.cpp VERBATIM)
add_executable(ROMDecompression "${CMAKE_SOURCE_DIR}/tests/ROMDecompression.cpp" "${rom_decompressor}")
target_include_directories(ROMDecompression PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_include_directories(ROMDecompression PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ROMDecompression PRIVATE PkgConfig::Zstd $<IF:$<BOOL:${USE_QT6}>,Qt6::Core,Qt5::Core>)
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

set(rom_reader "${CMAKE_CURRENT_BINARY_DIR}/romRead.inc")
add_custom_command(OUTPUT "${rom_reader}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/ROMPreparation.cpp"
        "bool ROMPreparation::Read(const QStringList& filepath, std::unique_ptr<u8[]>& filedata, u32& filelen, string& basepath, string& romname, std::stop_token stop) noexcept"
        "${rom_reader}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" ROMPreparation.cpp VERBATIM)
set(rom_window_methods)
foreach(pair IN ITEMS
        "Cancel|void MainWindow::cancelROMPreparation()"
        "CancelAll|void MainWindow::cancelROMPreparations()"
        "Close|bool MainWindow::deferROMClose()"
        "Progress|void MainWindow::showROMProgress()"
        "Start|void MainWindow::startROMPreparation(QStringList files, ROMAction action, bool rememberFolder)"
        "Finish|void MainWindow::finishROMPreparation(const ROMPreparation::Result& result)"
        "Pick|void MainWindow::pickFileFromArchive(const ROMPreparation::Result& result)"
        "Split|QStringList MainWindow::splitArchivePath(const QString& filename, bool useMemberSyntax)"
        "Preload|bool MainWindow::preloadROMs(QStringList file, QStringList gbafile, bool boot)"
        "Open|void MainWindow::onOpenFile()"
        "Insert|void MainWindow::onInsertCart()"
        "Recent|void MainWindow::onClickRecentFile()")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 method)
    list(GET parts 1 signature)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/romWindow${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/Window.cpp" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" Window.cpp VERBATIM)
    list(APPEND rom_window_methods "${output}")
endforeach()
add_executable(ROMPreparationUI "${CMAKE_SOURCE_DIR}/tests/ROMPreparationUI.cpp" ROMPreparation.h ${rom_window_methods})
set_property(TARGET ROMPreparationUI PROPERTY AUTOGEN_TARGET_DEPENDS ${rom_window_methods})
target_include_directories(ROMPreparationUI PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(ROMPreparationUI PRIVATE PkgConfig::LibArchive PkgConfig::Zstd ${QT_LINK_LIBS})
foreach(case IN ITEMS cancel-read cancel-extract cancel-list cancel-decode cancel-member
        reselect reselect-member late-completion close close-destruction os-blocked-close modal-close modal-reselect
        success-read success-extract success-member success-zstd read-failure apply-failure
        gba-save-route gba-save-reselect gba-save-cancel ds-save-route ds-save-reselect ds-save-cancel)
    add_test(NAME rom-preparation-${case} COMMAND ROMPreparationUI ${case})
    # Keep headless timing independent of native Windows dialog styling costs.
    set_tests_properties(rom-preparation-${case} PROPERTIES TIMEOUT 15
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QT_STYLE_OVERRIDE=Fusion")
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
foreach(pair IN ITEMS
        "VerifyDSBIOS|QString EmuInstance::verifyDSBIOS()"
        "VerifyDSiBIOS|QString EmuInstance::verifyDSiBIOS()"
        "VerifyDSFirmware|QString EmuInstance::verifyDSFirmware()"
        "VerifyDSiFirmware|QString EmuInstance::verifyDSiFirmware()"
        "LoadARM9BIOS|std::unique_ptr<ARM9BIOSImage> EmuInstance::loadARM9BIOS() noexcept"
        "LoadARM7BIOS|std::unique_ptr<ARM7BIOSImage> EmuInstance::loadARM7BIOS() noexcept"
        "LoadDSiARM9BIOS|std::unique_ptr<DSiBIOSImage> EmuInstance::loadDSiARM9BIOS() noexcept"
        "LoadDSiARM7BIOS|std::unique_ptr<DSiBIOSImage> EmuInstance::loadDSiARM7BIOS() noexcept")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 method)
    list(GET parts 1 signature)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/file${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstance.cpp" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstance.cpp VERBATIM)
    list(APPEND file_methods "${output}")
endforeach()
add_executable(FrontendFileIO "${CMAKE_SOURCE_DIR}/tests/FrontendFileIO.cpp" ArchiveUtil.cpp
    "${CMAKE_SOURCE_DIR}/tests/PlatformSync.cpp" "${CMAKE_SOURCE_DIR}/tests/PlatformHeadless.cpp"
    ${file_methods} "${rom_decompressor}" "${rom_reader}")
target_include_directories(FrontendFileIO PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(FrontendFileIO PRIVATE core PkgConfig::LibArchive PkgConfig::Zstd Threads::Threads)
if (USE_QT6)
    target_link_libraries(FrontendFileIO PRIVATE Qt6::Core)
else()
    target_link_libraries(FrontendFileIO PRIVATE Qt5::Core)
endif()
foreach(case IN ITEMS rom-normal rom-relative rom-zstd rom-zstd-invalid rom-empty rom-missing
        rom-short rom-error rom-oversize rom-wrapped rom-allocation rom-archive rom-archive-relative rom-archive-missing
        rtc-roundtrip rtc-truncated rtc-oversize rtc-short rtc-error rtc-missing rtc-write rtc-commit
        bios-boundaries firmware-verify)
    add_test(NAME frontend-file-${case} COMMAND FrontendFileIO ${case})
    set_tests_properties(frontend-file-${case} PROPERTIES TIMEOUT 15)
endforeach()

set(audio_callback "${CMAKE_CURRENT_BINARY_DIR}/audioCallback.inc")
add_custom_command(OUTPUT "${audio_callback}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstanceAudio.cpp"
        "void EmuInstance::audioCallback(void* data, Uint8* stream, int len)" "${audio_callback}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstanceAudio.cpp VERBATIM)
set(audio_sync "${CMAKE_CURRENT_BINARY_DIR}/audioSync.inc")
add_custom_command(OUTPUT "${audio_sync}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstanceAudio.cpp"
        "void EmuInstance::audioSync(int frameSamples, std::stop_token stopToken)" "${audio_sync}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstanceAudio.cpp VERBATIM)
add_executable(FrontendAudio "${CMAKE_SOURCE_DIR}/tests/FrontendAudio.cpp" "${audio_callback}" "${audio_sync}")
target_include_directories(FrontendAudio PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(FrontendAudio PRIVATE PkgConfig::SDL2 Threads::Threads)
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
        relocation-pending allocation-capture allocation-publish)
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
target_sources(SavestateLoad PRIVATE "${audio_callback}")
foreach(pair IN ITEMS "stateAudioEnable|audioEnable" "stateAudioDisable|audioDisable"
        "stateAudioReport|audioReportDiagnostics" "stateAudioReset|audioResetOutput")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 output_name)
    list(GET parts 1 method)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${output_name}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstanceAudio.cpp"
            "void EmuInstance::${method}()" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuInstanceAudio.cpp VERBATIM)
    target_sources(SavestateLoad PRIVATE "${output}")
endforeach()
target_link_libraries(SavestateLoad PRIVATE PkgConfig::SDL2)
melonds_configure_audio_kernels(SavestateLoad)
foreach(case IN ITEMS audio-success audio-rebase audio-rollback audio-preflight)
    add_test(NAME savestate-load-${case} COMMAND SavestateLoad ${case} interpreter)
    set_tests_properties(savestate-load-${case} PROPERTIES TIMEOUT 30)
endforeach()
if (USE_QT6)
    target_link_libraries(SavestateLoad PRIVATE Qt6::Core)
else()
    target_link_libraries(SavestateLoad PRIVATE Qt5::Core)
endif()
foreach(case IN ITEMS success late-section short-section missing-global empty-global load-error load-oom backup-error header short-read read-error oversize undo-error rollback-error
        cart-match cart-checksum cart-type cart-missing cart-unexpected)
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

foreach(pair IN ITEMS "statePrepareGL|bool EmuThread::prepareGL()"
        "stateReportGL|void EmuThread::reportGLFailure(int win)"
        "stateClearGL|void EmuThread::clearGLFailure(int win)")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 name)
    list(GET parts 1 signature)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${name}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/EmuThread.cpp" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuThread.cpp VERBATIM)
    target_sources(StateLoadMessages PRIVATE "${output}")
endforeach()

add_executable(VideoSettingsUI "${CMAKE_SOURCE_DIR}/tests/VideoSettingsUI.cpp"
    "${CMAKE_SOURCE_DIR}/tests/PlatformSync.cpp" "${CMAKE_SOURCE_DIR}/tests/PlatformHeadless.cpp"
    Config.cpp VideoSettingsDialog.h VideoSettingsDialog.ui)
target_include_directories(VideoSettingsUI PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_SOURCE_DIR}/src/net"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/.." "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(VideoSettingsUI PRIVATE MELONDS_TEST_FILE_EXISTS)
set_target_properties(VideoSettingsUI PROPERTIES AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(VideoSettingsUI PRIVATE core ${QT_LINK_LIBS} PkgConfig::SDL2 Threads::Threads)
add_test(NAME video-settings-dialog-recovery COMMAND VideoSettingsUI)
set_tests_properties(video-settings-dialog-recovery PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
target_link_libraries(StateLoadMessages PRIVATE core Threads::Threads)
if (USE_QT6)
    target_link_libraries(StateLoadMessages PRIVATE Qt6::Core)
else()
    target_link_libraries(StateLoadMessages PRIVATE Qt5::Core)
endif()
add_test(NAME savestate-message-recovery COMMAND StateLoadMessages)
add_test(NAME direct-boot-message-failure COMMAND StateLoadMessages boot-failure)
add_test(NAME gl-state-message-gate COMMAND StateLoadMessages gl-gate)
set_tests_properties(gl-state-message-gate PROPERTIES TIMEOUT 10)
add_test(NAME ds-save-request-consumer COMMAND StateLoadMessages ds-save-request)
set_tests_properties(ds-save-request-consumer PROPERTIES TIMEOUT 10)
set_tests_properties(savestate-message-recovery PROPERTIES TIMEOUT 10)
foreach(case IN ITEMS normal missing empty short error oversize allocation reset-failure paused-failure no-cart)
    add_test(NAME save-import-${case} COMMAND StateLoadMessages ${case})
    set_tests_properties(save-import-${case} PROPERTIES TIMEOUT 10)
endforeach()

add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/closeEvent.inc"
        "${CMAKE_CURRENT_BINARY_DIR}/prepareClose.inc" "${CMAKE_CURRENT_BINARY_DIR}/closeSaveManagers.inc"
        "${CMAKE_CURRENT_BINARY_DIR}/closeAppState.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/Window.cpp" "void MainWindow::onAppStateChanged(Qt::ApplicationState state)"
        "${CMAKE_CURRENT_BINARY_DIR}/closeAppState.inc"
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
    "${CMAKE_CURRENT_BINARY_DIR}/prepareClose.inc" "${CMAKE_CURRENT_BINARY_DIR}/closeSaveManagers.inc"
    "${CMAKE_CURRENT_BINARY_DIR}/closeAppState.inc")
# moc reads these includes while parsing the fixture's Q_OBJECT class. CMake
# does not recognize .inc files as C++ sources, so order their generation first.
set_property(TARGET FrontendClose PROPERTY AUTOGEN_TARGET_DEPENDS
    "${CMAKE_CURRENT_BINARY_DIR}/closeEvent.inc"
    "${CMAKE_CURRENT_BINARY_DIR}/prepareClose.inc"
    "${CMAKE_CURRENT_BINARY_DIR}/closeAppState.inc"
    "${CMAKE_CURRENT_BINARY_DIR}/closeSaveManagers.inc")
target_include_directories(FrontendClose PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_link_libraries(FrontendClose PRIVATE ${QT_LINK_LIBS})
foreach(case IN ITEMS cancel-ds cancel-gba cancel-firmware child-cancel clean secondary retry recovery recovery-cancel recovery-failure app-state
        capture capture-retry capture-recovery)
    add_test(NAME frontend-close-${case} COMMAND FrontendClose ${case})
    set_tests_properties(frontend-close-${case} PROPERTIES TIMEOUT 10 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()

set(cart_methods)
foreach(method IN ITEMS BuildPath RetryCapture FlushSave FlushAll AssetPath SaveError ReadSave LoadROM LoadGBA UpdateConsole Reset)
    if (method STREQUAL "AssetPath")
        set(signature "string EmuInstance::getAssetPath(bool gba, const string& configpath, const string& ext, const string& file = \"\")")
    elseif (method STREQUAL "BuildPath")
        set(signature "static string AssetPath(const string& directory, const string& name, const string& ext)")
    elseif (method STREQUAL "RetryCapture")
        set(signature "void EmuInstance::retrySaveCapture()")
    elseif (method STREQUAL "FlushSave")
        set(signature "static bool FlushSave(EmuInstance* instance, SaveManager* save, QString& errorstr)")
    elseif (method STREQUAL "FlushAll")
        set(signature "bool EmuInstance::flushSaveData(QString& errorstr)")
    elseif (method STREQUAL "ReadSave")
        set(signature "bool EmuInstance::loadSaveRAM(string path, string original, bool gba, unique_ptr<u8[]>& data, u32& length, QString& errorstr)")
    elseif (method STREQUAL "SaveError")
        set(signature "QString EmuInstance::getSavErrorString(std::string& filepath, bool gba)")
    elseif (method STREQUAL "LoadROM")
        set(signature "bool EmuInstance::loadROM(QStringList filepath, bool reset, QString& errorstr, const AssetIdentity::Selection& assets, const std::shared_ptr<ROMPreparation::Data>& prepared, std::optional<melonDS::u32> dsSaveType)")
    elseif (method STREQUAL "LoadGBA")
        set(signature "bool EmuInstance::loadGBAROM(QStringList filepath, QString& errorstr, const AssetIdentity::Selection& assets, const std::shared_ptr<ROMPreparation::Data>& prepared, u32 initialSaveLength)")
    elseif (method STREQUAL "Reset")
        set(signature "bool EmuInstance::reset(const AssetIdentity::Selection& dsAssets, const AssetIdentity::Selection& gbaAssets)")
    else()
        set(signature "bool EmuInstance::updateConsole(bool directBoot) noexcept")
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
        ds-asset-path gba-asset-path asset-reset asset-reset-failure invalid-sd
        ds-prepared-success gba-prepared-success ds-prepared-cancel gba-prepared-cancel
        ds-prepared-queued-cancel gba-prepared-queued-cancel ds-prepared-failure gba-prepared-failure ds-prepared-queued-failure
        capture-ds capture-gba capture-generated-firmware capture-raw-firmware
        gba-initial-roundtrip gba-initial-existing gba-initial-rejects gba-initial-prepared
        ds-capacity-unknown ds-capacity-metadata ds-capacity-unsupported ds-capacity-state ds-capacity-flash
        ds-manual-roundtrip ds-manual-existing ds-manual-rejects)
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
target_compile_definitions(FirmwareProfile PRIVATE MELONDS_TEST_FILE_EXISTS)
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
        "assetPrepareUI|bool EmuThread::prepareAssets(const QStringList& source, bool gba, bool allowExisting, AssetIdentity::Selection& selection, QString& error, std::stop_token stop)"
        "assetDSSaveUI|bool EmuThread::chooseDSSaveType(CartLoadRequest& request, QString& errorstr)"
        "assetBootUI|int EmuThread::bootROM(const QStringList& filename, QString& errorstr, const std::shared_ptr<ROMPreparation::Data>& prepared, bool chooseDSSave)"
        "assetInsertUI|int EmuThread::insertCart(const QStringList& filename, bool gba, QString& errorstr, const std::shared_ptr<ROMPreparation::Data>& prepared, bool chooseGBASave, bool chooseDSSave)"
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
foreach(case IN ITEMS cancel existing separate other reset worker-reset prepared-modal-cancel
        gba-initial-choices gba-initial-cancel ds-save-choices ds-save-cancel)
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

set(cheat_file_methods)
foreach(pair IN ITEMS "FileReadLine|bool FileReadLine(char* str, int count, FileHandle* file)"
        "FileWrite|u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)"
        "FileWriteFormatted|u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)"
        "FileFlush|bool FileFlush(FileHandle* file)"
        "FileExists|bool FileExists(const std::string& name)")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 method)
    list(GET parts 1 signature)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/cheat${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/Platform.cpp" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" Platform.cpp VERBATIM)
    list(APPEND cheat_file_methods "${output}")
endforeach()
set(cheat_finished "${CMAKE_CURRENT_BINARY_DIR}/cheatFinishedCurrent.inc")
set(cheat_enable "${CMAKE_CURRENT_BINARY_DIR}/cheatEnableCurrent.inc")
add_custom_command(OUTPUT "${cheat_finished}" "${cheat_enable}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/Window.cpp" "void MainWindow::onCheatsDialogFinished(int res)" "${cheat_finished}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuInstance.cpp" "void EmuInstance::enableCheats(bool enable)" "${cheat_enable}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" Window.cpp EmuInstance.cpp VERBATIM)
add_executable(CheatSaveUI "${CMAKE_SOURCE_DIR}/tests/CheatSaveUI.cpp"
    CheatsDialog.h CheatsDialog.ui CheatImportDialog.cpp CheatImportDialog.h CheatImportDialog.ui
    "${CMAKE_SOURCE_DIR}/src/ARCodeFile.cpp" "${CMAKE_SOURCE_DIR}/src/ARDatabaseDAT.cpp" "${CMAKE_SOURCE_DIR}/src/CRC32.cpp"
    ${ar_file_methods} ${cheat_file_methods} "${cheat_finished}" "${cheat_enable}")
target_include_directories(CheatSaveUI PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}")
set_target_properties(CheatSaveUI PROPERTIES AUTOUIC_SEARCH_PATHS "${CMAKE_CURRENT_SOURCE_DIR}")
if (USE_QT6)
    target_link_libraries(CheatSaveUI PRIVATE Qt6::Widgets)
else()
    target_link_libraries(CheatSaveUI PRIVATE Qt5::Widgets)
endif()
foreach(case IN ITEMS normal disabled open short commit retry discard changed invalid one-per-group)
    add_test(NAME cheat-save-ui-${case} COMMAND CheatSaveUI ${case})
    set_tests_properties(cheat-save-ui-${case} PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endforeach()

add_executable(ARCodeFileIO "${CMAKE_SOURCE_DIR}/tests/ARCodeFileIO.cpp" "${CMAKE_SOURCE_DIR}/src/ARCodeFile.cpp"
    ${ar_file_methods} ${cheat_file_methods})
target_include_directories(ARCodeFileIO PRIVATE "${CMAKE_SOURCE_DIR}/src" "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_definitions(ARCodeFileIO PRIVATE ARCODE_SERIALIZE_TESTS)
if (USE_QT6)
    target_link_libraries(ARCodeFileIO PRIVATE Qt6::Core)
else()
    target_link_libraries(ARCodeFileIO PRIVATE Qt5::Core)
endif()
foreach(case IN ITEMS controls preflight nested save-short save-error save-flush save-close
        load-unreadable load-zero load-error load-stalled load-malformed load-length serialize)
    add_test(NAME ar-code-file-${case} COMMAND ARCodeFileIO ${case})
    set_tests_properties(ar-code-file-${case} PROPERTIES TIMEOUT 15)
endforeach()


# Borrow ownership handshake: exact production case, acknowledgement, wait tail,
# and public request/return methods. Other dispatcher cases are not linked here.
set(gl_borrow_reduce [=[
from pathlib import Path
import sys
text = Path(sys.argv[1]).read_text(encoding="utf-8")
head = "        switch (msg.type)\n        {\n"
dequeue = "        Message msg = msgQueue.dequeue();\n"
case = "        case msg_BorrowGL:\n"
tail = "\n        }\n\n        msgSemaphore.release();"
for anchor in (head, dequeue, case, tail):
    if text.count(anchor) != 1:
        raise SystemExit("GL borrow extraction needs updating: " + repr(anchor))
body = text.index(head) + len(head)
start = text.index(case, body)
end = text.index("\n        case ", start + len(case))
close = text.index(tail, end)
# BorrowGL is a control message; the state-consumer GL preflight does not run
# for it. Keep the queue/ownership/acknowledgement path without that gate.
prefix_end = text.index(dequeue) + len(dequeue)
if prefix_end > text.index(head):
    raise SystemExit("GL borrow extraction: dequeue must precede dispatch")
reduced = text[:prefix_end] + head + text[start:end] + text[close:]
if reduced.count("{") != reduced.count("}"):
    raise SystemExit("GL borrow extraction: unbalanced braces")
Path(sys.argv[2]).write_text(reduced, encoding="utf-8")
header = Path(sys.argv[3]).read_text(encoding="utf-8")
anchor = "    QWaitCondition glBorrowCond;\n"
if header.count(anchor) != 1:
    raise SystemExit("GL borrow state extraction needs updating")
start = header.index(anchor)
end = header.index("\nsignals:", start)
Path(sys.argv[4]).write_text(header[start:end] + "\n", encoding="utf-8")
]=])
set(gl_borrow_reduce_script "${CMAKE_CURRENT_BINARY_DIR}/glBorrowReduce.py")
file(GENERATE OUTPUT "${gl_borrow_reduce_script}" CONTENT "${gl_borrow_reduce}")
set(gl_borrow_raw "${CMAKE_CURRENT_BINARY_DIR}/glBorrowFullHandler.inc")
set(gl_borrow_handler "${CMAKE_CURRENT_BINARY_DIR}/glBorrowHandler.inc")
set(gl_borrow_state "${CMAKE_CURRENT_BINARY_DIR}/glBorrowState.inc")
set(gl_borrow_request "${CMAKE_CURRENT_BINARY_DIR}/glBorrowRequest.inc")
set(gl_borrow_return "${CMAKE_CURRENT_BINARY_DIR}/glBorrowReturn.inc")
add_custom_command(
    OUTPUT "${gl_borrow_raw}" "${gl_borrow_handler}" "${gl_borrow_state}"
        "${gl_borrow_request}" "${gl_borrow_return}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuThread.cpp" "void EmuThread::handleMessages()" "${gl_borrow_raw}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuThread.cpp" "bool EmuThread::borrowGL()" "${gl_borrow_request}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuThread.cpp" "void EmuThread::returnGL()" "${gl_borrow_return}"
    COMMAND "${Python3_EXECUTABLE}" "${gl_borrow_reduce_script}"
        "${gl_borrow_raw}" "${gl_borrow_handler}"
        "${CMAKE_CURRENT_SOURCE_DIR}/EmuThread.h" "${gl_borrow_state}"
    DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" "${gl_borrow_reduce_script}"
        EmuThread.cpp EmuThread.h VERBATIM)
add_executable(GLBorrow "${CMAKE_SOURCE_DIR}/tests/GLBorrow.cpp"
    "${gl_borrow_raw}" "${gl_borrow_handler}" "${gl_borrow_state}"
    "${gl_borrow_request}" "${gl_borrow_return}")
target_include_directories(GLBorrow PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if (USE_QT6)
    target_link_libraries(GLBorrow PRIVATE Qt6::Core)
else()
    target_link_libraries(GLBorrow PRIVATE Qt5::Core)
endif()
foreach(case IN ITEMS early waiting)
    add_test(NAME gl-borrow-${case} COMMAND GLBorrow ${case})
    set_tests_properties(gl-borrow-${case} PROPERTIES TIMEOUT 20)
endforeach()

if (MELONDS_TEST_GPU)
    set(presentation_methods)
    foreach(pair IN ITEMS "Init|initOpenGL" "Deinit|deinitOpenGL" "Draw|drawScreen")
        string(REPLACE "|" ";" parts "${pair}")
        list(GET parts 0 name)
        list(GET parts 1 method)
        set(output "${CMAKE_CURRENT_BINARY_DIR}/presentation${name}.inc")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/Screen.cpp" "bool ScreenPanelGL::${method}()" "${output}"
            DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" Screen.cpp VERBATIM)
        list(APPEND presentation_methods "${output}")
    endforeach()
    add_executable(GLPresentation "${CMAKE_SOURCE_DIR}/tests/GLPresentation.cpp"
        "${CMAKE_SOURCE_DIR}/tests/PlatformSync.cpp" "${CMAKE_SOURCE_DIR}/tests/PlatformHeadless.cpp"
        ../glad/glad.c ${presentation_methods})
    set(presentation_osd "${CMAKE_CURRENT_BINARY_DIR}/presentationOSD.inc")
    add_custom_command(OUTPUT "${presentation_osd}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/Screen.cpp" "void ScreenPanel::osdUpdate()" "${presentation_osd}.raw"
        COMMAND "${Python3_EXECUTABLE}" -c
            "from pathlib import Path; p=Path(r'${presentation_osd}'); p.write_text(Path(str(p)+'.raw').read_text().replace('ScreenPanel::osdUpdate', 'ScreenPanelGL::osdUpdate'))"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" Screen.cpp VERBATIM)
    target_sources(GLPresentation PRIVATE "${presentation_osd}")
    set(presentation_handler_script "${CMAKE_CURRENT_BINARY_DIR}/presentationHandler.py")
    file(GENERATE OUTPUT "${presentation_handler_script}" CONTENT [=[
from pathlib import Path
import sys
s = Path(sys.argv[1]).read_text(encoding="utf-8")
a, b = "        case msg_DeInitGL:\n", "        case msg_BorrowGL:\n"
assert s.count(a) == s.count(b) == 1
body = s.split(a)[1].split(b)[0]
assert body.count("{") == body.count("}")
Path(sys.argv[2]).write_text(body, encoding="utf-8")
]=])
    set(presentation_handler "${CMAKE_CURRENT_BINARY_DIR}/presentationDeinitHandler.inc")
    set(presentation_renderer "${CMAKE_CURRENT_BINARY_DIR}/presentationRenderer.inc")
    add_custom_command(OUTPUT "${presentation_handler}" "${presentation_renderer}"
        COMMAND "${Python3_EXECUTABLE}" "${presentation_handler_script}"
            "${CMAKE_CURRENT_SOURCE_DIR}/EmuThread.cpp" "${presentation_handler}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/EmuThread.cpp" "void EmuThread::updateRenderer()" "${presentation_renderer}"
        DEPENDS "${presentation_handler_script}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" EmuThread.cpp VERBATIM)
    target_sources(GLPresentation PRIVATE "${presentation_handler}" "${presentation_renderer}")
    target_include_directories(GLPresentation PRIVATE "${CMAKE_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/src/frontend" "${CMAKE_CURRENT_BINARY_DIR}")
    target_link_libraries(GLPresentation PRIVATE core ${QT_LINK_LIBS} PkgConfig::SDL2 Threads::Threads)
    if (WIN32)
        target_sources(GLPresentation PRIVATE ../graphics/gl/context.cpp ../graphics/gl/context_wgl.cpp ../glad/glad_wgl.c)
        target_link_libraries(GLPresentation PRIVATE opengl32)
        add_test(NAME gl-presentation-native-replace COMMAND GLPresentation native)
        set_tests_properties(gl-presentation-native-replace PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77
            RUN_SERIAL TRUE ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
    add_test(NAME gl-presentation-deinit COMMAND GLPresentation)
    set_tests_properties(gl-presentation-deinit PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77
        RUN_SERIAL TRUE ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    foreach(mode IN ITEMS fail-screen fail-osd fail-current osd-reinit runtime-current runtime-swap retire-current)
        add_test(NAME gl-presentation-${mode} COMMAND GLPresentation ${mode})
        set_tests_properties(gl-presentation-${mode} PROPERTIES TIMEOUT 20 SKIP_RETURN_CODE 77
            RUN_SERIAL TRUE ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
    foreach(renderer IN ITEMS opengl compute)
        add_test(NAME gl-presentation-retire-${renderer} COMMAND GLPresentation ${renderer})
        set_tests_properties(gl-presentation-retire-${renderer} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77
            RUN_SERIAL TRUE ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endforeach()
endif()


# GL/WGL loader boundary through current GUI creation methods, with real Qt loans.
set(gl_loader_methods)
foreach(pair IN ITEMS
        "glLoaderCreateWindow|EmuInstance.cpp|void EmuInstance::createWindow(int id)"
        "glLoaderBroadcast|main.cpp|void broadcastInstanceCommand(int cmd, QVariant& param, int sourceinst)"
        "glLoaderCreatePanel|Window.cpp|void MainWindow::createScreenPanel()"
        "glLoaderCreateContext|Screen.cpp|bool ScreenPanelGL::createContext()")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 method)
    list(GET parts 1 source)
    list(GET parts 2 signature)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/${source}" "${signature}" "${output}"
        DEPENDS "${CMAKE_SOURCE_DIR}/tests/ExtractFunction.py" "${source}" VERBATIM)
    list(APPEND gl_loader_methods "${output}")
endforeach()
set(gl_loader_guard_extract [=[
from pathlib import Path
import re, sys
source = Path(sys.argv[1]).read_text(encoding="utf-8")
header = Path(sys.argv[2]).read_text(encoding="utf-8")
registry = re.findall(r"const int kMaxEmuInstances = \d+;\nEmuInstance\* emuInstances\[kMaxEmuInstances\];", source)
if len(registry) != 1:
    raise SystemExit("GL loader registry extraction needs updating")
result = registry[0] + "\n"
anchor = "class ScopedGLWorkers\n{\n"
if anchor in header:
    if header.count(anchor) != 1:
        raise SystemExit("Duplicate GL worker scope declaration")
    start = header.index(anchor)
    end = header.index("\n};", start) + 3
    result += header[start:end] + "\n"
    state = "static unsigned glWorkerScopeDepth = 0;\nstatic std::vector<EmuThread*> glWorkersHeld;"
    if source.count(state) != 1:
        raise SystemExit("GL worker scope state extraction needs updating")
    result += state + "\n"
    for signature in ("ScopedGLWorkers::ScopedGLWorkers(EmuThread* extra, bool allInstances)", "ScopedGLWorkers::~ScopedGLWorkers()"):
        needle = signature + "\n{\n"
        if source.count(needle) != 1:
            raise SystemExit("GL worker scope definition missing: " + signature)
        start = source.index(needle)
        end = source.index("\n}", start) + 2
        body = source[start:end]
        if body.count("{") != body.count("}"):
            raise SystemExit("GL worker scope definition has unbalanced braces")
        result += body + "\n"
elif "ScopedGLWorkers::" in source:
    raise SystemExit("GL worker scope source/header mismatch")
Path(sys.argv[3]).write_text(result, encoding="utf-8")
]=])
set(gl_loader_guard_script "${CMAKE_CURRENT_BINARY_DIR}/glLoaderGuardExtract.py")
file(GENERATE OUTPUT "${gl_loader_guard_script}" CONTENT "${gl_loader_guard_extract}")
set(gl_loader_guard "${CMAKE_CURRENT_BINARY_DIR}/glLoaderGuard.inc")
add_custom_command(OUTPUT "${gl_loader_guard}"
    COMMAND "${Python3_EXECUTABLE}" "${gl_loader_guard_script}"
        "${CMAKE_CURRENT_SOURCE_DIR}/main.cpp" "${CMAKE_CURRENT_SOURCE_DIR}/main.h" "${gl_loader_guard}"
    DEPENDS "${gl_loader_guard_script}" main.cpp main.h VERBATIM)
add_executable(GLLoaderBoundary "${CMAKE_SOURCE_DIR}/tests/GLLoaderBoundary.cpp"
    ${gl_loader_methods} "${gl_loader_guard}" "${gl_borrow_handler}" "${gl_borrow_state}"
    "${gl_borrow_request}" "${gl_borrow_return}")
target_include_directories(GLLoaderBoundary PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
if (USE_QT6)
    target_link_libraries(GLLoaderBoundary PRIVATE Qt6::Core)
else()
    target_link_libraries(GLLoaderBoundary PRIVATE Qt5::Core)
endif()
foreach(case IN ITEMS root replace shared unregistered failure idle broadcast release-failure release-nested
        release-created-root release-created-shared)
    add_test(NAME gl-loader-${case} COMMAND GLLoaderBoundary ${case})
    set_tests_properties(gl-loader-${case} PROPERTIES TIMEOUT 20)
endforeach()
