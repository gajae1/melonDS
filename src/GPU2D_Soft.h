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

#pragma once

#include "GPU2D.h"
#include <array>
#include <vector>

namespace melonDS
{
class SoftRenderer;

class SoftRenderer2D : public Renderer2D
{
public:
    SoftRenderer2D(melonDS::GPU2D& gpu2D, SoftRenderer& parent);
    ~SoftRenderer2D() override;
    bool Init() override { return true; }
    void Reset() override;

    void DrawScanline(u32 line) override;
    void DrawSprites(u32 line) override;
    // Reuse the native layer stack with a different 3D sample per host pixel.
    void ComposeScaledLine(u32* dst, const u32* pixels3D, int scale, int subline = 0) const;
    bool HasScaledLayers() const { return Scaled3DActive || CaptureLayersActive; }
    void VBlank() override {}
    void VBlankEnd() override {};

private:
    SoftRenderer& Parent;

    enum
    {
        OBJ_StandardPal = (1<<12),
        OBJ_DirectColor = (1<<15),
        OBJ_BGPrioMask = (0x3<<16),
        OBJ_IsOpaque = (1<<18),
        OBJ_OpaPrioMask = (OBJ_BGPrioMask | OBJ_IsOpaque),
        OBJ_IsSprite = (1<<19),
        OBJ_Mosaic = (1<<20),
    };

    alignas(8) u32 BGOBJLine[256*2];
    alignas(8) u32 Below3D[256*2] {};
    bool Scaled3DActive = false;
    // Keep each native candidate, not just the top two: a captured subpixel
    // can become transparent and expose a layer discarded by native rendering.
    std::array<std::array<u32, 256>, 5> DisplayLayers{};
    struct BitmapLine
    {
        bool enabled = false;
        u16 control = 0;
        s32 x = 0, y = 0;
        s16 a = 0, b = 0, c = 0, d = 0;
    };
    std::array<BitmapLine, 2> BitmapLines{};
    bool CaptureLayersActive = false;
    u32 CaptureScale = 1, DisplayBackdrop = 0;
    std::vector<u32> CaptureOutput;
    u32 SampleBitmapLayer(u32 layer, u32 x, u32 subx, u32 suby) const;
    void PrepareCapturedLine(u32 line);
    template<u32 effect> void ComposeCapturedLine(u32* dst, u32 subline) const;
    void Resolve3DPixel(int x, u32 color, u32& top, u32& second) const;
    template<u32 effect> void ComposeScaledLine(u32* dst, const u32* pixels3D, int scale) const;

    alignas(8) u8 WindowMask[256];

    alignas(8) u32 OBJLine[256];
    alignas(8) u8 OBJWindow[256];
    // Display-only, current-scanline OBJ winners. Merge every native candidate
    // so a captured transparent origin cannot discard a lower OBJ prematurely.
    std::vector<u32> CaptureOBJLine;
    u32 CaptureOBJScale = 0;
    bool CaptureOBJActive = false;

    u32 NumSprites;

    u8* CurBGXMosaicTable;
    array2d<u8, 16, 256> MosaicTable = []() constexpr
    {
        array2d<u8, 16, 256> table {};
        // initialize mosaic table
        for (int m = 0; m < 16; m++)
        {
            for (int x = 0; x < 256; x++)
            {
                int offset = x % (m+1);
                table[m][x] = offset;
            }
        }

        return table;
    }();

    template<u32 effect> void ColorComposite(u32* dst) const;

    template<u32 bgmode> void DrawScanlineBGMode(u32 line);
    void DrawScanlineBGMode6(u32 line);
    void DrawScanlineBGMode7(u32 line);
    void DrawScanline_BGOBJ(u32 line, u32* dst);

    void DrawPixel(u32* dst, u16 color, u32 flag);

    void DrawBG_3D();
    template<bool mosaic> void DrawBG_Text(u32 line, u32 bgnum);
    template<bool mosaic> void DrawBG_Affine(u32 line, u32 bgnum);
    template<bool mosaic> void DrawBG_Extended(u32 line, u32 bgnum);
    template<bool mosaic> void DrawBG_Large(u32 line);

    void ApplySpriteMosaicX();
    void InterleaveSprites(u32 prio);
    template<bool window> void DrawSpritePixel(int color, u32 pixelattr, s32 xpos, u32 captureAddress = ~0u);
    template<bool window> void DrawSprite_Rotscale(u32 num, u32 boundwidth, u32 boundheight, u32 width, u32 height, s32 xpos, s32 ypos);
    template<bool window> void DrawSprite_Normal(u32 num, u32 width, u32 height, s32 xpos, s32 ypos);
};

}
