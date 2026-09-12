// SPDX-License-Identifier: GPL-3.0-or-later
// Generated instructions, production DMA/MMIO and remap consumers. No external images.
#include "Args.h"
#include "ARM.h"
#include "DSi.h"
#include "NDS.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <utility>

using namespace melonDS;

namespace
{
constexpr u32 RAM = 0x02008000, WRAM = 0x03001000;
constexpr u32 Neighbor = 0x20, Data = 0x40;
unsigned checks = 0, failures = 0;
bool jit, fast;

struct ObservedNDS : NDS
{
    using NDS::NDS;
    using NDS::DMAs;
    static DMA& Channel(NDS& nds, unsigned cpu) { return (nds.*&ObservedNDS::DMAs)[cpu * 4]; }
};

void Check(bool ok, const char* what)
{
    ++checks;
    failures += !ok;
    std::printf("CHECK %s result=%s\n", what, ok ? "PASS" : "FAIL");
}

void Write(NDS& nds, unsigned cpu, u32 addr, u32 value, unsigned width = 4)
{
    if (cpu)
    {
        if (width == 2) nds.ARM7Write16(addr, value);
        else nds.ARM7Write32(addr, value);
    }
    else
    {
        if (width == 2) nds.ARM9Write16(addr, value);
        else nds.ARM9Write32(addr, value);
    }
}

u32 Read(NDS& nds, unsigned cpu, u32 addr)
{
    return cpu ? nds.ARM7Read32(addr) : nds.ARM9Read32(addr);
}

void Program(NDS& nds, unsigned cpu, u32 addr, u32 value)
{
    // Same bytes: ARM MOV r2,#value; Thumb MOV r0,#value then B +0x740.
    Write(nds, cpu, addr, 0xE3A02000 | value);
    Write(nds, cpu, addr + 4, 0xEAFFFFFE);
    Write(nds, cpu, addr + 0x746, 0xE7FE, 2);
}

#ifdef JIT_ENABLED
JitBlock* Block(NDS& nds, unsigned cpu, u32 addr, bool thumb)
{
    auto& map = cpu ? nds.JIT.JitBlocks7 : nds.JIT.JitBlocks9;
    auto it = map.find(addr | u32(thumb));
    return it == map.end() ? nullptr : it->second;
}

JitBlockEntry Lookup(NDS& nds, unsigned cpu, u32 addr, bool thumb)
{
    u64* table = nullptr;
    u32 start = 0, size = 0;
    if (!nds.JIT.SetupExecutableRegion(cpu, addr, table, start, size)) return nullptr;
    return nds.JIT.LookUpBlock(cpu, table, addr - start, addr, thumb);
}
#endif

void Run(NDS& nds, unsigned num, bool thumb, u32 addr, u32 expected,
         const char* phase, bool native = false, bool load = false)
{
    auto& cpu = num ? static_cast<ARM&>(nds.ARM7) : static_cast<ARM&>(nds.ARM9);
    bool cached = false;
    u32 physical = 0; // JIT physical-index diagnostics are unavailable in OFF builds.
    long long emitted = 0;
#ifdef JIT_ENABLED
    cached = jit && Lookup(nds, num, addr, thumb);
    const auto* before = nds.JIT.JITCompiler.GetCodePtr();
    physical = nds.JIT.LocaliseCodeAddress(num, addr);
#endif
    nds.CurCPU = num;
    cpu.CPSR = 0xA00000DF;
    cpu.StopExecution = 0;
    cpu.R[0] = cpu.R[2] = 0;
    cpu.R[3] = 0;
    cpu.R[4] = WRAM + Data;
    cpu.JumpTo(addr | u32(thumb));
    cpu.Cycles = 0;
    auto& timestamp = num ? nds.ARM7Timestamp : nds.ARM9Timestamp;
    auto& target = num ? nds.ARM7Target : nds.ARM9Target;
    timestamp = 0;
    target = 128;
#ifdef JIT_ENABLED
    if (jit)
    {
        if (num) nds.ARM7.Execute<CPUExecuteMode::JIT>();
        else nds.ARM9.Execute<CPUExecuteMode::JIT>();
    }
    else
#endif
    {
        if (num) nds.ARM7.Execute<CPUExecuteMode::Interpreter>();
        else nds.ARM9.Execute<CPUExecuteMode::Interpreter>();
    }
#ifdef JIT_ENABLED
    emitted = nds.JIT.JITCompiler.GetCodePtr() - before;
#endif
    const bool values = load ? cpu.R[3] == expected && cpu.R[0] == 0 && cpu.R[2] == 0
        : cpu.R[thumb ? 0 : 2] == expected && cpu.R[thumb ? 2 : 0] == 0;
    const bool ok = values
        && cpu.R[15] == addr + (thumb ? (load ? 4 : 0x748) : 8)
        && cpu.CPSR == (thumb ? (load ? 0xA00000FFu : 0x200000FFu) : 0xA00000DFu)
        && (!jit || !native || emitted == 0);
    ++checks;
    failures += !ok;
    std::printf("RUN %s ARM%d %s addr=%08X physical=%08X opcode=%08X r0=%u r2=%u r3=%u expected=%u pc=%08X cpsr=%08X cycles=%llu cached=%d emitted=%lld native_required=%d result=%s\n",
        phase, num ? 7 : 9, thumb ? "thumb" : "arm", addr, physical, Read(nds, num, addr),
        cpu.R[0], cpu.R[2], cpu.R[3], expected, cpu.R[15], cpu.CPSR,
        static_cast<unsigned long long>(timestamp), cached, static_cast<long long>(emitted), native,
        ok ? "PASS" : "FAIL");
}

void Warm(NDS& nds, unsigned cpu, bool thumb, u32 addr, u32 value)
{
    Run(nds, cpu, thumb, addr, value, "cold");
    Run(nds, cpu, thumb, addr, value, "warm", true);
}

void DMAWrite(NDS& nds, DMA& dma, unsigned writer, u32 dest, u32 value, unsigned width)
{
    constexpr u32 Source = 0x02010000, DMA0 = 0x040000B0;
    Write(nds, writer, Source, value);
    auto& timestamp = writer ? nds.ARM7Timestamp : nds.ARM9Timestamp;
    auto& target = writer ? nds.ARM7Target : nds.ARM9Target;
    nds.CurCPU = writer;
    timestamp = 0;
    target = 128;
    Write(nds, writer, DMA0, Source);
    Write(nds, writer, DMA0 + 4, dest);
    Write(nds, writer, DMA0 + 8, (1u << 31) | (width == 4 ? 1u << 26 : 0) | 1);
    const bool started = dma.IsRunning();
    dma.Run();
    Check(started && !dma.IsRunning() && timestamp > 0, "MMIO-DMA-completed");
}

void TestDMA(NDSArgs&& args)
{
    auto nds = std::make_unique<ObservedNDS>(std::move(args));
    for (unsigned writer : {0u, 1u})
    for (unsigned width : {2u, 4u})
    for (bool mirror : {false, true})
    {
        std::printf("CASE dma writer=%u width=%u mirror=%d\n", writer, width, mirror);
        nds->Reset();
        NDS::Current = nds.get();
        Program(*nds, 0, RAM, 7);
        Program(*nds, 0, RAM + Neighbor, 42);
#ifdef JIT_ENABLED
        JitBlock* neighbors[2][2]{};
#endif
        for (unsigned cpu : {0u, 1u})
        for (bool thumb : {false, true})
        {
            Warm(*nds, cpu, thumb, RAM + Neighbor, 42);
#ifdef JIT_ENABLED
            neighbors[cpu][thumb] = Block(*nds, cpu, RAM + Neighbor, thumb);
#endif
            Warm(*nds, cpu, thumb, RAM, 7);
        }
        const u32 dest = RAM + (mirror ? 0x00400000 : 0);
        DMAWrite(*nds, nds->DMAs[writer * 4], writer, dest, 0xE3A02009, width);
        Check(Read(*nds, 0, RAM) == 0xE3A02009, "DMA-changed-real-opcode");
        for (unsigned cpu : {0u, 1u})
        for (bool thumb : {false, true})
        {
#ifdef JIT_ENABLED
            Check(!jit || !Block(*nds, cpu, RAM, thumb), "DMA-invalidated-CPU-ISA-block");
            Check(!jit || neighbors[cpu][thumb] == Block(*nds, cpu, RAM + Neighbor, thumb), "DMA-kept-neighbor");
#endif
            Warm(*nds, cpu, thumb, RAM, 9);
            if (mirror) Warm(*nds, cpu, thumb, dest, 9);
            Run(*nds, cpu, thumb, RAM + Neighbor, 42, "neighbor-after-DMA", true);
        }
        DMAWrite(*nds, nds->DMAs[writer * 4], writer, dest + Data, 0x12345678, width);
        Check((Read(*nds, writer, dest + Data) & (width == 2 ? 0xFFFFu : ~0u))
              == (width == 2 ? 0x5678u : 0x12345678u), "DMA-normal-data");
        for (unsigned cpu : {0u, 1u})
        for (bool thumb : {false, true})
            Run(*nds, cpu, thumb, RAM, 9, "normal-DMA-kept-code", true);
    }
}

void TestRemap(NDSArgs&& args, int bank, unsigned cpu, bool thumb)
{
    std::unique_ptr<NDS> nds;
    if (bank < 0) nds = std::make_unique<NDS>(std::move(args));
    else
    {
        DSiArgs dsiArgs;
        static_cast<NDSArgs&>(dsiArgs) = std::move(args);
        nds = std::make_unique<DSi>(std::move(dsiArgs));
    }
    nds->Reset();
    NDS::Current = nds.get();
    auto map = [&](unsigned backing, bool same = false) {
        if (bank < 0)
            nds->ARM9Write8(0x04000247, backing ? (cpu ? 2 : 1) : (same ? (cpu ? 1 : 2) : (cpu ? 3 : 0)));
        else
        {
            auto& dsi = static_cast<DSi&>(*nds);
            const u32 reg = 0x04004040 + (bank == 0 ? 0 : bank == 1 ? 4 : 12);
            // Two physical pages compete for slot zero. No overlapping enabled pages.
            dsi.ARM9Write8(reg, backing ? 0 : 0x80 | cpu);
            dsi.ARM9Write8(reg + 1, backing ? 0x80 | cpu : 0);
            if (same)
                dsi.MapNWRAMRange(cpu, bank, bank == 0 ? 0x00200000 : 0x00100000);
        }
    };
    if (bank >= 0)
    {
        auto& dsi = static_cast<DSi&>(*nds);
        // Enable NWRAM through the production SCFG_EXT ARM7 MMIO path.
        dsi.ARM7Write32(0x04004008, dsi.SCFG_EXT[1] | (1u << 25));
        dsi.MapNWRAMRange(cpu, bank, bank == 0 ? 0x00100000 : 0x00080000);
    }
    // Populate both physical backings before warming either of them.
    map(1);
    Program(*nds, cpu, WRAM, 9);
    Program(*nds, cpu, WRAM + Neighbor, 42);
    Write(*nds, cpu, WRAM + Data, 9);
    map(0);
    Program(*nds, cpu, WRAM, 7);
    Program(*nds, cpu, WRAM + Neighbor, 42);
    Write(*nds, cpu, WRAM + Data, 7);
    constexpr u32 Load = RAM + 0x1000;
    Write(*nds, cpu, Load, thumb ? 0xE7FE6823 : 0xE5943000); // LDR r3,[r4]; B .
    if (!thumb) Write(*nds, cpu, Load + 4, 0xEAFFFFFE);
    Run(*nds, cpu, thumb, Load, 7, "load-cold", false, true);
    Run(*nds, cpu, thumb, Load, 7, "load-native", true, true);
    Program(*nds, cpu, RAM, 31);
    Warm(*nds, cpu, thumb, RAM, 31);
#ifdef JIT_ENABLED
    const auto normal = Block(*nds, cpu, RAM, thumb);
#endif
    Warm(*nds, cpu, thumb, WRAM + Neighbor, 42);
#ifdef JIT_ENABLED
    const auto neighbor = Block(*nds, cpu, WRAM + Neighbor, thumb);
#endif
    Warm(*nds, cpu, thumb, WRAM, 7);
#ifdef JIT_ENABLED
    const auto original = Block(*nds, cpu, WRAM, thumb);
    const u32 oldPhysical = nds->JIT.LocaliseCodeAddress(cpu, WRAM);
#endif
    map(0, true);
#ifdef JIT_ENABLED
    Check(oldPhysical == nds->JIT.LocaliseCodeAddress(cpu, WRAM), "same-physical-backing");
#endif
    Run(*nds, cpu, thumb, WRAM, 7, "same-backing-remap", true);
#ifdef JIT_ENABLED
    Check(!jit || Block(*nds, cpu, WRAM, thumb) == original, "same-backing-kept-entry");
#endif
    const u32 alias = WRAM + (bank < 0 ? 0x4000 : bank == 0 ? 0x10000 : 0x8000);
#ifdef JIT_ENABLED
    Check(oldPhysical == nds->JIT.LocaliseCodeAddress(cpu, alias), "physical-alias");
#endif
    Warm(*nds, cpu, thumb, alias, 7);
    Run(*nds, cpu, thumb, WRAM, 7, "alias-return", true);
    // Keep the opposite ISA in the old physical fast slot during retirement.
    Warm(*nds, cpu, !thumb, WRAM, 7);
#ifdef JIT_ENABLED
    const auto otherISA = Block(*nds, cpu, WRAM, !thumb);
    const auto oldSlot = nds->JIT.FastBlockLookupRegions[oldPhysical >> 27][(oldPhysical & 0x7FFFFFF) / 2];
#endif
    map(1);
#ifdef JIT_ENABLED
    const u32 newPhysical = nds->JIT.LocaliseCodeAddress(cpu, WRAM);
#endif
    Check(
#ifdef JIT_ENABLED
        oldPhysical != newPhysical &&
#endif
        Read(*nds, cpu, WRAM) == 0xE3A02009, "changed-physical-backing-opcode");
    Run(*nds, cpu, thumb, WRAM, 9, "changed-backing-reentry");
    Run(*nds, cpu, thumb, WRAM, 9, "changed-backing-warm", true);
#ifdef JIT_ENABLED
    const auto& oldRange = nds->JIT.CodeMemRegions[oldPhysical >> 27][(oldPhysical & 0x7FFFFFF) / 512];
    Check(!jit || oldRange.Blocks.Find(original) == -1, "remap-detached-old-physical-index");
    Check(!jit || (Block(*nds, cpu, WRAM, !thumb) == otherISA
          && nds->JIT.FastBlockLookupRegions[oldPhysical >> 27][(oldPhysical & 0x7FFFFFF) / 2] == oldSlot),
          "remap-kept-opposite-ISA-slot");
    Check(!jit || Block(*nds, cpu, WRAM + Neighbor, thumb) == neighbor, "remap-kept-unentered-neighbor");
    Check(!jit || Block(*nds, cpu, RAM, thumb) == normal, "remap-kept-normal-region");
#endif
    Run(*nds, cpu, thumb, RAM, 31, "normal-region", true);
    Run(*nds, cpu, thumb, Load, 9, "load-new-backing", true, true);
    // Write the retired backing while the same virtual key belongs to the new
    // block. Its physical index must not still own that retired block pointer.
    map(0, true);
    DMAWrite(*nds, ObservedNDS::Channel(*nds, cpu), cpu, WRAM, 0xE3A0200B, 4);
    Check(Read(*nds, cpu, WRAM) == 0xE3A0200B, "DMA-old-backing-opcode");
#ifdef JIT_ENABLED
    const auto newBlock = Block(*nds, cpu, WRAM, thumb);
    Check(!jit || (newBlock && newBlock->StartAddrLocal == newPhysical), "old-backing-write-kept-new-block");
    Check(!jit || (!Block(*nds, cpu, WRAM, !thumb) && !Block(*nds, cpu, alias, thumb)), "old-DMA-invalidated-ISA-alias");
#endif
    map(1);
    Run(*nds, cpu, thumb, WRAM, 9, "new-backing-after-old-DMA", true);
    DMAWrite(*nds, ObservedNDS::Channel(*nds, cpu), cpu, WRAM + Data, 0x12345678, 4);
    Run(*nds, cpu, thumb, WRAM, 9, "normal-WRAM-DMA", true);
    Run(*nds, cpu, thumb, Load, 0x12345678, "load-normal-DMA", true, true);
    DMAWrite(*nds, ObservedNDS::Channel(*nds, cpu), cpu, WRAM, 0xE3A0200D, 4);
    Check(Read(*nds, cpu, WRAM) == 0xE3A0200D, "DMA-new-backing-opcode");
    Warm(*nds, cpu, thumb, WRAM, 13);
    map(0, true);
    Warm(*nds, cpu, thumb, WRAM, 11);
    Run(*nds, cpu, thumb, WRAM + Neighbor, 42, "old-neighbor-return", true);
    Run(*nds, cpu, thumb, Load, 7, "load-old-backing-return", true, true);
}
}

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 3) return 2;
    if (std::strcmp(argv[1], "interpreter") != 0 && std::strcmp(argv[1], "jit") != 0
        && std::strcmp(argv[1], "fastmem") != 0) return 2;
    jit = std::strcmp(argv[1], "interpreter") != 0;
    fast = std::strcmp(argv[1], "fastmem") == 0;
#ifndef JIT_ENABLED
    if (jit) return 77;
#else
    if (fast && !ARMJIT_Memory::IsFastMemSupported()) return 77;
#endif
    NDSArgs args;
#ifdef JIT_ENABLED
    if (!jit) args.JIT = std::nullopt;
    else args.JIT->FastMemory = fast;
    std::printf("MODE %s jit_build=1 BranchOptimizations=1 LiteralOptimizations=1 budget=128\n", argv[1]);
#else
    std::printf("MODE %s jit_build=0 physical_index_diagnostics=unavailable budget=128\n", argv[1]);
#endif
    if (std::strcmp(argv[2], "dma") == 0) TestDMA(std::move(args));
    else
    {
        if (argc != 5) return 2;
        const int bank = std::strcmp(argv[2], "swram") == 0 ? -1 : argv[2][0] - 'a';
        if (bank < -1 || bank > 2) return 2;
        TestRemap(std::move(args), bank, argv[3][0] == '7', std::strcmp(argv[4], "thumb") == 0);
    }
    std::printf("SUMMARY checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
