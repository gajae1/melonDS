// SPDX-License-Identifier: GPL-3.0-or-later
// Fixed-chip packet consumer and transactional serializer; no ROM/BIOS input.
// Packet order follows GodMode9i ARM7 gba.c at
// 8a8f806054133978e186d3a55d4058c551dde497 (read/write routines).
#include "GBACartEEPROM.h"
#include "Savestate.h"
#include "Platform.h"
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <type_traits>
#include <vector>

using namespace melonDS;
using GBACart::EEPROM;
static_assert(std::is_trivially_copyable_v<EEPROM>);
static_assert(std::is_nothrow_copy_assignable_v<EEPROM>);

namespace melonDS::Platform
{
void Log(LogLevel, const char* format, ...)
{
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
}
}

static unsigned Failures = 0;
static void Check(bool value, const char* detail)
{
    if (!value)
    {
        ++Failures;
        std::fprintf(stderr, "EEPROM FAIL: %s\n", detail);
    }
}

static std::vector<u16> Request(bool read, unsigned addressBits, u16 address,
    const std::array<u8, 8>& data = {}, u16 dummy = 0)
{
    std::vector<u16> packet {1, u16(read)};
    for (int bit = addressBits - 1; bit >= 0; --bit)
        packet.push_back((address >> bit) & 1);
    if (!read)
        for (u8 byte : data)
            for (int bit = 7; bit >= 0; --bit) packet.push_back((byte >> bit) & 1);
    packet.push_back(dummy);
    return packet;
}

static unsigned Send(EEPROM& chip, std::span<u8> save, std::span<const u16> packet,
    u64 timestamp, u32& offset)
{
    unsigned commits = 0;
    for (u16 bit : packet)
    {
        const u32 before = offset;
        const bool committed = chip.Write(bit | 0xA500, save, timestamp, offset);
        commits += committed;
        Check(committed || offset == before, "non-commit changed callback offset");
        Check(chip.IsValid(static_cast<u32>(save.size())), "generated protocol state invalid");
    }
    return commits;
}

static std::array<u8, 8> Receive(EEPROM& chip, std::span<const u8> save, u64 timestamp)
{
    for (unsigned i = 0; i < 4; ++i) Check(chip.Read(save, timestamp) == 0, "read dummy nibble");
    std::array<u8, 8> result {};
    for (auto& byte : result)
        for (unsigned i = 0; i < 8; ++i) byte = (byte << 1) | chip.Read(save, timestamp);
    return result;
}

struct StateImage
{
    std::array<u8, 128> Bytes {};
    u32 Length = 0;
};
static StateImage Snapshot(EEPROM chip, u32 length)
{
    StateImage image;
    Savestate file(image.Bytes.data(), static_cast<u32>(image.Bytes.size()), true);
    file.Section("GBCS"); // The caller owns section and version.
    chip.DoSavestate(&file, length);
    file.Finish();
    Check(!file.Error, "serialize valid chip");
    image.Length = file.Length();
    Check(image.Length == 16 + 16 + 21, "explicit chip payload length");
    return image;
}
static bool Restore(EEPROM& chip, StateImage& image, u32 length)
{
    Savestate file(image.Bytes.data(), image.Length, false);
    file.Section("GBCS");
    chip.DoSavestate(&file, length);
    return !file.Error;
}
static bool SameState(const StateImage& a, const StateImage& b)
{
    return a.Length == b.Length && std::equal(a.Bytes.begin(), a.Bytes.begin() + a.Length, b.Bytes.begin());
}

