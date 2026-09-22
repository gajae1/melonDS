// SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in real GL test: compares palette texels with the former full upload,
// records complete rendered frames for baseline/candidate byte comparison,
// and checks upload work. No ROM, BIOS, timing loop, or production test hooks.
#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <vector>
#include "NDS.h"
#include "GPU_OpenGL.h"

using namespace melonDS;
namespace
{
PFNGLTEXSUBIMAGE2DPROC DriverUpload;
GLuint Palettes[2];
unsigned Calls[2], Bytes[2];
void APIENTRY Upload(GLenum target, GLint level, GLint x, GLint y,
                     GLsizei w, GLsizei h, GLenum format, GLenum type, const void* data)
{
    GLint texture = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
    for (int e = 0; e < 2; ++e)
        if (GLuint(texture) == Palettes[e]) { ++Calls[e]; Bytes[e] += w*h*2; }
    DriverUpload(target, level, x, y, w, h, format, type, data);
}

// Obtain the renderer-owned textures from its actual uploads (no layout/ABI
// dependency on GLRenderer). BG palettes uniquely have height 65.
PFNGLTEXIMAGE2DPROC DriverImage;
unsigned PaletteCount;
void APIENTRY Image(GLenum target, GLint level, GLint internal, GLsizei w,
                    GLsizei h, GLint border, GLenum format, GLenum type, const void* data)
{
    DriverImage(target, level, internal, w, h, border, format, type, data);
    if (w == 256 && h == 65 && PaletteCount < 2)
    {
        GLint texture;
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture);
        Palettes[PaletteCount++] = texture;
    }
}

bool Install(NDS& nds)
{
    PaletteCount = 0;
    glad_glTexImage2D = Image;
    nds.SetRenderer(std::make_unique<GLRenderer>(nds, false));
    glad_glTexImage2D = DriverImage;
    auto* renderer = dynamic_cast<GLRenderer*>(&nds.GetRenderer());
    if (!renderer || PaletteCount != 2) return false;
    RendererSettings settings{1, false, false, false};
    if (!renderer->SetRenderSettings(settings)) return false;
    while (renderer->NeedsShaderCompile())
    {
        int step, total;
        if (!renderer->ShaderCompileStep(step, total)) return false;
    }
    return true;
}

void Normal(NDS& nds, u16 color)
{
    for (unsigned e = 0; e < 2; ++e)
        nds.ARM9Write16(0x05000002 + e*0x400, color);
}

void Extended(NDS& nds, unsigned row, u16 color)
{
    for (unsigned e = 0; e < 2; ++e)
    {
        // E and H are unmap/write/remapped before the next renderer sample.
        const u32 control = e ? 0x04000248 : 0x04000244;
        const u32 address = e ? 0x06898000 : 0x06880000;
        nds.ARM9Write8(control, 0x80);
        nds.ARM9Write16(address + row*512 + 2, color);
        nds.ARM9Write8(control, e ? 0x82 : 0x84);
    }
}

void Configure(NDS& nds)
{
    nds.ARM9Write16(0x04000304, 0x0203);
    nds.GPU.ScreensEnabled = true;
    nds.ARM9Write8(0x04000240, 0x81); // A: engine A BG
    nds.ARM9Write8(0x04000242, 0x84); // C: engine B BG
    for (unsigned e = 0; e < 2; ++e)
    {
        const u32 vram = e ? 0x06200000 : 0x06000000;
        for (unsigned i = 0; i < 64; i += 4)
            nds.ARM9Write32(vram+i, 0x01010101); // 8bpp tile, color 1
        for (unsigned i = 0; i < 2048; i += 2)
            nds.ARM9Write16(vram+0x800+i, ((i/2)%16)<<12);
        auto& engine = e ? nds.GPU.GPU2D_B : nds.GPU.GPU2D_A;
        engine.Enabled = true;
        engine.DispCnt = 0x00010100;
        engine.BGCnt[0] = 0x0180; // text, 8bpp, map at 0x800
        engine.LayerEnable = 1;
        engine.ForcedBlank = 0;
    }
    Normal(nds, 0x001F);
    for (unsigned row = 0; row < 64; ++row)
        Extended(nds, row, 0x8000 | ((row*997 + 31) & 0x7FFF));
}

