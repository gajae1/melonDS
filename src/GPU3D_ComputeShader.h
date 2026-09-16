// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>
#include <stdexcept>

namespace melonDS::ComputeShader {
inline constexpr unsigned Count = 32;
struct Config {
    int ScreenWidth;
    int ScreenHeight;
    int MaxWorkTiles;
    int TileSize;
    int CoarseTileCountY;
    int CoarseTileArea;
    int ClearCoarseBinMaskLocalSize;
};
// Vulkan uses the same 1..16 scale range and tile geometry as GL Compute.
// The build tools and runtime derive their dimensions from this configuration.
inline constexpr int VulkanMaxScale = 16;
inline Config VulkanConfig(int scale)
{
    if (scale < 1 || scale > VulkanMaxScale) throw std::invalid_argument("Vulkan scale must be between 1 and 16");
    const int tile = scale >= 9 ? 32 : scale >= 5 ? 16 : 8;
    const int coarseY = tile < 32 ? 4 : 6;
    const int tiles = (256 * scale / tile) * (192 * scale / tile);
    return {256 * scale, 192 * scale, tiles * 16, tile, coarseY, 8 * coarseY, tile < 32 ? 64 : 48};
}
// The same variants and integer raster math feed both APIs.
std::string BuildSource(unsigned variant, const Config& config, bool vulkan);
}
