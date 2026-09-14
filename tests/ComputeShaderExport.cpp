// SPDX-License-Identifier: GPL-3.0-or-later
#include "GPU3D_ComputeShader.h"
#include <filesystem>
#include <fstream>
#include <cstdio>

int main(int argc, char** argv)
{
    if (argc != 2) return 1;
    const std::filesystem::path directory(argv[1]);
    std::filesystem::create_directories(directory);
    const melonDS::ComputeShader::Config config{256,192,12288,8,4,32,64};
    for (unsigned i = 0; i < melonDS::ComputeShader::Count; ++i)
    {
        std::ofstream file(directory / (std::to_string(i) + ".comp"));
        file << melonDS::ComputeShader::BuildSource(i,config,true);
        if (!file) return 2;
    }
    std::puts("Exported 32 native-resolution compute stages for Vulkan compilation");
    return 0;
}
