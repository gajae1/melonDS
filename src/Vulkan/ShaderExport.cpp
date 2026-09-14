// SPDX-License-Identifier: GPL-3.0-or-later
// Build tool only; the runtime loads no GLSL or SPIR-V files.
#include "GPU3D_ComputeShader.h"
#include <filesystem>
#include <fstream>

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    const std::filesystem::path directory(argv[1]);
    for (int scale = 1; scale <= 3; ++scale)
    {
        const auto scaledDirectory = directory / std::to_string(scale);
        std::filesystem::create_directories(scaledDirectory);
        const auto config = melonDS::ComputeShader::VulkanConfig(scale);
        for (unsigned i = 0; i < melonDS::ComputeShader::Count; ++i)
        {
            std::ofstream file(scaledDirectory / (std::to_string(i) + ".comp"));
            file << melonDS::ComputeShader::BuildSource(i, config, true);
            if (!file) return 2;
        }
    }
    return 0;
}
