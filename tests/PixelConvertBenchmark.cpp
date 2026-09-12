// SPDX-License-Identifier: GPL-3.0-or-later
//
// Two workloads per frame per supported kernel: one whole-frame call (frame,
// 49152 pixels) and 192 successive 256-pixel calls (scanline192), matching the
// soft renderer's per-line ExpandPixels(dst, 256). Both memcpy the same
// generated 256x192 RGB666 frame first (generated data, not a captured
// framebuffer), so the copy cost is equal and ns_per_frame_copy_and_convert is
// comparable. Preflight compares both shapes exactly against independently
// selected Scalar; no check runs in a measured loop, a timed mismatch exits
// nonzero, and --verify-only runs just the preflight.
#include "PixelConvert.h"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace melonDS::PixelConvert;
using melonDS::u32;
using melonDS::u64;

namespace
{
constexpr size_t kWidth = 256;
constexpr size_t kHeight = 192;
constexpr size_t kFramePixels = kWidth * kHeight;
constexpr int kIterations = 2000;
constexpr int kRounds = 9;
constexpr int kSeed = 12345; // same seed as the previous benchmark

struct Task { const char* name; bool scanline; Backend backend; };

const char* BackendName(Backend backend)
{
    // Enum order is fixed by PixelConvert.h and asserted in tests/PixelConvert.cpp.
    static constexpr const char* names[] = {"Auto", "Scalar", "AVX2", "AVX512", "AVX512F", "NEON"};
    const size_t index = size_t(backend);
    return index < sizeof(names) / sizeof(names[0]) ? names[index] : "?";
}

u64 Checksum(const std::vector<u32>& pixels)
{
    u64 checksum = 0;
    for (const auto pixel : pixels) checksum = checksum * 31 + pixel;
    return checksum;
}

// One frame-sized memcpy of the shared input, then the workload's calls.
void Convert(const Function fn, const std::vector<u32>& input, std::vector<u32>& output, bool scanline)
{
    std::memcpy(output.data(), input.data(), kFramePixels * sizeof(u32));
    if (!scanline) { fn(output.data(), kFramePixels); return; }
    for (size_t y = 0; y < kHeight; ++y) fn(output.data() + y * kWidth, kWidth);
}

double Time(const Function fn, const std::vector<u32>& input, std::vector<u32>& output, bool scanline)
{
    const auto start = std::chrono::steady_clock::now();
    for (int i = 0; i < kIterations; ++i) Convert(fn, input, output, scanline);
    return std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/kIterations;
}

// Independently selected Scalar output for either shape, used by the preflight
// and by the timed-output check.
std::vector<u32> ScalarReference(const std::vector<u32>& source, bool scanline)
{
    std::vector<u32> work(kFramePixels);
    Convert(Select(Backend::Scalar), source, work, scanline);
    return work;
}
}

int main(int argc, char** argv)
{
    using namespace melonDS;
#ifdef _WIN32
    if (!SetThreadAffinityMask(GetCurrentThread(), 1)) return 2;
#endif
    bool verifyOnly = false;
    for (int i = 1; i < argc; ++i) verifyOnly |= std::strcmp(argv[i], "--verify-only") == 0;

    std::vector<Backend> backends{Backend::Scalar, Backend::AVX2, Backend::AVX512, Backend::AVX512F, Backend::NEON};
    std::erase_if(backends, [](Backend backend) { return !IsSupported(backend); });

    std::mt19937 rng(kSeed);
    std::vector<u32> source(kFramePixels), output(kFramePixels), work(kFramePixels);
    for (auto& pixel : source) pixel = rng() & 0x003F3F3F;

    // Shape-specific preflight against Scalar: full-vector equality for frame
    // and scanline192, for every supported backend, before any timing.
    const auto wholeRef = ScalarReference(source, false);
    const auto scanlineRef = ScalarReference(source, true);
    for (const auto backend : backends)
    {
        for (const bool lines : {false, true})
        {
            Convert(Select(backend), source, work, lines);
            if (work == (lines ? scanlineRef : wholeRef)) continue;
            std::fprintf(stderr, "preflight mismatch %s: %s\n", lines ? "scanline192" : "frame", BackendName(backend));
            return 3;
        }
    }
    if (verifyOnly)
    {
        std::printf("verify-only: %u backends match Scalar exactly for frame and scanline192\n", unsigned(backends.size()));
        return 0;
    }

    std::vector<Task> tasks;
    for (const auto backend : backends)
        for (const bool scanline : {false, true})
            tasks.push_back({scanline ? "scanline192" : "frame", scanline, backend});

    std::puts("round,workload,backend,backend_name,ns_per_frame_copy_and_convert,checksum");
    bool ok = true;
    for (int round = 0; round < kRounds; ++round)
    {
        // Shuffle workloads and backends together so no position is fixed per round.
        std::shuffle(tasks.begin(), tasks.end(), rng);
        for (const auto& task : tasks)
        {
            const double ns = Time(Select(task.backend), source, output, task.scanline);
            if (output != (task.scanline ? scanlineRef : wholeRef))
            {
                std::fprintf(stderr, "timed mismatch %s: %s\n", task.name, BackendName(task.backend));
                ok = false;
            }
            std::printf("%d,%s,%d,%s,%.3f,%llu\n", round, task.name, int(task.backend), BackendName(task.backend),
                        ns, (unsigned long long)Checksum(output));
        }
    }
    return ok ? 0 : 3;
}