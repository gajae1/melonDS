// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <SDL2/SDL.h>
#include "types.h"
#include "AudioLowPass.h"
#include "AudioOutputRamp.h"
#include "AudioDiagnostics.h"
using namespace melonDS;

// Drive the production SDL callback with a deterministic sample producer.
// Guard samples catch writes past the requested device buffer.
struct SampleSource
{
    int available = 1024;
    int rateChanges = 0;
    int ReadOutput(s16* output, int frames)
    {
        const int count = std::min(available, frames);
        for (int i = 0; i < count; ++i)
        {
            output[i * 2] = 1000;
            output[i * 2 + 1] = -1000;
        }
        return count;
    }
    void SetOutputSkew(double) { ++rateChanges; }
};
struct Console { SampleSource SPU; };
struct AudioState
{
    Console* nds;
    double curFPS = 60, targetFPS = 60;
    int audioBufSize = 512;
    int audioFreq = 48000;
    AudioLowPass audioLowPass;
    AudioOutputRamp audioOutputRamp;
    AudioDiagnostics audioDiagnostics;
    std::atomic<int> audioLowPassCutoff{0};
    std::atomic<int> audioVolume{256};
    bool audioMutedByWindowFocus = false, audioMutedToggle = false, audioMutedByFastForward = false;
    SDL_mutex* audioSyncLock = SDL_CreateMutex();
    SDL_cond* audioSyncCond = SDL_CreateCond();
    ~AudioState() { SDL_DestroyCond(audioSyncCond); SDL_DestroyMutex(audioSyncLock); }
    static void audioCallback(void* data, Uint8* stream, int len);
};
#define EmuInstance AudioState
#include "audioCallback.inc"
#undef EmuInstance

