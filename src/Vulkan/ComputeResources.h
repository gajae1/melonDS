// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Device.h"
#include "GPU3D_ComputeData.h"
#include "GPU3D_ComputeShader.h"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace melonDS::Vulkan {
// Checked before allocating buffers or creating shader pipelines.
// Scratch storage is bounded by the work that a batch can dispatch.
struct ComputeResources {
    ComputeShader::Config config;
    uint32_t Pixels, Tiles, Work, BatchWork, MaxSpans;
    std::array<VkDeviceSize, 11> Sizes;
    ComputeResources(int scale, const VkPhysicalDeviceLimits& limits)
        : config(ComputeShader::VulkanConfig(scale)),
          Pixels(config.ScreenWidth * config.ScreenHeight),
          Tiles(Pixels / (config.TileSize * config.TileSize)), Work(config.MaxWorkTiles),
          BatchWork(uint32_t(std::min({uint64_t(Work), uint64_t(limits.maxComputeWorkGroupCount[2]),
              uint64_t(limits.maxComputeWorkGroupCount[0]) * 32,
              uint64_t(limits.maxStorageBufferRange) / (config.TileSize * config.TileSize * 4)}))),
          // Each accepted scanline costs a full tile row in the batch bound.
          MaxSpans(uint32_t(std::min({uint64_t(BatchWork) / (config.ScreenWidth / config.TileSize) * config.TileSize,
              uint64_t(limits.maxComputeWorkGroupCount[0]) * 32,
              uint64_t(limits.maxTexelBufferElements),
              uint64_t(limits.maxStorageBufferRange) / sizeof(ComputeData::SpanSetupX)})) & ~31u),
          Sizes{2048 * sizeof(ComputeData::RenderPolygon), VkDeviceSize(MaxSpans) * sizeof(ComputeData::SpanSetupX),
              12288 * sizeof(ComputeData::SpanSetupY),
              VkDeviceSize(BatchWork) * config.TileSize * config.TileSize * 4,
              VkDeviceSize(BatchWork) * config.TileSize * config.TileSize * 4,
              VkDeviceSize(BatchWork) * config.TileSize * config.TileSize * 4,
              VkDeviceSize(Pixels) * 7 * 4,
              sizeof(ComputeData::BinResultHeader) + VkDeviceSize(Tiles) * (2 + 64 + 64) * 4,
              VkDeviceSize(Work) * 16, sizeof(ComputeData::MetaUniform),
              VkDeviceSize(MaxSpans) * sizeof(ComputeData::SetupIndices)}
    {
        const uint32_t localX = std::max({32, config.TileSize, config.CoarseTileArea,
            config.ClearCoarseBinMaskLocalSize});
        if (limits.maxComputeWorkGroupInvocations < uint32_t(std::max(config.TileSize * config.TileSize, int(localX))) ||
            limits.maxComputeWorkGroupSize[0] < localX || limits.maxComputeWorkGroupSize[1] < uint32_t(config.TileSize))
            throw std::runtime_error("Compute scale exceeds local workgroup limits");
        if (limits.maxStorageBufferRange < *std::max_element(Sizes.begin(), Sizes.begin() + 9) ||
            limits.maxUniformBufferRange < Sizes[9] || limits.maxPushConstantsSize < 24 ||
            limits.maxImageDimension2D < uint32_t(config.ScreenWidth) ||
            limits.maxImageDimension2D < uint32_t(config.ScreenHeight))
            throw std::runtime_error("Compute scale exceeds image or buffer limits");
        if (BatchWork < Tiles || MaxSpans < uint32_t(config.ScreenHeight) ||
            limits.maxComputeWorkGroupCount[0] < std::max({64u, Tiles / uint32_t(config.ClearCoarseBinMaskLocalSize),
                uint32_t(config.ScreenWidth / config.TileSize)}) ||
            limits.maxComputeWorkGroupCount[1] < uint32_t(config.ScreenHeight))
            throw std::runtime_error("Compute scale exceeds dispatch limits");
    }
};
}
