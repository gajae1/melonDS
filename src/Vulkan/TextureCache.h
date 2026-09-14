// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "ComputePipeline.h"
#include "GPU3D_Texcache.h"

namespace melonDS::Vulkan
{
using TextureHandle = std::shared_ptr<const ComputePipeline::Texture>;

// The common cache owns VRAM hashing, invalidation, decoding and array layers.
// All uploads complete before the frame submission reads the arrays.
struct TextureLoader
{
    ComputePipeline& Pipeline;
    TextureHandle GenerateTexture(u32 width, u32 height, u32 layers)
    {
        return Pipeline.CreateTexture(width, height, layers);
    }
    void UploadTexture(const TextureHandle& texture, u32 width, u32 height, u32 layer, void* data)
    {
        Pipeline.UploadTextureLayer(*texture, layer, {static_cast<const u32*>(data), size_t(width) * height});
    }
    void DeleteTexture(const TextureHandle&) {} // Cache containers release shared ownership.
};

using TextureCache = Texcache<TextureLoader, TextureHandle>;
}
