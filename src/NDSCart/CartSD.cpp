/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "CartSD.h"
#include "../NDS.h"
#include "../Utils.h"
#include <vector>

// CartSD: base class for homebrew cartridges with a SD card
// provides support for DLDI patching

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

namespace NDSCart
{

CartSD::CartSD(const u8* rom, u32 len, u32 chipid, ROMListEntry romparams, void* userdata, std::optional<FATStorage>&& sdcard) :
    CartSD(CopyToUnique(rom, len), len, chipid, romparams, userdata, std::move(sdcard))
{}

CartSD::CartSD(std::unique_ptr<u8[]>&& rom, u32 len, u32 chipid, ROMListEntry romparams, void* userdata, std::optional<FATStorage>&& sdcard) :
    CartCommon(std::move(rom), len, chipid, false, romparams, CartType::Homebrew, userdata),
    SD(std::move(sdcard))
{
    LenientAddressing = true;
    if (SD && !SD->IsValid()) SD.reset();

    sdcard = std::nullopt;
    // std::move on optionals usually results in an optional with a moved-from object
}

CartSD::~CartSD() = default;
// The SD card is destroyed by the optional's destructor


bool CartSD::ApplyDLDIPatchAt(u8* binary, u32 binarylen, u32 dldioffset, const u8* patch, u32 patchlen, bool readonly) const
{
    constexpr char magic[] = "\xED\xA5\x8D\xBF Chishm";
    const auto invalid = []()
    {
        Log(LogLevel::Error, "Invalid or truncated DLDI patch/target\n");
        return false;
    };
    if (!binary || !patch || patchlen < 0x80 || dldioffset > binarylen || binarylen - dldioffset < 0x80)
        return invalid();
    u8* target = binary + dldioffset;
    if (memcmp(patch, magic, sizeof(magic)) || memcmp(target, magic, sizeof(magic)) ||
        patch[0x0C] != 1 || target[0x0C] != 1 || (patch[0x0E] & ~0x0F) ||
        patch[0x0D] < 7 || patch[0x0D] >= 32 || target[0x0F] < 7 || target[0x0F] >= 32)
        return invalid();

    const u32 patchsize = 1U << patch[0x0D];
    const u32 capacity = 1U << target[0x0F];
    // Driver size is reserved memory, not file length (melonDLDI is 548/1024).
    if (patchsize > capacity || patchlen > patchsize || capacity > binarylen - dldioffset)
        return invalid();
    const auto read32 = [](const u8* data) { u32 value; memcpy(&value, data, sizeof(value)); return value; };
    const auto write32 = [](u8* data, u32 value) { memcpy(data, &value, sizeof(value)); };
    const u32 patchbase = read32(patch + 0x40);
    u32 memaddr = read32(target + 0x40);
    if (!memaddr)
    {
        const u32 startup = read32(target + 0x68);
        if (startup < 0x80) return invalid();
        memaddr = startup - 0x80;
    }
    if ((patchbase & 3) || (memaddr & 3) || u64(patchbase) + patchsize > 0x100000000ULL ||
        u64(memaddr) + patchsize > 0x100000000ULL)
        return invalid();

    struct { u32 start = 0, end = 0; } sections[4];
    const u8 fixmask = patch[0x0E];
    for (u32 section = 0; section < 4; ++section)
    {
        const u32 start = read32(patch + 0x40 + section * 8);
        const u32 end = read32(patch + 0x44 + section * 8);
        if (section && !start && !end) continue; // Absent optional section.
        if (start < patchbase || end < start || end - patchbase > (section == 3 ? patchsize : patchlen))
            return invalid();
        auto& range = sections[section];
        range.start = start - patchbase;
        range.end = end - patchbase;
        if (section < 3 && (fixmask & (1U << section)) && ((range.start | range.end) & 3))
            return invalid(); // Every relocation read/write must contain a whole word.
        if (section && range.start != range.end && range.start < 0x80)
            return invalid();
    }
    if (sections[0].end < 0x80 ||
        (sections[3].start != sections[3].end && sections[3].start < sections[0].end))
        return invalid();
    for (u32 address = 0x68; address <= 0x7C; address += 4)
    {
        const u32 entry = read32(patch + address);
        if (!entry) continue;
        const u32 code = entry & ~1U;
        const u32 width = (entry & 1) ? 2 : 4;
        if (code < patchbase || code - patchbase < 0x80 || (code & (width - 1)) ||
            code - patchbase > sections[0].end || width > sections[0].end - (code - patchbase))
            return invalid();
    }
    u32 writesec = 0;
    if (readonly)
    {
        const u32 entry = read32(patch + 0x74);
        // This replacement is two ARM instructions, not a Thumb entry point.
        if ((entry & 3) || entry < patchbase || entry - patchbase < 0x80 ||
            entry - patchbase > sections[0].end || 8 > sections[0].end - (entry - patchbase))
            return invalid();
        writesec = entry - patchbase;
    }

    // All source, destination and fixup spans are valid. Prepare before commit;
    // use original words so overlapping fix ranges never relocate twice.
    Log(LogLevel::Info, "existing driver is: %.48s\n", &target[0x10]);
    Log(LogLevel::Info, "new driver is: %.48s\n", &patch[0x10]);
    std::vector<u8> relocated(patch, patch + patchlen);
    const u32 delta = memaddr - patchbase;
    const auto relocate = [&](u32 address) { write32(relocated.data() + address, read32(patch + address) + delta); };
    for (u32 address = 0x40; address <= 0x5C; address += 4) relocate(address);
    for (u32 address = 0x68; address <= 0x7C; address += 4) relocate(address);
    for (u32 section = 0; section < 3; ++section)
    {
        if (!(fixmask & (1U << section))) continue;
        for (u32 address = sections[section].start; address < sections[section].end; address += 4)
        {
            const u32 value = read32(patch + address);
            if (value >= patchbase && u64(value) < u64(patchbase) + patchsize) relocate(address);
        }
    }
    if (readonly)
    {
        relocated[0x64] &= ~0x02; // clear can-write
        write32(relocated.data() + writesec, 0xE3A00000); // mov r0, #0
        write32(relocated.data() + writesec + 4, 0xE12FFF1E); // bx lr
    }
    relocated[0x0F] = target[0x0F]; // Keep the slot available for subsequent resets.
    memcpy(target, relocated.data(), patchlen);
    if (fixmask & 0x08)
        memset(target + sections[3].start, 0, sections[3].end - sections[3].start);

    Log(LogLevel::Debug, "applied DLDI patch at %08X\n", dldioffset);
    return true;
}

void CartSD::ApplyDLDIPatch(const u8* patch, u32 patchlen, bool readonly)
{
    constexpr char magic[] = "\xED\xA5\x8D\xBF Chishm";
    if (!patch || patchlen < 0x80 || memcmp(patch, magic, sizeof(magic)) || patch[0x0C] != 1)
    {
        Log(LogLevel::Error, "bad DLDI patch\n");
        return;
    }

    if (!ROM.get() || ROMLength < 0x30) return;
    u32 offset, size;
    memcpy(&offset, ROM.get() + 0x20, sizeof(offset));
    memcpy(&size, ROM.get() + 0x2C, sizeof(size));
    if (offset < 0x200 || offset > ROMLength || size > ROMLength - offset)
    {
        Log(LogLevel::Error, "DLDI ARM9 binary is outside the ROM\n");
        return;
    }
    if (size < 0x80) return;

    u8* binary = &ROM[offset];

    for (u32 i = 0; i <= size - 0x80; )
    {
        if (!memcmp(binary + i, magic, sizeof(magic)) &&
            ApplyDLDIPatchAt(binary, size, i, patch, patchlen, readonly))
        {
            Log(LogLevel::Debug, "DLDI structure found at %08X (%08X)\n", i, offset+i);
            i += patchlen;
        }
        else
            i++;
    }
}


}

}
