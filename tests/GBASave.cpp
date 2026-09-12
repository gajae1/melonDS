// SPDX-License-Identifier: GPL-3.0-or-later
// Real GBA Flash command decoder and save notifications; generated data only.
#include "NDS.h"
#include "GBACart.h"
#include "GBASaveDatabase.h"
#include "Platform.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

using namespace melonDS;

static std::vector<u8> Snapshot(GBACart::CartGame& cart);

struct SaveTrace
{
    const u8* Data = nullptr;
    u32 Capacity = 0;
    u32 Offset = 0;
    u32 Length = 0;
    unsigned Calls = 0;
    bool InRange = true;
    GBACart::CartGame* Cart = nullptr;
    std::vector<u8> ObservedState;
};

namespace melonDS::Platform
{
void WriteGBASave(const u8* data, u32 capacity, u32 offset, u32 length, void* userdata)
{
    auto& trace = *static_cast<SaveTrace*>(userdata);
    trace.Data = data;
    trace.Capacity = capacity;
    trace.Offset = offset;
    trace.Length = length;
    ++trace.Calls;
    trace.InRange &= offset <= capacity && length <= capacity - offset;
    if (trace.Cart) trace.ObservedState = Snapshot(*trace.Cart);
}
void Log(LogLevel, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}
void Addon_RumbleStart(u32, void*) { std::abort(); }
void Addon_RumbleStop(void*) { std::abort(); }
bool Addon_KeyDown(KeyType, void*) { std::abort(); }
float Addon_MotionQuery(MotionQueryType, void*) { std::abort(); }
}

static void Command(GBACart::CartGame& cart, u8 command)
{
    cart.SRAMWrite(0x5555, 0xAA);
    cart.SRAMWrite(0x2AAA, 0x55);
    cart.SRAMWrite(0x5555, command);
}

static void Bank(GBACart::CartGame& cart, u8 bank)
{
    Command(cart, 0xB0);
    cart.SRAMWrite(0, bank);
}

static void Erase(GBACart::CartGame& cart, u32 address, u8 command = 0x30)
{
    Command(cart, 0x80);
    cart.SRAMWrite(0x5555, 0xAA);
    cart.SRAMWrite(0x2AAA, 0x55);
    cart.SRAMWrite(address, command);
}

static std::vector<u8> Snapshot(GBACart::CartGame& cart)
{
    Savestate state(0x21080);
    cart.DoSavestate(&state);
    state.Finish();
    if (state.Error) std::abort();
    const auto* bytes = static_cast<const u8*>(state.Buffer());
    return {bytes, bytes + state.Length()};
}

