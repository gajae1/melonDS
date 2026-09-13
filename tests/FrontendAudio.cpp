// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <future>
#include <semaphore>
#include <stop_token>
#include <thread>
#include <string>
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
    int queuedFrames = 0;
    int historyResets = 0;
    void ResetOutputHistory() { ++historyResets; queuedFrames = 0; }
    void SetOutputSampleRate(double) { ++rateChanges; }
    int GetOutputSize() const { return queuedFrames; }
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
    int audioRequestedBuffer = 512;
    int audioFreq = 48000;
    AudioLowPass audioLowPass;
    AudioOutputRamp audioOutputRamp;
    AudioDiagnostics audioDiagnostics;
    std::atomic<int> audioLowPassCutoff{0};
    std::atomic<int> audioVolume{256};
    bool audioMutedByWindowFocus = false, audioMutedToggle = false, audioMutedByFastForward = false;
    SDL_mutex* audioSyncLock = SDL_CreateMutex();
    SDL_cond* audioSyncCond = SDL_CreateCond();
    SDL_AudioDeviceID audioDevice = 0;
    ~AudioState() { if (audioDevice > 1) SDL_CloseAudioDevice(audioDevice); SDL_DestroyCond(audioSyncCond); SDL_DestroyMutex(audioSyncLock); }
    static void audioCallback(void* data, Uint8* stream, int len);
    void audioSync(int frameSamples, std::stop_token stopToken = {});
    bool audioOpenOutput(int frames);
    bool audioSetBufferSize(int frames, std::string& error);
    void audioReportDiagnostics() {}
    void audioEnable();
    bool micStarted = false;
    void micOpen() {}
};
static int failedOpens = 0;
static bool negotiateRate = false;
static SDL_AudioDeviceID OpenOutput(const char* name, int capture, const SDL_AudioSpec* desired,
                                   SDL_AudioSpec* obtained, int changes)
{
    if (failedOpens > 0)
    {
        --failedOpens;
        SDL_SetError("Injected output-open failure");
        return 0;
    }
    auto wanted = *desired;
    if (negotiateRate) wanted.freq = 44100;
    return SDL_OpenAudioDevice(name, capture, &wanted, obtained, changes);
}
static std::counting_semaphore<> syncWaitEntered(0);
static int ObserveSyncWait(SDL_cond* cond, SDL_mutex* mutex, Uint32 timeout)
{
    syncWaitEntered.release();
    return SDL_CondWaitTimeout(cond, mutex, timeout);
}
#define EmuInstance AudioState
#include "audioCallback.inc"
#define SDL_CondWaitTimeout ObserveSyncWait
#include "audioSync.inc"
#undef SDL_CondWaitTimeout
#define SDL_OpenAudioDevice OpenOutput
#include "audioOpenOutput.inc"
#undef SDL_OpenAudioDevice
#include "audioSetBufferSize.inc"
#include "audioEnable.inc"
#undef EmuInstance

