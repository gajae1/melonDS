// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GPU3D.h"
#include "Vulkan/TextureCache.h"
#include <array>
#include <memory>

namespace melonDS
{
class VulkanRenderer3D final : public Renderer3D
{
public:
    explicit VulkanRenderer3D(melonDS::GPU3D& gpu, const std::string& preferred = {});
    std::string DeviceName() const { return Device ? Device->Properties().deviceName : ""; }
    ~VulkanRenderer3D() override;
    bool Init() override;
    bool SetRenderSettings(int scale, bool hires);
    void Reset() override;
    void RenderFrame() override;
    void RestartFrame() override;
    u32* GetLine(int line) override;
    void GetScaledLine(int line, int subline, int scale, u32* dst) const;
    bool HasFailed() const { return Failed; }
    void ClearPipelineCache() { if (Device) Device->ClearPipelineCache(); }

private:
    std::string PreferredDevice;
    void DrawFrame();
    std::shared_ptr<Vulkan::Device> Device;
    std::unique_ptr<Vulkan::ComputePipeline> Pipeline;
    std::unique_ptr<Vulkan::TextureCache> Texcache;
    std::array<u32, 256 * 192> ColorBuffer{};
    std::vector<u32> ScaledColorBuffer;
    int RenderedScale = 1;
    std::array<u32, 256> ScrolledLine{};
    std::array<u32, 256 * 256> ClearColor{}, ClearDepth{};
    u8 ClearBitmapDirty = 3;
    int ScaleFactor = 1;
    bool HiresCoordinates = false;
    bool FrameDirty = true;
    bool Failed = false;
};
}
