// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Device.h"
#include "GPU2D_Soft.h"
#include <array>
#include <span>

namespace melonDS::Vulkan {
// CPU replay is used only when the optional final pass is unavailable. The
// unchanged SoftRenderer2D compositor remains the differential-test oracle.
void ComposeDisplayCPU(std::span<const SoftRenderer2D::ScaledLineContext> lines,
    std::span<const u32> pixels3D, u32 sourceScale, u32 scale, std::span<u32> destination);

class DisplayCompositor final {
public:
    DisplayCompositor(std::shared_ptr<Device> device, std::span<const u32> shader, u32 scale, u32 buffers = 1);
    ~DisplayCompositor();
    DisplayCompositor(const DisplayCompositor&) = delete;
    DisplayCompositor& operator=(const DisplayCompositor&) = delete;
    // Input is RGBA8 in GENERAL layout; output texels are packed CpuBGRA words.
    // Direct backing is renderer-owned, cached/coherent, and exactly matches
    // destination. Only composed row ranges are transferred; CPU capture/fill
    // and Keep rows survive. Null backing retains the vector/staging path.
    // Both paths complete synchronously before returning.
    void Compose(u32 screen, std::span<const SoftRenderer2D::ScaledLineContext> lines,
        const std::shared_ptr<Device::Image>& image3D, u32 sourceScale, std::span<u32> destination,
        const Device::Buffer* direct = nullptr);
    // Complete GPU image in GENERAL layout, with the same packed BGRA words as
    // Compose. CPU rows are read only for Keep/CaptureOverride; no GPU->host
    // transfer or CPU pixel conversion occurs. The returned image is borrowed
    // until the next composition of this screen. Copy/consume it before reuse.
    std::shared_ptr<Device::Image> ComposeResident(u32 screen,
        std::span<const SoftRenderer2D::ScaledLineContext> lines,
        const std::shared_ptr<Device::Image>& image3D, u32 sourceScale,
        std::span<u32> cpuRows, std::span<const bool> changedRows = {});
    // Explicit CPU demand (screenshot/fallback). Valid only after this screen's
    // latest successful ComposeResident and before it is overwritten.
    void ReadbackResident(u32 screen, std::span<u32> destination);
private:
    void ComposeImpl(u32 screen, std::span<const SoftRenderer2D::ScaledLineContext> lines,
        const std::shared_ptr<Device::Image>& image3D, u32 sourceScale, std::span<u32> destination,
        const Device::Buffer* direct, bool resident, std::span<const bool> changedRows = {});
    void Init(std::span<const u32> shader);
    void Cleanup();
    std::shared_ptr<Device> owner;
    const volk::VolkDeviceTable& f;
    VkDevice device;
    u32 scale;
    VkDescriptorSetLayout bindings{};
    VkDescriptorPool pool{};
    VkDescriptorSet descriptors{};
    VkPipelineLayout layout{};
    VkPipeline pipeline{};
    std::shared_ptr<Device::Buffer> contexts, readback;
    std::shared_ptr<Device::Buffer> overrides;
    std::vector<std::shared_ptr<Device::Image>> outputs;
    std::vector<bool> residentValid;
    std::shared_ptr<Device::Image> blank3D;
};
}
