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

#include <cstring>
#include <algorithm>
#include <memory>
#include "ARDatabaseDAT.h"
#include "Platform.h"

/* FILE FORMAT for usrcheat.dat

 header:
 00: ID string "R4 CheatCode" (12 bytes)
 0C: version? must be 0x100
 10: database description
 4C: ??? (gets set by r4cce.exe, but zero in usrcheat.dat)
 50: database active flag
 100: entry list

 entry:
 00: game code
 04: checksum of ROM header (logical NOT of CRC32 over first 512 bytes)
 08: offset to cheat data (absolute)
 0C: zero

 the entry list is terminated with an all-zero entry

 cheat data: header then items

 header:
 * game name (null-term string)
 * padding (to align entry below to 4-byte boundary)
 * flags (word): bit0-27 = total number of items, bit28-31 = game active (0=inactive, F=active)
 * master codes (8 words) - use unclear

 item:
 * flags (word): bit0-23 = data length, bit24 = flag, bit28 = type
 * item name (null-term string)
 * item description (null-term string)
 * padding (to align data below to 4-byte boundary)
 * item data

 meaning of item type/flag:
 * type=0: cheat, bit0-23 = total data length in words, bit24 = cheat active
 * type=1: category, bit0-23 = number of cheats inside, bit24 = only one cheat may be active in this category

 empty description still takes up one extra byte (for the null terminator)

 for a category: item data is a list of cheat items

 for a cheat: item data is as follows:
 * number of code words (word)
 * code
*/

