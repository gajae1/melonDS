// SPDX-License-Identifier: GPL-3.0-or-later
#include <SDL3/SDL.h>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include "Platform.h"
namespace melonDS::Platform { void Log(LogLevel, const char*, ...) {} }

// Ordinary allocation counter, active only on the thread and window that set it.
static thread_local bool trackAllocs = false;
static thread_local unsigned trackedAllocs = 0;
static void* CountedAlloc(std::size_t size)
{
    if (trackAllocs) ++trackedAllocs;
    if (void* p = std::malloc(size ? size : 1)) return p;
    throw std::bad_alloc();
}
void* operator new(std::size_t size) { return CountedAlloc(size); }
void* operator new[](std::size_t size) { return CountedAlloc(size); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

static bool failOpen = false;
static std::string observedHint;
static SDL_AudioStreamCallback capturedCallback = nullptr;
static void* capturedUserdata = nullptr;
static SDL_AudioStream* capturedStream = nullptr;
static SDL_AudioStream* OpenStream(SDL_AudioDeviceID device, const SDL_AudioSpec* spec,
                                   SDL_AudioStreamCallback callback, void* userdata)
{
    const char* hint = SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
    observedHint = hint ? hint : "";
    if (failOpen) { SDL_SetError("injected stream-open failure"); return nullptr; }
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(device, spec, callback, userdata);
    capturedCallback = callback;
    capturedUserdata = userdata;
    capturedStream = stream;
    return stream;
}
// Manual mode leaves the dummy stream physically paused so no device thread
// competes with the captured callback, and collects PCM in fixed storage.
static bool manualResume = false, capturing = false;
alignas(int16_t) static uint8_t putStorage[1 << 16];
static int putCalls = 0, putBytes = 0;
static bool putOverflow = false;
static bool ResumeStream(SDL_AudioStream* stream)
{
    return manualResume ? true : SDL_ResumeAudioStreamDevice(stream);
}
static bool PutStream(SDL_AudioStream* stream, const void* data, int bytes)
{
    if (!capturing) return SDL_PutAudioStreamData(stream, data, bytes);
    ++putCalls;
    if (bytes < 0 || bytes > static_cast<int>(sizeof(putStorage)) - putBytes) { putOverflow = true; return false; }
    std::memcpy(putStorage + putBytes, data, bytes);
    putBytes += bytes;
    return true;
}
static bool SetHint(const char* name, const char* value)
{
    const bool result = SDL_SetHint(name, value);
    SDL_SetError("hint write replaced the error");
    return result;
}
// Post-open failure injection. The pre-open device-format query passes a null
// sample_frames and must stay normal; only the post-open query (non-null) or a
// missing bound logical device is sabotaged here.
static bool failLogicalDevice = false;
static bool failDeviceFormat = false;
static int openedFramesOverride = -1;
static SDL_AudioDeviceID StreamDevice(SDL_AudioStream* stream)
{
    if (failLogicalDevice) return 0;
    return SDL_GetAudioStreamDevice(stream);
}
static bool DeviceFormat(SDL_AudioDeviceID device, SDL_AudioSpec* spec, int* sample_frames)
{
    if (sample_frames)
    {
        if (failDeviceFormat) { SDL_SetError("injected device-format failure"); return false; }
        if (openedFramesOverride >= 0)
        {
            if (!SDL_GetAudioDeviceFormat(device, spec, nullptr)) return false;
            *sample_frames = openedFramesOverride;
            return true;
        }
    }
    return SDL_GetAudioDeviceFormat(device, spec, sample_frames);
}
static int destroyCalls = 0;
static void DestroyStream(SDL_AudioStream* stream) { ++destroyCalls; SDL_DestroyAudioStream(stream); }
#define SDL_OpenAudioDeviceStream OpenStream
#define SDL_SetHint SetHint
#define SDL_GetAudioStreamDevice StreamDevice
#define SDL_GetAudioDeviceFormat DeviceFormat
#define SDL_DestroyAudioStream DestroyStream
#define SDL_ResumeAudioStreamDevice ResumeStream
#define SDL_PutAudioStreamData PutStream
#include "AudioOutput.cpp"
#undef SDL_PutAudioStreamData
#undef SDL_ResumeAudioStreamDevice
#undef SDL_DestroyAudioStream
#undef SDL_GetAudioDeviceFormat
#undef SDL_GetAudioStreamDevice
#undef SDL_SetHint
#undef SDL_OpenAudioDeviceStream

static int producerCalls = 0, producerBytes = 0;
static uint32_t producerFrame = 0;
// Left ramps up from 1; right is its negation, so swapped or shifted channels show.
static void PatternProducer(void*, uint8_t* data, int bytes)
{
    ++producerCalls;
    producerBytes += bytes;
    auto* samples = reinterpret_cast<int16_t*>(data);
    for (int i = 0; i < bytes / 4; ++i)
    {
        const int16_t value = static_cast<int16_t>((producerFrame++ & 0x3fff) + 1);
        samples[2 * i] = value;
        samples[2 * i + 1] = static_cast<int16_t>(-value);
    }
}

struct Drive { unsigned allocs; int producerCalls, producerBytes, putCalls, putBytes; bool pcmOk; };
// One captured SDL request. Allocations are counted only across the callback.
static Drive DriveRequest(int request, int warmupFrames)
{
    producerCalls = producerBytes = putCalls = putBytes = 0;
    putOverflow = false;
    const uint32_t first = producerFrame;
    trackedAllocs = 0;
    trackAllocs = true;
    capturing = true;
    capturedCallback(capturedUserdata, capturedStream, request, 0);
    capturing = false;
    trackAllocs = false;
    Drive result{trackedAllocs, producerCalls, producerBytes, putCalls, putBytes, !putOverflow};
    const auto* pcm = reinterpret_cast<const int16_t*>(putStorage);
    for (int i = 0; i < putBytes / 4; ++i)
    {
        const int left = pcm[2 * i], right = pcm[2 * i + 1];
        if (left < 0 || right != -left) result.pcmOk = false; // holds through the resume ramp
        if (i >= warmupFrames && left != static_cast<int>(((first + i) & 0x3fff) + 1)) result.pcmOk = false;
    }
    return result;
}

static bool TestFirstCallbackAllocation()
{
    bool passed = true;
    AudioOutput output;
    std::string error;
    SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, nullptr);
    manualResume = true;
    passed &= output.Open({AudioOutput::SDL, {}, 256}, PatternProducer, nullptr, error);
    passed &= capturedCallback && capturedUserdata && capturedStream;
    passed &= output.Start(error);
    const int frames = output.GetSpec().frames;
    const int period = frames * 4;
    if (!passed || frames <= 0 || period * 2 > static_cast<int>(sizeof(putStorage)))
    {
        manualResume = false;
        output.Close();
        return false;
    }

    const Drive first = DriveRequest(period, 64);
    const Drive repeat = DriveRequest(period, 0);
    std::printf("first-callback allocations: first=%u repeat=%u (frames=%d)\n", first.allocs, repeat.allocs, frames);
    for (const Drive& d : {first, repeat})
    {
        passed &= d.allocs == 0 && d.producerCalls == 1 && d.producerBytes == period;
        passed &= d.putCalls == 1 && d.putBytes == period && d.pcmOk;
    }

    for (int ignored : {0, 1, 3, -1})
    {
        const Drive d = DriveRequest(ignored, 0);
        passed &= d.producerCalls == 0 && d.putCalls == 0 && d.putBytes == 0;
    }
    // A partial trailing frame is dropped; the whole frames go out in one callback.
    const Drive partial = DriveRequest(period + 3, 0);
    passed &= partial.producerCalls == 1 && partial.producerBytes == period;
    passed &= partial.putCalls == 1 && partial.putBytes == period && partial.pcmOk;
    // A larger request may allocate to grow, but is still one producer callback.
    const Drive larger = DriveRequest(period * 2, 0);
    passed &= larger.producerCalls == 1 && larger.producerBytes == period * 2;
    passed &= larger.putCalls == 1 && larger.putBytes == period * 2 && larger.pcmOk;

    output.Stop();
    passed &= output.Close();
    manualResume = false;
    return passed;
}

