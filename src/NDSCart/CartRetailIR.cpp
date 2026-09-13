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

#include "CartRetailIR.h"
#include "../NDS.h"
#include "../Utils.h"

// CartRetailIR: NDS cartridge with IR transceiver (Pokémon games)
// the IR transceiver is connected to the SPI interface, with a passthrough command for SRAM access

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

namespace NDSCart
{

CartRetailIR::CartRetailIR(const u8* rom, u32 len, u32 chipid, u32 irversion, bool badDSiDump, ROMListEntry romparams, std::unique_ptr<u8[]>&& sram, u32 sramlen, void* userdata) :
    CartRetailIR(CopyToUnique(rom, len), len, chipid, irversion, badDSiDump, romparams, std::move(sram), sramlen, userdata)
{
}

CartRetailIR::CartRetailIR(
    std::unique_ptr<u8[]>&& rom,
    u32 len,
    u32 chipid,
    u32 irversion,
    bool badDSiDump,
    ROMListEntry romparams,
    std::unique_ptr<u8[]>&& sram,
    u32 sramlen,
    void* userdata
) :
    CartRetail(std::move(rom), len, chipid, badDSiDump, romparams, std::move(sram), sramlen, userdata, CartType::RetailIR),
    IRVersion(irversion)
{
}

CartRetailIR::~CartRetailIR() = default;

void CartRetailIR::Reset()
{
    CartRetail::Reset();

    IRCmd = 0;
    IRPos = 0;
}

void CartRetailIR::PrepareSavestate(Savestate* file, u8 pendingSPI) const
{
    CartRetail::PrepareSavestate(file, IRPos && IRCmd == 0 ? pendingSPI : 0);
}

void CartRetailIR::DoSavestate(Savestate* file)
{
    u32 sectionEnd = 0;
    if (!file->Saving)
    {
        file->Section("NDCS");
        if (file->Error) return;
        // Section() validated this header and its span. Remember its end before
        // the base loader consumes the variable-length SRAM record.
        const u32 sectionStart = file->Length() - 16;
        u32 sectionLength;
        memcpy(&sectionLength, static_cast<const u8*>(file->Buffer()) + sectionStart + 4, sizeof(sectionLength));
        sectionEnd = sectionStart + sectionLength;
    }

    CartRetail::DoSavestate(file);
    if (file->Error) return;

    const bool hasPosition = file->IsAtLeastVersion(14, 1);
    const u32 irLength = hasPosition ? 5 : 1;
    if (!file->Saving && (file->Length() > sectionEnd || irLength > sectionEnd - file->Length()))
    {
        Log(LogLevel::Error, "savestate: truncated IR cartridge record\n");
        file->Error = true;
        return;
    }

    file->Var8(&IRCmd);
    if (hasPosition)
        file->Var32(&IRPos);
    else if (!file->Saving)
    {
        // 14.0 did not store the IR phase. Start a new command rather than
        // combining the restored command with the live session's position.
        IRPos = 0;
        CartRetail::SPISelect();
    }
}

void CartRetailIR::SPISelect()
{
    CartRetail::SPISelect();
    IRPos = 0;
}

u8 CartRetailIR::SPITransmitReceive(u8 val)
{
    if (IRPos == 0)
    {
        IRCmd = val;
        IRPos++;
        return 0;
    }

    // TODO: emulate actual IR comm

    // Match the retail SPI fallback for unemulated commands. This is not a
    // measured response from the IR hardware.
    u8 ret = 0xFF;
    switch (IRCmd)
    {
    case 0x00: // pass-through
        ret = CartRetail::SPITransmitReceive(val);
        break;

    case 0x08: // ID
        ret = 0xAA;
        break;
    }

    // Only zero marks a new command; a long/restored transfer must not wrap.
    if (IRPos != 0xFFFFFFFF) IRPos++;
    return ret;
}


}

}
