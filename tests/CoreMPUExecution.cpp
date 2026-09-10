// SPDX-License-Identifier: GPL-3.0-or-later
// Actual ARM9 guest execution; generated RAM programs and exception vectors.
// ARM DDI 0201D 2.3.8/4.2.3: instruction AP=0 denies, AP=3 permits access.
// ARM DDI 0100I A2.6.5: Prefetch Abort saves fault address+4 and the old CPSR.
// https://documentation-service.arm.com/static/5e8e3ee588295d1e18d3aa82
// https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a
#include "Args.h"
#include "ARM.h"
#include "NDS.h"

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
