// SPDX-License-Identifier: GPL-3.0-or-later
// An ARM9 load's data timing follows its runtime register address, even when
// the native block was traced at a faster DTCM address.
#include "Args.h"
#include "NDS.h"
#include "ARM.h"
#include <cstdio>
#include <memory>
#include <stdexcept>

using namespace melonDS;

namespace {
constexpr u32 Code = 0x00001000;
constexpr u32 BlockCode = 0x00001020;
constexpr u32 DTCM = 0x00800000;
constexpr u32 RAM = 0x02010000;
constexpr u32 ReturnCode = RAM + 0x200;
constexpr u32 ReturnTarget = Code + 0x100;
constexpr u32 DTCMValue = 0x11223344;
constexpr u32 RAMValue = 0x55667788;

void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Result {
    u32 value;
    u32 prefix;
    u32 pc;
    u32 cpsr;
    u32 cycles;
    bool operator==(const Result&) const = default;
};

Result Dispatch(ARMv5& cpu, JitBlockEntry entry, u32 address, u32 code = Code)
{
    cpu.R[0] = address;
    cpu.R[1] = 0;
    cpu.R[2] = 0;
    cpu.CPSR = 0x000000DF;
    cpu.StopExecution = 0;
    cpu.JumpTo(code);
    cpu.Cycles = 0; // Exclude the pipeline refill at entry.
    ARM_Dispatch(&cpu, entry);
    return {cpu.R[1], cpu.R[2], cpu.R[15], cpu.CPSR, u32(cpu.Cycles)};
}

void PrepareTrace(ARMv5& cpu, u32 address, u32 code = Code)
{
    cpu.R[0] = address;
    cpu.R[1] = 0;
    cpu.R[2] = 0;
    cpu.CPSR = 0x000000DF;
    cpu.StopExecution = 0;
    cpu.JumpTo(code);
    cpu.Cycles = 0;
}
}

