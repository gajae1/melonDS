// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "GPU_Vulkan.h"
#include "GPU3D_Vulkan.h"
#include "NDS.h"
#include "GPU_ColorOp.h"
#include "Platform.h"
#include <algorithm>

namespace melonDS
{
VulkanRenderer::VulkanRenderer(NDS& nds, const std::string& preferred)
    : SoftRenderer(nds, std::make_unique<VulkanRenderer3D>(*this, nds.GPU.GPU3D, preferred))
{
}

bool VulkanRenderer::IsAvailable(std::string& error)
{
    return bool(Vulkan::Device::Create(error));
}

std::string VulkanRenderer::DeviceName() const
{
    return static_cast<const VulkanRenderer3D&>(*Rend3D).DeviceName();
}

u64 VulkanRenderer::SubmissionCount() const
{
    return static_cast<const VulkanRenderer3D&>(*Rend3D).SubmissionCount();
}

bool VulkanRenderer::SetRenderSettings(RendererSettings& settings)
{
    const int scale = settings.ScaleFactor;
    if (scale < 1 || scale > ComputeShader::VulkanMaxScale) return false;
    try
    {
        DisplayBuffers next;
        if (scale != DisplayScale && scale > 1)
        {
            const int width = 256 * scale, height = 192 * scale;
            const int oldWidth = 256 * DisplayScale;
            for (int buffer = 0; buffer < 2; ++buffer)
            for (int screen = 0; screen < 2; ++screen)
            {
                const u32* old = DisplayScale > 1 ? ScaledBuffers[buffer][screen].data() : Framebuffer[buffer][screen];
                auto& output = next[buffer][screen];
                output.resize(size_t(width) * height);
                for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    output[size_t(y) * width + x] = old[size_t(y * DisplayScale / scale) * oldWidth + x * DisplayScale / scale];
            }
        }
        if (!static_cast<VulkanRenderer3D&>(*Rend3D).SetRenderSettings(scale, settings.HiresCoordinates)) return false;
        if (scale != DisplayScale)
        {
            ScaledBuffers.swap(next);
            DisplayScale = scale;
            ScaledDisplay = scale > 1;
        }
        return SoftRenderer::SetRenderSettings(settings);
    }
    catch (const std::exception& error)
    {
        Platform::Log(Platform::LogLevel::Error, "Vulkan display allocation failed: %s\n", error.what());
        return false;
    }
}


void VulkanRenderer::Reset()
{
    SoftRenderer::Reset();
    DisplayCaptures = {};
    for (auto& buffer : ScaledBuffers)
        for (auto& screen : buffer) std::fill(screen.begin(), screen.end(), 0);
}

void VulkanRenderer::Stop()
{
    SoftRenderer::Stop();
    DisplayCaptures = {};
    for (auto& buffer : ScaledBuffers)
        for (auto& screen : buffer) std::fill(screen.begin(), screen.end(), 0);
}

void VulkanRenderer::DrawScanline(u32 line)
{
    const bool capturedDisplay = DrawCapturedDisplay(line);
    // Guest layers/capture run once; capture-aware layer output was latched
    // by each 2D compositor before native capture writes this scanline.
    SoftRenderer::DrawScanline(line);
    if (DisplayScale == 1) return;
    const int scale = DisplayScale, width = 256 * scale;
    const u32 vcount = GPU.VCount;
    const int mainScreen = GPU.ScreenSwap ? 0 : 1;
    const auto& rasterizer = static_cast<const VulkanRenderer3D&>(*Rend3D);
    for (int screen = 0; screen < 2; ++screen)
    {
        const bool main = screen == mainScreen;
        if (capturedDisplay && main) continue;
        const auto& compositor = static_cast<const SoftRenderer2D&>(main ? *Rend2D_A : *Rend2D_B);
        const u32 display = main ? GPU.GPU2D_A.DispCnt : GPU.GPU2D_B.DispCnt;
        const bool compose = GPU.ScreensEnabled && vcount < 192 &&
            ((display >> 16) & (main ? 3 : 1)) == 1 && compositor.HasScaledLayers();
        u32* first = ScaledBuffers[BackBuffer][screen].data() + size_t(line) * scale * width;
        if (!compose)
        {
            const u32* native = Framebuffer[BackBuffer][screen] + line * 256;
            for (int x = 0; x < 256; ++x) std::fill_n(first + x * scale, scale, native[x]);
            for (int sub = 1; sub < scale; ++sub) std::copy_n(first, width, first + sub * width);
            continue;
        }
        const u16 brightness = main ? GPU.MasterBrightnessA : GPU.MasterBrightnessB;
        const u32 mode = brightness >> 14, factor = std::min<u32>(brightness & 31, 16);
        for (int sub = 0; sub < scale; ++sub)
        {
            u32* dst = first + sub * width;
            if (main) rasterizer.GetScaledLine(vcount, sub, scale, ScaledLine3D.data());
            compositor.ComposeScaledLine(dst, ScaledLine3D.data(), scale, sub);
            if (mode == 1)
                for (int x = 0; x < width; ++x) dst[x] = ColorBrightnessUp(dst[x], factor, 0x0);
            else if (mode == 2)
                for (int x = 0; x < width; ++x) dst[x] = ColorBrightnessDown(dst[x], factor, 0xF);
            ExpandPixels(dst, width);
        }
    }
}

bool VulkanRenderer::GetDisplayFramebuffers(void** top, void** bottom, int& width, int& height)
{
    width = 256 * DisplayScale;
    height = 192 * DisplayScale;
    if (DisplayScale == 1) return GetFramebuffers(top, bottom);
    *top = ScaledBuffers[BackBuffer ^ 1][0].data();
    *bottom = ScaledBuffers[BackBuffer ^ 1][1].data();
    return true;
}

void VulkanRenderer::ClearPipelineCache()
{
    static_cast<VulkanRenderer3D&>(*Rend3D).ClearPipelineCache();
}

bool VulkanRenderer::HasRenderFailure() const
{
    return static_cast<const VulkanRenderer3D&>(*Rend3D).HasFailed();
}
}
