// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AudioInterpolationStream.h"
#include "SPU.h"
#include <algorithm>
#include <cmath>

namespace melonDS
{
// Host-only reconstruction. All observed decoder events retain their original
// integer timestamps; game capture and serialized channel state are untouched.
class AudioInterpolationRenderer
{
    struct Point
    {
        u64 Clock;
        s16 Sample;
        unsigned Period, Volume, Shift, Pan;
        bool Eligible;
    };
    struct Channel
    {
        std::array<Point, 512 + 3> Points;
        unsigned Count = 0;
        s16 Sample = 0, Published = 0;
        unsigned Period = 65536;
        Point Control{};
    };
    std::array<std::shared_ptr<const AudioInterpolationBank>, 2> Banks;
    std::vector<AudioInterpolationStream> Streams;
    std::array<Channel, 16> Channels;
    u64 BeginClock = 0, EndClock = 0;
    unsigned Interval = 512, Control = 0, Volume = 0;
    bool Collecting = false, Muted = true;
    std::array<double, 2> Nitro{};
    u16 I2SControl = 0;
    std::array<s16, 2> DSP{};

public:
    // May allocate/validate. Prepare outside callbacks and before changing the
    // active renderer. Concurrent instances share only immutable coefficient data.
    static std::unique_ptr<AudioInterpolationRenderer> Prepare();

