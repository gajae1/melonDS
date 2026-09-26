// SPDX-License-Identifier: GPL-3.0-or-later
// Native 2D scanline kernel: one software Output2D word per (line, x).
//
// Reproduces SoftRenderer2D::DrawScanline for one native pixel, from packed
// registers (Native2D::Line in ../Native2DState.h, 352 bytes): disabled
// and forced-blank fills, backdrop, CalculateWindowMaskIntervals, BG coordinates
// and horizontal mosaic, bgDecode lookup, native 3D BG0, preselected-OBJ
// ApplySpriteMosaicX and InterleaveSprites, and CompositePixel. Output is the
// Output2D word before display mode, master brightness and RGB expansion.
//
// The including shader enables GL_GOOGLE_include_directive and, before #include,
// declares readonly storage buffers:
//   uint BGDecodeVRAM[];     engine BG VRAM bytes (see BGDecode.glsl)
//   uint BGDecodePalette[];  packed u16 palette: BG 256 + 4 x 4096 extended BG
//                            (bgDecode layout), then OBJ 256 at 16640 and
//                            extended OBJ 4096 at 16896
// and afterwards defines the three per-pixel providers declared below. They
// supply inputs the kernel does not rasterize itself (yet):
//   scanlineRawOBJ      SoftRenderer2D::OBJLine[x] before ApplySpriteMosaicX
//   scanlineOBJWindow   SoftRenderer2D::OBJWindow[x] (nonzero = inside)
//   scanlineResolved3D  the native SoftRenderer::Output3D[x] of this line
// Only x and the line index are passed, so a provider can read a fixture
// buffer or a resident image with its own shift/abort handling.

#ifndef SCANLINE_GLSL
#define SCANLINE_GLSL

#include "BGDecode.glsl"
#include "ColorOps.glsl"

const uint SCANLINE_TYPE_3D = 6u;               // Layer.source.type of native 3D BG0
const uint SCANLINE_OBJ_PALETTE = 16640u;       // OBJ standard palette entry 0
const uint SCANLINE_OBJ_EXT_PALETTE = 16896u;   // OBJ extended palette entry 0

// SoftRenderer2D OBJLine bits.
const uint SCANLINE_OBJ_STANDARD_PAL = 1u << 12;
const uint SCANLINE_OBJ_DIRECT_COLOR = 1u << 15;
const uint SCANLINE_OBJ_PRIO_MASK = 3u << 16;
const uint SCANLINE_OBJ_OPAQUE = 1u << 18;
const uint SCANLINE_OBJ_MOSAIC = 1u << 20;

// Native2D::Layer, 64 bytes.
struct ScanlineLayer
{
    BGDecodeConfig source;
    int originX, originY, stepX, stepY; // signed 8.8 source position at screen x = 0 and per x
    uint mosaicX;                       // horizontal mosaic register size 0..15 (0 = off)
    uint wrap;                          // nonzero: wrap coordinates, else skip outside texels
    uint priority;                      // BGCNT & 3
    uint isActive;                      // Layer::active ("active" is reserved in GLSL); nonzero: drawn
};

// Native2D::Line, 352 bytes.
struct ScanlineLine
{
    ScanlineLayer layers[4];
    uint engine, enabled, forcedBlank, layerEnable;
    uint dispCnt, blendCnt, eva, evb;
    uint evy, win0X1, win0X2, win0Active;
    uint win1X1, win1X2, win1Active, winCnt0;
    uint winCnt1, winCntOutside, winCntOBJ, objMosaicX;
    uint numSprites, reserved0, reserved1, reserved2; // reserved0: optional hires BG page table
};

uint scanlineRawOBJ(uint lineIndex, uint x);
uint scanlineOBJWindow(uint lineIndex, uint x);
uint scanlineResolved3D(uint lineIndex, uint x);

// OverlayWindowInterval coverage with the incoming Win*Active value:
// bit 0 vertical, bit 1 horizontal latch carried from the previous line.
bool scanlineInWindow(uint x, uint x1, uint x2, uint state)
{
    bool latched = state == 3u;
    bool covered = (state | 2u) == 3u;
    if (x1 < x2)
        return (latched && x < x1) || (covered && x >= x1 && x < x2);
    return (latched && x < x2) || (x1 > x2 && covered && x >= x1);
}

// CalculateWindowMaskIntervals: outside, then OBJ window, WIN1, WIN0 on top.
uint scanlineWindowMask(ScanlineLine s, uint lineIndex, uint x)
{
    if ((s.dispCnt & 0xE000u) == 0u)
        return 0xFFu;
    uint mask = s.winCntOutside;
    if ((s.dispCnt & 0x8000u) != 0u && scanlineOBJWindow(lineIndex, x) != 0u)
        mask = s.winCntOBJ;
    if ((s.dispCnt & 0x4000u) != 0u && scanlineInWindow(x, s.win1X1, s.win1X2, s.win1Active))
        mask = s.winCnt1;
    if ((s.dispCnt & 0x2000u) != 0u && scanlineInWindow(x, s.win0X1, s.win0X2, s.win0Active))
        mask = s.winCnt0;
    return mask;
}

