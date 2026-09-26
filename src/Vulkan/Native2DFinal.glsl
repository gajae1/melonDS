// SPDX-License-Identifier: GPL-3.0-or-later
// Final display selection, master brightness and 6->8 bit expansion for one native
// scanline pixel, mirroring Native2DFinal.h. Operation for operation from
// src/GPU_Soft.cpp DrawScanline/DrawScanlineA/DrawScanlineB/ApplyMasterBrightness
// and src/PixelConvert.cpp ExpandScalar, including the early white and black
// returns and the modular register arithmetic of ColorBrightnessUp/Down.
//
// Include-only, with GL_GOOGLE_include_directive enabled. The including shader
// declares the immutable source provider before this include:
//     uint nativeFinalReadWord(uint absoluteWord);
// It returns one little-endian word of the single latched 512-byte source row,
// so halfword x of that row is word sourceWordBase+x/2, low halfword for even x.
#ifndef NATIVE_FINAL_GLSL
#define NATIVE_FINAL_GLSL

// Matches melonDS::Vulkan::Native2D::FinalConfig, 32 bytes.
struct NativeFinalConfig {
    uint dispCnt, masterBrightness, screensEnabled, vcount;
    uint sourceWordBase, vramMapped, engine, reserved;
};

// Immutable source provider, defined by the including shader. The prototype
// keeps the include working whether that definition comes before or after it.
uint nativeFinalReadWord(uint absoluteWord);

// PixelConvert expand: 6-bit fields at bits0-5/8-13/16-21 to opaque RGBA8.
// The alpha store also drops a retained Output2D flag byte.
uint nativeFinalExpand(uint value) {
    uint rgb=((value<<18)&0x00FC0000u)|((value<<2)&0x0000FC00u)|((value>>14)&0x000000FCu);
    return rgb|((rgb&0x00C0C0C0u)>>6)|0xFF000000u;
}

// GPU_ColorOp ColorBrightnessUp/Down with the ApplyMasterBrightness bias0/15.
uint nativeFinalBrightness(uint value,uint factor,bool up) {
    uint bias=up?0u:0xFu;
    uint rb=value&0x3F003Fu,g=value&0x003F00u;
    if (up) {
        rb+=(((((0x3F003Fu-rb)*factor)+bias*0x010001u)>>4)&0x3F003Fu);
        g+=(((((0x003F00u-g)*factor)+bias*0x000100u)>>4)&0x003F00u);
    } else {
        rb-=((((rb*factor)+bias*0x010001u)>>4)&0x3F003Fu);
        g-=((((g*factor)+bias*0x000100u)>>4)&0x003F00u);
    }
    return rb|g|0xFF000000u;
}

// DrawScanlineA case2/case3 BGR555 to the native 6-bit word; bit15 is not read.
uint nativeFinalRGB555(uint color) {
    return ((color&0x001Fu)<<1)|(((color&0x03E0u)>>4)<<8)|(((color&0x7C00u)>>9)<<16);
}

// One halfword of the latched 512-byte source row, low halfword of the word first.
uint nativeFinalHalfword(NativeFinalConfig c,uint x) {
    uint word=nativeFinalReadWord(c.sourceWordBase+(x>>1u));
    return (word>>((x&1u)*16u))&0xFFFFu;
}

uint nativeFinalFinish(NativeFinalConfig c,uint value) {
    uint reg=c.masterBrightness&0xFFFFu,brightnessMode=reg>>14;
    if (brightnessMode!=0u) {
        uint factor=min(reg&0x1Fu,16u);
        if (brightnessMode==1u) value=nativeFinalBrightness(value,factor,true);
        else if (brightnessMode==2u) value=nativeFinalBrightness(value,factor,false);
    }
    return nativeFinalExpand(value);
}

// x is0..255; raw2D is the native Output2D word of this pixel, used as-is when
// the engine displays the regular2D output. Returns the native framebuffer word.
uint nativeFinalPixel(NativeFinalConfig c,uint x,uint raw2D) {
    if (c.screensEnabled==0u) return 0xFF000000u;
    bool engineB=c.engine!=0u;
    uint white=engineB?0xFF3F3F3Fu:0x003F3F3Fu;
    // No video signal outside0..191: white before mode or master brightness.
    if (c.vcount>=192u) return nativeFinalExpand(white);
    uint mode=engineB?(c.dispCnt>>16)&1u:(c.dispCnt>>16)&3u;
    if (mode==0u) return nativeFinalExpand(white);
    uint value=raw2D;
    if (mode!=1u) {
        uint bank=(c.dispCnt>>18)&3u;
        value=0u;
        if (mode==3u || (c.vramMapped&(1u<<bank))!=0u)
            value=nativeFinalRGB555(nativeFinalHalfword(c,x));
    }
    return nativeFinalFinish(c,value);
}

#endif
