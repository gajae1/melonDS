// SPDX-License-Identifier: GPL-3.0-or-later
// Hand-authored ARM instructions, real core/scheduler/software renderer.
// No external ROM/BIOS; this is a regression test, NOT real-hardware validation.
#include "Args.h"
#include "NDS.h"
#include "ARM.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
#if defined(JIT_ENABLED) && defined(__x86_64__)
#include "jit/CPUDetect.h"
#endif
using namespace melonDS;

int TestALUExecution(NDSArgs&& args, bool jit);
int TestThumbShiftTiming(NDSArgs&& args, bool jit);
int TestBlockTransferExecution(NDSArgs&& args, bool jit);
int TestDTCMExecution(NDSArgs&& args, bool jit);
int TestMPUExecution(NDSArgs&& args, bool jit);
int TestDeviceExecution(NDSArgs&& args, bool jit);
int TestDSiNDMAExecution(NDSArgs&& args);
int TestDSiResetI2C(NDSArgs&& args);
int TestDSiBTDMP(NDSArgs&& args);
int TestSchedulerExecution(NDSArgs&& args);

static int TestSchedulerSavestate(NDSArgs&& args)
{
    struct SchedulerFixture : NDS
    {
        using NDS::NDS;
        using NDS::SchedListMask;
        using NDS::RunSystem;
        bool LateRegistration = false;
        unsigned LateCalls = 0;

        void DoSavestateExtra(Savestate* file) override
        {
            if (file->Saving || !LateRegistration) return;
            // Exercise the real load hook used to recreate DSP HLE callbacks.
            CancelEvent(Event_DSi_DSPHLE);
            UnregisterEventFuncs(Event_DSi_DSPHLE);
            RegisterEventFuncs(Event_DSi_DSPHLE, this, {[](void* that, u32) {
                ++static_cast<SchedulerFixture*>(that)->LateCalls;
            }});
        }
    };
    auto instance = std::make_unique<SchedulerFixture>(std::move(args));
    auto& nds = *instance;
    constexpr u32 divMask = 1u << Event_Div;
    const struct {
        const char* name;
        u32 event;
        u32 func;
        u32 mask;
        bool accept;
        bool late = false;
    } cases[] = {
        {"active-registered", Event_Div, 0, divMask, true},
        {"active-last-registered", Event_LCD, 2, 1u << Event_LCD, true},
        {"inactive-stale-id", Event_DSi_DSPHLE, 0xFFFFFFFFu, divMask, true},
        {"inactive-null-callback", Event_Div, 1, 0, true},
        {"funcid-3", Event_Div, 3, divMask, false},
        {"funcid-uint-max", Event_Div, 0xFFFFFFFFu, divMask, false},
        {"unregistered-callback", Event_Div, 1, divMask, false},
        {"invalid-mask", Event_Div, 0, divMask | (1u << Event_MAX), false},
        {"active-late-registered", Event_DSi_DSPHLE, 0, 1u << Event_DSi_DSPHLE, true, true},
    };
    unsigned failures = 0;
    for (const auto& test : cases)
    {
        nds.Reset();
        nds.UnregisterEventFuncs(Event_DSi_DSPHLE);
        nds.LateRegistration = test.late;
        nds.LateCalls = 0;
        nds.SchedListMask = test.mask;
        auto& event = nds.SchedList[test.event];
        event.FuncID = test.func;
        event.Timestamp = 1234;
        event.Param = 0x12345678;

        // Use the real serializer so only the scheduler metadata is malformed,
        // with no duplicated NDSG layout or hard-coded byte offsets.
        Savestate saved;
        if (!nds.DoSavestate(&saved) || saved.Error)
        {
            std::fprintf(stderr, "scheduler fixture save failed: %s\n", test.name);
            return 2;
        }
        nds.Reset();
        // A rejected load must not replace this runnable event with bad input.
        nds.SchedListMask = divMask;
        nds.SchedList[Event_Div].Timestamp = 4321;
        nds.SchedList[Event_Div].Param = 0x87654321;
        const auto previous = nds.SchedList[Event_Div];
        Savestate load(saved.Buffer(), saved.Length(), false);
        if (load.Error)
        {
            std::fprintf(stderr, "scheduler fixture header failed: %s\n", test.name);
            return 2;
        }
        const bool accepted = nds.DoSavestate(&load);
        bool matches = accepted == test.accept && (!test.accept ||
            (!load.Error && nds.SchedListMask == test.mask &&
             event.FuncID == test.func && event.Timestamp == 1234 &&
             event.Param == 0x12345678));
        if (matches && !test.accept)
        {
            const auto& current = nds.SchedList[Event_Div];
            matches = nds.SchedListMask == divMask && current.FuncID == previous.FuncID &&
                current.Timestamp == previous.Timestamp && current.Param == previous.Param &&
                current.Funcs[0] == previous.Funcs[0] && current.That == previous.That;
            // Dispatch only after checking that the old valid event survived.
            if (matches)
            {
                nds.RunSystem(previous.Timestamp);
                matches = nds.SchedListMask == 0;
            }
        }
        if (matches && test.late)
        {
            nds.RunSystem(1234);
            matches = nds.LateCalls == 1 && nds.SchedListMask == 0;
        }
        std::printf("scheduler-load %s: %s (accepted=%d expected=%d error=%d)\n",
                    test.name, matches ? "PASS" : "FAIL", accepted, test.accept, load.Error);
        if (!matches) ++failures;
        // Full CPU/RAM/device rollback is separate from scheduler safety.
    }
    std::printf("savestate-scheduler: %zu cases, %u failures\n", std::size(cases), failures);
    return failures ? 1 : 0;
}

