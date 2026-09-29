// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MELONDS_AUDIOSINC_H
#define MELONDS_AUDIOSINC_H

#include <array>
#include <vector>
#include <span>
#include "types.h"

namespace melonDS
{
class Savestate;

// The longest support is Taps * 512 source samples (timer period 1).
// A mirrored ring keeps each SIMD dot product contiguous across wraparound.
class AudioSinc
{
public:
    static constexpr unsigned Taps = 48;
    static constexpr unsigned Phases = 256;
    static constexpr unsigned Capacity = 32768;

    AudioSinc();
    void Reset(s16 sample = 0);
    void Push(s16 sample);
    s32 Output(u32 elapsed, u32 period, u32 mixPeriod = 512) const;
    void DoSavestate(Savestate* file);
    bool Empty() const { return ZeroTail == Capacity; }

private:
    std::vector<float> History;
    u32 Head = 0;
    u32 ZeroTail = Capacity; // Consecutive zeros from Head (saturated); all-zero means Empty.
};

// Host-only stereo reconstruction, downstream of the guest mixer and capture.
// Like blip_buf, its queue/history is discarded after a committed state load.
class AudioSincOutput
{
public:
    static constexpr double ClockCorrectionMin = 0.999;
    static constexpr double ClockCorrectionMax = 1.001;

    // Sets the nominal rates and the anti-alias filter. The current clock
    // correction is retained and applied on top of the new nominal step.
    void SetRates(double inputRate, double outputRate);
    // Scales the effective input clock by correction (>1 consumes input faster,
    // yielding fewer output samples). Accepts finite values in [0.999, 1.001]
    // after SetRates; otherwise returns false and changes nothing. It never
    // allocates, rebuilds coefficients, or touches history/pending samples.
    bool SetClockCorrection(double correction);
    // Zero means the nominal filter lacks upsampling headroom for the bound.
    double ClockCorrectionStep() const
    { return Count && NominalStep > 0 && NominalStep * ClockCorrectionMax <= 1 ? NominalStep : 0; }

    // Clears history and pending output, and restores correction to 1.
    void Reset();
    void Push(const s16* stereo);
    std::span<const s16> Samples() const { return Pending; }
    void ClearSamples() { Pending.clear(); }

private:
    std::array<std::vector<float>, 2> History;
    std::vector<float> Weights;
    std::vector<s16> Pending;
    unsigned Head = 0;
    unsigned Capacity = 0;
    unsigned Count = 0;
    double Ratio = 0;
    double NominalStep = 1;
    double Correction = 1;
    double Step = 1; // NominalStep * Correction
    double Position = 0;
};
}
#endif
