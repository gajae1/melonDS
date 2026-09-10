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

static PFNGLGENFRAMEBUFFERSPROC DriverGenFramebuffers;
static std::vector<GLuint> RendererFramebuffers;

static void APIENTRY RecordRendererFramebuffers(GLsizei count, GLuint* framebuffers)
{
    DriverGenFramebuffers(count, framebuffers);
    RendererFramebuffers.insert(RendererFramebuffers.end(), framebuffers, framebuffers + count);
}

static PFNGLDISPATCHCOMPUTEPROC DriverDispatchCompute;
static PFNGLMEMORYBARRIERPROC DriverMemoryBarrier;
static GLbitfield BarriersAfterDispatch;

static void APIENTRY RecordComputeDispatch(GLuint x, GLuint y, GLuint z)
{
    DriverDispatchCompute(x, y, z);
    BarriersAfterDispatch = 0;
}

static void APIENTRY RecordMemoryBarrier(GLbitfield barriers)
{
    DriverMemoryBarrier(barriers);
    BarriersAfterDispatch |= barriers;
}

static bool CheckComputeSampling(melonDS::NDS& nds, melonDS::GLRenderer& renderer)
{
    using namespace melonDS;
    const char* vertex = R"(#version 140
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";
    const char* fragment = R"(#version 140
uniform sampler2D InputTex;
out vec4 oColor;
void main() {
    oColor = texture(InputTex, gl_FragCoord.xy / vec2(256.0, 192.0));
})";
    GLuint program = 0;
    if (!OpenGL::CompileVertexFragmentProgram(program, vertex, fragment,
            "ComputeSamplerValidation", {}, {{"oColor", 0}})) return false;
    glUseProgram(program);
    glUniform1i(glGetUniformLocation(program, "InputTex"), 0);

    GLuint output, framebuffer, vertexArray, sampler;
    glGenTextures(1, &output);
    glBindTexture(GL_TEXTURE_2D, output);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 256, 192);
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, output, 0);
    glDrawBuffer(GL_COLOR_ATTACHMENT0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    bool passed = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
    glGenVertexArrays(1, &vertexArray);
    glGenSamplers(1, &sampler);
    glSamplerParameteri(sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    auto& gpu = nds.GPU.GPU3D;
    gpu.RenderNumPolygons = 0;
    gpu.RenderDispCnt = 0;
    gpu.RenderClearAttr2 = 0x7FFF;
    gpu.RenderFrameIdentical = false;
    renderer.Start3DRendering();
    GLint source = 0;
    glGetIntegeri_v(GL_IMAGE_BINDING_NAME, 0, &source);
    passed &= source != 0;
    DriverDispatchCompute = glad_glDispatchCompute;
    DriverMemoryBarrier = glad_glMemoryBarrier;
    constexpr u32 clearColors[] = {0x001F, 0x03E0, 0x7C00, 0x7FFF};
    constexpr u32 expected[] = {0xFF0000FF, 0xFF00FF00, 0xFFFF0000, 0xFFFFFFFF};
    constexpr int frames = 32;
    size_t mismatches = 0;
    int missingBarriers = 0;
    std::vector<u32> pixels(256 * 192);
    for (int frame = 0; frame < frames; ++frame)
    {
        gpu.RenderClearAttr1 = (31u << 16) | clearColors[frame % 4];
        BarriersAfterDispatch = 0;
        glad_glDispatchCompute = RecordComputeDispatch;
        glad_glMemoryBarrier = RecordMemoryBarrier;
        renderer.Start3DRendering();
        glad_glDispatchCompute = DriverDispatchCompute;
        glad_glMemoryBarrier = DriverMemoryBarrier;
        // OpenGL 4.3 section 7.12.2 requires visibility for this consumer even
        // on drivers where the pixel comparison happens to pass without it.
        missingBarriers += !(BarriersAfterDispatch & GL_TEXTURE_FETCH_BARRIER_BIT);

        // Sample the production image immediately: no fixture barrier, finish,
        // or CPU readback between the image store and the sampler draw.
        glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
        glViewport(0, 0, 256, 192);
        glUseProgram(program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, source);
        glBindSampler(0, sampler);
        glBindVertexArray(vertexArray);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        // Read the sampler's separate raster output, not the image-store target.
        // This also completes each sample before the next image overwrite.
        glReadPixels(0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        for (u32 pixel : pixels) mismatches += pixel != expected[frame % 4];
    }
    glBindSampler(0, 0);
    glDeleteSamplers(1, &sampler);
    glDeleteVertexArrays(1, &vertexArray);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &output);
    glDeleteProgram(program);
    std::printf("compute_sampler_frames=%d pixel_mismatches=%zu missing_texture_fetch_barriers=%d\n",
                frames, mismatches, missingBarriers);
    return passed && !mismatches && !missingBarriers && glGetError() == GL_NO_ERROR;
}

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
            while (renderer.NeedsShaderCompile())
            {
                int step, total;
                if (!renderer.ShaderCompileStep(step, total)) return false;
            }
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

struct SamplerObservation { int Unit, S, T; };
static std::vector<SamplerObservation> CaptureSamplers;
static PFNGLDISPATCHCOMPUTEINDIRECTPROC DriverDispatchIndirect;

static void APIENTRY ObserveRasterSampler(GLintptr indirect)
{
    GLint program = 0;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    const GLint location = glGetUniformLocation(program, "TexIsCapture");
    if (location >= 0)
    {
        GLint unit = 0, sampler = 0, s = 0, t = 0;
        glGetUniformiv(program, location, &unit);
        glGetIntegeri_v(GL_SAMPLER_BINDING, unit, &sampler);
        if (sampler)
        {
            glGetSamplerParameteriv(sampler, GL_TEXTURE_WRAP_S, &s);
            glGetSamplerParameteriv(sampler, GL_TEXTURE_WRAP_T, &t);
        }
        CaptureSamplers.push_back({unit, s, t});
    }
    DriverDispatchIndirect(indirect);
}

static bool CheckCaptureSamplers(melonDS::NDS& nds, melonDS::GLRenderer& renderer)
{
    using namespace melonDS;
    // Production source-B capture -> bank mapping -> five real polygons.
    // No fixture texture replaces the capture, and no sampler bind is repaired.
    // S=13/8 chooses clamp=white, repeat=blue, mirror=green. T=-3/8
    // chooses clamp=red, repeat=blue, mirror=green. These points avoid the
    // uncaptured lower 64 rows of the 256x192 capture and all wrap boundaries.
    constexpr u16 colors[] = {0x801F, 0x83E0, 0xFC00, 0xFFFF};
    constexpr int wraps[] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT,
                             GL_CLAMP_TO_EDGE, GL_CLAMP_TO_EDGE};
    auto& gpu = nds.GPU.GPU3D;
    bool passed = true;
    for (int width : {128, 256})
    for (int axis : {0, 1})
    {
        gpu.RenderNumPolygons = 0;
        nds.ARM9Write8(0x04000241, 0x80); // capture destination B -> LCDC
        nds.ARM9Write8(0x04000242, 0x80); // ordinary texture input C -> LCDC
        for (int y = 0; y < 192; ++y)
            for (int x = 0; x < 256; ++x)
                nds.ARM9Write16(0x06800000 + 2 * (y * 256 + x),
                    colors[std::min(3, (axis ? y : x) / (width / 4))]);
        for (int i = 0; i < 64; ++i) nds.ARM9Write16(0x06840000 + i * 2, 0xFC1F);
        nds.ARM9Write32(0x04000064, width == 128 ? 0xA0010000 : 0xA0310000);
        nds.RunFrame();
        nds.ARM9Write8(0x04000241, 0x83); // captured B -> texture slot 0
        nds.ARM9Write8(0x04000242, 0x8B); // ordinary C -> texture slot 1
        int captureInfo[16];
        nds.GPU.GetCaptureInfo_Texture(captureInfo);
        if (captureInfo[0] != 4) { std::fprintf(stderr, "Capture fixture did not publish bank B\n"); return false; }

        Vertex vertices[5][3]{};
        melonDS::Polygon polygons[5]{};
        for (int i = 0; i < 5; ++i)
        {
            auto& polygon = polygons[i];
            polygon.NumVertices = 3;
            polygon.Attr = (31 << 16) | (3 << 6) | (i << 24);
            // A new decal variant after the ordinary texture must return to
            // the capture unit even though its sampler matches the prior one.
            if (i == 4) polygon.Attr |= 1 << 4;
            polygon.FacingView = true;
            polygon.VTop = 0;
            polygon.VBottom = 2;
            polygon.YTop = 48;
            polygon.YBottom = 144;
            const int size = width == 128 ? 4 : 5;
            polygon.TexParam = i == 3 ? (7u << 26) | 0x4000 :
                (7u << 26) | (size << 20) | (size << 23);
            if (i == 1 || i == 2) polygon.TexParam |= 1u << (16 + axis);
            if (i == 2) polygon.TexParam |= 1u << (18 + axis);
            const int x = 8 + i * 48;
            const int positions[3][2] = {{x, 48}, {x + 38, 48}, {x + 19, 144}};
            for (int v = 0; v < 3; ++v)
            {
                auto& vertex = vertices[i][v];
                polygon.Vertices[v] = &vertex;
                polygon.FinalZ[v] = polygon.FinalW[v] = 0x1000;
                for (int c = 0; c < 3; ++c) vertex.FinalColor[c] = 63 << 3;
                for (int c = 0; c < 2; ++c)
                {
                    vertex.FinalPosition[c] = positions[v][c];
                    vertex.HiresPosition[c] = positions[v][c] << 4;
                }
                vertex.TexCoords[0] = (axis ? width / 8 : width * 13 / 8) * 16;
                vertex.TexCoords[1] = (axis ? -width * 3 / 8 : width / 8) * 16;
            }
            gpu.RenderPolygonRAM[i] = &polygon;
        }
        gpu.RenderNumPolygons = 5;
        gpu.RenderDispCnt = 1;
        gpu.RenderClearAttr1 = 0;
        gpu.RenderClearAttr2 = 0x7FFF;
        gpu.RenderFrameIdentical = false;
        CaptureSamplers.clear();
        DriverDispatchIndirect = glad_glDispatchComputeIndirect;
        glad_glDispatchComputeIndirect = ObserveRasterSampler;
        renderer.Start3DRendering();
        glad_glDispatchComputeIndirect = DriverDispatchIndirect;
        int bindingErrors = CaptureSamplers.size() != 5;
        for (size_t i = 0; i < CaptureSamplers.size() && i < 5; ++i)
        {
            const auto& state = CaptureSamplers[i];
            bindingErrors += state.Unit != (i == 3 ? 0 : width == 128 ? 1 : 2) ||
                state.S != (axis ? GL_CLAMP_TO_EDGE : wraps[i]) ||
                state.T != (axis ? wraps[i] : GL_CLAMP_TO_EDGE);
        }
        GLint output = 0;
        glGetIntegeri_v(GL_IMAGE_BINDING_NAME, 0, &output);
        glBindTexture(GL_TEXTURE_2D, output);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        std::vector<u32> pixels(256 * 192);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
        const int edge = axis ? 1 : 7; // RGB channel-presence mask
        const int expected[] = {edge, 4, 2, 5, edge};
        int pixelErrors = 0;
        for (int i = 0; i < 5; ++i)
        for (int dy = -2; dy <= 2; ++dy)
        for (int dx = -2; dx <= 2; ++dx)
        {
            const u32 pixel = pixels[(80 + dy) * 256 + 27 + i * 48 + dx];
            bool ok = (pixel >> 24) > 200;
            for (int c = 0; c < 3; ++c)
                ok &= (expected[i] & (1 << c)) ? ((pixel >> (8 * c)) & 255) > 200 :
                    ((pixel >> (8 * c)) & 255) == 0;
            pixelErrors += !ok;
        }
        std::printf("capture_sampler width=%d axis=%c variants=%zu binding_errors=%d pixel_errors=%d/125\n",
            width, axis ? 'T' : 'S', CaptureSamplers.size(), bindingErrors, pixelErrors);
        passed &= !bindingErrors && !pixelErrors && glGetError() == GL_NO_ERROR;
        gpu.RenderNumPolygons = 0;
        std::fill_n(gpu.RenderPolygonRAM.begin(), 5, nullptr);
    }
    nds.ARM9Write8(0x04000241, 0x80);
    nds.ARM9Write8(0x04000242, 0);
    return passed;
}

