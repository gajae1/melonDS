// SPDX-License-Identifier: GPL-3.0-or-later
// Opt-in hardware test. No external ROM/BIOS; not a game or hardware oracle.
#include <SDL.h>
#include "frontend/glad/glad.h"
#include "NDS.h"
#include "ARM.h"
#include "GPU_OpenGL.h"
#include <chrono>
#include <cstdio>
#include <vector>
#include <cstring>
#include <algorithm>

static bool CheckComputeSettings(melonDS::NDS& nds, melonDS::GLRenderer& renderer)
{
    using namespace melonDS;
    Vertex vertices[3]{};
    constexpr int positions[3][2] = {{64, 48}, {160, 48}, {112, 144}};
    melonDS::Polygon polygon{};
    polygon.NumVertices = 3;
    polygon.Attr = (31 << 16) | (3 << 6);
    polygon.FacingView = true;
    polygon.VTop = 0;
    polygon.VBottom = 2;
    polygon.YTop = 48;
    polygon.YBottom = 144;
    for (int i = 0; i < 3; ++i)
    {
        polygon.Vertices[i] = &vertices[i];
        polygon.FinalZ[i] = 0x1000;
        polygon.FinalW[i] = 0x1000;
        vertices[i].FinalColor[0] = 63 << 3;
        for (int axis = 0; axis < 2; ++axis)
        {
            vertices[i].FinalPosition[axis] = positions[i][axis];
            // The two projection paths can round differently. Deliberately
            // distinguish them so that a blank/unrendered frame cannot pass.
            vertices[i].HiresPosition[axis] = ((positions[i][axis] + 1) << 4) + 8;
        }
    }
    auto& gpu = nds.GPU.GPU3D;
    gpu.RenderNumPolygons = 1;
    gpu.RenderPolygonRAM[0] = &polygon;
    gpu.RenderDispCnt = 0;
    gpu.RenderClearAttr1 = 0;
    gpu.RenderClearAttr2 = 0x7FFF;
    bool passed = true;
    for (int scale : {1, 2})
    {
        std::vector<u32> images[2];
        for (int hires = 0; hires < 2; ++hires)
        {
            RendererSettings settings{scale, false, static_cast<bool>(hires), false};
            gpu.RenderFrameIdentical = scale > 1 && hires != 0;
            renderer.SetRenderSettings(settings);
            if (hires && renderer.NeedsShaderCompile())
            {
                std::fprintf(stderr, "Coordinate-only change rebuilds compute shaders\n");
                passed = false;
            }
            while (renderer.NeedsShaderCompile()) { int step, total; renderer.ShaderCompileStep(step, total); }
            renderer.Start3DRendering();
            GLint texture = 0;
            glGetIntegeri_v(GL_IMAGE_BINDING_NAME, 0, &texture);
            glBindTexture(GL_TEXTURE_2D, texture);
            glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
            images[hires].resize(256 * 192 * scale * scale);
            glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, images[hires].data());
            if (std::none_of(images[hires].begin(), images[hires].end(), [](u32 pixel) { return pixel & 0xFF; }))
            {
                std::fprintf(stderr, "Compute coordinate fixture did not render its triangle\n");
                passed = false;
            }
        }
        size_t changed = 0;
        for (size_t i = 0; i < images[0].size(); ++i) changed += images[0][i] != images[1][i];
        std::printf("compute_scale=%d coordinate_toggle_changed_pixels=%zu\n", scale, changed);
        if ((scale == 1 && changed != 0) || (scale == 2 && changed == 0)) passed = false;
    }
    gpu.RenderNumPolygons = 0;
    gpu.RenderPolygonRAM[0] = nullptr;
    return passed && glGetError() == GL_NO_ERROR;
}

int main(int argc, char** argv)
{
    using namespace melonDS;
    const bool compute = argc > 1 && std::strcmp(argv[1], "compute") == 0;
    if (SDL_Init(SDL_INIT_VIDEO)) return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, compute ? 4 : 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, compute ? 3 : 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    auto* window = SDL_CreateWindow("melonDS GPU validation", 0, 0, 256, 192, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) { std::fprintf(stderr, "%s\n", SDL_GetError()); return 77; }
    auto context = SDL_GL_CreateContext(window);
    if (!context || !gladLoadGLLoader(SDL_GL_GetProcAddress)) return 77;
    std::printf("GPU=%s GL=%s compute=%d\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), compute);
    {
        NDSArgs args;
        args.JIT = std::nullopt;
        auto nds = std::make_unique<NDS>(std::move(args));
        nds->Reset();
        nds->SetRenderer(std::make_unique<GLRenderer>(*nds, compute));
        auto* renderer = dynamic_cast<GLRenderer*>(&nds->GetRenderer());
        if (!renderer) return 1;
        RendererSettings settings{1, false, false, false};
        renderer->SetRenderSettings(settings);
        while (renderer->NeedsShaderCompile()) { int step, total; renderer->ShaderCompileStep(step, total); }
        nds->ARM9Write32(0x02000000, 0xEAFFFFFE);
        nds->ARM9Write32(0x02000200, 0xEAFFFFFE);
        nds->ARM9.JumpTo(0x02000000);
        nds->ARM7.JumpTo(0x02000200);
        nds->ARM9Write16(0x04000304, 0x0203);
        nds->ARM9Write8(0x04000240, 0x80);
        nds->ARM9Write8(0x04000241, 0x80);
        nds->ARM9Write32(0x04000000, 0x00020000);
        for (unsigned i = 0; i < 256*192; ++i) nds->ARM9Write16(0x06800000+2*i, 0x801F);
        nds->Start();
        nds->RunFrame(); nds->RunFrame();
        void* texture = nullptr; void* unused = nullptr;
        renderer->GetFramebuffers(&texture, &unused);
        if (!texture) return 2;
        GLuint fb;
        glGenFramebuffers(1, &fb);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, fb);
        glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, *static_cast<GLuint*>(texture), 0, 1);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        if (glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) return 3;
        std::vector<u32> pixels(256*192);
        glReadPixels(0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        // GL_RGBA bytes: expanded 5-bit red is 251, matching the software path.
        for (unsigned i = 0; i < pixels.size(); ++i)
            if (pixels[i] != 0xFF0000FB) { std::fprintf(stderr,"pixel[%u]=%08x\n",i,pixels[i]); return 4; }
        glDeleteFramebuffers(1, &fb);
        // Capture source B (VRAM A) into bank B, 256x192.
        nds->ARM9Write32(0x04000064, 0xA0310000);
        nds->RunFrame();
        renderer->SyncVRAMCapture(1, 0, 3, true);
        for (unsigned i = 0; i < 256*192; ++i) {
            u16 value;
            std::memcpy(&value, nds->GPU.VRAM[1] + 2*i, sizeof(value));
            if (value != 0x801F) { std::fprintf(stderr,"capture[%u]=%04x\n",i,value); return 5; }
        }
        std::vector<double> samples;
        for (int i = 0; i < 31; ++i) {
            glFinish();
            const auto start = std::chrono::steady_clock::now();
            renderer->SyncVRAMCapture(1, 0, 3, true);
            const double us = std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-start).count();
            if (i) samples.push_back(us);
        }
        if (glGetError() != GL_NO_ERROR) return 6;
        std::sort(samples.begin(), samples.end());
        std::printf("frame_pixels=%zu capture_pixels=49152 readback_median_us=%.3f PASS\n", pixels.size(), (samples[14]+samples[15])/2);
        if (compute && !CheckComputeSettings(*nds, *renderer)) return 7;
    }
    SDL_GL_DeleteContext(context); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
