// SPDX-License-Identifier: GPL-3.0-or-later
#include "GBACartEEPROM.h"
#include "Savestate.h"
#include <limits>

namespace melonDS::GBACart
{
void EEPROM::ClearProtocol() noexcept
{
    State = Phase::Idle;
    AddressBits = 0;
    Bits = 0;
    Address = 0;
    Data = 0;
}

void EEPROM::Reset() noexcept
{
    ClearProtocol();
    BusyUntil = 0;
}

void EEPROM::Deselect(bool abort) noexcept
{
    if (abort || State != Phase::ReadData || Bits != 0)
        ClearProtocol();
}

u16 EEPROM::Ready(u64 timestamp) const noexcept
{
    return timestamp >= BusyUntil ? 1 : 0;
}

bool EEPROM::Active(u64 timestamp) const noexcept
{
    return State != Phase::Idle || !Ready(timestamp);
}

bool EEPROM::AcceptStorage(std::size_t length) noexcept
{
    if ((length != 512 && length != 8192) ||
        (State != Phase::Idle && AddressBits != (length == 512 ? 6 : 14)))
    {
        ClearProtocol();
        return false;
    }
    return true;
}

u16 EEPROM::Read(std::span<const u8> save, u64 timestamp) noexcept
{
    if (!Ready(timestamp)) return 0;
    BusyUntil = 0;
    if (!AcceptStorage(save.size()) || State != Phase::ReadData) return 1;

    u16 result = 0;
    if (Bits >= 4)
    {
        const unsigned bit = Bits - 4;
        result = (save[Address * 8 + bit / 8] >> (7 - bit % 8)) & 1;
    }
    if (++Bits == 68) ClearProtocol();
    return result;
}

bool EEPROM::Write(u16 value, std::span<u8> save, u64 timestamp, u32& changedOffset) noexcept
{
    if (!Ready(timestamp)) return false;
    BusyUntil = 0;
    if (!AcceptStorage(save.size())) return false;
    const u8 bit = value & 1;

    switch (State)
    {
    case Phase::Idle:
        if (bit)
        {
            AddressBits = save.size() == 512 ? 6 : 14;
            State = Phase::Command;
        }
        break;
    case Phase::Command:
        State = bit ? Phase::ReadAddress : Phase::WriteAddress;
        break;
    case Phase::ReadAddress:
    case Phase::WriteAddress:
        Address = (Address << 1) | bit;
        if (++Bits == AddressBits)
        {
            // 8 KiB chips clock 14 address bits, but decode only the low ten.
            Address &= save.size() / 8 - 1;
            Bits = 0;
            State = State == Phase::ReadAddress ? Phase::ReadDummy : Phase::WriteData;
        }
        break;
    case Phase::ReadDummy:
        // Fixed-chip NBA STATE_EAT_DUMMY, ares ReadValidate and Mesen2
        // ReadCommand ignore this value. Extra write clocks in ReadData are
        // ignored too: a 512-byte chip keeps the first six address bits of a
        // long-address read probe. See the original insideGadgets 2017-01-22
        // hardware observations and GodMode9i's ARM7 EEPROM consumer.
        State = Phase::ReadData;
        break;
    case Phase::ReadData:
        break;
    case Phase::WriteData:
        Data = (Data << 1) | bit;
        if (++Bits == 64)
        {
            Bits = 0;
            State = Phase::WriteDummy;
        }
        break;
    case Phase::WriteDummy:
        // The same fixed-chip implementations consume either dummy value.
        // Stage until this clock so a partial packet cannot alter the old save.
        if (timestamp > std::numeric_limits<u64>::max() - WriteBusyCycles)
        {
            ClearProtocol();
            return false;
        }
        changedOffset = Address * 8;
        for (unsigned i = 0; i < 8; ++i)
            save[changedOffset + i] = static_cast<u8>(Data >> (56 - 8 * i));
        BusyUntil = timestamp + WriteBusyCycles;
        ClearProtocol();
        return true;
    }
    return false;
}

bool EEPROM::IsValid(u32 saveLength) const noexcept
{
    if (BusyUntil && (BusyUntil < WriteBusyCycles || (saveLength != 512 && saveLength != 8192)))
        return false;
    if (State == Phase::Idle)
        return AddressBits == 0 && Bits == 0 && Address == 0 && Data == 0;
    if (BusyUntil || (saveLength != 512 && saveLength != 8192) ||
        AddressBits != (saveLength == 512 ? 6 : 14))
        return false;

    switch (State)
    {
    case Phase::Command:
        return Bits == 0 && Address == 0 && Data == 0;
    case Phase::ReadAddress:
    case Phase::WriteAddress:
        return Bits < AddressBits && Address < (1u << Bits) && Data == 0;
    case Phase::ReadDummy:
        return Bits == 0 && Address < saveLength / 8 && Data == 0;
    case Phase::ReadData:
        return Bits < 68 && Address < saveLength / 8 && Data == 0;
    case Phase::WriteData:
        return Bits < 64 && Address < saveLength / 8 && (Data >> Bits) == 0;
    case Phase::WriteDummy:
        return Bits == 0 && Address < saveLength / 8;
    default:
        return false;
    }
}

void EEPROM::DoSavestate(Savestate* file, u32 saveLength) noexcept
{
    if (file->Error) return;
    if (file->Saving && !IsValid(saveLength))
    {
        file->Error = true;
        return;
    }

    EEPROM staged = *this;
    u8 phase = static_cast<u8>(staged.State);
    file->Var8(&phase);
    file->Var8(&staged.AddressBits);
    file->Var8(&staged.Bits);
    file->Var16(&staged.Address);
    file->Var64(&staged.Data);
    file->Var64(&staged.BusyUntil);
    if (file->Error) return;
    staged.State = static_cast<Phase>(phase);
    if (!staged.IsValid(saveLength))
    {
        file->Error = true;
        return;
    }
    if (!file->Saving) *this = staged;
}
}
