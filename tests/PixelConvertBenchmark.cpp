// SPDX-License-Identifier: GPL-3.0-or-later
#include "PixelConvert.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

int main()
{
    using namespace melonDS;
    using namespace PixelConvert;
#ifdef _WIN32
    if (!SetThreadAffinityMask(GetCurrentThread(), 1)) return 2;
#endif
    constexpr size_t count = 256 * 192;
    constexpr int iterations = 2000;
    std::vector<u32> source(count), output(count);
    std::mt19937 rng(12345);
    for (auto& pixel : source) pixel = rng() & 0x003F3F3F;
    std::vector<Backend> backends{Backend::Scalar, Backend::AVX2, Backend::AVX512, Backend::AVX512F};
    std::erase_if(backends, [](Backend backend) { return !IsSupported(backend); });
    for (auto backend : backends) {
        std::memcpy(output.data(), source.data(), count * sizeof(u32));
        Select(backend)(output.data(), count);
    }
    std::puts("round,backend,ns_per_frame_copy_and_convert,checksum");
    for (int round = 0; round < 9; ++round) {
        std::shuffle(backends.begin(), backends.end(), rng);
        for (auto backend : backends) {
            const auto fn = Select(backend);
            const auto start = std::chrono::steady_clock::now();
            for (int i = 0; i < iterations; ++i) {
                std::memcpy(output.data(), source.data(), count * sizeof(u32));
                fn(output.data(), count);
            }
            const double ns = std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/iterations;
            u64 checksum = 0;
            for (auto pixel : output) checksum = checksum * 31 + pixel;
            std::printf("%d,%d,%.3f,%llu\n", round, int(backend), ns, (unsigned long long)checksum);
        }
    }
}
