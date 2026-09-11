// SPDX-License-Identifier: GPL-3.0-or-later
// Actual binning producer and indirect consumer, with unsafe GPU work stopped.
#include "NDS.h"
#include "GPU_OpenGL.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

namespace
{
using namespace melonDS;
PFNGLDISPATCHCOMPUTEINDIRECTPROC DriverIndirect;
PFNGLGETINTEGERI_VPROC DriverGetIndexed;
GLint LimitZ;
unsigned IndirectCalls;
unsigned MaxProduced;

void APIENTRY GetIndexed(GLenum name, GLuint index, GLint* value)
{
    DriverGetIndexed(name, index, value);
    if (name == GL_MAX_COMPUTE_WORK_GROUP_COUNT && index == 2 && LimitZ)
        *value = std::min(*value, LimitZ);
}

void APIENTRY Indirect(GLintptr offset)
{
    GLuint groups[3];
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glGetBufferSubData(GL_DISPATCH_INDIRECT_BUFFER, offset, sizeof(groups), groups);
    for (GLuint axis = 0; axis < 3; ++axis)
    {
        GLint limit;
        GetIndexed(GL_MAX_COMPUTE_WORK_GROUP_COUNT, axis, &limit);
        if (groups[axis] > GLuint(limit))
            throw std::runtime_error("producer exceeded indirect dispatch limit");
    }
    // First variant's W is the producer's total work count. Binning can reach
    // the sorted half of this buffer before the first indirect consumer; stop
    // there, before sorting/rasterising can access outside its allocation.
    GLuint produced;
    glGetBufferSubData(GL_DISPATCH_INDIRECT_BUFFER, 12, sizeof(produced), &produced);
    GLint workBuffer;
    DriverGetIndexed(GL_SHADER_STORAGE_BUFFER_BINDING, 7, &workBuffer);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, workBuffer);
    GLint64 size;
    glGetBufferParameteri64v(GL_SHADER_STORAGE_BUFFER, GL_BUFFER_SIZE, &size);
    MaxProduced = std::max(MaxProduced, produced);
    if (produced > size / 16)
        throw std::runtime_error("producer exceeded work/tile storage capacity");
    ++IndirectCalls;
    DriverIndirect(offset);
}

struct Scene
{
    std::vector<std::array<Vertex, 4>> Vertices;
    std::vector<melonDS::Polygon> Polygons;
    Scene(unsigned count) : Vertices(count), Polygons(count)
    {
        constexpr int positions[][2] = {{0, 0}, {256, 0}, {256, 192}, {0, 192}};
        for (unsigned i = 0; i < count; ++i)
        {
            for (int v = 0; v < 4; ++v)
            {
                // A distinctive last polygon detects dropping overflow work.
                Vertices[i][v].FinalColor[i + 1 == count ? 2 : 0] = 63 << 3;
                for (int axis = 0; axis < 2; ++axis)
                {
                    Vertices[i][v].FinalPosition[axis] = positions[v][axis];
                    Vertices[i][v].HiresPosition[axis] = positions[v][axis] << 4;
                }
            }
        }
        for (unsigned i = 0; i < count; ++i)
        {
            auto& p = Polygons[i];
            p.NumVertices = 4;
            p.Attr = (31u << 16) | (3 << 6) | ((i % 63 + 1) << 24);
            p.FacingView = true;
            p.VTop = 0;
            p.VBottom = 2;
            p.YBottom = 192;
            for (int v = 0; v < 4; ++v)
            {
                p.Vertices[v] = &Vertices[i][v];
                p.FinalZ[v] = 0x100000 - i * 0x100;
                p.FinalW[v] = 0x1000;
            }
        }
    }
};

