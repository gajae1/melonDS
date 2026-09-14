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
