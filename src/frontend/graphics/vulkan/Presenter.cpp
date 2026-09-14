// SPDX-License-Identifier: GPL-3.0-or-later
#include "Presenter.h"
#include <QtGui/qtguiglobal.h>
#if defined(Q_OS_WIN) && __has_include(<vulkan/vulkan.h>)
#define VK_USE_PLATFORM_WIN32_KHR
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <QVulkanInstance>
#include <QVulkanFunctions>
#endif
#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <vector>

namespace Vulkan {
#if defined(Q_OS_WIN) && __has_include(<vulkan/vulkan.h>) && defined(VK_EXT_swapchain_maintenance1) && QT_CONFIG(vulkan)
struct Presenter::Impl {
    QVulkanInstance instance;
    QVulkanFunctions* f = nullptr;
    QVulkanDeviceFunctions* d = nullptr;
    HWND window = nullptr;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t family = 0, current = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkExtent2D extent{};
    bool recreate = true;
    PFN_vkDestroySurfaceKHR destroySurface = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR surfaceSupport = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR capabilities = nullptr;
    PFN_vkGetPhysicalDeviceSurfaceFormatsKHR formats = nullptr;
    PFN_vkCreateSwapchainKHR createSwapchain = nullptr;
    PFN_vkDestroySwapchainKHR destroySwapchain = nullptr;
    PFN_vkGetSwapchainImagesKHR getImages = nullptr;
    PFN_vkAcquireNextImageKHR acquire = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
    struct Frame {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize capacity = 0;
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore available = VK_NULL_HANDLE;
    };
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkSemaphore finished = VK_NULL_HANDLE;
        VkFence presented = VK_NULL_HANDLE;
        bool pending = false;
    };
    std::array<Frame, 2> frames{};
    std::vector<Image> images;
    std::string error;

