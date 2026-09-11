// SPDX-License-Identifier: GPL-3.0-or-later
// Generated 3D -> source-A capture -> guest LDRH, with no fixture GL barriers.
// This is bounded renderer evidence, not a game or physical-hardware oracle.
#include "NDS.h"
#include "GPU_OpenGL.h"
#include "GPU3D_TexcacheOpenGL.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

namespace
{
using namespace melonDS;
constexpr u16 Red = 0x801F, Green = 0x83E0, Blue = 0xFC00;
constexpr u32 Idle = 0x02000000, Code = 0x02010000;
constexpr u32 Addresses = 0x02011000, Results = 0x02012000;

bool CheckDirtyWrap()
{
    auto nds = std::make_unique<NDS>();
    auto cache = std::make_unique<TexcacheOpenGL>(nds->GPU, TexcacheOpenGLLoader(false));
    bool passed = true;
    for (u32 bytes : {0x80000u, 0x20000u}) // texture RAM and palette RAM
    {
        std::vector<u8> ram(bytes, 0);
        const u32 words = bytes / (VRAMDirtyGranularity * 64);
        // Zero padding makes the old out-of-domain lookup deterministic without
        // asking the baseline to read beyond the fixture's physical allocation.
        std::vector<u64> dirty(words * 2, 0);
        dirty[0] = 1;
        const u64 original = cache->MaskedHash(ram.data(), bytes, bytes - 256, 512);
        passed &= !cache->CheckInvalid(bytes - 256, 512, original, dirty.data(), ram.data(), bytes);
        ram[0] = 0x5A;
        const bool changed = cache->CheckInvalid(bytes - 256, 512, original, dirty.data(), ram.data(), bytes);
        passed &= changed;
        std::printf("dirty_wrap bytes=%u wrapped_change_seen=%d\n", bytes, changed);
    }
    return passed;
}

struct Scene
{
    Vertex Vertices[2][3]{};
    melonDS::Polygon Polygons[2]{};

    Scene()
    {
        for (int p = 0; p < 2; ++p)
        {
            auto& polygon = Polygons[p];
            polygon.NumVertices = 3;
            polygon.FacingView = true;
            polygon.VBottom = 2;
            polygon.YTop = 16;
            polygon.YBottom = 56;
            const int x = 16 + p * 56;
            const int positions[][2] = {{x,16}, {x+40,16}, {x+20,56}};
            for (int v = 0; v < 3; ++v)
            {
                polygon.Vertices[v] = &Vertices[p][v];
                polygon.FinalZ[v] = polygon.FinalW[v] = 0x1000;
                for (int axis = 0; axis < 2; ++axis)
                {
                    Vertices[p][v].FinalPosition[axis] = positions[v][axis];
                    Vertices[p][v].HiresPosition[axis] = positions[v][axis] << 4;
                }
            }
        }
    }

    void Set(unsigned alpha, u32 texparam, bool wbuffer, bool textureAlpha = false)
    {
        for (int p = 0; p < 2; ++p)
        {
            auto& polygon = Polygons[p];
            polygon.Attr = (alpha << 16) | (3 << 6) | ((p + 1) << 24);
            polygon.Translucent = alpha < 31 || textureAlpha;
            polygon.WBuffer = wbuffer;
            polygon.TexParam = texparam;
            for (auto& vertex : Vertices[p])
            {
                vertex.FinalColor[0] = 63 << 3;
                vertex.FinalColor[1] = vertex.FinalColor[2] = texparam ? 63 << 3 : 0;
                vertex.TexCoords[0] = 32 * 16;
                vertex.TexCoords[1] = p * 16;
            }
        }
    }

