cmake_minimum_required(VERSION 3.16)
project(melonDSCoreRegressionTests LANGUAGES CXX)
enable_testing()

# Standalone: no Qt, SDL, BIOS images, ROMs, or full core build required.
add_executable(SavestateSections SavestateSections.cpp ../src/Savestate.cpp)
target_include_directories(SavestateSections PRIVATE ../src)
target_compile_features(SavestateSections PRIVATE cxx_std_17)

foreach(case roundtrip invalid-length truncated-header zero-length-skip oversized-skip)
    add_test(NAME savestate-${case} COMMAND SavestateSections ${case})
    set_tests_properties(savestate-${case} PROPERTIES TIMEOUT 5)
endforeach()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/CaptureMethod.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/GPU_OpenGL.cpp"
        "void GLRenderer::SyncVRAMCapture(u32 bank, u32 start, u32 len, bool complete)"
        "${CMAKE_CURRENT_BINARY_DIR}/CaptureMethod.inc"
    DEPENDS ExtractFunction.py ../src/GPU_OpenGL.cpp
    VERBATIM)
add_executable(CaptureReadback CaptureReadback.cpp "${CMAKE_CURRENT_BINARY_DIR}/CaptureMethod.inc")
target_include_directories(CaptureReadback PRIVATE ../src "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(CaptureReadback PRIVATE cxx_std_17)
add_test(NAME capture-readback-ranges COMMAND CaptureReadback)
set_tests_properties(capture-readback-ranges PROPERTIES TIMEOUT 10)

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/JitMethod.inc" "${CMAKE_CURRENT_BINARY_DIR}/JitArgs.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/ARMJIT.cpp"
        "void ARMJIT::SetMaxBlockSize(int size) noexcept"
        "${CMAKE_CURRENT_BINARY_DIR}/JitMethod.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/Args.h"
        "struct JITArgs"
        "${CMAKE_CURRENT_BINARY_DIR}/JitArgs.inc"
    DEPENDS ExtractFunction.py ../src/ARMJIT.cpp ../src/Args.h
    VERBATIM)
add_executable(JitSettings JitSettings.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/JitMethod.inc" "${CMAKE_CURRENT_BINARY_DIR}/JitArgs.inc")
target_include_directories(JitSettings PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
target_compile_features(JitSettings PRIVATE cxx_std_17)
add_test(NAME jit-setting-forwarding COMMAND JitSettings)
set_tests_properties(jit-setting-forwarding PROPERTIES TIMEOUT 5)
