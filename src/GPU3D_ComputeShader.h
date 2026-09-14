// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <string>

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
// The same variants and integer raster math feed both APIs.
std::string BuildSource(unsigned variant, const Config& config, bool vulkan);
}
