# Included by DSiSDTransfer.cmake. Reuse production sector methods in both the
# SD controller regression and Qt's real-file boundary checks.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(dsifat_root "${CMAKE_CURRENT_LIST_DIR}/..")
set(dsifat_outputs)
set(dsifat_includes "")
foreach(entry IN ITEMS
        "ReadSectors|u32 FATStorage::ReadSectors(u32 start, u32 num, u8* data) const"
        "WriteSectors|u32 FATStorage::WriteSectors(u32 start, u32 num, const u8* data)"
        "GetSectorCount|u64 FATStorage::GetSectorCount() const"
        "ReadSectorsInternal|u32 FATStorage::ReadSectorsInternal(FileHandle* file, u64 filelen, u32 start, u32 num, u8* data)"
        "WriteSectorsInternal|u32 FATStorage::WriteSectorsInternal(FileHandle* file, u64 filelen, u32 start, u32 num, const u8* data)")
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 method)
    list(GET parts 1 signature)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/DSiSDFAT${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${dsifat_root}/tests/ExtractFunction.py"
            "${dsifat_root}/src/FATStorage.cpp" "${signature}" "${output}"
        DEPENDS "${dsifat_root}/tests/ExtractFunction.py" "${dsifat_root}/src/FATStorage.cpp" VERBATIM)
    list(APPEND dsifat_outputs "${output}")
    string(APPEND dsifat_includes "#include \"DSiSDFAT${method}.inc\"\n")
endforeach()
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/DSiSDFATMethods.inc" "${dsifat_includes}")
add_custom_target(DSiSDFATMethods DEPENDS ${dsifat_outputs})
add_dependencies(DSiSDTransfer DSiSDFATMethods)

