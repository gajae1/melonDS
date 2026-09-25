// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <memory>
#include <string>
#ifdef VULKANRENDERER_ENABLED
#include "Vulkan/Device.h"
#include <span>
#endif

namespace Vulkan {
// Owns submission storage. Input pixels need only survive Present().
// First WSI implementation is Windows; unsupported hosts retain native output.
class Presenter {
public:
    Presenter();
    enum class Result { Presented, Skipped, Failed };
    struct Diagnostics {
        bool Enabled = false;
        uint64_t Calls = 0, Submits = 0, Skips = 0, Failures = 0;
        uint64_t StagingBytes = 0, TransferBytes = 0;
        uint64_t UploadNs = 0, SubmitNs = 0, QueuePresentNs = 0;
    };
    // Host observations only; neither API return nor these counters proves
    // display completion. A skipped return may still follow a real submit.
    Diagnostics GetDiagnostics() const;
    static std::unique_ptr<Presenter> Create(void* nativeWindow, std::string& error, const std::string& preferredId = {});
    ~Presenter();
    Result Present(const void* bgra, uint32_t width, uint32_t height, uint32_t stride, std::string& error);
#ifdef VULKANRENDERER_ENABLED
    struct Texture {
        const void* pixels = nullptr;
        uint32_t width = 0, height = 0, stride = 0;
        std::shared_ptr<melonDS::Vulkan::Device::Image> resident;
    };
    struct Quad {
        uint32_t texture = 0;
        // Unit texture coordinates to physical window pixels, in Qt matrix order.
        std::array<float, 6> transform{};
        bool filter = false;
    };
    static std::unique_ptr<Presenter> CreateShared(void* nativeWindow,
        std::shared_ptr<melonDS::Vulkan::Device> device, std::string& error);
    bool UsesDevice(const std::shared_ptr<melonDS::Vulkan::Device>& device) const;
    // The caller serializes shared-device submission with renderer access.
    Result Present(uint32_t width, uint32_t height, std::span<const Texture> textures,
        std::span<const Quad> quads, std::string& error);
#endif
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