int main(int, char**)
{
    Console console;
    AudioState state{&console};
    state.audioLowPass.Init(state.audioFreq);
    state.audioOutputRamp.Init(state.audioFreq);
    constexpr s16 poison = 0x5555;
    constexpr int frames = 16;
    std::array<s16, 2048> output;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::fprintf(stderr, "%s\n", message); }
    };
    const auto run = [&] {
        output.fill(poison);
        AudioState::audioCallback(&state, reinterpret_cast<Uint8*>(output.data()), frames * 4);
        check(std::none_of(output.begin(), output.begin() + frames * 2,
                          [](s16 sample) { return sample == poison; }),
              "SDL output buffer is only partially initialized");
        check(std::all_of(output.begin() + frames * 2, output.end(),
                         [](s16 sample) { return sample == poison; }),
              "SDL callback writes beyond the device buffer");
    };
    state.curFPS = 30;
    run();
    state.curFPS = 240;
    run();
    state.curFPS = 60;
    console.SPU.available = 3;
    state.audioVolume = 128;
    run();
    check(output[0] == 500 && output[1] == -500, "Stereo volume scaling changed");
    check(output[frames * 2 - 2] >= 0 && output[frames * 2 - 2] <= 500 &&
          std::abs(output[frames * 2 - 2] + output[frames * 2 - 1]) <= 1,
          "Underrun tail clips or mixes stereo channels");
    console.SPU.available = 0;
    for (unsigned i = 0; i < 4; ++i) run();
    check(std::all_of(output.begin(), output.begin() + frames * 2,
                      [](s16 sample) { return sample == 0; }), "Empty queue is not silent");
    console.SPU.available = 1024;
    state.audioMutedToggle = true;
    run();
    check(std::all_of(output.begin(), output.begin() + frames * 2,
                      [](s16 sample) { return sample == 0; }), "Muted audio is not silent");
    check(console.SPU.rateChanges == 0, "Device thread modifies the core resampler");

    // A constant source has no real discontinuity. Missing samples and their
    // return must not introduce a full-amplitude step at callback boundaries.
    // The source shortage is deliberate; this does not reproduce a device fault.
    for (int buffer : {128, 512})
    {
        Console gapConsole;
        AudioState gapState{&gapConsole};
        gapState.audioLowPass.Init(gapState.audioFreq);
        gapState.audioOutputRamp.Init(gapState.audioFreq);
        gapState.audioDiagnostics.Enabled = true;
        int previous = 1000, maxStep = 0;
        for (int available : {buffer, buffer - 2, 0, 0, buffer, buffer})
        {
            gapConsole.SPU.available = available;
            AudioState::audioCallback(&gapState, reinterpret_cast<Uint8*>(output.data()), buffer * 4);
            for (int i = 0; i < buffer; ++i)
            {
                maxStep = std::max(maxStep, std::abs(int(output[2 * i]) - previous));
                previous = output[2 * i];
                check(std::abs(int(output[2 * i]) + output[2 * i + 1]) <= 1,
                      "Gap recovery mixes stereo channels");
            }
        }
        std::printf("Audio shortage/recovery buffer=%d: largest step=%d / amplitude=1000\n", buffer, maxStep);
        check(maxStep < 250, "A missing/returning audio block creates a full-amplitude click");
        check(output[0] == 1000 && output[1] == -1000 && previous == 1000,
              "Normal delivery remains attenuated after recovery");
        const auto& stats = gapState.audioDiagnostics;
        check(stats.Callbacks == 6 && stats.RequestedFrames == 6u * buffer &&
              stats.SuppliedFrames == 4u * buffer - 2 && stats.Underruns == 3 && stats.EmptyCallbacks == 2,
              "Audio diagnostics do not count the supplied/short/missing blocks");
    }

    // Compare actual output and retained state through cutoff changes, bypass,
    // mute and unmute. Fusing may change rounding by one output LSB.
    int maxDifference = 0;
    for (auto backend : {AudioLowPass::Backend::SSE2, AudioLowPass::Backend::FMA})
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        AudioLowPass reference, accelerated;
        reference.Init(rate, AudioLowPass::Backend::Scalar);
        accelerated.Init(rate, backend);
        u32 seed = 0x12345678;
        std::array<s16, 514> expected, actual;
        for (int block = 0; block < 300; ++block)
        {
            const double cutoff = block < 50 ? 20 : block < 100 ? 6000 :
                                  block < 200 ? reference.WideOpenCutoff() : 1000;
            expected.fill(poison);
            for (int i = 0; i < 512; ++i)
            {
                seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
                expected[i + 1] = block >= 250 ? 0 : static_cast<s16>(seed);
            }
            actual = expected;
            if (block >= 150 && block < 200)
            {
                reference.ProcessMuted(256, cutoff, 256 / rate);
                accelerated.ProcessMuted(256, cutoff, 256 / rate);
            }
            else
            {
                reference.Process(expected.data() + 1, 256, cutoff, 256 / rate);
                accelerated.Process(actual.data() + 1, 256, cutoff, 256 / rate);
            }
            for (int i = 1; i < 513; ++i)
                maxDifference = std::max(maxDifference, std::abs(int(expected[i]) - int(actual[i])));
            check(actual.front() == poison && actual.back() == poison, "FMA filter writes outside the stereo block");
            for (int channel = 0; channel < 2; ++channel)
            {
                const double a = reference.ProcessSample(0, channel);
                const double b = accelerated.ProcessSample(0, channel);
                check(std::isfinite(b) && std::abs(a - b) < 0.01, "FMA filter state drifts or becomes non-finite");
            }
        }
    }
    check(maxDifference <= 1, "FMA changes output by more than one 16-bit LSB");
    std::printf("FMA supported=%d; max stereo output difference=%d LSB\n",
                AudioLowPass::IsSupported(AudioLowPass::Backend::FMA), maxDifference);

    // Measure an actual stereo signal, not a duplicate of the filter equations.
    AudioLowPass filter;
    filter.Init(48000);
    filter.SetCutoffNow(6000);
    std::array<s16, 512 * 2> signal;
    double energy[2] = {};
    int measured = 0;
    const auto started = std::chrono::steady_clock::now();
    for (int block = 0; block < 1000; ++block)
    {
        for (int i = 0; i < 512; ++i)
        {
            const double phase = 2 * std::numbers::pi * (block * 512 + i) / 48000.0;
            signal[i * 2] = std::lround(10000 * std::sin(phase * 1000));
            signal[i * 2 + 1] = std::lround(10000 * std::sin(phase * 12000));
        }
        filter.Process(signal.data(), 512, 6000, 512 / 48000.0);
        if (block >= 10)
        {
            for (int i = 0; i < 512; ++i)
                for (int channel = 0; channel < 2; ++channel)
                    energy[channel] += double(signal[i * 2 + channel]) * signal[i * 2 + channel];
            measured += 512;
        }
    }
    const double passGain = std::sqrt(energy[0] / measured) / (10000 / std::sqrt(2.0));
    const double stopGain = std::sqrt(energy[1] / measured) / (10000 / std::sqrt(2.0));
    check(passGain > 0.97 && passGain < 1.02, "Low-pass filter damages the 1 kHz passband");
    check(stopGain < 0.05, "Low-pass filter does not attenuate 12 kHz or mixes stereo channels");
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("Low-pass 6kHz: 1kHz gain=%.6f 12kHz gain=%.6f; synthesis+filter %.3f ms / 10.67 s audio\n",
                passGain, stopGain, elapsed * 1000);
    return failures ? 1 : 0;
}