find_package(Qt6 QUIET COMPONENTS Core)
if (TARGET Qt6::Core)
    set(dsifat_platform_outputs)
    set(dsifat_platform_includes "")
    foreach(entry IN ITEMS
            "IsEndOfFile|bool IsEndOfFile(FileHandle* file)"
            "FileSeek|bool FileSeek(FileHandle* file, s64 offset, FileSeekOrigin origin)"
            "FileRead|u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)"
            "FileWrite|u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* file)"
            "FileReadLine|bool FileReadLine(char* str, int count, FileHandle* file)"
            "FileWriteFormatted|u64 FileWriteFormatted(FileHandle* file, const char* fmt, ...)"
            "FileLength|u64 FileLength(FileHandle* file)"
            "FileFlush|bool FileFlush(FileHandle* file)")
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 method)
        list(GET parts 1 signature)
        set(output "${CMAKE_CURRENT_BINARY_DIR}/DSiSDFATQt${method}.inc")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${Python3_EXECUTABLE}" "${dsifat_root}/tests/ExtractFunction.py"
                "${dsifat_root}/src/frontend/qt_sdl/Platform.cpp" "${signature}" "${output}"
            DEPENDS "${dsifat_root}/tests/ExtractFunction.py"
                "${dsifat_root}/src/frontend/qt_sdl/Platform.cpp" VERBATIM)
        list(APPEND dsifat_platform_outputs "${output}")
        string(APPEND dsifat_platform_includes "#include \"DSiSDFATQt${method}.inc\"\n")
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/DSiSDFATQtMethods.inc" "${dsifat_platform_includes}")
    add_executable(DSiSDFATBacking "${CMAKE_CURRENT_LIST_DIR}/DSiSDFATBacking.cpp" ${dsifat_platform_outputs})
    add_dependencies(DSiSDFATBacking DSiSDFATMethods)
    target_include_directories(DSiSDFATBacking PRIVATE "${dsifat_root}/src" "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_features(DSiSDFATBacking PRIVATE cxx_std_26)
    target_link_libraries(DSiSDFATBacking PRIVATE Qt6::Core)
    foreach(case IN ITEMS seek-read seek-write normal sparse read-error write-error)
        add_test(NAME dsi-sd-fat-${case} COMMAND DSiSDFATBacking ${case})
        set_tests_properties(dsi-sd-fat-${case} PROPERTIES TIMEOUT 10)
    endforeach()

    # Current Qt atomic writer, shared by FAT and NAND export regressions.
    set(storage_export_atomic "${CMAKE_CURRENT_BINARY_DIR}/StorageExportAtomic.inc")
    add_custom_command(OUTPUT "${storage_export_atomic}"
        COMMAND "${Python3_EXECUTABLE}" "${dsifat_root}/tests/ExtractFunction.py"
            "${dsifat_root}/src/frontend/qt_sdl/Platform.cpp"
            "bool WriteFileAtomically(const std::string& path, const std::function<bool(const FileWriteCallback&)>& write, bool local)"
            "${storage_export_atomic}"
        DEPENDS "${dsifat_root}/tests/ExtractFunction.py"
            "${dsifat_root}/src/frontend/qt_sdl/Platform.cpp" VERBATIM)
    # The test includes full FATStorage.cpp; seams inject only storage failures.
    add_executable(FATStorageLifecycle "${CMAKE_CURRENT_LIST_DIR}/FATStorageLifecycle.cpp"
        "${dsifat_root}/src/sha1/sha1.c" "${dsifat_root}/src/FATIO.cpp"
        "${dsifat_root}/src/fatfs/ff.c" "${dsifat_root}/src/fatfs/ffsystem.c"
        "${dsifat_root}/src/fatfs/ffunicode.c" ${dsifat_platform_outputs} "${storage_export_atomic}")
    target_include_directories(FATStorageLifecycle PRIVATE "${dsifat_root}/src" "${CMAKE_CURRENT_BINARY_DIR}")
    target_compile_features(FATStorageLifecycle PRIVATE cxx_std_26)
    target_link_libraries(FATStorageLifecycle PRIVATE Qt6::Core)
    if (TARGET FrontendClose)
        # Reuse the real FAT/Qt I/O fixture in the existing close-window tests.
        target_sources(FrontendClose PRIVATE
            "${dsifat_root}/src/FATIO.cpp" "${dsifat_root}/src/sha1/sha1.c"
            "${dsifat_root}/src/fatfs/ff.c" "${dsifat_root}/src/fatfs/ffsystem.c"
            "${dsifat_root}/src/fatfs/ffunicode.c")
        target_include_directories(FrontendClose PRIVATE "${dsifat_root}/src" "${CMAKE_CURRENT_BINARY_DIR}")
        add_dependencies(FrontendClose FATStorageLifecycle)
        foreach(case IN ITEMS cancel-ds cancel-dldi child-cancel child-retry clean readonly
                retry recovery recovery-cancel recovery-failure)
            add_test(NAME frontend-close-sd-${case} COMMAND FrontendClose sd-${case})
            set_tests_properties(frontend-close-sd-${case} PROPERTIES
                TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        endforeach()
    endif()
    foreach(case IN ITEMS normal aliases errors)
        add_test(NAME fs-recovery-fat-${case} COMMAND FATStorageLifecycle recovery-${case})
        set_tests_properties(fs-recovery-fat-${case} PROPERTIES TIMEOUT 15)
    endforeach()
    set(fat_delete_cases file-normal dir-normal file-edit dir-edit dir-new file-type dir-type index-commit
        readonly-normal root-alias dir-symlink)
    if (WIN32)
        list(APPEND fat_delete_cases file-denied dir-denied readonly-denied)
    endif()
    foreach(case IN LISTS fat_delete_cases)
        add_test(NAME fs-delete-fat-${case} COMMAND FATStorageLifecycle delete-${case})
        set_tests_properties(fs-delete-fat-${case} PROPERTIES TIMEOUT 15)
    endforeach()
    set_tests_properties(fs-delete-fat-root-alias fs-delete-fat-dir-symlink PROPERTIES SKIP_RETURN_CODE 77)
    foreach(case IN ITEMS normal empty mount-read-error mount-seek-error length-error malformed format-write-error)
        add_test(NAME fat-storage-${case} COMMAND FATStorageLifecycle ${case})
        set_tests_properties(fat-storage-${case} PROPERTIES TIMEOUT 15)
    endforeach()
    set(storage_export_cases normal empty read short-read backing-read short-write close commit)
    if (WIN32)
        list(APPEND storage_export_cases replacement)
    endif()
    foreach(case IN LISTS storage_export_cases)
        add_test(NAME fs-export-fat-${case} COMMAND FATStorageLifecycle export-${case})
        set_tests_properties(fs-export-fat-${case} PROPERTIES TIMEOUT 15)
    endforeach()
    foreach(case IN ITEMS new-read host-conflict new-conflict host-collision guest-collision mixed index-write index-commit)
        add_test(NAME fs-export-fat-${case} COMMAND FATStorageLifecycle export-${case})
        set_tests_properties(fs-export-fat-${case} PROPERTIES TIMEOUT 15)
    endforeach()
    foreach(case IN ITEMS legacy-normal legacy-pending no-index-adoption host-only no-source-index-error)
        add_test(NAME fs-sync-${case} COMMAND FATStorageLifecycle sync-${case})
        set_tests_properties(fs-sync-${case} PROPERTIES TIMEOUT 15)
    endforeach()
else()
    message(STATUS "Qt Core unavailable: DSiSDFATBacking real-file checks not registered")
endif()