static bool State(u32 length, bool solar = false)
{
    SaveTrace trace;
    const auto makeCart = [&](u32 size, u8 value) -> std::unique_ptr<GBACart::CartGame> {
        auto rom = std::make_unique<u8[]>(0x200);
        auto save = size ? std::make_unique<u8[]>(size) : nullptr;
        if (size) std::fill_n(save.get(), size, value);
        if (solar)
            return std::make_unique<GBACart::CartGameSolarSensor>(
                std::move(rom), 0x200, std::move(save), size, &trace);
        return std::make_unique<GBACart::CartGame>(
            std::move(rom), 0x200, std::move(save), size, &trace);
    };
    auto source = makeCart(length, 0xFF);
    if (length == 0x20010) std::fill_n(source->GetSaveMemory() + 0x20000, 16, 0xA6);
    source->ROMWrite(0xC8, 1);
    source->ROMWrite(0xC6, 0xF);
    source->ROMWrite(0xC4, 5);
    if (solar) source->SetInput(GBACart::Input_SolarSensorUp, true);
    if (length >= 0x20000) Bank(*source, 1);
    Command(*source, 0xA0); // The first write after restoration must remain payload.
    const auto saved = Snapshot(*source);
    bool passed = true;
    const auto check = [&](bool ok, const char* reason) {
        if (!ok) { passed = false; std::fprintf(stderr, "state/%u: %s\n", length, reason); }
    };
    // Same-capacity and resized loads must both preserve all live fields when
    // the GBCS payload is incomplete, including its trailing command metadata.
    // Target the original flash/solar payload, not an optional extension that
    // a 13.x/14.2 reader intentionally ignores at the end of this section.
    const std::size_t deviceEnd = 48 + length + (solar ? 4 : 0);
    for (u32 targetLength : {length, 0x8000u})
    for (unsigned corruption : {0u, 1u, 2u})
    {
        auto target = makeCart(targetLength, 0x39);
        const auto before = Snapshot(*target);
        const auto* owner = target->GetSaveMemory();
        auto truncated = saved;
        if (corruption == 0) truncated.resize(42 + length / 2);
        else if (corruption == 1) truncated.resize(deviceEnd - 1);
        else truncated[47 + length] = 0xFF; // Undefined device type.
        u32 size = static_cast<u32>(truncated.size());
        std::memcpy(truncated.data() + 8, &size, 4);
        size -= 16;
        std::memcpy(truncated.data() + 20, &size, 4);
        trace = {};
        Savestate load(truncated.data(), static_cast<u32>(truncated.size()), false);
        target->DoSavestate(&load);
        check(load.Error, "incomplete or invalid device state was accepted");
        check(target->GetSaveMemory() == owner && Snapshot(*target) == before,
              "rejected state changed save ownership, bytes or device state");
        check(trace.Calls == 0, "rejected state notified persistence");
    }
    for (u16 major : {u16(13), u16(SAVESTATE_MAJOR)})
    {
        auto target = makeCart(0x8000, 0x39);
        auto bytes = saved;
        // GBCS has the same payload in format 13; this is a generated fixture,
        // not a claim of general legacy whole-console state compatibility.
        std::memcpy(bytes.data() + 4, &major, 2);
        if (major == 13) { bytes[6] = 0; bytes[7] = 0; }
        trace = {};
        trace.Cart = target.get();
        Savestate load(bytes.data(), static_cast<u32>(bytes.size()), false);
        target->DoSavestate(&load);
        check(!load.Error && Snapshot(*target) == saved, "valid state did not restore all fields");
        check(trace.ObservedState == saved, "save callback observed partially restored device state");
        check(trace.Calls == 1 && trace.Data == target->GetSaveMemory() &&
              trace.Capacity == length && trace.Offset == 0 && trace.Length == length && trace.InRange,
              "valid state notified the wrong owner or range");
        target->SRAMWrite(0x1234, 0x3C);
        auto expected = std::vector<u8>(source->GetSaveMemory(), source->GetSaveMemory() + length);
        expected[length >= 0x20000 ? 0x11234 : 0x1234] = 0x3C;
        check(std::equal(expected.begin(), expected.end(), target->GetSaveMemory()),
              "restored pending program lost its selected bank or RTC bytes");
        auto empty = makeCart(0, 0);
        auto emptyBytes = Snapshot(*empty);
        trace = {};
        Savestate clear(emptyBytes.data(), static_cast<u32>(emptyBytes.size()), false);
        target->DoSavestate(&clear);
        check(!clear.Error && !target->GetSaveMemory() && target->GetSaveMemoryLength() == 0 &&
              trace.Calls == 0, "empty state did not clear save memory quietly");
    }
    std::printf("gba-save/state%s/%u: %s\n", solar ? "-solar" : "", length, passed ? "PASS" : "FAIL");
    return passed;
}

