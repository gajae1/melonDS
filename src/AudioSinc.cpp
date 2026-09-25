// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioSinc.h"
#include "AudioInterpolationMath.h"
#include "Savestate.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <numbers>

namespace melonDS
{
namespace
{
// libc++ has no std::cyl_bessel_i. The power series of I0 converges to double
// precision within a few dozen terms for the Kaiser window's arguments.
double BesselI0(double x)
{
    const double quarter = x * x / 4;
    double sum = 1, term = 1;
    for (int k = 1; term > sum * 1e-17; ++k)
    {
        term *= quarter / (double(k) * k);
        sum += term;
    }
    return sum;
}

template<unsigned Taps>
struct SincTables
{
    static constexpr double Cutoff = 0.925;
    static constexpr double Beta = 7.0;
    std::array<std::array<float, Taps>, AudioSinc::Phases + 1> Phase;
    std::array<float, Taps / 2 * AudioSinc::Phases + 1> Kernel;

    SincTables()
    {
        const double scale = BesselI0(Beta);
        for (unsigned i = 0; i < Kernel.size(); ++i)
        {
            const double x = double(i) / AudioSinc::Phases;
            const double p = std::numbers::pi * Cutoff * x;
            const double window = BesselI0(Beta * std::sqrt(
                std::max(0.0, 1 - x*x/(Taps*Taps/4)))) / scale;
            Kernel[i] = (i ? std::sin(p)/p : 1.0) * Cutoff * window;
        }
        Kernel.back() = 0;
        for (unsigned phase = 0; phase <= AudioSinc::Phases; ++phase)
        {
            double sum = 0;
            for (unsigned i = 0; i < Taps; ++i)
            {
                const int index = int(i * AudioSinc::Phases + phase) -
                    int(Taps / 2 * AudioSinc::Phases);
                sum += Phase[phase][i] = Kernel[std::abs(index)];
            }
            for (float& weight : Phase[phase]) weight /= sum;
        }
    }

