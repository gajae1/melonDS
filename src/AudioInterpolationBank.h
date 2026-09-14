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
    using Moment = std::array<double, Degree>;

    // Packed little-endian coefficients; malformed data throws before an owner
    // is returned. Loading does not modify another instance's bank or history.
    static std::shared_ptr<const AudioInterpolationBank> Create(std::span<const u8> data);

    unsigned MixInterval() const noexcept { return Interval; }
    std::span<const Moment> Weights() const noexcept { return OutputWeights; }
    std::span<const Moment> Coefficients(unsigned period) const;
    u64 SupportClocks(unsigned period) const;
    // Evaluated for each live sparse tail; expose the lookup to the optimizer
    // without changing response arithmetic or retaining per-tail copies.
    double Step(unsigned period, u64 age) const
    {
        const auto& record = GetRecord(period);
        if (period <= DensePeriods) throw std::logic_error("Dense interpolation requires moments");
        const double t = double(age) * 256 / period;
        if (t >= record.Length - 1) return 1;
        const auto i = size_t(t);
        const double a = record.Values[i];
        const double value = a + (record.Values[i + 1] - a) * (t - i);
        return period < Interval ? 1 - value : value;
    }

private:
    struct Record
    {
        unsigned Length = 0;
        std::vector<Moment> Moments;
        std::vector<double> Values;
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
