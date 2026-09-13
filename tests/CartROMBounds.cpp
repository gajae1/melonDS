// SPDX-License-Identifier: GPL-3.0-or-later
#include "NDS.h"
#include "GBACart.h"
#include "NDSCart/CartHomebrew.h"
#include "NDSCart/CartR4.h"
#include "NDSCart/CartRetailNAND.h"
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

    // Public factory callers must get the same override validation as the UI.
    std::vector<u8> retail(0x8000);
    auto* header = reinterpret_cast<NDSHeader*>(retail.data());
    header->ARM9ROMOffset = 0x4000; header->ARM7ROMOffset = 0x4004;
    header->ARM9Size = header->ARM7Size = 4;
    std::memcpy(header->GameCode, "ZZZA", 4);
    for (u32 invalid : {8u, 10u, 18u, UINT32_MAX})
    {
        NDSCart::NDSCartArgs args; args.SPISaveType = invalid;
        if (NDSCart::ParseROM(retail.data(), retail.size(), nullptr, std::move(args))) return 8;
        auto owned = std::make_unique<u8[]>(retail.size());
        std::memcpy(owned.get(), retail.data(), retail.size());
        auto* original = owned.get();
        args.SPISaveType = invalid;
        if (NDSCart::ParseROM(std::move(owned), retail.size(), nullptr, std::move(args)) ||
            owned.get() != original) return 9;
    }
    for (u32 type : {0u, 1u, 7u, 11u, 12u, 13u, 14u, 15u, 16u, 17u})
    {
        NDSCart::NDSCartArgs args; args.SPISaveType = type;
        auto cart = NDSCart::ParseROM(retail.data(), retail.size(), nullptr, std::move(args));
        if (!cart || cart->Type() != NDSCart::CartType::Retail || cart->GetROMParams().SaveMemType != type) return 10;
    }
    for (const char* code : {"####", "ASMA", "UAMA"})
    {
        std::memcpy(header->GameCode, code, 4);
        std::memset(header->GameTitle, 0, sizeof(header->GameTitle));
        if (!std::strcmp(code, "ASMA")) std::memcpy(header->GameTitle + 1, "SD/TF-NDS", 9);
        auto automatic = NDSCart::ParseROM(retail.data(), retail.size());
        // R4 retains CartSD's historical Homebrew state tag. Check the actual
        // factory class rather than assuming the unused UnlicensedR4 tag.
        const bool family = !std::strcmp(code, "####") ? dynamic_cast<NDSCart::CartHomebrew*>(automatic.get()) != nullptr :
            !std::strcmp(code, "ASMA") ? dynamic_cast<NDSCart::CartR4*>(automatic.get()) != nullptr :
            dynamic_cast<NDSCart::CartRetailNAND*>(automatic.get()) != nullptr;
        if (!family) return 11;
        for (u32 type : {0u, 2u, 7u, 11u, 14u, 15u, 16u, 17u})
        {
            NDSCart::NDSCartArgs args; args.SPISaveType = type;
            if (NDSCart::ParseROM(retail.data(), retail.size(), nullptr, std::move(args))) return 12;
        }
    }
    for (const char* code : {"IZZZ", "UZPZ"})
    for (u32 type : {1u, 11u, 14u, 15u, 16u, 17u})
    {
        std::memcpy(header->GameCode, code, 4);
        NDSCart::NDSCartArgs args; args.SPISaveType = type;
        auto cart = NDSCart::ParseROM(retail.data(), retail.size(), nullptr, std::move(args));
        const auto expected = code[0] == 'I' ? NDSCart::CartType::RetailIR : NDSCart::CartType::RetailBT;
        if (!cart || cart->Type() != expected || cart->GetROMParams().SaveMemType != type) return 13;
    }
    puts("DS manual SPI factory: supported standard/IR/BT overrides, invalid values, NAND/homebrew/R4 family guards: PASS");
}
