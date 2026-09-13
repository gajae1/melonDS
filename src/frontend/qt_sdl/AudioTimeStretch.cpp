// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioTimeStretch.h"
#include "rubberband/RubberBandStretcher.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <stdexcept>

using RB = RubberBand::RubberBandStretcher;
struct AudioTimeStretch::Impl
{
    RB processor;
    std::array<float, BlockFrames> left{}, right{}, outputLeft{}, outputRight{};
    std::array<int16_t, CapacityFrames * 2> output{};
    std::atomic<uint64_t> read{0}, write{0};
    size_t discard = 0;
    double speed = 1.0;
    bool healthy = true; // producer/configuration only

    explicit Impl(int rate) : processor(rate, 2,
        RB::OptionProcessRealTime | RB::OptionEngineFiner | RB::OptionWindowShort |
        RB::OptionThreadingNever | RB::OptionChannelsTogether)
    {
        processor.setMaxProcessSize(BlockFrames);
        Reset();
    }
    void Reset()
    {
        processor.reset();
        processor.setTimeRatio(1.0 / speed);
        read.store(0, std::memory_order_relaxed);
        write.store(0, std::memory_order_relaxed);
        discard = processor.getStartDelay();
        left.fill(0);
        right.fill(0);
        const float* input[] = {left.data(), right.data()};
        // Known startup silence and output compensation follow the library's
        // real-time contract. Actual source input is still needed before sound.
        size_t pad = processor.getPreferredStartPad();
        while (pad)
        {
            const size_t n = std::min(pad, BlockFrames);
            processor.process(input, n, false);
            pad -= n;
        }
        healthy = true;
    }
    size_t Queued() const
    {
        const auto r = read.load(std::memory_order_acquire);
        const auto w = write.load(std::memory_order_acquire);
        return static_cast<size_t>(w - r);
    }
    void Drain()
    {
        float* target[] = {outputLeft.data(), outputRight.data()};
        while (processor.available() > 0)
        {
            const auto w = write.load(std::memory_order_relaxed);
            const auto r = read.load(std::memory_order_acquire);
            const size_t free = CapacityFrames - static_cast<size_t>(w - r);
            if (!free && !discard) return;
            const size_t wanted = std::min({BlockFrames, free + discard,
                                           static_cast<size_t>(processor.available())});
            const size_t count = processor.retrieve(target, wanted);
            if (!count) throw std::runtime_error("Time stretch returned no ready samples");
            const size_t skip = std::min(discard, count);
            discard -= skip;
            for (size_t i = skip; i < count; ++i)
            {
                const size_t index = static_cast<size_t>(w + i - skip) % CapacityFrames * 2;
                const float l = outputLeft[i], rch = outputRight[i];
                if (!std::isfinite(l) || !std::isfinite(rch))
                    throw std::runtime_error("Time stretch returned invalid samples");
                output[index] = static_cast<int16_t>(std::lrint(std::clamp(l * 32768.0f, -32768.0f, 32767.0f)));
                output[index + 1] = static_cast<int16_t>(std::lrint(std::clamp(rch * 32768.0f, -32768.0f, 32767.0f)));
            }
            write.store(w + count - skip, std::memory_order_release);
        }
    }
};

AudioTimeStretch::AudioTimeStretch() = default;
AudioTimeStretch::~AudioTimeStretch() = default;
bool AudioTimeStretch::Configure(int rate, std::string& error)
{
    error.clear();
    if (rate < 8000 || rate > 192000)
    {
        error = "Pitch-preserving output requires a device rate between 8000 and 192000 Hz";
        return false;
    }
    try
    {
        auto next = std::make_unique<Impl>(rate);
        impl = std::move(next);
        return true;
    }
    catch (const std::exception& e) { error = e.what(); return false; }
}
void AudioTimeStretch::Clear() { impl.reset(); }
bool AudioTimeStretch::Reset()
{
    if (!impl) return false;
    try { impl->Reset(); return true; }
    catch (...) { impl->healthy = false; return false; }
}
bool AudioTimeStretch::SetSpeed(double speed)
{
    if (!Healthy() || !std::isfinite(speed) || speed <= 0 || !std::isfinite(1.0 / speed)) return false;
    if (impl->speed == speed) return true;
    try
    {
        impl->processor.setTimeRatio(1.0 / speed);
        impl->speed = speed;
        return true;
    }
    catch (...) { impl->healthy = false; return false; }
}
void AudioTimeStretch::Drain()
{
    if (!Healthy()) return;
    try { impl->Drain(); }
    catch (...) { impl->healthy = false; }
}
bool AudioTimeStretch::CanPush()
{
    Drain();
    return Healthy() && impl->processor.available() <= 0 && impl->Queued() < CapacityFrames;
}
bool AudioTimeStretch::Push(const int16_t* input, size_t frames)
{
    if (!input || frames > BlockFrames || !CanPush()) return false;
    if (!frames) return true;
    try
    {
        for (size_t i = 0; i < frames; ++i)
        {
            impl->left[i] = input[i * 2] / 32768.0f;
            impl->right[i] = input[i * 2 + 1] / 32768.0f;
        }
        const float* channels[] = {impl->left.data(), impl->right.data()};
        impl->processor.process(channels, frames, false);
        impl->Drain();
        return true;
    }
    catch (...) { impl->healthy = false; return false; }
}
size_t AudioTimeStretch::Read(int16_t* output, size_t frames)
{
    if (!impl || !output) return 0;
    const auto r = impl->read.load(std::memory_order_relaxed);
    const auto w = impl->write.load(std::memory_order_acquire);
    frames = std::min(frames, static_cast<size_t>(w - r));
    const size_t index = static_cast<size_t>(r) % CapacityFrames;
    const size_t first = std::min(frames, CapacityFrames - index);
    std::memcpy(output, impl->output.data() + index * 2, first * 2 * sizeof(int16_t));
    if (frames > first) std::memcpy(output + first * 2, impl->output.data(), (frames - first) * 2 * sizeof(int16_t));
    impl->read.store(r + frames, std::memory_order_release);
    return frames;
}
size_t AudioTimeStretch::QueuedFrames() const { return impl ? impl->Queued() : 0; }
size_t AudioTimeStretch::PendingFrames() const
{
    return Healthy() ? impl->Queued() + static_cast<size_t>(std::max(0, impl->processor.available())) : 0;
}
bool AudioTimeStretch::Healthy() const { return impl && impl->healthy; }
