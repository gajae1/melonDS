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

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <string_view>
#include "NDS.h"
#include "GBACart.h"
#include "CRC32.h"
#include "Platform.h"
#include "Utils.h"

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;

namespace GBACart
{

const char SOLAR_SENSOR_GAMECODES[10][5] =
{
    "U3IJ", // Bokura no Taiyou - Taiyou Action RPG (Japan)
    "U3IE", // Boktai - The Sun Is in Your Hand (USA)
    "U3IP", // Boktai - The Sun Is in Your Hand (Europe)
    "U32J", // Zoku Bokura no Taiyou - Taiyou Shounen Django (Japan)
    "U32E", // Boktai 2 - Solar Boy Django (USA)
    "U32P", // Boktai 2 - Solar Boy Django (Europe)
    "U33J", // Shin Bokura no Taiyou - Gyakushuu no Sabata (Japan)
    "A3IJ"  // Boktai - The Sun Is in Your Hand (USA) (Sample)
};

CartCommon::CartCommon(GBACart::CartType type) : CartType(type)
{
}

void CartCommon::Reset()
{
}

void CartCommon::DoSavestate(Savestate* file)
{
    file->Section("GBCS");
}

void CartCommon::SetSaveMemory(const u8* savedata, u32 savelen)
{
}

int CartCommon::SetInput(int num, bool pressed)
{
    return -1;
}

u16 CartCommon::ROMRead(u32 addr) const
{
    return 0;
}

void CartCommon::ROMWrite(u32 addr, u16 val)
{
}

u8 CartCommon::SRAMRead(u32 addr)
{
    return 0;
}

void CartCommon::SRAMWrite(u32 addr, u8 val)
{
}

u8* CartCommon::GetSaveMemory() const
{
    return nullptr;
}

u32 CartCommon::GetSaveMemoryLength() const
{
    return 0;
}

CartGame::CartGame(const u8* rom, u32 len, const u8* sram, u32 sramlen, void* userdata, GBACart::CartType type) :
    CartGame(CopyToUnique(rom, len), len, CopyToUnique(sram, sramlen), sramlen, userdata, type)
{
}

CartGame::CartGame(std::unique_ptr<u8[]>&& rom, u32 len, std::unique_ptr<u8[]>&& sram, u32 sramlen, void* userdata, GBACart::CartType type) :
    CartCommon(type),
    ROM(std::move(rom)),
    ROMLength(len),
    SRAM(std::move(sram)),
    SRAMLength(sramlen),
    UserData(userdata)
{
    if (SRAM && SRAMLength)
    {
        SetupSave(sramlen);
    }
}

CartGame::~CartGame() = default;
// unique_ptr cleans up the allocated memory

u32 CartGame::Checksum() const
{
    u32 crc = CRC32(ROM.get(), 0xC0, 0);

    // TODO: hash more contents?

    return crc;
}

void CartGame::Reset()
{
    memset(&GPIO, 0, sizeof(GPIO));
    SerialSave.Reset();
}

void CartGame::DoSavestate(Savestate* file)
{
    DoSavestate(file, nullptr, 0);
    if (!file->Saving && !file->Error && SRAM)
        Platform::WriteGBASave(SRAM.get(), SRAMLength, 0, SRAMLength, UserData);
}

void CartGame::DoSavestate(Savestate* file, u8* extra, u32 extraLength)
{
    PrepareSavestate(file);
    CartCommon::DoSavestate(file);

    auto gpio = GPIO;
    auto flash = SRAMFlashState;
    auto serial = SerialSave;
    u32 length = SRAMLength;
    u8 type = static_cast<u8>(SRAMType);
    file->Var16(&gpio.control);
    file->Var16(&gpio.data);
    file->Var16(&gpio.direction);
    file->Var32(&length);
    if (file->Error) return;

    std::unique_ptr<u8[]> restored;
    if (!file->Saving)
    {
        // Keep live bytes and command state until the whole payload validates,
        // including same-capacity loads and the metadata after the save data.
        if (length > file->BufferLength() - file->Length())
        {
            file->Error = true;
            return;
        }
        try { if (length) restored = std::make_unique<u8[]>(length); }
        catch (const std::bad_alloc&) { file->Error = true; return; }
    }
    if (length)
    {
        file->VarArray(file->Saving ? SRAM.get() : restored.get(), length);
        file->Var8(&flash.bank);
        file->Var8(&flash.cmd);
        file->Var8(&flash.device);
        file->Var8(&flash.manufacturer);
        file->Var8(&flash.state);
        file->Var8(&type);
    }
    // A derived cart's tail belongs to the same transaction as its save data.
    file->VarArray(extra, extraLength);
    // Append even an idle payload: another device may require 14.5 later in
    // the same file. Older readers ignore this section tail. Older files have
    // no serial transaction, so loading one resets the chip to ready/idle.
    if (file->Saving || file->IsAtLeastVersion(14, 5))
    {
        serial.DoSavestate(file, length);
    }
    else serial.Reset();
    if (file->Error || file->Saving) return;
    if (!length) type = S_NULL;
    if ((length && type > S_FLASH1M) ||
        ((type == S_EEPROM4K || type == S_EEPROM64K) &&
         length != (type == S_EEPROM4K ? 512u : 8192u)) ||
        (serial.Active(0) && type != S_EEPROM4K && type != S_EEPROM64K))
    {
        file->Error = true;
        return;
    }

    GPIO = gpio;
    SRAM = std::move(restored);
    SRAMLength = length;
    SRAMType = length ? static_cast<SaveType>(type) : S_NULL;
    SRAMFlashState = flash;
    SerialSave = serial;
}

void CartGame::SetupSave(u32 type)
{
    // TODO: have type be determined from some list, like in NDSCart
    // and not this gross hack!!
    SRAMLength = type;
    switch (SRAMLength)
    {
    case 512:
        SRAMType = S_EEPROM4K;
        break;
    case 8192:
        SRAMType = S_EEPROM64K;
        break;
    case 32768:
        SRAMType = S_SRAM256K;
        break;
    case 65536:
        SRAMType = S_FLASH512K;
        break;
    case 128*1024:
    case (128*1024 + 0x10): // .sav file with appended real time clock data (ex: emulator mGBA)
        SRAMType = S_FLASH1M;
        break;
    case 0:
        SRAMType = S_NULL;
        break;
    default:
        Log(LogLevel::Warn, "!! BAD GBA SAVE LENGTH %d\n", SRAMLength);
    }

    if (SRAMType == S_FLASH512K)
    {
        // Panasonic 64K chip
        SRAMFlashState.device = 0x1B;
        SRAMFlashState.manufacturer = 0x32;
    }
    else if (SRAMType == S_FLASH1M)
    {
        // Sanyo 128K chip
        SRAMFlashState.device = 0x13;
        SRAMFlashState.manufacturer = 0x62;
    }
}

void CartGame::SetSaveMemory(const u8* savedata, u32 savelen)
{
    if (!savedata || !savelen) return;

    // Stage before replacing the old owner: savedata may alias its SRAM.
    std::unique_ptr<u8[]> imported;
    try
    {
        imported = std::make_unique<u8[]>(savelen);
    }
    catch (const std::bad_alloc&)
    {
        // GBACartSlot::SetSaveMemory is noexcept. Preserve the valid old save.
        Log(LogLevel::Error, "Failed to allocate %u bytes for the GBA save. Previous save kept.\n", savelen);
        return;
    }
    memcpy(imported.get(), savedata, savelen);

    SRAM = std::move(imported);
    SetupSave(savelen);
    SerialSave.Reset();
    Platform::WriteGBASave(SRAM.get(), SRAMLength, 0, SRAMLength, UserData);
}

bool CartGame::EEPROMSelected(u32 addr) const noexcept
{
    if (SRAMType != S_EEPROM4K && SRAMType != S_EEPROM64K) return false;
    // EEPROM's address pin is A23 (byte address bit 24). On 32 MiB
    // cartridges the ROM decoder leaves only the final 256 bytes for it.
    return (addr & 0x01000000) &&
        (ROMLength <= 0x01000000 || (addr & 0x01FFFF00) == 0x01FFFF00);
}

u16 CartGame::ROMReadBus(u32 addr, u64 timestamp, bool dma)
{
    if (!EEPROMSelected(addr))
    {
        SerialSave.Deselect(true);
        return ROMRead(addr);
    }
    // An isolated CPU access can poll ready, but cannot continue the serial
    // response of an earlier DMA. Reads during programming return busy.
    if (!dma) SerialSave.Deselect(true);
    return SerialSave.Read({SRAM.get(), SRAMLength}, timestamp);
}

void CartGame::ROMWriteBus(u32 addr, u16 val, u64 timestamp, bool dma)
{
    if (!EEPROMSelected(addr))
    {
        SerialSave.Deselect(true);
        ROMWrite(addr, val);
        return;
    }
    if (!dma)
    {
        SerialSave.Deselect(true);
        return;
    }
    u32 offset = 0;
    if (SerialSave.Write(val, {SRAM.get(), SRAMLength}, timestamp, offset))
        Platform::WriteGBASave(SRAM.get(), SRAMLength, offset, 8, UserData);
}

u16 CartGame::ROMRead(u32 addr) const
{
    addr &= 0x01FFFFFF;

    if (addr >= 0xC4 && addr < 0xCA)
    {
        if (GPIO.control & 0x1)
        {
            switch (addr)
            {
            case 0xC4: return GPIO.data;
            case 0xC6: return GPIO.direction;
            case 0xC8: return GPIO.control;
            }
        }
    }

    // CHECKME: does ROM mirror?
    if (addr < ROMLength)
        return *(u16*)&ROM[addr];

    return 0;
}

void CartGame::ROMWrite(u32 addr, u16 val)
{
    addr &= 0x01FFFFFF;

    switch (addr)
    {
        case 0xC4:
            GPIO.data &= ~GPIO.direction;
            GPIO.data |= val & GPIO.direction;
            ProcessGPIO();
            break;

        case 0xC6:
            GPIO.direction = val;
            break;

        case 0xC8:
            GPIO.control = val;
            break;

        default:
            Log(LogLevel::Warn, "Unknown GBA GPIO write 0x%02X @ 0x%04X\n", val, addr);
            break;
    }
}

u8 CartGame::SRAMRead(u32 addr)
{
    addr &= 0xFFFF;

    switch (SRAMType)
    {
    case S_EEPROM4K:
    case S_EEPROM64K:
        return SRAMRead_EEPROM(addr);

    case S_FLASH512K:
    case S_FLASH1M:
        return SRAMRead_FLASH(addr);

    case S_SRAM256K:
        return SRAMRead_SRAM(addr);
    default:
        break;
    }

    return 0xFF;
}

void CartGame::SRAMWrite(u32 addr, u8 val)
{
    addr &= 0xFFFF;

    switch (SRAMType)
    {
    case S_EEPROM4K:
    case S_EEPROM64K:
        return SRAMWrite_EEPROM(addr, val);

    case S_FLASH512K:
    case S_FLASH1M:
        return SRAMWrite_FLASH(addr, val);

    case S_SRAM256K:
        return SRAMWrite_SRAM(addr, val);
    default:
        break;
    }
}

u8* CartGame::GetSaveMemory() const
{
    return SRAM.get();
}

u32 CartGame::GetSaveMemoryLength() const
{
    return SRAMLength;
}

void CartGame::ProcessGPIO()
{
}

u8 CartGame::SRAMRead_EEPROM(u32 addr)
{
    return 0;
}

void CartGame::SRAMWrite_EEPROM(u32 addr, u8 val)
{
    // TODO: could be used in homebrew?
}

// mostly ported from DeSmuME
u8 CartGame::SRAMRead_FLASH(u32 addr)
{
    if (SRAMFlashState.cmd == 0) // no cmd
    {
        const u32 offset = addr + 0x10000 * SRAMFlashState.bank;
        const u32 flashLength = SRAMType == S_FLASH1M ? 0x20000 : 0x10000;
        return offset < flashLength ? SRAMRead_SRAM(offset) : 0xFF;
    }

    switch (SRAMFlashState.cmd)
    {
        case 0x90: // chip ID
            if (addr == 0x0000) return SRAMFlashState.manufacturer;
            if (addr == 0x0001) return SRAMFlashState.device;
            break;
        case 0xF0: // terminate command (TODO: break if non-Macronix chip and not at the end of an ID call?)
            SRAMFlashState.state = 0;
            SRAMFlashState.cmd = 0;
            break;
        case 0xA0: // write command
            break; // ignore here, handled in Write_Flash()
        case 0xB0: // bank switching (128K only)
            break; // ignore here, handled in Write_Flash()
        default:
            Log(LogLevel::Warn, "GBACart_SRAM::Read_Flash: unknown command 0x%02X @ 0x%04X\n", SRAMFlashState.cmd, addr);
            break;
    }

    return 0xFF;
}

// mostly ported from DeSmuME
void CartGame::SRAMWrite_FLASH(u32 addr, u8 val)
{
    const u32 flashLength = SRAMType == S_FLASH1M ? 0x20000 : 0x10000;
    if (SRAMFlashState.cmd == 0xA0)
    {
        // The byte after A0 is payload, even at an unlock/reset address.
        const u32 offset = addr + 0x10000 * SRAMFlashState.bank;
        if (offset < flashLength) SRAMWrite_SRAM(offset, val);
        SRAMFlashState.state = 0;
        SRAMFlashState.cmd = 0;
        return;
    }
    if (SRAMFlashState.cmd == 0xB0)
    {
        // Keep the selected bank on unsupported values; never expose a save
        // trailer or host memory as another physical Flash bank.
        if (addr == 0 && (val == 0 || (val == 1 && SRAMType == S_FLASH1M)))
            SRAMFlashState.bank = val;
        SRAMFlashState.state = 0;
        SRAMFlashState.cmd = 0;
        return;
    }

    switch (SRAMFlashState.state)
    {
        case 0x00:
            if (addr == 0x5555)
            {
                if (val == 0xF0)
                {
                    // reset
                    SRAMFlashState.state = 0;
                    SRAMFlashState.cmd = 0;
                    return;
                }
                else if (val == 0xAA)
                {
                    SRAMFlashState.state = 1;
                    return;
                }
            }
            break;
        case 0x01:
            if (addr == 0x2AAA && val == 0x55)
            {
                SRAMFlashState.state = 2;
                return;
            }
            SRAMFlashState.state = 0;
            break;
        case 0x02:
            if (addr == 0x5555)
            {
                // send command
                switch (val)
                {
                    case 0x80: // erase
                        SRAMFlashState.state = 0x80;
                        break;
                    case 0x90: // chip ID
                        SRAMFlashState.state = 0x90;
                        break;
                    case 0xA0: // write
                        SRAMFlashState.state = 0;
                        break;
                    default:
                        SRAMFlashState.state = 0;
                        break;
                }

                SRAMFlashState.cmd = val;
                return;
            }
            SRAMFlashState.state = 0;
            break;
        // erase
        case 0x80:
            if (addr == 0x5555 && val == 0xAA)
            {
                SRAMFlashState.state = 0x81;
                return;
            }
            SRAMFlashState.state = 0;
            break;
        case 0x81:
            if (addr == 0x2AAA && val == 0x55)
            {
                SRAMFlashState.state = 0x82;
                return;
            }
            SRAMFlashState.state = 0;
            break;
        case 0x82:
            if (val == 0x10 && addr == 0x5555)
            {
                // Chip erase covers both banks, but not appended RTC data.
                if (flashLength <= SRAMLength)
                {
                    memset(SRAM.get(), 0xFF, flashLength);
                    Platform::WriteGBASave(SRAM.get(), SRAMLength, 0, flashLength, UserData);
                }
            }
            else if (val == 0x30)
            {
                const u32 start = (addr & 0xF000) + 0x10000 * SRAMFlashState.bank;
                if (start < flashLength && start <= SRAMLength && 0x1000 <= SRAMLength - start)
                {
                    memset(&SRAM[start], 0xFF, 0x1000);
                    Platform::WriteGBASave(SRAM.get(), SRAMLength, start, 0x1000, UserData);
                }
            }
            SRAMFlashState.state = 0;
            SRAMFlashState.cmd = 0;
            return;
        // chip ID
        case 0x90:
            if (addr == 0x5555 && val == 0xAA)
            {
                SRAMFlashState.state = 0x91;
                return;
            }
            SRAMFlashState.state = 0;
            break;
        case 0x91:
            if (addr == 0x2AAA && val == 0x55)
            {
                SRAMFlashState.state = 0x92;
                return;
            }
            SRAMFlashState.state = 0;
            break;
        case 0x92:
            SRAMFlashState.state = 0;
            SRAMFlashState.cmd = 0;
            return;
        default:
            break;
    }

    Log(LogLevel::Debug, "GBACart_SRAM::Write_Flash: unknown write 0x%02X @ 0x%04X (state: 0x%02X)\n",
        val, addr, SRAMFlashState.state);
}

u8 CartGame::SRAMRead_SRAM(u32 addr)
{
    if (addr >= SRAMLength) return 0xFF;

    return SRAM[addr];
}

void CartGame::SRAMWrite_SRAM(u32 addr, u8 val)
{
    if (addr >= SRAMLength) return;

    u8 prev = *(u8*)&SRAM[addr];
    if (prev != val)
    {
        *(u8*)&SRAM[addr] = val;

        // TODO: optimize this!!
        Platform::WriteGBASave(SRAM.get(), SRAMLength, addr, 1, UserData);
    }
}


CartGameSolarSensor::CartGameSolarSensor(const u8* rom, u32 len, const u8* sram, u32 sramlen, void* userdata) :
    CartGameSolarSensor(CopyToUnique(rom, len), len, CopyToUnique(sram, sramlen), sramlen, userdata)
{
}

CartGameSolarSensor::CartGameSolarSensor(std::unique_ptr<u8[]>&& rom, u32 len, std::unique_ptr<u8[]>&& sram, u32 sramlen, void* userdata) :
    CartGame(std::move(rom), len, std::move(sram), sramlen, userdata, CartType::GameSolarSensor)
{
}

std::unique_ptr<CartGameSolarSensor> CreateFakeSolarSensorROM(const char* gamecode, const NDSCart::CartCommon& cart, void* userdata) noexcept
{
    return CreateFakeSolarSensorROM(gamecode, cart.GetHeader().NintendoLogo, userdata);
}

std::unique_ptr<CartGameSolarSensor> CreateFakeSolarSensorROM(const char* gamecode, const GBACart::CartGame& cart, void* userdata) noexcept
{
    return CreateFakeSolarSensorROM(gamecode, cart.GetHeader().NintendoLogo, userdata);
}

std::unique_ptr<CartGameSolarSensor> CreateFakeSolarSensorROM(const char* gamecode, const u8* logo, void* userdata) noexcept
{
    if (!gamecode)
        return nullptr;

    if (strnlen(gamecode, sizeof(GBAHeader::GameCode)) > sizeof(GBAHeader::GameCode))
        return nullptr;

    bool solarsensor = false;
    for (const char* i : SOLAR_SENSOR_GAMECODES)
    {
        if (strcmp(gamecode, i) == 0) {
            solarsensor = true;
            break;
        }
    }

    if (!solarsensor)
        return nullptr;

    // just 256 bytes; we don't need a whole ROM!
    constexpr size_t FAKE_BOKTAI_ROM_LENGTH = 0x100;
    std::unique_ptr<u8[]> rom = std::make_unique<u8[]>(FAKE_BOKTAI_ROM_LENGTH);

    // create a fake ROM
    GBAHeader& header = *reinterpret_cast<GBAHeader*>(rom.get());
    memcpy(header.Title, BOKTAI_STUB_TITLE, strnlen(BOKTAI_STUB_TITLE, sizeof(header.Title)));
    memcpy(header.GameCode, gamecode, strnlen(gamecode, sizeof(header.GameCode)));
    header.FixedValue = 0x96;
    if (logo)
    {
        memcpy(header.NintendoLogo, logo, sizeof(header.NintendoLogo));
    }
    else
    {
        memset(header.NintendoLogo, 0xFF, sizeof(header.NintendoLogo));
    }

    return std::make_unique<CartGameSolarSensor>(std::move(rom), FAKE_BOKTAI_ROM_LENGTH, nullptr, 0, userdata);
}

const int CartGameSolarSensor::kLuxLevels[11] = {0, 5, 11, 18, 27, 42, 62, 84, 109, 139, 183};

void CartGameSolarSensor::Reset()
{
    CartGame::Reset();
    LightEdge = false;
    LightCounter = 0;
    LightSample = 0xFF;
    LightLevel = 0;
}

void CartGameSolarSensor::DoSavestate(Savestate* file)
{
    u8 sensor[] = {u8(LightEdge), LightCounter, LightSample, LightLevel};
    CartGame::DoSavestate(file, sensor, sizeof(sensor));
    if (file->Error || file->Saving) return;
    LightEdge = sensor[0] != 0;
    LightCounter = sensor[1];
    LightSample = sensor[2];
    LightLevel = sensor[3];
    if (SRAM)
        Platform::WriteGBASave(SRAM.get(), SRAMLength, 0, SRAMLength, UserData);
}

int CartGameSolarSensor::SetInput(int num, bool pressed)
{
    if (!pressed) return -1;

    if (num == Input_SolarSensorDown)
    {
        if (LightLevel > 0)
            LightLevel--;

        return LightLevel;
    }
    else if (num == Input_SolarSensorUp)
    {
        if (LightLevel < 10)
            LightLevel++;

        return LightLevel;
    }

    return -1;
}

void CartGameSolarSensor::SetLightLevel(u8 level) noexcept
{
    LightLevel = std::clamp<u8>(level, 0, 10);
}

void CartGameSolarSensor::ProcessGPIO()
{
    if (GPIO.data & 4) return; // Boktai chip select
    if (GPIO.data & 2) // Reset
    {
        u8 prev = LightSample;
        LightCounter = 0;
        LightSample = (0xFF - (0x16 + kLuxLevels[LightLevel]));
        Log(LogLevel::Debug, "Solar sensor reset (sample: 0x%02X -> 0x%02X)\n", prev, LightSample);
    }
    if (GPIO.data & 1 && LightEdge) LightCounter++;

    LightEdge = !(GPIO.data & 1);

    bool sendBit = LightCounter >= LightSample;
    if (GPIO.control & 1)
    {
        GPIO.data = (GPIO.data & GPIO.direction) | ((sendBit << 3) & ~GPIO.direction & 0xF);
    }
}


CartRAMExpansion::CartRAMExpansion() : CartCommon(RAMExpansion)
{
}

CartRAMExpansion::~CartRAMExpansion() = default;

void CartRAMExpansion::Reset()
{
    memset(RAM, 0xFF, sizeof(RAM));
    RAMEnable = 1;
}

void CartRAMExpansion::DoSavestate(Savestate* file)
{
    CartCommon::DoSavestate(file);

    file->VarArray(RAM, sizeof(RAM));
    file->Var16(&RAMEnable);
}

u16 CartRAMExpansion::ROMRead(u32 addr) const
{
    addr &= 0x01FFFFFF;

    if (addr < 0x01000000)
    {
        switch (addr)
        {
        case 0xB0: return 0xFFFF;
        case 0xB2: return 0x0000;
        case 0xB4: return 0x2400;
        case 0xB6: return 0x2424;
        case 0xB8: return 0xFFFF;
        case 0xBA: return 0xFFFF;
        case 0xBC: return 0xFFFF;
        case 0xBE: return 0x7FFF;

        case 0x1FFFC: return 0xFFFF;
        case 0x1FFFE: return 0x7FFF;

        case 0x240000: return RAMEnable;
        case 0x240002: return 0x0000;
        }

        return 0xFFFF;
    }
    else if (addr < 0x01800000)
    {
        if (!RAMEnable) return 0xFFFF;

        return *(u16*)&RAM[addr & 0x7FFFFF];
    }

    return 0xFFFF;
}

void CartRAMExpansion::ROMWrite(u32 addr, u16 val)
{
    addr &= 0x01FFFFFF;

    if (addr < 0x01000000)
    {
        switch (addr)
        {
        case 0x240000:
            RAMEnable = val & 0x0001;
            return;
        }
    }
    else if (addr < 0x01800000)
    {
        if (!RAMEnable) return;

        *(u16*)&RAM[addr & 0x7FFFFF] = val;
    }
}

CartRumblePak::CartRumblePak(void* userdata) : 
    CartCommon(RumblePak),
    UserData(userdata)
{
}

CartRumblePak::~CartRumblePak() = default;

void CartRumblePak::Reset()
{
    RumbleState = 0;
}

void CartRumblePak::DoSavestate(Savestate* file)
{
    CartCommon::DoSavestate(file);
    file->Var16(&RumbleState);
}

u16 CartRumblePak::ROMRead(u32 addr) const
{
    // A1 is pulled low on a real Rumble Pak, so return the
    // necessary detection value here,
    // and let the existing open bus implementation take care of the rest
    return 0xFFFD;
}

void CartRumblePak::ROMWrite(u32 addr, u16 val)
{
    addr &= 0x01FFFFFF;
    // Only AD1 is connected; older savestates may contain other bus bits.
    val &= 0x0002;
    RumbleState &= 0x0002;
    if (RumbleState != val)
    {
	Platform::Addon_RumbleStop(UserData);
	RumbleState = val;
	Platform::Addon_RumbleStart(16, UserData);
    }
}

CartGuitarGrip::CartGuitarGrip(void* userdata) : 
    CartCommon(GuitarGrip),
    UserData(userdata)
{
}

CartGuitarGrip::~CartGuitarGrip() = default;

u16 CartGuitarGrip::ROMRead(u32 addr) const
{
    return 0xF9FF;
}

u8 CartGuitarGrip::SRAMRead(u32 addr)
{
    return ~((Platform::Addon_KeyDown(Platform::KeyGuitarGripGreen, UserData) ? 0x40 : 0)
        | (Platform::Addon_KeyDown(Platform::KeyGuitarGripRed, UserData) ? 0x20 : 0)
        | (Platform::Addon_KeyDown(Platform::KeyGuitarGripYellow, UserData) ? 0x10 : 0)
        | (Platform::Addon_KeyDown(Platform::KeyGuitarGripBlue, UserData) ? 0x08 : 0));
}

GBACartSlot::GBACartSlot(melonDS::NDS& nds, std::unique_ptr<CartCommon>&& cart) noexcept : NDS(nds), Cart(std::move(cart))
{
}

void GBACartSlot::Reset() noexcept
{
    Deselect();
    if (Cart) Cart->Reset();
}

void GBACartSlot::DoSavestate(Savestate* file) noexcept
{
    PrepareSavestate(file);
    file->Section("GBAC"); // Game Boy Advance Cartridge

    // little state here
    // no need to save OpenBusDecay, it will be set later

    u32 carttype = 0;
    u32 cartchk = 0;
    if (Cart)
    {
        carttype = Cart->Type();
        cartchk = Cart->Checksum();
    }

    if (file->Saving)
    {
        file->Var32(&carttype);
        file->Var32(&cartchk);
    }
    else
    {
        u32 savetype = 0;
        file->Var32(&savetype);
        if (file->Error || savetype != carttype) return;

        u32 savechk = 0;
        file->Var32(&savechk);
        if (file->Error || savechk != cartchk) return;
    }

    auto owner = DMAOwner;
    auto reading = DMAReading;
    auto aborted = DMAAborted;
    if (file->Saving || file->IsAtLeastVersion(14, 5))
    {
        file->Var8(&owner);
        file->VarBool(&reading);
        file->VarBool(&aborted);
        if (file->Error) return;
        if ((owner != 0xFF && owner >= 8) ||
            (owner == 0xFF && (reading || aborted)))
        {
            file->Error = true;
            return;
        }
    }
    else { owner = 0xFF; reading = false; aborted = false; }

    const auto oldOwner = DMAOwner;
    const auto oldReading = DMAReading;
    const auto oldAborted = DMAAborted;
    if (!file->Saving)
    {
        // The cart's persistence callback must observe its restored bus owner.
        DMAOwner = owner;
        DMAReading = reading;
        DMAAborted = aborted;
    }
    if (Cart) Cart->DoSavestate(file);
    if (!file->Saving)
    {
        if (file->Error)
        {
            DMAOwner = oldOwner;
            DMAReading = oldReading;
            DMAAborted = oldAborted;
        }
        else DMAInUnit = false;
    }
}

std::unique_ptr<CartCommon> ParseROM(std::unique_ptr<u8[]>&& romdata, u32 romlen, void* userdata)
{
    return ParseROM(std::move(romdata), romlen, nullptr, 0, userdata);
}

std::unique_ptr<CartCommon> ParseROM(const u8* romdata, u32 romlen, const u8* sramdata, u32 sramlen, void* userdata)
{
    // Validate the supplied length before padding can hide a truncated header.
    if (romdata == nullptr || romlen < 0xB0 || romlen > (u32{1} << 31))
        return nullptr;

    auto [romcopy, romcopylen] = PadToPowerOf2(romdata, romlen);

    return ParseROM(std::move(romcopy), romcopylen, CopyToUnique(sramdata, sramlen), sramlen, userdata);
}

std::unique_ptr<CartCommon> ParseROM(const u8* romdata, u32 romlen, void* userdata)
{
    return ParseROM(romdata, romlen, nullptr, 0, userdata);
}

static u32 DetectSaveLength(std::string_view rom)
{
    // Nintendo SDK identifiers describe SRAM/Flash capacities, but EEPROM_V
    // does not distinguish 512 bytes from 8 KiB. See ares' GBA media analyzer.
    // This is a fallback for a missing save, not a ROM identity database.
    constexpr struct { std::string_view Prefix; u32 Length; } identifiers[] = {
        {"SRAM_V", 32768}, {"SRAM_F_V", 32768}, {"EEPROM_V", 0},
        {"FLASH_V", 65536}, {"FLASH512_V", 65536}, {"FLASH1M_V", 131072}
    };
    u32 length = 0;
    bool eeprom = false;
    for (const auto& identifier : identifiers)
    {
        for (auto pos = rom.find(identifier.Prefix); pos != std::string_view::npos;
             pos = rom.find(identifier.Prefix, pos))
        {
            pos += identifier.Prefix.size();
            const auto digit = [](char c) { return c >= '0' && c <= '9'; };
            if (rom.size() - pos < 3 || !digit(rom[pos]) || !digit(rom[pos + 1]) ||
                !digit(rom[pos + 2]) || (rom.size() - pos > 3 && digit(rom[pos + 3])))
                continue;
            if (!identifier.Length) eeprom = true;
            else if (length && length != identifier.Length)
            {
                Log(LogLevel::Warn, "GBA save identifiers conflict; capacity was not guessed.\n");
                return 0;
            }
            else length = identifier.Length;
            break;
        }
    }
    if (eeprom)
    {
        Log(LogLevel::Warn, "GBA EEPROM capacity is unknown without an existing save; new storage was not initialized.\n");
        return 0;
    }
    return length;
}

std::unique_ptr<CartCommon> ParseROM(std::unique_ptr<u8[]>&& romdata, u32 romlen, std::unique_ptr<u8[]>&& sramdata, u32 sramlen, void* userdata)
{
    if (romdata == nullptr)
    {
        Log(LogLevel::Error, "GBACart: romdata is null\n");
        return nullptr;
    }

    // The game code occupies bytes 0xAC..0xAF; do not read a short header.
    if (romlen < 0xB0 || romlen > (u32{1} << 31))
    {
        Log(LogLevel::Error, "GBACart: ROM length is outside the supported range\n");
        return nullptr;
    }

    // Supplied save bytes always win, including unfamiliar sizes and RTC tails.
    // Detection only initializes erased storage; the first guest write owns the
    // persistence notification. Padding contains zeros, never SDK identifiers.
    if (!sramdata && !sramlen)
    {
        sramlen = DetectSaveLength({reinterpret_cast<const char*>(romdata.get()), romlen});
        if (sramlen)
        {
            sramdata = std::make_unique_for_overwrite<u8[]>(sramlen);
            memset(sramdata.get(), 0xFF, sramlen);
        }
    }

    auto [cartrom, cartromsize] = PadToPowerOf2(std::move(romdata), romlen);

    char gamecode[5] = { '\0' };
    memcpy(&gamecode, cartrom.get() + 0xAC, 4);

    bool solarsensor = false;
    for (const char* i : SOLAR_SENSOR_GAMECODES)
    {
        if (strcmp(gamecode, i) == 0)
            solarsensor = true;
    }

    if (solarsensor)
    {
        Log(LogLevel::Info, "GBA solar sensor support detected!\n");
    }

    std::unique_ptr<CartCommon> cart;
    if (solarsensor)
        cart = std::make_unique<CartGameSolarSensor>(std::move(cartrom), cartromsize, std::move(sramdata), sramlen, userdata);
    else
        cart = std::make_unique<CartGame>(std::move(cartrom), cartromsize, std::move(sramdata), sramlen, userdata);

    cart->Reset();

    return cart;
}

std::unique_ptr<CartCommon> LoadAddon(int type, void* userdata)
{
    std::unique_ptr<CartCommon> cart;
    switch (type)
    {
    case GBAAddon_RAMExpansion:
        cart = std::make_unique<CartRAMExpansion>();
        break;
    case GBAAddon_RumblePak:
        cart = std::make_unique<CartRumblePak>(userdata);
        break;
    case GBAAddon_SolarSensorBoktai1:
        // US Boktai 1
        cart = CreateFakeSolarSensorROM("U3IE", nullptr, userdata);
        break;
    case GBAAddon_SolarSensorBoktai2:
        // US Boktai 2
        cart = CreateFakeSolarSensorROM("U32E", nullptr, userdata);
        break;
    case GBAAddon_SolarSensorBoktai3:
        // JP Boktai 3
        cart = CreateFakeSolarSensorROM("U33J", nullptr, userdata);
        break;
    case GBAAddon_MotionPakHomebrew:
        cart = std::make_unique<CartMotionPakHomebrew>(userdata);
        break;
    case GBAAddon_MotionPakRetail:
        cart = std::make_unique<CartMotionPakRetail>(userdata);
        break;
    case GBAAddon_GuitarGrip:
        cart = std::make_unique<CartGuitarGrip>(userdata);
        break;
    default:
        Log(LogLevel::Warn, "GBACart: !! invalid addon type %d\n", type);
        return nullptr;
    }

    cart->Reset();
    return cart;
}

void GBACartSlot::SetCart(std::unique_ptr<CartCommon>&& cart) noexcept
{
    AbortDMA();
    Cart = std::move(cart);
    if (Cart) Cart->ROMDeselect(true);

    if (!Cart)
    {
        Log(LogLevel::Info, "Ejected GBA cart\n");
        return;
    }

    const u8* cartrom = Cart->GetROM();

    if (cartrom)
    {
        char gamecode[5] = { '\0' };
        memcpy(&gamecode, Cart->GetROM() + 0xAC, 4);
        Log(LogLevel::Info, "Inserted GBA cart with game code: %s\n", gamecode);
    }
    else
    {
        Log(LogLevel::Info, "Inserted GBA cart with no game code (it's probably an accessory)\n");
    }
}

void GBACartSlot::SetSaveMemory(const u8* savedata, u32 savelen) noexcept
{
    if (Cart)
    {
        const auto* oldSave = Cart->GetSaveMemory();
        Cart->SetSaveMemory(savedata, savelen);
        // Import stages the new allocation while the old one is still alive.
        // A changed pointer proves success; failure must retain the live DMA.
        if (oldSave != Cart->GetSaveMemory()) AbortDMA();
    }
}

std::unique_ptr<CartCommon> GBACartSlot::EjectCart() noexcept
{
    AbortDMA();
    return std::move(Cart);
    // Cart will be nullptr after this function returns, due to the move
}


int GBACartSlot::SetInput(int num, bool pressed) noexcept
{
    if (Cart) return Cart->SetInput(num, pressed);

    return -1;
}


void GBACartSlot::Deselect() noexcept
{
    if (Cart) Cart->ROMDeselect(true);
    DMAOwner = 0xFF;
    DMAReading = false;
    DMAAborted = false;
    DMAInUnit = false;
}

void GBACartSlot::AbortDMA() noexcept
{
    if (Cart) Cart->ROMDeselect(true);
    DMAAborted = DMAOwner != 0xFF;
    DMAInUnit = false;
}

void GBACartSlot::BeginDMAUnit(u32 cpu, u32 channel, u32 source, u32 destination,
    u32 width, bool start, bool enabled) noexcept
{
    if (!Cart || !Cart->UsesSerialROM() || cpu != ((NDS.ExMemCnt[0] >> 7) & 1)) return;
    const u8 owner = cpu * 4 + channel;
    auto rom = [](u32 address) { return address >= 0x08000000 && address < 0x0A000000; };
    const bool read = rom(source), write = rom(destination);
    if (DMAOwner != 0xFF && (DMAOwner != owner || start)) Deselect();
    if (!enabled || read == write)
    {
        if (DMAOwner != 0xFF) Deselect();
        return;
    }
    // Reset, import, or owner loss must not reinterpret an old DMA's suffix
    // as a fresh command. Only an actual new DMA burst can select it again.
    if (DMAAborted) return;
    // Memory handlers ignore the low address bits for a halfword/word DMA.
    const u32 address = (read ? source : destination) & ~(width - 1);
    if (start || (DMAOwner != 0xFF && read != DMAReading))
        Cart->ROMDeselect(false);
    DMAOwner = owner;
    DMAReading = read;
    DMAInUnit = true;
    UnitAddress = address;
    UnitWidth = width;
}

void GBACartSlot::EndDMA(u32 cpu, u32 channel, bool abort) noexcept
{
    if (DMAOwner != cpu * 4 + channel) return;
    if (Cart) Cart->ROMDeselect(abort);
    DMAOwner = 0xFF;
    DMAReading = false;
    DMAAborted = false;
    DMAInUnit = false;
}

u16 GBACartSlot::ROMRead(u32 addr) noexcept
{
    if (Cart)
    {
        const bool dma = DMAInUnit && DMAReading &&
            addr >= UnitAddress && addr - UnitAddress < UnitWidth;
        if (!dma) AbortDMA();
        else if ((addr & 0x1FFFF) == 0x1FFFE) Cart->ROMDeselect(false);
        const auto timestamp = (NDS.ExMemCnt[0] & 0x80) ? NDS.ARM7Timestamp
            : NDS.ARM9Timestamp >> NDS.ARM9ClockShift;
        return Cart->ROMReadBus(addr, timestamp, dma);
    }

    return ((addr >> 1) & 0xFFFF) | OpenBusDecay;
}

void GBACartSlot::ROMWrite(u32 addr, u16 val) noexcept
{
    if (Cart)
    {
        const bool dma = DMAInUnit && !DMAReading &&
            addr >= UnitAddress && addr - UnitAddress < UnitWidth;
        if (!dma) AbortDMA();
        else if ((addr & 0x1FFFF) == 0x1FFFE) Cart->ROMDeselect(false);
        const auto timestamp = (NDS.ExMemCnt[0] & 0x80) ? NDS.ARM7Timestamp
            : NDS.ARM9Timestamp >> NDS.ARM9ClockShift;
        Cart->ROMWriteBus(addr, val, timestamp, dma);
    }
}

u8 GBACartSlot::SRAMRead(u32 addr) noexcept
{
    AbortDMA();
    if (Cart) return Cart->SRAMRead(addr);

    return 0xFF;
}

void GBACartSlot::SRAMWrite(u32 addr, u8 val) noexcept
{
    AbortDMA();
    if (Cart) Cart->SRAMWrite(addr, val);
}

}

}
