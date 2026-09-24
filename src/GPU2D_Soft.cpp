/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "GPU_Soft.h"
#include "GPU_ColorOp.h"
#include <algorithm>
#include <bit>
#include "Platform.h"

namespace melonDS
{

SoftRenderer2D::SoftRenderer2D(melonDS::GPU2D& gpu2D, SoftRenderer& parent)
    : Renderer2D(gpu2D), Parent(parent)
{
    // mosaic table is initialized at compile-time
}

SoftRenderer2D::~SoftRenderer2D()
{
}

void SoftRenderer2D::Reset()
{
    memset(BGOBJLine, 0, sizeof(BGOBJLine));
    memset(WindowMask, 0, sizeof(WindowMask));
    memset(OBJLine, 0, sizeof(OBJLine));
    memset(OBJWindow, 0, sizeof(OBJWindow));

    NumSprites = 0;
    Scaled3DActive = false;
    CaptureLayersActive = false;
    CaptureOBJScale = 0;
    CaptureOBJActive = false;
    CaptureOBJMosaic = false;
    CaptureOBJProvenance = {};
}

bool SoftRenderer2D::ExportScaledContext(ScaledLineContext& context, u16 brightness) const
{
    if (!Scaled3DActive || CaptureLayersActive) return false;
    for (u32 x = 0; x < 256; ++x)
        context.pixels[x] = {BGOBJLine[x], BGOBJLine[x + 256],
            Below3D[x], Below3D[x + 256], WindowMask[x]};
    context.blendCnt = GPU2D.BlendCnt;
    context.eva = GPU2D.EVA;
    context.evb = GPU2D.EVB;
    context.evy = GPU2D.EVY;
    context.masterBrightness = brightness;
    context.mode = ScaledLineContext::Composite3D;
    return true;
}

// Keep RGB6 components in separate 16-bit lanes for the weighted sum. The
// largest normal sum is 63 * (16 + 16) + 8, so no carry crosses a lane.
static u32 ColorBlend4Packed(u32 val1, u32 val2, u32 eva, u32 evb)
{
    // Also preserve the old unsigned arithmetic for out-of-range state/flags.
    if (eva > 16 || evb > 16) return ColorBlend4(val1, val2, eva, evb);

    const u64 rgb1 = (val1 & 0x3F003F) | (u64(val1 & 0x003F00) << 24);
    const u64 rgb2 = (val2 & 0x3F003F) | (u64(val2 & 0x003F00) << 24);
    u64 rgb = (rgb1 * eva + rgb2 * evb + 0x000800080008ULL) >> 4;
    const u64 overflow = rgb & 0x004000400040ULL;
    rgb = (rgb | (overflow - (overflow >> 6))) & 0x003F003F003FULL;
    return u32(rgb & 0x3F003F) | u32((rgb >> 24) & 0x003F00) | 0xFF000000;
}

static u32 ColorBlend5Packed(u32 val1, u32 val2)
{
    const u32 eva = ((val1 >> 24) & 0x1F) + 1;
    if (eva == 32) return val1;

    const u64 rgb1 = (val1 & 0x3F003F) | (u64(val1 & 0x003F00) << 24);
    const u64 rgb2 = (val2 & 0x3F003F) | (u64(val2 & 0x003F00) << 24);
    // EVA + EVB = 32: the rounded result cannot exceed 63 in any lane.
    const u64 rgb = (rgb1 * eva + rgb2 * (32 - eva) + 0x001000100010ULL) >> 5;
    return u32(rgb & 0x3F003F) | u32((rgb >> 24) & 0x003F00) | 0xFF000000;
}

template<u32 effect>
static u32 CompositePixel(u32 val1, u32 val2, u32 blendCnt, u32 eva, u32 evb, u32 evy, u8 window)
{
    u32 flag1 = val1 >> 24;
    if constexpr (effect == 1)
        if (!(flag1 & 0xC0) && (!(blendCnt & flag1) || !(window & 0x20))) return val1;
    if (effect == 1 || (flag1 & 0xC0))
    {
        const u32 flag2 = val2 >> 24;
        const u32 target2 = (flag2 & 0x80) ? 0x1000 : (flag2 & 0x40) ? 0x0100 : flag2 << 8;
        if (blendCnt & target2)
        {
            if ((flag1 & 0xC0) == 0x40) return ColorBlend5Packed(val1, val2);
            if ((flag1 & 0xC0) == 0xC0)
                return ColorBlend4Packed(val1, val2, flag1 & 0x1F, 16 - (flag1 & 0x1F));
            return ColorBlend4Packed(val1, val2, eva, evb);
        }
    }
    if constexpr (effect >= 2)
    {
        if (flag1 & 0x80) flag1 = 0x10;
        else if (flag1 & 0x40) flag1 = 0x01;
        if ((blendCnt & flag1) && (window & 0x20))
        {
            if constexpr (effect == 2) return ColorBrightnessUp(val1, evy, 0x8);
            else return ColorBrightnessDown(val1, evy, 0x7);
        }
    }
    return val1;
}

// Replaces the per-pixel coordinate scan in GPU2D::CalculateWindowMask.
// The horizontal latch only changes at the x1/x2 events, so the masked
// pixels form at most two intervals. The latch still updates while the
// window is vertically inactive, and for x1 > x2 it survives the end of
// the line (set at x1, cleared at x2). For x1 == x2 the clear at x2 runs
// last, so no pixels are covered and the latch ends cleared.
static void OverlayWindowInterval(u8* windowMask, u8 x1, u8 x2, u8& active, u8 winCnt)
{
    const bool latched = active == 0x3;
    const bool covered = (active | 0x2) == 0x3; // vertically active
    if (x1 < x2)
    {
        if (latched) memset(windowMask, winCnt, x1);
        if (covered) memset(windowMask + x1, winCnt, x2 - x1);
    }
    else
    {
        if (latched) memset(windowMask, winCnt, x2);
        if (x1 > x2 && covered) memset(windowMask + x1, winCnt, 256 - x1);
    }
    active = u8((active & ~0x2) | (x1 > x2 ? 0x2 : 0x0));
}

static void CalculateWindowMaskIntervals(GPU2D& gpu2D, u8* windowMask, const u8* objWindow)
{
    memset(windowMask, gpu2D.WinCnt[2], 256); // window outside

    if (gpu2D.DispCnt & (1<<15))
    {
        // OBJ window
        for (int i = 0; i < 256; i++)
        {
            if (objWindow[i])
                windowMask[i] = gpu2D.WinCnt[3];
        }
    }

    if (gpu2D.DispCnt & (1<<14))
        OverlayWindowInterval(windowMask, gpu2D.Win1Coords[0], gpu2D.Win1Coords[1],
            gpu2D.Win1Active, gpu2D.WinCnt[1]);

    if (gpu2D.DispCnt & (1<<13))
        OverlayWindowInterval(windowMask, gpu2D.Win0Coords[0], gpu2D.Win0Coords[1],
            gpu2D.Win0Active, gpu2D.WinCnt[0]);
}

void SoftRenderer2D::Resolve3DPixel(int x, u32 color, u32& top, u32& second) const
{
    if (((top >> 24) & 0xC0) == 0x40)
    {
        if (color >> 24) top = color | 0x40000000;
        else { top = Below3D[x]; second = Below3D[x + 256]; }
    }
    else if (((second >> 24) & 0xC0) == 0x40)
        second = (color >> 24) ? color | 0x40000000 : Below3D[x];
}

template<u32 effect>
void SoftRenderer2D::ColorComposite(u32* dst) const
{
    const u32 control = GPU2D.BlendCnt;
    const u32 eva = GPU2D.EVA, evb = GPU2D.EVB, evy = GPU2D.EVY;
    if constexpr (effect <= 1)
    {
        if (!Scaled3DActive && !(control & 0x3F00))
        {
            memcpy(dst, BGOBJLine, 256 * sizeof(u32));
            return;
        }
    }
    else if (!(control & 0x3F00) && (control & 0x3F) == 0x3F)
    {
        // With no target2 blend and every target1 layer selected, brightness
        // applies to any non-null top flag. Keep the per-pixel checks so a
        // closed effect window or an unusual zero flag still takes the generic path.
        bool fallback = false;
        for (int x = 0; x < 256; ++x)
        {
            u32 top = BGOBJLine[x];
            if (Scaled3DActive)
            {
                u32 second = BGOBJLine[x + 256];
                Resolve3DPixel(x, Parent.Output3D[x], top, second);
            }
            if (!(top >> 24) || !(WindowMask[x] & 0x20))
            {
                fallback = true;
                break;
            }
            dst[x] = effect == 2 ? ColorBrightnessUp(top, evy, 0x8)
                                 : ColorBrightnessDown(top, evy, 0x7);
        }
        if (!fallback) return;
    }
    for (int x = 0; x < 256; ++x)
    {
        u32 top = BGOBJLine[x], second = BGOBJLine[x + 256];
        if (Scaled3DActive) Resolve3DPixel(x, Parent.Output3D[x], top, second);
        dst[x] = CompositePixel<effect>(top, second, control, eva, evb, evy, WindowMask[x]);
    }
}

template<u32 effect>
void SoftRenderer2D::ComposeScaledLine(u32* dst, const u32* pixels3D, int scale) const
{
    const u32 control = GPU2D.BlendCnt;
    const u32 eva = GPU2D.EVA, evb = GPU2D.EVB, evy = GPU2D.EVY;
    for (int x = 0; x < 256; ++x)
    {
        const u32 top0 = BGOBJLine[x], second0 = BGOBJLine[x + 256];
        u32* out = dst + size_t(x) * scale;
        const u32* src = pixels3D + size_t(x) * scale;
        if (((top0 >> 24) & 0xC0) != 0x40 && ((second0 >> 24) & 0xC0) != 0x40)
        {
            // Resolve3DPixel only rewrites placeholder slots; without one the
            // composite cannot see the 3D buffer and is uniform per subpixel.
            std::fill_n(out, scale,
                CompositePixel<effect>(top0, second0, control, eva, evb, evy, WindowMask[x]));
            continue;
        }
        // Every transparent subpixel resolves to the same per-pixel outcome.
        u32 topT = top0, secondT = second0;
        Resolve3DPixel(x, 0, topT, secondT);
        const u32 transparent =
            CompositePixel<effect>(topT, secondT, control, eva, evb, evy, WindowMask[x]);
        u32 lastColor = 0, lastResult = 0;
        for (int sub = 0; sub < scale; ++sub)
        {
            const u32 color = src[sub];
            u32 result;
            if (!(color >> 24))
            {
                result = transparent;
            }
            // Opaque subpixels still resolve individually; a repeated color
            // (flat fills, texel interiors) reuses the previous composite.
            else if (color == lastColor)
            {
                result = lastResult;
            }
            else
            {
                u32 top = top0, second = second0;
                Resolve3DPixel(x, color, top, second);
                result = CompositePixel<effect>(top, second, control, eva, evb, evy, WindowMask[x]);
                lastColor = color;
                lastResult = result;
            }
            out[sub] = result;
        }
    }
}

void SoftRenderer2D::ComposeScaledLine(u32* dst, const u32* pixels3D, int scale, int subline) const
{
    if (CaptureLayersActive && CaptureScale == u32(scale))
    {
        std::copy_n(CaptureOutput.data() + size_t(subline) * 256 * scale, 256 * scale, dst);
        return;
    }
    if (!Scaled3DActive)
    {
        for (int x = 0; x < 256; ++x)
            std::fill_n(dst + x * scale, scale, Parent.Output2D[GPU2D.Num][x]);
        return;
    }
    switch ((GPU2D.BlendCnt >> 6) & 3)
    {
        case 0: ComposeScaledLine<0>(dst, pixels3D, scale); break;
        case 1: ComposeScaledLine<1>(dst, pixels3D, scale); break;
        case 2: ComposeScaledLine<2>(dst, pixels3D, scale); break;
        case 3: ComposeScaledLine<3>(dst, pixels3D, scale); break;
    }
}

u32 SoftRenderer2D::SampleBitmapLayer(u32 layer, u32 x, u32 subx, u32 suby) const
{
    const auto& bitmap = BitmapLines[layer - 2];
    const u32 native = DisplayLayers[layer][x];
    if (!bitmap.enabled || !(WindowMask[x] & (1u << layer))) return native;
    const u32 dimensions = bitmap.control >> 14;
    const u32 width = dimensions == 0 ? 128 : dimensions == 1 ? 256 : 512;
    const u32 height = dimensions == 0 ? 128 : dimensions == 3 ? 512 : 256;
    const u32 mask = GPU2D.Num ? 0x1FFFF : 0x7FFFF;
    const u32 base = (bitmap.control & 0x1F00) << 6;
    u32 address, fracX, fracY, denominator;
    if (bitmap.a == 256 && bitmap.b == 0 && bitmap.c == 0 && bitmap.d == 256 &&
        ((bitmap.x | bitmap.y) & 255) == 0)
    {
        // Common captured-screen path: integer translation and a 1:1 affine
        // matrix. Keep the exact subpixel, avoiding division by the scale.
        const s64 px = s64(bitmap.x) / 256 + x, py = s64(bitmap.y) / 256;
        if (!(bitmap.control & (1u << 13)) &&
            (px < 0 || py < 0 || px >= width || py >= height)) return 0;
        address = (base + (((u32(py) & (height - 1)) * width +
            (u32(px) & (width - 1))) * 2)) & mask;
        fracX = subx; fracY = suby; denominator = CaptureScale;
    }
    else
    {
        const s64 unit = 256 * CaptureScale;
        // Signed wide intermediates preserve negative affine coordinates.
        s64 fx = (s64(bitmap.x) + s64(x) * bitmap.a) * CaptureScale +
            s64(subx) * bitmap.a + s64(suby) * bitmap.b;
        s64 fy = (s64(bitmap.y) + s64(x) * bitmap.c) * CaptureScale +
            s64(subx) * bitmap.c + s64(suby) * bitmap.d;
        if (bitmap.control & (1u << 13))
        {
            fx %= width * unit; if (fx < 0) fx += width * unit;
            fy %= height * unit; if (fy < 0) fy += height * unit;
        }
        else if (fx < 0 || fy < 0 || fx >= width * unit || fy >= height * unit) return 0;
        address = (base + ((fy / unit) * width + fx / unit) * 2) & mask;
        fracX = fx % unit; fracY = fy % unit; denominator = unit;
    }
    u16 color;
    if (!Parent.SampleCapturedBackground(GPU2D.Num, address, fracX, fracY, denominator, color)) return native;
    if (!(color & 0x8000)) return 0;
    return ((color & 31) << 1) | ((color & 0x3E0) << 4) |
        ((color & 0x7C00) << 7) | (0x01000000u << layer);
}

template<u32 effect>
void SoftRenderer2D::ComposeCapturedLine(u32* dst, u32 subline) const
{
    // Resolve before the subpixel loop. These pointers never survive this
    // synchronous composition or a subsequent capture write/invalidation.
    std::array<std::array<const u16*, 256>, 2> rows{};
    for (u32 layer = 2; layer < 4; ++layer)
    {
        const auto& bitmap = BitmapLines[layer - 2];
        if (!bitmap.enabled || bitmap.a != 256 || bitmap.b != 0 || bitmap.c != 0 ||
            bitmap.d != 256 || ((bitmap.x | bitmap.y) & 255) != 0) continue;
        const u32 dimensions = bitmap.control >> 14;
        const u32 width = dimensions == 0 ? 128 : dimensions == 1 ? 256 : 512;
        const u32 height = dimensions == 0 ? 128 : dimensions == 3 ? 512 : 256;
        const u32 mask = GPU2D.Num ? 0x1FFFF : 0x7FFFF;
        const u32 base = (bitmap.control & 0x1F00) << 6;
        const s64 py = s64(bitmap.y) / 256;
        for (u32 x = 0; x < 256; ++x)
        {
            if (!(WindowMask[x] & (1u << layer))) continue;
            const s64 px = s64(bitmap.x) / 256 + x;
            if (!(bitmap.control & (1u << 13)) &&
                (px < 0 || py < 0 || px >= width || py >= height)) continue;
            const u32 address = (base + (((u32(py) & (height - 1)) * width +
                (u32(px) & (width - 1))) * 2)) & mask;
            rows[layer - 2][x] = Parent.CapturedBackgroundRow(GPU2D.Num, address, subline, CaptureScale);
        }
    }
    const bool capturedOBJ = CaptureOBJActive && CaptureOBJScale == CaptureScale &&
        (GPU2D.LayerEnable & 0x10) && NumSprites;
    const u16* objPal = reinterpret_cast<const u16*>(&GPU.Palette[GPU2D.Num ? 0x600 : 0x200]);
    const u16* objExtPal = capturedOBJ ? GPU2D.GetOBJExtPal() : nullptr;
    const u32 control = GPU2D.BlendCnt;
    const u32 eva = GPU2D.EVA, evb = GPU2D.EVB, evy = GPU2D.EVY;
    const u32 bgRanks[4] = {(GPU2D.BGCnt[0] & 3) * 8 + 1u,
        (GPU2D.BGCnt[1] & 3) * 8 + 2u, (GPU2D.BGCnt[2] & 3) * 8 + 3u,
        (GPU2D.BGCnt[3] & 3) * 8 + 4u};
    for (u32 x = 0; x < 256; ++x)
    {
        const u8 window = WindowMask[x];
        u32 staticTop = DisplayBackdrop, staticSecond = 0;
        u32 staticTopRank = 32, staticSecondRank = 33;
        u32 varying[4], numVarying = 0;
        for (u32 layer = 0; layer < 5; ++layer)
        {
            const u32 color = DisplayLayers[layer][x];
            // Enabled captured BGs can be opaque even at a transparent native
            // sample or without a borrowed row. Let the sampler decide.
            if ((layer == 0 && color == 0x40000000) ||
                (layer >= 2 && layer < 4 && BitmapLines[layer - 2].enabled &&
                    (window & (1u << layer))) || (layer == 4 && capturedOBJ))
            {
                varying[numVarying++] = layer;
                continue;
            }
            if (!color) continue;
            const u32 rank = layer == 4 ? ((OBJLine[x] >> 16) & 3) * 8 : bgRanks[layer];
            if (rank < staticTopRank)
            {
                staticSecond = staticTop; staticSecondRank = staticTopRank;
                staticTop = color; staticTopRank = rank;
            }
            else if (rank < staticSecondRank) { staticSecond = color; staticSecondRank = rank; }
        }
        u32* out = dst + x * CaptureScale;
        if (!numVarying)
        {
            std::fill_n(out, CaptureScale,
                CompositePixel<effect>(staticTop, staticSecond, control, eva, evb, evy, window));
            continue;
        }
        for (u32 subx = 0; subx < CaptureScale; ++subx)
        {
            // Retain two static candidates: transparent dynamic pixels may
            // expose either one. All metadata is local to this composition.
            u32 top = staticTop, second = staticSecond;
            u32 topRank = staticTopRank, secondRank = staticSecondRank;
            for (u32 i = 0; i < numVarying; ++i)
            {
                const u32 layer = varying[i];
                u32 color, rank;
                if (layer == 4)
                {
                    const u32 object = CaptureOBJLine[(size_t(subline) * 256 + x) * CaptureScale + subx];
                    color = 0;
                    if ((object & OBJ_IsOpaque) && (window & 0x10))
                    {
                        const u16 pixel = object & OBJ_DirectColor ? object & 0x7FFF :
                            object & OBJ_StandardPal ? objPal[object & 0xFF] : objExtPal[object & 0xFFF];
                        color = ((pixel & 31) << 1) | ((pixel & 0x3E0) << 4) |
                            ((pixel & 0x8000) >> 7) | ((pixel & 0x7C00) << 7) | (object & 0xFF000000);
                    }
                    rank = ((object >> 16) & 3) * 8;
                }
                else
                {
                    rank = bgRanks[layer];
                    if (layer == 0)
                    {
                        // Read the incoming 3D sample before overwriting dst.
                        color = out[subx];
                        color = color >> 24 ? color | 0x40000000 : 0;
                    }
                    else if (const auto* row = rows[layer - 2][x])
                    {
                        const u16 pixel = row[subx];
                        color = pixel & 0x8000 ? ((pixel & 31) << 1) | ((pixel & 0x3E0) << 4) |
                            ((pixel & 0x7C00) << 7) | (0x01000000u << layer) : 0;
                    }
                    else color = SampleBitmapLayer(layer, x, subx, subline);
                }
                if (!color) continue;
                if (rank < topRank) { second = top; secondRank = topRank; top = color; topRank = rank; }
                else if (rank < secondRank) { second = color; secondRank = rank; }
            }
            out[subx] = CompositePixel<effect>(top, second, control, eva, evb, evy, window);
        }
    }
}

void SoftRenderer2D::PrepareCapturedLine(u32 line)
{
    try { CaptureOutput.resize(size_t(256) * CaptureScale * CaptureScale); }
    catch (const std::exception& error)
    {
        CaptureLayersActive = false;
        Platform::Log(Platform::LogLevel::Warn, "Captured layer display unavailable: %s\n", error.what());
        return;
    }
    for (u32 sub = 0; sub < CaptureScale; ++sub)
    {
        auto* dst = CaptureOutput.data() + sub * 256 * CaptureScale;
        if (Scaled3DActive) Parent.GetCaptureDisplay3DLine(line, sub, CaptureScale, dst);
        switch ((GPU2D.BlendCnt >> 6) & 3)
        {
            case 0: ComposeCapturedLine<0>(dst, sub); break;
            case 1: ComposeCapturedLine<1>(dst, sub); break;
            case 2: ComposeCapturedLine<2>(dst, sub); break;
            case 3: ComposeCapturedLine<3>(dst, sub); break;
        }
    }
}

void SoftRenderer2D::DrawScanline(u32 line)
{
    Scaled3DActive = false;
    CaptureLayersActive = false;
    u32* dst = Parent.Output2D[GPU2D.Num];

    if (!GPU2D.Enabled)
    {
        // if this 2D unit is disabled in POWCNT, the output is a fixed color
        // (black for unit A, white for unit B)
        u32 fillcolor = (GPU2D.Num == 0) ? 0xFF000000 : 0xFF3F3F3F;
        for (int i = 0; i < 256; i++)
            dst[i] = fillcolor;

        return;
    }

    if (GPU2D.ForcedBlank)
    {
        // forced blank
        for (int i = 0; i < 256; i++)
            dst[i] = 0xFF3F3F3F;

        return;
    }

    if (GPU2D.Num == 0)
    {
        auto bgDirty = GPU.VRAMDirty_ABG.DeriveState(GPU.VRAMMap_ABG, GPU);
        GPU.MakeVRAMFlat_ABGCoherent(bgDirty);
        auto bgExtPalDirty = GPU.VRAMDirty_ABGExtPal.DeriveState(GPU.VRAMMap_ABGExtPal, GPU);
        GPU.MakeVRAMFlat_ABGExtPalCoherent(bgExtPalDirty);
        auto objExtPalDirty = GPU.VRAMDirty_AOBJExtPal.DeriveState(&GPU.VRAMMap_AOBJExtPal, GPU);
        GPU.MakeVRAMFlat_AOBJExtPalCoherent(objExtPalDirty);
    }
    else
    {
        auto bgDirty = GPU.VRAMDirty_BBG.DeriveState(GPU.VRAMMap_BBG, GPU);
        GPU.MakeVRAMFlat_BBGCoherent(bgDirty);
        auto bgExtPalDirty = GPU.VRAMDirty_BBGExtPal.DeriveState(GPU.VRAMMap_BBGExtPal, GPU);
        GPU.MakeVRAMFlat_BBGExtPalCoherent(bgExtPalDirty);
        auto objExtPalDirty = GPU.VRAMDirty_BOBJExtPal.DeriveState(&GPU.VRAMMap_BOBJExtPal, GPU);
        GPU.MakeVRAMFlat_BOBJExtPalCoherent(objExtPalDirty);
    }

    const u32 mode = GPU2D.DispCnt & 7;
    const auto directBitmap = [&](u32 bg) {
        return (GPU2D.LayerEnable & (1u << bg)) && (GPU2D.BGCnt[bg] & 0xC4) == 0x84;
    };
    const bool capturedBG = (mode == 5 && directBitmap(2)) || (mode >= 3 && mode <= 5 && directBitmap(3));
    CaptureScale = Parent.ScaledDisplay && capturedBG ? Parent.CaptureBackgroundScale(GPU2D.Num) : 0;
    if (Parent.ScaledDisplay && (GPU2D.LayerEnable & 0x10) && NumSprites && CaptureOBJActive)
        CaptureScale = std::max(CaptureScale, CaptureOBJScale);
    CaptureLayersActive = CaptureScale > 1;
    if (CaptureLayersActive) { DisplayLayers = {}; BitmapLines = {}; }
    // Native registers and layer rendering execute once. Resolve enhanced
    // samples now, before this scanline can overwrite a capture source.
    DrawScanline_BGOBJ(line, dst);
    if (CaptureLayersActive) PrepareCapturedLine(line);
}

#define DoDrawBG(type, line, num) \
    do \
    { \
        if ((bgCnt[num] & (1<<6)) && (GPU2D.BGMosaicSize[0] > 0)) \
        { \
            DrawBG_##type<true>(line, num); \
        } \
        else \
        { \
            DrawBG_##type<false>(line, num); \
        } \
    } while (false)

#define DoDrawBG_Large(line) \
    do \
    { \
        if ((bgCnt[2] & (1<<6)) && (GPU2D.BGMosaicSize[0] > 0)) \
        { \
            DrawBG_Large<true>(line); \
        } \
        else \
        { \
            DrawBG_Large<false>(line); \
        } \
    } while (false)

template<u32 bgmode>
void SoftRenderer2D::DrawScanlineBGMode(u32 line)
{
    u32 dispCnt = GPU2D.DispCnt;
    u16* bgCnt = GPU2D.BGCnt;
    for (int i = 3; i >= 0; i--)
    {
        if ((bgCnt[3] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<3))
            {
                if (bgmode >= 3)
                    DoDrawBG(Extended, line, 3);
                else if (bgmode >= 1)
                    DoDrawBG(Affine, line, 3);
                else
                    DoDrawBG(Text, line, 3);
            }
        }
        if ((bgCnt[2] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<2))
            {
                if (bgmode == 5)
                    DoDrawBG(Extended, line, 2);
                else if (bgmode == 4 || bgmode == 2)
                    DoDrawBG(Affine, line, 2);
                else
                    DoDrawBG(Text, line, 2);
            }
        }
        if ((bgCnt[1] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<1))
            {
                DoDrawBG(Text, line, 1);
            }
        }
        if ((bgCnt[0] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<0))
            {
                if (!GPU2D.Num && (dispCnt & 0x8))
                    DrawBG_3D();
                else
                    DoDrawBG(Text, line, 0);
            }
        }
        if ((GPU2D.LayerEnable & (1<<4)) && NumSprites)
        {
            InterleaveSprites(i);
        }

    }
}

