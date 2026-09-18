// SPDX-License-Identifier: GPL-3.0-or-later
#include "Vulkan/Device.h"
#include <cstdio>
#include <algorithm>
int main()
{
    std::string error;
    const auto adapters = melonDS::Vulkan::Device::Enumerate(error);
    if (adapters.empty()) { std::fprintf(stderr, "%s\n", error.c_str()); return 77; }
    for (const auto& adapter : adapters)
    {
        auto device = melonDS::Vulkan::Device::Create(error, adapter.id);
        if (!device || device->Id() != adapter.id) return 1;
        std::printf("Explicit GPU: %s type=%u PASS\n", adapter.name.c_str(), adapter.type);
    }
    auto automatic = melonDS::Vulkan::Device::Create(error);
    if (!automatic) return 2;
    if (std::any_of(adapters.begin(), adapters.end(), [](const auto& a) { return a.type == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU; }) &&
        automatic->Properties().deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) return 3;
    if (melonDS::Vulkan::Device::Create(error, "missing-gpu") || error.empty()) return 4;
    std::printf("Automatic GPU: %s; missing explicit GPU rejected PASS\n", automatic->Properties().deviceName);
    return 0;
}
