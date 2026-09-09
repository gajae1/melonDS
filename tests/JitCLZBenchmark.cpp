// SPDX-License-Identifier: GPL-3.0-or-later
// A synthetic guest loop, not a game FPS benchmark.
#include "Args.h"
#include "NDS.h"
#include "jit/CPUDetect.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <memory>
#include <random>
#ifdef _WIN32
#include <windows.h>
#endif

int main()
{
    using namespace melonDS;
    if (!cpu_info.bLZCNT) return 77;
#ifdef _WIN32
    if (!SetThreadAffinityMask(GetCurrentThread(), 1)) return 2;
#endif
    std::array<std::unique_ptr<NDS>, 2> machines;
    for (int mode = 0; mode < 2; ++mode)
    {
        cpu_info.bLZCNT = mode != 0;
        NDSArgs args;
        args.JIT->FastMemory = false;
        machines[mode] = std::make_unique<NDS>(std::move(args));
        auto& nds = *machines[mode];
        nds.Reset();
        // Vary the CLZ input using x = 9*x + 1; accumulate results in r3.
        constexpr u32 code[] = {0xE16F2F11, 0xE0833002, 0xE0811181,
                                0xE2811001, 0xE2500001, 0x1AFFFFF9, 0xEAFFFFFE};
        for (unsigned i = 0; i < std::size(code); ++i)
            nds.ARM9Write32(0x02000800 + i * 4, code[i]);
        nds.ARM9Write32(0x02000200, 0xEAFFFFFE);
        nds.ARM9.R[0] = 0x10000000;
        nds.ARM9.R[1] = 12345;
        nds.ARM9.R[3] = 0;
        nds.ARM9.JumpTo(0x02000800);
        nds.ARM7.JumpTo(0x02000200);
        nds.Start();
        for (int i = 0; i < 16; ++i) nds.RunFrame();
    }
    std::mt19937 rng(12345);
    std::array order{0, 1};
    std::puts("round,lzcnt,ns_per_emulated_frame,checksum");
    for (int round = 0; round < 9; ++round)
    {
        std::array<u32, 2> checksum;
        std::shuffle(order.begin(), order.end(), rng);
        for (int mode : order)
        {
            cpu_info.bLZCNT = mode != 0;
            auto& nds = *machines[mode];
            const auto begin = std::chrono::steady_clock::now();
            for (int i = 0; i < 32; ++i) nds.RunFrame();
            const double ns = std::chrono::duration<double, std::nano>(
                std::chrono::steady_clock::now() - begin).count() / 32;
            checksum[mode] = nds.ARM9.R[3];
            std::printf("%d,%d,%.3f,%u\n", round, mode, ns, checksum[mode]);
        }
        if (checksum[0] != checksum[1]) return 1;
    }
    cpu_info.bLZCNT = true;
}
