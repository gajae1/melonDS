// SPDX-License-Identifier: GPL-3.0-or-later
// Use ExtractFunction.py to generate the three includes from the current source.
// This runs the production emitter on any host; emitted A64 code is not executed.
#include "jit/Arm64Emitter.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>

using namespace melonDS;
using namespace Arm64Gen;
constexpr ARM64Reg RCPSR = W27;

#include "ARM64JitOp2.inc"
;

class Compiler : public ARM64XEmitter
{
public:
    struct { u8 SetFlags = 0xF; } CurInstr;
    bool CPSRDirty = false;
    void Comp_Arithmetic(int op, bool S, ARM64Reg rd, ARM64Reg rn, Op2 op2);
    void Comp_RetriveFlags(bool retriveCV);
};

#include "ARM64JitArithmetic.inc"
#include "ARM64JitFlags.inc"

// Only the straight-line, 32-bit integer instructions used by these carry cases.
// Decode actual bytes using Arm DDI 0602 encodings, independently of emitter APIs.
// https://documentation-service.arm.com/static/606ef2575e70d934bc69e1bf
// Unsupported encodings fail, so an unmodelled instruction cannot silently pass.
struct A64State
{
    std::array<u32, 32> w{};
    u32 nzcv = 0;

    [[noreturn]] static void Unsupported(u32 instr)
    {
        std::fprintf(stderr, "Unsupported A64 encoding: %08X\n", instr);
        std::exit(2);
    }

    u32 Add(u32 a, u32 b, u32 carry, bool flags)
    {
        const u64 sum = u64(a) + b + carry;
        const u32 result = u32(sum);
        if (flags)
            nzcv = (result & 0x80000000) | (u32(result == 0) << 30) |
                   (u32(sum >> 32) << 29) | ((~(a ^ b) & (a ^ result) & 0x80000000) >> 3);
        return result;
    }

    bool Condition(u32 cond, u32 instr) const
    {
        if (cond >= 8) Unsupported(instr);
        const unsigned bit = std::array{30u, 29u, 31u, 28u}[cond >> 1];
        return bool((nzcv >> bit) & 1) != bool(cond & 1);
    }

    void Run(std::span<const u32> code)
    {
        for (const u32 instr : code)
        {
            if (instr >> 31) Unsupported(instr);
            const u32 d = instr & 31, n = (instr >> 5) & 31, m = (instr >> 16) & 31;
            const bool flags = instr & (1u << 29), sub = instr & (1u << 30);
            if ((instr & 0x1F000000) == 0x11000000) // ADD(S)/SUB(S) immediate
            {
                const u32 imm = ((instr >> 10) & 0xFFF) << ((instr & (1u << 22)) ? 12 : 0);
                w[d] = Add(w[n], sub ? ~imm : imm, sub, flags);
            }
            else if ((instr & 0x1F200000) == 0x0B000000) // ADD(S)/SUB(S), LSL #0
            {
                if (instr & 0x00C0FC00) Unsupported(instr);
                w[d] = Add(w[n], sub ? ~w[m] : w[m], sub, flags);
            }
            else if ((instr & 0x3FE0FC00) == 0x3A000000) // ADCS/SBCS
                w[d] = Add(w[n], sub ? ~w[m] : w[m], (nzcv >> 29) & 1, true);
            else if ((instr & 0x7FE0FC00) == 0x2A200000) // ORN, LSL #0 (including MVN)
                w[d] = w[n] | ~w[m];
            else if ((instr & 0x7FE00C00) == 0x1A800400) // CSINC, including CSET
                w[d] = Condition((instr >> 12) & 15, instr) ? w[n] : w[m] + 1;
            else if ((instr & 0x7F800000) == 0x53000000) // UBFM: UBFX
            {
                const u32 r = (instr >> 16) & 63, s = (instr >> 10) & 63;
                if (r > s || s >= 32) Unsupported(instr);
                w[d] = (w[n] >> r) & (0xFFFFFFFFu >> (31 - (s - r)));
            }
            else if ((instr & 0x7F800000) == 0x33000000) // BFM: BFI
            {
                const u32 r = (instr >> 16) & 63, s = (instr >> 10) & 63;
                if (r <= s || r >= 32) Unsupported(instr);
                const u32 mask = (0xFFFFFFFFu >> (31 - s)) << (32 - r);
                w[d] = (w[d] & ~mask) | ((w[n] << (32 - r)) & mask);
            }
            else if ((instr & 0x1F800000) == 0x12800000) // MOVN/Z/K
            {
                const u32 shift = ((instr >> 21) & 3) * 16, op = (instr >> 29) & 3;
                if (shift >= 32 || op == 1) Unsupported(instr);
                const u32 imm = ((instr >> 5) & 0xFFFF) << shift;
                w[d] = op == 0 ? ~imm : op == 2 ? imm : (w[d] & ~(0xFFFFu << shift)) | imm;
            }
            else
                Unsupported(instr);
            w[31] = 0; // WZR; no SP operands occur in these snippets.
        }
    }
};

