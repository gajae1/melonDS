# Full NAND implementation and real FatFs/crypto, with disposable memory I/O.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(dsi_title_root "${CMAKE_CURRENT_LIST_DIR}/..")
set(dsi_title_generated "${CMAKE_CURRENT_BINARY_DIR}/dsi-title")
file(MAKE_DIRECTORY "${dsi_title_generated}")
set(dsi_title_methods)
foreach(entry IN ITEMS "ROL16|void DSi_AES::ROL16(u8* val, u32 n)"
        "DeriveNormalKey|void DSi_AES::DeriveNormalKey(u8* keyX, u8* keyY, u8* normalkey)")
    string(REPLACE "|" ";" parts "${entry}")
    list(GET parts 0 method)
    list(GET parts 1 signature)
    set(output "${dsi_title_generated}/${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${dsi_title_root}/tests/ExtractFunction.py"
            "${dsi_title_root}/src/DSi_AES.cpp" "${signature}" "${output}"
        DEPENDS "${dsi_title_root}/tests/ExtractFunction.py" "${dsi_title_root}/src/DSi_AES.cpp" VERBATIM)
    list(APPEND dsi_title_methods "${output}")
endforeach()

find_package(Qt6 QUIET COMPONENTS Widgets)
if (TARGET Qt6::Widgets)
    set(dsi_title_frontend "${dsi_title_root}/src/frontend/qt_sdl")
    set(dsi_title_completion "${dsi_title_generated}/CompleteImport.inc")
    add_custom_command(OUTPUT "${dsi_title_completion}"
        COMMAND "${Python3_EXECUTABLE}" "${dsi_title_root}/tests/ExtractFunction.py"
            "${dsi_title_frontend}/TitleManagerDialog.cpp"
            "void TitleManagerDialog::onImportTitleFinished(int res)" "${dsi_title_completion}"
        DEPENDS "${dsi_title_root}/tests/ExtractFunction.py" "${dsi_title_frontend}/TitleManagerDialog.cpp" VERBATIM)
    add_executable(DSiTitleUI "${CMAKE_CURRENT_LIST_DIR}/DSiTitleUI.cpp" "${dsi_title_completion}")
    set_target_properties(DSiTitleUI PROPERTIES AUTOUIC ON AUTOUIC_SEARCH_PATHS "${dsi_title_frontend}")
    target_include_directories(DSiTitleUI PRIVATE "${dsi_title_root}/src" "${dsi_title_generated}")
    target_compile_features(DSiTitleUI PRIVATE cxx_std_26)
    target_link_libraries(DSiTitleUI PRIVATE Qt6::Widgets)
    add_test(NAME dsi-title-ui COMMAND DSiTitleUI)
    set_tests_properties(dsi-title-ui PROPERTIES TIMEOUT 15 ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endif()
add_executable(DSiTitle "${CMAKE_CURRENT_LIST_DIR}/DSiTitle.cpp" ${dsi_title_methods}
    "${dsi_title_root}/src/FATIO.cpp" "${dsi_title_root}/src/fatfs/ff.c"
    "${dsi_title_root}/src/fatfs/ffunicode.c" "${dsi_title_root}/src/fatfs/ffsystem.c"
    "${dsi_title_root}/src/tiny-AES-c/aes.c" "${dsi_title_root}/src/sha1/sha1.c")
target_include_directories(DSiTitle PRIVATE "${dsi_title_root}/src" "${dsi_title_generated}")
target_compile_features(DSiTitle PRIVATE cxx_std_26)
foreach(case IN ITEMS normal invalid source short-write staging swap rollback-5 rollback-6 rollback-7 uncertain cleanup space persistent new-failures new-rollback-disk)
    add_test(NAME dsi-title-${case} COMMAND DSiTitle ${case})
    set_tests_properties(dsi-title-${case} PROPERTIES TIMEOUT 30)
endforeach()
