// SPDX-License-Identifier: GPL-3.0-or-later
// Production Vulkan 3D -> native compositor/capture/guest memory. No ROM/BIOS.
#include "NDS.h"
#include "GPU_Vulkan.h"
#include "Savestate.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#ifdef _WIN32
#include <windows.h>
#endif
#include <utility>
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

std::unique_ptr<NDS> Console(bool vulkan, int scale = 1)
{
    NDSArgs args; args.JIT = std::nullopt;
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    if (vulkan)
    {
        nds->SetRenderer(std::make_unique<VulkanRenderer>(*nds));
        Require(dynamic_cast<VulkanRenderer*>(&nds->GetRenderer()), "Vulkan initialization fell back");
    }
    RendererSettings settings{scale, false, false, false};
    Require(nds->GetRenderer().SetRenderSettings(settings), "render scale rejected");
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

melonDS::Polygon& Scene(NDS& nds, unsigned format = 0, bool wbuffer = false, bool legacyFacing = true)
{
    auto& gpu = nds.GPU.GPU3D;
    auto& polygon = gpu.PolygonRAM[0];
    polygon = {};
    polygon.NumVertices = 4;
    polygon.Attr = (31u << 16) | (3 << 6);
    // Preserve the original 1x regression fixture, including its reversed
    // edge orientation. New scaled fixtures use the orientation matching the
    // vertex order, so a constant rectangle has scale-invariant native coverage.
    polygon.FacingView = legacyFacing;
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

void ScaledDisplay(int scale)
{
    auto nds = Console(true, scale);
    Scene(*nds, 0, false, false);
    Screen(*nds); Screen(*nds, false);
    void* top = nullptr; void* bottom = nullptr;
    int width = 0, height = 0;
    Require(nds->GetRenderer().GetDisplayFramebuffers(&top, &bottom, width, height), "display is not RAM");
    Require(width == 256 * scale && height == 192 * scale, "scaled Vulkan display extent is still native");
    const auto* pixels = static_cast<const u32*>(nds->GPU.ScreenSwap ? top : bottom);
    Require(pixels && pixels[(80 * scale) * width + 80 * scale] == 0xFFFF0000, "scaled display lost foreground");
    Require(pixels[(12 * scale) * width + 80 * scale] != 0xFFFF0000, "scaled display has wrong row stride");
    std::printf("Vulkan %dx display extent and full-image addressing PASS\n", scale);
}


unsigned DisplayOrigins(NDS& nds, int scale, bool requireDetail = false)
{
    void *nativeTop, *nativeBottom, *displayTop, *displayBottom;
    auto& renderer = nds.GetRenderer();
    Require(renderer.GetFramebuffers(&nativeTop, &nativeBottom), "native framebuffer missing");
    int width = 0, height = 0;
    Require(renderer.GetDisplayFramebuffers(&displayTop, &displayBottom, width, height), "display framebuffer missing");
    Require(width == 256 * scale && height == 192 * scale, "display extent mismatch");
    const u32* native[] = {static_cast<const u32*>(nativeTop), static_cast<const u32*>(nativeBottom)};
    const u32* display[] = {static_cast<const u32*>(displayTop), static_cast<const u32*>(displayBottom)};
    unsigned detail = 0;
    for (int screen = 0; screen < 2; ++screen)
    for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
    {
        const u32 expected = native[screen][(y / scale) * 256 + x / scale];
        const u32 actual = display[screen][size_t(y) * width + x];
        if (x % scale == 0 && y % scale == 0)
            Require(actual == expected, "scaled composition changed a native-origin pixel");
        detail += actual != expected;
    }
    if (requireDetail) Require(detail != 0, "scaled display only stretches native pixels");
    return detail;
}

void ScaledDisplayLifecycle(int scale)
{
    auto nds = Console(true, scale);
    auto& polygon = Scene(*nds, 0, false, false);
    for (auto* vertex : std::span(polygon.Vertices, polygon.NumVertices))
        vertex->HiresPosition[0] += 8;
    // Transparent clear pixels expose the backdrop when the native sample
    // misses a shape that still covers a displayed subpixel.
    nds->GPU.GPU3D.RenderClearAttr1 = 0;
    nds->ARM9Write16(0x05000000, 0x7C00);
    RendererSettings settings{scale, false, true, false};
    Require(nds->GetRenderer().SetRenderSettings(settings), "hires display setting rejected");
    Screen(*nds); Screen(*nds, false);
    DisplayOrigins(*nds, scale, true);
    for (u16 scroll : {u16(7), u16(256), u16(505), u16(0)})
    {
        nds->GPU.GPU3D.RenderXPos = scroll;
        Screen(*nds, false);
        DisplayOrigins(*nds, scale);
    }
    for (u16 brightness : {u16(0x4008), u16(0x8010), u16(0x401F), u16(0)})
    {
        nds->ARM9Write16(0x0400006C, brightness);
        Screen(*nds, false);
        DisplayOrigins(*nds, scale);
    }
    nds->GPU.ScreenSwap = !nds->GPU.ScreenSwap;
    Screen(*nds, false);
    DisplayOrigins(*nds, scale, true);
    nds->ARM9Write16(0x04000040, (64 << 8) | 192);
    nds->ARM9Write16(0x04000044, (32 << 8) | 160);
    nds->ARM9Write16(0x04000048, 0x3E); // Window hides BG0, preserves other layers.
    nds->ARM9Write16(0x0400004A, 0x3F);
    for (u32 display : {0x00012108u, 0x00010188u, 0x00020000u, 0x00030000u, 0u, 0x00010108u})
    {
        nds->ARM9Write32(0x04000000, display);
        Screen(*nds, false); Screen(*nds, false);
        DisplayOrigins(*nds, scale);
    }
    for (bool reset : {false, true})
    {
        if (reset) nds->GetRenderer().Reset();
        else nds->GetRenderer().Stop();
        void *top, *bottom; int width, height;
        nds->GetRenderer().GetDisplayFramebuffers(&top, &bottom, width, height);
        Require(width == 256 * scale && height == 192 * scale, "stop/reset lost display allocation");
        for (const auto* pixels : {static_cast<const u32*>(top), static_cast<const u32*>(bottom)})
            Require(std::all_of(pixels, pixels + size_t(width) * height, [](u32 p) { return p == 0; }),
                "stop/reset left stale scaled pixels");
    }
    std::printf("Vulkan %dx: real subpixel coverage, native-origin equivalence, scroll, brightness, swap, window, modes, stop/reset PASS\n", scale);
}

void CapturedDisplay(int scale)
{
    auto nds = Console(true, scale);
    nds->ARM9Write8(0x04000241, 0x80);
    nds->Start(); nds->RunFrame();
    auto& polygon = Scene(*nds, 0, false, false);
    for (auto* vertex : std::span(polygon.Vertices, polygon.NumVertices))
        vertex->HiresPosition[0] += 8;
    RendererSettings settings{scale, false, true, false};
    Require(nds->GetRenderer().SetRenderSettings(settings), "capture scale setup failed");
    nds->GetRenderer().Start3DRendering();
    nds->ARM9Write32(0x04000064, 0x81310000); // 3D -> bank B, 256x192.
    nds->RunFrame();
    nds->ARM9Write32(0x04000000, 0x00060000); // Display bank B directly.
    Screen(*nds, false);
    DisplayOrigins(*nds, scale, true);
    nds->GPU.SyncAllVRAMCaptures();
    Screen(*nds, false);
    Require(DisplayOrigins(*nds, scale) == 0, "invalidated display capture retained detail");
    std::printf("Captured 3D %dx: subpixel detail, native origins and invalidation PASS\n", scale);
}

void CapturedBitmapBG(int scale, unsigned engine, bool benchmark = false)
{
    auto nds = Console(true, scale);
    const unsigned bank = engine ? 2 : 1;
    nds->ARM9Write8(0x04000240 + bank, 0x80);
    nds->Start(); nds->RunFrame();
    auto& polygon = Scene(*nds, 0, false, false);
    for (auto* vertex : std::span(polygon.Vertices, polygon.NumVertices))
        vertex->HiresPosition[0] += 8;
    nds->GPU.GPU3D.RenderClearAttr1 = 0;
    RendererSettings settings{scale, false, true, false};
    Require(nds->GetRenderer().SetRenderSettings(settings), "BG capture settings failed");
    nds->GetRenderer().Start3DRendering();
    nds->ARM9Write32(0x04000064, 0x81300000 | (bank << 16));
    nds->RunFrame();
    const std::vector<u8> captured(nds->GPU.VRAM[bank], nds->GPU.VRAM[bank] + 131072);
    nds->ARM9Write8(0x04000240 + bank, engine ? 0x84 : 0x81);
    const u32 reg = 0x04000000 + engine * 0x1000;
    nds->ARM9Write32(0x04000000, 0x00010000);
    nds->ARM9Write16(0x05000000 + engine * 0x400, 0x7C00);
    nds->ARM9Write32(reg, 0x00010405); // Mode 5, direct-color BG2.
    nds->ARM9Write16(reg + 0x0C, 0x4084); // 256x256, map base 0.
    nds->ARM9Write16(reg + 0x20, 0x100);
    nds->ARM9Write16(reg + 0x26, 0x100);
    nds->ARM9Write32(reg + 0x28, 0); nds->ARM9Write32(reg + 0x2C, 0);
    Screen(*nds, false); Screen(*nds, false);
    DisplayOrigins(*nds, scale, true);
    Require(std::equal(captured.begin(), captured.end(), nds->GPU.VRAM[bank]), "BG enhancement modified guest capture");
    if (benchmark)
    {
        for (int frame = 0; frame < 4; ++frame) Screen(*nds, false);
        const auto begin = std::chrono::steady_clock::now();
        for (int frame = 0; frame < 16; ++frame) Screen(*nds, false);
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - begin).count() / 16;
        void *top, *bottom; int width, height;
        nds->GetRenderer().GetDisplayFramebuffers(&top, &bottom, width, height);
        u64 checksum = 0;
        for (const auto* pixels : {static_cast<const u32*>(top), static_cast<const u32*>(bottom)})
        for (size_t pixel = 0; pixel < size_t(width) * height; ++pixel) checksum += pixels[pixel];
        std::printf("bitmap_ns=%lld checksum=%llu scale=%d\n",
            static_cast<long long>(elapsed), static_cast<unsigned long long>(checksum), scale);
        return;
    }
    // Integer affine transforms must retain the native-origin samples.
    // Other subpixels may differ: that is the requested display enhancement.
    unsigned cases = 0;
    const auto checkFrame = [&] {
        Screen(*nds, false); Screen(*nds, false);
        DisplayOrigins(*nds, scale);
        ++cases;
    };
    for (const auto& transform : {std::array<s32, 6>{256,0,0,256,0,0},
            {256,0,0,256,17*256,9*256}, {-256,0,0,256,255*256,0},
            {0,256,-256,0,0,191*256}, {512,0,0,256,-32*256,0}})
    {
        for (u32 i = 0; i < 4; ++i) nds->ARM9Write16(reg + 0x20 + 2*i, transform[i]);
        nds->ARM9Write32(reg + 0x28, transform[4]);
        nds->ARM9Write32(reg + 0x2C, transform[5]);
        for (bool wrap : {false, true}) {
            nds->ARM9Write16(reg + 0x0C, 0x4084 | (wrap ? 0x2000 : 0));
            checkFrame();
        }
    }
    nds->ARM9Write16(reg + 0x20, 256); nds->ARM9Write16(reg + 0x22, 0);
    nds->ARM9Write16(reg + 0x24, 0); nds->ARM9Write16(reg + 0x26, 256);
    nds->ARM9Write32(reg + 0x28, 0); nds->ARM9Write32(reg + 0x2C, 0);
    nds->ARM9Write16(reg + 0x0C, 0x4084);
    // A second captured bitmap can expose a lower layer through transparent edges.
    nds->ARM9Write32(reg, 0x00010C05);
    nds->ARM9Write16(reg + 0x0E, 0x4085);
    nds->ARM9Write16(reg + 0x30, 256); nds->ARM9Write16(reg + 0x36, 256);
    nds->ARM9Write32(reg + 0x38, 32*256); nds->ARM9Write32(reg + 0x3C, 0);
    nds->ARM9Write16(reg + 0x52, 0x0808); nds->ARM9Write16(reg + 0x54, 7);
    for (u16 effect = 0; effect < 4; ++effect) {
        nds->ARM9Write16(reg + 0x50, (effect << 6) | 0x3F3F);
        for (u16 priority : {u16(0), u16(1), u16(3)}) {
            nds->ARM9Write16(reg + 0x0C, 0x4084 | priority);
            checkFrame();
        }
    }
    // Native OBJ still competes with captured BG at every priority/effect.
    nds->ARM9Write8(0x04000243, 0x80); // D: prepare an ordinary OBJ tile.
    for (u32 offset = 0; offset < 32; offset += 2)
        nds->ARM9Write16(0x06860000 + offset, 0x1111);
    nds->ARM9Write8(0x04000243, engine ? 0x84 : 0x82);
    const u32 oam = 0x07000000 + engine * 0x400;
    for (u32 object = 0; object < 128; ++object)
        nds->ARM9Write16(oam + object * 8, 0x0200);
    nds->ARM9Write16(oam + 2, 28);
    nds->ARM9Write16(0x05000202 + engine * 0x400, 0x7C1F);
    nds->ARM9Write32(reg, 0x00011C05);
    for (u16 spriteMode : {u16(0), u16(0x400)})
    for (u16 priority : {u16(0), u16(1), u16(3)})
    {
        nds->ARM9Write16(oam, 64 | spriteMode);
        nds->ARM9Write16(oam + 4, priority << 10);
        for (u16 effect = 0; effect < 4; ++effect) {
            nds->ARM9Write16(reg + 0x50, (effect << 6) | 0x3F3F);
            checkFrame();
        }
    }
    nds->ARM9Write16(oam, 0x0200);
    nds->ARM9Write16(reg + 0x40, (24 << 8) | 110);
    nds->ARM9Write16(reg + 0x44, 192);
    nds->ARM9Write16(reg + 0x48, 0x3B); // Hide BG2 inside WIN0.
    nds->ARM9Write16(reg + 0x4A, 0x3F);
    nds->ARM9Write32(reg, 0x00012C05); checkFrame();
    nds->ARM9Write32(reg, 0x00010405); nds->ARM9Write16(reg + 0x50, 0);
    nds->ARM9Write16(reg + 0x0C, 0x4084);
    nds->ARM9Write16(reg + 0x4C, 0x0033);
    nds->ARM9Write16(reg + 0x0C, 0x40C4); checkFrame(); // Mosaic stays native.
    nds->ARM9Write16(reg + 0x0C, 0x4084);
    checkFrame(); DisplayOrigins(*nds, scale, true);
    Require(std::equal(captured.begin(), captured.end(), nds->GPU.VRAM[bank]),
        "display-only affine/blend processing modified emulated VRAM");
    // Conflicting mappings must retain the hardware's ORed native value.
    const u32 otherControl = engine ? 0x04000248 : 0x04000240; // H follows WRAMCNT.
    nds->ARM9Write8(otherControl, 0x81);
    Require((engine ? nds->GPU.VRAMMap_BBG[0] : nds->GPU.VRAMMap_ABG[0]) ==
        (engine ? ((1u << 2) | (1u << 7)) : ((1u << 0) | (1u << 1))),
        "fixture did not create overlapping background mappings");
    checkFrame();
    {
        void *top, *bottom, *nativeTop, *nativeBottom; int width, height;
        nds->GetRenderer().GetDisplayFramebuffers(&top, &bottom, width, height);
        nds->GetRenderer().GetFramebuffers(&nativeTop, &nativeBottom);
        const unsigned selected = engine ? !nds->GPU.ScreenSwap : nds->GPU.ScreenSwap;
        const auto* display = static_cast<const u32*>(selected ? top : bottom);
        const auto* native = static_cast<const u32*>(selected ? nativeTop : nativeBottom);
        for (int y = 0; y < 64 * scale; ++y)
        for (int x = 0; x < width; ++x)
            Require(display[size_t(y)*width+x] == native[(y/scale)*256+x/scale],
                "overlapping VRAM mappings used one bank's enhanced pixels");
    }
    nds->ARM9Write8(otherControl, 0);
    checkFrame(); DisplayOrigins(*nds, scale, true);
    if (!engine)
    {
        nds->ARM9Write32(reg, 0x0001050D); // Captured BG2 and live 3D BG0.
        for (u16 effect = 0; effect < 4; ++effect) {
            nds->ARM9Write16(reg + 8, effect & 3);
            nds->ARM9Write16(reg + 0x50, (effect << 6) | 0x3F3F);
            checkFrame();
        }
        nds->ARM9Write32(reg, 0x00010405); nds->ARM9Write16(reg + 0x50, 0);
    }
    for (u16 dimensions = 0; dimensions < 4; ++dimensions) {
        nds->ARM9Write16(reg + 0x0C, 0x2084 | (dimensions << 14));
        checkFrame();
    }
    nds->ARM9Write16(reg + 0x0C, 0x4084);
    nds->ARM9Write16(reg + 0x0E, 0x4084);
    nds->ARM9Write32(reg + 0x38, 0);
    for (u32 mode : {3u, 4u, 5u}) {
        nds->ARM9Write32(reg, 0x00010800 | mode);
        checkFrame();
    }
    nds->ARM9Write32(reg, 0x00010405);
    for (int nextScale : {1, scale == 2 ? 3 : 2, scale}) {
        RendererSettings next{nextScale, false, true, false};
        Require(nds->GetRenderer().SetRenderSettings(next), "captured BG scale transition failed");
        Screen(*nds, false); Screen(*nds, false);
        DisplayOrigins(*nds, nextScale, nextScale == scale);
    }
    // A same-value CPU write still invalidates the enhanced provenance.
    const u32 alias = engine ? 0x06200000 : 0x06000000;
    const u16 original = nds->ARM9Read16(alias + 0xA040);
    nds->ARM9Write16(alias + 0xA040, original);
    checkFrame();
    void *top, *bottom; int displayWidth, displayHeight;
    nds->GetRenderer().GetDisplayFramebuffers(&top, &bottom, displayWidth, displayHeight);
    void *nativeTop, *nativeBottom; nds->GetRenderer().GetFramebuffers(&nativeTop, &nativeBottom);
    for (const auto& pair : {std::pair{top,nativeTop}, std::pair{bottom,nativeBottom}})
    for (int y = 0; y < displayHeight; ++y)
    for (int x = 0; x < displayWidth; ++x)
        Require(static_cast<const u32*>(pair.first)[size_t(y)*displayWidth+x] ==
                static_cast<const u32*>(pair.second)[(y/scale)*256+x/scale],
                "CPU alias write left stale enhanced bitmap samples");
    std::printf("BG engine %u scale %d: %u affine/priority/effect/window/mosaic/invalidation frames PASS\n",
        engine, scale, cases);
    std::printf("Captured bitmap BG engine %u at %dx retains detail and native VRAM PASS\n", engine, scale);
}

void CheckCacheClear(NDS& nds, const std::vector<u32>& native)
{
    auto& renderer = static_cast<VulkanRenderer&>(nds.GetRenderer());
    void *top, *bottom; int width, height;
    Require(renderer.GetDisplayFramebuffers(&top, &bottom, width, height), "display view unavailable");
    const size_t count = size_t(width) * height;
    const std::vector<u32> beforeTop(static_cast<u32*>(top), static_cast<u32*>(top)+count);
    const std::vector<u32> beforeBottom(static_cast<u32*>(bottom), static_cast<u32*>(bottom)+count);
    renderer.ClearPipelineCache();
    int afterWidth, afterHeight;
    Require(renderer.GetDisplayFramebuffers(&top, &bottom, afterWidth, afterHeight) &&
        width == afterWidth && height == afterHeight, "cache clear changed display extent");
    Require(std::equal(beforeTop.begin(), beforeTop.end(), static_cast<u32*>(top)) &&
        std::equal(beforeBottom.begin(), beforeBottom.end(), static_cast<u32*>(bottom)),
        "cache clear changed the current display pixels");
    Require(renderer.GetFramebuffers(&top, &bottom), "native view unavailable after cache clear");
    Require(std::equal(native.begin(), native.end(), static_cast<u32*>(nds.GPU.ScreenSwap ? top : bottom)),
        "cache clear changed the current native pixels");
}

void TexturesAndState(int scale)
{
    auto vk = Console(true, scale);
    auto soft = Console(false);
    unsigned comparisons = 0;
    for (bool wbuffer : {false, true})
    for (unsigned format = 0; format <= 7; ++format)
    {
        Scene(*vk, format, wbuffer, scale == 1); Scene(*soft, format, wbuffer, scale == 1);
        Texture(*vk, format, 0x801F); Texture(*soft, format, 0x801F);
        const auto expected = Screen(*soft), actual = Screen(*vk);
        if (actual != expected)
        {
            unsigned differences = 0;
            for (unsigned i = 0; i < actual.size(); ++i)
            {
                if (actual[i] == expected[i]) continue;
                if (differences++ < 4)
                    std::fprintf(stderr, "scale=%d format=%u w=%d xy=%u,%u Vulkan=%08x Software=%08x\n",
                        scale, format, wbuffer, i % 256, i / 256, actual[i], expected[i]);
            }
            std::fprintf(stderr, "%u native pixel differences\n", differences);
        }
        Require(actual == expected, "native Vulkan/Software screen pixels differ");
        CheckCacheClear(*vk, actual);
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
    RendererSettings settings{scale, false, false, false};
    Require(dynamic_cast<VulkanRenderer*>(&vk->GetRenderer()) &&
        vk->GetRenderer().SetRenderSettings(settings) && Screen(*vk) == saved,
        "Software to Vulkan switch did not rebuild output");
    std::printf("Vulkan %dx: %u native full-screen comparisons, texture/palette invalidation, scroll, state and renderer switching PASS\n", scale, comparisons);
}

void Capture(int scale)
{
    auto nds = Console(true, scale);
    nds->ARM9Write8(0x04000241, 0x80);
    nds->ARM9Write8(0x04000243, 0x80);
    nds->Start(); nds->RunFrame();
    Scene(*nds, 0, false, scale == 1);
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
    auto& polygon = Scene(*nds, 7, false, scale == 1);
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

void Batches(int scale)
{
    auto vk = Console(true, scale), soft = Console(false);
    for (auto* nds : {vk.get(), soft.get()})
    {
        Scene(*nds, 0, false, scale == 1);
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
    std::printf("Vulkan %dx batch planner: 257 overlapping polygons, final polygon preserved PASS\n", scale);
}

void HighScales()
{
    auto nds = Console(true), soft = Console(false);
    Scene(*soft, 0, false, false);
    Screen(*soft);
    const auto expected = Screen(*soft, false);
    for (int scale = 4; scale <= 16; ++scale)
    {
        RendererSettings settings{scale, false, false, false};
        Require(nds->GetRenderer().SetRenderSettings(settings), "high Vulkan scale rejected");
        auto& polygon = Scene(*nds, 0, false, false);
        for (bool wbuffer : {false, true})
        {
            polygon.WBuffer = wbuffer;
            nds->GPU.GPU3D.RenderFrameIdentical = false;
            Screen(*nds);
            const auto native = Screen(*nds, false);
            Require(native == expected, "high scale changed native rectangle/capture source");
            void* top = nullptr; void* bottom = nullptr; int width = 0, height = 0;
            Require(nds->GetRenderer().GetDisplayFramebuffers(&top, &bottom, width, height), "high scale RAM view unavailable");
            Require(width == 256 * scale && height == 192 * scale, "high display extent mismatch");
            const u32* pixels = static_cast<const u32*>(nds->GPU.ScreenSwap ? top : bottom);
            for (unsigned y = 0; y < 192; ++y)
            for (unsigned x = 0; x < 256; ++x)
                Require(pixels[size_t(y * scale) * width + x * scale] == native[y * 256 + x], "high scaled origin mismatch");
        }
        std::printf("Vulkan %dx: Z/W, full native rectangle and scaled origins PASS\n", scale);
        std::fflush(stdout);
    }
    RendererSettings native{1, false, false, false};
    Require(nds->GetRenderer().SetRenderSettings(native) && Screen(*nds) == expected, "16x to native recovery failed");
    Batches(16); Capture(16);
}

void ScaleChanges()
{
    auto nds = Console(true);
    auto& polygon = Scene(*nds, 7, false, false);
    Texture(*nds, 7, 0x801F);
    // A half-native-pixel translation disappears at 1x, but must affect actual
    // coverage at 2x/3x. Merely resizing a native image cannot pass this check.
    for (auto* vertex : std::span(polygon.Vertices, polygon.NumVertices))
        vertex->HiresPosition[0] += 8;
    Screen(*nds); // Settle the two-scanline display-enable latch before comparing retained frames.
    const auto native = Screen(*nds, false);
    Require(Screen(*nds, false) == native, "compositor fixture is unstable before changing settings");
    auto previous = native;
    for (int scale : {2, 3, 1, 3, 2, 1})
    {
        RendererSettings settings{scale, false, false, false};
        Require(nds->GetRenderer().SetRenderSettings(settings), "live scale change rejected");
        Require(Screen(*nds, false) == previous, "scale change discarded the current frame");
        nds->GPU.GPU3D.RenderFrameIdentical = true;
        Require(Screen(*nds) == native, "integer-coordinate rectangle resolve changed native coverage");
        settings.HiresCoordinates = true;
        Require(nds->GetRenderer().SetRenderSettings(settings), "hires setting change rejected");
        Require(Screen(*nds, false) == native, "hires change discarded the current frame");
        const auto actual = Screen(*nds);
        Require((actual == native) == (scale == 1), "scale/hires change did not update subpixel coverage");
        previous = actual;
    }
    for (int scale : {0, 17})
    {
        RendererSettings settings{scale, false, false, false};
        Require(!nds->GetRenderer().SetRenderSettings(settings), "unsupported Vulkan scale was accepted");
        Require(!nds->GetRenderer().HasRenderFailure() && Screen(*nds, false) == previous,
            "invalid settings damaged the current renderer/frame");
    }
    // Native and high-resolution coordinates can differ by whole pixels due
    // to the DS divider's precision loss. 1x must retain the original DS path.
    for (auto* vertex : std::span(polygon.Vertices, polygon.NumVertices))
        vertex->HiresPosition[0] += 16;
    RendererSettings nativeHires{1, false, true, false};
    Require(nds->GetRenderer().SetRenderSettings(nativeHires), "native hires setting rejected");
    nds->GPU.GPU3D.RenderFrameIdentical = false;
    Require(Screen(*nds) == native, "1x hires setting bypassed native DS coordinate rounding");
    std::puts("Vulkan live 1x/2x/3x transitions: subpixel coverage, cached texture, frame lifetime, native rounding and invalid scale rejection PASS");
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
        if (argc == 2 && std::strcmp(argv[1], "captured-display") == 0) { for (int scale : {2, 3, 5, 8, 16}) CapturedDisplay(scale); return 0; }
        if (argc == 3 && std::strcmp(argv[1], "bitmap-benchmark") == 0) {
#ifdef _WIN32
            DWORD_PTR mask = 0, system = 0;
            Require(GetProcessAffinityMask(GetCurrentProcess(), &mask, &system) &&
                SetProcessAffinityMask(GetCurrentProcess(), mask & (~mask + 1)), "CPU affinity failed");
#endif
            CapturedBitmapBG(std::atoi(argv[2]), 0, true); return 0;
        }
        if (argc == 2 && std::strcmp(argv[1], "captured-bitmap-bg") == 0) { for (int scale : {2, 3, 5, 8}) { CapturedBitmapBG(scale, 0); CapturedBitmapBG(scale, 1); } return 0; }
        if (argc == 2 && std::strcmp(argv[1], "high-scales") == 0) { HighScales(); return 0; }
        const int scale = argc == 2 && std::strcmp(argv[1], "2") == 0 ? 2 :
            argc == 2 && std::strcmp(argv[1], "3") == 0 ? 3 : 1;
        if (scale > 1) { ScaledDisplay(scale); ScaledDisplayLifecycle(scale); }
        TexturesAndState(scale); Batches(scale); Capture(scale);
        if (scale == 1) ScaleChanges();
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Vulkan integration: %s\n", error.what());
        return 1;
    }
}
