// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Native2DMemory.h"
#include "Native2DRecord.h"
#include "Native2DPipeline.h"
#include <array>
#include <vector>
#include <utility>
#include <bitset>
#include <unordered_map>
namespace melonDS { class GPU; }
namespace melonDS::Vulkan::Native2D {
struct CapturedMemory {
    std::shared_ptr<Device::Buffer> source;
    uint64_t revision = 0;
    std::array<std::bitset<512>, 4> owned{};
    // CPU demand may synchronize guest bytes while the display derivative is
    // still valid. Enhanced ownership therefore cannot be inferred from owned.
    std::array<std::bitset<512>, 4> hiresOwned{};
};
// Captures guest state at its original events; never computes a pixel. The
// renderer submits Records(), then calls Retire only after GPU completion.
class Queue {
public:
    explicit Queue(melonDS::GPU& gpu);
    void CaptureSprites(uint32_t engine, uint32_t line, const CapturedMemory& captured = {});
    void CaptureLine(uint32_t engine, uint32_t physicalLine, uint32_t source3DScale,
        const std::shared_ptr<Device::Buffer>& lcdc = {}, uint32_t lcdcSegments = 0,
        const CapturedMemory& captured = {}, uint32_t hiresLCDC = 0);
    void CaptureHiresSource(uint32_t rawFirst, uint32_t captureIndex);
    const Memory& Data() const { return memory; }
    std::span<const Record> Records() const { return records; }
    // Transfer one-use GPU input leases to the submission that consumes them.
    std::vector<MemoryCopy> TakeCopies() { return std::exchange(copies, {}); }
    std::span<const MemoryMerge> Merges() const { return merges; }
    // Preserve a prefetched OBJ snapshot across the batch boundary; rendered
    // OBJ history was copied by Pipeline into its two persistent engine slots.
    void Retire();
private:
    struct PageSource {
        std::shared_ptr<Device::Buffer> source;
        uint32_t page, half, sourceWord;
        bool direct;
    };
    struct StageKey {
        const Device::Buffer* source;
        uint32_t word;
        bool operator==(const StageKey&) const = default;
    };
    struct StageHash {
        size_t operator()(const StageKey& key) const {
            return std::hash<const void*>{}(key.source) ^ (size_t(key.word) * 2654435761u);
        }
    };
    struct Engine {
        Memory::View bg, palette, obj, oam, hiresBG, hiresOBJ;
        std::array<uint8_t, PaletteEntries * 2> paletteBytes{};
        ObjectState object{};
        std::array<uint8_t, Memory::MaxPages> bgGPU{}, objGPU{};
        uint64_t bgRevision = 0, objRevision = 0;
        std::vector<PageSource> objSources;
    };
    void CaptureBackground(uint32_t engine, const CapturedMemory& captured);
    void CaptureHiresBackground(uint32_t engine, const CapturedMemory& captured);
    void CaptureHiresObjects(uint32_t engine, const CapturedMemory& captured);
    Memory::View CaptureMapped(std::span<const uint8_t> bytes, Memory::View previous,
        std::span<const uint64_t> dirty, const uint32_t* mapping,
        std::array<uint8_t, Memory::MaxPages>& masks, uint64_t& revision,
        const CapturedMemory& captured, std::vector<PageSource>* retained = nullptr);
    void AddPageSource(const PageSource& source, Memory::View view);
    melonDS::GPU& gpu;
    Memory memory;
    std::array<Engine, 2> engines;
    std::vector<Record> records;
    std::vector<MemoryCopy> copies;
    std::vector<MemoryMerge> merges;
    std::unordered_map<StageKey, uint32_t, StageHash> staging;
    std::vector<Memory::PageOverride> overrides;
};
}
