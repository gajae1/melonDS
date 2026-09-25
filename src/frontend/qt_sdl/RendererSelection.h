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

// Vulkan 3D renders into GPU images that the Vulkan presenter samples on the
// same device; the OpenGL display would read every painted frame back and
// re-upload it. Where that presenter is built (Windows), the Vulkan renderer
// therefore always uses the Vulkan display. Stored Screen.UseGL/UseVulkan
// values still decide for the other renderers.
inline bool RendererImpliesVulkanDisplay(int renderer)
{
#if defined(VULKANRENDERER_ENABLED) && defined(_WIN32)
    return renderer == renderer3D_Vulkan;
#else
    (void)renderer;
    return false;
#endif
}
