// SPDX-License-Identifier: GPL-3.0-or-later
#include "NDS.h"
#include "DSi.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>
using namespace melonDS;

static std::vector<s16> CapturePCM(bool dsi, AudioBitDepth depth, bool applySetter)
{
    NDSArgs args;
    args.JIT.reset();
    args.BitDepth = depth;
    std::unique_ptr<NDS> nds;
    if (dsi)
    {
        DSiArgs dsiArgs;
        static_cast<NDSArgs&>(dsiArgs) = std::move(args);
        nds = std::make_unique<DSi>(std::move(dsiArgs));
    }
    else nds = std::make_unique<NDS>(std::move(args));
    if (applySetter) nds->SPU.SetDegrade10Bit(depth);
    std::vector<s16> result;
    for (unsigned reset = 0; reset < 2; ++reset)
    {
        nds->Reset();
        nds->ARM9.Halt(1);
        nds->ARM7.Halt(1);
        nds->ARM7Write16(0x04000304, 1); // Enable sound power.
        if (dsi) static_cast<DSi&>(*nds).I2S.WriteSndExCnt(0x8008, 0xFFFF); // NITRO mixer only.
        // A repeating PCM16 signal with bits below the 10-bit output boundary.
        constexpr u32 source = 0x02004000;
        for (unsigned i = 0; i < 16; ++i)
            nds->ARM7Write16(source + i * 2, i < 8 ? 12345 : u16(-12345));
        nds->ARM7Write32(0x04000404, source);
        nds->ARM7Write32(0x04000408, 0x0000FE00); // Timer; loop position zero.
        nds->ARM7Write32(0x0400040C, 8); // 32-byte loop.
        nds->ARM7Write16(0x04000504, 0x0200); // Neutral sound bias.
        nds->ARM7Write16(0x04000500, 0x807F); // Enable mixer, full volume.
        nds->ARM7Write32(0x04000400, 0xA840007F); // PCM16, repeat, center pan.
        nds->Start();
        std::array<s16, 4096> samples{};
        for (unsigned frame = 0; frame < 3; ++frame)
        {
            nds->RunFrame();
            const int count = nds->SPU.ReadOutput(samples.data(), samples.size() / 2);
            result.insert(result.end(), samples.begin(), samples.begin() + count * 2);
        }
    }
    return result;
}

static bool TestInitialBitDepth()
{
    bool passed = true;
    for (bool dsi : {false, true})
    {
        const auto ten = CapturePCM(dsi, AudioBitDepth::_10Bit, true);
        const auto sixteen = CapturePCM(dsi, AudioBitDepth::_16Bit, true);
        if (ten.empty() || ten.size() != sixteen.size() || ten == sixteen ||
            std::none_of(sixteen.begin(), sixteen.end(), [](s16 sample) { return sample != 0; }))
        {
            std::fprintf(stderr, "Audio bitdepth %s: reference signal cannot distinguish policies\n", dsi ? "DSi" : "DS");
            return false;
        }
        for (auto depth : {AudioBitDepth::Auto, AudioBitDepth::_10Bit, AudioBitDepth::_16Bit})
        {
            const auto& expected = depth == AudioBitDepth::_10Bit ||
                (depth == AudioBitDepth::Auto && !dsi) ? ten : sixteen;
            const bool equal = CapturePCM(dsi, depth, false) == expected;
            std::printf("Audio first/reset PCM %s depth=%d: %s\n", dsi ? "DSi" : "DS", int(depth), equal ? "PASS" : "FAIL");
            passed &= equal;
        }
    }
    return passed;
}

int main()
{
    if (!TestInitialBitDepth()) return 4;
    NDSArgs args;
    args.JIT.reset();
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    nds->ARM9.Halt(1);
    nds->ARM7.Halt(1);
    nds->Start();
    std::array<s16, 4096> samples;
    for (double speed : {1.0, 0.5, 1.0})
    {
        nds->SPU.DrainOutput();
        nds->SPU.SetOutputSkew(speed);
        int total = 0;
        for (int frame = 0; frame < 20; ++frame)
        {
            nds->RunFrame();
            total += nds->SPU.ReadOutput(samples.data(), samples.size() / 2);
        }
        const double expected = 20 * 48000.0 / 59.8260982880808 / speed;
        // One unfinished core audio batch can remain in blip at each boundary.
        if (std::abs(total - expected) > 400)
        {
            std::fprintf(stderr, "Audio clock %.1fx produced %d frames, expected %.0f\n", speed, total, expected);
            return 1;
        }
        std::printf("Audio clock %.1fx: %d output frames in 20 emulated frames\n", speed, total);
    }
    if (nds->SPU.GetOutputDroppedFrames() != 0) return 2;
    // Stop consuming: the actual bounded core FIFO must report overwritten
    // frames. Draining later must not erase that diagnostic evidence.
    for (int frame = 0; frame < 20; ++frame) nds->RunFrame();
    const u64 dropped = nds->SPU.GetOutputDroppedFrames();
    nds->SPU.DrainOutput();
    if (!dropped || nds->SPU.GetOutputDroppedFrames() != dropped) return 3;
    std::printf("Audio FIFO: overwritten frames=%llu; drain preserves the counter\n", (unsigned long long)dropped);
}
