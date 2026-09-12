// SPDX-License-Identifier: GPL-3.0-or-later
// Real GBA Flash command decoder and save notifications; generated data only.
#include "NDS.h"
#include "GBACart.h"
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

struct SaveTrace
{
    const u8* Data = nullptr;
    u32 Capacity = 0;
    u32 Offset = 0;
    u32 Length = 0;
    unsigned Calls = 0;
    bool InRange = true;
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

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    bool passed = true;
    for (u32 length : {0x10000u, 0x20000u, 0x20010u}) passed &= Run(argv[1], length);
    return passed ? 0 : 1;
}
