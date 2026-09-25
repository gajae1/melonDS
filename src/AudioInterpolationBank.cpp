// SPDX-License-Identifier: GPL-3.0-or-later
#include "AudioInterpolationBank.h"
#include "AudioInterpolationMath.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>
#include <thread>

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

// Sparse responses are sampled on up to this many workers. The fill is a
// streaming write of ~91 MB over both rates and reaches memory bandwidth well
// before this cap, and a smaller host reports a smaller hardware_concurrency
// (which may also be 0 when restricted).
constexpr unsigned MaxFillWorkers = 8;
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

    // Sparse responses are sampled after the whole blob has been validated.
    // Every sampled point depends only on its own record's packed rows and the
    // shared knot table, so the evaluations are independent per record and run
    // on several workers. The tables stay byte-identical: the same Dot calls in
    // the same order, only spread over threads. Rows are kept until their
    // record has been filled. The hot path (StepPosition reading Values) is
    // untouched and always sees a completed table.
    struct SparseJob
    {
        Record* Target;
        std::unique_ptr<Moment[]> Rows;
    };
    std::vector<SparseJob> sparse;

    for (unsigned period = 1; period <= Interval; ++period)
    {
        const auto id = reader.Word();
        auto& record = Records[period - 1];
        record.Length = reader.Word();
        const auto rows = reader.Word();
        if (id != period || record.Length < 2 || record.Length > MaxResponseLength)
            throw std::invalid_argument("Invalid interpolation record");

        // The last record is one exact response shared by every P >= Interval.
        if (period == Interval)
        {
            if (rows != 0) throw std::invalid_argument("Invalid shared interpolation response");
            record.Values = std::make_unique_for_overwrite<double[]>(record.Length);
            for (unsigned i = 0; i < record.Length; ++i) record.Values[i] = reader.Value();
            continue;
        }

        const unsigned span = period <= DensePeriods ? Interval : 256;
        // The original P1..32 fits include one trailing, zero-support block.
        // Preserve their exact table layout; later fits omit that block.
        const unsigned expectedRows = (record.Length + span - 1) / span + (period <= 32 ? 1 : 0);
        if (rows != expectedRows)
            throw std::invalid_argument("Invalid interpolation coefficient count");
        auto coefficients = std::make_unique_for_overwrite<Moment[]>(rows);
        for (unsigned row = 0; row < rows; ++row)
            for (double& value : coefficients[row]) value = reader.Value();
        if (period <= DensePeriods)
        {
            record.Moments = std::move(coefficients);
            record.Rows = rows;
            continue;
        }

        record.Values = std::make_unique_for_overwrite<double[]>(record.Length);
        sparse.push_back({&record, std::move(coefficients)});
    }
    if (!reader.Empty()) throw std::invalid_argument("Trailing interpolation bank data");

    if (!sparse.empty())
    {
        const unsigned cpus = std::thread::hardware_concurrency();
        const unsigned workers = std::min(std::clamp(cpus ? cpus : 1, 1u, MaxFillWorkers),
                                          unsigned(sparse.size()));
        // knotWeights is a function-local static: no capture is needed.
        const auto fill = [&sparse](unsigned first, unsigned step)
        {
            for (size_t job = first; job < sparse.size(); job += step)
            {
                const unsigned length = sparse[job].Target->Length;
                double* values = sparse[job].Target->Values.get();
                const auto* rows = sparse[job].Rows.get();
                for (unsigned i = 0; i + 1 < length; ++i)
                    values[i] = AudioInterpolationMath::Dot(rows[i / 256].data(), knotWeights[i % 256].data());
                values[length - 1] = 0;
            }
        };
        std::vector<std::thread> helpers;
        helpers.reserve(workers - 1);
        try
        {
            for (unsigned w = 1; w < workers; ++w) helpers.emplace_back(fill, w, workers);
        }
        catch (...)
        {
            // A thread that never started must not leave a joinable thread.
            for (auto& helper : helpers) helper.join();
            throw;
        }
        fill(0, workers);
        for (auto& helper : helpers) helper.join();
    }
}

std::span<const AudioInterpolationBank::Moment> AudioInterpolationBank::Coefficients(unsigned period) const
{
    const auto& record = GetRecord(period);
    return std::span<const Moment>(record.Moments.get(), record.Rows);
}

u64 AudioInterpolationBank::SupportClocks(unsigned period) const
{
    const auto& record = GetRecord(period);
    if (period <= DensePeriods) return record.Length;
    // Sparse responses use a 256-point grid. Round its integer clock span up
    // exactly, without a floating division and ceil for each decoder event.
    return (u64(record.Length) * period + 255) / 256;
}
}
