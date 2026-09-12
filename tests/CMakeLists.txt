cmake_minimum_required(VERSION 3.30)
if (CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    project(melonDSCoreRegressionTests LANGUAGES C CXX)
endif()
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# No C++ module units are used; do not require a separate dependency scanner.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
enable_testing()
include(LocalMPQueue.cmake)
include(DSiSDTransfer.cmake)
include(DSiBoot26.cmake)
include(DSiTitle.cmake)
include(WifiState.cmake)
include(DSiAES.cmake)

# Standalone: no Qt, SDL, BIOS images, ROMs, or full core build required.
add_executable(SavestateSections SavestateSections.cpp ../src/Savestate.cpp)
target_include_directories(SavestateSections PRIVATE ../src)
target_compile_features(SavestateSections PRIVATE cxx_std_26)

foreach(case roundtrip invalid-length truncated-header zero-length-skip oversized-skip array-bounds section-bounds read-only-finish)
    add_test(NAME savestate-${case} COMMAND SavestateSections ${case})
    set_tests_properties(savestate-${case} PROPERTIES TIMEOUT 5)
endforeach()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/FirmwareCRC16.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/SPI.cpp" "u16 CRC16(const u8* data, u32 len, u32 start)"
        "${CMAKE_CURRENT_BINARY_DIR}/FirmwareCRC16.inc"
    DEPENDS ExtractFunction.py ../src/SPI.cpp VERBATIM)
