// SPDX-License-Identifier: GPL-3.0-or-later
// CPU-specific empty-list behavior: see plans/releases/1.1.16.md for sources
// and the distinction between a compatibility model and physical DS timing.
#include "Args.h"
#include "ARM.h"
#include "ARMInterpreter.h"
#include "NDS.h"
#include <cstdio>
#include <memory>

using namespace melonDS;

namespace
{
constexpr u32 Code = 0x02020000, Idle = 0x02000200, Base = 0x02004080;
constexpr u32 Sentinel = 0xDEADBEEF, Value = 0x12345678;

bool HasBlock(NDS& nds, bool arm7, u32 addr, bool jit)
{
#ifdef JIT_ENABLED
    if (jit) return (arm7 ? nds.JIT.JitBlocks7 : nds.JIT.JitBlocks9).contains(addr);
#endif
    return true;
}

void ClearData(NDS& nds)
{
    for (u32 addr = Base - 64; addr <= Base + 64; addr += 4)
        nds.ARM9Write32(addr, Sentinel);
}

// Real ARM9 memory/cycle methods, with counters around the data bus only.
// This proves no transfer, rather than merely reading an unchanged RAM value.
struct ObservedARM9 : ARMv5
{
    explicit ObservedARM9(melonDS::NDS& nds) : ARMv5(nds, std::nullopt, false) {}
    unsigned reads = 0, writes = 0;
    u32 BusRead32(u32 addr) override { ++reads; return ARMv5::BusRead32(addr); }
    void BusWrite32(u32 addr, u32 value) override { ++writes; ARMv5::BusWrite32(addr, value); }
};
}

