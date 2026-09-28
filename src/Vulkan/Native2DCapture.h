// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Device.h"
#include <span>
#include <array>
#include <cstddef>
#include <vector>
namespace melonDS::Vulkan::Native2D {
class Pipeline;
struct CaptureRow {
    uint32_t control, line, rawFirst, sourceScale;
    uint32_t sourceX, sourceAbort;
};
struct CaptureCommand {
    CaptureRow row;
    uint32_t hasB, sourceWord, gpuMask, hiresMask, scaledRawFirst, hiresWriteMask;
    std::array<uint16_t, 256> sourceB;
};
static_assert(sizeof(CaptureCommand) == 560 && offsetof(CaptureCommand, sourceB) == 48);
// Native guest capture only. Publication/CPU demand and dirty ranges are owned
// by the renderer; this class never writes guest RAM or evaluates CPU 2D pixels.
class CapturePipeline {
public:
    CapturePipeline(std::shared_ptr<Device> owner, std::span<const uint32_t> shader);
    ~CapturePipeline();
    CapturePipeline(const CapturePipeline&) = delete;
    CapturePipeline& operator=(const CapturePipeline&) = delete;
    // Four-byte aligned range, contained in one 128KiB bank. Only completed
    // capture ranges may be read; untouched bank contents are not guest VRAM.
    void ReadRange(uint32_t bank, uint32_t firstHalfword, std::span<uint16_t> destination);
    // Completed guest capture bytes only. Retaining this snapshot makes the next
    // writer copy-on-write, so queued consumers keep their original byte version.
    std::shared_ptr<Device::Buffer> Snapshot() const;
    uint64_t Revision() const;
    // Optional presentation-only derivative. Allocation/limit failure leaves
    // exact guest capture enabled. Configure once before submitting commands.
    bool EnableHires(std::span<const uint32_t> shader, uint32_t scale);
    // Drop display-scale capture storage between batches (scale change).
    void DisableHires();
    const std::shared_ptr<Device::Buffer>& Hires() const { return hires; }
    uint32_t HiresScale() const { return hiresScale; }
    uint32_t HiresPitch() const { return hiresPitch; }
private:
    friend class Pipeline;
    // Queries and resource reuse drain the one submitted writer first. The
    // writer and capture owner both detach this link before either is freed.
    void CompletePending() const;
    Pipeline* pendingWriter = nullptr;
    // Prepare all allocation/descriptors before the shared command begins.
    // Record adds ordered capture dispatches to the caller's 2D submission.
    void Prepare(std::span<const CaptureCommand> commands, const std::shared_ptr<Device::Buffer>& raw,
        uint32_t rawWords, const std::shared_ptr<Device::Image>& native3D,
        const std::shared_ptr<Device::Buffer>& scaledRaw = {},
        const std::shared_ptr<Device::Image>& display3D = {});
    void Record(VkCommandBuffer command);
    void Complete() { initialized = true; hiresInitialized = bool(hires); ++revision; dispatches.clear(); previousBanks.reset(); }
    void Cleanup();
    std::shared_ptr<Device> owner;
    VkDescriptorSetLayout bindings{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkPipeline hiresPipeline{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    std::shared_ptr<Device::Buffer> source, banks, landing, previousBanks;
    std::shared_ptr<Device::Buffer> hires;
    std::shared_ptr<Device::Image> blank;
    bool initialized = false;
    bool hiresInitialized = false;
    uint32_t hiresScale = 0, hiresPitch = 0;
    uint64_t revision = 0;
    std::vector<uint32_t> dispatches;
};
}