void SoftRenderer2D::DrawScanlineBGMode6(u32 line)
{
    u32 dispCnt = GPU2D.DispCnt;
    u16* bgCnt = GPU2D.BGCnt;
    for (int i = 3; i >= 0; i--)
    {
        if ((bgCnt[2] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<2))
            {
                DoDrawBG_Large(line);
            }
        }
        if ((bgCnt[0] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<0))
            {
                if ((!GPU2D.Num) && (dispCnt & 0x8))
                    DrawBG_3D();
            }
        }
        if ((GPU2D.LayerEnable & (1<<4)) && NumSprites)
        {
            InterleaveSprites(i);
        }
    }
}

void SoftRenderer2D::DrawScanlineBGMode7(u32 line)
{
    u32 dispCnt = GPU2D.DispCnt;
    u16* bgCnt = GPU2D.BGCnt;
    // mode 7 only has text-mode BG0 and BG1

    for (int i = 3; i >= 0; i--)
    {
        if ((bgCnt[1] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<1))
            {
                DoDrawBG(Text, line, 1);
            }
        }
        if ((bgCnt[0] & 0x3) == i)
        {
            if (GPU2D.LayerEnable & (1<<0))
            {
                if (!GPU2D.Num && (dispCnt & 0x8))
                    DrawBG_3D();
                else
                    DoDrawBG(Text, line, 0);
            }
        }
        if ((GPU2D.LayerEnable & (1<<4)) && NumSprites)
        {
            InterleaveSprites(i);
        }
    }
}

