// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdarg>
#include <cstring>
#include <future>
#include <limits>
#include <semaphore>
#include <stop_token>
#include <thread>
#include <string>
#include <SDL2/SDL.h>
#include "types.h"
#include "AudioLowPass.h"
#include "AudioOutputRamp.h"
#include "AudioDiagnostics.h"
#include "AudioClockCorrection.h"
#include "AudioOutput.h"
#include "AudioTimeStretch.h"
#include "Platform.h"
using namespace melonDS;
static std::binary_semaphore ownerStopTimedOut(0);
namespace melonDS::Platform
{
void Log(LogLevel, const char* format, ...)
{
    if (!std::strstr(format, "Audio %s exceeded")) return;
    va_list args;
    va_start(args, format);
    const char* call = va_arg(args, const char*);
    const bool ownerStop = std::strcmp(call, "control-thread stop") == 0;
    va_end(args);
    if (ownerStop) ownerStopTimedOut.release();
}
}

// Drive the production SDL callback with a deterministic sample producer.
// Guard samples catch writes past the requested device buffer.
struct SampleSource
{
    int available = 1024;
    int rateChanges = 0;
    int queuedFrames = 0;
    int historyResets = 0;
    double clockCorrection = 1;
    double clockStep = 0;
    bool acceptCorrection = true;
    int outputCapacity = 2047;
    u64 droppedFrames = 0;
    // Frontend-visible SPU queries. Queue getters may only run under the audio lock.
    mutable std::atomic<int> stepQueries{0}, queueQueries{0};
    bool SetOutputClockCorrection(double correction) { clockCorrection = correction; return acceptCorrection; }
    double GetOutputClockCorrectionStep() const { ++stepQueries; return clockStep; }
    u64 GetOutputDroppedFrames() const { ++queueQueries; return droppedFrames; }
    void ResetOutputHistory() { ++historyResets; queuedFrames = 0; }
    void SetOutputSampleRate(double) { ++rateChanges; }
    int GetOutputSize() const { ++queueQueries; return queuedFrames; }
    // Defaults to SPU's 2048-entry ring minus the kept-empty slot.
    int GetOutputCapacity() const { ++queueQueries; return outputCapacity; }
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
    AudioClockDelivery audioClockDelivery;
    AudioClockCorrection audioClockCorrection;
    std::atomic<bool> doAudioClockCorrection{false};
    std::atomic<AudioClockCorrection::Status> audioClockStatus{AudioClockCorrection::Status::Inactive};
    bool audioClockEligible = false;
    double audioClockStep = 0, audioClockFPS = 0;
    std::atomic<bool> doAudioSync{false};
    void audioResetClockCorrection();
    void audioPrepareClockCorrection(bool normalSpeed, double outputFPS);
    void audioFinishClockCorrection();

