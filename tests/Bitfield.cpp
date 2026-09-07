// SPDX-License-Identifier: GPL-3.0-or-later
#include <stdio.h>
#include <string.h>
#include "NonStupidBitfield.h"

using namespace melonDS;

static bool EmptyRanges()
{
    NonStupidBitField<128> bits;
    for (u32 start : {0u, 1u, 63u, 64u, 127u, 128u})
    {
        bits.SetRange(start, 0);
        if (bits || bits.CheckRange(start, 0))
            return false;
    }
    bits.SetRange(0, 128);
    for (u32 start : {0u, 1u, 63u, 64u, 127u, 128u})
    {
        bits.SetRange(start, 0);
        if (bits.CheckRange(start, 0) || bits.Min() != 0 || bits.Max() != 127)
            return false;
    }
    return true;
}

static bool Padding()
{
    NonStupidBitField<65> bits;
    bits.Data[1] = ~u64{0};
    auto it = bits.Begin();
    if (it == bits.End() || *it != 64)
        return false;
    ++it;
    if (it != bits.End())
        return false;
    bits.Data[0] = 1;
    bits.Data[1] = ~u64{1}; // Padding only in the final word.
    auto second = bits.Begin();
    if (*second != 0)
        return false;
    ++second;
    return second == bits.End();
}

template<u32 Size>
static bool Ranges()
{
    for (u32 start = 0; start < Size; ++start)
    {
        for (u32 count = 1; count <= Size - start; ++count)
        {
            NonStupidBitField<Size> bits(start, count);
            if (bits.Min() != static_cast<int>(start) ||
                bits.Max() != static_cast<int>(start + count - 1))
                return false;
            u32 expected = start;
            for (auto it = bits.Begin(); it != bits.End(); ++it)
            {
                if (*it != expected++ || expected > start + count)
                    return false;
            }
            if (expected != start + count || !bits.CheckRange(start, count))
                return false;
            if (start && bits.CheckRange(0, start))
                return false;
            if (start + count < Size && bits.CheckRange(start + count, Size - start - count))
                return false;
        }
    }
    return true;
}

static bool SparseAndDense()
{
    u64 random = 0x243f6a8885a308d3ULL;
    for (u32 sample = 0; sample < 128; ++sample)
    {
        NonStupidBitField<1024> bits;
        for (u64& word : bits.Data)
        {
            random ^= random << 13;
            random ^= random >> 7;
            random ^= random << 17;
            word = sample == 0 ? 0 : sample == 1 ? ~u64{0} : random;
        }
        u32 next = 0;
        for (auto it = bits.Begin(); it != bits.End(); ++it)
        {
            while (next < 1024 && !bits[next])
                ++next;
            if (next == 1024 || *it != next++)
                return false;
        }
        while (next < 1024 && !bits[next])
            ++next;
        if (next != 1024)
            return false;
    }
    return true;
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "all";
    bool passed = true;
    if (!strcmp(mode, "empty") || !strcmp(mode, "all"))
        passed = EmptyRanges() && passed;
    if (!strcmp(mode, "padding") || !strcmp(mode, "all"))
        passed = Padding() && passed;
    if (!strcmp(mode, "ranges") || !strcmp(mode, "all"))
        passed = Ranges<1>() && Ranges<65>() && Ranges<128>() && SparseAndDense() && passed;
    if (strcmp(mode, "all") && strcmp(mode, "empty") && strcmp(mode, "padding") && strcmp(mode, "ranges"))
        return 2;
    printf("bitfield %s: %s\n", mode, passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
