// SPDX-License-Identifier: GPL-3.0-or-later
// Integer 2D color operations on software Output2D words (flag byte << 24 | B6 << 16 | G6 << 8 | R6).
//
// Copied operation-for-operation from the released src/Vulkan/DisplayCompose.comp
// blend4/blend5/brightness/composite, which reproduce GPU_ColorOp.h and
// SoftRenderer2D CompositePixel, including the unsigned wraparound for
// out-of-range EVA/EVB and flag-derived weights. Private copy: the released shader
// is not modified; a later integration can make both use this file.

#ifndef COLOR_OPS_GLSL
#define COLOR_OPS_GLSL

// SoftRenderer2D MakePixel: RGB555 plus bit 15 as green LSB, with a flag byte.
uint colorMakePixel(uint color, uint flag)
{
    uint r = (color & 0x001Fu) << 1;
    uint g = ((color & 0x03E0u) >> 4) | ((color & 0x8000u) >> 15);
    uint b = (color & 0x7C00u) >> 9;
    return r | (g << 8) | (b << 16) | flag;
}

uint colorBlend4(uint a, uint b, uint eva, uint evb)
{
    uint r = (((a & 0x3fu) * eva) + ((b & 0x3fu) * evb) + 8u) >> 4;
    uint g = ((((a & 0x3f00u) * eva) + ((b & 0x3f00u) * evb) + 0x800u) >> 4) & 0x7f00u;
    uint bl = ((((a & 0x3f0000u) * eva) + ((b & 0x3f0000u) * evb) + 0x80000u) >> 4) & 0x7f0000u;
    return min(r, 0x3fu) | min(g, 0x3f00u) | min(bl, 0x3f0000u) | 0xff000000u;
}

uint colorBlend5(uint a, uint b)
{
    uint eva = ((a >> 24) & 31u) + 1u;
    if (eva == 32u) return a;
    uint evb = 32u - eva;
    uint r = (((a & 0x3fu) * eva) + ((b & 0x3fu) * evb) + 16u) >> 5;
    uint g = ((((a & 0x3f00u) * eva) + ((b & 0x3f00u) * evb) + 0x1000u) >> 5) & 0x7f00u;
    uint bl = ((((a & 0x3f0000u) * eva) + ((b & 0x3f0000u) * evb) + 0x100000u) >> 5) & 0x7f0000u;
    return min(r, 0x3fu) | min(g, 0x3f00u) | min(bl, 0x3f0000u) | 0xff000000u;
}

// up: ColorBrightnessUp(value, factor, bias); otherwise ColorBrightnessDown.
uint colorBrightness(uint value, uint factor, uint bias, bool up)
{
    uint rb = value & 0x3f003fu, g = value & 0x3f00u;
    if (up) {
        rb += (((((0x3f003fu - rb) * factor) + bias * 0x10001u) >> 4) & 0x3f003fu);
        g += (((((0x3f00u - g) * factor) + bias * 0x100u) >> 4) & 0x3f00u);
    } else {
        rb -= ((((rb * factor) + bias * 0x10001u) >> 4) & 0x3f003fu);
        g -= ((((g * factor) + bias * 0x100u) >> 4) & 0x3f00u);
    }
    return rb | g | 0xff000000u;
}

// CompositePixel<(control >> 6) & 3>(top, second, BLDCNT, EVA, EVB, EVY, window mask).
uint colorComposite(uint a, uint b, uint control, uint eva, uint evb, uint evy, uint window)
{
    uint effect = (control >> 6) & 3u, flag = a >> 24;
    if (effect == 1u && (flag & 0xc0u) == 0u &&
        ((control & flag) == 0u || (window & 0x20u) == 0u)) return a;
    if (effect == 1u || (flag & 0xc0u) != 0u) {
        uint flag2 = b >> 24;
        uint target2 = (flag2 & 0x80u) != 0u ? 0x1000u :
            (flag2 & 0x40u) != 0u ? 0x100u : flag2 << 8;
        if ((control & target2) != 0u) {
            if ((flag & 0xc0u) == 0x40u) return colorBlend5(a, b);
            if ((flag & 0xc0u) == 0xc0u) return colorBlend4(a, b, flag & 31u, 16u - (flag & 31u));
            return colorBlend4(a, b, eva, evb);
        }
    }
    if (effect >= 2u) {
        if ((flag & 0x80u) != 0u) flag = 0x10u;
        else if ((flag & 0x40u) != 0u) flag = 1u;
        if ((control & flag) != 0u && (window & 0x20u) != 0u)
            return colorBrightness(a, evy, effect == 2u ? 8u : 7u, effect == 2u);
    }
    return a;
}

#endif
