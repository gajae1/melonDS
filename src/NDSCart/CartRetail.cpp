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
constexpr std::array<u32, 11> SaveLengths = {
    0, 512, 8192, 65536, 128*1024, 256*1024, 512*1024, 1024*1024,
    8192*1024, 16384*1024, 65536*1024
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
    if (type <= 4) return 2; // Regular EEPROM.
    if (type <= 7) return 3; // SPI Flash.
    return 4; // NAND.
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

    u32 savememtype = ROMParams.SaveMemType <= 10 ? ROMParams.SaveMemType : 0;
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
    SRAMStatus = 0;

    SRAMSaveAddr = 0;
    SRAMSaveLen = 0;
    FlashPending = false;
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
        (SRAMType == 4) != (*restoredType >= 8) ||
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
        // valid, including the conditional pending Flash payload.
        try { if (fileLength) restored = std::make_unique<u8[]>(fileLength); }
        catch (const std::bad_alloc&) { file->Error = true; return; }
        if (SRAMFileLength) memcpy(restored.get(), SRAM.get(), SRAMFileLength);
        if (fileLength > SRAMFileLength)
            memset(restored.get() + SRAMFileLength, 0xFF, fileLength - SRAMFileLength);
    }
    if (length) file->VarArray(file->Saving ? SRAM.get() : restored.get(), length);
    u32 pos = SRAMPos, addr = SRAMAddr, saveAddr = SRAMSaveAddr, saveLen = SRAMSaveLen;
    u8 cmd = SRAMCmd, status = SRAMStatus;
    auto flash = FlashBuffer;
    // The flag extends only active Flash records. Idle and non-Flash layouts
    // remain byte-for-byte compatible, including derived IR/NAND tails.
    constexpr u32 flashFlag = 1u << 31;
    if (file->Saving && FlashPending) saveLen |= flashFlag;
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
    const bool pending = !legacy && (saveLen & flashFlag);
    if (pending)
    {
        saveLen &= ~flashFlag;
        const bool page = cmd == 0x02 || cmd == 0x0A;
        const bool erase = cmd == 0xDB || cmd == 0xD8;
        if (!file->IsAtLeastVersion(14, 6) || SaveProtocol(*restoredType) != 3 || !(status & 2) ||
            saveAddr > 0xFFFFFF ||
            !(page ? pos >= 5 && saveLen > 0 && saveLen <= 256 :
              erase && pos == 4 && saveLen == (cmd == 0xDB ? 256u : 65536u)))
        {
            file->Error = true;
            return;
        }
        file->VarArray(flash.data(), flash.size());
    }
    if (file->Error || file->Saving) return;
    SRAM = std::move(restored);
    SRAMLength = length;
    SRAMFileLength = fileLength;
    SRAMType = SaveProtocol(*restoredType);
    SRAMPos = pos; SRAMCmd = cmd; SRAMAddr = addr; SRAMStatus = status;
    SRAMSaveAddr = saveAddr; SRAMSaveLen = saveLen;
    FlashPending = pending;
    FlashBuffer = flash;
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
    FlashPending = false;
    if (SRAMType == 3) SRAMSaveLen = 0;
}

void CartRetail::SPIRelease()
{
    if (FlashPending)
    {
        u32 length = (SRAMCmd == 0xD8) ? 65536 : 256;
        u32 offset = (SRAMSaveAddr & (SRAMLength - 1)) & ~(length - 1);
        if (SRAMCmd == 0x02 || SRAMCmd == 0x0A)
        {
            for (u32 i = 0; i < SRAMSaveLen; ++i)
            {
                const u32 index = (SRAMAddr - SRAMSaveLen + i) & 255;
                if (SRAMCmd == 0x02) SRAM[offset + index] &= FlashBuffer[index];
                else SRAM[offset + index] = FlashBuffer[index];
            }
            const u32 first = (SRAMAddr - SRAMSaveLen) & 255;
            if (SRAMSaveLen <= 256 - first)
            {
                offset += first;
                length = SRAMSaveLen;
            }
        }
        else
            memset(SRAM.get() + offset, 0xFF, length);
        Platform::WriteNDSSave(SRAM.get(), SRAMFileLength, offset, length, UserData);
        FlashPending = false;
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
}

u8 CartRetail::SPITransmitReceive(u8 val)
{
    if (SRAMType == 0) return 0;

    u8 ret = 0xFF;

    if (SRAMPos == 0)
    {
        // handle generic commands with no parameters
        switch (val)
        {
        case 0x04: // write disable
            SRAMStatus &= ~(1<<1);
            return 0;
        case 0x06: // write enable
            SRAMStatus |= (1<<1);
            return 0;

        default:
            SRAMCmd = val;
            SRAMAddr = 0;
            break;
        }
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

    SRAMPos++;
    return ret;
}

u8 CartRetail::SRAMWrite_EEPROMTiny(u8 val)
{
    switch (SRAMCmd)
    {
    case 0x01: // write status register
        // TODO: WP bits should be nonvolatile!
        if (SRAMPos == 1)
            SRAMStatus = (SRAMStatus & 0x01) | (val & 0x0C);
        return 0;

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
            // TODO: implement WP bits!
            if (SRAMStatus & (1<<1))
            {
                // The starting address latches the page; only its low bits advance.
                SRAM[(SRAMSaveAddr & 0x1F0) | (SRAMAddr & 0xF)] = val;
                SRAMSaveLen++;
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
    case 0x01: // write status register
        // TODO: WP bits should be nonvolatile!
        if (SRAMPos == 1)
            SRAMStatus = (SRAMStatus & 0x01) | (val & 0x0C);
        return 0;

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
            // TODO: implement WP bits
            // TODO: restrict writing to page based on EEPROM size
            // except for FRAM????
            if (SRAMStatus & (1<<1))
            {
                SRAM[SRAMAddr & (SRAMLength-1)] = val;
                SRAMSaveLen++;
            }
            SRAMAddr++;
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
                if (!FlashPending)
                {
                    FlashBuffer.fill(0xFF);
                    FlashPending = true;
                    SRAMSaveLen = 0;
                }
                FlashBuffer[SRAMAddr & 255] = val;
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
            FlashPending = true;
            FlashBuffer.fill(0xFF);
            SRAMSaveLen = SRAMCmd == 0xDB ? 256 : 65536;
        }
        else if (SRAMPos > 3)
        {
            // ERASE ends immediately after its address; extra data cancels it.
            FlashPending = false;
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
