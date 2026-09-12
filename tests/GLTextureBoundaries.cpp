// SPDX-License-Identifier: GPL-3.0-or-later
// Generated 3D -> source-A capture -> guest LDRH, with no fixture GL barriers.
// This is bounded renderer evidence, not a game or physical-hardware oracle.
#include "NDS.h"
#include "GPU_OpenGL.h"
#include "GPU_Soft.h"
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

class ShadingReference : public SoftRenderer
{
public:
    using SoftRenderer::SoftRenderer;
    u32 Pixel(int x, int y)
    {
        Finish3DRendering();
        return Rend3D->GetLine(y)[x];
    }
};

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
    if (!std::strcmp(backend, "software"))
        nds->SetRenderer(std::make_unique<ShadingReference>(*nds));
    else
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

bool CaptureAndRead(NDS& nds, u16 first, u16 second, u16 background = Green)
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
    const std::array<u16, 3> expected{first, second, background};
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
    struct Case
    {
        unsigned Alpha, Reference;
        bool Enabled, Texture;
        u16 Expected;
        bool Blend = false;
        unsigned ClearAlpha = 31;
    };
    constexpr Case cases[] = {
        {31, 0, true, false, Red},
        {31, 31, true, false, Green},
        {16, 15, true, false, Red},
        {16, 16, true, false, Green},
        {16, 15, true, false, 0x81D0, true},
        {16, 31, false, false, Red},
        {31, 15, true, true, 0x81D0, true},
        {31, 15, true, true, Red},
        {31, 16, true, true, Green},
        {31, 31, false, true, Red},
        {16, 15, true, false, Red, true, 0},
    };
    bool passed = true;
    for (const auto& test : cases)
    {
        nds->ARM9Write16(0x04000060,
            (test.Enabled ? 4 : 0) | (test.Texture ? 1 : 0) | (test.Blend ? 8 : 0));
        nds->ARM9Write32(0x04000350, (test.ClearAlpha << 16) | (Green & 0x7FFF));
        nds->ARM9Write8(0x04000340, test.Reference);
        nds->RunFrame(); // Latch through the real VBlank, including disabled alpha test.
        if (nds->GPU.GPU3D.RenderAlphaRef != (test.Enabled ? test.Reference : 0)) return false;
        scene.Set(test.Alpha, test.Texture ? 6u << 26 : 0, wbuffer, test.Texture);
        scene.Submit(*nds);
        const bool ok = CaptureAndRead(*nds, test.Expected, test.Expected,
            test.ClearAlpha ? Green : Green & 0x7FFF);
        std::printf("alpha=%s scale=%d w=%d polygon=%u texture=%d ref=%u enabled=%d blend=%d clear_alpha=%u result=%s\n",
            backend, scale, wbuffer, test.Alpha, test.Texture, test.Reference, test.Enabled,
            test.Blend, test.ClearAlpha, ok ? "pass" : "fail");
        passed &= ok;
    }
    return passed;
}