    std::atomic<int> audioLowPassCutoff{0};
    std::atomic<int> audioVolume{256};
    bool audioMutedByWindowFocus = false, audioMutedToggle = false, audioMutedByFastForward = false;
    SDL_mutex* audioSyncLock = SDL_CreateMutex();
    SDL_cond* audioSyncCond = SDL_CreateCond();
    AudioOutput audioDevice;
    bool audioStartRequested = false;
    AudioTimeStretch audioTimeStretch;
    bool audioTimeStretchEnabled = false;
    bool fakeRunning = false;
    bool audioIsRunning() const { return fakeRunning || audioDevice.IsRunning(); }
    ~AudioState() { audioDevice.Close(); SDL_DestroyCond(audioSyncCond); SDL_DestroyMutex(audioSyncLock); }
    static void audioCallback(void* data, Uint8* stream, int len);
    void audioSync(int frameSamples, std::stop_token stopToken = {});
    void audioPumpTimeStretch(int maxQueued);
    void audioSetSpeed(double speed);
    void audioTimeStretchFailed();
    bool audioOpenOutput(const AudioOutput::Settings& settings, std::string& error);
    bool audioSetOutput(const AudioOutput::Settings& requested, std::string& error);
    void audioUpdateOutputState(int previousRate);
    bool audioSetBufferSize(int frames, std::string& error)
    {
        auto settings = audioDevice.GetSettings(); settings.frames = frames;
        return audioSetOutput(settings, error);
    }
    void audioReportDiagnostics() {}
    void audioEnable();
    void audioStartPending();
    bool micStarted = false;
    void micOpen() {}
};
static int failedOpens = 0;
static bool negotiateRate = false;
static bool manualOutput = false, manualPaused = true;
static std::atomic<bool> outputDisconnected{false};
static bool blockOpen = false;
static std::binary_semaphore openEntered(0), releaseOpen(0);
static bool blockClose = false;
static std::binary_semaphore closeEntered(0), releaseClose(0);
static std::thread::id deviceOwner;
static int wrongCloseThread = 0;
static std::atomic<int> closedNative{0};
static SDL_AudioStatus OutputStatus(SDL_AudioDeviceID id)
{
    if (outputDisconnected) return SDL_AUDIO_STOPPED;
    return manualOutput ? (manualPaused ? SDL_AUDIO_PAUSED : SDL_AUDIO_PLAYING)
                        : SDL_GetAudioDeviceStatus(id);
}
static int enumerationCount = -2; // -2 uses the real SDL driver.
static int CountOutputs(int capture)
{
    return enumerationCount == -2 ? SDL_GetNumAudioDevices(capture) : enumerationCount;
}
static SDL_AudioSpec manualSpec{};
static SDL_AudioDeviceID OpenOutput(const char* name, int capture, const SDL_AudioSpec* desired,
                                   SDL_AudioSpec* obtained, int changes)
{
    if (blockOpen) { openEntered.release(); releaseOpen.acquire(); }
    if (failedOpens > 0)
    {
        --failedOpens;
        SDL_SetError("Injected output-open failure");
        return 0;
    }
    auto wanted = *desired;
    if (negotiateRate) wanted.freq = 44100;
    if (manualOutput) manualSpec = wanted;
    const auto id = SDL_OpenAudioDevice(name, capture, &wanted, obtained, changes);
    if (id) { outputDisconnected = false; deviceOwner = std::this_thread::get_id(); }
    return id;
}
static void CloseOutput(SDL_AudioDeviceID id)
{
    if (std::this_thread::get_id() != deviceOwner) ++wrongCloseThread;
    if (blockClose) { closeEntered.release(); releaseClose.acquire(); }
    SDL_CloseAudioDevice(id);
    ++closedNative;
}
static void PauseOutput(SDL_AudioDeviceID id, int paused)
{
    // Keep the real dummy device paused while this test drives its callback.
    if (manualOutput) manualPaused = paused != 0;
    else SDL_PauseAudioDevice(id, paused);
}
static std::counting_semaphore<> syncWaitEntered(0);
static int ObserveSyncWait(SDL_cond* cond, SDL_mutex* mutex, Uint32 timeout)
{
    syncWaitEntered.release();
    return SDL_CondWaitTimeout(cond, mutex, timeout);
}
#define SDL_OpenAudioDevice OpenOutput
#define SDL_PauseAudioDevice PauseOutput
#define SDL_GetNumAudioDevices CountOutputs
#define SDL_GetAudioDeviceStatus OutputStatus
#define SDL_CloseAudioDevice CloseOutput
#include "AudioOutput.cpp"
#undef SDL_CloseAudioDevice
#undef SDL_OpenAudioDevice
#undef SDL_PauseAudioDevice
#undef SDL_GetNumAudioDevices
#undef SDL_GetAudioDeviceStatus
#define EmuInstance AudioState
#include "audioCallback.inc"
#include "audioResetClockCorrection.inc"
#include "audioPrepareClockCorrection.inc"
#include "audioFinishClockCorrection.inc"
#define SDL_CondWaitTimeout ObserveSyncWait
#include "audioSync.inc"
#undef SDL_CondWaitTimeout
#include "audioOpenOutput.inc"
#include "audioSetOutput.inc"
#include "audioUpdateOutputState.inc"
#include "audioEnable.inc"
#include "audioStartPending.inc"
#include "audioPumpTimeStretch.inc"
#include "audioSetSpeed.inc"
#include "audioTimeStretchFailed.inc"
#undef EmuInstance

