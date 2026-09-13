// SPDX-License-Identifier: GPL-3.0-or-later
#include "NDS.h"
#include "Args.h"
#include "GBACart.h"
#include "NDSCart/CartHomebrew.h"
#include "NDSCart/CartR4.h"
#include "NDSCart/CartRetailNAND.h"
#include <array>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <cstring>
#include <vector>
using namespace melonDS;

static void RequireNAND(bool ok, const char* reason)
{
    if (!ok) throw std::runtime_error(reason);
}

struct NANDConsole : NDS
{
    using NDS::NDS;
    bool FailNextLoad = false;

    void DoSavestateExtra(Savestate* state) override
    {
        // Fail after the real RAM/device/cart records have already been loaded.
        if (!state->Saving && FailNextLoad)
        {
            FailNextLoad = false;
            state->Error = true;
        }
    }

    void StepROM()
    {
        RequireNAND(EventScheduled(Event_CartROMTransfer9), "Missing NAND ROM scheduler event");
        const auto deadline = SchedList[Event_CartROMTransfer9].Timestamp;
        RunSystem(deadline);
        ARM9Timestamp = deadline << ARM9ClockShift;
        ARM7Timestamp = deadline;
    }
};

struct NANDImportFixture
{
    static constexpr u32 Base = 0x20000, Length = 8 * 1024 * 1024, Offset = 0x1800;
    std::unique_ptr<NANDConsole> Machine;
    NDSCart::CartRetailNAND* Cart;

    explicit NANDImportFixture(u8 seed = 0x5A)
    {
        NDSArgs args; args.JIT = std::nullopt;
        Machine = std::make_unique<NANDConsole>(std::move(args));
        Machine->Reset();
        auto rom = std::make_unique<u8[]>(Base);
        NDSHeader header{};
        std::memcpy(header.GameCode, "NAN0", 4);
        header.ARM9ROMOffset = 0x4000; header.ARM7ROMOffset = 0x4004;
        header.ARM9Size = header.ARM7Size = 4;
        std::memcpy(rom.get(), &header, sizeof(header));
        rom[0x96] = 1; // Generated ROM declares the NAND save base in 128 KiB units.
        auto save = std::make_unique<u8[]>(Length);
        std::memset(save.get(), seed, Length);
        auto cart = std::make_unique<NDSCart::CartRetailNAND>(std::move(rom), Base, 0,
            ROMListEntry{0, Base, 8}, std::move(save), Length, nullptr);
        Cart = cart.get();
        Machine->SetNDSCart(std::move(cart));
        Machine->NDSCartSlots[0]->SetupDirectBoot("generated-nand.nds");
        Machine->ARM9Write16(0x04000204, 0);
        Machine->ARM9Write16(0x040001A0, 0x8000);
        Machine->ARM9Write32(0x02000200, 0x12340000 | seed);
    }

    void Command(u8 opcode, u32 address = 0, const std::array<u32, 128>* block = nullptr)
    {
        const u8 command[8] = {opcode, u8(address >> 24), u8(address >> 16), u8(address >> 8), u8(address)};
        Machine->CurCPU = 0;
        for (u32 i = 0; i < 8; ++i) Machine->ARM9Write8(0x040001A8 + i, command[i]);
        Machine->ARM9Write32(0x040001A4, (1u << 31) | (1u << 29) |
            (block ? (1u << 30) | (1u << 24) : 0));
        if (block)
            for (u32 word : *block)
            {
                RequireNAND(Machine->ARM9Read32(0x040001A4) & (1u << 23), "NAND write FIFO did not request data");
                Machine->ARM9Write32(0x04100010, word);
                Machine->StepROM();
            }
        Machine->StepROM(); // Zero-data command or final write completion.
        RequireNAND(!(Machine->ARM9Read32(0x040001A4) & (1u << 31)), "NAND ROM command did not complete");
    }
};