bool RunBlend(const char* backend, int scale, bool wbuffer)
{
    Scene scene;
    auto nds = CreateNDS(backend, scale);
    if (!nds) return false;
    // Overlap both triangles. Equal IDs reject the second translucent polygon;
    // different IDs must blend with the first polygon's already rounded result.
    for (int v = 0; v < 3; ++v)
    {
        scene.Vertices[1][v].FinalPosition[0] -= 56;
        scene.Vertices[1][v].HiresPosition[0] -= 56 << 4;
    }
    bool passed = true;
    for (bool sameID : {true, false})
    {
        nds->ARM9Write16(0x04000060, 8);
        nds->RunFrame();
        scene.Set(8, 0, wbuffer);
        scene.Polygons[1].Attr = (16 << 16) | (3 << 6) | ((sameID ? 1 : 2) << 24);
        for (auto& vertex : scene.Vertices[1])
        {
            vertex.FinalColor[0] = 0;
            vertex.FinalColor[2] = 63 << 3;
        }
        scene.Submit(*nds);
        const bool ok = CaptureAndRead(*nds, sameID ? 0x82C8 : 0xC143, Green);
        std::printf("blend=%s scale=%d w=%d same_id=%d result=%s\n",
            backend, scale, wbuffer, sameID, ok ? "pass" : "fail");
        passed &= ok;
    }
    // A low-intensity rear plane distinguishes six-bit integer truncation from
    // normalized GL rounding even when the red component happens to agree.
    nds->ARM9Write32(0x04000350, (31u << 16) | (8 << 5));
    nds->RunFrame();
    scene.Set(16, 0, wbuffer);
    scene.Polygons[1].Attr = scene.Polygons[0].Attr;
    scene.Submit(*nds);
    const bool low = CaptureAndRead(*nds, 0x8070, 0x8100, 0x8100);
    std::printf("blend=%s scale=%d w=%d low_clear result=%s\n",
        backend, scale, wbuffer, low ? "pass" : "fail");
    passed &= low;
    // Bitmap rear planes supply their own per-pixel alpha. Bank B supplies
    // texture slot 3 (depth), C supplies slot 2 (color); D stays the capture sink.
    nds->ARM9Write8(0x04000241, 0x80);
    for (u32 i = 0; i < 0x20000; i += 2) nds->ARM9Write16(0x06820000 + i, 0x7FFF);
    nds->ARM9Write8(0x04000241, 0x9B);
    const struct { u16 color, expected; } bitmaps[] = {
        {Green, 0x81D0}, {Green & 0x7FFF, Red}, {0x8100, 0x8070},
    };
    for (const auto& bitmap : bitmaps)
    {
        nds->ARM9Write8(0x04000242, 0x80);
        for (u32 i = 0; i < 0x20000; i += 2) nds->ARM9Write16(0x06840000 + i, bitmap.color);
        nds->ARM9Write8(0x04000242, 0x93);
        nds->ARM9Write16(0x04000060, (1 << 14) | 8);
        nds->RunFrame();
        scene.Submit(*nds);
        const bool ok = CaptureAndRead(*nds, bitmap.expected, bitmap.color, bitmap.color);
        std::printf("blend=%s scale=%d w=%d bitmap=%04x result=%s\n",
            backend, scale, wbuffer, bitmap.color, ok ? "pass" : "fail");
        passed &= ok;
    }
    return passed;
}

// Direct six-bit shading evidence. Unlike the capture tests above, this reads
// the GPU output explicitly: fifteen-bit guest capture cannot distinguish 30
// from 31 in a six-bit channel. It is not evidence for capture synchronization.
u32 ShadingPixel(NDS& nds, const char* backend, int scale, int x, int y)
{
    if (auto* soft = dynamic_cast<ShadingReference*>(&nds.GetRenderer())) return soft->Pixel(x, y);
    GLint texture = 0;
    if (!std::strcmp(backend, "compute"))
    {
        glGetIntegeri_v(GL_IMAGE_BINDING_NAME, 0, &texture);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    }
    else
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);
    std::vector<u32> pixels(256 * 192 * scale * scale);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    const u32 color = pixels[(y * scale) * (256 * scale) + x * scale];
    return ((color >> 2) & 0x003F3F3F) | ((color >> 3) & 0x1F000000);
}

