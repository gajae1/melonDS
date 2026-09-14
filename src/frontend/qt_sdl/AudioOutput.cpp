// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioOutput.h"
#include "AudioOutputRamp.h"
#include <SDL2/SDL.h>
#include <atomic>
#include <bit>
#include <algorithm>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include "miniaudio/DeviceOnly.h"
#endif

struct AudioOutput::Impl
{
    SDL_AudioDeviceID sdl = 0;
    Callback callback = nullptr;
    void* userdata = nullptr;
    std::atomic<bool> running{false};
    static constexpr unsigned Paused = 1, InCallback = 2;
    std::atomic<unsigned> callbackState{Paused};
    std::atomic<bool> resumeTransition{false};
    AudioOutputRamp outputRamp;
#ifdef _WIN32
    ma_context context{};
    ma_device device{};
    bool contextReady = false, deviceReady = false;
    static void Render(ma_device* device, void* output, const void*, ma_uint32 frames)
    {
        RenderOutput(device->pUserData, static_cast<uint8_t*>(output), static_cast<int>(frames * 4));
    }
#endif
    static void RenderOutput(void* userdata, uint8_t* output, int bytes)
    {
        auto& self = *static_cast<Impl*>(userdata);
        auto* samples = reinterpret_cast<int16_t*>(output);
        const int frames = bytes / 4;
        unsigned expected = 0;
        if (!self.callbackState.compare_exchange_strong(expected, InCallback,
                std::memory_order_acquire, std::memory_order_relaxed))
        {
            self.outputRamp.Process(samples, 0, frames);
            return;
        }
        if (self.resumeTransition.exchange(false, std::memory_order_acquire))
            self.outputRamp.FadeFromLast();
        self.callback(self.userdata, output, bytes);
        self.outputRamp.Process(samples, frames, frames);
        if (self.callbackState.fetch_and(~InCallback, std::memory_order_release) & Paused)
            self.callbackState.notify_all();
    }
    void PauseCallbacks()
    {
        unsigned state = callbackState.fetch_or(Paused, std::memory_order_acq_rel) | Paused;
        while (state & InCallback)
        {
            callbackState.wait(state, std::memory_order_acquire);
            state = callbackState.load(std::memory_order_acquire);
        }
    }
    void ResumeCallbacks()
    {
        if (callbackState.load(std::memory_order_relaxed) & Paused)
        {
            resumeTransition.store(true, std::memory_order_release);
            callbackState.fetch_and(~Paused, std::memory_order_release);
        }
    }
#ifdef _WIN32
    static void Notify(const ma_device_notification* notification)
    {
        auto& self = *static_cast<Impl*>(notification->pDevice->pUserData);
        if (notification->type == ma_device_notification_type_started)
            self.running.store(true, std::memory_order_relaxed);
        else if (notification->type == ma_device_notification_type_stopped)
            self.running.store(false, std::memory_order_relaxed);
    }
#endif
    ~Impl()
    {
        PauseCallbacks();
        if (sdl) SDL_CloseAudioDevice(sdl);
#ifdef _WIN32
        if (deviceReady)
        {
            ma_device_uninit(&device);
        }
        if (contextReady) ma_context_uninit(&context);
#endif
    }
};

AudioOutput::AudioOutput() = default;
AudioOutput::~AudioOutput() = default;
AudioOutput::operator bool() const { return impl != nullptr; }
bool AudioOutput::IsRunning() const
{
    if (impl && (impl->callbackState.load(std::memory_order_acquire) & Impl::Paused))
        return false;
    return impl && impl->running.load(std::memory_order_relaxed) &&
        (!impl->sdl || SDL_GetAudioDeviceStatus(impl->sdl) == SDL_AUDIO_PLAYING);
}
bool AudioOutput::NeedsRecovery() const
{
    if (!impl) return true;
    // SDL updates enabled on removal, but this wrapper's last Start cannot
    // observe it. Querying status reads SDL's atomic enabled/paused flags.
    if (impl->sdl) return SDL_GetAudioDeviceStatus(impl->sdl) == SDL_AUDIO_STOPPED;
    return !impl->running.load(std::memory_order_relaxed);
}
void AudioOutput::Close() { impl.reset(); }

