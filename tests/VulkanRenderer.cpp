// SPDX-License-Identifier: GPL-3.0-or-later
// Production Vulkan 3D -> native compositor/capture/guest memory. No ROM/BIOS.
#include "NDS.h"
#include "GPU_Vulkan.h"
#include "Vulkan/ComputePipeline.h"
#include "Vulkan/EmbeddedShaders.h"
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
#include <initializer_list>
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

bool GetCpuDisplayFrame(Renderer& renderer, Renderer::DisplayFrame& frame)
{
    return renderer.GetDisplayFrame(frame) && frame.kind == Renderer::DisplayFrame::Kind::CpuBGRA;
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
    Renderer::DisplayFrame frame;
    const auto& [kind, top, bottom, width, height, generation] = frame;
    Require(GetCpuDisplayFrame(nds->GetRenderer(), frame), "display is not RAM");
    Require(width == 256 * scale && height == 192 * scale, "scaled Vulkan display extent is still native");
    const auto* pixels = static_cast<const u32*>(nds->GPU.ScreenSwap ? top : bottom);
    Require(pixels && pixels[(80 * scale) * width + 80 * scale] == 0xFFFF0000, "scaled display lost foreground");
    Require(pixels[(12 * scale) * width + 80 * scale] != 0xFFFF0000, "scaled display has wrong row stride");
    std::printf("Vulkan %dx display extent and full-image addressing PASS\n", scale);
}


