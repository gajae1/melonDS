// SPDX-License-Identifier: GPL-3.0-or-later
#include "ExternalDisplay.h"
#include <stdexcept>

#if defined(_WIN32) && defined(VK_USE_PLATFORM_WIN32_KHR)
#define MELONDS_VK_EXTERNAL_WIN32 1
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace melonDS::Vulkan {
#ifdef MELONDS_VK_EXTERNAL_WIN32
namespace { constexpr uint64_t FenceTimeoutNs = 5'000'000'000ull; }

struct ExternalDisplay::State {
    enum class Phase { VulkanOwned, ExternalOwned, Quarantined };
    std::shared_ptr<Device> device;
    std::array<std::shared_ptr<Device::Image>,2> images;
    VkQueue queue{};
    VkCommandPool pool{};
    VkCommandBuffer command{};
    VkFence fence{};
    VkSemaphore ready{}, returned{};
    HANDLE readyHandle{}, returnedHandle{};
    Phase phase = Phase::VulkanOwned;
    bool healthy = true;
    // Preallocated (non-allocating to use) self reference: while it is set the State
    // and everything it owns cannot be destroyed. Only ~ExternalDisplay or a failed
    // constructor resets it, and only when nothing external is unresolved.
    std::shared_ptr<State> self;

    ~State() { Destroy(); }

    // Quarantine: never free, never wait. The unresolved work may involve a GL peer
    // that never signals, or a lost device; freeing objects still referenced by such
    // work is undefined, and an idle wait could block forever. Leaking is bounded.
    void Quarantine() noexcept {
        phase = Phase::Quarantined;
        healthy = false;
        device->RetireExternalWork();
    }

    void Destroy() noexcept {
        if (phase == Phase::Quarantined || !device) return;
        const auto& f = device->Functions();
        const VkDevice d = device->Handle();
        if (fence) f.vkDestroyFence(d, fence, nullptr);
        if (pool) f.vkDestroyCommandPool(d, pool, nullptr);
        if (ready) f.vkDestroySemaphore(d, ready, nullptr);
        if (returned) f.vkDestroySemaphore(d, returned, nullptr);
        if (readyHandle) CloseHandle(readyHandle);
        if (returnedHandle) CloseHandle(returnedHandle);
        fence = {}; pool = {}; command = {}; ready = returned = {};
        readyHandle = returnedHandle = nullptr;
        images = {};
        device.reset();
    }

    void CreateExported(VkSemaphore& semaphore, HANDLE& handle) {
        const auto& f = device->Functions();
        VkExportSemaphoreCreateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        exportInfo.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &exportInfo};
        Device::Check(f.vkCreateSemaphore(device->Handle(), &info, nullptr, &semaphore), "Create external semaphore");
        VkSemaphoreGetWin32HandleInfoKHR get{VK_STRUCTURE_TYPE_SEMAPHORE_GET_WIN32_HANDLE_INFO_KHR};
        get.semaphore = semaphore;
        get.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
        Device::Check(f.vkGetSemaphoreWin32HandleKHR(device->Handle(), &get, &handle), "Export external semaphore");
    }

    void Setup() {
        if (!device) throw std::invalid_argument("External display session needs a device");
        if (!device->ExternalImagesSupported()) throw std::runtime_error("Vulkan external display images are unsupported");
        if (device->ExternalWorkFailed()) throw std::runtime_error("Vulkan external display work already failed");
        const auto& f = device->Functions();
        const VkDevice d = device->Handle();
        f.vkGetDeviceQueue(d, device->QueueFamily(), 0, &queue);
        CreateExported(ready, readyHandle);
        CreateExported(returned, returnedHandle);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = device->QueueFamily();
        Device::Check(f.vkCreateCommandPool(d, &poolInfo, nullptr, &pool), "Create external display command pool");
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        commandInfo.commandPool = pool;
        commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        commandInfo.commandBufferCount = 1;
        Device::Check(f.vkAllocateCommandBuffers(d, &commandInfo, &command), "Allocate external display command buffer");
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        Device::Check(f.vkCreateFence(d, &fenceInfo, nullptr, &fence), "Create external display fence");
    }

    // Records one ownership-transfer barrier per image, then submits and waits with
    // the private fence. submitted is set as soon as the queue may own the work.
    void Run(bool release, bool& submitted) {
        const auto& f = device->Functions();
        const uint32_t family = device->QueueFamily();
        Device::Check(f.vkResetCommandBuffer(command, 0), "Reset external display commands");
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        Device::Check(f.vkBeginCommandBuffer(command, &begin), "Begin external display commands");
        std::array<VkImageMemoryBarrier,2> barriers{};
        for (size_t i = 0; i < barriers.size(); ++i) {
            auto& b = barriers[i];
            b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.image = images[i]->Handle();
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            if (release) {
                b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT;
                b.srcQueueFamilyIndex = family;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
            } else {
                b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                    VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
                b.dstQueueFamilyIndex = family;
            }
        }
        f.vkCmdPipelineBarrier(command,
            release ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            release ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0, 0, nullptr, 0, nullptr, static_cast<uint32_t>(barriers.size()), barriers.data());
        Device::Check(f.vkEndCommandBuffer(command), "End external display commands");
        const VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        if (!release) {
            submit.waitSemaphoreCount = 1;
            submit.pWaitSemaphores = &returned;
            submit.pWaitDstStageMask = &waitStage;
        }
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command;
        if (release) {
            submit.signalSemaphoreCount = 1;
            submit.pSignalSemaphores = &ready;
        }
        // From here the outcome may be unknown even if the call reports an error.
        submitted = true;
        Device::Check(f.vkQueueSubmit(queue, 1, &submit, fence), "Submit external display handoff");
        Device::Check(f.vkWaitForFences(device->Handle(), 1, &fence, VK_TRUE, FenceTimeoutNs), "Wait for external display handoff");
        Device::Check(f.vkResetFences(device->Handle(), 1, &fence), "Reset external display fence");
    }
};

ExternalDisplay::ExternalDisplay(std::shared_ptr<Device> device)
{
    state = std::make_shared<State>();
    state->device = std::move(device);
    state->self = state;
    try { state->Setup(); }
    catch (...) {
        // Nothing was submitted: free everything and break the cycle.
        state->Destroy();
        state->self.reset();
        throw;
    }
}

ExternalDisplay::~ExternalDisplay()
{
    if (!state) return;
    // A live handoff cannot be waited for blindly (GL may never signal): quarantine.
    if (state->phase == State::Phase::ExternalOwned) state->Quarantine();
    if (state->phase == State::Phase::Quarantined) return;  // self keeps State alive
    auto owned = std::move(state);
    owned->self.reset();  // VulkanOwned: all work fence-confirmed, safe to free
}

void* ExternalDisplay::ReadyHandle() const { return state ? state->readyHandle : nullptr; }
void* ExternalDisplay::ReturnedHandle() const { return state ? state->returnedHandle : nullptr; }
bool ExternalDisplay::Healthy() const { return state && state->healthy && state->phase != State::Phase::Quarantined; }

void ExternalDisplay::Abandon() noexcept
{
    if (!state) return;
    if (state->phase == State::Phase::VulkanOwned) state->healthy = false;  // nothing unresolved
    else state->Quarantine();
}

void ExternalDisplay::Release(const std::array<std::shared_ptr<Device::Image>,2>& images)
{
    if (!Healthy() || state->device->ExternalWorkFailed()) throw std::runtime_error("External display session is unusable");
    auto& s = *state;
    if (s.phase != State::Phase::VulkanOwned) throw std::logic_error("External display Release before Acquire");
    if (!images[0] || !images[1] || images[0] == images[1] || images[0]->Handle() == images[1]->Handle())
        throw std::invalid_argument("External display needs two distinct images");
    for (const auto& image : images) {
        if (!image->Handle() || !image->Width() || !image->Height() || !image->BelongsTo(*s.device) || !image->ExternalHandle())
            throw std::invalid_argument("External display image is empty, foreign or not external");
    }
    s.images = images;  // retained until Acquire completes
    bool submitted = false;
    try {
        s.Run(true, submitted);
        s.phase = State::Phase::ExternalOwned;
    } catch (...) {
        if (submitted) s.Quarantine();
        else { s.images = {}; s.healthy = false; }
        throw;
    }
}

void ExternalDisplay::Acquire()
{
    if (!Healthy() || state->device->ExternalWorkFailed()) throw std::runtime_error("External display session is unusable");
    auto& s = *state;
    if (s.phase != State::Phase::ExternalOwned) throw std::logic_error("External display Acquire without Release");
    bool submitted = false;
    try {
        s.Run(false, submitted);
        s.phase = State::Phase::VulkanOwned;
        s.images = {};
    } catch (...) {
        // Ownership and the Returned wait are already ambiguous; even a
        // pre-submit failure cannot restore them.
        s.Quarantine();
        throw;
    }
}
#else
struct ExternalDisplay::State {};
ExternalDisplay::ExternalDisplay(std::shared_ptr<Device>) { throw std::runtime_error("Vulkan external display requires Win32"); }
ExternalDisplay::~ExternalDisplay() = default;
void* ExternalDisplay::ReadyHandle() const { return nullptr; }
void* ExternalDisplay::ReturnedHandle() const { return nullptr; }
bool ExternalDisplay::Healthy() const { return false; }
void ExternalDisplay::Abandon() noexcept {}
void ExternalDisplay::Release(const std::array<std::shared_ptr<Device::Image>,2>&) { throw std::runtime_error("Vulkan external display requires Win32"); }
void ExternalDisplay::Acquire() { throw std::runtime_error("Vulkan external display requires Win32"); }
#endif
}
