// SPDX-License-Identifier: GPL-3.0-or-later
// ARM register-ROR count, carry and alias semantics in x64 Comp_RegShiftReg.
// Compiles real ARM7/ARM9 JIT blocks, dispatches the cached native entry and compares
// R0-R15, CPSR and guest cycles with the actual interpreter. Scaffolding mirrors
// tests/Optimization161A3.cpp (CompileBlock, ARM_Dispatch, interpreter trace cycles).
// Run with any argument to also print compiled native bytes per program.
#include "Args.h"
#include "NDS.h"
#include "ARM.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace melonDS;
namespace {
void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
struct State {
    std::array<u32, 16> Registers;
    u32 CPSR;
    u64 Cycles;
    bool operator==(const State&) const = default;
};
State Read(ARM& cpu, u64 cycles) {
    State result{};
    std::copy(cpu.R, cpu.R + 16, result.Registers.begin());
    result.CPSR = cpu.CPSR; result.Cycles = cycles;
    return result;
}

// Followers decide which shifter/CPSR bits are still live after the ROR instruction.
struct Follower { const char* Name; std::vector<u32> Code; bool CondDependent; };
const Follower None{"exit-live", {}, false};
const Follower CS{"MOVCS", {0x23A05007}, true};                 // reads incoming C, C stays live to exit
const Follower Adds{"ADDS", {0xE2966001}, false};               // overwrites NZCV: shifter C and NZ dead
const Follower EqAdds{"MOVEQ+ADDS", {0x03A05007, 0xE2966001}, true};  // NZ live, C dead
const Follower CsAdds{"MOVCS+ADDS", {0x23A05007, 0xE2966001}, true};  // C live control

enum Alias { Distinct, RdRm, RdRs, RmRs, All };
struct Program {
    const char* Op; unsigned Opcode; bool S; Alias Alias;
    const Follower* Follow;
    unsigned Rd, Rm, Rs, Rn;
};
Program Make(const char* op, unsigned opcode, bool s, Alias alias, const Follower& f) {
    Program p{op, opcode, s, alias, &f, 4, 0, 1, 3};
    switch (alias) {
    case Distinct: break;
    case RdRm: p.Rd = 0; break;   // rd == rm
    case RdRs: p.Rd = 1; break;   // rd == rs
    case RmRs: p.Rm = 1; break;   // rm == rs
    case All: p.Rd = 1; p.Rm = 1; break;
    }
    return p;
}
const char* AliasName[] = {"distinct", "rd=rm", "rd=rs", "rm=rs", "rd=rm=rs"};
}