void SoftRenderer2D::DrawScanline_BGOBJ(u32 line, u32* dst)
{
    u64 backdrop;
    if (GPU2D.Num)
        backdrop = *(u16*)&GPU.Palette[0x400];
    else
        backdrop = *(u16*)&GPU.Palette[0];

    {
        u8 r = (backdrop & 0x001F) << 1;
        u8 g = ((backdrop & 0x03E0) >> 4) | ((backdrop & 0x8000) >> 15);
        u8 b = (backdrop & 0x7C00) >> 9;

        backdrop = r | (g << 8) | (b << 16) | 0x20000000;
        backdrop |= (backdrop << 32);

        for (int i = 0; i < 256; i+=2)
            *(u64*)&BGOBJLine[i] = backdrop;
        for (int i = 256; i < 512; i+=2)
            *(u64*)&BGOBJLine[i] = 0;
    }

    if (CaptureLayersActive) DisplayBackdrop = BGOBJLine[0];

    if (GPU2D.DispCnt & 0xE000)
        CalculateWindowMaskIntervals(GPU2D, WindowMask, OBJWindow);
    else
        memset(WindowMask, 0xFF, 256);

    ApplySpriteMosaicX();
    CurBGXMosaicTable = MosaicTable[GPU2D.BGMosaicSize[0]].data();

    switch (GPU2D.DispCnt & 0x7)
    {
        case 0: DrawScanlineBGMode<0>(line); break;
        case 1: DrawScanlineBGMode<1>(line); break;
        case 2: DrawScanlineBGMode<2>(line); break;
        case 3: DrawScanlineBGMode<3>(line); break;
        case 4: DrawScanlineBGMode<4>(line); break;
        case 5: DrawScanlineBGMode<5>(line); break;
        case 6: DrawScanlineBGMode6(line); break;
        case 7: DrawScanlineBGMode7(line); break;
    }

    // Color special effects: specialize the scanline-invariant normal effect.
    switch ((GPU2D.BlendCnt >> 6) & 0x3)
    {
        case 0: ColorComposite<0>(dst); break;
        case 1: ColorComposite<1>(dst); break;
        case 2: ColorComposite<2>(dst); break;
        case 3: ColorComposite<3>(dst); break;
    }
}


