// SPDX-License-Identifier: GPL-3.0-or-later
// Captured direct-color BGs use display subpixels; guest sampling stays in
// Scanline.glsl. Ownership and mappings belong to the latched scanline page.
bool hiresBGCoordinate(int origin,int a,int b,uint x,uvec2 subpixel,uint dimension,
    bool wrap,out uint position) {
    int scale=int(pc.displayScale), extent=int(dimension)*256;
    int value;
    if (wrap) {
        // Dimensions are powers of two. Reduce before multiplying by scale,
        // preserving wide signed affine arithmetic without shaderInt64.
        uint reduced=(uint(origin)+uint(int(x)*a))&uint(extent-1);
        value=int(reduced)*scale+int(subpixel.x)*a+int(subpixel.y)*b;
        value%=extent*scale;
        if (value<0) value+=extent*scale;
    } else {
        int offset=int(x)*a, padding=abs(a)+abs(b);
        if (origin < -offset-padding || origin >= extent-offset+padding) return false;
        value=(origin+offset)*scale+int(subpixel.x)*a+int(subpixel.y)*b;
        if (value<0 || value>=extent*scale) return false;
    }
    position=uint(value);
    return true;
}

uint hiresBGColor(ScanlineLayer layer,uint page,uint bg,uint x,uvec2 subpixel,uint nativeColor) {
    uint matrix=data[page+72u+bg-2u];
    int b=int(matrix<<16)>>16,d=int(matrix)>>16;
    uint fx,fy;
    if (!hiresBGCoordinate(layer.originX,layer.stepX,b,x,subpixel,layer.source.width,layer.wrap!=0u,fx) ||
        !hiresBGCoordinate(layer.originY,layer.stepY,d,x,subpixel,layer.source.height,layer.wrap!=0u,fy))
        return 0u;
    uint unit=256u*pc.displayScale;
    uint address=(layer.source.mapBase+((fy/unit)*layer.source.width+fx/unit)*2u)&layer.source.vramMask;
    uint mapping=address>>14;
    uint bank=(data[page+64u+mapping/4u]>>((mapping&3u)*8u))&255u;
    if (bank>=4u) return nativeColor;
    uint segment=(address&0x1FFFFu)>>8;
    if ((data[page+bank*16u+segment/32u]&(1u<<(segment&31u)))==0u) return nativeColor;
    uint pitch=(pc.displayScale*pc.displayScale+1u)&~1u;
    uint word=(bank*65536u+((address&0x1FFFFu)>>1))*pitch+
        ((fy%unit)/256u)*pc.displayScale+(fx%unit)/256u;
    uint color=(hires[word>>1]>>((word&1u)*16u))&65535u;
    if ((color&0x8000u)==0u) return 0u;
    return ((color&31u)<<1)|((color&0x3E0u)<<4)|((color&0x7C00u)<<7)|(0x01000000u<<bg);
}
