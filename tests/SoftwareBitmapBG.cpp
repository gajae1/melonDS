// SPDX-License-Identifier: GPL-3.0-or-later
// Compare complete native and capture-layer output from the real software
// direct-color bitmap BG renderer against a fixed baseline hash.
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

void Configure(Fixture& f, unsigned engine, unsigned bg, unsigned size, int x, int y,
               unsigned variant, bool capture, bool mosaic)
{
    auto& g = engine ? f.nds->GPU.GPU2D_B : f.nds->GPU.GPU2D_A;
    auto& r = engine ? f.b : f.a;
    g.BGCnt[bg] = 0x84 | (size << 14) | ((variant & 31) << 8) |
                  ((variant & 32) ? 0x2000 : 0) | (mosaic ? 0x40 : 0);
    g.BGRotA[bg - 2] = 0x100;
    g.BGRotB[bg - 2] = 0;
    g.BGRotC[bg - 2] = 0;
    g.BGRotD[bg - 2] = 0x100;
    g.BGXRefInternal[bg - 2] = x;
    g.BGYRefInternal[bg - 2] = y;
    r.CaptureLayersActive = capture;
    r.DisplayLayers = {};
    r.BitmapLines = {};
    r.CurBGXMosaicTable = r.MosaicTable[7].data();
    for (unsigned i = 0; i < 256; ++i)
    {
        r.WindowMask[i] = (variant % 3 == 0) ? 255 : (variant % 3 == 1) ? 0 : (i % 3 == 0 ? 0 : 255);
        r.BGOBJLine[i] = 0x20000000 + i * 13;
        r.BGOBJLine[i + 256] = 0x40000000 + i * 17;
    }
}

void Draw(Fixture& f, unsigned engine, unsigned bg, bool mosaic)
{
    auto& r = engine ? f.b : f.a;
    if (mosaic) r.DrawBG_Extended<true>(0, bg);
    else r.DrawBG_Extended<false>(0, bg);
}

void Check(Fixture& f, const char* path)
{
    std::ofstream out(path, std::ios::binary);
    Require(bool(out), "output");
    unsigned cases = 0;
    for (unsigned e = 0; e < 2; ++e)
    for (unsigned bg = 2; bg < 4; ++bg)
    for (unsigned size = 0; size < 4; ++size)
    for (int x : {-65536, -255, -1, 0, 255, 32512, 65280, 130816, 131072})
    for (unsigned v = 0; v < 6; ++v)
    for (unsigned control = 0; control < 3; ++control)
    {
        const unsigned variant = v * 13;
        const bool capture = control == 1, mosaic = control == 2;
        Configure(f, e, bg, size, x, (v == 5 ? -1 : 127 * 256 + 19), variant, capture, mosaic);
        Draw(f, e, bg, mosaic);
        auto& r = e ? f.b : f.a;
        out.write((const char*)r.BGOBJLine, sizeof(r.BGOBJLine));
        out.write((const char*)r.DisplayLayers.data(), sizeof(r.DisplayLayers));
        ++cases;
    }
    Require(bool(out), "write");
    std::printf("cases=%u bytes=%llu\n", cases, (unsigned long long)out.tellp());
}

void Bench(Fixture& f)
{
    using Clock = std::chrono::steady_clock;
    std::puts("engine,mode,trial,ns,checksum");
    for (unsigned e = 0; e < 2; ++e)
    for (unsigned mode = 0; mode < 4; ++mode)
    {
        Configure(f, e, 2, 1, 0, 0, mode == 2 ? 2 : mode == 3 ? 1 : 0, false, false);
        auto& r = e ? f.b : f.a;
        auto& g = e ? f.nds->GPU.GPU2D_B : f.nds->GPU.GPU2D_A;
        u8* vram;
        u32 mask;
        g.GetBGVRAM(vram, mask);
        for (u32 i = 0; i <= mask / 2; ++i)
        {
            u16 c = u16(i * 137 + 12345) | (mode == 0 ? 0x8000 : 0);
            std::memcpy(vram + 2 * i, &c, 2);
        }
        for (unsigned trial = 0; trial < 6; ++trial)
        {
            auto start = Clock::now();
            for (unsigned n = 0; n < 12000; ++n)
            {
                g.BGXRefInternal[0] = (n & 255) << 8;
                g.BGYRefInternal[0] = ((n >> 2) & 255) << 8;
                Draw(f, e, 2, false);
            }
            auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count();
            if (trial) std::printf("%u,%u,%u,%lld,%u\n", e, mode, trial, (long long)ns, r.BGOBJLine[123]);
        }
    }
}
}

int main(int argc, char** argv)
{
    try
    {
        Fixture f;
        if (argc == 3 && std::string(argv[1]) == "check") Check(f, argv[2]);
        else if (argc == 2 && std::string(argv[1]) == "bench") Bench(f);
        else return 2;
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "%s\n", e.what());
        return 1;
    }
}

// The renderer template members are private to the software renderer translation
// unit, so the test instantiates them from the same definitions.
#include "GPU2D_Soft.cpp"