namespace melonDS
{
using namespace Platform;

namespace
{
using DatabaseFile = std::unique_ptr<FileHandle, decltype(&CloseFile)>;

bool ReadData(FileHandle* f, void* data, u64 size, u64 end)
{
    const u64 pos = FilePosition(f);
    return pos <= end && size <= end - pos &&
           (size == 0 || FileRead(data, 1, size, f) == size);
}

bool ReadNTString(FileHandle* f, u64 end, std::string& text)
{
    text.clear();
    while (true)
    {
        const u64 pos = FilePosition(f);
        if (pos >= end) return false;

        char buffer[256];
        const u64 count = std::min<u64>(sizeof(buffer), end - pos);
        if (FileRead(buffer, 1, count, f) != count) return false;

        const char* terminator = static_cast<const char*>(memchr(buffer, 0, count));
        if (terminator)
        {
            const size_t length = terminator - buffer;
            text.append(buffer, length);
            return FileSeek(f, pos + length + 1, FileSeekOrigin::Start);
        }
        text.append(buffer, count);
    }
}

bool AlignFilePos(FileHandle* f, u64 end)
{
    const u64 pos = FilePosition(f);
    if (pos > end) return false;
    const u64 aligned = (pos + 3) & ~u64{3};
    return aligned <= end &&
           (aligned == pos || FileSeek(f, aligned, FileSeekOrigin::Start));
}
}


ARDatabaseDAT::ARDatabaseDAT(const std::string& filename)
{
    Filename = filename;

    if (!LoadEntries())
        Error = true;
}

bool ARDatabaseDAT::FindGameCode(u32 gamecode)
{
    auto it = EntryList.find(gamecode);
    if (it == EntryList.end())
        return false;
    if ((*it).second.empty())
        return false;
    return true;
}

ARDatabaseEntryList ARDatabaseDAT::GetEntriesByGameCode(u32 gamecode)
{
    ARDatabaseEntryList ret;
    auto it = EntryList.find(gamecode);
    if (it == EntryList.end())
        return ret;

    // Build in the final slots: Parent pointers must not target a temporary
    // entry, nor a root moved by vector growth during this lookup.
    ret.reserve(it->second.size());
    for (const auto& info : it->second)
    {
        ret.emplace_back();
        if (!LoadCheatCodes(info, ret.back()))
        {
            ret.pop_back();
            Error = true;
            Log(LogLevel::Error, "AR: failed to load game entry at offset %08X\n", info.Offset);
        }
    }

    return ret;
}

bool ARDatabaseDAT::LoadEntries()
{
    DatabaseFile file(OpenFile(Filename, FileMode::Read), CloseFile);
    FileHandle* f = file.get();
    if (!f) return false;

    const u64 filelen = FileLength(f);
    if (filelen < 0x110 || filelen > 0xFFFFFFFFULL)
        return false;

    char header[16];
    if (!ReadData(f, header, sizeof(header), filelen) ||
        memcmp(header, "R4 CheatCode\x00\x01\x00\x00", 16) != 0)
        return false;

    char name[0x3D] = {0};
    if (!ReadData(f, name, 0x3C, filelen) || !FileSeek(f, 0x100, FileSeekOrigin::Start))
        return false;

    std::vector<EntryInfo> entries;
    u64 indexEnd = filelen;
    bool terminated = false;
    for (u64 pos = 0x100; pos + 16 <= indexEnd; pos += 16)
    {
        u32 entrydata[4];
        if (!ReadData(f, entrydata, sizeof(entrydata), indexEnd)) return false;

        if (entrydata[0] == 0 && entrydata[1] == 0 && entrydata[2] == 0 && entrydata[3] == 0)
        {
            terminated = true;
            break;
        }

        if (!entrydata[0] || entrydata[3] || entrydata[2] < 0x100 || entrydata[2] >= filelen)
        {
            Log(LogLevel::Error, "AR: malformed database file (invalid offset %08X)\n", entrydata[2]);
            return false;
        }

        indexEnd = std::min<u64>(indexEnd, entrydata[2]);
        entries.push_back({entrydata[0], entrydata[1], entrydata[2], 0});
    }
    if (!terminated) return false;

    // Offset order need not match game-code order. Bound each payload by the
    // next distinct payload offset, without changing the index's display order.
    std::vector<u32> offsets;
    offsets.reserve(entries.size());
    for (const auto& entry : entries) offsets.push_back(entry.Offset);
    std::sort(offsets.begin(), offsets.end());
    decltype(EntryList) entryList;
    for (auto& entry : entries)
    {
        const auto next = std::upper_bound(offsets.begin(), offsets.end(), entry.Offset);
        entry.EndOffset = next == offsets.end() ? static_cast<u32>(filelen) : *next;
        entryList[entry.GameCode].push_back(entry);
    }
    EntryList = std::move(entryList);
    DBName = name;
    return true;
}

bool ARDatabaseDAT::LoadCheatCodes(const EntryInfo& info, ARDatabaseEntry& entry)
{
    DatabaseFile file(OpenFile(Filename, FileMode::Read), CloseFile);
    FileHandle* f = file.get();
    if (!f) return false;

    const u64 filelen = FileLength(f);
    if (filelen > 0xFFFFFFFFULL || info.Offset < 0x100 ||
        info.Offset >= info.EndOffset || info.EndOffset > filelen)
        return false;

    entry.GameCode = info.GameCode;
    entry.Checksum = info.Checksum;

    if (!FileSeek(f, info.Offset, FileSeekOrigin::Start) ||
        !ReadNTString(f, info.EndOffset, entry.Name) || !AlignFilePos(f, info.EndOffset))
        return false;

    u32 flags[9];
    if (!ReadData(f, flags, sizeof(flags), info.EndOffset)) return false;

    entry.RootCat.Parent = nullptr;
    entry.RootCat.OnlyOneCodeEnabled = false;
    entry.RootCat.Children.clear();

    ARCodeCat* curcat = &entry.RootCat;
    int catlen = 0;

    const u32 numentries = flags[0] & 0x0FFFFFFF;
    const u64 itemsStart = FilePosition(f);
    // Even a category needs its flags and two aligned string terminators.
    if (itemsStart > info.EndOffset || numentries > (info.EndOffset - itemsStart) / 8)
        return false;

    for (u32 i = 0; i < numentries; i++)
    {
        u32 itemflags;
        if (!ReadData(f, &itemflags, sizeof(itemflags), info.EndOffset)) return false;

        const u32 totallen = itemflags & 0xFFFFFF;
        u64 itemEnd = info.EndOffset;
        if (!(itemflags & (1 << 28)))
        {
            const u64 pos = FilePosition(f);
            const u64 bytes = u64{totallen} * 4;
            if (pos > info.EndOffset || bytes > info.EndOffset - pos) return false;
            itemEnd = pos + bytes;
        }

        std::string itemname, itemdesc;
        if (!ReadNTString(f, itemEnd, itemname) || !ReadNTString(f, itemEnd, itemdesc) ||
            !AlignFilePos(f, itemEnd))
            return false;

        if (itemflags & (1<<28))
        {
            // this item is a category

            if (catlen != 0 || totallen >= 0x10000 || totallen == 0 || totallen > numentries - i - 1)
            {
                Log(LogLevel::Error, "AR: unreasonable category length %08X\n",
                    totallen);
                Log(LogLevel::Error, "game=%s, offset=%08X, cat=%s\n",
                    entry.Name.c_str(), info.Offset, itemname.c_str());

                return false;
            }

            ARCodeCat cat = {
                    .Parent = &entry.RootCat,
                    .Name = itemname,
                    .Description = itemdesc,
                    .OnlyOneCodeEnabled = !!(itemflags & (1<<24)),
                    .Children = {}
            };
            entry.RootCat.Children.emplace_back(cat);
            curcat = &std::get<ARCodeCat>(entry.RootCat.Children.back());

            catlen = totallen;
        }
        else
        {
            // this item is a code

            u32 codelen;
            if (!ReadData(f, &codelen, sizeof(codelen), itemEnd)) return false;

            const u64 pos = FilePosition(f);
            if (pos > itemEnd || u64{codelen} * 4 != itemEnd - pos)
            {
                Log(LogLevel::Error, "AR: malformed code entry, codelen=%08X, totallen=%08X\n",
                    codelen, totallen);
                Log(LogLevel::Error, "game=%s, offset=%08X, cheat=%s\n",
                    entry.Name.c_str(), info.Offset, itemname.c_str());

                return false;
            }

            if ((codelen >= 0x100000) || (codelen & 1))
            {
                Log(LogLevel::Error, "AR: unreasonable code length %08X\n",
                    codelen);
                Log(LogLevel::Error, "game=%s, offset=%08X, cheat=%s\n",
                    entry.Name.c_str(), info.Offset, itemname.c_str());

                return false;
            }

            if (catlen == 0)
            {
                curcat = &entry.RootCat;
            }

            ARCode code {};
            code.Parent = curcat;
            code.Name = itemname;
            code.Description = itemdesc;
            code.Enabled = !!(itemflags & (1<<24));

            code.Code.resize(codelen);
            if (!ReadData(f, code.Code.data(), u64{codelen} * 4, itemEnd)) return false;
            curcat->Children.emplace_back(std::move(code));

            if (catlen > 0)
                catlen--;
        }
    }
    if (catlen != 0) return false;

    for (auto& item : entry.RootCat.Children)
    {
        if (!std::holds_alternative<ARCodeCat>(item))
            continue;

        auto& cat = std::get<ARCodeCat>(item);
        if (!cat.OnlyOneCodeEnabled)
            continue;

        // for categories that only allow one code to be enabled:
        // make sure we don't have multiple ones enabled

        bool foundone = false;
        for (auto& childitem : cat.Children)
        {
            auto& code = std::get<ARCode>(childitem);
            if (!code.Enabled) continue;
            if (foundone)
                code.Enabled = false;
            else
                foundone = true;
        }
    }

    return true;
}

}
