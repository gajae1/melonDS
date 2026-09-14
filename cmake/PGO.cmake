# SPDX-License-Identifier: GPL-3.0-or-later
set(MELONDS_PGO "OFF" CACHE STRING "Core PGO: OFF, GENERATE, or USE (GCC)")
set_property(CACHE MELONDS_PGO PROPERTY STRINGS OFF GENERATE USE)
set(MELONDS_PGO_DIR "" CACHE PATH "Directory for one source/compiler PGO profile")
if (NOT MELONDS_PGO MATCHES "^(OFF|GENERATE|USE)$")
    message(FATAL_ERROR "MELONDS_PGO must be OFF, GENERATE, or USE")
endif()
if (NOT MELONDS_PGO STREQUAL "OFF")
    if (NOT CMAKE_C_COMPILER_ID STREQUAL "GNU" OR NOT CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        message(FATAL_ERROR "The current PGO workflow requires GCC for both C and C++")
    endif()
    if (NOT CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_CONFIGURATION_TYPES OR CMAKE_UNITY_BUILD)
        message(FATAL_ERROR "PGO requires a single-config Release build without unity compilation")
    endif()
    if (NOT MELONDS_PGO_DIR)
        message(FATAL_ERROR "Set MELONDS_PGO_DIR to a dedicated profile directory")
    endif()
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
endif()

function(melonds_configure_pgo target)
    if (MELONDS_PGO STREQUAL "OFF")
        return()
    endif()
    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    set(tool "${CMAKE_SOURCE_DIR}/tools/pgo.py")
    execute_process(COMMAND "${Python3_EXECUTABLE}" "${tool}" configure
        --repo "${CMAKE_SOURCE_DIR}" --build "${CMAKE_BINARY_DIR}"
        --profile "${MELONDS_PGO_DIR}" --mode "${MELONDS_PGO}"
        --cc "${CMAKE_C_COMPILER}" --cxx "${CMAKE_CXX_COMPILER}"
        RESULT_VARIABLE result ERROR_VARIABLE error)
    if (NOT result EQUAL 0)
        message(FATAL_ERROR "PGO configuration rejected: ${error}")
    endif()
    add_custom_target(melonds_pgo_check
        COMMAND "${Python3_EXECUTABLE}" "${tool}" verify --build "${CMAKE_BINARY_DIR}"
        COMMENT "Checking PGO source, compiler, commands and profile identity" VERBATIM)
    add_dependencies(${target} melonds_pgo_check)
    file(TO_NATIVE_PATH "${CMAKE_BINARY_DIR}" profile_prefix)
    if (MELONDS_PGO STREQUAL "GENERATE")
        target_compile_options(${target} PRIVATE
            "$<$<COMPILE_LANGUAGE:C,CXX>:-fprofile-generate=${MELONDS_PGO_DIR}/data>"
            "$<$<COMPILE_LANGUAGE:C,CXX>:-fprofile-prefix-path=${profile_prefix}>"
            "$<$<COMPILE_LANGUAGE:C,CXX>:-fprofile-update=atomic>")
        target_link_options(${target} INTERFACE "-fprofile-generate=${MELONDS_PGO_DIR}/data")
    else()
        # Only translation units with collected data receive -fprofile-use.
        # Missing or inconsistent data for those units remains a compiler error.
        include("${CMAKE_BINARY_DIR}/pgo-use.cmake")
    endif()
endfunction()
