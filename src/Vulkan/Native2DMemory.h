// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>
#include <array>

namespace melonDS::Vulkan::Native2D {
// Immutable 512-byte VRAM/palette/OAM pages for deferred scanlines. Offsets are
// words in one storage buffer; each view starts with its page-to-word table.
// Reset only after every queued consumer (including retained OBJ state) retires.
class Memory {
public:
    static constexpr uint32_t PageBytes = 512;
    static constexpr uint32_t PageWords = PageBytes / sizeof(uint32_t);
    static constexpr uint32_t MaxPages = 1024; // engine A BG VRAM, 512 KiB
    struct View {
        uint32_t table = 0, pages = 0;
        uint64_t generation = 0;
        explicit operator bool() const { return pages != 0; }
    };
    // CPU baseline for a page whose final bytes will also receive GPU input.
    // Force preserves a new source epoch even when this host baseline is equal.
    struct PageOverride {
        uint32_t page;
        bool force;
        std::array<uint32_t, PageWords> words;
    };
    explicit Memory(size_t byteLimit = 16u * 1024u * 1024u);
    // Dirty bits describe the existing GPU::VRAMDirtyGranularity pages. Empty
    // bits compare all pages, useful for the small standard palette/OAM views.
    // Unchanged pages and entire views reuse storage. A failed allocation or
    // limit check leaves all previous views and the arena contents intact.
    View Capture(std::span<const uint8_t> bytes, View previous,
        std::span<const uint64_t> dirty = {}, std::span<const PageOverride> overrides = {});
    uint32_t AppendWords(std::span<const uint32_t> words);
    std::span<const uint32_t> Words() const { return storage; }
    size_t ByteSize() const { return storage.size() * sizeof(uint32_t); }
    size_t ByteLimit() const { return limitWords * sizeof(uint32_t); }
    void Reset();
private:
    void Reserve(size_t extraWords);
    std::vector<uint32_t> storage;
    size_t limitWords;
    uint64_t generation = 1;
};
}
