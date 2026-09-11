# SPDX-License-Identifier: GPL-3.0-or-later
#
# Verifiable package source identity for melonDS.
#
# MELONDS_PACKAGE_IDENTITY=ON embeds a content identity of the current
# source tree into the binary through a generated header. The identity is
# recomputed on every build, before melonDS compiles, but the header is only
# rewritten when the identity changes, so documentation-only edits do not
# trigger rebuilds. The default OFF writes a static header with an empty
# identity through configure_file and requires neither Python nor Git.

option(MELONDS_PACKAGE_IDENTITY "Embed a verifiable source identity into the build" OFF)

set(MELONDS_PACKAGE_IDENTITY_DIR "${CMAKE_CURRENT_LIST_DIR}")
set(MELONDS_PACKAGE_IDENTITY_HEADER_DIR "${CMAKE_BINARY_DIR}/package-identity")
set(MELONDS_PACKAGE_IDENTITY_HEADER "${MELONDS_PACKAGE_IDENTITY_HEADER_DIR}/package_identity.h")
set(MELONDS_PACKAGE_IDENTITY_JSON "${MELONDS_PACKAGE_IDENTITY_HEADER_DIR}/package-source.json")

function(melonds_add_package_identity target)
    if (NOT MELONDS_PACKAGE_IDENTITY)
        set(MELONDS_PACKAGE_IDENTITY_SCHEMA 1)
        set(MELONDS_PACKAGE_IDENTITY_SOURCE_ID "")
        set(MELONDS_PACKAGE_IDENTITY_KIND "off")
        configure_file("${MELONDS_PACKAGE_IDENTITY_DIR}/package_identity.h.in"
                       "${MELONDS_PACKAGE_IDENTITY_HEADER}" @ONLY)
        configure_file("${MELONDS_PACKAGE_IDENTITY_DIR}/package-source.json.in"
                       "${MELONDS_PACKAGE_IDENTITY_JSON}" @ONLY)
        target_include_directories(${target} PRIVATE "${MELONDS_PACKAGE_IDENTITY_HEADER_DIR}")
        return()
    endif()

    find_package(Python3 3.11 REQUIRED COMPONENTS Interpreter)
    find_package(Git 2.43 REQUIRED)

    execute_process(
        COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_SOURCE_DIR}" rev-parse --is-inside-work-tree
        RESULT_VARIABLE _identity_worktree_result
        OUTPUT_QUIET
        ERROR_QUIET)
    if (NOT _identity_worktree_result EQUAL 0)
        message(FATAL_ERROR
            "MELONDS_PACKAGE_IDENTITY requires ${CMAKE_SOURCE_DIR} to be a Git worktree")
    endif()

    file(MAKE_DIRECTORY "${MELONDS_PACKAGE_IDENTITY_HEADER_DIR}")

    # Always runs before the target compiles; the script rewrites the header
    # only when the identity changes, so unchanged content does not rebuild.
    # BYPRODUCTS registers the header with the build graph so that changing
    # the identity re-compiles dependents, while unchanged content (restat)
    # keeps documentation-only edits from rebuilding anything.
    add_custom_target(melonds_package_identity ALL
        BYPRODUCTS "${MELONDS_PACKAGE_IDENTITY_HEADER}" "${MELONDS_PACKAGE_IDENTITY_JSON}"
        COMMAND "${Python3_EXECUTABLE}"
                "${MELONDS_PACKAGE_IDENTITY_DIR}/../tools/source_identity.py"
                generate
                --repo "${CMAKE_SOURCE_DIR}"
                --header "${MELONDS_PACKAGE_IDENTITY_HEADER}"
                --json "${MELONDS_PACKAGE_IDENTITY_JSON}"
        COMMENT "Computing melonDS package source identity"
        VERBATIM)
    add_dependencies(${target} melonds_package_identity)
    target_include_directories(${target} PRIVATE "${MELONDS_PACKAGE_IDENTITY_HEADER_DIR}")
endfunction()
