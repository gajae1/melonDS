// SPDX-License-Identifier: GPL-3.0-or-later
// Actual ARM9 guest execution; generated RAM programs and exception vectors.
// ARM DDI 0201D 2.3.8/4.2.3: instruction AP=0 denies, AP=3 permits access.
// ARM DDI 0100I A2.6.5: Prefetch Abort saves fault address+4 and the old CPSR.
// https://documentation-service.arm.com/static/5e8e3ee588295d1e18d3aa82
// https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a
#include "Args.h"
#include "ARM.h"
#include "NDS.h"
#include "Savestate.h"

#include <cstdio>
#include <memory>
#include <utility>

using namespace melonDS;

namespace
{
constexpr u32 Driver = 0x02000000;
constexpr u32 DynamicDriver = Driver + 0x100;
constexpr u32 Idle = Driver + 0x200;
constexpr u32 Target = 0x02004000;
constexpr u32 Data = 0x02008000;
constexpr u32 Vectors = 0xFFFF0000;
constexpr u32 Permit = 0x00000333; // Regions 0/1/2: vectors/driver/target.
constexpr u32 DenyTarget = 0x00000033;
constexpr u32 InitialCPSR = 0xA800005F; // N,C,Q,F; System/ARM, IRQ enabled.
constexpr u32 AbortCPSR = 0xA80000D7; // Same flags/F; Abort/ARM, IRQ disabled.
constexpr u32 Sentinel = 0xDEADC0DE;
constexpr u32 Stored = 0x13579BDF;
constexpr u32 LinkSentinel = 0x12345678;
constexpr u32 SPSRSentinel = 0x11223344;

struct MPUCase
{
    const char* Name;
    u32 Entry;
    u32 Permissions;
    bool Abort;
    bool Warm;
};

constexpr MPUCase Cases[] = {
    {"allowed-cold", Driver, Permit, false, false},
    {"allowed-warm", Driver, Permit, false, true},
    {"revoked-warm-static", Driver, DenyTarget, true, true},
    // This cold dynamic branch uses the existing JumpTo permission check.
    {"denied-dynamic-control", DynamicDriver, DenyTarget, true, false},
    {"restored-warm-static", Driver, Permit, false, true},
};
}

