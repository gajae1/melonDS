// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioLowPass.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

static bool VerifyRounding()
{
    // These impulses leave the next sample close to half an output LSB.
    // Adding 0.5 before truncating can round the double below 0.5 up to 1.0.
    // Use the scalar filter as the oracle, including its retained state.
    struct Fixture { double cutoff, impulse; };
    constexpr Fixture fixtures[] = {
        {1111, 0x1.6058495763adep+11}, {1444, 0x1.0823d9c8fbeb2p+10},
        {1518, 0x1.b706b58e6630ap+9}, {1629, 0x1.5278de30c86cp+9},
        {1666, 0x1.37adcb18a7b7dp+9}, {1703, 0x1.1f91fd7bc86f6p+9},
    };
    bool passed = true;
    for (auto backend : {AudioLowPass::Backend::SSE2, AudioLowPass::Backend::FMA})
    {
        const int tolerance = backend == AudioLowPass::Backend::FMA &&
                              AudioLowPass::IsSupported(backend) ? 1 : 0;
        int maxDifference = 0;
        for (const auto& fixture : fixtures)
        for (int neighbor = -2; neighbor <= 2; ++neighbor)
        {
            double impulse = fixture.impulse;
            for (int i = 0; i < std::abs(neighbor); ++i)
                impulse = std::nextafter(impulse, neighbor < 0 ? 0.0 : INFINITY);
            AudioLowPass reference, candidate;
            reference.Init(48000, AudioLowPass::Backend::Scalar);
            candidate.Init(48000, backend);
            for (auto* filter : {&reference, &candidate})
            {
                filter->SetCutoffNow(fixture.cutoff);
                filter->ProcessSample(impulse, 0);
                filter->ProcessSample(-impulse, 1);
            }
            std::array<int16_t, 4> expected{12345, 0, 0, -12345}, actual = expected;
            reference.Process(expected.data() + 1, 1, fixture.cutoff, 1.0 / 48000);
            candidate.Process(actual.data() + 1, 1, fixture.cutoff, 1.0 / 48000);
            passed &= actual.front() == expected.front() && actual.back() == expected.back();
            for (int ch = 1; ch <= 2; ++ch)
                maxDifference = std::max(maxDifference, std::abs(int(actual[ch]) - int(expected[ch])));
        }
        passed &= maxDifference <= tolerance;
        std::printf("Audio rounding backend=%d supported=%d max_difference=%d tolerance=%d: %s\n",
                    int(backend), AudioLowPass::IsSupported(backend), maxDifference, tolerance,
                    maxDifference <= tolerance ? "PASS" : "FAIL");
    }
    return passed;
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--verify") == 0)
        return VerifyRounding() ? 0 : 1;
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
