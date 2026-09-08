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
    }
    SDL_GL_DeleteContext(context); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