int TestMPUExecution(NDSArgs&& args, bool jit)
{
    // Replace the input image with generated B . vectors, not a user BIOS.
    args.ARM9BIOS = std::make_unique<ARM9BIOSImage>();
    for (size_t i = 0; i < args.ARM9BIOS->size(); i += 4)
    {
        (*args.ARM9BIOS)[i] = 0xFE;
        (*args.ARM9BIOS)[i + 1] = 0xFF;
        (*args.ARM9BIOS)[i + 2] = 0xFF;
        (*args.ARM9BIOS)[i + 3] = 0xEA;
    }

    auto nds = std::make_unique<NDS>(std::move(args));
    nds->CurCPU = 0;
    nds->Reset();
    auto& cpu = nds->ARM9;

    // MCR p15,0,r0,c5,c0,3; B Target. The target is a different MPU page.
    nds->ARM9Write32(Driver, 0xEE050F70);
    nds->ARM9Write32(Driver + 4, 0xEA000FFD);
    // The identical permission update followed by BX r4 is the control.
    nds->ARM9Write32(DynamicDriver, 0xEE050F70);
    nds->ARM9Write32(DynamicDriver + 4, 0xE12FFF14);
    nds->ARM9Write32(Idle, 0xEAFFFFFE);
    nds->ARM9Write32(Target, 0xE3A06066);     // MOV r6,#0x66
    nds->ARM9Write32(Target + 4, 0xE5887000); // STR r7,[r8], no writeback
    nds->ARM9Write32(Target + 8, 0xEAFFFFFE); // B .

    // Four disjoint 4 KiB regions, caches/TCM disabled, high vectors enabled.
    // No region overlaps or cache invalidation are needed for this contract.
    cpu.CP15Write(0x600, Vectors | 0x17);
    cpu.CP15Write(0x610, Driver | 0x17);
    cpu.CP15Write(0x620, Target | 0x17);
    cpu.CP15Write(0x630, Data | 0x17);
    cpu.CP15Write(0x502, 0x00003333);
    cpu.CP15Write(0x503, Permit);
    cpu.CP15Write(0x100, 0x00002001);
    nds->ARM7.JumpTo(Idle);
    nds->Start();

    unsigned failures = 0;
    for (const auto& test : Cases)
    {
        bool warm = true;
#ifdef JIT_ENABLED
        if (jit && test.Warm)
            warm = nds->JIT.JitBlocks9.contains(test.Entry);
#endif
        // Keep compiled code and program bytes across all entries. Only the
        // guest's MCR changes the target permission after initial setup.
        const u32 oldCPSR = cpu.CPSR;
        cpu.CPSR = InitialCPSR;
        cpu.UpdateMode(oldCPSR, cpu.CPSR);
        cpu.StopExecution = 0;
        cpu.Cycles = 0;
        cpu.R[0] = test.Permissions;
        cpu.R[4] = Target;
        cpu.R[6] = Sentinel;
        cpu.R[7] = Stored;
        cpu.R[8] = Data;
        cpu.R[14] = LinkSentinel;
        cpu.R_ABT[2] = SPSRSentinel;
        nds->ARM9Write32(Data, Sentinel);
        cpu.JumpTo(test.Entry);
        nds->RunFrame();

        // The B . at the exception vector keeps the next-instruction address
        // observable: this core's between-instruction R15 is address+4 in ARM.
        const u32 expectedPC = test.Abort ? Vectors + 0x0C : Target + 8;
        const u32 expectedLR = test.Abort ? Target + 4 : LinkSentinel;
        const u32 expectedCPSR = test.Abort ? AbortCPSR : InitialCPSR;
        const u32 expectedSPSR = test.Abort ? InitialCPSR : SPSRSentinel;
        const u32 expectedRegister = test.Abort ? Sentinel : 0x66;
        const u32 expectedMemory = test.Abort ? Sentinel : Stored;
        const u32 memory = nds->ARM9Read32(Data);
        const bool ok = warm && nds->IsRunning() &&
            cpu.CP15Read(0x503) == test.Permissions &&
            cpu.R[15] == expectedPC + 4 && cpu.R[14] == expectedLR &&
            cpu.CPSR == expectedCPSR && cpu.R_ABT[2] == expectedSPSR &&
            cpu.R[6] == expectedRegister && memory == expectedMemory &&
            cpu.R[4] == Target && cpu.R[7] == Stored && cpu.R[8] == Data;

        std::printf("mpu/%s/%s: %s\n", jit ? "jit" : "interpreter",
                    test.Name, ok ? "PASS" : "FAIL");
        if (!ok)
        {
            ++failures;
            std::fprintf(stderr,
                "warm=%d running=%d AP=%08x/%08x PC=%08x/%08x LR=%08x/%08x "
                "CPSR=%08x/%08x SPSR=%08x/%08x r6=%08x/%08x memory=%08x/%08x\n",
                warm, nds->IsRunning(), cpu.CP15Read(0x503), test.Permissions,
                cpu.R[15], expectedPC + 4, cpu.R[14], expectedLR,
                cpu.CPSR, expectedCPSR, cpu.R_ABT[2], expectedSPSR,
                cpu.R[6], expectedRegister, memory, expectedMemory);
        }
    }

    std::printf("MPU execution: %u/5 cases passed\n", 5 - failures);
    return failures ? 1 : 0;
}

