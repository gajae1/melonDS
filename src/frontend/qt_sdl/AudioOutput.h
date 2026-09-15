// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Output-device lifetime only. The emulator owns PCM generation and processing.
// Public lifetime operations are serialized by the UI with the producer stopped.
// Native Open/Close run on one owned thread (SDL/WASAPI COM ownership).
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
    // Retire the stopped output and prepare its replacement without blocking UI.
    // Until FinishReopen succeeds, client delivery stays unavailable/paused.
    bool BeginReopen(const Settings&, Callback, void* userdata, std::string& error);
    bool BeginClose(std::string& error);
    bool IsOpening() const;
    bool IsOpenReady() const;
    bool FinishReopen(std::string& error);
    // Routine Open/Close waits are bounded; callback drain and the final
    // destructor join still wait for native work. A failed Open/Close leaves the
    // request outstanding so the next call can still consume it; a false
    // return with "bounded wait" in the error names the native call that did
    // not finish. The limit is TeardownTimeoutMs (default 2000,
    // MELONDS_AUDIO_TIMEOUT_MS overrides, clamped to 100..60000).
    bool Close();
    bool Start(std::string& error);
    int TeardownTimeoutMs() const;
    // Waits until client callbacks finish. Native delivery fades to silence
    // without consuming source PCM; Close releases the device itself.
    void Stop();
    explicit operator bool() const;
    bool IsRunning() const;
    // Native failure is distinct from intentionally paused client delivery.
    bool NeedsRecovery() const;
    const Spec& GetSpec() const { return spec; }
    const Settings& GetSettings() const { return settings; }
    static std::vector<DeviceInfo> Enumerate(int backend, std::string& error);
private:
    struct Impl;
    struct Owner;
    std::unique_ptr<Owner> owner;
    std::unique_ptr<Impl> impl;
    Settings settings;
    Spec spec;
};
