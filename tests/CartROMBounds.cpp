// SPDX-License-Identifier: GPL-3.0-or-later
#include "NDS.h"
#include "GBACart.h"
#include <array>
#include <cstdio>
#include <memory>
using namespace melonDS;
int main()
{
    std::array<u8, 0x200> data{};
    // Padding 0xAF bytes to 0x100 must not make a missing game-code byte valid.
    if (GBACart::ParseROM(data.data(), 0xAF, nullptr)) return 1;
    for (u32 size : {0U, 1U, 0x80U, 0xACU, 0xAFU, 0x80000001U, 0xFFFFFFFFU})
    {
        if (GBACart::ParseROM(data.data(), size, nullptr)) return 2;
        auto owned = std::make_unique<u8[]>(data.size());
        if (GBACart::ParseROM(std::move(owned), size, nullptr)) return 3;
    }
    if (GBACart::ParseROM(static_cast<const u8*>(nullptr), 0x200, nullptr)) return 4;
    for (u32 size : {0xB0U, 0xC0U, 0x100U, 0x200U})
        if (!GBACart::ParseROM(data.data(), size, nullptr)) return 5;
    puts("GBA original length, overflow, null and minimal accepted input: PASS");
}
