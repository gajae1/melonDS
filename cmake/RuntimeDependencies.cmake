# SPDX-License-Identifier: GPL-3.0-or-later
# Script-mode PE dependency resolution for tools/deploy-windows.py.
cmake_minimum_required(VERSION 3.21)
if(POLICY CMP0207)
    cmake_policy(SET CMP0207 NEW)
endif()
file(READ "${INPUT_FILE}" request)
foreach(key IN ITEMS prefix executable objdump)
    string(JSON ${key} GET "${request}" "${key}")
endforeach()
foreach(key IN ITEMS libraries modules system_filters)
    set(${key})
    string(JSON count LENGTH "${request}" "${key}")
    if(count GREATER 0)
        math(EXPR last "${count} - 1")
        foreach(index RANGE ${last})
            string(JSON value GET "${request}" "${key}" ${index})
            list(APPEND ${key} "${value}")
        endforeach()
    endif()
endforeach()
set(CMAKE_GET_RUNTIME_DEPENDENCIES_PLATFORM "windows+pe")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_TOOL "objdump")
set(CMAKE_GET_RUNTIME_DEPENDENCIES_COMMAND "${objdump}")
file(GET_RUNTIME_DEPENDENCIES
    EXECUTABLES "${executable}"
    LIBRARIES ${libraries}
    MODULES ${modules}
    DIRECTORIES "${prefix}/bin"
    PRE_EXCLUDE_REGEXES "^api-ms-.*\\.dll$" "^ext-ms-.*\\.dll$"
    POST_EXCLUDE_REGEXES ${system_filters}
    RESOLVED_DEPENDENCIES_VAR resolved
    UNRESOLVED_DEPENDENCIES_VAR unresolved
    CONFLICTING_DEPENDENCIES_PREFIX conflicts)
if(unresolved OR conflicts_FILENAMES)
    message(FATAL_ERROR "Runtime dependencies unresolved=[${unresolved}], conflicting=[${conflicts_FILENAMES}]")
endif()
list(JOIN resolved "\n" lines)
file(WRITE "${OUTPUT_FILE}" "${lines}\n")