static bool Run(const char* mode, u32 length)
{
    // Extra owned backing turns the old decoder's out-of-range access into a
    // deterministic canary mismatch, without corrupting another allocation.
    // The cart's declared device size and production decoder are unchanged.
    const u32 backingLength = std::max(length + 0x1000, 0x31000u);
    auto backing = std::make_unique<u8[]>(backingLength);
    std::fill_n(backing.get(), backingLength, 0xA6);
    std::fill_n(backing.get(), length, 0x31);
    if (length > 0x10000) std::fill_n(backing.get() + 0x10000, 0x10000, 0x42);
    std::vector<u8> expected(backing.get(), backing.get() + backingLength);
    auto* data = backing.get();
    auto rom = std::make_unique<u8[]>(0x200);
    SaveTrace trace;
    GBACart::CartGame cart(std::move(rom), 0x200, std::move(backing), length, &trace);
    const bool banked = length >= 0x20000;
    bool passed = true;
    const auto check = [&](bool value, const char* reason) {
        if (!value) { passed = false; std::fprintf(stderr, "%s/%u: %s\n", mode, length, reason); }
    };
    const auto notification = [&](u32 offset, u32 count) {
        check(trace.Calls == 1 && trace.Data == data && trace.Capacity == length &&
              trace.Offset == offset && trace.Length == count && trace.InRange,
              "wrong save notification ownership or range");
    };
    if (!std::strcmp(mode, "sector"))
    {
        for (u32 address : {0x2000u, 0x2345u, 0xFFFFu})
        {
            std::copy(expected.begin(), expected.end(), data);
            trace = {};
            if (banked) Bank(cart, 1);
            Erase(cart, address);
            // Literal expected sector boundaries from the command contract.
            const u32 sector = (address == 0xFFFF ? 0xF000 : 0x2000) + (banked ? 0x10000 : 0);
            auto erased = expected;
            std::fill_n(erased.data() + sector, 0x1000, 0xFF);
            check(std::equal(erased.begin(), erased.end(), data), "sector erase crossed its sector/device or missed its prefix");
            notification(sector, 0x1000);
            check(cart.SRAMRead(address) == 0xFF, "erased sector not readable");
        }
    }
    else if (!std::strcmp(mode, "chip"))
    {
        for (u8 bank = 0; bank < (banked ? 2 : 1); ++bank)
        {
            std::copy(expected.begin(), expected.end(), data);
            if (banked) Bank(cart, bank);
            trace = {};
            // Neither a bare command, a command without erase setup, nor a
            // fully unlocked command at the wrong address may erase the chip.
            cart.SRAMWrite(0x5555, 0x10);
            Command(cart, 0x10);
            cart.SRAMWrite(0x5555, 0xF0);
            Erase(cart, 0x5554, 0x10);
            check(std::equal(expected.begin(), expected.end(), data) && trace.Calls == 0,
                  "invalid chip erase changed save data or notified a write");

            Erase(cart, 0x5555, 0x10);
            auto erased = expected;
            const u32 physicalLength = banked ? 0x20000 : 0x10000;
            std::fill_n(erased.data(), physicalLength, 0xFF);
            check(std::equal(erased.begin(), erased.end(), data),
                  "chip erase missed a bank or changed RTC/trailing backing");
            notification(0, physicalLength);
            check(cart.SRAMRead(0x1234) == 0xFF, "chip erase did not return to read mode");

            trace = {};
            Command(cart, 0xA0);
            cart.SRAMWrite(0x1234, 0x5A);
            const u32 offset = bank ? 0x11234 : 0x1234;
            erased[offset] = 0x5A;
            check(std::equal(erased.begin(), erased.end(), data),
                  "chip erase lost the selected bank or blocked subsequent programming");
            notification(offset, 1);
        }
    }
    else if (!std::strcmp(mode, "bank"))
    {
        if (banked) Bank(cart, 1);
        Bank(cart, banked ? 2 : 1);
        check(cart.SRAMRead(0x1234) == (banked ? 0x42 : 0x31), "unsupported bank replaced the valid bank");
        Command(cart, 0xA0);
        cart.SRAMWrite(0x1234, 0x19);
        const u32 offset = banked ? 0x11234 : 0x1234;
        expected[offset] = 0x19;
        check(std::equal(expected.begin(), expected.end(), data), "bank write touched another bank/backing");
        notification(offset, 1);
    }
    else if (!std::strcmp(mode, "program"))
    {
        if (banked) Bank(cart, 1);
        // Command-looking addresses/data remain payload after the A0 command.
        for (const auto step : {std::pair{0x5555u, u8(0xAA)}, {0x5555u, u8(0xF0)}, {0u, u8(0xB0)}})
        {
            const u32 offset = step.first + (banked ? 0x10000 : 0);
            // Each payload starts in erased backing; this does not assert that
            // physical Flash can program a zero bit back to one without erase.
            std::fill_n(data + (offset & ~0xFFFu), 0x1000, 0xFF);
            std::fill_n(expected.data() + (offset & ~0xFFFu), 0x1000, 0xFF);
            trace = {};
            Command(cart, 0xA0);
            cart.SRAMWrite(step.first, step.second);
            expected[offset] = step.second;
            check(std::equal(expected.begin(), expected.end(), data), "byte program interpreted data as a command");
            notification(offset, 1);
            check(cart.SRAMRead(step.first) == step.second, "byte program did not return to read mode");
        }
    }
    else return false;
    std::printf("gba-save/%s/%u: %s\n", mode, length, passed ? "PASS" : "FAIL");
    return passed;
}