int main(int argc, char** argv)
{
    static_assert(int(AudioLowPass::Backend::Auto) == 0 &&
                  int(AudioLowPass::Backend::Scalar) == 1 &&
                  int(AudioLowPass::Backend::FMA) == 2 &&
                  int(AudioLowPass::Backend::SSE2) == 3);
    const bool requireNEON = argc == 2 && std::strcmp(argv[1], "--neon") == 0;
    if (requireNEON && !AudioLowPass::IsSupported(AudioLowPass::Backend::NEON))
    {
        std::fputs("NEON unavailable; frontend verification not run\n", stderr);
        return 77;
    }
    Console console;
    AudioState state{&console};
    state.audioLowPass.Init(state.audioFreq, requireNEON ? AudioLowPass::Backend::NEON :
                                                        AudioLowPass::Backend::Auto);
    state.audioOutputRamp.Init(state.audioFreq);
    constexpr s16 poison = 0x5555;
    constexpr int frames = 16;
    std::array<s16, 2048> output;
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::fprintf(stderr, "%s\n", message); }
    };
    // Reopen the real SDL dummy output using the production lifecycle methods.
    // Failure injection is confined to device creation; callbacks and pause /
    // close synchronization remain SDL's real implementation.
    if (SDL_AudioInit("dummy") != 0) return 2;
    {
        Console deviceConsole;
        AudioState device{&deviceConsole};
        std::string error;
        for (int buffer : {512, 64, 32})
        {
            check(device.audioSetBufferSize(buffer, error), "SDL buffer reopen failed");
            check(device.audioBufSize == buffer && device.audioRequestedBuffer == buffer &&
                  SDL_GetAudioDeviceStatus(device.audioDevice) == SDL_AUDIO_PAUSED,
                  "Buffer change lost the requested size or resumed paused playback");
        }
        const int before = deviceConsole.SPU.historyResets;
        const auto unchanged = device.audioDevice;
        check(device.audioSetBufferSize(32, error) && device.audioDevice == unchanged &&
              deviceConsole.SPU.historyResets == before, "Unchanged buffer reopened or cleared output");
        failedOpens = 1;
        check(!device.audioSetBufferSize(64, error) && !error.empty() && device.audioDevice &&
              device.audioRequestedBuffer == 32, "Failed reopen did not restore the previous output");
        failedOpens = 2;
        check(!device.audioSetBufferSize(64, error) && !device.audioDevice &&
              error.find("previous output") != std::string::npos,
              "Double open failure hid output loss or kept a stale device ID");
        negotiateRate = true;
        check(device.audioSetBufferSize(64, error) && device.audioFreq == 44100 &&
              deviceConsole.SPU.rateChanges == 1, "Device rate negotiation did not update the producer");
        negotiateRate = false;
        device.audioDiagnostics.Enabled = true;
        check(device.audioSetBufferSize(32, error), "Running output reopen failed");
        device.audioEnable();
        SDL_Delay(30);
        SDL_PauseAudioDevice(device.audioDevice, 1);
        check(device.audioDiagnostics.Callbacks > 0, "Reopened running output did not deliver callbacks");
        std::printf("SDL output reopen: 32/64 frames, pause, rollback, recovery, rate and callbacks verified\n");
    }
    SDL_AudioQuit();
    // Exercise the real SDL wait: a control request must not depend on the
    // 500ms starvation fallback, and a spurious wake must recheck the queue.
    using namespace std::chrono_literals;
    Console syncConsole;
    AudioState sync{&syncConsole};
    syncConsole.SPU.queuedFrames = 4096;
    sync.audioSync(800); // No device: no wait.
    sync.audioDevice = 1; // Only the predicate uses this ID; no device is opened.
    std::stop_source alreadyStopped;
    alreadyStopped.request_stop();
    sync.audioSync(800, alreadyStopped.get_token());
    check(!syncWaitEntered.try_acquire(), "No-device or cancelled sync entered a wait");
    for (bool consume : {false, true, false})
    {
        std::stop_source stop;
        syncConsole.SPU.queuedFrames = 4096;
        std::promise<void> complete;
        auto done = complete.get_future();
        std::thread waiter([&] { sync.audioSync(800, stop.get_token()); complete.set_value(); });
        const bool waiting = syncWaitEntered.try_acquire_for(200ms);
        check(waiting, "Audio sync did not wait above its existing threshold");
        if (waiting)
        {
            SDL_LockMutex(sync.audioSyncLock);
            SDL_CondSignal(sync.audioSyncCond); // Queue still high: must wait again.
            SDL_UnlockMutex(sync.audioSyncLock);
            check(syncWaitEntered.try_acquire_for(200ms), "Audio sync accepted a spurious wake");
        }
        if (consume)
        {
            SDL_LockMutex(sync.audioSyncLock);
            syncConsole.SPU.queuedFrames = 0;
            SDL_CondSignal(sync.audioSyncCond);
            SDL_UnlockMutex(sync.audioSyncLock);
        }
        else stop.request_stop();
        check(done.wait_for(200ms) == std::future_status::ready,
              "Audio control/consumption waited for the 500ms fallback");
        waiter.join();
        check(syncConsole.SPU.queuedFrames == (consume ? 0 : 4096), "Cancellation drained audio");
        while (syncWaitEntered.try_acquire()) {}
    }
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
    for (int buffer : {32, 64, 128, 512})
    {
        Console gapConsole;
        AudioState gapState{&gapConsole};
        gapState.audioLowPass.Init(gapState.audioFreq);
        gapState.audioOutputRamp.Init(gapState.audioFreq);
        gapState.audioDiagnostics.Enabled = true;
        int previous = 1000, maxStep = 0;
        for (int available : {buffer, buffer - 2, 0, 0, buffer, buffer, buffer})
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
        check(stats.Callbacks == 7 && stats.RequestedFrames == 7u * buffer &&
              stats.SuppliedFrames == 5u * buffer - 2 && stats.Underruns == 3 && stats.EmptyCallbacks == 2,
              "Audio diagnostics do not count the supplied/short/missing blocks");
    }

    // Unmuting a continuously supplied signal must recover from the silence
    // actually delivered, including when a callback is shorter than the ramp.
    for (auto mute : {&AudioState::audioMutedToggle, &AudioState::audioMutedByFastForward,
                      &AudioState::audioMutedByWindowFocus})
    for (int buffer : {16, 128, 512})
    {
        Console muteConsole;
        AudioState muteState{&muteConsole};
        muteState.audioLowPass.Init(muteState.audioFreq);
        muteState.audioOutputRamp.Init(muteState.audioFreq);
        muteState.audioDiagnostics.Enabled = true;
        AudioState::audioCallback(&muteState, reinterpret_cast<Uint8*>(output.data()), buffer * 4);
        muteState.*mute = true;
        output.fill(poison);
        AudioState::audioCallback(&muteState, reinterpret_cast<Uint8*>(output.data()), buffer * 4);
        check(std::all_of(output.begin(), output.begin() + buffer * 2,
                          [](s16 sample) { return sample == 0; }),
              "Muting does not immediately deliver silence");
        check(std::all_of(output.begin() + buffer * 2, output.end(),
                          [](s16 sample) { return sample == poison; }),
              "Muted callback writes beyond the device buffer");
        muteState.*mute = false;
        int previous = 0, maxStep = 0;
        for (int block = 0; block < 4; ++block)
        {
            AudioState::audioCallback(&muteState, reinterpret_cast<Uint8*>(output.data()), buffer * 4);
            for (int i = 0; i < buffer; ++i)
            {
                maxStep = std::max(maxStep, std::abs(int(output[2 * i]) - previous));
                previous = output[2 * i];
                check(std::abs(int(output[2 * i]) + output[2 * i + 1]) <= 1,
                      "Unmute recovery mixes stereo channels");
            }
        }
        std::printf("Audio unmute buffer=%d: largest step=%d / amplitude=1000\n", buffer, maxStep);
        check(maxStep < 250, "Unmuting creates a full-amplitude click");
        check(previous == 1000 && output[buffer * 2 - 1] == -1000,
              "Normal delivery remains attenuated after unmuting");
        const auto& stats = muteState.audioDiagnostics;
        check(stats.SuppliedFrames == stats.RequestedFrames && stats.Underruns == 0,
              "Intentional mute is counted as a source underrun");
    }

    // Compare actual output and retained state through cutoff changes, bypass,
    // mute and unmute. Fusing may change rounding by one output LSB.
    int maxDifference = 0;
    for (auto backend : {AudioLowPass::Backend::SSE2, AudioLowPass::Backend::FMA, AudioLowPass::Backend::NEON})
    for (double rate : {44100.0, 48000.0, 96000.0})
    {
        if (!AudioLowPass::IsSupported(backend)) continue;
        if (requireNEON && backend != AudioLowPass::Backend::NEON) continue;
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
            check(actual.front() == poison && actual.back() == poison, "SIMD filter writes outside the stereo block");
            for (int channel = 0; channel < 2; ++channel)
            {
                const double a = reference.ProcessSample(0, channel);
                const double b = accelerated.ProcessSample(0, channel);
                check(std::isfinite(b) && std::abs(a - b) < 0.01, "SIMD filter state drifts or becomes non-finite");
            }
        }
    }
    check(maxDifference <= 1, "SIMD changes output by more than one 16-bit LSB");
    std::printf("FMA supported=%d; NEON supported=%d; max stereo output difference=%d LSB\n",
                AudioLowPass::IsSupported(AudioLowPass::Backend::FMA),
                AudioLowPass::IsSupported(AudioLowPass::Backend::NEON), maxDifference);

    // Measure an actual stereo signal, not a duplicate of the filter equations.
    AudioLowPass filter;
    filter.Init(48000, requireNEON ? AudioLowPass::Backend::NEON : AudioLowPass::Backend::Auto);
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
