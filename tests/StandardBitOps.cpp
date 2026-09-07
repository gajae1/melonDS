// SPDX-License-Identifier: GPL-3.0-or-later
#include "ARM.h"
#include "Utils.h"
#include <cstdio>
#include <limits>
#include <type_traits>

using namespace melonDS;

#ifdef REQUIRE_CONSTEXPR
static_assert(ROR(0x12345678, 4) == 0x81234567);
static_assert(ROR(0x12345678, 32) == 0x12345678);
static_assert(GetMSBit(u32{0}) == 0);
static_assert(GetMSBit(u64{0xFFFFFFFFFFFFFFFFULL}) == 0x8000000000000000ULL);
#endif

template <typename T>
static bool CheckFloor(T value)
{
    T expected = 0;
    for (unsigned i = 0; i < std::numeric_limits<T>::digits; ++i)
        if (value & (T{1} << i)) expected = T{1} << i;
    return GetMSBit(value) == expected;
}

int main()
{
    for (u32 value = 0; value <= 0xFFFF; ++value)
    {
        if (!CheckFloor(static_cast<u16>(value)) || !CheckFloor(static_cast<u8>(value))) return 1;
    }
    u64 state = 0xBAD5EED123456789ULL;
    for (unsigned i = 0; i < 20000; ++i)
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        if (!CheckFloor(state) || !CheckFloor(static_cast<u32>(state))) return 2;
        for (u32 amount : {0U, 1U, 15U, 31U, 32U, 33U, 64U, 255U, 0x80000000U, 0xFFFFFFFFU})
        {
            u32 expected = static_cast<u32>(state);
            for (u32 bit = 0; bit < (amount & 31); ++bit)
                expected = (expected >> 1) | (expected << 31);
            if (ROR(static_cast<u32>(state), amount) != expected) return 3;
        }
    }
    if (!CheckFloor(u64{0}) || !CheckFloor(u64{1} << 63)) return 4;
    puts("Standard rotate and highest-bit helpers: PASS");
}