static int TestUnalignedMemory(NDSArgs&& args, bool jit)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (args.JIT)
    {
        args.JIT->MaxBlockSize = 1;
        args.JIT->BranchOptimizations = false;
    }
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    NDS::Current = nds.get();
    // Little endian, alignment checking disabled. Do not test abort/PC operands.
    if (nds->ARM9.CP15Control & ((1u << 1) | (1u << 7))) return 2;
    constexpr u32 code = 0x02008000, data = 0x02012000;
    constexpr u32 initial = 0x807F1234, value = 0xA1B2C3D4;
    constexpr u32 before = 0x0BADF00D, after = 0xDEADBEEF;
    // ARM DDI 0100I: pre-v6 ARM LDR rotates the aligned word, STR ignores bits 1:0.
    // Aligned LDRH/LDRSH zero/sign-extend. Odd halfwords and unaligned Thumb words
    // are UNPREDICTABLE: observe them without treating backend agreement as an oracle.
    // All opcodes use r0,[r1,r2]; warm runs change r2 on the SAME cached block.
    const struct {
        const char* name;
        u32 arm;
        u16 thumb;
        bool store, half;
        unsigned offsets;
        u32 expected[4]; // destination register for loads, containing word for stores
    } cases[] = {
        {"ldr",   0xE7910002, 0x5888, false, false, 0xF, {initial, 0x34807F12, 0x1234807F, 0x7F123480}},
        {"str",   0xE7810002, 0x5088, true,  false, 0x9, {value, 0, 0, value}},
        {"ldrh",  0xE19100B2, 0x5A88, false, true,  0xD, {0x1234, 0, 0x807F, 0}},
        {"ldrsh", 0xE19100F2, 0x5E88, false, true,  0xD, {0x1234, 0, 0xFFFF807F, 0}},
        {"strh",  0xE18100B2, 0x5288, true,  true,  0xD, {0x807FC3D4, 0, 0xC3D41234, 0}},
    };
    unsigned checked = 0, observed = 0, failures = 0;
    for (bool arm7 : {false, true})
    for (bool thumb : {false, true})
    {
        nds->CurCPU = arm7 ? 1 : 0;
        ARM& cpu = arm7 ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
        const u32 cpsr = 0xA00000DF | (thumb ? 0x20 : 0);
        for (unsigned i = 0; i < std::size(cases); ++i)
        {
            const auto& test = cases[i];
            const u32 addr = code + (thumb ? 0x100 : 0) + i * 16;
            nds->ARM9Write32(addr, thumb ? u32(test.thumb) | 0xE7FE0000 : test.arm);
            nds->ARM9Write32(addr + 4, 0xEAFFFFFE);
#if defined(JIT_ENABLED) && defined(__x86_64__)
            std::vector<u8> compiled;
#endif
            for (unsigned offset = 0; offset < 4; ++offset)
            {
                if (!(test.offsets & (1u << offset))) continue;
                for (unsigned run = 0; run < (jit && offset == 0 ? 2u : 1u); ++run)
                {
                    const bool cached = jit && (offset != 0 || run != 0);
                    for (unsigned r = 0; r < 15; ++r) cpu.R[r] = 0x01020300 + r;
                    cpu.R[0] = value;
                    cpu.R[1] = data;
                    cpu.R[2] = offset;
                    nds->ARM9Write32(data - 4, before);
                    nds->ARM9Write32(data, initial);
                    nds->ARM9Write32(data + 4, after);
                    cpu.CPSR = cpsr;
                    cpu.JumpTo(addr | u32(thumb));
                    cpu.Cycles = 0;
                    auto& timestamp = arm7 ? nds->ARM7Timestamp : nds->ARM9Timestamp;
                    auto& target = arm7 ? nds->ARM7Target : nds->ARM9Target;
                    timestamp = 0;
                    target = 1;
#ifdef JIT_ENABLED
                    if (jit)
                    {
                        auto& blocks = arm7 ? nds->JIT.JitBlocks7 : nds->JIT.JitBlocks9;
                        if (cached)
                        {
                            if (!blocks.contains(addr | u32(thumb))) return 2;
                            ARM_Dispatch(&cpu, blocks.at(addr | u32(thumb))->EntryPoint);
                            timestamp = cpu.Cycles;
                        }
                        else if (arm7) nds->ARM7.Execute<CPUExecuteMode::JIT>();
                        else nds->ARM9.Execute<CPUExecuteMode::JIT>();
#if defined(__x86_64__)
                        const auto* entry = reinterpret_cast<const u8*>(blocks.at(addr | u32(thumb))->EntryPoint);
                        if (!cached)
                        {
                            const auto* end = nds->JIT.JITCompiler.GetCodePtr();
                            if (end <= entry || end - entry > 1024) return 2;
                            compiled.assign(entry, end);
                        }
                        if (offset == 3)
                        {
                            std::printf("native-memory ARM%d %s %s fastmem=%d unchanged=%d bytes=",
                                arm7 ? 7 : 9, thumb ? "thumb" : "arm", test.name,
                                nds->JIT.FastMemoryEnabled(), !std::memcmp(compiled.data(), entry, compiled.size()));
                            for (size_t b = 0; b < compiled.size(); ++b) std::printf("%02X", entry[b]);
                            std::puts("");
                        }
#endif
                    }
                    else
#endif
                    {
                        if (arm7) nds->ARM7.Execute<CPUExecuteMode::Interpreter>();
                        else nds->ARM9.Execute<CPUExecuteMode::Interpreter>();
                    }
                    const u32 word = nds->ARM9Read32(data);
                    const bool unpredictable = test.half ? (offset & 1) : thumb && offset != 0;
                    bool ok = true;
                    if (unpredictable) ++observed;
                    else
                    {
                        const u32 expectedR0 = test.store ? value : test.expected[offset];
                        const u32 expectedWord = test.store ? test.expected[offset] : initial;
                        ok = cpu.R[0] == expectedR0 && word == expectedWord &&
                            cpu.R[1] == data && cpu.R[2] == offset && cpu.CPSR == cpsr &&
                            cpu.R[15] == addr + (thumb ? 4 : 8) && timestamp > 0 &&
                            nds->ARM9Read32(data - 4) == before && nds->ARM9Read32(data + 4) == after;
                        for (unsigned r = 3; r < 15; ++r) ok &= cpu.R[r] == 0x01020300 + r;
                        ++checked;
                        failures += !ok;
                    }
                    std::printf("%s ARM%d %s %s opcode=%08X offset=%u: %s r0=%08X memory=%08X/%08X/%08X pc=%08X cpsr=%08X cycles=%llu\n",
                        jit ? (cached ? "dispatch" : "jit-cold") : "interpreter", arm7 ? 7 : 9,
                        thumb ? "thumb" : "arm", test.name, thumb ? test.thumb : test.arm, offset,
                        unpredictable ? "OBSERVATION-UNPREDICTABLE" : ok ? "PASS" : "FAIL", cpu.R[0],
                        nds->ARM9Read32(data - 4), word, nds->ARM9Read32(data + 4), cpu.R[15], cpu.CPSR,
                        static_cast<unsigned long long>(timestamp));
                }
            }
        }
    }
    std::printf("single memory alignment: %u defined checks, %u failures; %u unpredictable observations\n",
        checked, failures, observed);
    return failures ? 1 : 0;
}

