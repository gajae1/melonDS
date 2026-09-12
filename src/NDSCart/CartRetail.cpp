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

#include "CartRetail.h"
#include "../NDS.h"
#include "../Utils.h"
#include <array>

// CartRetail: basic retail NDS cartridge (ROM + SRAM)

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

namespace NDSCart
{

namespace
{
constexpr std::array<u32, 15> SaveLengths = {
    0, 512, 8192, 65536, 128*1024, 256*1024, 512*1024, 1024*1024,
    8192*1024, 16384*1024, 65536*1024,
    8192, 65536, 128*1024, 32*1024
};

std::optional<u32> SaveTypeForLength(u32 length)
{
    for (u32 type = 0; type < SaveLengths.size(); ++type)
        if (SaveLengths[type] == length) return type;
    return std::nullopt;
}

u32 SaveProtocol(u32 type)
{
    if (type <= 1) return type; // None or tiny EEPROM.
    if (type <= 4 || type >= 11) return 2; // Legacy or explicit EEPROM/FRAM.
    if (type <= 7) return 3; // SPI Flash.
    return 4; // NAND.
}

u32 EEPROMPageSize(u32 profile)
{
    // M95640, M95512 and M95M01. Legacy capacity-only codes and FM25W256
    // FRAM deliberately have no page latch.
    switch (profile)
    {
        case 11: return 32;
        case 12: return 128;
        case 13: return 256;
        default: return 0;
    }
}

bool IsStatusCommand(u32 protocol, u8 command)
{
    return protocol > 0 && protocol < 4 &&
        (command == 0x04 || command == 0x06 || (protocol < 3 && command == 0x01));
}
}

std::optional<u32> CartRetail::SPITypeForSaveLength(u32 length)
{
    const auto type = SaveTypeForLength(length);
    return type && *type < 8 ? type : std::nullopt;
}

// SRAM TODO: emulate write delays???

CartRetail::CartRetail(const u8* rom, u32 len, u32 chipid, bool badDSiDump, ROMListEntry romparams, std::unique_ptr<u8[]>&& sram, u32 sramlen, void* userdata, melonDS::NDSCart::CartType type) :
    CartRetail(CopyToUnique(rom, len), len, chipid, badDSiDump, romparams, std::move(sram), sramlen, userdata, type)
{
}

CartRetail::CartRetail(std::unique_ptr<u8[]>&& rom, u32 len, u32 chipid, bool badDSiDump, ROMListEntry romparams, std::unique_ptr<u8[]>&& sram, u32 sramlen, void* userdata, melonDS::NDSCart::CartType type) :
    CartCommon(std::move(rom), len, chipid, badDSiDump, romparams, type, userdata)
{
    LenientAddressing = false;

    u32 savememtype = ROMParams.SaveMemType < SaveLengths.size() ? ROMParams.SaveMemType : 0;
    SRAMProfile = savememtype >= 11 ? savememtype : 0;
    SRAMLength = SaveLengths[savememtype];
    SRAMFileLength = std::max(SRAMLength, sram ? sramlen : 0);

    if (SRAMFileLength)
    {
        if (sram && sramlen == SRAMFileLength)
        {
            SRAM = std::move(sram);
        }
        else
        {
            SRAM = std::make_unique<u8[]>(SRAMFileLength);
            memset(SRAM.get(), 0xFF, SRAMFileLength);

            if (sram)
            {
                memcpy(SRAM.get(), sram.get(), sramlen);
            }
        }
    }

    SRAMType = SaveProtocol(savememtype);
}

CartRetail::~CartRetail() = default;
// std::unique_ptr cleans up the SRAM and ROM

void CartRetail::Reset()
{
    CartCommon::Reset();

    SRAMPos = 0;
    SRAMCmd = 0;
    SRAMAddr = 0;
    // BP and SRWD/WPEN are nonvolatile within the inserted cartridge. Raw
    // .sav files contain only the array, so reopening still loses these bits.
    SRAMStatus &= SRAMType == 1 ? 0x0C : SRAMType == 2 ? 0x8C : 0;

    SRAMSaveAddr = 0;
    SRAMSaveLen = 0;
    PagePending = false;
}

void CartRetail::PrepareSavestate(Savestate* file) const
{
    if (!file->Saving) return;
    if (SRAMPos && IsStatusCommand(SRAMType, SRAMCmd)) file->RequireMinorVersion(8);
    else if (SRAMProfile) file->RequireMinorVersion(7);
    else if (PagePending) file->RequireMinorVersion(6);
}

void CartRetail::DoSavestate(Savestate* file)
{
    PrepareSavestate(file);
    CartCommon::DoSavestate(file);

    // we reload the SRAM contents.
    // it should be the same file, but the contents may change

    u32 length = SRAMLength;
    file->Var32(&length);
    if (file->Error) return;
    const auto restoredType = SaveTypeForLength(length);
    if (!file->Saving && (!restoredType ||
        (SRAMType == 4) != (SaveProtocol(*restoredType) == 4) ||
        length > file->BufferLength() - file->Length()))
    {
        file->Error = true;
        return;
    }
    std::unique_ptr<u8[]> restored;
    const u32 fileLength = std::max(SRAMFileLength, length);
    if (!file->Saving)
    {
        // Stage the save bytes and SPI latch until the full retail record is
        // valid, including the conditional profile and pending page payload.
        try { if (fileLength) restored = std::make_unique<u8[]>(fileLength); }
        catch (const std::bad_alloc&) { file->Error = true; return; }
        if (SRAMFileLength) memcpy(restored.get(), SRAM.get(), SRAMFileLength);
        if (fileLength > SRAMFileLength)
            memset(restored.get() + SRAMFileLength, 0xFF, fileLength - SRAMFileLength);
    }
    if (length) file->VarArray(file->Saving ? SRAM.get() : restored.get(), length);
    u32 pos = SRAMPos, addr = SRAMAddr, saveAddr = SRAMSaveAddr, saveLen = SRAMSaveLen;
    u8 cmd = SRAMCmd, status = SRAMStatus;
    auto pageBuffer = PageBuffer;
    u32 profile = file->Saving ? SRAMProfile : 0;
    // No-profile records keep the old layout and derived IR/NAND tails. Exact
    // media must be saved even when idle: capacity cannot distinguish them.
    constexpr u32 pendingFlag = 1u << 31, profileFlag = 1u << 30;
    if (file->Saving && PagePending) saveLen |= pendingFlag;
    if (file->Saving && profile) saveLen |= profileFlag;
    const bool legacy = !file->Saving && file->MajorVersion() == 13;
    if (!legacy) file->Var32(&pos);
    file->Var8(&cmd);
    file->Var32(&addr);
    file->Var8(&status);
    if (!legacy)
    {
        file->Var32(&saveAddr);
        file->Var32(&saveLen);
    }
    const bool pending = !legacy && (saveLen & pendingFlag);
    const bool hasProfile = !legacy && (saveLen & profileFlag);
    if (!legacy) saveLen &= ~(pendingFlag | profileFlag);
    if (hasProfile)
    {
        file->Var32(&profile);
        if (file->Error || !file->IsAtLeastVersion(14, 7) || profile < 11 ||
            profile >= SaveLengths.size() || SaveLengths[profile] != length)
        {
            file->Error = true;
            return;
        }
    }
    else if (*restoredType >= 11)
    {
        // In particular, a 32K state without profile metadata is not FRAM.
        file->Error = true;
        return;
    }
    const u32 protocol = SaveProtocol(hasProfile ? profile : *restoredType);
    if (!file->Saving && IsStatusCommand(protocol, cmd))
    {
        if (legacy)
        {
            // Version 13 has no byte position to resume. Keep its status.
            pos = 0;
            cmd = 0;
        }
        else if (file->IsAtLeastVersion(14, 8) &&
                 (pending || saveLen || (cmd == 0x01 && addr > 0xFF)))
        {
            file->Error = true;
            return;
        }
        // In 14.0..7, WRSR already cleared WEL when its data byte arrived,
        // so release cannot replay it. A saved opcode with no data can still
        // receive that byte and complete; preserve its position and WEL.
    }
    if (pending)
    {
        const bool flash = protocol == 3;
        const u32 pageSize = flash ? 256 : EEPROMPageSize(profile);
        const bool page = cmd == 0x02 || (flash && cmd == 0x0A);
        const bool erase = flash && (cmd == 0xDB || cmd == 0xD8);
        const u32 addressBytes = flash || length > 65536 ? 3 : 2;
        if (!file->IsAtLeastVersion(14, flash ? 6 : 7) || !pageSize || !(status & 2) ||
            saveAddr >= (1u << (addressBytes * 8)) ||
            !(page ? pos >= addressBytes + 2 && saveLen > 0 && saveLen <= pageSize :
              erase && pos == 4 && saveLen == (cmd == 0xDB ? 256u : 65536u)))
        {
            file->Error = true;
            return;
        }
        file->VarArray(pageBuffer.data(), pageBuffer.size());
    }
    if (file->Error || file->Saving) return;
    SRAM = std::move(restored);
    SRAMLength = length;
    SRAMFileLength = fileLength;
    SRAMType = protocol;
    SRAMProfile = profile;
    SRAMPos = pos; SRAMCmd = cmd; SRAMAddr = addr; SRAMStatus = status;
    SRAMSaveAddr = saveAddr; SRAMSaveLen = saveLen;
    PagePending = pending;
    PageBuffer = pageBuffer;
    // Pre-14.6 states already contain their transmitted bytes. Keep those
    // bytes and legacy dirty range; newly received data uses the page latch.
    if (SRAM)
        Platform::WriteNDSSave(SRAM.get(), SRAMFileLength, 0, SRAMLength, UserData);
}

void CartRetail::SetSaveMemory(const u8* savedata, u32 savelen)
{
    if (!SRAM) return;

    u32 len = std::min(savelen, SRAMFileLength);
    memcpy(SRAM.get(), savedata, len);
    // An imported prefix updates part of the existing save, not its capacity.
    Platform::WriteNDSSave(SRAM.get(), SRAMFileLength, 0, len, UserData);
}

void CartRetail::SPISelect()
{
    SRAMPos = 0;
    PagePending = false;
    if (SRAMType == 3 || EEPROMPageSize(SRAMProfile)) SRAMSaveLen = 0;
}

void CartRetail::SPIRelease()
{
    if (SRAMPos && IsStatusCommand(SRAMType, SRAMCmd))
    {
        // EEPROM/Flash commands must end at their defined byte boundary.
        // FM25W256 documents only the normal lengths: rejecting malformed
        // extra bytes is our policy, not a measured FRAM hardware behavior.
        // WP is inactive here: this interface exposes no physical WP input.
        if (SRAMCmd == 0x06 && SRAMPos == 1) SRAMStatus |= 2;
        else if (SRAMCmd == 0x04 && SRAMPos == 1) SRAMStatus &= ~2;
        else if (SRAMCmd == 0x01 && SRAMPos == 2 && (SRAMStatus & 2))
            SRAMStatus = SRAMAddr & (SRAMType == 1 ? 0x0C : 0x8C);
        SRAMPos = 0;
        SRAMCmd = 0;
        return;
    }
    if (PagePending)
    {
        const bool flash = SRAMType == 3;
        const u32 pageSize = flash ? 256 : EEPROMPageSize(SRAMProfile);
        u32 length = (flash && SRAMCmd == 0xD8) ? 65536 : pageSize;
        u32 offset = (SRAMSaveAddr & (SRAMLength - 1)) & ~(length - 1);
        if (SRAMCmd == 0x02 || SRAMCmd == 0x0A)
        {
            for (u32 i = 0; i < SRAMSaveLen; ++i)
            {
                const u32 index = (SRAMAddr - SRAMSaveLen + i) & (pageSize - 1);
                if (flash && SRAMCmd == 0x02) SRAM[offset + index] &= PageBuffer[index];
                else SRAM[offset + index] = PageBuffer[index];
            }
            const u32 first = (SRAMAddr - SRAMSaveLen) & (pageSize - 1);
            if (SRAMSaveLen <= pageSize - first)
            {
                offset += first;
                length = SRAMSaveLen;
            }
        }
        else
            memset(SRAM.get() + offset, 0xFF, length);
        Platform::WriteNDSSave(SRAM.get(), SRAMFileLength, offset, length, UserData);
        PagePending = false;
        SRAMStatus &= ~2;
        SRAMSaveAddr = SRAMSaveLen = 0;
        return;
    }
    if (SRAMLength && (SRAMStatus & (1<<1)) && (SRAMSaveLen > 0))
    {
        // Dirty ranges wrap inside the emulated chip, never into file padding.
        // A wrapped/full-chip write publishes the chip prefix in one callback.
        u32 offset = SRAMType == 1 ? SRAMSaveAddr & 0x1F0 : SRAMSaveAddr & (SRAMLength-1);
        u32 length = SRAMType == 1 ? 16 : std::min(SRAMSaveLen, SRAMLength);
        if (length > SRAMLength - offset)
        {
            offset = 0;
            length = SRAMLength;
        }
        Platform::WriteNDSSave(SRAM.get(), SRAMFileLength, offset, length, UserData);

        SRAMStatus &= ~(1<<1);
        SRAMSaveAddr = 0;
        SRAMSaveLen = 0;
    }
    // FM25W256 consumes WEL on WRITE completion even if BP blocked the
    // first byte. A rejected EEPROM page does not start a write cycle.
    if (SRAMProfile == 14 && SRAMCmd == 0x02 && SRAMPos >= 4) SRAMStatus &= ~2;
}

u8 CartRetail::SPITransmitReceive(u8 val)
{
    if (SRAMType == 0) return 0;

    u8 ret = 0xFF;

    if (SRAMPos == 0)
    {
        SRAMCmd = val;
        SRAMAddr = 0;
        if (IsStatusCommand(SRAMType, val)) SRAMSaveAddr = SRAMSaveLen = 0;
        if (val == 0x04 || val == 0x06) ret = 0;
    }
    else if (IsStatusCommand(SRAMType, SRAMCmd))
    {
        if (SRAMCmd == 0x01 && SRAMPos == 1) SRAMAddr = val;
        ret = 0;
    }
    else
    {
        switch (SRAMType)
        {
            case 1: ret = SRAMWrite_EEPROMTiny(val); break;
            case 2: ret = SRAMWrite_EEPROM(val); break;
            case 3: ret = SRAMWrite_FLASH(val); break;
            default: break;
        }
    }

    // Only zero denotes a new opcode; an overlong or restored transaction
    // must never wrap around and reinterpret payload as another command.
    if (SRAMPos != 0xFFFFFFFF) SRAMPos++;
    return ret;
}

bool CartRetail::IsWriteProtected(u32 address) const
{
    // The BP layout is common to M95040/M95xxx and FM25W256. It is not
    // the M25PE Flash lock-register protocol.
    const u32 bp = (SRAMStatus >> 2) & 3;
    const u32 start = bp == 0 ? SRAMLength : bp == 1 ? SRAMLength * 3 / 4 :
                      bp == 2 ? SRAMLength / 2 : 0;
    return (address & (SRAMLength - 1)) >= start;
}

u8 CartRetail::SRAMWrite_EEPROMTiny(u8 val)
{
    switch (SRAMCmd)
    {
    case 0x05: // read status register
        return SRAMStatus | 0xF0;

    case 0x02: // write low
    case 0x0A: // write high
        if (SRAMPos < 2)
        {
            SRAMAddr = val;
            SRAMSaveAddr = SRAMAddr + ((SRAMCmd==0x0A)?0x100:0);
            SRAMSaveLen = 0;
        }
        else
        {
            if ((SRAMStatus & (1<<1)) && !IsWriteProtected(SRAMSaveAddr))
            {
                // The starting address latches the page; only its low bits advance.
                SRAM[(SRAMSaveAddr & 0x1F0) | (SRAMAddr & 0xF)] = val;
                SRAMSaveLen = std::min(SRAMSaveLen + 1, 16u);
            }
            SRAMAddr++;
        }
        return 0;

    case 0x03: // read low
    case 0x0B: // read high
        if (SRAMPos < 2)
        {
            SRAMAddr = val;
            return 0;
        }
        else
        {
            u8 ret = SRAM[(SRAMAddr + ((SRAMCmd==0x0B)?0x100:0)) & 0x1FF];
            SRAMAddr++;
            return ret;
        }

    case 0x9F: // read JEDEC ID
        return 0xFF;

    default:
        if (SRAMPos == 1)
            Log(LogLevel::Warn, "unknown tiny EEPROM save command %02X\n", SRAMCmd);
        return 0xFF;
    }
}

u8 CartRetail::SRAMWrite_EEPROM(u8 val)
{
    u32 addrsize = 2;
    if (SRAMLength > 65536) addrsize++;

    switch (SRAMCmd)
    {
    case 0x05: // read status register
        return SRAMStatus;

    case 0x02: // write
        if (SRAMPos <= addrsize)
        {
            SRAMAddr <<= 8;
            SRAMAddr |= val;
            SRAMSaveAddr = SRAMAddr;
            SRAMSaveLen = 0;
        }
        else
        {
            const u32 pageSize = EEPROMPageSize(SRAMProfile);
            // FM25W256 stops incrementing at the first protected address;
            // the rest of this burst must not wrap back into writable RAM.
            if (IsWriteProtected(SRAMAddr)) return 0;
            if (SRAMStatus & (1<<1))
            {
                if (pageSize)
                {
                    if (!PagePending)
                    {
                        PageBuffer.fill(0xFF);
                        PagePending = true;
                        SRAMSaveLen = 0;
                    }
                    PageBuffer[SRAMAddr & (pageSize - 1)] = val;
                    SRAMSaveLen = std::min(SRAMSaveLen + 1, pageSize);
                }
                else
                {
                    SRAM[SRAMAddr & (SRAMLength-1)] = val;
                    SRAMSaveLen = std::min(SRAMSaveLen + 1, SRAMLength);
                }
            }
            SRAMAddr = pageSize ? (SRAMAddr & ~(pageSize - 1)) | ((SRAMAddr + 1) & (pageSize - 1))
                                : SRAMAddr + 1;
        }
        return 0;

    case 0x03: // read
        if (SRAMPos <= addrsize)
        {
            SRAMAddr <<= 8;
            SRAMAddr |= val;
            return 0;
        }
        else
        {
            // TODO: size limit!!
            u8 ret = SRAM[SRAMAddr & (SRAMLength-1)];
            SRAMAddr++;
            return ret;
        }

    case 0x9F: // read JEDEC ID
        // TODO: GBAtek implies it's not always all FF (FRAM)
        return 0xFF;

    default:
        if (SRAMPos == 1)
            Log(LogLevel::Warn, "unknown EEPROM save command %02X\n", SRAMCmd);
        return 0xFF;
    }
}

u8 CartRetail::SRAMWrite_FLASH(u8 val)
{
    // M25PE family: 256-byte page latch; PP only clears bits, PW replaces
    // addressed bytes, and erase restores FF. Commit when chip select rises.
    // Programming delays, protection and chip variants remain separate work.
    switch (SRAMCmd)
    {
    case 0x05: // read status register
        return SRAMStatus;

    case 0x02: // page program
    case 0x0A: // page write
        if (SRAMPos <= 3)
        {
            SRAMAddr <<= 8;
            SRAMAddr |= val;
            SRAMSaveAddr = SRAMAddr;
            SRAMSaveLen = 0;
        }
        else
        {
            if (SRAMStatus & (1<<1))
            {
                if (!PagePending)
                {
                    PageBuffer.fill(0xFF);
                    PagePending = true;
                    SRAMSaveLen = 0;
                }
                PageBuffer[SRAMAddr & 255] = val;
                SRAMSaveLen = std::min(SRAMSaveLen + 1, 256u);
            }
            SRAMAddr = (SRAMAddr & ~255u) | ((SRAMAddr + 1) & 255);
        }
        return 0;

    case 0x03: // read
        if (SRAMPos <= 3)
        {
            SRAMAddr <<= 8;
            SRAMAddr |= val;
            return 0;
        }
        else
        {
            u8 ret = SRAM[SRAMAddr & (SRAMLength-1)];
            SRAMAddr++;
            return ret;
        }

    case 0x0B: // fast read
        if (SRAMPos <= 3)
        {
            SRAMAddr <<= 8;
            SRAMAddr |= val;
            return 0;
        }
        else if (SRAMPos == 4)
        {
            // dummy byte
            return 0;
        }
        else
        {
            u8 ret = SRAM[SRAMAddr & (SRAMLength-1)];
            SRAMAddr++;
            return ret;
        }

    case 0x9F: // read JEDEC IC
        // GBAtek says it should be 0xFF. verify?
        return 0xFF;

    case 0xD8: // sector erase
    case 0xDB: // page erase
        if (SRAMPos <= 3)
        {
            SRAMAddr <<= 8;
            SRAMAddr |= val;
            SRAMSaveAddr = SRAMAddr;
            SRAMSaveLen = 0;
        }
        if ((SRAMPos == 3) && (SRAMStatus & (1<<1)))
        {
            PagePending = true;
            PageBuffer.fill(0xFF);
            SRAMSaveLen = SRAMCmd == 0xDB ? 256 : 65536;
        }
        else if (SRAMPos > 3)
        {
            // ERASE ends immediately after its address; extra data cancels it.
            PagePending = false;
            SRAMSaveLen = 0;
        }
        return 0;

    default:
        if (SRAMPos == 1)
            Log(LogLevel::Warn, "unknown FLASH save command %02X\n", SRAMCmd);
        return 0xFF;
    }
}


}

}
