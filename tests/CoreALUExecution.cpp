// SPDX-License-Identifier: GPL-3.0-or-later
// Generated guest programs run through the real core and scheduler twice:
// compilation interprets a block once, so the second entry must use cached code.
#include "Args.h"
#include "NDS.h"
#include "ARM.h"
#include <array>
#include <cstdio>
#include <memory>

using namespace melonDS;
namespace
{
constexpr u32 Code = 0x02008000, Idle = 0x02000200;
constexpr u32 N = 1u << 31, Z = 1u << 30, C = 1u << 29, V = 1u << 28;
struct ShiftCase { unsigned op, count; u32 result; bool carry; };
// Rm=0x80000001. The expected boundary values are hand-calculated from the
// ARM barrel-shifter contract, not from the interpreter or a host shift helper.
constexpr ShiftCase shifts[] = {
    {0,0,0x80000001,0}, {0,31,0x80000000,0}, {0,32,0,1},
    {0,33,0,0}, {0,255,0,0}, {0,256,0x80000001,0},
    {1,0,0x80000001,0}, {1,31,1,0}, {1,32,0,1},
    {1,33,0,0}, {1,255,0,0}, {1,256,0x80000001,0},
    {2,0,0x80000001,0}, {2,31,0xFFFFFFFF,0}, {2,32,0xFFFFFFFF,1},
    {2,33,0xFFFFFFFF,1}, {2,255,0xFFFFFFFF,1}, {2,256,0x80000001,0},
    {3,0,0x80000001,0}, {3,31,3,0}, {3,32,0x80000001,1},
    {3,33,0xC0000000,1}, {3,255,3,0}, {3,256,0x80000001,0},
};
u32 NZ(u32 value) { return (value & N) | (value == 0 ? Z : 0); }
void Write(NDS& nds, u32 addr, const std::array<u32, 6>& code)
{
    for (unsigned i = 0; i < code.size(); ++i) nds.ARM9Write32(addr + 4 * i, code[i]);
}
bool HasBlock(NDS& nds, bool arm7, u32 addr, bool jit)
{
#ifdef JIT_ENABLED
    if (jit) return (arm7 ? nds.JIT.JitBlocks7 : nds.JIT.JitBlocks9).contains(addr);
#endif
    return true;
}
}

