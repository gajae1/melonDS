// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "GPU_Vulkan.h"
#include "GPU3D_Vulkan.h"
#include "Platform.h"
#include <algorithm>
#include <bit>
#include <cstring>

namespace melonDS
{
u32 VulkanRenderer::CaptureBackgroundScale(u32 engine) const
{
    if (DisplayScale == 1) return 0;
    const u32* mapping = engine ? GPU.VRAMMap_BBG : GPU.VRAMMap_ABG;
    for (u32 i = 0; i < (engine ? 8u : 32u); ++i)
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
    const u32 y = offset / capture.width, x = offset % capture.width;
    if (y >= capture.height || !capture.valid[y]) return false;
    const u32 sx = subx * capture.scale / DisplayScale, sy = suby * capture.scale / DisplayScale;
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
        for (u32 x = 0; x < 256; ++x)
        for (u32 sx = 0; sx < scale; ++sx)
        {
            u16 pixel = row ? row[x * scale + sx] : native[vcount * 256 + x];
            if (!row) ReadDisplayCapture(bank, vcount * 256 + x, sx, sy, pixel);
            dst[x * scale + sx] = ((pixel & 31) << 1) | ((pixel & 0x3E0) << 4) | ((pixel & 0x7C00) << 7);
        }
        // Master brightness is applied after display selection, not in capture.
        for (u32 x = 0; x < width; x += 256) ApplyMasterBrightness(GPU.MasterBrightnessA, dst + x);
        ExpandPixels(dst, width);
    }
    return true;
}

void VulkanRenderer::DoCapture(u32 line)
{
    const u32 control = GPU.CaptureCnt, size = (control >> 20) & 3;
    const u32 width = size ? 256 : 128, height = size ? 64 * size : 128;
    const u32 bank = (control >> 16) & 3, start = (control >> 18) & 3;
    if (DisplayScale == 1) DisplayCaptures[bank * 4 + start] = {};
    if (DisplayScale == 1 || line >= height || !(GPU.VRAMMap_LCDC & (1u << bank)))
    {
        SoftRenderer::DoCapture(line);
        return;
    }
    auto& captured = DisplayCaptures[bank * 4 + start];
    if (captured.scale != u32(DisplayScale)) AllocCapture(bank, start, size);
    if (captured.pixels.empty() || captured.width != width || captured.height != height)
    {
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
    for (u32 sy = 0; sy < scale; ++sy)
    {
        if (mode != 1)
        {
            raster.GetScaledLine(line, sy, scale, ScaledLine3D.data());
            if (!(control & (1u << 24)))
                compositor.ComposeScaledLine(ScaledLine3D.data(), ScaledLine3D.data(), scale, sy);
        }
        for (u32 x = 0; x < width; ++x)
        for (u32 sx = 0; sx < scale; ++sx)
        {
            const u32 a = mode == 1 ? 0 : ScaledLine3D[x * scale + sx];
            const u16 ca = ((a >> 1) & 31) | ((a >> 4) & 0x3E0) | ((a >> 7) & 0x7C00) | ((a >> 24) ? 0x8000 : 0);
            u16 cb = 0;
            if (mode != 0 && sourceB)
            {
                cb = sourceB[fifo ? x : sourceOffset + x];
                if (!fifo) ReadDisplayCapture(sourceBank, sourceOffset + x, sx, sy, cb);
            }
            u16 result = mode == 0 ? ca : cb;
            if (mode >= 2)
            {
                const u32 weightA = (ca & 0x8000) ? eva : 0, weightB = (cb & 0x8000) ? evb : 0;
                result = (weightA || weightB) ? 0x8000 : 0;
                for (u32 shift : {0u, 5u, 10u})
                {
                    const u32 color = ((((ca >> shift) & 31) * weightA + ((cb >> shift) & 31) * weightB + 8) >> 4);
                    result |= std::min(color, 31u) << shift;
                }
            }
            CaptureRow[(sy * width + x) * scale + sx] = result;
        }
    }
    // Emulated VRAM is written once by the unchanged native capture algorithm.
    // The sidecar is for presentation only, never for a CPU/DMA read.
    SoftRenderer::DoCapture(line);
    std::copy_n(CaptureRow.data(), size_t(width) * scale * scale,
        captured.pixels.data() + size_t(line) * width * scale * scale);
    captured.valid[line] = true;
}
}
