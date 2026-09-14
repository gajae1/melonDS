// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
// CPU/shader storage layouts shared by compute backends.
#pragma once
#include <cstddef>
#include "types.h"

namespace melonDS { struct Polygon; }

namespace melonDS::ComputeData {
inline constexpr int MaxVariants = 256;

struct SpanSetupY
{
    // Attributes
    s32 Z0, Z1, W0, W1;
    s32 ColorR0, ColorG0, ColorB0;
    s32 ColorR1, ColorG1, ColorB1;
    s32 TexcoordU0, TexcoordV0;
    s32 TexcoordU1, TexcoordV1;

    // Interpolator
    s32 I0, I1;
    s32 Linear;
    s32 IRecip;
    s32 W0n, W0d, W1d;

    // Slope
    s32 Increment;

    s32 X0, X1, Y0, Y1;
    s32 XMin, XMax;
    s32 DxInitial;

    s32 XCovIncr;
    u32 IsDummy;
};

struct SpanSetupX
{
    s32 X0, X1;

    s32 EdgeLenL, EdgeLenR, EdgeCovL, EdgeCovR;

    s32 XRecip;

    u32 Flags;

    s32 Z0, Z1, W0, W1;
    s32 ColorR0, ColorG0, ColorB0;
    s32 ColorR1, ColorG1, ColorB1;
    s32 TexcoordU0, TexcoordV0;
    s32 TexcoordU1, TexcoordV1;

    s32 CovLInitial, CovRInitial;
};

struct SetupIndices
{
    u16 PolyIdx, SpanIdxL, SpanIdxR, Y;
};

struct RenderPolygon
{
    u32 FirstXSpan;
    s32 YTop, YBot;

    s32 XMin, XMax;
    s32 XMinY, XMaxY;

    u32 Variant;
    u32 Attr;

    float TextureLayer;
};

struct BinResultHeader
{
    u32 VariantWorkCount[MaxVariants*4];
    u32 SortedWorkOffset[MaxVariants];

    u32 SortWorkWorkCount[4];
};

struct MetaUniform
{
    u32 NumPolygons;
    u32 NumVariants;

    u32 AlphaRef;
    u32 DispCnt;

    u32 ToonTable[4*34];

    u32 ClearColor, ClearDepth, ClearAttr;

    u32 FogOffset, FogShift, FogColor;

    float ClearBitmapOffset[2];
};

static_assert(sizeof(SpanSetupY) == 124);
static_assert(sizeof(SpanSetupX) == 96);
static_assert(sizeof(SetupIndices) == 8);
static_assert(sizeof(RenderPolygon) == 40);
static_assert(sizeof(MetaUniform) == 592);
static_assert(offsetof(MetaUniform, ToonTable) == 16);
static_assert(offsetof(MetaUniform, ClearBitmapOffset) == 584);
static_assert(offsetof(BinResultHeader, SortWorkWorkCount) == 5120);
// Shared CPU edge setup preserves the DS's fixed-point interpolation rules.
void SetupYSpan(RenderPolygon* polygon, SpanSetupY* span, Polygon* source, int from, int to, int side, s32 positions[10][2]);
void SetupYSpanDummy(RenderPolygon* polygon, SpanSetupY* span, Polygon* source, int vertex, int side, s32 positions[10][2]);
}