    void Submit(NDS& nds)
    {
        auto& gpu = nds.GPU.GPU3D;
        gpu.RenderNumPolygons = 2;
        for (int p = 0; p < 2; ++p) gpu.RenderPolygonRAM[p] = &Polygons[p];
        gpu.RenderFrameIdentical = false;
        nds.GetRenderer().Start3DRendering();
    }
};

std::unique_ptr<NDS> CreateNDS(const char* backend, int scale)
{
    NDSArgs args;
    args.JIT = std::nullopt;
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    if (std::strcmp(backend, "software"))
    {
        nds->SetRenderer(std::make_unique<GLRenderer>(*nds, !std::strcmp(backend, "compute")));
        if (!dynamic_cast<GLRenderer*>(&nds->GetRenderer())) return nullptr;
    }
    RendererSettings settings{scale, false, false, false};
    if (!nds->GetRenderer().SetRenderSettings(settings)) return nullptr;
    while (nds->GetRenderer().NeedsShaderCompile())
    {
        int step, count;
        if (!nds->GetRenderer().ShaderCompileStep(step, count)) return nullptr;
    }
    nds->ARM9Write32(Idle, 0xEAFFFFFE);
    nds->ARM9Write32(Idle + 0x200, 0xEAFFFFFE);
    nds->ARM9.JumpTo(Idle);
    nds->ARM7.JumpTo(Idle + 0x200);
    nds->ARM9Write16(0x04000304, 0x020F);
    nds->ARM9Write32(0x04000350, (31u << 16) | (Green & 0x7FFF));
    nds->ARM9Write32(0x04000354, 0x7FFF);
    nds->ARM9Write32(0x04000000, 0x00010000);
    for (u32 bank = 0; bank < 4; ++bank) nds->ARM9Write8(0x04000240 + bank, 0x80);
    nds->Start();
    nds->RunFrame();
    return nds;
}

bool CaptureAndRead(NDS& nds, u16 first, u16 second)
{
    nds.ARM9Write32(0x04000064, 0x81030000); // source A 3D -> bank D, 128x128
    nds.RunFrame();
    bool passed = nds.IsRunning() && !(nds.GPU.CaptureCnt & (1u << 31));
    // The first access to the capture is an actual guest CPU load. Its bus
    // synchronization is the production consumer; no glFinish/readback precedes it.
    constexpr u32 program[] = {0xE4931004, 0xE1D100B0, 0xE4820004,
                               0xE2544001, 0x1AFFFFFA, 0xEAFFFFFE};
    for (unsigned i = 0; i < std::size(program); ++i) nds.ARM9Write32(Code + i * 4, program[i]);
    constexpr int x[] = {36, 92, 4};
    for (unsigned i = 0; i < std::size(x); ++i)
    {
        nds.ARM9Write32(Addresses + i * 4, 0x06860000 + (24 * 128 + x[i]) * 2);
        nds.ARM9Write32(Results + i * 4, 0xDEAD);
    }
    nds.ARM9.R[2] = Results;
    nds.ARM9.R[3] = Addresses;
    nds.ARM9.R[4] = std::size(x);
    nds.ARM9.JumpTo(Code);
    nds.RunFrame();
    const std::array<u16, 3> expected{first, second, Green};
    for (unsigned i = 0; i < expected.size(); ++i)
    {
        const u32 actual = nds.ARM9Read32(Results + i * 4);
        if (actual != expected[i])
        {
            std::fprintf(stderr, "probe=%u actual=%04x expected=%04x\n", i, actual, expected[i]);
            passed = false;
        }
    }
    passed &= nds.ARM9.R[4] == 0 && glGetError() == GL_NO_ERROR;
    nds.ARM9.JumpTo(Idle);
    return passed;
}

bool RunAlpha(const char* backend, int scale, bool wbuffer)
{
    Scene scene; // Must outlive the renderer, including the software worker.
    auto nds = CreateNDS(backend, scale);
    if (!nds) return false;
    // A5I3 texels have alpha 16 and palette index zero. Their polygon remains
    // fully opaque, so a polygon-only alpha rejection cannot satisfy the test.
    for (unsigned i = 0; i < 64; i += 2) nds->ARM9Write16(0x06800000 + i, 0x8080);
    nds->ARM9Write8(0x04000244, 0x80);
    nds->ARM9Write16(0x06880000, Red & 0x7FFF);
    nds->ARM9Write8(0x04000240, 0x83);
    nds->ARM9Write8(0x04000244, 0x83);
    struct Case { unsigned Alpha, Reference; bool Enabled, Texture; u16 Expected; };
    constexpr Case cases[] = {
        {31, 0, true, false, Red},
        {31, 31, true, false, Green},
        {16, 15, true, false, Red},
        {16, 16, true, false, Green},
        {16, 31, false, false, Red},
        {31, 15, true, true, Red},
        {31, 16, true, true, Green},
        {31, 31, false, true, Red},
    };
    bool passed = true;
    for (const auto& test : cases)
    {
        nds->ARM9Write16(0x04000060, (test.Enabled ? 4 : 0) | (test.Texture ? 1 : 0));
        nds->ARM9Write8(0x04000340, test.Reference);
        nds->RunFrame(); // Latch through the real VBlank, including disabled alpha test.
        if (nds->GPU.GPU3D.RenderAlphaRef != (test.Enabled ? test.Reference : 0)) return false;
        scene.Set(test.Alpha, test.Texture ? 6u << 26 : 0, wbuffer, test.Texture);
        scene.Submit(*nds);
        const bool ok = CaptureAndRead(*nds, test.Expected, test.Expected);
        std::printf("alpha=%s scale=%d w=%d polygon=%u texture=%d ref=%u enabled=%d result=%s\n",
            backend, scale, wbuffer, test.Alpha, test.Texture, test.Reference, test.Enabled, ok ? "pass" : "fail");
        passed &= ok;
    }
    return passed;
}

bool RunTextureEnd(const char* backend, int scale, int width, bool crossing)
{
    Scene scene;
    auto nds = CreateNDS(backend, scale);
    if (!nds) return false;
    // A real source-A capture supplies red texels. Ordinary bank C supplies
    // blue immediately before the texture address wraps from 0x7ffff to zero.
    for (unsigned i = 0; i < 0x10000; ++i) nds->ARM9Write16(0x06840000 + i * 2, Blue);
    nds->ARM9Write32(0x04000350, (31u << 16) | (Red & 0x7FFF));
    nds->RunFrame();
    const int offset = !crossing && width == 128 ? 3 : 0;
    const u32 size = width == 128 ? 0 : 3;
    nds->ARM9Write32(0x04000064, 0x81010000 | (offset << 18) | (size << 20));
    nds->RunFrame();
    nds->ARM9Write8(0x04000240, 0); // Remove source A's LCDC mapping.
    nds->ARM9Write8(0x04000241, crossing ? 0x83 : 0x9B); // B -> texture slot 0 or 3.
    nds->ARM9Write8(0x04000242, crossing ? 0x9B : 0); // C -> texture slot 3.
    int captureInfo[16];
    nds->GPU.GetCaptureInfo_Texture(captureInfo);
    const int captureIndex = crossing ? 0 : width == 128 ? 15 : 12;
    if (captureInfo[captureIndex] != 4 + offset || (crossing && captureInfo[15] != -1)) return false;
    const u32 start = crossing ? 0x80000 - width * 2 : width == 128 ? 0x78000 : 0x60000;
    const u32 dimension = width == 128 ? 4 : 5;
    const u32 texparam = (7u << 26) | (dimension << 20) | (dimension << 23) | (start >> 3);
    nds->ARM9Write32(0x04000350, (31u << 16) | (Green & 0x7FFF));
    nds->ARM9Write16(0x04000060, 1);
    nds->RunFrame();
    scene.Set(31, texparam, false);
    scene.Submit(*nds);
    const bool initial = CaptureAndRead(*nds, crossing ? Blue : Red, Red);
    // Change only bank B after the first decode. For a wrapping texture the
    // dirty bytes are after address zero, while the bank-3 prefix stays clean.
    // The cache must invalidate from that wrapped part of the dirty bitfield.
    nds->ARM9Write8(0x04000241, 0x80);
    for (int y = 0; y < 2; ++y)
        nds->ARM9Write16(0x06820000 + offset * 0x8000 + (y * width + 32) * 2, 0xFFFF);
    nds->ARM9Write8(0x04000241, crossing ? 0x83 : 0x9B);
    scene.Submit(*nds);
    const bool updated = CaptureAndRead(*nds, crossing ? Blue : 0xFFFF, 0xFFFF);
    std::printf("texture_end=%s scale=%d width=%d start=%05x crossing=%d initial=%s updated=%s\n",
        backend, scale, width, start, crossing, initial ? "pass" : "fail", updated ? "pass" : "fail");
    return initial && updated;
}
}

int CheckGLTextureBoundaries(const char* name)
{
    if (!std::strcmp(name, "dirty-wrap")) return CheckDirtyWrap() ? 0 : 1;
    const char* backend;
    const bool alpha = std::strncmp(name, "alpha-", 6) == 0;
    if (alpha) backend = name + 6;
    else if (std::strncmp(name, "capture-", 8) == 0) backend = name + 8;
    else return 2;
    if (std::strcmp(backend, "software") && std::strcmp(backend, "opengl") && std::strcmp(backend, "compute")) return 2;
    if (alpha && !std::strcmp(backend, "compute")) return 2;
    bool passed = true;
    for (int scale : {1, 2})
    {
        if (alpha)
            for (bool wbuffer : {false, true}) passed &= RunAlpha(backend, scale, wbuffer);
        else
            for (int width : {128, 256})
                for (bool crossing : {false, true}) passed &= RunTextureEnd(backend, scale, width, crossing);
    }
    return passed ? 0 : 1;
}
