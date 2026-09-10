// SPDX-License-Identifier: GPL-3.0-or-later
// Real Timer0 MMIO -> HALT wake -> ARM9 IRQ, including cached guest execution.
// ARM DDI 0100I A2.6.8: IRQ preserves F, saves CPSR and next-instruction+4.
// https://documentation-service.arm.com/static/5f8dacc8f86e16515cdb865a
// DS timer, IF/IE/IME and ARM9 HALT contracts: original GBATEK research.
// https://problemkaputt.de/gbatek.htm#dstimers
// https://problemkaputt.de/gbatek.htm#dsinterrupts
#include "Args.h"
#include "ARM.h"
#include "NDS.h"

#include <cstdio>
#include <memory>
#include <utility>

using namespace melonDS;

namespace
{
constexpr u32 Code = 0x02010000;
constexpr u32 Idle = 0x02000200;
constexpr u32 Vectors = 0xFFFF0000;
constexpr u32 Timer0 = 0x04000100;
constexpr u32 IMEAddress = 0x04000208;
constexpr u32 IEAddress = 0x04000210;
constexpr u32 IFAddress = 0x04000214;
constexpr u32 TimerIRQ = 1u << 3;
constexpr u32 TimerConfig = 0x00C00000; // Reload=0, /1, IRQ enabled, start.
constexpr u64 FirstOverflow = 65536;   // 16-bit counter, not CPU instruction cycles.
constexpr u32 Sentinel = 0xDEADC0DE;
constexpr u32 LinkSentinel = 0x12345678;
constexpr u32 SPSRSentinel = 0x11223344;

enum class Outcome { IRQ, Held, Resumed };
struct DeviceCase
{
    const char* Name;
    u32 MasterEnable;
    u32 EnabledIRQs;
    u32 InitialCPSR;
    u32 FinalCPSR;
    Outcome Result;
};

constexpr DeviceCase Cases[] = {
    {"irq-f-set-control", 1, TimerIRQ, 0xA800005F, 0xA80000D2, Outcome::IRQ},
    {"irq-f-clear-warm",  1, TimerIRQ, 0xA800001F, 0xA8000092, Outcome::IRQ},
    {"halt-ime-masked",   0, TimerIRQ, 0xA800001F, 0xA800001F, Outcome::Held},
    {"halt-ie-masked",    1, 0,        0xA800001F, 0xA800001F, Outcome::Held},
    {"halt-cpsr-masked",  1, TimerIRQ, 0xA800009F, 0xA800009F, Outcome::Resumed},
};

// Observe real MMIO; delegate every operation to the production implementation.
struct ObservedNDS final : NDS
{
    using NDS::NDS;
    bool TimerStarted = false;
    u64 TimerStartedAt = 0;
    u64 HandlerReadAt = 0;
    unsigned HandlerReads = 0;

    void ARM9Write32(u32 addr, u32 value) override
    {
        NDS::ARM9Write32(addr, value);
        if (addr == Timer0 && (value & 0x00800000))
        {
            TimerStarted = true;
            TimerStartedAt = ARM9Timestamp >> ARM9ClockShift;
        }
    }

    u32 ARM9Read32(u32 addr) override
    {
        const u32 value = NDS::ARM9Read32(addr);
        if (addr == IFAddress && (ARM9.CPSR & 0x1F) == 0x12)
        {
            ++HandlerReads;
            HandlerReadAt = ARM9Timestamp >> ARM9ClockShift;
        }
        return value;
    }
};
}

