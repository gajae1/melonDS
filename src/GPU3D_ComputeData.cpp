// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "GPU3D_ComputeData.h"
#include "GPU3D.h"
#include <algorithm>
#include <cassert>

namespace melonDS::ComputeData {
static void SetupAttrs(SpanSetupY* span, Polygon* poly, int from, int to)
{
    span->Z0 = poly->FinalZ[from];
    span->W0 = poly->FinalW[from];
    span->Z1 = poly->FinalZ[to];
    span->W1 = poly->FinalW[to];
    span->ColorR0 = poly->Vertices[from]->FinalColor[0];
    span->ColorG0 = poly->Vertices[from]->FinalColor[1];
    span->ColorB0 = poly->Vertices[from]->FinalColor[2];
    span->ColorR1 = poly->Vertices[to]->FinalColor[0];
    span->ColorG1 = poly->Vertices[to]->FinalColor[1];
    span->ColorB1 = poly->Vertices[to]->FinalColor[2];
    span->TexcoordU0 = poly->Vertices[from]->TexCoords[0];
    span->TexcoordV0 = poly->Vertices[from]->TexCoords[1];
    span->TexcoordU1 = poly->Vertices[to]->TexCoords[0];
    span->TexcoordV1 = poly->Vertices[to]->TexCoords[1];
}

void SetupYSpanDummy(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int vertex, int side, s32 positions[10][2])
{
    s32 x0 = positions[vertex][0];
    if (side)
    {
        span->DxInitial = -0x40000;
        x0--;
    }
    else
    {
        span->DxInitial = 0;
    }

    span->X0 = span->X1 = x0;
    span->XMin = x0;
    span->XMax = x0;
    span->Y0 = span->Y1 = positions[vertex][1];

    if (span->XMin < rp->XMin)
    {
        rp->XMin = span->XMin;
        rp->XMinY = span->Y0;
    }
    if (span->XMax > rp->XMax)
    {
        rp->XMax = span->XMax;
        rp->XMaxY = span->Y0;
    }

    span->Increment = 0;

    span->I0 = span->I1 = span->IRecip = 0;
    span->Linear = true;

    span->XCovIncr = 0;

    span->IsDummy = true;

    SetupAttrs(span, poly, vertex, vertex);
}

void SetupYSpan(RenderPolygon* rp, SpanSetupY* span, Polygon* poly, int from, int to, int side, s32 positions[10][2])
{
    span->X0 = positions[from][0];
    span->X1 = positions[to][0];
    span->Y0 = positions[from][1];
    span->Y1 = positions[to][1];

    SetupAttrs(span, poly, from, to);

    s32 minXY, maxXY;
    bool negative = false;
    if (span->X1 > span->X0)
    {
        span->XMin = span->X0;
        span->XMax = span->X1-1;

        minXY = span->Y0;
        maxXY = span->Y1;
    }
    else if (span->X1 < span->X0)
    {
        span->XMin = span->X1;
        span->XMax = span->X0-1;
        negative = true;

        minXY = span->Y1;
        maxXY = span->Y0;
    }
    else
    {
        span->XMin = span->X0;
        if (side) span->XMin--;
        span->XMax = span->XMin;

        // doesn't matter for completely vertical slope
        minXY = span->Y0;
        maxXY = span->Y0;
    }

    if (span->XMin < rp->XMin)
    {
        rp->XMin = span->XMin;
        rp->XMinY = minXY;
    }
    if (span->XMax > rp->XMax)
    {
        rp->XMax = span->XMax;
        rp->XMaxY = maxXY;
    }

    span->IsDummy = false;

    s32 xlen = span->XMax+1 - span->XMin;
    s32 ylen = span->Y1 - span->Y0;

    // slope increment has a 18-bit fractional part
    // note: for some reason, x/y isn't calculated directly,
    // instead, 1/y is calculated and then multiplied by x
    // TODO: this is still not perfect (see for example x=169 y=33)
    if (ylen == 0)
    {
        span->Increment = 0;
    }
    else if (ylen == xlen)
    {
        span->Increment = 0x40000;
    }
    else
    {
        s32 yrecip = (1<<18) / ylen;
        span->Increment = (span->X1-span->X0) * yrecip;
        if (span->Increment < 0) span->Increment = -span->Increment;
    }

    bool xMajor = (span->Increment > 0x40000);

    if (side)
    {
        // right

        if (xMajor)
            span->DxInitial = negative ? (0x20000 + 0x40000) : (span->Increment - 0x20000);
        else if (span->Increment != 0)
            span->DxInitial = negative ? 0x40000 : 0;
        else
            span->DxInitial = -0x40000;
    }
    else
    {
        // left

        if (xMajor)
            span->DxInitial = negative ? ((span->Increment - 0x20000) + 0x40000) : 0x20000;
        else if (span->Increment != 0)
            span->DxInitial = negative ? 0x40000 : 0;
        else
            span->DxInitial = 0;
    }

    if (xMajor)
    {
        if (side)
        {
            span->I0 = span->X0 - 1;
            span->I1 = span->X1 - 1;
        }
        else
        {
            span->I0 = span->X0;
            span->I1 = span->X1;
        }

        // used for calculating AA coverage
        span->XCovIncr = (ylen << 10) / xlen;
    }
    else
    {
        span->I0 = span->Y0;
        span->I1 = span->Y1;
    }

    if (span->I0 != span->I1)
        span->IRecip = (1<<30) / (span->I1 - span->I0);
    else
        span->IRecip = 0;

    span->Linear = (span->W0 == span->W1) && !(span->W0 & 0x7E) && !(span->W1 & 0x7E);

    if ((span->W0 & 0x1) && !(span->W1 & 0x1))
    {
        span->W0n = (span->W0 - 1) >> 1;
        span->W0d = (span->W0 + 1) >> 1;
        span->W1d = span->W1 >> 1;
    }
    else
    {
        span->W0n = span->W0 >> 1;
        span->W0d = span->W0 >> 1;
        span->W1d = span->W1 >> 1;
    }
}

void PreparePolygon(Polygon* polygon, u32 index, RenderPolygon& rp,
    std::span<SpanSetupY> edges, int& numEdges, std::span<SetupIndices> indices, int& numIndices,
    int scale, bool hires)
{
    const u32 nverts = polygon->NumVertices;
    u32 vtop = polygon->VTop, vbot = polygon->VBottom;
    u32 curVL = vtop, curVR = vtop, nextVL, nextVR;
    rp.FirstXSpan = numIndices;
    rp.Attr = polygon->Attr;
    if (polygon->FacingView)
    {
        nextVL = curVL + 1;
        if (nextVL >= nverts) nextVL = 0;
        nextVR = curVR - 1;
        if ((s32)nextVR < 0) nextVR = nverts - 1;
    }
    else
    {
        nextVL = curVL - 1;
        if ((s32)nextVL < 0) nextVL = nverts - 1;
        nextVR = curVR + 1;
        if (nextVR >= nverts) nextVR = 0;
    }

    s32 scaledPositions[10][2];
    s32 ytop = (192 * scale), ybot = 0;
    for (int i = 0; i < polygon->NumVertices; i++)
    {
        if (hires)
        {
            scaledPositions[i][0] = (polygon->Vertices[i]->HiresPosition[0] * scale) >> 4;
            scaledPositions[i][1] = (polygon->Vertices[i]->HiresPosition[1] * scale) >> 4;
        }
        else
        {
            scaledPositions[i][0] = polygon->Vertices[i]->FinalPosition[0] * scale;
            scaledPositions[i][1] = polygon->Vertices[i]->FinalPosition[1] * scale;
        }
        ytop = std::min(scaledPositions[i][1], ytop);
        ybot = std::max(scaledPositions[i][1], ybot);
    }
    rp.YTop = ytop;
    rp.YBot = ybot;
    rp.XMin = (256 * scale);
    rp.XMax = 0;

    if (ybot == ytop)
    {
        vtop = 0; vbot = 0;

        rp.YBot++;

        int j = 1;
        if (scaledPositions[j][0] < scaledPositions[vtop][0]) vtop = j;
        if (scaledPositions[j][0] > scaledPositions[vbot][0]) vbot = j;

        j = nverts - 1;
        if (scaledPositions[j][0] < scaledPositions[vtop][0]) vtop = j;
        if (scaledPositions[j][0] > scaledPositions[vbot][0]) vbot = j;

        assert(numEdges < edges.size());
        u32 curSpanL = numEdges;
        SetupYSpanDummy(&rp, &edges[numEdges++], polygon, vtop, 0, scaledPositions);
        assert(numEdges < edges.size());
        u32 curSpanR = numEdges;
        SetupYSpanDummy(&rp, &edges[numEdges++], polygon, vbot, 1, scaledPositions);

        assert(numIndices < indices.size());
        indices[numIndices].PolyIdx = index;
        indices[numIndices].SpanIdxL = curSpanL;
        indices[numIndices].SpanIdxR = curSpanR;
        indices[numIndices].Y = ytop;
        numIndices++;
    }
    else
    {
        u32 curSpanL = numEdges;
        assert(numEdges < edges.size());
        SetupYSpan(&rp, &edges[numEdges++], polygon, curVL, nextVL, 0, scaledPositions);
        u32 curSpanR = numEdges;
        assert(numEdges < edges.size());
        SetupYSpan(&rp, &edges[numEdges++], polygon, curVR, nextVR, 1, scaledPositions);

        for (u32 y = ytop; y < ybot; y++)
        {
            if (y >= scaledPositions[nextVL][1] && curVL != polygon->VBottom)
            {
                while (y >= scaledPositions[nextVL][1] && curVL != polygon->VBottom)
                {
                    curVL = nextVL;
                    if (polygon->FacingView)
                    {
                        nextVL = curVL + 1;
                        if (nextVL >= nverts)
                            nextVL = 0;
                    }
                    else
                    {
                        nextVL = curVL - 1;
                        if ((s32)nextVL < 0)
                            nextVL = nverts - 1;
                    }
                }


                assert(numEdges < edges.size());
                curSpanL = numEdges;
                SetupYSpan(&rp, &edges[numEdges++], polygon, curVL, nextVL, 0, scaledPositions);
            }
            if (y >= scaledPositions[nextVR][1] && curVR != polygon->VBottom)
            {
                while (y >= scaledPositions[nextVR][1] && curVR != polygon->VBottom)
                {
                    curVR = nextVR;
                    if (polygon->FacingView)
                    {
                        nextVR = curVR - 1;
                        if ((s32)nextVR < 0)
                            nextVR = nverts - 1;
                    }
                    else
                    {
                        nextVR = curVR + 1;
                        if (nextVR >= nverts)
                            nextVR = 0;
                    }
                }

                assert(numEdges < edges.size());
                curSpanR = numEdges;
                SetupYSpan(&rp ,&edges[numEdges++], polygon, curVR, nextVR, 1, scaledPositions);
            }

            assert(numIndices < indices.size());
            indices[numIndices].PolyIdx = index;
            indices[numIndices].SpanIdxL = curSpanL;
            indices[numIndices].SpanIdxR = curSpanR;
            indices[numIndices].Y = y;
            numIndices++;
        }
    }

}

MetaUniform PrepareMeta(const GPU3D& gpu, u32 polygons, u32 variants)
{
    MetaUniform meta{};
    meta.DispCnt = gpu.RenderDispCnt;
    meta.NumPolygons = polygons;
    meta.NumVariants = variants;
    meta.AlphaRef = gpu.RenderAlphaRef;
    {
        u32 r = (gpu.RenderClearAttr1 << 1) & 0x3E; if (r) r++;
        u32 g = (gpu.RenderClearAttr1 >> 4) & 0x3E; if (g) g++;
        u32 b = (gpu.RenderClearAttr1 >> 9) & 0x3E; if (b) b++;
        u32 a = (gpu.RenderClearAttr1 >> 16) & 0x1F;
        meta.ClearColor = r | (g << 8) | (b << 16) | (a << 24);
        meta.ClearDepth = ((gpu.RenderClearAttr2 & 0x7FFF) * 0x200) + 0x1FF;
        meta.ClearAttr = gpu.RenderClearAttr1 & 0x3F008000;

        u8 xoff = (gpu.RenderClearAttr2 >> 16) & 0xFF;
        u8 yoff = (gpu.RenderClearAttr2 >> 24) & 0xFF;
        meta.ClearBitmapOffset[0] = (float)xoff / 256.0;
        meta.ClearBitmapOffset[1] = (float)yoff / 256.0;
    }
    for (u32 i = 0; i < 32; i++)
    {
        u32 color = gpu.RenderToonTable[i];
        u32 r = (color << 1) & 0x3E;
        u32 g = (color >> 4) & 0x3E;
        u32 b = (color >> 9) & 0x3E;
        if (r) r++;
        if (g) g++;
        if (b) b++;

        meta.ToonTable[i*4+0] = r | (g << 8) | (b << 16);
    }
    for (u32 i = 0; i < 34; i++)
    {
        meta.ToonTable[i*4+1] = gpu.RenderFogDensityTable[i];
    }
    for (u32 i = 0; i < 8; i++)
    {
        u32 color = gpu.RenderEdgeTable[i];
        u32 r = (color << 1) & 0x3E;
        u32 g = (color >> 4) & 0x3E;
        u32 b = (color >> 9) & 0x3E;
        if (r) r++;
        if (g) g++;
        if (b) b++;

        meta.ToonTable[i*4+2] = r | (g << 8) | (b << 16);
    }
    meta.FogOffset = gpu.RenderFogOffset;
    meta.FogShift = gpu.RenderFogShift;
    {
        u32 fogR = (gpu.RenderFogColor << 1) & 0x3E; if (fogR) fogR++;
        u32 fogG = (gpu.RenderFogColor >> 4) & 0x3E; if (fogG) fogG++;
        u32 fogB = (gpu.RenderFogColor >> 9) & 0x3E; if (fogB) fogB++;
        u32 fogA = (gpu.RenderFogColor >> 16) & 0x1F;
        meta.FogColor = fogR | (fogG << 8) | (fogB << 16) | (fogA << 24);
    }

    return meta;
}

void DecodeClearBitmap(const u8* textureVRAM, u32* colors, u32* depths, u8 dirty)
{
    if (dirty & (1 << 0))
    {
        const u16* vram = (const u16*)&textureVRAM[0x40000];
        for (int i = 0; i < 256*256; i++)
        {
            u16 color = vram[i];
            u32 r = (color << 1) & 0x3E; if (r) r++;
            u32 g = (color >> 4) & 0x3E; if (g) g++;
            u32 b = (color >> 9) & 0x3E; if (b) b++;
            u32 a = (color & 0x8000) ? 31 : 0;

            colors[i] = r | (g << 8) | (b << 16) | (a << 24);
        }
    }
    if (dirty & (1 << 1))
    {
        const u16* vram = (const u16*)&textureVRAM[0x60000];
        for (int i = 0; i < 256*256; i++)
        {
            u16 val = vram[i];
            u32 depth = ((val & 0x7FFF) * 0x200) + 0x1FF;
            u32 fog = (val & 0x8000) << 9;

            depths[i] = depth | fog;
        }
    }
}

}
