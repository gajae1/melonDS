// SPDX-License-Identifier: GPL-3.0-or-later
#include "Utils.h"
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

using namespace melonDS;

static int CheckCopies()
{
    std::vector<u8> source(4097);
    for (size_t i = 0; i < source.size(); ++i)
        source[i] = static_cast<u8>(i * 73 + 19);
    for (u32 len = 1; len <= source.size(); ++len)
    {
        u32 expected = 1;
        while (expected < len) expected *= 2;
        auto [raw, rawSize] = PadToPowerOf2(source.data(), len);
        auto owned = CopyToUnique(source.data(), len);
        const auto original = owned.get();
        auto [moved, movedSize] = PadToPowerOf2(std::move(owned), len);
        if (!raw || !moved || owned || rawSize != expected || movedSize != expected)
            return 1;
        if (len == expected && moved.get() != original) return 2;
        if (std::memcmp(raw.get(), source.data(), len) != 0 ||
            std::memcmp(moved.get(), source.data(), len) != 0) return 3;
        for (u32 i = len; i < expected; ++i)
            if (raw[i] != 0 || moved[i] != 0) return 4;
    }
    return 0;
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "copies";
    if (std::strcmp(mode, "overflow") == 0)
    {
        // Rejected before allocation or reads; source deliberately holds one byte.
        const u8 byte = 0xAB;
        for (u32 len : {0x80000001U, std::numeric_limits<u32>::max()})
        {
            auto [result, size] = PadToPowerOf2(&byte, len);
            if (result || size != 0) return 5;
            auto owned = std::make_unique<u8[]>(1);
            auto [moved, movedSize] = PadToPowerOf2(std::move(owned), len);
            if (moved || movedSize != 0 || !owned) return 6;
        }
    }
    else if (std::strcmp(mode, "null") == 0)
    {
        auto [result, size] = PadToPowerOf2(static_cast<const u8*>(nullptr), 12);
        if (result || size != 0) return 7;
        auto [empty, emptySize] = PadToPowerOf2(static_cast<const u8*>(nullptr), 0);
        if (empty || emptySize != 0 || CopyToUnique(nullptr, 12)) return 8;
    }
    else if (std::strcmp(mode, "copies") == 0)
    {
        if (const int result = CheckCopies()) return result;
    }
    else return 9;
    std::printf("ROM buffer %s: PASS\n", mode);
}
