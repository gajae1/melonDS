cmake_minimum_required(VERSION 3.30)

cmake_policy(VERSION 3.15)
if (POLICY CMP0076)
    cmake_policy(SET CMP0076 NEW)
endif()

set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)

set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake" ${CMAKE_MODULE_PATH})
set(CMAKE_USER_MAKE_RULES_OVERRIDE "${CMAKE_CURRENT_SOURCE_DIR}/cmake/DefaultBuildFlags.cmake")
set(CMAKE_OSX_DEPLOYMENT_TARGET "13.0" CACHE STRING "Minimum OS X deployment version")

option(USE_VCPKG "Use vcpkg for dependency packages" OFF)
if (USE_VCPKG)
	include(ConfigureVcpkg)
endif()

project(melonDS
    VERSION 1.1.38
    DESCRIPTION "DS emulator, sorta"
    HOMEPAGE_URL "https://melonds.kuribo64.net"
    LANGUAGES C CXX)

# Fork releases display a two-digit patch (1.1.01, 1.1.02, ...). Keep the
# project version numeric for Windows resources and package version ordering.
set(MELONDS_VERSION_DISPLAY "${melonDS_VERSION}")
if (melonDS_VERSION_PATCH LESS 10)
    set(MELONDS_VERSION_DISPLAY "${melonDS_VERSION_MAJOR}.${melonDS_VERSION_MINOR}.0${melonDS_VERSION_PATCH}")
endif()
file(WRITE "${CMAKE_BINARY_DIR}/release-version.txt" "${MELONDS_VERSION_DISPLAY}\n")

if (NOT "cxx_std_26" IN_LIST CMAKE_CXX_COMPILE_FEATURES)
    message(FATAL_ERROR "melonDS requires a compiler with C++26 mode (GCC 14+ or Clang 17+). No older-standard fallback is used.")
endif()

include(CheckSymbolExists)
include(CheckLibraryExists)
include(CMakeDependentOption)
include(CheckIPOSupported)

include(SetupCCache)
include(Sanitizers)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# No C++ module units are used; do not require a separate dependency scanner.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
include(HostFeatures)

option(MELONDS_CURRENT_TOOLCHAIN "Require the toolchain release floors audited for this fork" OFF)
if (MELONDS_CURRENT_TOOLCHAIN)
    if (CMAKE_VERSION VERSION_LESS 4.4.3)
        message(FATAL_ERROR "Current-toolchain profile requires CMake 4.4.3 or newer")
    endif()
    foreach(language IN ITEMS C CXX)
        if (CMAKE_${language}_COMPILER_ID STREQUAL "Clang")
            if (CMAKE_${language}_COMPILER_VERSION VERSION_LESS 23.1.0)
                message(FATAL_ERROR "Current-toolchain profile requires Clang 23.1.0 or newer (${language})")
            endif()
        elseif (CMAKE_${language}_COMPILER_ID STREQUAL "GNU")
            if (CMAKE_${language}_COMPILER_VERSION VERSION_LESS 16.2.0)
                message(FATAL_ERROR "Current-toolchain profile requires GCC 16.2.0 or newer (${language})")
            endif()
        else()
            message(FATAL_ERROR "Use the generic build or a verified Clang/GCC toolchain")
        endif()
    endforeach()
endif()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Choose the type of build." FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "MinSizeRel" "RelWithDebInfo")
endif()

function(detect_architecture symbol arch)
    if (NOT DEFINED ARCHITECTURE)
        set(CMAKE_REQUIRED_QUIET 1)
        check_symbol_exists("${symbol}" "" ARCHITECTURE_${arch})
        unset(CMAKE_REQUIRED_QUIET)

        # The output variable needs to be unique across invocations otherwise
        # CMake's crazy scope rules will keep it defined
        if (ARCHITECTURE_${arch})
            set(ARCHITECTURE "${arch}" PARENT_SCOPE)
            set(ARCHITECTURE_${arch} 1 PARENT_SCOPE)
            add_definitions(-DARCHITECTURE_${arch}=1)
        endif()
    endif()
endfunction()

detect_architecture("__x86_64__" x86_64)
detect_architecture("__i386__" x86)
detect_architecture("__arm__" ARM)
detect_architecture("__aarch64__" ARM64)

option(ENABLE_ASM "Allow handwritten assembly, including native JIT linkage" ON)
set(MELONDS_HAVE_ASM OFF)
if (ENABLE_ASM)
    include(CheckLanguage)
    check_language(ASM)
    if (CMAKE_ASM_COMPILER)
        set(MELONDS_HAVE_ASM ON)
    endif()
endif()
if (NOT MELONDS_HAVE_ASM)
    message(STATUS "Assembler disabled/unavailable: using interpreter, native JIT disabled")
endif()

cmake_dependent_option(ENABLE_JIT "Enable JIT recompiler" ON
    "MELONDS_HAVE_ASM;ARCHITECTURE STREQUAL x86_64 OR ARCHITECTURE STREQUAL ARM64;NOT ARCHITECTURE STREQUAL x86_64 OR NOT APPLE" OFF)
cmake_dependent_option(ENABLE_JIT_PROFILING "Enable JIT profiling with VTune" OFF "ENABLE_JIT" OFF)
option(ENABLE_OGLRENDERER "Enable OpenGL renderer" ON)

check_ipo_supported(RESULT IPO_SUPPORTED)
cmake_dependent_option(ENABLE_LTO_RELEASE "Enable link-time optimizations for release builds" ON "IPO_SUPPORTED" OFF)
cmake_dependent_option(ENABLE_LTO "Enable link-time optimizations" OFF "IPO_SUPPORTED" OFF)

if (ENABLE_LTO_RELEASE)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE TRUE)
endif()

if (ENABLE_LTO)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
endif()

if (NOT APPLE)
    set(CMAKE_EXE_LINKER_FLAGS_RELEASE "${CMAKE_EXE_LINKER_FLAGS_RELEASE} -s")
endif()

if (WIN32)
    option(BUILD_STATIC "Statically link dependencies" OFF)
    add_definitions(-DWIN32_LEAN_AND_MEAN -DNOMINMAX)
    if (NOT MINGW)
        add_definitions(-D__WIN32__ -D_CRT_SECURE_NO_WARNINGS -D_USE_MATH_DEFINES)
        if (BUILD_STATIC)
            set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
        endif()
    elseif (BUILD_STATIC AND MINGW)
        set(CMAKE_FIND_LIBRARY_SUFFIXES .a)
    endif()
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(ENABLE_GDBSTUB "Enable GDB stub" ON)
if (ENABLE_GDBSTUB)
    add_definitions(-DGDBSTUB_ENABLED)
endif()

option(BUILD_QT_SDL "Build Qt/SDL frontend" ON)
option(MELONDS_BUILD_TESTS "Build focused regression and core integration tests" OFF)
if (MELONDS_BUILD_TESTS)
    enable_testing()
endif()

add_subdirectory(src)

if (BUILD_QT_SDL)
    add_subdirectory(src/frontend/qt_sdl)
endif()

if (MELONDS_BUILD_TESTS)
    add_subdirectory(tests)
endif()
