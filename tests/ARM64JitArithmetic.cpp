// SPDX-License-Identifier: GPL-3.0-or-later
// Use ExtractFunction.py to generate includes from the current source.
// Runs the production emitter on any host; the bounded ISA model below checks
// its bytes. ARM64_JIT_MUL_TEST also exports blocks for an external A64 executor.
#include "jit/Arm64Emitter.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string_view>
#ifdef ARM64_JIT_BLOCK_TEST
#include "NDS.h"
#include "ARM.h"
#include "ARMInterpreter.h"
#include "ARMJIT_RegisterCache.h"
#include <cstring>
#include <memory>
extern "C" void ARM_Ret();
#endif

using namespace melonDS;
using namespace Arm64Gen;
namespace A64Test
{
constexpr ARM64Reg RCPSR = W27;
#ifdef ARM64_JIT_BLOCK_TEST
constexpr ARM64Reg RCycles = W28, RCPU = X29, RMemBase = X26;
#endif

#include "ARM64JitOp2.inc"
;

class Compiler : public ARM64XEmitter
{
public:
#ifdef ARM64_JIT_BLOCK_TEST
    using CompileFunc = void (Compiler::*)();
    // Code-cache exhaustion and fastmem setup are outside these no-memory blocks.
    // Keep their interface; an unexpected cache reset fails the fixture.
    struct BlockContext
    {
        decltype(melonDS::NDS::ARM7MemTimings)& ARM7MemTimings;
        struct
        {
            struct { void *FastMem9Start = nullptr, *FastMem7Start = nullptr; } Memory;
            void ResetBlockCache() { std::abort(); }
        } JIT;
    } NDS;
    explicit Compiler(melonDS::NDS& nds) : NDS{nds.ARM7MemTimings} {}
    FetchedInstr CurInstr{};
    RegisterCache<Compiler, ARM64Reg> RegCache;
    ARM* CurCPU = nullptr;
    bool Thumb = false, Exit = false, IrregularCycles = false;
    u32 Num = 0, R15 = 0, CodeRegion = 0, ConstantCycles = 0;
    u32 JitMemMainSize = 1024 * 1024, JitMemSecondarySize = 1024 * 1024;
    ptrdiff_t OtherCodeRegion = JitMemMainSize;
    ARM64Reg MapReg(int reg) { return RegCache.Mapping[reg]; }
    JitBlockEntry CompileBlock(ARM* cpu, bool thumb, FetchedInstr instrs[], int instrsCount, bool hasMemInstr);
    void Comp_BranchSpecialBehaviour(bool taken);
    FixupBranch CheckCondition(u32 cond);
    void Comp_AddCycles_C(bool forceNonConstant = false);
    void Comp_AddCycles_CI(u32 numI);
#ifdef ARM64_JIT_MUL_TEST
    void Comp_AddCycles_CI(u32 c, ARM64Reg numI, ArithOption shift);
    void Comp_Mul_Mla(bool S, bool mla, ARM64Reg rd, ARM64Reg rm, ARM64Reg rs, ARM64Reg rn);
    void A_Comp_Mul();
    void A_Comp_Mul_Long();
    void T_Comp_ALU();
    void Comp_Compare(int op, ARM64Reg rn, Op2 op2);
    bool FlagsNZNeeded() const { return CurInstr.SetFlags & 0xC; }
#endif
    void LoadCycles();
    void SaveCycles();
    void LoadCPSR();
    void SaveCPSR(bool markClean = true);
    void LoadReg(int reg, ARM64Reg nativeReg);
    void SaveReg(int reg, ARM64Reg nativeReg);
    void A_Comp_ALUTriOp();
    void A_Comp_GetOp2(bool S, Op2& op2);
    void Comp_RegShiftReg(int op, bool S, Op2& op2, ARM64Reg rs);
    void Comp_RegShiftImm(int op, int amount, bool S, Op2& op2, ARM64Reg tmp = W0);
    // Outside this fixture's ADD/nonbranch scope. Never silently emit a stub.
    void Comp_Logical(int, bool, ARM64Reg, ARM64Reg, Op2) { std::abort(); }
    void Comp_JumpTo(ARM64Reg, bool, bool) { std::abort(); }
#else
    struct { u8 SetFlags = 0xF; } CurInstr;
#endif
    bool CPSRDirty = false;
    void Comp_Arithmetic(int op, bool S, ARM64Reg rd, ARM64Reg rn, Op2 op2);
    void Comp_RetriveFlags(bool retriveCV);
};

#include "ARM64JitArithmetic.inc"
#include "ARM64JitFlags.inc"

#ifdef ARM64_JIT_BLOCK_TEST
// Use the real register cache; only the available host-register list is a fixture.
// It matches the A64 backend's allocation order.
}
template <> const ARM64Reg melonDS::RegisterCache<A64Test::Compiler, ARM64Reg>::NativeRegAllocOrder[] =
    {W19, W20, W21, W22, W23, W24, W25, W8, W9, W10, W11, W12, W13, W14, W15};
