// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "GPU3D_Vulkan.h"
#include "Vulkan/EmbeddedShaders.h"
#include "Platform.h"
#include <algorithm>
#include <stdexcept>

namespace melonDS
{
namespace
{
using Pipeline = Vulkan::ComputePipeline;

struct PreparedBatch
{
    std::vector<ComputeData::RenderPolygon> Polygons;
    std::vector<ComputeData::SpanSetupY> Edges;
    std::vector<ComputeData::SetupIndices> Indices;
    std::vector<Pipeline::Variant> Variants;
};

// Match the native pipeline's full-width work bound. Keeping whole polygons in
// order preserves depth, translucent IDs and shadow stencil across submissions.
u32 BatchSize(std::span<Polygon* const> polygons)
{
    u32 work = 0, count = 0;
    for (const auto* polygon : polygons)
    {
        int top = 192, bottom = 0;
        for (u32 v = 0; v < polygon->NumVertices; ++v)
        {
            top = std::min(top, polygon->Vertices[v]->FinalPosition[1]);
            bottom = std::max(bottom, polygon->Vertices[v]->FinalPosition[1]);
        }
        bottom = std::max(top + 1, bottom);
        const u32 tiles = 32 * ((bottom + 7) / 8 - top / 8);
        if (count == ComputeData::MaxVariants || work + tiles > 12288) break;
        work += tiles;
        ++count;
    }
    if (!count && !polygons.empty()) throw std::runtime_error("Vulkan polygon exceeds batch capacity");
    return count;
}

Pipeline::Variant MakeVariant(const Polygon& polygon, u32 dispCnt, bool wbuffer,
    Vulkan::TextureCache& cache, u32& layer)
{
    Pipeline::Variant variant{};
    const u32 mode = polygon.IsShadowMask ? 4 : ((polygon.Attr >> 4) & 3);
    const bool highlight = dispCnt & 2;
    const u32 noTexture[] = {5, 5, highlight ? 9u : 7u, 5, 19};
    const u32 textured[] = {13, 11, highlight ? 17u : 15u, 11, 19};
    const bool useTexture = (dispCnt & 1) && ((polygon.TexParam >> 26) & 7) && !polygon.IsShadowMask;
    variant.shader = (useTexture ? textured[mode] : noTexture[mode]) + wbuffer;
    layer = 0;
    if (useTexture)
    {
        u32* lastVariant;
        // Captures are already in native VRAM through SoftRenderer::DoCapture.
        // Decode them with the same bitmap path as CPU-written textures.
        cache.GetTexture(polygon.TexParam, polygon.TexPalette, variant.texture, layer, lastVariant);
        variant.wrapU = (polygon.TexParam & (1 << 16)) ? ((polygon.TexParam & (1 << 18)) ? 2 : 1) : 0;
        variant.wrapV = (polygon.TexParam & (1 << 17)) ? ((polygon.TexParam & (1 << 19)) ? 2 : 1) : 0;
    }
    return variant;
}
}

VulkanRenderer3D::VulkanRenderer3D(melonDS::GPU3D& gpu) : Renderer3D(gpu) {}
VulkanRenderer3D::~VulkanRenderer3D() = default;

bool VulkanRenderer3D::Init()
{
    try
    {
        std::string error;
        auto device = Vulkan::Device::Create(error);
        if (!device) throw std::runtime_error(error);
        Pipeline = std::make_unique<Vulkan::ComputePipeline>(device, Vulkan::EmbeddedShaders());
        Texcache = std::make_unique<Vulkan::TextureCache>(GPU, Vulkan::TextureLoader{*Pipeline});
        Platform::Log(Platform::LogLevel::Info, "Vulkan 3D: %s (256x192 compute)\n", device->Properties().deviceName);
        return true;
    }
    catch (const std::exception& error)
    {
        Platform::Log(Platform::LogLevel::Error, "Vulkan 3D initialization failed: %s\n", error.what());
        Failed = true;
        return false;
    }
}

void VulkanRenderer3D::Reset()
{
    if (Texcache) Texcache->Reset();
    ColorBuffer.fill(0);
    ClearBitmapDirty = 3;
    FrameDirty = true;
}

void VulkanRenderer3D::RestartFrame()
{
    FrameDirty = true;
    RenderFrame();
}

void VulkanRenderer3D::RenderFrame()
{
    if (Failed) return;
    try
    {
        DrawFrame();
    }
    catch (const std::exception& error)
    {
        // GPU entrypoints are noexcept. Retire the backend and let the frontend
        // replace it at the frame boundary; never claim a failed frame as GPU output.
        Failed = true;
        ColorBuffer.fill(0);
        Platform::Log(Platform::LogLevel::Error, "Vulkan 3D frame failed: %s\n", error.what());
    }
}

void VulkanRenderer3D::DrawFrame()
{
    u8 dirty;
    const bool texturesChanged = Texcache->Update(dirty);
    ClearBitmapDirty |= dirty;
    if (!texturesChanged && GPU3D.RenderFrameIdentical && !FrameDirty) return;
    if ((GPU3D.RenderDispCnt & (1 << 14)) && ClearBitmapDirty)
    {
        ComputeData::DecodeClearBitmap(GPU.VRAMFlat_Texture, ClearColor.data(), ClearDepth.data(), ClearBitmapDirty);
        Pipeline->UploadClearBitmap(ClearColor, ClearDepth);
        ClearBitmapDirty = 0;
    }

    const bool wbuffer = GPU3D.RenderNumPolygons && GPU3D.RenderPolygonRAM[0]->WBuffer;
    std::vector<Polygon*> visible;
    for (u32 i = 0; i < GPU3D.RenderNumPolygons; ++i)
    {
        auto* polygon = GPU3D.RenderPolygonRAM[i];
        if (!polygon->Degenerate && polygon->YTop < 192) visible.push_back(polygon);
    }
    const std::span<Polygon* const> polygons(visible);
    std::vector<PreparedBatch> prepared;
    u32 first = 0;
    do
    {
        const u32 count = BatchSize(polygons.subspan(first));
        auto& batch = prepared.emplace_back();
        batch.Polygons.resize(count);
        batch.Edges.resize(count * 12);
        batch.Indices.resize(count * 192);
        int numEdges = 0, numIndices = 0;
        for (u32 i = 0; i < count; ++i)
        {
            Polygon* source = polygons[first + i];
            auto& polygon = batch.Polygons[i];
            ComputeData::PreparePolygon(source, i, polygon, batch.Edges, numEdges,
                batch.Indices, numIndices, 1, false);
            u32 layer;
            auto variant = MakeVariant(*source, GPU3D.RenderDispCnt, wbuffer, *Texcache, layer);
            const auto found = std::find_if(batch.Variants.begin(), batch.Variants.end(), [&](const auto& previous) {
                return previous.shader == variant.shader && previous.texture == variant.texture &&
                    previous.wrapU == variant.wrapU && previous.wrapV == variant.wrapV;
            });
            polygon.Variant = found - batch.Variants.begin();
            if (found == batch.Variants.end()) batch.Variants.push_back(std::move(variant));
            polygon.TextureLayer = float(layer);
        }
        batch.Edges.resize(numEdges);
        batch.Indices.resize(numIndices);
        first += count;
    } while (first < polygons.size());

    std::vector<Vulkan::ComputePipeline::Batch> batches;
    for (const auto& batch : prepared)
        batches.push_back({batch.Polygons, batch.Edges, batch.Indices, batch.Variants,
            ComputeData::PrepareMeta(GPU3D, batch.Polygons.size(), batch.Variants.size()), wbuffer});
    const auto pixels = Pipeline->Render(batches);
    // RGBA8 UNORM preserves all native RGB6/A5 values through this conversion.
    for (size_t i = 0; i < pixels.size(); ++i)
        ColorBuffer[i] = ((pixels[i] >> 2) & 0x003F3F3F) | ((pixels[i] >> 3) & 0x1F000000);
    FrameDirty = false;
}

u32* VulkanRenderer3D::GetLine(int line)
{
    if (GPU3D.AbortFrame || line < 0 || line >= 192)
    {
        ScrolledLine.fill(0);
        return ScrolledLine.data();
    }
    u32* raw = ColorBuffer.data() + line * 256;
    const u16 xpos = GPU3D.RenderXPos;
    if (!xpos) return raw;
    for (u32 x = 0; x < 256; ++x)
    {
        const u32 source = (x + xpos) & 511;
        ScrolledLine[x] = source < 256 ? raw[source] : 0;
    }
    return ScrolledLine.data();
}
}