static int TestNANDImportState()
{
    unsigned failures = 0;
    const char* scenarios[] = {"normal-commit", "import-live", "import-cold", "normal-cold", "failed-load-rollback"};
    for (unsigned scenario = 0; scenario < std::size(scenarios); ++scenario)
    {
        const bool imported = scenario == 1 || scenario == 2;
        auto active = std::make_unique<NANDImportFixture>();
        auto expected = std::vector<u8>(active->Cart->GetSaveMemory(),
            active->Cart->GetSaveMemory() + NANDImportFixture::Length);
        active->Command(0xB2, NANDImportFixture::Base);
        active->Command(0x85);
        std::array<u32, 128> block; block.fill(0xA5A5A5A5);
        // Four complete 512-byte ROM writes stage one aligned 2048-byte page.
        for (unsigned chunk = 0; chunk < 4; ++chunk)
            active->Command(0x81, NANDImportFixture::Base + NANDImportFixture::Offset, &block);
        RequireNAND(std::equal(expected.begin(), expected.end(), active->Cart->GetSaveMemory()),
                    "NAND page committed before the explicit 0x82 command");
        if (imported)
        {
            // Retain the model's read-only ID tail; replace only writable data.
            std::fill(expected.begin(), expected.end() - 0x20000, 0xC3);
            active->Machine->SetNDSSave(expected.data(), expected.size());
            RequireNAND(std::equal(expected.begin(), expected.end(), active->Cart->GetSaveMemory()),
                        "NAND raw import did not install the generated save");
        }
        if (scenario == 2 || scenario == 3)
        {
            Savestate saved;
            RequireNAND(active->Machine->DoSavestate(&saved) && !saved.Error, "NAND whole-console save failed");
            saved.Finish();
            auto restored = std::make_unique<NANDImportFixture>(0x7E);
            Savestate load(saved.Buffer(), saved.Length(), false);
            RequireNAND(restored->Machine->DoSavestate(&load) && !load.Error, "NAND cold restore failed");
            RequireNAND(restored->Machine->ARM9Read32(0x02000200) == 0x1234005A &&
                        std::equal(expected.begin(), expected.end(), restored->Cart->GetSaveMemory()),
                        "NAND cold restore lost console RAM or imported bytes");
            active = std::move(restored);
        }
        if (scenario == 4)
        {
            Savestate backup;
            RequireNAND(active->Machine->DoSavestate(&backup) && !backup.Error, "NAND rollback backup failed");
            backup.Finish();
            NANDImportFixture other(0x7E);
            Savestate target;
            RequireNAND(other.Machine->DoSavestate(&target) && !target.Error, "NAND rollback target save failed");
            target.Finish();
            active->Machine->FailNextLoad = true;
            Savestate failed(target.Buffer(), target.Length(), false);
            RequireNAND(!active->Machine->DoSavestate(&failed) && failed.Error &&
                        !active->Machine->FailNextLoad &&
                        active->Machine->ARM9Read32(0x02000200) == 0x1234007E &&
                        std::equal(other.Cart->GetSaveMemory(), other.Cart->GetSaveMemory() + NANDImportFixture::Length,
                                   active->Cart->GetSaveMemory()),
                        "NAND injected load failure did not reach the replaced console/cart state");
            // Match the frontend's failed-load recovery with the whole-console backup.
            Savestate recovery(backup.Buffer(), backup.Length(), false);
            RequireNAND(active->Machine->DoSavestate(&recovery) && !recovery.Error &&
                        active->Machine->ARM9Read32(0x02000200) == 0x1234005A &&
                        std::equal(expected.begin(), expected.end(), active->Cart->GetSaveMemory()),
                        "NAND failed-load rollback lost the previous console/save bytes");
        }
        active->Command(0x82); // No new 0x81/payload after import or restore.
        if (!imported) std::fill_n(expected.begin() + NANDImportFixture::Offset, 0x800, 0xA5);
        size_t changed = 0, first = expected.size();
        for (size_t i = 0; i < expected.size(); ++i)
            if (expected[i] != active->Cart->GetSaveMemory()[i])
            { if (!changed) first = i; ++changed; }
        std::printf("NAND import-state %s: mismatch=%zu first=%08X expected=%02X actual=%02X\n",
            scenarios[scenario],
            changed, static_cast<unsigned>(first), changed ? expected[first] : 0,
            changed ? active->Cart->GetSaveMemory()[first] : 0);
        if (changed)
        {
            ++failures;
            std::fprintf(stderr, "%s\n", imported ? "NAND commit replayed a pre-import page over the replacement save" :
                         "NAND pending page was not committed correctly");
        }
    }
    std::printf("CartROMBounds nand-import-state: %u failures\n", failures);
    return failures ? 1 : 0;
}

