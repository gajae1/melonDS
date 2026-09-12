// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef GBACART_EEPROM_H
#define GBACART_EEPROM_H

#include <span>
#include "types.h"

namespace melonDS
{
class Savestate;

namespace GBACart
{
// Fixed-capacity GBA EEPROM chip. The caller owns storage, bus selection and
// continuous-transfer boundaries; timestamps use the 33,513,982 Hz DS clock.
// Reset when importing/replacing storage, even if its capacity is unchanged.
class EEPROM
{
public:
    // round(33,513,982 * 0.0065) = round(217,840.883).
    static constexpr u64 WriteBusyCycles = 217841;

    void Reset() noexcept;
    // A normal command/response gap keeps a complete, unread read command.
    // Aborting clears protocol state, but neither form cancels a committed write.
    void Deselect(bool abort) noexcept;

    u16 Read(std::span<const u8> save, u64 timestamp) noexcept;
    // True exactly once per completed eight-byte write, including same-data
    // writes. changedOffset is untouched on false. Unsupported capacity or
    // timestamp overflow cannot modify storage. No allocation or callback here.
    bool Write(u16 value, std::span<u8> save, u64 timestamp, u32& changedOffset) noexcept;
    u16 Ready(u64 timestamp) const noexcept;
    bool Active(u64 timestamp) const noexcept;

    // Explicit 21-byte payload, no section/version changes. Idle is valid for
    // non-EEPROM storage too. Loading publishes only a complete, valid state.
    void DoSavestate(Savestate* file, u32 saveLength) noexcept;
    bool IsValid(u32 saveLength) const noexcept;

private:
    enum class Phase : u8
    {
        Idle, Command, ReadAddress, ReadDummy, ReadData,
        WriteAddress, WriteData, WriteDummy
    };

    void ClearProtocol() noexcept;
    bool AcceptStorage(std::size_t length) noexcept;

    Phase State = Phase::Idle;
    u8 AddressBits = 0;
    u8 Bits = 0;
    u16 Address = 0;
    u64 Data = 0;
    u64 BusyUntil = 0;
};
}
}

#endif