int main()
{
    try {
        NDSArgs args;
        args.JIT->MaxBlockSize = 2;
        args.JIT->FastMemory = false;
        args.JIT->LiteralOptimizations = false;
        args.JIT->BranchOptimizations = false;
        auto nds = std::make_unique<NDS>(std::move(args));
        NDS::Current = nds.get();
        nds->Reset();
        nds->CurCPU = 0;
        auto& cpu = nds->ARM9;

        // Region 0 covers both addresses. Data cache timing is three cycles;
        // ITCM code fetch and DTCM data access each cost one cycle.
        cpu.CP15Write(0x600, 0x3F);
        cpu.CP15Write(0x502, 3);
        cpu.CP15Write(0x503, 3);
        cpu.CP15Write(0x200, 1);
        cpu.CP15Write(0x911, 0x0C); // 32 KiB ITCM
        cpu.CP15Write(0x910, DTCM | 0x0A); // 16 KiB DTCM
        cpu.CP15Write(0x100, cpu.CP15Read(0x100) | 0x00050005);
        Require(cpu.DataWrite32(Code, 0xE3A02007), "ITCM prefix write failed"); // mov r2,#7
        Require(cpu.DataWrite32(Code + 4, 0xE5901000), "ITCM load write failed"); // ldr r1,[r0]
        Require(cpu.DataWrite32(DTCM, DTCMValue), "DTCM data write failed");
        Require(cpu.DataWrite32(RAM, RAMValue), "RAM data write failed");

        u32 probe = 0;
        Require(cpu.DataRead32(DTCM, &probe) && probe == DTCMValue && cpu.DataCycles == 1,
                "DTCM timing is not one cycle");
        Require(cpu.DataRead32(RAM, &probe) && probe == RAMValue && cpu.DataCycles == 3,
                "RAM timing is not three cycles");

        PrepareTrace(cpu, DTCM);
        nds->JIT.CompileBlock(&cpu);
        Require(cpu.DataCycles == 1, "first trace did not load from DTCM");
        Require(nds->JIT.JitBlocks9.contains(Code), "DTCM block was not compiled");
        const auto cachedEntry = nds->JIT.JitBlocks9.at(Code)->EntryPoint;
        const Result dtcm = Dispatch(cpu, cachedEntry, DTCM);
        Require(dtcm.value == DTCMValue && dtcm.prefix == 7 && dtcm.cycles > 0,
                "DTCM native load failed");

        const Result reused = Dispatch(cpu, cachedEntry, RAM);
        Require(reused.value == RAMValue && reused.prefix == 7,
                "cached native block read wrong RAM value or prefix");
        Require(nds->JIT.JitBlocks9.at(Code)->EntryPoint == cachedEntry,
                "native block was replaced before RAM reuse");

        nds->JIT.ResetBlockCache();
        PrepareTrace(cpu, RAM);
        nds->JIT.CompileBlock(&cpu);
        Require(cpu.DataCycles == 3, "fresh trace did not load from RAM");
        Require(nds->JIT.JitBlocks9.contains(Code), "RAM block was not compiled");
        const Result fresh = Dispatch(cpu, nds->JIT.JitBlocks9.at(Code)->EntryPoint, RAM);
        Require(fresh.value == RAMValue && fresh.prefix == 7 && fresh.cycles > dtcm.cycles,
                "fresh RAM native load lacks the expected timing difference");
        if (!(reused == fresh)) {
            std::fprintf(stderr,
                "FAIL: cached DTCM->RAM vs fresh RAM: value=%08x/%08x prefix=%08x/%08x pc=%08x/%08x "
                "cpsr=%08x/%08x cycles=%u/%u (DTCM=%u)\n",
                reused.value, fresh.value, reused.prefix, fresh.prefix,
                reused.pc, fresh.pc, reused.cpsr, fresh.cpsr,
                reused.cycles, fresh.cycles, dtcm.cycles);
            return 1;
        }

        // Multi-register transfers sum nonsequential and sequential data
        // timings. Their cached native block must follow the same address
        // change as the single load above.
        nds->JIT.SetMaxBlockSize(1);
        Require(cpu.DataWrite32(BlockCode, 0xE8900006), "ITCM block load write failed");
        Require(cpu.DataWrite32(DTCM + 4, DTCMValue + 1), "DTCM second word write failed");
        Require(cpu.DataWrite32(RAM + 4, RAMValue + 1), "RAM second word write failed");
        PrepareTrace(cpu, DTCM, BlockCode);
        nds->JIT.CompileBlock(&cpu);
        Require(nds->JIT.JitBlocks9.contains(BlockCode), "DTCM block transfer was not compiled");
        const auto blockEntry = nds->JIT.JitBlocks9.at(BlockCode)->EntryPoint;
        const Result cachedBlock = Dispatch(cpu, blockEntry, RAM, BlockCode);
        Require(cachedBlock.value == RAMValue && cachedBlock.prefix == RAMValue + 1,
                "cached native block transfer read wrong RAM values");
        nds->JIT.ResetBlockCache();
        PrepareTrace(cpu, RAM, BlockCode);
        nds->JIT.CompileBlock(&cpu);
        Require(nds->JIT.JitBlocks9.contains(BlockCode), "RAM block transfer was not compiled");
        const Result freshBlock = Dispatch(cpu, nds->JIT.JitBlocks9.at(BlockCode)->EntryPoint,
                                           RAM, BlockCode);
        if (!(cachedBlock == freshBlock)) {
            std::fprintf(stderr,
                "FAIL: cached DTCM->RAM block vs fresh RAM: r1=%08x/%08x r2=%08x/%08x "
                "pc=%08x/%08x cpsr=%08x/%08x cycles=%u/%u\n",
                cachedBlock.value, freshBlock.value, cachedBlock.prefix, freshBlock.prefix,
                cachedBlock.pc, freshBlock.pc, cachedBlock.cpsr, freshBlock.cpsr,
                cachedBlock.cycles, freshBlock.cycles);
            return 1;
        }

        // LDM with PC refills the target pipeline before the interpreter
        // charges the load. The native path must use the same final code
        // timing, including when the first execution builds the block.
        Require(cpu.DataWrite32(ReturnCode, 0xE8BD8010), "return load write failed");
        Require(cpu.DataWrite32(ReturnTarget, 0xE1A00000), "return target write failed");
        Require(cpu.DataWrite32(DTCM + 0x100, 0x12345678), "return stack value write failed");
        Require(cpu.DataWrite32(DTCM + 0x104, ReturnTarget), "return stack PC write failed");
        cpu.CPSR = 0x000000DF;
        cpu.R[4] = 0;
        cpu.R[13] = DTCM + 0x100;
        cpu.StopExecution = 0;
        cpu.JumpTo(ReturnCode);
        cpu.Cycles = 0;
        nds->JIT.CompileBlock(&cpu);
        const Result interpretedReturn = {cpu.R[4], cpu.R[13], cpu.R[15], cpu.CPSR,
                                          u32(cpu.Cycles)};
        Require(nds->JIT.JitBlocks9.contains(ReturnCode), "return block was not compiled");
        cpu.CPSR = 0x000000DF;
        cpu.R[4] = 0;
        cpu.R[13] = DTCM + 0x100;
        cpu.StopExecution = 0;
        cpu.JumpTo(ReturnCode);
        cpu.Cycles = 0;
        ARM_Dispatch(&cpu, nds->JIT.JitBlocks9.at(ReturnCode)->EntryPoint);
        const Result nativeReturn = {cpu.R[4], cpu.R[13], cpu.R[15], cpu.CPSR,
                                     u32(cpu.Cycles)};
        if (!(interpretedReturn == nativeReturn)) {
            std::fprintf(stderr,
                "FAIL: LDM PC interpreter vs native: r4=%08x/%08x sp=%08x/%08x "
                "pc=%08x/%08x cpsr=%08x/%08x cycles=%u/%u\n",
                interpretedReturn.value, nativeReturn.value,
                interpretedReturn.prefix, nativeReturn.prefix,
                interpretedReturn.pc, nativeReturn.pc,
                interpretedReturn.cpsr, nativeReturn.cpsr,
                interpretedReturn.cycles, nativeReturn.cycles);
            return 1;
        }

        // The one-register form must use the same post-refill timing path.
        Require(cpu.DataWrite32(ReturnCode, 0xE8BD8000), "single return load write failed");
        Require(cpu.DataWrite32(DTCM + 0x100, ReturnTarget), "single return PC write failed");
        nds->JIT.ResetBlockCache();
        cpu.CPSR = 0x000000DF;
        cpu.R[13] = DTCM + 0x100;
        cpu.StopExecution = 0;
        cpu.JumpTo(ReturnCode);
        cpu.Cycles = 0;
        nds->JIT.CompileBlock(&cpu);
        const Result interpretedSingle = {cpu.R[4], cpu.R[13], cpu.R[15], cpu.CPSR,
                                          u32(cpu.Cycles)};
        Require(nds->JIT.JitBlocks9.contains(ReturnCode), "single return block was not compiled");
        cpu.CPSR = 0x000000DF;
        cpu.R[13] = DTCM + 0x100;
        cpu.StopExecution = 0;
        cpu.JumpTo(ReturnCode);
        cpu.Cycles = 0;
        ARM_Dispatch(&cpu, nds->JIT.JitBlocks9.at(ReturnCode)->EntryPoint);
        const Result nativeSingle = {cpu.R[4], cpu.R[13], cpu.R[15], cpu.CPSR,
                                     u32(cpu.Cycles)};
        Require(interpretedSingle == nativeSingle,
                "single-register LDM PC interpreter/native timing differs");

        // A conditional return with a long register list exercises the
        // executed-path charge separately from the skipped instruction.
        Require(cpu.DataWrite32(ReturnCode, 0x08BD8FF8), "conditional return write failed");
        for (u32 i = 0; i < 10; ++i)
            Require(cpu.DataWrite32(DTCM + 0x200 + i * 4,
                                    i == 9 ? ReturnTarget : 0x11110000 + i),
                    "conditional return stack write failed");
        nds->JIT.ResetBlockCache();
        cpu.CPSR = 0x400000DF; // EQ executes.
        for (int reg = 3; reg <= 11; ++reg) cpu.R[reg] = 0;
        cpu.R[13] = DTCM + 0x200;
        cpu.StopExecution = 0;
        cpu.JumpTo(ReturnCode);
        cpu.Cycles = 0;
        nds->JIT.CompileBlock(&cpu);
        const Result interpretedConditional = {cpu.R[4], cpu.R[13], cpu.R[15], cpu.CPSR,
                                               u32(cpu.Cycles)};
        Require(nds->JIT.JitBlocks9.contains(ReturnCode),
                "conditional return block was not compiled");
        cpu.CPSR = 0x400000DF;
        for (int reg = 3; reg <= 11; ++reg) cpu.R[reg] = 0;
        cpu.R[13] = DTCM + 0x200;
        cpu.StopExecution = 0;
        cpu.JumpTo(ReturnCode);
        cpu.Cycles = 0;
        ARM_Dispatch(&cpu, nds->JIT.JitBlocks9.at(ReturnCode)->EntryPoint);
        const Result nativeConditional = {cpu.R[4], cpu.R[13], cpu.R[15], cpu.CPSR,
                                          u32(cpu.Cycles)};
        if (!(interpretedConditional == nativeConditional)) {
            std::fprintf(stderr, "FAIL: conditional LDM PC cycles=%u/%u pc=%08x/%08x\n",
                         interpretedConditional.cycles, nativeConditional.cycles,
                         interpretedConditional.pc, nativeConditional.pc);
            return 1;
        }
        Require(cpu.DataWrite32(ReturnCode, 0x18BD8FF8),
                "skipped conditional return write failed");
        nds->JIT.ResetBlockCache();
        cpu.CPSR = 0x400000DF; // NE skips.
        cpu.R[13] = DTCM + 0x200;
        cpu.StopExecution = 0;
        cpu.JumpTo(ReturnCode);
        cpu.Cycles = 0;
        nds->JIT.CompileBlock(&cpu);
        const Result interpretedSkipped = {cpu.R[4], cpu.R[13], cpu.R[15], cpu.CPSR,
                                           u32(cpu.Cycles)};
        Require(nds->JIT.JitBlocks9.contains(ReturnCode),
                "skipped return block was not compiled");
        cpu.CPSR = 0x400000DF;
        cpu.R[13] = DTCM + 0x200;
        cpu.StopExecution = 0;
        cpu.JumpTo(ReturnCode);
        cpu.Cycles = 0;
        ARM_Dispatch(&cpu, nds->JIT.JitBlocks9.at(ReturnCode)->EntryPoint);
        const Result nativeSkipped = {cpu.R[4], cpu.R[13], cpu.R[15], cpu.CPSR,
                                      u32(cpu.Cycles)};
        if (!(interpretedSkipped == nativeSkipped)) {
            std::fprintf(stderr, "FAIL: skipped LDM PC cycles=%u/%u pc=%08x/%08x\n",
                         interpretedSkipped.cycles, nativeSkipped.cycles,
                         interpretedSkipped.pc, nativeSkipped.pc);
            return 1;
        }
        std::printf("PASS: cached memory timing and LDM-PC refill timing match\n");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