// ARM DDI 0201D 4.1.1: with the MPU disabled, all instruction/data
// accesses are noncacheable, regardless of the I/C enable bits.
// Compare actual guest execution with the uncached control, without
// asserting new absolute hardware timings. Setup/refill cycles are excluded.
int TestCacheMPUDisabled(NDSArgs&& args, bool jit)
{
    if (args.JIT)
    {
        args.JIT->MaxBlockSize = 1;
        args.JIT->BranchOptimizations = false;
        args.JIT->LiteralOptimizations = false;
    }
    auto nds = std::make_unique<NDS>(std::move(args));
    NDS::Current = nds.get(); // Same context as RunFrame, needed by JIT callbacks.
    auto& cpu = nds->ARM9;
    constexpr u32 boot = 0x100, ramCode = 0x02008000, ramData = 0x02030000;
    constexpr u32 dtcm = 0x03000000, tcmControl = (1u << 18) | (1u << 16);
    constexpr u32 initial = 0x2468ACE0, stored = 0x13579BDF, sentinel = 0xDEADC0DE;
    constexpr u32 cpsr = 0xA80000DF;
    const struct { const char* name; u32 control; bool disable; } configs[] = {
        {"uncached-control", 0, false},
        {"mpu-off-i", 0x1000, false},
        {"mpu-off-d", 0x0004, false},
        {"mpu-off-id", 0x1004, false},
        {"mpu-on-id-control", 0x1005, false},
        {"disable-mpu-id", 0x1004, true},
    };
    const struct { const char* name; u32 code, data, instr, result, memory; bool external; } probes[] = {
        {"fetch-ram", ramCode, ramData, 0xE3A0005A, 0x5A, initial, true}, // MOV r0,#0x5A
        {"load-ram", 0x1000, ramData, 0xE5910000, initial, initial, true}, // LDR r0,[r1]
        {"store-ram", 0x1020, ramData, 0xE5812000, sentinel, stored, true}, // STR r2,[r1]
        {"fetch-load-ram", ramCode + 0x20, ramData, 0xE5910000, initial, initial, true},
        {"itcm-dtcm-control", 0x1040, dtcm, 0xE5910000, initial, initial, false},
    };
    u64 reference[std::size(probes)][2] = {};
    unsigned checked = 0, failures = 0;
    for (unsigned config = 0; config < std::size(configs); ++config)
    {
        const auto& setting = configs[config];
        nds->Reset();
        nds->CurCPU = 0;
        // A valid 4 GiB RWX region is required before the MPU-on control.
        cpu.CP15Write(0x600, 0x3F);
        cpu.CP15Write(0x502, 3);
        cpu.CP15Write(0x503, 3);
        cpu.CP15Write(0x200, 1);
        cpu.CP15Write(0x201, 1);
        cpu.CP15Write(0x300, 1);
        cpu.CP15Write(0x911, 0x0C); // 32 KiB ITCM
        cpu.CP15Write(0x910, dtcm | 0x0A); // 16 KiB DTCM
        cpu.CP15Write(0x100, tcmControl);
        cpu.DataWrite32(boot, 0xEE010F10); // MCR p15,0,r0,c1,c0,0
        cpu.DataWrite32(boot + 4, 0xEAFFFFFE);
        cpu.DataWrite32(boot + 8, 0xE1A00000);
        auto setControl = [&](u32 value) {
            cpu.CPSR = cpsr;
            cpu.R[0] = value;
            cpu.StopExecution = 0;
            cpu.JumpTo(boot);
            cpu.Cycles = 0;
            nds->ARM9Timestamp = 0;
            nds->ARM9Target = 1;
#ifdef JIT_ENABLED
            if (jit) cpu.Execute<CPUExecuteMode::JIT>();
            else
#endif
                cpu.Execute<CPUExecuteMode::Interpreter>();
            return (cpu.CP15Read(0x100) & 0x000FF085) == value && cpu.R[15] == boot + 8;
        };
        if (setting.disable && !setControl(tcmControl | 0x1005)) return 2;
        if (!setControl(tcmControl | setting.control)) return 2;
        for (unsigned p = 0; p < std::size(probes); ++p)
        {
            const auto& probe = probes[p];
            cpu.DataWrite32(probe.code, probe.instr);
            cpu.DataWrite32(probe.code + 4, 0xEAFFFFFE);
            cpu.DataWrite32(probe.code + 8, 0xE1A00000);
            for (unsigned run = 0; run < 2; ++run)
            {
                cpu.DataWrite32(probe.data, initial);
                cpu.R[0] = sentinel;
                cpu.R[1] = probe.data;
                cpu.R[2] = stored;
                cpu.CPSR = cpsr;
                cpu.StopExecution = 0;
                cpu.JumpTo(probe.code); // Refresh fetch region after guest MCR.
                cpu.Cycles = 0;
                nds->ARM9Timestamp = 0;
                nds->ARM9Target = 1;
#ifdef JIT_ENABLED
                if (jit)
                {
                    if (run == 0) cpu.Execute<CPUExecuteMode::JIT>();
                    else
                    {
                        if (!nds->JIT.JitBlocks9.contains(probe.code)) return 2;
                        ARM_Dispatch(&cpu, nds->JIT.JitBlocks9.at(probe.code)->EntryPoint);
                        nds->ARM9Timestamp = cpu.Cycles;
                    }
                }
                else
#endif
                    cpu.Execute<CPUExecuteMode::Interpreter>();
                const u64 cycles = nds->ARM9Timestamp;
                if (config == 0) reference[p][run] = cycles;
                // The enabled-cache control must distinguish the external bus.
                const bool cached = (setting.control & 1) && probe.external;
                const bool timing = cached ? cycles < reference[p][run] : cycles == reference[p][run];
                u32 memory = 0;
                cpu.DataRead32(probe.data, &memory);
                const bool state = cpu.R[0] == probe.result && memory == probe.memory &&
                    cpu.R[1] == probe.data && cpu.R[2] == stored &&
                    cpu.R[15] == probe.code + 8 && cpu.CPSR == cpsr;
                const bool ok = state && timing && cycles > 0;
                ++checked;
                failures += !ok;
                std::printf("{\"config\":\"%s\",\"probe\":\"%s\",\"execution\":\"%s\","
                    "\"run\":%u,\"control\":%u,\"cycles\":%llu,\"uncached_cycles\":%llu,"
                    "\"r0\":%u,\"memory\":%u,\"pc\":%u,\"cpsr\":%u,\"state_ok\":%s,\"ok\":%s}\n",
                    setting.name, probe.name, jit ? (run ? "jit-warm-dispatch" : "jit-cold") : "interpreter",
                    run, cpu.CP15Read(0x100), cycles, reference[p][run], cpu.R[0], memory,
                    cpu.R[15], cpu.CPSR, state ? "true" : "false", ok ? "true" : "false");
            }
        }
    }
    std::printf("{\"summary\":true,\"checked\":%u,\"failures\":%u}\n", checked, failures);
    return failures ? 1 : 0;
}

