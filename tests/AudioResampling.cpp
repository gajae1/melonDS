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

static std::vector<s16> CapturePCM(bool dsi, AudioBitDepth depth, bool applySetter, bool chunked = false)
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
        std::array<s16, 4098> samples{};
        for (unsigned frame = 0; frame < 3; ++frame)
        {
            nds->RunFrame();
            if (!chunked)
            {
                const int count = nds->SPU.ReadOutput(samples.data(), 2048);
                result.insert(result.end(), samples.begin(), samples.begin() + count * 2);
                continue;
            }
            constexpr s16 guard = 0x6AAD;
            // Consume the same actual PCM with empty/short/oversized requests;
            // three frames also take the ring across its wrap boundary.
            for (int request : {0, 1, 7, 63, 256, 511, 2048, 1})
            {
                samples.fill(guard);
                const int count = nds->SPU.ReadOutput(samples.data() + 1, request);
                if (count < 0 || count > request || samples.front() != guard ||
                    !std::all_of(samples.begin() + 1 + count * 2, samples.end(),
                                 [](s16 sample) { return sample == guard; }))
                    return {};
                result.insert(result.end(), samples.begin() + 1, samples.begin() + 1 + count * 2);
            }
            if (nds->SPU.GetOutputSize() != 0) return {};
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
        if (CapturePCM(dsi, AudioBitDepth::_16Bit, true, true) != sixteen)
        {
            std::fprintf(stderr, "Audio chunked read changes %s PCM or destination guards\n", dsi ? "DSi" : "DS");
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

static bool TestCaptureSource()
{
    // GBATEK: DS Sound Capture / Block Diagrams / Capture Clipping/Rounding.
    // https://problemkaputt.de/gbatek.htm#dssoundcapture
    // The channel tap follows volume, precedes pan, and has a both-negative
    // quirk. Expected samples below are independent, literal register vectors.
    struct CaptureCase
    {
        const char* Name;
        std::array<s16, 4> Samples;
        u8 Volume;
        u8 Divider;
        bool ChannelSource;
        std::array<s16, 2> Expected16;
        std::array<s8, 2> Expected8;
    };
    constexpr CaptureCase cases[] = {
        {"channel", {12288, 1024, -8192, 2048}, 127, 0, true, {12288, -8192}, {48, -32}},
        {"volume-divider", {12288, 1024, -8192, 2048}, 64, 1, true, {3072, -2048}, {12, -8}},
        {"both-negative", {-4096, -2048, -8192, -1024}, 127, 0, true, {-32768, -32768}, {-128, -128}},
        {"zero-associated", {-4096, 0, 0, -1024}, 127, 0, true, {-4096, 0}, {-16, 0}},
        {"negative-fraction", {-257, 1024, -255, 2048}, 64, 0, true, {-128, -127}, {-1, 0}},
        {"positive-fraction", {257, 1024, 255, 2048}, 64, 0, true, {128, 127}, {0, 0}},
        {"mixer", {12288, 1024, -8192, 2048}, 127, 0, false, {-6912, 14592}, {-27, 57}},
    };
    NDSArgs args;
    args.JIT.reset();
    auto nds = std::make_unique<NDS>(std::move(args));
    bool passed = true;
    for (const auto& test : cases)
    for (bool pcm8 : {false, true})
    for (bool oneShot : {false, true})
    {
        nds->Reset();
        nds->ARM9.Halt(1);
        nds->ARM7.Halt(1);
        nds->ARM7Write16(0x04000304, 1);
        // Deliberately zero master volume: capture is before the master stage.
        nds->ARM7Write16(0x04000500, 0x8000);
        for (unsigned channel = 0; channel < 5; ++channel)
        {
            const u32 source = 0x02004000 + channel * 32;
            const s16 sample = channel < 4 ? test.Samples[channel] : 512;
            for (unsigned i = 0; i < 16; ++i)
                nds->ARM7Write16(source + i * 2, u16(sample));
            const u32 reg = 0x04000400 + channel * 16;
            nds->ARM7Write32(reg + 4, source);
            nds->ARM7Write32(reg + 8, 0x0000FE00);
            nds->ARM7Write32(reg + 12, 8);
            // Opposite pans distinguish Ch0/2 from both mixer and each other.
            const u32 pan = channel == 4 ? 64 : (channel == 0 || channel == 3 ? 127 : 0);
            const bool sourceChannel = channel == 0 || channel == 2;
            const u32 volume = sourceChannel ? test.Volume : 127;
            const u32 divider = sourceChannel ? test.Divider : 0;
            nds->ARM7Write32(reg, 0xA8000000 | (pan << 16) | (divider << 8) | volume);
        }
        nds->Start();
        nds->RunFrame(); // Let PCM startup/FIFO latency expire before capture.
        constexpr u32 captureBase = 0x02005000;
        constexpr u32 captureBytes = 20; // Full FIFO flush plus a partial flush.
        constexpr u32 guard = 0x5A5A5A5A;
        for (unsigned unit = 0; unit < 2; ++unit)
        {
            const u32 dest = captureBase + unit * 64;
            for (u32 offset = 0; offset < captureBytes + 8; offset += 4)
                nds->ARM7Write32(dest - 4 + offset, guard);
            nds->ARM7Write32(0x04000510 + unit * 8, dest);
            nds->ARM7Write16(0x04000514 + unit * 8, captureBytes / 4);
        }
        const u8 mode = 0x80 | (test.ChannelSource ? 2 : 0) | (pcm8 ? 8 : 0) | (oneShot ? 4 : 0);
        if (pcm8)
        {
            nds->ARM7Write8(0x04000508, mode);
            nds->ARM7Write8(0x04000509, mode);
        }
        else if (oneShot) nds->ARM7Write16(0x04000508, u16(mode) * 0x0101);
        else nds->ARM7Write32(0x04000508, u32(mode) * 0x0101);
        nds->RunFrame();
        for (unsigned unit = 0; unit < 2; ++unit)
        {
            const u32 dest = captureBase + unit * 64;
            const int expected = pcm8 ? test.Expected8[unit] : test.Expected16[unit];
            unsigned mismatches = 0;
            for (u32 offset = 0; offset < captureBytes; offset += pcm8 ? 1 : 2)
            {
                const int actual = pcm8 ? s8(nds->ARM7Read8(dest + offset)) : s16(nds->ARM7Read16(dest + offset));
                mismatches += actual != expected;
            }
            const bool intact = nds->ARM7Read32(dest - 4) == guard &&
                nds->ARM7Read32(dest + captureBytes) == guard;
            const bool status = nds->ARM7Read8(0x04000508 + unit) == (oneShot ? mode & 0x7F : mode);
            const int first = pcm8 ? s8(nds->ARM7Read8(dest)) : s16(nds->ARM7Read16(dest));
            std::printf("Audio capture %s unit=%u pcm=%u one-shot=%u: first=%d expected=%d mismatches=%u guards=%u status=%u\n",
                test.Name, unit, pcm8 ? 8 : 16, unsigned(oneShot), first, expected, mismatches, unsigned(intact), unsigned(status));
            passed &= !mismatches && intact && status;
        }
    }
    return passed;
}

int main()
{
    if (!TestCaptureSource()) return 5;
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
