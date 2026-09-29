// SPDX-License-Identifier: GPL-3.0-or-later
// Real SPU/Sinc production and ring delivery; only host event timing is virtual.
#include "NDS.h"
#include "AudioClockCorrection.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
using namespace melonDS;
static void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
struct Scenario
{
    const char* name;
    double ppm = 0;
    unsigned mixInterval = 1024;
    double rate = 48000;
    double producerJitter = 0, callbackJitter = 0;
    bool stall = false, callbackStall = false;
    double speed = 1;
    double seconds = 120;
};
static void Run(const Scenario& s, bool trace)
{
    NDSArgs args;
    args.JIT.reset(); args.Interpolation = AudioInterpolation::Sinc;
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    auto& spu = nds->SPU;
    spu.SetOutputSampleRate(s.rate);
    spu.SetOutputSkew(60.0 / 59.8260982880808);
    spu.SetSampleRate(s.mixInterval == 704 ? AudioSampleRate::_47KHz : AudioSampleRate::_32KHz);
    spu.DrainOutput();
    AudioClockCorrection control;
    control.Reset(s.rate, 60);
    AudioClockDelivery delivery;
    constexpr double clock = 1e9;
    constexpr unsigned block = 480;
    std::array<s16, block * 2> pcm{};
    const double period = block / (s.rate * (1 + s.ppm * 1e-6));
    uint64_t frame = 0, callback = 1, guest = 0, nextMix = 0;
    bool firstMix = true, resumed = false;
    double correction = 1, nextFrame = 0, nextCallback = period;
    double minCorrection = 1, maxCorrection = 1;
    uint64_t tailMissing = 0, tailDropped = 0;
    bool tailStarted = false;
    auto callbackTime = [&] {
        double t = callback * period + s.callbackJitter * std::sin(callback * 1.7);
        if (s.callbackStall && t >= 30 && t < 30.1) t = 30.1;
        return t;
    };
    auto frameTime = [&] {
        return frame / (60 * s.speed) + s.producerJitter * std::sin(frame * 0.9);
    };
    while (std::min(nextFrame, nextCallback) <= s.seconds)
    {
        if (nextCallback <= nextFrame)
        {
            const auto got = spu.ReadOutput(pcm.data(), block);
            delivery.Record(block, got, uint64_t(std::llround(nextCallback * clock)), clock, s.rate);
            ++callback; nextCallback = callbackTime();
            continue;
        }
        // Both sides of production use the same integer performance counter.
        // Do not compare a rounded callback tick with an unrounded frame time.
        const double now = std::llround(nextFrame * clock) / clock;
        ++frame; nextFrame = frameTime();
        if (s.stall && now >= 30 && now < 30.1) continue;
        const uint64_t target = guest + 560190;
        while (nextMix <= target)
        {
            spu.Mix(firstMix ? 0 : s.mixInterval / 2);
            firstMix = false;
            nextMix += s.mixInterval;
        }
        guest = target;
        spu.BufferAudio();
        AudioClockCorrection::Observation o{
            delivery.lastTick / clock, delivery.lastFrames, delivery.requested,
            delivery.supplied, spu.GetOutputDroppedFrames(), delivery.gapSerial,
            spu.GetOutputSize(), spu.GetOutputCapacity()};
        const auto before = control.GetStatus();
        correction = control.Update(now, &o);
        if (trace && before == AudioClockCorrection::Status::Active && before != control.GetStatus())
            std::printf("restart t=%.12f callback=%.12f future_ns=%.3f q=%d missing=%llu\n", now, o.callbackTime,
                (o.callbackTime-now)*1e9, o.queued, (unsigned long long)(o.requested-o.supplied));
        Require(spu.SetOutputClockCorrection(correction), "eligible Sinc rejected controller correction");
        minCorrection = std::min(minCorrection, correction);
        maxCorrection = std::max(maxCorrection, correction);
        if (s.stall && now >= 30.1 && !resumed)
        {
            Require(correction == 1, "Producer stall was incorporated as drift debt");
            resumed = true;
        }
        if (!tailStarted && now >= s.seconds - 30)
        {
            tailStarted = true;
            tailMissing = delivery.requested - delivery.supplied;
            tailDropped = spu.GetOutputDroppedFrames();
        }
    }
    const auto missing = delivery.requested - delivery.supplied;
    const auto dropped = spu.GetOutputDroppedFrames();
    const double expected = 1 / (1 + s.ppm * 1e-6);
    std::printf("%s correction_ppm=%.3f expected_ppm=%.3f status=%d missing=%llu dropped=%llu tail_missing=%llu tail_dropped=%llu range_ppm=[%.1f,%.1f]\n",
        s.name, (correction-1)*1e6, (expected-1)*1e6, int(control.GetStatus()),
        (unsigned long long)missing, (unsigned long long)dropped,
        (unsigned long long)(missing-tailMissing), (unsigned long long)(dropped-tailDropped),
        (minCorrection-1)*1e6, (maxCorrection-1)*1e6);
    Require(minCorrection >= .999 && maxCorrection <= 1.001, "Clock correction exceeded bound");
    if (s.speed != 1)
    {
        Require(control.GetStatus() == AudioClockCorrection::Status::Suspended && correction == 1,
                "Sustained overload was treated as compensable clock drift");
        control.Reset(s.rate, 60);
        Require(control.GetStatus() == AudioClockCorrection::Status::Qualifying, "Lifecycle reset retained overload latch");
        return;
    }
    Require(control.GetStatus() == AudioClockCorrection::Status::Active, "Regular delivery did not acquire tracking");
    Require(std::abs(correction - expected) < 0.000020, "Persistent clock mismatch did not converge");
    // Count late loss separately from acquisition/stall loss. No buffer lead is
    // added, so isolated quantization shortages remain possible.
    Require(missing - tailMissing <= 4 && dropped == tailDropped, "Steady delivery kept accumulating loss");
}
static void Gaps()
{
    AudioClockCorrection c;
    c.Reset(48000, 60);
    AudioClockCorrection::Observation o{1,480,480,480,0,0,800,2047};
    c.Update(1, &o);
    o.callbackTime = 1.1; o.requested = o.supplied = 5280;
    Require(c.Update(1.1, &o) == 1, "Long completion gap not rejected");
    Require(c.Update(1.2, nullptr) == 1, "Snapshot loss retained correction");
    o.callbackFrames = 2048; o.callbackTime = 1.3;
    c.Update(1.3, &o);
    Require(c.GetStatus() == AudioClockCorrection::Status::Inactive, "Incompatible block size accepted");
    o.callbackFrames = 480; o.callbackTime = 1.4;
    c.Update(1.4, &o);
    Require(c.GetStatus() == AudioClockCorrection::Status::Qualifying, "Compatible delivery stayed inactive");
}
int main(int argc, char** argv) try
{
    Gaps();
    const Scenario cases[] = {
        {"nominal"}, {"device+100",100}, {"device-100",-100},
        {"device+500",500}, {"device-500",-500},
        {"DSi47k+500",500,704}, {"DSi47k-500",-500,704},
        {"DS32k44.1k",500,1024,44100},
        {"producer-stall",0,1024,48000,0,0,true},
        {"learned-stall",500,1024,48000,0,0,true},
        {"callback-stall",0,1024,48000,0,0,false,true},
        {"jitter250us",500,1024,48000,.00025,.00025},
        {"jitter1ms",500,1024,48000,.001,.001},
        {"overload",0,1024,48000,0,0,false,false,.98,10}
    };
    bool found = false;
    for (const auto& s : cases)
    {
        if (argc >= 2 && std::string(argv[1]) != s.name) continue;
        Run(s, argc >= 3); found = true;
    }
    Require(found, "Unknown clock scenario");
    return 0;
}
catch (const std::exception& e) { std::fprintf(stderr,"%s\n",e.what()); return 1; }