int TestBlockTransferExecution(NDSArgs&& args, bool jit)
{
    auto nds = std::make_unique<NDS>(std::move(args));
    unsigned failures = 0, checks = 0, program = 0;
    for (bool arm7 : {false, true})
    {
        nds->Reset();
        nds->ARM9Write32(Idle, 0xEAFFFFFE);
        nds->ARM9.JumpTo(Idle);
        nds->ARM7.JumpTo(Idle);
        nds->Start();
        ARM& cpu = arm7 ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
        // IA/IB/DA/DB addresses are distinct from the 16-register writeback
        // distance. Nonempty {R1} controls must still transfer one word / 4B.
        for (bool empty : {false, true})
        for (bool load : {false, true})
        for (unsigned mode = 0; mode < 4; ++mode)
        for (bool writeback : {false, true})
        for (unsigned condition = 0; condition < (empty ? 3u : 1u); ++condition)
        {
            const bool up = mode < 2, pre = (mode & 1) != 0;
            const u32 distance = empty ? 64 : 4;
            const u32 transfer = up ? Base + (pre ? 4 : 0) : Base - distance + (pre ? 0 : 4);
            const u32 addr = Code + program++ * 128, target = addr + 64;
            const u32 opcode = (condition ? 0u : 0xE0000000) | 0x08040000 |
                (pre ? 1u << 24 : 0) | (up ? 1u << 23 : 0) |
                (writeback ? 1u << 21 : 0) | (load ? 1u << 20 : 0) | (empty ? 0 : 2);
            nds->ARM9Write32(addr, 0xE3560000); // CMP R6,#0, live flags before fallback.
            nds->ARM9Write32(addr + 4, opcode);
            nds->ARM9Write32(addr + 8, 0xE10F8000); // MRS R8,CPSR
            nds->ARM9Write32(addr + 12, 0xE3A05011);
            nds->ARM9Write32(addr + 16, 0xEAFFFFFE);
            nds->ARM9Write32(target, 0xE10F8000);
            nds->ARM9Write32(target + 4, 0xE3A05022);
            nds->ARM9Write32(target + 8, 0xEAFFFFFE);
            for (unsigned run = 0; run < 2; ++run)
            {
                // Conditional branches change their outcome on warmed entry.
                const bool zero = condition == 0 || (condition == 1 ? run != 0 : run == 0);
                const bool execute = condition == 0 || zero;
                ClearData(*nds);
                if (load) nds->ARM9Write32(transfer, empty ? target | 3 : Value);
                cpu.R[1] = load ? Sentinel : Value;
                cpu.R[4] = Base;
                cpu.R[5] = cpu.R[8] = 0;
                cpu.R[6] = zero ? 0 : 1;
                cpu.CPSR = 0xDF;
                const bool warm = run == 0 || HasBlock(*nds, arm7, addr, jit);
                cpu.JumpTo(addr);
                nds->RunFrame();
                const bool branch = execute && empty && load && arm7;
                const u32 expectedBase = execute && writeback ? (up ? Base + distance : Base - distance) : Base;
                const u32 expectedFlags = (zero ? 0x60000000 : 0x20000000) | 0xDF;
                const u32 expectedR1 = load && (!execute || empty) ? Sentinel : Value;
                bool memoryOK = true;
                for (u32 mem = Base - 64; mem <= Base + 64; mem += 4)
                {
                    u32 expected = Sentinel;
                    if (load && mem == transfer) expected = empty ? target | 3 : Value;
                    if (!load && execute && (!empty || arm7) && mem == transfer)
                        expected = empty ? addr + 16 : Value; // Instruction A=addr+4, PC=A+12.
                    memoryOK &= nds->ARM9Read32(mem) == expected;
                }
                ++checks;
                if (!warm || !memoryOK || cpu.R[4] != expectedBase || cpu.R[1] != expectedR1 ||
                    cpu.R[5] != (branch ? 0x22u : 0x11u) || cpu.R[8] != expectedFlags || cpu.CPSR != expectedFlags)
                {
                    if (failures++ < 20)
                        std::fprintf(stderr, "ARM%d %08x run=%u empty=%d warm=%d mem=%d: base=%08x/%08x marker=%x/%x flags=%08x/%08x\n",
                            arm7 ? 7 : 9, opcode, run, empty, warm, memoryOK, cpu.R[4], expectedBase,
                            cpu.R[5], branch ? 0x22 : 0x11, cpu.R[8], expectedFlags);
                }
            }
        }

        for (bool empty : {false, true})
        for (bool load : {false, true})
        {
            const u32 addr = Code + program++ * 128, target = addr + 64;
            const u16 opcode = (load ? 0xCC00 : 0xC400) | (empty ? 0 : 2);
            nds->ARM9Write16(addr, 0x2E00); // CMP R6,#0
            nds->ARM9Write16(addr + 2, opcode);
            nds->ARM9Write16(addr + 4, 0x465D); // MOV R5,R11 (flags unchanged)
            nds->ARM9Write16(addr + 6, 0xE7FE);
            nds->ARM9Write16(target, 0x4655); // MOV R5,R10
            nds->ARM9Write16(target + 2, 0xE7FE);
            for (unsigned run = 0; run < 2; ++run)
            {
                ClearData(*nds);
                if (load) nds->ARM9Write32(Base, empty ? target : Value); // bit0=0 retains Thumb on ARM7.
                cpu.R[1] = load ? Sentinel : Value;
                cpu.R[4] = Base;
                cpu.R[5] = cpu.R[6] = 0;
                cpu.R[10] = 0x22;
                cpu.R[11] = 0x11;
                cpu.CPSR = 0xDF;
                const bool warm = run == 0 || HasBlock(*nds, arm7, addr | 1, jit);
                cpu.JumpTo(addr | 1);
                nds->RunFrame();
                const u32 expectedMemory = load ? (empty ? target : Value) :
                    !empty ? Value : arm7 ? addr + 8 : Sentinel; // Instruction A=addr+2, PC=A+6.
                ++checks;
                if (!warm || cpu.R[4] != Base + (empty ? 64 : 4) || cpu.R[1] != (load && empty ? Sentinel : Value) ||
                    cpu.R[5] != (empty && load && arm7 ? 0x22u : 0x11u) || cpu.CPSR != 0x600000FF ||
                    nds->ARM9Read32(Base) != expectedMemory || nds->ARM9Read32(Base + 4) != Sentinel)
                {
                    if (failures++ < 24)
                        std::fprintf(stderr, "ARM%d Thumb %04x run=%u warm=%d: base=%08x marker=%x flags=%08x memory=%08x/%08x\n",
                            arm7 ? 7 : 9, opcode, run, warm, cpu.R[4], cpu.R[5], cpu.CPSR,
                            nds->ARM9Read32(Base), expectedMemory);
                }
            }
        }

        // Ordinary STM has an independent ARM7 PC-store oracle: A+12. Include
        // a second register to exercise both single-word and block emit paths.
        for (bool pair : {false, true})
        {
            const u32 addr = Code + program++ * 128;
            nds->ARM9Write32(addr, 0xE8A48000 | (pair ? 2 : 0));
            nds->ARM9Write32(addr + 4, 0xEAFFFFFE);
            for (unsigned run = 0; run < 2; ++run)
            {
                ClearData(*nds);
                cpu.R[1] = Value;
                cpu.R[4] = Base;
                cpu.CPSR = 0xDF;
                const bool warm = run == 0 || HasBlock(*nds, arm7, addr, jit);
                cpu.JumpTo(addr);
                nds->RunFrame();
                const u32 storedPC = nds->ARM9Read32(Base + (pair ? 4 : 0));
                ++checks;
                if (!warm || storedPC != addr + (arm7 ? 12 : 8) || cpu.R[4] != Base + (pair ? 8 : 4) ||
                    (pair && nds->ARM9Read32(Base) != Value) || nds->ARM9Read32(Base + 8) != Sentinel || cpu.CPSR != 0xDF)
                {
                    ++failures;
                    std::fprintf(stderr, "ARM%d STM PC pair=%d run=%u warm=%d: stored=%08x/%08x base=%08x\n",
                        arm7 ? 7 : 9, pair, run, warm, storedPC, addr + (arm7 ? 12 : 8), cpu.R[4]);
                }
            }
        }

        // ARM7 empty LDM^ is a PC transfer and restores SPSR, including Thumb
        // and banked SP. ARM9 empty LDM has no PC transfer / status restoration.
        for (bool samePipelinePC : {false, true})
        {
            if (samePipelinePC && !arm7) continue;
            const u32 addr = Code + program++ * 128, target = addr + 64;
            const u32 thumbTarget = samePipelinePC ? addr + 6 : target;
            nds->ARM9Write32(addr, samePipelinePC ? 0x08F40000 : 0xE8F40000); // LDMEQ/AL R4!,{}^
            nds->ARM9Write32(addr + 4, 0xE3A05011);
            nds->ARM9Write32(addr + 8, 0xEAFFFFFE);
            // At A+6, Thumb refill leaves R15=A+8, equal to ARM's pre-jump R15.
            // A changed instruction set must still end the old compiled block.
            nds->ARM9Write16(thumbTarget, 0x4655);
            nds->ARM9Write16(thumbTarget + 2, 0xE7FE);
            for (unsigned run = 0; run < 2; ++run)
            {
                cpu.UpdateMode(cpu.CPSR, 0xDF);
                cpu.CPSR = 0xDF;
                cpu.R[13] = 0x02007000;
                cpu.UpdateMode(cpu.CPSR, 0xD3);
                cpu.CPSR = samePipelinePC ? 0x400000D3 : 0xD3;
                cpu.R[13] = 0x02007800;
                cpu.R_SVC[2] = 0xA00000FF;
                cpu.R[4] = Base;
                cpu.R[5] = 0;
                cpu.R[10] = 0x22;
                nds->ARM9Write32(Base, thumbTarget);
                const bool warm = run == 0 || HasBlock(*nds, arm7, addr, jit);
                cpu.JumpTo(addr);
                nds->RunFrame();
                ++checks;
                if (!warm || cpu.R[4] != Base + 64 || cpu.R[5] != (arm7 ? 0x22u : 0x11u) ||
                    cpu.CPSR != (arm7 ? 0xA00000FFu : 0xD3u) || cpu.R[13] != (arm7 ? 0x02007000u : 0x02007800u))
                {
                    ++failures;
                    std::fprintf(stderr, "ARM%d empty LDM^ samePC=%d run=%u warm=%d: base=%08x marker=%x flags=%08x SP=%08x\n",
                        arm7 ? 7 : 9, samePipelinePC, run, warm, cpu.R[4], cpu.R[5], cpu.CPSR, cpu.R[13]);
                }
            }
        }
        cpu.JumpTo(Idle);
    }

    // No-transfer instructions must not charge a previous instruction's data
    // wait time. The equality is a model invariant, not an absolute cycle oracle.
    auto observed = std::make_unique<ObservedARM9>(*nds);
    observed->CP15Reset();
    observed->Reset();
    for (u32 opcode : {0xE8A40000u, 0xE8B40000u, 0xC400u, 0xCC00u})
    {
        s32 elapsed[2]{};
        for (unsigned previous = 0; previous < 2; ++previous)
        {
            observed->R[15] = Code + 8;
            observed->CPSR = opcode <= 0xFFFF ? 0xFF : 0xDF;
            observed->CurInstr = opcode;
            observed->R[4] = Base;
            observed->CodeCycles = 4;
            observed->DataCycles = previous ? 99 : 1;
            observed->Cycles = 0;
            observed->reads = observed->writes = 0;
            if (opcode <= 0xFFFF)
                ARMInterpreter::THUMBInstrTable[opcode >> 6](observed.get());
            else
                ARMInterpreter::ARMInstrTable[((opcode >> 4) & 0xF) | ((opcode >> 16) & 0xFF0)](observed.get());
            elapsed[previous] = observed->Cycles;
            ++checks;
            if (observed->reads || observed->writes || observed->R[4] != Base + 64 || observed->R[15] != Code + 8)
            {
                ++failures;
                std::fprintf(stderr, "ARM9 no-transfer %08x reads=%u writes=%u base=%08x PC=%08x\n",
                    opcode, observed->reads, observed->writes, observed->R[4], observed->R[15]);
            }
        }
        ++checks;
        if (elapsed[0] <= 0 || elapsed[0] != elapsed[1])
        {
            ++failures;
            std::fprintf(stderr, "ARM9 no-transfer %08x retained previous data cycles: %d/%d\n", opcode, elapsed[0], elapsed[1]);
        }
    }
    std::printf("block transfers / empty lists: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
