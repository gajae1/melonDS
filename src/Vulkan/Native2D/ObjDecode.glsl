// SPDX-License-Identifier: GPL-3.0-or-later
// Native OBJ candidate evaluation. Includer supplies objOAM16(index), objVRAM8(addr),
// objVRAM16(addr); the VRAM accessors apply the engine mask after the address sum.
#ifndef NATIVE_OBJ_DECODE_GLSL
#define NATIVE_OBJ_DECODE_GLSL
const uint OBJ_STANDARD_PALETTE = 0x1000u;
const uint OBJ_PRIORITY = 0x30000u;
const uint OBJ_OPAQUE = 0x40000u;
const uint OBJ_PRESENT = 0x80000u;
const uint OBJ_MOSAIC = 0x100000u;
const uint OBJ_WIDTH[16] = uint[16](8,16,8,8,16,32,8,8,32,32,16,8,64,64,32,8);
const uint OBJ_HEIGHT[16] = uint[16](8,8,16,8,16,8,32,8,32,16,32,8,64,32,64,8);
struct ObjDecodeState { uint dispCnt, line, mosaicLine, reserved; };

// False means no native candidate. A true candidate may be transparent (-1),
// which still affects the transparent priority/mosaic latch during OAM merge.
bool objCandidate(ObjDecodeState state, uint number, uint screenX,
    out int sampleValue, out uint attributes, out bool isWindow)
{
    uint a0 = objOAM16(number*4u), a1 = objOAM16(number*4u+1u), a2 = objOAM16(number*4u+2u);
    uint transformType = (a0 >> 8) & 3u;
    sampleValue = -1; attributes = 0u; isWindow = false;
    if (transformType == 2u) return false;
    uint shape = (a0 >> 14) | ((a1 & 0xC000u) >> 12);
    uint width = OBJ_WIDTH[shape], height = OBJ_HEIGHT[shape];
    uint boundWidth = width << (transformType == 3u ? 1u : 0u);
    uint boundHeight = height << (transformType == 3u ? 1u : 0u);
    uint row = (state.line - (a0 & 255u)) & 255u;
    if (row >= boundHeight) return false;
    int xpos = int((a1 & 511u) ^ 256u) - 256;
    int localX = int(screenX) - xpos;
    if (localX < 0 || uint(localX) >= boundWidth) return false;
    isWindow = ((a0 >> 10) & 3u) == 2u;
    if ((a0 & 0x1000u) != 0u && !isWindow) {
        row = (state.mosaicLine - (a0 & 255u)) & 255u;
        if (row >= boundHeight) row = 0u;
    }
    uint sx, sy;
    if ((transformType & 1u) != 0u) {
        uint matrix = (((a1 >> 9) & 31u) * 16u) + 3u;
        int pa = int(objOAM16(matrix) << 16) >> 16;
        int pb = int(objOAM16(matrix+4u) << 16) >> 16;
        int pc = int(objOAM16(matrix+8u) << 16) >> 16;
        int pd = int(objOAM16(matrix+12u) << 16) >> 16;
        int fx = (localX-int(boundWidth/2u))*pa + (int(row)-int(boundHeight/2u))*pb + int(width<<7);
        int fy = (localX-int(boundWidth/2u))*pc + (int(row)-int(boundHeight/2u))*pd + int(height<<7);
        if (uint(fx) >= (width<<8) || uint(fy) >= (height<<8)) return false;
        sx = uint(fx)>>8; sy = uint(fy)>>8;
    } else {
        sx = (a1 & 0x1000u) != 0u ? width-1u-uint(localX) : uint(localX);
        sy = (a1 & 0x2000u) != 0u ? height-1u-row : row;
    }
    attributes = ((a2 & 0xC00u) << 6) | OBJ_PRESENT | OBJ_OPAQUE;
    if ((a0 & 0x1000u) != 0u && !isWindow) attributes |= OBJ_MOSAIC;
    uint mode = isWindow ? 0u : ((a0 >> 10) & 3u);
    uint tile = a2 & 0x3FFu;
    if (mode == 3u) {
        uint alpha = a2 >> 12;
        if (alpha == 0u) return false;
        attributes |= 0xC0000000u | ((alpha+1u)<<24);
        uint base, pitch;
        if ((state.dispCnt & 0x40u) != 0u) {
            if ((state.dispCnt & 0x20u) != 0u) return false;
            base = tile << (7u + ((state.dispCnt >> 22) & 1u)); pitch = width*2u;
        } else if ((state.dispCnt & 0x20u) != 0u) {
            base = ((tile & 31u)<<4) + ((tile & 0x3E0u)<<7); pitch = 512u;
        } else {
            base = ((tile & 15u)<<4) + ((tile & 0x3F0u)<<7); pitch = 256u;
        }
        uint color = objVRAM16(base + sy*pitch + sx*2u);
        sampleValue = (color & 0x8000u) != 0u ? int(color) : -1;
        return true;
    }
    attributes |= mode == 1u ? 0x80000000u : 0x10000000u;
    bool depth8 = (a0 & 0x2000u) != 0u;
    uint tilePitch;
    if ((state.dispCnt & 16u) != 0u) {
        tile <<= ((state.dispCnt >> 20) & 3u);
        tilePitch = (width>>3) << (depth8 ? 1u : 0u);
    } else tilePitch = 32u;
    uint base = (tile + (sy>>3)*tilePitch) << 5;
    uint color;
    if (depth8) {
        if (!isWindow) {
            if ((state.dispCnt & 0x80000000u) == 0u) attributes |= OBJ_STANDARD_PALETTE;
            else attributes |= (a2 & 0xF000u) >> 4;
        }
        color = objVRAM8(base + ((sy & 7u)<<3) + ((sx>>3)<<6) + (sx & 7u));
    } else {
        if (!isWindow) attributes |= OBJ_STANDARD_PALETTE | ((a2 & 0xF000u)>>8);
        color = objVRAM8(base + ((sy & 7u)<<2) + ((sx>>3)<<5) + ((sx & 7u)>>1));
        color = (color >> ((sx & 1u)*4u)) & 15u;
    }
    sampleValue = color != 0u ? int(color) : -1;
    return true;
}

void objMerge(int sampleValue, uint attributes, inout uint oldPixel)
{
    bool opaque = (oldPixel & OBJ_OPAQUE) != 0u;
    if (sampleValue >= 0 && (!opaque || (attributes & OBJ_PRIORITY) < (oldPixel & OBJ_PRIORITY)))
        oldPixel = uint(sampleValue) | attributes;
    else if (sampleValue < 0 && !opaque) {
        oldPixel &= ~(OBJ_MOSAIC | OBJ_PRIORITY);
        oldPixel |= attributes & (OBJ_PRESENT | OBJ_MOSAIC | OBJ_PRIORITY);
    }
}
#endif