int main(int argc, char** argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--async-output") == 0)
    {
        if (SDL_AudioInit("dummy") != 0) return 2;
        bool passed = true;
        {
            AudioOutput output;
            std::string error;
            const auto silence = [](void*, uint8_t* bytes, int count) { std::memset(bytes, 0, count); };
            passed &= output.Open({AudioOutput::SDL, {}, 128}, silence, nullptr, error);
            passed &= deviceOwner != std::this_thread::get_id();
            blockOpen = true;
            passed &= output.BeginReopen({AudioOutput::SDL, {}, 256}, silence, nullptr, error);
            const bool entered = openEntered.try_acquire_for(std::chrono::seconds(2));
            passed &= entered && output.IsOpening() && !output.IsOpenReady() && !output && !output.IsRunning();
            passed &= !output.FinishReopen(error) && output.IsOpening();
            releaseOpen.release();
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!output.IsOpenReady() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            passed &= output.IsOpenReady() && output.FinishReopen(error);
            passed &= output.GetSettings().frames == 256 && !output.IsRunning();
            passed &= output.BeginClose(error);
            const auto closeDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (!output.IsOpenReady() && std::chrono::steady_clock::now() < closeDeadline) std::this_thread::yield();
            passed &= output.IsOpenReady() && !output.FinishReopen(error) && error.empty() && !output;
            passed &= output.BeginReopen({AudioOutput::SDL, {}, 128}, silence, nullptr, error);
            passed &= openEntered.try_acquire_for(std::chrono::seconds(2));
            // Destruction drains an outstanding native open before its callback
            // data or SDL can be released. The native API itself is not cancelled.
            std::jthread unblock([] { releaseOpen.release(); });
            output.Close();
            passed &= wrongCloseThread == 0 && !output.IsOpening() && !output;
        }
        SDL_AudioQuit();
        std::printf("Audio asynchronous replacement and native owner: %s\n", passed ? "passed" : "FAILED");
        return passed ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--bounded-teardown") == 0)
    {
        // The bound is read once per process; every phase below must see the
        // same short limit that the native owner thread applies.
#ifdef _WIN32
        _putenv_s("MELONDS_AUDIO_TIMEOUT_MS", "300");
#else
        setenv("MELONDS_AUDIO_TIMEOUT_MS", "300", 1);
#endif
        if (SDL_AudioInit("dummy") != 0) return 2;
        using namespace std::chrono;
        const auto elapsed = [](const auto& from) {
            return duration_cast<milliseconds>(steady_clock::now() - from).count();
        };
        bool passed = true;
        const auto phaseStart = steady_clock::now();
        long long phaseAElapsed = 0;
        const auto silence = [](void*, uint8_t* bytes, int count) { std::memset(bytes, 0, count); };
        {
            AudioOutput output;
            std::string error;
            const AudioOutput::Settings settings{AudioOutput::SDL, {}, 128};
            passed &= output.TeardownTimeoutMs() == 300;
            const auto firstOpen = steady_clock::now();
            passed &= output.Open(settings, silence, nullptr, error);
            std::printf("phase A open: %lld ms\n", (long long)elapsed(firstOpen));
            // A native close that outlives the bound must report a bounded
            // wait instead of blocking the caller for the whole call.
            blockClose = true;
            const auto closeStarted = steady_clock::now();
            passed &= !output.Close();
            const auto closeElapsed = elapsed(closeStarted);
            std::printf("first close: %lld ms\n", (long long)closeElapsed);
            passed &= closeElapsed >= 120 && closeElapsed < 1500;
            passed &= closeEntered.try_acquire_for(2s);
            std::printf("close entered after: %lld ms\n", (long long)elapsed(closeStarted));
            releaseClose.release();
            // The outstanding close finishes natively; the next call only
            // observes it and must not repeat the native release.
            const auto drainDeadline = steady_clock::now() + 2s;
            while (closedNative.load() < 1 && steady_clock::now() < drainDeadline) std::this_thread::yield();
            passed &= closedNative.load() >= 1;
            const auto drainStart = steady_clock::now();
            passed &= output.Close();
            std::printf("drain tail: %lld ms\n", (long long)elapsed(drainStart));
            passed &= elapsed(drainStart) < 500;
            passed &= !output.IsOpening() && !output;
            blockClose = false;
            phaseAElapsed = elapsed(closeStarted);
            std::printf("phase A scope end: %lld ms\n", (long long)elapsed(closeStarted));
        }
        std::printf("phase A destroyed: %lld -> %lld ms\n", (long long)phaseAElapsed, (long long)elapsed(phaseStart));
        std::printf("phase close-timeout: %lld ms\n", (long long)elapsed(phaseStart));
        const auto phaseB = steady_clock::now();
        {
            AudioOutput output;
            std::string error;
            const AudioOutput::Settings settings{AudioOutput::SDL, {}, 128};
            blockOpen = true;
            const auto openStarted = steady_clock::now();
            const bool opened = output.Open(settings, silence, nullptr, error);
            const auto openElapsed = elapsed(openStarted);
            passed &= !opened;
            passed &= openElapsed >= 120 && openElapsed < 1500;
            passed &= error.find("bounded wait") != std::string::npos;
            passed &= openEntered.try_acquire_for(2s);
            releaseOpen.release();
            // The outstanding open must still be usable and finish normally.
            const auto readyDeadline = steady_clock::now() + 2s;
            while (!output.IsOpenReady() && steady_clock::now() < readyDeadline) std::this_thread::yield();
            passed &= output.FinishReopen(error) && error.empty() && static_cast<bool>(output);
            blockOpen = false;
            output.Close();
        }
        std::printf("phase open-timeout: %lld ms\n", (long long)elapsed(phaseB));
        const auto phaseC = steady_clock::now();
        {
            std::string error;
            const AudioOutput::Settings settings{AudioOutput::SDL, {}, 128};
            const auto destroyStarted = steady_clock::now();
            {
                AudioOutput output;
                passed &= output.Open(settings, silence, nullptr, error);
                blockClose = true;
                output.Close(); // Times out; destruction still drains the thread.
                std::thread unblock([started = steady_clock::now()] {
                    std::this_thread::sleep_for(std::chrono::milliseconds(400));
                    (void)started;
                    releaseClose.release();
                });
                unblock.detach();
            }
            // Destruction keeps the join, so a stuck native close is drained
            // with a diagnostic before this scope is left.
            passed &= elapsed(destroyStarted) < 2000;
            blockClose = false;
            while (releaseClose.try_acquire()) {} // Leave the flag drained for later phases.
        }
        std::printf("phase destroy-drain: %lld ms\n", (long long)elapsed(phaseC));
        const auto phaseD = steady_clock::now();
        {
            // The product path behind the bound: a close times out and the very
            // next request is a new open. The retired device must still be
            // released on the control thread, and the deferred open must not be
            // lost when the caller submits against the pending handoff.
            AudioOutput output;
            std::string error;
            const AudioOutput::Settings settings{AudioOutput::SDL, {}, 128};
            passed &= output.Open(settings, silence, nullptr, error);
            blockClose = true;
            passed &= !output.Close();
            const bool reopened = output.Open(settings, silence, nullptr, error);
            if (!reopened) passed &= error.find("bounded wait") != std::string::npos;
            releaseClose.release();
            passed &= closeEntered.try_acquire_for(2s);
            if (!reopened)
            {
                const auto readyDeadline = steady_clock::now() + 2s;
                while (!output.IsOpenReady() && steady_clock::now() < readyDeadline) std::this_thread::yield();
                passed &= output.IsOpenReady() && output.FinishReopen(error);
            }
            // The retired device was released on the control thread before the
            // deferred open could report ready.
            passed &= static_cast<bool>(output) && closedNative.load() >= 1;
            blockClose = false;
            passed &= output.Close();
            passed &= wrongCloseThread == 0;
        }
        std::printf("phase reopen-after-close-timeout: %lld ms\n", (long long)elapsed(phaseD));
        // Destruction after a timed-out open must close the eventual device
        // on its native owner thread, not in the caller's future destructor.
        while (ownerStopTimedOut.try_acquire()) {}
        const auto beforePendingOpen = closedNative.load();
        std::atomic<bool> observedOwnerExit{false};
        {
            std::jthread unblock([&] {
                observedOwnerExit = ownerStopTimedOut.try_acquire_for(3s);
                releaseOpen.release();
            });
            {
                AudioOutput output;
                std::string error;
                blockOpen = true;
                passed &= output.BeginReopen({AudioOutput::SDL, {}, 128}, silence, nullptr, error);
                passed &= openEntered.try_acquire_for(2s);
                // Both bounded waits expire before unblock releases the API.
            }
        }
        blockOpen = false;
        const bool pendingOwnerCorrect = observedOwnerExit &&
            closedNative.load() == beforePendingOpen + 1 && wrongCloseThread == 0;
        passed &= pendingOwnerCorrect;
        std::printf("pending-open destructor: close count=%d, wrong owner=%d, exit observed=%d\n",
            closedNative.load() - beforePendingOpen, wrongCloseThread, int(observedOwnerExit.load()));
        SDL_AudioQuit();
        std::printf("Audio bounded teardown waits: %s\n", passed ? "passed" : "FAILED");
        return passed ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--device-loss") == 0)
    {
        if (SDL_AudioInit("dummy") != 0) return 2;
        bool passed = true;
        {
            Console console;
            AudioState state{&console};
            manualOutput = true;
            std::string error;
            passed &= state.audioSetBufferSize(128, error);
            console.SPU.queuedFrames = 512;
            state.audioEnable();
            passed &= state.audioDevice.IsRunning();
            outputDisconnected = true;
            if (state.audioDevice.IsRunning())
            {
                std::puts("disconnected output still reports running");
                passed = false;
            }
            // Selecting the same device must reopen a dead native handle.
            const auto requested = state.audioDevice.GetSettings();
            passed &= state.audioSetOutput(requested, error);
            passed &= !outputDisconnected && state.audioDevice.GetSettings() == requested;
            passed &= state.audioDevice.Start(error) && state.audioDevice.IsRunning();
        }
        manualOutput = false; outputDisconnected = false; SDL_AudioQuit();
        std::printf("audio device loss / same settings reopen: %s\n", passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }
    if (argc == 2 && std::strcmp(argv[1], "--clock-lifecycle") == 0)
    {
        using namespace std::chrono_literals;
        int failures = 0;
        const auto check = [&](bool ok, const char* message) {
            if (!ok) { ++failures; std::fprintf(stderr, "%s\n", message); }
        };
        constexpr double applied = 1.0004;
        constexpr double step = 0.0005;
        constexpr double quiet = std::numeric_limits<double>::quiet_NaN();
        constexpr double infinite = std::numeric_limits<double>::infinity();
        Console console;
        AudioState state{&console};
        state.audioLowPass.Init(state.audioFreq);
        state.audioOutputRamp.Init(state.audioFreq);
        std::array<s16, 2048> output;
        const auto reset = [&] {
            state.audioResetClockCorrection();
            state.doAudioClockCorrection = true;
            state.doAudioSync = false;
            state.audioTimeStretchEnabled = false;
            state.fakeRunning = true;
            state.audioClockDelivery = {};
            console.SPU.clockStep = step;
            console.SPU.acceptCorrection = true;
            console.SPU.outputCapacity = 2047;
            console.SPU.queuedFrames = 700;
            console.SPU.available = 96;
            console.SPU.clockCorrection = 1;
            console.SPU.historyResets = console.SPU.rateChanges = 0;
            console.SPU.stepQueries = console.SPU.queueQueries = 0;
        };
        const auto callback = [&](int frames) {
            AudioState::audioCallback(&state, reinterpret_cast<Uint8*>(output.data()), frames * 4);
        };

        // Every independent eligibility gate must stop a running correction and
        // restore unity without touching queued PCM or the resampler history.
        const char* names[] = {"mode", "filter", "option", "sync", "stretch", "output", "NaN fps", "zero fps", "infinite fps"};
        for (int gate = 0; gate < 9; ++gate)
        {
            const auto prepare = [&](bool open) {
                if (!open)
                {
                    if (gate == 1) console.SPU.clockStep = 0;
                    if (gate == 2) state.doAudioClockCorrection = false;
                    if (gate == 3) state.doAudioSync = true;
                    if (gate == 4) state.audioTimeStretchEnabled = true;
                    if (gate == 5) state.fakeRunning = false;
                }
                const double fps = !open && gate == 6 ? quiet : !open && gate == 7 ? 0 : !open && gate == 8 ? infinite : 60;
                state.audioPrepareClockCorrection(open || gate != 0, fps);
            };
            reset();
            prepare(false);
            check(!state.audioClockEligible && console.SPU.stepQueries == 1 && console.SPU.clockCorrection == 1,
                  names[gate]);
            console.SPU.clockCorrection = applied;
            prepare(false); // Never-eligible state must not rewrite an unrelated correction.
            check(!state.audioClockEligible && console.SPU.clockCorrection == applied, names[gate]);

            reset();
            prepare(true);
            check(state.audioClockEligible && state.audioClockStep == step && state.audioClockFPS == 60 &&
                  state.audioClockCorrection.GetStatus() == AudioClockCorrection::Status::Qualifying,
                  "Fully eligible correction did not start");
            console.SPU.clockCorrection = applied;
            prepare(true);
            check(state.audioClockEligible && console.SPU.clockCorrection == applied,
                  "Unchanged eligible frame restarted correction");
            prepare(false);
            const bool cleared = !state.audioClockEligible && state.audioClockStep == 0 && state.audioClockFPS == 0 &&
                console.SPU.clockCorrection == 1 &&
                state.audioClockStatus == AudioClockCorrection::Status::Inactive &&
                state.audioClockCorrection.GetStatus() == AudioClockCorrection::Status::Inactive;
            check(cleared, names[gate]);
            check(console.SPU.queuedFrames == 700 && console.SPU.historyResets == 0 && console.SPU.rateChanges == 0,
                  "Clock reset changed queued PCM or resampler history");
            state.audioFinishClockCorrection();
            check(console.SPU.queueQueries == 0, "Ineligible finish queried the SPU queue");
        }

        // A changed SPU step or output FPS is a new epoch. Finish also discards
        // a frame if the guest changes the mix rate while it runs.
        reset();
        state.audioPrepareClockCorrection(true, 60);
        console.SPU.clockStep = 0.001;
        console.SPU.clockCorrection = applied;
        state.audioPrepareClockCorrection(true, 60);
        check(state.audioClockEligible && state.audioClockStep == 0.001 && console.SPU.clockCorrection == 1,
              "Changed SPU step did not restart correction");
        console.SPU.clockCorrection = applied;
        state.audioPrepareClockCorrection(true, 50);
        check(state.audioClockEligible && state.audioClockFPS == 50 && console.SPU.clockCorrection == 1,
              "Changed output FPS did not restart correction");
        console.SPU.clockStep = step;
        console.SPU.clockCorrection = applied;
        console.SPU.queueQueries = 0;
        state.audioFinishClockCorrection();
        check(!state.audioClockEligible && console.SPU.clockCorrection == 1 && console.SPU.queueQueries == 0 &&
              console.SPU.queuedFrames == 700 && console.SPU.historyResets == 0,
              "Mid-frame SPU step change did not discard the observation and preserve PCM");

        reset();
        state.audioPrepareClockCorrection(true, 60);
        console.SPU.acceptCorrection = false;
        state.audioFinishClockCorrection();
        check(!state.audioClockEligible && console.SPU.clockCorrection == 1 && console.SPU.queuedFrames == 700,
              "Rejected correction did not reset to unity");

        // The production callback records requested/supplied frames only when
        // correction is enabled, under the same lock Finish later samples.
        reset();
        state.doAudioClockCorrection = false;
        callback(128);
        check(state.audioClockDelivery.callbacks == 0 && state.audioClockDelivery.requested == 0,
              "Disabled correction recorded callback delivery");
        state.doAudioClockCorrection = true;
        callback(128);
        console.SPU.available = 128;
        callback(128);
        const auto& delivery = state.audioClockDelivery;
        check(delivery.callbacks == 2 && delivery.requested == 256 && delivery.supplied == 224 &&
              delivery.lastFrames == 128 && delivery.lastTick != 0,
              "Callback did not record delivered frames");

        // A missed try-lock must not touch any SPU queue getter. The worker is
        // the only thread requesting the lock while this thread owns it. Capacity
        // is deliberately too small for a valid observation, so a locked finish
        // becomes Inactive but a missed lock stays Qualifying.
        reset();
        console.SPU.outputCapacity = 900;
        state.audioPrepareClockCorrection(true, 60);
        callback(128);
        console.SPU.queueQueries = 0;
        std::binary_semaphore started(0), finished(0);
        SDL_LockMutex(state.audioSyncLock);
        std::thread worker([&] {
            started.release();
            state.audioFinishClockCorrection();
            finished.release();
        });
        check(started.try_acquire_for(5s), "Clock worker did not start");
        const bool returned = finished.try_acquire_for(5s); // Failure means Finish blocked on the audio lock.
        check(returned, "Clock finish blocked on a contended audio lock");
        check(console.SPU.queueQueries == 0, "Clock finish read the SPU queue without the audio lock");
        check(state.audioClockEligible && console.SPU.clockCorrection == 1 &&
              state.audioClockStatus == AudioClockCorrection::Status::Qualifying,
              "Missed audio lock did not keep the previous qualification state");
        SDL_UnlockMutex(state.audioSyncLock);
        worker.join();
        SDL_Delay(2); // Guarantee a strictly later completed-frame timestamp on coarse clocks.
        state.audioFinishClockCorrection();
        check(console.SPU.queueQueries > 0 &&
              state.audioClockStatus == AudioClockCorrection::Status::Inactive,
              "Uncontended finish did not sample the callback and SPU queue");
        std::printf("audio clock correction lifecycle: %s\n", failures ? "FAIL" : "PASS");
        return failures ? 1 : 0;
    }
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
            check(device.audioBufSize == buffer && device.audioDevice.GetSettings().frames == buffer &&
                  !device.audioDevice.IsRunning(),
                  "Buffer change lost the requested size or resumed paused playback");
        }
        const int before = deviceConsole.SPU.historyResets;
        check(device.audioSetBufferSize(32, error) &&
              deviceConsole.SPU.historyResets == before, "Unchanged buffer reopened or cleared output");
        failedOpens = 1;
        check(!device.audioSetBufferSize(64, error) && !error.empty() && device.audioDevice &&
              device.audioDevice.GetSettings().frames == 32, "Failed reopen did not restore the previous output");
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
        deviceConsole.SPU.queuedFrames = 64;
        device.audioEnable();
        SDL_Delay(30);
        device.audioDevice.Stop();
        check(device.audioDiagnostics.Callbacks > 0, "Reopened running output did not deliver callbacks");
        const auto previous = device.audioDevice.GetSettings();
        check(!device.audioSetOutput({99, "unavailable", 64}, error) && !error.empty() &&
              device.audioDevice && device.audioDevice.GetSettings() == previous,
              "Unavailable backend lost the previous complete output settings");
        const auto outputs = AudioOutput::Enumerate(AudioOutput::SDL, error);
        check(error.empty() && !outputs.empty(), "Initialized dummy output was not enumerated");
        enumerationCount = 0;
        const auto defaultOnly = AudioOutput::Enumerate(AudioOutput::SDL, error);
        check(defaultOnly.size() == 1 && defaultOnly[0].id.empty() && error.empty(),
              "SDL zero-name enumeration incorrectly blocked default output");
        enumerationCount = -1;
        SDL_SetError("Unrelated stale SDL error");
        const auto unnamed = AudioOutput::Enumerate(AudioOutput::SDL, error);
        check(unnamed.size() == 1 && unnamed[0].id.empty() && error.empty(),
              "SDL driver without enumeration lost its default output");
        enumerationCount = -2;
        check(AudioOutput::Enumerate(99, error).empty() && !error.empty(),
              "Unsupported backend exposed a usable default");
        if (outputs.size() > 1)
        {
            const AudioOutput::Settings selected{AudioOutput::SDL, outputs[1].id, 64};
            check(device.audioSetOutput(selected, error) && device.audioDevice.GetSettings() == selected,
                  "Explicit output device was not applied");
            check(device.audioSetOutput(previous, error) && device.audioDevice.GetSettings() == previous,
                  "Previous output device was not restored");
        }
        std::printf("SDL output reopen: 32/64 frames, pause, rollback, recovery, rate and callbacks verified\n");
    }
    SDL_AudioQuit();
    {
        std::string error;
        check(AudioOutput::Enumerate(AudioOutput::SDL, error).empty() && !error.empty(),
              "Uninitialized audio exposed a usable default");
    }
    // Exercise the real SDL wait: a control request must not depend on the
    // 500ms starvation fallback, and a spurious wake must recheck the queue.
    using namespace std::chrono_literals;
    Console syncConsole;
    AudioState sync{&syncConsole};
    syncConsole.SPU.queuedFrames = 4096;
    sync.audioSync(800); // No device: no wait.
    sync.fakeRunning = true; // Only the sync predicate is substituted; waits are real SDL.
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
        check(stats.MaxProcessingTicks >= stats.MaxReadTicks,
              "Audio processing measurement excludes the source read");
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

    // Observe the final device buffer, after volume, recovery and low-pass DSP.
    // Backend silence during pause must not step directly from the last sample;
    // filter history must not bypass the transition when playback resumes.
    if (SDL_AudioInit("dummy") != 0) return 2;
    for (int cutoff : {0, 20, 6000})
    for (int buffer : {16, 128, 512})
    {
        manualOutput = true;
        manualPaused = true;
        Console resumeConsole;
        AudioState resume{&resumeConsole};
        resumeConsole.SPU.queuedFrames = 128;
        resume.audioLowPassCutoff = cutoff;
        resume.audioLowPass.Init(resume.audioFreq);
        resume.audioLowPass.SetCutoffNow(cutoff ? cutoff : resume.audioLowPass.WideOpenCutoff());
        resume.audioOutputRamp.Init(resume.audioFreq);
        resume.audioDiagnostics.Enabled = true;
        std::string error;
        check(resume.audioDevice.Open({AudioOutput::SDL, {}, 128}, AudioState::audioCallback, &resume, error),
              "Manual device open failed");
        const auto pump = [&] {
            output.fill(poison);
            if (manualPaused) std::fill_n(output.data(), buffer * 2, 0);
            else manualSpec.callback(manualSpec.userdata, reinterpret_cast<Uint8*>(output.data()), buffer * 4);
            check(std::all_of(output.begin() + buffer * 2, output.end(),
                              [](s16 sample) { return sample == poison; }), "Device transition writes past its buffer");
        };
        resume.audioEnable();
        for (int i = 0; i < 48000 / buffer; ++i) pump();
        check(output[0] == 1000 && output[1] == -1000, "Steady filtered input did not settle");
        check(resume.audioDevice.Start(error), "Idempotent device start failed");
        pump();
        check(output[0] == 1000 && output[1] == -1000, "Idempotent start fades continuous output");
        const auto calls = resume.audioDiagnostics.Callbacks;
        resume.audioDevice.Stop();
        int previous = 1000, maxStep = 0;
        const auto observe = [&] {
            for (int i = 0; i < buffer; ++i)
            {
                maxStep = std::max(maxStep, std::abs(int(output[i * 2]) - previous));
                previous = output[i * 2];
                check(std::abs(int(output[i * 2]) + output[i * 2 + 1]) <= 1,
                      "Device transition mixes stereo channels");
            }
        };
        for (int i = 0; i < 8; ++i) { pump(); observe(); }
        check(previous == 0 && resume.audioDiagnostics.Callbacks == calls && !resume.audioDevice.IsRunning(),
              "Paused output is not silent or consumes source PCM");
        resume.audioEnable();
        for (int i = 0; i < 48000 / buffer; ++i) { pump(); observe(); }
        std::printf("Device pause/resume cutoff=%d buffer=%d: largest step=%d / amplitude=1000\n", cutoff, buffer, maxStep);
        check(maxStep < 250, "Device pause/resume creates a full-amplitude step");
        check(previous == 1000 && resume.audioDiagnostics.Underruns == 0,
              "Resume remains attenuated or creates a source shortage");
        resume.audioDevice.Close();
        manualOutput = false;
    }
    SDL_AudioQuit();

    // A resumed empty source must not be consumed before emulation has had a
    // chance to produce PCM. Native delivery may continue with paused silence.
    if (SDL_AudioInit("dummy") != 0) return 2;
    {
        manualOutput = true;
        manualPaused = true;
        Console emptyConsole;
        emptyConsole.SPU.available = 0;
        AudioState empty{&emptyConsole};
        empty.audioLowPass.Init(empty.audioFreq);
        empty.audioOutputRamp.Init(empty.audioFreq);
        empty.audioDiagnostics.Enabled = true;
        std::string error;
        check(empty.audioDevice.Open({AudioOutput::SDL, {}, 128}, AudioState::audioCallback, &empty, error),
              "Empty-source device open failed");
        empty.audioEnable();
        check(!empty.audioDevice.IsRunning(), "Resume started consumption before the first PCM was produced");
        if (!manualPaused)
            manualSpec.callback(manualSpec.userdata, reinterpret_cast<Uint8*>(output.data()), 128 * 4);
        check(empty.audioDiagnostics.Callbacks == 0, "Empty resume consumed a source callback");
        emptyConsole.SPU.available = 128;
        emptyConsole.SPU.queuedFrames = 128;
        empty.audioStartPending();
        check(empty.audioDevice.IsRunning(), "Prepared PCM did not start pending playback");
        manualSpec.callback(manualSpec.userdata, reinterpret_cast<Uint8*>(output.data()), 128 * 4);
        check(empty.audioDiagnostics.SuppliedFrames == 128 && empty.audioDiagnostics.Underruns == 0 &&
              output[254] == 1000 && output[255] == -1000,
              "First prepared PCM was lost, attenuated indefinitely or replaced with silence");
        emptyConsole.SPU.available = 0;
        emptyConsole.SPU.queuedFrames = 0;
        empty.audioStartPending();
        manualSpec.callback(manualSpec.userdata, reinterpret_cast<Uint8*>(output.data()), 128 * 4);
        check(empty.audioDiagnostics.Underruns == 1, "Startup gate hid a later underrun");
        empty.audioDevice.Close();
        manualOutput = false;
    }
    SDL_AudioQuit();

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
