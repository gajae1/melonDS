// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef VOLK_NAMESPACE
#define VOLK_NAMESPACE
#endif
#include <volk.h>
#include <memory>
#include <string>

namespace melonDS::Vulkan {
// A core compute device, independent of Qt, windows and the presentation device.
// One rendering thread owns command recording/submission for each Device.
class Device final : public std::enable_shared_from_this<Device> {
public:
    class Buffer {
    public:
        ~Buffer();
        Buffer(const Buffer&) = delete;
        Buffer& operator=(const Buffer&) = delete;
        VkBuffer Handle() const { return buffer; }
        VkDeviceSize Size() const { return size; }
        void* Data() const { return mapped; }
    private:
        friend class Device;
        explicit Buffer(std::shared_ptr<Device> owner) : owner(std::move(owner)) {}
        std::shared_ptr<Device> owner;
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        VkDeviceSize size{};
        void* mapped{};
    };
    class Image {
    public:
        ~Image();
        Image(const Image&) = delete;
        Image& operator=(const Image&) = delete;
        VkImage Handle() const { return image; }
        VkImageView View() const { return view; }
        bool BelongsTo(const Device& device) const { return owner.get()==&device; }
    private:
        friend class Device;
        explicit Image(std::shared_ptr<Device> owner) : owner(std::move(owner)) {}
        std::shared_ptr<Device> owner;
        VkImage image{};
        VkImageView view{};
        VkDeviceMemory memory{};
    };
    static std::shared_ptr<Device> Create(std::string& error);
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    VkDevice Handle() const { return device; }
    const volk::VolkDeviceTable& Functions() const { return functions; }
    const VkPhysicalDeviceProperties& Properties() const { return properties; }
    std::shared_ptr<Buffer> CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible);
    std::shared_ptr<Image> CreateImage(uint32_t width,uint32_t height,uint32_t layers,
        VkFormat format,VkImageUsageFlags usage,bool arrayView=false);
    // Buffers may be reused after successful SubmitAndWait. A submission failure
    // retires this device; create a new device instead of resetting pending work.
    VkCommandBuffer Begin();
    void SubmitAndWait();
    static void Check(VkResult result, const char* operation);
private:
    Device() = default;
    void Init();
    uint32_t MemoryType(uint32_t bits,VkMemoryPropertyFlags required) const;
    VkInstance instance{};
    volk::VolkInstanceTable instanceFunctions{};
    VkPhysicalDevice physical{};
    VkPhysicalDeviceProperties properties{};
    VkPhysicalDeviceMemoryProperties memoryProperties{};
    VkDevice device{};
    volk::VolkDeviceTable functions{};
    VkQueue queue{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    bool recording=false;
    bool failed=false;
};
}
