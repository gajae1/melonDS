// SPDX-License-Identifier: GPL-3.0-or-later
// Real DS MMIO, slot, scheduler, retail EEPROM and savestate. SPI callbacks are
// observed without replacing their implementation. No external game/BIOS/files.
// Register contract: libnds v1.8.0 include/nds/card.h (AUXSPICNT 13/7/6).
#include "Args.h"
#include "NDS.h"
#include "DSi.h"
#include "NDSCart/CartRetail.h"
#include "NDSCart/CartRetailIR.h"
#include "Platform.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace melonDS;
static unsigned Failures = 0;
static void Check(bool ok, const char* reason)
{
    if (!ok) { ++Failures; std::fprintf(stderr, "%s\n", reason); }
}

static bool ObserveSaves = false;
static unsigned SaveCalls = 0;
static u32 SaveOffset = 0, SaveLength = 0;
static std::vector<u8> SaveBytes;
namespace melonDS::Platform
{
void WriteNDSSave(const u8* bytes, u32 total, u32 offset, u32 length, void*)
{
    if (!ObserveSaves) return;
    ++SaveCalls;
    Check(offset <= total && length <= total - offset, "Save callback range exceeds backing storage");
    SaveOffset = offset; SaveLength = length;
    SaveBytes.assign(bytes, bytes + total);
}
void WriteGBASave(const u8*, u32, u32, u32, void*) {}
}

struct ObservedCart : NDSCart::CartRetail
{
    unsigned Selects = 0, Releases = 0;
    std::vector<u8> Delivered;
    size_t DeliveredAtRelease = 0;
    static std::unique_ptr<u8[]> MakeROM(bool key1)
    {
        auto rom = std::make_unique<u8[]>(0x10000);
        if (key1) std::memcpy(rom.get() + 0x0C, "KEYT", 4);
        return rom;
    }
    explicit ObservedCart(bool key1 = false, u32 saveType = 1) : CartRetail(MakeROM(key1), 0x10000, key1 ? 0x1FC2 : 0, false,
                               {0, 0x10000, saveType}, nullptr, 0, nullptr) {}
    void SPISelect() override { ++Selects; CartRetail::SPISelect(); }
    void SPIRelease() override
    { ++Releases; DeliveredAtRelease = Delivered.size(); CartRetail::SPIRelease(); }
    u8 SPITransmitReceive(u8 val) override
    { Delivered.push_back(val); return CartRetail::SPITransmitReceive(val); }
    void ClearCounts() { Selects = Releases = 0; Delivered.clear(); DeliveredAtRelease = 0; }
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
    u64 SPIDeadline(u32 cpu) const
    {
        const auto event = cpu == 0 ? Event_CartSPITransfer9 : Event_CartSPITransfer7;
        if (!(SchedListMask & (1u << event))) throw std::runtime_error("SPI event was not scheduled");
        return SchedList[event].Timestamp;
    }
    void AdvanceTo(u64 timestamp)
    {
        RunSystem(timestamp);
        ARM9Timestamp = timestamp << ARM9ClockShift;
        ARM7Timestamp = timestamp;
    }
    void Finish(u32 cpu)
    {
        AdvanceTo(SPIDeadline(cpu));
        if (NDSCartSlots[0]->ReadSPICnt(cpu) & 0x80) throw std::runtime_error("SPI event did not clear busy");
    }
    bool SaveScheduled() const { return SchedListMask & (1u << Event_CartSave); }
    u64 SaveDeadline() const
    {
        if (!SaveScheduled()) throw std::runtime_error("Internal save event was not scheduled");
        return SchedList[Event_CartSave].Timestamp;
    }
    void FinishSave()
    {
        if (!GetNDSCart() || !GetNDSCart()->GetSaveDelay()) return;
        AdvanceTo(SaveDeadline());
        Check(!GetNDSCart()->GetSaveDelay() && !SaveScheduled(), "Internal save event did not complete");
    }
};

