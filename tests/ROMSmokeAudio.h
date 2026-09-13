// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef ROMSMOKEAUDIO_H
#define ROMSMOKEAUDIO_H

#include <atomic>
#include <cmath>
#include <stop_token>
#include "AudioLowPass.h"
#include "AudioOutputRamp.h"
#include "AudioDiagnostics.h"

// Optional device-delivery probe using the production callback and sync method.
// The final device submission is silenced; this measures supply, not listening
// quality or physical output latency. No microphone or user settings are used.
struct SmokeAudio
{
    melonDS::NDS* nds;
    SDL_AudioDeviceID audioDevice = 0;
    SDL_mutex* audioSyncLock = SDL_CreateMutex();
    SDL_cond* audioSyncCond = SDL_CreateCond();
    int audioFreq = 48000, audioBufSize = 512;
    AudioLowPass audioLowPass;
    AudioOutputRamp audioOutputRamp;
    AudioDiagnostics audioDiagnostics;
    std::atomic<int> audioLowPassCutoff{0}, audioVolume{256};
    bool audioMutedByWindowFocus = false, audioMutedToggle = false, audioMutedByFastForward = false;
    bool Started = false;
    // This generic core smoke fixture exercises the default audio path. The
    // optional converter is covered by its native streaming tests separately.
    static constexpr bool audioTimeStretchEnabled = false;
    struct DisabledTimeStretch
    {
        size_t Read(int16_t*, size_t) { std::abort(); }
        size_t PendingFrames() const { std::abort(); }
    } audioTimeStretch;
    void audioPumpTimeStretch(int) {}
    bool audioIsRunning() const { return audioDevice && SDL_GetAudioDeviceStatus(audioDevice) == SDL_AUDIO_PLAYING; }
    double LastTime = 0, FrameLimitError = 0;

    ~SmokeAudio()
    {
        if (audioDevice) SDL_CloseAudioDevice(audioDevice);
        if (audioSyncCond) SDL_DestroyCond(audioSyncCond);
        if (audioSyncLock) SDL_DestroyMutex(audioSyncLock);
    }

    bool Open()
    {
        const char* buffer = std::getenv("MELONDS_SMOKE_AUDIO_BUFFER");
        if (!buffer) return true;
        const char* interpolation = std::getenv("MELONDS_SMOKE_AUDIO_INTERPOLATION");
        char* end = nullptr;
        const long requested = std::strtol(buffer, &end, 10);
        if (!*buffer || *end || (requested != 128 && requested != 256 && requested != 512 && requested != 1024))
            return false;
        long type = 0;
        if (interpolation)
        {
            type = std::strtol(interpolation, &end, 10);
            if (!*interpolation || *end || type < 0 || type > 4) return false;
        }
        if (!audioSyncLock || !audioSyncCond || SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
        SDL_AudioSpec wanted{}, obtained{};
        wanted.freq = audioFreq;
        wanted.format = AUDIO_S16SYS;
        wanted.channels = 2;
        wanted.samples = static_cast<Uint16>(requested);
        wanted.userdata = this;
        wanted.callback = [](void* data, Uint8* stream, int len) {
            audioCallback(data, stream, len);
            std::memset(stream, 0, len);
        };
        audioDevice = SDL_OpenAudioDevice(nullptr, 0, &wanted, &obtained, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (!audioDevice) return false;
        audioFreq = obtained.freq;
        audioBufSize = obtained.samples;
        audioLowPass.Init(audioFreq);
        audioOutputRamp.Init(audioFreq);
        audioDiagnostics.Enabled = true;
        nds->SPU.SetOutputSampleRate(audioFreq);
        nds->SPU.SetOutputSkew(60.0 / 59.8260982880808);
        nds->SPU.SetInterpolation(static_cast<melonDS::AudioInterpolation>(type));
        std::printf("audio_probe driver=%s requested_buffer=%ld buffer=%d rate=%d interpolation=%ld submission=silent\n",
            SDL_GetCurrentAudioDriver(), requested, audioBufSize, audioFreq, type);
        return true;
    }

    void AfterFrame(int lines)
    {
        if (!audioDevice) return;
        if (!Started)
        {
            Started = true;
            LastTime = Now();
            SDL_PauseAudioDevice(audioDevice, 0);
        }
        audioSync(static_cast<int>(std::ceil(audioFreq * lines / (60.0 * 263.0))));
        // Same delay/error policy as the frontend at 60 FPS. Qt events and
        // screen presentation are absent, so this is not full frontend timing.
        const double step = std::max(0.001, lines / (60.0 * 263.0));
        double now = Now();
        FrameLimitError = std::clamp(FrameLimitError + step - (now - LastTime), -step, step);
        const double delay = std::round(FrameLimitError * 1000);
        if (delay > 0)
        {
            SDL_Delay(static_cast<Uint32>(delay));
            const double after = Now();
            FrameLimitError -= after - now;
            now = after;
        }
        LastTime = now;
    }

    void Report()
    {
        if (!audioDevice) return;
        SDL_PauseAudioDevice(audioDevice, 1);
        const auto& d = audioDiagnostics;
        const double tickUs = 1e6 / SDL_GetPerformanceFrequency();
        std::printf("audio_delivery callbacks=%llu requested=%llu supplied=%llu missing=%llu underruns=%llu empty=%llu "
            "max_read_us=%.1f max_gap_us=%.1f core_dropped=%llu queue=%d\n",
            (unsigned long long)d.Callbacks, (unsigned long long)d.RequestedFrames,
            (unsigned long long)d.SuppliedFrames, (unsigned long long)(d.RequestedFrames - d.SuppliedFrames),
            (unsigned long long)d.Underruns, (unsigned long long)d.EmptyCallbacks,
            d.MaxReadTicks * tickUs, d.MaxGapTicks * tickUs,
            (unsigned long long)nds->SPU.GetOutputDroppedFrames(), nds->SPU.GetOutputSize());
    }

    static double Now() { return double(SDL_GetPerformanceCounter()) / SDL_GetPerformanceFrequency(); }
    void audioSync(int frameSamples, std::stop_token stopToken = {});
    static void audioCallback(void*, Uint8*, int);
};

using namespace melonDS;
#define EmuInstance SmokeAudio
#include "ROMSmokeAudioCallback.inc"
#include "ROMSmokeAudioSync.inc"
#undef EmuInstance
#endif
