// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Device.h"
#include "Native2DRecord.h"
#include "Native2DCapture.h"
#include <array>
#include <memory>
#include <span>
#include <vector>

namespace melonDS::Vulkan::Native2D {
struct MemoryCopy {
    std::shared_ptr<Device::Buffer> source;
    uint32_t sourceWord, destinationWord, words;
};
struct MemoryMerge {
    uint32_t sourceWord, destinationWord, words;
};
static_assert(sizeof(MemoryMerge) == 12);
class Pipeline {
public:
    static constexpr uint32_t MaxRecords = 512;
    Pipeline(std::shared_ptr<Device> owner, std::span<const uint32_t> shader,
        std::span<const uint32_t> mergeShader = {}, uint32_t scale = 1);
    ~Pipeline();
    Pipeline(const Pipeline&) = delete;
    Pipeline& operator=(const Pipeline&) = delete;
    // Inputs are copied before recording. Queued rows retain their own VRAM and
    // register epochs; only an explicit history dependency/repeated destination
    // splits the dispatch. All work shares one submission and completion fence.
    // display3D feeds enhanced subpixels only and falls back to native3D/blank;
    // it must match each sampled row's source3DScale like the guest image.
    // Synchronous form: Submit() followed by Complete().
    void Render(std::span<const uint32_t> memory, std::span<const Record> lines,
        const std::shared_ptr<Device::Image>& native3D, uint32_t buffer,
        CapturePipeline* capture = nullptr, std::span<const CaptureCommand> captures = {},
        std::vector<MemoryCopy> copies = {}, std::span<const MemoryMerge> merges = {},
        const std::shared_ptr<Device::Image>& display3D = {}, uint64_t memoryEpoch = 0);
    // A nonzero memoryEpoch identifies an append-only immutable arena. Reuse
    // it only while the prefix of memory is unchanged; advance it on reset.
    // The device retains that prefix, including GPU copy/merge results, across
    // submissions and allocation growth. Zero uploads all bytes (legacy API).
    // Deferred form. Completes any earlier batch, copies new host input, then
    // queues this batch without waiting. On return the caller's spans may be
    // reused; copy sources, 3D images, hires capture storage and the replaced
    // scaled history stay leased until completion. Output() is published for
    // later work on this device's queue: the next Device::Begin() completes the
    // batch first, and ReadFrame/ReadRaw/SetScale complete it explicitly.
    // A batch with capture commands completes before Submit returns, because
    // CapturePipeline publishes Revision/Snapshot only on completion and those
    // host queries do not drain the device. A deferred batch therefore never
    // writes capture-owned storage and never references the CapturePipeline.
    void Submit(std::span<const uint32_t> memory, std::span<const Record> lines,
        const std::shared_ptr<Device::Image>& native3D, uint32_t buffer,
        CapturePipeline* capture = nullptr, std::span<const CaptureCommand> captures = {},
        std::vector<MemoryCopy> copies = {}, std::span<const MemoryMerge> merges = {},
        const std::shared_ptr<Device::Image>& display3D = {}, uint64_t memoryEpoch = 0);
    // Waits for a deferred batch and releases its leases. No-op when nothing
    // is pending; throws on device failure. Capture revisions are published
    // only by the synchronous capture branch inside Submit.
    void Complete();
    bool Pending() const { return inFlight.active; }
    // Input storage placement: 0 mapped device-local, 1 device-local filled
    // from an upload buffer, 2 mapped host memory (original fallback).
    uint32_t InputTier() const { return inputTier; }
    const std::shared_ptr<Device::Image>& Output(uint32_t buffer, uint32_t screen) const;
    void ReadRaw(uint32_t first, std::span<uint32_t> destination);
    // Stop clears visible frames without discarding the OBJ prefetch/history.
    // The next use clears each image before its first partial row update.
    void InvalidateFrames() { initialized.fill(false); }
    // Optional nativeDestination (256x192) receives the separate guest frame in
    // the same submission as the display-scale image.
    void ReadFrame(uint32_t buffer, uint32_t screen, std::span<uint32_t> destination,
        std::span<uint32_t> nativeDestination = {});
    uint32_t Scale() const { return displayScale; }
    // Replace display-scale storage between batches. Guest OBJ history and
    // descriptors survive; display frames must be read back first.
    void SetScale(uint32_t scale);
private:
    // Leases for one submitted batch. The device's single pending submission
    // is this batch whenever active is set: every other Begin() completes it.
    struct InFlight {
        bool active = false;
        std::vector<MemoryCopy> copies;
        std::shared_ptr<Device::Buffer> scaledHistory, hires, inputPrefix;
        std::shared_ptr<Device::Image> native3D, display3D;
    };
    void Cleanup();
    void EnsureBuffer(std::shared_ptr<Device::Buffer>& buffer, size_t bytes,
        VkBufferUsageFlags usage, bool host);
    // GPU-read memory/records storage. An allocation failure moves to the next
    // InputTier() for this pipeline's lifetime. Mapped buffers are written in
    // place; unmapped device-local buffers are filled from upload.
    void PrepareInputs(size_t memoryBytes, size_t recordBytes);
    void ImageBarrier(VkCommandBuffer command, const Device::Image& image,
        VkImageLayout from, VkImageLayout to, VkPipelineStageFlags src,
        VkPipelineStageFlags dst, VkAccessFlags read, VkAccessFlags write);
    std::shared_ptr<Device> owner;
    VkDescriptorSetLayout bindings{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkPipeline mergePipeline{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    std::shared_ptr<Device::Buffer> memory, records, raw, history, landing, upload;
    // nativeFrames: four 256x192 guest frames, [buffer * 2 + screen].
    // scaledRaw: engine A display-scale composites for hires capture source A.
    std::shared_ptr<Device::Buffer> nativeFrames, scaledRaw;
    // Display-scale OBJ words for lines that reuse an earlier OBJ state
    // (MaxRecords + 2 slots, allocated on the first enhanced OBJ batch).
    std::shared_ptr<Device::Buffer> scaledHistory;
    std::array<std::shared_ptr<Device::Image>, 4> outputs;
    std::array<bool, 4> initialized{};
    std::shared_ptr<Device::Image> blank3D;
    bool historyInitialized = false, blankInitialized = false;
    InFlight inFlight;
    uint64_t uploadedEpoch = 0;
    size_t uploadedBytes = 0;
    uint32_t inputTier = 0;
    uint32_t rawWords = 0;
    uint32_t displayScale = 1;
};
}