void SoftRenderer2D::DrawPixel(u32* dst, u16 color, u32 flag)
{
    u8 r = (color & 0x001F) << 1;
    u8 g = ((color & 0x03E0) >> 4) | ((color & 0x8000) >> 15);
    u8 b = (color & 0x7C00) >> 9;

    *(dst+256) = *dst;
    *dst = r | (g << 8) | (b << 16) | flag;
    if (CaptureLayersActive)
    {
        const u32 layer = flag & 0xF0000000 ? 4 : std::countr_zero(flag >> 24);
        DisplayLayers[layer][dst - BGOBJLine] = *dst;
    }
}

void SoftRenderer2D::DrawBG_3D()
{
    if (Parent.ScaledDisplay)
    {
        // A native transparent sample may cover a different scaled subpixel.
        // Preserve both lower layers and let subsequent layers occlude this
        // placeholder normally. No guest registers or sprite state are replayed.
        memcpy(Below3D, BGOBJLine, sizeof(Below3D));
        Scaled3DActive = true;
        for (int x = 0; x < 256; ++x)
        {
            if (!(WindowMask[x] & 1)) continue;
            BGOBJLine[x + 256] = BGOBJLine[x];
            BGOBJLine[x] = 0x40000000;
            if (CaptureLayersActive) DisplayLayers[0][x] = 0x40000000;
        }
        return;
    }
    for (int i = 0; i < 256; i++)
    {
        u32 c = Parent.Output3D[i];

        if ((c >> 24) == 0) continue;
        if (!(WindowMask[i] & 0x01)) continue;

        BGOBJLine[i+256] = BGOBJLine[i];
        BGOBJLine[i] = c | 0x40000000;
    }
}

