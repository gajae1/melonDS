// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "GPU_Vulkan.h"
#include "GPU3D_Vulkan.h"
#include "Platform.h"
#include "RenderCost.h"
#include <algorithm>
#include <bit>
#include <cstring>

namespace melonDS
{
using Cost = RenderCostVulkanMeter;
bool VulkanRenderer::CaptureTexturePixels(u32 texparam, u32 scale, std::vector<u32>& pixels) const
{
    if (scale <= 1 || scale != u32(DisplayScale)) return false;
    int info[16];
    GPU.GetCaptureInfo_Texture(info);
    const int block = GetTextureCaptureBlock(texparam, info);
    if (block < 0) return false;
    const u32 width = TextureWidth(texparam), height = TextureHeight(texparam);
    const u32 address = (texparam & 0xFFFF) * 8, count = width * height;
    // The shared GL helper finds a candidate, not full-range provenance.
    // Require every covered page to belong to this one capture; overlapping
    // bank mappings and ordinary VRAM pages are reported as -1 by the GPU.
    const u32 end = (address + count * 2 + 0x7FFF) >> 15;
    for (u32 page = address >> 15; page < end; ++page)
        if (info[page] != block) return false;
    const auto& capture = DisplayCaptures[block];
    if (capture.pixels.empty() || capture.scale != scale) return false;
    const u32 offset = ((address & 0x1FFFF) / 2 - capture.start * 16384) & 0xFFFF;
    if (offset + count > capture.width * capture.height) return false;
    for (u32 row = offset / capture.width; row <= (offset + count - 1) / capture.width; ++row)
        if (!capture.valid[row]) return false;

    // Crop the exact DS texture domain, including non-row-aligned addresses
    // and 128/256 pitch reinterpretation. Sampler clamp/repeat/mirror then
    // operate on the texture, not on a whole capture bank or an offset image.
    pixels.resize(size_t(width) * height * scale * scale);
    const auto unorm = [](u32 channel) {
        const u32 rgb6 = channel ? channel * 2 + 1 : 0;
        // The shared capture shader truncates UNORM * 63. Round upward here
        // to recover the native bitmap decoder's exact nonzero RGB5->RGB6.
        return (rgb6 * 255 + 62) / 63;
    };
    for (u32 y = 0; y < height; ++y)
    for (u32 sy = 0; sy < scale; ++sy)
    for (u32 x = 0; x < width; ++x)
    {
        const u32 word = offset + y * width + x;
        const auto* source = capture.pixels.data() +
            (size_t(word / capture.width * scale + sy) * capture.width + word % capture.width) * scale;
        auto* dest = pixels.data() + (size_t(y * scale + sy) * width + x) * scale;
        for (u32 sx = 0; sx < scale; ++sx)
        {
            const u16 color = source[sx];
            dest[sx] = unorm(color & 31) | (unorm((color >> 5) & 31) << 8) |
                (unorm((color >> 10) & 31) << 16) | ((color & 0x8000) ? 0xFF000000 : 0);
        }
    }
    return true;
}

u32 VulkanRenderer::CaptureBackgroundScale(u32 engine) const
{
    return CaptureMappedScale(engine ? GPU.VRAMMap_BBG : GPU.VRAMMap_ABG, engine ? 8 : 32);
}

u32 VulkanRenderer::CaptureObjectScale(u32 engine) const
{
    return CaptureMappedScale(engine ? GPU.VRAMMap_BOBJ : GPU.VRAMMap_AOBJ, engine ? 8 : 16);
}

u32 VulkanRenderer::CaptureMappedScale(const u32* mapping, u32 count) const
{
    if (DisplayScale == 1) return 0;
    for (u32 i = 0; i < count; ++i)
    {
        const u32 mask = mapping[i];
        // Multiple mapped banks are ORed by hardware; never replace that
        // result with a high-resolution sample from just one bank.
        if (!std::has_single_bit(mask) || !(mask & 15)) continue;
        const u32 first = std::countr_zero(mask) * 4;
        for (u32 slot = first; slot < first + 4; ++slot)
            if (!DisplayCaptures[slot].pixels.empty() &&
                std::any_of(DisplayCaptures[slot].valid.begin(), DisplayCaptures[slot].valid.end(),
                    [](bool valid) { return valid; })) return DisplayScale;
    }
    return 0;
}

void VulkanRenderer::GetCaptureDisplay3DLine(u32 line, u32 subline, u32 scale, u32* dst) const
{
    static_cast<const VulkanRenderer3D&>(*Rend3D).GetScaledLine(line, subline, scale, dst);
}

bool VulkanRenderer::SampleCapturedBackground(u32 engine, u32 address, u32 fracX,
    u32 fracY, u32 denominator, u16& color) const
{
    const u32 mask = engine ? GPU.VRAMMap_BBG[(address >> 14) & 7]
                            : GPU.VRAMMap_ABG[(address >> 14) & 31];
    if (!std::has_single_bit(mask) || !(mask & 15)) return false;
    const u32 bank = std::countr_zero(mask), word = (address & 0x1FFFF) / 2;
    const int slot = GPU.GetCaptureBlock_LCDC(bank * 131072 + word * 2);
    if (slot < 0) return false;
    const auto& capture = DisplayCaptures[slot];
    if (capture.pixels.empty()) return false;
    const u32 offset = (word - capture.start * 16384) & 0xFFFF;
    const u32 y = offset / capture.width, x = offset % capture.width;
    if (y >= capture.height || !capture.valid[y]) return false;
    const u32 sx = capture.scale == denominator ? fracX : u64(fracX) * capture.scale / denominator;
    const u32 sy = capture.scale == denominator ? fracY : u64(fracY) * capture.scale / denominator;
    color = capture.pixels[(size_t(y * capture.scale + sy) * capture.width + x) * capture.scale + sx];
    return true;
}

const u16* VulkanRenderer::CapturedBackgroundRow(u32 engine, u32 address,
    u32 subline, u32 scale) const
{
    const u32 mask = engine ? GPU.VRAMMap_BBG[(address >> 14) & 7]
                            : GPU.VRAMMap_ABG[(address >> 14) & 31];
    return CapturedMappedRow(mask, address, subline, scale);
}

const u16* VulkanRenderer::CapturedObjectRow(u32 engine, u32 address,
    u32 subline, u32 scale) const
{
    const u32 mask = engine ? GPU.VRAMMap_BOBJ[(address >> 14) & 7]
                            : GPU.VRAMMap_AOBJ[(address >> 14) & 15];
    return CapturedMappedRow(mask, address, subline, scale);
}

const u16* VulkanRenderer::CapturedMappedRow(u32 mask, u32 address, u32 subline, u32 scale) const
{
    if (!std::has_single_bit(mask) || !(mask & 15)) return nullptr;
    const u32 bank = std::countr_zero(mask), word = (address & 0x1FFFF) / 2;
    const int slot = GPU.GetCaptureBlock_LCDC(bank * 131072 + word * 2);
    if (slot < 0) return nullptr;
    const auto& capture = DisplayCaptures[slot];
    if (capture.pixels.empty() || capture.scale != scale || subline >= scale) return nullptr;
    const u32 offset = (word - capture.start * 16384) & 0xFFFF;
    const u32 y = offset / capture.width, x = offset % capture.width;
    if (y >= capture.height || !capture.valid[y]) return nullptr;
    return capture.pixels.data() + (size_t(y * scale + subline) * capture.width + x) * scale;
}

void VulkanRenderer::AllocCapture(u32 bank, u32 start, u32 size)
{
    if (DisplayScale == 1) { DisplayCaptures[bank * 4 + start] = {}; return; }
    // The GPU's existing provenance flags invalidate CPU/DMA-written captures.
    // Release obsolete slots so overlapping captures do not retain old images.
    for (u32 slot = bank * 4; slot < bank * 4 + 4; ++slot)
        if (GPU.GetCaptureBlock_LCDC(slot * 32768) != int(slot)) DisplayCaptures[slot] = {};
    auto& capture = DisplayCaptures[bank * 4 + start];
    const u32 width = size ? 256 : 128, height = size ? size * 64 : 128;
    if (capture.width == width && capture.height == height && capture.scale == u32(DisplayScale)) return;
    try
    {
        DisplayCapture next{width, height, u32(DisplayScale), start};
        next.pixels.resize(size_t(width) * height * DisplayScale * DisplayScale);
        capture = std::move(next);
    }
    catch (const std::exception& error)
    {
        capture = {};
        Platform::Log(Platform::LogLevel::Warn, "Vulkan capture enhancement unavailable: %s\n", error.what());
    }
}

bool VulkanRenderer::ReadDisplayCapture(u32 bank, u32 word, u32 subx, u32 suby, u16& color) const
{
    word &= 0xFFFF;
    const int slot = GPU.GetCaptureBlock_LCDC(bank * 131072 + word * 2);
    if (slot < 0) return false;
    const auto& capture = DisplayCaptures[slot];
    if (capture.pixels.empty()) return false;
    const u32 offset = (word - capture.start * 16384) & 0xFFFF;
    // AllocCapture stores only 128/256-wide images. Keep every provenance
    // lookup, but do not divide by the pitch or an identical scale per sample.
    const u32 y = offset >> (capture.width == 256 ? 8 : 7), x = offset & (capture.width - 1);
    if (y >= capture.height || !capture.valid[y]) return false;
    const bool sameScale = capture.scale == u32(DisplayScale);
    const u32 sx = sameScale ? subx : subx * capture.scale / DisplayScale;
    const u32 sy = sameScale ? suby : suby * capture.scale / DisplayScale;
    color = capture.pixels[(size_t(y * capture.scale + sy) * capture.width + x) * capture.scale + sx];
    return true;
}

bool VulkanRenderer::DrawCapturedDisplay(u32 line)
{
    const u32 display = GPU.GPU2D_A.DispCnt, vcount = GPU.VCount;
    if (DisplayScale == 1 || !GPU.ScreensEnabled || vcount >= 192 || ((display >> 16) & 3) != 2) return false;
    const u32 bank = (display >> 18) & 3;
    if (!(GPU.VRAMMap_LCDC & (1u << bank))) return false;
    u16 first;
    if (!ReadDisplayCapture(bank, vcount * 256, 0, 0, first)) return false;
    RenderCostVulkanScope lcdc(Costs(), Cost::LCDC);
    const auto* native = reinterpret_cast<const u16*>(GPU.VRAM[bank]);
    const u32 scale = DisplayScale, width = 256 * scale;
    const int screen = GPU.ScreenSwap ? 0 : 1;
    const auto& capture = DisplayCaptures[GPU.GetCaptureBlock_LCDC(bank * 131072 + vcount * 512)];
    const u32 captureY = ((vcount * 256 - capture.start * 16384) & 0xFFFF) / capture.width;
    for (u32 sy = 0; sy < scale; ++sy)
    {
        auto* dst = ScaledBuffers[BackBuffer][screen].data() + (size_t(line) * scale + sy) * width;
        // Same-scale full-width captures are contiguous; avoid provenance
        // lookup and coordinate division for each displayed subpixel.
        const u16* row = capture.width == 256 && capture.scale == scale
            ? capture.pixels.data() + size_t(captureY * scale + sy) * width : nullptr;
        if (row)
        {
            // Keep the contiguous conversion independent of the provenance
            // sampler so it can vectorize across the whole scaled scanline.
            std::transform(row, row + width, dst, [](u16 pixel) {
                return ((pixel & 31) << 1) | ((pixel & 0x3E0) << 4) | ((pixel & 0x7C00) << 7);
            });
        }
        else
        {
            for (u32 x = 0; x < 256; ++x)
            for (u32 sx = 0; sx < scale; ++sx)
            {
                u16 pixel = native[vcount * 256 + x];
                ReadDisplayCapture(bank, vcount * 256 + x, sx, sy, pixel);
                dst[x * scale + sx] = ((pixel & 31) << 1) | ((pixel & 0x3E0) << 4) | ((pixel & 0x7C00) << 7);
            }
        }
        // Master brightness is applied after display selection, not in capture.
        for (u32 x = 0; x < width; x += 256) ApplyMasterBrightness(GPU.MasterBrightnessA, dst + x);
        ExpandPixels(dst, width);
    }
    return true;
}

void VulkanRenderer::DoCapture(u32 line)
{
    // Nested full-image readback, memcpy, conversion and native capture have
    // their own scopes. Only the sidecar's exclusive host work remains here.
    RenderCostVulkanScope capture(Costs(), Cost::Capture);
    const u32 control = GPU.CaptureCnt, size = (control >> 20) & 3;
    const u32 width = size ? 256 : 128, height = size ? 64 * size : 128;
    const u32 bank = (control >> 16) & 3, start = (control >> 18) & 3;
    if (DisplayScale == 1) DisplayCaptures[bank * 4 + start] = {};
    if (DisplayScale == 1 || line >= height || !(GPU.VRAMMap_LCDC & (1u << bank)))
    {
        RenderCostVulkanScope native(Costs(), Cost::NativeCapture);
        SoftRenderer::DoCapture(line);
        return;
    }
    auto& captured = DisplayCaptures[bank * 4 + start];
    if (captured.scale != u32(DisplayScale)) AllocCapture(bank, start, size);
    if (captured.pixels.empty() || captured.width != width || captured.height != height)
    {
        RenderCostVulkanScope native(Costs(), Cost::NativeCapture);
        SoftRenderer::DoCapture(line);
        return;
    }
    const auto& raster = static_cast<const VulkanRenderer3D&>(*Rend3D);
    const auto& compositor = static_cast<const SoftRenderer2D&>(*Rend2D_A);
    const u32 scale = DisplayScale, mode = (control >> 29) & 3;
    const u32 eva = std::min(control & 31, 16u), evb = std::min((control >> 8) & 31, 16u);
    const u32 display = GPU.GPU2D_A.DispCnt, sourceBank = (display >> 18) & 3;
    const bool fifo = control & (1u << 25);
    const u16* sourceB = fifo ? GPU.DispFIFOBuffer :
        (GPU.VRAMMap_LCDC & (1u << sourceBank)) ? reinterpret_cast<const u16*>(GPU.VRAM[sourceBank]) : nullptr;
    const u32 sourceOffset = (line * 256 + (((display >> 16) & 3) == 2 ? 0 : ((control >> 26) & 3) * 16384)) & 0xFFFF;
    const u16* nativeB = sourceB ? sourceB + (fifo ? 0 : sourceOffset) : nullptr;
    const u16* capturedB = nullptr;
    bool sampleB = false;
    if (mode != 0 && sourceB && !fifo)
    {
        // sourceOffset is 256-word aligned and width <= 256: this run stays
        // in one provenance page and one row of a full-width capture. Neither
        // provenance nor the source pixels change until the native write below.
        const int slot = GPU.GetCaptureBlock_LCDC(sourceBank * 131072 + sourceOffset * 2);
        if (slot >= 0)
        {
            const auto& source = DisplayCaptures[slot];
            if (!source.pixels.empty())
            {
                if (source.width == 256 && source.scale == scale)
                {
                    const u32 y = ((sourceOffset - source.start * 16384) & 0xFFFF) >> 8;
                    if (y < source.height && source.valid[y])
                        capturedB = source.pixels.data() + size_t(y) * 256 * scale * scale;
                }
                else sampleB = true; // Preserve narrow-row and scale-mismatch sampling.
            }
        }
    }
    for (u32 sy = 0; sy < scale; ++sy)
    {
        if (mode != 1)
        {
            raster.GetScaledLine(line, sy, scale, ScaledLine3D.data());
            if (!(control & (1u << 24)))
                compositor.ComposeScaledLine(ScaledLine3D.data(), ScaledLine3D.data(), scale, sy);
        }
        if (mode == 0)
        {
            // Source A already contains every subpixel (including composed
            // 2D). Convert one contiguous run without per-pixel mode/sampler
            // branches; retain CaptureRow's snapshot and native-write boundary.
            std::transform(ScaledLine3D.data(), ScaledLine3D.data() + width * scale,
                CaptureRow.data() + sy * width * scale, [](u32 a) -> u16 {
                    return ((a >> 1) & 31) | ((a >> 4) & 0x3E0) | ((a >> 7) & 0x7C00) | ((a >> 24) ? 0x8000 : 0);
                });
            continue;
        }
        auto* row = CaptureRow.data() + sy * width * scale;
        if (capturedB)
            std::copy_n(capturedB + sy * 256 * scale, width * scale, row);
        else if (sampleB)
        {
            for (u32 x = 0; x < width; ++x)
            for (u32 sx = 0; sx < scale; ++sx)
            {
                u16 cb = nativeB[x];
                ReadDisplayCapture(sourceBank, sourceOffset + x, sx, sy, cb);
                row[x * scale + sx] = cb;
            }
        }
        else if (nativeB)
        {
            for (u32 x = 0; x < width; ++x)
                std::fill_n(row + x * scale, scale, nativeB[x]);
        }
        else std::fill_n(row, width * scale, 0);

        // Mode 1 is only the B gather above. Blend a contiguous scaled row
        // separately, without mode/FIFO/provenance branches per subpixel.
        if (mode >= 2)
        {
            std::transform(ScaledLine3D.data(), ScaledLine3D.data() + width * scale,
                row, row, [eva, evb](u32 a, u16 cb) -> u16 {
                    const u16 ca = ((a >> 1) & 31) | ((a >> 4) & 0x3E0) | ((a >> 7) & 0x7C00) | ((a >> 24) ? 0x8000 : 0);
                    const u32 weightA = (ca & 0x8000) ? eva : 0, weightB = (cb & 0x8000) ? evb : 0;
                    u16 result = (weightA || weightB) ? 0x8000 : 0;
                    for (u32 shift : {0u, 5u, 10u})
                    {
                        const u32 color = ((((ca >> shift) & 31) * weightA + ((cb >> shift) & 31) * weightB + 8) >> 4);
                        result |= std::min(color, 31u) << shift;
                    }
                    return result;
                });
        }
    }
    // Emulated VRAM is written once by the unchanged native capture algorithm.
    // The sidecar is for presentation only, never for a CPU/DMA read.
    {
        RenderCostVulkanScope native(Costs(), Cost::NativeCapture);
        SoftRenderer::DoCapture(line);
    }
    {
        RenderCostVulkanScope copy(Costs(), Cost::CaptureCopy);
        std::copy_n(CaptureRow.data(), size_t(width) * scale * scale,
            captured.pixels.data() + size_t(line) * width * scale * scale);
        if (Costs()) Costs()->Transfer(Cost::CaptureCopyBytes, uint64_t(width) * scale * scale * sizeof(u16));
    }
    captured.valid[line] = true;
}
}