int main()
{
    if (!SDL_Init(SDL_INIT_AUDIO)) return 2;
    bool passed = true;
    {
        AudioOutput output;
        std::string error;
        auto silent = [](void*, uint8_t* data, int bytes) { std::memset(data, 0, bytes); };
        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, nullptr);
        passed &= output.Open({AudioOutput::SDL, {}, 128}, silent, nullptr, error);
        passed &= observedHint == "128" && !SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
        passed &= output.Close();

        SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "768");
        for (bool fail : {true, false})
        {
            failOpen = fail;
            passed &= output.Open({AudioOutput::SDL, {}, 256}, silent, nullptr, error) == !fail;
            const char* hint = SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
            passed &= observedHint == "256" && hint && std::strcmp(hint, "768") == 0;
            if (fail) passed &= error == "injected stream-open failure";
            passed &= output.Close();
        }

        // Post-open failures must fail the open instead of reporting the
        // requested frames as the negotiated period, and each retired stream
        // is destroyed exactly once by the local Impl on the owner thread.
        for (int mode = 0; mode < 3; ++mode)
        {
            failLogicalDevice = mode == 0;
            failDeviceFormat = mode == 1;
            openedFramesOverride = mode == 2 ? 0 : -1;
            destroyCalls = 0;
            passed &= !output.Open({AudioOutput::SDL, {}, 256}, silent, nullptr, error);
            passed &= !output;
            passed &= destroyCalls == 1;
            const char* hint = SDL_GetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES);
            passed &= observedHint == "256" && hint && std::strcmp(hint, "768") == 0;
            if (mode == 0) passed &= error == "SDL3 stream opened without a bound logical audio device";
            if (mode == 1) passed &= error == "injected device-format failure";
            if (mode == 2) passed &= error == "SDL3 opened audio device reported an invalid period";
            failLogicalDevice = failDeviceFormat = false;
            openedFramesOverride = -1;
            passed &= output.Open({AudioOutput::SDL, {}, 256}, silent, nullptr, error);
            passed &= static_cast<bool>(output) && output.GetSpec().frames > 0;
            passed &= output.Close();
            passed &= destroyCalls == 2;
        }
    }
    passed &= TestFirstCallbackAllocation();
    SDL_Quit();
    std::printf("SDL3 output hint isolation, failure preservation, first-callback allocation: %s\n", passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