int main(int argc, char** argv) {
    try {
        const bool verbose = argc > 1;
        std::vector<Program> programs;
        // Main ROR coverage: MOV over every alias and every live/dead carry shape.
        for (Alias a : {Distinct, RdRm, RdRs, RmRs, All}) {
            programs.push_back(Make("MOV", 0xD, false, a, None));
            programs.push_back(Make("MOV", 0xD, false, a, CS));
            programs.push_back(Make("MOVS", 0xD, true, a, Adds));
            programs.push_back(Make("MOVS", 0xD, true, a, None));
            programs.push_back(Make("MOVS", 0xD, true, a, EqAdds));
            programs.push_back(Make("MOVS", 0xD, true, a, CsAdds));
        }
        // Other ALU consumers of the shifted operand (distinct registers only).
        programs.push_back(Make("MVN", 0xF, false, Distinct, None));
        programs.push_back(Make("MVNS", 0xF, true, Distinct, Adds));
        programs.push_back(Make("MVNS", 0xF, true, Distinct, EqAdds));
        programs.push_back(Make("ADD", 0x4, false, Distinct, None));
        programs.push_back(Make("ADDS", 0x4, true, Distinct, None));
        programs.push_back(Make("TST", 0x8, true, Distinct, None));
        programs.push_back(Make("TST", 0x8, true, Distinct, EqAdds));

        NDSArgs args;
        args.JIT->FastMemory = false;
        args.JIT->LiteralOptimizations = false;
        args.JIT->BranchOptimizations = false;
        auto nds = std::make_unique<NDS>(std::move(args));
        NDS::Current = nds.get();
        unsigned checked = 0;
        const u32 counts[] = {0, 1, 31, 32, 33, 128, 255, 256, 0xFFFFFFFFu};
        const u32 flagsIn[] = {0x90000000u /* N,V,!C */, 0x60000000u /* Z,C,!V */};
        for (unsigned cpuNum = 0; cpuNum < 2; ++cpuNum) {
            nds->Reset();
            nds->CurCPU = cpuNum;
            ARM& cpu = cpuNum ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
            auto& blocks = cpuNum ? nds->JIT.JitBlocks7 : nds->JIT.JitBlocks9;
            unsigned index = 0;
            for (const Program& p : programs) {
                const u32 addr = 0x02008000 + (index++) * 32;
                const u32 key = addr;
                std::vector<u32> code;
                code.push_back(0xE0000000u | p.Opcode << 21 | u32(p.S) << 20 | p.Rn << 16 |
                    (p.Opcode == 0x8 ? 0 : p.Rd) << 12 | p.Rs << 8 | 0x70 | p.Rm); // op Rd,Rn,Rm ROR Rs
                if (p.Opcode == 0xD || p.Opcode == 0xF) code[0] &= ~(0xFu << 16);    // Rn SBZ
                code.insert(code.end(), p.Follow->Code.begin(), p.Follow->Code.end());
                code.push_back(0xEA000000); code.push_back(0xE1A00000); code.push_back(0xEAFFFFFE);
                for (unsigned i = 0; i < code.size(); ++i) nds->ARM9Write32(addr + i * 4, code[i]);

                // Rm-value sweeps only where Rm is independent of the count register.
                std::vector<u32> values{0x80000001u};
                if (p.Alias == Distinct || p.Alias == RdRm) values = {0, 0x80000001u};
                if (p.Alias == RmRs || p.Alias == All) values = {0};

                u64 traceCycles = 0;
                for (u32 value : values)
                for (u32 flags : flagsIn)
                for (u32 count : counts) {
                    auto prepare = [&] {
                        for (unsigned r = 0; r < 15; ++r) cpu.R[r] = 0x12340000 + r;
                        cpu.R[3] = 0x0F0F0F0F; cpu.R[4] = 0xDEADBEEF; cpu.R[5] = 0; cpu.R[6] = 0xFFFFFFFF;
                        cpu.R[p.Rm] = value; cpu.R[p.Rs] = count; // count wins when Rm == Rs
                        cpu.CPSR = 0x000000DF | flags;
                        cpu.StopExecution = 0;
                        cpu.JumpTo(key); cpu.Cycles = 0;
                        (cpuNum ? nds->ARM7Timestamp : nds->ARM9Timestamp) = 0;
                    };
                    prepare();
                    if (!blocks.contains(key)) {
                        nds->JIT.CompileBlock(&cpu);
                        traceCycles = cpu.Cycles;
                        if (verbose)
                            std::printf("SIZE ARM%u %-5s %-8s %-11s %zu\n", cpuNum ? 7 : 9, p.Op, AliasName[p.Alias], p.Follow->Name,
                                size_t(nds->JIT.JITCompiler.GetCodePtr() - reinterpret_cast<const u8*>(blocks.at(key)->EntryPoint)));
                    }
                    Require(blocks.contains(key), "block was not compiled");
                    Require(traceCycles > 0, "tracing interpreter has no executed cycles");
                    const auto entry = blocks.at(key)->EntryPoint;
                    prepare();
                    ARM_Dispatch(&cpu, entry);
                    const State native = Read(cpu, cpu.Cycles);
                    Require(native.Cycles > 0, "block has no executed cycles");
                    prepare();
                    // Conditional followers can change executed-path cycles per input;
                    // then the interpreter runs to the native total and PC/cycles must still match.
                    (cpuNum ? nds->ARM7Target : nds->ARM9Target) = p.Follow->CondDependent ? native.Cycles : traceCycles;
                    if (cpuNum) nds->ARM7.Execute<CPUExecuteMode::Interpreter>();
                    else nds->ARM9.Execute<CPUExecuteMode::Interpreter>();
                    const State reference = Read(cpu, cpuNum ? nds->ARM7Timestamp : nds->ARM9Timestamp);
                    if (!(native == reference)) {
                        std::fprintf(stderr, "ARM%u %s S=%d %s follower=%s rd=r%u rm=r%u rs=r%u value=%08x count=%08x flagsIn=%08x\n"
                            "  native/reference pc=%08x/%08x cpsr=%08x/%08x cycles=%llu/%llu\n",
                            cpuNum ? 7 : 9, p.Op, p.S, AliasName[p.Alias], p.Follow->Name, p.Rd, p.Rm, p.Rs, value, count, flags,
                            native.Registers[15], reference.Registers[15], native.CPSR, reference.CPSR,
                            (unsigned long long)native.Cycles, (unsigned long long)reference.Cycles);
                        for (unsigned r = 0; r < 15; ++r)
                            if (native.Registers[r] != reference.Registers[r])
                                std::fprintf(stderr, "  r%u native=%08x reference=%08x\n", r, native.Registers[r], reference.Registers[r]);
                        throw std::runtime_error("native/interpreter state mismatch");
                    }
                    ++checked;
                }
            }
        }
        std::printf("PASS %u ARM7/ARM9 register-ROR native/interpreter register+CPSR+cycle comparisons\n", checked);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
}
