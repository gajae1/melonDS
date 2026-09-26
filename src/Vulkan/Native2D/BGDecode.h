// SPDX-License-Identifier: GPL-3.0-or-later
// Background source layout shared with BGDecode.glsl.
#pragma once

#include <cstddef>
#include <cstdint>

namespace melonDS::Vulkan::BGDecode
{

enum Type : uint32_t
{
    Type_Text4bpp = 0,     // text, 16-color tiles
    Type_Text8bpp = 1,     // text, 256-color tiles
    Type_Affine = 2,       // affine, 8-bit map entries
    Type_ExtendedTile = 3, // extended affine, 16-bit map entries
    Type_Bitmap8bpp = 4,   // extended 256-color bitmap and large BG (mapBase 0)
    Type_Direct = 5,       // extended direct-color bitmap
};

struct Config
{
    uint32_t width, height; // source size in texels
    uint32_t type;          // Type
    uint32_t paletteBase;   // types 1 and 3 only: StandardPaletteBase or ExtendedPaletteBase(slot)
    uint32_t tileBase;      // tile data byte address (types 0-3)
    uint32_t mapBase;       // map or bitmap byte address
    uint32_t vramMask;      // engine A 0x7FFFF, B 0x1FFFF
    uint32_t reserved;
};

// Palette buffer: 256 standard BG entries (GPU.Palette at 0x000 for A, 0x400 for B),
// then the engine's four 4096-entry extended BG slots (VRAMFlat_xBGExtPal).
constexpr uint32_t StandardPaletteBase = 0;
constexpr uint32_t ExtendedPaletteBase(uint32_t slot) { return 256 + slot * 4096; }
constexpr uint32_t PaletteEntries = 256 + 4 * 4096;

constexpr uint32_t Transparent = 0;
constexpr uint32_t OpaqueBit = 0x80000000u; // opaque: OpaqueBit | B6 << 16 | G6 << 8 | R6

}