int main(int argc, char** argv)
{
    const std::string_view filter = argc == 2 ? argv[1] : "all";
    if (argc > 2 || (filter != "all" && filter != "ADC" && filter != "SBC" &&
                    filter != "RSC-reg" && filter != "RSC-imm"))
    {
        std::fprintf(stderr, "Usage: ARM64JitArithmetic [all|ADC|SBC|RSC-reg|RSC-imm]\n");
        return 2;
    }
    // Guest-immediate values here are all encodable by A32's rotated imm8.
    // Expected NZCV comes from the mathematical sum/difference, not host flags.
    struct Case { u32 op, a, b, carry, result, flags; };
    constexpr Case cases[] = {
        {5, 0x7FFFFFFF, 0x80000000, 1, 0x00000000, 0x60000000}, // two overflows cancel
        {5, 0x7FFFFFFF, 0x00000001, 1, 0x80000001, 0x90000000},
        {5, 0xFFFFFFFF, 0x00000000, 1, 0x00000000, 0x60000000},
        {5, 0x80000000, 0x80000000, 0, 0x00000000, 0x70000000},
        {5, 0xFFFFFFFF, 0x00000001, 0, 0x00000000, 0x60000000},
        {5, 0x00000000, 0x00000000, 0, 0x00000000, 0x40000000},
        {5, 0x7FFFFFFF, 0x00000000, 1, 0x80000000, 0x90000000},
        {5, 0x7FFFFFFF, 0x80000000, 0, 0xFFFFFFFF, 0x80000000},
        {5, 0x7FFFFFFE, 0x80000000, 1, 0xFFFFFFFF, 0x80000000},
        {6, 0x80000000, 0x80000000, 1, 0x00000000, 0x60000000},
        {6, 0x80000000, 0x80000000, 0, 0xFFFFFFFF, 0x80000000},
        {6, 0x00000000, 0x00000001, 1, 0xFFFFFFFF, 0x80000000},
        {6, 0x80000000, 0x00000001, 1, 0x7FFFFFFF, 0x30000000},
        {6, 0x00000001, 0x80000000, 1, 0x80000001, 0x90000000},
        {6, 0x12345678, 0x00000001, 0, 0x12345676, 0x20000000},
        {7, 0x80000000, 0x80000000, 1, 0x00000000, 0x60000000},
        {7, 0x80000000, 0x80000000, 0, 0xFFFFFFFF, 0x80000000},
        {7, 0x00000001, 0x00000000, 1, 0xFFFFFFFF, 0x80000000},
        {7, 0x00000001, 0x80000000, 1, 0x7FFFFFFF, 0x30000000},
        {7, 0x80000000, 0x00000001, 1, 0x80000001, 0x90000000},
        {7, 0x00000001, 0x00000012, 0, 0x00000010, 0x20000000},
    };
    unsigned failures = 0, checked = 0;
    constexpr const char* names[] = {"ADC", "SBC", "RSC"};
    std::array<bool, 3> printed{};
    for (const Case& c : cases)
        for (const bool immediate : {true, false})
            for (const ARM64Reg rd : {W4, W5, W6}) // distinct or either source as destination
            for (const unsigned flagMode : {0u, 1u, 2u})
            {
                // A large RSC immediate needs a scratch register even when S is
                // clear, or the block analysis has made its flag writes dead.
                if (flagMode && !(c.op == 7 && immediate && c.b == 0x80000000)) continue;
                const char* name = names[c.op - 5];
                if (filter != "all" && filter != name &&
                    !(filter == "RSC-reg" && c.op == 7 && !immediate) &&
                    !(filter == "RSC-imm" && c.op == 7 && immediate)) continue;
                std::array<u32, 128> code{};
                Compiler compiler;
                compiler.CurInstr.SetFlags = flagMode == 2 ? 0 : 0xF;
                compiler.SetCodeBase(reinterpret_cast<u8*>(code.data()), reinterpret_cast<u8*>(code.data()));
                // A_Comp_ALUTriOp maps a zero immediate to WZR before this helper.
                const Op2 op2 = immediate ? (c.b ? Op2(c.b) : Op2(WZR)) : Op2(W6);
                compiler.Comp_Arithmetic(c.op, flagMode != 1, rd, W5, op2);
                const auto words = std::span(code).first(compiler.GetCodeOffset() / 4);
                if (!printed[c.op - 5])
                {
                    printed[c.op - 5] = true;
                    std::printf("%s regression emitted words:", name);
                    for (const u32 instr : words) std::printf(" %08X", instr);
                    std::puts("");
                }
                A64State state;
                state.w[5] = c.a;
                state.w[6] = c.b;
                constexpr u32 otherFlags = 0x0800001F;
                const u32 initialFlags = otherFlags | 0x90000000 | (c.carry << 29);
                const u32 expectedFlags = flagMode ? initialFlags : (otherFlags | c.flags);
                state.w[27] = initialFlags;
                state.Run(words);
                ++checked;
                if (state.w[rd] != c.result || state.w[27] != expectedFlags ||
                    (rd != W6 && state.w[6] != c.b) || (rd != W5 && state.w[5] != c.a) || compiler.CPSRDirty != (flagMode == 0))
                {
                    ++failures;
                    std::printf("FAIL %s%s a=%08X b=%08X C=%u rd=W%u flagMode=%u: result=%08X CPSR=%08X; expected=%08X/%08X\n",
                                name, immediate ? "-imm" : "-reg", c.a, c.b, c.carry, unsigned(rd), flagMode,
                                state.w[rd], state.w[27], c.result, expectedFlags);
                }
            }
    std::printf("A64 emitted ADC/SBC/RSC result/NZCV, input preservation and destination aliasing: %u cases, %u failures (ISA model; no native ARM execution)\n",
                checked, failures);
    return failures ? 1 : 0;
}
