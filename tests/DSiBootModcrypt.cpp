// SPDX-License-Identifier: GPL-3.0-or-later
// Synthetic headers and OpenSSL-generated vectors; no private ROM/BIOS inputs.
#include "Args.h"
#include "DSi.h"
#include "NDSCart.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

using namespace melonDS;

int DSiBootMetadata(const char* mode);

static std::array<u8, 32> Unhex(const char* hex)
{
    std::array<u8, 32> bytes{};
    for (unsigned i = 0; i < bytes.size(); ++i)
    {
        unsigned value;
        std::sscanf(hex + 2*i, "%2x", &value);
        bytes[i] = value;
    }
    return bytes;
}

static int Modcrypt()
{
    // The public TWL contract treats key/IV and each data block as LE u128.
    // https://github.com/TuxSH/twlnandtool/blob/master/source/crypto/crypto_twl.hpp
    // Plaintext = 00..1f; header[0:16] = "BOOT VECTOR!TST0";
    // keyY = 80..8f; IV1 = f0..ff; IV2 = 20..2f.
    // Independently calculated using Python integer XOR/add/ROL42, then:
    // openssl enc -aes-128-ctr -K <key> -iv <reversed IV>
    // Reverse each 16-byte input/output block around OpenSSL.
    // Dev key: 3054535421524f5443455620544f4f42
    // Retail:  df74d44d2bed0448f09d1f6f73542b94
    const std::array<std::array<u8, 32>, 2> dev = {
        Unhex("77c5f3564df80d2e074aff44e0112f3b21a35d7f22d8a1d2974ec34f5ddfc400"),
        Unhex("7dc9ec4636836fae5759fdebd02fad63a80c9ada850118179e72033716bb5e36")};
    const std::array<std::array<u8, 32>, 2> retail = {
        Unhex("bc90c397f08e210ee418226b0647d5c8dd4fecabeb56e7ce1de5f320dfce75a2"),
        Unhex("62090d2cb2249cd0a8f75b8c1fbd3c3acb9e702b5bfc11fe994c515b781e6e7d")};
    const u32 romOffsets[] = {0x10000, 0x11000, 0x12000, 0x13000};
    const u32 ramAddresses[] = {0x02010000, 0x02011000, 0x02012000, 0x02013000};
    const char* modes[] = {"retail", "dev-app", "dev-debugger"};
    auto dsi = std::make_unique<DSi>(DSiArgs{});
    int failures = 0;
    for (unsigned mode = 0; mode < 3; ++mode)
    for (int binary = -1; binary < 4; ++binary)
    {
        // Each key gets a binary-base control, then two disjoint subareas in
        // each of the four permitted binaries. Gaps and other binaries survive.
        const unsigned selected = binary < 0 ? 2 : binary;
        const unsigned start = binary < 0 ? 0 : 0x20;
        std::vector<u8> rom(0x20000);
        NDSHeader header{};
        std::memcpy(header.GameTitle, "BOOT VECTOR!", 12);
        std::memcpy(header.GameCode, "TST0", 4);
        header.UnitCode = 2;
        header.DSiRegionMask = RegionFree;
        header.DSiCryptoFlags = 3 | (mode == 2 ? 4 : 0);
        header.AppFlags = mode == 1 ? 0x80 : 0;
        header.ARM9ROMOffset = romOffsets[0];
        header.ARM7ROMOffset = romOffsets[1];
        header.DSiARM9iROMOffset = romOffsets[2];
        header.DSiARM7iROMOffset = romOffsets[3];
        header.ARM9RAMAddress = header.ARM9EntryAddress = ramAddresses[0];
        header.ARM7RAMAddress = header.ARM7EntryAddress = ramAddresses[1];
        header.DSiARM9iRAMAddress = ramAddresses[2];
        header.DSiARM7iRAMAddress = ramAddresses[3];
        header.ARM9Size = header.ARM7Size = header.DSiARM9iSize = header.DSiARM7iSize = 0x80;
        header.DSiModcrypt1Offset = romOffsets[selected] + start;
        header.DSiModcrypt1Size = 32;
        header.DSiModcrypt2Offset = romOffsets[selected] + 0x60;
        header.DSiModcrypt2Size = binary < 0 ? 0 : 32;
        for (unsigned i = 0; i < 16; ++i)
        {
            header.DSiARM9iHash[i] = 0x80 + i;
            header.DSiARM9Hash[i] = 0xF0 + i;
            header.DSiARM7Hash[i] = 0x20 + i;
        }
        std::memcpy(rom.data(), &header, sizeof(header));
        for (u32 offset : romOffsets)
            std::fill_n(rom.data() + offset, 0x80, 0xA5);
        auto expected = rom;
        const auto& vectors = mode == 0 ? retail : dev;
        for (unsigned area = 0; area < (binary < 0 ? 1u : 2u); ++area)
        {
            const u32 offset = area == 0 ? header.DSiModcrypt1Offset : header.DSiModcrypt2Offset;
            std::copy(vectors[area].begin(), vectors[area].end(), rom.begin() + offset);
            for (unsigned i = 0; i < 32; ++i) expected[offset+i] = i;
        }
        auto cart = std::make_unique<NDSCart::CartCommon>(rom.data(), rom.size(), 0x400000C2,
            false, ROMListEntry{}, NDSCart::Default, nullptr);
        dsi->SetNDSCart(std::move(cart));
        dsi->Reset();
        if (!dsi->SetupDirectBoot()) return 2; // Real DSi loading/decryption and guest memory access.
        unsigned mismatches = 0;
        for (unsigned b = 0; b < 4; ++b)
        for (unsigned i = 0; i < 0x80; ++i)
            mismatches += dsi->ARM9Read8(ramAddresses[b] + i) != expected[romOffsets[b] + i];
        std::printf("%s %s%d: %u mismatched bytes\n", modes[mode],
            binary < 0 ? "base-control " : "subareas binary ", selected, mismatches);
        failures += mismatches != 0;
    }
    return failures ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    if (std::strcmp(argv[1], "modcrypt") == 0) return Modcrypt();
    return DSiBootMetadata(argv[1]);
}
