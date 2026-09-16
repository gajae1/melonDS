// SPDX-License-Identifier: GPL-3.0-or-later
#include "GPU3D_ComputeShader.h"
#include <cstdio>
#include <stdexcept>
int main()
{
    try
    {
        for (int scale = 1; scale <= 16; ++scale)
        {
            const auto config = melonDS::ComputeShader::VulkanConfig(scale);
            const int tiles = config.ScreenWidth / config.TileSize * (config.ScreenHeight / config.TileSize);
            if (config.ScreenWidth != 256 * scale || config.ScreenHeight != 192 * scale ||
                config.ScreenWidth % (8 * config.TileSize) ||
                config.ScreenHeight % (config.CoarseTileCountY * config.TileSize) ||
                tiles % config.ClearCoarseBinMaskLocalSize || config.CoarseTileArea != 8 * config.CoarseTileCountY ||
                config.MaxWorkTiles < tiles || tiles > 65535 || config.TileSize * config.TileSize > 1024)
                throw std::runtime_error("invalid scalable compute grid");
        }
        for (int invalid : {-1, 0, 17, 2147483647})
        {
            bool rejected = false;
            try { (void)melonDS::ComputeShader::VulkanConfig(invalid); }
            catch (const std::invalid_argument&) { rejected = true; }
            if (!rejected) throw std::runtime_error("invalid scale accepted");
        }
        std::puts("Vulkan scale config 1..16, exact dispatch grids and invalid inputs PASS");
        return 0;
    }
    catch (const std::exception& error) { std::fprintf(stderr, "%s\n", error.what()); return 1; }
}