template <> const int melonDS::RegisterCache<A64Test::Compiler, ARM64Reg>::NativeRegsAvailable = 15;
namespace A64Test
{
// Only ADD entries are populated. Clearing one entry explicitly forces the real
// interpreter fallback for the same defined instruction, without an ISA guess.
std::array<Compiler::CompileFunc, ARMInstrInfo::ak_Count> A_Comp{};
std::array<Compiler::CompileFunc, ARMInstrInfo::tk_Count> T_Comp{};
#include "ARM64JitCompileBlock.inc"
#include "ARM64JitBranchSpecial.inc"
#include "ARM64JitCondition.inc"
#include "ARM64JitCyclesC.inc"
#include "ARM64JitCyclesCI.inc"
#include "ARM64JitLoadCycles.inc"
#include "ARM64JitSaveCycles.inc"
#include "ARM64JitLoadCPSR.inc"
#include "ARM64JitSaveCPSR.inc"
#include "ARM64JitLoadReg.inc"
#include "ARM64JitSaveReg.inc"
#include "ARM64JitTriOp.inc"
#include "ARM64JitGetOp2.inc"
#include "ARM64JitShiftReg.inc"
#include "ARM64JitShiftImm.inc"
#ifdef ARM64_JIT_MUL_TEST
// Current production functions, also used by the external Unicorn execution.
#include "ARM64JitCyclesVariable.inc"
#include "ARM64JitMulMla.inc"
#include "ARM64JitMul.inc"
#include "ARM64JitMulLong.inc"
#include "ARM64JitThumbALU.inc"
#include "ARM64JitCompare.inc"
#endif
#endif

// Only the integer instructions used by these arithmetic/conditional cases.
// Decode actual bytes using Arm DDI 0602 encodings, independently of emitter APIs.
// https://documentation-service.arm.com/static/606ef2575e70d934bc69e1bf
// Unsupported encodings fail, so an unmodelled instruction cannot silently pass.
struct A64State
{
    std::array<u32, 32> w{};
    u32 nzcv = 0;
#ifdef ARM64_JIT_BLOCK_TEST
    std::array<u64, 32> x{};
    ARM* cpu = nullptr;
    InterpreterFunc fallback = nullptr;
    unsigned calls = 0;
#endif

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
        if (cond == 10) return ((nzcv >> 31) & 1) == ((nzcv >> 28) & 1); // GE
        if (cond >= 8) Unsupported(instr);
        const unsigned bit = std::array{30u, 29u, 31u, 28u}[cond >> 1];
        return bool((nzcv >> bit) & 1) != bool(cond & 1);
    }

