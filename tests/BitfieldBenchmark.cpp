// SPDX-License-Identifier: GPL-3.0-or-later
// Build the same file against the before/after headers; compare equal checksums.
#include <chrono>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "NonStupidBitfield.h"

using namespace melonDS;
using Field = NonStupidBitField<1024>;

int main(int argc, char** argv)
{
    if (argc != 2)
        return 2;
    Field fields[128];
    u64 random = 0x243f6a8885a308d3ULL;
    for (Field& field : fields)
    {
        for (u64& word : field.Data)
        {
            random ^= random << 13;
            random ^= random >> 7;
            random ^= random << 17;
            if (!strcmp(argv[1], "empty")) word = 0;
            else if (!strcmp(argv[1], "sparse")) word = 1ULL << (random & 63);
            else if (!strcmp(argv[1], "mixed")) word = random;
            else if (!strcmp(argv[1], "dense")) word = ~u64{0};
            else return 2;
        }
    }
    u64 checksum = 0;
    constexpr unsigned iterations = 1024;
    auto start = std::chrono::steady_clock::now();
    for (unsigned repeat = 0; repeat < iterations; ++repeat)
    {
        // Vary input each pass so the scan cannot be hoisted out of this loop.
        fields[repeat % 128].Data[(repeat / 128) % 16] ^= 1ULL << (repeat & 63);
        for (Field& field : fields)
            for (auto it = field.Begin(); it != field.End(); ++it)
                checksum += *it;
    }
    auto elapsed = std::chrono::steady_clock::now() - start;
    double ns = std::chrono::duration<double, std::nano>(elapsed).count();
    printf("%s,%.0f,%llu\n", argv[1], ns, static_cast<unsigned long long>(checksum));
}
