// SPDX-License-Identifier: GPL-3.0-or-later
// Exercises PCM16 decoding, loop history, the mixer and the real 48 kHz PCM
// output. The companion spectrum check measures these files, not FIR taps.
#include "NDS.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <stdexcept>
#include <string>

using namespace melonDS;

static void Require(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static std::vector<u8> Save(NDS& nds)
{
    Savestate state;
    Require(nds.DoSavestate(&state), "Sinc full-state save failed");
    state.Finish();
    Require(!state.Error && state.MinorVersion() == 15, "Sinc state history version missing");
    const auto* first = static_cast<const u8*>(state.Buffer());
    return {first, first+state.Length()};
}

static void OutputTest()
{
    NDSArgs args;
    args.JIT.reset();
    args.Interpolation = AudioInterpolation::Sinc;
    args.BitDepth = AudioBitDepth::_16Bit;
    auto nds = std::make_unique<NDS>(std::move(args));
    auto start = [&]
    {
        nds->Reset();
        nds->ARM7Write16(0x04000304, 1);
        nds->ARM7Write16(0x04000500, 0x807F);
        nds->ARM7Write16(0x04000504, 0x200);
        for (unsigned i = 0; i < 32; ++i) nds->ARM7Write16(0x02004000+2*i, 6000);
        nds->ARM7Write32(0x04000404, 0x02004000);
        nds->ARM7Write32(0x04000408, 0xFE00);
        nds->ARM7Write32(0x0400040C, 16);
        nds->ARM7Write32(0x04000400, 0xA800007F); // Left only.
    };
    auto stream = [&](unsigned cycles, unsigned ticks, unsigned block)
    {
        std::vector<s16> result;
        std::array<s16, 4096> buffer;
        for (unsigned i = 0; i < ticks; ++i)
        {
            nds->SPU.Mix(cycles);
            if ((i+1) % block == 0 || i+1 == ticks)
            {
                nds->SPU.BufferAudio();
                const int count = nds->SPU.ReadOutput(buffer.data(), buffer.size()/2);
                result.insert(result.end(), buffer.begin(), buffer.begin()+2*count);
            }
        }
        Require(!nds->SPU.GetOutputDroppedFrames(), "Sinc output queue lost frames");
        return result;
    };
    auto constant = [](const auto& pcm)
    {
        Require(pcm.size() >= 256, "Sinc output missing");
        for (size_t i = pcm.size()-256; i < pcm.size(); i += 2)
            Require(pcm[i] == 6000 && pcm[i+1] == 0, "Sinc DC/stereo gain changed");
    };
    start();
    nds->SPU.Mix(0);
    nds->SPU.BufferAudio();
    Require(nds->SPU.GetOutputSize() == 0, "Zero-cycle event advanced Sinc output clock");
    for (double rate : {16000.0, 44100.0, 48000.0, 96000.0})
    {
        nds->SPU.SetOutputSampleRate(rate);
        for (double skew : {1.0, 0.5, 2.0, 1.0})
        {
            nds->SPU.SetOutputSkew(skew);
            for (unsigned cycles : {512u, 352u, 512u})
            {
                nds->SPU.SetSampleRate(cycles == 512 ? AudioSampleRate::_32KHz : AudioSampleRate::_47KHz);
                const auto pcm = stream(cycles, 2048, 32);
                const double expected = 2048.0*cycles*rate/(16756991.0*skew);
                Require(std::abs(pcm.size()/2.0 - expected) < 2, "Sinc output clock drift");
                constant(pcm);
            }
        }
    }
    nds->SPU.SetOutputSampleRate(48000);
    start();
    const auto one = stream(512, 4096, 1);
    start();
    Require(stream(512, 4096, 37) == one, "Sinc output depends on buffer chunking/reset");
    for (auto mode : {AudioInterpolation::None, AudioInterpolation::MinimumPhase, AudioInterpolation::Sinc})
    {
        nds->SPU.SetInterpolation(mode);
        const auto pcm = stream(512, 2048, 32);
        Require(nds->SPU.GetInterpolation() == mode && !pcm.empty(), "Sinc mode switch failed");
        if (mode == AudioInterpolation::Sinc) constant(pcm);
    }
    auto saved = Save(*nds);
    nds->SPU.SetOutputSampleRate(44100);
    nds->SPU.SetOutputSkew(0.5);
    nds->SPU.SetSampleRate(AudioSampleRate::_47KHz);
    stream(352, 1024, 32);
    Savestate restore(saved.data(), saved.size(), false);
    Require(nds->DoSavestate(&restore) && !restore.Error, "Sinc output state load failed");
    nds->SPU.ResetOutputHistory(); // Same committed-load contract as the frontend.
    Require(nds->SPU.GetOutputSize() == 0, "Sinc load retained queued host output");
    const auto loaded = stream(512, 2048, 32);
    Require(std::abs(loaded.size()/2.0 - 2048*512.0*44100/(16756991*0.5)) < 2,
            "Sinc load lost host rates or restored mixer clock");
    constant(loaded);
    nds->SPU.SetInterpolation(AudioInterpolation::None);
    Savestate old;
    Require(nds->DoSavestate(&old), "Legacy state save failed");
    old.Finish();
    Require(old.MinorVersion() < 15, "Legacy mode unnecessarily changed save version");
    nds->SPU.SetInterpolation(AudioInterpolation::Sinc);
    Savestate oldRestore(old.Buffer(), old.Length(), false);
    Require(nds->DoSavestate(&oldRestore) && !oldRestore.Error, "Legacy state no longer loads in Sinc");
    nds->SPU.ResetOutputHistory();
    Require(nds->SPU.GetInterpolation() == AudioInterpolation::Sinc, "State load changed selected mode");
    constant(stream(512, 2048, 32));
    nds->Reset();
    const auto silence = stream(512, 1024, 32);
    Require(std::all_of(silence.begin(), silence.end(), [](s16 x) { return x == 0; }),
            "Sinc reset leaked old audio");
    std::puts("Sinc output rates/skew, stereo DC, chunking, mode switch and state reset PASS");
}

static void HistoryTest()
{
    NDSArgs args;
    args.JIT.reset();
    args.Interpolation = AudioInterpolation::Sinc;
    args.BitDepth = AudioBitDepth::_16Bit;
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    nds->ARM7Write16(0x04000304, 1);
    nds->ARM7Write16(0x04000500, 0x807F);
    nds->ARM7Write16(0x04000504, 0x200);
    for (unsigned i = 0; i < 32; ++i)
        nds->ARM7Write16(0x02004000+2*i, s16(i*1709-28000));
    nds->ARM7Write32(0x04000404, 0x02004000);
    nds->ARM7Write32(0x04000408, 0x0003FE80); // Nonzero loop; 384-cycle period.
    nds->ARM7Write32(0x0400040C, 13);
    nds->ARM7Write32(0x04000400, 0xA840007F);
    // Cross the mirrored history boundary with a non-power-of-two source loop.
    for (unsigned i = 0; i < 25000; ++i) nds->SPU.Mix(512);
    auto initial = Save(*nds);
    auto capture = [&]
    {
        // Capture the channel tap, avoiding deliberately unsaved host buffers.
        nds->ARM7Write16(0x04000418, 0xFE00);
        nds->ARM7Write32(0x04000510, 0x02008000);
        nds->ARM7Write16(0x04000514, 64);
        nds->ARM7Write8(0x04000508, 0x86);
        for (unsigned i = 0; i < 128; ++i) nds->SPU.Mix(512);
        std::array<u32,64> output;
        for (unsigned i = 0; i < output.size(); ++i) output[i] = nds->ARM7Read32(0x02008000+4*i);
        return output;
    };
    const auto before = capture();
    nds->Reset();
    Savestate restore(initial.data(), initial.size(), false);
    Require(nds->DoSavestate(&restore) && !restore.Error, "Sinc full-state load failed");
    Require(capture() == before, "Sinc loop/history changed after full-state restore");
    Require(std::any_of(before.begin(), before.end(), [](u32 x) { return x != 0; }), "Sinc test captured silence");

    // Independent constant-input checks catch stale tails on stop and reset,
    // and failure to extend the last one-shot sample when HOLD is enabled.
    for (u32 period : {1u, 256u, 384u, 512u, 2048u})
    {
        nds->Reset();
        nds->ARM7Write16(0x04000304, 1);
        nds->ARM7Write16(0x04000500, 0x807F);
        for (unsigned i = 0; i < 32; ++i) nds->ARM7Write16(0x02004000+2*i, 6000);
        nds->ARM7Write32(0x04000404, 0x02004000);
        nds->ARM7Write32(0x04000408, 65536-period);
        nds->ARM7Write32(0x0400040C, 16);
        nds->ARM7Write32(0x04000400, 0xB040807F);
        for (unsigned i = 0; i < 128*std::max(period, 512u)/512; ++i) nds->SPU.Mix(512);
        const auto held = capture();
        Require(std::all_of(held.begin(), held.end(), [](u32 x) { return x == 0x17701770; }),
                "Sinc one-shot HOLD did not settle at its final value");
        nds->ARM7Write32(0x04000400, 0);
        const auto stopped = capture();
        Require(std::all_of(stopped.begin(), stopped.end(), [](u32 x) { return x == 0; }),
                "Explicit stop left a sinc tail");
        nds->Reset();
        nds->ARM7Write16(0x04000304, 1);
        nds->ARM7Write16(0x04000500, 0x807F);
        const auto reset = capture();
        Require(std::all_of(reset.begin(), reset.end(), [](u32 x) { return x == 0; }),
                "Reset retained sinc history");
    }
    std::puts("Sinc loop, wrapped history, full-state roundtrip, HOLD, stop and reset PASS");
}

static void Capture(NDS& nds, unsigned period, unsigned bin, bool square,
                    const std::filesystem::path& path)
{
    nds.Reset();
    nds.ARM9.Halt(1);
    nds.ARM7.Halt(1);
    nds.SPU.SetOutputSampleRate(48000);
    nds.ARM7Write16(0x04000304, 1);
    nds.ARM7Write16(0x04000504, 0x200);
    nds.ARM7Write16(0x04000500, 0x807F);
    constexpr unsigned length = 4096;
    for (unsigned i = 0; i < length; ++i)
    {
        const double phase = 2 * std::numbers::pi * bin * i / length;
        const double wave = square ? ((bin*i % length) < length/2 ? 1 : -1) : std::sin(phase);
        nds.ARM7Write16(0x02004000 + 2*i, static_cast<s16>(std::lround(20000*wave)));
    }
    nds.ARM7Write32(0x04000404, 0x02004000);
    nds.ARM7Write32(0x04000408, 65536-period);
    nds.ARM7Write32(0x0400040C, length/2);
    nds.ARM7Write32(0x04000400, 0xA840007F);
    std::ofstream out(path, std::ios::binary);
    if (!out) throw std::runtime_error("Cannot create PCM output");
    std::array<s16, 4096> samples;
    for (unsigned i = 0; i < 32768; ++i)
    {
        nds.SPU.Mix(512);
        if (i % 32 == 31)
        {
            const int count = nds.SPU.ReadOutput(samples.data(), samples.size()/2);
            out.write(reinterpret_cast<const char*>(samples.data()), count*2*sizeof(s16));
        }
    }
    if (!out || nds.SPU.GetOutputDroppedFrames()) throw std::runtime_error("PCM capture lost samples");
}

int main(int argc, char** argv) try
{
    if (argc == 2 && std::string(argv[1]) == "--history")
    {
        HistoryTest();
        OutputTest();
        return 0;
    }
    if (argc < 3) return 2;
    const std::filesystem::path output(argv[1]);
    std::filesystem::create_directories(output);
    for (int arg = 2; arg < argc; ++arg)
    {
        const int mode = std::stoi(argv[arg]);
        NDSArgs args;
        args.JIT.reset();
        args.BitDepth = AudioBitDepth::_16Bit;
        args.Interpolation = static_cast<AudioInterpolation>(mode);
        auto nds = std::make_unique<NDS>(std::move(args));
        for (unsigned period : {256u, 384u, 512u, 768u, 1024u, 2048u})
        {
            const double ratio = std::min(1.0, period/512.0);
            for (double fraction : {0.1, 0.25, 0.5, 0.75, 0.85, 0.9, 0.95, 0.96875, 1.05, 1.2, 1.5})
            {
                const unsigned bin = std::lround(2048*ratio*fraction);
                if (bin >= 2048) continue;
                const auto name = "m"+std::to_string(mode)+"-p"+std::to_string(period)+"-b"+std::to_string(bin);
                Capture(*nds, period, bin, false, output/(name+"-sine.pcm"));
                if (fraction == 0.1 || fraction == 0.75)
                    Capture(*nds, period, bin, true, output/(name+"-square.pcm"));
            }
        }
        std::printf("mode %d capture complete\n", mode);
        std::fflush(stdout);
    }
    return 0;
}
catch (const std::exception& error)
{
    std::fprintf(stderr, "%s\n", error.what());
    return 1;
}
