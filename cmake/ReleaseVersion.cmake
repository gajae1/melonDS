# SPDX-License-Identifier: GPL-3.0-or-later

set(MELONDS_VERSION_DISPLAY "${melonDS_VERSION}")
string(LENGTH "${melonDS_VERSION_PATCH}" _melonds_patch_length)
if (_melonds_patch_length LESS 2)
    set(MELONDS_VERSION_DISPLAY "${melonDS_VERSION_MAJOR}.${melonDS_VERSION_MINOR}.0${melonDS_VERSION_PATCH}")
endif()

# RC numeric literals must be decimal, while the display preserves existing zeros.
set(MELON_RC_VERSION "")
foreach (_melonds_component MAJOR MINOR PATCH)
    string(REGEX REPLACE "^0+" "" _melonds_decimal "${melonDS_VERSION_${_melonds_component}}")
    if (_melonds_decimal STREQUAL "")
        set(_melonds_decimal "0")
    endif()
    string(LENGTH "${_melonds_decimal}" _melonds_digits)
    if (_melonds_digits GREATER 5 OR _melonds_decimal GREATER 65535)
        message(FATAL_ERROR
            "melonDS VERSION ${_melonds_component} '${melonDS_VERSION_${_melonds_component}}' exceeds the Windows resource WORD range (0..65535)")
    endif()
    string(APPEND MELON_RC_VERSION "${_melonds_decimal},")
endforeach()
string(APPEND MELON_RC_VERSION "0")

unset(_melonds_patch_length)
unset(_melonds_component)
unset(_melonds_decimal)
unset(_melonds_digits)
