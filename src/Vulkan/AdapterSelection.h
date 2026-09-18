// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <string>
namespace melonDS::Vulkan {
// VkPhysicalDeviceType values; no loader dependency for the Qt presenter.
inline int AdapterRank(int type) { return type == 2 ? 2 : type == 1 ? 1 : 0; }
inline std::string AdapterId(const std::uint8_t* uuid)
{
    constexpr char digits[] = "0123456789abcdef";
    std::string result;
    for (int i = 0; i < 16; ++i)
    {
        result += digits[uuid[i] >> 4];
        result += digits[uuid[i] & 15];
    }
    return result;
}
}
