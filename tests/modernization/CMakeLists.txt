cmake_minimum_required(VERSION 3.30)
project(melonDSModernizationTests LANGUAGES C CXX)
enable_testing()
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fwrapv>")
endif()
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# No C++ module units are used; do not require a separate dependency scanner.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
get_filename_component(MELONDS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
include("${MELONDS_ROOT}/cmake/HostFeatures.cmake")

add_executable(PixelConversion ../PixelConvert.cpp "${MELONDS_ROOT}/src/PixelConvert.cpp")
target_include_directories(PixelConversion PRIVATE "${MELONDS_ROOT}/src")
melonds_configure_pixel_kernels(PixelConversion)
add_test(NAME pixel-conversion-exact COMMAND PixelConversion)

add_executable(TeakraBitWidth ../TeakraBit.cpp)
target_include_directories(TeakraBitWidth PRIVATE "${MELONDS_ROOT}/src")
add_test(NAME teakra-standard-bit-width COMMAND TeakraBitWidth)

add_executable(CLanguageStandard ../CStandard.c)
add_test(NAME c23-language-mode COMMAND CLanguageStandard)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_custom_command(
    OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/SoftwareCaptureMethod.inc"
    COMMAND "${Python3_EXECUTABLE}" "${MELONDS_ROOT}/tests/ExtractFunction.py"
        "${MELONDS_ROOT}/src/GPU_Soft.cpp" "void SoftRenderer::DoCapture(u32 line)"
        "${CMAKE_CURRENT_BINARY_DIR}/SoftwareCaptureMethod.inc"
    DEPENDS "${MELONDS_ROOT}/tests/ExtractFunction.py" "${MELONDS_ROOT}/src/GPU_Soft.cpp"
    VERBATIM)
add_executable(SoftwareCapture ../SoftwareCapture.cpp "${CMAKE_CURRENT_BINARY_DIR}/SoftwareCaptureMethod.inc")
target_include_directories(SoftwareCapture PRIVATE "${MELONDS_ROOT}/src" "${CMAKE_CURRENT_BINARY_DIR}")
add_test(NAME software-capture-dirty-wrap COMMAND SoftwareCapture)
set_tests_properties(pixel-conversion-exact teakra-standard-bit-width c23-language-mode
    software-capture-dirty-wrap PROPERTIES TIMEOUT 30)

add_executable(HeaderCompleteness ../HeaderCompleteness.cpp)
target_include_directories(HeaderCompleteness PRIVATE "${MELONDS_ROOT}/src")
add_test(NAME complete-type-headers COMMAND HeaderCompleteness)

find_package(Threads REQUIRED)
add_executable(NetworkConcurrency ../NetworkConcurrency.cpp
    "${MELONDS_ROOT}/src/net/Net.cpp" "${MELONDS_ROOT}/src/net/PacketDispatcher.cpp")
target_include_directories(NetworkConcurrency PRIVATE "${MELONDS_ROOT}/src")
target_link_libraries(NetworkConcurrency PRIVATE Threads::Threads)
add_test(NAME network-queue-boundaries COMMAND NetworkConcurrency queues)
add_test(NAME network-driver-serialization COMMAND NetworkConcurrency threads)
set_tests_properties(network-queue-boundaries network-driver-serialization PROPERTIES TIMEOUT 30)
