// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#ifndef VOLK_NAMESPACE
#define VOLK_NAMESPACE
#endif
#include <volk.h>
#include <array>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace melonDS { class RenderCostVulkanMeter; }
namespace melonDS::Vulkan {
// A core compute device, independent of Qt. A frontend sharing its queue must
// serialize command recording/submission with the rendering thread.
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
        uint32_t Width() const { return width; }
        uint32_t Height() const { return height; }
        VkFormat Format() const { return format; }
        VkImageUsageFlags Usage() const { return usage; }
        bool BelongsTo(const Device& device) const { return owner.get()==&device; }
        // Win32 NT handle of a CreateExternalDisplayImage() image; nullptr for
        // ordinary images. Owned by this Image and closed with it: a GL importer
        // must retain its shared_ptr<Image> until the GL objects are deleted.
        void* ExternalHandle() const { return externalHandle; }
        VkDeviceSize AllocationSize() const { return allocationSize; }
    private:
        friend class Device;
        explicit Image(std::shared_ptr<Device> owner) : owner(std::move(owner)) {}
        std::shared_ptr<Device> owner;
        VkImage image{};
        VkImageView view{};
        VkDeviceMemory memory{};
        uint32_t width{}, height{};
        VkFormat format{};
        VkImageUsageFlags usage{};
        void* externalHandle{};
        VkDeviceSize allocationSize{};
    };
    struct Adapter { std::string id, name; VkPhysicalDeviceType type; };
    static std::vector<Adapter> Enumerate(std::string& error);
    static std::shared_ptr<Device> Create(std::string& error, const std::string& preferred = {}, bool requestPresentation = false);
    const std::string& Id() const { return id; }
    ~Device();
    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    VkDevice Handle() const { return device; }
    const volk::VolkDeviceTable& Functions() const { return functions; }
    const VkPhysicalDeviceProperties& Properties() const { return properties; }
    bool PresentationSupported() const { return presentation; }
    // True only for Device::Create(requestPresentation=true) on Win32 when the
    // chosen GPU exports/imports OPAQUE_WIN32 R32_UINT images and semaphores.
    bool ExternalImagesSupported() const { return externalImages; }
    // External presentation uses a separate command pool after core work has
    // completed. If its completion becomes unknown, retained images/session
    // keep this device alive; never submit again or idle-wait during fallback.
    void RetireExternalWork() noexcept { externalWorkFailed = true; failed = true; }
    bool ExternalWorkFailed() const { return externalWorkFailed; }
    bool HasSubmissionFailed() const { return failed; }
    const std::array<uint8_t,VK_UUID_SIZE>& DeviceUUID() const { return deviceUuid; }
    const std::array<uint8_t,VK_UUID_SIZE>& DriverUUID() const { return driverUuid; }
    VkInstance Instance() const { return instance; }
    VkPhysicalDevice PhysicalDevice() const { return physical; }
    uint32_t QueueFamily() const { return queueFamily; }
    // additionalRequired is strict, including on allocation retry. Display
    // backing requires HOST_CACHED rather than accepting an uncached fallback.
    std::shared_ptr<Buffer> CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool hostVisible,
        VkMemoryPropertyFlags preferred = 0, VkMemoryPropertyFlags additionalRequired = 0);
    std::shared_ptr<Image> CreateImage(uint32_t width,uint32_t height,uint32_t layers,
        VkFormat format,VkImageUsageFlags usage,bool arrayView=false);
    // Dedicated, exportable, optimal R32_UINT output image (usage 0x9f, mutable
    // format) for GL external-memory import. Throws if unsupported.
    std::shared_ptr<Image> CreateExternalDisplayImage(uint32_t width,uint32_t height);
    // Buffers may be reused after successful SubmitAndWait. A submission failure
    // retires this device; create a new device instead of resetting pending work.
    enum class SubmitKind { Other, Upload, ThreeD, FullReadback, Display };
    enum class TimestampStage { Upload, ThreeD, NativeReadback, FullReadback, DisplayCompose, DisplayReadback, Other };
    VkCommandBuffer Begin(SubmitKind kind = SubmitKind::Other);
    // Split form for work that may overlap host-side emulation: Submit() ends
    // recording and queues the commands; WaitForSubmission() must run before
    // the next Begin() and before reading or reusing referenced resources.
    void Submit();
    void WaitForSubmission();
    void SubmitAndWait();
    // An owner that leaves a submission pending past its own call registers how
    // to finish it. The next Begin() runs that first, so other users of this
    // device never observe the pending state. WaitForSubmission() clears it.
    void SetPendingCompletion(std::function<void()> complete) { pendingCompletion = std::move(complete); }
    // Optional observations in the existing command buffer/fence lifetime.
    // A null meter is the default OFF path: no clocks, pools or query commands.
    RenderCostVulkanMeter* Costs() const { return costs.get(); }
    void Timestamp(TimestampStage stage) noexcept;
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
    void Init(const std::string& preferred, std::vector<Adapter>* adapters = nullptr, bool requestPresentation = false);
    bool presentation = false;
    bool externalImages = false;
    bool externalWorkFailed = false;
    std::array<uint8_t,VK_UUID_SIZE> deviceUuid{}, driverUuid{};
    uint32_t queueFamily = 0;
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
    bool pending=false;
    std::function<void()> pendingCompletion;
    bool failed=false;
    uint64_t submissionCount=0;
    std::unique_ptr<RenderCostVulkanMeter> costs;
    SubmitKind submitKind = SubmitKind::Other;
    static constexpr uint32_t MaxTimestamps = 8;
    VkQueryPool timestampPool{};
    uint32_t timestampBits = 0, timestampCount = 0;
    bool timestampInitialized = false, timestampUsable = false;
    std::array<TimestampStage, MaxTimestamps> timestampStages{};
    void BeginCosts() noexcept;
    void CollectCosts() noexcept;
};
}
