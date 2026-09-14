// SPDX-License-Identifier: GPL-3.0-or-later
// Production Vulkan 3D -> native compositor/capture/guest memory. No ROM/BIOS.
#include "NDS.h"
#include "GPU_Vulkan.h"
#include "Savestate.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <span>
#include <vector>

using namespace melonDS;
namespace
{
void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

std::unique_ptr<NDS> Console(bool vulkan)
{
    NDSArgs args; args.JIT = std::nullopt;
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    if (vulkan)
    {
        nds->SetRenderer(std::make_unique<VulkanRenderer>(*nds));
        Require(dynamic_cast<VulkanRenderer*>(&nds->GetRenderer()), "Vulkan initialization fell back");
    }
    RendererSettings settings{1, false, false, false};
    Require(nds->GetRenderer().SetRenderSettings(settings), "native settings rejected");
    nds->ARM9Write16(0x04000304, 0x020F);
    nds->ARM9Write32(0x04000350, (31u << 16) | 0x3E0);
    nds->ARM9Write32(0x04000354, 0x7FFF);
    nds->ARM9Write32(0x04000000, 0x00010108); // 3D BG0 -> engine A.
    nds->ARM9Write32(0x02000000, 0xEAFFFFFE);
    nds->ARM9Write32(0x02000200, 0xEAFFFFFE);
    nds->ARM9.JumpTo(0x02000000);
    nds->ARM7.JumpTo(0x02000200);
    return nds;
}

melonDS::Polygon& Scene(NDS& nds, unsigned format = 0, bool wbuffer = false)
{
    auto& gpu = nds.GPU.GPU3D;
    auto& polygon = gpu.PolygonRAM[0];
    polygon = {};
    polygon.NumVertices = 4;
    polygon.Attr = (31u << 16) | (3 << 6);
    polygon.FacingView = true;
    polygon.VTop = 0; polygon.VBottom = 2;
    polygon.YTop = 24; polygon.YBottom = 168;
    polygon.TexParam = format << 26;
    polygon.WBuffer = wbuffer;
    constexpr s32 positions[][2] = {{32,24}, {224,24}, {224,168}, {32,168}};
    for (unsigned v = 0; v < 4; ++v)
    {
        auto& vertex = gpu.VertexRAM[v];
        vertex = {};
        vertex.Position[3] = 0x1000; // Savestate rebuilds the clipped-polygon validity from W.
        polygon.Vertices[v] = &vertex;
        polygon.FinalZ[v] = polygon.FinalW[v] = 0x1000;
        for (unsigned axis = 0; axis < 2; ++axis)
        {
            vertex.FinalPosition[axis] = positions[v][axis];
            vertex.HiresPosition[axis] = positions[v][axis] << 4;
        }
        vertex.FinalColor[0] = 63 << 3;
        if (format) vertex.FinalColor[1] = vertex.FinalColor[2] = 63 << 3;
    }
    gpu.RenderNumPolygons = 1;
    gpu.RenderPolygonRAM[0] = &polygon;
    gpu.RenderClearAttr1 = (31u << 16) | 0x3E0;
    gpu.RenderClearAttr2 = 0x7FFF;
    gpu.RenderDispCnt = format ? 1 : 0;
    gpu.RenderFrameIdentical = false;
    return polygon;
}

void Texture(NDS& nds, unsigned format, u16 color)
{
    // Production VRAM mapping/dirty tracking and all seven DS texture decoders.
    nds.ARM9Write8(0x04000240, 0x80);
    nds.ARM9Write8(0x04000241, 0x80);
    nds.ARM9Write8(0x04000244, 0x80);
    for (unsigned i = 0; i < 128; i += 2)
        nds.ARM9Write16(0x06800000 + i, format == 1 ? 0xE0E0 : format == 6 ? 0xF8F8 : 0);
    if (format == 7)
        for (unsigned i = 0; i < 64; ++i) nds.ARM9Write16(0x06800000 + i * 2, color);
    for (unsigned i = 0; i < 8; ++i) nds.ARM9Write16(0x06820000 + i * 2, 0);
    nds.ARM9Write16(0x06880000, color & 0x7FFF);
    nds.ARM9Write8(0x04000240, 0x83);
    nds.ARM9Write8(0x04000241, 0x8B);
    nds.ARM9Write8(0x04000244, 0x83);
}

std::vector<u32> Screen(NDS& nds, bool render3D = true)
{
    auto& renderer = nds.GetRenderer();
    nds.GPU.ScreensEnabled = true;
    if (render3D) renderer.Start3DRendering();
    Require(!renderer.HasRenderFailure(), "Vulkan frame retired");
    for (u32 y = 0; y < 192; ++y)
    {
        nds.GPU.VCount = y;
        nds.GPU.GPU2D_A.UpdateWindows(y);
        nds.GPU.GPU2D_B.UpdateWindows(y);
        nds.GPU.GPU2D_A.UpdateRegistersPreDraw(y == 0);
        nds.GPU.GPU2D_B.UpdateRegistersPreDraw(y == 0);
        renderer.DrawSprites(y);
        renderer.DrawScanline(y);
        nds.GPU.GPU2D_A.UpdateRegistersPostDraw(y == 0);
        nds.GPU.GPU2D_B.UpdateRegistersPostDraw(y == 0);
    }
    renderer.SwapBuffers();
    void* top; void* bottom;
    Require(renderer.GetFramebuffers(&top, &bottom), "native screen readback unavailable");
    const auto* pixels = static_cast<const u32*>(nds.GPU.ScreenSwap ? top : bottom);
    return {pixels, pixels + 256 * 192};
}

void TexturesAndState()
{
    auto vk = Console(true);
    auto soft = Console(false);
    unsigned comparisons = 0;
    for (bool wbuffer : {false, true})
    for (unsigned format = 0; format <= 7; ++format)
    {
        Scene(*vk, format, wbuffer); Scene(*soft, format, wbuffer);
        Texture(*vk, format, 0x801F); Texture(*soft, format, 0x801F);
        const auto expected = Screen(*soft), actual = Screen(*vk);
        Require(actual == expected, "native Vulkan/Software screen pixels differ");
        if (actual[80 * 256 + 80] != 0xFFFF0000)
            std::fprintf(stderr, "native format=%u w=%d center=%08x dispcnt=%08x\n",
                format, wbuffer, actual[80 * 256 + 80], vk->GPU.GPU2D_A.DispCnt);
        Require(actual[80 * 256 + 80] == 0xFFFF0000, "test scene did not reach engine A");
        ++comparisons;
        if (format)
        {
            vk->GPU.GPU3D.RenderFrameIdentical = soft->GPU.GPU3D.RenderFrameIdentical = true;
            Texture(*vk, format, 0xFC00); Texture(*soft, format, 0xFC00);
            const auto updated = Screen(*vk);
            Require(updated == Screen(*soft) && updated != actual, "texture/palette invalidation retained stale pixels");
            ++comparisons;
        }
    }
    for (u16 scroll : {u16(7), u16(256), u16(505)})
    {
        vk->GPU.GPU3D.RenderXPos = soft->GPU.GPU3D.RenderXPos = scroll;
        Require(Screen(*vk) == Screen(*soft), "Vulkan 3D signed scroll differs");
        ++comparisons;
    }
    vk->GPU.GPU3D.RenderXPos = 0;
    const auto saved = Screen(*vk);
    Savestate state;
    Require(vk->DoSavestate(&state), "Vulkan state save rejected"); state.Finish();
    Require(!state.Error, "Vulkan state save failed");
    Require(Screen(*vk, false) == saved, "Saving a state discarded the current Vulkan 3D frame");
    Texture(*vk, 7, 0x801F);
    Require(Screen(*vk) != saved, "state test did not change texture");
    Savestate restore(state.Buffer(), state.Length(), false);
    Require(vk->DoSavestate(&restore) && !restore.Error, "Vulkan state restore rejected");
    Require(Screen(*vk) == saved, "Vulkan state restore retained stale texture/output");
    vk->SetRenderer(std::make_unique<SoftRenderer>(*vk));
    Require(Screen(*vk) == saved, "Vulkan to Software switch changed native output");
    vk->SetRenderer(std::make_unique<VulkanRenderer>(*vk));
    Require(dynamic_cast<VulkanRenderer*>(&vk->GetRenderer()) && Screen(*vk) == saved,
        "Software to Vulkan switch did not rebuild output");
    std::printf("Vulkan native: %u full-screen comparisons, texture/palette invalidation, scroll, state and renderer switching PASS\n", comparisons);
}

void Capture()
{
    auto nds = Console(true);
    nds->ARM9Write8(0x04000241, 0x80);
    nds->ARM9Write8(0x04000243, 0x80);
    nds->Start(); nds->RunFrame();
    Scene(*nds);
    nds->GetRenderer().Start3DRendering();
    nds->ARM9Write32(0x04000064, 0x81310000); // 3D -> B, 256x192.
    nds->RunFrame();
    Require(!nds->GetRenderer().HasRenderFailure(), "capture frame retired Vulkan");

    // Guest ARM LDRH reads captured VRAM, storing the result in guest RAM.
    constexpr u32 code = 0x02010000;
    nds->ARM9Write32(code, 0xE1D100B0); // ldrh r0,[r1]
    nds->ARM9Write32(code + 4, 0xE5820000); // str r0,[r2]
    nds->ARM9Write32(code + 8, 0xEAFFFFFE);
    nds->ARM9.R[1] = 0x06820000 + (80 * 256 + 80) * 2;
    nds->ARM9.R[2] = 0x02012000;
    nds->ARM9.JumpTo(code); nds->RunFrame();
    Require(nds->ARM9Read32(0x02012000) == 0x801F, "guest CPU did not read Vulkan capture");

    // Reuse that captured bitmap as a texture and capture the new 3D output.
    nds->ARM9.JumpTo(0x02000000);
    nds->ARM9Write8(0x04000241, 0x83);
    auto& polygon = Scene(*nds, 7);
    polygon.TexParam |= (5 << 20) | (5 << 23);
    for (auto* vertex : std::span(polygon.Vertices, polygon.NumVertices))
        vertex->TexCoords[0] = vertex->TexCoords[1] = 80 * 16;
    nds->ARM9Write16(0x04000060, 1);
    nds->GetRenderer().Start3DRendering();
    nds->ARM9Write32(0x04000064, 0x81330000); // 3D -> D.
    nds->RunFrame();
    Require(nds->ARM9Read16(0x06860000 + (80 * 256 + 80) * 2) == 0x801F,
        "Vulkan capture-as-texture did not survive native VRAM round trip");
    std::puts("Vulkan 3D -> capture -> guest LDRH -> texture -> capture PASS");
}

void Batches()
{
    auto vk = Console(true), soft = Console(false);
    for (auto* nds : {vk.get(), soft.get()})
    {
        Scene(*nds);
        auto& gpu = nds->GPU.GPU3D;
        const auto original = gpu.PolygonRAM[0];
        for (u32 i = 0; i < 257; ++i)
        {
            auto& polygon = gpu.PolygonRAM[i];
            polygon = original;
            polygon.Attr |= (i % 63 + 1) << 24;
            for (u32 v = 0; v < 4; ++v)
            {
                gpu.VertexRAM[i * 4 + v] = gpu.VertexRAM[v];
                polygon.Vertices[v] = &gpu.VertexRAM[i * 4 + v];
                polygon.FinalZ[v] = 0x100000 - i * 0x100;
                if (i == 256)
                {
                    polygon.Vertices[v]->FinalColor[0] = 0;
                    polygon.Vertices[v]->FinalColor[2] = 63 << 3;
                }
            }
            gpu.RenderPolygonRAM[i] = &polygon;
        }
        gpu.RenderNumPolygons = 257;
    }
    const auto actual = Screen(*vk);
    Require(actual == Screen(*soft) && actual[80 * 256 + 80] == 0xFF0000FF,
        "native batch planner dropped/reordered polygons");
    std::puts("Vulkan native batch planner: 257 overlapping polygons, final polygon preserved PASS");
}
}

int main(int argc, char** argv)
{
    try
    {
        std::string error;
        const bool available = VulkanRenderer::IsAvailable(error);
        if (argc == 2 && std::strcmp(argv[1], "unavailable") == 0)
        {
            Require(!available, "unavailable-device test found a GPU");
            auto nds = Console(false);
            nds->SetRenderer(std::make_unique<VulkanRenderer>(*nds));
            Require(typeid(nds->GetRenderer()) == typeid(SoftRenderer), "unavailable Vulkan did not select Software");
            std::puts("Vulkan unavailable: explicit Software fallback PASS");
            return 0;
        }
        if (!available) { std::fprintf(stderr, "%s\n", error.c_str()); return 77; }
        TexturesAndState(); Batches(); Capture();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Vulkan integration: %s\n", error.what());
        return 1;
    }
}