    float At(double x) const
    {
        x = std::abs(x) * AudioSinc::Phases;
        if (x >= Kernel.size()-1) return 0;
        const auto index = static_cast<unsigned>(x);
        return Kernel[index] + (Kernel[index+1]-Kernel[index]) * float(x-index);
    }
};

template<unsigned Taps = AudioSinc::Taps>
const SincTables<Taps>& Tables()
{
    static const SincTables<Taps> tables;
    return tables;
}

// Same result as std::lround (half away from zero) for |value| < 2^22, where
// the remainder after truncation is exact, without a CRT call per sample.
s32 RoundSample(float value) noexcept
{
    const s32 truncated = s32(value);
    const float remainder = value - float(truncated);
    if (remainder >= 0.5f) return truncated + 1;
    if (remainder <= -0.5f) return truncated - 1;
    return truncated;
}
}

AudioSinc::AudioSinc() : History(Capacity * 2)
{
    // Prepare the shared table before the channel processing path.
    Tables();
}

void AudioSinc::Reset(s16 sample)
{
    std::fill(History.begin(), History.end(), sample);
    Head = 0;
    Nonzero = sample ? Capacity : 0;
}

void AudioSinc::Push(s16 sample)
{
    Head = (Head-1) & (Capacity-1);
    Nonzero -= History[Head] != 0;
    Nonzero += sample != 0;
    History[Head] = History[Head+Capacity] = sample;
}

s32 AudioSinc::Output(u32 elapsed, u32 period, u32 mixPeriod) const
{
    const auto& tables = Tables();
    const double phase = std::min(double(elapsed)/period, 1.0);
    float value = 0;
    if (period >= mixPeriod)
    {
        const double position = phase * Phases;
        const unsigned index = std::min(unsigned(position), Phases-1);
        // Interpolate adjacent phases to avoid phase-quantization sidebands.
        const float a = AudioInterpolationMath::DotFloat(History.data()+Head, tables.Phase[index].data(), Taps);
        const float b = AudioInterpolationMath::DotFloat(History.data()+Head, tables.Phase[index+1].data(), Taps);
        value = a + float(position-index)*(b-a);
    }
    else
    {
        // Scale both cutoff and support. Merely narrowing a fixed short FIR
        // loses stopband rejection as the source rate rises above the mixer.
        // Sampling the shared kernel avoids one large bank per timer period.
        const double ratio = double(period) / mixPeriod;
        const unsigned count = (unsigned(std::ceil(Taps/ratio)) + 15) & ~15u;
        double sum = 0;
        for (unsigned i = 0; i < count; i += 16)
        {
            std::array<float, 16> weights;
            for (unsigned j = 0; j < weights.size(); ++j)
                sum += weights[j] = tables.At((i+j+phase)*ratio-Taps/2);
            value += AudioInterpolationMath::DotFloat(History.data()+Head+i, weights.data(), 16);
        }
        value /= sum;
    }
    return RoundSample(value);
}

void AudioSinc::DoSavestate(Savestate* file)
{
    file->Var32(&Head);
    file->VarArray(History.data(), Capacity*sizeof(float));
    if (!file->Saving && !file->Error)
    {
        if (Head >= Capacity || std::any_of(History.begin(), History.begin()+Capacity,
            [](float value) { return !std::isfinite(value) || value < -32768 || value > 32767; }))
        {
            file->Error = true;
            return;
        }
        std::copy_n(History.begin(), Capacity, History.begin()+Capacity);
        Nonzero = std::count_if(History.begin(), History.begin()+Capacity,
                               [](float value) { return value != 0; });
    }
}

void AudioSincOutput::SetRates(double inputRate, double outputRate)
{
    const double step = inputRate / outputRate;
    Position *= step / Step; // Retain the time remaining until the next output.
    Step = step;
    const double ratio = std::min(1.0, 1.0 / step);
    if (ratio == Ratio) return;

    // Downsampling scales support as well as cutoff. The stereo stage shares
    // the prototype with the voice filter, but has its own streaming history.
    constexpr unsigned taps = 48;
    const unsigned count = (unsigned(std::ceil(taps / ratio)) + 15) & ~15u;
    const unsigned capacity = std::bit_ceil(count);
    std::vector<float> weights((AudioSinc::Phases + 1) * count);
    const auto& tables = Tables<taps>();
    for (unsigned phase = 0; phase <= AudioSinc::Phases; ++phase)
    {
        float* row = weights.data() + phase * count;
        double sum = 0;
        for (unsigned i = 0; i < count; ++i)
            sum += row[i] = tables.At((i + double(phase)/AudioSinc::Phases) * ratio - taps/2);
        for (unsigned i = 0; i < count; ++i) row[i] /= sum;
    }
    if (capacity != Capacity)
    {
        for (auto& history : History)
        {
            std::vector<float> resized(2 * capacity);
            for (unsigned i = 0; i < capacity; ++i)
                resized[i] = resized[i+capacity] = Capacity
                    ? history[Head + std::min(i, Capacity-1)] : 0;
            history = std::move(resized);
        }
        Capacity = capacity;
        Head = 0;
    }
    Weights = std::move(weights);
    Count = count;
    Ratio = ratio;
    Pending.reserve(2 * (unsigned(std::ceil(192 / step)) + 1));
}

void AudioSincOutput::Reset()
{
    for (auto& history : History) std::fill(history.begin(), history.end(), 0);
    Head = 0;
    Position = 0;
    Pending.clear();
}

void AudioSincOutput::Push(const s16* stereo)
{
    Head = (Head-1) & (Capacity-1);
    for (unsigned ch = 0; ch < 2; ++ch)
        History[ch][Head] = History[ch][Head+Capacity] = stereo[ch];
    while (Position < 1)
    {
        const double phase = Position * AudioSinc::Phases;
        const unsigned index = unsigned(phase);
        const float fraction = float(phase-index);
        const float* a = Weights.data() + index * Count;
        const float* b = a + Count;
        for (const auto& history : History)
        {
            const float first = AudioInterpolationMath::DotFloat(history.data()+Head, a, Count);
            const float second = AudioInterpolationMath::DotFloat(history.data()+Head, b, Count);
            Pending.push_back(s16(std::clamp(RoundSample(first + fraction*(second-first)), -32768, 32767)));
        }
        Position += Step;
    }
    Position -= 1;
}
}
