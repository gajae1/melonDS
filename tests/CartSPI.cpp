// SPDX-License-Identifier: GPL-3.0-or-later
// Real DS MMIO, slot, scheduler, retail EEPROM and savestate. SPI callbacks are
// observed without replacing their implementation. No external game/BIOS/files.
// Register contract: libnds v1.8.0 include/nds/card.h (AUXSPICNT 13/7/6).
#include "Args.h"
#include "NDS.h"
#include "DSi.h"
#include "NDSCart/CartRetail.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

using namespace melonDS;
static unsigned Failures = 0;
static void Check(bool ok, const char* reason)
{
    if (!ok) { ++Failures; std::fprintf(stderr, "%s\n", reason); }
}

struct ObservedCart : NDSCart::CartRetail
{
    unsigned Selects = 0, Releases = 0;
    static std::unique_ptr<u8[]> MakeROM(bool key1)
    {
        auto rom = std::make_unique<u8[]>(0x10000);
        if (key1) std::memcpy(rom.get() + 0x0C, "KEYT", 4);
        return rom;
    }
    explicit ObservedCart(bool key1 = false) : CartRetail(MakeROM(key1), 0x10000, key1 ? 0x1FC2 : 0, false,
                               {0, 0x10000, 1}, nullptr, 0, nullptr) {}
    void SPISelect() override { ++Selects; CartRetail::SPISelect(); }
    void SPIRelease() override { ++Releases; CartRetail::SPIRelease(); }
    void ClearCounts() { Selects = Releases = 0; }
};

struct Console : NDS
{
    using NDS::NDS;
    void FinishROM(u32 cpu)
    {
        const auto event = cpu == 0 ? Event_CartROMTransfer9 : Event_CartROMTransfer7;
        if (!(SchedListMask & (1u << event))) throw std::runtime_error("Missing ROM event");
        const auto timestamp = SchedList[event].Timestamp;
        RunSystem(timestamp);
        ARM9Timestamp = timestamp << ARM9ClockShift;
        ARM7Timestamp = timestamp;
    }
    void Finish(u32 cpu)
    {
        const auto event = cpu == 0 ? Event_CartSPITransfer9 : Event_CartSPITransfer7;
        if (!(SchedListMask & (1u << event))) throw std::runtime_error("SPI event was not scheduled");
        const auto timestamp = SchedList[event].Timestamp;
        RunSystem(timestamp);
        ARM9Timestamp = timestamp << ARM9ClockShift;
        ARM7Timestamp = timestamp;
        if (NDSCartSlots[0]->ReadSPICnt(cpu) & 0x80) throw std::runtime_error("SPI event did not clear busy");
    }
};

struct Fixture
{
    std::unique_ptr<Console> Machine;
    ObservedCart* Cart;
    u32 CPU;
    explicit Fixture(u32 cpu, bool key1 = false) : CPU(cpu)
    {
        NDSArgs args;
        args.JIT = std::nullopt;
        Machine = std::make_unique<Console>(std::move(args));
        Machine->Reset();
        auto cart = std::make_unique<ObservedCart>(key1);
        Cart = cart.get();
        Machine->SetNDSCart(std::move(cart));
        // ARM9 owns EXMEMCNT's slot-selection bit even when testing ARM7 I/O.
        Machine->ARM9Write16(0x04000204, cpu ? 0x0800 : 0);
        if (cpu) Machine->ARM7Write32(0x040001A4, 1u << 29);
        else Machine->ARM9Write32(0x040001A4, 1u << 29);
        Cart->ClearCounts();
    }
    void Write8(u32 address, u8 value, u32 cpu)
    {
        if (cpu) Machine->ARM7Write8(address, value);
        else Machine->ARM9Write8(address, value);
    }
    void Control(u16 value)
    {
        if (CPU) Machine->ARM7Write16(0x040001A0, value);
        else Machine->ARM9Write16(0x040001A0, value);
    }
    void Low(u8 value) { Write8(0x040001A0, value, CPU); }
    void High(u8 value) { Write8(0x040001A1, value, CPU); }
    void Data(u8 value, bool finish = true)
    {
        Write8(0x040001A2, value, CPU);
        if (finish) Machine->Finish(CPU);
    }
    u16 Count() const { return Machine->NDSCartSlots[0]->ReadSPICnt(CPU); }
    void StartWrite()
    {
        Control(0xA000); // enable, SPI, no hold: finish WREN with CS high
        Data(0x06);
        Cart->ClearCounts();
        Control(0xA040);
        Data(0x02);
        Data(0x05);
        Data(0xA1);
        if (Cart->GetSaveMemory()[5] != 0xA1) throw std::runtime_error("Initial EEPROM write failed");
    }
    void EndWrite()
    {
        Data(0xB2);
        Control(0x8041); // actual bit13 falling edge terminates WRITE
        Check(Cart->GetSaveMemory()[5] == 0xA1 && Cart->GetSaveMemory()[6] == 0xB2,
              "A partial control write lost EEPROM data in a held transaction");
        Check(Cart->Selects == 1 && Cart->Releases == 1, "Unexpected chip-select edges during WRITE");
        std::printf("CPU%u SRAM[5:6]=%02X,%02X select=%u release=%u\n", CPU,
                    Cart->GetSaveMemory()[5], Cart->GetSaveMemory()[6], Cart->Selects, Cart->Releases);
    }
};

