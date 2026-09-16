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
    void ClearPipelineCache();
    void Reset() override;
    void Stop() override;
    void DrawScanline(u32 line) override;
    void AllocCapture(u32 bank, u32 start, u32 size) override;
    void InvalidateDisplayCapture(u32 bank, u32 start) override { DisplayCaptures[bank * 4 + start] = {}; }
    bool GetDisplayFramebuffers(void** top, void** bottom, int& width, int& height) override;

private:
    using DisplayBuffers = std::array<std::array<std::vector<u32>, 2>, 2>;
    DisplayBuffers ScaledBuffers;
    int DisplayScale = 1;
    struct DisplayCapture
    {
        u32 width = 0, height = 0, scale = 1, start = 0;
        std::array<bool, 192> valid{};
        std::vector<u16> pixels;
    };
    std::array<DisplayCapture, 16> DisplayCaptures;
    // A whole scaled row is prepared before replacing a capture, including
    // when source B aliases the destination bank.
    std::array<u16, 256 * ComputeShader::VulkanMaxScale * ComputeShader::VulkanMaxScale> CaptureRow{};
    void DoCapture(u32 line) override;
    bool ReadDisplayCapture(u32 bank, u32 word, u32 subx, u32 suby, u16& color) const;
    bool DrawCapturedDisplay(u32 line);
    u32 CaptureBackgroundScale(u32 engine) const override;
    bool SampleCapturedBackground(u32 engine, u32 address, u32 fracX,
        u32 fracY, u32 denominator, u16& color) const override;
    void GetCaptureDisplay3DLine(u32 line, u32 subline, u32 scale, u32* dst) const override;

    std::array<u32, 256 * ComputeShader::VulkanMaxScale> ScaledLine3D {};
};
}
