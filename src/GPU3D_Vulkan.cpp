// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "GPU3D_Vulkan.h"
#include "GPU_Vulkan.h"
#include "RenderCost.h"
#include "Vulkan/EmbeddedShaders.h"
#include "Platform.h"
#include <algorithm>
#include <stdexcept>

namespace melonDS
{
namespace
{
using Pipeline = Vulkan::ComputePipeline;
using Cost = RenderCostVulkanMeter;

struct PreparedBatch
{
    std::vector<ComputeData::RenderPolygon> Polygons;
    std::vector<ComputeData::SpanSetupY> Edges;
    std::vector<ComputeData::SetupIndices> Indices;
    std::vector<Pipeline::Variant> Variants;
    std::vector<ComputeData::RenderPolygon> DisplayPolygons;
    std::vector<Pipeline::Variant> DisplayVariants;
};

u32 AddVariant(std::vector<Pipeline::Variant>& variants, const Pipeline::Variant& variant)
{
    const auto found = std::find_if(variants.begin(), variants.end(), [&](const auto& previous) {
        return previous.shader == variant.shader && previous.texture == variant.texture &&
            previous.wrapU == variant.wrapU && previous.wrapV == variant.wrapV &&
            previous.captureYOffset == variant.captureYOffset && previous.captureScale == variant.captureScale;
    });
    const u32 index = found - variants.begin();
    if (found == variants.end()) variants.push_back(variant);
    return index;
}

// Match the pipeline's scaled full-width work bound and indirect-dispatch limit.
// Whole polygons in order preserve depth, translucent IDs and shadow stencil.
u32 BatchSize(std::span<Polygon* const> polygons, int scale, bool hires, u32 capacity, u32 spanCapacity, u32 tileSize)
{
    u32 work = 0, count = 0, spans = 0;
    for (const auto* polygon : polygons)
    {
        int top = 192 * scale, bottom = 0;
        for (u32 v = 0; v < polygon->NumVertices; ++v)
        {
            const auto& vertex = *polygon->Vertices[v];
            const int y = hires ? (vertex.HiresPosition[1] * scale) >> 4 : vertex.FinalPosition[1] * scale;
            top = std::min(top, y);
            bottom = std::max(bottom, y);
        }
        bottom = std::max(top + 1, bottom);
        const u32 tiles = (256 * scale / tileSize) * ((bottom + tileSize - 1) / tileSize - top / tileSize);
        const u32 lines = bottom - top;
        if (count == ComputeData::MaxVariants || work + tiles > capacity || spans + lines > spanCapacity) break;
        work += tiles;
        spans += lines;
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
        // Always retain the native decode for guest-visible output. Captured
        // display samples are substituted only in the separate display batch.
        cache.GetTexture(polygon.TexParam, polygon.TexPalette, variant.texture, layer, lastVariant);
        variant.wrapU = (polygon.TexParam & (1 << 16)) ? ((polygon.TexParam & (1 << 18)) ? 2 : 1) : 0;
        variant.wrapV = (polygon.TexParam & (1 << 17)) ? ((polygon.TexParam & (1 << 19)) ? 2 : 1) : 0;
    }
    return variant;
}
}

VulkanRenderer3D::VulkanRenderer3D(VulkanRenderer& parent, melonDS::GPU3D& gpu, const std::string& preferred)
    : Renderer3D(gpu), Parent(parent), PreferredDevice(preferred) {}
VulkanRenderer3D::~VulkanRenderer3D() = default;

bool VulkanRenderer3D::Init()
{
    try
    {
        std::string error;
        Device = Vulkan::Device::Create(error, PreferredDevice);
        if (!Device) throw std::runtime_error(error);
        DisplaySubmissions = 0;
        Pipeline = std::make_unique<Vulkan::ComputePipeline>(Device, Vulkan::EmbeddedShaders());
        Pipeline->SetUploadBatching(true);
        Texcache = std::make_unique<Vulkan::TextureCache>(GPU, Vulkan::TextureLoader{*Pipeline});
        Platform::Log(Platform::LogLevel::Info, "Vulkan 3D: %s (256x192 compute)\n", Device->Properties().deviceName);
        return true;
    }
    catch (const std::exception& error)
    {
        Platform::Log(Platform::LogLevel::Error, "Vulkan 3D initialization failed: %s\n", error.what());
        Failed = true;
        return false;
    }
}

bool VulkanRenderer3D::SetRenderSettings(int scale, bool hires)
{
    if (scale < 1 || scale > ComputeShader::VulkanMaxScale || Failed || !Pipeline) return false;
    // Native coordinates include the DS divider's precision loss. Keep 1x
    // byte-identical even when the shared high-resolution option is enabled.
    hires = hires && scale > 1;
    if (scale == ScaleFactor && hires == HiresCoordinates) return true;
    try
    {
        if (scale != ScaleFactor)
        {
            // A paused resize keeps the prior display image/sample grid alive.
            // Its old pipeline owns the full-readback staging, so snapshot before replacement.
            GetScaledPixels();
            auto pipeline = std::make_unique<Vulkan::ComputePipeline>(Device, Vulkan::EmbeddedShaders(scale), scale);
            pipeline->SetUploadBatching(true);
            auto cache = std::make_unique<Vulkan::TextureCache>(GPU, Vulkan::TextureLoader{*pipeline});
            // Commit the pair only after successful initialization. Local destruction
            // releases the old cache before the pipeline its TextureLoader references.
            Texcache.swap(cache);
            Pipeline.swap(pipeline);
            ScaleFactor = scale;
            ClearBitmapDirty = 3;
            Compositor.reset();
            if (scale > 1)
            {
                const u64 displayBefore = TotalSubmissionCount();
                try
                {
                    Pipeline->EnableNativeReadback(Vulkan::EmbeddedNativeReadback());
                    Compositor = std::make_unique<Vulkan::DisplayCompositor>(Device,
                        Vulkan::EmbeddedDisplayCompose(), scale);
                }
                catch (const std::exception& error)
                {
                    // Optional display resources do not invalidate the native backend.
                    Platform::Log(Platform::LogLevel::Warn, "Vulkan GPU composition unavailable: %s\n", error.what());
                }
                DisplaySubmissions += TotalSubmissionCount() - displayBefore;
            }
        }
        HiresCoordinates = hires;
        FrameDirty = true;
        // Keep native scanlines from the current frame until the next RenderFrame.
        return true;
    }
    catch (const std::exception& error)
    {
        Platform::Log(Platform::LogLevel::Error, "Vulkan 3D scale change failed: %s\n", error.what());
        return false;
    }
}

void VulkanRenderer3D::Reset()
{
    if (Texcache) Texcache->Reset();
    ColorBuffer.fill(0);
    ScaledColorBuffer = {};
    ScaledColorStorage.clear();
    RenderedImage.reset();
    RenderedScale = 1;
    ClearBitmapDirty = 3;
    FrameDirty = true;
    HadCaptureTextures = false;
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
        ScaledColorBuffer = {};
        ScaledColorStorage.clear();
        RenderedImage.reset();
        RenderedScale = 1;
        Platform::Log(Platform::LogLevel::Error, "Vulkan 3D frame failed: %s\n", error.what());
    }
}

