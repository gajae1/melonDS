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


int TestSMCLiteralRegions(NDSArgs&& args, bool jit)
{
    if (args.JIT)
    {
        args.JIT->LiteralOptimizations = true;
        args.JIT->BranchOptimizations = false;
    }
    NDSArgs referenceArgs;
    referenceArgs.JIT.reset();
    auto reference = std::make_unique<NDS>(std::move(referenceArgs));
    auto actual = std::make_unique<NDS>(std::move(args));
    unsigned checks = 0, failures = 0;
    for (unsigned region = 0; region < 5; ++region)
    for (unsigned num = 0; num < 2; ++num)
    for (bool thumb : {false, true})
    {
        if (region == 0 && num) continue; // DTCM belongs to ARM9.
        const u32 boundary = region == 0 ? 0x02004000 : region == 1 ? 0x04000000
            : region == 2 ? 0x02400000 : 0x03000000;
        const u32 code = region == 3 ? boundary - 16 : boundary - (thumb ? 4 : 8);
        const u32 literal = boundary + (region == 1 ? 0x210 : 0); // IE is a mutable I/O register.
        constexpr u32 neighbor = 0x02020000;
        for (NDS* nds : {reference.get(), actual.get()})
        {
            nds->Reset();
            NDS::Current = nds;
            if (region == 0)
            {
                nds->ARM9.CP15Write(0x910, boundary | 0xA);
                nds->ARM9.CP15Write(0x100, nds->ARM9.CP15Read(0x100) | (1u << 16));
            }
            if (region >= 3)
            {
                // Populate both physical halves once, then change only their
                // mapping: a data write would hide missing remap invalidation.
                nds->ARM9Write8(0x04000247, 0);
                nds->ARM9Write32(0x03000000, 0x1111);
                nds->ARM9Write32(0x03004000, 0x2222);
                if (num) nds->ARM9Write8(0x04000247, 3);
            }
            ARM& cpu = num ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
            if (thumb)
            {
                cpu.DataWrite16(code, 0x4800 | ((literal - ((code + 4) & ~3u)) >> 2));
                cpu.DataWrite16(code + 2, 0xE7FE);
            }
            else
            {
                cpu.DataWrite32(code, 0xE59F0000 | (literal - (code + 8)));
                cpu.DataWrite32(code + 4, 0xEAFFFFFE);
            }
            cpu.DataWrite32(neighbor, 0xE3A0A02A);
            cpu.DataWrite32(neighbor + 4, 0xEAFFFFFE);
            Execute(*nds, cpu, nds == actual.get() && jit, neighbor);
            Execute(*nds, cpu, nds == actual.get() && jit, neighbor);
        }
#ifdef JIT_ENABLED
        const auto neighborEntry = jit ? Lookup(*actual, num, neighbor, false) : nullptr;
#endif
        for (u32 value : {0x1111u, 0x2222u})
        {
            for (NDS* nds : {reference.get(), actual.get()})
            {
                NDS::Current = nds;
                if (region >= 3)
                    nds->ARM9Write8(0x04000247, num ? (value == 0x1111 ? 3 : 2) : (value == 0x1111 ? 0 : 1));
                else if (num) nds->ARM7.DataWrite32(literal, value);
                else nds->ARM9.DataWrite32(literal, value);
            }
            for (unsigned warm = 0; warm < 2; ++warm)
            {
                ARM& ref = num ? static_cast<ARM&>(reference->ARM7) : static_cast<ARM&>(reference->ARM9);
                ARM& cpu = num ? static_cast<ARM&>(actual->ARM7) : static_cast<ARM&>(actual->ARM9);
                ref.R[0] = cpu.R[0] = 0;
                NDS::Current = reference.get();
                const auto expectedCycles = Execute(*reference, ref, false, code | u32(thumb));
                NDS::Current = actual.get();
#ifdef JIT_ENABLED
                const auto* before = actual->JIT.JITCompiler.GetCodePtr();
                const bool cached = !jit || Lookup(*actual, num, code, thumb);
#endif
                const auto cycles = Execute(*actual, cpu, jit, code | u32(thumb));
                bool ok = cpu.R[0] == value && cpu.R[0] == ref.R[0] && cpu.R[15] == ref.R[15]
                    && cpu.CPSR == ref.CPSR && cycles == expectedCycles;
#ifdef JIT_ENABLED
                ok = ok && (!jit || (Lookup(*actual, num, neighbor, false) == neighborEntry
                    && (!warm || (cached && actual->JIT.JITCompiler.GetCodePtr() == before))));
                if (jit && region >= 3)
                {
                    const auto& blocks = num ? actual->JIT.JitBlocks7 : actual->JIT.JitBlocks9;
                    const auto found = blocks.find(code | u32(thumb));
                    ok = ok && found != blocks.end() && found->second->NumLiterals == 1
                        && actual->JIT.InvalidLiterals.Length == 0;
                }
#endif
                ++checks;
                failures += !ok;
                std::printf("literal region=%u ARM%d thumb=%d value=%04X warm=%u r0=%08X pc=%08X cycles=%llu expected=%llu %s\n",
                    region, num ? 7 : 9, thumb, value, warm, cpu.R[0], cpu.R[15],
                    static_cast<unsigned long long>(cycles), static_cast<unsigned long long>(expectedCycles), ok ? "PASS" : "FAIL");
            }
        }
    }
    std::printf("Literal regions checks=%u failures=%u\n", checks, failures);
    return failures ? 1 : 0;
}