    void Run(std::span<const u32> code)
    {
        unsigned steps = 0;
        for (ptrdiff_t pc = 0; pc < ptrdiff_t(code.size()); ++pc)
        {
            const u32 instr = code[pc];
            if (++steps > 1024) Unsupported(instr);
            const u32 d = instr & 31, n = (instr >> 5) & 31, m = (instr >> 16) & 31;
            auto branch = [&](s32 offset) {
                const ptrdiff_t target = pc + offset;
                if (target < 0 || target > ptrdiff_t(code.size())) Unsupported(instr);
                pc = target - 1;
            };
#ifdef ARM64_JIT_BLOCK_TEST
            if ((instr & 0xFFDFFC1F) == 0xD61F0000) // BR / BLR
            {
                if (instr & (1u << 21))
                {
                    if (!fallback || x[n] != u64(fallback) || x[0] != u64(cpu)) Unsupported(instr);
                    fallback(cpu); // actual interpreter; model only the host call boundary
                    ++calls;
                    for (unsigned reg = 0; reg <= 18; ++reg) w[reg] = 0xCCCCCCCC;
                }
                else
                {
                    if (x[n] != u64(ARM_Ret)) Unsupported(instr);
                    return;
                }
                continue;
            }
            if ((instr & 0xFFC00000) == 0xB9000000 || (instr & 0xFFC00000) == 0xB9400000)
            {
                const u32 offset = ((instr >> 10) & 0xFFF) * 4;
                if (!cpu || n != 29 || x[n] != u64(cpu) || offset + 4 > sizeof(ARM)) Unsupported(instr);
                auto* addr = reinterpret_cast<u8*>(cpu) + offset;
                if (instr & (1u << 22)) std::memcpy(&w[d], addr, 4);
                else std::memcpy(addr, &w[d], 4);
                x[d] = w[d];
                continue;
            }
            if ((instr & 0xFFE0FFE0) == 0xAA0003E0) // MOV Xd,Xm
            {
                x[d] = x[m];
                w[d] = u32(x[d]);
                continue;
            }
            if ((instr & 0x9F800000) == 0x92800000) // 64-bit MOVN/Z/K for call targets
            {
                const u32 shift = ((instr >> 21) & 3) * 16, op = (instr >> 29) & 3;
                if (op == 1) Unsupported(instr);
                const u64 imm = u64((instr >> 5) & 0xFFFF) << shift;
                x[d] = op == 0 ? ~imm : op == 2 ? imm : (x[d] & ~(u64(0xFFFF) << shift)) | imm;
                w[d] = u32(x[d]);
                continue;
            }
#endif
            if (instr >> 31) Unsupported(instr);
            const bool flags = instr & (1u << 29), sub = instr & (1u << 30);
            if ((instr & 0xFC000000) == 0x14000000) // B, signed word offset from this instruction
            {
                branch(s32(instr << 6) >> 6);
                continue;
            }
            else if ((instr & 0x7E000000) == 0x34000000) // CBZ/CBNZ Wt
            {
                const bool nonzero = d != 31 && w[d] != 0;
                if (nonzero == bool(instr & (1u << 24))) branch(s32(instr << 8) >> 13);
                continue;
            }
            else if ((instr & 0x7E000000) == 0x36000000) // TBZ/TBNZ (bits 0..31)
            {
                const bool set = w[d] & (1u << ((instr >> 19) & 31));
                if (set == bool(instr & (1u << 24))) branch(s32(instr << 13) >> 18);
                continue;
            }
            else if ((instr & 0x7FE0FC00) == 0x1AC02000) // LSLV
                w[d] = w[n] << (w[m] & 31);
            else if ((instr & 0x7FE00C00) == 0x1A800000) // CSEL
                w[d] = Condition((instr >> 12) & 15, instr) ? w[n] : w[m];
            else if ((instr & 0x1F000000) == 0x11000000) // ADD(S)/SUB(S) immediate
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
#ifdef ARM64_JIT_BLOCK_TEST
            x[d] = w[d];
            x[31] = 0;
#endif
        }
    }
};