static int TestThumbPushTiming(NDSArgs&& args, bool jit)
{
    if (args.JIT)
    {
        args.JIT->MaxBlockSize = 1;
        args.JIT->BranchOptimizations = false;
    }
    auto nds = std::make_unique<NDS>(std::move(args));
    NDS::Current = nds.get();
    // These are existing melonDS memory-model costs, not measured DS timings.
    // Main RAM defaults: N16=8, S16=1, N32=9. Private WRAM: all 1.
    // Same-main-RAM code/data accesses serialize; separate regions retain the
    // existing overlap model. Vary N/S independently to catch using S for PUSH.
    const struct { const char* name; u32 code, stack, n16, s16, cycles; } cases[] = {
        {"default-main-main", 0x02008000, 0x02012040, 0, 0, 17},
        {"c4-main-main",      0x02008000, 0x02012040, 4, 4, 13},
        {"n4-s1-main-main",   0x02008000, 0x02012040, 4, 1, 13},
        {"default-main-wram", 0x02008000, 0x03802040, 0, 0,  8},
        {"default-wram-main", 0x03808000, 0x02012040, 0, 0,  9},
    };
    constexpr u32 lr = 0x12345678, guard = 0x89ABCDEF, cpsr = 0xA00000FF;
    unsigned checked = 0, failures = 0;
    for (const auto& test : cases)
    {
        nds->Reset(); // Restore default timings and discard differently timed blocks.
        nds->CurCPU = 1;
        auto& cpu = nds->ARM7;
        if (test.n16)
        {
            auto& timing = nds->ARM7MemTimings[test.code >> 15];
            timing[0] = timing[2] = test.n16;
            timing[1] = timing[3] = test.s16;
        }
        std::printf("push-timing %s: code N16=%u S16=%u data N32=%u expected-model=%u\n",
            test.name, nds->ARM7MemTimings[test.code >> 15][0],
            nds->ARM7MemTimings[test.code >> 15][1],
            nds->ARM7MemTimings[(test.stack - 4) >> 15][2], test.cycles);
        for (u32 halfword : {0u, 2u})
        {
            const u32 addr = test.code + halfword * 8 + halfword;
            nds->ARM7Write16(addr, 0xB500); // PUSH {LR}, one nonempty stack transfer.
            nds->ARM7Write16(addr + 2, 0xE7FE);
            for (unsigned run = 0; run < (jit ? 2u : 1u); ++run)
            {
                cpu.R[13] = test.stack;
                cpu.R[14] = lr;
                for (int offset : {-8, -4, 0}) nds->ARM7Write32(test.stack + offset, guard);
                cpu.CPSR = cpsr;
                cpu.JumpTo(addr | 1);
                cpu.Cycles = 0;
                cpu.DataCycles = 37;
                nds->ARM7Timestamp = 0;
                nds->ARM7Target = 1;
#ifdef JIT_ENABLED
                if (jit)
                {
                    if (run)
                    {
                        if (!nds->JIT.JitBlocks7.contains(addr | 1)) return 2;
                        ARM_Dispatch(&cpu, nds->JIT.JitBlocks7.at(addr | 1)->EntryPoint);
                        nds->ARM7Timestamp = cpu.Cycles;
                    }
                    else cpu.Execute<CPUExecuteMode::JIT>();
#if defined(__x86_64__)
                    if (!run && !halfword)
                    {
                        const auto* entry = reinterpret_cast<const u8*>(nds->JIT.JitBlocks7.at(addr | 1)->EntryPoint);
                        const auto* end = nds->JIT.JITCompiler.GetCodePtr();
                        if (end <= entry || end - entry > 1024) return 2;
                        std::printf("native-push %s bytes=", test.name);
                        for (auto* p = entry; p != end; ++p) std::printf("%02X", *p);
                        std::puts("");
                    }
#endif
                }
                else
#endif
                    cpu.Execute<CPUExecuteMode::Interpreter>();
                const bool ok = nds->ARM7Timestamp == test.cycles &&
                    cpu.R[13] == test.stack - 4 && cpu.R[14] == lr &&
                    cpu.R[15] == addr + 4 && cpu.CPSR == cpsr &&
                    nds->ARM7Read32(test.stack - 4) == lr &&
                    nds->ARM7Read32(test.stack - 8) == guard && nds->ARM7Read32(test.stack) == guard;
                ++checked;
                failures += !ok;
                std::printf("%s %s halfword=%u run=%u: %s cycles=%llu expected-model=%u sp=%08X pc=%08X stored=%08X\n",
                    jit ? (run ? "dispatch" : "jit-cold") : "interpreter", test.name,
                    halfword, run, ok ? "PASS" : "FAIL",
                    static_cast<unsigned long long>(nds->ARM7Timestamp), test.cycles,
                    cpu.R[13], cpu.R[15], nds->ARM7Read32(test.stack - 4));
            }
        }
    }
    std::printf("ARM7 Thumb PUSH timing: %u checks, %u failures\n", checked, failures);
    return failures ? 1 : 0;
}

