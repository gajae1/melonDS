// SPDX-License-Identifier: GPL-3.0-or-later
// Integer lookup for the six melonDS BG source formats (Vulkan native 2D).
//
// Before #include, the including shader declares readonly storage buffers that expose
//   uint BGDecodeVRAM[];     engine BG VRAM as little-endian bytes, vramMask + 1 bytes long
//   uint BGDecodePalette[];  packed u16 palette entries: 0..255 standard BG palette,
//                            then extended slot s at 256 + s * 4096 (16 banks x 256)
//
// Semantics follow SoftRenderer2D::DrawBG_Text/_Affine/_Extended/_Large for one
// in-bounds source texel. Scroll, affine transform, mosaic, wrap/clamp, windows,
// priority and blending belong to the caller.
//
// Result: 0 = transparent, otherwise 0x80000000 | B6 << 16 | G6 << 8 | R6.

#ifndef BG_DECODE_GLSL
#define BG_DECODE_GLSL

const uint BG_TYPE_TEXT_4BPP     = 0u; // text, 16-color tiles
const uint BG_TYPE_TEXT_8BPP     = 1u; // text, 256-color tiles
const uint BG_TYPE_AFFINE        = 2u; // affine, 8-bit map entries
const uint BG_TYPE_EXTENDED_TILE = 3u; // extended affine, 16-bit map entries
const uint BG_TYPE_BITMAP_8BPP   = 4u; // extended 256-color bitmap and large BG
const uint BG_TYPE_DIRECT        = 5u; // extended direct-color bitmap

const uint BG_TRANSPARENT = 0u;
const uint BG_OPAQUE      = 0x80000000u;

struct BGDecodeConfig
{
    uint width;       // source size in texels
    uint height;
    uint type;        // BG_TYPE_*
    uint paletteBase; // types 1 and 3 only: 0 = standard, 256 + slot * 4096 = extended slot
    uint tileBase;    // tile data byte address (types 0-3)
    uint mapBase;     // map or bitmap byte address
    uint vramMask;    // engine BG VRAM byte mask (A 0x7FFFF, B 0x1FFFF)
    uint reserved;
};

// Memory providers apply the scanline's immutable page tables.
uint bgReadVRAM8(uint address, uint mask);
uint bgReadVRAM16(uint address, uint mask);
uint bgReadPalette(uint entry);

// SoftRenderer2D MakePixel: RGB555 to RGB666, bit 15 supplies the green LSB.
uint bgColor666(uint color)
{
    uint r = (color & 0x001Fu) << 1;
    uint g = ((color & 0x03E0u) >> 4) | ((color & 0x8000u) >> 15);
    uint b = (color & 0x7C00u) >> 9;
    return r | (g << 8) | (b << 16) | BG_OPAQUE;
}

// Color index 0 is transparent in every indexed format.
uint bgIndexedColor(uint entry, uint index)
{
    return index == 0u ? BG_TRANSPARENT : bgColor666(bgReadPalette(entry));
}

// Text screen blocks are 32x32 entries (2 KB); a 512-texel axis adds a second block.
uint bgTextMapAddress(BGDecodeConfig cfg, uint x, uint y)
{
    uint addr = cfg.mapBase + (((x >> 3) & 0x1Fu) << 1) + (((y >> 3) & 0x1Fu) << 6);
    if (cfg.width == 512u)
        addr += ((x >> 8) & 1u) << 11;
    if (cfg.height == 512u)
        addr += ((y >> 8) & 1u) << (cfg.width == 512u ? 12u : 11u);
    return addr;
}

uint bgAffineMapIndex(BGDecodeConfig cfg, uint x, uint y)
{
    return (y >> 3) * (cfg.width >> 3) + (x >> 3);
}

// Row-major texel index 0..63 inside an 8x8 tile after map bits 10 (H) and 11 (V) flip it.
uint bgFlippedTexel(uint tile, uint x, uint y)
{
    uint col = x & 7u;
    uint row = y & 7u;
    if ((tile & 0x400u) != 0u) col = 7u - col;
    if ((tile & 0x800u) != 0u) row = 7u - row;
    return (row << 3) | col;
}

// 256-color map entries select a bank with bits 12-15 only inside an extended slot.
uint bgTile8PaletteEntry(BGDecodeConfig cfg, uint tile, uint index)
{
    return cfg.paletteBase != 0u ? cfg.paletteBase + ((tile >> 12) << 8) + index : index;
}

uint bgDecode(BGDecodeConfig cfg, uint x, uint y)
{
    uint mask = cfg.vramMask;
    switch (cfg.type)
    {
    case BG_TYPE_TEXT_4BPP:
    {
        uint tile = bgReadVRAM16(bgTextMapAddress(cfg, x, y), mask);
        uint texel = bgFlippedTexel(tile, x, y);
        uint pair = bgReadVRAM8(cfg.tileBase + ((tile & 0x3FFu) << 5) + (texel >> 1), mask);
        uint index = (pair >> ((texel & 1u) << 2)) & 0xFu;
        return bgIndexedColor(((tile >> 12) << 4) + index, index);
    }
    case BG_TYPE_TEXT_8BPP:
    {
        uint tile = bgReadVRAM16(bgTextMapAddress(cfg, x, y), mask);
        uint index = bgReadVRAM8(cfg.tileBase + ((tile & 0x3FFu) << 6) + bgFlippedTexel(tile, x, y), mask);
        return bgIndexedColor(bgTile8PaletteEntry(cfg, tile, index), index);
    }
    case BG_TYPE_AFFINE:
    {
        uint tile = bgReadVRAM8(cfg.mapBase + bgAffineMapIndex(cfg, x, y), mask);
        uint index = bgReadVRAM8(cfg.tileBase + (tile << 6) + ((y & 7u) << 3) + (x & 7u), mask);
        return bgIndexedColor(index, index);
    }
    case BG_TYPE_EXTENDED_TILE:
    {
        uint tile = bgReadVRAM16(cfg.mapBase + (bgAffineMapIndex(cfg, x, y) << 1), mask);
        uint index = bgReadVRAM8(cfg.tileBase + ((tile & 0x3FFu) << 6) + bgFlippedTexel(tile, x, y), mask);
        return bgIndexedColor(bgTile8PaletteEntry(cfg, tile, index), index);
    }
    case BG_TYPE_BITMAP_8BPP:
    {
        uint index = bgReadVRAM8(cfg.mapBase + y * cfg.width + x, mask);
        return bgIndexedColor(index, index);
    }
    case BG_TYPE_DIRECT:
    {
        // Bit 15 is alpha here and never reaches the green LSB.
        uint color = bgReadVRAM16(cfg.mapBase + ((y * cfg.width + x) << 1), mask);
        return (color & 0x8000u) != 0u ? bgColor666(color & 0x7FFFu) : BG_TRANSPARENT;
    }
    }
    return BG_TRANSPARENT;
}

#endif