int CheckComputeFailure(const char* name);
int CheckCaptureReadback(const char* backend);
int CheckMidCapture(const char* backend);
int CheckGLResourceLifetime(const char* name);
int CheckGLAllocationFailure(const char* name);

int main(int argc, char** argv)
{
    using namespace melonDS;
    const bool failureCase = argc == 3 && std::strcmp(argv[1], "compute-failure") == 0;
    const bool captureCase = argc == 3 && std::strcmp(argv[1], "capture-readback") == 0;
    const bool midCaptureCase = argc == 3 && std::strcmp(argv[1], "capture-mid") == 0;
    const bool resourceCase = argc == 3 && std::strcmp(argv[1], "gl-resource") == 0;
    const bool allocationCase = argc == 3 && std::strcmp(argv[1], "gl-allocation") == 0;
    const bool compute = failureCase || allocationCase || ((captureCase || midCaptureCase) && std::strcmp(argv[2], "compute") == 0) ||
                         (argc > 1 && std::strcmp(argv[1], "compute") == 0);
    if (SDL_Init(SDL_INIT_VIDEO)) return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, compute ? 4 : 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, compute ? 3 : 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    auto* window = SDL_CreateWindow("melonDS GPU validation", 0, 0, 256, 192, SDL_WINDOW_OPENGL | SDL_WINDOW_HIDDEN);
    if (!window) { std::fprintf(stderr, "%s\n", SDL_GetError()); return 77; }
    auto context = SDL_GL_CreateContext(window);
    if (!context || !gladLoadGLLoader(SDL_GL_GetProcAddress)) return 77;
    std::printf("GPU=%s GL=%s compute=%d\n", glGetString(GL_RENDERER), glGetString(GL_VERSION), compute);
    if (failureCase || captureCase || midCaptureCase || resourceCase || allocationCase)
    {
        const int result = failureCase ? CheckComputeFailure(argv[2]) :
            captureCase ? CheckCaptureReadback(argv[2]) :
            midCaptureCase ? CheckMidCapture(argv[2]) :
            resourceCase ? CheckGLResourceLifetime(argv[2]) : CheckGLAllocationFailure(argv[2]);
        SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return result;
    }
    {
        NDSArgs args;
        args.JIT = std::nullopt;
        auto nds = std::make_unique<NDS>(std::move(args));
        nds->Reset();
        // Record real driver objects created by the renderer, excluding the
        // fixture's own readback framebuffer. Keep the context alive at teardown.
        DriverGenFramebuffers = glad_glGenFramebuffers;
        glad_glGenFramebuffers = RecordRendererFramebuffers;
        nds->SetRenderer(std::make_unique<GLRenderer>(*nds, compute));
        glad_glGenFramebuffers = DriverGenFramebuffers;
        auto* renderer = dynamic_cast<GLRenderer*>(&nds->GetRenderer());
        if (!renderer) return 1;
        RendererSettings settings{1, false, false, false};
        renderer->SetRenderSettings(settings);
        while (renderer->NeedsShaderCompile())
        {
            int step, total;
            if (!renderer->ShaderCompileStep(step, total)) return 1;
        }
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
        if (compute && !CheckComputeSampling(*nds, *renderer)) return 9;
        if (compute && !CheckCaptureSamplers(*nds, *renderer)) return 10;
        if (compute && !CheckComputeSettings(*nds, *renderer)) return 7;
    }
    size_t liveFramebuffers = 0;
    for (GLuint framebuffer : RendererFramebuffers)
    {
        if (!glIsFramebuffer(framebuffer)) continue;
        std::fprintf(stderr, "Renderer framebuffer %u survived renderer destruction\n", framebuffer);
        ++liveFramebuffers;
    }
    std::printf("renderer_framebuffers=%zu live_after_teardown=%zu\n", RendererFramebuffers.size(), liveFramebuffers);
    if (RendererFramebuffers.empty() || liveFramebuffers || glGetError() != GL_NO_ERROR) return 8;
    SDL_GL_DeleteContext(context); SDL_DestroyWindow(window); SDL_Quit();
    return 0;
}
