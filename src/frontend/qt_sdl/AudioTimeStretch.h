// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Optional host processing. Configure/Reset/Clear require both threads stopped.
// SetSpeed/CanPush/Push/Drain run on the producer; Read runs on one audio callback.
// The callback never enters Rubber Band or allocates memory.
class AudioTimeStretch
{
public:
    static constexpr size_t BlockFrames = 1024;
    static constexpr size_t CapacityFrames = 16384;
    AudioTimeStretch();
    ~AudioTimeStretch();
    AudioTimeStretch(const AudioTimeStretch&) = delete;
    AudioTimeStretch& operator=(const AudioTimeStretch&) = delete;
    bool Configure(int rate, std::string& error);
    void Clear();
    bool Reset();
    bool SetSpeed(double speed);
    bool CanPush();
    bool Push(const int16_t* interleaved, size_t frames);
    void Drain();
    size_t Read(int16_t* interleaved, size_t frames);
    size_t QueuedFrames() const;
    size_t PendingFrames() const; // producer only; includes native ready output
    bool Healthy() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