bool AudioOutput::Open(const Settings& requested, Callback callback, void* userdata, std::string& error)
{
    error.clear();
    if (impl) { error = "Audio output must be closed before opening"; return false; }
    auto output = std::make_unique<Impl>();
    output->callback = callback;
    output->userdata = userdata;
    auto next = requested;
    next.frames = std::bit_ceil(static_cast<unsigned>(std::clamp(next.frames, 32, 1024)));
    Spec obtained;
    if (next.backend == SDL)
    {
        SDL_AudioSpec wanted{}, actual{};
        wanted.freq = 48000;
        wanted.format = AUDIO_S16SYS;
        wanted.channels = 2;
        wanted.samples = next.frames;
        wanted.callback = Impl::RenderOutput;
        wanted.userdata = output.get();
        output->sdl = SDL_OpenAudioDevice(next.device.empty() ? nullptr : next.device.c_str(), 0,
            &wanted, &actual, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
        if (!output->sdl) { error = SDL_GetError(); return false; }
        obtained.rate = actual.freq;
        obtained.frames = actual.samples;
        output->outputRamp.Init(actual.freq);
        const char* driver = SDL_GetCurrentAudioDriver();
        obtained.backend = std::string("SDL / ") + (driver ? driver : "unknown");
    }
#ifdef _WIN32
    else if (next.backend == WASAPIShared)
    {
        const ma_backend backend = ma_backend_wasapi;
        auto result = ma_context_init(&backend, 1, nullptr, &output->context);
        if (result != MA_SUCCESS) { error = ma_result_description(result); return false; }
        output->contextReady = true;
        ma_device_id id{};
        if (!next.device.empty() && !MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
            next.device.c_str(), -1, id.wasapi, static_cast<int>(std::size(id.wasapi))))
        { error = "Invalid WASAPI output device ID"; return false; }
        auto config = ma_device_config_init(ma_device_type_playback);
        config.playback.pDeviceID = next.device.empty() ? nullptr : &id;
        config.playback.format = ma_format_s16;
        config.playback.channels = 2;
        config.playback.shareMode = ma_share_mode_shared;
        config.sampleRate = 0; // use the endpoint rate; SPU generates at this rate
        config.periodSizeInFrames = next.frames;
        config.performanceProfile = ma_performance_profile_low_latency;
        config.noFixedSizedCallback = MA_TRUE;
        config.noPreSilencedOutputBuffer = MA_TRUE; // callback always fills the complete buffer
        config.wasapi.noAutoConvertSRC = MA_TRUE; // permit IAudioClient3 shared engine periods
        config.dataCallback = Impl::Render;
        config.notificationCallback = Impl::Notify;
        config.pUserData = output.get();
        result = ma_device_init(&output->context, &config, &output->device);
        if (result != MA_SUCCESS) { error = ma_result_description(result); return false; }
        output->deviceReady = true;
        output->outputRamp.Init(output->device.sampleRate);
        // Validate start before accepting a new preference, including when the
        // emulator is paused. Keep the stream primed with silence until Start
        // opens client delivery; stopping here would empty the native buffer.
        result = ma_device_start(&output->device);
        if (result != MA_SUCCESS) { error = ma_result_description(result); return false; }
        obtained.rate = output->device.sampleRate;
        obtained.frames = output->device.playback.internalPeriodSizeInFrames;
        obtained.bufferFrames = output->device.wasapi.actualBufferSizeInFramesPlayback;
        obtained.backend = "WASAPI shared";
    }
#endif
    else { error = "Requested audio output backend is unavailable"; return false; }
    settings = std::move(next);
    spec = std::move(obtained);
    impl = std::move(output); // client delivery remains paused
    return true;
}

bool AudioOutput::Start(std::string& error)
{
    error.clear();
    if (!impl) { error = "Audio output unavailable"; return false; }
    if (impl->sdl)
    {
        if (SDL_GetAudioDeviceStatus(impl->sdl) == SDL_AUDIO_STOPPED)
        {
            impl->running.store(false, std::memory_order_relaxed);
            error = "Audio output device disconnected";
            return false;
        }
        impl->ResumeCallbacks();
        if (!impl->running.load(std::memory_order_relaxed))
        {
            SDL_PauseAudioDevice(impl->sdl, 0);
            impl->running.store(true, std::memory_order_relaxed);
        }
        return true;
    }
#ifdef _WIN32
    if (!impl->running.load(std::memory_order_relaxed))
    {
        const auto result = ma_device_start(&impl->device);
        if (result != MA_SUCCESS) { error = ma_result_description(result); return false; }
    }
    impl->ResumeCallbacks();
    // The notification owns running state so a device-loss stop cannot be overwritten here.
    return true;
#else
    return false;
#endif
}

void AudioOutput::Stop()
{
    if (!impl) return;
    // Retain native delivery so the final output can fade to silence without
    // consuming further source PCM. Close still stops/releases the device.
    impl->PauseCallbacks();
}

std::vector<AudioOutput::DeviceInfo> AudioOutput::Enumerate(int backend, std::string& error)
{
    error.clear();
    std::vector<DeviceInfo> result;
    if (backend == SDL)
    {
        if (!SDL_GetCurrentAudioDriver())
        { error = "SDL audio is not initialized"; return result; }
        const int count = SDL_GetNumAudioDevices(0);
        // SDL permits <= 0 when a driver cannot enumerate names but can still
        // open its default output. Only Open can determine whether it works.
        result.push_back({"", "System default"});
        for (int i = 0; i < count; ++i)
            if (const char* name = SDL_GetAudioDeviceName(i, 0)) result.push_back({name, name});
    }
#ifdef _WIN32
    else if (backend == WASAPIShared)
    {
        Impl owner;
        const ma_backend kind = ma_backend_wasapi;
        auto status = ma_context_init(&kind, 1, nullptr, &owner.context);
        if (status != MA_SUCCESS) { error = ma_result_description(status); return result; }
        owner.contextReady = true;
        ma_device_info* devices = nullptr;
        ma_uint32 count = 0;
        status = ma_context_get_devices(&owner.context, &devices, &count, nullptr, nullptr);
        if (status != MA_SUCCESS) { error = ma_result_description(status); return result; }
        if (!count)
        { error = "No WASAPI output devices are available"; return result; }
        result.push_back({"", "System default"});
        for (ma_uint32 i = 0; i < count; ++i)
        {
            char id[256]{};
            if (WideCharToMultiByte(CP_UTF8, 0, devices[i].id.wasapi, -1, id, sizeof(id), nullptr, nullptr))
                result.push_back({id, devices[i].name});
        }
    }
#endif
    else error = "Requested audio output backend is unavailable";
    return result;
}
