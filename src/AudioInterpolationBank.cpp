// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioInterpolationBank.h"
#include "AudioInterpolationMath.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace melonDS
{
namespace
{
class Reader
{
public:
    explicit Reader(std::span<const u8> bytes) : Bytes(bytes) {}

    u32 Word()
    {
        if (Bytes.size() < 4) throw std::invalid_argument("Truncated interpolation bank");
        const u32 value = u32(Bytes[0]) | (u32(Bytes[1]) << 8) |
                          (u32(Bytes[2]) << 16) | (u32(Bytes[3]) << 24);
        Bytes = Bytes.subspan(4);
        return value;
    }

    double Value()
    {
        const u64 low = Word();
        const u64 bits = low | (u64(Word()) << 32);
        const double value = std::bit_cast<double>(bits);
        if (!std::isfinite(value)) throw std::invalid_argument("Nonfinite interpolation coefficient");
        return value;
    }

    bool Empty() const noexcept { return Bytes.empty(); }

private:
    std::span<const u8> Bytes;
};

AudioInterpolationBank::Moment MakeWeights(unsigned offset, unsigned interval)
{
    AudioInterpolationBank::Moment result;
    const double x = 1 - 2.0 * offset / interval;
    result[0] = 1;
    result[1] = x;
    for (unsigned k = 2; k < result.size(); ++k)
        result[k] = 2 * x * result[k - 1] - result[k - 2];
    return result;
}
}

std::shared_ptr<const AudioInterpolationBank> AudioInterpolationBank::Create(std::span<const u8> data)
{
    return std::shared_ptr<const AudioInterpolationBank>(new AudioInterpolationBank(data));
}

AudioInterpolationBank::AudioInterpolationBank(std::span<const u8> data)
{
    Reader reader(data);
    if (reader.Word() != 0x31425148) throw std::invalid_argument("Unknown interpolation bank format");
    Interval = reader.Word();
    if ((Interval != 352 && Interval != 512) || reader.Word() != Interval)
        throw std::invalid_argument("Invalid interpolation bank interval");

    Records.resize(Interval);
    OutputWeights.reserve(Interval);
    for (unsigned offset = 0; offset < Interval; ++offset)
        OutputWeights.push_back(MakeWeights(offset, Interval));
    static const auto knotWeights = [] {
        std::array<Moment, 256> weights;
        for (unsigned i = 0; i < weights.size(); ++i) weights[i] = MakeWeights(i, 256);
        return weights;
    }();

    for (unsigned period = 1; period <= Interval; ++period)
    {
        const auto id = reader.Word();
        auto& record = Records[period - 1];
        record.Length = reader.Word();
        const auto rows = reader.Word();
        if (id != period || record.Length < 2 || record.Length > 50000)
            throw std::invalid_argument("Invalid interpolation record");

        // The last record is one exact response shared by every P >= Interval.
        if (period == Interval)
        {
            if (rows != 0) throw std::invalid_argument("Invalid shared interpolation response");
            record.Values.resize(record.Length);
            for (double& value : record.Values) value = reader.Value();
            continue;
        }

        const unsigned span = period <= DensePeriods ? Interval : 256;
        // The original P1..32 fits include one trailing, zero-support block.
        // Preserve their exact table layout; later fits omit that block.
        const unsigned expectedRows = (record.Length + span - 1) / span + (period <= 32 ? 1 : 0);
        if (rows != expectedRows)
            throw std::invalid_argument("Invalid interpolation coefficient count");
        std::vector<Moment> coefficients(rows);
        for (auto& row : coefficients)
            for (double& value : row) value = reader.Value();
        if (period <= DensePeriods)
        {
            record.Moments = std::move(coefficients);
            continue;
        }

        record.Values.resize(record.Length);
        for (unsigned i = 0; i + 1 < record.Length; ++i)
            record.Values[i] = AudioInterpolationMath::Dot(coefficients[i / 256].data(), knotWeights[i % 256].data());
        record.Values.back() = 0;
    }
    if (!reader.Empty()) throw std::invalid_argument("Trailing interpolation bank data");
}

const AudioInterpolationBank::Record& AudioInterpolationBank::GetRecord(unsigned period) const
{
    if (period == 0 || period > 65536) throw std::out_of_range("Invalid sound period");
    return Records[std::min(period, Interval) - 1];
}

std::span<const AudioInterpolationBank::Moment> AudioInterpolationBank::Coefficients(unsigned period) const
{
    return GetRecord(period).Moments;
}

u64 AudioInterpolationBank::SupportClocks(unsigned period) const
{
    const auto& record = GetRecord(period);
    return u64(std::ceil(double(record.Length) * period / std::min(period, 256u)));
}

double AudioInterpolationBank::Step(unsigned period, u64 age) const
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
}
