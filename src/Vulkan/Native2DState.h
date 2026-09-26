// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Native2D/BGDecode.h"
#include <cstddef>
#include <cstdint>
namespace melonDS { class GPU2D; }
namespace melonDS::Vulkan::Native2D {
struct Layer {
    BGDecode::Config source;
    int32_t originX, originY, stepX, stepY; // signed 8.8; source at screen x=0
    uint32_t mosaicX, wrap, priority, active; // mosaicX is register size 0..15
};
struct Line {
    Layer layers[4];
    uint32_t engine, enabled, forcedBlank, layerEnable;
    uint32_t dispCnt, blendCnt, eva, evb;
    uint32_t evy, win0X1, win0X2, win0Active;
    uint32_t win1X1, win1X2, win1Active, winCnt0;
    uint32_t winCnt1, winCntOutside, winCntOBJ, objMosaicX;
    uint32_t numSprites, reserved0, reserved1, reserved2; // reserved0: optional hires BG page table
};
static_assert(sizeof(Layer)==64 && sizeof(Line)==352);
static_assert(offsetof(Line,engine)==256 && offsetof(Line,numSprites)==336);
constexpr uint32_t Type3D = 6;
constexpr uint32_t BGPaletteEntries = BGDecode::PaletteEntries;
constexpr uint32_t OBJPaletteBase = BGPaletteEntries;
constexpr uint32_t OBJExtPaletteBase = OBJPaletteBase + 256;
constexpr uint32_t PaletteEntries = OBJExtPaletteBase + 4096;
constexpr uint32_t WorkgroupSize = 64;
Line PackLine(const melonDS::GPU2D& gpu, uint32_t vcount, uint32_t numSprites);
// CPU window horizontal state still advances at the original draw boundary,
// even if GPU commands are submitted later. Disabled/blank paths do not advance.
void AdvanceWindowState(melonDS::GPU2D& gpu);
}