void VulkanRenderer3D::DrawFrame()
{
    // The next 3D frame starts at VCount 215, before the display buffer swap.
    // Finish consumers of the old image BEFORE any render can reuse it.
    Parent.FinishDisplayComposition();
    if (Failed) return;
    RenderCostVulkanScope prepare(Device->Costs(), Cost::Prepare3D);
    std::unordered_map<u32, std::shared_ptr<const Pipeline::Texture>> captures;
    bool hasCaptures = false;
    if (ScaleFactor > 1 && (GPU3D.RenderDispCnt & 1))
    {
        std::vector<u32> pixels;
        for (u32 i = 0; i < GPU3D.RenderNumPolygons; ++i)
        {
            const auto& polygon = *GPU3D.RenderPolygonRAM[i];
            if (polygon.Degenerate || polygon.YTop >= 192 || polygon.IsShadowMask ||
                ((polygon.TexParam >> 26) & 7) != 7) continue;
            const u32 key = polygon.TexParam & ~0xC00F0000u;
            auto [entry, inserted] = captures.try_emplace(key);
            if (!inserted) continue;
            try
            {
                // Snapshot before Update: a DIFFERENT wrapping texture can
                // SyncAllVRAMCaptures and retire the parent's sidecar. Keep
                // that native ordering without losing this texture's detail.
                if (!Parent.CaptureTexturePixels(key, ScaleFactor, pixels)) continue;
                entry->second = Pipeline->UploadTexture(TextureWidth(key) * ScaleFactor,
                    TextureHeight(key) * ScaleFactor, 1, pixels, true);
                hasCaptures = true;
            }
            catch (const std::exception& error)
            {
                // Optional display storage must not retire the native backend.
                // Failed/invalid entries also prevent duplicate attempts this frame.
                Platform::Log(Platform::LogLevel::Warn, "Vulkan capture texture unavailable: %s\n", error.what());
            }
        }
    }
    // Preserve the pre-Update capture boundary: a wrapping native texture
    // may SyncAllVRAMCaptures and invalidate the display sidecars below.
    // All captured snapshots complete together before that synchronization.
    Pipeline->FlushUploads();
    u8 dirty;
    const bool texturesChanged = Texcache->Update(dirty);
    ClearBitmapDirty |= dirty;
    // Native bytes can be identical while subpixels change or are invalidated.
    if (!texturesChanged && GPU3D.RenderFrameIdentical && !FrameDirty && !hasCaptures && !HadCaptureTextures) return;
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
        const u32 count = BatchSize(polygons.subspan(first), ScaleFactor, HiresCoordinates, Pipeline->WorkCapacity(), Pipeline->SpanCapacity(), Pipeline->TileSize());
        auto& batch = prepared.emplace_back();
        batch.Polygons.resize(count);
        if (hasCaptures) batch.DisplayPolygons.resize(count);
        batch.Edges.resize(count * 12);
        batch.Indices.resize(count * 192 * ScaleFactor);
        int numEdges = 0, numIndices = 0;
        for (u32 i = 0; i < count; ++i)
        {
            Polygon* source = polygons[first + i];
            auto& polygon = batch.Polygons[i];
            ComputeData::PreparePolygon(source, i, polygon, batch.Edges, numEdges,
                batch.Indices, numIndices, ScaleFactor, HiresCoordinates);
            u32 layer;
            auto variant = MakeVariant(*source, GPU3D.RenderDispCnt, wbuffer, *Texcache, layer);
            polygon.Variant = AddVariant(batch.Variants, variant);
            polygon.TextureLayer = float(layer);
            if (hasCaptures)
            {
                auto& display = batch.DisplayPolygons[i];
                display = polygon;
                const auto captured = captures.find(source->TexParam & ~0xC00F0000u);
                if (variant.texture && captured != captures.end() && captured->second)
                {
                    variant.texture = captured->second;
                    variant.captureScale = ScaleFactor;
                    display.TextureLayer = 0;
                }
                display.Variant = AddVariant(batch.DisplayVariants, variant);
            }
        }
        batch.Edges.resize(numEdges);
        batch.Indices.resize(numIndices);
        first += count;
    } while (first < polygons.size());

    std::vector<Vulkan::ComputePipeline::Batch> batches;
    for (const auto& batch : prepared)
        batches.push_back({batch.Polygons, batch.Edges, batch.Indices, batch.Variants,
            ComputeData::PrepareMeta(GPU3D, batch.Polygons.size(), batch.Variants.size()), wbuffer});
    // Consume the completed readback view before any subsequent render reuses it.
    const bool gpuComposition = bool(Compositor);
    auto pixels = Pipeline->RenderView(batches, gpuComposition ? Pipeline::Readback::Native : Pipeline::Readback::Full);
    // Sample native pixel origins without filtering RGB6/A5 or inventing
    // capture alpha. At 1x this is exactly the previous byte conversion.
    // Scaled edge coverage follows the shared compute rasterizer, not a
    // stretched native image. The clear VRAM bitmap remains 256x256.
    const u32 width = 256 * ScaleFactor;
    {
        RenderCostVulkanScope convert(Device->Costs(), Cost::NativeConvert);
        for (u32 y = 0; y < 192; ++y)
        for (u32 x = 0; x < 256; ++x)
        {
            const u32 pixel = pixels[gpuComposition ? y * 256 + x : (y * ScaleFactor) * width + x * ScaleFactor];
            ColorBuffer[y * 256 + x] = ((pixel >> 2) & 0x003F3F3F) | ((pixel >> 3) & 0x1F000000);
        }
    }
    // Never derive guest pixels from captured subpixel UVs: even a native
    // screen origin can address a fractional texel. The native result above
    // must be consumed before RenderView reuses its readback allocation.
    if (hasCaptures)
    {
        for (size_t i = 0; i < batches.size(); ++i)
        {
            batches[i].polygons = prepared[i].DisplayPolygons;
            batches[i].variants = prepared[i].DisplayVariants;
            batches[i].meta.NumVariants = prepared[i].DisplayVariants.size();
        }
        pixels = Pipeline->RenderView(batches, gpuComposition ? Pipeline::Readback::None : Pipeline::Readback::Full);
    }
    if (ScaleFactor > 1 && !gpuComposition)
    {
        RenderCostVulkanScope convert(Device->Costs(), Cost::ScaledConvert);
        ScaledColorStorage.resize(pixels.size());
        std::transform(pixels.begin(), pixels.end(), ScaledColorStorage.begin(), [](u32 pixel) {
            return ((pixel >> 2) & 0x003F3F3F) | ((pixel >> 3) & 0x1F000000);
        });
        ScaledColorBuffer = ScaledColorStorage;
    }
    else ScaledColorBuffer = {};
    RenderedImage = Pipeline->OutputImage();
    RenderedScale = ScaleFactor;
    HadCaptureTextures = hasCaptures;
    FrameDirty = false;
}

