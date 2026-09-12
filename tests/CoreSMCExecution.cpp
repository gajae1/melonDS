// SPDX-License-Identifier: GPL-3.0-or-later
// Generated guest code through the real interpreter, dispatcher and write barriers.
#include "Args.h"
#include "ARM.h"
#include "NDS.h"
#include <cstdio>
#include <memory>
#include <utility>

using namespace melonDS;

namespace
{
constexpr u32 Code = 0x02008000, Neighbor = Code + 0x20, Data = Code + 0x40;
constexpr u32 Mirror = 0x00400000;

u64 Execute(NDS& nds, ARM& cpu, bool jit, u32 addr)
{
    nds.CurCPU = cpu.Num;
    cpu.CPSR = 0xA00000DF;
    cpu.StopExecution = 0;
    cpu.JumpTo(addr);
    cpu.Cycles = 0;
    auto& timestamp = cpu.Num ? nds.ARM7Timestamp : nds.ARM9Timestamp;
    auto& target = cpu.Num ? nds.ARM7Target : nds.ARM9Target;
    timestamp = 0;
    // Let the interpreter finish even ARM9 STM and enter the terminal B .
    // before comparing with JIT execution, whose budget check is per block.
    target = 128;
#ifdef JIT_ENABLED
    if (jit)
    {
        if (cpu.Num) nds.ARM7.Execute<CPUExecuteMode::JIT>();
        else nds.ARM9.Execute<CPUExecuteMode::JIT>();
    }
    else
#endif
    {
        if (cpu.Num) nds.ARM7.Execute<CPUExecuteMode::Interpreter>();
        else nds.ARM9.Execute<CPUExecuteMode::Interpreter>();
    }
    return timestamp;
}

#ifdef JIT_ENABLED
JitBlockEntry Lookup(NDS& nds, u32 num, u32 addr, bool thumb)
{
    u64* entries = nullptr;
    u32 start = 0, size = 0;
    if (!nds.JIT.SetupExecutableRegion(num, addr, entries, start, size)) return nullptr;
    return nds.JIT.LookUpBlock(num, entries, addr - start, addr, thumb);
}
#endif
}

int TestSMCExecution(NDSArgs&& args, bool jit)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const bool fast = args.JIT && args.JIT->FastMemory;
#ifdef JIT_ENABLED
    if (fast && !ARMJIT_Memory::IsFastMemSupported()) return 77;
#endif
    if (args.JIT)
    {
        args.JIT->MaxBlockSize = 32;
        args.JIT->BranchOptimizations = false;
        args.JIT->LiteralOptimizations = false;
    }
    auto nds = std::make_unique<NDS>(std::move(args));
    unsigned failures = 0, checks = 0;
    for (bool arm7 : {false, true})
    for (bool thumb : {false, true})
    for (bool mirror : {false, true})
    for (bool multiple : {false, true})
    {
        nds->Reset();
        NDS::Current = nds.get();
        auto& cpu = arm7 ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
        const u32 store = thumb ? (multiple ? 0xC2FA : 0x8011)
                                : (multiple ? 0xE88203FA : 0xE5821000);
        const u32 initial = thumb ? 0x2007 : 0xE3A00007; // MOV r0,#7
        const u32 updated = thumb ? 0x2009 : 0xE3A00009; // MOV r0,#9
        if (thumb)
        {
            nds->ARM9Write16(Code, initial);
            nds->ARM9Write16(Code + 2, store); // STRH r1,[r2] or STMIA r2!,{r1,r3-r7}
            nds->ARM9Write16(Code + 4, 0xE7FE);
        }
        else
        {
            nds->ARM9Write32(Code, initial);
            nds->ARM9Write32(Code + 4, store); // STR r1,[r2] or STMIA r2,{r1,r3-r9}
            nds->ARM9Write32(Code + 8, 0xEAFFFFFE);
        }
        // The last STM word is outside the trace's 16-byte code granule.
        // Its earlier words must still invalidate the new block. Keep the
        // remaining program bytes unchanged so each reentry is well defined.
        cpu.R[1] = multiple && thumb ? (store << 16) | updated : updated;
        cpu.R[3] = thumb ? 0xE7FEE7FE : store;
        cpu.R[4] = thumb ? 0x12345678 : 0xEAFFFFFE;
        for (unsigned r = 5; r < 10; ++r) cpu.R[r] = 0x10203040 + r;

        nds->ARM9Write32(Neighbor, 0xE3A0A02A); // MOV r10,#42; B .
        nds->ARM9Write32(Neighbor + 4, 0xEAFFFFFE);
        Execute(*nds, cpu, jit, Neighbor);
        Execute(*nds, cpu, jit, Neighbor);
        if (cpu.R[10] != 42) return 2;
#ifdef JIT_ENABLED
        const auto neighborEntry = jit ? Lookup(*nds, arm7, Neighbor, false) : nullptr;
        if (jit && !neighborEntry) return 2;
#endif

        for (unsigned run = 0; run < 5; ++run)
        {
            // First three entries modify code. Then ordinary stores in another
            // granule must retain the block and execute natively on run 4.
            const u32 entry = Code + (mirror && run >= 3 ? Mirror : 0);
            const u32 write = (run < 3 ? Code : Data) + (mirror ? Mirror : 0);
            cpu.R[0] = 0xBAD;
            cpu.R[2] = write;
            bool cached = false, neighborKept = true;
            long long emitted = 0;
#ifdef JIT_ENABLED
            const auto* nativeBefore = nds->JIT.JITCompiler.GetCodePtr();
            if (jit) cached = Lookup(*nds, arm7, entry, thumb) != nullptr;
#endif
            const u64 timestamp = Execute(*nds, cpu, jit, entry | u32(thumb));
#ifdef JIT_ENABLED
            emitted = nds->JIT.JITCompiler.GetCodePtr() - nativeBefore;
            if (jit) neighborKept = Lookup(*nds, arm7, Neighbor, false) == neighborEntry;
#endif
            const u32 observed = thumb ? nds->ARM9Read16(Code) : nds->ARM9Read32(Code);
            const u32 stored = thumb && !multiple ? nds->ARM9Read16(write) : nds->ARM9Read32(write);
            const u32 expectedPC = entry + (thumb ? 6 : 12);
            const u32 expectedCPSR = thumb ? 0x200000FF : 0xA00000DF;
            const bool ok = cpu.R[0] == (run ? 9u : 7u) && observed == updated
                && stored == cpu.R[1] && cpu.R[15] == expectedPC && cpu.CPSR == expectedCPSR
                && neighborKept && (!jit || run != 4 || (cached && emitted == 0));
            failures += !ok;
            checks++;
            std::printf("SMC %s ARM%d %s mirror=%d multiple=%d run=%u entry=%08X write=%08X cached=%d opcode=%08X stored=%08X r0=%u expected=%u pc=%08X cpsr=%08X cycles=%llu neighbor=%d emitted=%lld result=%s\n",
                fast ? "fastmem" : jit ? "jit" : "interpreter", arm7 ? 7 : 9,
                thumb ? "thumb" : "arm", mirror, multiple, run, entry, write, cached, observed,
                stored, cpu.R[0], run ? 9u : 7u, cpu.R[15], cpu.CPSR,
                static_cast<unsigned long long>(timestamp), neighborKept, emitted, ok ? "PASS" : "FAIL");
        }
        cpu.R[10] = 0;
        Execute(*nds, cpu, jit, Neighbor);
        if (cpu.R[10] != 42) return 2;
    }
    std::printf("SMC checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