static void TestKey1State(u32 cpu)
{
    auto start = [cpu](Fixture& f, const u8 (&command)[8], u32 size) {
        f.Control(0xC000);
        for (u32 i = 0; i < 8; i++) f.Write8(0x040001A8 + i, command[i], cpu);
        const u32 control = (1u << 31) | (1u << 29) | (size << 24);
        if (cpu) f.Machine->ARM7Write32(0x040001A4, control);
        else f.Machine->ARM9Write32(0x040001A4, control);
        f.Machine->FinishROM(cpu);
    };
    // Chip-ID command 10 00 00 00 00 00 00 00 encrypted for game code KEYT
    // and the generated BIOS's zero KEY1 table. No firmware image is required.
    constexpr u8 enter[8] = {0x3C};
    constexpr u8 query[8] = {0x42, 0xD9, 0x97, 0x5C, 0x24, 0xE1, 0x99, 0x07};
    auto readID = [cpu, &start, &query](Fixture& f) {
        start(f, query, 7);
        return cpu ? f.Machine->ARM7Read32(0x04100010) : f.Machine->ARM9Read32(0x04100010);
    };
    Fixture original(cpu, true);
    start(original, enter, 0);
    Savestate saved;
    if (!original.Machine->DoSavestate(&saved) || saved.Error) throw std::runtime_error("KEY1 save failed");
    saved.Finish();
    const u32 before = readID(original);
    Check(before == 0x1FC2, "Generated encrypted chip-ID command failed before saving");

    // Reusing the first console would hide a missing key schedule in the save.
    Fixture restored(cpu, true);
    Savestate load(saved.Buffer(), saved.Length(), false);
    if (!restored.Machine->DoSavestate(&load) || load.Error) throw std::runtime_error("KEY1 restore failed");
    const u32 after = readID(restored);
    Check(after == 0x1FC2, "Cold savestate load did not restore KEY1 command decryption");
    std::printf("CPU%u encrypted chip ID before=%08X restored=%08X\n", cpu, before, after);
}