int TestALUExecution(NDSArgs&& args, bool jit)
{
    auto nds = std::make_unique<NDS>(std::move(args));
    unsigned checks = 0, failures = 0;
    for (bool arm7 : {false, true})
    {
        nds->Reset();
        nds->ARM9Write32(Idle, 0xEAFFFFFE);
        nds->ARM9.JumpTo(Idle);
        nds->ARM7.JumpTo(Idle);
        nds->Start();
        ARM& cpu = arm7 ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
        for (unsigned op = 0; op < 4; ++op)
        for (unsigned rd : {1u, 2u, 3u}) // Rm alias, distinct, Rs alias.
        for (unsigned mode = 0; mode < 3; ++mode)
        {
            // MOVS Rd,R1,<shift> R3; optionally overwrite NZ or NZC so only
            // part of the first instruction's flag result remains live.
            const u32 addr = Code + (op * 9 + (rd - 1) * 3 + mode) * 32;
            const u32 follow = mode == 1 ? 0xE3B06000 : mode == 2 ? 0xE1B06087 : 0xE1A06006;
            Write(*nds, addr, {0xE1B00311 | (rd << 12) | (op << 5), follow,
                              0xE10F8000, 0x6A000000, 0xE3A0A001, 0xEAFFFFFE});
            for (const auto& test : shifts)
            {
                if (test.op != op) continue;
                for (bool carry : {false, true})
                for (unsigned run = 0; run < 2; ++run)
                {
                    cpu.R[1] = 0x80000001;
                    cpu.R[3] = test.count;
                    cpu.R[6] = 0xCCCCCCCC;
                    cpu.R[7] = 1;
                    cpu.R[8] = cpu.R[10] = 0;
                    cpu.CPSR = 0xDF | V | (carry ? C : 0);
                    const bool outCarry = (test.count & 0xFF) == 0 ? carry : test.carry;
                    u32 expectedFlags = V | NZ(test.result) | (outCarry ? C : 0);
                    if (mode == 1) expectedFlags = (expectedFlags & (C | V)) | Z;
                    if (mode == 2) expectedFlags = V; // MOVS #1 LSL #1 -> 2, C=0.
                    const bool warm = run == 0 || HasBlock(*nds, arm7, addr, jit);
                    cpu.JumpTo(addr);
                    nds->RunFrame();
                    ++checks;
                    if (!warm || cpu.R[rd] != test.result || cpu.R[8] != (0xDF | expectedFlags) ||
                        cpu.R[10] != 0 || (rd != 1 && cpu.R[1] != 0x80000001) ||
                        (rd != 3 && cpu.R[3] != test.count))
                    {
                        if (failures++ < 12)
                            std::fprintf(stderr, "ARM%d shift op=%u count=%u rd=%u flags=%u C=%u run=%u warm=%d: result=%08x flags=%08x expected=%08x/%08x\n",
                                arm7 ? 7 : 9, op, test.count, rd, mode, carry, run, warm,
                                cpu.R[rd], cpu.R[8], test.result, 0xDF | expectedFlags);
                    }
                }
            }
        }
        // RRX consumes carry even when the shifter carry output is dead.
        for (unsigned setFlags : {0u, 1u})
        {
            const u32 addr = Code + 0x1000 + setFlags * 32;
            Write(*nds, addr, {0xE1A02061 | (setFlags << 20), 0xE10F8000,
                              0xE1A00000, 0xE1A00000, 0xE1A00000, 0xEAFFFFFE});
            for (bool carry : {false, true})
            for (unsigned run = 0; run < 2; ++run)
            {
                cpu.R[1] = 3;
                cpu.CPSR = 0xDF | V | (carry ? C : 0);
                cpu.JumpTo(addr);
                nds->RunFrame();
                const u32 result = 1 | (carry ? N : 0);
                const u32 flags = setFlags ? V | C | NZ(result) : V | (carry ? C : 0);
                ++checks;
                if (cpu.R[2] != result || cpu.R[8] != (0xDF | flags))
                {
                    ++failures;
                    std::fprintf(stderr, "ARM%d RRX S=%u C=%u run=%u: %08x/%08x expected=%08x/%08x\n",
                        arm7 ? 7 : 9, setFlags, carry, run, cpu.R[2], cpu.R[8], result, 0xDF | flags);
                }
            }
        }
        // Rm=PC reads +8 with an immediate shift and +12 with a register shift.
        // ASRS #32 also exercises the shifter's constant PC/carry path.
        for (unsigned form : {0u, 1u, 2u, 3u})
        {
            const u32 addr = Code + 0x1100 + form * 32;
            Write(*nds, addr, {form == 3 ? 0xE1B02041 : form == 2 ? 0xE1B0204F : form == 1 ? 0xE1A0231F : 0xE1A0200F, 0xE10F8000,
                              0xE1A00000, 0xE1A00000, 0xE1A00000, 0xEAFFFFFE});
            for (unsigned run = 0; run < 2; ++run)
            {
                cpu.R[1] = 0x80000001;
                cpu.R[3] = 0;
                cpu.CPSR = 0xB00000DF;
                cpu.JumpTo(addr);
                nds->RunFrame();
                ++checks;
                const u32 result = form == 3 ? 0xFFFFFFFF : form == 2 ? 0 : addr + 8 + form * 4;
                const u32 flags = form == 2 ? 0x500000DF : 0xB00000DF;
                if (cpu.R[2] != result || cpu.R[8] != flags)
                {
                    ++failures;
                    std::fprintf(stderr, "ARM%d PC operand form=%u run=%u: %08x/%08x expected=%08x/%08x\n",
                        arm7 ? 7 : 9, form, run, cpu.R[2], cpu.R[8], result, flags);
                }
            }
        }
        // ADC/SBC/RSC with destination aliases and shifted Operand2. Widened
        // signed/unsigned arithmetic supplies the oracle, independently of JIT
        // host flags and the interpreter's flag helper functions.
        const struct { u32 a, b; bool carry; } arithmetic[] = {
            {0x7FFFFFFF, 1, 1}, {0x7FFFFFFF, 0x80000000, 1},
            {0xFFFFFFFF, 0, 1}, {0x80000000, 0x80000000, 0},
            {0, 1, 0}, {0x80000000, 1, 1},
        };
        for (unsigned op = 5; op <= 7; ++op)
        for (unsigned rd : {0u, 1u, 2u})
        for (unsigned mode = 0; mode < 3; ++mode)
        for (unsigned shifted : {0u, 1u})
        {
            const u32 addr = Code + 0x2000 + (((op - 5) * 9 + rd * 3 + mode) * 2 + shifted) * 32;
            const u32 opcode = 0xE0000000 | (op << 21) | (mode == 2 ? 0 : 1 << 20) | (rd << 12) | (shifted ? 0x311 : 1);
            Write(*nds, addr, {opcode, mode == 1 ? 0xE3B06000 : 0xE1A06006,
                              0xE10F8000, 0x6A000000, 0xE3A0A001, 0xEAFFFFFE});
            for (const auto& test : arithmetic)
            for (unsigned run = 0; run < 2; ++run)
            {
                u32 a = test.a, b = test.b * (shifted ? 2u : 1u);
                if (op == 7) std::swap(a, b);
                const u64 wide = op == 5 ? u64(a) + b + test.carry : u64(a) - b - !test.carry;
                const s64 signedWide = op == 5 ? s64(s32(a)) + s32(b) + test.carry : s64(s32(a)) - s32(b) - !test.carry;
                const u32 result = static_cast<u32>(wide);
                const bool carry = op == 5 ? wide > 0xFFFFFFFF : u64(a) >= u64(b) + !test.carry;
                const bool overflow = signedWide > 0x7FFFFFFFLL || signedWide < -0x80000000LL;
                u32 flags = NZ(result) | (carry ? C : 0) | (overflow ? V : 0);
                if (mode == 1) flags = (flags & (C | V)) | Z;
                if (mode == 2) flags = N | V | (test.carry ? C : 0);
                cpu.R[0] = test.a;
                cpu.R[1] = test.b;
                cpu.R[3] = 1;
                cpu.R[8] = cpu.R[10] = 0;
                cpu.CPSR = 0xDF | N | V | (test.carry ? C : 0);
                const bool warm = run == 0 || HasBlock(*nds, arm7, addr, jit);
                cpu.JumpTo(addr);
                nds->RunFrame();
                ++checks;
                if (!warm || cpu.R[rd] != result || cpu.R[8] != (0xDF | flags) ||
                    cpu.R[10] != ((flags & V) ? 0u : 1u) ||
                    (rd != 0 && cpu.R[0] != test.a) || (rd != 1 && cpu.R[1] != test.b))
                {
                    if (failures++ < 12)
                        std::fprintf(stderr, "ARM%d arithmetic op=%u rd=%u mode=%u shift=%u run=%u: %08x/%08x expected=%08x/%08x\n",
                            arm7 ? 7 : 9, op, rd, mode, shifted, run, cpu.R[rd], cpu.R[8], result, 0xDF | flags);
                }
            }
        }
        cpu.JumpTo(Idle);
    }
    std::printf("warmed ARM ALU/shift/partial flags/RRX/PC: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}

int TestThumbShiftTiming(NDSArgs&& args, bool jit)
{
    auto nds = std::make_unique<NDS>(std::move(args));
    unsigned failures = 0;
    for (bool arm7 : {false, true})
    {
        u32 iterations[2]{};
        for (unsigned asr = 0; asr < 2; ++asr)
        {
            nds->Reset();
            nds->ARM9Write32(Idle, 0xEAFFFFFE);
            nds->ARM9.JumpTo(Idle);
            nds->ARM7.JumpTo(Idle);
            // Same positive input and instruction fetches: ADDS R0,#1;
            // MOV R2,R4; LSRS/ASRS R2,R1; B back. Both shifts need one I cycle.
            const u16 code[] = {0x3001, 0x4622, static_cast<u16>(asr ? 0x410A : 0x40CA), 0xE7FB};
            for (unsigned i = 0; i < std::size(code); ++i) nds->ARM9Write16(Code + 2 * i, code[i]);
            ARM& cpu = arm7 ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
            cpu.R[0] = 0;
            cpu.R[1] = 3;
            cpu.R[4] = 0x12345678;
            cpu.CPSR = 0xDF;
            cpu.JumpTo(Code | 1);
            nds->Start();
            nds->RunFrame();
            if (!HasBlock(*nds, arm7, Code | 1, jit))
            {
                std::fprintf(stderr, "ARM%d Thumb loop was not compiled\n", arm7 ? 7 : 9);
                return 2;
            }
            const u32 before = cpu.R[0];
            nds->RunFrame();
            iterations[asr] = cpu.R[0] - before;
        }
        std::printf("ARM%d warmed Thumb LSR/ASR: %u/%u iterations per frame\n", arm7 ? 7 : 9, iterations[0], iterations[1]);
        if (iterations[0] != iterations[1]) ++failures;
    }
    return failures ? 1 : 0;
}
