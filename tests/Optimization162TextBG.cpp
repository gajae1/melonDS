// SPDX-License-Identifier: GPL-3.0-or-later
// Compare binary output from the real software text-BG renderer before/after
// the tile-span change. Build and benchmark instructions are private in
// local-docs/optimization-162-20260923/text-bg/.
#include "NDS.h"
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>

#define private public
#include "GPU_Soft.h"
#undef private

using namespace melonDS;

namespace
{
void Require(bool valid, const char* message)
{
    if (!valid) throw std::runtime_error(message);
}

u32 Random(u32& seed)
{
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}

template<size_t N>
void Fill(u8 (&bytes)[N], u32& seed)
{
    for (auto& byte : bytes) byte = u8(Random(seed));
}

void Initialize(GPU& gpu)
{
    u32 seed = 0x162B610Du;
    Fill(gpu.Palette, seed);
    Fill(gpu.VRAMFlat_ABG, seed);
    Fill(gpu.VRAMFlat_BBG, seed);
    Fill(gpu.VRAMFlat_ABGExtPal, seed);
    Fill(gpu.VRAMFlat_BBGExtPal, seed);
}

struct Fixture
{
    std::unique_ptr<NDS> nds;
    SoftRenderer parent;
    SoftRenderer2D a, b;

    Fixture() : nds([] {
        NDSArgs args;
        args.JIT = std::nullopt;
        return std::make_unique<NDS>(std::move(args));
    }()), parent(*nds), a(nds->GPU.GPU2D_A, parent), b(nds->GPU.GPU2D_B, parent)
    {
        nds->Reset();
        Initialize(nds->GPU);
        a.Reset(); b.Reset();
    }
};

void Configure(Fixture& fixture, unsigned engine, unsigned bg, bool depth8,
               bool extpal, bool mosaic, unsigned size, u16 x, u16 y,
               bool masked, unsigned variant)
{
    auto& gpu = engine ? fixture.nds->GPU.GPU2D_B : fixture.nds->GPU.GPU2D_A;
    auto& renderer = engine ? fixture.b : fixture.a;
    // Vary all four screen sizes, tile-map bases, both A-only DISPCNT bases,
    // the BG0/BG1 extended-palette slot bit, and vertical mosaic source.
    gpu.BGCnt[bg] = u16(((variant * 7) & 31) << 8) | u16(size << 14)
        | u16(((variant >> 1) & 3) << 2) | u16(depth8 ? 0x80 : 0)
        | u16(mosaic ? 0x40 : 0) | u16((variant & 1) ? 0x2000 : 0);
    gpu.DispCnt = (extpal ? (1u << 30) : 0)
        | ((variant & 7) << 24) | (((variant >> 2) & 7) << 27);
    gpu.BGXPos[bg] = x;
    gpu.BGYPos[bg] = y;
    gpu.BGMosaicLine = (variant & 1) ? 191 : 0;
    gpu.BGMosaicSize[0] = mosaic ? ((variant & 1) ? 3 : 15) : 0;
    renderer.CurBGXMosaicTable = renderer.MosaicTable[gpu.BGMosaicSize[0]].data();
    renderer.CaptureLayersActive = false;
    for (unsigned i = 0; i < 256; ++i)
    {
        renderer.WindowMask[i] = masked && ((i + variant) % 7 < 3)
            ? u8(0xFF & ~(1u << bg)) : 0xFF;
        renderer.BGOBJLine[i] = 0x20000000 | ((i * 17 + variant) & 0x3F);
        renderer.BGOBJLine[256 + i] = 0x40000000 | ((i * 31 + variant) & 0x3F);
    }
}

void Draw(Fixture& fixture, unsigned engine, unsigned bg, bool mosaic, u32 line)
{
    auto& renderer = engine ? fixture.b : fixture.a;
    if (mosaic) renderer.DrawBG_Text<true>(line, bg);
    else renderer.DrawBG_Text<false>(line, bg);
}

void Correctness(Fixture& fixture, const char* path)
{
    std::ofstream out(path, std::ios::binary);
    Require(bool(out), "cannot open output");
    const u32 magic = 0x162B6001;
    out.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    const u16 xs[] = {0, 1, 7, 8, 251, 65535};
    const u16 ys[] = {0, 7, 255, 65535};
    unsigned cases = 0;
    std::array<u32, 256> composite;
    for (unsigned engine = 0; engine < 2; ++engine)
    for (unsigned bg = 0; bg < 4; ++bg)
    for (unsigned depth = 0; depth < 2; ++depth)
    for (unsigned extpal = 0; extpal < 2; ++extpal)
    for (unsigned size = 0; size < 4; ++size)
    for (unsigned xi = 0; xi < 6; ++xi)
    for (unsigned yi = 0; yi < 4; ++yi)
    for (unsigned mosaic = 0; mosaic < 2; ++mosaic)
    {
        const unsigned variant = cases + xi * 3 + yi * 5 + bg * 11;
        const bool masked = (variant & 2) != 0;
        Configure(fixture, engine, bg, depth, extpal, mosaic, size,
                  xs[xi], ys[yi], masked, variant);
        Draw(fixture, engine, bg, mosaic, (variant & 1) ? 191 : 0);
        auto& renderer = engine ? fixture.b : fixture.a;
        renderer.ColorComposite<0>(composite.data());
        out.write(reinterpret_cast<const char*>(&renderer.BGOBJLine),
                  sizeof(renderer.BGOBJLine));
        out.write(reinterpret_cast<const char*>(composite.data()),
                  sizeof(composite));
        ++cases;
    }
    Require(bool(out), "output write failed");
    std::printf("check cases=%u engines=2 BGs=4 depths=4/8 extpal=on/off flips=randomized map wrapping=x/y/VRAM mosaic=on/off bytes=%llu\n",
                cases, static_cast<unsigned long long>(out.tellp()));
}

void Benchmark(Fixture& fixture)
{
    struct Workload { const char* name; bool depth8, extpal, mosaic, masked; };
    constexpr Workload loads[] = {
        {"text-4bpp", false, false, false, false},
        {"text-8bpp", true, false, false, false},
        {"text-8bpp-ext", true, true, false, false},
        {"mosaic-4bpp-control", false, false, true, false},
        {"mosaic-8bpp-control", true, true, true, false},
        {"window-off-8bpp-control", true, false, false, true},
    };
    constexpr unsigned calls = 5000;
    u32 checksum = 0;
    std::puts("engine,workload,trial,calls,elapsed_ns,checksum");
    for (unsigned engine = 0; engine < 2; ++engine)
    for (const auto& load : loads)
    for (unsigned trial = 0; trial < 5; ++trial)
    {
        Configure(fixture, engine, 1, load.depth8, load.extpal, load.mosaic,
                  3, 251, 255, load.masked, 0x25);
        auto& renderer = engine ? fixture.b : fixture.a;
        if (load.masked) std::memset(renderer.WindowMask, 0, sizeof(renderer.WindowMask));
        Draw(fixture, engine, 1, load.mosaic, 191); // untimed warmup
        const auto start = std::chrono::steady_clock::now();
        for (unsigned n = 0; n < calls; ++n)
            Draw(fixture, engine, 1, load.mosaic, 191);
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - start).count();
        checksum += renderer.BGOBJLine[(trial * 53) & 255];
        std::printf("%u,%s,%u,%u,%lld,%08x\n", engine, load.name, trial,
                    calls, static_cast<long long>(ns), checksum);
    }
}
}

int main(int argc, char** argv)
{
    try
    {
        Require(argc >= 2, "usage: text-bg check output.bin | bench");
        Fixture fixture;
        if (std::strcmp(argv[1], "check") == 0)
        {
            Require(argc == 3, "check requires output path");
            Correctness(fixture, argv[2]);
        }
        else if (std::strcmp(argv[1], "bench") == 0)
        {
            Require(argc == 2, "bench takes no output path");
            Benchmark(fixture);
        }
        else throw std::runtime_error("unknown command");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
