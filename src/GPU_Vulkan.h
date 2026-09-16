// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GPU_Soft.h"
#include <string>
#include "GPU3D_ComputeShader.h"
#include <array>
#include <vector>

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
    void Reset() override;
    void Stop() override;
    void DrawScanline(u32 line) override;
    bool GetDisplayFramebuffers(void** top, void** bottom, int& width, int& height) override;

private:
    using DisplayBuffers = std::array<std::array<std::vector<u32>, 2>, 2>;
    DisplayBuffers ScaledBuffers;
    int DisplayScale = 1;
    std::array<u32, 256 * ComputeShader::VulkanMaxScale> ScaledLine3D {};
};
}
