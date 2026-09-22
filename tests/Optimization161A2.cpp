// SPDX-License-Identifier: GPL-3.0-or-later
// CPU-only differential fixture. Build/run instructions live in the A2 evidence directory.
#include "NDS.h"
#include "GPU3D_Texcache.h"
// Instantiate all output formats from the production decoder for this CPU fixture.
#include "../src/GPU3D_Texcache.cpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

using namespace melonDS;
namespace
{
void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

u32 Random(u32& seed)
{
    seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
    return seed;
}

struct Output
{
    std::ofstream File;
    explicit Output(const char* path) : File(path, std::ios::binary) {}
    void Bytes(const void* data, size_t size)
    {
        File.write(static_cast<const char*>(data), size);
        Require(bool(File), "output write failed");
    }
    void Word(u32 value) { Bytes(&value, sizeof(value)); }
    void Pixels(const std::vector<u32>& data) { Bytes(data.data(), data.size() * sizeof(u32)); }
};

struct InputState
{
    std::vector<u8> Texture, Palette;
    explicit InputState(GPU& gpu)
        : Texture(std::begin(gpu.VRAMFlat_Texture), std::end(gpu.VRAMFlat_Texture)),
          Palette(std::begin(gpu.VRAMFlat_TexPal), std::end(gpu.VRAMFlat_TexPal)) {}
    void Check(GPU& gpu) const
    {
        Require(!std::memcmp(Texture.data(), gpu.VRAMFlat_Texture, Texture.size()), "texture RAM changed");
        Require(!std::memcmp(Palette.data(), gpu.VRAMFlat_TexPal, Palette.size()), "palette RAM changed");
    }
};

void Initialize(GPU& gpu)
{
    u32 seed = 0x161A2;
    for (auto& byte : gpu.VRAMFlat_Texture) byte = u8(Random(seed));
    for (auto& byte : gpu.VRAMFlat_TexPal) byte = u8(Random(seed));
    // Exercise zero, low nonzero, rounding boundaries, all channel extremes,
    // and both stored high-bit values. Other palette entries remain random.
    const u16 edge[] = {0, 0x8000, 1, 0x20, 0x400, 0x7FFF, 0xFFFF, 0x4210,
                       0x001F, 0x03E0, 0x7C00, 0x7BDE, 0x1234, 0x9234, 0x0421, 0x8421};
    std::memcpy(gpu.VRAMFlat_TexPal, edge, sizeof(edge));
    std::memcpy(gpu.VRAMFlat_TexPal + 0x1FFE0, edge, sizeof(edge));
}

void Blocks(GPU& gpu, u32 size, u32 addr, u32 aux, int mode, int pattern, bool wrap)
{
    u32 seed = 0xCAB161;
    for (u32 b = 0; b < size * size / 16; ++b)
    {
        u32 data = 0;
        for (u32 p = 0; p < 16; ++p)
        {
            // Four rotations put every color index at every pixel position;
            // four constant blocks and one random pattern cover low entropy.
            u32 index = pattern < 4 ? (p + pattern) & 3 :
                        pattern < 8 ? pattern - 4 : Random(seed) & 3;
            data |= index << (2 * p);
        }
        const u32 offset = wrap && b % 3 == 0 ? 0x3FFF :
                           b % 3 == 1 ? (b % 4) : Random(seed) & 0x3FFF;
        const u16 control = u16(((mode < 0 ? b % 4 : mode) << 14) | offset);
        std::memcpy(gpu.VRAMFlat_Texture + ((addr + b * 4) & 0x7FFFF), &data, 4);
        std::memcpy(gpu.VRAMFlat_Texture + ((aux + b * 2) & 0x7FFFF), &control, 2);
    }
}

void Decode(int format, u32 size, u32* out, u32 addr, u32 aux, u32 pal, GPU& gpu)
{
    switch (format)
    {
    case outputFmt_RGB6A5: ConvertCompressedTexture<outputFmt_RGB6A5>(size, size, out, addr, aux, pal, gpu); break;
    case outputFmt_RGBA8: ConvertCompressedTexture<outputFmt_RGBA8>(size, size, out, addr, aux, pal, gpu); break;
    case outputFmt_BGRA8: ConvertCompressedTexture<outputFmt_BGRA8>(size, size, out, addr, aux, pal, gpu); break;
    default: throw std::runtime_error("invalid output format");
    }
}

struct Uploads
{
    u32 Count = 0, Arrays = 0, Deletes = 0;
    size_t Position = 0;
    std::vector<u32> Pixels;
};
struct Loader
{
    Uploads& Result;
    u32 GenerateTexture(u32, u32, u32) { return ++Result.Arrays; }
    void UploadTexture(u32, u32 width, u32 height, u32, const u32* pixels)
    {
        Require(Result.Position + width * height <= Result.Pixels.size(), "unexpected extra upload");
        std::memcpy(Result.Pixels.data() + Result.Position, pixels, width * height * sizeof(u32));
        Result.Position += width * height;
        ++Result.Count;
    }
    void DeleteTexture(u32) { ++Result.Deletes; }
};
using Cache = Texcache<Loader, u32>;

u32 Parameter(u32 size)
{
    u32 log = 0;
    for (u32 s = size; s > 8; s >>= 1) ++log;
    return (5u << 26) | (log << 20) | (log << 23);
}

void Correctness(GPU& gpu, Output& out)
{
    unsigned cases = 0;
    for (u32 size : {8u, 32u, 256u})
    for (int mode = 0; mode < 4; ++mode)
    for (bool wrap : {false, true})
    for (int pattern = 0; pattern < 9; ++pattern)
    {
        const u32 addr = wrap ? 0x7FFF8 : 0;
        const u32 aux = wrap ? 0x5FFF8 : 0x20000;
        const u32 pal = wrap ? 0x1FFF0 : 0;
        Initialize(gpu);
        Blocks(gpu, size, addr, aux, mode, pattern, wrap);
        InputState state(gpu);
        for (int format = 0; format < 3; ++format)
        {
            constexpr u32 Guard = 0xDEADBEEF;
            std::vector<u32> pixels(size * size + 2, Guard);
            Decode(format, size, pixels.data() + 1, addr, aux, pal, gpu);
            Require(pixels.front() == Guard && pixels.back() == Guard, "decoder output overrun");
            for (u32 p = 1; p <= size * size; ++p)
            {
                const u32 alpha = pixels[p] >> 24;
                Require(alpha == 0 || alpha == (format == 0 ? 31u : 255u), "invalid alpha");
                if (alpha == 0) Require(pixels[p] == 0, "transparent entry has RGB bits");
                if (pattern == 7 && mode < 2) Require(pixels[p] == 0, "transparent index 3 not zero");
                if (mode >= 2) Require(alpha != 0, "opaque mode became transparent");
            }
            out.Word(size); out.Word(mode); out.Word(wrap); out.Word(pattern); out.Word(format);
            out.Pixels(pixels);
            ++cases;
        }
        state.Check(gpu);
    }

    // Actual product cache misses, warm reuse, palette mutation after reset,
    // full upload payloads, handles/layers and mutable helper state.
    for (u32 size : {8u, 256u})
    {
        Initialize(gpu);
        Blocks(gpu, size, 0, 0x20000, -1, 0, true);
        Uploads uploads;
        uploads.Pixels.resize(size * size * 16);
        auto cache = std::make_unique<Cache>(gpu, Loader{uploads});
        for (u32 epoch = 0; epoch < 2; ++epoch)
        {
            if (epoch) gpu.VRAMFlat_TexPal[0] ^= 0x1F;
            InputState state(gpu);
            for (u32 pass = 0; pass < 2; ++pass)
            for (u32 key = 0; key < 8; ++key)
            {
                u32 handle, layer, *helper;
                cache->GetTexture(Parameter(size), key, handle, layer, helper);
                Require(*helper == (pass ? 0xA200 + key : 0), "cache helper state mismatch");
                out.Word(handle); out.Word(layer); out.Word(*helper);
                *helper = 0xA200 + key;
                Require(uploads.Count == epoch * 8 + (pass ? 8 : key + 1), "cache miss/hit count mismatch");
            }
            state.Check(gpu);
            cache->Reset();
        }
        out.Word(uploads.Count); out.Word(uploads.Arrays); out.Word(uploads.Deletes);
        out.Pixels(uploads.Pixels);
    }
    std::printf("PASS decoder_cases=%u cache_sizes=8,256 cache_misses=32 cache_hits=32 input_state_unchanged=1\n", cases);
}

template<class Function>
long long Elapsed(Function function)
{
    const auto start = std::chrono::steady_clock::now();
    function();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - start).count();
}