static void Packets(u32 length)
{
    const unsigned initial = Failures;
    const unsigned width = length == 512 ? 6 : 14;
    std::vector<u8> save(length, 0x69);
    const auto original = save;
    const std::array<u8, 8> data {0x01, 0x80, 0xA5, 0x5A, 0xFF, 0x00, 0x37, 0xC2};
    const u16 block = u16(length / 8 - 1);
    // Long chips ignore upper four bits, independently of packet length.
    const u16 address = length == 8192 ? block | 0x3C00 : block;
    for (u16 dummy : {u16(0), u16(1)})
    {
        EEPROM chip;
        save = original;
        const auto packet = Request(false, width, address, data, dummy);
        u32 offset = 0xDEADBEEF;
        Check(Send(chip, save, std::span(packet).first(packet.size() - 1), 100, offset) == 0,
              "write committed before dummy");
        Check(save == original, "partial write changed old save");
        Check(chip.Active(100), "partial write not active");
        Check(Send(chip, save, std::span(packet).last(1), 100, offset) == 1 && offset == length - 8,
              "completed write callback range/count");
        auto expected = original;
        std::copy(data.begin(), data.end(), expected.end() - 8);
        Check(save == expected, "completed write changed neighbors or wrong bit order");
        const u64 readyAt = 100 + EEPROM::WriteBusyCycles;
        chip.Deselect(true);
        Check(!chip.Ready(readyAt - 1) && chip.Active(readyAt - 1), "abort canceled busy");
        Check(chip.Read(save, readyAt - 1) == 0, "busy read returned ready");
        Check(Send(chip, save, packet, readyAt - 1, offset) == 0 && save == expected,
              "busy write accepted another packet");
        Check(chip.Ready(readyAt) && !chip.Active(readyAt), "busy expiry boundary");
        Check(Send(chip, save, Request(true, width, block, {}, dummy), readyAt, offset) == 0,
              "read command committed data");
        chip.Deselect(false);
        Check(chip.Ready(readyAt) == 1 && chip.Active(readyAt), "ready polling consumed output");
        Check(Receive(chip, save, readyAt) == data && !chip.Active(readyAt), "read response/gap");
        // Same-data programming is still a completed operation for persistence.
        Check(Send(chip, save, packet, readyAt, offset) == 1, "same-data write lost notification");
        chip.Reset();
        Check(chip.Ready(readyAt) && !chip.Active(readyAt), "Reset failed to intentionally clear busy");
    }
    std::printf("packets/%u: %s\n", length, initial == Failures ? "PASS" : "FAIL");
}

static void LongProbe()
{
    const unsigned initial = Failures;
    std::array<u8, 512> save;
    for (unsigned i = 0; i < save.size(); ++i) save[i] = u8(i * 37 + (i >> 3) * 19 + 0x25);
    EEPROM chip;
    u32 offset = 42;
    for (u16 address : {u16(0), u16(1), u16(255), u16(256)})
    {
        Send(chip, save, Request(true, 14, address), 0, offset);
        chip.Deselect(false);
        const auto result = Receive(chip, save, 0);
        Check(std::equal(result.begin(), result.end(), save.begin() + (address >> 8) * 8),
              "existing GodMode9i 512-byte long-address probe");
    }
    Check(offset == 42, "read probe produced persistence offset");
    std::printf("long-probe: %s\n", initial == Failures ? "PASS" : "FAIL");
}

static void InFlight(u32 length)
{
    const unsigned initial = Failures;
    const unsigned width = length == 512 ? 6 : 14;
    std::vector<u8> save(length, 0x39);
    const std::array<u8, 8> data {0x12,0xE4,0xA7,0x10,0x55,0xAA,0x06,0xC3};
    const auto packet = Request(false, width, 5, data);
    // Mid-command, partial address, partial data, and awaiting write dummy.
    for (unsigned split : {1u, 4u, width + 19, unsigned(packet.size() - 1)})
    {
        EEPROM source, restored;
        std::fill(save.begin(), save.end(), 0x39);
        u32 offset = 1234;
        Send(source, save, std::span(packet).first(split), 200, offset);
        auto image = Snapshot(source, length);
        Check(Restore(restored, image, length), "restore partial write");
        Check(SameState(image, Snapshot(restored, length)), "partial fields not restored");
        Check(Send(restored, save, std::span(packet).subspan(split), 200, offset) == 1 && offset == 40,
              "restored command did not complete once");
        Check(std::equal(data.begin(), data.end(), save.begin() + 40), "restored partial payload");
        auto busy = Snapshot(restored, length);
        EEPROM busyRestored;
        Check(Restore(busyRestored, busy, length), "restore committed busy state");
        busyRestored.Deselect(false);
        busyRestored.Deselect(true);
        Check(!busyRestored.Ready(200 + EEPROM::WriteBusyCycles - 1) &&
              busyRestored.Ready(200 + EEPROM::WriteBusyCycles), "restored busy duration");
    }
    for (unsigned readCount : {0u, 2u, 19u, 67u})
    {
        EEPROM source, restored;
        u32 offset = 999;
        Send(source, save, Request(true, width, 5), 900, offset);
        source.Deselect(false);
        for (unsigned i = 0; i < readCount; ++i) source.Read(save, 900);
        auto image = Snapshot(source, length);
        Check(Restore(restored, image, length), "restore read output");
        Check(restored.Ready(900) == 1, "ready during output");
        for (unsigned i = readCount; i < 68; ++i)
        {
            const u16 expected = i < 4 ? 0 : (data[(i - 4) / 8] >> (7 - (i - 4) % 8)) & 1;
            Check(restored.Read(save, 900) == expected, "restored output bit position");
        }
        Check(!restored.Active(900), "completed restored read still active");
    }
    std::printf("in-flight/%u: %s\n", length, initial == Failures ? "PASS" : "FAIL");
}

