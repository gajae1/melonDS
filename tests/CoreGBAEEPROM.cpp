// SPDX-License-Identifier: GPL-3.0-or-later
// Actual NDS MMIO DMA -> GBACartSlot -> ParseROM/CartGame acceptance.
// Generated ROM/save only; PlatformHeadless owns the non-persistent host stubs.
#include "Args.h"
#include "NDS.h"
#include "Savestate.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace melonDS;

namespace
{
using Block = std::array<u8, 8>;
constexpr Block Payload {0x01, 0x80, 0xA5, 0x5A, 0xFF, 0x00, 0x37, 0xC2};
constexpr u32 EEPROMPort = 0x09FFFF00;
constexpr u64 BusyCycles = 217841; // Selected chip model: 6.5 ms at the DS clock.

void Require(bool ok, const char* reason)
{
    if (!ok) throw std::runtime_error(reason);
}

// GodMode9i ARM7 gba.c, commit 8a8f806054133978e186d3a55d4058c551dde497:
// https://github.com/DS-Homebrew/GodMode9i/blob/8a8f806054133978e186d3a55d4058c551dde497/arm7/source/gba.c#L30
// Exact consumer wire order/lengths, with a vector replacing its C VLA.
// Both original descending byte loops advance their input/output pointers;
// bytes are NOT reversed. Long requests deliberately retain all 14 address bits.
std::vector<u16> ConsumerPacket(bool read, u16 address, bool shortAddress,
                                const Block& data = {})
{
    std::vector<u16> packet {1, u16(read)};
    for (int bit = shortAddress ? 5 : 13; bit >= 0; --bit)
        packet.push_back((address >> bit) & 1);
    if (!read)
        for (u8 byte : data)
            for (int bit = 7; bit >= 0; --bit) packet.push_back((byte >> bit) & 1);
    packet.push_back(0);
    return packet;
}

Block ConsumerDecode(std::span<const u16> packet)
{
    Require(packet.size() == 68, "consumer response must contain 68 halfwords");
    Block out {};
    size_t position = 4;
    for (auto& byte : out)
        for (int bit = 7; bit >= 0; --bit) byte |= (packet[position++] & 1) << bit;
    return out;
}

struct Console : NDS
{
    static NDSArgs Arguments()
    {
        NDSArgs args;
        args.JIT.reset();
        return args;
    }
    Console() : NDS(Arguments())
    {
        Reset();
        ARM9Write8(0x04000247, 0); // Shared WRAM to ARM9; ARM7 uses private WRAM.
        AdvanceTo(1000);
    }
    u16 Read16(u32 cpu, u32 address)
    {
        return cpu ? ARM7Read16(address) : ARM9Read16(address);
    }
    void Write16(u32 cpu, u32 address, u16 value)
    {
        if (cpu) ARM7Write16(address, value); else ARM9Write16(address, value);
    }
    void Write32(u32 cpu, u32 address, u32 value)
    {
        if (cpu) ARM7Write32(address, value); else ARM9Write32(address, value);
    }
    u64 Now(u32 cpu) const
    {
        return cpu ? ARM7Timestamp : ARM9Timestamp >> ARM9ClockShift;
    }
    void AdvanceTo(u64 timestamp)
    {
        SysTimestamp = timestamp;
        ARM7Timestamp = timestamp;
        ARM9Timestamp = timestamp << ARM9ClockShift;
    }
    bool Active(u32 cpu, u32 channel) const { return DMAs[4 * cpu + channel].IsRunning(); }
    void Slice(u32 cpu, u32 channel, u32 cycles)
    {
        CurCPU = cpu;
        if (cpu) ARM7Target = ARM7Timestamp + cycles;
        else ARM9Target = ARM9Timestamp + (u64(cycles) << ARM9ClockShift);
        DMAs[4 * cpu + channel].Run();
        SysTimestamp = std::max(SysTimestamp, Now(cpu));
    }
};

std::vector<u8> InitialSave(u32 length)
{
    std::vector<u8> save(length);
    for (u32 i = 0; i < length; ++i)
        save[i] = u8((i * 37 + (i >> 3) * 19 + 0x25) ^ (i >> 8));
    save[7] = 0x34; // Last two bits zero: distinguishes low/high boundary clocks.
    return save;
}

std::unique_ptr<GBACart::CartCommon> ParseCart(std::span<const u8> save)
{
    std::array<u8, 0x200> rom;
    rom.fill(0xA6);
    std::memcpy(rom.data() + 0xAC, "TEST", 4);
    std::memcpy(rom.data() + 0x100, "EEPROM_V124", 11);
    auto cart = GBACart::ParseROM(rom.data(), u32(rom.size()), save.data(), u32(save.size()));
    Require(bool(cart), "production ParseROM rejected generated cart");
    Require(cart->GetSaveMemoryLength() == save.size(), "ParseROM changed known save capacity");
    return cart;
}

enum class AddressMode { Increment, Decrement, Fixed };

struct Fixture
{
    std::unique_ptr<Console> Nds = std::make_unique<Console>();
    u32 CPU, Length, Channel = 3, Port = EEPROMPort, RAM;
    bool Word = false;
    AddressMode Mode = AddressMode::Increment;
    unsigned Slices = 0;

