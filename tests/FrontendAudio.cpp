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
    check(output[frames * 2 - 2] == 500 && output[frames * 2 - 1] == -500,
          "Underrun does not extend the last stereo sample");
    console.SPU.available = 0;
    run();
    check(std::all_of(output.begin(), output.begin() + frames * 2,
                      [](s16 sample) { return sample == 0; }), "Empty queue is not silent");
    console.SPU.available = 1024;
    state.audioMutedToggle = true;
    run();
    check(std::all_of(output.begin(), output.begin() + frames * 2,
                      [](s16 sample) { return sample == 0; }), "Muted audio is not silent");
    check(console.SPU.rateChanges == 0, "Device thread modifies the core resampler");

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