static int TestThumbStack(NDSArgs&& args, bool jit)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (args.JIT)
    {
        args.JIT->MaxBlockSize = 1;
        args.JIT->BranchOptimizations = false;
    }
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    // RunFrame normally selects the instance used by JIT memory helpers.
    NDS::Current = nds.get();
    constexpr u32 code = 0x02008000, stack = 0x02012040, targetPC = 0x02009000;
    constexpr u32 sentinel = 0x12345678, cpsr = 0xA00000FF;
    const struct { const char* name; u16 instr; u32 target; } cases[] = {
        {"empty-push", 0xB400, 0}, {"empty-pop", 0xBC00, targetPC | 1},
        {"lr-only", 0xB500, 0},
        {"pc-only-thumb", 0xBD00, targetPC | 1},
        {"pc-only-even", 0xBD00, targetPC},
    };
    // ARM DDI 0100I A7.1.49/50 explicitly make an empty 9-bit list
    // UNPREDICTABLE. Record raw empty-list observations without asserting
    // SP, PC, memory or timing. Only the defined LR/PC-only controls can PASS.
    unsigned checked = 0, failures = 0, observed = 0;
    for (bool arm7 : {false, true})
    {
        nds->CurCPU = arm7 ? 1 : 0;
        ARM& cpu = arm7 ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
        nds->ARM9.MemTimings[code >> 12][0] = 4;
        nds->ARM9.MemTimings[targetPC >> 12][0] = 4;
        for (unsigned i = 0; i < 4; ++i) nds->ARM7MemTimings[code >> 15][i] = 4;
        nds->ARM9Write32(targetPC, 0xEAFFFFFE);
        for (unsigned i = 0; i < std::size(cases); ++i)
        {
            const auto& test = cases[i];
            const u32 addr = code + i * 16;
            nds->ARM9Write16(addr, test.instr);
            nds->ARM9Write16(addr + 2, 0xE7FE);
            for (s32 previousData : {1, 37})
            for (unsigned run = 0; run < (jit ? 2u : 1u); ++run)
            {
                const bool cached = jit && (run != 0 || previousData != 1);
                for (unsigned r = 0; r < 15; ++r) cpu.R[r] = sentinel + r;
                cpu.R[13] = stack;
                for (u32 p = stack - 64; p <= stack + 4; p += 4)
                    nds->ARM9Write32(p, p == stack && test.target ? test.target : sentinel);
                cpu.CPSR = cpsr;
                cpu.JumpTo(addr | 1);
                cpu.Cycles = 0; // Exclude initial pipeline refill.
                cpu.DataCycles = previousData;
                cpu.DataRegion = stack;
                auto& timestamp = arm7 ? nds->ARM7Timestamp : nds->ARM9Timestamp;
                auto& target = arm7 ? nds->ARM7Target : nds->ARM9Target;
                timestamp = 0;
                target = 1;
#ifdef JIT_ENABLED
                if (jit)
                {
                    auto& blocks = arm7 ? nds->JIT.JitBlocks7 : nds->JIT.JitBlocks9;
                    if (cached)
                    {
                        if (!blocks.contains(addr | 1)) return 2;
                        // Exactly one warmed native block, even if it costs zero.
                        ARM_Dispatch(&cpu, blocks.at(addr | 1)->EntryPoint);
                        timestamp = cpu.Cycles;
                    }
                    else if (arm7) nds->ARM7.Execute<CPUExecuteMode::JIT>();
                    else nds->ARM9.Execute<CPUExecuteMode::JIT>();
#if defined(__x86_64__)
                    if (previousData == 1 && run == 0 && i < 2)
                    {
                        if (!blocks.contains(addr | 1)) return 2;
                        const auto* entry = reinterpret_cast<const u8*>(blocks.at(addr | 1)->EntryPoint);
                        const auto* end = nds->JIT.JITCompiler.GetCodePtr();
                        if (end <= entry || end - entry > 1024) return 2;
                        std::printf("native-stack ARM%d guest=%04X bytes=", arm7 ? 7 : 9, test.instr);
                        for (auto* p = entry; p != end; ++p) std::printf("%02X", *p);
                        std::puts("");
                    }
#endif
                }
                else
#endif
                {
                    if (arm7) nds->ARM7.Execute<CPUExecuteMode::Interpreter>();
                    else nds->ARM9.Execute<CPUExecuteMode::Interpreter>();
                }
                if (i < 2)
                {
                    ++observed;
                    std::printf("OBSERVATION %s ARM%d %s opcode=%04X previousD=%d run=%u cycles=%llu sp=%08X pc=%08X cpsr=%08X",
                        jit ? (cached ? "dispatch" : "jit-cold") : "interpreter", arm7 ? 7 : 9,
                        test.name, test.instr, previousData, run,
                        static_cast<unsigned long long>(timestamp), cpu.R[13], cpu.R[15], cpu.CPSR);
                    for (unsigned r = 0; r < 15; ++r)
                        if (r != 13 && cpu.R[r] != sentinel + r)
                            std::printf(" r%u=%08X", r, cpu.R[r]);
                    unsigned writes = 0;
                    for (u32 p = stack - 64; p <= stack + 4; p += 4)
                    {
                        const u32 initial = p == stack && test.target ? test.target : sentinel;
                        const u32 value = nds->ARM9Read32(p);
                        if (value != initial)
                        {
                            ++writes;
                            std::printf(" mem[%08X]=%08X", p, value);
                        }
                    }
                    std::printf(" changed-stack-words=%u (no silicon oracle)\n", writes);
                    continue;
                }
                const bool pushLR = test.instr == 0xB500;
                const bool toARM = test.target && !arm7 && !(test.target & 1);
                const u32 expectedPC = test.target ? targetPC + (toARM ? 4 : 2) : addr + 4;
                const u32 expectedSP = pushLR ? stack - 4 : test.target ? stack + 4 : stack;
                bool ok = cpu.R[13] == expectedSP && cpu.R[15] == expectedPC &&
                    cpu.CPSR == (cpsr & ~(toARM ? 0x20u : 0u)) && timestamp > 0;
                for (unsigned r = 0; r < 15; ++r)
                    if (r != 13) ok &= cpu.R[r] == sentinel + r;
                for (u32 p = stack - 64; p <= stack + 4; p += 4)
                {
                    const u32 expected = pushLR && p == stack - 4 ? sentinel + 14 :
                        p == stack && test.target ? test.target : sentinel;
                    ok &= nds->ARM9Read32(p) == expected;
                }
                ++checked;
                failures += !ok;
                std::printf("%s ARM%d %s opcode=%04X previousD=%d run=%u: %s cycles=%llu sp=%08X pc=%08X cpsr=%08X\n",
                    jit ? (cached ? "dispatch" : "jit-cold") : "interpreter", arm7 ? 7 : 9,
                    test.name, test.instr, previousData, run, ok ? "PASS" : "FAIL",
                    static_cast<unsigned long long>(timestamp), cpu.R[13], cpu.R[15], cpu.CPSR);
            }
        }
    }
    std::printf("thumb stack: %u defined LR-PC checks, %u failures; %u empty-list observations (unverified)\n",
        checked, failures, observed);
    return failures ? 1 : 0;
}

