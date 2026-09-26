// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Device.h"
#include "Native2DRecord.h"
#include "Native2DCapture.h"
#include <array>
#include <span>

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
    void Render(std::span<const uint32_t> memory, std::span<const Record> lines,
        const std::shared_ptr<Device::Image>& native3D, uint32_t buffer,
        CapturePipeline* capture = nullptr, std::span<const CaptureCommand> captures = {},
        std::vector<MemoryCopy> copies = {}, std::span<const MemoryMerge> merges = {},
        const std::shared_ptr<Device::Image>& display3D = {});
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
private:
    void Cleanup();
    void EnsureBuffer(std::shared_ptr<Device::Buffer>& buffer, size_t bytes,
        VkBufferUsageFlags usage, bool host);
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
    std::shared_ptr<Device::Buffer> memory, records, raw, history, landing;
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
    uint32_t rawWords = 0;
    uint32_t displayScale = 1;
};
}
