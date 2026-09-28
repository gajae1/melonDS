// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "GPU_Vulkan.h"
#include "GPU3D_Vulkan.h"
#include "Platform.h"
#include "RenderCost.h"
#include "Vulkan/EmbeddedShaders.h"
#include "Vulkan/Native2DPipeline.h"
#include "Vulkan/Native2DQueue.h"
#include "Vulkan/Native2DCapture.h"
#include <algorithm>
#include <stdexcept>

namespace melonDS
{
namespace {
Vulkan::Native2D::CapturedMemory CapturedState(Vulkan::Native2D::CapturePipeline& capture,
    const std::array<std::bitset<512>, 4>& owned,
    const std::array<std::bitset<512>, 4>& hiresOwned)
{
    Vulkan::Native2D::CapturedMemory result;
    result.hiresOwned = hiresOwned;
    if (capture.Revision() && std::any_of(owned.begin(), owned.end(), [](const auto& bank) { return bank.any(); }))
    {
        result.source = capture.Snapshot();
        result.revision = capture.Revision();
        result.owned = owned;
    }
    return result;
}
}
bool VulkanRenderer::InitNative2D()
{
    if (NativePipeline) return true;
    auto& rasterizer = static_cast<VulkanRenderer3D&>(*Rend3D);
    if (!rasterizer.Device || rasterizer.HasFailed()) return false;
    try
    {
        auto pipeline = std::make_unique<Vulkan::Native2D::Pipeline>(rasterizer.Device,
            Vulkan::EmbeddedNative2D(), Vulkan::EmbeddedNative2DMerge(), DisplayScale);
        auto queue = std::make_unique<Vulkan::Native2D::Queue>(GPU);
        auto capture = std::make_unique<Vulkan::Native2D::CapturePipeline>(rasterizer.Device,
            Vulkan::EmbeddedNative2DCapture());
        if (DisplayScale > 1 && !capture->EnableHires(Vulkan::EmbeddedNative2DCaptureHires(), DisplayScale))
            Platform::Log(Platform::LogLevel::Warn, "Vulkan high-resolution capture storage unavailable; using native pixels\n");
        NativeCaptures.reserve(Vulkan::Native2D::Pipeline::MaxRecords / 2);
        if (!rasterizer.SetNativeImageRetention(true)) return false;
        NativePipeline = std::move(pipeline);
        NativeQueue = std::move(queue);
        NativeCapture = std::move(capture);
        NativeSource3D.reset();
        NativeDisplay3D.reset();
        NativeCaptureDirty = {};
        NativeHiresOwned = {};
        NativeCaptureWriteThrough = false;
        NativeCaptureControl = GPU.CaptureCnt;
        return true;
    }
    catch (const std::exception& error)
    {
        Platform::Log(Platform::LogLevel::Warn, "Vulkan native 2D unavailable: %s\n", error.what());
        return false;
    }
}

void VulkanRenderer::ResetNative2D()
{
    if (!NativePipeline) return;
    CompleteNative2D();
    NativeSource3D.reset();
    NativeDisplay3D.reset();
    NativeQueue.reset();
    NativePipeline.reset();
    NativeCapture.reset();
    NativeCaptures.clear();
    NativeCaptureDirty = {};
    NativeHiresOwned = {};
    if (!InitNative2D())
        static_cast<VulkanRenderer3D&>(*Rend3D).Failed = true;
}

void VulkanRenderer::FailNative2D(const std::exception& error) noexcept
{
    // Device/host failure retires the backend through the existing frame-boundary
    // frontend contract. Resampling current guest RAM would not replay the queued
    // scanlines and must never be published as a successful native frame.
    static_cast<VulkanRenderer3D&>(*Rend3D).Failed = true;
    NativeHiresOwned = {};
    Platform::Log(Platform::LogLevel::Error, "Vulkan native 2D failed: %s\n", error.what());
}

void VulkanRenderer::MigrateNative2DScale(u32 scale)
{
    // Called between batches after the displayed frames were read back. Old
    // display-scale captures stop enhancing consumers, as legacy DisplayCaptures
    // do at a different scale; guest captures, dirty ownership, the OBJ
    // prefetch and guest OBJ history stay on the device.
    if (!NativeQueue->Records().empty() || !NativeCaptures.empty())
        throw std::logic_error("Native 2D scale change with queued rows");
    NativePipeline->Complete();
    NativeQueue->DropHires();
    NativeHiresOwned = {};
    NativeCapture->DisableHires();
    NativePipeline->SetScale(scale);
    if (scale > 1 && !NativeCapture->EnableHires(Vulkan::EmbeddedNative2DCaptureHires(), scale))
        Platform::Log(Platform::LogLevel::Warn, "Vulkan high-resolution capture storage unavailable; using native pixels\n");
}

void VulkanRenderer::DisableNative2D()
{
    // Frontends apply settings after RunFrame returns at VBlank, where no OBJ
    // prefetch is pending (none follows line 191; line 0's runs at VCOUNT 262).
    // Publish displayed frames and GPU-only guest capture rows for CPU 2D.
    if (!NativeQueue->Records().empty()) throw std::logic_error("Native 2D disable with queued rows");
    NativePipeline->Complete();
    ReadbackDisplay(0);
    ReadbackDisplay(1);
    for (u32 bank = 0; bank < 4; ++bank)
        if (NativeCaptureDirty[bank].any()) SyncNativeCaptureRows(bank, 0, 512);
    ResidentImages = {};
    ResidentCPUValid = {};
    NativeSource3D.reset();
    NativeDisplay3D.reset();
    NativeQueue.reset();
    NativePipeline.reset();
    NativeCapture.reset();
    NativeCaptures.clear();
    NativeCaptureDirty = {};
    NativeHiresOwned = {};
    NativeCaptureWriteThrough = false;
    static_cast<VulkanRenderer3D&>(*Rend3D).SetNativeImageRetention(false);
}

void VulkanRenderer::BindNative3D()
{
    auto& rasterizer = static_cast<VulkanRenderer3D&>(*Rend3D);
    if (!NativeQueue->Records().empty() && NativeSourceVersion != rasterizer.RenderedVersion)
        FinishNative2D();
    if (rasterizer.HasFailed()) throw std::runtime_error("Native 2D device has failed");
    if (NativeQueue->Records().empty())
    {
        NativeSource3D = rasterizer.NativeImage;
        NativeDisplay3D = rasterizer.RenderedImage;
        NativeSourceVersion = rasterizer.RenderedVersion;
        NativeSourceScale = NativeSource3D ? rasterizer.RenderedScale : 1;
    }
}

void VulkanRenderer::CompleteNative2D() noexcept
{
    if (!NativePipeline || !NativePipeline->Pending()) return;
    try { NativePipeline->Complete(); }
    catch (const std::exception& error) { FailNative2D(error); }
}

void VulkanRenderer::FinishNative2D(bool deferred) noexcept
{
    // Dependency callers (captures, textures, 3D, lifecycle) rely on this
    // returning with no native batch in flight.
    CompleteNative2D();
    if (!NativePipeline || NativeQueue->Records().empty() || HasRenderFailure()) return;
    auto& rasterizer = static_cast<VulkanRenderer3D&>(*Rend3D);
    const u64 before = rasterizer.TotalSubmissionCount();
    try
    {
        // A deferred batch copied every host input and leases its GPU sources,
        // so the queue retires now. Capture batches still complete inside
        // Submit: CapturedState reads Revision/Snapshot without draining.
        if (deferred)
            NativePipeline->Submit(NativeQueue->Data().Words(), NativeQueue->Records(), NativeSource3D, BackBuffer,
                NativeCapture.get(), NativeCaptures, NativeQueue->TakeCopies(), NativeQueue->Merges(), NativeDisplay3D,
                NativeQueue->Epoch());
        else
            NativePipeline->Render(NativeQueue->Data().Words(), NativeQueue->Records(), NativeSource3D, BackBuffer,
                NativeCapture.get(), NativeCaptures, NativeQueue->TakeCopies(), NativeQueue->Merges(), NativeDisplay3D,
                NativeQueue->Epoch());
        for (u32 screen = 0; screen < 2; ++screen)
        {
            ResidentImages[BackBuffer][screen] = NativePipeline->Output(BackBuffer, screen);
            ResidentCPUValid[BackBuffer][screen] = false;
        }
        NativeQueue->Retire(true);
        NativeCaptures.clear();
        NativeSource3D.reset();
        NativeDisplay3D.reset();
    }
    catch (const std::exception& error) { FailNative2D(error); }
    rasterizer.DisplaySubmissions += rasterizer.TotalSubmissionCount() - before;
}

void VulkanRenderer::DrawSprites(u32 line)
{
    if (NativePipeline && !HasRenderFailure()) DrawNativeSprites(line);
    else SoftRenderer::DrawSprites(line);
}

void VulkanRenderer::DrawNativeSprites(u32 line)
{
    if (HasRenderFailure()) return;
    try
    {
        SyncNativeSources(true);
        // Reserve room before either dirty tracker advances. Retrying a failed
        // OBJ capture with a preserved prefetch view could otherwise reuse that
        // older view after the first attempt consumed its physical dirty bits.
        constexpr size_t spriteBudget = 1024 * 1024;
        if (NativeQueue->Data().Words().size_bytes() > NativeQueue->Data().ByteLimit() - spriteBudget)
        {
            const bool empty = NativeQueue->Records().empty();
            FinishNative2D();
            if (HasRenderFailure()) return;
            if (empty) NativeQueue->Retire();
        }
        const auto captured = CapturedState(*NativeCapture, NativeCaptureDirty, NativeHiresOwned);
        for (u32 engine = 0; engine < 2; ++engine) NativeQueue->CaptureSprites(engine, line, captured);
    }
    catch (const std::exception& error) { FailNative2D(error); }
}

void VulkanRenderer::DrawNativeLine(u32 line)
{
    if (HasRenderFailure()) return;
    try
    {
        if (GPU.CaptureEnable && GPU.VCount < 192) PrepareNativeCapture();
        SyncNativeSources(false);
        // Keep A/B together: capture below borrows A's raw output from this
        // batch. Include CPU baselines plus bounded GPU staging for both engines.
        constexpr size_t pairBudget = 2 * 1024 * 1024;
        if (NativeQueue->Records().size() > Vulkan::Native2D::Pipeline::MaxRecords - 2 ||
            NativeQueue->Data().Words().size_bytes() > NativeQueue->Data().ByteLimit() - pairBudget)
        {
            const bool empty = NativeQueue->Records().empty();
            FinishNative2D();
            if (HasRenderFailure()) return;
            if (empty) NativeQueue->Retire();
        }
        BindNative3D();
        auto captured = CapturedState(*NativeCapture, NativeCaptureDirty, NativeHiresOwned);
        u32 first = 0;
        for (u32 engine = 0; engine < 2; ++engine)
        {
            std::shared_ptr<Vulkan::Device::Buffer> lcdc;
            u32 segments = 0, hiresLCDC = 0;
            if (!engine && GPU.VCount < 192)
            {
                const u32 display = GPU.GPU2D_A.DispCnt, bank = (display >> 18) & 3;
                if (((display >> 16) & 3) == 2 && (GPU.VRAMMap_LCDC & (1u << bank)))
                {
                    for (u32 i = 0; i < 2; ++i)
                    {
                        if (NativeCaptureDirty[bank][GPU.VCount * 2 + i]) segments |= 1u << i;
                        if (NativeHiresOwned[bank][GPU.VCount * 2 + i]) hiresLCDC |= 1u << i;
                    }
                    if (segments) lcdc = NativeCapture->Snapshot();
                    if (hiresLCDC) hiresLCDC |= bank << 2;
                }
            }
            NativeQueue->CaptureLine(engine, line, NativeSourceScale, lcdc, segments, captured, hiresLCDC);
            if (!engine) first = NativeQueue->Records().back().rawOutput;
        }
        captured.source.reset();
        if (GPU.CaptureEnable && GPU.VCount < 192) CaptureNativeLine(first);
        // The last visible row closes the frame's batch. Run it on the GPU
        // while VBlank emulation continues; the 3D start (DrawFrame), any
        // Device::Begin and every dependency/lifecycle path complete it.
        if (line == 191) FinishNative2D(true);
    }
    catch (const std::exception& error) { FailNative2D(error); }
}

void VulkanRenderer::PreSavestate()
{
    FinishDisplayComposition();
    if (NativePipeline && !HasRenderFailure())
    {
        try
        {
            // PreSavestate is called for both save and load. Loaded RAM must
            // never later be replaced by capture data from the discarded state.
            for (u32 bank = 0; bank < 4; ++bank)
                if (NativeCaptureDirty[bank].any()) SyncNativeCaptureRows(bank, 0, 512);
            NativeCaptureWriteThrough = true;
        }
        catch (const std::exception& error) { FailNative2D(error); }
    }
    NativeHiresOwned = {};
    SoftRenderer::PreSavestate();
}

bool VulkanRenderer::GetFramebuffers(void** top, void** bottom)
{
    if (CompositionSubmitted) FinishDisplayComposition();
    try { if (NativePipeline) ReadbackDisplay(BackBuffer ^ 1); }
    catch (const std::exception& error)
    {
        FailNative2D(error);
        *top = *bottom = nullptr;
        return false;
    }
    return SoftRenderer::GetFramebuffers(top, bottom);
}

void VulkanRenderer::SyncNativeCaptureRows(u32 bank, u32 first, u32 count)
{
    if (!NativeCapture || bank >= 4 || first >= 512 || count > 512 - first)
        throw std::invalid_argument("Invalid native capture range");
    auto& dirty = NativeCaptureDirty[bank];
    const u32 end = first + count;
    while (first < end)
    {
        while (first < end && !dirty[first]) ++first;
        const u32 begin = first;
        while (first < end && dirty[first]) ++first;
        if (begin == first) break;
        if (!NativeCaptures.empty())
        {
            FinishNative2D();
            if (HasRenderFailure()) throw std::runtime_error("Native capture submission failed");
        }
        const u32 halfword = begin * 128, words = (first - begin) * 128;
        auto* ram = reinterpret_cast<u16*>(GPU.VRAM[bank]);
        auto& rasterizer = static_cast<VulkanRenderer3D&>(*Rend3D);
        const u64 before = rasterizer.TotalSubmissionCount();
        NativeCapture->ReadRange(bank, halfword, {ram + halfword, words});
        rasterizer.DisplaySubmissions += rasterizer.TotalSubmissionCount() - before;
        for (u32 row = begin; row < first; ++row)
        {
            dirty.reset(row);
            GPU.VRAMDirty[bank][row / 2] = true;
        }
    }
}

void VulkanRenderer::PrepareNativeCapture()
{
    const u32 control = GPU.CaptureCnt;
    if ((NativeCaptureControl ^ control) & 0x003F0000)
    {
        // A live destination/size change invalidates the old capture grouping.
        // Resolve only captured bytes before clearing provenance; CPU edits in
        // untouched rows must survive. New rows then publish through to RAM.
        GPU.SyncAllVRAMCaptures();
        for (u32 bank = 0; bank < 4; ++bank)
            if (NativeCaptureDirty[bank].any()) SyncNativeCaptureRows(bank, 0, 512);
        NativeCaptureWriteThrough = true;
    }
    NativeCaptureControl = control;
}

void VulkanRenderer::CaptureNativeLine(u32 rawFirst)
{
    const u32 control = GPU.CaptureCnt, line = GPU.VCount;
    const u32 size = (control >> 20) & 3, width = size ? 256 : 128, height = size ? size * 64 : 128;
    const u32 bank = (control >> 16) & 3;
    if (line >= height || !(GPU.VRAMMap_LCDC & (1u << bank))) return;

    Vulkan::Native2D::CaptureCommand command{};
    command.row = {control, line, rawFirst, NativeSourceScale, GPU.GPU3D.RenderXPos, GPU.GPU3D.AbortFrame};
    // CPU-owned source B is immutable at this event. GPU-owned segments refer
    // to the bank version at this point in the ordered capture command stream;
    // they need no round trip through RAM even when source aliases destination.
    if (((control >> 29) & 3) != 0)
    {
        if (control & (1u << 25))
        {
            std::copy_n(GPU.DispFIFOBuffer, width, command.sourceB.data());
            command.hasB = 1;
        }
        else
        {
            const u32 display = GPU.GPU2D_A.DispCnt, srcBank = (display >> 18) & 3;
            if (GPU.VRAMMap_LCDC & (1u << srcBank))
            {
                const u32 offset = (line * 256 + (((display >> 16) & 3) == 2 ? 0 :
                    (((control >> 26) & 3) << 14))) & 0xFFFF;
                command.hasB = 1;
                command.sourceWord = srcBank * 32768 + offset / 2;
                for (u32 segment = 0; segment < width / 128; ++segment)
                {
                    if (NativeHiresOwned[srcBank][offset / 128 + segment]) command.hiresMask |= 1u << segment;
                    if (NativeCaptureDirty[srcBank][offset / 128 + segment])
                        command.gpuMask |= 1u << segment;
                    else
                        std::copy_n(reinterpret_cast<const u16*>(GPU.VRAM[srcBank]) + offset + segment * 128,
                            128, command.sourceB.data() + segment * 128);
                }
            }
        }
    }
    const u32 first = (((((control >> 18) & 3) << 14) + line * width) & 0xFFFF) / 128;
    for (u32 i = 0; i < width / 128; ++i)
    {
        // Only armed CBF blocks route later CPU/DMA writes to invalidation.
        const bool eligible = NativeCapture->Hires() && GPU.GetCaptureBlock_LCDC(bank * 131072 + (first + i) * 256) >= 0;
        NativeHiresOwned[bank].set(first + i, eligible);
        if (eligible) command.hiresWriteMask |= 1u << i;
        NativeCaptureDirty[bank].set(first + i);
    }
    if (command.hiresWriteMask && !(control & (1u << 24)) && ((control >> 29) & 3) != 1)
    {
        NativeQueue->CaptureHiresSource(rawFirst, u32(NativeCaptures.size()));
        command.scaledRawFirst = u32(NativeCaptures.size()) * 256 * DisplayScale * DisplayScale;
    }
    NativeCaptures.push_back(command);
    if (NativeCaptureWriteThrough) SyncNativeCaptureRows(bank, first, width / 128);
}

void VulkanRenderer::SyncVRAMCapture(u32 bank, u32 start, u32 size, bool complete)
{
    if (!NativePipeline || HasRenderFailure()) return;
    try
    {
        if (bank >= 4 || start >= 4 || size >= 4)
            throw std::invalid_argument("Invalid native capture layout");
        // The core skips later reads once CBFlag_Synced is set; an incomplete
        // capture must keep RAM current for all subsequent writes this frame.
        if (!complete) NativeCaptureWriteThrough = true;
        for (u32 block = 0; block < std::max(1u, size); ++block)
            SyncNativeCaptureRows(bank, ((start + block) & 3) * 128, 128);
    }
    catch (const std::exception& error) { FailNative2D(error); }
}

void VulkanRenderer::SyncNativeSources(bool sprites)
{
    u32 banks = 0;
    const auto add = [&](const u32* mappings, u32 count) {
        for (u32 i = 0; i < count; ++i) banks |= mappings[i];
    };
    if (sprites)
    {
        if (GPU.GPU2D_A.Enabled && GPU.GPU2D_A.OBJEnable) add(GPU.VRAMMap_AOBJ, 16);
        if (GPU.GPU2D_B.Enabled && GPU.GPU2D_B.OBJEnable) add(GPU.VRAMMap_BOBJ, 8);
    }
    else if (GPU.VCount < 192)
    {
        if (GPU.GPU2D_A.Enabled && !GPU.GPU2D_A.ForcedBlank) add(GPU.VRAMMap_ABG, 32);
        if (GPU.GPU2D_B.Enabled && !GPU.GPU2D_B.ForcedBlank) add(GPU.VRAMMap_BBG, 8);
        const u32 display = GPU.GPU2D_A.DispCnt;
        const u32 bank = (display >> 18) & 3;
        if (((display >> 16) & 3) == 2 && (GPU.VRAMMap_LCDC & (1u << bank)) &&
            (NativeCaptureDirty[bank][GPU.VCount * 2] || NativeCaptureDirty[bank][GPU.VCount * 2 + 1]) &&
            !NativeCaptures.empty())
        {
            // Establish the completed capture version for this LCDC event.
            // Queue retains it; its bytes stay on-device even if this same row
            // is captured again in the new batch.
            FinishNative2D();
            if (HasRenderFailure()) throw std::runtime_error("Native LCDC dependency failed");
        }
    }
    // Captured pages stay on-device. A pending writer must complete before its
    // version can be latched; immutable page sources then survive later writes.
    for (u32 bank = 0; bank < 4; ++bank)
        if ((banks & (1u << bank)) && NativeCaptureDirty[bank].any() && !NativeCaptures.empty())
        {
            FinishNative2D();
            if (HasRenderFailure()) throw std::runtime_error("Native mapped capture dependency failed");
            break;
        }
}

}
