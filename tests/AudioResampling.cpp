// SPDX-License-Identifier: GPL-3.0-or-later
#include "NDS.h"
#include "DSi.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
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

static bool TestOneShotHoldTiming()
{
    // Drive the public Mix entry point in its normal 512-cycle quanta, with
    // the scheduler's normal pre-dispatch cancellation. Observe only ARM7
    // MMIO and capture RAM; no private channel state or injected results.
    constexpr u32 source = 0x02004000, dest = 0x02008000;
    constexpr u32 guard = 0x5A5A5A5A;
    bool passed = true;
    for (unsigned format = 0; format < 3; ++format)
    for (bool hold : {false, true})
    {
        NDSArgs args;
        args.JIT.reset();
        auto nds = std::make_unique<NDS>(std::move(args));
        nds->Reset();
        nds->SPU.SetInterpolation(AudioInterpolation::None);
        nds->ARM7Write16(0x04000304, 1);
        nds->ARM7Write16(0x04000504, 0x0200);
        nds->ARM7Write16(0x04000500, 0x807F);
        for (unsigned i = 0; i < 16; ++i) nds->ARM7Write8(source + i, 0);
        const s16 last = format == 0 ? 12288 : format == 1 ? -8192 : 4097;
        if (format == 0)
        {
            for (unsigned i = 0; i < 16; ++i) nds->ARM7Write8(source + i, i == 15 ? 48 : 16);
        }
        else if (format == 1)
        {
            for (unsigned i = 0; i < 8; ++i) nds->ARM7Write16(source + i * 2, u16(i == 7 ? last : 4096));
        }
        else
        {
            nds->ARM7Write32(source, 4096);
            nds->ARM7Write8(source + 15, 0x10);
        }
        nds->ARM7Write32(0x04000404, source);
        nds->ARM7Write32(0x04000408, 0x0000FC00); // Two mixer ticks per source period.
        nds->ARM7Write32(0x0400040C, 4);
        const u32 control = 0x9040007F | (format << 29) | (hold ? 0x8000 : 0);
        nds->ARM7Write32(0x04000400, control);
        nds->ARM7Write16(0x04000418, 0xFE00); // Capture every half source period.
        nds->ARM7Write32(0x04000510, dest);
        for (unsigned i = 0; i < 200; i += 4) nds->ARM7Write32(dest - 4 + i, guard);
        nds->ARM7Write16(0x04000514, 48); // 96 captured half periods.
        nds->ARM7Write8(0x04000508, 0x86);
        auto tick = [&] {
            nds->CancelEvent(Event_SPU);
            nds->SPU.Mix(512);
        };
        const unsigned first = format == 2 ? 22 : 6;
        const unsigned count = format == 0 ? 16 : format == 1 ? 8 : 24;
        const unsigned final = first + 2 * (count - 1);
        std::array<bool, 96> busy;
        for (unsigned i = 0; i < busy.size(); ++i)
        {
            tick();
            busy[i] = (nds->ARM7Read32(0x04000400) >> 31) != 0;
        }
        unsigned dataErrors = 0, busyErrors = 0;
        for (unsigned i = 0; i < busy.size(); ++i)
        {
            const unsigned time = i + 1;
            const s16 expected = time < first ? 0 : time < final ? 4096 :
                time < final + 2 || hold ? last : 0;
            dataErrors += s16(nds->ARM7Read16(dest + i * 2)) != expected;
            busyErrors += busy[i] != (time < final);
        }
        const bool guards = nds->ARM7Read32(dest - 4) == guard && nds->ARM7Read32(dest + 192) == guard;
        std::printf("HOLD timing format=%u hold=%u final-tick=%u busy-before/final/after=%u/%u/%u capture-final/half/end=%d/%d/%d data-errors=%u busy-errors=%u guards=%u\n",
            format, hold, final, busy[final - 2], busy[final - 1], busy[final],
            s16(nds->ARM7Read16(dest + (final - 1) * 2)), s16(nds->ARM7Read16(dest + final * 2)),
            s16(nds->ARM7Read16(dest + (final + 1) * 2)), dataErrors, busyErrors, guards);
        passed &= !dataErrors && !busyErrors && guards && !(nds->ARM7Read8(0x04000508) & 0x80);
        if (format != 1) continue;

        // Each observation captures two real consecutive mixer ticks. A
        // four-byte capture commits both samples and clears capture Busy.
        auto pair = [&](const char* name, s16 expected0, s16 expected1, bool expectedBusy) {
            nds->ARM7Write32(dest, guard);
            nds->ARM7Write16(0x04000514, 1);
            nds->ARM7Write8(0x04000508, 0x86);
            tick();
            const bool busy0 = (nds->ARM7Read32(0x04000400) >> 31) != 0;
            tick();
            const s16 actual0 = s16(nds->ARM7Read16(dest)), actual1 = s16(nds->ARM7Read16(dest + 2));
            const bool busy1 = (nds->ARM7Read32(0x04000400) >> 31) != 0;
            const bool ok = actual0 == expected0 && actual1 == expected1 &&
                busy0 == expectedBusy && busy1 == expectedBusy && !(nds->ARM7Read8(0x04000508) & 0x80);
            std::printf("HOLD control hold=%u %s capture=%d/%d expected=%d/%d busy=%u/%u: %s\n",
                hold, name, actual0, actual1, expected0, expected1, busy0, busy1, ok ? "PASS" : "FAIL");
            passed &= ok;
        };
        const s16 held = hold ? last : 0;
        nds->ARM7Write16(0x04000500, 0x007F);
        nds->ARM7Write32(dest, guard);
        nds->ARM7Write16(0x04000514, 1);
        nds->ARM7Write8(0x04000508, 0x86);
        tick(); tick();
        passed &= nds->ARM7Read32(dest) == guard && (nds->ARM7Read8(0x04000508) & 0x80) &&
            !(nds->ARM7Read32(0x04000400) & 0x80000000);
        nds->ARM7Write16(0x04000500, 0x807F);
        tick(); tick();
        passed &= s16(nds->ARM7Read16(dest)) == held && s16(nds->ARM7Read16(dest + 2)) == held;
        pair("master-reenabled", held, held, false);
        nds->ARM7Write16(0x04000304, 0);
        pair("sound-power-off-capture", held, held, false);
        nds->ARM7Write16(0x04000304, 1);

        // A new key-on must refill the real source FIFO. GBATEK documents
        // retained output only in the first PCM startup period with HOLD.
        for (unsigned i = 0; i < 8; ++i) nds->ARM7Write16(source + i * 2, 6144);
        nds->ARM7Write8(0x04000403, u8(control >> 24));
        pair("retrigger-first-period", held, 0, true);
        pair("retrigger-dummy-periods", 0, 0, true);
        pair("retrigger-first-sample", 0, 6144, true);
        // Preserve the existing explicit Stop behavior; retention on a
        // manual stop with HOLD set has no independent hardware oracle here.
        nds->ARM7Write16(0x04000402, u16((control & ~0x80000000u) >> 16));
        pair("explicit-stop", 0, 0, false);
        nds->ARM7Write32(0x04000400, control);
        for (unsigned i = 0; i < 24; ++i) tick();
        pair("retrigger-completed", hold ? 6144 : 0, hold ? 6144 : 0, false);
        nds->ARM7Write8(0x04000401, 0); // Clear HOLD without setting Start.
        pair("clear-hold", 0, 0, false);
        nds->ARM7Write8(0x04000401, 0x80);
        pair("hold-after-clear", 0, 0, false);
        nds->ARM7Write32(0x04000400, control | 0x8000);
        for (unsigned i = 0; i < 24; ++i) tick();
        pair("held-before-reset", 6144, 6144, false);
        // The inactive-state compatibility normalization must preserve an
        // actual newly completed HOLD through the public serializer, too.
        Savestate heldState;
        if (!nds->DoSavestate(&heldState)) return false;
        heldState.Finish();
        if (heldState.Error) return false;
        nds->Reset();
        Savestate restored(heldState.Buffer(), heldState.Length(), false);
        if (!nds->DoSavestate(&restored) || restored.Error) return false;
        pair("held-state-roundtrip", 6144, 6144, false);
        nds->Reset();
        nds->ARM7Write16(0x04000500, 0x807F);
        nds->ARM7Write32(0x04000400, 0x3040807F); // HOLD alone after reset.
        nds->ARM7Write16(0x04000418, 0xFE00);
        nds->ARM7Write32(0x04000510, dest);
        pair("reset-no-stale-sample", 0, 0, false);
    }
    return passed;
}