int TestDeviceExecution(NDSArgs&& args, bool jit)
{
    // All vectors are generated; no external BIOS or IRQ wrapper is involved.
    args.ARM9BIOS = std::make_unique<ARM9BIOSImage>();
    auto vectorWord = [&](u32 offset, u32 instruction) {
        for (unsigned byte = 0; byte < 4; ++byte)
            (*args.ARM9BIOS)[offset + byte] = instruction >> (byte * 8);
    };
    for (u32 offset = 0; offset < args.ARM9BIOS->size(); offset += 4)
        vectorWord(offset, 0xEAFFFFFE); // B .
    vectorWord(0x18, 0xE2899001); // ADD r9,r9,#1: count IRQ handler entries.
    vectorWord(0x1C, 0xE10FB000); // MRS r11,CPSR: observe F in the guest.
    vectorWord(0x20, 0xE598C000); // LDR r12,[r8]: observe the real pending IF.

    auto nds = std::make_unique<ObservedNDS>(std::move(args));
    nds->CurCPU = 0;
    nds->Reset();
    auto& cpu = nds->ARM9;
    constexpr u32 program[] = {
        0xE5823000, // STR r3,[r2]: IME
        0xE5845000, // STR r5,[r4]: IE
        0xE5887000, // STR r7,[r8]: acknowledge old Timer0 IF
        0xE5810000, // STR r0,[r1]: Timer0 reload/control in one MMIO write
        0xE598A000, // LDR r10,[r8]: IF must still be clear before HALT
        0xEE076F90, // MCR p15,0,r6,c7,c0,4: ARM9 HALT
        0xE3A06066, // MOV r6,#0x66: reached only by wake without IRQ
        0xEAFFFFFE, // B .
    };
    for (unsigned i = 0; i < sizeof(program) / sizeof(program[0]); ++i)
        nds->ARM9Write32(Code + i * 4, program[i]);
    nds->ARM9Write32(Idle, 0xEAFFFFFE);
    nds->ARM7.JumpTo(Idle);
    nds->Start();

    unsigned failures = 0;
    unsigned caseIndex = 0;
    for (const auto& test : Cases)
    {
        // Neutralize the previous run through real MMIO. Do not reset the CPU,
        // scheduler, code bytes or JIT cache between the five guest entries.
        nds->CurCPU = 0;
        nds->ARM9Write16(Timer0 + 2, 0);
        nds->ARM9Write32(IMEAddress, 0);
        nds->ARM9Write32(IEAddress, 0);
        nds->ARM9Write32(IFAddress, TimerIRQ);
        nds->TimerStarted = false;
        nds->HandlerReads = 0;
        nds->HandlerReadAt = 0;

        const u32 oldCPSR = cpu.CPSR;
        cpu.CPSR = test.InitialCPSR;
        cpu.UpdateMode(oldCPSR, cpu.CPSR);
        cpu.StopExecution = 0;
        cpu.Cycles = 0;
        cpu.R[0] = TimerConfig;
        cpu.R[1] = Timer0;
        cpu.R[2] = IMEAddress;
        cpu.R[3] = test.MasterEnable;
        cpu.R[4] = IEAddress;
        cpu.R[5] = test.EnabledIRQs;
        cpu.R[6] = Sentinel;
        cpu.R[7] = TimerIRQ;
        cpu.R[8] = IFAddress;
        cpu.R[9] = 0;
        cpu.R[10] = cpu.R[11] = cpu.R[12] = Sentinel;
        cpu.R[14] = LinkSentinel;
        cpu.R_IRQ[2] = SPSRSentinel;
        cpu.JumpTo(Code);
        bool warm = true;
#ifdef JIT_ENABLED
        if (jit && caseIndex != 0)
            warm = nds->JIT.JitBlocks9.contains(Code);
#endif
        nds->RunFrame();

        const bool irq = test.Result == Outcome::IRQ;
        const bool held = test.Result == Outcome::Held;
        const bool resumed = test.Result == Outcome::Resumed;
        // Architectural IRQ entry is Vectors+0x18. The handler parks at +0x24;
        // between ARM instructions this core exposes the next address+4 in R15.
        const u32 expectedPC = irq ? Vectors + 0x28 : Code + (held ? 28 : 32);
        const u32 expectedLR = irq ? Code + 28 : LinkSentinel;
        const u32 expectedSPSR = irq ? test.InitialCPSR : SPSRSentinel;
        const u64 now = nds->ARM9Timestamp >> nds->ARM9ClockShift;
        const u64 elapsed = now - nds->TimerStartedAt;
        const bool timerOK = nds->TimerStarted && elapsed >= FirstOverflow &&
            (nds->IF[0] & TimerIRQ) && !(cpu.R[10] & TimerIRQ);
        const bool handlerOK = cpu.R[9] == (irq ? 1u : 0u) &&
            nds->HandlerReads == (irq ? 1u : 0u) &&
            cpu.R[11] == (irq ? test.FinalCPSR : Sentinel) &&
            (irq ? (cpu.R[12] & TimerIRQ) != 0 : cpu.R[12] == Sentinel) &&
            (!irq || nds->HandlerReadAt >= nds->TimerStartedAt + FirstOverflow);
        const bool ok = warm && nds->IsRunning() && timerOK && handlerOK &&
            nds->IME[0] == test.MasterEnable && nds->IE[0] == test.EnabledIRQs &&
            cpu.IRQ == (held ? 0u : 1u) && cpu.Halted == (held ? 1u : 0u) &&
            cpu.CPSR == test.FinalCPSR && cpu.R_IRQ[2] == expectedSPSR &&
            cpu.R[14] == expectedLR && cpu.R[15] == expectedPC &&
            cpu.R[6] == (resumed ? 0x66u : Sentinel);

        std::printf("device/%s/%s: %s\n", jit ? "jit" : "interpreter",
                    test.Name, ok ? "PASS" : "FAIL");
        if (!ok)
        {
            ++failures;
            std::fprintf(stderr,
                "warm=%d timer=%d handler=%d IF=%08x IE=%08x IME=%u IRQ=%u Halted=%u "
                "PC=%08x/%08x LR=%08x/%08x CPSR=%08x/%08x SPSR=%08x/%08x "
                "entries=%u guestCPSR=%08x resume=%08x elapsed=%llu handlerDelta=%llu\n",
                warm, timerOK, handlerOK, nds->IF[0], nds->IE[0], nds->IME[0],
                cpu.IRQ, cpu.Halted, cpu.R[15], expectedPC, cpu.R[14], expectedLR,
                cpu.CPSR, test.FinalCPSR, cpu.R_IRQ[2], expectedSPSR, cpu.R[9],
                cpu.R[11], cpu.R[6], static_cast<unsigned long long>(elapsed),
                static_cast<unsigned long long>(nds->HandlerReads ?
                    nds->HandlerReadAt - nds->TimerStartedAt : 0));
        }
        ++caseIndex;
    }

    std::printf("Device execution: %u/5 cases passed\n", 5 - failures);
    return failures ? 1 : 0;
}
