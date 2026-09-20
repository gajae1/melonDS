// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "GPU3D.h"
#include "Vulkan/TextureCache.h"
#include "Vulkan/DisplayCompositor.h"
#include <array>
#include <memory>

namespace melonDS
{
class VulkanRenderer;
class VulkanRenderer3D final : public Renderer3D
{
public:
    VulkanRenderer3D(VulkanRenderer& parent, melonDS::GPU3D& gpu, const std::string& preferred = {});
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
    // Preserve the original 3D upload/render diagnostic now that 2D shares
    // this device. TotalSubmissionCount also includes final-display work.
    u64 SubmissionCount() const { return TotalSubmissionCount() - DisplaySubmissions; }
    u64 TotalSubmissionCount() const { return Device ? Device->SubmissionCount() : 0; }
    void ClearPipelineCache() { if (Device) Device->ClearPipelineCache(); }

private:
    friend class VulkanRenderer;
    VulkanRenderer& Parent;
    std::span<const u32> GetScaledPixels() const;
    std::unique_ptr<Vulkan::DisplayCompositor> Compositor;
    // Retain the actual previous image through a settings change. Before its
    // pipeline is replaced, lazy CPU samples are materialized for capture/fallback.
    std::shared_ptr<Vulkan::Device::Image> RenderedImage;
    std::string PreferredDevice;
    void DrawFrame();
    std::shared_ptr<Vulkan::Device> Device;
    std::unique_ptr<Vulkan::ComputePipeline> Pipeline;
    std::unique_ptr<Vulkan::TextureCache> Texcache;
    std::array<u32, 256 * 192> ColorBuffer{};
    // Invalidating the view keeps the storage sized: conversion overwrites every
    // pixel, so a new frame needs no redundant clear/resize zero-fill pass.
    mutable std::vector<u32> ScaledColorStorage;
    mutable std::span<const u32> ScaledColorBuffer;
    int RenderedScale = 1;
    std::array<u32, 256> ScrolledLine{};
    std::array<u32, 256 * 256> ClearColor{}, ClearDepth{};
    u8 ClearBitmapDirty = 3;
    int ScaleFactor = 1;
    bool HiresCoordinates = false;
    bool FrameDirty = true;
    bool HadCaptureTextures = false;
    mutable bool Failed = false;
    u64 DisplaySubmissions = 0;
};
}
