// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Device.h"
#include <array>
#include <span>

namespace melonDS::Vulkan {
// Decodes flat guest VRAM on-device into the same packed RGB6A5 words used by
// the shared texture cache. The caller copies these words into cache images.
class TextureDecode {
public:
    // Kind selects what a job decodes. Kind::Texture decodes the texture
    // described by texParam/palBase; Kind::ClearColor and Kind::ClearDepth
    // ignore both and decode one 256x256 word plane of the captured clear
    // bitmap (colour words at 0x40000, depth words at 0x60000).
    enum class Kind : uint32_t { Texture, ClearColor, ClearDepth, HiresCapture };
    // HiresCapture batches bind word-major enhanced banks at captured; palBase
    // packs scale<<2 | bank. They output cropped RGBA8 UNORM words, not RGB6A5.
    struct Job { uint32_t texParam, palBase; Kind kind = Kind::Texture; };
    struct Slice { uint32_t word, width, height; };
    TextureDecode(std::shared_ptr<Device> device, std::span<const uint32_t> shader);
    ~TextureDecode();
    TextureDecode(const TextureDecode&) = delete;
    TextureDecode& operator=(const TextureDecode&) = delete;
    // All allocation/descriptor updates happen before command recording. Input
    // buffers contain 512KiB texture / 128KiB palette bytes and remain immutable
    // until the caller's fence. One preparation is live at a time. Supplying
    // captured bank data switches the texture layout to 524288 CPU baseline
    // bytes followed by 2048 bank masks, one per 256 logical bytes (532480
    // bytes total), and samples the four 128KiB physical banks in captured
    // (512KiB total) wherever a mask marks a logical byte as bank backed.
    // Clear jobs ignore texParam/palBase and produce one 256x256 slice each,
    // matching GPU3D_ComputeData.cpp DecodeClearBitmap word for word.
    std::span<const Slice> Prepare(std::span<const Job> jobs,
        const std::shared_ptr<Device::Buffer>& textures,
        const std::shared_ptr<Device::Buffer>& palettes,
        const std::shared_ptr<Device::Buffer>& captured = {});
    void Record(VkCommandBuffer command);
    // Call only after the command containing Record has completed successfully.
    void Complete();
    const std::shared_ptr<Device::Buffer>& Output() const { return output; }
private:
    void Cleanup();
    std::shared_ptr<Device> owner;
    VkDescriptorSetLayout bindings{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    VkDescriptorPool pool{};
    VkDescriptorSet set{};
    std::shared_ptr<Device::Buffer> output;
    std::array<std::shared_ptr<Device::Buffer>, 3> inputs;
    std::vector<Job> jobs;
    std::vector<Slice> slices;
    bool prepared = false, recorded = false, hasCaptured = false;
};
}
