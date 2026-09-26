// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Native2DState.h"
#include "Native2DFinal.h"

namespace melonDS::Vulkan::Native2D {
constexpr uint32_t NoHistory = ~uint32_t(0);
struct ObjectState {
    uint32_t dispCnt, line, mosaicLine, enabled;
    uint32_t vramTable, vramMask, oamTable, historyRead;
};
struct Record {
    Line layers;
    ObjectState object;
    uint32_t bgTable, paletteTable, physicalLine, screen;
    uint32_t source3DLine, source3DX, source3DAbort, source3DScale;
    FinalConfig finalDisplay;
    // hiresLCDC: two segment bits plus physical bank in bits2..3.
    // scaledCapture: CaptureCommand index+1 (zero means no composed source A).
    uint32_t objectWrite, rawOutput, hiresLCDC, scaledCapture;
};
static_assert(sizeof(ObjectState) == 32 && sizeof(Record) == 464);
static_assert(offsetof(Record, object) == 352 && offsetof(Record, finalDisplay) == 416);
// Storage buffers:0 immutable paged memory;1 Record[];2 raw Output2D;
// 3 retained OBJ metadata (256post-mosaic words +256window words per state).
// Images:4 native3D RGBA8;5/6 final top/bottom R32_UINT. One WG64 per record.
// Split dispatches with a shader barrier at history dependencies or repeated
// physical output rows. Never overwrite a memory page used by a queued record.
}
