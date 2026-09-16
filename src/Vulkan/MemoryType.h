// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vulkan/vulkan_core.h>
#include <stdexcept>
namespace melonDS::Vulkan {
// Preferences never relax the required flags or the buffer's allowed type mask.
// With no preference, preserve the driver's original first-compatible order.
inline uint32_t SelectMemoryType(const VkPhysicalDeviceMemoryProperties& properties,
    uint32_t bits, VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred = 0)
{
    uint32_t fallback = VK_MAX_MEMORY_TYPES;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const auto flags = properties.memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i)) || (flags & required) != required) continue;
        if ((flags & preferred) == preferred) return i;
        if (fallback == VK_MAX_MEMORY_TYPES) fallback = i;
    }
    if (fallback != VK_MAX_MEMORY_TYPES) return fallback;
    throw std::runtime_error("Required Vulkan memory type unavailable");
}
}