void Row(const std::string& name, u32 size, u32 calls, u32 decodes, long long ns, u32 uploads)
{
    std::printf("%s,%u,%u,%u,%llu,%lld,%u\n", name.c_str(), size, calls, decodes,
                static_cast<unsigned long long>(decodes) * size * size, ns, uploads);
}

void Benchmark(GPU& gpu, Output& out)
{
    std::puts("case,size,calls,decodes,decoded_pixels,elapsed_ns,uploads");
    for (u32 size : {8u, 256u})
    {
        const u32 count = size == 8 ? 8192 : 128;
        std::vector<u32> pixels(size * size);
        for (int mode = 0; mode < 5; ++mode)
        {
            Initialize(gpu);
            Blocks(gpu, size, 0, 0x20000, mode == 4 ? -1 : mode, 8, false);
            InputState state(gpu);
            Decode(0, size, pixels.data(), 0, 0x20000, 0, gpu); // untimed warmup
            const auto ns = Elapsed([&] {
                for (u32 n = 0; n < count; ++n)
                    ConvertCompressedTexture<outputFmt_RGB6A5>(size, size, pixels.data(), 0, 0x20000, 0, gpu);
            });
            state.Check(gpu);
            out.Pixels(pixels);
            Row("decode-mode" + std::to_string(mode), size, count, count, ns, 0);
        }
        // Unchanged indexed decoder control, measured with identical output reuse.
        Initialize(gpu);
        InputState indexedState(gpu);
        ConvertNColorsTexture<outputFmt_RGB6A5, 8>(size, size, pixels.data(), 0, 0, true, gpu);
        const auto indexedNs = Elapsed([&] {
            for (u32 n = 0; n < count; ++n)
                ConvertNColorsTexture<outputFmt_RGB6A5, 8>(size, size, pixels.data(), 0, 0, true, gpu);
        });
        indexedState.Check(gpu);
        out.Pixels(pixels);
        Row("indexed-control", size, count, count, indexedNs, 0);

        Initialize(gpu);
        Blocks(gpu, size, 0, 0x20000, -1, 8, false);
        InputState cacheState(gpu);
        Uploads uploads;
        uploads.Pixels.resize(size * size * 128);
        auto cache = std::make_unique<Cache>(gpu, Loader{uploads});
        u32 handle = 0, layer = 0, *helper = nullptr;
        const u32 parameter = Parameter(size);
        // Warm the code/allocator, then clear entries outside the measured region.
        cache->GetTexture(parameter, 0, handle, layer, helper);
        cache->Reset();
        uploads.Count = uploads.Arrays = uploads.Deletes = 0;
        uploads.Position = 0;
        const auto missNs = Elapsed([&] {
            for (u32 key = 0; key < 128; ++key)
                cache->GetTexture(parameter, key, handle, layer, helper);
        });
        Require(uploads.Count == 128, "benchmark did not execute 128 real misses");
        cacheState.Check(gpu);
        out.Pixels(uploads.Pixels);
        out.Word(handle); out.Word(layer); out.Word(*helper);
        Row("cache-miss", size, 128, uploads.Count, missNs, uploads.Count);
        u32 sink = 0;
        const auto hitNs = Elapsed([&] {
            for (u32 n = 0; n < 100000; ++n)
            {
                cache->GetTexture(parameter, 0, handle, layer, helper);
                sink += handle + layer + *helper;
            }
        });
        Require(uploads.Count == 128, "warm control decoded a texture");
        cacheState.Check(gpu);
        out.Word(sink); out.Word(uploads.Count); out.Word(uploads.Arrays);
        Row("warm-control", size, 100000, 0, hitNs, 0);
        cache->Reset();
        out.Word(uploads.Deletes);
    }
}
}

int main(int argc, char** argv)
{
    try
    {
        Require(argc == 3, "usage: Optimization161A2.exe check|bench output.bin");
        auto nds = std::make_unique<NDS>();
        Output out(argv[2]);
        if (std::string(argv[1]) == "check") Correctness(nds->GPU, out);
        else if (std::string(argv[1]) == "bench") Benchmark(nds->GPU, out);
        else throw std::runtime_error("unknown command");
        return 0;
    }
    catch (const std::exception& e)
    {
        std::fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
