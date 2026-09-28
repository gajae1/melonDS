// SPDX-License-Identifier: GPL-3.0-or-later
// CPU-only semantics of the deferred native 2D arena: a capture that follows a
// mid-scanline change publishes a new view while older views keep resolving,
// sparse dirty bits above the first mask word are honored, unchanged dirty
// pages append nothing, a forced GPU override republishes even on a clean mask
// with equal host bytes, malformed overrides are rejected without touching the
// arena, and a budget failure leaves every published view readable.
#include "Vulkan/Native2DMemory.h"
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>

using melonDS::Vulkan::Native2D::Memory;

namespace
{
constexpr uint32_t PageBytes = Memory::PageBytes;
constexpr uint32_t PageWords = Memory::PageWords;
using Page = std::array<uint8_t, PageBytes>;

unsigned checks = 0;

void Require(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

// Test-owned content spec. Expectations are rebuilt from it, never read back
// from the arena under test.
Page SpecPage(uint32_t page, uint32_t seed)
{
    Page bytes{};
    uint32_t state = 0x9E3779B9u * (page + 1u) + 0x85EBCA6Bu * (seed + 3u);
    for (uint32_t i = 0; i < PageBytes; ++i)
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        bytes[i] = uint8_t(state >> 24);
    }
    return bytes;
}

std::vector<uint8_t> SpecRegion(uint32_t pages, uint32_t seed)
{
    std::vector<uint8_t> bytes(size_t(pages) * PageBytes);
    for (uint32_t page = 0; page < pages; ++page)
    {
        const Page spec = SpecPage(page, seed);
        std::memcpy(bytes.data() + size_t(page) * PageBytes, spec.data(), PageBytes);
    }
    return bytes;
}

void WritePage(std::vector<uint8_t>& region, uint32_t page, uint32_t seed)
{
    const Page spec = SpecPage(page, seed);
    std::memcpy(region.data() + size_t(page) * PageBytes, spec.data(), PageBytes);
}

std::array<uint32_t, PageWords> SpecWords(uint32_t page, uint32_t seed)
{
    std::array<uint32_t, PageWords> words{};
    const Page spec = SpecPage(page, seed);
    std::memcpy(words.data(), spec.data(), PageBytes);
    return words;
}

// A view starts with its page-to-word table; the arena is only read through it.
uint32_t PageSlot(const Memory& memory, Memory::View view, uint32_t page)
{
    Require(page < view.pages, "view page is inside the region");
    const auto words = memory.Words();
    Require(size_t(view.table) + page < words.size(), "view holds a slot per page");
    return words[size_t(view.table) + page];
}

const uint8_t* ViewPage(const Memory& memory, Memory::View view, uint32_t page)
{
    const auto words = memory.Words();
    const uint32_t slot = PageSlot(memory, view, page);
    Require(slot <= words.size() && PageWords <= words.size() - slot, "page slot is inside the arena");
    return reinterpret_cast<const uint8_t*>(words.data() + slot);
}

void RequirePage(const Memory& memory, Memory::View view, uint32_t page, const Page& expected, const char* message)
{
    Require(std::memcmp(ViewPage(memory, view, page), expected.data(), PageBytes) == 0, message);
}

bool SameView(Memory::View a, Memory::View b)
{
    return a.table == b.table && a.pages == b.pages && a.generation == b.generation;
}

template<class Body> bool ThrowsInvalidArgument(Body body)
{
    try { body(); }
    catch (const std::invalid_argument&) { return true; }
    catch (...) { return false; }
    return false;
}

template<class Body> bool ThrowsLengthError(Body body)
{
    try { body(); }
    catch (const std::length_error&) { return true; }
    catch (...) { return false; }
    return false;
}

// A 65-page region spans two dirty words. Page 64 changes; the second word
// carries bits for pages that no longer exist, which must be ignored.
void SparsePageAboveFirstMaskWord()
{
    constexpr uint32_t Pages = 65;
    constexpr uint32_t ChangedPage = 64;
    Memory memory;
    std::vector<uint8_t> region = SpecRegion(Pages, 11);
    const Memory::View first = memory.Capture(region, {});
    Require(first.pages == Pages && first.table != 0, "first capture publishes every page");
    const size_t firstBytes = memory.ByteSize();
    for (uint32_t page = 0; page < Pages; ++page)
        RequirePage(memory, first, page, SpecPage(page, 11), "first view holds the region spec");

    WritePage(region, ChangedPage, 12);
    // Word 0 marks unchanged pages below the change (compare must skip them
    // without appending); word 1 marks the change and seven tail bits above
    // the region, one for every page that does not exist.
    const std::array<uint64_t, 2> dirty = {0x8000000000000021ull, 0xFFFFFFFFFFFFFFFFull};
    const Memory::View second = memory.Capture(region, first, dirty);
    Require(second.table != first.table && second.pages == Pages, "changed page publishes a new view");
    Require(second.generation == first.generation, "capture keeps the reset generation");
    Require(memory.ByteSize() == firstBytes + (PageWords + Pages) * sizeof(uint32_t),
        "only the changed page and the new table were appended");
    for (uint32_t page = 0; page < Pages; ++page)
    {
        if (page == ChangedPage)
            Require(PageSlot(memory, second, page) != PageSlot(memory, first, page), "changed page takes a fresh slot");
        else
            Require(PageSlot(memory, second, page) == PageSlot(memory, first, page), "unchanged page reuses its slot");
        RequirePage(memory, second, page, SpecPage(page, page == ChangedPage ? 12 : 11),
            "second view holds the current region");
    }
    for (uint32_t page = 0; page < Pages; ++page)
        RequirePage(memory, first, page, SpecPage(page, 11), "mid-scanline view stays immutable");
}

void UnchangedDirtyPagesReuseTheView()
{
    Memory memory;
    const std::vector<uint8_t> region = SpecRegion(6, 21);
    const Memory::View first = memory.Capture(region, {});
    const size_t firstBytes = memory.ByteSize();
    const std::array<uint64_t, 1> dirty = {0x2ull | 0x8ull | 0x20ull}; // pages 1, 3, 5
    const Memory::View second = memory.Capture(region, first, dirty);
    Require(SameView(second, first), "unchanged dirty pages reuse the same view");
    Require(memory.ByteSize() == firstBytes, "unchanged dirty pages append nothing");
    const Memory::View third = memory.Capture(region, first, {}); // empty mask compares every page
    Require(SameView(third, first), "full comparison of an unchanged region reuses the view");
    Require(memory.ByteSize() == firstBytes, "full comparison of an unchanged region appends nothing");
}

void ForcedOverrideOnCleanMask()
{
    Memory memory;
    const std::vector<uint8_t> region = SpecRegion(4, 31);
    const Memory::View first = memory.Capture(region, {});
    const size_t firstBytes = memory.ByteSize();
    const std::array<uint64_t, 1> clean = {0};
    const Memory::PageOverride forced{2, true, SpecWords(2, 32)};
    const Memory::View second = memory.Capture(region, first, clean, {&forced, 1});
    Require(!SameView(second, first), "forced override republishes the view");
    Require(second.generation == first.generation && second.pages == 4, "forced override keeps region and generation");
    Require(memory.ByteSize() == firstBytes + (PageWords + 4) * sizeof(uint32_t),
        "forced override appends only the GPU page and the new table");
    Require(PageSlot(memory, second, 2) != PageSlot(memory, first, 2), "override page takes a fresh slot");
    RequirePage(memory, second, 2, SpecPage(2, 32), "override words are visible in the new view");
    for (uint32_t page = 0; page < 4; ++page)
    {
        if (page == 2) continue;
        Require(PageSlot(memory, second, page) == PageSlot(memory, first, page), "unaffected page keeps its slot");
        RequirePage(memory, second, page, SpecPage(page, 31), "unaffected page keeps the CPU bytes");
    }
    RequirePage(memory, first, 2, SpecPage(2, 31), "the superseded view keeps its own page");
    // A soft override that matches the published bytes must not append even
    // though its page is absent from the clean dirty mask.
    const size_t secondBytes = memory.ByteSize();
    const Memory::PageOverride soft{2, false, SpecWords(2, 32)};
    const Memory::View third = memory.Capture(region, second, clean, {&soft, 1});
    Require(SameView(third, second) && memory.ByteSize() == secondBytes,
        "matching soft override reuses the view");
}

void OverrideRejection()
{
    Memory memory;
    const std::vector<uint8_t> region = SpecRegion(4, 41);
    const Memory::View first = memory.Capture(region, {});
    const size_t firstBytes = memory.ByteSize();
    const std::array<Memory::PageOverride, 2> duplicate = {{{1, false, SpecWords(1, 42)}, {1, true, SpecWords(1, 43)}}};
    Require(ThrowsInvalidArgument([&] { memory.Capture(region, first, {}, duplicate); }),
        "duplicate page override is rejected");
    Require(memory.ByteSize() == firstBytes, "rejected duplicate leaves the arena size unchanged");
    const Memory::PageOverride outside{4, false, SpecWords(0, 44)};
    Require(ThrowsInvalidArgument([&] { memory.Capture(region, first, {}, {&outside, 1}); }),
        "override past the region is rejected");
    Require(memory.ByteSize() == firstBytes, "rejected override leaves the arena size unchanged");
    for (uint32_t page = 0; page < 4; ++page)
        RequirePage(memory, first, page, SpecPage(page, 41), "rejected override leaves the view readable");
    Require(SameView(memory.Capture(region, first, {}), first), "rejected override leaves capture reusable");
    Require(memory.ByteSize() == firstBytes, "reusable capture still appends nothing");
}

void BudgetFailurePreservesArena()
{
    // The constructor floor is MaxPages * (PageWords + 1) words; filling to a
    // sub-page margin makes the next changed capture fail at reserve time.
    Memory memory(size_t(Memory::MaxPages) * (PageWords + 1) * sizeof(uint32_t));
    const std::vector<uint8_t> region = SpecRegion(4, 51);
    std::vector<uint8_t> changed = region;
    WritePage(changed, 2, 52);
    const Memory::View first = memory.Capture(region, {});
    const size_t firstBytes = memory.ByteSize();
    const size_t freeWords = memory.ByteLimit() / sizeof(uint32_t) - memory.Words().size();
    Require(freeWords > 131, "budget has room to fill");
    const std::vector<uint32_t> filler(freeWords - 131, 0xC0FFEEu);
    memory.AppendWords(filler);
    const size_t filledWords = memory.Words().size();
    Require(memory.ByteLimit() - memory.ByteSize() == 131 * sizeof(uint32_t), "budget filled to the margin");

    Require(ThrowsLengthError([&] { memory.Capture(changed, first, {}); }),
        "capture past the budget reports length_error");
    Require(memory.Words().size() == filledWords && memory.ByteSize() == filledWords * sizeof(uint32_t),
        "failed capture leaves the arena size intact");
    Require(PageSlot(memory, first, 2) == memory.Words()[size_t(first.table) + 2], "failed capture keeps the table");
    for (uint32_t page = 0; page < 4; ++page)
        RequirePage(memory, first, page, SpecPage(page, 51), "failed capture preserves the published view");
    RequirePage(memory, first, 2, SpecPage(2, 51), "the rejected change never entered the arena");
    const std::array<uint64_t, 1> dirty = {0xF};
    Require(SameView(memory.Capture(region, first, dirty), first), "no-op capture still reuses the view");
    Require(memory.ByteSize() == filledWords * sizeof(uint32_t), "no-op capture at the ceiling appends nothing");
    const std::vector<uint32_t> tail(131, 0xABCDEF01u);
    const uint32_t tailOffset = memory.AppendWords(tail);
    Require(tailOffset == filledWords && memory.ByteSize() == memory.ByteLimit(), "arena stays coherent after the failure");
    Require(memory.Words()[tailOffset] == 0xABCDEF01u && memory.Words()[tailOffset + 130] == 0xABCDEF01u,
        "appended words land contiguously");
    for (uint32_t page = 0; page < 4; ++page)
        RequirePage(memory, first, page, SpecPage(page, 51), "published view survives later appends");
    Require(memory.ByteSize() == firstBytes + filler.size() * sizeof(uint32_t) + tail.size() * sizeof(uint32_t),
        "arena size accounts for every append");
}
}

int main()
{
    try
    {
        SparsePageAboveFirstMaskWord();
        UnchangedDirtyPagesReuseTheView();
        ForcedOverrideOnCleanMask();
        OverrideRejection();
        BudgetFailurePreservesArena();
        std::printf("Native 2D memory semantics: %u checks, sparse/reuse/override/rejection/budget PASS\n", checks);
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Native 2D memory semantics FAIL: %s\n", error.what());
        return 1;
    }
}