static int TestConditionalCycles(NDSArgs&& args, bool jit)
{
    if (args.JIT)
    {
        args.JIT->MaxBlockSize = 1;
        args.JIT->BranchOptimizations = false;
    }
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    constexpr u32 code = 0x02008000, sentinel = 0x12345678;
    const struct { u32 instr, result, cycles; } instructions[] = {
        {0x00810312, 3, 2}, {0x10810312, 3, 2},
        {0x00810002, 2, 1}, {0xE0810312, 3, 2},
#if defined(__x86_64__)
        // Nearest control for the x64 register-valued C+I helper (guest ARM7).
        {0x00000291, 1, 2}, // MULEQ r0,r1,r2; r1=r2=1
#endif
    };
    unsigned checked = 0, failures = 0;
    for (bool arm7 : {false, true})
    {
        ARM& cpu = arm7 ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
        nds->ARM9.MemTimings[code >> 12][0] = 1;
        for (unsigned i = 0; i < 4; ++i) nds->ARM7MemTimings[code >> 15][i] = 1;
        for (unsigned i = 0; i < std::size(instructions); ++i)
        {
            const auto& test = instructions[i];
            const u32 addr = code + i * 16, instr = test.instr;
            nds->ARM9Write32(addr, instr);
            nds->ARM9Write32(addr + 4, 0xEAFFFFFE);
            for (bool z : {true, false})
            for (unsigned run = 0; run < 2; ++run)
            {
                bool warm = true;
#ifdef JIT_ENABLED
                if (jit && run == 1)
                    warm = (arm7 ? nds->JIT.JitBlocks7 : nds->JIT.JitBlocks9).contains(addr);
#endif
                cpu.R[0] = sentinel;
                cpu.R[1] = cpu.R[2] = cpu.R[3] = 1;
                const u32 cpsr = 0xDF | (z ? 1u << 30 : 0);
                cpu.CPSR = cpsr;
                cpu.JumpTo(addr);
                cpu.Cycles = 0; // Exclude pipeline refill; measure this instruction only.
                const u32 beforePC = cpu.R[15];
                auto& timestamp = arm7 ? nds->ARM7Timestamp : nds->ARM9Timestamp;
                auto& target = arm7 ? nds->ARM7Target : nds->ARM9Target;
                timestamp = 0;
                target = 1;
#ifdef JIT_ENABLED
                if (jit)
                {
                    if (run == 1)
                    {
                        // Dispatch exactly one already-cached block. A zero-cycle
                        // defect must not run the following instruction to reach Target.
                        if (!warm) return 2;
                        ARM_Dispatch(&cpu, (arm7 ? nds->JIT.JitBlocks7 : nds->JIT.JitBlocks9).at(addr)->EntryPoint);
                        timestamp = cpu.Cycles;
                    }
                    else if (arm7) nds->ARM7.Execute<CPUExecuteMode::JIT>();
                    else nds->ARM9.Execute<CPUExecuteMode::JIT>();
#if defined(__x86_64__)
                    if (z && run == 0)
                    {
                        // First compilation is bounded to this one instruction.
                        // Print actual native bytes for objdump; no instruction model.
                        const auto* entry = reinterpret_cast<const u8*>(
                            (arm7 ? nds->JIT.JitBlocks7 : nds->JIT.JitBlocks9).at(addr)->EntryPoint);
                        const auto* end = nds->JIT.JITCompiler.GetCodePtr();
                        if (end <= entry || end - entry > 1024) return 2;
                        std::printf("native-block ARM%d guest=%08X bytes=", arm7 ? 7 : 9, instr);
                        for (auto* p = entry; p != end; ++p) std::printf("%02X", *p);
                        std::puts("");
                    }
#endif
                }
                else
#endif
                {
                    if (arm7) nds->ARM7.Execute<CPUExecuteMode::Interpreter>();
                    else nds->ARM9.Execute<CPUExecuteMode::Interpreter>();
                }
                const bool taken = instr >> 28 == 14 || (instr >> 28 == 0 ? z : !z);
                const u32 result = taken ? test.result : sentinel;
                const u32 cycles = taken ? test.cycles : 1;
                const bool ok = warm && cpu.R[0] == result && timestamp == cycles &&
                    cpu.R[15] == addr + 8 && cpu.CPSR == cpsr &&
                    cpu.R[1] == 1 && cpu.R[2] == 1 && cpu.R[3] == 1;
                ++checked;
                failures += !ok;
                std::printf("%s ARM%d %08X Z=%u run=%u warm=%d: %s r0=%08X cycles=%llu expected=%08X/%u pc=%08X->%08X\n",
                    jit ? (run == 1 ? "dispatch" : "jit") : "interpreter", arm7 ? 7 : 9,
                    instr, unsigned(z), run, warm, ok ? "PASS" : "FAIL", cpu.R[0],
                    static_cast<unsigned long long>(timestamp), result, cycles, beforePC, cpu.R[15]);
            }
        }
    }
    std::printf("core conditional cycles: %u checks, %u failures\n", checked, failures);
    return failures ? 1 : 0;
}

