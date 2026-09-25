// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GPU_Soft.h"
#include <string>
#include "GPU3D_ComputeShader.h"
#include "Vulkan/Device.h"
#include <span>
#include <array>
#include <vector>

namespace melonDS
{
// Vulkan rasterizes 3D; the established scanline compositor handles 2D, FIFO,
// brightness and capture in native RAM, including CPU access to capture VRAM.
class VulkanRenderer final : public SoftRenderer
{
public:
    explicit VulkanRenderer(NDS& nds, const std::string& preferred = {});
    ~VulkanRenderer() override;
    RenderCostVulkanMeter* Costs() const;
    std::string DeviceName() const;
    // Rendering-thread 3D upload/render diagnostic, retaining its original
    // scope. Total includes display composition; the presenter counts WSI submits.
    u64 SubmissionCount() const;
    u64 TotalSubmissionCount() const;
    static bool IsAvailable(std::string& error);
    bool SetRenderSettings(RendererSettings& settings) override;
    bool HasRenderFailure() const override;
    void ClearPipelineCache();
    void Reset() override;
    void Stop() override;
    void DrawScanline(u32 line) override;
    void SwapBuffers() override;
    void AllocCapture(u32 bank, u32 start, u32 size) override;
    void InvalidateDisplayCapture(u32 bank, u32 start) override { DisplayCaptures[bank * 4 + start] = {}; }
    bool GetDisplayFrame(DisplayFrame& frame) override;
    struct ResidentFrame
    {
        std::array<std::shared_ptr<Vulkan::Device::Image>, 2> images;
        u32 width = 0, height = 0;
        u64 generation = 0;
    };
    // Callers serialize these with emulation and retain images until their
    // submission completes. GetDisplayFrame materializes CPU pixels on demand.
    std::shared_ptr<Vulkan::Device> DisplayDevice() const;
    int DisplayScaleFactor() const { return DisplayScale; }
    bool EnableDirectDisplay();
    void DisableDirectDisplay(const std::string& reason, bool permanent = true);
    const std::string& DirectDisplayStatus() const { return DisplayStatus; }
    bool GetResidentFrame(ResidentFrame& frame);

private:
    friend class VulkanRenderer3D;
    bool CaptureTexturePixels(u32 texparam, u32 scale, std::vector<u32>& pixels) const;
    using DisplayBuffers = std::array<std::array<std::span<u32>, 2>, 2>;
    using DisplayStorage = std::array<std::array<std::vector<u32>, 2>, 2>;
    using DisplayMemory = std::array<std::array<std::shared_ptr<Vulkan::Device::Buffer>, 2>, 2>;
    // Views keep CPU consumers unchanged. Ownership is independent of the
    // optional compositor, including its reset-before-CPU-replay failure path.
    DisplayBuffers ScaledBuffers;
    DisplayStorage ScaledStorage;
    DisplayMemory ScaledMemory;
    int DisplayScale = 1;
    std::array<std::array<std::shared_ptr<Vulkan::Device::Image>, 2>, 2> ResidentImages;
    std::array<std::array<bool, 2>, 2> ResidentCPUValid{};
    std::array<bool, 192> ChangedDisplayRows{};
    bool DirectDisplay = false;
    bool DirectDisplayFailed = false;
    std::string DisplayStatus = "RAM display (Vulkan output inactive)";
    void ReadbackDisplay(u32 buffer);
    using CompositionLine = SoftRenderer2D::ScaledLineContext;
    std::array<std::vector<CompositionLine>, 2> CompositionLines;
    bool CompositionPending = false;
    void FinishDisplayComposition() noexcept;
    void DiscardDisplayComposition();
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
    // Optional GPU compute for the scaled mode>=2 capture blend: source A is
    // the device-resident rendered image, source B a host-visible upload, and
    // the result lands in a host-visible buffer copied through the unchanged
    // staging/publication boundary. Lazily created; any failure keeps the CPU
    // path permanently for this renderer instance.
    struct CaptureBlendState
    {
        std::shared_ptr<Vulkan::Device> Owner;
        VkShaderModule Module = nullptr;
        VkDescriptorSetLayout Bindings = nullptr;
        VkDescriptorPool Pool = nullptr;
        VkDescriptorSet Descriptors = nullptr;
        VkPipelineLayout Layout = nullptr;
        VkPipeline Pipeline = nullptr;
        std::shared_ptr<Vulkan::Device::Buffer> Upload, Landing;
        std::shared_ptr<Vulkan::Device::Image> BoundImage;
        // A deferred submit is in flight until CaptureBlendFinish drains it;
        // RowBytes is the landing extent for the row being computed.
        bool Pending = false;
        size_t RowBytes = 0;
        ~CaptureBlendState();
    };
    std::unique_ptr<CaptureBlendState> CaptureBlend;
    // A creation/submission failure disables the GPU path permanently for this
    // renderer instance; the released CPU path stays the fallback.
    bool CaptureBlendDisabled = false;
    bool CaptureBlendRow(u32 line, u32 scale, u32 eva, u32 evb,
        const u16* capturedB, const u16* nativeB);
    // Drains the deferred dispatch into CaptureRow. False means the device
    // failed mid-row; the caller then recomputes the row on the CPU path.
    bool CaptureBlendFinish();
    void DoCapture(u32 line) override;
    bool ReadDisplayCapture(u32 bank, u32 word, u32 subx, u32 suby, u16& color) const;
    bool DrawCapturedDisplay(u32 line);
    u32 CaptureBackgroundScale(u32 engine) const override;
    bool SampleCapturedBackground(u32 engine, u32 address, u32 fracX,
        u32 fracY, u32 denominator, u16& color) const override;
    const u16* CapturedBackgroundRow(u32 engine, u32 address,
        u32 subline, u32 scale) const override;
    u32 CaptureObjectScale(u32 engine) const override;
    const u16* CapturedObjectRow(u32 engine, u32 address,
        u32 subline, u32 scale) const override;
    u32 CaptureMappedScale(const u32* mapping, u32 count) const;
    const u16* CapturedMappedRow(u32 mask, u32 address, u32 subline, u32 scale) const;
    void GetCaptureDisplay3DLine(u32 line, u32 subline, u32 scale, u32* dst) const override;

    std::array<u32, 256 * ComputeShader::VulkanMaxScale> ScaledLine3D {};
};
}