bool Check(NDS& nds, const char* stage, unsigned expectedBytes, unsigned expectedCalls,
           bool optimized, FILE* snapshot)
{
    bool ok = true;
    for (unsigned e = 0; e < 2; ++e)
    {
        std::array<u16, 65*256> expected{}, actual{};
        std::memcpy(expected.data(), &nds.GPU.Palette[e*0x400], 512);
        const auto* flat = e ? nds.GPU.VRAMFlat_BBGExtPal : nds.GPU.VRAMFlat_ABGExtPal;
        std::memcpy(expected.data()+256, flat, 32768);
        glBindTexture(GL_TEXTURE_2D, Palettes[e]);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_SHORT_1_5_5_5_REV, actual.data());
        const bool texels = actual == expected;
        const bool work = !optimized || (Bytes[e] == expectedBytes && Calls[e] == expectedCalls);
        std::printf("%s engine=%u bytes=%u calls=%u texels=%s work=%s\n",
                    stage, e, Bytes[e], Calls[e], texels ? "pass" : "FAIL", work ? "pass" : "FAIL");
        ok &= texels && work;
        if (snapshot) std::fwrite(actual.data(), sizeof(actual), 1, snapshot);
        Bytes[e] = Calls[e] = 0;
    }
    return ok && glGetError() == GL_NO_ERROR;
}

bool Sequence(NDS& nds, bool optimized, FILE* snapshot)
{
    Configure(nds);
    bool ok = true;
    auto& renderer = nds.GetRenderer();
    for (unsigned line = 0; line < 192; ++line)
    {
        const char* stage = nullptr;
        unsigned bytes = 0, calls = 0;
        switch (line)
        {
        case 0: stage = "first"; bytes = 33280; calls = 1; break;
        case 8: stage = "clean"; break;
        case 16: Normal(nds, 0x03E0); stage = "normal"; bytes = 512; calls = 1; break;
        case 24:
            // observed: OBJ reuses TempPalBuffer; subsequent BG gaps need fresh staging.
            nds.ARM9Write16(0x07000000, 0x0200);
            nds.ARM9Write16(0x07000400, 0x0200);
            renderer.DrawSprites(line);
            stage = "obj-staging"; break;
        case 32:
            nds.GPU.GPU2D_A.DispCnt |= 1u<<30; nds.GPU.GPU2D_B.DispCnt |= 1u<<30;
            Extended(nds, 2, 0x7C00); Extended(nds, 9, 0x03FF);
            // observed: extended rows 2..9 bound eight texture rows, including gaps.
            stage = "sparse"; bytes = 4096; calls = 1; break;
        case 48:
            Extended(nds, 15, 0x7FE0); Extended(nds, 16, 0x7C1F);
            stage = "slot-boundary"; bytes = 1024; calls = 1; break;
        case 64: Extended(nds, 63, 0x7FFF); stage = "last-row"; bytes = 512; calls = 1; break;
        case 80:
            Normal(nds, 0x7C00); Extended(nds, 0, 0x03E0);
            stage = "combined"; bytes = 1024; calls = 1; break;
        case 96:
            nds.GPU.ScreensEnabled = false;
            Extended(nds, 3, 0x001F); Normal(nds, 0x03FF);
            stage = "disabled-normal"; bytes = 512; calls = 1; break;
        case 104:
            Extended(nds, 4, 0x7FFF); stage = "disabled-extended"; break;
        case 112:
            nds.GPU.ScreensEnabled = true;
            stage = "reenabled"; bytes = 1024; calls = 1; break;
        case 128:
            nds.ARM9Write8(0x04000244, 0x80); nds.ARM9Write8(0x04000248, 0x80);
            stage = "unmap"; bytes = 32768; calls = 1; break;
        case 144:
            nds.ARM9Write8(0x04000244, 0x84); nds.ARM9Write8(0x04000248, 0x82);
            stage = "remap"; bytes = 32768; calls = 1; break;
        case 160:
            nds.GPU.GPU2D_A.DispCnt &= ~(1u<<30); nds.GPU.GPU2D_B.DispCnt &= ~(1u<<30);
            stage = "normal-mode"; break;
        case 176:
            Normal(nds, 0x7C1F);
            for (unsigned row = 1; row < 64; row += 2)
                Extended(nds, row, 0x8000 | ((row*313 + 7) & 0x7FFF));
            // observed: normal + alternating extended rows bound all 65 rows.
            stage = "fragmented"; bytes = 33280; calls = 1; break;
        }
        renderer.DrawScanline(line);
        if (stage) ok &= Check(nds, stage, bytes, calls, optimized, snapshot);
    }
    renderer.VBlank();
    renderer.SwapBuffers();
    void *top = nullptr, *bottom = nullptr;
    renderer.GetFramebuffers(&top, &bottom);
    if (!top) return false;
    std::vector<u32> pixels(256*192*2);
    glBindTexture(GL_TEXTURE_2D_ARRAY, *static_cast<GLuint*>(top));
    glGetTexImage(GL_TEXTURE_2D_ARRAY, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    if (snapshot) std::fwrite(pixels.data(), pixels.size()*sizeof(u32), 1, snapshot);
    // A normal palette change must affect only subsequent screen sections.
    // The differential snapshot additionally covers every pixel and flag.
    const bool visible = pixels[8*256+8] != pixels[24*256+8];
    std::printf("frame delayed_change=%s\n", visible ? "pass" : "FAIL");
    return ok && visible && glGetError() == GL_NO_ERROR;
}
}