    std::array<s16, 2> LastOutput{};
    explicit AudioInterpolationRenderer(std::array<std::shared_ptr<const AudioInterpolationBank>, 2> banks)
        : Banks(std::move(banks))
    {
        if (!Banks[0] || Banks[0]->MixInterval() != 352 ||
            !Banks[1] || Banks[1]->MixInterval() != 512)
            throw std::invalid_argument("Both interpolation rates must be prepared");
        size_t directCapacity = 0, blockCapacity = 0;
        for (const auto& bank : Banks)
        {
            // A channel has one fixed reload during Run(). Its begin boundary
            // can also publish a sample retained from the previous interval.
            // Therefore each output bucket has at most two dense periods.
            size_t rows = 0;
            for (unsigned p = 1; p <= AudioInterpolationBank::DensePeriods; ++p)
                rows = std::max(rows, bank->Coefficients(p).size());
            blockCapacity = std::max(blockCapacity, 2 * (rows + 1));

            // Direct periods are >=257 clocks: at most ceil(M/257) decoder
            // changes plus one merged begin change per M-clock interval.
            // Retain every possible interval intersecting the longest support,
            // plus two edge intervals for publication/retirement ordering.
            u64 support = 0;
            for (unsigned p = 257; p <= bank->MixInterval(); ++p)
                support = std::max(support, bank->SupportClocks(p));
            support = std::max(support, bank->SupportClocks(65536));
            const u64 mix = bank->MixInterval();
            const size_t intervals = (support + mix - 1) / mix + 2;
            const size_t events = (mix + 256) / 257 + 1;
            directCapacity = std::max(directCapacity, intervals * events);
        }
        Streams.reserve(16);
        for (unsigned c = 0; c < 16; ++c)
            Streams.emplace_back(Banks[1], directCapacity, blockCapacity);
    }
    size_t HistoryBytes() const noexcept
    { size_t bytes = 0; for (const auto& stream : Streams) bytes += stream.HistoryBytes(); return bytes; }
    size_t ActiveTails() const noexcept
    { size_t tails = 0; for (const auto& stream : Streams) tails += stream.ActiveTails(); return tails; }
    void Reset() noexcept
    {
        for (auto& stream : Streams) stream.Reset();
        for (auto& channel : Channels)
        {
            channel.Count = 0; channel.Sample = channel.Published = 0;
            channel.Period = 65536; channel.Control = {};
        }
        BeginClock = EndClock = 0; Collecting = false; Nitro = {}; DSP = {}; I2SControl = 0;
    }
    void Begin(unsigned cycles, unsigned interval, const SPUChannel* channels,
               unsigned control, unsigned volume, bool muted)
    {
        if (Collecting || (interval != 352 && interval != 512) ||
            (cycles != 0 && cycles != 352 && cycles != 512))
            throw std::logic_error("Invalid interpolation collection interval");
        // A rate write changes the next event, not the already scheduled one.
        // Reconstruct the interval actually executed; the first boot event has
        // zero elapsed clocks and uses the configured rate for its empty epoch.
        interval = cycles ? cycles : interval;
        if (Interval != interval)
        {
            Reset(); Interval = interval;
            for (auto& stream : Streams) stream.ChangeBank(Banks[interval == 512]);
        }
        Control = control; Volume = volume; Muted = muted;
        EndClock = BeginClock + cycles; Collecting = true;
        for (unsigned c = 0; c < 16; ++c)
        {
            Channels[c].Count = 0;
            Observe(channels[c], BeginClock);
        }
    }
    u64 End() const noexcept { return EndClock; }
    void Observe(const SPUChannel& input, u64 clock)
    {
        auto& channel = Channels[input.Num];
        if (!Collecting || clock < BeginClock || clock > EndClock || channel.Count == channel.Points.size())
            throw std::logic_error("Invalid interpolation observation");
        channel.Points[channel.Count++] = {clock, input.CurSample, 65536u - input.TimerReload,
            input.Volume, input.VolumeShift, input.Pan,
            !((((input.Cnt >> 29) & 3) == 3 && input.Num < 8) ||
              ((input.Cnt & (1u << 31)) && ((input.Cnt >> 29) & 3) < 3 && input.Length + input.LoopPos < 16))};
    }
    void Finish(const SPUChannel* channels)
    {
        if (!Collecting) throw std::logic_error("Interpolation collection not started");
        std::array<double, 16> left{}, right{};
        double sumLeft = 0, sumRight = 0;
        for (unsigned c = 0; c < 16; ++c)
        {
            Observe(channels[c], EndClock);
            auto& channel = Channels[c];
            unsigned i = 0;
            while (i < channel.Count)
            {
                const u64 clock = channel.Points[i].Clock;
                do
                {
                    const auto& point = channel.Points[i++];
                    if (point.Sample != channel.Sample)
                    { channel.Sample = point.Sample; channel.Period = point.Period; }
                    channel.Control = point;
                } while (i < channel.Count && channel.Points[i].Clock == clock);
                if (clock == EndClock) continue; // Merge with next begin before publication.
                if (channel.Sample != channel.Published)
                {
                    Streams[c].Push(clock, channel.Sample, channel.Period);
                    channel.Published = channel.Sample;
                }
                if (clock == BeginClock)
                {
                    const auto& ctrl = channel.Control;
                    const double reconstructed = Streams[c].Read(clock);
                    const double value = ctrl.Eligible
                        ? reconstructed * double(1u << ctrl.Shift) * ctrl.Volume / 1024 : 0;
                    left[c] = value * (128 - ctrl.Pan); right[c] = value * ctrl.Pan;
                }
            }
            if ((c == 1 && (Control & (1u << 12))) || (c == 3 && (Control & (1u << 13)))) continue;
            sumLeft += left[c]; sumRight += right[c];
        }
        const auto select = [](unsigned mode, double sum, const auto& side)
        {
            switch (mode)
            { case 0: return sum; case 1: return side[1]; case 2: return side[3]; default: return side[1] + side[3]; }
        };
        if (BeginClock != EndClock)
        {
            const double gain = Muted || !(Control & (1u << 15)) ? 0 : Volume / (128.0 * 256);
            Nitro = {select((Control >> 8) & 3, sumLeft, left) * gain,
                     select((Control >> 10) & 3, sumRight, right) * gain};
        }
        BeginClock = EndClock; Collecting = false;
    }
    void ObserveI2S(u16 control, const s16* dsp) noexcept
    {
        I2SControl = control;
        DSP = dsp ? std::array<s16, 2>{dsp[0], dsp[1]} : std::array<s16, 2>{};
    }
    std::array<s16, 2> Output(bool applyBias, u16 bias, bool muted, bool dsi, bool degrade10) const
    {
        std::array<s16, 2> output;
        for (unsigned side = 0; side < 2; ++side)
        {
            const double value = Nitro[side] + (applyBias ? int(bias << 6) - 0x8000 : 0);
            if (!std::isfinite(value)) throw std::logic_error("Nonfinite interpolation output");
            s32 sample = muted ? 0 : s32(std::clamp(std::round(value), -32768.0, 32767.0));
            if (dsi)
            {
                if (!(I2SControl & (1u << 15)) || (I2SControl & (1u << 14))) sample = 0;
                else
                {
                    const unsigned nitro = std::min<unsigned>(I2SControl & 15, 8);
                    sample = (sample * int(nitro) + DSP[side] * int(8 - nitro)) >> 3;
                }
            }
            output[side] = s16(sample);
            if (degrade10) output[side] &= 0xFFC0;
        }
        return output;
    }
};
}