struct Fixture
{
    std::unique_ptr<Console> Machine;
    ObservedCart* Cart;
    u32 CPU;
    explicit Fixture(u32 cpu, bool key1 = false, u32 saveType = 1) : CPU(cpu)
    {
        NDSArgs args;
        args.JIT = std::nullopt;
        Machine = std::make_unique<Console>(std::move(args));
        Machine->Reset();
        auto cart = std::make_unique<ObservedCart>(key1, saveType);
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
        Check(Cart->GetSaveMemory()[5] == 0xFF, "Held tiny EEPROM write committed before CS release");
    }
    void EndWrite()
    {
        Data(0xB2);
        Control(0x8041); // actual bit13 falling edge terminates WRITE
        Check(Cart->GetSaveMemory()[5] == 0xFF && Cart->GetSaveMemory()[6] == 0xFF,
              "Tiny EEPROM committed before internal write completion");
        Machine->FinishSave();
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

struct FlashFixture
{
    static constexpr u32 Length = 512 * 1024;
    std::unique_ptr<NDS> Machine;
    NDSCart::CartRetail* Cart;
    u32 CPU;
    bool Infrared;

    FlashFixture(u32 cpu, bool dsi, bool infrared = false, u8 seed = 0x5A, u32 saveType = 6)
        : CPU(cpu), Infrared(infrared)
    {
        if (dsi)
        {
            DSiArgs args;
            args.JIT = std::nullopt;
            Machine = std::make_unique<DSi>(std::move(args));
        }
        else
        {
            NDSArgs args;
            args.JIT = std::nullopt;
            Machine = std::make_unique<Console>(std::move(args));
        }
        Machine->Reset();
        auto sram = std::make_unique<u8[]>(Length);
        for (u32 i = 0; i < Length; ++i) sram[i] = seed ^ (i * 37 + (i >> 8));
        std::unique_ptr<NDSCart::CartRetail> cart;
        const ROMListEntry params {0, 0x10000, saveType};
        if (infrared)
            cart = std::make_unique<NDSCart::CartRetailIR>(ObservedCart::MakeROM(false),
                0x10000, 0, 1, false, params, std::move(sram), Length, nullptr);
        else
            cart = std::make_unique<NDSCart::CartRetail>(ObservedCart::MakeROM(false),
                0x10000, 0, false, params, std::move(sram), Length, nullptr);
        Cart = cart.get();
        Machine->SetNDSCart(std::move(cart));
        if (dsi)
        {
            // Generated DSi, no external NAND/BIOS. Power Slot-1 through SCFG_MC.
            Machine->ARM7Write16(0x04004010, 0x0004);
            Machine->ARM7Write16(0x04004010, 0x0008);
            // DSi is final: use its public frame scheduler with both CPUs halted,
            // rather than invoking the SPI callback or executing generated BIOS.
            Machine->ARM9.Halt(1);
            Machine->ARM7.Halt(1);
            Machine->Start();
        }
        Machine->ARM9Write16(0x04000204, cpu ? 0x0800 : 0);
        if (cpu) Machine->ARM7Write32(0x040001A4, 1u << 29);
        else Machine->ARM9Write32(0x040001A4, 1u << 29);
        Machine->ARM9Write32(0x02000200, 0x12340000 | seed);
    }

    void Control(u16 value)
    {
        if (CPU) Machine->ARM7Write16(0x040001A0, value);
        else Machine->ARM9Write16(0x040001A0, value);
    }
    u8 Data(u8 value, bool finish = true)
    {
        Machine->CurCPU = CPU; // Match the CPU that a real execution loop is running.
        if (CPU) Machine->ARM7Write8(0x040001A2, value);
        else Machine->ARM9Write8(0x040001A2, value);
        const auto busy = [&] {
            return (CPU ? Machine->ARM7Read16(0x040001A0)
                        : Machine->ARM9Read16(0x040001A0)) & 0x80;
        };
        if (!busy()) throw std::runtime_error("Flash MMIO transfer did not become busy");
        if (!finish) return 0;
        if (Machine->ConsoleType == 1) Machine->RunFrame();
        else static_cast<Console*>(Machine.get())->Finish(CPU);
        if (busy()) throw std::runtime_error("Flash scheduler did not complete MMIO transfer");
        return CPU ? Machine->ARM7Read8(0x040001A2) : Machine->ARM9Read8(0x040001A2);
    }
    void FinishSave()
    {
        if (Machine->ConsoleType == 0) static_cast<Console*>(Machine.get())->FinishSave();
        else
        {
            // At most two emulated seconds, including the 1.5s sector erase.
            for (unsigned frame = 0; Cart->GetSaveDelay() && frame < 120; ++frame) Machine->RunFrame();
            Check(!Cart->GetSaveDelay(), "DSi scheduler did not finish internal save operation");
        }
    }
    void Release(bool complete = true)
    {
        Control(0x8040); // AUXSPICNT bit 13 falling edge, actual CS high.
        if (complete) FinishSave();
    }
    void Begin(u8 command)
    {
        Control(0xA040);
        if (Infrared) Data(0x00);
        Data(command);
    }
    void EnableWrite() { Begin(0x06); Release(); }
    u8 Status()
    { Begin(0x05); const u8 value = Data(0); Release(false); return value; }
    void Expect(const std::vector<u8>& expected, const char* reason)
    {
        Check(Cart->GetSaveMemoryLength() == expected.size(), "Flash backing length changed");
        if (Cart->GetSaveMemoryLength() != expected.size()) return;
        const auto difference = std::mismatch(expected.begin(), expected.end(), Cart->GetSaveMemory());
        if (difference.first != expected.end())
        {
            Check(false, reason);
            std::fprintf(stderr, "Flash mismatch at %06X: expected %02X, actual %02X\n",
                static_cast<unsigned>(difference.first - expected.begin()),
                *difference.first, *difference.second);
        }
    }
    void Save(Savestate& state, u16 minor)
    {
        if (!Machine->DoSavestate(&state) || state.Error)
            throw std::runtime_error("Flash whole-console save failed");
        state.Finish();
        if (state.MajorVersion() != 14 || state.MinorVersion() != minor)
        {
            Check(false, "Flash whole-console state used an unexpected format version");
            std::fprintf(stderr, "Expected 14.%u, wrote %u.%u\n",
                minor, state.MajorVersion(), state.MinorVersion());
        }
    }
    void Restore(Savestate& state)
    {
        Savestate load(state.Buffer(), state.Length(), false);
        if (!Machine->DoSavestate(&load) || load.Error)
            throw std::runtime_error("Flash cold whole-console restore failed");
        Check(Machine->ARM9Read32(0x02000200) == 0x1234005A,
            "Flash state did not restore the console's RAM marker");
    }
};

static void TestFlashWriteState(u32 cpu, bool dsi, u8 command, bool infrared = false)
{
    std::printf("Flash state %s ARM%u command=%02X IR=%u\n", dsi ? "DSi" : "DS",
        cpu ? 7 : 9, command, infrared);
    std::fflush(stdout);
    FlashFixture original(cpu, dsi, infrared);
    const std::vector<u8> before(original.Cart->GetSaveMemory(),
        original.Cart->GetSaveMemory() + FlashFixture::Length);
    { Savestate idle; original.Save(idle, 2); }
    original.EnableWrite();
    original.Begin(command);
    original.Data(0x00); // mid-address: two address bytes have not arrived yet
    original.Expect(before, "Flash changed SRAM during its address phase");
    Savestate address;
    original.Save(address, 2);

    // Matching ROM and chip, but different initial save bytes and no prior SPI.
    FlashFixture addressed(cpu, dsi, infrared, 0xC3);
    addressed.Restore(address);
    addressed.Expect(before, "Cold mid-address restore lost the physical save bytes");
    addressed.Data(0x12);
    addressed.Data(0xFF);
    addressed.Data(0xF0); // first payload byte, still held at the end of page 0x1200
    addressed.Expect(before, "Flash committed its first payload before CS release");
    Savestate payload;
    addressed.Save(payload, 6);

    FlashFixture restored(cpu, dsi, infrared, 0x3C);
    restored.Restore(payload);
    restored.Expect(before, "Cold pending-page restore prematurely committed the latch");
    restored.Data(0x0F);
    restored.Data(0xA5);
    restored.Expect(before, "Restored held Flash write changed SRAM before CS release");
    restored.Release();
    auto expected = before;
    // This transaction crosses the 256-byte page boundary, not the chip end.
    for (const auto& byte : {std::pair<u32, u8>{0x12FF, 0xF0}, {0x1200, 0x0F}, {0x1201, 0xA5}})
        expected[byte.first] = command == 0x02 ? before[byte.first] & byte.second : byte.second;
    restored.Expect(expected, "Flash page continuation or its untouched neighbors were corrupted");
    Savestate committed;
    restored.Save(committed, 2); // an idle cart must not permanently raise the minor version
    original.Restore(committed);
    original.Release();
    original.Expect(expected, "Idle 14.2 restore lost committed bytes or replayed an old latch");
    if (infrared)
    {
        // Pass-through position is the derived-cart state tail after the latch.
        original.Control(0xA040);
        original.Data(0x08);
        Check(original.Data(0) == 0xAA, "Flash latch state displaced the IR command tail");
        original.Release();
    }
}

static void TestProfileState(u32 type, u32 cpu, bool dsi, bool infrared = false)
{
    std::printf("EEPROM/FRAM state type=%u %s ARM%u IR=%u\n", type, dsi ? "DSi" : "DS", cpu ? 7 : 9, infrared);
    std::fflush(stdout);
    FlashFixture original(cpu, dsi, infrared, 0x5A, type);
    const bool fram = type == 14;
    const unsigned width = type == 13 ? 3 : 2;
    const u32 page = type == 11 ? 32 : type == 12 ? 128 : 256;
    const u32 start = fram ? 32767 : 3 * page - 1;
    const u32 next = fram ? 0 : 2 * page;
    std::vector<u8> expected(original.Cart->GetSaveMemory(),
        original.Cart->GetSaveMemory() + FlashFixture::Length);
    original.EnableWrite();
    original.Begin(0x02);
    for (int shift = int(width - 1) * 8; shift >= 0; shift -= 8)
        original.Data(u8(start >> shift));
    original.Data(0xA6);
    if (fram) expected[start] = 0xA6;
    original.Expect(expected, "Profile used the wrong before-CS commit behavior");
    Savestate held;
    original.Save(held, 7);
    // Load into a default EEPROM8K cart, requiring the state to restore the
    // media profile as well as capacity. File padding belongs to the receiver.
    FlashFixture restored(cpu, dsi, infrared, 0x5A, 2);
    restored.Machine->ARM9Write32(0x02000200, 0);
    restored.Cart->GetSaveMemory()[17] ^= 0xFF;
    restored.Restore(held);
    restored.Expect(expected, "Profile restore lost held data or receiver padding");
    restored.Data(0x19); restored.Data(0xC3);
    if (fram) { expected[next] = 0x19; expected[next + 1] = 0xC3; }
    restored.Expect(expected, "Restored profile changed commit timing");
    restored.Release();
    expected[start] = 0xA6; expected[next] = 0x19; expected[next + 1] = 0xC3;
    restored.Expect(expected, "Profile continuation lost page/chip wrap");
    Savestate idle;
    restored.Save(idle, 7); // Idle exact profiles still need media metadata.
    original.Restore(idle);
    original.Release();
    original.Expect(expected, "Idle profile restore replayed an old page latch");
    if (infrared)
    {
        original.Control(0xA040); original.Data(0x08);
        Check(original.Data(0) == 0xAA, "Profile state displaced the derived IR tail");
        original.Release();
    }
}

static void TestStatusProtection(u32 type, u32 cpu, bool dsi, bool infrared)
{
    std::printf("SPI protection type=%u %s ARM%u IR=%u\n", type, dsi ? "DSi" : "DS", cpu ? 7 : 9, infrared);
    FlashFixture original(cpu, dsi, infrared, 0x5A, type);
    auto expected = std::vector<u8>(original.Cart->GetSaveMemory(),
        original.Cart->GetSaveMemory() + FlashFixture::Length);
    const auto status = [](FlashFixture& f) {
        f.Begin(0x05); const u8 value = f.Data(0); f.Release(); return value;
    };
    // Save while WREN is still selected, then finish through real AUXSPI CS.
    original.Begin(0x06);
    Savestate enable; original.Save(enable, 8);
    FlashFixture restored(cpu, dsi, infrared, 0x5A, type);
    restored.Restore(enable); restored.Release();
    Check(status(restored) & 2, "Restored WREN did not enable writes at CS release");
    if (type != 14)
    {
        restored.Begin(0x04); restored.Data(0x06); restored.Release();
        Check(status(restored) & 2, "Overlong WRDI was executed or its payload became a new WREN");
        restored.Begin(0x04); restored.Release();
        restored.Begin(0x06); restored.Data(0x06); restored.Release();
        Check(!(status(restored) & 2), "Overlong WREN enabled writes");
    }
    if (type == 6)
    {
        // M25PE Flash is not an EEPROM status/BP register.
        restored.EnableWrite(); restored.Begin(0x01); restored.Data(0x0C); restored.Release();
        Check((status(restored) & 0x0E) == 2, "EEPROM protection bits leaked into Flash status");
        restored.Expect(expected, "Status control changed Flash memory");
        return;
    }
    restored.EnableWrite(); restored.Begin(0x01); restored.Data(0x04);
    Savestate protect; restored.Save(protect, 8);
    original.Restore(protect); original.Release();
    Check((status(original) & 0x0E) == 4, "Restored WRSR lost BP or WEL completion");
    const u32 boundary = type == 1 ? 0x180 : type == 14 ? 0x6000 : 0x1800;
    const auto start = [&](FlashFixture& f, u32 address) {
        f.EnableWrite(); f.Begin(type == 1 && address >= 256 ? 0x0A : 0x02);
        if (type != 1) f.Data(u8(address >> 8));
        f.Data(u8(address));
    };
    start(original, boundary); original.Data(0xA6); original.Release();
    original.Expect(expected, "Protected WRITE changed memory through actual AUXSPI");
    if (type == 14)
    {
        start(original, boundary - 1); original.Data(0xC3); original.Data(0xA6);
        expected[boundary - 1] = 0xC3;
        Savestate stopped; original.Save(stopped, 7);
        restored.Restore(stopped); restored.Data(0x19); restored.Release();
        restored.Expect(expected, "Restored FRAM resumed a burst blocked by BP");
    }
    else
    {
        start(original, boundary - 1); original.Data(0xC3); original.Release();
        expected[boundary - 1] = 0xC3;
        original.Expect(expected, "Unprotected neighbor stopped accepting writes");
    }
}

static void TestFlashEraseState(u32 cpu, bool dsi)
{
    const u8 command = cpu ? 0xDB : 0xD8;
    std::printf("Flash erase state %s ARM%u command=%02X\n", dsi ? "DSi" : "DS", cpu ? 7 : 9, command);
    std::fflush(stdout);
    FlashFixture original(cpu, dsi);
    const std::vector<u8> before(original.Cart->GetSaveMemory(),
        original.Cart->GetSaveMemory() + FlashFixture::Length);
    // Bounded command-shape checks through MMIO, rather than a chip-size matrix.
    for (unsigned shape = 0; shape < 3; ++shape)
    {
        if (shape) original.EnableWrite();
        original.Begin(command);
        original.Data(0x01);
        original.Data(0x23);
        if (shape != 1) original.Data(0x45); // shape 1: truncated address
        if (shape == 2) original.Data(0); // extra byte cancels an otherwise valid erase
        original.Release();
        original.Expect(before, "Flash erase accepted missing WREN, truncation, or extra data");
    }
    original.EnableWrite();
    original.Begin(command);
    original.Data(0x01);
    original.Data(0x23);
    original.Data(0x45);
    original.Expect(before, "Flash erase changed SRAM before address-ending CS release");
    Savestate pending;
    original.Save(pending, 6);
    FlashFixture restored(cpu, dsi, false, 0xC3);
    restored.Restore(pending);
    restored.Expect(before, "Restoring a held erase committed it prematurely");
    restored.Release();
    auto expected = before;
    const u32 first = command == 0xDB ? 0x12300 : 0x10000;
    const u32 end = command == 0xDB ? 0x12400 : 0x20000;
    std::fill(expected.begin() + first, expected.begin() + end, 0xFF);
    restored.Expect(expected, "Restored erase missed alignment or changed neighboring data");
    Savestate idle;
    restored.Save(idle, 2);
}

static void TestByteCompletion(u32 cpu)
{
    std::printf("Byte completion ARM%u\n", cpu ? 7 : 9);
    {
        Fixture f(cpu, false, 11);
        f.Control(0xA000);
        f.Data(0x06, false);
        const auto deadline = f.Machine->SPIDeadline(cpu);
        Check((f.Count() & 0x80) && f.Cart->Selects == 1 && f.Cart->Delivered.empty() && f.Cart->Releases == 0,
              "Starting an automatic-CS byte delivered it or released CS before the event");
        f.Machine->AdvanceTo(deadline - 1);
        Check(f.Cart->Delivered.empty() && f.Cart->Releases == 0,
              "SPI byte reached the cart before its scheduled completion");
        f.Machine->Finish(cpu);
        Check(f.Cart->Delivered == std::vector<u8>{0x06} && f.Cart->Releases == 1 &&
              f.Cart->DeliveredAtRelease == 1, "Completion did not deliver once before automatic CS release");
        f.Machine->AdvanceTo(deadline + 1);
        Check(f.Cart->Delivered.size() == 1 && f.Cart->Releases == 1,
              "Completed SPI byte was delivered or released twice");
        f.Control(0xA040); f.Data(0x05);
        f.Control(0xA000); f.Data(0);
        const u8 status = cpu ? f.Machine->ARM7Read8(0x040001A2) : f.Machine->ARM9Read8(0x040001A2);
        Check(status & 2, "Scheduled WREN/release did not reach the real EEPROM status register");
    }

    for (bool eligible : {false, true})
    {
        Fixture f(cpu);
        const u32 startOwner = eligible ? cpu : cpu ^ 1;
        f.Machine->ARM9Write16(0x04000204, startOwner ? 0x0800 : 0);
        f.Control(0xA000); f.Data(0x06, false);
        Check(f.Cart->Delivered.empty() && f.Cart->Selects == (eligible ? 1u : 0u),
              "SPI start did not capture initial cart ownership without delivering data");
        Savestate pending;
        if (!f.Machine->DoSavestate(&pending) || pending.Error)
            throw std::runtime_error("Controller in-flight SPI save failed");
        pending.Finish();
        Check(pending.MajorVersion() == 14 && pending.MinorVersion() == 9,
              "Accepted controller byte, including non-owner, did not require 14.9");
        f.Machine->ARM9Write16(0x04000204, startOwner ? 0 : 0x0800);
        f.Machine->Finish(cpu);
        Check(f.Cart->Delivered == (eligible ? std::vector<u8>{0x06} : std::vector<u8>{}) &&
              f.Cart->Releases == (eligible ? 1u : 0u),
              "Completion re-evaluated ownership instead of using byte-start eligibility");
    }

    const auto beginWrite = [](Fixture& f)
    {
        f.Control(0xA000); f.Data(0x06);
        f.Cart->ClearCounts();
        f.Control(0xA040);
        f.Data(0x02); f.Data(0x00); f.Data(0x05); f.Data(0xA1);
    };
    for (bool hold : {false, true})
    {
        Fixture original(cpu, false, 11);
        beginWrite(original);
        Check(original.Cart->Delivered == std::vector<u8>({0x02, 0, 5, 0xA1}) &&
              original.Cart->Releases == 0, "Completed held command bytes lost their CS context");
        original.Control(hold ? 0xA040 : 0xA000);
        original.Data(0xB2, false);
        const auto deadline = original.Machine->SPIDeadline(cpu);
        original.Machine->AdvanceTo(deadline - 1);
        Check(original.Cart->Delivered.size() == 4 && original.Cart->Releases == 0 &&
              original.Cart->GetSaveMemory()[5] == 0xFF && original.Cart->GetSaveMemory()[6] == 0xFF,
              "In-flight payload reached the held EEPROM page before completion");
        original.Machine->ARM9Write32(0x02000200, 0x12345678);
        Savestate saved;
        if (!original.Machine->DoSavestate(&saved) || saved.Error)
            throw std::runtime_error("In-flight SPI save failed");
        saved.Finish();
        Check(saved.MajorVersion() == 14 && saved.MinorVersion() == 9,
              "Undelivered SPI byte did not require savestate 14.9");

        Fixture restored(cpu, false, 11);
        Savestate load(saved.Buffer(), saved.Length(), false);
        if (!restored.Machine->DoSavestate(&load) || load.Error)
            throw std::runtime_error("Cold in-flight SPI restore failed");
        Check(restored.Machine->ARM9Read32(0x02000200) == 0x12345678 &&
              restored.Machine->SPIDeadline(cpu) == deadline,
              "Cold restore lost console RAM or the original SPI deadline");
        Check(restored.Cart->Delivered.empty() && restored.Cart->Releases == 0 &&
              restored.Cart->GetSaveMemory()[5] == 0xFF && restored.Cart->GetSaveMemory()[6] == 0xFF,
              "Loading an in-flight byte delivered it or committed the held page");
        restored.Machine->AdvanceTo(deadline - 1);
        Check(restored.Cart->Delivered.empty(), "Restored byte was delivered before its deadline");
        restored.Machine->Finish(cpu);
        Check(restored.Cart->Delivered == std::vector<u8>{0xB2} &&
              restored.Cart->Releases == (hold ? 0u : 1u),
              "Restored byte was lost/replayed or its captured hold setting changed");
        if (hold)
        {
            Check(restored.Cart->GetSaveMemory()[5] == 0xFF && restored.Cart->GetSaveMemory()[6] == 0xFF,
                  "Completed held payload committed before CS release");
            restored.Control(0x8040);
        }
        Check(restored.Cart->GetSaveMemory()[5] == 0xFF && restored.Cart->GetSaveMemory()[6] == 0xFF,
              "SPI completion bypassed the internal EEPROM write cycle");
        restored.Machine->AdvanceTo(deadline + 1);
        Check(restored.Cart->Delivered.size() == 1 && restored.Cart->Releases == 1,
              "Restored SPI completion was repeated");
        restored.Machine->FinishSave();
        Check(restored.Cart->GetSaveMemory()[4] == 0xFF && restored.Cart->GetSaveMemory()[5] == 0xA1 &&
              restored.Cart->GetSaveMemory()[6] == 0xB2 && restored.Cart->GetSaveMemory()[7] == 0xFF &&
              restored.Cart->DeliveredAtRelease == 1,
              "Restored completion/release lost previous page bytes or changed neighbors");
    }

    {
        Fixture f(cpu, false, 11);
        beginWrite(f);
        f.Data(0xB2, false);
        const auto deadline = f.Machine->SPIDeadline(cpu);
        f.High(0x80); // Real mode-bit falling edge aborts only the unfinished byte.
        Check((f.Count() & 0x80) && f.Machine->SPIDeadline(cpu) == deadline,
              "Mode clear changed the pending controller completion");
        f.High(0xA0); // Reselect before the deadline must not revive canceled data.
        f.Machine->Finish(cpu);
        Check(f.Cart->Delivered == std::vector<u8>({0x02, 0, 5, 0xA1}) && f.Cart->Releases == 1,
              "Mode clear delivered the aborted byte or duplicated CS release");
        f.Machine->FinishSave();
        Check(f.Cart->GetSaveMemory()[5] == 0xA1 && f.Cart->GetSaveMemory()[6] == 0xFF,
              "Aborting a byte lost completed page data or committed unfinished data");
    }
    for (bool replace : {false, true})
    {
        Fixture f(cpu);
        f.Control(0xA000); f.Data(0x06, false);
        const auto deadline = f.Machine->SPIDeadline(cpu);
        Check(f.Cart->Delivered.empty(), "Byte was delivered before eject/replacement could cancel it");
        auto old = f.Machine->EjectCart(); // Keep the old observer alive after ejection.
        ObservedCart* replacement = nullptr;
        if (replace)
        {
            auto next = std::make_unique<ObservedCart>();
            replacement = next.get();
            f.Machine->SetNDSCart(std::move(next));
        }
        Check((f.Count() & 0x80) && f.Machine->SPIDeadline(cpu) == deadline,
              "Eject/replacement discarded the pending controller busy event");
        f.Machine->Finish(cpu);
        Check(f.Cart->Delivered.empty() && (!replacement || replacement->Delivered.empty()),
              "An old pending byte reached the ejected or replacement cart");
        if (replacement)
        {
            f.Control(0xA000); f.Data(0x06);
            Check(replacement->Delivered == std::vector<u8>{0x06} && replacement->Releases == 1,
                  "Cancelling an old byte prevented a fresh replacement-cart transfer");
        }
    }

    // Existing generated DSi fixture drives the real SCFG power path and frame
    // scheduler. Power off/on before completion must not revive the old byte.
    FlashFixture powered(cpu, true);
    auto observed = std::make_unique<ObservedCart>();
    auto* cart = observed.get();
    powered.Machine->SetNDSCart(std::move(observed));
    powered.Cart = cart;
    // DSi is final, so complete through its real frame scheduler. The DS
    // cases above additionally inspect the cycle immediately before deadline.
    powered.Control(0xA000);
    if (cpu) powered.Machine->ARM7Write8(0x040001A2, 0x06);
    else powered.Machine->ARM9Write8(0x040001A2, 0x06);
    Check(cart->Selects == 1 && cart->Delivered.empty() && cart->Releases == 0 &&
          (powered.Machine->NDSCartSlots[0]->ReadSPICnt(cpu) & 0x80),
          "DSi MMIO start did not select without delivering/releasing the unfinished byte");
    powered.Machine->RunFrame();
    Check(cart->Delivered == std::vector<u8>{0x06} && cart->Releases == 1 &&
          cart->DeliveredAtRelease == 1 && !(powered.Machine->NDSCartSlots[0]->ReadSPICnt(cpu) & 0x80),
          "DSi scheduler did not deliver once before automatic CS release and busy clear");
    cart->ClearCounts();
    powered.Control(0xA000);
    if (cpu) powered.Machine->ARM7Write8(0x040001A2, 0x06);
    else powered.Machine->ARM9Write8(0x040001A2, 0x06);
    Check(cart->Delivered.empty(), "DSi SPI delivered a byte before power-off cancellation");
    powered.Machine->ARM7Write16(0x04004010, 0x0000);
    Check((powered.Machine->ARM7Read16(0x04004010) & 0x000C) == 0,
          "Generated DSi fixture did not power Slot-1 off");
    powered.Machine->ARM7Write16(0x04004010, 0x0004);
    powered.Machine->ARM7Write16(0x04004010, 0x0008);
    powered.Machine->RunFrame();
    Check(cart->Delivered.empty(), "Power cycling revived a pending byte at its old completion event");
    cart->ClearCounts();
    powered.Control(0xA000); powered.Data(0x06);
    Check(cart->Delivered == std::vector<u8>{0x06} && cart->Releases == 1,
          "Power cycling prevented a fresh scheduled SPI transfer");
}

static void BeginTimedWrite(FlashFixture& f, u32 type, u8 command, u32 address,
                            const std::vector<u8>& data)
{
    f.EnableWrite(); f.Begin(command);
    const unsigned width = type == 1 ? 1 : type == 13 || (type >= 5 && type <= 7) ? 3 : 2;
    for (int shift = int(width - 1) * 8; shift >= 0; shift -= 8) f.Data(u8(address >> shift));
    for (u8 byte : data) f.Data(byte);
}

static void TestWriteTiming(u32 cpu)
{
    // Durations are the chosen chip contract, independently of GetSaveDelay().
    struct Operation { u32 type; u8 command; u32 address, microseconds; std::vector<u8> data; };
    for (const auto& op : {Operation{1, 0x02, 5, 5000, {0x96, 0x3C}},
                          Operation{11, 0x02, 0x27, 5000, {0x96, 0x3C}},
                          Operation{6, 0x02, 0x127, 50, std::vector<u8>(9, 0x0F)},
                          Operation{6, 0x0A, 0x127, 11000, {0x96, 0x3C}},
                          Operation{6, 0xDB, 0x12345, 10000, {}},
                          Operation{6, 0xD8, 0x12345, 1500000, {}}})
    {
        std::printf("WIP ARM%u type=%u command=%02X\n", cpu ? 7 : 9, op.type, op.command);
        FlashFixture f(cpu, false, false, 0x5A, op.type);
        auto& machine = *static_cast<Console*>(f.Machine.get());
        const std::vector<u8> before(f.Cart->GetSaveMemory(), f.Cart->GetSaveMemory() + FlashFixture::Length);
        SaveCalls = 0;
        BeginTimedWrite(f, op.type, op.command, op.address, op.data);
        f.Expect(before, "Timed write changed SRAM before CS release");
        Check(!f.Cart->GetSaveDelay() && !machine.SaveScheduled() && SaveCalls == 0,
              "Internal operation started before the command-ending CS edge");
        const u64 edge = cpu ? machine.ARM7Timestamp : machine.ARM9Timestamp >> machine.ARM9ClockShift;
        // Deliberately make CurCPU and the other CPU clock unsuitable as the
        // source of a manually driven MMIO edge's absolute timestamp.
        machine.CurCPU = cpu ^ 1;
        if (cpu) machine.ARM9Timestamp += 1000 << machine.ARM9ClockShift;
        else machine.ARM7Timestamp += 1000;
        f.Release(false);
        const u64 cycles = (u64(op.microseconds) * 33513982 + 999999) / 1000000;
        const u64 deadline = machine.SaveDeadline();
        Check(deadline == edge + cycles && f.Cart->GetSaveDelay() == cycles,
              "Save deadline used the wrong CPU/time unit or chip duration");
        f.Expect(before, "CS release committed the internal write early");
        Check(SaveCalls == 0 && (f.Status() & 3) == 3,
              "Busy write published a callback or lost WIP/WEL");
        Check(machine.SaveDeadline() == deadline, "Status polling restarted the write timer");
        machine.AdvanceTo(deadline - 1);
        f.Expect(before, "Internal write committed before its deadline");
        Check(SaveCalls == 0 && f.Cart->GetSaveDelay(), "Callback or idle state preceded write completion");
        machine.AdvanceTo(deadline); // No SPI read is needed to complete the array.
        auto expected = before;
        const u32 offset = op.command == 0xDB ? 0x12300 : op.command == 0xD8 ? 0x10000 : op.address;
        const u32 length = op.command == 0xDB ? 256 : op.command == 0xD8 ? 65536 : op.data.size();
        if (op.command == 0xDB || op.command == 0xD8)
            std::fill(expected.begin() + offset, expected.begin() + offset + length, 0xFF);
        else for (u32 i = 0; i < length; ++i)
            expected[offset + i] = op.type == 6 && op.command == 0x02 ? before[offset + i] & op.data[i] : op.data[i];
        f.Expect(expected, "Scheduled completion lost data or changed neighbors/padding");
        Check(!f.Cart->GetSaveDelay() && !machine.SaveScheduled() && SaveCalls == 1 &&
              SaveOffset == offset && SaveLength == length && SaveBytes == expected,
              "Completion did not publish exactly one correct array snapshot/range");
        machine.AdvanceTo(deadline + 1);
        Check((f.Status() & 3) == 0 && SaveCalls == 1, "WIP/WEL remained set or completion callback repeated");
    }
}

static void TestWriteTimingState(u32 cpu)
{
    FlashFixture original(cpu, false, false, 0x5A, 11);
    auto& machine = *static_cast<Console*>(original.Machine.get());
    const auto initial = std::vector<u8>(original.Cart->GetSaveMemory(), original.Cart->GetSaveMemory() + FlashFixture::Length);
    auto expected = initial;
    Savestate oldIdle; original.Save(oldIdle, 7); // Existing implicit 25-event layout.
    original.EnableWrite(); original.Begin(0x02); original.Data(0); original.Data(0x27);
    original.Control(0xA000); original.Data(0x96, false);
    const u64 byteEnd = machine.SPIDeadline(cpu);
    machine.CurCPU = cpu ^ 1;
    machine.AdvanceTo(byteEnd + 19); // Intentionally late callback must use its scheduled edge.
    Check(machine.SaveDeadline() == byteEnd + 167570, "Automatic CS deadline was anchored to late callback time");
    const auto deadline = machine.SaveDeadline();
    original.Expect(expected, "Automatic CS committed before the internal deadline");
    Savestate pending; original.Save(pending, 10);
    // Build a contradictory whole-console image through the real scheduler
    // API, without duplicating offsets in the serialized NDSG record.
    machine.CancelEvent(Event_CartSave);
    Savestate missingEvent; original.Save(missingEvent, 10);
    {
        FlashFixture rejected(cpu, false, false, 0x5A, 11);
        Savestate invalid(missingEvent.Buffer(), missingEvent.Length(), false);
        Check(!rejected.Machine->DoSavestate(&invalid) && invalid.Error,
              "State accepted an internal chip write without its scheduler event");
        // This checks rejection, not atomic rollback of every console device.
    }
    FlashFixture restored(cpu, false, false, 0x5A, 11);
    restored.Restore(pending);
    auto& receiver = *static_cast<Console*>(restored.Machine.get());
    Check(receiver.SaveDeadline() == deadline && restored.Cart->GetSaveDelay() == 167570,
          "Cold restore restarted or lost the internal write timer");
    SaveCalls = 0; // State load may publish its restored, still-unmodified array.
    restored.Expect(expected, "WIP state restore committed pending bytes");

    // READ rejected while busy must stay rejected past the deadline, even if
    // its remaining bytes resemble an address or a fresh WREN command.
    restored.Begin(0x03);
    receiver.AdvanceTo(deadline - 1);
    Check(SaveCalls == 0, "Restored internal write callback ran early");
    receiver.AdvanceTo(deadline);
    expected[0x27] = 0x96;
    restored.Data(0); restored.Data(0x27);
    Check(restored.Data(0) == 0xFF && restored.Data(0x06) == 0xFF,
          "Busy-rejected READ resumed within the same CS after completion");
    restored.Release(false);
    restored.Expect(expected, "Busy-rejected command damaged the independent pending latch");
    Check(SaveCalls == 1 && (restored.Status() & 3) == 0,
          "Busy-rejected opcode revived after completion or callback repeated");
    restored.Begin(0x03); restored.Data(0); restored.Data(0x27);
    Check(restored.Data(0) == 0x96, "A fresh READ after CS release did not see completed data");
    restored.Release(false);

    // Poll data ends exactly when the next array operation completes. The
    // lower-numbered SPI event must not observe stale WIP from event ordering.
    BeginTimedWrite(restored, 11, 0x02, 0x28, {0x3C}); restored.Release(false);
    const auto simultaneous = receiver.SaveDeadline();
    restored.Begin(0x05);
    receiver.AdvanceTo(simultaneous - 64);
    restored.Control(0xA000); restored.Data(0, false);
    Check(receiver.SPIDeadline(cpu) == simultaneous, "Simultaneous SPI/save fixture missed its deadline");
    receiver.Finish(cpu);
    const u8 status = cpu ? receiver.ARM7Read8(0x040001A2) : receiver.ARM9Read8(0x040001A2);
    Check(!(status & 3) && restored.Cart->GetSaveMemory()[0x28] == 0x3C && SaveCalls == 2,
          "Same-timestamp RDSR observed old WIP or duplicated completion");

    restored.EnableWrite(); restored.Begin(0x01); restored.Data(0x04); restored.Release(false);
    const auto statusEnd = receiver.SaveDeadline();
    Check((restored.Status() & 0x0F) == 3, "EEPROM WRSR changed BP before its internal deadline");
    receiver.AdvanceTo(statusEnd);
    Check((restored.Status() & 0x0F) == 4 && SaveCalls == 2,
          "WRSR completion lost BP/WEL/WIP semantics or published an array callback");

    BeginTimedWrite(restored, 11, 0x02, 0x29, {0xA6}); restored.Release(false);
    const auto cancelled = receiver.SaveDeadline();
    restored.Restore(oldIdle);
    const auto afterRestore = SaveCalls;
    Check(!restored.Cart->GetSaveDelay() && !receiver.SaveScheduled(),
          "Old implicit-format idle state retained the receiver's new save event");
    receiver.AdvanceTo(cancelled);
    restored.Expect(initial, "Old idle state was overwritten by the discarded internal write");
    Check(SaveCalls == afterRestore, "Old-state load allowed a stale internal save callback");
}

static void TestWriteTimingLifecycle(u32 cpu)
{
    for (unsigned action = 0; action < 3; ++action)
    {
        FlashFixture f(cpu, false, false, 0x5A, 11);
        auto& machine = *static_cast<Console*>(f.Machine.get());
        auto expected = std::vector<u8>(f.Cart->GetSaveMemory(), f.Cart->GetSaveMemory() + FlashFixture::Length);
        BeginTimedWrite(f, 11, 0x02, 0x27, {0x96}); f.Release(false);
        const auto deadline = machine.SaveDeadline();
        std::unique_ptr<NDSCart::CartCommon> ejected;
        if (action == 0) machine.Reset();
        else if (action == 1) ejected = machine.EjectCart();
        else
        {
            const u8 prefix[] = {0xA6, 0x19};
            machine.SetNDSSave(prefix, sizeof(prefix));
            std::copy(std::begin(prefix), std::end(prefix), expected.begin());
        }
        SaveCalls = 0; // The explicit import has its own legitimate callback.
        Check(!f.Cart->GetSaveDelay() && !machine.SaveScheduled(), "Reset/eject/import left an internal save scheduled");
        machine.AdvanceTo(deadline);
        f.Expect(expected, "Cancelled internal write later committed across lifecycle boundary");
        Check(SaveCalls == 0, "Cancelled internal write published a future save callback");
    }
    for (bool powerOff : {false, true})
    {
        FlashFixture f(cpu, true);
        const auto before = std::vector<u8>(f.Cart->GetSaveMemory(), f.Cart->GetSaveMemory() + FlashFixture::Length);
        BeginTimedWrite(f, 6, 0x0A, 0x27, {0x96}); f.Release(false);
        f.Expect(before, "DSi write committed before internal completion");
        Check(f.Cart->GetSaveDelay() != 0, "DSi did not start its internal write timer");
        std::unique_ptr<FlashFixture> restored;
        if (!powerOff)
        {
            Savestate pending; f.Save(pending, 10);
            restored = std::make_unique<FlashFixture>(cpu, true, false, 0xC3);
            restored->Restore(pending);
            Check(restored->Machine->SchedList[Event_CartSave].Timestamp == f.Machine->SchedList[Event_CartSave].Timestamp &&
                  restored->Cart->GetSaveDelay() == f.Cart->GetSaveDelay(),
                  "DSi cold restore restarted or lost its internal save deadline");
        }
        FlashFixture& active = restored ? *restored : f;
        SaveCalls = 0;
        active.Machine->ARM7Write16(0x04004010, powerOff ? 0 : 4);
        Check((active.Machine->ARM7Read16(0x04004010) & 0xC) == (powerOff ? 0 : 4),
              "DSi lifecycle fixture missed the requested slot power state");
        if (!powerOff)
        {
            Check(active.Cart->GetSaveDelay() != 0, "Clock-off power state 1 cancelled a powered internal write");
            active.Machine->ARM7Write8(0x04000301, 0xC0); // Real HALTCNT sleep request.
            Check(active.Machine->CPUStop & CPUStop_Sleep, "HALTCNT did not enter emulated sleep");
        }
        active.Machine->RunFrame(); // No SPI polling; powered writes progress during sleep.
        auto expected = before;
        if (!powerOff) expected[0x27] = 0x96;
        active.Expect(expected, "DSi power/sleep policy lost a completed write or revived a cancelled one");
        Check(!active.Cart->GetSaveDelay() && SaveCalls == (powerOff ? 0u : 1u),
              "DSi internal timer did not progress/cancel independently of SPI clocks");
    }
}

int main(int argc, char** argv)
try
{
    if (argc != 2) return 2;
    const std::string mode = argv[1];
    if (mode == "write-timing")
    {
        ObserveSaves = true;
        for (u32 cpu : {0u, 1u})
        {
            TestWriteTiming(cpu);
            TestWriteTimingState(cpu);
            TestWriteTimingLifecycle(cpu);
        }
        std::printf("Cart SPI write-timing: %u failures\n", Failures);
        return Failures ? 1 : 0;
    }
    if (mode == "byte-completion")
    {
        for (u32 cpu : {0u, 1u}) TestByteCompletion(cpu);
        std::printf("Cart SPI byte-completion: %u failures\n", Failures);
        return Failures ? 1 : 0;
    }
    if (mode == "status-protection")
    {
        TestStatusProtection(1, 0, false, false);
        TestStatusProtection(11, 1, false, true);
        TestStatusProtection(14, 0, false, false);
        TestStatusProtection(11, 1, true, false);
        TestStatusProtection(14, 0, true, false);
        TestStatusProtection(6, 0, false, false);
        std::printf("Cart SPI status-protection: %u failures\n", Failures);
        return Failures ? 1 : 0;
    }
    if (mode == "profile-state")
    {
        TestProfileState(11, 0, false);
        TestProfileState(12, 1, false, true);
        TestProfileState(13, 1, true);
        TestProfileState(14, 1, false);
        TestProfileState(14, 0, true);
        std::printf("Cart SPI profile-state: %u failures\n", Failures);
        return Failures ? 1 : 0;
    }
    if (mode == "flash-state")
    {
        for (bool dsi : {false, true})
        for (u32 cpu : {0u, 1u})
        {
            for (u8 command : {0x02, 0x0A}) TestFlashWriteState(cpu, dsi, command);
            TestFlashEraseState(cpu, dsi);
        }
        TestFlashWriteState(1, false, 0x0A, true);
        std::printf("Cart SPI flash-state: %u failures\n", Failures);
        return Failures ? 1 : 0;
    }
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
