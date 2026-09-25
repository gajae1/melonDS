# melonDS uses std::stop_token/std::jthread in the core and Qt frontend.
# libc++ older than 20 only exposes them when the compiler driver is told to
# enable the experimental library, so probe for the types and turn that flag on
# when it is the only way to get them. Where libc++experimental itself is not
# installed (FreeBSD/OpenBSD base), the header-only types are still reachable
# through _LIBCPP_ENABLE_EXPERIMENTAL, which is tried last.
include(CheckCXXSourceCompiles)
# std::jthread needs the thread library at link time (FreeBSD libthr).
find_package(Threads REQUIRED)
set(_saved_required_libraries "${CMAKE_REQUIRED_LIBRARIES}")
list(APPEND CMAKE_REQUIRED_LIBRARIES Threads::Threads)

set(_stop_token_source "
#include <stop_token>
#include <thread>

int main()
{
    std::stop_source source;
    std::stop_token token = source.get_token();
    std::jthread worker{[] (std::stop_token) {}};
    return token.stop_requested() ? 0 : 1;
}
")

check_cxx_source_compiles("${_stop_token_source}" MELONDS_HAS_STD_STOP_TOKEN)

if (NOT MELONDS_HAS_STD_STOP_TOKEN)
    # The probe is linked as well, so this also proves that the runtime library
    # the flag pulls in (libc++experimental) is actually available; otherwise
    # the build flags stay untouched and the toolchain error is unchanged.
    set(_saved_required_flags "${CMAKE_REQUIRED_FLAGS}")
    set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -fexperimental-library")
    check_cxx_source_compiles("${_stop_token_source}" MELONDS_HAS_STD_STOP_TOKEN_EXPERIMENTAL_LIBRARY)
    set(CMAKE_REQUIRED_FLAGS "${_saved_required_flags}")

    if (MELONDS_HAS_STD_STOP_TOKEN_EXPERIMENTAL_LIBRARY)
        message(STATUS "std::stop_token requires -fexperimental-library on this toolchain: enabling it for compile and link")
        add_compile_options("$<$<COMPILE_LANGUAGE:CXX>:-fexperimental-library>")
        add_link_options(-fexperimental-library)
    else()
        set(_saved_required_definitions "${CMAKE_REQUIRED_DEFINITIONS}")
        list(APPEND CMAKE_REQUIRED_DEFINITIONS -D_LIBCPP_ENABLE_EXPERIMENTAL)
        check_cxx_source_compiles("${_stop_token_source}" MELONDS_HAS_STD_STOP_TOKEN_EXPERIMENTAL_MACRO)
        set(CMAKE_REQUIRED_DEFINITIONS "${_saved_required_definitions}")
        if (MELONDS_HAS_STD_STOP_TOKEN_EXPERIMENTAL_MACRO)
            message(STATUS "std::stop_token requires _LIBCPP_ENABLE_EXPERIMENTAL on this toolchain: defining it")
            add_compile_definitions(_LIBCPP_ENABLE_EXPERIMENTAL)
        endif()
    endif()
endif()

unset(_stop_token_source)
set(CMAKE_REQUIRED_LIBRARIES "${_saved_required_libraries}")
unset(_saved_required_libraries)
unset(_saved_required_flags)
unset(_saved_required_definitions)