#ifdef ARM64_JIT_BLOCK_TEST
#ifdef ARM64_JIT_MUL_TEST
// Arm DDI 0029E sections 4.7.3, 4.8.3 and 5.4/table 5-5; DDI 0029G
// table 6-23. These explicit byte boundaries are independent of CLS/CLZ.
// The runner must execute these production CompileBlock bytes (e.g. Unicorn),
// not treat successful emission or interpreter agreement as a passing test.
int ExportMultiplyBlocks()
{
    struct Boundary { u32 value, signedM, unsignedM; };
    constexpr Boundary boundaries[] = {
        {0, 1, 1}, {1, 1, 1}, {0x7F, 1, 1}, {0x80, 1, 1}, {0xFF, 1, 1},
        {0x100, 2, 2}, {0x7FFF, 2, 2}, {0x8000, 2, 2}, {0xFFFF, 2, 2},
        {0x10000, 3, 3}, {0x7FFFFF, 3, 3}, {0x800000, 3, 3}, {0xFFFFFF, 3, 3},
        {0x1000000, 4, 4}, {0x7FFFFFFF, 4, 4}, {0x80000000, 4, 4},
        {0xFFFFFFFF, 1, 4}, {0xFFFFFF80, 1, 4}, {0xFFFFFF00, 1, 4},
        {0xFFFFFEFF, 2, 4}, {0xFFFF0000, 2, 4}, {0xFFFEFFFF, 3, 4},
        {0xFF000000, 3, 4}, {0xFEFFFFFF, 4, 4},
    };
    struct Variant
    {
        const char* name;
        bool s = false;
        u8 flags = 0xC;
        unsigned alias = 0, cond = 14;
        bool z = false, prefix = false, consumeZ = false, waitstates = false;
        u32 multiplicand = 1, halfword = 0;
    };
    constexpr Variant guards[] = {
        {"live-NZ", true}, {"live-N", true, 8}, {"live-Z", true, 4},
        {"dead-NZ", true, 0}, {"alias-Rs", true, 12, 1},
        {"alias-accumulator", true, 12, 2},
        {"EQ-taken", true, 12, 0, 0, true},
        {"EQ-skipped", true, 12, 0, 0, false},
        {"NE-taken", false, 12, 0, 1, false},
        {"NE-skipped", false, 12, 0, 1, true},
        {"prefix", false, 12, 0, 14, false, true},
        {"consume-Z", true, 12, 0, 14, false, false, true},
        {"waitstates", true, 12, 0, 14, false, true, false, true},
        {"wide-multiplicand", true, 12, 0, 14, false, false, false, false, 0x81234567},
        {"second-halfword", true, 12, 0, 14, false, false, false, false, 1, 2},
    };
    constexpr const char* names[] = {"MUL", "MLA", "UMULL", "UMLAL", "SMULL", "SMLAL", "Thumb-MUL"};
    NDSArgs args;
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    unsigned count = 0;
    auto printRegs = [](const u32* regs) {
        std::putchar('[');
        for (unsigned i = 0; i < 16; ++i) std::printf("%s%u", i ? "," : "", regs[i]);
        std::putchar(']');
    };
    auto emit = [&](unsigned num, unsigned op, Boundary b, Variant v) {
        const bool thumb = op == 6, longOp = op >= 2 && op <= 5;
        if ((thumb && (v.cond != 14 || v.prefix || v.consumeZ || v.alias == 2)) ||
            (!thumb && v.halfword) || (op == 0 && v.alias == 2)) return;
        ARM& cpu = num ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
        const u32 ns = v.waitstates ? 5 : 1, seq = v.waitstates ? 2 : 1;
        for (unsigned i = 0; i < 4; ++i) nds->ARM7MemTimings[1][i] = i & 1 ? seq : ns;
        u32 rd = 0, rm = 1, rs = 2, rn = 3;
        if (longOp) { if (v.alias) rs = v.alias == 1 ? rd : rn; }
        else if (!thumb) { if (v.alias) rd = v.alias == 1 ? rs : rn; }
        if (thumb) { rs = v.alias ? 0 : 1; }
        u32 opcode = thumb ? 0x4340 | (rs << 3) | rd :
            (v.cond << 28) | (u32(v.s) << 20) | (rs << 8) | rm | 0x90;
        if (!thumb)
            opcode |= longOp ? 0x800000 | (u32(op >= 4) << 22) | ((op & 1) << 21) | (rn << 16) | (rd << 12)
                             : (u32(op == 1) << 21) | (rd << 16) | (op == 1 ? rn << 12 : 0);
        FetchedInstr instrs[3]{};
        unsigned length = 0;
        auto append = [&](u32 instr, Compiler::CompileFunc method, u8 flags) {
            auto& f = instrs[length];
            f.Instr = instr;
            f.Addr = 0x02008000 + v.halfword + length * (thumb ? 2 : 4);
            f.CodeCycles = 1;
            f.SetFlags = flags;
            f.Info = ARMInstrInfo::Decode(thumb, num, instr, false);
            (thumb ? T_Comp[f.Info.Kind] : A_Comp[f.Info.Kind]) = method;
            ++length;
        };
        if (v.prefix) append(0xE2844001, &Compiler::A_Comp_ALUTriOp, 0);
        append(opcode, thumb ? &Compiler::T_Comp_ALU : longOp ? &Compiler::A_Comp_Mul_Long : &Compiler::A_Comp_Mul, v.flags);
        if (v.consumeZ) append(0x02866001, &Compiler::A_Comp_ALUTriOp, 0); // ADDEQ r6,r6,#1
        std::array<u32, 512> code{};
        Compiler compiler(*nds);
        compiler.SetCodeBase(reinterpret_cast<u8*>(code.data()), reinterpret_cast<u8*>(code.data()));
        compiler.CompileBlock(&cpu, thumb, instrs, length, false);
        const auto words = std::span(code).first(compiler.GetCodeOffset() / 4);
        for (unsigned i = 0; i < 16; ++i) cpu.R[i] = 0x13572468 + i;
        cpu.R[rm] = v.multiplicand;
        cpu.R[thumb ? rd : rs] = b.value;
        if (thumb && rs != rd) cpu.R[rs] = v.multiplicand;
        cpu.R[15] = instrs[0].Addr + (thumb ? 4 : 8);
        const u32 cpsr = 0xB80000DF | (v.z ? 1u << 30 : 0) | (thumb ? 0x20 : 0);
        cpu.CPSR = cpsr;
        cpu.Cycles = 7;
        cpu.CodeCycles = 1;
        std::printf("{\"name\":\"ARM%u-%s-%08X-%s\",\"num\":%u,\"op\":%u,\"s\":%u,\"flags\":%u,"
                    "\"m\":%u,\"unsignedM\":%u,\"rd\":%u,\"rm\":%u,\"rs\":%u,\"rn\":%u,"
                    "\"ns\":%u,\"seq\":%u,\"prefix\":%u,\"consumeZ\":%u,\"cpsr\":%u,\"initial\":",
                    num ? 7 : 9, names[op], b.value, v.name, num, op, unsigned(thumb || v.s), v.flags,
                    op == 2 || op == 3 ? b.unsignedM : b.signedM, b.unsignedM, rd, rm, rs, rn,
                    ns, seq, unsigned(v.prefix), unsigned(v.consumeZ), cpsr);
        printRegs(cpu.R);
        std::printf(",\"base\":%llu,\"return\":%llu,\"regOffset\":%zu,\"cpsrOffset\":%zu,\"cyclesOffset\":%zu,\"words\":[",
                    u64(code.data()), u64(ARM_Ret), offsetof(ARM, R), offsetof(ARM, CPSR), offsetof(ARM, Cycles));
        for (unsigned i = 0; i < words.size(); ++i) std::printf("%s%u", i ? "," : "", words[i]);
        std::printf("],\"instrs\":[");
        for (unsigned i = 0; i < length; ++i)
        {
            const auto& f = instrs[i];
            std::printf("%s%u", i ? "," : "", f.Instr);
            cpu.CurInstr = f.Instr;
            cpu.R[15] = f.Addr + (thumb ? 4 : 8);
            const bool taken = thumb || f.Cond() == 14 ||
                (f.Cond() == 0 ? bool(cpu.CPSR & (1u << 30)) : !bool(cpu.CPSR & (1u << 30)));
            if (taken) (thumb ? InterpretTHUMB[f.Info.Kind] : InterpretARM[f.Info.Kind])(&cpu);
            else cpu.AddCycles_C();
        }
        std::printf("],\"interpreter\":");
        printRegs(cpu.R);
        std::printf(",\"interpreterCPSR\":%u,\"interpreterCycles\":%u}\n", cpu.CPSR, cpu.Cycles - 7);
        ++count;
    };
    for (unsigned num : {1u, 0u})
        for (unsigned op = 0; op < 7; ++op)
        {
            // Every ARM7 termination boundary; ARM9 has fixed internal timing.
            for (const auto b : boundaries)
                if (num || b.value == 0 || b.value == 1 || b.value == 0x1000000 || b.value == 0xFFFFFFFF)
                    emit(num, op, b, {"boundary"});
            for (const auto v : guards) emit(num, op, {0x1000000, 4, 4}, v);
            emit(num, op, {0, 1, 1}, {"zero-flags", true});
            emit(num, op, {1, 1, 1}, {"operand-order", true, 12, 0, 14, false, false, false, false, 0x1000000});
        }
    std::fprintf(stderr, "Exported %u production A64 multiply blocks; execution by external runner required.\n", count);
    return 0;
}
#endif