add_executable(FirmwareFileIO FirmwareFileIO.cpp ../src/SPI_Firmware.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/FirmwareCRC16.inc")
target_include_directories(FirmwareFileIO PRIVATE ../src "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME firmware-file-input COMMAND FirmwareFileIO)
set_tests_properties(firmware-file-input PROPERTIES TIMEOUT 10)

add_test(NAME package-source-identity
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/PackageIdentity.py")
set_tests_properties(package-source-identity PROPERTIES TIMEOUT 120)
add_test(NAME package-version-boundaries
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/PackageVersion.py")
set_tests_properties(package-version-boundaries PROPERTIES TIMEOUT 120)
if(Python3_VERSION VERSION_GREATER_EQUAL 3.12)
    add_test(NAME libarchive-overlay-preservation
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/LibarchiveOverlay.py")
    set_tests_properties(libarchive-overlay-preservation PROPERTIES TIMEOUT 30)
endif()
if(WIN32 AND MINGW AND CMAKE_SIZEOF_VOID_P EQUAL 8
        AND CMAKE_C_COMPILER MATCHES "/ucrt64/bin/")
    get_filename_component(_melonds_ucrt_bin "${CMAKE_C_COMPILER}" DIRECTORY)
    get_filename_component(_melonds_ucrt_prefix "${_melonds_ucrt_bin}" DIRECTORY)
    add_test(NAME windows-runtime-deployment
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/WindowsDeploy.py"
            --msys-prefix "${_melonds_ucrt_prefix}")
    set_tests_properties(windows-runtime-deployment PROPERTIES TIMEOUT 120
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_melonds_ucrt_bin}")
    if(Python3_VERSION VERSION_GREATER_EQUAL 3.11)
        add_test(NAME windows-debug-symbols
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/DebugSymbols.py"
                --msys-prefix "${_melonds_ucrt_prefix}")
        set_tests_properties(windows-debug-symbols PROPERTIES TIMEOUT 120
            ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${_melonds_ucrt_bin}")
    endif()
endif()
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
target_compile_features(CaptureReadback PRIVATE cxx_std_26)
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
target_compile_features(JitSettings PRIVATE cxx_std_26)
add_test(NAME jit-setting-forwarding COMMAND JitSettings)
set_tests_properties(jit-setting-forwarding PROPERTIES TIMEOUT 5)

add_executable(LanguageStandard LanguageStandard.cpp)
target_compile_features(LanguageStandard PRIVATE cxx_std_26)
add_test(NAME cxx26-language-mode COMMAND LanguageStandard)

add_executable(Bitfield Bitfield.cpp)
target_include_directories(Bitfield PRIVATE ../src)
target_compile_features(Bitfield PRIVATE cxx_std_26)
add_test(NAME bitfield-boundaries COMMAND Bitfield all)
set_tests_properties(bitfield-boundaries PROPERTIES TIMEOUT 15)

option(MELONDS_BUILD_MICROBENCHMARKS "Build standalone microbenchmarks (not performance gates)" OFF)
if (MELONDS_BUILD_MICROBENCHMARKS)
    add_executable(BitfieldBenchmark BitfieldBenchmark.cpp)
    target_include_directories(BitfieldBenchmark PRIVATE ../src)
    target_compile_features(BitfieldBenchmark PRIVATE cxx_std_26)
endif()

add_executable(UTF8Paths UTF8Paths.cpp)
target_include_directories(UTF8Paths PRIVATE ../src)
target_compile_features(UTF8Paths PRIVATE cxx_std_26)
add_test(NAME utf8-path-roundtrip COMMAND UTF8Paths)

add_subdirectory(modernization)


if (TARGET core)
    find_package(Threads REQUIRED)
    add_executable(TeakraExecution TeakraExecution.cpp PlatformSync.cpp ../src/Savestate.cpp)
    target_include_directories(TeakraExecution PRIVATE ../src ../src/teakra/src)
    target_link_libraries(TeakraExecution PRIVATE teakra Threads::Threads)
    add_executable(TeakraVerifier ../src/teakra/src/test_verifier/main.cpp PlatformSync.cpp ../src/Savestate.cpp)
    target_include_directories(TeakraVerifier PRIVATE ../src ../src/teakra/src)
    target_link_libraries(TeakraVerifier PRIVATE teakra Threads::Threads)
    add_test(NAME teakra-execution-events-state COMMAND TeakraExecution)
    set_tests_properties(teakra-execution-events-state PROPERTIES TIMEOUT 60)
endif()

if (TARGET core)
    add_executable(SoftwareThread SoftwareThread.cpp PlatformHeadless.cpp)
    target_link_libraries(SoftwareThread PRIVATE core Threads::Threads)
    foreach(case IN ITEMS before-start after-done aborted reset stop normal)
        add_test(NAME software-thread-${case} COMMAND SoftwareThread ${case})
        set_tests_properties(software-thread-${case} PROPERTIES TIMEOUT 15)
    endforeach()
    add_executable(CoreExecution CoreExecution.cpp CoreALUExecution.cpp CoreBlockTransferExecution.cpp CoreDTCMExecution.cpp CoreMPUExecution.cpp CoreDeviceExecution.cpp CoreSchedulerExecution.cpp PlatformSync.cpp PlatformHeadless.cpp)
    target_link_libraries(CoreExecution PRIVATE core Threads::Threads)
    add_executable(AudioResampling AudioResampling.cpp PlatformSync.cpp PlatformHeadless.cpp)
    target_link_libraries(AudioResampling PRIVATE core Threads::Threads)
    add_test(NAME audio-core-clock COMMAND AudioResampling)
    set_tests_properties(audio-core-clock PROPERTIES TIMEOUT 30)
    add_test(NAME core-interpreter-execution COMMAND CoreExecution interpreter)
    if (ENABLE_GDBSTUB)
        add_test(NAME core-interpreter-gdb-spsr COMMAND CoreExecution interpreter gdb-spsr)
        set_tests_properties(core-interpreter-gdb-spsr PROPERTIES TIMEOUT 30)
        if (ENABLE_JIT)
            add_test(NAME core-jit-gdb-spsr COMMAND CoreExecution jit gdb-spsr)
            set_tests_properties(core-jit-gdb-spsr PROPERTIES TIMEOUT 30)
        endif()
    endif()
    add_test(NAME savestate-scheduler-values COMMAND CoreExecution interpreter savestate-scheduler)
    set_tests_properties(savestate-scheduler-values PROPERTIES TIMEOUT 30)
    add_test(NAME core-scheduler-execution COMMAND CoreExecution interpreter scheduler-execution)
    add_test(NAME core-dsi-ndma COMMAND CoreExecution interpreter dsi-ndma)
    set_tests_properties(core-dsi-ndma PROPERTIES TIMEOUT 30)
    add_test(NAME core-dma-slot-timing COMMAND CoreExecution interpreter dma-slot-timing)
    set_tests_properties(core-dma-slot-timing PROPERTIES TIMEOUT 30)
    add_test(NAME core-dsi-reset-i2c COMMAND CoreExecution interpreter dsi-reset-i2c)
    set_tests_properties(core-dsi-reset-i2c PROPERTIES TIMEOUT 30)
    add_test(NAME core-dsi-btdmp COMMAND CoreExecution interpreter dsi-btdmp)
    set_tests_properties(core-dsi-btdmp PROPERTIES TIMEOUT 30)
    set_tests_properties(core-scheduler-execution PROPERTIES TIMEOUT 30)
    foreach(backend IN ITEMS interpreter jit fastmem)
        if (NOT ENABLE_JIT AND NOT backend STREQUAL "interpreter")
            continue()
        endif()
        foreach(group IN ITEMS alu-shift thumb-shift-timing multiply-cycles thumb-stack thumb-push-timing unaligned-memory block-transfer dtcm-remap mpu-execution cache-mpu-disabled device-execution)
            add_test(NAME core-${backend}-${group} COMMAND CoreExecution ${backend} ${group})
            set_tests_properties(core-${backend}-${group} PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
        endforeach()
    endforeach()
    if (ENABLE_JIT)
        add_test(NAME core-jit-execution COMMAND CoreExecution jit)
        add_test(NAME core-jit-fastmem-execution COMMAND CoreExecution fastmem)
        if (ARCHITECTURE STREQUAL "x86_64")
            add_test(NAME core-jit-lzcnt-fallback COMMAND CoreExecution jit no-lzcnt)
            set_tests_properties(core-jit-lzcnt-fallback PROPERTIES TIMEOUT 30)
        endif()
    endif()
    set_tests_properties(core-interpreter-execution PROPERTIES TIMEOUT 30)
    if (ENABLE_JIT)
        set_tests_properties(core-jit-execution core-jit-fastmem-execution PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    endif()
endif()

add_executable(BufferUtilities BufferUtilities.cpp ../src/Utils.cpp)
target_include_directories(BufferUtilities PRIVATE ../src)
foreach(case IN ITEMS copies overflow null)
    add_test(NAME buffer-${case} COMMAND BufferUtilities ${case})
    set_tests_properties(buffer-${case} PROPERTIES TIMEOUT 5)
endforeach()

add_executable(StandardBitOps StandardBitOps.cpp)
target_include_directories(StandardBitOps PRIVATE ../src)
target_compile_definitions(StandardBitOps PRIVATE REQUIRE_CONSTEXPR)
add_test(NAME standard-bit-operations COMMAND StandardBitOps)

add_executable(ByteSwap ByteSwap.cpp)
target_include_directories(ByteSwap PRIVATE ../src)
add_test(NAME aes-byteswap-alignment-overlap COMMAND ByteSwap)

add_executable(GlibString GlibString.c ../src/net/libslirp/glib/glib.c)
target_include_directories(GlibString PRIVATE ../src/net/libslirp/glib)
set_target_properties(GlibString PROPERTIES C_STANDARD 23 C_STANDARD_REQUIRED ON)
add_test(NAME glib-string-boundaries COMMAND GlibString)

if (MELONDS_BUILD_MICROBENCHMARKS)
    add_executable(PixelConvertBenchmark PixelConvertBenchmark.cpp ../src/PixelConvert.cpp)
    target_include_directories(PixelConvertBenchmark PRIVATE ../src)
    melonds_configure_pixel_kernels(PixelConvertBenchmark)
    add_executable(AudioFilterBenchmark AudioFilterBenchmark.cpp)
    target_include_directories(AudioFilterBenchmark PRIVATE ../src/frontend/qt_sdl)
    melonds_configure_audio_kernels(AudioFilterBenchmark)
    if (TARGET core AND ENABLE_JIT AND ARCHITECTURE STREQUAL "x86_64")
        add_executable(JitCLZBenchmark JitCLZBenchmark.cpp PlatformSync.cpp PlatformHeadless.cpp)
        target_link_libraries(JitCLZBenchmark PRIVATE core Threads::Threads)
    endif()
endif()

option(MELONDS_TEST_GPU "Run tests requiring a real OpenGL context" OFF)
if (MELONDS_TEST_GPU)
    if (NOT TARGET core OR NOT ENABLE_OGLRENDERER)
        message(FATAL_ERROR "GPU tests require the full OpenGL core and SDL2 frontend dependencies")
    endif()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GPU_TEST_SDL2 REQUIRED IMPORTED_TARGET sdl2)
    add_executable(GLFrameReadback GLFrameReadback.cpp ComputeFailure.cpp ComputeWorkload.cpp GLTextureBoundaries.cpp GLCaptureReadback.cpp GLResourceLifetime.cpp GLAllocationFailure.cpp PlatformSync.cpp PlatformHeadless.cpp
        ../src/frontend/glad/glad.c)
    target_link_libraries(GLFrameReadback PRIVATE core PkgConfig::GPU_TEST_SDL2 Threads::Threads)
    foreach(method IN ITEMS UpdateRenderer CompileShaders)
        if (method STREQUAL "UpdateRenderer")
            set(signature "void EmuThread::updateRenderer()")
        else()
            set(signature "void EmuThread::compileShaders()")
        endif()
        set(extracted "${CMAKE_CURRENT_BINARY_DIR}/compute${method}.inc")
        add_custom_command(OUTPUT "${extracted}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/../src/frontend/qt_sdl/EmuThread.cpp" "${signature}" "${extracted}"
            DEPENDS ExtractFunction.py ../src/frontend/qt_sdl/EmuThread.cpp VERBATIM)
        target_sources(GLFrameReadback PRIVATE "${extracted}")
    endforeach()
    target_include_directories(GLFrameReadback PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
    add_executable(ROMSmoke ROMSmoke.cpp PlatformSync.cpp PlatformHeadless.cpp
        ../src/frontend/glad/glad.c)
    foreach(method IN ITEMS Callback Sync)
        if (method STREQUAL "Callback")
            set(signature "void EmuInstance::audioCallback(void* data, Uint8* stream, int len)")
        else()
            set(signature "void EmuInstance::audioSync(int frameSamples)")
        endif()
        set(extracted "${CMAKE_CURRENT_BINARY_DIR}/ROMSmokeAudio${method}.inc")
        add_custom_command(OUTPUT "${extracted}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
                "${CMAKE_CURRENT_SOURCE_DIR}/../src/frontend/qt_sdl/EmuInstanceAudio.cpp"
                "${signature}" "${extracted}"
            DEPENDS ExtractFunction.py ../src/frontend/qt_sdl/EmuInstanceAudio.cpp VERBATIM)
        target_sources(ROMSmoke PRIVATE "${extracted}")
    endforeach()
    target_include_directories(ROMSmoke PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" ../src/frontend/qt_sdl)
    melonds_configure_audio_kernels(ROMSmoke)
    target_link_libraries(ROMSmoke PRIVATE core PkgConfig::GPU_TEST_SDL2 Threads::Threads)
    target_compile_definitions(ROMSmoke PRIVATE MELONDS_TEST_MEMORY_FILES)
    add_test(NAME gpu-opengl-frame-capture COMMAND GLFrameReadback)
    add_test(NAME gpu-software-2d-composite COMMAND GLFrameReadback software-2d-composite)
    set_tests_properties(gpu-software-2d-composite PROPERTIES TIMEOUT 30)
    add_test(NAME gpu-compute-frame-capture COMMAND GLFrameReadback compute)
    set_tests_properties(gpu-opengl-frame-capture gpu-compute-frame-capture
        PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
    foreach(case IN ITEMS alpha-software alpha-opengl alpha-compute blend-software blend-opengl blend-compute shading-software shading-opengl shading-compute capture-software capture-opengl capture-compute bitmap-software bitmap-opengl bitmap-compute dirty-wrap)
        add_test(NAME gpu-gl-texture-${case} COMMAND GLFrameReadback gl-texture-boundaries ${case})
        set_tests_properties(gpu-gl-texture-${case}
            PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
    endforeach()
    foreach(workload IN ITEMS storage dispatch spans control blend)
        add_test(NAME gpu-compute-workload-${workload} COMMAND GLFrameReadback compute-workload ${workload})
        set_tests_properties(gpu-compute-workload-${workload}
            PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
    endforeach()
    foreach(failure IN ITEMS control capability compile link recompile frontend)
        add_test(NAME gpu-compute-failure-${failure} COMMAND GLFrameReadback compute-failure ${failure})
        set_tests_properties(gpu-compute-failure-${failure}
            PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
    endforeach()
    foreach(backend IN ITEMS software opengl compute)
        if (ENABLE_JIT)
            add_test(NAME gpu-${backend}-capture-jit COMMAND GLFrameReadback capture-jit ${backend})
            set_tests_properties(gpu-${backend}-capture-jit
                PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
        endif()
        add_test(NAME gpu-${backend}-capture-mid COMMAND GLFrameReadback capture-mid ${backend})
        set_tests_properties(gpu-${backend}-capture-mid
            PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
        add_test(NAME gpu-${backend}-capture-readback COMMAND GLFrameReadback capture-readback ${backend})
        set_tests_properties(gpu-${backend}-capture-readback
            PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
    endforeach()
    foreach(resource IN ITEMS normal common-fail 3d-fail)
        add_test(NAME gpu-gl-resource-${resource} COMMAND GLFrameReadback gl-resource ${resource})
        set_tests_properties(gpu-gl-resource-${resource}
            PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
    endforeach()
    foreach(allocation IN ITEMS control texture-limit viewport-limit ssbo-limit texel-limit buffer-fail texture-fail oom invalid-scale capture-fail 2d-fail classic-fail classic-blend-fail)
        add_test(NAME gpu-gl-allocation-${allocation} COMMAND GLFrameReadback gl-allocation ${allocation})
        set_tests_properties(gpu-gl-allocation-${allocation}
            PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 RUN_SERIAL TRUE)
    endforeach()
endif()

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/CLZMethod.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/ARMInterpreter_ALU.cpp" "void A_CLZ(ARM* cpu)"
        "${CMAKE_CURRENT_BINARY_DIR}/CLZMethod.inc"
    DEPENDS ExtractFunction.py ../src/ARMInterpreter_ALU.cpp
    VERBATIM)
add_executable(CountLeadingZeros CountLeadingZeros.cpp "${CMAKE_CURRENT_BINARY_DIR}/CLZMethod.inc")
target_include_directories(CountLeadingZeros PRIVATE ../src "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME arm-clz-semantics COMMAND CountLeadingZeros)

# Run the real A64 emitter on the local host and check the generated integer
# instruction bytes. This is deliberately separate from native ARM execution.
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ARM64JitOp2.inc"
           "${CMAKE_CURRENT_BINARY_DIR}/ARM64JitArithmetic.inc"
           "${CMAKE_CURRENT_BINARY_DIR}/ARM64JitFlags.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/ARMJIT_A64/ARMJIT_Compiler.h" "struct Op2"
        "${CMAKE_CURRENT_BINARY_DIR}/ARM64JitOp2.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/ARMJIT_A64/ARMJIT_ALU.cpp"
        "void Compiler::Comp_Arithmetic(int op, bool S, ARM64Reg rd, ARM64Reg rn, Op2 op2)"
        "${CMAKE_CURRENT_BINARY_DIR}/ARM64JitArithmetic.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/ARMJIT_A64/ARMJIT_ALU.cpp"
        "void Compiler::Comp_RetriveFlags(bool retriveCV)"
        "${CMAKE_CURRENT_BINARY_DIR}/ARM64JitFlags.inc"
    DEPENDS ExtractFunction.py ../src/ARMJIT_A64/ARMJIT_Compiler.h ../src/ARMJIT_A64/ARMJIT_ALU.cpp
    VERBATIM)
add_executable(ARM64JitArithmetic ARM64JitArithmetic.cpp ../src/jit/Arm64Emitter.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/ARM64JitOp2.inc" "${CMAKE_CURRENT_BINARY_DIR}/ARM64JitArithmetic.inc"
    "${CMAKE_CURRENT_BINARY_DIR}/ARM64JitFlags.inc")
target_include_directories(ARM64JitArithmetic PRIVATE ../src "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME arm64-jit-arithmetic-encoding COMMAND ARM64JitArithmetic)

if (TARGET core AND ENABLE_JIT)
    # Keep CompileBlock, its branch/cycle helpers, and the ADD emitter as current
    # production definitions. The small ISA model also calls the real fallback.
    set(arm64_block_definitions
        "CompileBlock|ARMJIT_Compiler.cpp|JitBlockEntry Compiler::CompileBlock(ARM* cpu, bool thumb, FetchedInstr instrs[], int instrsCount, bool hasMemInstr)"
        "BranchSpecial|ARMJIT_Compiler.cpp|void Compiler::Comp_BranchSpecialBehaviour(bool taken)"
        "Condition|ARMJIT_Compiler.cpp|FixupBranch Compiler::CheckCondition(u32 cond)"
        "CyclesC|ARMJIT_Compiler.cpp|void Compiler::Comp_AddCycles_C(bool forceNonConstant)"
        "CyclesCI|ARMJIT_Compiler.cpp|void Compiler::Comp_AddCycles_CI(u32 numI)"
        "CyclesVariable|ARMJIT_Compiler.cpp|void Compiler::Comp_AddCycles_CI(u32 c, ARM64Reg numI, ArithOption shift)"
        "MulMla|ARMJIT_ALU.cpp|void Compiler::Comp_Mul_Mla(bool S, bool mla, ARM64Reg rd, ARM64Reg rm, ARM64Reg rs, ARM64Reg rn)"
        "Mul|ARMJIT_ALU.cpp|void Compiler::A_Comp_Mul()"
        "MulLong|ARMJIT_ALU.cpp|void Compiler::A_Comp_Mul_Long()"
        "ThumbALU|ARMJIT_ALU.cpp|void Compiler::T_Comp_ALU()"
        "Compare|ARMJIT_ALU.cpp|void Compiler::Comp_Compare(int op, ARM64Reg rn, Op2 op2)"
        "LoadCycles|ARMJIT_Compiler.cpp|void Compiler::LoadCycles()"
        "SaveCycles|ARMJIT_Compiler.cpp|void Compiler::SaveCycles()"
        "LoadCPSR|ARMJIT_Compiler.cpp|void Compiler::LoadCPSR()"
        "SaveCPSR|ARMJIT_Compiler.cpp|void Compiler::SaveCPSR(bool markClean)"
        "LoadReg|ARMJIT_Compiler.cpp|void Compiler::LoadReg(int reg, ARM64Reg nativeReg)"
        "SaveReg|ARMJIT_Compiler.cpp|void Compiler::SaveReg(int reg, ARM64Reg nativeReg)"
        "TriOp|ARMJIT_ALU.cpp|void Compiler::A_Comp_ALUTriOp()"
        "GetOp2|ARMJIT_ALU.cpp|void Compiler::A_Comp_GetOp2(bool S, Op2& op2)"
        "ShiftReg|ARMJIT_ALU.cpp|void Compiler::Comp_RegShiftReg(int op, bool S, Op2& op2, ARM64Reg rs)"
        "ShiftImm|ARMJIT_ALU.cpp|void Compiler::Comp_RegShiftImm(int op, int amount, bool S, Op2& op2, ARM64Reg tmp)")
    foreach(definition IN LISTS arm64_block_definitions)
        string(REPLACE "|" ";" fields "${definition}")
        list(GET fields 0 name)
        list(GET fields 1 file)
        list(GET fields 2 signature)
        set(source "${CMAKE_CURRENT_SOURCE_DIR}/../src/ARMJIT_A64/${file}")
        set(output "${CMAKE_CURRENT_BINARY_DIR}/ARM64Jit${name}.inc")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
                "${source}" "${signature}" "${output}"
            DEPENDS ExtractFunction.py "${source}" VERBATIM)
        target_sources(ARM64JitArithmetic PRIVATE "${output}")
    endforeach()
    target_sources(ARM64JitArithmetic PRIVATE PlatformSync.cpp PlatformHeadless.cpp)
    target_link_libraries(ARM64JitArithmetic PRIVATE core Threads::Threads)
    target_compile_definitions(ARM64JitArithmetic PRIVATE ARM64_JIT_BLOCK_TEST ARM64_JIT_MUL_TEST)
    # The extracted backend uses the same ARM member offsets as core.
    target_compile_options(ARM64JitArithmetic PRIVATE $<$<CXX_COMPILER_ID:GNU,Clang>:-Wno-invalid-offsetof>)
    add_test(NAME arm64-jit-conditional-cycles COMMAND ARM64JitArithmetic conditional-cycles)
    set_tests_properties(arm64-jit-conditional-cycles PROPERTIES TIMEOUT 30)
    add_test(NAME core-interpreter-conditional-cycles COMMAND CoreExecution interpreter conditional-cycles)
    set_tests_properties(core-interpreter-conditional-cycles PROPERTIES TIMEOUT 30)
    add_test(NAME core-jit-conditional-cycles COMMAND CoreExecution jit conditional-cycles)
    set_tests_properties(core-jit-conditional-cycles PROPERTIES TIMEOUT 30)
    set(MELONDS_UNICORN_PYTHON "" CACHE FILEPATH
        "Optional Python with Unicorn for execution of generated A64 multiply blocks")
    if (MELONDS_UNICORN_PYTHON)
        add_test(NAME arm64-jit-multiply-execution
            COMMAND "${MELONDS_UNICORN_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/ARM64JitMultiply.py"
                "$<TARGET_FILE:ARM64JitArithmetic>")
        set_tests_properties(arm64-jit-multiply-execution PROPERTIES TIMEOUT 60)
    endif()
endif()

if (TARGET core)
    add_executable(CartSPI CartSPI.cpp PlatformSync.cpp PlatformHeadless.cpp)
    target_link_libraries(CartSPI PRIVATE core Threads::Threads)
    foreach(case IN ITEMS control arm9-low arm7-low busy-low mode-control ownership state-resume)
        add_test(NAME cart-spi-${case} COMMAND CartSPI ${case})
        set_tests_properties(cart-spi-${case} PROPERTIES TIMEOUT 15)
    endforeach()

    add_executable(CartROMBounds CartROMBounds.cpp PlatformSync.cpp PlatformHeadless.cpp)
    target_link_libraries(CartROMBounds PRIVATE core Threads::Threads)
    add_test(NAME gba-rom-input-bounds COMMAND CartROMBounds)
    set_tests_properties(gba-rom-input-bounds PROPERTIES TIMEOUT 10)

    add_executable(ARExecution ARExecution.cpp PlatformSync.cpp PlatformHeadless.cpp)
    target_link_libraries(ARExecution PRIVATE core Threads::Threads)
    foreach(case IN ITEMS controls loop-control empty odd literal-short literal-overflow skipped-overflow
            cancel-loop cancel-copy cancel-literal pending-cancel unsupported
            next-d1 next-d2 next-mixed condition-end loop-replace counter-mask)
        add_test(NAME ar-execution-${case} COMMAND ARExecution ${case})
        set_tests_properties(ar-execution-${case} PROPERTIES TIMEOUT 10)
    endforeach()
endif()

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/ROMHeaderMethod.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/NDSCart.cpp" "bool ValidateROM(u32 romlen, NDSHeader& header)"
        "${CMAKE_CURRENT_BINARY_DIR}/ROMHeaderMethod.inc"
    DEPENDS ExtractFunction.py ../src/NDSCart.cpp
    VERBATIM)
add_executable(ROMHeaderBounds ROMHeaderBounds.cpp "${CMAKE_CURRENT_BINARY_DIR}/ROMHeaderMethod.inc")
target_include_directories(ROMHeaderBounds PRIVATE ../src "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME nds-rom-header-bounds COMMAND ROMHeaderBounds)

set(rtc_methods)
foreach(pair IN ITEMS
        "ScheduleEvent|void NDS::ScheduleEvent(u32 id, bool periodic, s32 delay, u32 funcid, u32 param)"
        "RegisterEventFuncs|void NDS::RegisterEventFuncs(u32 id, void* that, const std::initializer_list<EventFunc>& funcs)"
        "UnregisterEventFuncs|void NDS::UnregisterEventFuncs(u32 id)"
        "Reschedule|void NDS::Reschedule(u64 target)"
        "RunSystem|void NDS::RunSystem(u64 timestamp)"
        "CancelEvent|void NDS::CancelEvent(u32 id)")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 method)
    list(GET parts 1 signature)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/RTC${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/NDS.cpp" "${signature}" "${output}"
        DEPENDS ExtractFunction.py ../src/NDS.cpp VERBATIM)
    list(APPEND rtc_methods "${output}")
endforeach()
add_executable(RTCCalendar RTCCalendar.cpp ../src/Savestate.cpp ${rtc_methods})
target_include_directories(RTCCalendar PRIVATE ../src "${CMAKE_CURRENT_BINARY_DIR}")
foreach(case IN ITEMS calendar hour-modes bcd serial-input serial-output alarm-irq periodic-irq minute-rearm reset-bus)
    add_test(NAME rtc-${case} COMMAND RTCCalendar ${case})
    set_tests_properties(rtc-${case} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(IRCartState IRCartState.cpp
    ../src/NDSCart/CartRetailIR.cpp ../src/NDSCart/CartRetail.cpp ../src/NDSCart/CartCommon.cpp
    ../src/Savestate.cpp ../src/CRC32.cpp ../src/Utils.cpp)
target_include_directories(IRCartState PRIVATE ../src)
foreach(case IN ITEMS id-control save-control unknown-command reset-command restore-id restore-command-start
        restore-save restore-write legacy-boundary truncated-section position-wrap)
    add_test(NAME ir-cart-${case} COMMAND IRCartState ${case})
    set_tests_properties(ir-cart-${case} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(RetailEEPROM RetailEEPROM.cpp
    ../src/NDSCart/CartRetail.cpp ../src/NDSCart/CartCommon.cpp
    ../src/Savestate.cpp ../src/CRC32.cpp ../src/Utils.cpp)
target_include_directories(RetailEEPROM PRIVATE ../src)
foreach(case IN ITEMS control page-ends page-overflow restore-write regular-control)
    add_test(NAME retail-eeprom-${case} COMMAND RetailEEPROM ${case})
    set_tests_properties(retail-eeprom-${case} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(GBARumble GBARumble.cpp ../src/GBACart.cpp ../src/GBACartMotionPak.cpp
    ../src/Savestate.cpp ../src/CRC32.cpp ../src/Utils.cpp)
target_include_directories(GBARumble PRIVATE ../src)
foreach(case IN ITEMS control ad1-mask state-load)
    add_test(NAME gba-rumble-${case} COMMAND GBARumble ${case})
    set_tests_properties(gba-rumble-${case} PROPERTIES TIMEOUT 15)
endforeach()

set(homebrew_methods)
foreach(pair IN ITEMS
        "SetupDirectBoot|CartHomebrew.cpp|void CartHomebrew::SetupDirectBoot(const std::string& romname, NDS& nds)"
        "ApplyDLDIPatch|CartSD.cpp|void CartSD::ApplyDLDIPatch(const u8* patch, u32 patchlen, bool readonly)"
        "ApplyDLDIPatchAt|CartSD.cpp|bool CartSD::ApplyDLDIPatchAt(u8* binary, u32 binarylen, u32 dldioffset, const u8* patch, u32 patchlen, bool readonly) const")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 method)
    list(GET parts 1 source)
    list(GET parts 2 signature)
    set(output "${CMAKE_CURRENT_BINARY_DIR}/Homebrew${method}.inc")
    add_custom_command(OUTPUT "${output}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/../src/NDSCart/${source}" "${signature}" "${output}"
        DEPENDS ExtractFunction.py "../src/NDSCart/${source}" VERBATIM)
    list(APPEND homebrew_methods "${output}")
endforeach()
add_executable(HomebrewInput HomebrewInput.cpp ${homebrew_methods})
target_include_directories(HomebrewInput PRIVATE ../src "${CMAKE_CURRENT_BINARY_DIR}")
foreach(case IN ITEMS argv-control argv-oversize argv-nul argv-address dldi-control dldi-repatch
        dldi-patch-guard dldi-rom-guard dldi-arm9-span dldi-scan-tail dldi-target-header dldi-source-header
        dldi-target-layout dldi-fix-control dldi-fix-overlap dldi-fix-range dldi-readonly-range dldi-address-overflow)
    add_test(NAME homebrew-${case} COMMAND HomebrewInput ${case})
    set_tests_properties(homebrew-${case} PROPERTIES TIMEOUT 15)
endforeach()

add_executable(GdbProtocol GdbProtocol.cpp ../src/CRC32.cpp)
target_include_directories(GdbProtocol PRIVATE ../src)
find_package(Threads REQUIRED)
target_link_libraries(GdbProtocol PRIVATE Threads::Threads)
if (WIN32)
    target_link_libraries(GdbProtocol PRIVATE ws2_32)
endif()
foreach(case IN ITEMS memory-control binary-tail binary-escape last-address short-hex missing-colon
        invalid-hex extra-hex wrap short-binary dangling-escape receive-limit checksum checksum-recovery
        coalesced response-limit response-chunks response-zero response-escape response-format memory-limit
        disconnect eof ack-eof poll-wait poll-error extra-binary stream-boundaries response-extra loopback q-xfer
        split-resend listener-error crc-control crc-chunks crc-wrap crc-malformed
        register-xml register-bulk register-malformed)
    add_test(NAME gdb-protocol-${case} COMMAND GdbProtocol ${case})
    set_tests_properties(gdb-protocol-${case} PROPERTIES TIMEOUT 15)
endforeach()


if (TARGET net-utils)
    add_executable(LANPacket LANPacket.cpp PlatformSync.cpp)
    target_include_directories(LANPacket PRIVATE ../src)
    target_link_libraries(LANPacket PRIVATE Threads::Threads)
    if (USE_VCPKG)
        find_package(unofficial-enet CONFIG REQUIRED)
        target_link_libraries(LANPacket PRIVATE unofficial::enet::enet)
    else()
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(LAN_TEST_ENET REQUIRED IMPORTED_TARGET libenet)
        target_link_libraries(LANPacket PRIVATE PkgConfig::LAN_TEST_ENET)
    endif()
    if (WIN32)
        target_link_libraries(LANPacket PRIVATE ws2_32)
    endif()
    add_test(NAME lan-packet-validation COMMAND LANPacket)
    set_tests_properties(lan-packet-validation PROPERTIES TIMEOUT 15)

    add_executable(LANLoopback LANLoopback.cpp PlatformSync.cpp)
    target_include_directories(LANLoopback PRIVATE ../src)
    target_link_libraries(LANLoopback PRIVATE net-utils Threads::Threads)
    add_test(NAME lan-loopback-session COMMAND LANLoopback)
    set_tests_properties(lan-loopback-session PROPERTIES TIMEOUT 20 RESOURCE_LOCK lan-loopback-port)

    find_package(Qt6 QUIET COMPONENTS Widgets Network)
    if (TARGET Qt6::Widgets AND TARGET Qt6::Network)
        set(lan_ui_dir "${CMAKE_SOURCE_DIR}/src/frontend/qt_sdl")
        add_executable(LANUI LANUI.cpp PlatformSync.cpp "${lan_ui_dir}/LANDialog.h"
            "${lan_ui_dir}/LANDialog.ui" "${lan_ui_dir}/LANStartHostDialog.ui"
            "${lan_ui_dir}/LANStartClientDialog.ui")
        set_target_properties(LANUI PROPERTIES AUTOMOC ON AUTOUIC ON AUTOUIC_SEARCH_PATHS "${lan_ui_dir}")
        target_include_directories(LANUI PRIVATE ../src "${lan_ui_dir}")
        target_link_libraries(LANUI PRIVATE net-utils Qt6::Widgets Qt6::Network Threads::Threads)
        add_test(NAME lan-ui-session COMMAND LANUI)
        set_tests_properties(lan-ui-session PROPERTIES TIMEOUT 20 RESOURCE_LOCK lan-loopback-port
            ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    endif()
endif()

add_executable(PCapInput PCapInput.cpp)
target_include_directories(PCapInput PRIVATE ../src ../src/net)
foreach(case IN ITEMS controls missing-symbols capture link-type io-errors open-lifetime enumeration)
    add_test(NAME pcap-input-${case} COMMAND PCapInput ${case})
    set_tests_properties(pcap-input-${case} PROPERTIES TIMEOUT 10)
endforeach()

add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/SlirpFinishUDPFrame.inc"
           "${CMAKE_CURRENT_BINARY_DIR}/SlirpHandleDNSFrame.inc"
           "${CMAKE_CURRENT_BINARY_DIR}/SlirpSendPacket.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/net/Net_Slirp.cpp" "void FinishUDPFrame(u8* data, int len)"
        "${CMAKE_CURRENT_BINARY_DIR}/SlirpFinishUDPFrame.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/net/Net_Slirp.cpp" "void Net_Slirp::HandleDNSFrame(u8* data, int len) noexcept"
        "${CMAKE_CURRENT_BINARY_DIR}/SlirpHandleDNSFrame.inc"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ExtractFunction.py"
        "${CMAKE_CURRENT_SOURCE_DIR}/../src/net/Net_Slirp.cpp" "int Net_Slirp::SendPacket(u8* data, int len) noexcept"
        "${CMAKE_CURRENT_BINARY_DIR}/SlirpSendPacket.inc"
    DEPENDS ExtractFunction.py ../src/net/Net_Slirp.cpp
    VERBATIM)
add_executable(SlirpDNS SlirpDNS.cpp "${CMAKE_CURRENT_BINARY_DIR}/SlirpFinishUDPFrame.inc"
    "${CMAKE_CURRENT_BINARY_DIR}/SlirpHandleDNSFrame.inc" "${CMAKE_CURRENT_BINARY_DIR}/SlirpSendPacket.inc")
target_include_directories(SlirpDNS PRIVATE ../src "${CMAKE_CURRENT_BINARY_DIR}")
if (WIN32)
    target_link_libraries(SlirpDNS PRIVATE ws2_32)
endif()
foreach(case IN ITEMS query-control header-guard transport-lengths ipv4-options label-guard
        query-shape fragments-forwarding name-response-bounds)
    add_test(NAME slirp-dns-${case} COMMAND SlirpDNS ${case})
    set_tests_properties(slirp-dns-${case} PROPERTIES TIMEOUT 10)
endforeach()
