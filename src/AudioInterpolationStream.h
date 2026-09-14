// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AudioInterpolationBank.h"
#include "AudioInterpolationMath.h"
#include "AudioInterpolationAccumulate.h"
#include <limits>
#include <stdexcept>

namespace melonDS
{
// One host-only stereo channel. The shared immutable bank is prepared before audio
// processing. Push receives observed PCM changes; Read publishes the mix grid.
// No file I/O, filter construction, or allocation occurs in Push/Read/Reset.
class AudioInterpolationStream
{
    using Bank = AudioInterpolationBank;
    using Moment = Bank::Moment;
    // The validated bank support fits 32 bits; each stereo tail shares one
    // cached reciprocal and one kernel lookup.
    static_assert(u64(Bank::MaxResponseLength) * 65536 / Bank::DensePeriods <=
                  std::numeric_limits<u32>::max());
    using Stereo = std::array<double, 2>;
    struct Active { u64 Clock; Stereo Delta; double Scale; u32 Support; unsigned Period; };
    struct Block
    {
        u64 End = 0;
        unsigned Period = 0;
        bool SameSides = true;
        // The right history is valid only after SameSides becomes false.
        // Do not clear/copy its unused 128 bytes for center-panned blocks.
        std::array<Moment, 2> Moments;
        std::span<const Moment> Coefficients;

        Block() noexcept { Moments[0].fill(0); }
        Block(const Block& other) noexcept { *this = other; }
        Block& operator=(const Block& other) noexcept
        {
            End = other.End; Period = other.Period; SameSides = other.SameSides;
            Coefficients = other.Coefficients;
            Moments[0] = other.Moments[0];
            if (!SameSides) Moments[1] = other.Moments[1];
            return *this;
        }
    };
    // Limits are allocated before processing. The renderer derives its limits
    // from the bank support and the SPU event contract.
    size_t DirectCapacity, BlockCapacity;
    std::shared_ptr<const Bank> Owner;
    std::vector<Active> Direct;
    std::vector<Block> Blocks;
    std::array<Block, Bank::DensePeriods> Pending{};
    std::array<unsigned, Bank::DensePeriods> PendingIndices{};
    unsigned PendingCount = 0;
    u64 PendingEnd = 0, LastInput = 0, LastOutput = 0;
    bool HasOutput = false;
    Stereo Current{};

    void Flush()
    {
        if (PendingCount > BlockCapacity - Blocks.size())
            throw std::length_error("Interpolation block capacity exceeded");
        for (unsigned i = 0; i < PendingCount; ++i)
        {
            auto& block = Pending[PendingIndices[i]];
            Blocks.push_back(block);
            block.Period = 0;
        }
        PendingCount = 0;
    }
public:
    explicit AudioInterpolationStream(std::shared_ptr<const Bank> bank,
                                      size_t directCapacity, size_t blockCapacity)
        : DirectCapacity(directCapacity), BlockCapacity(blockCapacity), Owner(std::move(bank))
    {
        if (!Owner) throw std::invalid_argument("Interpolation bank required");
        Direct.reserve(DirectCapacity);
        Blocks.reserve(BlockCapacity);
    }
    void Reset() noexcept
    {
        Direct.clear(); Blocks.clear();
        for (unsigned i = 0; i < PendingCount; ++i) Pending[PendingIndices[i]].Period = 0;
        PendingCount = 0; PendingEnd = LastInput = LastOutput = 0;
        HasOutput = false; Current = {};
    }
    size_t ActiveTails() const noexcept { return Direct.size() + Blocks.size() + PendingCount; }
    size_t HistoryBytes() const noexcept
    { return Direct.capacity() * sizeof(Active) + Blocks.capacity() * sizeof(Block) + sizeof(Pending); }
    void ChangeBank(std::shared_ptr<const Bank> bank)
    {
        if (!bank) throw std::invalid_argument("Interpolation bank required");
        Reset();
        Owner = std::move(bank);
    }
    void Push(u64 clock, Stereo value, unsigned period)
    {
        if (!period || period > 65536 || clock < LastInput || (HasOutput && clock <= LastOutput))
            throw std::invalid_argument("Interpolation input time or period invalid");
        const Stereo delta{value[0] - Current[0], value[1] - Current[1]};
        if (!delta[0] && !delta[1]) { LastInput = clock; return; }
        if (period > Bank::DensePeriods)
        {
            if (Direct.size() == DirectCapacity)
                throw std::length_error("Interpolation direct capacity exceeded");
            Direct.push_back({clock, delta, 256.0 / period, u32(Owner->SupportClocks(period)), period});
        }
        else
        {
            const unsigned mix = Owner->MixInterval();
            unsigned offset = clock % mix;
            if (offset) offset = mix - offset;
            if (clock > std::numeric_limits<u64>::max() - offset)
                throw std::invalid_argument("Interpolation input clock overflow");
            const u64 end = clock + offset;
            if (end != PendingEnd) { Flush(); PendingEnd = end; }
            auto& block = Pending[period - 1];
            if (!block.Period)
            {
                PendingIndices[PendingCount++] = period - 1;
                block.End = end; block.Period = period; block.SameSides = true;
                block.Moments[0].fill(0);
                block.Coefficients = Owner->Coefficients(period);
            }
            const auto& weights = Owner->Weights()[offset];
            // Center-panned events have identical L/R moments. Share their
            // accumulation until a real stereo difference enters this block.
            if (block.SameSides && delta[0] != delta[1])
            {
                block.Moments[1] = block.Moments[0];
                block.SameSides = false;
            }
            AudioInterpolationMath::Accumulate(block.Moments[0].data(), delta[0], weights.data());
            if (!block.SameSides)
                AudioInterpolationMath::Accumulate(block.Moments[1].data(), delta[1], weights.data());
        }
        Current = value; LastInput = clock;
    }
    Stereo Read(u64 clock)
    {
        const unsigned mix = Owner->MixInterval();
        if (clock % mix || clock < LastInput || (HasOutput && clock < LastOutput))
            throw std::invalid_argument("Interpolation output grid or time invalid");
        Flush();
        Stereo value = Current;
        size_t keep = 0;
        for (size_t i = 0; i < Direct.size(); ++i)
        {
            const auto tail = Direct[i];
            const u64 age = clock - tail.Clock;
            if (age >= tail.Support) continue;
            // Both sides share the timestamp and kernel lookup. Gain and pan
            // belong to the input event, so later notes cannot rescale old tails.
            const double residual = 1 - Owner->StepPosition(tail.Period, double(age) * tail.Scale);
            for (unsigned side = 0; side < 2; ++side)
                value[side] -= tail.Delta[side] * residual;
            if (keep != i) Direct[keep] = tail;
            ++keep;
        }
        Direct.resize(keep); keep = 0;
        for (size_t i = 0; i < Blocks.size(); ++i)
        {
            const auto& block = Blocks[i];
            const u64 row = (clock - block.End) / mix;
            const auto coefficients = block.Coefficients;
            if (row >= coefficients.size()) continue;
            const double left = AudioInterpolationMath::Dot(block.Moments[0].data(), coefficients[row].data());
            value[0] -= left;
            value[1] -= block.SameSides ? left
                : AudioInterpolationMath::Dot(block.Moments[1].data(), coefficients[row].data());
            if (keep != i) Blocks[keep] = block;
            ++keep;
        }
        Blocks.resize(keep);
        HasOutput = true; LastOutput = clock;
        return value;
    }
};
}
