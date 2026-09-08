// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MELONDS_UTF8_H
#define MELONDS_UTF8_H

#include <string>
#include <string_view>
#include <filesystem>

namespace melonDS
{
// Platform file APIs use UTF-8 bytes in std::string, not the native code page.
inline std::string UTF8ToString(std::u8string_view text)
{
    if (text.empty())
        return {};
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

inline std::filesystem::path PathFromUTF8(std::string_view text)
{
    // Own char8_t storage; do not alias a char buffer as char8_t.
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}
}

#endif // MELONDS_UTF8_H
