// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "GPU_Vulkan.h"
#include "GPU3D_Vulkan.h"
#include "NDS.h"
#include "GPU_ColorOp.h"
#include "Platform.h"
#include "RenderCost.h"
#include <algorithm>

namespace melonDS
{
using Cost = RenderCostVulkanMeter;
VulkanRenderer::VulkanRenderer(NDS& nds, const std::string& preferred)
    : SoftRenderer(nds, std::make_unique<VulkanRenderer3D>(*this, nds.GPU.GPU3D, preferred))
{
}

VulkanRenderer::~VulkanRenderer()
{
    if (const auto* cost = Costs(); cost && cost->Count())
    {
        char line[8192];
        cost->Report(line, sizeof(line), "renderer-final");
        Platform::Log(Platform::LogLevel::Info, "%s\n", line);
    }
}

RenderCostVulkanMeter* VulkanRenderer::Costs() const
{
    const auto& rasterizer = static_cast<const VulkanRenderer3D&>(*Rend3D);
    return rasterizer.Device ? rasterizer.Device->Costs() : nullptr;
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

u64 VulkanRenderer::TotalSubmissionCount() const
{
    return static_cast<const VulkanRenderer3D&>(*Rend3D).TotalSubmissionCount();
}

bool VulkanRenderer::SetRenderSettings(RendererSettings& settings)
{
    FinishDisplayComposition();
    InvalidateDisplayFrame();
    const int scale = settings.ScaleFactor;
    if (scale < 1 || scale > ComputeShader::VulkanMaxScale) return false;
    try
    {
        auto& rasterizer = static_cast<VulkanRenderer3D&>(*Rend3D);
        DisplayBuffers next;
        DisplayStorage nextStorage;
        DisplayMemory nextMemory;
        if (scale != DisplayScale && scale > 1)
        {
            const int width = 256 * scale, height = 192 * scale;
            const int oldWidth = 256 * DisplayScale;
            const size_t pixels = size_t(width) * height;
            try
            {
                if (rasterizer.Device)
                    for (auto& buffer : nextMemory)
                        for (auto& screen : buffer)
                            screen = rasterizer.Device->CreateBuffer(pixels * sizeof(u32),
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT, true, 0, VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
            }
            catch (const std::exception& error)
            {
                // Never force uncached mapped storage onto CPU-fill/capture
                // paths. A partial allocation also falls back as one unit.
                nextMemory = {};
                Platform::Log(Platform::LogLevel::Warn, "Vulkan direct display backing unavailable: %s\n", error.what());
            }
            for (int buffer = 0; buffer < 2; ++buffer)
            for (int screen = 0; screen < 2; ++screen)
            {
                const u32* old = DisplayScale > 1 ? ScaledBuffers[buffer][screen].data() : Framebuffer[buffer][screen];
                auto& output = next[buffer][screen];
                if (nextMemory[buffer][screen])
                    output = {static_cast<u32*>(nextMemory[buffer][screen]->Data()), pixels};
                else
                {
                    nextStorage[buffer][screen].resize(pixels);
                    output = nextStorage[buffer][screen];
                }
                for (int y = 0; y < height; ++y)
                for (int x = 0; x < width; ++x)
                    output[size_t(y) * width + x] = old[size_t(y * DisplayScale / scale) * oldWidth + x * DisplayScale / scale];
            }
        }
        if (!static_cast<VulkanRenderer3D&>(*Rend3D).SetRenderSettings(scale, settings.HiresCoordinates)) return false;
        if (scale != DisplayScale)
        {
            ScaledBuffers.swap(next);
            ScaledStorage.swap(nextStorage);
            ScaledMemory.swap(nextMemory);
            DisplayScale = scale;
            ScaledDisplay = scale > 1;
        }
        if (rasterizer.Compositor)
        {
            try
            {
                for (auto& lines : CompositionLines) lines.resize(192);
            }
            catch (const std::exception& error)
            {
                rasterizer.Compositor.reset();
                Platform::Log(Platform::LogLevel::Warn, "Vulkan composition context unavailable: %s\n", error.what());
            }
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
    DiscardDisplayComposition();
    SoftRenderer::Reset();
    DisplayCaptures = {};
    for (auto& buffer : ScaledBuffers)
        for (auto& screen : buffer) std::fill(screen.begin(), screen.end(), 0);
}

void VulkanRenderer::Stop()
{
    DiscardDisplayComposition();
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
    {
        RenderCostVulkanScope scan(Costs(), Cost::Scan2D);
        SoftRenderer::DrawScanline(line);
    }
    if (DisplayScale == 1) return;
    RenderCostVulkanScope display(Costs(), Cost::CpuDisplay);
    const int scale = DisplayScale, width = 256 * scale;
    const u32 vcount = GPU.VCount;
    const int mainScreen = GPU.ScreenSwap ? 0 : 1;
    const auto& rasterizer = static_cast<const VulkanRenderer3D&>(*Rend3D);
    for (int screen = 0; screen < 2; ++screen)
    {
        const bool main = screen == mainScreen;
        // Replace a repeated physical row, never leave an older pending context
        // that could overwrite a later CPU/capture result at the flush boundary.
        auto* context = CompositionLines[screen].size() == 192 ? &CompositionLines[screen][line] : nullptr;
        if (context) context->mode = CompositionLine::Keep;
        if (capturedDisplay && main)
        {
            if (context) context->mode = CompositionLine::CaptureOverride;
            continue;
        }
        const auto& compositor = static_cast<const SoftRenderer2D&>(main ? *Rend2D_A : *Rend2D_B);
        const u32 display = main ? GPU.GPU2D_A.DispCnt : GPU.GPU2D_B.DispCnt;
        const bool compose = GPU.ScreensEnabled && vcount < 192 &&
            ((display >> 16) & (main ? 3 : 1)) == 1 && compositor.HasScaledLayers();
        u32* first = ScaledBuffers[BackBuffer][screen].data() + size_t(line) * scale * width;
        const u16 brightness = main ? GPU.MasterBrightnessA : GPU.MasterBrightnessB;
        if (rasterizer.Compositor && context && compose)
        {
            if (compositor.ExportScaledContext(*context, brightness))
            {
                context->sourceLine = vcount;
                context->xpos = GPU.GPU3D.RenderXPos;
                context->abort = GPU.GPU3D.AbortFrame;
                CompositionPending = true;
                continue;
            }
            // Capture layers are already precomposed at the native provenance
            // boundary. Keep the original CPU row and omit it from GPU readback.
            context->mode = CompositionLine::CaptureOverride;
        }
        if (!compose)
        {
            const u32* native = Framebuffer[BackBuffer][screen] + line * 256;
            for (int x = 0; x < 256; ++x) std::fill_n(first + x * scale, scale, native[x]);
            for (int sub = 1; sub < scale; ++sub) std::copy_n(first, width, first + sub * width);
            continue;
        }
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

void VulkanRenderer::DiscardDisplayComposition()
{
    CompositionPending = false;
    for (auto& lines : CompositionLines)
        for (auto& line : lines) line.mode = CompositionLine::Keep;
}

void VulkanRenderer::FinishDisplayComposition() noexcept
{
    if (!CompositionPending) return;
    RenderCostVulkanScope display(Costs(), Cost::RecordDisplay);
    auto& rasterizer = static_cast<VulkanRenderer3D&>(*Rend3D);
    const auto pending = [](const auto& lines) {
        return std::any_of(lines.begin(), lines.end(), [](const auto& line) {
            return line.mode != CompositionLine::Keep && line.mode != CompositionLine::CaptureOverride;
        });
    };
    const u64 submissionsBefore = rasterizer.TotalSubmissionCount();
    bool replay = !rasterizer.Compositor;
    if (!replay)
    {
        try
        {
            for (u32 screen = 0; screen < 2; ++screen)
                if (pending(CompositionLines[screen]))
                    rasterizer.Compositor->Compose(screen, CompositionLines[screen], rasterizer.RenderedImage,
                        rasterizer.RenderedScale, ScaledBuffers[BackBuffer][screen], ScaledMemory[BackBuffer][screen].get());
        }
        catch (const std::exception& error)
        {
            rasterizer.Compositor.reset();
            replay = true;
            Platform::Log(Platform::LogLevel::Warn, "Vulkan composition falling back to CPU: %s\n", error.what());
        }
    }
    if (replay)
    {
        RenderCostVulkanScope fallback(Costs(), Cost::Fallback);
        try
        {
            const auto pixels = rasterizer.GetScaledPixels();
            const u32 sourceScale = rasterizer.ScaledColorBuffer.empty() ? 1 : rasterizer.RenderedScale;
            for (u32 screen = 0; screen < 2; ++screen)
                if (pending(CompositionLines[screen]))
                    Vulkan::ComposeDisplayCPU(CompositionLines[screen], pixels, sourceScale,
                        DisplayScale, ScaledBuffers[BackBuffer][screen]);
        }
        catch (const std::exception& error)
        {
            // A retired/lost device cannot supply GPU-only subpixels. Preserve
            // native visibility and the existing frontend retirement contract;
            // never claim this recovery image is a successful enhanced frame.
            rasterizer.Failed = true;
            Platform::Log(Platform::LogLevel::Error, "Vulkan composition recovery failed: %s\n", error.what());
            const u32 width = 256 * DisplayScale;
            for (u32 screen = 0; screen < 2; ++screen)
            for (u32 y = 0; y < CompositionLines[screen].size(); ++y)
            {
                const auto mode = CompositionLines[screen][y].mode;
                if (mode == CompositionLine::Keep || mode == CompositionLine::CaptureOverride) continue;
                auto* dst = ScaledBuffers[BackBuffer][screen].data() + size_t(y) * DisplayScale * width;
                const auto* src = Framebuffer[BackBuffer][screen] + y * 256;
                for (u32 x = 0; x < 256; ++x) std::fill_n(dst + x * DisplayScale, DisplayScale, src[x]);
                for (int sub = 1; sub < DisplayScale; ++sub) std::copy_n(dst, width, dst + sub * width);
            }
        }
    }
    rasterizer.DisplaySubmissions += rasterizer.TotalSubmissionCount() - submissionsBefore;
    DiscardDisplayComposition();
}

void VulkanRenderer::SwapBuffers()
{
    FinishDisplayComposition();
    Renderer::SwapBuffers();
}

bool VulkanRenderer::GetDisplayFrame(DisplayFrame& frame)
{
    if (DisplayScale == 1) return Renderer::GetDisplayFrame(frame);
    frame = {};
    const auto& buffers = ScaledBuffers[BackBuffer ^ 1];
    if (buffers[0].empty() || buffers[1].empty()) return false;
    const u32 width = 256 * DisplayScale, height = 192 * DisplayScale;
    frame = {DisplayFrame::Kind::CpuBGRA, buffers[0].data(), buffers[1].data(),
        width, height, GetDisplayFrameGeneration(width, height)};
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
