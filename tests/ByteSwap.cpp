// SPDX-License-Identifier: GPL-3.0-or-later
#include "DSi_AES.h"
#include <array>
#include <cstdio>

int main()
{
    for (unsigned source = 0; source < 32; ++source)
    for (unsigned destination = 0; destination < 32; ++destination)
    {
        std::array<melonDS::u8, 64> bytes;
        for (unsigned i = 0; i < bytes.size(); ++i) bytes[i] = i;
        auto expected = bytes;
        for (unsigned i = 0; i < 16; ++i) expected[destination+i] = bytes[source+15-i];
        melonDS::Bswap128(bytes.data()+destination, bytes.data()+source);
        if (bytes != expected) return 1;
    }
    puts("AES byte reversal: disjoint, unaligned and overlapping buffers PASS");
}
