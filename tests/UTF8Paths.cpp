// SPDX-License-Identifier: GPL-3.0-or-later
#include <filesystem>
#include <stdio.h>
#include <string.h>
#include "UTF8.h"

int main()
{
    const std::u8string names[] = {
        u8"", u8"saves/plain.sav", u8"\uD55C\uAE00/\uC800\uC7A5.sav",
        u8"\u65E5\u672C\u8A9E/\u30C6\u30B9\u30C8.nds",
        u8"\U0001F3AE/caf\u00E9.sav", std::u8string(512, u8'x')
    };
    for (const auto& name : names)
    {
        const std::filesystem::path path(name);
        const std::string bytes = melonDS::UTF8ToString(path.u8string());
        if (bytes.size() != name.size() || memcmp(bytes.data(), name.data(), name.size()) != 0)
            return 1;
        // Construct char8_t storage rather than aliasing a char buffer as char8_t.
        const std::u8string restored(bytes.begin(), bytes.end());
        if (std::filesystem::path(restored) != path)
            return 1;
    }
    const std::u8string withZero(u8"a\0b", 3);
    const std::string bytes = melonDS::UTF8ToString(withZero);
    if (bytes.size() != 3 || memcmp(bytes.data(), withZero.data(), 3) != 0)
        return 1;
    if (!melonDS::UTF8ToString({}).empty())
        return 1;
    puts("UTF-8 path bytes and round trips: PASS");
}
