// SPDX-License-Identifier: GPL-3.0-or-later
// Golden output captured from the scalar renderer at ae8f0e7f. Exercise the
// real pixel shader, including palette wrap, transparency and scalar formats.
#include "NDS.h"
#include <cstdio>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <utility>

#define private public
#include "GPU_Soft.h"
#undef private

using namespace melonDS;

namespace
{
struct Fixture
{
    std::unique_ptr<NDS> Nds;
    std::unique_ptr<SoftRenderer> Parent;
    std::unique_ptr<SoftRenderer3D> Renderer;

    Fixture()
    {
        NDSArgs args;
        args.JIT = std::nullopt;
        Nds = std::make_unique<NDS>(std::move(args));
        Nds->Reset();
        Parent = std::make_unique<SoftRenderer>(*Nds);
        Renderer = std::make_unique<SoftRenderer3D>(Nds->GPU.GPU3D, *Parent);
        u32 seed = 229;
        auto next = [&]() {
            seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
            return seed;
        };
        for (auto& value : Nds->GPU.VRAMFlat_Texture) value = u8(next());
        for (auto& value : Nds->GPU.VRAMFlat_TexPal) value = u8(next());
        for (auto& value : Nds->GPU.GPU3D.RenderToonTable) value = u16(next());
    }

    void BeginFrame()
    {
        Renderer->ClearBuffers();
        Renderer->RenderPolygons(false, nullptr, 0);
    }

    void Map()
    {
        auto& gpu = Nds->GPU;
        for (u32 i = 0; i < sizeof(gpu.VRAM_A); ++i) gpu.VRAM_A[i] = u8(i * 137 + 29);
        for (u32 i = 0; i < sizeof(gpu.VRAM_E); ++i) gpu.VRAM_E[i] = u8(i * 71 + 93);
        Nds->ARM9Write8(0x04000240, 0x83);
        Nds->ARM9Write8(0x04000244, 0x83);
    }

    void Sync()
    {
        auto& gpu = Nds->GPU;
        auto texture = gpu.VRAMDirty_Texture.DeriveState(gpu.VRAMMap_Texture, gpu);
        auto palette = gpu.VRAMDirty_TexPal.DeriveState(gpu.VRAMMap_TexPal, gpu);
        gpu.MakeVRAMFlat_TextureCoherent(texture);
        gpu.MakeVRAMFlat_TexPalCoherent(palette);
    }
};

u32 Param(u32 format, u32 widthLog, u32 heightLog, u32 base = 0)
{
    return (format << 26) | (widthLog << 20) | (heightLog << 23) | ((base >> 3) & 0xFFFF);
}

SoftRenderer3D::TextureInfo Info(u32 param, u32 palette)
{
    return {param, palette, (param & 0xFFFF) << 3,
        8 << ((param >> 20) & 7), 8 << ((param >> 23) & 7),
        (param >> 26) & 7, u8(param & (1u << 29) ? 0 : 31)};
}

u32 Shade(Fixture& f, const SoftRenderer3D::TextureInfo& texture,
    u32 k, u32 blend, s16 s, s16 t)
{
    SoftRenderer3D::PolygonPixelState state {&texture, blend,
        k % 3 == 0 ? 0u : k % 3 == 1 ? 7u : 31u, blend == 2 && bool(k & 1)};
    return f.Renderer->RenderPixel(state, k & 63, (k * 17) & 63, (k * 31) & 63, s, t);
}

void Write(std::ofstream& out, u32 pixel)
{
    const char bytes[] = {char(pixel), char(pixel >> 8), char(pixel >> 16), char(pixel >> 24)};
    out.write(bytes, sizeof(bytes));
}

void CheckPixels(std::ofstream& out)
{
    Fixture f;
    for (u32 format = 1; format <= 7; ++format)
    for (auto dims : {std::pair{0u, 0u}, {2u, 2u}, {5u, 5u}, {7u, 7u}, {7u, 0u}, {0u, 7u}})
    {
        if (format != 3 && format != 4 && dims != std::pair{2u, 2u}) continue;
        for (u32 base : {0u, 0x7FFF8u})
        for (u32 palette : {0u, 0x1FFFu})
        for (u32 alpha : {0u, 1u})
        {
            f.BeginFrame();
            const u32 param = Param(format, dims.first, dims.second, base) | (alpha << 29);
            for (u32 flags = 0; flags < 16; ++flags)
            {
                const auto texture = Info(param | (flags << 16), palette);
                f.Renderer->PrepareTexturePalette(texture.Param, palette);
                for (u32 blend = 0; blend < 4; ++blend)
                for (u32 k = 0; k < 64; ++k)
                    Write(out, Shade(f, texture, k, blend, s16(k * 1049 - 32768), s16(k * 731 - 24000)));
            }
        }
    }
}

void CheckCoherence(std::ofstream& out)
{
    for (u32 format : {3u, 4u})
    {
        Fixture f;
        f.Map();
        const u32 param = Param(format, 5, 5);
        const auto texture = Info(param, 0);
        for (u32 stage = 0; stage < 7; ++stage)
        {
            if (stage == 2)
            {
                f.Nds->ARM9Write8(0x04000244, 0x80);
                f.Nds->ARM9Write16(0x06880002, 0x1234);
                f.Nds->ARM9Write8(0x04000244, 0x83);
            }
            if (stage == 3)
            {
                f.Nds->ARM9Write8(0x04000240, 0x80);
                f.Nds->ARM9Write16(0x06800000, 0xABAB);
                f.Nds->ARM9Write8(0x04000240, 0x83);
            }
            if (stage == 4) f.Nds->ARM9Write8(0x04000240, 0x80);
            if (stage == 5) f.Nds->ARM9Write8(0x04000240, 0x83);
            f.Sync();
            f.BeginFrame();
            f.Renderer->PrepareTexturePalette(param, 0);
            for (u32 k = 0; k < 256; ++k)
                Write(out, Shade(f, texture, k, 0, s16(k * 16), 0));
        }
    }
}
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    try
    {
        std::ofstream out(argv[1], std::ios::binary);
        if (!out) throw std::runtime_error("cannot open output");
        CheckPixels(out);
        CheckCoherence(out);
        out.close();
        if (!out) throw std::runtime_error("output write failed");
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
