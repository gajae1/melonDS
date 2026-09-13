// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Output-device lifetime only. The emulator owns PCM generation and processing.
// Open/Close/Enumerate run on the UI thread (SDL WASAPI COM ownership).
// Start/Stop run with the producer quiescent, never from the audio callback.
class AudioOutput
{
public:
    enum Backend { SDL = 0, WASAPIShared = 1 };
    struct Settings
    {
        int backend = SDL;
        std::string device; // empty = system default; WASAPI uses a stable endpoint ID
        int frames = 512;
        bool operator==(const Settings&) const = default;
    };
    struct DeviceInfo { std::string id, name; };
    struct Spec
    {
        int rate = 48000;
        int frames = 512; // actual callback/engine period, not physical latency
        int bufferFrames = 0; // native WASAPI capacity; 0 = not available
        std::string backend;
    };
    using Callback = void (*)(void*, uint8_t*, int);
    AudioOutput();
    ~AudioOutput();
    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;
    bool Open(const Settings&, Callback, void* userdata, std::string& error);
    void Close();
    bool Start(std::string& error);
    void Stop(); // waits until callbacks finish
    explicit operator bool() const;
    bool IsRunning() const;
    const Spec& GetSpec() const { return spec; }
    const Settings& GetSettings() const { return settings; }
    static std::vector<DeviceInfo> Enumerate(int backend, std::string& error);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
    Settings settings;
    Spec spec;
};
