// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioLowPass.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <random>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

int main()
{
#ifdef _WIN32
    if (!SetThreadAffinityMask(GetCurrentThread(), 1)) return 2;
#endif
    constexpr int frames = 512, iterations = 2000;
    std::mt19937 rng(12345);
    std::array<int16_t, frames * 2> source, output;
    for (auto& sample : source) sample = static_cast<int16_t>(rng());
    std::vector backends{AudioLowPass::Backend::Scalar, AudioLowPass::Backend::SSE2, AudioLowPass::Backend::FMA};
    std::erase_if(backends, [](auto backend) { return !AudioLowPass::IsSupported(backend); });
    std::puts("round,backend,ns_per_block_copy_and_filter,checksum");
    for (int round = 0; round < 9; ++round)
    {
        std::shuffle(backends.begin(), backends.end(), rng);
        for (auto backend : backends)
        {
            AudioLowPass filter;
            filter.Init(48000, backend);
            filter.SetCutoffNow(6000);
            for (int i = 0; i < 16; ++i)
            {
                output = source;
                filter.Process(output.data(), frames, 6000, frames / 48000.0);
            }
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < iterations; ++i)
            {
                output = source;
                filter.Process(output.data(), frames, 6000, frames / 48000.0);
            }
            const double ns = std::chrono::duration<double, std::nano>(
                std::chrono::steady_clock::now() - start).count() / iterations;
            uint64_t checksum = 0;
            for (auto sample : output) checksum = checksum * 31 + static_cast<uint16_t>(sample);
            std::printf("%d,%d,%.3f,%llu\n", round, int(backend), ns, (unsigned long long)checksum);
        }
    }
}
