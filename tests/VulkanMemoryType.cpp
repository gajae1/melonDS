// SPDX-License-Identifier: GPL-3.0-or-later
#include "Vulkan/MemoryType.h"
#include <cstdio>
using namespace melonDS::Vulkan;
int main() {
    const auto host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    const auto cached = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    VkPhysicalDeviceMemoryProperties p{}; p.memoryTypeCount = 4;
    p.memoryTypes[0].propertyFlags = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    p.memoryTypes[1].propertyFlags = host;
    p.memoryTypes[2].propertyFlags = host | cached;
    p.memoryTypes[3].propertyFlags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | cached;
    try {
        if (SelectMemoryType(p, 15, host, cached) != 2) throw std::runtime_error("cached readback memory not preferred");
        if (SelectMemoryType(p, 15, host) != 1) throw std::runtime_error("default upload policy changed");
        if (SelectMemoryType(p, 2, host, cached) != 1) throw std::runtime_error("missing cache fallback failed");
        p.memoryTypes[2].propertyFlags &= ~cached;
        if (SelectMemoryType(p, 15, host, cached) != 1) throw std::runtime_error("unavailable preference rejected required memory");
        for (uint32_t bits : {0u, 1u, 8u}) {
            bool rejected = false;
            try { (void)SelectMemoryType(p, bits, host, cached); }
            catch (const std::runtime_error&) { rejected = true; }
            if (!rejected) throw std::runtime_error("required coherence/visibility or allowed bits ignored");
        }
        std::puts("Vulkan memory preference: cached, fallback, required properties and allowed masks PASS");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
}
