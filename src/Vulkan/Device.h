// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef VOLK_NAMESPACE
#define VOLK_NAMESPACE
#endif
#include <volk.h>
#include <memory>
#include <string>
#include <vector>

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
        VkMemoryPropertyFlags MemoryProperties() const { return properties; }
        VkBufferUsageFlags Usage() const { return usage; }
        bool BelongsTo(const Device& device) const { return owner.get() == &device; }
    private:
        friend class Device;
        explicit Buffer(std::shared_ptr<Device> owner) : owner(std::move(owner)) {}
        std::shared_ptr<Device> owner;
        VkBuffer buffer{};
        VkDeviceMemory memory{};
        VkDeviceSize size{};
        VkMemoryPropertyFlags properties{};
        VkBufferUsageFlags usage{};
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
    struct Adapter { std::string id, name; VkPhysicalDeviceType type; };
    static std::vector<Adapter> Enumerate(std::string& error);
    static std::shared_ptr<Device> Create(std::string& error, const std::string& preferred = {});
    const std::string& Id() const { return id; }
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    VkDevice Handle() const { return device; }
    const volk::VolkDeviceTable& Functions() const { return functions; }
    const VkPhysicalDeviceProperties& Properties() const { return properties; }
    // additionalRequired is strict, including on allocation retry. Display
    // backing requires HOST_CACHED rather than accepting an uncached fallback.
    std::shared_ptr<Buffer> CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
        VkMemoryPropertyFlags preferred = 0, VkMemoryPropertyFlags additionalRequired = 0);
    std::shared_ptr<Image> CreateImage(uint32_t width,uint32_t height,uint32_t layers,
        VkFormat format,VkImageUsageFlags usage,bool arrayView=false);
    // Buffers may be reused after successful SubmitAndWait. A submission failure
    // retires this device; create a new device instead of resetting pending work.
    VkCommandBuffer Begin();
    void SubmitAndWait();
    // Rendering-thread diagnostic: successful queue submissions, including uploads.
    uint64_t SubmissionCount() const { return submissionCount; }
    static void Check(VkResult result, const char* operation);
    // Device-owned, transient compiler cache. Optional OOM keeps uncached rendering.
    // Like command recording, initialization belongs to the rendering thread.
    VkPipelineCache GetPipelineCache();
    void ClearPipelineCache();
    void TrimPipelineCache();
private:
    Device() = default;
    void Init(const std::string& preferred, std::vector<Adapter>* adapters = nullptr);
    std::string id;
    uint32_t MemoryType(uint32_t bits,VkMemoryPropertyFlags required,
        VkMemoryPropertyFlags preferred = 0) const;
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
    VkPipelineCache pipelineCache{};
    bool pipelineCacheInitialized=false;
    bool recording=false;
    bool failed=false;
    uint64_t submissionCount=0;
};
}
