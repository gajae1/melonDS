// SPDX-License-Identifier: GPL-3.0-or-later
// The handler body is extracted from the production source at build time.
#include "types.h"
#include <bit>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>

using namespace melonDS;
struct ARM
{
    u32 Num = 0, R[16]{}, CurInstr = 0xE16F3F11, CPSR = 0xA000001F;
    u64 cycles = 0, undefined = 0;
    void AddCycles_C() { ++cycles; }
};
static void A_UNK(ARM* cpu) { ++cpu->undefined; }
#include "CLZMethod.inc"

static unsigned Reference(u32 value)
{
    unsigned result = 0;
    for (u32 bit = 0x80000000; bit && !(value & bit); bit >>= 1) ++result;
    return result;
}

int main(int argc, char** argv)
{
    u32 corpus[8192];
    u32 state = 0x12345678;
    for (size_t i = 0; i < std::size(corpus); ++i)
    {
        state ^= state << 13; state ^= state >> 17; state ^= state << 5;
        corpus[i] = i % 33 == 32 ? 0 : state >> (i % 33);
    }
    ARM cpu;
    if (argc == 2 && std::strcmp(argv[1], "benchmark") == 0)
    {
        u64 sum = 0;
        const auto begin = std::chrono::steady_clock::now();
        for (unsigned repeat = 0; repeat < 2048; ++repeat)
            for (const u32 value : corpus)
            {
                cpu.R[1] = value;
                A_CLZ(&cpu);
                sum += cpu.R[3];
            }
        const auto ns = std::chrono::duration<double, std::nano>(std::chrono::steady_clock::now() - begin).count();
        std::printf("%.6f %llu\n", ns / (2048 * std::size(corpus)), static_cast<unsigned long long>(sum));
        return 0;
    }
    for (const u32 value : corpus)
    {
        cpu.R[1] = value;
        const u64 before = cpu.cycles;
        A_CLZ(&cpu);
        if (cpu.R[3] != Reference(value) || cpu.R[1] != value ||
            cpu.cycles != before + 1 || cpu.CPSR != 0xA000001F || cpu.undefined) return 1;
    }
    for (unsigned i = 0; i < 32; ++i)
    {
        cpu.R[1] = u32{1} << i;
        A_CLZ(&cpu);
        if (cpu.R[3] != 31 - i) return 2;
    }
    cpu.R[1] = 0;
    A_CLZ(&cpu);
    if (cpu.R[3] != 32) return 3;
    cpu.Num = 1;
    const auto before = cpu.cycles;
    A_CLZ(&cpu);
    if (cpu.R[3] != 32 || cpu.cycles != before || cpu.undefined != 1) return 4;
    puts("ARM CLZ values, zero, flags, tick call and ARM7 guard: PASS");
}