template<bool mosaic>
void SoftRenderer2D::DrawBG_Text(u32 line, u32 bgnum)
{
    // workaround for backgrounds missing on aarch64 with lto build
    asm volatile ("" : : : "memory");

    u16 bgcnt = GPU2D.BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal;
    u32 extpal, extpalslot;

    u16 xoff = GPU2D.BGXPos[bgnum];
    u16 yoff = GPU2D.BGYPos[bgnum];

    if (bgcnt & (1<<6))
        yoff += GPU2D.BGMosaicLine;
    else
        yoff += line;
    /*u16 yoff = GPU2D.BGYPos[bgnum] + line;

    if (bgcnt & (1<<6))
    {
        // vertical mosaic
        yoff -= GPU2D.BGMosaicY;
    }*/

    u32 widexmask = (bgcnt & (1<<14)) ? 0x100 : 0;

    extpal = (GPU2D.DispCnt & (1<<30));
    if (extpal) extpalslot = ((bgnum<2) && (bgcnt&0x2000)) ? (2+bgnum) : bgnum;

    u8* bgvram;
    u32 bgvrammask;
    GPU2D.GetBGVRAM(bgvram, bgvrammask);
    if (GPU2D.Num)
    {
        tilesetaddr = ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&GPU.Palette[0x400];
    }
    else
    {
        tilesetaddr = ((GPU2D.DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((GPU2D.DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&GPU.Palette[0];
    }

    // adjust Y position in tilemap
    if (bgcnt & (1<<15))
    {
        tilemapaddr += ((yoff & 0x1F8) << 3);
        if (bgcnt & (1<<14))
            tilemapaddr += ((yoff & 0x100) << 3);
    }
    else
        tilemapaddr += ((yoff & 0xF8) << 3);

    if constexpr (!mosaic)
    {
        const u32 flag = 0x01000000 << bgnum;
        const u32 row = yoff & 7;
        if (bgcnt & (1<<7))
        {
            // Work on the visible part of each tile, including a partial tile
            // at either end of the scanline. Tile-map reads were already once
            // per tile; the span keeps coordinate/flip work out of each pixel.
            for (u32 i = 0; i < 256;)
            {
                const u32 span = std::min(256u - i, 8u - (xoff & 7));
                const u16 tile = *(u16*)&bgvram[(tilemapaddr + ((xoff & 0xF8) >> 2)
                    + ((xoff & widexmask) << 3)) & bgvrammask];
                u16* tilepal = extpal ? GPU2D.GetBGExtPal(extpalslot, tile >> 12) : pal;
                const u32 pixelsaddr = tilesetaddr + ((tile & 0x03FF) << 6)
                    + (((tile & (1<<11)) ? (7 - row) : row) << 3);
                const int step = (tile & (1<<10)) ? -1 : 1;
                int tilex = (tile & (1<<10)) ? 7 - (xoff & 7) : (xoff & 7);
                for (u32 end = i + span; i < end; ++i, tilex += step)
                {
                    if (WindowMask[i] & (1<<bgnum))
                    {
                        const u8 color = bgvram[(pixelsaddr + tilex) & bgvrammask];
                        if (color) DrawPixel(&BGOBJLine[i], tilepal[color], flag);
                    }
                }
                xoff += span;
            }
        }
        else
        {
            for (u32 i = 0; i < 256;)
            {
                const u32 span = std::min(256u - i, 8u - (xoff & 7));
                const u16 tile = *(u16*)&bgvram[(tilemapaddr + ((xoff & 0xF8) >> 2)
                    + ((xoff & widexmask) << 3)) & bgvrammask];
                u16* tilepal = pal + ((tile & 0xF000) >> 8);
                const u32 pixelsaddr = tilesetaddr + ((tile & 0x03FF) << 5)
                    + (((tile & (1<<11)) ? (7 - row) : row) << 2);
                const int step = (tile & (1<<10)) ? -1 : 1;
                int tilex = (tile & (1<<10)) ? 7 - (xoff & 7) : (xoff & 7);
                for (u32 end = i + span; i < end; ++i, tilex += step)
                {
                    if (WindowMask[i] & (1<<bgnum))
                    {
                        const u8 packed = bgvram[(pixelsaddr + (tilex >> 1)) & bgvrammask];
                        const u8 color = (tilex & 1) ? (packed >> 4) : (packed & 0x0F);
                        if (color) DrawPixel(&BGOBJLine[i], tilepal[color], flag);
                    }
                }
                xoff += span;
            }
        }
        return;
    }

    u16 curtile;
    u16* curpal;
    u32 pixelsaddr;
    u8 color;
    u32 lastxpos;

    if (bgcnt & (1<<7))
    {
        // 256-color

        // preload shit as needed
        if ((xoff & 0x7) || mosaic)
        {
            curtile = *(u16*)&bgvram[(tilemapaddr + ((xoff & 0xF8) >> 2) + ((xoff & widexmask) << 3)) & bgvrammask];

            if (extpal) curpal = GPU2D.GetBGExtPal(extpalslot, curtile>>12);
            else        curpal = pal;

            pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 6)
                                     + (((curtile & (1<<11)) ? (7-(yoff&0x7)) : (yoff&0x7)) << 3);
        }

        if (mosaic) lastxpos = xoff;

        for (int i = 0; i < 256; i++)
        {
            u32 xpos;
            if (mosaic) xpos = xoff - CurBGXMosaicTable[i];
            else        xpos = xoff;

            if ((!mosaic && (!(xpos & 0x7))) ||
                (mosaic && ((xpos >> 3) != (lastxpos >> 3))))
            {
                // load a new tile
                curtile = *(u16*)&bgvram[(tilemapaddr + ((xpos & 0xF8) >> 2) + ((xpos & widexmask) << 3)) & bgvrammask];

                if (extpal) curpal = GPU2D.GetBGExtPal(extpalslot, curtile>>12);
                else        curpal = pal;

                pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 6)
                                         + (((curtile & (1<<11)) ? (7-(yoff&0x7)) : (yoff&0x7)) << 3);

                if (mosaic) lastxpos = xpos;
            }

            // draw pixel
            if (WindowMask[i] & (1<<bgnum))
            {
                u32 tilexoff = (curtile & (1<<10)) ? (7-(xpos&0x7)) : (xpos&0x7);
                color = bgvram[(pixelsaddr + tilexoff) & bgvrammask];

                if (color)
                    DrawPixel(&BGOBJLine[i], curpal[color], 0x01000000<<bgnum);
            }

            xoff++;
        }
    }
    else
    {
        // 16-color

        // preload shit as needed
        if ((xoff & 0x7) || mosaic)
        {
            curtile = *(u16*)&bgvram[((tilemapaddr + ((xoff & 0xF8) >> 2) + ((xoff & widexmask) << 3))) & bgvrammask];
            curpal = pal + ((curtile & 0xF000) >> 8);
            pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 5)
                                     + (((curtile & (1<<11)) ? (7-(yoff&0x7)) : (yoff&0x7)) << 2);
        }

        if (mosaic) lastxpos = xoff;

        for (int i = 0; i < 256; i++)
        {
            u32 xpos;
            if (mosaic) xpos = xoff - CurBGXMosaicTable[i];
            else        xpos = xoff;

            if ((!mosaic && (!(xpos & 0x7))) ||
                (mosaic && ((xpos >> 3) != (lastxpos >> 3))))
            {
                // load a new tile
                curtile = *(u16*)&bgvram[(tilemapaddr + ((xpos & 0xF8) >> 2) + ((xpos & widexmask) << 3)) & bgvrammask];
                curpal = pal + ((curtile & 0xF000) >> 8);
                pixelsaddr = tilesetaddr + ((curtile & 0x03FF) << 5)
                                         + (((curtile & (1<<11)) ? (7-(yoff&0x7)) : (yoff&0x7)) << 2);

                if (mosaic) lastxpos = xpos;
            }

            // draw pixel
            if (WindowMask[i] & (1<<bgnum))
            {
                u32 tilexoff = (curtile & (1<<10)) ? (7-(xpos&0x7)) : (xpos&0x7);
                if (tilexoff & 0x1)
                {
                    color = bgvram[(pixelsaddr + (tilexoff >> 1)) & bgvrammask] >> 4;
                }
                else
                {
                    color = bgvram[(pixelsaddr + (tilexoff >> 1)) & bgvrammask] & 0x0F;
                }

                if (color)
                    DrawPixel(&BGOBJLine[i], curpal[color], 0x01000000<<bgnum);
            }

            xoff++;
        }
    }
}

template<bool mosaic>
void SoftRenderer2D::DrawBG_Affine(u32 line, u32 bgnum)
{
    u16 bgcnt = GPU2D.BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal;

    u32 coordmask;
    u32 yshift;
    switch ((bgcnt >> 14) & 0x3)
    {
        case 0: coordmask = 0x07800; yshift = 7; break;
        case 1: coordmask = 0x0F800; yshift = 8; break;
        case 2: coordmask = 0x1F800; yshift = 9; break;
        case 3: coordmask = 0x3F800; yshift = 10; break;
    }

    u32 overflowmask;
    if (bgcnt & (1<<13)) overflowmask = 0;
    else                 overflowmask = ~(coordmask | 0x7FF);

    s16 rotA = GPU2D.BGRotA[bgnum-2];
    s16 rotC = GPU2D.BGRotC[bgnum-2];

    s32 rotX = GPU2D.BGXRefInternal[bgnum-2];
    s32 rotY = GPU2D.BGYRefInternal[bgnum-2];

    u8* bgvram;
    u32 bgvrammask;
    GPU2D.GetBGVRAM(bgvram, bgvrammask);

    if (GPU2D.Num)
    {
        tilesetaddr = ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&GPU.Palette[0x400];
    }
    else
    {
        tilesetaddr = ((GPU2D.DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
        tilemapaddr = ((GPU2D.DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);

        pal = (u16*)&GPU.Palette[0];
    }

    u16 curtile;
    u8 color;

    yshift -= 3;

    for (int i = 0; i < 256; i++)
    {
        if (WindowMask[i] & (1<<bgnum))
        {
            s32 finalX, finalY;
            if (mosaic)
            {
                int im = CurBGXMosaicTable[i];
                finalX = rotX - (im * rotA);
                finalY = rotY - (im * rotC);
            }
            else
            {
                finalX = rotX;
                finalY = rotY;
            }

            if ((!((finalX|finalY) & overflowmask)))
            {
                curtile = bgvram[(tilemapaddr + ((((finalY & coordmask) >> 11) << yshift) + ((finalX & coordmask) >> 11))) & bgvrammask];

                // draw pixel
                u32 tilexoff = (finalX >> 8) & 0x7;
                u32 tileyoff = (finalY >> 8) & 0x7;

                color = bgvram[(tilesetaddr + (curtile << 6) + (tileyoff << 3) + tilexoff) & bgvrammask];

                if (color)
                    DrawPixel(&BGOBJLine[i], pal[color], 0x01000000<<bgnum);
            }
        }

        rotX += rotA;
        rotY += rotC;
    }
}

template<bool mosaic>
void SoftRenderer2D::DrawBG_Extended(u32 line, u32 bgnum)
{
    u16 bgcnt = GPU2D.BGCnt[bgnum];

    u32 tilesetaddr, tilemapaddr;
    u16* pal;
    u32 extpal;

    u8* bgvram;
    u32 bgvrammask;
    GPU2D.GetBGVRAM(bgvram, bgvrammask);

    extpal = (GPU2D.DispCnt & (1<<30));

    s16 rotA = GPU2D.BGRotA[bgnum-2];
    s16 rotC = GPU2D.BGRotC[bgnum-2];

    s32 rotX = GPU2D.BGXRefInternal[bgnum-2];
    s32 rotY = GPU2D.BGYRefInternal[bgnum-2];

    if (bgcnt & (1<<7))
    {
        // bitmap modes

        u32 xmask, ymask;
        u32 yshift;
        switch ((bgcnt >> 14) & 0x3)
        {
            case 0: xmask = 0x07FFF; ymask = 0x07FFF; yshift = 7; break;
            case 1: xmask = 0x0FFFF; ymask = 0x0FFFF; yshift = 8; break;
            case 2: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
            case 3: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
        }

        u32 ofxmask, ofymask;
        if (bgcnt & (1<<13))
        {
            ofxmask = 0;
            ofymask = 0;
        }
        else
        {
            ofxmask = ~xmask;
            ofymask = ~ymask;
        }

        tilemapaddr = ((bgcnt & 0x1F00) << 6);

        if (bgcnt & (1<<2))
        {
            // direct color bitmap
            if (CaptureLayersActive && !(bgcnt & (1u << 6)))
                BitmapLines[bgnum - 2] = {true, bgcnt, rotX, rotY, rotA,
                    GPU2D.BGRotB[bgnum - 2], rotC, GPU2D.BGRotD[bgnum - 2]};

            u16 color;

            for (int i = 0; i < 256; i++)
            {
                if (WindowMask[i] & (1<<bgnum))
                {
                    s32 finalX, finalY;
                    if (mosaic)
                    {
                        int im = CurBGXMosaicTable[i];
                        finalX = rotX - (im * rotA);
                        finalY = rotY - (im * rotC);
                    }
                    else
                    {
                        finalX = rotX;
                        finalY = rotY;
                    }

                    if (!(finalX & ofxmask) && !(finalY & ofymask))
                    {
                        color = *(u16*)&bgvram[(tilemapaddr + (((((finalY & ymask) >> 8) << yshift) + ((finalX & xmask) >> 8)) << 1)) & bgvrammask];

                        if (color & 0x8000)
                            DrawPixel(&BGOBJLine[i], color & 0x7FFF, 0x01000000<<bgnum);
                    }
                }

                rotX += rotA;
                rotY += rotC;
            }
        }
        else
        {
            // 256-color bitmap

            if (GPU2D.Num) pal = (u16*)&GPU.Palette[0x400];
            else           pal = (u16*)&GPU.Palette[0];

            u8 color;

            for (int i = 0; i < 256; i++)
            {
                if (WindowMask[i] & (1<<bgnum))
                {
                    s32 finalX, finalY;
                    if (mosaic)
                    {
                        int im = CurBGXMosaicTable[i];
                        finalX = rotX - (im * rotA);
                        finalY = rotY - (im * rotC);
                    }
                    else
                    {
                        finalX = rotX;
                        finalY = rotY;
                    }

                    if (!(finalX & ofxmask) && !(finalY & ofymask))
                    {
                        color = bgvram[(tilemapaddr + (((finalY & ymask) >> 8) << yshift) + ((finalX & xmask) >> 8)) & bgvrammask];

                        if (color)
                            DrawPixel(&BGOBJLine[i], pal[color], 0x01000000<<bgnum);
                    }
                }

                rotX += rotA;
                rotY += rotC;
            }
        }
    }
    else
    {
        // mixed affine/text mode

        u32 coordmask;
        u32 yshift;
        switch ((bgcnt >> 14) & 0x3)
        {
            case 0: coordmask = 0x07800; yshift = 7; break;
            case 1: coordmask = 0x0F800; yshift = 8; break;
            case 2: coordmask = 0x1F800; yshift = 9; break;
            case 3: coordmask = 0x3F800; yshift = 10; break;
        }

        u32 overflowmask;
        if (bgcnt & (1<<13)) overflowmask = 0;
        else                 overflowmask = ~(coordmask | 0x7FF);

        if (GPU2D.Num)
        {
            tilesetaddr = ((bgcnt & 0x003C) << 12);
            tilemapaddr = ((bgcnt & 0x1F00) << 3);

            pal = (u16*)&GPU.Palette[0x400];
        }
        else
        {
            tilesetaddr = ((GPU2D.DispCnt & 0x07000000) >> 8) + ((bgcnt & 0x003C) << 12);
            tilemapaddr = ((GPU2D.DispCnt & 0x38000000) >> 11) + ((bgcnt & 0x1F00) << 3);

            pal = (u16*)&GPU.Palette[0];
        }

        u16 curtile;
        u16* curpal;
        u8 color;

        yshift -= 3;

        for (int i = 0; i < 256; i++)
        {
            if (WindowMask[i] & (1<<bgnum))
            {
                s32 finalX, finalY;
                if (mosaic)
                {
                    int im = CurBGXMosaicTable[i];
                    finalX = rotX - (im * rotA);
                    finalY = rotY - (im * rotC);
                }
                else
                {
                    finalX = rotX;
                    finalY = rotY;
                }

                if ((!((finalX|finalY) & overflowmask)))
                {
                    curtile = *(u16*)&bgvram[(tilemapaddr + (((((finalY & coordmask) >> 11) << yshift) + ((finalX & coordmask) >> 11)) << 1)) & bgvrammask];

                    if (extpal) curpal = GPU2D.GetBGExtPal(bgnum, curtile>>12);
                    else        curpal = pal;

                    // draw pixel
                    u32 tilexoff = (finalX >> 8) & 0x7;
                    u32 tileyoff = (finalY >> 8) & 0x7;

                    if (curtile & (1<<10)) tilexoff = 7-tilexoff;
                    if (curtile & (1<<11)) tileyoff = 7-tileyoff;

                    color = bgvram[(tilesetaddr + ((curtile & 0x03FF) << 6) + (tileyoff << 3) + tilexoff) & bgvrammask];

                    if (color)
                        DrawPixel(&BGOBJLine[i], curpal[color], 0x01000000<<bgnum);
                }
            }

            rotX += rotA;
            rotY += rotC;
        }
    }
}

template<bool mosaic>
void SoftRenderer2D::DrawBG_Large(u32 line) // BG is always BG2
{
    u16 bgcnt = GPU2D.BGCnt[2];

    u16* pal;

    // large BG sizes:
    // 0: 512x1024
    // 1: 1024x512
    // 2: 512x256
    // 3: 512x512
    u32 xmask, ymask;
    u32 yshift;
    switch ((bgcnt >> 14) & 0x3)
    {
        case 0: xmask = 0x1FFFF; ymask = 0x3FFFF; yshift = 9; break;
        case 1: xmask = 0x3FFFF; ymask = 0x1FFFF; yshift = 10; break;
        case 2: xmask = 0x1FFFF; ymask = 0x0FFFF; yshift = 9; break;
        case 3: xmask = 0x1FFFF; ymask = 0x1FFFF; yshift = 9; break;
    }

    u32 ofxmask, ofymask;
    if (bgcnt & (1<<13))
    {
        ofxmask = 0;
        ofymask = 0;
    }
    else
    {
        ofxmask = ~xmask;
        ofymask = ~ymask;
    }

    s16 rotA = GPU2D.BGRotA[0];
    s16 rotC = GPU2D.BGRotC[0];

    s32 rotX = GPU2D.BGXRefInternal[0];
    s32 rotY = GPU2D.BGYRefInternal[0];

    u8* bgvram;
    u32 bgvrammask;
    GPU2D.GetBGVRAM(bgvram, bgvrammask);

    // 256-color bitmap

    if (GPU2D.Num) pal = (u16*)&GPU.Palette[0x400];
    else           pal = (u16*)&GPU.Palette[0];

    u8 color;

    for (int i = 0; i < 256; i++)
    {
        if (WindowMask[i] & (1<<2))
        {
            s32 finalX, finalY;
            if (mosaic)
            {
                int im = CurBGXMosaicTable[i];
                finalX = rotX - (im * rotA);
                finalY = rotY - (im * rotC);
            }
            else
            {
                finalX = rotX;
                finalY = rotY;
            }

            if (!(finalX & ofxmask) && !(finalY & ofymask))
            {
                color = bgvram[((((finalY & ymask) >> 8) << yshift) + ((finalX & xmask) >> 8)) & bgvrammask];

                if (color)
                    DrawPixel(&BGOBJLine[i], pal[color], 0x01000000<<2);
            }
        }

        rotX += rotA;
        rotY += rotC;
    }
}


void SoftRenderer2D::ApplySpriteMosaicX()
{
    /*
     * apply X mosaic if needed
     * X mosaic for sprites is applied after all sprites are rendered
     *
     * rules:
     * pixels are processed from left to right
     * current pixel value is latched if:
     * - the X mosaic counter is 0
     * - the current pixel doesn't receive sprite mosaic
     * - the current pixel receives sprite mosaic and the previous one didn't, or vice versa
     * - the current BG-relative priority value is lower than the previous one
     */

    u8 mosw = GPU2D.OBJMosaicSize[0];
    const bool capturedMosaic = CaptureOBJMosaic && CaptureOBJScale > 1;
    if (mosw == 0 && !capturedMosaic) return;

    u8 mosx = 0;
    u32 latchcolor;
    int latchx = 0;
    if (capturedMosaic) CaptureOBJActive = false;
    for (int i = 0; i < 256; i++)
    {
        u32 curcolor = OBJLine[i];
        bool latch = false;

        if (mosx == 0)
            latch = true;
        else if (!(curcolor & OBJ_Mosaic))
            latch = true;
        else if (!(latchcolor & OBJ_Mosaic))
            latch = true;
        else if ((curcolor & OBJ_BGPrioMask) < (latchcolor & OBJ_BGPrioMask))
            latch = true;

        if (latch)
        {
            latchcolor = curcolor;
            latchx = i;
        }

        OBJLine[i] = latchcolor;

        if (capturedMosaic)
        {
            // Reuse the NATIVE latch decision for every subpixel. The source
            // block is at or to our left and is not overwritten while latched.
            // Never let a losing captured candidate provide provenance for a
            // native-only winner, even on a mixed mosaic/non-mosaic line.
            const bool captured = CaptureOBJProvenance[latchx];
            CaptureOBJActive |= captured;
            for (u32 sy = 0; sy < CaptureOBJScale; ++sy)
            {
                auto* row = CaptureOBJLine.data() + size_t(sy) * 256 * CaptureOBJScale;
                auto* dst = row + i * CaptureOBJScale;
                if (!captured) std::fill_n(dst, CaptureOBJScale, latchcolor);
                else if (i != latchx)
                    std::copy_n(row + latchx * CaptureOBJScale, CaptureOBJScale, dst);
            }
        }

        if (mosx == mosw)
            mosx = 0;
        else
            mosx++;
    }
}

void SoftRenderer2D::InterleaveSprites(u32 prio)
{
    u32 attrmask = (prio << 16) | OBJ_IsOpaque;
    u16* pal = (u16*)&GPU.Palette[GPU2D.Num ? 0x600 : 0x200];
    u16* extpal = GPU2D.GetOBJExtPal();

    for (u32 i = 0; i < 256; i++)
    {
        if ((OBJLine[i] & OBJ_OpaPrioMask) != attrmask)
            continue;
        if (!(WindowMask[i] & 0x10))
            continue;

        u16 color;
        u32 pixel = OBJLine[i];

        if (pixel & OBJ_DirectColor)
            color = pixel & 0x7FFF;
        else if (pixel & OBJ_StandardPal)
            color = pal[pixel & 0xFF];
        else
            color = extpal[pixel & 0xFFF];

        DrawPixel(&BGOBJLine[i], color, pixel & 0xFF000000);
    }
}

#define DoDrawSprite(type, ...) \
    do \
    { \
        if (iswin) \
        { \
            DrawSprite_##type<true>(__VA_ARGS__); \
        } \
        else \
        { \
            DrawSprite_##type<false>(__VA_ARGS__); \
        } \
    } while (0)

void SoftRenderer2D::DrawSprites(u32 line)
{
    // the OBJ buffers don't get updated at all if the 2D engine is disabled
    if (!GPU2D.Enabled)
        return;

    if (GPU2D.Num == 0)
    {
        auto objDirty = GPU.VRAMDirty_AOBJ.DeriveState(GPU.VRAMMap_AOBJ, GPU);
        GPU.MakeVRAMFlat_AOBJCoherent(objDirty);
    }
    else
    {
        auto objDirty = GPU.VRAMDirty_BOBJ.DeriveState(GPU.VRAMMap_BOBJ, GPU);
        GPU.MakeVRAMFlat_BOBJCoherent(objDirty);
    }

    NumSprites = 0;
    memset(OBJLine, 0, sizeof(OBJLine));
    memset(OBJWindow, 0, sizeof(OBJWindow));
    CaptureOBJScale = 0;
    CaptureOBJActive = false;
    CaptureOBJMosaic = false;
    CaptureOBJProvenance = {};

    if (!GPU2D.OBJEnable)
        return;

    CaptureOBJScale = Parent.ScaledDisplay ? Parent.CaptureObjectScale(GPU2D.Num) : 0;
    if (CaptureOBJScale > 1)
    {
        try { CaptureOBJLine.assign(size_t(256) * CaptureOBJScale * CaptureOBJScale, 0); }
        catch (const std::exception& error)
        {
            CaptureOBJScale = 0;
            Platform::Log(Platform::LogLevel::Warn, "Captured OBJ display unavailable: %s\n", error.what());
        }
    }

    u16* oam = (u16*)&GPU.OAM[GPU2D.Num ? 0x400 : 0];

    const s32 spritewidth[16] =
    {
        8, 16, 8, 8,
        16, 32, 8, 8,
        32, 32, 16, 8,
        64, 64, 32, 8
    };
    const s32 spriteheight[16] =
    {
        8, 8, 16, 8,
        16, 8, 32, 8,
        32, 16, 32, 8,
        64, 32, 64, 8
    };

    for (int sprnum = 0; sprnum < 128; sprnum++)
    {
        u16* attrib = &oam[sprnum*4];

        u16 sprtype = (attrib[0] >> 8) & 0x3;
        if (sprtype == 2) // disabled
            continue;

        bool iswin = (((attrib[0] >> 10) & 0x3) == 2);

        u32 sizeparam = (attrib[0] >> 14) | ((attrib[1] & 0xC000) >> 12);
        s32 width = spritewidth[sizeparam];
        s32 height = spriteheight[sizeparam];
        s32 boundwidth = width;
        s32 boundheight = height;

        if (sprtype == 3) // double-size rotscale sprite
        {
            boundwidth <<= 1;
            boundheight <<= 1;
        }

        // TODO checkme (128-tall sprite overflow thing)
        s32 ypos = attrib[0] & 0xFF;
        if (((line - ypos) & 0xFF) >= boundheight)
            continue;

        s32 xpos = (s32)(attrib[1] << 23) >> 23;
        if (xpos <= -boundwidth)
            continue;

        if ((attrib[0] & (1<<12)) && (!iswin))
        {
            // The display-only blocks follow the native post-OBJ X latch.
            CaptureOBJMosaic = true;
            // adjust Y position for sprite mosaic
            // (sprite mosaic does not apply to OBJ-window sprites)
            // a ypos greater than the sprite height means we underflowed, due to OBJMosaicLine being
            // latched before the sprite's top, so we clamp it to 0
            ypos = (GPU2D.OBJMosaicLine - ypos) & 0xFF;
            if (ypos >= boundheight) ypos = 0;
        }
        else
            ypos = (line - ypos) & 0xFF;

        if (sprtype & 1)
            DoDrawSprite(Rotscale, sprnum, boundwidth, boundheight, width, height, xpos, ypos);
        else
            DoDrawSprite(Normal, sprnum, width, height, xpos, ypos);

        NumSprites++;
    }
}

template<bool window>
void SoftRenderer2D::DrawSpritePixel(int color, u32 pixelattr, s32 xpos, u32 captureAddress,
    const BitmapOBJTransform* transform)
{
    if (window)
    {
        if (color != -1)
            OBJWindow[xpos] = 1;
    }
    else
    {
        const auto merge = [pixelattr](u32& oldpixel, int sample) {
            if (sample == OBJ_Outside) return false;
            const bool oldisopaque = !!(oldpixel & OBJ_IsOpaque);
            const bool newisopaque = (sample != -1);
            const bool priocheck = (pixelattr & OBJ_BGPrioMask) < (oldpixel & OBJ_BGPrioMask);
            if (newisopaque && (!oldisopaque || priocheck))
            {
                oldpixel = sample | pixelattr;
                return true;
            }
            else if (!newisopaque && !oldisopaque)
            {
                oldpixel &= ~(OBJ_Mosaic | OBJ_BGPrioMask);
                oldpixel |= (pixelattr & (OBJ_IsSprite | OBJ_Mosaic | OBJ_BGPrioMask));
                return true;
            }
            return false;
        };
        // Native color and OAM ordering are unchanged. The display line uses
        // the same winner rules independently, including transparent origins.
        const bool nativeWinner = merge(OBJLine[xpos], color);
        // Integer identity uses the existing borrowed-row fast path. General
        // transforms need the source mapper as well as the native address.
        if (transform && color != OBJ_Outside && transform->a == 256 &&
            transform->b == 0 && transform->c == 0 && transform->d == 256 &&
            ((transform->x | transform->y) & 255) == 0)
        {
            captureAddress = (transform->base + (transform->y / 256) * transform->pitch +
                (transform->x / 256) * 2) & transform->mask;
            transform = nullptr;
        }
        if (transform && CaptureOBJScale)
        {
            const auto& t = *transform;
            const s64 scale = CaptureOBJScale, unit = 256 * scale;
            const bool capturedOrigin = color != OBJ_Outside && Parent.CapturedObjectRow(GPU2D.Num,
                (t.base + (t.y / 256) * t.pitch + (t.x / 256) * 2) & t.mask, 0, CaptureOBJScale);
            CaptureOBJActive |= capturedOrigin;
            if (nativeWinner) CaptureOBJProvenance[xpos] = capturedOrigin;
            for (u32 sy = 0; sy < CaptureOBJScale; ++sy)
            {
                // X/Y and A/B/C/D are signed 8.8. Multiplying the native
                // origin by S gives denominator 256*S; BOTH matrix columns
                // transform destination offsets. Bounds precede division, so
                // negative coordinates cannot truncate into source texel zero.
                s64 fx = s64(t.x) * scale + s64(sy) * t.b;
                s64 fy = s64(t.y) * scale + s64(sy) * t.d;
                auto* dst = CaptureOBJLine.data() + (size_t(sy) * 256 + xpos) * CaptureOBJScale;
                for (u32 sx = 0; sx < CaptureOBJScale; ++sx, fx += t.a, fy += t.c)
                {
                    int sample = color;
                    // Pin the exact native candidate, including fractional
                    // affine origins and absent (out-of-bounds) candidates.
                    if (sx || sy)
                    {
                        if (fx < 0 || fy < 0 || fx >= s64(t.width) * scale || fy >= s64(t.height) * scale)
                        {
                            // An unrelated captured OBJ must not trim this
                            // sprite's native-only coverage on partial mappings.
                            if (capturedOrigin) sample = OBJ_Outside;
                        }
                        else
                        {
                            const u32 address = (t.base + (fy / unit) * t.pitch + (fx / unit) * 2) & t.mask;
                            const u32 sourceX = (fx % unit) / 256, sourceY = (fy % unit) / 256;
                            if (const u16* row = Parent.CapturedObjectRow(GPU2D.Num, address, sourceY, CaptureOBJScale))
                            {
                                sample = (row[sourceX] & 0x8000) ? int(row[sourceX]) : -1;
                                CaptureOBJActive = true;
                            }
                            // Missing provenance/rows or incompatible scale:
                            // keep this destination's original native sample.
                        }
                    }
                    merge(dst[sx], sample);
                }
            }
            return;
        }
        for (u32 sy = 0; sy < CaptureOBJScale; ++sy)
        {
            const u16* row = captureAddress == ~0u ? nullptr :
                Parent.CapturedObjectRow(GPU2D.Num, captureAddress, sy, CaptureOBJScale);
            if (row) CaptureOBJActive = true;
            if (nativeWinner && sy == 0) CaptureOBJProvenance[xpos] = row != nullptr;
            auto* dst = CaptureOBJLine.data() + (size_t(sy) * 256 + xpos) * CaptureOBJScale;
            for (u32 sx = 0; sx < CaptureOBJScale; ++sx)
            {
                const int sample = row ? ((row[sx] & 0x8000) ? int(row[sx]) : -1) : color;
                merge(dst[sx], sample);
            }
        }
    }
}

template<bool window>
void SoftRenderer2D::DrawSprite_Rotscale(u32 num, u32 boundwidth, u32 boundheight, u32 width, u32 height, s32 xpos, s32 ypos)
{
    u16* oam = (u16*)&GPU.OAM[GPU2D.Num ? 0x400 : 0];
    u16* attrib = &oam[num * 4];
    u16* rotparams = &oam[(((attrib[1] >> 9) & 0x1F) * 16) + 3];

    u32 pixelattr = ((attrib[2] & 0x0C00) << 6) | OBJ_IsSprite | OBJ_IsOpaque;
    u32 tilenum = attrib[2] & 0x03FF;
    u32 spritemode = window ? 0 : ((attrib[0] >> 10) & 0x3);

    u32 ytilefactor;

    u8* objvram;
    u32 objvrammask;
    GPU2D.GetOBJVRAM(objvram, objvrammask);

    s32 centerX = boundwidth >> 1;
    s32 centerY = boundheight >> 1;

    if ((attrib[0] & (1<<12)) && !window)
    {
        // apply Y mosaic
        pixelattr |= OBJ_Mosaic;
    }

    u32 xoff;
    if (xpos >= 0)
    {
        xoff = 0;
        if ((xpos+boundwidth) > 256)
            boundwidth = 256-xpos;
    }
    else
    {
        xoff = -xpos;
        xpos = 0;
    }

    s16 rotA = (s16)rotparams[0];
    s16 rotB = (s16)rotparams[4];
    s16 rotC = (s16)rotparams[8];
    s16 rotD = (s16)rotparams[12];

    s32 rotX = ((s32(xoff)-centerX) * rotA) + ((ypos-centerY) * rotB) + s32(width << 7);
    s32 rotY = ((s32(xoff)-centerX) * rotC) + ((ypos-centerY) * rotD) + s32(height << 7);

    width <<= 8;
    height <<= 8;

    u16 color = 0; // transparent in all cases

    if (spritemode == 3)
    {
        u32 alpha = attrib[2] >> 12;
        if (!alpha) return;
        alpha++;

        pixelattr |= (0xC0000000 | (alpha << 24));

        u32 pixelsaddr;
        if (GPU2D.DispCnt & 0x40)
        {
            if (GPU2D.DispCnt & 0x20)
            {
                // 'reserved'
                // draws nothing

                return;
            }
            else
            {
                pixelsaddr = tilenum << (7 + ((GPU2D.DispCnt >> 22) & 0x1));
                ytilefactor = ((width >> 8) * 2);
            }
        }
        else
        {
            if (GPU2D.DispCnt & 0x20)
            {
                pixelsaddr = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                ytilefactor = (256 * 2);
            }
            else
            {
                pixelsaddr = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                ytilefactor = (128 * 2);
            }
        }

        BitmapOBJTransform transform{pixelsaddr, ytilefactor, objvrammask,
            width, height, rotX, rotY, rotA, rotB, rotC, rotD};
        for (; xoff < boundwidth;)
        {
            transform.x = rotX; transform.y = rotY;
            if ((u32)rotX < width && (u32)rotY < height)
            {
                color = *(u16*)&objvram[(pixelsaddr + ((rotY >> 8) * ytilefactor) + ((rotX >> 8) << 1)) & objvrammask];

                DrawSpritePixel<window>((color&0x8000) ? color : -1, pixelattr, xpos, ~0u, &transform);
            }
            else if (CaptureOBJScale)
            {
                // A subpixel can enter the source even when the native origin
                // is outside. Do not insert a transparent native OBJ candidate.
                DrawSpritePixel<window>(OBJ_Outside, pixelattr, xpos, ~0u, &transform);
            }

            rotX += rotA;
            rotY += rotC;
            xoff++;
            xpos++;
        }
    }
    else
    {
        u32 pixelsaddr = tilenum;
        if (GPU2D.DispCnt & (1<<4))
        {
            pixelsaddr <<= ((GPU2D.DispCnt >> 20) & 0x3);
            ytilefactor = (width >> 11) << ((attrib[0] & 0x2000) ? 1:0);
        }
        else
        {
            ytilefactor = 0x20;
        }

        if (spritemode == 1) pixelattr |= 0x80000000;
        else                 pixelattr |= 0x10000000;

        ytilefactor <<= 5;
        pixelsaddr <<= 5;

        if (attrib[0] & (1<<13))
        {
            // 256-color

            if (!window)
            {
                if (!(GPU2D.DispCnt & (1<<31)))
                    pixelattr |= OBJ_StandardPal;
                else
                    pixelattr |= ((attrib[2] & 0xF000) >> 4);
            }

            for (; xoff < boundwidth;)
            {
                if ((u32)rotX < width && (u32)rotY < height)
                {
                    color = objvram[(pixelsaddr + ((rotY>>11)*ytilefactor) + ((rotY&0x700)>>5) + ((rotX>>11)*64) + ((rotX&0x700)>>8)) & objvrammask];

                    DrawSpritePixel<window>(color ? color : -1, pixelattr, xpos);
                }

                rotX += rotA;
                rotY += rotC;
                xoff++;
                xpos++;
            }
        }
        else
        {
            // 16-color
            if (!window)
            {
                pixelattr |= OBJ_StandardPal;
                pixelattr |= ((attrib[2] & 0xF000) >> 8);
            }

            for (; xoff < boundwidth;)
            {
                if ((u32)rotX < width && (u32)rotY < height)
                {
                    color = objvram[(pixelsaddr + ((rotY>>11)*ytilefactor) + ((rotY&0x700)>>6) + ((rotX>>11)*32) + ((rotX&0x700)>>9)) & objvrammask];
                    if (rotX & 0x100)
                        color >>= 4;
                    else
                        color &= 0x0F;

                    DrawSpritePixel<window>(color ? color : -1, pixelattr, xpos);
                }

                rotX += rotA;
                rotY += rotC;
                xoff++;
                xpos++;
            }
        }
    }
}

template<bool window>
void SoftRenderer2D::DrawSprite_Normal(u32 num, u32 width, u32 height, s32 xpos, s32 ypos)
{
    u16* oam = (u16*)&GPU.OAM[GPU2D.Num ? 0x400 : 0];
    u16* attrib = &oam[num * 4];

    u32 pixelattr = ((attrib[2] & 0x0C00) << 6) | OBJ_IsSprite | OBJ_IsOpaque;
    u32 tilenum = attrib[2] & 0x03FF;
    u32 spritemode = window ? 0 : ((attrib[0] >> 10) & 0x3);

    u32 wmask = width - 8; // really ((width - 1) & ~0x7)

    if ((attrib[0] & (1<<12)) && !window)
    {
        // apply Y mosaic
        pixelattr |= OBJ_Mosaic;
    }

    u8* objvram;
    u32 objvrammask;
    GPU2D.GetOBJVRAM(objvram, objvrammask);

    // yflip
    if (attrib[1] & (1<<13))
        ypos = height-1 - ypos;

    u32 xoff;
    u32 xend = width;
    if (xpos >= 0)
    {
        xoff = 0;
        if ((xpos+xend) > 256)
            xend = 256-xpos;
    }
    else
    {
        xoff = -xpos;
        xpos = 0;
    }

    u16 color = 0; // transparent in all cases

    if (spritemode == 3)
    {
        // bitmap sprite

        u32 alpha = attrib[2] >> 12;
        if (!alpha) return;
        alpha++;

        pixelattr |= (0xC0000000 | (alpha << 24));

        u32 pixelsaddr = tilenum;
        u32 ytilefactor;
        if (GPU2D.DispCnt & 0x40)
        {
            if (GPU2D.DispCnt & 0x20)
            {
                // 'reserved'
                // draws nothing

                return;
            }
            else
            {
                pixelsaddr <<= (7 + ((GPU2D.DispCnt >> 22) & 0x1));
                ytilefactor = width * 2;
                pixelsaddr += (ypos * ytilefactor);
            }
        }
        else
        {
            if (GPU2D.DispCnt & 0x20)
            {
                pixelsaddr = ((tilenum & 0x01F) << 4) + ((tilenum & 0x3E0) << 7);
                ytilefactor = 256 * 2;
                pixelsaddr += (ypos * ytilefactor);
            }
            else
            {
                pixelsaddr = ((tilenum & 0x00F) << 4) + ((tilenum & 0x3F0) << 7);
                ytilefactor = 128 * 2;
                pixelsaddr += (ypos * ytilefactor);
            }
        }

        const u32 captureBase = pixelsaddr - ypos * ytilefactor;
        s32 pixelstride;

        if (attrib[1] & (1<<12)) // xflip
        {
            pixelsaddr += ((width-1) << 1);
            pixelsaddr -= (xoff << 1);
            pixelstride = -2;
        }
        else
        {
            pixelsaddr += (xoff << 1);
            pixelstride = 2;
        }

        // Reflection is around the native sample origin, not a reversal of
        // one texel's subpixel block: source = (width-1-x) - subx/S. Positive
        // destination offsets can therefore enter the preceding source texel.
        BitmapOBJTransform transform{captureBase, ytilefactor, objvrammask,
            width << 8, height << 8,
            s32((attrib[1] & 0x1000) ? width - 1 - xoff : xoff) * 256, ypos * 256,
            s16(pixelstride * 128), 0, 0, s16((attrib[1] & 0x2000) ? -256 : 256)};
        for (; xoff < xend;)
        {
            color = *(u16*)&objvram[pixelsaddr & objvrammask];
            const u32 captureAddress = pixelsaddr & objvrammask;
            pixelsaddr += pixelstride;

            DrawSpritePixel<window>((color&0x8000) ? color : -1, pixelattr, xpos, captureAddress,
                (attrib[1] & 0x3000) ? &transform : nullptr);
            transform.x += transform.a;

            xoff++;
            xpos++;
        }
    }
    else
    {
        u32 pixelsaddr = tilenum;
        if (GPU2D.DispCnt & (1<<4))
        {
            pixelsaddr <<= ((GPU2D.DispCnt >> 20) & 0x3);
            pixelsaddr += ((ypos >> 3) * (width >> 3)) << ((attrib[0] & 0x2000) ? 1:0);
        }
        else
        {
            pixelsaddr += ((ypos >> 3) * 0x20);
        }

        if (spritemode == 1) pixelattr |= 0x80000000;
        else                 pixelattr |= 0x10000000;

        if (attrib[0] & (1<<13))
        {
            // 256-color
            pixelsaddr <<= 5;
            pixelsaddr += ((ypos & 0x7) << 3);
            s32 pixelstride;

            if (!window)
            {
                if (!(GPU2D.DispCnt & (1<<31)))
                    pixelattr |= OBJ_StandardPal;
                else
                    pixelattr |= ((attrib[2] & 0xF000) >> 4);
            }

            if (attrib[1] & (1<<12)) // xflip
            {
                pixelsaddr += (((width-1) & wmask) << 3);
                pixelsaddr += ((width-1) & 0x7);
                pixelsaddr -= ((xoff & wmask) << 3);
                pixelsaddr -= (xoff & 0x7);
                pixelstride = -1;
            }
            else
            {
                pixelsaddr += ((xoff & wmask) << 3);
                pixelsaddr += (xoff & 0x7);
                pixelstride = 1;
            }

            for (; xoff < xend;)
            {
                color = objvram[pixelsaddr & objvrammask];

                pixelsaddr += pixelstride;

                DrawSpritePixel<window>(color ? color : -1, pixelattr, xpos);

                xoff++;
                xpos++;
                if (!(xoff & 0x7)) pixelsaddr += (56 * pixelstride);
            }
        }
        else
        {
            // 16-color
            pixelsaddr <<= 5;
            pixelsaddr += ((ypos & 0x7) << 2);
            s32 pixelstride;

            if (!window)
            {
                pixelattr |= OBJ_StandardPal;
                pixelattr |= ((attrib[2] & 0xF000) >> 8);
            }

            // TODO: optimize VRAM access!!
            // TODO: do xflip better? the 'two pixels per byte' thing makes it a bit shitty

            if (attrib[1] & (1<<12)) // xflip
            {
                pixelsaddr += (((width-1) & wmask) << 2);
                pixelsaddr += (((width-1) & 0x7) >> 1);
                pixelsaddr -= ((xoff & wmask) << 2);
                pixelsaddr -= ((xoff & 0x7) >> 1);
                pixelstride = -1;
            }
            else
            {
                pixelsaddr += ((xoff & wmask) << 2);
                pixelsaddr += ((xoff & 0x7) >> 1);
                pixelstride = 1;
            }

            for (; xoff < xend;)
            {
                if (attrib[1] & (1<<12))
                {
                    if (xoff & 0x1) { color = objvram[pixelsaddr & objvrammask] & 0x0F; pixelsaddr--; }
                    else              color = objvram[pixelsaddr & objvrammask] >> 4;
                }
                else
                {
                    if (xoff & 0x1) { color = objvram[pixelsaddr & objvrammask] >> 4; pixelsaddr++; }
                    else              color = objvram[pixelsaddr & objvrammask] & 0x0F;
                }

                DrawSpritePixel<window>(color ? color : -1, pixelattr, xpos);

                xoff++;
                xpos++;
                if (!(xoff & 0x7)) pixelsaddr += ((attrib[1] & 0x1000) ? -28 : 28);
            }
        }
    }
}

}
