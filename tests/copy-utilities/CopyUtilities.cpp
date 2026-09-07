// SPDX-License-Identifier: GPL-3.0-or-later
#include "Utils.h"
#include <cstdio>
#include <cstring>
#include <vector>

int main()
{
    using namespace melonDS;
    std::vector<u8> input(4097);
    for (size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<u8>(i * 73 + 19);
    for (u32 length = 0; length <= input.size(); ++length)
    {
        auto copy = CopyToUnique(input.data(), length);
        if (length == 0)
        {
            if (copy) return 1;
        }
        else
        {
            if (!copy || copy.get() == input.data() ||
                std::memcmp(copy.get(), input.data(), length) != 0) return 2;
            copy[0] ^= 0xFF;
            if (input[0] != 19) return 3;
        }
    }
    if (CopyToUnique(nullptr, 0) || CopyToUnique(nullptr, 12)) return 4;
    puts("CopyToUnique: contents, ownership, null and empty inputs PASS");
}
