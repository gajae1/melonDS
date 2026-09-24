// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MELONDS_AUDIOINTERPOLATIONBANK_H
#define MELONDS_AUDIOINTERPOLATIONBANK_H

#include <array>
#include <algorithm>
#include <stdexcept>
#include <memory>
#include <span>
#include <vector>
#include "types.h"

namespace melonDS
{
// Immutable host interpolation data. Prepare before audio processing, then keep
// the returned owner for as long as a renderer uses its coefficient views.
class AudioInterpolationBank
{
public:
    static constexpr unsigned Degree = 16;
    static constexpr unsigned DensePeriods = 256;
    static constexpr unsigned MaxResponseLength = 50000;
    using Moment = std::array<double, Degree>;

    // Packed little-endian coefficients; malformed data throws before an owner
    // is returned. Loading does not modify another instance's bank or history.
    static std::shared_ptr<const AudioInterpolationBank> Create(std::span<const u8> data);

    unsigned MixInterval() const noexcept { return Interval; }
    std::span<const Moment> Weights() const noexcept { return OutputWeights; }
    std::span<const Moment> Coefficients(unsigned period) const;
    u64 SupportClocks(unsigned period) const;
    // Reference evaluation retains division; streams cache the fixed reciprocal
    // per decoder event and use StepPosition for each subsequent output.
    double Step(unsigned period, u64 age) const
    {
        return StepPosition(period, double(age) * 256 / period);
    }

private:
    friend class AudioInterpolationStream;
    double StepPosition(unsigned period, double t) const
    {
        const auto& record = GetRecord(period);
        if (period <= DensePeriods) throw std::logic_error("Dense interpolation requires moments");
        if (t >= record.Length - 1) return 1;
        const auto i = size_t(t);
        const double a = record.Values[i];
        const double value = a + (record.Values[i + 1] - a) * (t - i);
        return period < Interval ? 1 - value : value;
    }

    // Coefficient rows and sparse responses are allocated uninitialised and
    // written completely by the loader; value-initialising them first would
    // zero-fill every byte only to overwrite it (98 MB over both rates).
    struct Record
    {
        unsigned Length = 0;
        unsigned Rows = 0;
        std::unique_ptr<Moment[]> Moments;
        std::unique_ptr<double[]> Values;
    };

    explicit AudioInterpolationBank(std::span<const u8> data);
    const Record& GetRecord(unsigned period) const
    {
        if (period == 0 || period > 65536) throw std::out_of_range("Invalid sound period");
        return Records[std::min(period, Interval) - 1];
    }

    unsigned Interval = 0;
    std::vector<Record> Records;
    std::vector<Moment> OutputWeights;
};
}
#endif
