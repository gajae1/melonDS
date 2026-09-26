// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ComputePipeline.h"
#include "GPU3D_Texcache.h"
namespace melonDS { class VulkanRenderer; }

namespace melonDS::Vulkan
{
using TextureHandle = std::shared_ptr<const ComputePipeline::Texture>;

// The common cache owns VRAM hashing, invalidation, decoding and array layers.
// All uploads complete before the frame submission reads the arrays.
struct TextureLoader
{
    ComputePipeline& Pipeline;
    TextureLoader(ComputePipeline& pipeline, VulkanRenderer& parent, GPU& gpu);
    bool BeginTextureUpdate(u64& generation);
    const u8* TextureBytes(bool cpuTextureChanged);
    u64 TextureSource(u32 start, u32 size);
    void DecodeTexture(const TextureHandle& texture, u32 layer, u32 texParam, u32 palBase);
    bool DecodeClearBitmap();
    TextureHandle GenerateTexture(u32 width, u32 height, u32 layers)
    {
        return Pipeline.CreateTexture(width, height, layers);
    }
    void UploadTexture(const TextureHandle& texture, u32 width, u32 height, u32 layer, void* data)
    {
        Pipeline.UploadTextureLayer(*texture, layer, {static_cast<const u32*>(data), size_t(width) * height});
    }
    void DeleteTexture(const TextureHandle&) {} // Cache containers release shared ownership.
private:
    struct Source;
    void PrepareInputs();
    std::shared_ptr<Source> source;
    VulkanRenderer& parent;
};

using TextureCache = Texcache<TextureLoader, TextureHandle>;
}
