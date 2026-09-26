// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
namespace melonDS::Vulkan::Native2D {
// Final display selection, master brightness and 6->8 bit expansion for one
// native scanline pixel: SoftRenderer::DrawScanline (src/GPU_Soft.cpp 102-165),
// DrawScanlineA/DrawScanlineB (173-255), ApplyMasterBrightness (410-431) and
// PixelConvert::ExpandScalar (src/PixelConvert.cpp 29-36) reproduced operation
// for operation. The shader side of this contract is Native2DFinal.glsl.
//
// Field contract for the host that packs the record:
//   dispCnt          raw DISPCNT. Engine A selects its display mode with bits16-17
//                    and the LCDC VRAM bank with bits18-19; engine B uses bit16 only.
//   masterBrightness MASTER_BRIGHT zero-extended from its u16 register. Modes1/2 keep
//                    the unsigned wraparound of ColorBrightnessUp/Down with factors
//                    taken from bits0-4 and clamped16.
//   screensEnabled   GPU.ScreensEnabled. Zero means opaque black for every row, mode
//                    and master brightness value.
//   vcount           source scanline, GPU.VCount at the original draw. Rows outside
//                    0..191 carry no signal and are white before any mode or
//                    brightness handling; the record also uses this as its draw gate.
//                    It does not address the source row, which the host latches first.
//   sourceWordBase   word index of the one latched 512-byte source row these VRAM/FIFO
//                    modes read (the mapped VRAM bank row, or the FIFO latch): pixel x
//                    is halfword x of it, word sourceWordBase+x/2, low halfword for
//                    even x, words little-endian like the other native kernels. Bit15
//                    of a halfword is not an attribute here, unlike a BG/OBJ palette
//                    entry.
//   vramMapped       VRAMMap_LCDC. An unmapped LCDC VRAM display is a zero source
//                    word, which master brightness still processes.
//   engine           GPU2D number:0engine A,1engine B.
//   reserved         unused, keep0.
// The stream is read only through the including shader's provider
// uint nativeFinalReadWord(uint absoluteWord); this header needs no GPU or Vulkan
// dependency and describes the 32-byte layout alone.
struct FinalConfig {
    uint32_t dispCnt, masterBrightness, screensEnabled, vcount;
    uint32_t sourceWordBase, vramMapped, engine, reserved;
};
static_assert(sizeof(FinalConfig)==32 && offsetof(FinalConfig,sourceWordBase)==16);
static_assert(offsetof(FinalConfig,engine)==24 && offsetof(FinalConfig,reserved)==28);
constexpr uint32_t ScanlinePixels = 256;
constexpr uint32_t FinalRowBytes = 512;
constexpr uint32_t FinalRowWords = FinalRowBytes / 4;
}