static bool Import()
{
    SaveTrace trace;
    auto rom = std::make_unique<u8[]>(0x200);
    GBACart::CartGame cart(std::move(rom), 0x200, nullptr, 0, &trace);
    bool passed = true;
    const auto check = [&](const std::vector<u8>& expected) {
        const auto* data = cart.GetSaveMemory();
        const bool ok = data && cart.GetSaveMemoryLength() == expected.size() &&
            std::equal(expected.begin(), expected.end(), data) && trace.Calls == 1 &&
            trace.Data == data && trace.Capacity == expected.size() && trace.Offset == 0 &&
            trace.Length == expected.size() && trace.InRange;
        if (!ok) { passed = false; std::fputs("import: incorrect bytes, capacity or notification owner\n", stderr); }
    };
    for (u32 size : {512u, 8192u, 32768u, 65536u, 131072u, 131088u, 8192u})
    {
        std::vector<u8> input(size);
        for (u32 i = 0; i < size; ++i) input[i] = u8(i * 29 + 0x37);
        const auto expected = input;
        trace = {};
        cart.SetSaveMemory(input.data(), size);
        std::fill(input.begin(), input.end(), 0xF1);
        check(expected); // The cart must retain a complete owned copy.
    }
    std::vector<u8> expected(cart.GetSaveMemory() + 128, cart.GetSaveMemory() + 640);
    trace = {};
    cart.SetSaveMemory(cart.GetSaveMemory() + 128, 512);
    check(expected);
    trace = {};
    cart.SetSaveMemory(cart.GetSaveMemory(), cart.GetSaveMemoryLength());
    check(expected);
    const auto* owner = cart.GetSaveMemory();
    trace = {};
    cart.SetSaveMemory(nullptr, 512);
    cart.SetSaveMemory(owner, 0);
    passed &= trace.Calls == 0 && cart.GetSaveMemory() == owner &&
              cart.GetSaveMemoryLength() == expected.size() &&
              std::equal(expected.begin(), expected.end(), owner);
    std::printf("gba-save/import: %s\n", passed ? "PASS" : "FAIL");
    return passed;
}