// ARM946E-S TRM 2.2.1: a denied single transfer restores the base and
// preserves the destination. Exercise real tracing and reused native blocks.
// ARM DDI0100I A4-213/215: SWP must preserve Rd on either access fault and
// suppress the store after a read fault, even if Abort mode could write there.
int TestMPUDataAbort(NDSArgs&& args, bool jit)
{
    if (args.JIT) {
        args.JIT->MaxBlockSize = 4;
        args.JIT->BranchOptimizations = false;
        args.JIT->LiteralOptimizations = true;
    }
    args.ARM9BIOS = std::make_unique<ARM9BIOSImage>();
    for (size_t i = 0; i < args.ARM9BIOS->size(); i += 4) {
        (*args.ARM9BIOS)[i] = 0xFE;
        (*args.ARM9BIOS)[i+1] = 0xFF;
        (*args.ARM9BIOS)[i+2] = 0xFF;
        (*args.ARM9BIOS)[i+3] = 0xEA;
    }
    // SUBS pc,lr,#8: retry the faulting instruction after permissions recover.
    const u8 abortReturn[] = {0x08, 0xF0, 0x5E, 0xE2};
    std::copy(std::begin(abortReturn), std::end(abortReturn), args.ARM9BIOS->begin()+0x10);
    auto nds = std::make_unique<NDS>(std::move(args));
    NDS::Current = nds.get();
    auto& cpu = nds->ARM9;
    constexpr u32 code = Driver+0xC00, data = Driver+0x1000;
    constexpr u32 flags = 0x6800005F; // ADDS r3,#1: FFFFFFFF+1 => Z,C; preserve Q,F
    struct Probe { const char* name; u32 op; bool thumb, store; u32 bits; bool pre, wb; bool user = false, skip = false; u32 deniedAP = 0; bool swap = false; };
    const Probe probes[] = {
        {"ldr-post", 0xE4910004, false, false, 32, false, true},
        {"ldr-user", 0xE4910004, false, false, 32, false, true, true, false, 1},
        {"str-readonly", 0xE4810004, false, true, 32, false, true, false, false, 5},
        {"ldr-ne-skipped", 0x14910004, false, false, 32, false, true, false, true},
        {"ldr-pre", 0xE5B10004, false, false, 32, true, true},
        {"str-post", 0xE4810004, false, true, 32, false, true},
        {"str-pre", 0xE5A10004, false, true, 32, true, true},
        {"ldrb-post", 0xE4D10004, false, false, 8, false, true},
        {"strb-post", 0xE4C10004, false, true, 8, false, true},
        {"ldrh-pre", 0xE1F100B4, false, false, 16, true, true},
        {"strh-pre", 0xE1E100B4, false, true, 16, true, true},
        {"ldrsb-post", 0xE0D100D4, false, false, 9, false, true},
        {"ldrsh-post", 0xE0D100F4, false, false, 17, false, true},
        {"ldr-literal", 0xE59F03F0, false, false, 32, false, false},
        {"thumb-ldr", 0x6808, true, false, 32, false, false},
        {"thumb-str", 0x6008, true, true, 32, false, false},
        {"thumb-ldrsb", 0x5708, true, false, 9, false, false},
        {"thumb-ldrsh", 0x5F08, true, false, 17, false, false},
        {"thumb-strh", 0x8008, true, true, 16, false, false},
        {"thumb-literal", 0x48FE, true, false, 32, false, false},
        {"swp-denied", 0xE1010092, false, true, 32, false, false, false, false, 0, true},
        {"swp-readonly", 0xE1010092, false, true, 32, false, false, false, false, 5, true},
        {"swp-user-noaccess", 0xE1010092, false, true, 32, false, false, true, false, 1, true},
        {"swpb-denied", 0xE1410092, false, true, 8, false, false, false, false, 0, true},
        {"swpb-user-readonly", 0xE1410092, false, true, 8, false, false, true, false, 2, true},
        {"swp-ne-skipped", 0x11010092, false, true, 32, false, false, false, true, 0, true},
    };
    unsigned failures = 0, checked = 0;
    for (const auto& probe : probes)
    {
        nds->Reset();
        nds->CurCPU = 0;
        const unsigned width = probe.thumb ? 2 : 4;
        if (probe.thumb) {
            nds->ARM9Write16(code, 0x4610); // MOV r0,r2: dirty destination before load
            nds->ARM9Write16(code+2, 0x3301); // ADDS r3,#1
            nds->ARM9Write16(code+4, probe.op);
            nds->ARM9Write16(code+6, 0x2601); // MOVS r6,#1: must not run after fault
            nds->ARM9Write16(code+8, 0xE7FE);
        } else {
            nds->ARM9Write32(code, 0xE1A00002); // MOV r0,r2
            nds->ARM9Write32(code+4, 0xE2933001); // ADDS r3,r3,#1
            nds->ARM9Write32(code+8, probe.op);
            nds->ARM9Write32(code+12, 0xE3B06001); // MOVS r6,#1
            nds->ARM9Write32(code+16, 0xEAFFFFFE);
        }
        cpu.CP15Write(0x600, Vectors | 0x17);
        cpu.CP15Write(0x610, Driver | 0x17);
        cpu.CP15Write(0x620, data | 0x17);
        cpu.CP15Write(0x503, 0x33);
        cpu.CP15Write(0x502, 0x333);
        cpu.CP15Write(0x100, 0x2001);
        // Cold denial, permission restored for training, warmed denial, recovery.
        for (unsigned phase = 0; phase < 4; ++phase)
        {
            const bool deny = !(phase & 1);
            bool cached = false;
#ifdef JIT_ENABLED
            cached = jit && nds->JIT.JitBlocks9.contains(code | unsigned(probe.thumb));
#endif
            const u32 old = cpu.CPSR;
            const u32 modeFlags = (flags & ~0x1Fu) | (probe.user ? 0x10 : 0x1F);
            cpu.CPSR = (InitialCPSR & ~0x1Fu) | (probe.user ? 0x10 : 0x1F) | (probe.thumb ? 0x20 : 0);
            cpu.UpdateMode(old, cpu.CPSR);
            cpu.CP15Write(0x502, deny ? 0x33 | (probe.deniedAP << 8) : 0x333);
            // Store-side permission faults must not affect the target bytes.
            // Do not invalidate a tracked literal by rewriting identical bytes
            // between warm phases; the revoked load must use its cached block.
            if (phase == 0 || probe.store) nds->ARM9Write32(data, Stored);
            cpu.R[0] = 0x1234;
            cpu.R[1] = data - (probe.pre ? 4 : 0);
            const u32 base = cpu.R[1];
            cpu.R[2] = Sentinel;
            cpu.R[3] = 0xFFFFFFFF;
            cpu.R[4] = 0; // Thumb register-offset probes
            cpu.R[6] = 0x9999;
            cpu.R_ABT[2] = SPSRSentinel;
            cpu.StopExecution = 0;
            cpu.JumpTo(code | unsigned(probe.thumb));
            cpu.Cycles = 0;
            nds->ARM9Timestamp = 0;
            for (unsigned step = 0; step < 4; ++step)
            {
                nds->ARM9Target = nds->ARM9Timestamp + 1;
#ifdef JIT_ENABLED
                if (jit) cpu.Execute<CPUExecuteMode::JIT>();
                else
#endif
                    cpu.Execute<CPUExecuteMode::Interpreter>();
                if ((cpu.CPSR & 0x1F) == 0x17 || cpu.R[6] == 1) break;
            }
            u32 loaded = Stored;
            if (probe.bits == 8) loaded &= 0xFF;
            if (probe.bits == 16) loaded &= 0xFFFF;
            if (probe.bits == 9) loaded = u32(s32(s8(Stored)));
            if (probe.bits == 17) loaded = u32(s32(s16(Stored)));
            const u32 mask = probe.bits == 8 ? 0xFF : probe.bits == 16 ? 0xFFFF : ~0u;
            const u32 expectedMemory = !deny && probe.store && !probe.skip ? (Stored & ~mask) | (Sentinel & mask) : Stored;
            const u32 expectedValue = deny || (probe.store && !probe.swap) || probe.skip ? Sentinel : loaded;
            const u32 expectedBase = !deny && probe.wb && !probe.skip ? base + 4 : base;
            bool ok = cpu.R[0] == expectedValue && cpu.R[1] == expectedBase &&
                cpu.R[3] == 0 && nds->ARM9Read32(data) == expectedMemory &&
                (!jit || phase < 2 || cached) &&
                (!jit || phase != 1 || probe.skip || !cached);
            if (deny && !probe.skip) {
                ok &= cpu.R[15] == Vectors+0x14 && cpu.R[14] == code+2*width+8 &&
                    cpu.CPSR == ((modeFlags & ~0xBFu) | 0x97) &&
                    cpu.R_ABT[2] == (modeFlags | (probe.thumb ? 0x20 : 0)) && cpu.R[6] == 0x9999;
            } else ok &= cpu.R[6] == 1 && (cpu.CPSR & 0x3F) == ((probe.user ? 0x10u : 0x1Fu) | (probe.thumb ? 0x20u : 0u));
            if (&probe == &probes[0] && phase == 0) {
                Savestate saved(0x20000);
                cpu.DoSavestate(&saved);
                saved.Finish();
                cpu.R[0] = 0;
                cpu.CP15Write(0x502, 0x333);
                Savestate restored(saved.Buffer(), saved.Length(), false);
                cpu.DoSavestate(&restored);
                ok &= !saved.Error && !restored.Error && cpu.R[0] == Sentinel &&
                    cpu.CP15Read(0x502) == 0x33 && !(cpu.PU_Map[data >> 12] & 1) &&
                    cpu.R[15] == Vectors+0x14 && cpu.R_ABT[2] == modeFlags;
            }
            ++checked;
            failures += !ok;
            std::printf("data-abort/%s/%s/%u: %s cached=%d r0=%08x base=%08x PC=%08x LR=%08x CPSR=%08x SPSR=%08x cycles=%llu\n",
                jit ? "jit" : "interpreter", probe.name, phase, ok ? "PASS" : "FAIL", cached,
                cpu.R[0], cpu.R[1], cpu.R[15], cpu.R[14], cpu.CPSR, cpu.R_ABT[2], nds->ARM9Timestamp);
            if (ok && probe.swap && !probe.skip && phase == 2)
            {
                cpu.CP15Write(0x502, 0x333);
                for (unsigned step = 0; step < 4 && cpu.R[6] != 1; ++step)
                {
                    nds->ARM9Target = nds->ARM9Timestamp + 1;
#ifdef JIT_ENABLED
                    if (jit) cpu.Execute<CPUExecuteMode::JIT>();
                    else
#endif
                        cpu.Execute<CPUExecuteMode::Interpreter>();
                }
                const bool retried = cpu.R[0] == loaded && cpu.R[1] == base &&
                    nds->ARM9Read32(data) == ((Stored & ~mask) | (Sentinel & mask)) &&
                    cpu.R[6] == 1 && (cpu.CPSR & 0x1F) == (probe.user ? 0x10u : 0x1Fu);
                ++checked;
                failures += !retried;
                std::printf("data-abort/%s/%s/exception-return-retry: %s\n",
                    jit ? "jit" : "interpreter", probe.name, retried ? "PASS" : "FAIL");
            }
        }
    }
    std::printf("MPU single/swap transfers: %u/%u passed\n", checked-failures, checked);
    return failures ? 1 : 0;
}