bool RunShading(const char* backend, int scale)
{
    Scene scene;
    auto nds = CreateNDS(backend, scale);
    if (!nds) return false;
    bool passed = true;
    // The same production shader handles polygon and texel alpha. Values just
    // below and above the first nonzero modulated alpha catch promotion of zero.
    const struct { unsigned polygon, texture; u32 expected; } alpha[] = {
        {4, 4, 0x00003F00}, {4, 5, 0x00003F00}, {4, 6, 0x013F3F3F}, {31, 31, 0x1F3F3F3F},
    };
    for (const auto& test : alpha)
    {
        nds->ARM9Write8(0x04000240, 0x80);
        for (u32 i = 0; i < 64; i += 2) nds->ARM9Write16(0x06800000 + i, test.texture * 0x0808);
        nds->ARM9Write8(0x04000244, 0x80);
        nds->ARM9Write16(0x06880000, 0x7FFF);
        nds->ARM9Write8(0x04000240, 0x83);
        nds->ARM9Write8(0x04000244, 0x83);
        nds->ARM9Write32(0x04000350, Green & 0x7FFF);
        nds->ARM9Write16(0x04000060, 1 | 8);
        nds->RunFrame();
        scene.Set(test.polygon, 6u << 26, false, true);
        scene.Submit(*nds);
        const u32 actual = ShadingPixel(*nds, backend, scale, 36, 24);
        std::printf("shading=%s scale=%d alpha=%u*%u actual=%08x expected=%08x\n",
            backend, scale, test.polygon, test.texture, actual, test.expected);
        passed &= actual == test.expected;
    }
    const struct { unsigned density; u16 color; bool alphaOnly; u32 expected; } fog[] = {
        {0, 0, false, 0x1F3F3F3F}, {64, 0, false, 0x1F1F1F1F},
        {65, 0, false, 0x1F1F1F1F}, {126, 0, false, 0x1F000000},
        {127, 8 << 5, false, 0x1F001100}, {65, 8 << 5, false, 0x1F1F271F},
        {65, 0, true, 0x0F3F3F3F},
    };
    for (const auto& test : fog)
    {
        nds->ARM9Write16(0x04000060, (1 << 7) | (test.alphaOnly ? 1 << 6 : 0));
        nds->ARM9Write32(0x04000350, (31u << 16) | 0xFFFF);
        nds->ARM9Write32(0x04000358, (test.alphaOnly ? 0 : 31u << 16) | test.color);
        for (unsigned i = 0; i < 32; ++i) nds->ARM9Write8(0x04000360 + i, test.density);
        nds->RunFrame();
        scene.Set(31, 0, false);
        scene.Submit(*nds);
        const u32 actual = ShadingPixel(*nds, backend, scale, 4, 24);
        std::printf("shading=%s scale=%d fog=%u color=%04x alpha_only=%d actual=%08x expected=%08x\n",
            backend, scale, test.density, test.color, test.alphaOnly, actual, test.expected);
        passed &= actual == test.expected;
        if (test.density == 65 && test.color == 0)
        {
            // The final fog texture also has to reach the ordinary capture
            // consumer after exchanging the two GL color textures. The raw
            // read above deliberately means this is not a synchronization test.
            passed &= CaptureAndRead(*nds, Red, Red, test.alphaOnly ? 0xFFFF : 0xBDEF);
        }
    }
    return passed && glGetError() == GL_NO_ERROR;
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

u32 BitmapPixel(NDS& nds, bool software, int scale, int x, int y)
{
    void* top = nullptr;
    void* bottom = nullptr;
    nds.GetRenderer().GetFramebuffers(&top, &bottom);
    if (software) return static_cast<u32*>(bottom)[y * 256 + x] & 0xFFFFFF;
    GLuint framebuffer;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
    glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                              *static_cast<GLuint*>(top), 0, 1);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    u32 pixel = 0;
    glReadPixels(x * scale, y * scale, 1, 1, GL_BGRA, GL_UNSIGNED_BYTE, &pixel);
    glDeleteFramebuffers(1, &framebuffer);
    return pixel & 0xFFFFFF;
}

