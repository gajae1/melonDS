// SPDX-License-Identifier: GPL-3.0-or-later
// CPU register snapshot: packs a GPU2D scanline into the shared Line ABI
// and advances the horizontal window latch at the software draw boundary.
// Pure CPU; no pixels are read and no GPU APIs are used here.

#include "Native2DState.h"
#include "GPU2D.h"

namespace melonDS::Vulkan::Native2D
{
namespace
{

// Tile/map base derivation, identical to SoftRenderer2D::DrawBG_*.
void TileMapBases(const GPU2D& gpu, uint32_t bgcnt, uint32_t& tileBase, uint32_t& mapBase)
{
    tileBase = (bgcnt & 0x003C) << 12;
    mapBase  = (bgcnt & 0x1F00) << 3;
    if (!gpu.Num) // engine A adds the DISPCNT character/screen base
    {
        tileBase += (gpu.DispCnt & 0x07000000) >> 8;
        mapBase  += (gpu.DispCnt & 0x38000000) >> 11;
    }
}

// Text 8bpp: extended-palette slot is bg+2 for BG0/1 with BGCNT bit13, else bg.
uint32_t TextPaletteBase(const GPU2D& gpu, uint32_t bg, uint32_t bgcnt)
{
    if (!(gpu.DispCnt & (1u << 30))) return BGDecode::StandardPaletteBase;
    const uint32_t slot = (bg < 2 && (bgcnt & 0x2000)) ? bg + 2 : bg;
    return BGDecode::ExtendedPaletteBase(slot);
}

constexpr uint32_t TextType(uint32_t bgcnt)
{
    return (bgcnt & (1u << 7)) ? BGDecode::Type_Text8bpp : BGDecode::Type_Text4bpp;
}

constexpr uint32_t ExtType(uint32_t bgcnt)
{
    if (!(bgcnt & (1u << 7))) return BGDecode::Type_ExtendedTile;
    return (bgcnt & (1u << 2)) ? BGDecode::Type_Direct : BGDecode::Type_Bitmap8bpp;
}

// Per-mode existence/type per contract: BG0 is native3D only on engine A with
// DISPCNT bit3 (and exists as nothing else in mode 6); BG1 is text, disabled in
// mode 6; BG2 is affine in modes 2/4, extended in 5, large bitmap in 6, off in
// 7; BG3 is affine in 1/2, extended in 3/4/5, off in 6/7. Text in all other
// modes (0/1/3 for BG2). Engine B never gets 3D.
uint32_t LayerType(const GPU2D& gpu, uint32_t bg, uint32_t bgcnt, uint32_t mode, bool& exists)
{
    exists = true;
    if (bg == 0)
    {
        if (!gpu.Num && (gpu.DispCnt & 0x8)) return Type3D;
        if (mode == 6) exists = false;
        return TextType(bgcnt);
    }
    if (bg == 1)
    {
        if (mode == 6) exists = false;
        return TextType(bgcnt);
    }
    if (bg == 2)
    {
        switch (mode)
        {
        case 2:
        case 4:  return BGDecode::Type_Affine;
        case 5:  return ExtType(bgcnt);
        case 6:  return BGDecode::Type_Bitmap8bpp; // large bitmap
        case 7:  exists = false; return BGDecode::Type_Affine;
        default: return TextType(bgcnt);           // modes 0/1/3
        }
    }
    // bg == 3
    if (mode >= 6) { exists = false; return BGDecode::Type_Affine; }
    if (mode >= 3) return ExtType(bgcnt);
    if (mode >= 1) return BGDecode::Type_Affine;
    return TextType(bgcnt);
}

void FillSize(BGDecode::Config& cfg, uint32_t type, uint32_t bgcnt, bool large)
{
    const uint32_t size = (bgcnt >> 14) & 3;
    switch (type)
    {
    case BGDecode::Type_Text4bpp:
    case BGDecode::Type_Text8bpp:
        cfg.width  = (size & 1) ? 512 : 256;
        cfg.height = (size & 2) ? 512 : 256;
        break;
    case BGDecode::Type_Affine:
    case BGDecode::Type_ExtendedTile:
        cfg.width = cfg.height = 128u << size;
        break;
    case BGDecode::Type_Bitmap8bpp:
    case BGDecode::Type_Direct:
        if (large) // mode 6 large BG: 512x1024 / 1024x512 / 512x256 / 512x512
        {
            static constexpr uint32_t w[4] = {512, 1024, 512, 512};
            static constexpr uint32_t h[4] = {1024, 512, 256, 512};
            cfg.width = w[size]; cfg.height = h[size];
        }
        else // extended bitmap: 128x128 / 256x256 / 512x256 / 512x512
        {
            static constexpr uint32_t w[4] = {128, 256, 512, 512};
            static constexpr uint32_t h[4] = {128, 256, 256, 512};
            cfg.width = w[size]; cfg.height = h[size];
        }
        break;
    default: // Type3D: Config fields unused; resolved3D is a 256x192 line
        cfg.width = 256; cfg.height = 192;
        break;
    }
}

Layer MakeLayer(const GPU2D& gpu, uint32_t bg, uint32_t vcount, uint32_t mode)
{
    const uint32_t bgcnt = gpu.BGCnt[bg];

    Layer l = {};
    bool exists;
    l.source.type = LayerType(gpu, bg, bgcnt, mode, exists);
    const bool is3d = (l.source.type == Type3D);
    const bool affine = !is3d && l.source.type != BGDecode::Type_Text4bpp &&
        l.source.type != BGDecode::Type_Text8bpp;

    FillSize(l.source, l.source.type, bgcnt, mode == 6 && bg == 2);

    if (affine)
    {
        TileMapBases(gpu, bgcnt, l.source.tileBase, l.source.mapBase);
        if (l.source.type == BGDecode::Type_Bitmap8bpp ||
            l.source.type == BGDecode::Type_Direct)
        {
            l.source.tileBase = 0;
            l.source.mapBase = (mode == 6) ? 0 : ((bgcnt & 0x1F00) << 6);
        }
        l.source.paletteBase = (l.source.type == BGDecode::Type_ExtendedTile)
            ? ((gpu.DispCnt & (1u << 30))
                ? BGDecode::ExtendedPaletteBase(bg)
                : BGDecode::StandardPaletteBase)
            : BGDecode::StandardPaletteBase;
    }
    else
    {
        TileMapBases(gpu, bgcnt, l.source.tileBase, l.source.mapBase);
        l.source.paletteBase = (l.source.type == BGDecode::Type_Text8bpp)
            ? TextPaletteBase(gpu, bg, bgcnt)
            : BGDecode::StandardPaletteBase;
    }
    l.source.vramMask = gpu.Num ? 0x1FFFF : 0x7FFFF;

    if (affine)
    {
        // latched internal reference + rotation params, signed 8.8
        l.originX = gpu.BGXRefInternal[bg - 2];
        l.originY = gpu.BGYRefInternal[bg - 2];
        l.stepX   = int32_t(gpu.BGRotA[bg - 2]);
        l.stepY   = int32_t(gpu.BGRotC[bg - 2]);
        l.wrap    = (bgcnt >> 13) & 1;
    }
    else
    {
        // u16 scroll offsets; source Y adds the latched mosaic line when the
        // BG mosaic flag is set, otherwise VCOUNT (u16 wrap, as software).
        const uint32_t yoff =
            (uint32_t(gpu.BGYPos[bg]) +
             ((bgcnt & (1u << 6)) ? gpu.BGMosaicLine : vcount)) & 0xFFFF;
        l.originX = int32_t(uint32_t(gpu.BGXPos[bg]) << 8);
        l.originY = int32_t(yoff << 8);
        l.stepX   = 0x100;
        l.stepY   = 0;
        l.wrap    = 1;
    }

    // horizontal mosaic only if BGCNT bit6, never for the 3D layer
    l.mosaicX  = (!is3d && (bgcnt & (1u << 6))) ? gpu.BGMosaicSize[0] : 0;
    l.priority = bgcnt & 0x3;
    l.active   = (exists && (gpu.LayerEnable & (1u << bg))) ? 1u : 0u;
    return l;
}

}

Line PackLine(const melonDS::GPU2D& gpu, uint32_t vcount, uint32_t numSprites)
{
    const uint32_t mode = gpu.DispCnt & 0x7;

    Line line = {};
    for (uint32_t bg = 0; bg < 4; bg++)
        line.layers[bg] = MakeLayer(gpu, bg, vcount, mode);

    line.engine      = gpu.Num;
    line.enabled     = gpu.Enabled ? 1u : 0u;
    line.forcedBlank = gpu.ForcedBlank ? 1u : 0u;
    line.layerEnable = gpu.LayerEnable;

    line.dispCnt  = gpu.DispCnt;
    line.blendCnt = gpu.BlendCnt;
    line.eva      = gpu.EVA;
    line.evb      = gpu.EVB;
    line.evy      = gpu.EVY;

    line.win0X1        = gpu.Win0Coords[0];
    line.win0X2        = gpu.Win0Coords[1];
    line.win0Active    = gpu.Win0Active;
    line.win1X1        = gpu.Win1Coords[0];
    line.win1X2        = gpu.Win1Coords[1];
    line.win1Active    = gpu.Win1Active;
    line.winCnt0       = gpu.WinCnt[0];
    line.winCnt1       = gpu.WinCnt[1];
    line.winCntOutside = gpu.WinCnt[2];
    line.winCntOBJ     = gpu.WinCnt[3];
    line.objMosaicX    = gpu.OBJMosaicSize[0];

    line.numSprites = numSprites;
    return line;
}

// Post-draw latch: after a 256-px horizontal scan, bit1 of each window-active
// byte collapses to "x1 > x2" (wraparound keeps it set), bit0 is the vertical
// state and is preserved. Runs only when the line was actually drawn and only
// for windows enabled in DISPCNT bit13/14.
void AdvanceWindowState(melonDS::GPU2D& gpu)
{
    if (!gpu.Enabled || gpu.ForcedBlank)
        return;

    if (gpu.DispCnt & (1u << 13))
        gpu.Win0Active = uint8_t((gpu.Win0Active & ~uint8_t(0x2)) |
            (gpu.Win0Coords[0] > gpu.Win0Coords[1] ? 0x2 : 0));
    if (gpu.DispCnt & (1u << 14))
        gpu.Win1Active = uint8_t((gpu.Win1Active & ~uint8_t(0x2)) |
            (gpu.Win1Coords[0] > gpu.Win1Coords[1] ? 0x2 : 0));
}

}
