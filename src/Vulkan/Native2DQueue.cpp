// SPDX-License-Identifier: GPL-3.0-or-later
#include "Native2DQueue.h"
#include "Native2DPipeline.h"
#include "GPU.h"
#include "GPU3D.h"
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace melonDS::Vulkan::Native2D {
namespace {
void CheckEngine(uint32_t engine) {
    if (engine > 1) throw std::invalid_argument("Invalid native 2D engine");
}
void SeedMemory(Memory& memory) {
    const uint32_t zero = 0;
    memory.AppendWords({&zero, 1}); // legal empty/blank record descriptor
}
std::vector<uint8_t> CopyView(const Memory& memory, Memory::View view) {
    std::vector<uint8_t> bytes(size_t(view.pages) * Memory::PageBytes);
    const auto words = memory.Words();
    for (uint32_t page = 0; page < view.pages; ++page)
        std::memcpy(bytes.data() + page * Memory::PageBytes,
            words.data() + words[view.table + page], Memory::PageBytes);
    return bytes;
}
}
Queue::Queue(melonDS::GPU& gpu) : gpu(gpu)
{
    SeedMemory(memory);
    records.reserve(Pipeline::MaxRecords);
    copies.reserve(Pipeline::MaxRecords);
    for (uint32_t i = 0; i < 2; ++i) {
        engines[i].object.historyRead = NoHistory;
        engines[i].object.vramMask = i ? 0x1FFFF : 0x3FFFF;
    }
}

void Queue::AddPageSource(const PageSource& source, Memory::View view)
{
    const uint32_t dest = memory.Words()[view.table + source.page] + source.half * 64;
    if (source.direct) {
        copies.push_back({source.source, source.sourceWord, dest, 64});
        return;
    }
    const StageKey key{source.source.get(), source.sourceWord};
    auto found = staging.find(key);
    if (found == staging.end()) {
        const std::array<uint32_t, 64> zero{};
        const auto offset = memory.AppendWords(zero);
        copies.push_back({source.source, source.sourceWord, offset, 64});
        found = staging.emplace(key, offset).first;
    }
    merges.push_back({found->second, dest, 64});
}

Memory::View Queue::CaptureMapped(std::span<const uint8_t> bytes, Memory::View previous,
    std::span<const uint64_t> dirty, const uint32_t* mapping,
    std::array<uint8_t, Memory::MaxPages>& masks, uint64_t& revision,
    const CapturedMemory& captured, std::vector<PageSource>* retained)
{
    if (!captured.source && !revision) {
        if (retained) retained->clear();
        return memory.Capture(bytes, previous, dirty);
    }
    overrides.clear();
    std::vector<PageSource> sources;
    const uint32_t pages = uint32_t(bytes.size() / Memory::PageBytes);
    for (uint32_t page = 0; page < pages; ++page) {
        const uint32_t address = page * Memory::PageBytes, mapped = mapping[address >> 14];
        uint8_t mask = 0;
        if (captured.source)
            for (uint32_t half = 0; half < 2; ++half)
                for (uint32_t bank = 0; bank < 4; ++bank)
                    if ((mapped & (1u << bank)) && captured.owned[bank][(address & 0x1FFFF) / 256 + half])
                        mask |= 1u << (half * 4 + bank);
        if (mask || masks[page]) {
            auto& replacement = overrides.emplace_back();
            replacement.page = page;
            replacement.force = mask != masks[page] || (mask && revision != captured.revision);
            replacement.words.fill(0);
            if (!mask) std::memcpy(replacement.words.data(), bytes.data() + address, 512);
            else for (uint32_t half = 0; half < 2; ++half) {
                const uint32_t gpuBanks = (mask >> (half * 4)) & 15;
                for (uint32_t bank = 0; bank < 9; ++bank) {
                    if (!(mapped & (1u << bank))) continue;
                    if (gpuBanks & (1u << bank)) {
                        sources.push_back({captured.source, page, half,
                            bank * 32768 + (address & 0x1FFFF) / 4 + half * 64,
                            mapped == (1u << bank)});
                    } else {
                        // Rebuild only the CPU contribution. Flat VRAM still
                        // contains stale bits from GPU-owned banks and cannot
                        // be used as the base of a hardware-OR mapping.
                        const auto* ram = gpu.VRAM[bank] + ((address + half * 256) & gpu.VRAMMask[bank]);
                        for (uint32_t word = 0; word < 64; ++word) {
                            uint32_t value;
                            std::memcpy(&value, ram + word * 4, 4);
                            replacement.words[half * 64 + word] |= value;
                        }
                    }
                }
            }
        }
        masks[page] = mask;
    }
    const auto view = memory.Capture(bytes, previous, dirty, overrides);
    for (const auto& source : sources)
        if (!previous || view.generation != previous.generation ||
            memory.Words()[view.table + source.page] != memory.Words()[previous.table + source.page])
            AddPageSource(source, view);
    revision = captured.revision;
    if (retained) *retained = std::move(sources);
    return view;
}

void Queue::CaptureSprites(uint32_t engine, uint32_t line, const CapturedMemory& captured)
{
    CheckEngine(engine);
    auto& reg = engine ? gpu.GPU2D_B : gpu.GPU2D_A;
    if (!reg.Enabled) return; // preserve the last, possibly post-mosaic OBJ state
    auto& current = engines[engine];
    auto next = current.object;
    Memory::View obj;
    u8* bytes; u32 mask;
    reg.GetOBJVRAM(bytes, mask);
    if (engine == 0) {
        auto dirty = gpu.VRAMDirty_AOBJ.DeriveState(gpu.VRAMMap_AOBJ, gpu);
        gpu.MakeVRAMFlat_AOBJCoherent(dirty);
        if (reg.OBJEnable) obj = CaptureMapped({bytes, size_t(mask) + 1}, current.obj, dirty.Data,
            gpu.VRAMMap_AOBJ, current.objGPU, current.objRevision, captured, &current.objSources);
    } else {
        auto dirty = gpu.VRAMDirty_BOBJ.DeriveState(gpu.VRAMMap_BOBJ, gpu);
        gpu.MakeVRAMFlat_BOBJCoherent(dirty);
        if (reg.OBJEnable) obj = CaptureMapped({bytes, size_t(mask) + 1}, current.obj, dirty.Data,
            gpu.VRAMMap_BOBJ, current.objGPU, current.objRevision, captured, &current.objSources);
    }
    // Small OAM snapshots compare two pages and reuse identical data; no pixel
    // selection is done on the CPU and an unrelated dirty-bit consumer is safe.
    Memory::View oam;
    if (reg.OBJEnable)
        oam = memory.Capture({gpu.OAM + engine * 1024, 1024}, current.oam);
    next.dispCnt = reg.DispCnt; next.line = line; next.mosaicLine = reg.OBJMosaicLine;
    next.enabled = reg.OBJEnable; next.vramTable = obj.table; next.vramMask = mask;
    next.oamTable = oam.table; next.historyRead = NoHistory;
    current.obj = obj; current.oam = oam; current.object = next;
    if (!reg.OBJEnable) { current.objSources.clear(); current.objGPU.fill(0); current.objRevision = 0; }
    if (reg.OBJEnable) CaptureHiresObjects(engine, captured);
    else current.hiresOBJ = {};
}

void Queue::CaptureHiresObjects(uint32_t engine, const CapturedMemory& captured)
{
    // Enhanced bitmap OBJ provenance at this prefetch event: 64 ownership
    // words, then one bank byte per 16 KiB OBJ block (0xFF = not a single
    // A-D bank). The GPU reads hires subpixels when it renders this snapshot;
    // captures are recorded only after both engines consumed their prefetch,
    // and a pending writer to an OBJ bank is submitted before this event, so
    // those subpixels are the ones present at the prefetch.
    auto& current = engines[engine];
    const auto* mapping = engine ? gpu.VRAMMap_BOBJ : gpu.VRAMMap_AOBJ;
    std::array<uint32_t, Memory::PageWords> page{};
    std::fill_n(page.begin() + 64, 4, ~0u);
    uint32_t banks = 0;
    for (uint32_t i = 0; i < (engine ? 8u : 16u); ++i) {
        const uint32_t mask = mapping[i];
        if (!std::has_single_bit(mask) || !(mask & 15)) continue;
        const uint32_t bank = std::countr_zero(mask);
        if (captured.hiresOwned[bank].none()) continue;
        banks |= mask;
        page[64 + i / 4] = (page[64 + i / 4] & ~(255u << ((i % 4) * 8))) | (bank << ((i % 4) * 8));
    }
    if (!banks) { current.hiresOBJ = {}; return; }
    for (uint32_t bank = 0; bank < 4; ++bank) {
        if (!(banks & (1u << bank))) continue;
        for (uint32_t bit = 0; bit < 512; ++bit)
            if (captured.hiresOwned[bank][bit]) page[bank * 16 + bit / 32] |= 1u << (bit % 32);
    }
    current.hiresOBJ = memory.Capture({reinterpret_cast<const uint8_t*>(page.data()), sizeof(page)}, current.hiresOBJ);
}

void Queue::CaptureHiresBackground(uint32_t engine, const CapturedMemory& captured)
{
    auto& current = engines[engine];
    const auto& reg = engine ? gpu.GPU2D_B : gpu.GPU2D_A;
    const auto* mapping = engine ? gpu.VRAMMap_BBG : gpu.VRAMMap_ABG;
    // One immutable page: 64 ownership words, 32 packed bank indices, two
    // signed B/D affine pairs, and a BG eligibility mask. Zero table disables
    // enhancement. BG reads finish before this batch's capture writes.
    std::array<uint32_t, Memory::PageWords> page{};
    std::fill_n(page.begin() + 64, 8, ~0u);
    uint32_t banks = 0;
    for (uint32_t i = 0; i < (engine ? 8u : 32u); ++i) {
        const uint32_t mask = mapping[i];
        if (!std::has_single_bit(mask) || !(mask & 15)) continue;
        const uint32_t bank = std::countr_zero(mask);
        if (captured.hiresOwned[bank].none()) continue;
        banks |= mask;
        page[64 + i / 4] = (page[64 + i / 4] & ~(255u << ((i % 4) * 8))) |
            (bank << ((i % 4) * 8));
    }
    if (!banks) { current.hiresBG = {}; return; }
    for (uint32_t bank = 0; bank < 4; ++bank) {
        if (!(banks & (1u << bank))) continue;
        for (uint32_t bit = 0; bit < 512; ++bit)
            if (captured.hiresOwned[bank][bit]) page[bank * 16 + bit / 32] |= 1u << (bit % 32);
    }
    for (uint32_t bg = 0; bg < 2; ++bg) {
        page[72 + bg] = uint16_t(reg.BGRotB[bg]) | (uint32_t(uint16_t(reg.BGRotD[bg])) << 16);
        if (!(reg.BGCnt[bg + 2] & 64)) page[74] |= 1u << (bg + 2);
    }
    current.hiresBG = memory.Capture({reinterpret_cast<const uint8_t*>(page.data()), sizeof(page)}, current.hiresBG);
}

void Queue::CaptureBackground(uint32_t engine, const CapturedMemory& captured)
{
    auto& reg = engine ? gpu.GPU2D_B : gpu.GPU2D_A;
    auto& current = engines[engine];
    NonStupidBitField<64> ext;
    NonStupidBitField<16> objExt;
    u8* bytes; u32 mask;
    reg.GetBGVRAM(bytes, mask);
    if (engine == 0) {
        auto dirty = gpu.VRAMDirty_ABG.DeriveState(gpu.VRAMMap_ABG, gpu);
        gpu.MakeVRAMFlat_ABGCoherent(dirty);
        current.bg = CaptureMapped({bytes, size_t(mask) + 1}, current.bg, dirty.Data,
            gpu.VRAMMap_ABG, current.bgGPU, current.bgRevision, captured);
        ext = gpu.VRAMDirty_ABGExtPal.DeriveState(gpu.VRAMMap_ABGExtPal, gpu);
        gpu.MakeVRAMFlat_ABGExtPalCoherent(ext);
        objExt = gpu.VRAMDirty_AOBJExtPal.DeriveState(&gpu.VRAMMap_AOBJExtPal, gpu);
        gpu.MakeVRAMFlat_AOBJExtPalCoherent(objExt);
    } else {
        auto dirty = gpu.VRAMDirty_BBG.DeriveState(gpu.VRAMMap_BBG, gpu);
        gpu.MakeVRAMFlat_BBGCoherent(dirty);
        current.bg = CaptureMapped({bytes, size_t(mask) + 1}, current.bg, dirty.Data,
            gpu.VRAMMap_BBG, current.bgGPU, current.bgRevision, captured);
        ext = gpu.VRAMDirty_BBGExtPal.DeriveState(gpu.VRAMMap_BBGExtPal, gpu);
        gpu.MakeVRAMFlat_BBGExtPalCoherent(ext);
        objExt = gpu.VRAMDirty_BOBJExtPal.DeriveState(&gpu.VRAMMap_BOBJExtPal, gpu);
        gpu.MakeVRAMFlat_BOBJExtPalCoherent(objExt);
    }
    std::array<uint64_t, 2> dirty{};
    const auto update = [&](uint32_t page, const uint8_t* source) {
        std::memcpy(current.paletteBytes.data() + page * 512, source, 512);
        dirty[page / 64] |= uint64_t(1) << (page % 64);
    };
    const uint32_t paletteBits = (gpu.PaletteDirty >> (engine * 2)) & 0x33;
    const uint8_t* palette = gpu.Palette + engine * 1024;
    if (!current.palette || (paletteBits & 0x11)) update(0, palette);
    if (!current.palette || (paletteBits & 0x22)) update(65, palette + 512);
    const uint8_t* extBytes = engine ? gpu.VRAMFlat_BBGExtPal : gpu.VRAMFlat_ABGExtPal;
    const uint8_t* objBytes = engine ? gpu.VRAMFlat_BOBJExtPal : gpu.VRAMFlat_AOBJExtPal;
    for (uint32_t p = 0; p < 64; ++p)
        if (!current.palette || ext[p]) update(p + 1, extBytes + p * 512);
    for (uint32_t p = 0; p < 16; ++p)
        if (!current.palette || objExt[p]) update(p + 66, objBytes + p * 512);
    current.palette = memory.Capture(current.paletteBytes, current.palette, dirty);
    gpu.PaletteDirty &= ~(0x33u << (engine * 2));
    CaptureHiresBackground(engine, captured);
}

void Queue::CaptureLine(uint32_t engine, uint32_t physicalLine, uint32_t source3DScale,
    const std::shared_ptr<Device::Buffer>& lcdc, uint32_t lcdcSegments, const CapturedMemory& captured,
    uint32_t hiresLCDC)
{
    CheckEngine(engine);
    if (lcdcSegments > 3 || (lcdcSegments && (!lcdc || engine)))
        throw std::invalid_argument("Invalid native LCDC snapshot");
    if (physicalLine >= 192 || records.size() == Pipeline::MaxRecords)
        throw std::length_error("Native 2D record batch is full");
    auto& reg = engine ? gpu.GPU2D_B : gpu.GPU2D_A;
    auto& current = engines[engine];
    const uint32_t vcount = gpu.VCount;
    if (vcount < 192 && reg.Enabled && !reg.ForcedBlank) CaptureBackground(engine, captured);
    Record record{};
    record.hiresLCDC = hiresLCDC;
    record.layers = PackLine(reg, vcount, 1);
    record.layers.reserved0 = current.hiresBG.table;
    // Only the event that consumes a fresh prefetch renders OBJ subpixels;
    // later reuse of this OBJ state reads the scaled history instead.
    if (current.object.historyRead == NoHistory) record.layers.reserved1 = current.hiresOBJ.table;
    // The core skips both 2D draws outside visible VCOUNT. The native result is
    // unused there; suppress memory lookup while the final pass emits white.
    if (vcount >= 192) record.layers.enabled = 0;
    record.object = current.object;
    record.bgTable = current.bg.table; record.paletteTable = current.palette.table;
    record.physicalLine = physicalLine;
    record.screen = engine ^ (gpu.ScreenSwap ? 0u : 1u);
    record.source3DLine = vcount; record.source3DX = gpu.GPU3D.RenderXPos;
    record.source3DAbort = gpu.GPU3D.AbortFrame; record.source3DScale = source3DScale;
    auto& final = record.finalDisplay;
    final.engine = engine; final.dispCnt = reg.DispCnt;
    final.masterBrightness = engine ? gpu.MasterBrightnessB : gpu.MasterBrightnessA;
    final.screensEnabled = gpu.ScreensEnabled; final.vcount = vcount;
    final.vramMapped = gpu.VRAMMap_LCDC;
    const uint32_t mode = (reg.DispCnt >> 16) & (engine ? 1 : 3);
    if (vcount < 192 && (mode == 2 || mode == 3)) {
        const uint32_t bank = (reg.DispCnt >> 18) & 3;
        const uint8_t* source = mode == 3 ? reinterpret_cast<const uint8_t*>(gpu.DispFIFOBuffer) :
            gpu.VRAM[bank] + vcount * 512;
        std::array<uint32_t, 128> row;
        std::memcpy(row.data(), source, 512);
        final.sourceWordBase = memory.AppendWords(row);
        if (mode == 2 && lcdcSegments)
        {
            const uint32_t first = (lcdcSegments & 1) ? 0 : 64;
            const uint32_t count = lcdcSegments == 3 ? 128 : 64;
            copies.push_back({lcdc, bank * 32768 + vcount * 128 + first,
                final.sourceWordBase + first, count});
        }
    }
    record.objectWrite = uint32_t(records.size() + 2) * 512;
    record.rawOutput = uint32_t(records.size()) * 256;
    records.push_back(record);
    // Only actual draws advance this latch; VCOUNT outside the visible range
    // skips the software 2D draw entirely.
    if (vcount < 192) AdvanceWindowState(reg);
    current.object.historyRead = record.objectWrite;
    // This object event now lives in queued GPU output/history. The input copy
    // list owns its sources until submission; only a fresh prefetch needs an
    // additional future-reader lease across Retire.
    current.objSources.clear();
}

void Queue::CaptureHiresSource(uint32_t rawFirst, uint32_t captureIndex)
{
    const uint32_t index = rawFirst / 256;
    if (rawFirst % 256 || index >= records.size() || records[index].layers.engine || captureIndex >= 256)
        throw std::invalid_argument("Invalid hires capture source record");
    records[index].scaledCapture = captureIndex + 1;
}

void Queue::Retire()
{
    Memory next(memory.ByteLimit());
    SeedMemory(next);
    std::array<ObjectState, 2> objects;
    std::array<Memory::View, 2> objViews{}, oamViews{}, hiresOBJViews{};
    for (uint32_t engine = 0; engine < 2; ++engine) {
        objects[engine] = engines[engine].object;
        auto& object = objects[engine];
        if (object.historyRead != NoHistory) object.historyRead = engine * 512;
        else if (object.enabled) {
            objViews[engine] = next.Capture(CopyView(memory, engines[engine].obj), {});
            oamViews[engine] = next.Capture(CopyView(memory, engines[engine].oam), {});
            if (engines[engine].hiresOBJ)
                hiresOBJViews[engine] = next.Capture(CopyView(memory, engines[engine].hiresOBJ), {});
            object.vramTable = objViews[engine].table; object.oamTable = oamViews[engine].table;
        }
    }
    memory = std::move(next);
    copies.clear(); merges.clear(); staging.clear();
    for (uint32_t engine = 0; engine < 2; ++engine) {
        engines[engine].bg = {}; engines[engine].palette = {}; engines[engine].hiresBG = {};
        engines[engine].obj = objViews[engine]; engines[engine].oam = oamViews[engine];
        engines[engine].hiresOBJ = hiresOBJViews[engine];
        engines[engine].object = objects[engine];
        engines[engine].bgGPU.fill(0); engines[engine].bgRevision = 0;
        if (objects[engine].historyRead == NoHistory && objects[engine].enabled) {
            for (const auto& source : engines[engine].objSources) AddPageSource(source, objViews[engine]);
        } else {
            engines[engine].objSources.clear(); engines[engine].objGPU.fill(0); engines[engine].objRevision = 0;
        }
    }
    records.clear();
}

void Queue::DropHires()
{
    if (!records.empty()) throw std::logic_error("Native 2D hires pages are still queued");
    for (auto& engine : engines) { engine.hiresBG = {}; engine.hiresOBJ = {}; }
}
}