int main(int argc, char** argv)
{
    if (argc == 2 && !std::strcmp(argv[1], "nand-import-state"))
    {
        try { return TestNANDImportState(); }
        catch (const std::exception& error) { std::fprintf(stderr, "NAND fixture: %s\n", error.what()); return 2; }
    }
    if (argc != 1) return 2;
    std::array<u8, 0x200> data{};
    // Padding 0xAF bytes to 0x100 must not make a missing game-code byte valid.
    if (GBACart::ParseROM(data.data(), 0xAF, nullptr)) return 1;
    for (u32 size : {0U, 1U, 0x80U, 0xACU, 0xAFU, 0x80000001U, 0xFFFFFFFFU})
    {
        if (GBACart::ParseROM(data.data(), size, nullptr)) return 2;
        auto owned = std::make_unique<u8[]>(data.size());
        if (GBACart::ParseROM(std::move(owned), size, nullptr)) return 3;
    }
    if (GBACart::ParseROM(static_cast<const u8*>(nullptr), 0x200, nullptr)) return 4;
    for (u32 size : {0xB0U, 0xC0U, 0x100U, 0x200U})
        if (!GBACart::ParseROM(data.data(), size, nullptr)) return 5;
    puts("GBA original length, overflow, null and minimal accepted input: PASS");

    // A trimmed ROM may omit unused padding, but not bytes declared as executable.
    for (bool arm9 : {false, true})
    for (u32 executableSize : {1U, 2U})
    {
        std::vector<u8> rom(0x1001, 0x5A);
        NDSHeader header{};
        std::memcpy(header.GameCode, "####", 4);
        header.ARM9ROMOffset = header.ARM7ROMOffset = 0x1000;
        header.ARM9Size = header.ARM7Size = 1;
        (arm9 ? header.ARM9Size : header.ARM7Size) = executableSize;
        std::memcpy(rom.data(), &header, sizeof(header));
        auto borrowed = NDSCart::ParseROM(rom.data(), static_cast<u32>(rom.size()));
        auto owned = std::make_unique<u8[]>(rom.size());
        std::memcpy(owned.get(), rom.data(), rom.size());
        auto moved = NDSCart::ParseROM(std::move(owned), static_cast<u32>(rom.size()));
        const bool valid = executableSize == 1;
        if (bool(borrowed) != valid || bool(moved) != valid)
        {
            std::fprintf(stderr, "NDS ARM%d declared size %u accepted=%d/%d expected=%d\n",
                arm9 ? 9 : 7, executableSize, bool(borrowed), bool(moved), valid);
            return 6;
        }
        if (valid && (borrowed->GetROM()[0x1000] != 0x5A || moved->GetROM()[0x1000] != 0x5A))
            return 7;
    }
    puts("NDS trimmed executable bounds in borrowed and owned input: PASS");

    // Public factory callers must get the same override validation as the UI.
    std::vector<u8> retail(0x8000);
    auto* header = reinterpret_cast<NDSHeader*>(retail.data());
    header->ARM9ROMOffset = 0x4000; header->ARM7ROMOffset = 0x4004;
    header->ARM9Size = header->ARM7Size = 4;
    std::memcpy(header->GameCode, "ZZZA", 4);
    for (u32 invalid : {8u, 10u, 18u, UINT32_MAX})
    {
        NDSCart::NDSCartArgs args; args.SPISaveType = invalid;
        if (NDSCart::ParseROM(retail.data(), retail.size(), nullptr, std::move(args))) return 8;
        auto owned = std::make_unique<u8[]>(retail.size());
        std::memcpy(owned.get(), retail.data(), retail.size());
        auto* original = owned.get();
        args.SPISaveType = invalid;
        if (NDSCart::ParseROM(std::move(owned), retail.size(), nullptr, std::move(args)) ||
            owned.get() != original) return 9;
    }
    for (u32 type : {0u, 1u, 7u, 11u, 12u, 13u, 14u, 15u, 16u, 17u})
    {
        NDSCart::NDSCartArgs args; args.SPISaveType = type;
        auto cart = NDSCart::ParseROM(retail.data(), retail.size(), nullptr, std::move(args));
        if (!cart || cart->Type() != NDSCart::CartType::Retail || cart->GetROMParams().SaveMemType != type) return 10;
    }
    for (const char* code : {"####", "ASMA", "UAMA"})
    {
        std::memcpy(header->GameCode, code, 4);
        std::memset(header->GameTitle, 0, sizeof(header->GameTitle));
        if (!std::strcmp(code, "ASMA")) std::memcpy(header->GameTitle + 1, "SD/TF-NDS", 9);
        auto automatic = NDSCart::ParseROM(retail.data(), retail.size());
        // R4 retains CartSD's historical Homebrew state tag. Check the actual
        // factory class rather than assuming the unused UnlicensedR4 tag.
        const bool family = !std::strcmp(code, "####") ? dynamic_cast<NDSCart::CartHomebrew*>(automatic.get()) != nullptr :
            !std::strcmp(code, "ASMA") ? dynamic_cast<NDSCart::CartR4*>(automatic.get()) != nullptr :
            dynamic_cast<NDSCart::CartRetailNAND*>(automatic.get()) != nullptr;
        if (!family) return 11;
        for (u32 type : {0u, 2u, 7u, 11u, 14u, 15u, 16u, 17u})
        {
            NDSCart::NDSCartArgs args; args.SPISaveType = type;
            if (NDSCart::ParseROM(retail.data(), retail.size(), nullptr, std::move(args))) return 12;
        }
    }
    for (const char* code : {"IZZZ", "UZPZ"})
    for (u32 type : {1u, 11u, 14u, 15u, 16u, 17u})
    {
        std::memcpy(header->GameCode, code, 4);
        NDSCart::NDSCartArgs args; args.SPISaveType = type;
        auto cart = NDSCart::ParseROM(retail.data(), retail.size(), nullptr, std::move(args));
        const auto expected = code[0] == 'I' ? NDSCart::CartType::RetailIR : NDSCart::CartType::RetailBT;
        if (!cart || cart->Type() != expected || cart->GetROMParams().SaveMemType != type) return 13;
    }
    puts("DS manual SPI factory: supported standard/IR/BT overrides, invalid values, NAND/homebrew/R4 family guards: PASS");
}