// One BG layer as the BGOBJLine word it would draw, or 0 if it draws nothing here.
uint scanlineLayerColor(ScanlineLayer layer, uint bg, uint lineIndex, uint x, uint window)
{
    if (layer.isActive == 0u || (window & (1u << bg)) == 0u)
        return 0u;
    if (layer.source.type == SCANLINE_TYPE_3D)
    {
        // DrawBG_3D: no mosaic, alpha 0 is transparent.
        uint c = scanlineResolved3D(lineIndex, x);
        return (c >> 24) == 0u ? 0u : c | 0x40000000u;
    }
    // MosaicTable offset; position arithmetic is 32-bit two's complement like rotX += rotA.
    uint sx = x - x % (layer.mosaicX + 1u);
    uint fx = uint(layer.originX) + uint(layer.stepX) * sx;
    uint fy = uint(layer.originY) + uint(layer.stepY) * sx;
    uint w = layer.source.width, h = layer.source.height;
    if (layer.wrap == 0u && ((fx & ~((w << 8) - 1u)) | (fy & ~((h << 8) - 1u))) != 0u)
        return 0u;
    uint c = bgDecode(layer.source, (fx >> 8) & (w - 1u), (fy >> 8) & (h - 1u));
    return c == BG_TRANSPARENT ? 0u : (c & 0x00FFFFFFu) | (0x01000000u << bg);
}

// ApplySpriteMosaicX for one pixel: the latch restarts at every mosaic group
// start, so at most 16 raw samples from the group start decide pixel x.
uint scanlineMosaicOBJ(uint mosaicX, uint lineIndex, uint x)
{
    uint start = x - x % (mosaicX + 1u);
    uint latch = scanlineRawOBJ(lineIndex, start);
    for (uint i = start + 1u; i <= x; ++i)
    {
        uint cur = scanlineRawOBJ(lineIndex, i);
        if ((cur & SCANLINE_OBJ_MOSAIC) == 0u || (latch & SCANLINE_OBJ_MOSAIC) == 0u ||
            (cur & SCANLINE_OBJ_PRIO_MASK) < (latch & SCANLINE_OBJ_PRIO_MASK))
            latch = cur;
    }
    return latch;
}

void scanlinePush(inout uint top, inout uint second, uint color)
{
    second = top;
    top = color;
}

// Decode native 2D sources once. Enhanced 3D subpixels only replace their
// layer color before the same priority/blend operation; OBJ/VRAM work is shared.
struct ScanlinePixelState
{
    uint layerColor[4];
    uint window, objColor, objPriority, backdrop;
};

ScanlinePixelState scanlinePreparePixel(ScanlineLine s, uint lineIndex, uint x)
{
    ScanlinePixelState pixelState;
    for (uint bg = 0u; bg < 4u; ++bg) pixelState.layerColor[bg] = 0u;
    pixelState.window = 0xFFu;
    pixelState.objColor = 0u;
    pixelState.objPriority = 4u;
    pixelState.backdrop = 0u;
    if (s.enabled == 0u || s.forcedBlank != 0u) return pixelState;
    uint window = scanlineWindowMask(s, lineIndex, x);
    pixelState.window = window;

    for (uint bg = 0u; bg < 4u; ++bg)
        pixelState.layerColor[bg] = scanlineLayerColor(s.layers[bg], bg, lineIndex, x, window);

    // InterleaveSprites: the opaque OBJ pixel at its priority, gated by window bit 4.
    if ((s.layerEnable & 0x10u) != 0u && s.numSprites != 0u && (window & 0x10u) != 0u)
    {
        uint pixel = scanlineMosaicOBJ(s.objMosaicX, lineIndex, x);
        if ((pixel & SCANLINE_OBJ_OPAQUE) != 0u)
        {
            uint color;
            if ((pixel & SCANLINE_OBJ_DIRECT_COLOR) != 0u)
                color = pixel & 0x7FFFu;
            else if ((pixel & SCANLINE_OBJ_STANDARD_PAL) != 0u)
                color = bgReadPalette(SCANLINE_OBJ_PALETTE + (pixel & 0xFFu));
            else
                color = bgReadPalette(SCANLINE_OBJ_EXT_PALETTE + (pixel & 0xFFFu));
            pixelState.objColor = colorMakePixel(color, pixel & 0xFF000000u);
            pixelState.objPriority = (pixel & SCANLINE_OBJ_PRIO_MASK) >> 16;
        }
    }

    pixelState.backdrop = colorMakePixel(bgReadPalette(0u), 0x20000000u);
    return pixelState;
}

uint scanlineCompositePixel(ScanlineLine s, ScanlinePixelState pixelState)
{
    if (s.enabled == 0u)
        return s.engine == 0u ? 0xFF000000u : 0xFF3F3F3Fu;
    if (s.forcedBlank != 0u) return 0xFF3F3F3Fu;
    // DrawScanlineBGMode: priorities 3..0, BG3..BG0, then the OBJ of that priority.
    uint top = pixelState.backdrop;
    uint second = 0u;
    for (uint p = 4u; p-- > 0u;)
    {
        for (uint bg = 4u; bg-- > 0u;)
            if (pixelState.layerColor[bg] != 0u && s.layers[bg].priority == p)
                scanlinePush(top, second, pixelState.layerColor[bg]);
        if (pixelState.objPriority == p)
            scanlinePush(top, second, pixelState.objColor);
    }

    return colorComposite(top, second, s.blendCnt, s.eva, s.evb, s.evy, pixelState.window);
}

uint scanlinePixel(ScanlineLine s, uint lineIndex, uint x)
{
    return scanlineCompositePixel(s, scanlinePreparePixel(s, lineIndex, x));
}

#endif
