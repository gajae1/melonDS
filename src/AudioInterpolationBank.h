// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef MELONDS_AUDIOINTERPOLATIONBANK_H
#define MELONDS_AUDIOINTERPOLATIONBANK_H

#include <array>
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
    double Step(unsigned period, u64 age) const;

private:
    struct Record
    {
        unsigned Length = 0;
        std::vector<Moment> Moments;
        std::vector<double> Values;
    };

    explicit AudioInterpolationBank(std::span<const u8> data);
    const Record& GetRecord(unsigned period) const;

    unsigned Interval = 0;
    std::vector<Record> Records;
    std::vector<Moment> OutputWeights;
};
}
#endif
