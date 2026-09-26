// SPDX-License-Identifier: GPL-3.0-or-later
// Captured direct-color bitmap OBJ at display scale, reproducing the legacy
// SoftRenderer2D::DrawSpritePixel subpixel rules. The includer provides the
// latched OBJ provenance page (objPageBase), the prefetched OAM/VRAM and the
// shared visible-object mask. Guest raw, history and window stay native.
uint objPageBase;

// First hires word of the captured subpixel block for this OBJ VRAM address,
// or -1 when the address is not owned by a single enhanced A-D capture bank.
int hiresObjWord(uint address) {
    uint block=address>>14;
    uint bank=(data[objPageBase+64u+block/4u]>>((block&3u)*8u))&255u;
    if (bank>=4u) return -1;
    uint segment=(address&0x1FFFFu)>>8;
    if ((data[objPageBase+bank*16u+segment/32u]&(1u<<(segment&31u)))==0u) return -1;
    uint pitch=(pc.displayScale*pc.displayScale+1u)&~1u;
    return int((bank*65536u+((address&0x1FFFFu)>>1))*pitch);
}

int hiresObjTexel(int base,uint sx,uint sy) {
    uint word=uint(base)+sy*pc.displayScale+sx;
    uint texel=(hires[word>>1]>>((word&1u)*16u))&65535u;
    return (texel&0x8000u)!=0u ? int(texel) : -1;
}

// False: this subpixel receives no candidate (legacy OBJ_Outside).
bool hiresObjSample(ObjBitmapSource bitmap,bool outside,bool capturedOrigin,int nativeSample,
    uint sx,uint sy,out int candidate) {
    candidate=nativeSample;
    if (bitmap.kind==1u) {
        int base=hiresObjWord(bitmap.address);
        if (base>=0) candidate=hiresObjTexel(base,sx,sy);
        return true;
    }
    if (bitmap.kind==0u) return !outside;
    // The origin subpixel keeps its exact native candidate.
    if (sx==0u && sy==0u) return !outside;
    int scale=int(pc.displayScale);
    int fx=bitmap.x*scale+int(sy)*bitmap.b+int(sx)*bitmap.a;
    int fy=bitmap.y*scale+int(sy)*bitmap.d+int(sx)*bitmap.c;
    // A captured sprite must not trim native-only coverage of partial mappings.
    if (fx<0 || fy<0 || fx>=int(bitmap.width)*scale || fy>=int(bitmap.height)*scale)
        return !outside && !capturedOrigin;
    uint unit=256u*pc.displayScale;
    uint address=(bitmap.base+(uint(fy)/unit)*bitmap.pitch+(uint(fx)/unit)*2u)&objVRAMMask();
    int base=hiresObjWord(address);
    if (base<0) return !outside;
    candidate=hiresObjTexel(base,(uint(fx)%unit)/256u,(uint(fy)%unit)/256u);
    return true;
}

// Merged OBJ word for one display subpixel of native pixel x, before mosaic.
uint hiresObjPixel(uint x,uint sx,uint sy) {
    ObjDecodeState state=ObjDecodeState(records[recordIndex].object.dispCnt,
        records[recordIndex].object.line,records[recordIndex].object.mosaicLine,0u);
    uint pixel=0u;
    for (uint word=0u;word<4u;++word) {
        uint remaining=visibleObjects[word];
        while (remaining!=0u) {
            uint number=word*32u+uint(findLSB(remaining)); remaining&=remaining-1u;
            int nativeSample,candidate; uint attributes; bool isWindow,outside; ObjBitmapSource bitmap;
            if (!objCandidateSource(state,number,x,true,nativeSample,attributes,isWindow,outside,bitmap) || isWindow)
                continue;
            bool capturedOrigin=bitmap.kind==2u && !outside && hiresObjWord(bitmap.address)>=0;
            if (hiresObjSample(bitmap,outside,capturedOrigin,nativeSample,sx,sy,candidate))
                objMerge(candidate,attributes,pixel);
        }
    }
    return pixel;
}
