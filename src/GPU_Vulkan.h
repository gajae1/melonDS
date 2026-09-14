// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GPU_Soft.h"
#include <string>

namespace melonDS
{
// Vulkan rasterizes 3D; the established scanline compositor handles 2D, FIFO,
// brightness and capture in native RAM, including CPU access to capture VRAM.
class VulkanRenderer final : public SoftRenderer
{
public:
    explicit VulkanRenderer(NDS& nds);
    static bool IsAvailable(std::string& error);
    bool SetRenderSettings(RendererSettings& settings) override;
    bool HasRenderFailure() const override;
};
}