std::span<const u32> VulkanRenderer3D::GetScaledPixels() const
{
    if (RenderedScale > 1 && RenderedImage && ScaledColorBuffer.empty())
    {
        if (RenderedImage != Pipeline->OutputImage())
            throw std::logic_error("Retained Vulkan image has no CPU snapshot");
        // Only enhanced capture or CPU fallback consumes this full image. The
        // ordinary display path keeps it on-device and reads just native origins.
        const auto pixels = Pipeline->ReadbackView();
        RenderCostVulkanScope convert(Device->Costs(), Cost::ScaledConvert);
        ScaledColorStorage.resize(pixels.size());
        std::transform(pixels.begin(), pixels.end(), ScaledColorStorage.begin(), [](u32 pixel) {
            return ((pixel >> 2) & 0x003F3F3F) | ((pixel >> 3) & 0x1F000000);
        });
        ScaledColorBuffer = ScaledColorStorage;
    }
    return ScaledColorBuffer.empty() ? std::span<const u32>(ColorBuffer) : std::span<const u32>(ScaledColorBuffer);
}

void VulkanRenderer3D::GetScaledLine(int line, int subline, int scale, u32* dst) const
{
    if (GPU3D.AbortFrame || line < 0 || line >= 192)
    {
        std::fill_n(dst, 256 * scale, 0);
        return;
    }
    try { GetScaledPixels(); }
    catch (const std::exception& error)
    {
        Failed = true;
        std::fill_n(dst, 256 * scale, 0);
        Platform::Log(Platform::LogLevel::Error, "Vulkan capture readback failed: %s\n", error.what());
        return;
    }
    // A settings change retains the previous frame until RenderFrame runs.
    // Sample that retained image at the new display size, not a freed pipeline.
    const int sourceScale = ScaledColorBuffer.empty() ? 1 : RenderedScale;
    const u32* source = ScaledColorBuffer.empty() ? ColorBuffer.data() : ScaledColorBuffer.data();
    const int sourceY = (line * scale + subline) * sourceScale / scale;
    source += sourceY * 256 * sourceScale;
    if (sourceScale == scale)
    {
        // The steady-state path is contiguous runs, not one division/modulo
        // per displayed pixel. The second half of the 9-bit scroll is blank.
        const u32 width = 256 * scale, period = width * 2;
        u32 position = (GPU3D.RenderXPos & 511) * scale;
        u32 remaining = width;
        while (remaining)
        {
            const u32 count = std::min(remaining, (position < width ? width : period) - position);
            if (position < width) std::copy_n(source + position, count, dst);
            else std::fill_n(dst, count, 0);
            dst += count;
            remaining -= count;
            position = (position + count) % period;
        }
        return;
    }
    for (int x = 0; x < 256 * scale; ++x)
    {
        const u32 sourceX = (x * sourceScale / scale + GPU3D.RenderXPos * sourceScale) % (512 * sourceScale);
        dst[x] = sourceX < u32(256 * sourceScale) ? source[sourceX] : 0;
    }
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
