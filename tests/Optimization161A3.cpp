// SPDX-License-Identifier: GPL-3.0-or-later
// Execute cached native blocks after tracing, and compare all registers, flags
// and cycles with the actual interpreter on exactly the same short program.
#include "Args.h"
#include "NDS.h"
#include "ARM.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
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
}

int main(int argc, char** argv) {
    try {
        const unsigned repeats = argc > 1 ? std::strtoul(argv[1], nullptr, 10) : 0;
        NDSArgs args;
        args.JIT->FastMemory = false;
        args.JIT->LiteralOptimizations = false;
        args.JIT->BranchOptimizations = false;
        auto nds = std::make_unique<NDS>(std::move(args));
        NDS::Current = nds.get();
        unsigned checked = 0;
        for (unsigned cpuNum = 0; cpuNum < 2; ++cpuNum) {
            nds->Reset();
            nds->CurCPU = cpuNum;
            ARM& cpu = cpuNum ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
            auto& blocks = cpuNum ? nds->JIT.JitBlocks7 : nds->JIT.JitBlocks9;
            unsigned program = 0;
            // Register LSL/LSR/ASR/ROR, immediate LSL/LSR/ASR, ARM RRX.
            for (unsigned form = 0; form < 8; ++form)
            for (unsigned mode = 0; mode < 5; ++mode) {
                if (repeats && (mode == 1 || mode == 2 || mode == 3)) continue;
                const bool thumb = form != 7;
                const std::vector<unsigned> counts = form < 4 ?
                    std::vector<unsigned>{0, 1, 31, 32, 33, 255, 256} :
                    form < 7 ? std::vector<unsigned>{0, 1, 31} : std::vector<unsigned>{0};
                for (unsigned count : counts) {
                    if (repeats && count != (form < 7 ? 31u : 0u)) continue;
                    if (repeats) nds->JIT.ResetBlockCache(); // Same entry alignment per timing cell.
                    const u32 addr = 0x02008000 + (program++) * 32;
                    const u32 key = addr | u32(thumb);
                    if (thumb) {
                        const unsigned regOps[] = {2, 3, 4, 7};
                        std::vector<u16> code{u16(form < 4 ?
                            0x4008 | (regOps[form] << 6) : ((form - 4) << 11) | (count << 6))};
                        if (mode == 1 || mode == 2) code.push_back(mode == 1 ? 0x415A : 0x419A); // ADC/SBC r2,r3
                        if (mode <= 2) code.push_back(0x3601); // ADD r6,#1 kills NZCV.
                        if (mode == 3) code.push_back(0x2600); // MOV r6,#0 kills NZ only.
                        code.push_back(0xE000); // forward B, definitive block end.
                        code.push_back(0x46C0); code.push_back(0xE7FE);
                        for (unsigned i = 0; i < code.size(); ++i) nds->ARM9Write16(addr + i * 2, code[i]);
                    } else {
                        std::vector<u32> code{0xE1B00060}; // MOVS r0,r0,RRX
                        if (mode == 1 || mode == 2) code.push_back(mode == 1 ? 0xE0B22003 : 0xE0D22003);
                        if (mode <= 2) code.push_back(0xE2966001);
                        if (mode == 3) code.push_back(0xE3B06000);
                        code.push_back(0xEA000000); code.push_back(0xE1A00000); code.push_back(0xEAFFFFFE);
                        for (unsigned i = 0; i < code.size(); ++i) nds->ARM9Write32(addr + i * 4, code[i]);
                    }
                    u64 traceCycles = 0;
                    for (u32 input : {0u, 1u, 0x80000001u, 0xFFFFFFFFu})
                    for (bool carry : {false, true}) {
                        if (repeats && (input != 0x80000001u || !carry)) continue;
                        auto prepare = [&] {
                            for (unsigned r = 0; r < 15; ++r) cpu.R[r] = 0x12340000 + r;
                            cpu.R[0] = input; cpu.R[1] = count;
                            cpu.R[2] = 0x7FFFFFFF; cpu.R[3] = 0; cpu.R[6] = 0xFFFFFFFF;
                            cpu.CPSR = 0x900000DF | (carry ? 1u << 29 : 0) | (thumb ? 0x20 : 0);
                            cpu.StopExecution = 0;
                            cpu.JumpTo(key); cpu.Cycles = 0;
                            (cpuNum ? nds->ARM7Timestamp : nds->ARM9Timestamp) = 0;
                        };
                        prepare();
                        if (!blocks.contains(key)) {
                            nds->JIT.CompileBlock(&cpu);
                            traceCycles = cpu.Cycles;
                        }
                        Require(blocks.contains(key), "block was not compiled");
                        Require(traceCycles > 0, "tracing interpreter has no executed cycles");
                        const auto entry = blocks.at(key)->EntryPoint;
                        prepare();
                        ARM_Dispatch(&cpu, entry);
                        const State native = Read(cpu, cpu.Cycles);
                        Require(native.Cycles > 0, "block has no executed cycles");
                        prepare();
                        (cpuNum ? nds->ARM7Target : nds->ARM9Target) = traceCycles;
                        if (cpuNum) nds->ARM7.Execute<CPUExecuteMode::Interpreter>();
                        else nds->ARM9.Execute<CPUExecuteMode::Interpreter>();
                        const State reference = Read(cpu, cpuNum ? nds->ARM7Timestamp : nds->ARM9Timestamp);
                        if (!(native == reference)) {
                            std::fprintf(stderr, "ARM%u form=%u mode=%u count=%u value=%08x carry=%d native/reference pc=%08x/%08x flags=%08x/%08x cycles=%llu/%llu\n",
                                cpuNum ? 7 : 9, form, mode, count, input, carry,
                                native.Registers[15], reference.Registers[15], native.CPSR, reference.CPSR,
                                (unsigned long long)native.Cycles, (unsigned long long)reference.Cycles);
                            throw std::runtime_error("native/interpreter state mismatch");
                        }
                        ++checked;
                        if (repeats) {
                            // Dispatch the immutable cached block repeatedly. CPU setup,
                            // compilation and correctness comparisons stay outside timing.
                            prepare();
                            for (unsigned i = 0; i < 2000; ++i) ARM_Dispatch(&cpu, entry);
                            prepare();
                            const auto start = std::chrono::steady_clock::now();
                            for (unsigned i = 0; i < repeats; ++i) ARM_Dispatch(&cpu, entry);
                            const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
                            Require(u64(cpu.Cycles) == native.Cycles * repeats, "native work count mismatch");
                            const auto bytes = nds->JIT.JITCompiler.GetCodePtr() - reinterpret_cast<const u8*>(entry);
                            std::printf("BENCH,%u,%u,%u,%u,%lld,%llu,%zu,%08x,%08x,%08x\n", cpuNum, form, mode, repeats,
                                (long long)ns, (unsigned long long)cpu.Cycles, size_t(bytes), cpu.R[0], cpu.R[6], cpu.CPSR);
                        }
                    }
                }
            }
        }
        std::printf("PASS %u native/interpreter register+CPSR+cycle comparisons\n", checked);
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what()); return 1;
    }
}