static bool Detection()
{
    SaveTrace trace;
    bool passed = true;
    const auto check = [&](bool ok, const char* reason) {
        if (!ok) { passed = false; std::fprintf(stderr, "detection: %s\n", reason); }
    };
    // Exercise the public factory used by the frontend, including an unpadded
    // ROM whose complete SDK identifier occupies its final bytes.
    for (const auto& [marker, length] : {
             std::pair{"SRAM_V110", 32768u}, {"SRAM_F_V103", 32768u},
             {"FLASH_V126", 65536u}, {"FLASH512_V130", 65536u},
             {"FLASH1M_V103", 131072u}})
    {
        std::vector<u8> rom(0x211, 0);
        std::memcpy(rom.data() + 0xAC, "TEST", 4);
        std::memcpy(rom.data() + rom.size() - std::strlen(marker), marker, std::strlen(marker));
        trace = {};
        auto cart = GBACart::ParseROM(rom.data(), u32(rom.size()), &trace);
        check(cart && cart->GetSaveMemoryLength() == length,
              "missing save was not initialized from a complete, unambiguous SDK identifier");
        if (cart && cart->GetSaveMemoryLength() == length)
        {
            check(std::all_of(cart->GetSaveMemory(), cart->GetSaveMemory() + length,
                              [](u8 value) { return value == 0xFF; }), "new chip was not erased");
            check(trace.Calls == 0, "detection published a save before a guest write");
            if (length == 32768) cart->SRAMWrite(0x1234, 0x52);
            else
            {
                cart->SRAMWrite(0x5555, 0xAA);
                cart->SRAMWrite(0x2AAA, 0x55);
                cart->SRAMWrite(0x5555, 0xA0);
                cart->SRAMWrite(0x1234, 0x52);
            }
            check(cart->SRAMRead(0x1234) == 0x52 && trace.Calls == 1 &&
                  trace.Capacity == length && trace.Offset == 0x1234 && trace.Length == 1,
                  "detected chip did not execute its write protocol and save notification");
        }
        // Supplied saves retain their complete length and contents even when
        // metadata disagrees; include the existing RTC tail and odd lengths.
        for (u32 oldLength : {512u, 8192u, 131088u, 37u})
        {
            std::vector<u8> old(oldLength, 0x39);
            trace = {};
            auto supplied = GBACart::ParseROM(rom.data(), u32(rom.size()), old.data(), oldLength, &trace);
            check(supplied && supplied->GetSaveMemoryLength() == oldLength &&
                  std::equal(old.begin(), old.end(), supplied->GetSaveMemory()) && !trace.Calls,
                  "detection replaced, resized or published an existing save");
        }
    }
    for (const char* marker : {"", "EEPROM_V124", "FLASH1M_V10", "FLASH1M_V10x",
                              "SRAM_V110 FLASH1M_V103", "SRAM_V110 EEPROM_V124"})
    {
        std::vector<u8> rom(0x211, 0);
        std::memcpy(rom.data() + 0xAC, "TEST", 4);
        std::memcpy(rom.data() + rom.size() - std::strlen(marker), marker, std::strlen(marker));
        trace = {};
        auto cart = GBACart::ParseROM(rom.data(), u32(rom.size()), &trace);
        check(cart && cart->GetSaveMemoryLength() == 0 && !trace.Calls,
              "unknown, incomplete or conflicting metadata guessed a chip capacity");
    }
    std::printf("gba-save/detection: %s\n", passed ? "PASS" : "FAIL");
    return passed;
}

static bool Database()
{
    // Independent reference rows from MAME's pinned CC0 gba.xml (007eon and
    // holybibl). The catalog contains no ROM bytes; these validate lookup and
    // the original-length guard, not execution of the commercial games.
    // https://github.com/mamedev/mame/blob/1a326ed01c3629259abcf61a2a6bebf25858d376/hash/gba.xml
    const std::array<u8, 20> small {0xFC,0x61,0x63,0xF9,0x9B,0x71,0xB0,0x5C,0x10,0x68,
        0x6A,0x0D,0x29,0x01,0x0B,0x31,0x27,0x4E,0x1D,0xC4};
    const std::array<u8, 20> large {0xE2,0xDD,0x02,0xDE,0xD6,0x17,0xF7,0x00,0x2C,0xA8,
        0x87,0x63,0x9C,0xF9,0x1B,0x9C,0xAC,0x8C,0xD7,0x45};
    const auto lookup = GBACart::SaveDatabase::Lookup;
    const bool passed = lookup(small, 8388608) == 512 && lookup(large, 33554432) == 8192 &&
        lookup(small, 8388607) == 0 && lookup(small, 8388609) == 0 &&
        lookup(large, 33554433) == 0 && lookup({}, 8388608) == 0;
    std::printf("gba-save/database: %s\n", passed ? "PASS" : "FAIL");
    return passed;
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    if (!std::strcmp(argv[1], "import")) return Import() ? 0 : 1;
    if (!std::strcmp(argv[1], "detection")) return Detection() ? 0 : 1;
    if (!std::strcmp(argv[1], "database")) return Database() ? 0 : 1;
    bool passed = true;
    for (u32 length : {0x10000u, 0x20000u, 0x20010u})
        passed &= !std::strcmp(argv[1], "state") ? State(length) :
                  !std::strcmp(argv[1], "state-solar") ? State(length, true) : Run(argv[1], length);
    return passed ? 0 : 1;
}
