// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioTimeStretch.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <limits>
#include <numbers>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

static void require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
static std::vector<int16_t> source()
{
    std::vector<int16_t> input(98304 * 2);
    for (size_t i = 0; i < input.size() / 2; ++i)
    {
        const double t = double(i) / 48000;
        input[i * 2] = input[i * 2 + 1] = static_cast<int16_t>(std::lrint(
            14000 * std::cos(2 * std::numbers::pi * 440 * t) + 3000 * std::sin(2 * std::numbers::pi * 880 * t)));
    }
    return input;
}
static void readSome(AudioTimeStretch& stretch, size_t wanted, std::vector<int16_t>& out)
{
    std::array<int16_t, 2052> samples;
    samples.fill(12345);
    const size_t n = stretch.Read(samples.data() + 2, wanted);
    require(n <= wanted && samples[0] == 12345 && samples[1] == 12345 &&
        samples[2 + n * 2] == 12345 && samples[3 + n * 2] == 12345, "Read exceeded returned frames");
    out.insert(out.end(), samples.begin() + 2, samples.begin() + 2 + n * 2);
}
static std::vector<int16_t> convert(double speed, size_t block, bool hold, bool dynamic = false)
{
    AudioTimeStretch stretch;
    std::string error;
    require(stretch.Configure(48000, error), "Configure failed");
    require(stretch.SetSpeed(speed), "SetSpeed failed");
    const auto input = source();
    std::vector<int16_t> output;
    size_t at = 0, full = 0;
    while (at < input.size() / 2)
    {
        if (!stretch.CanPush())
        {
            require(stretch.Healthy(), "Processing failed");
            ++full;
            readSome(stretch, 317, output);
            continue;
        }
        if (dynamic)
            require(stretch.SetSpeed(at < 32768 ? 1.0 : (at < 65536 ? 2.0 : 0.5)), "Dynamic speed failed");
        const size_t n = std::min(block, input.size() / 2 - at);
        require(stretch.Push(input.data() + at * 2, n), "Accepted input was rejected");
        at += n;
        require(stretch.QueuedFrames() <= AudioTimeStretch::CapacityFrames, "Output queue grew past capacity");
        if (!hold)
            while (stretch.QueuedFrames()) readSome(stretch, 127, output);
    }
    while (stretch.PendingFrames())
    {
        stretch.Drain();
        readSome(stretch, 317, output);
    }
    if (hold) require(full > 0, "Backpressure path was not exercised");
    for (size_t i = 0; i < output.size(); i += 2) require(output[i] == output[i + 1], "Identical stereo channels diverged");
    return output;
}
static void checkPitch(const std::vector<int16_t>& audio)
{
    require(audio.size() / 2 > 40000, "Too little output for pitch check");
    std::vector<double> crossings;
    for (size_t i = 8193; i < 32768; ++i)
    {
        const double a = audio[(i - 1) * 2], b = audio[i * 2];
        if (a <= 0 && b > 0) crossings.push_back(double(i - 1) - a / (b - a));
    }
    require(crossings.size() > 100, "Missing tone crossings");
    const double hz = 48000 * (crossings.size() - 1) / (crossings.back() - crossings.front());
    const double cents = 1200 * std::log2(hz / 440);
    std::printf("steady tone %.6f Hz, error %.4f cents\n", hz, cents);
    require(std::abs(cents) < 5, "Pitch error exceeded 5 cents");
}
int main(int argc, char** argv) try
{
    require(argc == 2, "case required");
    const std::string name = argv[1];
    if (name == "stream")
    {
        for (double speed : {0.5, 1.0, 2.0})
        {
            const auto a = convert(speed, 64, false);
            const auto b = convert(speed, 1024, false);
            require(a == b, "PCM changed with input packet size");
            checkPitch(a);
        }
        require(convert(1, 64, false, true) == convert(1, 1024, false, true), "Speed-change packet boundary changed PCM");
    }
    else if (name == "pressure")
    {
        require(convert(0.5, 1024, true) == convert(0.5, 1024, false), "Backpressure lost/reordered PCM");
        AudioTimeStretch stretch;
        std::string error;
        require(stretch.Configure(48000, error), "Configure failed");
        const auto input = source();
        const auto expected = convert(1, 1024, false);
        std::vector<int16_t> output;
        std::atomic<bool> finished{false};
        std::thread consumer([&] {
            while (!finished.load(std::memory_order_acquire) || stretch.QueuedFrames())
            {
                readSome(stretch, 63, output);
                std::this_thread::yield();
            }
        });
        for (size_t at = 0; at < input.size() / 2;)
        {
            if (stretch.CanPush())
            {
                if (!stretch.Push(input.data() + at * 2, 1024)) std::abort();
                at += 1024;
            }
            else std::this_thread::yield();
        }
        while (!stretch.CanPush()) std::this_thread::yield();
        finished.store(true, std::memory_order_release);
        consumer.join();
        require(output == expected, "Concurrent callback changed PCM");
    }
    else if (name == "reset")
    {
        AudioTimeStretch stretch;
        std::string error;
        require(stretch.Configure(48000, error), "Configure failed");
        const auto input = source();
        for (unsigned i = 0; i < 10; ++i) require(stretch.Push(input.data() + i * 1024 * 2, 1024), "Seed failed");
        const size_t queued = stretch.QueuedFrames();
        require(queued > 0, "No old output to preserve");
        require(!stretch.Configure(1, error) && !error.empty() && stretch.QueuedFrames() == queued && stretch.Healthy(), "Failed configure destroyed old output");
        require(!stretch.SetSpeed(0) && !stretch.SetSpeed(std::numeric_limits<double>::quiet_NaN()) && stretch.Healthy(), "Invalid ratio damaged converter");
        require(stretch.Reset() && stretch.QueuedFrames() == 0, "Reset retained output");
        std::array<int16_t, 2048> zero{};
        std::vector<int16_t> output;
        for (unsigned i = 0; i < 20; ++i)
        {
            require(stretch.Push(zero.data(), 1024), "Zero stream failed");
            while (stretch.QueuedFrames()) readSome(stretch, 64, output);
        }
        require(!output.empty() && std::all_of(output.begin(), output.end(), [](int16_t x) { return x == 0; }), "Old sound leaked after reset");
        stretch.Clear();
        require(!stretch.Healthy() && !stretch.QueuedFrames(), "Disabled converter kept output");
    }
    else require(false, "unknown case");
    std::puts("time stretch case passed");
}
catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); return 1; }
