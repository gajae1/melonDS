// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

// Stable persisted IDs, including builds without one of the optional backends.
enum
{
    renderer3D_Software = 0,
    renderer3D_OpenGL = 1,
    renderer3D_OpenGLCompute = 2,
    renderer3D_Vulkan = 3,
    renderer3D_Max,
};

inline bool RendererUsesOpenGL(int renderer)
{
    return renderer == renderer3D_OpenGL || renderer == renderer3D_OpenGLCompute;
}

// Prefer native Vulkan presentation for Vulkan rendering on Windows. When the
// required WSI features are missing, the panel may use GL external images on
// the same device, then RAM as the compatibility fallback. Stored display
// preferences still decide for the other renderers.
inline bool RendererImpliesVulkanDisplay(int renderer)
{
#if defined(VULKANRENDERER_ENABLED) && defined(_WIN32)
    return renderer == renderer3D_Vulkan;
#else
    (void)renderer;
    return false;
#endif
}
