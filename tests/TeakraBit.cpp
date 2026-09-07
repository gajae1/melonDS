// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdint.h>
#include <stdio.h>
#include "teakra/src/bit.h"
template<class T> static unsigned Reference(T x)
{
    unsigned result = 0;
    while (x) { ++result; x >>= 1; }
    return result;
}
int main()
{
    static_assert(std20::log2p1(uint64_t{0}) == 0);
    static_assert(std20::log2p1(uint64_t{1} << 63) == 64);
    static_assert(std20::log2p1(true) == 1);
    for (uint32_t i = 0; i <= 65535; ++i)
        if (std20::log2p1(static_cast<uint16_t>(i)) != Reference(i)) return 1;
    uint64_t x = 0x1020304050607080ULL;
    for (unsigned i = 0; i < 100000; ++i)
    {
        x ^= x << 13; x ^= x >> 7; x ^= x << 17;
        if (std20::log2p1(x) != Reference(x)) return 1;
    }
    puts("Teakra bit width: constexpr, bool, 65536 u16 and 100000 u64 cases PASS");
}
