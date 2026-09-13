// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "AudioInterpolationBank.h"
#include "AudioInterpolationMath.h"
#include "AudioInterpolationAccumulate.h"
#include <limits>
#include <stdexcept>

namespace melonDS
{
// One host-only channel. The shared immutable bank is prepared before audio
// processing. Push receives observed PCM changes; Read publishes the mix grid.
// No file I/O, filter construction, or allocation occurs in Push/Read/Reset.
class AudioInterpolationStream
{
    using Bank = AudioInterpolationBank;
    using Moment = Bank::Moment;
    struct Active { u64 Clock, Support; double Delta; unsigned Period; };
    struct Block
    {
        u64 End = 0;
        unsigned Period = 0;
        Moment Moments{};
        std::span<const Moment> Coefficients;
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
    s16 Current = 0;

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
        HasOutput = false; Current = 0;
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
    void Push(u64 clock, s16 value, unsigned period)
    {
        if (!period || period > 65536 || clock < LastInput || (HasOutput && clock <= LastOutput))
            throw std::invalid_argument("Interpolation input time or period invalid");
        const int delta = int(value) - Current;
        if (!delta) { LastInput = clock; return; }
        if (period > Bank::DensePeriods)
        {
            if (Direct.size() == DirectCapacity)
                throw std::length_error("Interpolation direct capacity exceeded");
            Direct.push_back({clock, Owner->SupportClocks(period), double(delta), period});
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
                block.End = end; block.Period = period; block.Moments.fill(0);
                block.Coefficients = Owner->Coefficients(period);
            }
            const auto& weights = Owner->Weights()[offset];
            AudioInterpolationMath::Accumulate(block.Moments.data(), delta, weights.data());
        }
        Current = value; LastInput = clock;
    }
    double Read(u64 clock)
    {
        const unsigned mix = Owner->MixInterval();
        if (clock % mix || clock < LastInput || (HasOutput && clock < LastOutput))
            throw std::invalid_argument("Interpolation output grid or time invalid");
        Flush();
        double value = Current;
        size_t keep = 0;
        for (size_t i = 0; i < Direct.size(); ++i)
        {
            const auto tail = Direct[i];
            const u64 age = clock - tail.Clock;
            if (age >= tail.Support) continue;
            value -= tail.Delta * (1 - Owner->Step(tail.Period, age));
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
            value -= AudioInterpolationMath::Dot(block.Moments.data(), coefficients[row].data());
            if (keep != i) Blocks[keep] = block;
            ++keep;
        }
        Blocks.resize(keep);
        HasOutput = true; LastOutput = clock;
        return value;
    }
};
}