int main(int argc, char** argv)
{
    if (argc != 3) return 2; // baseline|optimized snapshot.bin
    if (SDL_Init(SDL_INIT_VIDEO)) return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    auto* window = SDL_CreateWindow("GL2D palette validation", 0, 0, 256, 192,
                                    SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) return 77;
    auto context = SDL_GL_CreateContext(window);
    if (!context || !gladLoadGLLoader(SDL_GL_GetProcAddress)) return 77;
    DriverUpload = glad_glTexSubImage2D; DriverImage = glad_glTexImage2D;
    FILE* snapshot = std::fopen(argv[2], "wb");
    if (!snapshot) return 2;
    bool ok = true;
    {
        NDSArgs args; args.JIT = std::nullopt;
        auto nds = std::make_unique<NDS>(std::move(args));
        nds->Reset();
        if (!Install(*nds)) return 1;
        glad_glTexSubImage2D = Upload;
        const bool optimized = std::strcmp(argv[1], "optimized") == 0;
        ok &= Sequence(*nds, optimized, snapshot);
        nds->Reset();
        // First upload while disabled must initialize all rows, as before.
        nds->GPU.ScreensEnabled = false;
        nds->GetRenderer().DrawScanline(0);
        ok &= Check(*nds, "reset-disabled-first", 33280, 1, optimized, snapshot);
        ok &= Sequence(*nds, optimized, snapshot);
        Savestate saved;
        nds->GPU.DoSavestate(&saved);
        saved.Finish();
        Savestate restored(saved.Buffer(), saved.Length(), false);
        nds->GPU.DoSavestate(&restored);
        if (saved.Error || restored.Error) return 1;
        ok &= Sequence(*nds, optimized, snapshot);
        nds->SetRenderer(nullptr);
        if (!Install(*nds)) return 1;
        ok &= Sequence(*nds, optimized, snapshot);
    }
    glad_glTexSubImage2D = DriverUpload;
    std::fclose(snapshot);
    SDL_GL_DeleteContext(context); SDL_DestroyWindow(window); SDL_Quit();
    std::printf("GL2D palette result=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