static void RejectedStates()
{
    const unsigned initial = Failures;
    std::array<u8, 512> save {};
    EEPROM source, target;
    u32 offset = 77;
    Send(source, save, Request(true, 6, 31), 0, offset);
    source.Read(save, 0); // A live partial response must survive rejected loads.
    target = source;
    const auto before = Snapshot(target, 512);
    struct Corruption { unsigned Offset; u8 Value; };
    // Explicit wire fields, not object layout: phase, address width, count,
    // block address, unused staged data, impossible/existing busy state.
    for (const auto change : {Corruption{32, 0xFF}, {33, 14}, {34, 68}, {35, 64}, {37, 1}, {45, 1}})
    {
        auto invalid = before;
        invalid.Bytes[change.Offset] = change.Value;
        Check(!Restore(target, invalid, 512) && SameState(before, Snapshot(target, 512)),
              "invalid load changed live chip");
    }
    auto wrongCapacity = before;
    Check(!Restore(target, wrongCapacity, 8192) && SameState(before, Snapshot(target, 512)),
          "active state accepted wrong capacity");
    // Truncation inside command/address/data/deadline. Repair framing so these
    // reach the actual field reads instead of failing only the global header.
    for (u32 cut : {33u, 36u, 40u, 52u})
    {
        auto truncated = before;
        truncated.Length = cut;
        std::memcpy(truncated.Bytes.data() + 8, &cut, 4);
        const u32 sectionLength = cut - 16;
        std::memcpy(truncated.Bytes.data() + 20, &sectionLength, 4);
        Check(!Restore(target, truncated, 512) && SameState(before, Snapshot(target, 512)),
              "truncated field load changed live chip");
    }
    EEPROM idle;
    auto invalidBusy = Snapshot(idle, 512);
    invalidBusy.Bytes[45] = 1; // No representable commit can produce deadline 1.
    Check(!Restore(target, invalidBusy, 512) && SameState(before, Snapshot(target, 512)),
          "impossible busy deadline accepted");
    auto failedImage = before;
    Savestate failed(failedImage.Bytes.data(), failedImage.Length, false);
    failed.Error = true;
    target.DoSavestate(&failed, 512);
    Check(failed.Error && SameState(before, Snapshot(target, 512)), "preexisting error changed chip");
    std::printf("rejected-states: %s\n", initial == Failures ? "PASS" : "FAIL");
}

static void Boundaries()
{
    const unsigned initial = Failures;
    std::array<u8, 512> save {};
    const std::array<u8, 8> data {1,2,3,4,5,6,7,8};
    const auto packet = Request(false, 6, 0, data);
    EEPROM chip;
    u32 offset = 90;
    Send(chip, save, std::span(packet).first(40), 0, offset);
    chip.Deselect(false);
    Send(chip, save, std::span(packet).subspan(40), 0, offset);
    Check(std::all_of(save.begin(), save.end(), [](u8 b) { return b == 0; }), "split packet wrote save");
    chip.Reset();
    Send(chip, save, Request(true, 6, 0), 0, offset);
    chip.Read(save, 0);
    chip.Deselect(false);
    Check(!chip.Active(0), "partial output survived deselection");

    std::array<u8, 8192> other {};
    Send(chip, save, std::span(packet).first(20), 0, offset);
    Send(chip, other, std::span(packet).subspan(20), 0, offset);
    Check(std::all_of(other.begin(), other.end(), [](u8 b) { return b == 0; }), "capacity switch kept pending write");
    chip.Reset();
    Check(Send(chip, {}, packet, 0, offset) == 0 && !chip.Active(0), "empty capacity accepted");
    std::array<u8, 513> unsupported {};
    Check(Send(chip, unsupported, packet, 0, offset) == 0 && !chip.Active(0), "unsupported capacity accepted");

    constexpr u64 max = std::numeric_limits<u64>::max();
    chip.Reset();
    Check(Send(chip, save, packet, max - EEPROM::WriteBusyCycles + 1, offset) == 0 &&
          std::all_of(save.begin(), save.end(), [](u8 b) { return b == 0; }), "busy deadline overflow modified save");
    Check(Send(chip, save, packet, max - EEPROM::WriteBusyCycles, offset) == 1 &&
          !chip.Ready(max - 1) && chip.Ready(max), "largest representable busy deadline");
    auto image = Snapshot(chip, 512);
    EEPROM restored;
    Check(Restore(restored, image, 512) && !restored.Ready(max - 1) && restored.Ready(max),
          "largest deadline did not round trip");
    std::printf("boundaries: %s\n", initial == Failures ? "PASS" : "FAIL");
}

int main()
{
    Packets(512);
    Packets(8192);
    LongProbe();
    InFlight(512);
    InFlight(8192);
    RejectedStates();
    Boundaries();
    std::printf("EEPROM failures=%u\n", Failures);
    return Failures ? 1 : 0;
}