static bool TestOneShotState()
{
    // Changing registers while Busy is clear must not discard the retained
    // output on a full-state roundtrip. Observe actual channel capture RAM.
    struct StateCase { const char* Name; u32 Address; u32 Value; u16 Minor; s16 Sample; };
    constexpr StateCase cases[] = {
        {"unchanged", 0x0400040C, 4, 2, 6144},
        {"volume", 0x04000400, 0x30408040, 2, 3072},
        {"timer", 0x04000408, 0xFE00, 2, 6144},
        {"source", 0x04000404, 0x02004020, 2, 6144},
        {"length", 0x0400040C, 8, 4, 6144},
        {"zero-length", 0x0400040C, 0, 4, 6144},
        {"loop-position", 0x04000408, 0x0001FC00, 4, 6144},
        {"pcm8", 0x04000400, 0x1040807F, 4, 6144},
        {"adpcm", 0x04000400, 0x5040807F, 4, 6144},
        {"repeat", 0x04000400, 0x2840807F, 4, 6144},
        {"manual", 0x04000400, 0x2040807F, 4, 6144},
        {"released", 0x04000400, 0x3040007F, 2, 0},
    };
    constexpr u32 source = 0x02004000, dest = 0x02008000, guard = 0x5A5A5A5A;
    bool passed = true;
    for (bool dsi : {false, true})
    for (const auto& test : cases)
    {
        NDSArgs args;
        args.JIT.reset();
        std::unique_ptr<NDS> nds;
        if (dsi)
        {
            DSiArgs dsiArgs;
            static_cast<NDSArgs&>(dsiArgs) = std::move(args);
            nds = std::make_unique<DSi>(std::move(dsiArgs));
        }
        else nds = std::make_unique<NDS>(std::move(args));
        nds->Reset();
        nds->SPU.SetInterpolation(AudioInterpolation::None);
        nds->ARM7Write16(0x04000304, 1);
        nds->ARM7Write16(0x04000500, 0x807F);
        for (unsigned i = 0; i < 8; ++i) nds->ARM7Write16(source + i * 2, 6144);
        nds->ARM7Write32(0x04000404, source);
        nds->ARM7Write32(0x04000408, 0xFC00);
        nds->ARM7Write32(0x0400040C, 4);
        nds->ARM7Write32(0x04000400, 0xB040807F);
        auto tick = [&] {
            nds->CancelEvent(Event_SPU);
            nds->SPU.Mix(512);
        };
        for (unsigned i = 0; i < 24; ++i) tick();
        if (nds->ARM7Read32(0x04000400) & 0x80000000) return false;
        nds->ARM7Write32(test.Address, test.Value);
        auto capture = [&] {
            for (unsigned i = 0; i < 24; i += 4) nds->ARM7Write32(dest - 4 + i, guard);
            nds->ARM7Write16(0x04000418, 0xFE00);
            nds->ARM7Write32(0x04000510, dest);
            nds->ARM7Write16(0x04000514, 4);
            nds->ARM7Write8(0x04000508, 0x86);
            for (unsigned i = 0; i < 8; ++i) tick();
            bool ok = nds->ARM7Read32(dest - 4) == guard && nds->ARM7Read32(dest + 16) == guard &&
                !(nds->ARM7Read32(0x04000400) & 0x80000000) && !(nds->ARM7Read8(0x04000508) & 0x80);
            for (unsigned i = 0; i < 8; ++i) ok &= s16(nds->ARM7Read16(dest + i * 2)) == test.Sample;
            return ok;
        };
        const bool before = capture();
        Savestate saved;
        if (!nds->DoSavestate(&saved)) return false;
        saved.Finish();
        if (saved.Error) return false;
        nds->Reset();
        Savestate restored(saved.Buffer(), saved.Length(), false);
        const bool loaded = nds->DoSavestate(&restored) && !restored.Error;
        const bool after = loaded && capture();
        const bool version = saved.MajorVersion() == 14 && saved.MinorVersion() == test.Minor;
        std::printf("HOLD state %s %s minor=%u expected=%u before=%u loaded=%u after=%u sample=%d: %s\n",
            dsi ? "DSi" : "DS", test.Name, saved.MinorVersion(), test.Minor, before, loaded, after,
            s16(nds->ARM7Read16(dest)), before && after && version ? "PASS" : "FAIL");
        passed &= before && after && version;

        if (dsi || std::strcmp(test.Name, "length") || saved.MinorVersion() != 4) continue;
        // 14.4 needs an explicit empty legacy-cart record even when audio
        // alone raised the version. Missing/ambiguous records must fail;
        // 14.3 retains its strict pending-transfer-only contract.
        Savestate cart(saved.Buffer(), saved.Length(), false);
        cart.Section("NC13");
        if (cart.Error) return false;
        const u32 cartHeader = cart.Length() - 16;
        u8 mode = 0xFF;
        u32 count = ~0u;
        cart.Var8(&mode);
        cart.Var32(&count);
        if (cart.Error || mode || count) return false;
        const auto* bytes = static_cast<const u8*>(saved.Buffer());
        for (unsigned malformed = 0; malformed < 3; ++malformed)
        {
            std::vector<u8> invalid(bytes, bytes + saved.Length());
            if (malformed == 0) std::memcpy(invalid.data() + cartHeader, "MISS", 4);
            else if (malformed == 1) invalid[cartHeader + 16] = 1;
            else invalid[6] = 3;
            nds->Reset();
            Savestate rejected(invalid.data(), u32(invalid.size()), false);
            const bool refused = !nds->DoSavestate(&rejected) && rejected.Error;
            std::printf("HOLD state invalid-cart-record=%u: %s\n", malformed, refused ? "PASS" : "FAIL");
            passed &= refused;
        }
    }
    return passed;
}