#ifdef GDBSTUB_ENABLED
static int TestGdbSPSR(NDSArgs&& args)
{
    auto nds = std::make_unique<NDS>(std::move(args));
    constexpr u32 code = 0x02010000, returned = 0x02010100, idle = 0x02010200;
    constexpr u32 initial = 0x200000DF, saved = 0x600000DF;
    const Gdb::Register registers[] = {Gdb::Register::spsr_fiq, Gdb::Register::spsr_irq,
        Gdb::Register::spsr_svc, Gdb::Register::spsr_abt, Gdb::Register::spsr_und};
    constexpr u32 modes[] = {0x11, 0x12, 0x13, 0x17, 0x1B};
    unsigned checks = 0, failures = 0;
    for (bool arm7 : {false, true})
    for (unsigned bank = 0; bank < std::size(registers); ++bank)
    for (bool active : {false, true})
    {
        nds->Reset();
        nds->ARM9Write32(code, 0xE14F2000); // MRS r2,SPSR
        nds->ARM9Write32(code + 4, 0xE1B0F00E); // MOVS pc,lr: exception return
        nds->ARM9Write32(returned, 0xE10F3000); // MRS r3,CPSR
        nds->ARM9Write32(returned + 4, 0xE3A0405A); // MOV r4,#0x5A
        nds->ARM9Write32(returned + 8, 0xEAFFFFFE);
        nds->ARM9Write32(idle, 0xEAFFFFFE);
        nds->ARM9.JumpTo(idle);
        nds->ARM7.JumpTo(idle);
        ARM& cpu = arm7 ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
        u32* banks[] = {&cpu.R_FIQ[7], &cpu.R_IRQ[2], &cpu.R_SVC[2],
                       &cpu.R_ABT[2], &cpu.R_UND[2]};
        for (auto* value : banks) *value = initial;
        const u32 current = 0xA00000C0 | (active ? modes[bank] : 0x1F);
        cpu.UpdateMode(cpu.CPSR, current);
        cpu.CPSR = current;
        const bool read = cpu.ReadReg(registers[bank]) == initial;
        cpu.WriteReg(registers[bank], saved);
        bool passed = read && cpu.CPSR == current;
        for (unsigned other = 0; other < std::size(banks); ++other)
            passed &= *banks[other] == (other == bank ? saved : initial);
        if (passed)
        {
            // An inactive bank edit must be visible when that exception mode runs.
            const u32 handler = 0xA00000C0 | modes[bank];
            cpu.UpdateMode(cpu.CPSR, handler);
            cpu.CPSR = handler;
            cpu.R[14] = returned;
            cpu.R[2] = cpu.R[3] = cpu.R[4] = 0;
            cpu.JumpTo(code);
            nds->Start();
            nds->RunFrame();
            passed = cpu.R[2] == saved && cpu.R[3] == saved &&
                     cpu.R[4] == 0x5A && cpu.CPSR == saved;
        }
        ++checks;
        failures += !passed;
        std::printf("GDB ARM%d SPSR mode=%02x active=%d read=%d CPSR=%08x saved=%08x result=%s\n",
                    arm7 ? 7 : 9, modes[bank], active, read, cpu.CPSR, *banks[bank],
                    passed ? "pass" : "fail");
    }
    std::printf("GDB SPSR: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
#endif

int main(int argc, char** argv) {
    const bool jit = argc > 1 && std::strcmp(argv[1], "interpreter") != 0;
    const bool fast = argc > 1 && std::strcmp(argv[1], "fastmem") == 0;
#if defined(JIT_ENABLED) && defined(__x86_64__)
    if (argc > 2 && std::strcmp(argv[2], "no-lzcnt") == 0) cpu_info.bLZCNT = false;
#endif
#ifndef JIT_ENABLED
    if (jit) return 77;
#endif
    NDSArgs args;
    if (!jit) args.JIT = std::nullopt;
    else args.JIT->FastMemory = fast;
#ifdef GDBSTUB_ENABLED
    if (argc > 2 && std::strcmp(argv[2], "gdb-spsr") == 0)
        return TestGdbSPSR(std::move(args));
#endif
    if (argc > 2 && std::strcmp(argv[2], "unaligned-memory") == 0)
        return TestUnalignedMemory(std::move(args), jit);
    if (argc > 2 && std::strcmp(argv[2], "thumb-push-timing") == 0)
        return TestThumbPushTiming(std::move(args), jit);
    if (argc > 2 && std::strcmp(argv[2], "thumb-stack") == 0)
        return TestThumbStack(std::move(args), jit);
    if (argc > 2 && std::strcmp(argv[2], "conditional-cycles") == 0)
        return TestConditionalCycles(std::move(args), jit);
    if (argc > 2 && std::strcmp(argv[2], "alu-shift") == 0)
        return TestALUExecution(std::move(args), jit);
    if (argc > 2 && std::strcmp(argv[2], "thumb-shift-timing") == 0)
        return TestThumbShiftTiming(std::move(args), jit);
    if (argc > 2 && std::strcmp(argv[2], "block-transfer") == 0)
        return TestBlockTransferExecution(std::move(args), jit);
    if (argc > 2 && std::strcmp(argv[2], "dtcm-remap") == 0)
        return TestDTCMExecution(std::move(args), jit);
    if (argc > 2 && std::strcmp(argv[2], "mpu-execution") == 0)
        return TestMPUExecution(std::move(args), jit);
    if (argc > 2 && std::strcmp(argv[2], "device-execution") == 0)
        return TestDeviceExecution(std::move(args), jit);
    if (argc > 2 && std::strcmp(argv[2], "savestate-scheduler") == 0)
        return TestSchedulerSavestate(std::move(args));
    if (argc > 2 && std::strcmp(argv[2], "dsi-ndma") == 0)
        return TestDSiNDMAExecution(std::move(args));
    if (argc > 2 && std::strcmp(argv[2], "dsi-reset-i2c") == 0)
        return TestDSiResetI2C(std::move(args));
    if (argc > 2 && std::strcmp(argv[2], "dsi-btdmp") == 0)
        return TestDSiBTDMP(std::move(args));
    if (argc > 2 && std::strcmp(argv[2], "scheduler-execution") == 0)
        return TestSchedulerExecution(std::move(args));
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    RendererSettings settings{1, false, false, false};
    nds->GetRenderer().SetRenderSettings(settings);
    // r0=100 after a counted loop; store it then stay in a self-loop.
    constexpr u32 code[] = {
        0xE3A00000, // mov r0,#0
        0xE3A01402, // mov r1,#0x02000000
        0xE3A02064, // mov r2,#100
        0xE2800001, // add r0,r0,#1
        0xE2522001, // subs r2,r2,#1
        0x1AFFFFFC, // bne back to add
        0xE5810100, // str r0,[r1,#0x100]
        0xE16F3F10, // clz r3,r0 -> 25
        0xE5813104, // str r3,[r1,#0x104]
        0xE3A04000, // mov r4,#0
        0xE16F5F14, // clz r5,r4 -> 32
        0xE5815108, // str r5,[r1,#0x108]
        0xE3A06102, // mov r6,#0x80000000
        0xE16F7F16, // clz r7,r6 -> 0
        0xE581710C, // str r7,[r1,#0x10c]
        0xEAFFFFFE  // b self
    };
    for (unsigned i=0;i<std::size(code);++i) nds->ARM9Write32(0x02000000+4*i,code[i]);
    nds->ARM9Write32(0x02000200,0xEAFFFFFE);
    nds->ARM9.JumpTo(0x02000000);
    nds->ARM7.JumpTo(0x02000200);
    // Exercise the software output conversion with a constant RGB555 VRAM line.
    nds->ARM9Write16(0x04000304, 0x0203); // LCD, 2D A/B powered; default screen mapping
    nds->ARM9Write8(0x04000240, 0x80);    // VRAM A -> LCDC
    nds->ARM9Write32(0x04000000, 0x00020000); // Main display reads VRAM A
    for (unsigned i=0;i<256*192;++i) nds->ARM9Write16(0x06800000+2*i,0x001F);
    nds->Start();
    const auto lines = nds->RunFrame();
    if (!nds->IsRunning() || nds->ARM9Read32(0x02000100)!=100) {
        std::fprintf(stderr,"execution failed: lines=%u result=%u r0=%u r1=%08x pc=%08x\n",lines,
            nds->ARM9Read32(0x02000100),nds->ARM9.R[0],nds->ARM9.R[1],nds->ARM9.R[15]);
        return 1;
    }
    if (nds->ARM9Read32(0x02000104) != 25 || nds->ARM9Read32(0x02000108) != 32 ||
        nds->ARM9Read32(0x0200010C) != 0) return 8;
    Savestate saved;
    if (!nds->DoSavestate(&saved)) return 2;
    saved.Finish();
    if (saved.Error) return 3;
    nds->ARM9Write32(0x02000100,0xDEADBEEF);
    Savestate restore(saved.Buffer(),saved.Length(),false);
    if (!nds->DoSavestate(&restore) || restore.Error || nds->ARM9Read32(0x02000100)!=100) return 4;
    nds->RunFrame();
    if (!nds->IsRunning() || nds->ARM9Read32(0x02000100)!=100) return 5;
    void *top=nullptr, *bottom=nullptr;
    if (!nds->GetRenderer().GetFramebuffers(&top,&bottom) || !top || !bottom) return 6;
    const u32* screen = static_cast<const u32*>(bottom);
    // 5-bit 31 is doubled to 6-bit 62, then replicated to 8-bit 251, not 255.
    for (unsigned i=0;i<256*192;++i) {
        if (screen[i]!=0xFFFB0000) {
            std::fprintf(stderr,"framebuffer mismatch at %u: %08x\n",i,screen[i]);
            return 7;
        }
    }
#ifdef JIT_ENABLED
    if (jit) {
        // Two independent loops share a 512-byte index range, but not a
        // 16-byte code granule. Modifying one must preserve the other.
        constexpr u32 first = 0x02000400, second = 0x02000480;
        nds->ARM9Write32(first, 0xEAFFFFFE);
        nds->ARM9Write32(second, 0xEAFFFFFE);
        nds->JIT.JitEnableWrite();
        nds->ARM9.JumpTo(first);
        nds->JIT.CompileBlock(&nds->ARM9);
        nds->ARM9.JumpTo(second);
        nds->JIT.CompileBlock(&nds->ARM9);
        nds->JIT.JitEnableExecute();
        if (!nds->JIT.JitBlocks9.contains(first) || !nds->JIT.JitBlocks9.contains(second)) return 9;
        nds->ARM9Write32(first, 0xE3A0002A); // mov r0,#42
        if (nds->JIT.JitBlocks9.contains(first) || !nds->JIT.JitBlocks9.contains(second)) {
            std::fprintf(stderr, "JIT invalidation removed an unrelated code granule\n");
            return 10;
        }
        // The retained block still has to be protected from later writes.
        nds->ARM9Write32(second, 0xE3A00007);
        if (nds->JIT.JitBlocks9.contains(second)) return 11;
    }
#endif
    // Exercise the shipped replacement BIOS through its real SWI entry point.
    // sqrt is unsigned even when bit 31 is set.
    // Isolate BIOS execution from the preceding cache/savestate scenarios.
    // Reusing the same guest addresses on ARM9 then ARM7 also checks that a
    // retired JIT block can never be restored for the other CPU.
    nds->Reset();
    nds->ARM9Write32(0x02000200, 0xEAFFFFFE);
    nds->ARM9.JumpTo(0x02000200);
    nds->ARM7.JumpTo(0x02000200);
    nds->Start();
    constexpr u32 sqrtCode[] = {
        0xE59F000C, // ldr r0,[pc,#12] -> input at +20
        0xEF0D0000, // swi 0x0D0000
        0xE59F1008, // ldr r1,[pc,#8] -> result address at +24
        0xE5810000, // str r0,[r1]
        0xEAFFFFFE,
        0, 0x02000110
    };
    constexpr u32 sqrtCases[] = {0, 1, 4, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF};
    for (bool arm7 : {false, true})
    {
        auto& cpu = arm7 ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
        cpu.CPSR = 0xDF; // System mode, interrupts disabled; stacks normally set by boot.
        cpu.R[13] = 0x02002000;
        cpu.R_SVC[0] = 0x02003000;
        for (u32 value : sqrtCases)
        {
            for (unsigned i = 0; i < std::size(sqrtCode); ++i)
                nds->ARM9Write32(0x02000600 + i * 4, sqrtCode[i]);
            nds->ARM9Write32(0x02000614, value);
            nds->ARM9Write32(0x02000110, 0xDEADBEEF);
            cpu.JumpTo(0x02000600);
            nds->RunFrame();
            const u32 result = nds->ARM9Read32(0x02000110);
            if (u64(result) * result > value || u64(result + 1) * (result + 1) <= value)
            {
                std::fprintf(stderr, "ARM%d FreeBIOS sqrt(%08x) = %u\n", arm7 ? 7 : 9, value, result);
                return 12;
            }
        }
        cpu.JumpTo(0x02000200); // stop this CPU before checking the other
    }
    // Re-enter already compiled CLZ instructions with every leading-zero count,
    // including zero and aliased source/destination. Guest flags must survive.
    nds->Reset();
    nds->ARM9Write32(0x02000200, 0xEAFFFFFE);
    nds->ARM7.JumpTo(0x02000200);
    nds->ARM9Write32(0x02000800, 0xE16F2F10); // clz r2,r0
    nds->ARM9Write32(0x02000804, 0xE16F0F10); // clz r0,r0
    nds->ARM9Write32(0x02000808, 0xEAFFFFFE);
    nds->Start();
    for (unsigned run = 0; run < 2; ++run)
    for (unsigned zeros = 0; zeros <= 32; ++zeros)
    {
        nds->ARM9.R[0] = zeros == 32 ? 0 : 0xFFFFFFFFu >> zeros;
        nds->ARM9.CPSR = 0xA00000DF;
        nds->ARM9.JumpTo(0x02000800);
        nds->RunFrame();
        if (nds->ARM9.R[0] != zeros || nds->ARM9.R[2] != zeros ||
            nds->ARM9.CPSR != 0xA00000DF)
        {
            std::fprintf(stderr, "CLZ execution/flags mismatch: zeros=%u run=%u\n", zeros, run);
            return 15;
        }
    }
    // Branch following includes the self-branch twice. These different traces
    // have the same low 32 bits of XXH3_64bits (0x846DC331). Replacing code must
    // not restore the first program's JIT block.
    nds->Reset();
    nds->ARM9Write32(0x02000200, 0xEAFFFFFE);
    nds->ARM7.JumpTo(0x02000200);
    nds->Start();
    constexpr u32 collisionCode[][3] = {
        {0xE3A00EB9, 0xE3A01251, 0xEAFFFFFE}, // mov r0,#0xB90; mov r1,#0x10000005; b .
        {0xE3A000C1, 0xE3A0169B, 0xEAFFFFFE}  // mov r0,#0xC1; mov r1,#0x9B00000; b .
    };
    constexpr u32 collisionResults[][2] = {{0xB90, 0x10000005}, {0xC1, 0x9B00000}};
    for (unsigned version = 0; version < std::size(collisionCode); ++version)
    {
        for (unsigned i = 0; i < std::size(collisionCode[version]); ++i)
            nds->ARM9Write32(0x02000800 + 4 * i, collisionCode[version][i]);
        // CompileBlock interprets instructions while compiling. Enter again to
        // check the generated code rather than only that initial execution.
        for (unsigned run = 0; run < 2; ++run)
        {
            nds->ARM9.R[0] = nds->ARM9.R[1] = 0;
            nds->ARM9.JumpTo(0x02000800);
            nds->RunFrame();
            if (nds->ARM9.R[0] != collisionResults[version][0] ||
                nds->ARM9.R[1] != collisionResults[version][1])
            {
                std::fprintf(stderr, "JIT hash collision: program=%u run=%u r0=%08x r1=%08x\n",
                    version, run, nds->ARM9.R[0], nds->ARM9.R[1]);
                return 13;
            }
        }
    }
    // The same bytes are valid in both instruction sets: ARM sets r2 to 7;
    // Thumb sets r0 to 7 and branches to a separate Thumb self-loop.
    nds->Reset();
    nds->ARM9Write32(0x02000200, 0xEAFFFFFE);
    nds->ARM9Write32(0x02000804, 0xEAFFFFFE);
    nds->ARM9Write16(0x02000F46, 0xE7FE);
    nds->ARM9.JumpTo(0x02000200);
    nds->ARM7.JumpTo(0x02000200);
    nds->Start();
    for (u32 value : {7u, 9u})
    {
        // Invalidate both instruction sets, on both CPUs, with the same write.
        nds->ARM9Write32(0x02000800, 0xE3A02000 | value);
        for (bool arm7 : {false, true})
        {
            auto& cpu = arm7 ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
            for (bool thumb : {false, true, false, true})
            {
                cpu.R[0] = cpu.R[2] = 0;
                cpu.JumpTo(0x02000800 | u32(thumb));
                nds->RunFrame();
                if (cpu.R[0] != (thumb ? value : 0u) || cpu.R[2] != (thumb ? 0u : value))
                {
                    std::fprintf(stderr, "ARM%d instruction-set cache mismatch: thumb=%d r0=%08x r2=%08x\n",
                        arm7 ? 7 : 9, thumb, cpu.R[0], cpu.R[2]);
                    return 14;
                }
            }
            cpu.JumpTo(0x02000200);
        }
    }
    // CLZ and MOV both consume a code cycle. After warming each loop, they
    // must advance the same number of iterations within a scheduled frame.
    u32 iterations[2]{};
    for (unsigned clz = 0; clz < 2; ++clz)
    {
        nds->Reset();
        constexpr u32 loop[] = {0xE2800001, 0xE1A02001, 0xE2533001, 0x1AFFFFFB, 0xEAFFFFFE};
        for (unsigned i = 0; i < std::size(loop); ++i)
            nds->ARM9Write32(0x02000800 + 4 * i, loop[i]);
        if (clz) nds->ARM9Write32(0x02000804, 0xE16F2F11);
        nds->ARM9Write32(0x02000200, 0xEAFFFFFE);
        nds->ARM9.R[0] = 0;
        nds->ARM9.R[1] = 0x12345678;
        nds->ARM9.R[3] = 0x01000000;
        nds->ARM9.JumpTo(0x02000800);
        nds->ARM7.JumpTo(0x02000200);
        nds->Start();
        nds->RunFrame();
        const u32 before = nds->ARM9.R[0];
        nds->RunFrame();
        iterations[clz] = nds->ARM9.R[0] - before;
    }
    if (iterations[0] != iterations[1])
    {
        std::fprintf(stderr, "CLZ guest timing differs from MOV: %u vs %u iterations/frame\n",
                     iterations[1], iterations[0]);
        return 16;
    }
    std::printf("core=%s fastmem=%d lines=%u ARM-result=100 save-restore=PASS pixels=49152 CLZ-timing=%u\n",
                jit?"JIT":"interpreter",fast,lines,iterations[1]);
}
