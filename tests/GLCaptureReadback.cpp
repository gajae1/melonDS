// SPDX-License-Identifier: GPL-3.0-or-later
// Real 3D/source-B -> display capture -> guest LDRH, across renderer resize.
// No fixture barrier/readback is inserted before the production consumer.
#include "NDS.h"
#include "GPU_OpenGL.h"
#include "GPU_Soft.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

namespace
{
using namespace melonDS;
constexpr u32 BankB = 0x06820000;
constexpr u32 Code = 0x02010000, Addresses = 0x02011000, Results = 0x02012000;
constexpr u16 Red = 0x801F, Green = 0x83E0, Blue = 0xC000;
// Full weights add disjoint channels exactly. Endpoint primary colors avoid
// unrelated intermediate-intensity quantization in GL capture-as-texture.
constexpr u16 Purple = 0xC01F, Cyan = 0xC3E0, Poison = 0xDEAD;
struct CaptureCase { int Size, Offset, Source; bool Reuse; };
constexpr CaptureCase Cases[] = {
    {0, 0, 0, true},  // source A 3D, 128x128
    {3, 0, 0, true},  // source A 3D, 256x192
    {3, 3, 0, false}, // source A, destination wraps at end of bank
    {1, 2, 1, false}, // source B, 256x64
    {2, 3, 2, false}, // A+B, 256x128, wrapped destination
};
struct Probe { int X, Y; bool Inside; };
constexpr Probe Probes[] = {{32,24,true}, {64,30,true}, {64,40,true},
                             {5,5,false}, {120,60,false}};

bool Compile(Renderer& renderer)
{
    while (renderer.NeedsShaderCompile())
    {
        int step, count;
        if (!renderer.ShaderCompileStep(step, count)) return false;
    }
    return true;
}

struct Scene
{
    Vertex Vertices[3]{};
    melonDS::Polygon Polygon{};
    Scene()
    {
        constexpr int positions[][2] = {{16,16}, {112,16}, {64,56}};
        Polygon.NumVertices = 3;
        Polygon.Attr = (31 << 16) | (3 << 6);
        Polygon.FacingView = true;
        Polygon.VTop = 0;
        Polygon.VBottom = 2;
        Polygon.YTop = 16;
        Polygon.YBottom = 56;
        for (int i = 0; i < 3; ++i)
        {
            Polygon.Vertices[i] = &Vertices[i];
            Polygon.FinalZ[i] = Polygon.FinalW[i] = 0x1000;
            Vertices[i].FinalColor[0] = 63 << 3;
            for (int axis = 0; axis < 2; ++axis)
            {
                Vertices[i].FinalPosition[axis] = positions[i][axis];
                Vertices[i].HiresPosition[axis] = positions[i][axis] << 4;
            }
        }
    }
    void Submit(NDS& nds)
    {
        auto& gpu = nds.GPU.GPU3D;
        gpu.RenderNumPolygons = 1;
        gpu.RenderPolygonRAM[0] = &Polygon;
        gpu.RenderFrameIdentical = false;
        nds.GetRenderer().Start3DRendering();
    }
};

u16 Expected(const CaptureCase& test, bool inside)
{
    if (test.Source == 1) return Blue;
    if (test.Source == 2) return inside ? Purple : Cyan;
    return inside ? Red : Green;
}

// Actual CPU instructions load each capture probe and store it to guest RAM.
// The production bus triggers capture synchronization, not a direct GL read.
void ReadGuest(NDS& nds, const std::array<u32, 5>& addresses)
{
    constexpr u32 program[] = {0xE4931004, 0xE1D100B0, 0xE4820004,
                               0xE2544001, 0x1AFFFFFA, 0xEAFFFFFE};
    for (unsigned i = 0; i < std::size(program); ++i)
        nds.ARM9Write32(Code + i * 4, program[i]);
    for (unsigned i = 0; i < addresses.size(); ++i)
    {
        nds.ARM9Write32(Addresses + i * 4, addresses[i]);
        nds.ARM9Write32(Results + i * 4, Poison);
    }
    nds.ARM9.R[2] = Results;
    nds.ARM9.R[3] = Addresses;
    nds.ARM9.R[4] = addresses.size();
    nds.ARM9.JumpTo(Code);
    nds.RunFrame();
}

bool RunCase(const char* backend, const CaptureCase& test, int scale, int transition)
{
    // transition 0: unchanged control, 1: pending resize, 2: CPU-synced resize.
    const bool software = std::strcmp(backend, "software") == 0;
    NDSArgs args;
    args.JIT = std::nullopt;
    Scene scene; // vertices remain alive through renderer teardown
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    if (!software)
    {
        nds->SetRenderer(std::make_unique<GLRenderer>(*nds, std::strcmp(backend, "compute") == 0));
        if (!dynamic_cast<GLRenderer*>(&nds->GetRenderer())) return false;
    }
    RendererSettings settings{scale, false, false, false};
    nds->GetRenderer().SetRenderSettings(settings);
    if (!Compile(nds->GetRenderer())) return false;
    nds->ARM9Write32(0x02000000, 0xEAFFFFFE);
    nds->ARM9Write32(0x02000200, 0xEAFFFFFE);
    nds->ARM9.JumpTo(0x02000000);
    nds->ARM7.JumpTo(0x02000200);
    nds->ARM9Write16(0x04000304, 0x020F);
    nds->ARM9Write32(0x04000350, (31u << 16) | (Green & 0x7FFF));
    nds->ARM9Write32(0x04000354, 0x7FFF);
    nds->ARM9Write8(0x04000240, 0x80);
    nds->ARM9Write8(0x04000241, 0x80);
    nds->ARM9Write8(0x04000243, 0x80);
    nds->ARM9Write32(0x04000000, 0x00010000);
    for (unsigned i = 0; i < 0x10000; ++i)
    {
        nds->ARM9Write16(0x06800000 + i * 2, Blue);
        nds->ARM9Write16(BankB + i * 2, Poison);
    }
    nds->Start();
    nds->RunFrame();
    scene.Submit(*nds);
    const int width = test.Size ? 256 : 128;
    const int height = test.Size ? test.Size * 64 : 128;
    const u32 capture = 0x81010000 | (test.Offset << 18) | (test.Size << 20) |
                        (test.Source << 29) | (test.Source == 2 ? 0x1010 : 0);
    nds->ARM9Write32(0x04000064, capture);
    nds->RunFrame();
    const int captureBlock = nds->GPU.GetCaptureBlock_LCDC(0x20000 + test.Offset * 0x8000);
    bool passed = nds->IsRunning() && !(nds->GPU.CaptureCnt & (1u << 31)) && captureBlock >= 0;
    std::array<u32, 5> addresses;
    for (unsigned i = 0; i < addresses.size(); ++i)
        addresses[i] = BankB + ((test.Offset * 0x8000 + (Probes[i].Y * width + Probes[i].X) * 2) & 0x1FFFF);
    if (transition == 2)
        passed &= nds->ARM9Read16(addresses[0]) == Expected(test, true);
    if (transition)
    {
        settings.ScaleFactor = 3 - scale;
        nds->GetRenderer().SetRenderSettings(settings);
        if (!Compile(nds->GetRenderer())) return false;
    }
    ReadGuest(*nds, addresses);
    unsigned errors = 0;
    for (unsigned i = 0; i < addresses.size(); ++i)
    {
        const auto actual = nds->ARM9Read32(Results + i * 4);
        const auto expected = Expected(test, Probes[i].Inside);
        if (actual != expected)
        {
            if (!errors) std::fprintf(stderr, "capture first CPU pixel %04x expected %04x\n", actual, expected);
            ++errors;
        }
    }

    // Sample the old capture as a real direct-color 3D texture, then capture
    // that 3D output into another bank and read it through the CPU again.
    // This catches stale GPU references even when prior CPU reads preserved RAM.
    unsigned reuseErrors = 0;
    if (test.Reuse)
    {
        nds->ARM9.JumpTo(0x02000000);
        nds->ARM9Write8(0x04000241, 0x83);
        const u32 size = width == 128 ? 4 : 5;
        scene.Polygon.TexParam = (7u << 26) | (size << 20) | (size << 23);
        for (auto& vertex : scene.Vertices)
        {
            for (int c = 0; c < 3; ++c) vertex.FinalColor[c] = 63 << 3;
            vertex.TexCoords[0] = 32 * 16;
            vertex.TexCoords[1] = 24 * 16;
        }
        nds->ARM9Write16(0x04000060, 1);
        nds->GPU.GPU3D.RenderDispCnt = 1;
        scene.Submit(*nds);
        nds->ARM9Write32(0x04000064, 0x81030000); // source A, bank D,128x128
        nds->RunFrame();
        addresses.fill(0x06860000 + (24 * 128 + 32) * 2);
        ReadGuest(*nds, addresses);
        for (unsigned i = 0; i < addresses.size(); ++i)
        {
            const u32 value = nds->ARM9Read32(Results + i * 4);
            if (value != Red)
            {
                if (!reuseErrors) std::fprintf(stderr, "capture first reuse pixel %04x expected %04x\n", value, Red);
                ++reuseErrors;
            }
        }
    }
    std::printf("capture_readback=%s source=%d size=%dx%d offset=%d scale=%d transition=%d cpu_errors=%u reuse_errors=%u\n",
        backend, test.Source, width, height, test.Offset, scale, transition, errors, reuseErrors);
    passed &= !errors && !reuseErrors && nds->ARM9.R[4] == 0 && glGetError() == GL_NO_ERROR;
    nds->GPU.GPU3D.RenderNumPolygons = 0;
    nds->GPU.GPU3D.RenderPolygonRAM[0] = nullptr;
    return passed;
}
}

int CheckCaptureReadback(const char* backend)
{
    if (std::strcmp(backend, "software") && std::strcmp(backend, "opengl") && std::strcmp(backend, "compute")) return 2;
    bool passed = true;
    for (const auto& test : Cases)
    for (int scale : {1,2})
    for (int transition : {0,1,2})
        passed &= RunCase(backend, test, scale, transition);
    return passed ? 0 : 1;
}