int main(int argc, char** argv)
try
{
    if (argc != 2) return 2;
    const std::string mode = argv[1];
    for (u32 cpu : {0u, 1u})
    {
        if (mode == "dsi-state") break;
        if (mode == "key1-state") { TestKey1State(cpu); continue; }
        if ((mode == "arm9-low" && cpu != 0) || (mode == "arm7-low" && cpu != 1)) continue;
        Fixture f(cpu);
        if (mode == "state-bounds")
        {
            auto* original = f.Cart->GetSaveMemory();
            const u32 length = f.Cart->GetSaveMemoryLength();
            original[0] = 0x5A;
            Savestate malformed;
            f.Cart->CartCommon::DoSavestate(&malformed);
            u32 oversized = 1024 * 1024;
            malformed.Var32(&oversized);
            malformed.Finish();
            Savestate load(malformed.Buffer(), malformed.Length(), false);
            f.Cart->DoSavestate(&load);
            Check(load.Error, "Truncated SRAM payload was accepted");
            Check(f.Cart->GetSaveMemoryLength() == length && f.Cart->GetSaveMemory() == original
                && original[0] == 0x5A, "Rejected SRAM payload replaced live save memory");
        }
        else if (mode == "control" || mode == "arm9-low" || mode == "arm7-low" || mode == "state-resume")
        {
            f.StartWrite();
            if (mode == "state-resume")
            {
                Savestate saved;
                if (!f.Machine->DoSavestate(&saved) || saved.Error) throw std::runtime_error("State save failed");
                f.Control(0x8040);
                Savestate restore(saved.Buffer(), saved.Length(), false);
                if (!f.Machine->DoSavestate(&restore) || restore.Error) throw std::runtime_error("State restore failed");
                // The saved hardware CS was low; observers aren't serialized.
                f.Cart->Selects = 1;
                f.Cart->Releases = 0;
            }
            if (mode == "control") f.Control(0xA041);
            else f.Low(0x41);
            Check(f.Count() == 0xA041, "Partial write changed unrelated register bits");
            Check(f.Cart->Releases == 0, "Lower-byte write generated a false CS rising edge");
            f.EndWrite();
        }
        else if (mode == "busy-low")
        {
            f.Control(0xA040);
            f.Data(0x05, false);
            f.Low(0x41); // requested bit7=0 must not clear read-only busy
            Check(f.Count() == 0xA0C1 && f.Cart->Releases == 0, "Busy lower-byte write changed CS or busy");
            f.Machine->Finish(cpu);
            Check(f.Count() == 0xA041 && f.Cart->Selects == 1 && f.Cart->Releases == 0,
                  "Completing an in-flight transfer changed held CS");
        }
        else if (mode == "mode-control")
        {
            f.Control(0xA040);
            f.Data(0x05);
            f.High(0x20); // bit15 alone does not change CS
            f.High(0xA0);
            Check(f.Cart->Selects == 1 && f.Cart->Releases == 0, "Enable toggling changed CS");
            f.High(0x80);
            Check(f.Cart->Releases == 1, "Real high-byte SPI mode clear failed to release CS");
            f.High(0xA0);
            Check(f.Cart->Selects == 2, "Real high-byte SPI mode set failed to select CS");
            f.Control(0x8040);
            f.Control(0xA040);
            Check(f.Cart->Selects == 3 && f.Cart->Releases == 2, "Normal full-word mode transition regressed");
        }
        else if (mode == "ownership")
        {
            f.Control(0xA040);
            f.Data(0x05);
            const auto other = cpu ^ 1;
            f.Write8(0x040001A1, 0xA0, other);
            f.Write8(0x040001A0, 0x40, other);
            f.Write8(0x040001A2, 0x05, other);
            f.Machine->Finish(other);
            f.Write8(0x040001A0, 0x41, other);
            f.Write8(0x040001A1, 0x80, other);
            Check(f.Cart->Selects == 1 && f.Cart->Releases == 0, "Non-owner register writes reached the cart");
            auto held = f.Machine->NDSCartSlots[0]->EjectCart();
            const auto selects = f.Cart->Selects, releases = f.Cart->Releases;
            f.Low(0x41);
            f.High(0x80);
            Check(f.Cart->Selects == selects && f.Cart->Releases == releases, "Ejected cart received SPI callbacks");
        }
        else return 2;
    }
    if (mode == "key1-state" || mode == "dsi-state")
    {
        // A DSi-typed caller must reach the shared state loader and its DSi
        // section; a stale derived declaration previously failed to link.
        DSiArgs args;
        args.JIT = std::nullopt;
        auto dsi = std::make_unique<DSi>(std::move(args));
        dsi->Reset();
        for (u32 clock : {0u, 1u})
        for (u32 ram : {0u, 2u})
        {
            // Distinct physical RAM words make a stale 4/16 MiB mirror visible
            // through both CPUs' real memory reads after the whole-state load.
            const u32 ext = dsi->SCFG_EXT[0] & ~0xC000u;
            dsi->ARM9Write32(0x04004008, ext | 0x8000);
            dsi->ARM9Write32(0x02000200, 0x11223344);
            dsi->ARM9Write32(0x02400200, 0x55667788);
            dsi->ARM9Write32(0x04004008, ext | (ram << 14));
            dsi->ARM9Write16(0x04004004, 0x0186 | clock);
            // Low timestamp bits must survive too, without clock-write rounding.
            constexpr u64 timestamp = 0x12345, target = 0x23457;
            dsi->ARM9Timestamp = timestamp;
            dsi->ARM9Target = target;
            dsi->NWRAM_A[0x123] = 0x5A;
            Savestate saved;
            if (!dsi->DoSavestate(&saved) || saved.Error) throw std::runtime_error("DSi typed save failed");
            saved.Finish();

            // Reuse a dirty receiver with the opposite clock and RAM size.
            dsi->ARM9Write16(0x04004004, 0x0186 | (clock ^ 1));
            dsi->ARM9Write32(0x04004008, ext | ((ram ^ 2) << 14));
            dsi->ARM9Write32(0x02000200, 0xDEADBEEF);
            dsi->NWRAM_A[0x123] = 0xC3;
            Savestate load(saved.Buffer(), saved.Length(), false);
            if (!dsi->DoSavestate(&load) || load.Error) throw std::runtime_error("DSi typed restore failed");
            Check(dsi->SCFG_Clock9 == (0x0186 | clock) && dsi->ARM9ClockShift == clock + 1,
                  "DSi whole-state load retained the receiver's clock");
            Check(dsi->ARM9Timestamp == timestamp && dsi->ARM9Target == target,
                  "DSi whole-state load rescaled or rounded saved ARM9 timestamps");
            Check(dsi->MainRAMMask == (ram ? 0xFFFFFFu : 0x3FFFFFu),
                  "DSi whole-state load retained the receiver's RAM size");
            const u32 upper = ram ? 0x55667788 : 0x11223344;
            Check(dsi->ARM9Read32(0x02000200) == 0x11223344 && dsi->ARM9Read32(0x02400200) == upper
                && dsi->ARM7Read32(0x02000200) == 0x11223344 && dsi->ARM7Read32(0x02400200) == upper,
                  "DSi whole-state load restored incorrect RAM data or CPU mirrors");
            Check(dsi->NWRAM_A[0x123] == 0x5A, "DSi typed state roundtrip lost NWRAM");
        }
    }
    std::printf("Cart SPI %s: %u failures\n", argv[1], Failures);
    return Failures ? 1 : 0;
}
catch (const std::exception& error)
{
    std::fprintf(stderr, "Fixture error: %s\n", error.what());
    return 2;
}
