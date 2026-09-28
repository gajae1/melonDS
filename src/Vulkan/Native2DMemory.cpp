// SPDX-License-Identifier: GPL-3.0-or-later
#include "Native2DMemory.h"
#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace melonDS::Vulkan::Native2D {
Memory::Memory(size_t byteLimit) : limitWords(byteLimit / sizeof(uint32_t))
{
    if (byteLimit % sizeof(uint32_t) || limitWords > std::numeric_limits<uint32_t>::max() ||
        limitWords < MaxPages * (PageWords + 1))
        throw std::invalid_argument("Invalid native 2D memory budget");
}

void Memory::Reserve(size_t extraWords)
{
    if (extraWords > limitWords - storage.size())
        throw std::length_error("Native 2D memory batch is full");
    const size_t required = storage.size() + extraWords;
    if (required > storage.capacity())
        storage.reserve(std::min(limitWords, std::max(required, std::max<size_t>(262144, storage.capacity() * 2))));
}

Memory::View Memory::Capture(std::span<const uint8_t> bytes, View previous,
    std::span<const uint64_t> dirty, std::span<const PageOverride> overrides)
{
    if (bytes.empty() || bytes.size() % PageBytes || bytes.size() > MaxPages * PageBytes)
        throw std::invalid_argument("Invalid native 2D memory region");
    const uint32_t count = uint32_t(bytes.size() / PageBytes);
    if (!dirty.empty() && dirty.size() != (count + 63u) / 64u)
        throw std::invalid_argument("Invalid native 2D dirty mask");
    const bool valid = previous.generation == generation && previous.pages == count;
    // Most scanlines have no GPU-owned pages. Do not clear a full pointer
    // table for that path; only entries below count can ever be accessed.
    std::array<const PageOverride*, MaxPages> replaced;
    if (!overrides.empty()) std::fill_n(replaced.data(), count, nullptr);
    for (const auto& page : overrides) {
        if (page.page >= count || replaced[page.page])
            throw std::invalid_argument("Invalid native 2D page override");
        replaced[page.page] = &page;
    }
    if (valid && (previous.table > storage.size() || count > storage.size() - previous.table))
        throw std::invalid_argument("Invalid native 2D memory view");

    std::array<uint32_t, MaxPages> changed;
    uint32_t changedCount = 0;
    const auto inspect = [&](uint32_t page) {
        const auto* replacement = overrides.empty() ? nullptr : replaced[page];
        const bool force = replacement && replacement->force;
        if (valid && !force)
        {
            const uint32_t offset = storage[previous.table + page];
            if (offset > storage.size() || PageWords > storage.size() - offset)
                throw std::invalid_argument("Invalid native 2D page offset");
            const void* source = replacement ? static_cast<const void*>(replacement->words.data()) : bytes.data() + page * PageBytes;
            if (std::memcmp(storage.data() + offset, source, PageBytes) == 0)
                return;
        }
        changed[changedCount++] = page;
    };
    if (valid && !dirty.empty())
    {
        std::array<uint64_t, MaxPages / 64> candidates;
        auto masks = dirty;
        if (!overrides.empty()) {
            std::copy(dirty.begin(), dirty.end(), candidates.begin());
            for (const auto& page : overrides)
                candidates[page.page / 64] |= uint64_t(1) << (page.page % 64);
            masks = {candidates.data(), dirty.size()};
        }
        for (uint32_t word = 0; word < masks.size(); ++word) {
            uint64_t bits = masks[word];
            if (word + 1 == masks.size() && count % 64)
                bits &= (uint64_t(1) << (count % 64)) - 1;
            while (bits) {
                const uint32_t page = word * 64 + std::countr_zero(bits);
                bits &= bits - 1;
                inspect(page);
            }
        }
    }
    else
        for (uint32_t page = 0; page < count; ++page) inspect(page);
    if (!changedCount) return previous;

    // Reserve the whole transaction before changing storage or publishing a view.
    // The local table also avoids pointers invalidated by vector growth.
    std::array<uint32_t, MaxPages> pageMap;
    if (valid)
        std::copy_n(storage.data() + previous.table, count, pageMap.data());
    const size_t oldSize = storage.size();
    const size_t extra = size_t(changedCount) * PageWords + count;
    Reserve(extra);
    storage.resize(oldSize + extra);
    size_t cursor = oldSize;
    for (uint32_t i = 0; i < changedCount; ++i)
    {
        const uint32_t page = changed[i];
        pageMap[page] = uint32_t(cursor);
        const auto* replacement = overrides.empty() ? nullptr : replaced[page];
        const void* source = replacement ? static_cast<const void*>(replacement->words.data()) : bytes.data() + page * PageBytes;
        std::memcpy(storage.data() + cursor, source, PageBytes);
        cursor += PageWords;
    }
    std::copy_n(pageMap.data(), count, storage.data() + cursor);
    return {uint32_t(cursor), count, generation};
}

uint32_t Memory::AppendWords(std::span<const uint32_t> words)
{
    const size_t offset = storage.size();
    Reserve(words.size());
    storage.insert(storage.end(), words.begin(), words.end());
    return uint32_t(offset);
}

void Memory::Reset()
{
    storage.clear();
    ++generation;
}
}
