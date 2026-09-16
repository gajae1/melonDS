// SPDX-License-Identifier: GPL-3.0-or-later
#include "Vulkan/ComputeResources.h"
#include <cstdio>
#include <numeric>
#include <stdexcept>
using namespace melonDS::Vulkan;
static void Require(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static VkPhysicalDeviceLimits Limits()
{
    VkPhysicalDeviceLimits l{};
    l.maxImageDimension2D = 16384; l.maxTexelBufferElements = 1u << 27;
    l.maxStorageBufferRange = 0x7FFFFFFF; l.maxUniformBufferRange = 65536; l.maxPushConstantsSize = 128;
    l.maxComputeWorkGroupInvocations = 1024;
    for (int i = 0; i < 3; ++i) { l.maxComputeWorkGroupSize[i] = 1024; l.maxComputeWorkGroupCount[i] = 65535; }
    return l;
}
static void Reject(int scale, const VkPhysicalDeviceLimits& limits)
{
    bool rejected = false;
    try { ComputeResources r(scale, limits); }
    catch (const std::runtime_error&) { rejected = true; }
    Require(rejected, "inadequate Vulkan device limit accepted");
}
int main()
{
    try
    {
        for (int scale = 1; scale <= 16; ++scale)
        {
            const auto limits = Limits(); const ComputeResources r(scale, limits);
            Require(r.BatchWork >= r.Tiles && r.BatchWork <= limits.maxComputeWorkGroupCount[2], "unbounded tile work");
            Require(r.MaxSpans >= uint32_t(r.config.ScreenHeight) && r.MaxSpans % 32 == 0, "span tail is not bounded");
            Require(r.MaxSpans <= (r.BatchWork / (r.config.ScreenWidth / r.config.TileSize) * r.config.TileSize), "unreachable span allocation");
            auto bad = limits; bad.maxComputeWorkGroupInvocations = r.config.TileSize * r.config.TileSize - 1; Reject(scale, bad);
            bad = limits; bad.maxComputeWorkGroupSize[1] = r.config.TileSize - 1; Reject(scale, bad);
            bad = limits; bad.maxComputeWorkGroupSize[0] = r.config.TileSize - 1; Reject(scale, bad);
            bad = limits; bad.maxComputeWorkGroupCount[0] = 1; Reject(scale, bad);
            bad = limits; bad.maxComputeWorkGroupCount[1] = r.config.ScreenHeight - 1; Reject(scale, bad);
            bad = limits; bad.maxComputeWorkGroupCount[2] = r.Tiles - 1; Reject(scale, bad);
            bad = limits; bad.maxImageDimension2D = r.config.ScreenWidth - 1; Reject(scale, bad);
            bad = limits; bad.maxStorageBufferRange = r.Sizes[6] - 1; Reject(scale, bad);
            bad = limits; bad.maxUniformBufferRange = r.Sizes[9] - 1; Reject(scale, bad);
            bad = limits; bad.maxTexelBufferElements = r.config.ScreenHeight - 1; Reject(scale, bad);
            bad = limits; bad.maxPushConstantsSize = 23; Reject(scale, bad);
            bad = limits; bad.maxComputeWorkGroupCount[2] = r.Tiles;
            const ComputeResources onePolygon(scale, bad);
            Require(onePolygon.BatchWork == r.Tiles, "limited device did not bound batch work");
            const auto bytes = std::accumulate(r.Sizes.begin(), r.Sizes.end(), VkDeviceSize{});
            std::printf("scale=%d tile=%d batch=%u spans=%u buffer_bytes=%llu scratch_bytes=%llu\n",
                scale, r.config.TileSize, r.BatchWork, r.MaxSpans,
                (unsigned long long)bytes, (unsigned long long)(3 * r.Sizes[3]));
        }
        std::puts("Vulkan resource limits: 16 accepted, 176 rejected, 16 bounded-device cases PASS");
        return 0;
    }
    catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