bool Render(Scene& scene, int scale, GLint limit, std::vector<u32>& pixels, u32 effects = 0)
{
    LimitZ = limit;
    IndirectCalls = MaxProduced = 0;
    NDSArgs args;
    args.JIT = std::nullopt;
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    nds->SetRenderer(std::make_unique<GLRenderer>(*nds, true));
    auto* renderer = dynamic_cast<GLRenderer*>(&nds->GetRenderer());
    if (!renderer) return false;
    RendererSettings settings{scale, false, false, false};
    renderer->SetRenderSettings(settings);
    while (renderer->NeedsShaderCompile())
    {
        int step, count;
        if (!renderer->ShaderCompileStep(step, count)) return false;
    }
    auto& gpu = nds->GPU.GPU3D;
    gpu.RenderNumPolygons = scene.Polygons.size();
    for (unsigned i = 0; i < scene.Polygons.size(); ++i)
        gpu.RenderPolygonRAM[i] = &scene.Polygons[i];
    gpu.RenderDispCnt = (1 << 3) | effects;
    gpu.RenderClearAttr1 = (31u << 16) | 0x03E0;
    gpu.RenderClearAttr2 = 0x7FFF;
    gpu.RenderFogColor = (31u << 16) | 0x7C00;
    gpu.RenderFogShift = 3;
    for (unsigned i = 0; i < 34; ++i) gpu.RenderFogDensityTable[i] = i * 3;
    for (unsigned i = 0; i < 8; ++i) gpu.RenderEdgeTable[i] = 0x03E0;
    gpu.RenderFrameIdentical = false;
    bool success = true;
    try { renderer->Start3DRendering(); }
    catch (const std::runtime_error& error)
    {
        std::fprintf(stderr, "ComputeWorkload: %s\n", error.what());
        success = false;
    }
    if (success)
    {
        GLint output;
        DriverGetIndexed(GL_IMAGE_BINDING_NAME, 0, &output);
        glBindTexture(GL_TEXTURE_2D, output);
        glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
        pixels.resize(256 * 192 * scale * scale);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    }
    std::printf("workload polygons=%zu scale=%d limit=%d max_produced=%u indirect_calls=%u\n",
        scene.Polygons.size(), scale, limit, MaxProduced, IndirectCalls);
    return success && glGetError() == GL_NO_ERROR;
}

bool CheckBlendContinuation()
{
    Scene scene(7);
    for (unsigned i = 0; i < scene.Polygons.size(); ++i)
    {
        auto& poly = scene.Polygons[i];
        poly.Attr |= 1 << 15; // fog participates only in the final pass
        if (i == 1 || i == 2)
        {
            poly.Attr = (31 << 16) | 0x30 | (3 << 6);
            poly.IsShadowMask = true;
        }
        else if (i >= 3)
        {
            const unsigned id = i == 3 ? 2 : i == 6 ? 4 : 3;
            poly.Attr = (15 << 16) | (3 << 6) | (id << 24) | (1 << 15);
            if (i == 3) poly.Attr |= 0x30;
            poly.Translucent = true;
        }
        for (int v = 0; v < 4; ++v)
        {
            poly.FinalZ[v] = i == 1 ? 0x200000 : i == 2 ? 0x10000 : i >= 3 ? 0x1000 : 0x100000;
            auto& vertex = scene.Vertices[i][v];
            std::fill_n(vertex.FinalColor, 3, 0);
            vertex.FinalColor[i % 3] = 63 << 3;
        }
    }
    bool passed = true;
    for (bool wbuffer : {false, true})
    for (u32 effects : {0u, (1u << 4) | (1u << 5) | (1u << 7)})
    {
        for (auto& poly : scene.Polygons) poly.WBuffer = wbuffer;
        std::vector<u32> together, split;
        passed &= Render(scene, 1, 0, together, effects);
        const unsigned togetherCalls = IndirectCalls;
        passed &= Render(scene, 1, 1536, split, effects);
        passed &= IndirectCalls > togetherCalls && !together.empty() && together == split;
        std::printf("blend_continuation w=%d effects=%u pixels=%zu equal=%d calls=%u/%u\n",
            wbuffer, effects, together.size(), together == split, togetherCalls, IndirectCalls);
    }
    return passed;
}
}

int CheckComputeWorkload(const char* name)
{
    DriverIndirect = glad_glDispatchComputeIndirect;
    DriverGetIndexed = glad_glGetIntegeri_v;
    glad_glDispatchComputeIndirect = Indirect;
    glad_glGetIntegeri_v = GetIndexed;
    bool passed = false;
    if (!std::strcmp(name, "blend"))
        passed = CheckBlendContinuation();
    else
    {
        const bool storage = !std::strcmp(name, "storage");
        const bool dispatch = !std::strcmp(name, "dispatch");
        const bool spans = !std::strcmp(name, "spans");
        Scene scene(storage ? 17 : dispatch ? 10 : spans ? 685 : 1);
        std::vector<u32> pixels;
        const int scale = dispatch ? 3 : 1;
        passed = Render(scene, scale, 0, pixels);
        if (passed)
        {
            // Every polygon is retained; the last one is nearest. The complete
            // interior must contain its distinct blue rather than earlier red.
            for (int y = scale; y < 191 * scale; ++y)
                for (int x = scale; x < 255 * scale; ++x)
                    passed &= pixels[y * 256 * scale + x] == 0xFFFF0000;
        }
    }
    glad_glDispatchComputeIndirect = DriverIndirect;
    glad_glGetIntegeri_v = DriverGetIndexed;
    std::printf("compute_workload=%s %s\n", name, passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