    explicit Fixture(u32 length = 512, u32 cpu = 1) : CPU(cpu), Length(length),
        RAM(cpu ? 0x03800000 : 0x03000000)
    {
        Nds->SetGBACart(ParseCart(InitialSave(length)));
        Owner(cpu);
        Timing(0x0C);
    }
    void Owner(u32 cpu)
    {
        const u16 old = Nds->ARM9Read16(0x04000204);
        Nds->ARM9Write16(0x04000204, (old & ~0x80) | (cpu << 7));
    }
    void Timing(u16 bits)
    {
        const u16 old = Nds->Read16(CPU, 0x04000204);
        Nds->Write16(CPU, 0x04000204, (old & ~0x1C) | bits);
    }
    std::vector<u8> Save() const
    {
        const auto* cart = Nds->GetGBACart();
        Require(cart && cart->GetSaveMemoryLength() == Length, "save owner/capacity lost");
        const auto* data = cart->GetSaveMemory();
        return {data, data + Length};
    }
    u32 Registers() const { return 0x040000B0 + 12 * Channel; }
    void Stage(std::span<const u16> packet)
    {
        for (size_t i = 0; i < packet.size(); ++i)
            Nds->Write16(CPU, RAM + u32(i * 2), packet[i]);
        // Word DMA clocks one extra zero after an odd-length command. It is not
        // counted as a second packet or used to identify the chip capacity.
        if (packet.size() & 1) Nds->Write16(CPU, RAM + u32(packet.size() * 2), 0);
    }
    void Begin(u32 halfwords, bool receive)
    {
        Require(!Nds->Active(CPU, Channel), "attempted to replace a running test DMA");
        const u32 regs = Registers();
        Nds->Write32(CPU, regs, receive ? Port : RAM);
        Nds->Write32(CPU, regs + 4, receive ? RAM : Port);
        const u32 mode = Mode == AddressMode::Fixed ? 2 : Mode == AddressMode::Decrement ? 1 : 0;
        Nds->Write32(CPU, regs + 8, 0x80000000 | (Word ? 0x04000000 : 0) |
            (mode << (receive ? 23 : 21)) | (Word ? (halfwords + 1) / 2 : halfwords));
    }
    void Units(unsigned count)
    {
        for (unsigned i = 0; i < count; ++i)
        {
            Require(Nds->Active(CPU, Channel), "DMA ended before planned interruption");
            Nds->Slice(CPU, Channel, 1); // One actual unit then target yield.
            ++Slices;
        }
    }
    void Drain()
    {
        for (unsigned i = 0; i < 512 && Nds->Active(CPU, Channel); ++i)
        {
            Nds->Slice(CPU, Channel, 40);
            ++Slices;
        }
        Require(!Nds->Active(CPU, Channel), "DMA exceeded 512 bounded slices");
        Require(!(Nds->Read16(CPU, Registers() + 10) & 0x8000), "completed DMA kept enable set");
    }
    void Send(std::span<const u16> packet)
    {
        Stage(packet);
        Begin(u32(packet.size()), false);
        Drain();
    }
    std::vector<u16> Response(u32 count = 68)
    {
        std::vector<u16> packet(count);
        Stage(packet);
        Begin(count, true);
        Drain();
        return ReadBuffer(count);
    }
    std::vector<u16> ReadBuffer(u32 count)
    {
        std::vector<u16> packet(count);
        for (u32 i = 0; i < count; ++i) packet[i] = Nds->Read16(CPU, RAM + 2 * i);
        return packet;
    }
    Block Read(u16 block, bool shortAddress)
    {
        Send(ConsumerPacket(true, block, shortAddress));
        // The real EndDMA(false) between these transfers must retain the command.
        return ConsumerDecode(Response());
    }
    void WaitReady()
    {
        const u64 start = Nds->Now(CPU);
        for (u64 elapsed = 0; elapsed <= BusyCycles + 1; elapsed += 4096)
        {
            Nds->AdvanceTo(start + elapsed);
            if (Nds->Read16(CPU, Port) & 1) return;
        }
        Nds->AdvanceTo(start + BusyCycles + 1);
        Require(Nds->Read16(CPU, Port) & 1, "bounded consumer ready polling expired");
    }
    void Write(u16 block, const Block& data = Payload)
    {
        Send(ConsumerPacket(false, block, Length == 512, data));
        WaitReady(); // Direct MMIO reads do not advance emulated time themselves.
    }
};

void ExpectBlock(std::span<const u8> save, u32 block, const Block& data)
{
    Require(std::equal(data.begin(), data.end(), save.begin() + 8 * block), "consumer read returned wrong block/order");
}

void ExpectWrite(Fixture& f, std::vector<u8> expected, u32 block)
{
    std::copy(Payload.begin(), Payload.end(), expected.begin() + 8 * block);
    Require(f.Save() == expected, "DMA write changed wrong bytes, neighbors, or no bytes");
}

void Consumer()
{
    for (u32 length : {512u, 8192u})
    {
        Fixture f(length);
        const auto before = f.Save();
        const u16 block = u16(length / 8 - 1);
        ExpectBlock(before, block, f.Read(block, length == 512));
        Require(f.Save() == before, "consumer read modified storage");
        f.Write(block);
        ExpectWrite(f, before, block);
        ExpectBlock(f.Save(), block, f.Read(block, length == 512));
        for (u16 address : {u16(0), u16(1), u16(255), u16(256)})
            ExpectBlock(f.Save(), length == 512 ? address >> 8 : address,
                        f.Read(address, false));
        Require(f.Slices > 13, "consumer did not exercise target yields");
    }
}

void Isolation()
{
    for (u32 cpu : {0u, 1u})
    {
        Fixture f(cpu ? 8192 : 512, cpu);
        const auto before = f.Save();
        auto packet = ConsumerPacket(false, 5, f.Length == 512, Payload);
        for (u32 i = 0; i < packet.size(); ++i)
            f.Nds->Write16(cpu, f.Port + 2 * i, packet[i]);
        packet.push_back(0);
        for (u32 i = 0; i < packet.size(); i += 2)
            f.Nds->Write32(cpu, f.Port + 2 * i, packet[i] | (u32(packet[i + 1]) << 16));
        Require(f.Nds->Read16(cpu ^ 1, f.Port) == 0, "non-owner Slot-2 read was not gated");
        Require(f.Save() == before, "isolated CPU writes assembled a DMA packet");
        f.Write(5);
        ExpectWrite(f, before, 5);
    }
}

void Boundaries()
{
    for (const char* action : {"yield", "finish", "cancel", "owner", "cpu", "stall"})
    {
        const std::string kind(action);
        Fixture f(512, kind == "stall" ? 0 : 1);
        const auto before = f.Save();
        const auto packet = ConsumerPacket(false, 5, true, Payload);
        if (kind == "finish")
        {
            f.Send(std::span(packet).first(36));
            Require(f.Save() == before, "normal completion committed a partial write");
            f.Send(std::span(packet).subspan(36));
        }
        else
        {
            f.Stage(packet);
            f.Begin(u32(packet.size()), false);
            f.Units(12);
            Require(f.Nds->Active(f.CPU, f.Channel) && f.Save() == before, "partial DMA was not pending");
            if (kind == "cancel") f.Nds->Write32(f.CPU, f.Registers() + 8, 0);
            else if (kind == "owner") { f.Owner(f.CPU ^ 1); f.Owner(f.CPU); }
            else if (kind == "cpu") f.Nds->Write16(f.CPU, f.Port, 0);
            else if (kind == "stall")
            {
                // Actual GX stall/un-stall events between units. No DMA unit is
                // run while stalled. This asserts the selected model, not pins.
                f.Nds->GXFIFOStall();
                Require(f.Nds->Active(f.CPU, f.Channel), "standalone stall ended DMA");
                f.Nds->GXFIFOUnstall();
            }
            f.Drain();
        }
        if (kind == "yield" || kind == "stall") ExpectWrite(f, before, 5);
        else
        {
            Require(f.Save() == before, (kind + " allowed interrupted write").c_str());
            f.Write(5);
            ExpectWrite(f, before, 5);
        }
    }
}

void Arbitration()
{
    for (bool otherCPU : {false, true})
    {
        Fixture f(8192);
        const auto before = f.Save();
        const auto packet = ConsumerPacket(false, 9, false, Payload);
        f.Stage(packet);
        f.Begin(u32(packet.size()), false);
        f.Units(20);
        const u32 cpu = otherCPU ? 0 : f.CPU;
        const u32 ram = otherCPU ? 0x02001000 : 0x03801000;
        f.Nds->AdvanceTo(f.Nds->Now(f.CPU));
        f.Nds->Write16(cpu, ram, 0xB57A);
        f.Nds->Write32(cpu, 0x040000B0, ram);
        f.Nds->Write32(cpu, 0x040000B4, ram + 0x100);
        f.Nds->Write32(cpu, 0x040000B8, 0x80000001);
        f.Nds->Slice(cpu, 0, 100);
        Require(!f.Nds->Active(cpu, 0) && f.Nds->Read16(cpu, ram + 0x100) == 0xB57A,
                "intervening real DMA0 did not execute");
        f.Drain();
        if (otherCPU) ExpectWrite(f, before, 9);
        else Require(f.Save() == before, "same-CPU DMA preemption preserved partial write");
    }
}

void Model()
{
    // These assert the accepted emulator N/S model. They do not claim physical
    // EEPROM compatibility for every CPU, address mode, width or EXMEM setting.
    struct Case { u32 cpu; bool word; AddressMode mode; bool mainRAM; u16 timing; };
    const Case cases[] {
        {0, false, AddressMode::Increment, false, 0x0C},
        {0, true,  AddressMode::Increment, false, 0x0C},
        {1, false, AddressMode::Fixed,     false, 0x0C},
        {1, true,  AddressMode::Decrement, false, 0x0C},
        {0, false, AddressMode::Decrement, true,  0x08},
        {1, true,  AddressMode::Fixed,     true,  0x1C},
    };
    for (const auto& test : cases)
    {
        Fixture f(512, test.cpu);
        f.Word = test.word;
        f.Mode = test.mode;
        f.Port = 0x09001200; // All +/-/fixed halfwords stay in EEPROM decode.
        if (test.mainRAM) f.RAM = 0x02004000;
        f.Timing(test.timing);
        const auto before = f.Save();
        ExpectBlock(before, 0, f.Read(0, true));
        f.Write(5);
        ExpectWrite(f, before, 5);
    }
    for (bool word : {false, true})
    {
        Fixture f(512, 1);
        f.Word = word;
        f.Port = 0x09001203; // The core masks the unused DMA address bits.
        const auto before = f.Save();
        ExpectBlock(before, 0, f.Read(0, true));
        f.Write(5);
        ExpectWrite(f, before, 5);
    }
}

void WordBoundary()
{
    Fixture f(512, 0);
    f.Word = true;
    const auto before = f.Save();
    auto packet = ConsumerPacket(false, 5, true, Payload);
    packet.insert(packet.begin(), 0); // Idle clock puts write dummy in HIGH half.
    f.Port = 0x09020000 - u32(packet.size() * 2);
    f.Send(packet);
    Require(f.Save() == before, "128 KiB last-halfword N completed an interrupted write");

    f.Port = EEPROMPort;
    f.Send(ConsumerPacket(true, 0, true));
    f.Port = 0x09020000 - 68 * 2;
    const auto response = f.Response();
    for (u32 i = 4; i < 67; ++i)
        Require((response[i] & 1) == ((before[(i - 4) / 8] >> (7 - (i - 4) % 8)) & 1),
                "word low halfword was reordered or deselected before the boundary");
    Require((response[66] & 1) == 0 && (response[67] & 1) == 1,
            "last HIGH halfword must reselect after the valid LOW halfword");
    Require(f.Save() == before, "interrupted read changed storage");
}

void Busy()
{
    for (u32 cpu : {0u, 1u})
    {
        Fixture f(8192, cpu);
        const auto before = f.Save();
        f.Send(ConsumerPacket(false, 7, false, Payload));
        const u64 committed = f.Nds->Now(cpu);
        ExpectWrite(f, before, 7);
        Require(f.Nds->Read16(cpu, f.Port) == 0 && f.Nds->Now(cpu) == committed,
                "immediate MMIO poll was ready or advanced time");
        const auto programmed = f.Save();
        f.Send(ConsumerPacket(false, 8, false, Payload));
        Require(f.Save() == programmed, "busy chip accepted another DMA write");
        f.Nds->AdvanceTo(committed + BusyCycles - 1);
        Require(f.Nds->Read16(cpu, f.Port) == 0, "busy became ready before deadline");
        f.Nds->AdvanceTo(committed + BusyCycles);
        Require(f.Nds->Read16(cpu, f.Port) == 1, "busy deadline/CPU clock normalization incorrect");
        Require(f.Save() == programmed, "ready polling changed storage");
    }
}

std::vector<u8> Snapshot(Console& nds)
{
    Savestate state;
    Require(nds.DoSavestate(&state) && !state.Error, "mid-DMA core serialization failed");
    state.Finish();
    Require(!state.Error && state.MinorVersion() == 5, "pending DMA/chip did not require state 14.5");
    const auto* bytes = static_cast<const u8*>(state.Buffer());
    return {bytes, bytes + state.Length()};
}

void Restore(Console& nds, std::vector<u8>& image)
{
    Savestate state(image.data(), u32(image.size()), false);
    Require(!state.Error && nds.DoSavestate(&state) && !state.Error, "mid-DMA core state restore failed");
}

void State()
{
    Fixture writing;
    const auto before = writing.Save();
    const auto packet = ConsumerPacket(false, 5, true, Payload);
    writing.Stage(packet);
    writing.Begin(u32(packet.size()), false);
    writing.Units(12);
    auto state = Snapshot(*writing.Nds);
    writing.Drain();
    ExpectWrite(writing, before, 5);
    Restore(*writing.Nds, state);
    Require(writing.Nds->Active(writing.CPU, writing.Channel) && writing.Save() == before,
            "restore lost active DMA or staged-write storage");
    writing.Drain();
    ExpectWrite(writing, before, 5);

    Fixture reading(512, 0);
    reading.Word = true;
    const auto initial = reading.Save();
    reading.Send(ConsumerPacket(true, 0, true));
    reading.Stage(std::vector<u16>(68));
    reading.Begin(68, true);
    reading.Units(7); // Includes four dummy clocks and part of the second byte.
    state = Snapshot(*reading.Nds);
    reading.Drain();
    Restore(*reading.Nds, state);
    Require(reading.Nds->Active(reading.CPU, reading.Channel), "mid-read DMA did not resume");
    reading.Drain();
    ExpectBlock(initial, 0, ConsumerDecode(reading.ReadBuffer(68)));
    Require(reading.Save() == initial, "restored read changed storage");
}

void Replacement()
{
    for (bool replaceCart : {false, true})
    {
        Fixture f;
        const auto packet = ConsumerPacket(false, 5, true, Payload);
        std::vector<u16> stream(packet.begin(), packet.begin() + 12);
        // A COMPLETE command in the old DMA's suffix must also be suppressed.
        // Merely testing an incomplete suffix would miss a cleared block latch.
        stream.insert(stream.end(), packet.begin(), packet.end());
        f.Stage(stream);
        f.Begin(u32(stream.size()), false);
        f.Units(12);
        const std::vector<u8> imported(f.Length, replaceCart ? 0x77 : 0x96);
        if (replaceCart) f.Nds->SetGBACart(ParseCart(imported));
        else f.Nds->SetGBASave(imported.data(), u32(imported.size()));
        Require(f.Nds->Active(f.CPU, f.Channel), "import/replacement unexpectedly stopped core DMA");
        f.Drain();
        Require(f.Save() == imported, "old active DMA wrote a fresh packet into replacement storage");
        f.Write(5);
        ExpectWrite(f, imported, 5);
    }
}
}

int main(int argc, char** argv)
{
    const std::pair<const char*, void (*)()> groups[] {
        {"consumer", Consumer}, {"isolation", Isolation}, {"boundaries", Boundaries},
        {"arbitration", Arbitration}, {"model", Model}, {"word-boundary", WordBoundary},
        {"busy", Busy}, {"state", State}, {"replacement", Replacement},
    };
    if (argc > 2) return 2;
    bool found = argc == 1;
    unsigned failures = 0;
    for (const auto& [name, run] : groups)
    {
        if (argc == 2 && std::strcmp(argv[1], name)) continue;
        found = true;
        try { run(); std::printf("core-gba-eeprom/%s: PASS\n", name); }
        catch (const std::exception& error)
        {
            ++failures;
            std::fprintf(stderr, "core-gba-eeprom/%s: FAIL: %s\n", name, error.what());
        }
    }
    return !found ? 2 : failures ? 1 : 0;
}
