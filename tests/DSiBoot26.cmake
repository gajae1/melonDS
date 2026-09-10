# Include after the real core target. No frontend or private inputs required.
if (TARGET core)
    find_package(Threads REQUIRED)
    add_executable(DSiBoot26
        "${CMAKE_CURRENT_LIST_DIR}/DSiBootModcrypt.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/DSiBootMetadata.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/PlatformSync.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/PlatformHeadless.cpp")
    target_compile_definitions(DSiBoot26 PRIVATE MELONDS_TEST_MEMORY_FILES)
    target_link_libraries(DSiBoot26 PRIVATE core Threads::Threads)
    foreach(case IN ITEMS modcrypt metadata-exact metadata-failures metadata-caller core-controls)
        add_test(NAME dsi-boot26-${case} COMMAND DSiBoot26 ${case})
        set_tests_properties(dsi-boot26-${case} PROPERTIES TIMEOUT 30)
    endforeach()
endif()

option(MELONDS_TEST_DSI_BOOT_FRONTEND "Build focused DSi boot frontend regression" OFF)
if (TARGET core AND (TARGET Qt6::Core OR MELONDS_TEST_DSI_BOOT_FRONTEND))
    find_package(Qt6 REQUIRED COMPONENTS Core)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(dsi_boot_frontend "${CMAKE_CURRENT_LIST_DIR}/../src/frontend/qt_sdl")
    set(dsi_boot_generated "${CMAKE_CURRENT_BINARY_DIR}/dsi-boot26-frontend")
    file(MAKE_DIRECTORY "${dsi_boot_generated}" "${dsi_boot_generated}/tmp")
    set(dsi_boot_methods)
    foreach(method IN ITEMS BuildPath FlushSave FlushAll AssetPath SaveError ReadSave LoadROM LoadGBA UpdateConsole Reset BootToMenu)
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
        elseif (method STREQUAL "BootToMenu")
            set(signature "bool EmuInstance::bootToMenu(QString& errorstr)")
        else()
            set(signature "bool EmuInstance::updateConsole(bool directBoot) noexcept")
        endif()
        set(output "${dsi_boot_generated}/cart${method}.inc")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/ExtractFunction.py"
                "${dsi_boot_frontend}/EmuInstance.cpp" "${signature}" "${output}"
            DEPENDS "${CMAKE_CURRENT_LIST_DIR}/ExtractFunction.py" "${dsi_boot_frontend}/EmuInstance.cpp" VERBATIM)
        list(APPEND dsi_boot_methods "${output}")
    endforeach()
    add_executable(DSiBootFrontend
        "${dsi_boot_frontend}/SaveManager.h"
        "${CMAKE_CURRENT_LIST_DIR}/DSiBootFrontend.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/DSiBootMetadata.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/PlatformSync.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/PlatformHeadless.cpp" ${dsi_boot_methods})
    target_include_directories(DSiBootFrontend PRIVATE "${dsi_boot_frontend}" "${dsi_boot_generated}")
    set_target_properties(DSiBootFrontend PROPERTIES AUTOMOC ON)
    target_compile_definitions(DSiBootFrontend PRIVATE MELONDS_TEST_CART_SAVE MELONDS_TEST_MEMORY_FILES)
    target_link_libraries(DSiBootFrontend PRIVATE core Qt6::Core Threads::Threads)
    foreach(case IN ITEMS retain late success firmware legacy-ds-reset-success legacy-ds-queued-failure legacy-asset-reset-failure)
        add_test(NAME dsi-boot26-frontend-${case} COMMAND DSiBootFrontend ${case})
        set_tests_properties(dsi-boot26-frontend-${case} PROPERTIES TIMEOUT 30
            ENVIRONMENT "TMP=${dsi_boot_generated}/tmp;TEMP=${dsi_boot_generated}/tmp")
    endforeach()
endif()
