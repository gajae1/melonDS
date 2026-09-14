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
// Vulkan embeds these three configurations at build time. Keep the shader
// dimensions and runtime resource bounds sourced from the same configuration.
inline Config VulkanConfig(int scale)
{
    if (scale < 1 || scale > 3) throw std::invalid_argument("Vulkan scale must be 1, 2 or 3");
    return {256 * scale, 192 * scale, 12288 * scale * scale, 8, 4, 32, 64};
}
// The same variants and integer raster math feed both APIs.
std::string BuildSource(unsigned variant, const Config& config, bool vulkan);
}