    bool Check(VkResult result, const char* operation) {
        if (result == VK_SUCCESS) return true;
        error = std::string(operation) + " (Vulkan " + std::to_string(int(result)) + ")";
        return false;
    }
    void FreeBuffer(Frame& frame) {
        if (frame.mapped) d->vkUnmapMemory(device, frame.memory);
        if (frame.buffer) d->vkDestroyBuffer(device, frame.buffer, nullptr);
        if (frame.memory) d->vkFreeMemory(device, frame.memory, nullptr);
        frame.mapped = nullptr; frame.buffer = VK_NULL_HANDLE;
        frame.memory = VK_NULL_HANDLE; frame.capacity = 0;
    }
    void FreeSwapchain() {
        for (auto& image : images) {
            if (image.finished) d->vkDestroySemaphore(device, image.finished, nullptr);
            if (image.presented) d->vkDestroyFence(device, image.presented, nullptr);
        }
        images.clear();
        if (swapchain) destroySwapchain(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
    ~Impl() {
        if (device && d) {
            const auto idle = d->vkDeviceWaitIdle(device);
            // Queue idle alone does not prove presentation has released its
            // resources. Maintenance1 supplies an explicit presentation fence.
            if (idle == VK_SUCCESS) for (auto& image : images)
                if (image.pending) d->vkWaitForFences(device, 1, &image.presented, VK_TRUE, UINT64_MAX);
            FreeSwapchain();
            for (auto& frame : frames) {
                FreeBuffer(frame);
                if (frame.fence) d->vkDestroyFence(device, frame.fence, nullptr);
                if (frame.available) d->vkDestroySemaphore(device, frame.available, nullptr);
            }
            if (pool) d->vkDestroyCommandPool(device, pool, nullptr);
            d->vkDestroyDevice(device, nullptr);
            instance.resetDeviceFunctions(device);
        }
        else if (device && f) {
            const auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(f->vkGetDeviceProcAddr(device,"vkDestroyDevice"));
            if (destroy) destroy(device,nullptr);
        }
        if (surface && destroySurface) destroySurface(instance.vkInstance(), surface, nullptr);
    }
    bool Init(void* handle) {
        window = static_cast<HWND>(handle);
        if (!window || !IsWindow(window)) { error = "No valid native window"; return false; }
        instance.setApiVersion(QVersionNumber(1,1));
        const QByteArrayList required = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
        for (const auto& name : required) if (!instance.supportedExtensions().contains(name)) {
            error = "Presentation extension unavailable: " + name.toStdString(); return false;
        }
        instance.setExtensions(required);
        if (!instance.create()) { error = "Vulkan instance creation failed"; return false; }
        f = instance.functions();
#define LOAD_INSTANCE(name, type) name = reinterpret_cast<type>(instance.getInstanceProcAddr(#type + 4))
        LOAD_INSTANCE(destroySurface, PFN_vkDestroySurfaceKHR);
        LOAD_INSTANCE(surfaceSupport, PFN_vkGetPhysicalDeviceSurfaceSupportKHR);
        LOAD_INSTANCE(capabilities, PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
        LOAD_INSTANCE(formats, PFN_vkGetPhysicalDeviceSurfaceFormatsKHR);
#undef LOAD_INSTANCE
        const auto createSurface = reinterpret_cast<PFN_vkCreateWin32SurfaceKHR>(instance.getInstanceProcAddr("vkCreateWin32SurfaceKHR"));
        const auto features2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(instance.getInstanceProcAddr("vkGetPhysicalDeviceFeatures2"));
        if (!f || !destroySurface || !surfaceSupport || !capabilities || !formats || !createSurface || !features2) {
            error = "Vulkan presentation functions unavailable"; return false;
        }
        VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.hinstance = GetModuleHandle(nullptr); surfaceInfo.hwnd = window;
        if (!Check(createSurface(instance.vkInstance(), &surfaceInfo, nullptr, &surface), "Create surface")) return false;
        uint32_t count = 0;
        if (!Check(f->vkEnumeratePhysicalDevices(instance.vkInstance(), &count, nullptr), "Enumerate GPUs")) return false;
        std::vector<VkPhysicalDevice> devices(count);
        if (!Check(f->vkEnumeratePhysicalDevices(instance.vkInstance(), &count, devices.data()), "Enumerate GPUs")) return false;
        for (const auto candidate : devices) {
            uint32_t n = 0;
            if (f->vkEnumerateDeviceExtensionProperties(candidate, nullptr, &n, nullptr) != VK_SUCCESS) continue;
            std::vector<VkExtensionProperties> extensions(n);
            if (f->vkEnumerateDeviceExtensionProperties(candidate, nullptr, &n, extensions.data()) != VK_SUCCESS) continue;
            const auto has = [&](const char* name) {
                return std::any_of(extensions.begin(), extensions.end(), [&](const auto& e) { return !std::strcmp(e.extensionName,name); });
            };
            if (!has(VK_KHR_SWAPCHAIN_EXTENSION_NAME) || !has(VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME)) continue;
            VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
            VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features.pNext = &maintenance; features2(candidate, &features);
            if (!maintenance.swapchainMaintenance1) continue;
            f->vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, nullptr);
            std::vector<VkQueueFamilyProperties> families(n);
            f->vkGetPhysicalDeviceQueueFamilyProperties(candidate, &n, families.data());
            for (uint32_t i = 0; i < n; ++i) {
                VkBool32 supported = false;
                if ((families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) &&
                    surfaceSupport(candidate,i,surface,&supported) == VK_SUCCESS && supported) {
                    physical = candidate; family = i; break;
                }
            }
            if (physical) break;
        }
        if (!physical) { error = "No GPU with safe swapchain presentation support"; return false; }
        const float priority = 1;
        VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex = family; queueInfo.queueCount = 1; queueInfo.pQueuePriorities = &priority;
        const char* extensions[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_SWAPCHAIN_MAINTENANCE_1_EXTENSION_NAME};
        VkPhysicalDeviceSwapchainMaintenance1FeaturesEXT maintenance{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SWAPCHAIN_MAINTENANCE_1_FEATURES_EXT};
        maintenance.swapchainMaintenance1 = VK_TRUE;
        VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        info.pNext = &maintenance; info.queueCreateInfoCount = 1; info.pQueueCreateInfos = &queueInfo;
        info.enabledExtensionCount = 2; info.ppEnabledExtensionNames = extensions;
        if (!Check(f->vkCreateDevice(physical,&info,nullptr,&device),"Create device")) return false;
        d = instance.deviceFunctions(device);
        if (!d) { error = "Vulkan device functions unavailable"; return false; }
#define LOAD_DEVICE(name, type) name = reinterpret_cast<type>(f->vkGetDeviceProcAddr(device, #type + 4))
        LOAD_DEVICE(createSwapchain, PFN_vkCreateSwapchainKHR);
        LOAD_DEVICE(destroySwapchain, PFN_vkDestroySwapchainKHR);
        LOAD_DEVICE(getImages, PFN_vkGetSwapchainImagesKHR);
        LOAD_DEVICE(acquire, PFN_vkAcquireNextImageKHR);
        LOAD_DEVICE(present, PFN_vkQueuePresentKHR);
#undef LOAD_DEVICE
        if (!createSwapchain || !destroySwapchain || !getImages || !acquire || !present) {
            error = "Vulkan swapchain functions unavailable"; return false;
        }
        d->vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        poolInfo.queueFamilyIndex = family; poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (!Check(d->vkCreateCommandPool(device,&poolInfo,nullptr,&pool),"Create command pool")) return false;
        for (auto& frame : frames) {
            VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            VkSemaphoreCreateInfo semaphoreInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            commandInfo.commandPool=pool; commandInfo.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY; commandInfo.commandBufferCount=1;
            if (!Check(d->vkCreateFence(device,&fenceInfo,nullptr,&frame.fence),"Create frame fence") ||
                !Check(d->vkCreateSemaphore(device,&semaphoreInfo,nullptr,&frame.available),"Create acquire semaphore") ||
                !Check(d->vkAllocateCommandBuffers(device,&commandInfo,&frame.command),"Allocate command buffer")) return false;
        }
        return true;
    }
    bool Allocate(Frame& frame, VkDeviceSize size) {
        if (frame.capacity >= size) return true;
        FreeBuffer(frame);
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size=size; info.usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT; info.sharingMode=VK_SHARING_MODE_EXCLUSIVE;
        if (!Check(d->vkCreateBuffer(device,&info,nullptr,&frame.buffer),"Create upload buffer")) return false;
        VkMemoryRequirements req{}; d->vkGetBufferMemoryRequirements(device,frame.buffer,&req);
        VkPhysicalDeviceMemoryProperties props{}; f->vkGetPhysicalDeviceMemoryProperties(physical,&props);
        uint32_t type=props.memoryTypeCount;
        for (uint32_t i=0;i<props.memoryTypeCount;++i)
            if ((req.memoryTypeBits & (1u<<i)) && (props.memoryTypes[i].propertyFlags &
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) { type=i; break; }
        if (type==props.memoryTypeCount) { error="No coherent upload memory"; return false; }
        VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        allocation.allocationSize=req.size; allocation.memoryTypeIndex=type;
        if (!Check(d->vkAllocateMemory(device,&allocation,nullptr,&frame.memory),"Allocate upload memory") ||
            !Check(d->vkBindBufferMemory(device,frame.buffer,frame.memory,0),"Bind upload memory") ||
            !Check(d->vkMapMemory(device,frame.memory,0,size,0,&frame.mapped),"Map upload memory")) return false;
        frame.capacity=size; return true;
    }
    Result Recreate(uint32_t width, uint32_t height) {
        for (auto& frame : frames) {
            const auto status=d->vkGetFenceStatus(device,frame.fence);
            if (status==VK_NOT_READY) return Result::Skipped;
            if (!Check(status,"Check frame fence")) return Result::Failed;
        }
        for (auto& image : images) if (image.pending) {
            const auto status=d->vkGetFenceStatus(device,image.presented);
            if (status==VK_NOT_READY) return Result::Skipped;
            if (!Check(status,"Check presentation fence")) return Result::Failed;
        }
        VkSurfaceCapabilitiesKHR caps{};
        if (!Check(capabilities(physical,surface,&caps),"Surface capabilities")) return Result::Failed;
        if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { error="Surface cannot accept image uploads"; return Result::Failed; }
        VkExtent2D size=caps.currentExtent;
        if (size.width==UINT32_MAX) size={std::clamp(width,caps.minImageExtent.width,caps.maxImageExtent.width),
                                         std::clamp(height,caps.minImageExtent.height,caps.maxImageExtent.height)};
        if (!size.width || !size.height || size.width!=width || size.height!=height) return Result::Skipped;
        uint32_t count=0;
        if (!Check(formats(physical,surface,&count,nullptr),"Surface formats")) return Result::Failed;
        std::vector<VkSurfaceFormatKHR> supported(count);
        if (!Check(formats(physical,surface,&count,supported.data()),"Surface formats")) return Result::Failed;
        const auto format=std::find_if(supported.begin(),supported.end(),[](const auto& value) {
            return value.format==VK_FORMAT_B8G8R8A8_UNORM && value.colorSpace==VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        });
        if (format==supported.end()) { error="Surface lacks BGRA8 output"; return Result::Failed; }
        VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
        info.surface=surface; info.minImageCount=caps.minImageCount+1;
        if (caps.maxImageCount) info.minImageCount=std::min(info.minImageCount,caps.maxImageCount);
        info.imageFormat=format->format; info.imageColorSpace=format->colorSpace; info.imageExtent=size;
        info.imageArrayLayers=1; info.imageUsage=VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        info.imageSharingMode=VK_SHARING_MODE_EXCLUSIVE; info.preTransform=caps.currentTransform;
        // Opaque client pixels; reject an unsupported alpha mode explicitly.
        if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) { error="Opaque presentation unavailable"; return Result::Failed; }
        info.compositeAlpha=VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR; info.presentMode=VK_PRESENT_MODE_FIFO_KHR;
        info.clipped=VK_TRUE;
        info.oldSwapchain=swapchain;
        VkSwapchainKHR replacement=VK_NULL_HANDLE;
        if (!Check(createSwapchain(device,&info,nullptr,&replacement),"Create swapchain")) return Result::Failed;
        FreeSwapchain();
        swapchain=replacement;
        if (!Check(getImages(device,swapchain,&count,nullptr),"Swapchain images")) return Result::Failed;
        std::vector<VkImage> handles(count);
        if (!Check(getImages(device,swapchain,&count,handles.data()),"Swapchain images")) return Result::Failed;
        images.resize(count);
        for (uint32_t i=0;i<count;++i) {
            images[i].image=handles[i];
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (!Check(d->vkCreateSemaphore(device,&semaphore,nullptr,&images[i].finished),"Create present semaphore") ||
                !Check(d->vkCreateFence(device,&fence,nullptr,&images[i].presented),"Create presentation fence")) return Result::Failed;
        }
        extent=size; recreate=false; return Result::Presented;
    }
    Result Draw(const void* pixels,uint32_t width,uint32_t height,uint32_t stride) {
        if (!width || !height || IsIconic(window)) return Result::Skipped;
        if (!pixels || width>UINT32_MAX/4 || stride<width*4 || uint64_t(stride)*height>SIZE_MAX) {
            error="Invalid presentation image dimensions"; return Result::Failed;
        }
        if (recreate || width!=extent.width || height!=extent.height) {
            const auto result=Recreate(width,height); if (result!=Result::Presented) return result;
        }
        auto& frame=frames[current];
        const auto ready=d->vkGetFenceStatus(device,frame.fence);
        if (ready==VK_NOT_READY) return Result::Skipped;
        if (!Check(ready,"Check upload fence") || !Allocate(frame,VkDeviceSize(width)*height*4)) return Result::Failed;
        // Poll presentation retirement before acquiring another image. This
        // first output stage skips a busy paint instead of blocking the UI.
        for (auto& image : images) if (image.pending) {
            const auto status=d->vkGetFenceStatus(device,image.presented);
            if (status==VK_NOT_READY) return Result::Skipped;
            if (!Check(status,"Retire presentation")) return Result::Failed;
            image.pending=false;
        }
        uint32_t index=0;
        const auto acquired=acquire(device,swapchain,0,frame.available,VK_NULL_HANDLE,&index);
        if (acquired==VK_NOT_READY || acquired==VK_TIMEOUT) return Result::Skipped;
        if (acquired==VK_ERROR_OUT_OF_DATE_KHR) { recreate=true; return Result::Skipped; }
        if (acquired!=VK_SUBOPTIMAL_KHR && !Check(acquired,"Acquire image")) return Result::Failed;
        recreate=acquired==VK_SUBOPTIMAL_KHR;
        auto& image=images[index];
        for (uint32_t y=0;y<height;++y)
            std::memcpy(static_cast<char*>(frame.mapped)+size_t(y)*width*4,static_cast<const char*>(pixels)+size_t(y)*stride,size_t(width)*4);
        if (!Check(d->vkResetCommandBuffer(frame.command,0),"Reset command buffer")) return Result::Failed;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!Check(d->vkBeginCommandBuffer(frame.command,&begin),"Begin upload")) return Result::Failed;
        VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        barrier.image=image.image; barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
        d->vkCmdPipelineBarrier(frame.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        VkBufferImageCopy copy{}; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; copy.imageExtent={width,height,1};
        d->vkCmdCopyBufferToImage(frame.command,frame.buffer,image.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
        barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=0;
        d->vkCmdPipelineBarrier(frame.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,0,0,nullptr,0,nullptr,1,&barrier);
        if (!Check(d->vkEndCommandBuffer(frame.command),"End upload") ||
            !Check(d->vkResetFences(device,1,&frame.fence),"Reset upload fence") ||
            !Check(d->vkResetFences(device,1,&image.presented),"Reset presentation fence")) return Result::Failed;
        const VkPipelineStageFlags waitStage=VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount=1; submit.pWaitSemaphores=&frame.available; submit.pWaitDstStageMask=&waitStage;
        submit.commandBufferCount=1; submit.pCommandBuffers=&frame.command;
        submit.signalSemaphoreCount=1; submit.pSignalSemaphores=&image.finished;
        if (!Check(d->vkQueueSubmit(queue,1,&submit,frame.fence),"Submit upload")) return Result::Failed;
        VkSwapchainPresentFenceInfoEXT fenceInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
        fenceInfo.swapchainCount=1; fenceInfo.pFences=&image.presented;
        VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        info.pNext=&fenceInfo; info.waitSemaphoreCount=1; info.pWaitSemaphores=&image.finished;
        info.swapchainCount=1; info.pSwapchains=&swapchain; info.pImageIndices=&index;
        const auto result=present(queue,&info);
        // OUT_OF_DATE and SURFACE_LOST still enqueue their semaphore waits.
        image.pending=result==VK_SUCCESS || result==VK_SUBOPTIMAL_KHR ||
            result==VK_ERROR_OUT_OF_DATE_KHR || result==VK_ERROR_SURFACE_LOST_KHR ||
            result==VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT;
        if (result==VK_ERROR_OUT_OF_DATE_KHR) { recreate=true; current=(current+1)%frames.size(); return Result::Skipped; }
        if (result!=VK_SUBOPTIMAL_KHR && !Check(result,"Present image")) return Result::Failed;
        recreate |= result==VK_SUBOPTIMAL_KHR;
        current=(current+1)%frames.size();
        return Result::Presented;
    }
};
#else
struct Presenter::Impl {};
#endif
Presenter::Presenter() : impl(std::make_unique<Impl>()) {}
Presenter::~Presenter() = default;
std::unique_ptr<Presenter> Presenter::Create(void* nativeWindow,std::string& error) {
    error.clear();
#if defined(Q_OS_WIN) && __has_include(<vulkan/vulkan.h>) && defined(VK_EXT_swapchain_maintenance1) && QT_CONFIG(vulkan)
    auto result=std::unique_ptr<Presenter>(new Presenter);
    if (!result->impl->Init(nativeWindow)) { error=result->impl->error; return nullptr; }
    return result;
#else
    error="Vulkan display is not built for this platform"; return nullptr;
#endif
}
Presenter::Result Presenter::Present(const void* pixels,uint32_t width,uint32_t height,uint32_t stride,std::string& error) {
#if defined(Q_OS_WIN) && __has_include(<vulkan/vulkan.h>) && defined(VK_EXT_swapchain_maintenance1) && QT_CONFIG(vulkan)
    const auto result=impl->Draw(pixels,width,height,stride); error=impl->error; return result;
#else
    error="Vulkan display is not built for this platform"; return Result::Failed;
#endif
}
}
