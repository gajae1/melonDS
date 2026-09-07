// SPDX-License-Identifier: GPL-3.0-or-later
// Runs the production DoCapture method with recording VRAM dirty-index access.
#include <array>
#include <stdio.h>
#include "types.h"
using namespace melonDS;
static constexpr unsigned VRAMDirtyGranularity = 512;
struct DirtyBits
{
    std::array<bool, 256> Bits{};
    bool OutOfRange = false;
    bool Sink = false;
    bool& operator[](size_t index)
    {
        if (index >= Bits.size()) { OutOfRange = true; return Sink; }
        return Bits[index];
    }
};
struct GPUState
{
    u32 CaptureCnt = 0;
    u32 VRAMMap_LCDC = 15;
    struct { u32 DispCnt = 0; } GPU2D_A;
    std::array<std::array<u16, 65536>, 4> Memory{};
    std::array<u16*, 4> VRAM{Memory[0].data(), Memory[1].data(), Memory[2].data(), Memory[3].data()};
    std::array<DirtyBits, 4> VRAMDirty{};
    u16 DispFIFOBuffer[256]{};
};
struct SoftRenderer
{
    GPUState& GPU;
    u32 Output3D[256]{};
    u32 Storage2D[256]{};
    u32* Output2D[2]{Storage2D, Storage2D};
    void DoCapture(u32 line);
};
#include "SoftwareCaptureMethod.inc"
int main()
{
    GPUState gpu;
    SoftRenderer renderer{gpu};
    for (auto& v : renderer.Storage2D) v = 0xFF3F3F3F;
    unsigned cases = 0, failures = 0;
    for (unsigned bank = 0; bank < 4; ++bank)
    for (unsigned start = 0; start < 4; ++start)
    for (unsigned size = 0; size < 4; ++size)
    for (unsigned line = 0; line < 192; ++line)
    {
        gpu.VRAMDirty = {};
        gpu.CaptureCnt = (bank << 16) | (start << 18) | (size << 20);
        renderer.DoCapture(line);
        const unsigned width = size ? 256 : 128;
        const unsigned height = size ? size * 64 : 128;
        const unsigned address = (start * 16384 + line * width) % 65536;
        const unsigned expected = address * sizeof(u16) / VRAMDirtyGranularity;
        bool ok = true;
        for (unsigned b = 0; b < 4; ++b)
        {
            ok &= !gpu.VRAMDirty[b].OutOfRange;
            for (unsigned i = 0; i < 256; ++i)
                ok &= gpu.VRAMDirty[b].Bits[i] == (b == bank && line < height && i == expected);
        }
        if (line < height)
            for (unsigned i = 0; i < width; ++i)
                ok &= gpu.Memory[bank][address + i] == 0xFFFF;
        if (!ok && failures++ < 3)
            printf("FAIL bank=%u start=%u size=%u line=%u\n", bank, start, size, line);
        ++cases;
    }
    printf("Software capture: %u cases, %u failures\n", cases, failures);
    return failures != 0;
}
