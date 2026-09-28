// SPDX-License-Identifier: GPL-3.0-or-later
// Build tool only; the runtime loads no GLSL or SPIR-V files.
#include "GPU3D_ComputeShader.h"
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    const std::filesystem::path directory(argv[1]);
    for (int scale = 1; scale <= melonDS::ComputeShader::VulkanMaxScale; ++scale)
    {
        const auto scaledDirectory = directory / std::to_string(scale);
        std::filesystem::create_directories(scaledDirectory);
        const auto config = melonDS::ComputeShader::VulkanConfig(scale);
        for (unsigned i = 0; i < melonDS::ComputeShader::Count; ++i)
        {
            const auto path = scaledDirectory / (std::to_string(i) + ".comp");
            const auto source = melonDS::ComputeShader::BuildSource(i, config, true);
            {
                std::ifstream previous(path);
                if (previous && std::string(std::istreambuf_iterator<char>(previous), {}) == source)
                    continue;
            }
            std::ofstream file(path);
            file << source;
            file.close();
            if (!file) return 2;
        }
    }
    return 0;
}
