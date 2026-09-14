// SPDX-License-Identifier: GPL-3.0-or-later
// Build tool only; the runtime loads no GLSL or SPIR-V files.
#include "GPU3D_ComputeShader.h"
#include <filesystem>
#include <fstream>

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    const melonDS::ComputeShader::Config config{256, 192, 12288, 8, 4, 32, 64};
    for (unsigned i = 0; i < melonDS::ComputeShader::Count; ++i)
    {
        std::ofstream file(directory / (std::to_string(i) + ".comp"));
        file << melonDS::ComputeShader::BuildSource(i, config, true);
        if (!file) return 2;
    }
    return 0;
}