bool RunBitmap(const char* backend, int scale, int size, bool direct)
{
    auto nds = CreateNDS(backend, scale);
    if (!nds) return false;
    const bool software = !std::strcmp(backend, "software");
    nds->ARM9Write16(0x04000304, 0x820F); // Sub engine on the bottom screen.
    nds->ARM9Write8(0x04000242, 0x84); // Bank C -> sub BG.
    for (u32 address = 0; address < 0x20000; address += 2)
        nds->ARM9Write16(0x06200000 + address, direct ? Red : 0x0101);
    nds->ARM9Write16(0x05000402, Red & 0x7FFF);
    nds->ARM9Write16(0x05000404, Green & 0x7FFF);
    nds->ARM9Write16(0x05000406, Blue & 0x7FFF);
    nds->ARM9Write32(0x04001000, 0x10405); // Mode 5, BG2 bitmap.
    nds->ARM9Write16(0x0400100C, 0x80 | (direct ? 4 : 0) | (size == 256 ? 0x4000 : 0));
    nds->ARM9Write16(0x04001020, 0x100);
    nds->ARM9Write16(0x04001026, 0x100);
    nds->RunFrame();
    const int boundary = size / 2;
    // These literal colors follow the existing 2D RGB555 readback convention.
    bool passed = BitmapPixel(*nds, software, scale, 8, boundary) == 0xFB0000;
    constexpr u32 program[] = {0xE1C010B0, 0xEAFFFFFE}; // STRH r1,[r0]; B .
    for (unsigned i = 0; i < std::size(program); ++i) nds->ARM9Write32(Code + i * 4, program[i]);
    auto writePixel = [&](int y, u16 value) {
        const u32 address = 0x06200000 + (y * size + 8) * (direct ? 2 : 1);
        nds->ARM9.R[0] = address;
        nds->ARM9.R[1] = value;
        nds->ARM9.JumpTo(Code);
        nds->RunFrame();
        nds->ARM9.JumpTo(Idle);
        return nds->ARM9Read16(address) == value;
    };
    passed &= writePixel(boundary - 1, direct ? Green : 0x0102);
    const u32 before = BitmapPixel(*nds, software, scale, 8, boundary - 1);
    passed &= writePixel(boundary, direct ? Blue : 0x0103);
    const u32 after = BitmapPixel(*nds, software, scale, 8, boundary);
    // A first-half write must not be needed to reveal the second-half change.
    passed &= writePixel(boundary - 1, direct ? Green : 0x0102);
    const u32 refreshed = BitmapPixel(*nds, software, scale, 8, boundary);
    passed &= before == 0x00FB00 && after == 0x0000FB && after == refreshed;
    std::printf("bitmap=%s scale=%d size=%d direct=%d y=%d before=%06x after=%06x refreshed=%06x result=%s\n",
                backend, scale, size, direct, boundary, before, after, refreshed, passed ? "pass" : "fail");
    return passed && glGetError() == GL_NO_ERROR;
}
}

int CheckGLTextureBoundaries(const char* name)
{
    if (!std::strcmp(name, "dirty-wrap")) return CheckDirtyWrap() ? 0 : 1;
    const char* backend;
    const bool alpha = std::strncmp(name, "alpha-", 6) == 0;
    const bool blend = std::strncmp(name, "blend-", 6) == 0;
    const bool shading = std::strncmp(name, "shading-", 8) == 0;
    const bool bitmap = std::strncmp(name, "bitmap-", 7) == 0;
    if (alpha || blend) backend = name + 6;
    else if (shading) backend = name + 8;
    else if (bitmap) backend = name + 7;
    else if (std::strncmp(name, "capture-", 8) == 0) backend = name + 8;
    else return 2;
    if (std::strcmp(backend, "software") && std::strcmp(backend, "opengl") && std::strcmp(backend, "compute")) return 2;
    bool passed = true;
    for (int scale : {1, 2})
    {
        if (alpha)
            for (bool wbuffer : {false, true}) passed &= RunAlpha(backend, scale, wbuffer);
        else if (blend)
            for (bool wbuffer : {false, true}) passed &= RunBlend(backend, scale, wbuffer);
        else if (shading)
            passed &= RunShading(backend, scale);
        else if (bitmap)
        {
            for (int size : {128, 256}) passed &= RunBitmap(backend, scale, size, true);
            passed &= RunBitmap(backend, scale, 256, false);
        }
        else
            for (int width : {128, 256})
                for (bool crossing : {false, true}) passed &= RunTextureEnd(backend, scale, width, crossing);
    }
    return passed ? 0 : 1;
}
