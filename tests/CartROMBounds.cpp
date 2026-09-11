// SPDX-License-Identifier: GPL-3.0-or-later
#include "NDS.h"
#include "GBACart.h"
#include <array>
#include <cstdio>
#include <memory>
#include <cstring>
#include <vector>
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

    // A trimmed ROM may omit unused padding, but not bytes declared as executable.
    for (bool arm9 : {false, true})
    for (u32 executableSize : {1U, 2U})
    {
        std::vector<u8> rom(0x1001, 0x5A);
        NDSHeader header{};
        std::memcpy(header.GameCode, "####", 4);
        header.ARM9ROMOffset = header.ARM7ROMOffset = 0x1000;
        header.ARM9Size = header.ARM7Size = 1;
        (arm9 ? header.ARM9Size : header.ARM7Size) = executableSize;
        std::memcpy(rom.data(), &header, sizeof(header));
        auto borrowed = NDSCart::ParseROM(rom.data(), static_cast<u32>(rom.size()));
        auto owned = std::make_unique<u8[]>(rom.size());
        std::memcpy(owned.get(), rom.data(), rom.size());
        auto moved = NDSCart::ParseROM(std::move(owned), static_cast<u32>(rom.size()));
        const bool valid = executableSize == 1;
        if (bool(borrowed) != valid || bool(moved) != valid)
        {
            std::fprintf(stderr, "NDS ARM%d declared size %u accepted=%d/%d expected=%d\n",
                arm9 ? 9 : 7, executableSize, bool(borrowed), bool(moved), valid);
            return 6;
        }
        if (valid && (borrowed->GetROM()[0x1000] != 0x5A || moved->GetROM()[0x1000] != 0x5A))
            return 7;
    }
    puts("NDS trimmed executable bounds in borrowed and owned input: PASS");
}