unsigned DisplayOrigins(NDS& nds, int scale, bool requireDetail = false)
{
    void *nativeTop, *nativeBottom;
    auto& renderer = nds.GetRenderer();
    Require(renderer.GetFramebuffers(&nativeTop, &nativeBottom), "native framebuffer missing");
    Renderer::DisplayFrame frame;
    const auto& [kind, displayTop, displayBottom, width, height, generation] = frame;
    Require(GetCpuDisplayFrame(renderer, frame), "display framebuffer missing");
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
        Renderer::DisplayFrame frame;
        const auto& [kind, top, bottom, width, height, generation] = frame;
        Require(GetCpuDisplayFrame(nds->GetRenderer(), frame), "stop/reset display unavailable");
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

void CapturedBitmapOBJ(int scale, unsigned engine, bool boundaryOnly = false)
{
    auto nds = Console(true, scale);
    const unsigned bank = engine ? 3 : 1; // B -> AOBJ, D -> BOBJ.
    nds->ARM9Write8(0x04000240 + bank, 0x80);
    // The capture ends at row 192. Keep a distinguishable native-only tail
    // for partial-provenance/fallback checks, without later VRAM writes.
    for (u32 offset = 192 * 512; offset < 131072; offset += 2)
        nds->ARM9Write16(0x06800000 + bank * 131072 + offset, 0xFC00);
    nds->Start(); nds->RunFrame();
    auto& polygon = Scene(*nds, 0, false, false);
    for (auto* vertex : std::span(polygon.Vertices, polygon.NumVertices))
        vertex->HiresPosition[0] += 8;
    if (boundaryOnly)
    {
        // Expand the original captured rectangle by one source texel on each
        // side, preserving its half-pixel X offset. No renderer edits.
        for (unsigned i = 0; i < polygon.NumVertices; ++i)
        {
            auto& v = *polygon.Vertices[i];
            const int dx = (i == 0 || i == 3) ? -1 : 1;
            const int dy = i < 2 ? -1 : 1;
            v.FinalPosition[0] += dx; v.HiresPosition[0] += dx * 16;
            v.FinalPosition[1] += dy; v.HiresPosition[1] += dy * 16;
        }
        --polygon.YTop; ++polygon.YBottom;
    }
    nds->GPU.GPU3D.RenderClearAttr1 = 0;
    RendererSettings settings{scale, false, true, false};
    Require(nds->GetRenderer().SetRenderSettings(settings), "OBJ capture settings failed");
    nds->GetRenderer().Start3DRendering();
    nds->ARM9Write32(0x04000064, 0x81300000 | (bank << 16));
    nds->RunFrame();
    const std::vector<u8> captured(nds->GPU.VRAM[bank], nds->GPU.VRAM[bank] + 131072);

    // Read the same capture through the existing LCDC display, before remapping.
    // Transparent clear is black, matching the OBJ scene backdrop.
    nds->ARM9Write32(0x04000000, 0x00020000 | (bank << 18));
    Screen(*nds, false); Screen(*nds, false);
    Renderer::DisplayFrame frame;
    const auto& [kind, top, bottom, width, height, generation] = frame;
    Require(GetCpuDisplayFrame(nds->GetRenderer(), frame),
        "LCDC reference framebuffer unavailable");
    Require(width == 256 * scale && height == 192 * scale, "LCDC reference extent mismatch");
    const auto* source = static_cast<const u32*>(nds->GPU.ScreenSwap ? top : bottom);
    const std::vector<u32> reference(source, source + size_t(width) * height);
    if (scale > 1) DisplayOrigins(*nds, scale, true);

    nds->ARM9Write8(0x04000240 + bank, engine ? 0x84 : 0x82);
    Require((engine ? nds->GPU.VRAMMap_BOBJ[0] : nds->GPU.VRAMMap_AOBJ[0]) == (1u << bank),
        "fixture did not map capture to OBJ");
    const u32 reg = 0x04000000 + engine * 0x1000;
    const u32 oam = 0x07000000 + engine * 0x400;
    nds->ARM9Write32(0x04000000, 0x00010000);
    nds->ARM9Write16(0x05000000 + engine * 0x400, 0);
    for (u32 object = 0; object < 128; ++object) nds->ARM9Write16(oam + object * 8, 0x0200);
    if (boundaryOnly)
    {
        Require(scale == 5, "boundary fixture requires exact 5x scale");
        nds->ARM9Write16(oam, 0x0C00 | 29);
        nds->ARM9Write16(oam + 2, 0xF000 | 17); // 64x64, XY flip, x=17.
        nds->ARM9Write16(oam + 4, 0xF004); // Source (32,0), alpha 16.
        nds->ARM9Write32(reg, 0x00011020);
        Screen(*nds, false); Screen(*nds, false);
        void *nt, *nb;
        Require(nds->GetRenderer().GetFramebuffers(&nt, &nb), "boundary native frame missing");
        Require(GetCpuDisplayFrame(nds->GetRenderer(), frame),
            "boundary display frame missing");
        const bool selected = engine ? !nds->GPU.ScreenSwap : nds->GPU.ScreenSwap;
        const auto* display = static_cast<const u32*>(selected ? top : bottom);
        const auto* nativeFrame = static_cast<const u32*>(selected ? nt : nb);
        size_t nativeErrors = 0, displayErrors = 0;
        // CPU reference uses guest RGB555 bytes at native origins and the
        // analytic expanded rectangle elsewhere, never the capture sampler.
        for (int y = 0; y < 192; ++y)
        for (int x = 0; x < 256; ++x)
        {
            const int lx = x - 17, ly = y - 29;
            const bool covered = lx >= 0 && lx < 64 && ly >= 0 && ly < 64;
            u32 nativeExpected = 0xFF000000;
            if (covered)
            {
                const size_t address = ((63 - ly) * 256 + 95 - lx) * 2;
                const u16 c = captured[address] | (u16(captured[address + 1]) << 8);
                const auto channel = [](u32 v) { return v * 8 + v / 8; };
                if (c & 0x8000) nativeExpected |= channel(c & 31) << 16 |
                    channel((c >> 5) & 31) << 8 | channel((c >> 10) & 31);
            }
            nativeErrors += nativeFrame[y * 256 + x] != nativeExpected;
            for (int sy = 0; sy < 5; ++sy)
            for (int sx = 0; sx < 5; ++sx)
            {
                u32 expected = nativeExpected;
                if (sx || sy)
                {
                    const int sourceX = (95 - lx) * 5 - sx;
                    const int sourceY = (63 - ly) * 5 - sy;
                    // Local source domain is [0,64); capture rectangle is
                    // [31.5,225.5) x [23,169), quantized to the 5x grid.
                    const bool inside = covered && (63 - lx) * 5 >= sx &&
                        (63 - ly) * 5 >= sy;
                    expected = inside && sourceX >= 31 * 5 + 2 && sourceX < 225 * 5 + 2 &&
                        sourceY >= 23 * 5 && sourceY < 169 * 5 ? 0xFFFB0000 : 0xFF000000;
                }
                const u32 observed = display[size_t(y * 5 + sy) * width + x * 5 + sx];
                if (observed != expected)
                {
                    if (!displayErrors) std::fprintf(stderr,
                        "Boundary engine=%u dst=(%d,%d)+(%d,%d) CPU=%08x actual=%08x\n",
                        engine, x, y, sx, sy, expected, observed);
                    ++displayErrors;
                }
            }
        }
        const bool unchanged = std::equal(captured.begin(), captured.end(), nds->GPU.VRAM[bank]);
        std::printf("Boundary 5x engine=%u origin=(17,29) expansion=1 XY-flip: native=49152 errors=%zu display=1228800 errors=%zu guest-unchanged=%d\n",
            engine, nativeErrors, displayErrors, unchanged);
        Require(!nativeErrors && !displayErrors && unchanged, "5x inverted OBJ boundary differs from CPU reference");
        return;
    }
    nds->ARM9Write16(oam, 0x0C00); // Bitmap OBJ at y=0.
    nds->ARM9Write16(oam + 2, 0xC000); // 64x64 at x=0.
    nds->ARM9Write16(oam + 4, 0xF004); // Alpha 16; source x=32, y=0.
    nds->ARM9Write32(reg, 0x00011020); // 2D bitmap OBJ mapping, 256-pixel pitch.
    Screen(*nds, false); Screen(*nds, false);
    DisplayOrigins(*nds, scale);
    Require(std::equal(captured.begin(), captured.end(), nds->GPU.VRAM[bank]),
        "OBJ display modified guest capture VRAM");
    Require(GetCpuDisplayFrame(nds->GetRenderer(), frame),
        "OBJ display framebuffer unavailable");
    const bool selected = engine ? !nds->GPU.ScreenSwap : nds->GPU.ScreenSwap;
    const auto* actual = static_cast<const u32*>(selected ? top : bottom);
    void *nativeTop, *nativeBottom;
    Require(nds->GetRenderer().GetFramebuffers(&nativeTop, &nativeBottom), "native OBJ framebuffer unavailable");
    const auto* native = static_cast<const u32*>(selected ? nativeTop : nativeBottom);
    // Independent RGB555 VRAM -> native OBJ check; no enhanced renderer oracle.
    for (u32 y = 0; y < 64; ++y)
    for (u32 x = 0; x < 64; ++x)
    {
        const u32 address = (y * 256 + x + 32) * 2;
        const u16 pixel = captured[address] | (u16(captured[address + 1]) << 8);
        // Native six-bit expansion replicates the high bits into the low bits.
        const auto channel = [](u32 value) { return value * 8 + value / 8; };
        const u32 expected = !(pixel & 0x8000) ? 0xFF000000 :
            0xFF000000 | (channel(pixel & 31) << 16) |
            (channel((pixel >> 5) & 31) << 8) | channel((pixel >> 10) & 31);
        if (native[y * 256 + x] != expected)
        {
            std::fprintf(stderr, "OBJ native engine=%u src=(%u,%u) bank=%u offset=%05x dst=(%u,%u) rgb555=%04x expected=%08x actual=%08x\n",
                engine, x + 32, y, bank, address, x, y, pixel, expected, native[y * 256 + x]);
            Require(false, "native OBJ differs from source VRAM");
        }
    }
    std::printf("OBJ engine=%u scale=%d native VRAM comparison PASS (4096 pixels)\n", engine, scale);
    // RGB555 red expands through the native six-bit compositor (31 -> 62 -> 251).
    Require(reference[size_t(40 * scale) * width + 48 * scale] == 0xFFFB0000 &&
        actual[size_t(40 * scale) * width + 16 * scale] == 0xFFFB0000,
        "fixture bitmap OBJ did not reach the display");
    if (scale == 3)
    {
        // The known boundary: source texel (32,24) -> OBJ texel (0,24).
        // Read final renderer output, LCDC reference and guest bytes separately.
        const size_t guestOffset = (24 * 256 + 32) * 2;
        const u16 guestPixel = captured[guestOffset] | (u16(captured[guestOffset + 1]) << 8);
        std::printf("OBJ boundary engine=%u source=(32,24) guestByteOffset=%zu RGB555=%04x nativeARGB=%08x\n",
            engine, guestOffset, guestPixel, native[24 * 256]);
        for (int sy = 0; sy < 3; ++sy)
        {
            const size_t row = size_t(72 + sy) * width;
            std::printf("subrow=%d OBJ=[%08x %08x %08x] LCDC=[%08x %08x %08x]\n", sy,
                actual[row], actual[row + 1], actual[row + 2],
                reference[row + 96], reference[row + 97], reference[row + 98]);
        }
    }
    size_t differences = 0;
    for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x)
    {
        const u32 expected = x < 64 * scale && y < 64 * scale
            ? reference[size_t(y) * width + x + 32 * scale] : 0xFF000000;
        if (actual[size_t(y) * width + x] != expected)
        {
            // Diagnostics must also be safe for a mismatch at the frame edge.
            if (!differences) std::fprintf(stderr,
                "OBJ engine=%u scale=%d xy=%d,%d expected=%08x actual=%08x nativeOrigin=%08x\n",
                engine, scale, x, y, expected, actual[size_t(y) * width + x],
                native[(size_t(y) / scale) * 256 + x / scale]);
            ++differences;
        }
    }
    std::printf("OBJ engine=%u scale=%d LCDC pixel differences=%zu\n", engine, scale, differences);
    Require(differences == 0, "bitmap OBJ lost captured subpixel detail");
    // Independent known-color assertion, not another enhanced renderer oracle.
    // This fixture's half-pixel red edge has a transparent native origin at 3x.
    if (scale == 3)
        for (int sy = 0; sy < scale; ++sy)
        {
            const size_t row = size_t(24 * scale + sy) * width;
            Require(actual[row] == 0xFF000000 && actual[row + 1] == 0xFFFB0000 &&
                actual[row + 2] == 0xFFFB0000, "independent red OBJ boundary mismatch");
        }

    unsigned cases = 0;
    const auto checkFrame = [&](int currentScale) {
        Screen(*nds, false); Screen(*nds, false);
        DisplayOrigins(*nds, currentScale);
        Require(std::equal(captured.begin(), captured.end(), nds->GPU.VRAM[bank]),
            "OBJ variant changed guest capture bytes");
        Require(GetCpuDisplayFrame(nds->GetRenderer(), frame),
            "OBJ variant display unavailable");
        Require(nds->GetRenderer().GetFramebuffers(&nativeTop, &nativeBottom),
            "OBJ variant native framebuffer unavailable");
        actual = static_cast<const u32*>(selected ? top : bottom);
        native = static_cast<const u32*>(selected ? nativeTop : nativeBottom);
        ++cases;
    };
    const auto requireNativeDisplay = [&](int currentScale) {
        for (int y = 0; y < height; ++y)
        for (int x = 0; x < width; ++x)
            Require(actual[size_t(y) * width + x] == native[(y / currentScale) * 256 + x / currentScale],
                "unsupported OBJ condition did not retain native display");
    };
    const auto pixel = [&](int x, int y, int subx = 0) {
        return actual[size_t(y * scale) * width + x * scale + subx];
    };

    // Both 1D boundaries on engine A, and 128-pixel 2D pitch, reuse the same
    // guest addresses. Engine B masks out DISPCNT bit 22 (GPU2D::Write32), so
    // its tile number must still select the 128-byte boundary in that case.
    struct Layout { u32 control; u16 tile; u32 pitch; };
    for (const auto layout : {Layout{0x00011040, 0x60, 128},
            Layout{0x00411040, u16(engine ? 0x60 : 0x30), 128}, Layout{0x00011000, 0x60, 256}})
    {
        nds->ARM9Write32(reg, layout.control);
        if (engine) Require(!(nds->ARM9Read32(reg) & (1u << 22)), "engine B accepted the reserved bitmap boundary bit");
        nds->ARM9Write16(oam + 4, 0xF000 | layout.tile);
        checkFrame(scale);
        for (int y = 0; y < 64 * scale; ++y)
        for (int x = 0; x < 64 * scale; ++x)
        {
            const u32 word = (0x3000 + (y / scale) * layout.pitch + (x / scale) * 2) / 2;
            const size_t src = size_t((word / 256) * scale + y % scale) * width +
                (word % 256) * scale + x % scale;
            Require(actual[size_t(y) * width + x] == reference[src], "OBJ bitmap pitch/addressing mismatch");
        }
    }
    nds->ARM9Write32(reg, 0x00011020);
    nds->ARM9Write16(oam + 4, 0x7004); // Alpha 8 over a blue backdrop.
    nds->ARM9Write16(0x05000000 + engine * 0x400, 0x7C00);
    nds->ARM9Write16(reg + 0x50, 0x2000);
    checkFrame(scale);
    Require(pixel(16, 40) == 0xFF7D007D, "bitmap OBJ alpha blend mismatch");
    if (scale == 3)
        Require(pixel(0, 24) == 0xFF0000FB && pixel(0, 24, 1) == 0xFF7D007D,
            "captured OBJ transparency/alpha did not resolve per subpixel");
    nds->ARM9Write16(0x05000000 + engine * 0x400, 0);
    nds->ARM9Write16(reg + 0x50, 0);
    nds->ARM9Write16(oam + 4, 0x0004);
    checkFrame(scale);
    Require(std::all_of(actual, actual + size_t(width) * height, [](u32 c) { return c == 0xFF000000; }),
        "zero-alpha bitmap OBJ became visible");
    nds->ARM9Write16(oam + 4, 0xF004);

    if (scale == 3)
    {
        // A later ordinary OBJ uses existing capture bytes as palette indices.
        // No guest VRAM writes or new capture source are needed for overlap.
        for (u32 index : {1u, 8u, 15u})
            nds->ARM9Write16(0x05000200 + engine * 0x400 + index * 2, 0x7C00);
        nds->ARM9Write16(oam + 8, 24);
        nds->ARM9Write16(oam + 10, 0);
        nds->ARM9Write16(oam + 12, 0x0183);
        checkFrame(scale);
        Require(pixel(0, 24) == 0xFF0000FB && pixel(0, 24, 1) == 0xFFFB0000,
            "native-transparent captured OBJ lost OAM priority or its lower OBJ candidate");
        nds->ARM9Write16(oam + 4, 0xF404);
        checkFrame(scale);
        Require(pixel(0, 24, 1) == 0xFF0000FB, "ordinary OBJ priority did not occlude captured detail");
        nds->ARM9Write16(oam, 24); nds->ARM9Write16(oam + 2, 0); nds->ARM9Write16(oam + 4, 0x0183);
        nds->ARM9Write16(oam + 8, 0x0C00); nds->ARM9Write16(oam + 10, 0xC000); nds->ARM9Write16(oam + 12, 0xF004);
        checkFrame(scale);
        Require(pixel(0, 24, 1) == 0xFF0000FB, "equal-priority OAM ordering changed");
        nds->ARM9Write16(oam + 8, 0x0200);
        nds->ARM9Write16(oam, 0x0C00); nds->ARM9Write16(oam + 2, 0xC000); nds->ARM9Write16(oam + 4, 0xF004);
    }

    // OBJ/BG priority must use the enhanced winner, not the native-origin OBJ.
    const u32 bgBank = engine ? 2 : 0;
    nds->ARM9Write8(0x04000240 + bgBank, 0x80);
    for (u32 offset = 0; offset < 32; offset += 2)
        nds->ARM9Write16(0x06800000 + bgBank * 0x20000 + 0x4000 + offset, 0x1111);
    nds->ARM9Write8(0x04000240 + bgBank, engine ? 0x84 : 0x81);
    nds->ARM9Write16(0x05000002 + engine * 0x400, 0x7C00);
    nds->ARM9Write16(reg + 0x0A, 4); // Solid blue text BG1, priority 0.
    nds->ARM9Write32(reg, 0x00011220);
    checkFrame(scale);
    Require(pixel(16, 40) == 0xFFFB0000, "equal-priority BG occluded captured OBJ");
    if (scale == 3)
        Require(pixel(0, 24) == 0xFF0000FB && pixel(0, 24, 1) == 0xFFFB0000,
            "captured OBJ native-transparent edge lost BG priority");
    nds->ARM9Write16(oam + 4, 0xF404);
    checkFrame(scale);
    Require(pixel(16, 40) == 0xFF0000FB && pixel(0, 24, scale > 1 ? 1 : 0) == 0xFF0000FB,
        "higher-priority BG did not occlude captured OBJ");
    nds->ARM9Write16(oam + 4, 0xF004);
    nds->ARM9Write8(0x04000240 + bgBank, 0);

    nds->ARM9Write16(oam + 8, 0x0818); // Ordinary OBJ-window sprite at (0,24).
    nds->ARM9Write16(oam + 10, 0); nds->ARM9Write16(oam + 12, 0x0183);
    nds->ARM9Write16(reg + 0x4A, 0x2F3F);
    nds->ARM9Write32(reg, 0x00019020);
    checkFrame(scale);
    Require(pixel(0, 24, scale > 1 ? 1 : 0) == 0xFF000000 && pixel(16, 40) == 0xFFFB0000,
        "OBJ-window did not mask captured OBJ detail");
    nds->ARM9Write16(oam + 8, 0x0200);
    nds->ARM9Write16(reg + 0x40, 8);
    nds->ARM9Write16(reg + 0x44, (24 << 8) | 32);
    nds->ARM9Write16(reg + 0x48, 0x2F); // WIN0 hides OBJ.
    nds->ARM9Write16(reg + 0x4A, 0x3F);
    nds->ARM9Write32(reg, 0x00013020);
    checkFrame(scale);
    Require(pixel(4, 28) == 0xFF000000 && pixel(16, 40) == 0xFFFB0000, "OBJ window masking changed");
    nds->ARM9Write32(reg, 0x00011020);

    // Coordinate/color oracle: no LCDC or enhanced sampler is read here.
    // Compute from absolute destination coordinates, then quantize onto the
    // source capture grid. Fixed color probes below also pin the convention.
    struct Transform
    {
        const char* name;
        u16 type = 0x0D00, flips = 0, tile = 4;
        int x = 0, y = 0;
        s16 a = 256, b = 0, c = 0, d = 256;
        u32 control = 0x00011020, base = 64, pitch = 512;
        bool companion = false;
    };
    const auto guestColor = [&](u32 address) {
        Require(address + 1 < captured.size(), "transform oracle source address outside fixture");
        const u16 c = captured[address] | (u16(captured[address + 1]) << 8);
        const auto channel = [](u32 value) { return value * 8 + value / 8; };
        return !(c & 0x8000) ? 0xFF000000u : 0xFF000000u |
            (channel(c & 31) << 16) | (channel((c >> 5) & 31) << 8) | channel((c >> 10) & 31);
    };
    const auto capturedColor = [&](s64 hx, s64 hy) {
        // The fixture's half-pixel X offset is quantized by the rasterizer
        // onto this scale's grid; Y is integral. RGB555 red expands to 251.
        return hx >= 32 * scale + scale / 2 && hx < 224 * scale + scale / 2 &&
            hy >= 24 * scale && hy < 168 * scale ? 0xFFFB0000u : 0xFF000000u;
    };
    unsigned transforms = 0, fixedProbes = 0;
    size_t transformDifferences = 0, fixedFailures = 0;
    for (const Transform t : {
        Transform{.name = "x-flip", .type = 0x0C00, .flips = 0x1000, .tile = 6, .base = 96},
        Transform{.name = "y-flip", .type = 0x0C00, .flips = 0x2000},
        Transform{.name = "xy-flip", .type = 0x0C00, .flips = 0x3000, .tile = 6, .base = 96},
        Transform{.name = "identity"},
        Transform{.name = "fractional", .a = 384, .d = 192},
        Transform{.name = "shear", .b = 128, .c = -64},
        Transform{.name = "quarter-turn", .a = 0, .b = -256, .c = 256, .d = 0},
        Transform{.name = "negative-matrix", .a = -256, .b = 64, .c = -128},
        Transform{.name = "double-half", .type = 0x0F00, .a = 128, .d = 128},
        Transform{.name = "double-clipped", .type = 0x0F00, .x = 237, .y = 174,
            .a = 128, .b = 64, .c = -64, .d = 192},
        Transform{.name = "negative-position", .x = -7, .y = -11,
            .a = 192, .b = -128, .c = 128, .d = 192},
        Transform{.name = "signed-extremes", .a = -32768, .b = 32767, .c = 32767, .d = -32768},
        Transform{.name = "affine-1d-128", .tile = 0x60, .a = 384, .b = 64, .c = -64, .d = 192,
            .control = 0x00011040, .base = 0x3000, .pitch = 128},
        Transform{.name = "affine-1d-boundary", .tile = u16(engine ? 0x60 : 0x30),
            .a = 384, .b = 64, .c = -64, .d = 192,
            .control = 0x00411040, .base = 0x3000, .pitch = 128},
        Transform{.name = "affine-2d-128", .tile = 0x60, .a = 384, .b = 64, .c = -64, .d = 192,
            .control = 0x00011000, .base = 0x3000, .pitch = 256},
        Transform{.name = "partial-provenance", .type = 0x0C00, .flips = 0x1000,
            .tile = 0x286, .base = 0x14060, .companion = true}})
    {
        nds->ARM9Write32(reg, t.control);
        nds->ARM9Write16(oam, t.type | (t.y & 255));
        nds->ARM9Write16(oam + 2, 0xC000 | t.flips | (t.x & 511));
        nds->ARM9Write16(oam + 4, 0xF000 | t.tile);
        nds->ARM9Write16(oam + 6, u16(t.a)); nds->ARM9Write16(oam + 14, u16(t.b));
        nds->ARM9Write16(oam + 22, u16(t.c)); nds->ARM9Write16(oam + 30, u16(t.d));
        // A captured companion makes the display-only OBJ line active even
        // where the first sprite's source is native-only. It does not overlap.
        nds->ARM9Write16(oam + 8, t.companion ? 0x0C00 : 0x0200);
        nds->ARM9Write16(oam + 10, 0xC080); nds->ARM9Write16(oam + 12, 0xF004);
        checkFrame(scale);
        const int bound = (t.type & 0x0200) ? 128 : 64;
        const auto coordinates = [&](s64 dx, s64 dy, s64 units) -> std::array<s64, 2> {
            if (t.type & 0x0100)
            {
                dx -= bound * units / 2; dy -= bound * units / 2;
                return {32 * 256 * units + t.a * dx + t.b * dy,
                        32 * 256 * units + t.c * dx + t.d * dy};
            }
            return {256 * ((t.flips & 0x1000) ? 63 * units - dx : dx),
                    256 * ((t.flips & 0x2000) ? 63 * units - dy : dy)};
        };
        const auto inside = [](const auto& p, s64 units) {
            return p[0] >= 0 && p[1] >= 0 && p[0] < 64 * 256 * units && p[1] < 64 * 256 * units;
        };
        size_t differences = 0;
        for (int ny = 0; ny < 192; ++ny)
        for (int nx = 0; nx < 256; ++nx)
        {
            const int dx = nx - t.x, dy = (ny - t.y) & 255;
            const bool covered = dx >= 0 && dx < bound && dy < bound;
            const bool companion = t.companion && nx >= 128 && nx < 192 && ny < 64;
            const auto origin = coordinates(dx, dy, 1);
            const bool nativeInside = covered && inside(origin, 1);
            const u32 nativeAddress = nativeInside ?
                t.base + u32(origin[1] / 256) * t.pitch + u32(origin[0] / 256) * 2 : 0;
            const u32 nativeExpected = nativeInside ? guestColor(nativeAddress) : companion ?
                guestColor((ny * 256 + nx - 128 + 32) * 2) : 0xFF000000u;
            if (native[ny * 256 + nx] != nativeExpected)
            {
                std::fprintf(stderr, "OBJ %s engine=%u native=(%d,%d) expected=%08x actual=%08x\n",
                    t.name, engine, nx, ny, nativeExpected, native[ny * 256 + nx]);
                Require(false, "transformed native OBJ differs from coordinate/guest-VRAM oracle");
            }
            for (int sy = 0; sy < scale; ++sy)
            for (int sx = 0; sx < scale; ++sx)
            {
                u32 expected = nativeExpected;
                if (sx || sy)
                {
                    if (covered)
                    {
                        const auto p = coordinates(s64(dx) * scale + sx, s64(dy) * scale + sy, scale);
                        if (inside(p, scale))
                        {
                            // First quantize to the high-resolution source grid,
                            // then apply the independent native bitmap layout.
                            const s64 hx = p[0] / 256, hy = p[1] / 256;
                            const u32 address = t.base + u32(hy / scale) * t.pitch + u32(hx / scale) * 2;
                            if (address < 192 * 512)
                                expected = capturedColor((address / 2 % 256) * scale + hx % scale,
                                    (address / 512) * scale + hy % scale);
                            // No valid capture: retain THIS destination's native
                            // result, not a newly transformed guest-VRAM sample.
                        }
                        else if (nativeInside && nativeAddress < 192 * 512)
                            expected = 0xFF000000;
                    }
                    else if (companion)
                        expected = capturedColor((nx - 128 + 32) * scale + sx, ny * scale + sy);
                }
                const u32 observed = actual[size_t(ny * scale + sy) * width + nx * scale + sx];
                if (observed != expected)
                {
                    if (!differences) std::fprintf(stderr,
                        "OBJ %s engine=%u scale=%d dst=(%d,%d)+(%d,%d) expected=%08x actual=%08x\n",
                        t.name, engine, scale, nx, ny, sx, sy, expected, observed);
                    ++differences;
                }
            }
        }
        const auto fixed = [&](int x, int y, int sx, int sy, u32 expected) {
            ++fixedProbes;
            const u32 observed = actual[size_t(y * scale + sy) * width + x * scale + sx];
            if (observed != expected)
            {
                ++fixedFailures;
                std::fprintf(stderr, "OBJ fixed %s (%d,%d)+(%d,%d): expected=%08x actual=%08x\n",
                    t.name, x, y, sx, sy, expected, observed);
            }
        };
        if (scale == 3)
        {
            if (std::strcmp(t.name, "x-flip") == 0)
            { fixed(63, 40, 0, 0, 0xFFFB0000); fixed(63, 40, 1, 0, 0xFF000000); }
            if (std::strcmp(t.name, "y-flip") == 0)
            { fixed(16, 39, 0, 0, 0xFFFB0000); fixed(16, 39, 0, 1, 0xFF000000); }
            if (std::strcmp(t.name, "xy-flip") == 0)
            { fixed(63, 39, 0, 0, 0xFFFB0000); fixed(63, 39, 1, 1, 0xFF000000); }
            if (std::strcmp(t.name, "identity") == 0)
            { fixed(0, 24, 0, 0, 0xFF000000); fixed(0, 24, 1, 0, 0xFFFB0000); }
            if (std::strcmp(t.name, "fractional") == 0)
            { fixed(11, 40, 0, 0, 0xFF000000); fixed(11, 40, 1, 0, 0xFFFB0000); fixed(11, 40, 0, 1, 0xFFFB0000); }
            if (std::strcmp(t.name, "quarter-turn") == 0)
            { fixed(24, 0, 0, 0, 0xFF000000); fixed(24, 0, 0, 1, 0xFFFB0000); }
            if (std::strcmp(t.name, "double-half") == 0)
            { fixed(0, 48, 0, 0, 0xFF000000); fixed(0, 48, 1, 0, 0xFF000000); fixed(0, 48, 2, 0, 0xFFFB0000); }
            if (std::strcmp(t.name, "signed-extremes") == 0)
            { fixed(32, 32, 0, 0, 0xFFFB0000); fixed(32, 32, 1, 0, 0xFF000000); }
            if (t.companion)
            { fixed(63, 32, 0, 0, 0xFF0000FB); fixed(63, 32, 1, 0, 0xFF0000FB);
              fixed(63, 0, 0, 0, 0xFFFB0000); fixed(63, 0, 1, 0, 0xFF000000); }
        }
        std::printf("OBJ transform=%s engine=%u scale=%d native=49152 display=%zu differences=%zu\n",
            t.name, engine, scale, size_t(width) * height, differences);
        transformDifferences += differences;
        ++transforms;
    }
    std::printf("OBJ transforms engine=%u scale=%d cases=%u fixed-probes=%u fixed-failures=%zu differences=%zu\n",
        engine, scale, transforms, fixedProbes, fixedFailures, transformDifferences);
    Require(transformDifferences == 0 && fixedFailures == 0, "captured OBJ transform feature assertions failed");

    nds->ARM9Write16(oam + 8, 0x0200);
    nds->ARM9Write16(oam + 2, 0xC000); nds->ARM9Write16(oam + 4, 0xF004);
    nds->ARM9Write16(oam + 6, 0x100); nds->ARM9Write16(oam + 14, 0);
    nds->ARM9Write16(oam + 22, 0); nds->ARM9Write16(oam + 30, 0x100);
    nds->ARM9Write32(reg, 0x00011020);
    // Fixed source coordinates below encode the latch boundaries, not a
    // second implementation of ApplySpriteMosaicX. Expected colors come from
    // guest RGB555 at native origins and the analytic red rectangle elsewhere.
    // Use the production frame loop: Screen() draws sprites after pre-draw
    // register updates, whereas the hardware pipeline prepares the NEXT line.
    struct MosaicProbe
    {
        int x, y, sourceX = 0, sourceY = 0;
        u32 solid = 0;
        int stepX = 1, stepY = 1;
    };
    unsigned mosaicCases = 0, mosaicProbes = 0;
    size_t mosaicNativeErrors = 0, mosaicDisplayErrors = 0;
    const auto object = [&](u32 index, u16 attr0, u16 attr1, u16 attr2) {
        nds->ARM9Write16(oam + index * 8, attr0);
        nds->ARM9Write16(oam + index * 8 + 2, attr1);
        nds->ARM9Write16(oam + index * 8 + 4, attr2);
    };
    const auto checkMosaic = [&](const char* name, std::initializer_list<MosaicProbe> probes) {
        nds->RunFrame(); nds->RunFrame();
        DisplayOrigins(*nds, scale);
        Require(std::equal(captured.begin(), captured.end(), nds->GPU.VRAM[bank]),
            "mosaic display changed guest capture bytes");
        Require(GetCpuDisplayFrame(nds->GetRenderer(), frame),
            "mosaic display unavailable");
        Require(nds->GetRenderer().GetFramebuffers(&nativeTop, &nativeBottom),
            "mosaic native framebuffer unavailable");
        actual = static_cast<const u32*>(selected ? top : bottom);
        native = static_cast<const u32*>(selected ? nativeTop : nativeBottom);
        size_t nativeErrors = 0, displayErrors = 0;
        for (const auto p : probes)
        {
            const u32 origin = p.solid ? p.solid : guestColor((p.sourceY * 256 + p.sourceX) * 2);
            if (native[p.y * 256 + p.x] != origin)
            {
                if (!nativeErrors) std::fprintf(stderr,
                    "Mosaic %s engine=%u scale=%d native=(%d,%d) expected=%08x actual=%08x\n",
                    name, engine, scale, p.x, p.y, origin, native[p.y * 256 + p.x]);
                ++nativeErrors;
            }
            for (int sy = 0; sy < scale; ++sy)
            for (int sx = 0; sx < scale; ++sx)
            {
                const u32 expected = p.solid || p.sourceY >= 192 || !(sx || sy) ? origin :
                    capturedColor(p.sourceX * scale + p.stepX * sx, p.sourceY * scale + p.stepY * sy);
                const u32 observed = actual[size_t(p.y * scale + sy) * width + p.x * scale + sx];
                if (observed != expected)
                {
                    if (!displayErrors) std::fprintf(stderr,
                        "Mosaic %s engine=%u scale=%d dst=(%d,%d)+(%d,%d) expected=%08x actual=%08x\n",
                        name, engine, scale, p.x, p.y, sx, sy, expected, observed);
                    ++displayErrors;
                }
            }
        }
        // Log all native pixels, not only the probes, for before/after evidence.
        u64 nativeHash = 14695981039346656037ull;
        for (size_t i = 0; i < 256 * 192; ++i)
            for (unsigned shift = 0; shift < 32; shift += 8)
                nativeHash = (nativeHash ^ ((native[i] >> shift) & 255)) * 1099511628211ull;
        std::printf("Mosaic %s engine=%u scale=%d probes=%zu native-errors=%zu display-errors=%zu native-fnv64=%016llx guest-unchanged=1\n",
            name, engine, scale, probes.size(), nativeErrors, displayErrors,
            static_cast<unsigned long long>(nativeHash));
        ++cases; ++mosaicCases;
        mosaicProbes += probes.size();
        mosaicNativeErrors += nativeErrors; mosaicDisplayErrors += displayErrors;
    };
    object(0, 0x1C00, 0xC001, 0xF004); // Mosaic starts between global X latches.
    nds->ARM9Write16(reg + 0x4C, 0x3300);
    checkMosaic("xy-4", {{0,24,0,0,0xFF000000}, {1,23,32,20},
        {1,24,32,24}, {2,25,32,24}, {3,27,32,24}, {4,24,35,24},
        {7,27,35,24}, {8,28,39,28}, {64,24,95,24}, {65,24,0,0,0xFF000000}});
    nds->ARM9Write16(reg + 0x4C, 0x0100);
    checkMosaic("x-2", {{1,24,32,24}, {2,24,33,24}, {3,24,33,24}, {4,24,35,24}});
    nds->ARM9Write16(reg + 0x4C, 0x0F00);
    checkMosaic("x-16", {{1,24,32,24}, {7,24,32,24}, {15,24,32,24},
        {16,24,47,24}, {31,24,47,24}, {32,24,63,24}});

    nds->ARM9Write16(reg + 0x4C, 0x0300);
    object(1, 0x0C00, 0xC051, 0xF004);
    checkMosaic("mixed-mosaic-first", {{1,24,32,24}, {3,24,32,24},
        {81,24,32,24}, {82,24,33,24}, {83,24,34,24}, {84,24,35,24}});
    object(0, 0x0C00, 0xC051, 0xF004);
    object(1, 0x1C00, 0xC001, 0xF004);
    checkMosaic("mixed-mosaic-last", {{1,24,32,24}, {3,24,32,24},
        {81,24,32,24}, {82,24,33,24}, {83,24,34,24}, {84,24,35,24}});

    // The solid 8bpp tile borrows opaque red capture bytes (indices 31/128).
    // A native-only winner must not inherit a losing capture's subpixels.
    nds->ARM9Write16(0x05000200 + engine * 0x400 + 31 * 2, 0x7C00);
    nds->ARM9Write16(0x05000200 + engine * 0x400 + 128 * 2, 0x7C00);
    object(0, 0x1C00, 0xC001, 0xF804); // BG-relative priority 2.
    object(1, 0x3018, 2, 0x0184); // Mosaic palette OBJ, priority 0, x=2..9.
    checkMosaic("priority-latch", {{1,24,32,24}, {2,24,0,0,0xFF0000FB},
        {3,24,0,0,0xFF0000FB}, {8,24,0,0,0xFF0000FB},
        {9,24,0,0,0xFF0000FB}, {10,24,0,0,0xFF0000FB},
        {11,24,0,0,0xFF0000FB}, {12,24,43,24}});
    object(1, 0x2018, 3, 0x0184); // Non-mosaic at x=3..10: both transitions.
    checkMosaic("mosaic-transitions", {{1,24,32,24}, {2,24,32,24},
        {3,24,0,0,0xFF0000FB}, {4,24,0,0,0xFF0000FB},
        {10,24,0,0,0xFF0000FB}, {11,24,42,24}, {12,24,43,24}});
    object(1, 0x1C18, 2, 0xF304); // Uncaptured blue row 192, also priority 0.
    checkMosaic("missing-row-latch", {{1,24,32,24}, {2,24,32,192},
        {3,24,32,192}, {8,24,38,192}, {10,24,38,192},
        {11,24,38,192}, {12,24,43,24}});

    // At x=1 the high-priority capture is natively transparent but has red
    // subpixels. The native palette winner (or later transparent metadata)
    // has NO capture provenance: red must not leak into its latched block.
    object(0, 0x1C00, 0xC001, 0xF004);
    object(1, 0x3018, 1, 0x0584);
    object(2, 0x0C00, 0xC051, 0xF004);
    checkMosaic("native-winner-last", {{1,24,0,0,scale == 1 ? 0xFFFB0000u : 0xFF0000FBu},
        {2,24,33,24}, {81,24,32,24}});
    object(0, 0x3018, 1, 0x0584);
    object(1, 0x1C00, 0xC001, 0xF004);
    checkMosaic("native-winner-first", {{1,24,0,0,scale == 1 ? 0xFFFB0000u : 0xFF0000FBu},
        {2,24,33,24}, {81,24,32,24}});
    object(0, 0x1C00, 0xC001, 0xF004);
    object(1, 0x3018, 1, 0x0400); // Transparent tile 0, priority 1.
    checkMosaic("transparent-winner", {{1,24,0,0,guestColor((24 * 256 + 32) * 2)},
        {2,24,33,24}, {81,24,32,24}});
    object(2, 0x0200, 0, 0);

    // E duplicates only the low AOBJ region: the other region on this mosaic
    // line keeps its detail. I is mirrored over ALL of BOBJ, so engine B must
    // fall back everywhere until that mapping is removed.
    object(0, 0x1C00, 0xC001, 0xF004);
    object(1, 0x0C00, 0xC051, 0xF204); // Source starts at row 128.
    const u32 partialControl = engine ? 0x04000249 : 0x04000244;
    nds->ARM9Write8(partialControl, 0x82);
    const u32 duplicate = (1u << bank) | (engine ? (1u << 8) : (1u << 4));
    Require((engine ? nds->GPU.VRAMMap_BOBJ[0] : nds->GPU.VRAMMap_AOBJ[0]) == duplicate &&
        (engine ? nds->GPU.VRAMMap_BOBJ[4] : nds->GPU.VRAMMap_AOBJ[4]) == (engine ? duplicate : (1u << bank)),
        "mosaic fixture duplicate/mirrored mapping mismatch");
    checkMosaic(engine ? "mirrored-duplicate" : "partial-duplicate",
        {{1,24,0,0,guestColor((24 * 256 + 32) * 2)},
        {3,24,0,0,guestColor((24 * 256 + 32) * 2)}, {4,24,35,24},
        {81,24,32,152,engine ? guestColor((152 * 256 + 32) * 2) : 0u}, {82,24,33,152}});
    nds->ARM9Write8(partialControl, 0);
    checkMosaic("mapping-restored", {{1,24,32,24}, {3,24,32,24}, {81,24,32,152}});
    object(1, 0x0200, 0, 0);

    object(0, 0x1C1A, 0xC001, 0xF064); // y=26, captured source begins at row 24.
    nds->ARM9Write16(reg + 0x4C, 0x3300);
    checkMosaic("y-top-clamp", {{1,25,0,0,0xFF000000}, {1,26,32,24},
        {2,27,32,24}, {1,28,32,26}, {3,31,32,26}, {4,32,35,30}});
    nds->ARM9Write16(reg + 0x4C, 0x3000); // Y mosaic with X width one.
    checkMosaic("y-only", {{1,26,32,24}, {2,27,33,24}, {3,27,34,24},
        {1,28,32,26}, {2,31,33,26}});

    object(0, 0x1C00, 0xC001, 0xF004);
    // At enlarged scales, this window tile has two transparent pixels in its
    // first row, then opaque rows. Its own mosaic bit must affect neither axis.
    object(1, 0x3819, 2, 0x0182); // Window x=2..9, y=25..32.
    nds->ARM9Write16(reg + 0x4C, 0x3300);
    nds->ARM9Write16(reg + 0x4A, 0x2F3F);
    nds->ARM9Write32(reg, 0x00019020);
    checkMosaic("obj-window", {{1,25,32,24},
        {2,25,32,24,scale == 1 ? 0xFF000000u : 0u},
        {4,25,0,0,0xFF000000}, {2,26,0,0,0xFF000000},
        {9,26,0,0,0xFF000000}, {10,26,39,24},
        {2,24,32,24}, {2,32,0,0,0xFF000000}, {2,33,32,32}});
    object(1, 0x0200, 0, 0);
    nds->ARM9Write32(reg, 0x00011020);
    nds->ARM9Write16(reg + 0x4A, 0x3F);

    object(0, 0x1D00, 0xC001, 0xF004); // Affine identity, existing matrix slot 0.
    nds->ARM9Write16(reg + 0x4C, 0x0300);
    checkMosaic("affine-identity", {{1,24,32,24}, {2,24,32,24},
        {3,24,32,24}, {4,24,35,24}});
    object(0, 0x1C00, 0xE001, 0xF004); // Y flip: keep the whole scale-wide block.
    checkMosaic("y-flip-block", {{1,39,32,24,0,1,-1}, {2,39,32,24,0,1,-1},
        {3,39,32,24,0,1,-1}, {4,39,35,24,0,1,-1}});
    object(0, 0x1C00, 0xC0FD, 0xF004); // Clipped at the right screen boundary.
    checkMosaic("right-edge", {{252,24,0,0,0xFF000000},
        {253,24,32,24}, {254,24,32,24}, {255,24,32,24}});
    std::printf("Mosaic summary engine=%u scale=%d cases=%u probes=%u native-errors=%zu display-errors=%zu\n",
        engine, scale, mosaicCases, mosaicProbes, mosaicNativeErrors, mosaicDisplayErrors);
    Require(!mosaicNativeErrors && !mosaicDisplayErrors, "captured OBJ mosaic fixed-coordinate/color assertions failed");

    // Exercise mapping/scale/invalidation fallbacks on the new affine path.
    nds->ARM9Write16(oam + 2, 0xC000); nds->ARM9Write16(oam + 4, 0xF004);
    nds->ARM9Write16(oam, 0x0D00); nds->ARM9Write16(reg + 0x4C, 0);

    const u32 otherControl = engine ? 0x04000249 : 0x04000240;
    nds->ARM9Write8(otherControl, 0x82); // I -> BOBJ, A -> AOBJ: overlapping bank.
    Require((engine ? nds->GPU.VRAMMap_BOBJ[0] : nds->GPU.VRAMMap_AOBJ[0]) ==
        ((1u << bank) | (engine ? (1u << 8) : 1u)), "OBJ fixture did not overlap VRAM banks");
    checkFrame(scale);
    for (int y = 0; y < 32 * scale; ++y)
    for (int x = 0; x < 64 * scale; ++x)
        Require(actual[size_t(y) * width + x] == native[(y / scale) * 256 + x / scale],
            "overlapping OBJ banks used one bank's capture detail");
    nds->ARM9Write8(otherControl, 0);
    checkFrame(scale);
    if (scale > 1) DisplayOrigins(*nds, scale, true);

    if (scale > 1)
    {
        const int different = scale == 2 ? 3 : 2;
        RendererSettings next{different, false, true, false};
        Require(nds->GetRenderer().SetRenderSettings(next), "OBJ scale transition failed");
        checkFrame(different); requireNativeDisplay(different);
        object(0, 0x1D00, 0xC001, 0xF004);
        nds->ARM9Write16(reg + 0x4C, 0x3300);
        checkFrame(different); requireNativeDisplay(different);
        object(0, 0x0D00, 0xC000, 0xF004);
        nds->ARM9Write16(reg + 0x4C, 0);
        next.ScaleFactor = scale;
        Require(nds->GetRenderer().SetRenderSettings(next), "OBJ scale restoration failed");
        checkFrame(scale); DisplayOrigins(*nds, scale, true);
    }
    const u32 alias = engine ? 0x06600000 : 0x06400000;
    const u16 original = nds->ARM9Read16(alias + 0x3040);
    nds->ARM9Write16(alias + 0x3040, original);
    checkFrame(scale); requireNativeDisplay(scale);
    object(0, 0x1D00, 0xC001, 0xF004);
    nds->ARM9Write16(reg + 0x4C, 0x3300);
    checkFrame(scale); requireNativeDisplay(scale);
    std::printf("OBJ engine=%u scale=%d: 131072 guest bytes unchanged; %zu baseline display pixels; %u layout/alpha/priority/window/fallback/invalidation cases PASS\n",
        engine, scale, reference.size(), cases);
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
        Renderer::DisplayFrame frame;
        const auto& [kind, top, bottom, width, height, generation] = frame;
        Require(GetCpuDisplayFrame(nds->GetRenderer(), frame), "benchmark display unavailable");
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
        void *nativeTop, *nativeBottom;
        Renderer::DisplayFrame frame;
        const auto& [kind, top, bottom, width, height, generation] = frame;
        Require(GetCpuDisplayFrame(nds->GetRenderer(), frame), "overlap display unavailable");
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
    Renderer::DisplayFrame frame;
    const auto& [kind, top, bottom, displayWidth, displayHeight, generation] = frame;
    Require(GetCpuDisplayFrame(nds->GetRenderer(), frame), "invalidated capture display unavailable");
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
    Renderer::DisplayFrame before, after;
    Require(GetCpuDisplayFrame(renderer, before), "display view unavailable");
    const size_t count = size_t(before.width) * before.height;
    const auto* topPixels = static_cast<const u32*>(before.top);
    const auto* bottomPixels = static_cast<const u32*>(before.bottom);
    const std::vector<u32> beforeTop(topPixels, topPixels + count);
    const std::vector<u32> beforeBottom(bottomPixels, bottomPixels + count);
    renderer.ClearPipelineCache();
    Require(GetCpuDisplayFrame(renderer, after) &&
        before.width == after.width && before.height == after.height, "cache clear changed display extent");
    Require(std::equal(beforeTop.begin(), beforeTop.end(), static_cast<const u32*>(after.top)) &&
        std::equal(beforeBottom.begin(), beforeBottom.end(), static_cast<const u32*>(after.bottom)),
        "cache clear changed the current display pixels");
    void *top, *bottom;
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

// Integer rectangles and explicit 12.4 UVs make the capture probes independent
// of either renderer's capture sampler. The companion console disables only
// display provenance, retaining identical guest bytes and the native pipeline.
void CaptureTextureQuad(NDS& nds, unsigned index, u32 param, int x, int y,
    int width, int height, int u0, int v0, int u1, int v1)
{
    auto& gpu = nds.GPU.GPU3D;
    if (!index) Scene(nds, 7, false, false);
    auto& polygon = gpu.PolygonRAM[index];
    if (index) polygon = gpu.PolygonRAM[0];
    polygon.TexParam = param;
    polygon.YTop = y; polygon.YBottom = y + height;
    const int positions[][2] = {{x,y}, {x+width,y}, {x+width,y+height}, {x,y+height}};
    const int uv[][2] = {{u0,v0}, {u1,v0}, {u1,v1}, {u0,v1}};
    for (unsigned v = 0; v < 4; ++v)
    {
        auto& vertex = gpu.VertexRAM[index * 4 + v];
        if (index) vertex = gpu.VertexRAM[v];
        polygon.Vertices[v] = &vertex;
        for (unsigned axis = 0; axis < 2; ++axis)
        {
            vertex.FinalPosition[axis] = positions[v][axis];
            vertex.HiresPosition[axis] = positions[v][axis] * 16;
            vertex.TexCoords[axis] = uv[v][axis];
        }
    }
    gpu.RenderPolygonRAM[index] = &polygon;
    gpu.RenderNumPolygons = index + 1;
    gpu.RenderClearAttr1 = 0;
    nds.ARM9Write16(0x05000000, 0);
    nds.ARM9Write32(0x04000000, 0x00010108);
}

void CapturedTexture(int scale)
{
    struct Probe { int x, y, sx, sy; u32 color; };
    unsigned cases = 0, probes = 0;
    size_t nativeErrors = 0, displayErrors = 0, fallbackPixels = 0;
    for (const unsigned captureSize : {3u, 0u})
    {
        const unsigned start = captureSize ? 0 : 2;
        const unsigned textureWidth = captureSize ? 256 : 128;
        const u32 base = start * 32768;
        const u32 param = (7u << 26) | ((captureSize ? 5u : 4u) << 20) | (4u << 23) | (base / 8);
        auto vk = Console(true, scale), reference = Console(true, scale);
        const std::array<NDS*, 2> consoles{vk.get(), reference.get()};
        std::vector<u8> guest;
        for (auto* nds : consoles)
        {
            nds->ARM9Write8(0x04000241, 0x80);
            nds->Start(); nds->RunFrame();
            RendererSettings settings{scale, false, true, false};
            Require(nds->GetRenderer().SetRenderSettings(settings), "capture texture settings failed");
        }
        const auto capture = [&](bool partial = false) {
            for (auto* nds : consoles)
            {
                nds->ARM9Write8(0x04000241, 0x80);
                auto& polygon = Scene(*nds, 0, false, false);
                for (auto* vertex : std::span(polygon.Vertices, polygon.NumVertices))
                {
                    vertex->HiresPosition[0] += 8;
                    vertex->HiresPosition[1] += 8;
                }
                nds->GPU.GPU3D.RenderClearAttr1 = 0;
                nds->GetRenderer().Start3DRendering();
                const u32 control = 0x81010000 | (captureSize << 20) | (start << 18);
                if (!partial)
                {
                    nds->ARM9Write32(0x04000064, control);
                    nds->RunFrame();
                }
                else
                {
                    // Recreate an allocated but only partially produced sidecar.
                    // Actual DrawScanline/DoCapture supplies the first 64 rows;
                    // no test-only injection into renderer storage is involved.
                    nds->GetRenderer().InvalidateDisplayCapture(1, start);
                    nds->GetRenderer().AllocCapture(1, start, captureSize);
                    nds->GPU.CaptureCnt = control;
                    nds->GPU.CaptureEnable = true;
                    for (u32 y = 0; y < 64; ++y)
                    {
                        nds->GPU.VCount = y;
                        nds->GetRenderer().DrawScanline(y);
                    }
                    nds->GPU.CaptureEnable = false;
                }
                Require(!nds->GetRenderer().HasRenderFailure(), "capture texture producer failed");
                nds->ARM9Write8(0x04000241, 0x83);
            }
            reference->GetRenderer().InvalidateDisplayCapture(1, start);
            guest.assign(vk->GPU.VRAM[1], vk->GPU.VRAM[1] + 131072);
            Require(std::equal(guest.begin(), guest.end(), reference->GPU.VRAM[1]),
                "capture texture producer consoles disagree");
            int info[16]; vk->GPU.GetCaptureInfo_Texture(info);
            Require(info[base >> 15] == int(4 + start), "fixture lost texture capture provenance");
        };
        capture();
        const auto quad = [&](u32 texture, int u, int v, unsigned index = 0) {
            for (auto* nds : consoles)
                CaptureTextureQuad(*nds, index, texture, 16 + index * 40, 64, 32, 32, u, v, u, v);
        };
        const auto check = [&](const char* name, std::initializer_list<Probe> expected,
            bool fallback = false, int currentScale = 0) {
            if (!currentScale) currentScale = scale;
            std::array<std::vector<u32>, 2> native, display;
            for (unsigned i = 0; i < consoles.size(); ++i)
            {
                auto& nds = *consoles[i];
                Screen(nds);
                native[i] = Screen(nds, false);
                Renderer::DisplayFrame frame;
                const auto& [kind, top, bottom, width, height, generation] = frame;
                Require(GetCpuDisplayFrame(nds.GetRenderer(), frame) &&
                    width == 256 * currentScale && height == 192 * currentScale, "capture texture display extent");
                const auto* pixels = static_cast<const u32*>(nds.GPU.ScreenSwap ? top : bottom);
                display[i].assign(pixels, pixels + size_t(width) * height);
                Require(std::equal(guest.begin(), guest.end(), nds.GPU.VRAM[1]),
                    "3D display modified source guest capture bytes");
            }
            size_t nativeDiff = 0, displayDiff = 0;
            u64 hash = 14695981039346656037ull;
            for (size_t i = 0; i < native[0].size(); ++i)
            {
                nativeDiff += native[0][i] != native[1][i];
                for (unsigned shift : {0u, 8u, 16u, 24u})
                    hash = (hash ^ ((native[0][i] >> shift) & 255)) * 1099511628211ull;
            }
            if (fallback || currentScale == 1)
            {
                for (size_t i = 0; i < display[0].size(); ++i)
                    displayDiff += display[0][i] != display[1][i];
                fallbackPixels += display[0].size();
            }
            for (const auto& p : expected)
            {
                const u32 actual = display[0][size_t(p.y * currentScale + p.sy) * 256 * currentScale +
                    p.x * currentScale + p.sx];
                if (actual != p.color)
                {
                    if (displayErrors + displayDiff < 20)
                        std::fprintf(stderr, "3D capture %s size=%u scale=%d xy=(%d,%d)+(%d,%d) expected=%08x actual=%08x\n",
                            name, captureSize, currentScale, p.x, p.y, p.sx, p.sy, p.color, actual);
                    ++displayDiff;
                }
                ++probes;
            }
            ++cases; nativeErrors += nativeDiff; displayErrors += displayDiff;
            std::printf("3D capture %s size=%u scale=%d probes=%zu native-errors=%zu display-errors=%zu native-fnv64=%016llx guest-bytes=131072 unchanged=1\n",
                name, captureSize, currentScale, expected.size(), nativeDiff, displayDiff,
                static_cast<unsigned long long>(hash));
        };
        const u32 edgeNative = scale == 1 ? 0xFFFF0000u : 0xFF000000u;
        quad(param, 32 * 16, 40 * 16);
        quad(param, 32 * 16 + 8, 40 * 16, 1);
        quad(param, 40 * 16, 24 * 16, 2);
        quad(param, 40 * 16, 24 * 16 + 8, 3);
        quad(param, 40 * 16, 40 * 16, 4);
        check("fixed-uv-alpha", {{24,72,0,0,edgeNative}, {64,72,0,0,0xFFFF0000},
            {104,72,0,0,edgeNative}, {144,72,0,0,0xFFFF0000}, {184,72,0,0,0xFFFF0000}});

        for (auto* nds : consoles)
            CaptureTextureQuad(*nds, 0, param, 16, 16, 96, 96, 0, 0, 96 * 16, 96 * 16);
        check("interpolated-uv", {{48,64,0,0,edgeNative}, {48,64,scale-1,0,0xFFFF0000},
            {64,40,0,0,edgeNative}, {64,40,0,scale-1,0xFFFF0000}, {80,80,0,0,0xFFFF0000}});

        quad(param + 1, 28 * 16 + 8, 40 * 16); // Four-texel, non-row-aligned offset.
        // The last four texels of a 128x128 capture are outside its provenance.
        check("unaligned-subrange", {{24,72,0,0,captureSize ? 0xFFFF0000u : edgeNative}}, !captureSize);
        const u32 shortParam = (param & ~(7u << 23)) | (3u << 23);
        quad(shortParam + textureWidth * 16 * 2 / 8, 32 * 16 + 8, 24 * 16);
        check("short-height-row-offset", {{24,72,0,0,0xFFFF0000}});

        for (const unsigned wrap : {1u, 2u})
        {
            const int u = wrap == 1 ? int(textureWidth + 32) * 16 + 8 : int(2 * textureWidth - 32) * 16 - 8;
            quad(param | (1u << 16) | (wrap == 2 ? 1u << 18 : 0), u, 40 * 16);
            quad(shortParam | (1u << 17), 40 * 16, (64 + 24) * 16 + 8, 1);
            check(wrap == 1 ? "repeat-uv" : "mirror-u-repeat-v",
                {{24,72,0,0,0xFFFF0000}, {64,72,0,0,0xFFFF0000}});
        }
        quad(param | (1u << 16), (32 - int(textureWidth)) * 16 + 8, 40 * 16);
        check("negative-repeat", {{24,72,0,0,0xFFFF0000}});

        quad((param & ~(7u << 26)) | (4u << 26), 32 * 16 + 8, 40 * 16);
        check("indexed-native-fallback", {}, true);
        quad((param & ~(7u << 23)) | (5u << 23), 32 * 16 + 8, 40 * 16);
        quad(param, 32 * 16 + 8, 40 * 16, 1);
        check("partial-range-and-valid-neighbor", {{24,72,0,0,edgeNative}, {64,72,0,0,0xFFFF0000}});

        for (auto* nds : consoles) nds->ARM9Write8(0x04000240, 0x83); // A OR B, not just B.
        quad(param, 32 * 16 + 8, 40 * 16);
        check("multi-bank-or", {}, true);
        for (auto* nds : consoles) nds->ARM9Write8(0x04000240, 0);
        quad(param, 32 * 16 + 8, 40 * 16);
        check("mapping-restored", {{24,72,0,0,0xFFFF0000}});

        if (scale > 1)
        {
            const int otherScale = 2;
            for (auto* nds : consoles)
            {
                RendererSettings settings{otherScale, false, true, false};
                Require(nds->GetRenderer().SetRenderSettings(settings), "capture texture scale transition failed");
                nds->GPU.GPU3D.RenderFrameIdentical = true;
            }
            check("scale-mismatch", {}, true, otherScale);
            for (auto* nds : consoles)
            {
                RendererSettings settings{scale, false, true, false};
                Require(nds->GetRenderer().SetRenderSettings(settings), "capture texture scale restoration failed");
            }
            check("scale-restored", {{24,72,0,0,0xFFFF0000}});
        }

        // Capture the textured result through the actual scanline/guest path.
        // A fractional UV exposes red only in the enhanced display at 3x/5x.
        for (auto* nds : consoles)
        {
            nds->ARM9Write8(0x04000243, 0x80);
            nds->ARM9Write16(0x04000060, 1);
            nds->GetRenderer().Start3DRendering();
            nds->ARM9Write32(0x04000064, 0x81330000);
            nds->RunFrame();
        }
        Require(std::equal(vk->GPU.VRAM[3], vk->GPU.VRAM[3] + 131072, reference->GPU.VRAM[3]),
            "enhanced 3D texture sampling leaked into guest recapture");
        Require(vk->ARM9Read16(0x06860000 + (72 * 256 + 24) * 2) == (scale == 1 ? 0x801F : 0),
            "guest recapture used a display-only fractional texture sample");
        std::printf("3D capture guest-roundtrip size=%u scale=%d compared-bytes=131072 PASS\n", captureSize, scale);

        capture(true);
        quad(param, 32 * 16 + 8, 40 * 16);
        quad(shortParam, 32 * 16 + 8, 40 * 16, 1);
        check("invalid-row-and-valid-neighbor", {{24,72,0,0,edgeNative}, {64,72,0,0,0xFFFF0000}});
        quad(shortParam, 32 * 16 + 8, 40 * 16);
        check("valid-row-before-invalidation", {{24,72,0,0,0xFFFF0000}});
        vk->GetRenderer().InvalidateDisplayCapture(1, start);
        for (auto* nds : consoles) nds->GPU.GPU3D.RenderFrameIdentical = true;
        check("missing-sidecar-identical-frame", {}, true);
        vk->GetRenderer().AllocCapture(1, start, captureSize);
        check("allocated-but-invalid-rows", {}, true);

        capture();
        quad(param, 32 * 16 + 8, 40 * 16);
        check("before-cpu-invalidation", {{24,72,0,0,0xFFFF0000}});
        for (auto* nds : consoles)
        {
            nds->ARM9Write8(0x04000241, 0x80);
            nds->ARM9Write16(0x06820000 + base, 0); // Same bytes, now CPU provenance.
            nds->ARM9Write8(0x04000241, 0x83);
            nds->GPU.GPU3D.RenderFrameIdentical = true;
        }
        check("cpu-invalidated-identical-frame", {}, true);

        capture();
        // A wrapping texture triggers Texcache::Update's existing SyncAll
        // ordering. It must fall back without disabling its valid neighbor.
        for (auto* nds : consoles) nds->ARM9Write8(0x04000240, 0x9B);
        quad((param & ~0xFFFFu) | 0xFFFFu, 0, 0);
        quad(param, 32 * 16 + 8, 40 * 16, 1);
        check("wrapping-and-valid-neighbor", {{24,72,0,0,0xFF000000}, {64,72,0,0,0xFFFF0000}});
        check("post-sync-stale-sidecar", {}, true);
    }
    std::printf("3D capture summary scale=%d cases=%u probes=%u native-pixels=%zu native-errors=%zu display-errors=%zu fallback-pixels=%zu\n",
        scale, cases, probes, size_t(cases) * 49152, nativeErrors, displayErrors, fallbackPixels);
    Require(!nativeErrors && !displayErrors, "captured 3D texture fixed-coordinate/color or native/fallback assertions failed");
}

// The same ROM-free input is also a bounded before/after submission probe.
// Initialization is excluded; output is checked against the software renderer,
// not against the batching implementation or a timing threshold.
void UploadBatching(int scale, bool enforceBatching)
{
    constexpr unsigned textureCount = 32;
    auto vk = Console(true, scale), soft = Console(false);
    auto& renderer = static_cast<VulkanRenderer&>(vk->GetRenderer());
    const auto writeTextures = [&](NDS& nds, unsigned generation) {
        nds.ARM9Write8(0x04000240, 0x80);
        for (unsigned i = 0; i < textureCount; ++i)
        {
            const u16 color = 0x8000 | ((i + generation) & 31) |
                (((i * 3 + generation) & 31) << 5) | (((i * 7 + generation) & 31) << 10);
            for (unsigned p = 0; p < 64; ++p)
                nds.ARM9Write16(0x06800000 + i * 128 + p * 2, color);
        }
        nds.ARM9Write8(0x04000240, 0x83);
        // Keep the clear bitmap dirty too, so its two images join the textures.
        nds.ARM9Write8(0x04000242, 0x80);
        nds.ARM9Write8(0x04000243, 0x80);
        for (unsigned p = 0; p < 256 * 256; ++p)
        {
            nds.ARM9Write16(0x06840000 + p * 2, 0x8000 | ((generation & 31) << 5));
            nds.ARM9Write16(0x06860000 + p * 2, 0x7FFF);
        }
        nds.ARM9Write8(0x04000242, 0x93);
        nds.ARM9Write8(0x04000243, 0x9B);
    };
    for (auto* nds : {vk.get(), soft.get()})
    {
        writeTextures(*nds, 0);
        for (unsigned i = 0; i < textureCount; ++i)
            CaptureTextureQuad(*nds, i, (7u << 26) | (i * 16),
                16 + (i % 8) * 28, 24 + (i / 8) * 32, 24, 24, 0, 0, 0, 0);
        nds->GPU.GPU3D.RenderDispCnt |= 1u << 14;
    }
    std::printf("Upload probe device=%s scale=%d textures=%u\n",
        renderer.DeviceName().c_str(), scale, textureCount);
    u64 total = 0;
    for (unsigned frame = 0; frame < 20; ++frame)
    {
        // Cold, warm, sixteen changed frames, same-byte remapping, then idle.
        // Remapping still dirties the clear bitmap even when texture hashes match.
        const unsigned generation = frame < 2 ? 0 : std::min(frame - 1, 16u);
        if (frame >= 2 && frame <= 18)
            for (auto* nds : {vk.get(), soft.get()}) writeTextures(*nds, generation);
        for (auto* nds : {vk.get(), soft.get()}) nds->GPU.GPU3D.RenderFrameIdentical = frame >= 2;
        const u64 before = renderer.SubmissionCount();
        Screen(*vk);
        const auto actual = Screen(*vk, false); // Settle the display-enable latch.
        const u64 submits = renderer.SubmissionCount() - before;
        Screen(*soft);
        Require(actual == Screen(*soft, false), "upload batch changed native texture/clear pixels");
        u64 hash = 14695981039346656037ull;
        for (u32 pixel : actual)
            for (unsigned shift : {0u, 8u, 16u, 24u})
                hash = (hash ^ ((pixel >> shift) & 255)) * 1099511628211ull;
        total += submits;
        std::printf("Upload probe frame=%u scale=%d submits=%llu native-fnv64=%016llx pixels=%zu\n",
            frame, scale, static_cast<unsigned long long>(submits),
            static_cast<unsigned long long>(hash), actual.size());
        if (enforceBatching)
        {
            Require(submits <= 1, "texture/clear uploads did not share the render submission");
            if (frame == 1) Require(submits == 1, "warm texture cache submitted redundant uploads");
            if (frame == 18) Require(submits == 1, "same-byte remapping changed clear refresh/cache semantics");
            if (frame == 19) Require(submits == 0, "unchanged cached frame submitted redundant work");
        }
    }
    std::printf("Upload probe summary scale=%d frames=20 submits=%llu native-pixels=%u PASS\n",
        scale, static_cast<unsigned long long>(total), 20u * 256u * 192u);
}

void UploadLifetime()
{
    using Pipeline = Vulkan::ComputePipeline;
    std::string error;
    auto device = Vulkan::Device::Create(error);
    Require(bool(device), error.c_str());
    Pipeline reference(device, Vulkan::EmbeddedShaders()), batched(device, Vulkan::EmbeddedShaders());
    batched.SetUploadBatching(true);
    auto nds = Console(false);
    Scene(*nds, 7, false, false);
    std::array<ComputeData::RenderPolygon, 1> polygons;
    std::array<ComputeData::SpanSetupY, 12> edges;
    std::array<ComputeData::SetupIndices, 192> indices;
    int edgeCount = 0, indexCount = 0;
    ComputeData::PreparePolygon(&nds->GPU.GPU3D.PolygonRAM[0], 0, polygons[0], edges,
        edgeCount, indices, indexCount, 1, false);
    std::array<Pipeline::Variant, 1> variants;
    variants[0].shader = 13;
    Pipeline::Batch batch{polygons, std::span(edges).first(edgeCount),
        std::span(indices).first(indexCount), variants, ComputeData::PrepareMeta(nds->GPU.GPU3D, 1, 1)};
    const auto compare = [&](const auto& expectedTexture, const auto& actualTexture, unsigned layer) {
        polygons[0].TextureLayer = float(layer);
        variants[0].texture = expectedTexture;
        const auto expected = reference.Render(batch);
        variants[0].texture = actualTexture;
        Require(batched.Render(batch) == expected, "deferred staging/layer contents differ from synchronous uploads");
    };
    std::array<u32, 64> pixels;
    const std::array<u32, 4> colors{0x1F00003F, 0x1F003F00, 0x1F3F0000, 0x1F3F3F3F};
    const u64 synchronousBefore = device->SubmissionCount();
    auto expected = reference.CreateTexture(8, 8, 4);
    Require(device->SubmissionCount() == synchronousBefore + 1, "default texture creation stopped being synchronous");
    for (unsigned i = 0; i < colors.size(); ++i)
    {
        pixels.fill(colors[i]);
        reference.UploadTextureLayer(*expected, i % 3, pixels);
    }
    const u64 before = device->SubmissionCount();
    auto actual = batched.CreateTexture(8, 8, 4);
    for (unsigned i = 0; i < colors.size(); ++i)
    {
        pixels.fill(colors[i]);
        batched.UploadTextureLayer(*actual, i % 3, pixels);
    }
    // Grow staging after accepting smaller ranges; immediately release the
    // caller's image and overwrite its input, as a temporary capture can do.
    std::vector<u32> temporaryPixels(64 * 64, 0xFFFFFFFF);
    auto temporary = batched.UploadTexture(64, 64, 1, temporaryPixels, true);
    std::weak_ptr<Vulkan::Device::Image> retained = temporary->image;
    temporary.reset();
    std::fill(temporaryPixels.begin(), temporaryPixels.end(), 0);
    pixels.fill(0);
    Require(!retained.expired(), "queued upload released its destination before the fence");
    Require(device->SubmissionCount() == before, "deferred uploads submitted before their boundary");
    // Another owner may use the shared Device while only upload metadata is queued.
    device->Begin(); device->SubmitAndWait();
    batched.FlushUploads();
    Require(device->SubmissionCount() == before + 2 && retained.expired(),
        "upload flush did not submit once/release completed images");
    batched.FlushUploads();
    Require(device->SubmissionCount() == before + 2, "empty upload flush submitted work");
    for (unsigned layer = 0; layer < 4; ++layer) compare(expected, actual, layer);

    auto expectedLarge = reference.CreateTexture(512, 512, 1);
    auto actualLarge = batched.CreateTexture(512, 512, 1);
    std::vector<u32> large(512 * 512);
    const u64 largeBefore = device->SubmissionCount();
    for (unsigned i = 0; i < 20; ++i)
    {
        std::fill(large.begin(), large.end(), colors[i % colors.size()]);
        batched.UploadTextureLayer(*actualLarge, 0, large);
    }
    batched.FlushUploads();
    const u64 largeSubmits = device->SubmissionCount() - largeBefore;
    Require(largeSubmits <= 4, "20 MiB upload stream reverted to per-image submissions");
    reference.UploadTextureLayer(*expectedLarge, 0, large);
    compare(expectedLarge, actualLarge, 0);

    const u64 manyBefore = device->SubmissionCount();
    for (unsigned i = 0; i < 300; ++i)
    {
        pixels.fill(colors[i % colors.size()]);
        batched.UploadTextureLayer(*actual, 0, pixels);
    }
    batched.SetUploadBatching(false); // Disabling batching must complete pending copies.
    const u64 manySubmits = device->SubmissionCount() - manyBefore;
    Require(manySubmits <= 4, "small upload stream reverted to per-image submissions");
    reference.UploadTextureLayer(*expected, 0, pixels);
    compare(expected, actual, 0);
    std::printf("Upload lifetime: retained images, staging growth, repeated layers, zeroed unused layer, shared device, empty flush PASS; 20MiB-submits=%llu 300-layer-submits=%llu\n",
        static_cast<unsigned long long>(largeSubmits), static_cast<unsigned long long>(manySubmits));
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
            Renderer::DisplayFrame frame;
            const auto& [kind, top, bottom, width, height, generation] = frame;
            Require(GetCpuDisplayFrame(nds->GetRenderer(), frame), "high scale RAM view unavailable");
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
        if (argc == 2 && std::strcmp(argv[1], "upload-lifetime") == 0) { UploadLifetime(); return 0; }
        if (argc == 3 && (std::strcmp(argv[1], "upload-batching") == 0 ||
            std::strcmp(argv[1], "upload-measure") == 0)) {
            const int scale = std::atoi(argv[2]);
            Require(scale == 1 || scale == 3, "invalid upload probe scale");
            UploadBatching(scale, std::strcmp(argv[1], "upload-batching") == 0); return 0;
        }
        if (argc == 3 && std::strcmp(argv[1], "captured-texture") == 0) {
            const int scale = std::atoi(argv[2]);
            Require(scale == 1 || scale == 3 || scale == 5, "invalid capture texture scale");
            CapturedTexture(scale); return 0;
        }
        if (argc == 2 && std::strcmp(argv[1], "captured-obj-boundary") == 0) {
            CapturedBitmapOBJ(5, 0, true); CapturedBitmapOBJ(5, 1, true); return 0;
        }
        if (argc == 4 && std::strcmp(argv[1], "captured-bitmap-obj") == 0) {
            const int scale = std::atoi(argv[2]), engine = std::atoi(argv[3]);
            Require(scale >= 1 && scale <= 16 && engine >= 0 && engine <= 1, "invalid OBJ fixture arguments");
            CapturedBitmapOBJ(scale, engine); return 0;
        }
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