int TestConditionalCycles()
{
    NDSArgs args;
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    // Arm DDI 0029G tables 1-2/1-4/1-6 and sections 6.5/6.19:
    // ADD uses the shifted operand; EQ means Z=1. With C=I=1 the register
    // shift takes 2 cycles when executed and 1 when skipped. S is clear.
    const struct { const char* name; u32 instr; bool fallback, prefix; u32 result, cycles; } cases[] = {
        {"ADDEQ-shift", 0x00810312, false, false, 3, 2},
        {"ADDNE-shift", 0x10810312, false, false, 3, 2},
        {"ADDEQ-normal", 0x00810002, false, false, 2, 1},
        {"ADDAL-shift", 0xE0810312, false, false, 3, 2},
        {"ADDEQ-fallback", 0x00810312, true, false, 3, 2},
        {"ADDEQ-after-ADDAL", 0x00810312, false, true, 3, 2},
        {"ADDEQ-fallback-after-ADDAL", 0x00810312, true, true, 3, 2},
    };
    unsigned checked = 0, failures = 0;
    for (unsigned num : {0u, 1u})
    {
        ARM& cpu = num ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
        for (unsigned timing = 0; timing < 4; ++timing) nds->ARM7MemTimings[1][timing] = 1;
        for (const auto& test : cases)
        {
            FetchedInstr instrs[2]{};
            const unsigned count = test.prefix ? 2 : 1;
            for (unsigned i = 0; i < count; ++i)
            {
                auto& instr = instrs[i];
                instr.Instr = test.prefix && i == 0 ? 0xE2844001 : test.instr; // ADD r4,r4,#1
                instr.Addr = 0x02008000 + i * 4;
                instr.CodeCycles = 1;
                instr.Info = ARMInstrInfo::Decode(false, num, instr.Instr, false);
                A_Comp[instr.Info.Kind] = &Compiler::A_Comp_ALUTriOp;
            }
            if (test.fallback) A_Comp[instrs[count - 1].Info.Kind] = nullptr;
            std::array<u32, 256> code{};
            Compiler compiler(*nds);
            compiler.SetCodeBase(reinterpret_cast<u8*>(code.data()), reinterpret_cast<u8*>(code.data()));
            compiler.CompileBlock(&cpu, false, instrs, count, false);
            const auto words = std::span(code).first(compiler.GetCodeOffset() / 4);
            if (num == 0 && !test.prefix)
            {
                std::printf("%s emitted words:", test.name);
                for (u32 word : words) std::printf(" %08X", word);
                std::puts("");
            }
            for (bool z : {false, true})
            {
                constexpr u32 sentinel = 0x12345678, initialCycles = 7;
                cpu.R[0] = sentinel;
                cpu.R[1] = cpu.R[2] = cpu.R[3] = 1;
                cpu.R[4] = 0;
                cpu.CPSR = 0xDF | (z ? 1u << 30 : 0);
                cpu.Cycles = initialCycles;
                cpu.CodeCycles = 1;
                const u32 cpsr = cpu.CPSR;
                A64State state;
                state.cpu = &cpu;
                state.fallback = InterpretARM[instrs[count - 1].Info.Kind];
                state.x[29] = u64(&cpu);
                state.w[27] = cpu.CPSR;
                state.w[28] = cpu.Cycles;
                state.Run(words);
                const bool taken = test.instr >> 28 == 14 || (test.instr >> 28 == 0 ? z : !z);
                const u32 result = taken ? test.result : sentinel;
                const u32 cycles = (taken ? test.cycles : 1) + unsigned(test.prefix);
                const bool ok = cpu.R[0] == result && state.w[28] == initialCycles + cycles &&
                    state.w[27] == cpsr && cpu.R[1] == 1 && cpu.R[2] == 1 && cpu.R[3] == 1 &&
                    cpu.R[4] == unsigned(test.prefix) && state.calls == unsigned(test.fallback && taken);
                ++checked;
                failures += !ok;
                std::printf("ARM%u %s Z=%u: %s r0=%08X cycles=%u expected=%08X/%u calls=%u\n",
                    num ? 7 : 9, test.name, unsigned(z), ok ? "PASS" : "FAIL", cpu.R[0],
                    state.w[28] - initialCycles, result, cycles, state.calls);
            }
        }
    }
    std::printf("A64 production CompileBlock conditional cycles: %u cases, %u failures (ISA model; no native A64 execution)\n",
                checked, failures);
    return failures ? 1 : 0;
}
#endif
}

int main(int argc, char** argv)
{
    using A64Test::Compiler;
    using A64Test::Op2;
    using A64Test::A64State;
    const std::string_view filter = argc == 2 ? argv[1] : "all";
#ifdef ARM64_JIT_BLOCK_TEST
#ifdef ARM64_JIT_MUL_TEST
    if (filter == "export-mul-cycles") return A64Test::ExportMultiplyBlocks();
#endif
    if (filter == "conditional-cycles") return A64Test::TestConditionalCycles();
    NDSArgs args;
    auto nds = std::make_unique<NDS>(std::move(args));
#endif
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
#ifdef ARM64_JIT_BLOCK_TEST
                Compiler compiler(*nds);
#else
                Compiler compiler;
#endif
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
