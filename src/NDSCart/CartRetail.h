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

#ifndef NDSCART_CARTRETAIL_H
#define NDSCART_CARTRETAIL_H

#include "CartCommon.h"
#include <optional>
#include <array>

namespace melonDS::NDSCart
{

// CartRetail -- regular retail cart (ROM, SPI SRAM)
class CartRetail : public CartCommon
{
    friend class NDSCartSlot;
public:
    CartRetail(
            const u8* rom,
            u32 len,
            u32 chipid,
            bool badDSiDump,
            ROMListEntry romparams,
            std::unique_ptr<u8[]>&& sram,
            u32 sramlen,
            void* userdata,
            melonDS::NDSCart::CartType type = CartType::Retail
    );
    CartRetail(
            std::unique_ptr<u8[]>&& rom,
            u32 len, u32 chipid,
            bool badDSiDump,
            ROMListEntry romparams,
            std::unique_ptr<u8[]>&& sram,
            u32 sramlen,
            void* userdata,
            melonDS::NDSCart::CartType type = CartType::Retail
    );
    ~CartRetail() override;

    void Reset() override;

    void DoSavestate(Savestate* file) override;
    void PrepareSavestate(Savestate* file, u8 pendingSPI = 0) const override;

    void SetSaveMemory(const u8* savedata, u32 savelen) override;

    void SPISelect() override;
    void SPIRelease() override;
    u8 SPITransmitReceive(u8 val) override;
    u32 GetSaveDelay() const override { return WriteDelay ? WriteDelay : PowerDelay; }
    void CompleteSave() override;
    void CancelSave(bool powerOff = false) override;

    // ROM metadata remains authoritative. This fallback only identifies the
    // SPI capacities we emulate; a large file must not imply a NAND cartridge.
    static std::optional<u32> SPITypeForSaveLength(u32 length);

    u8* GetSaveMemory() override { return SRAM.get(); }
    const u8* GetSaveMemory() const override { return SRAM.get(); }
    u32 GetSaveMemoryLength() const override { return SRAMFileLength; }

protected:
    void BeginSave(u8 command, u32 address, u32 first, u32 length);
    bool IsWriteProtected(u32 address) const;
    u8 SRAMWrite_EEPROMTiny(u8 val);
    u8 SRAMWrite_EEPROM(u8 val);
    u8 SRAMWrite_FLASH(u8 val);

    std::unique_ptr<u8[]> SRAM = nullptr;
    // The emulated chip wraps at SRAMLength. Extra bytes in an existing save
    // belong to the backing file and must survive guest writes and state loads.
    u32 SRAMLength = 0;
    u32 SRAMFileLength = 0;
    u32 SRAMType = 0;
    // Zero retains the legacy capacity-only behavior; 11..17 identify media.
    u32 SRAMProfile = 0;

    u32 SRAMPos = 0;
    u8 SRAMCmd = 0;
    u32 SRAMAddr = 0;
    //u32 SRAMFirstAddr = 0;
    u8 SRAMStatus = 0;

    //bool SRAMNeedsSaving = false;
    u32 SRAMSaveAddr = 0;
    u32 SRAMSaveLen = 0;

    // EEPROM/Flash pages latch until CS rises. Only the last received byte for
    // each offset is committed; FRAM writes directly without this buffer.
    std::array<u8, 256> PageBuffer {};
    bool PagePending = false;

    // The internal program latch survives new SPI transactions (including RDSR).
    u32 WriteDelay = 0;
    u8 WriteCommand = 0;
    u32 WriteAddress = 0, WriteFirst = 0, WriteLength = 0;
    std::array<u8, 256> WriteBuffer {};
    // M25PE T9HX: volatile write-lock/lock-down bits, one per64KiB sector.
    std::array<u8, 16> FlashLocks {};
    // Power transitions share the internal-operation event, but never set WIP
    // or overlap a write. CS selected before wake completion stays rejected.
    enum class FlashPower : u8 { Awake, Entering, Asleep, Waking };
    FlashPower PowerState = FlashPower::Awake;
    u32 PowerDelay = 0;
    bool PowerBlocked = false;
};

}

#endif