static bool TestOneShotHold()
{
    // GBATEK DS Sound Notes: one-shot HOLD keeps the final decoded sample
    // after Busy clears. The oracle is an explicit PCM16 continuation of
    // that waveform, through the same real mixer/resampler/capture path.
    // https://problemkaputt.de/gbatek.htm#dssoundnotes
    constexpr u32 source = 0x02004000, destination = 0x02008000;
    constexpr u32 guard = 0x5A5A5A5A;
    bool passed = true;
    for (auto interpolation : {AudioInterpolation::None, AudioInterpolation::Linear,
             AudioInterpolation::Cosine, AudioInterpolation::Cubic, AudioInterpolation::SNESGaussian})
    for (unsigned format = 0; format < 3; ++format)
    for (bool hold : {false, true})
    {
        // All formats use the hardware path; optional filters need only the
        // shared PCM16 HOLD transition, with a different signed final sample.
        if (interpolation != AudioInterpolation::None && (format != 1 || !hold)) continue;
        const s16 last = format == 0 ? 12288 : format == 1 ? -8192 : 4097;
        const unsigned count = format == 0 ? 16 : format == 1 ? 8 : 24;
        const unsigned padding = format == 2 ? 8 : 0;
        std::array<std::vector<s16>, 2> output;
        std::array<std::vector<s16>, 2> captured;
        for (unsigned reference = 0; reference < 2; ++reference)
        {
            NDSArgs args;
            args.JIT.reset();
            args.BitDepth = AudioBitDepth::_16Bit;
            auto nds = std::make_unique<NDS>(std::move(args));
            nds->Reset();
            nds->SPU.SetInterpolation(interpolation);
            nds->ARM9.Halt(1);
            nds->ARM7.Halt(1);
            nds->ARM7Write16(0x04000304, 1);
            nds->ARM7Write16(0x04000504, 0x0200);
            nds->ARM7Write16(0x04000500, 0x807F);
            if (reference)
            {
                for (unsigned i = 0; i < 4096; ++i)
                {
                    const s16 sample = i < padding ? 0 : i < padding + count - 1 ? 4096 :
                        i == padding + count - 1 || hold ? last : 0;
                    nds->ARM7Write16(source + i * 2, u16(sample));
                }
            }
            else if (format == 0)
            {
                for (unsigned i = 0; i < 16; ++i)
                    nds->ARM7Write8(source + i, i == 15 ? 48 : 16);
            }
            else if (format == 1)
            {
                for (unsigned i = 0; i < 8; ++i)
                    nds->ARM7Write16(source + i * 2, u16(i == 7 ? last : 4096));
            }
            else
            {
                // Index 0, step 7: 23 zero nibbles leave 4096 unchanged;
                // the final nibble 1 adds exactly 1, with index still zero.
                nds->ARM7Write32(source, 4096);
                for (unsigned i = 4; i < 16; ++i)
                    nds->ARM7Write8(source + i, i == 15 ? 0x10 : 0);
            }
            nds->ARM7Write32(0x04000404, source);
            nds->ARM7Write32(0x04000408, 0x0000FE00);
            nds->ARM7Write32(0x0400040C, reference ? 2048 : 4);
            nds->ARM7Write32(0x04000400, 0x9040007F |
                ((reference ? 1 : format) << 29) | (!reference && hold ? 0x8000 : 0));
            nds->ARM7Write16(0x04000418, 0xFE00);
            nds->ARM7Write16(0x04000438, 0xFE00);
            nds->Start();
            std::array<s16, 4096> samples;
            auto frame = [&] {
                nds->RunFrame();
                const int frames = nds->SPU.ReadOutput(samples.data(), samples.size() / 2);
                output[reference].insert(output[reference].end(), samples.begin(), samples.begin() + frames * 2);
            };
            frame();
            for (unsigned unit = 0; unit < 2; ++unit)
            {
                const u32 dest = destination + unit * 64;
                for (unsigned i = 0; i < 40; i += 4) nds->ARM7Write32(dest - 4 + i, guard);
                nds->ARM7Write32(0x04000510 + unit * 8, dest);
                nds->ARM7Write16(0x04000514 + unit * 8, 8);
                nds->ARM7Write8(0x04000508 + unit, unit == 0 ? 0x86 : 0x84);
            }
            frame();
            if (format == 1 && interpolation == AudioInterpolation::None)
            {
                // Compare the complete audible mute/resume transients, too.
                nds->ARM7Write16(0x04000500, 0x007F);
                frame();
                nds->ARM7Write16(0x04000500, 0x807F);
                frame();
                nds->ARM7Write16(0x04000304, 0);
                frame();
                nds->ARM7Write16(0x04000304, 1);
                frame();
            }
            const bool busy = (nds->ARM7Read32(0x04000400) >> 31) != 0;
            for (unsigned unit = 0; unit < 2; ++unit)
            {
                const u32 dest = destination + unit * 64;
                const s16 expected = hold ? (unit == 0 ? last : last / 2) : 0;
                unsigned mismatches = 0;
                for (unsigned i = 0; i < 32; i += 2)
                {
                    const s16 actual = s16(nds->ARM7Read16(dest + i));
                    captured[reference].push_back(actual);
                    if (interpolation == AudioInterpolation::None) mismatches += actual != expected;
                }
                const bool intact = nds->ARM7Read32(dest - 4) == guard && nds->ARM7Read32(dest + 32) == guard;
                const bool captureStopped = !(nds->ARM7Read8(0x04000508 + unit) & 0x80);
                std::printf("HOLD format=%u hold=%u interp=%u reference=%u tap=%s capture=%d hardware-mismatches=%u busy=%u guards=%u stopped=%u\n",
                    format, hold, unsigned(interpolation), reference, unit == 0 ? "channel" : "mixer", s16(nds->ARM7Read16(dest)),
                    mismatches, busy, intact, captureStopped);
                passed &= !mismatches && intact && captureStopped && busy == bool(reference);
            }
        }
        const auto diff = std::mismatch(output[0].begin(), output[0].end(), output[1].begin(), output[1].end());
        const bool equal = !output[0].empty() && output[0] == output[1] && captured[0] == captured[1];
        std::printf("HOLD output format=%u hold=%u interp=%u frames=%zu reference-frames=%zu first-difference=%zu actual=%d expected=%d: %s\n",
            format, hold, unsigned(interpolation), output[0].size() / 2, output[1].size() / 2, size_t(diff.first - output[0].begin()),
            diff.first == output[0].end() ? 0 : *diff.first, diff.second == output[1].end() ? 0 : *diff.second,
            equal ? "PASS" : "FAIL");
        passed &= equal;
    }
    return TestOneShotHoldTiming() && passed;
}

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "one-shot-hold") == 0) return TestOneShotHold() ? 0 : 6;
    if (argc == 2 && std::strcmp(argv[1], "one-shot-state") == 0) return TestOneShotState() ? 0 : 7;
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
