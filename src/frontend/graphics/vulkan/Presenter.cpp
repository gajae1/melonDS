// SPDX-License-Identifier: GPL-3.0-or-later
#include "Presenter.h"
#include "RenderCost.h"
#include "Vulkan/AdapterSelection.h"
#ifdef VULKANRENDERER_ENABLED
#include "Vulkan/EmbeddedShaders.h"
#endif
#include <QtGui/qtguiglobal.h>
#if defined(Q_OS_WIN) && __has_include(<vulkan/vulkan.h>)
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
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
#ifdef VULKANRENDERER_ENABLED
    std::shared_ptr<melonDS::Vulkan::Device> sharedDevice;
#endif
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
#ifdef VULKANRENDERER_ENABLED
    struct TextureStorage {
        VkImage image{};
        VkImageView view{};
        VkDeviceMemory memory{};
        uint32_t width{}, height{};
    };
    VkRenderPass renderPass{};
    VkDescriptorSetLayout bindings{};
    VkPipelineLayout pipelineLayout{};
    VkPipeline pipeline{};
    VkSampler sampler{};
#endif
    struct Frame {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mapped = nullptr;
        VkDeviceSize capacity = 0;
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VkSemaphore available = VK_NULL_HANDLE;
#ifdef VULKANRENDERER_ENABLED
        VkDescriptorPool descriptors{};
        std::vector<TextureStorage> textures;
        std::vector<std::shared_ptr<melonDS::Vulkan::Device::Image>> residents;
#endif
    };
    struct Image {
        VkImage image = VK_NULL_HANDLE;
        VkSemaphore finished = VK_NULL_HANDLE;
        VkFence presented = VK_NULL_HANDLE;
        bool pending = false;
#ifdef VULKANRENDERER_ENABLED
        VkImageView view{};
        VkFramebuffer framebuffer{};
#endif
    };
    std::array<Frame, 2> frames{};
    std::vector<Image> images;
    std::string error;
    Diagnostics diagnostics;

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
#ifdef VULKANRENDERER_ENABLED
            if (image.framebuffer) d->vkDestroyFramebuffer(device, image.framebuffer, nullptr);
            if (image.view) d->vkDestroyImageView(device, image.view, nullptr);
#endif
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
#ifdef VULKANRENDERER_ENABLED
                for (auto& texture : frame.textures) FreeTexture(texture);
                frame.residents.clear();
                if (frame.descriptors) d->vkDestroyDescriptorPool(device, frame.descriptors, nullptr);
#endif
                if (frame.fence) d->vkDestroyFence(device, frame.fence, nullptr);
                if (frame.available) d->vkDestroySemaphore(device, frame.available, nullptr);
            }
            if (pool) d->vkDestroyCommandPool(device, pool, nullptr);
#ifdef VULKANRENDERER_ENABLED
            if (pipeline) d->vkDestroyPipeline(device, pipeline, nullptr);
            if (pipelineLayout) d->vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            if (bindings) d->vkDestroyDescriptorSetLayout(device, bindings, nullptr);
            if (sampler) d->vkDestroySampler(device, sampler, nullptr);
            if (renderPass) d->vkDestroyRenderPass(device, renderPass, nullptr);
            if (!sharedDevice)
#endif
                d->vkDestroyDevice(device, nullptr);
            instance.resetDeviceFunctions(device);
        }
        else if (device && f
#ifdef VULKANRENDERER_ENABLED
            && !sharedDevice
#endif
        ) {
            const auto destroy = reinterpret_cast<PFN_vkDestroyDevice>(f->vkGetDeviceProcAddr(device,"vkDestroyDevice"));
            if (destroy) destroy(device,nullptr);
        }
        if (surface && destroySurface) destroySurface(instance.vkInstance(), surface, nullptr);
    }
    bool Init(void* handle, const std::string& preferredId = {}) {
        diagnostics.Enabled = melonDS::RenderCostEnabled();
        window = static_cast<HWND>(handle);
        if (!window || !IsWindow(window)) { error = "No valid native window"; return false; }
        instance.setApiVersion(QVersionNumber(1,1));
        const QByteArrayList required = {VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME,
            VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME, VK_EXT_SURFACE_MAINTENANCE_1_EXTENSION_NAME};
        for (const auto& name : required) if (!instance.supportedExtensions().contains(name)) {
            error = "Presentation extension unavailable: " + name.toStdString(); return false;
        }
        instance.setExtensions(required);
#ifdef VULKANRENDERER_ENABLED
        if (sharedDevice) {
            if (!sharedDevice->PresentationSupported()) { error = "Renderer device lacks safe presentation support"; return false; }
            instance.setFlags(QVulkanInstance::NoDebugOutputRedirect);
            instance.setVkInstance(sharedDevice->Instance());
        }
#endif
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
        const auto properties2 = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(instance.getInstanceProcAddr("vkGetPhysicalDeviceProperties2"));
        if (!f || !destroySurface || !surfaceSupport || !capabilities || !formats || !createSurface || !features2 || !properties2) {
            error = "Vulkan presentation functions unavailable"; return false;
        }
        VkWin32SurfaceCreateInfoKHR surfaceInfo{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
        surfaceInfo.hinstance = GetModuleHandle(nullptr); surfaceInfo.hwnd = window;
        if (!Check(createSurface(instance.vkInstance(), &surfaceInfo, nullptr, &surface), "Create surface")) return false;
#ifdef VULKANRENDERER_ENABLED
        if (sharedDevice) {
            physical = sharedDevice->PhysicalDevice();
            family = sharedDevice->QueueFamily();
            VkBool32 supported = false;
            if (!Check(surfaceSupport(physical, family, surface, &supported), "Renderer presentation support") || !supported) {
                error = "Renderer queue cannot present to this window"; return false;
            }
            device = sharedDevice->Handle();
        } else
#endif
        {
        uint32_t count = 0;
        if (!Check(f->vkEnumeratePhysicalDevices(instance.vkInstance(), &count, nullptr), "Enumerate GPUs")) return false;
        std::vector<VkPhysicalDevice> devices(count);
        if (!Check(f->vkEnumeratePhysicalDevices(instance.vkInstance(), &count, devices.data()), "Enumerate GPUs")) return false;
        int bestRank = std::numeric_limits<int>::min();
        for (const auto candidate : devices) {
            VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            properties.pNext = &id; properties2(candidate, &properties);
            if (!preferredId.empty() && melonDS::Vulkan::AdapterId(id.deviceUUID) != preferredId) continue;
            const int rank = melonDS::Vulkan::AdapterRank(int(properties.properties.deviceType));
            if (physical && rank <= bestRank) continue;
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
                    physical = candidate; family = i; bestRank = rank; break;
                }
            }
            if (physical && !preferredId.empty()) break;
        }
        if (!physical) {
            error = preferredId.empty() ? "No GPU with safe swapchain presentation support"
                : "Selected GPU is unavailable or lacks safe swapchain presentation support";
            return false;
        }
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
        }
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
#ifdef VULKANRENDERER_ENABLED
        if (renderPass) {
            if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) { error="Surface cannot render textured screens"; return Result::Failed; }
            info.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
#endif
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
#ifdef VULKANRENDERER_ENABLED
            if (renderPass && !CreateTarget(images[i], size)) return Result::Failed;
#endif
            VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
            VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            if (!Check(d->vkCreateSemaphore(device,&semaphore,nullptr,&images[i].finished),"Create present semaphore") ||
                !Check(d->vkCreateFence(device,&fence,nullptr,&images[i].presented),"Create presentation fence")) return Result::Failed;
        }
        extent=size; recreate=false; return Result::Presented;
    }
#ifdef VULKANRENDERER_ENABLED
    void FreeTexture(TextureStorage& texture) {
        if (texture.view) d->vkDestroyImageView(device, texture.view, nullptr);
        if (texture.image) d->vkDestroyImage(device, texture.image, nullptr);
        if (texture.memory) d->vkFreeMemory(device, texture.memory, nullptr);
        texture = {};
    }
    bool CreateTarget(Image& image, VkExtent2D size) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image=image.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=VK_FORMAT_B8G8R8A8_UNORM;
        view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if (!Check(d->vkCreateImageView(device,&view,nullptr,&image.view),"Create presentation view")) return false;
        VkFramebufferCreateInfo target{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        target.renderPass=renderPass; target.attachmentCount=1; target.pAttachments=&image.view;
        target.width=size.width; target.height=size.height; target.layers=1;
        return Check(d->vkCreateFramebuffer(device,&target,nullptr,&image.framebuffer),"Create presentation target");
    }
    bool InitQuads() {
        if (renderPass) return true;
        VkAttachmentDescription attachment{};
        attachment.format=VK_FORMAT_B8G8R8A8_UNORM; attachment.samples=VK_SAMPLE_COUNT_1_BIT;
        attachment.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; attachment.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
        attachment.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE; attachment.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED; attachment.finalLayout=VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        VkAttachmentReference color{0,VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS; subpass.colorAttachmentCount=1; subpass.pColorAttachments=&color;
        VkSubpassDependency dependency{};
        dependency.srcSubpass=VK_SUBPASS_EXTERNAL; dependency.dstSubpass=0;
        dependency.srcStageMask=dependency.dstStageMask=VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependency.dstAccessMask=VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        pass.attachmentCount=1; pass.pAttachments=&attachment; pass.subpassCount=1; pass.pSubpasses=&subpass;
        pass.dependencyCount=1; pass.pDependencies=&dependency;
        if (!Check(d->vkCreateRenderPass(device,&pass,nullptr,&renderPass),"Create presentation pass")) return false;
        VkDescriptorSetLayoutBinding binding{0,VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,1,VK_SHADER_STAGE_FRAGMENT_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo layout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        layout.bindingCount=1; layout.pBindings=&binding;
        if (!Check(d->vkCreateDescriptorSetLayout(device,&layout,nullptr,&bindings),"Create presentation bindings")) return false;
        VkPushConstantRange constants{VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,80};
        VkPipelineLayoutCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pipelineInfo.setLayoutCount=1; pipelineInfo.pSetLayouts=&bindings;
        pipelineInfo.pushConstantRangeCount=1; pipelineInfo.pPushConstantRanges=&constants;
        if (!Check(d->vkCreatePipelineLayout(device,&pipelineInfo,nullptr,&pipelineLayout),"Create presentation layout")) return false;
        VkSamplerCreateInfo sample{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sample.magFilter=sample.minFilter=VK_FILTER_NEAREST;
        sample.addressModeU=sample.addressModeV=sample.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (!Check(d->vkCreateSampler(device,&sample,nullptr,&sampler),"Create presentation sampler")) return false;
        const std::array shaders{melonDS::Vulkan::EmbeddedPresent_vert(),melonDS::Vulkan::EmbeddedPresent_frag()};
        std::array<VkShaderModule,2> modules{};
        std::array<VkPipelineShaderStageCreateInfo,2> stages{};
        bool valid=true;
        for (size_t i=0;i<shaders.size();++i) {
            VkShaderModuleCreateInfo module{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            module.codeSize=shaders[i].size_bytes(); module.pCode=shaders[i].data();
            if (!Check(d->vkCreateShaderModule(device,&module,nullptr,&modules[i]),"Create presentation shader")) { valid=false; break; }
            stages[i]={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
            stages[i].stage=i ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT;
            stages[i].module=modules[i]; stages[i].pName="main";
        }
        VkPipelineVertexInputStateCreateInfo vertices{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        assembly.topology=VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        viewport.viewportCount=viewport.scissorCount=1;
        VkPipelineRasterizationStateCreateInfo raster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        raster.polygonMode=VK_POLYGON_MODE_FILL; raster.lineWidth=1;
        VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        multisample.rasterizationSamples=VK_SAMPLE_COUNT_1_BIT;
        VkPipelineColorBlendAttachmentState blend{};
        blend.blendEnable=VK_TRUE; blend.srcColorBlendFactor=blend.srcAlphaBlendFactor=VK_BLEND_FACTOR_ONE;
        blend.dstColorBlendFactor=blend.dstAlphaBlendFactor=VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend.colorBlendOp=blend.alphaBlendOp=VK_BLEND_OP_ADD; blend.colorWriteMask=15;
        VkPipelineColorBlendStateCreateInfo blending{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blending.attachmentCount=1; blending.pAttachments=&blend;
        const VkDynamicState dynamicStates[]={VK_DYNAMIC_STATE_VIEWPORT,VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount=2; dynamic.pDynamicStates=dynamicStates;
        VkGraphicsPipelineCreateInfo graphics{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        graphics.stageCount=2; graphics.pStages=stages.data(); graphics.pVertexInputState=&vertices;
        graphics.pInputAssemblyState=&assembly; graphics.pViewportState=&viewport; graphics.pRasterizationState=&raster;
        graphics.pMultisampleState=&multisample; graphics.pColorBlendState=&blending; graphics.pDynamicState=&dynamic;
        graphics.layout=pipelineLayout; graphics.renderPass=renderPass;
        if (valid) valid=Check(d->vkCreateGraphicsPipelines(device,VK_NULL_HANDLE,1,&graphics,nullptr,&pipeline),"Create presentation pipeline");
        for (auto module : modules) if (module) d->vkDestroyShaderModule(device,module,nullptr);
        recreate=true;
        return valid;
    }
    bool AllocateTexture(TextureStorage& texture, uint32_t width, uint32_t height) {
        if (texture.width==width && texture.height==height) return true;
        FreeTexture(texture);
        VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        image.imageType=VK_IMAGE_TYPE_2D; image.format=VK_FORMAT_R32_UINT; image.extent={width,height,1};
        image.mipLevels=image.arrayLayers=1; image.samples=VK_SAMPLE_COUNT_1_BIT;
        image.tiling=VK_IMAGE_TILING_OPTIMAL; image.usage=VK_IMAGE_USAGE_TRANSFER_DST_BIT|VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!Check(d->vkCreateImage(device,&image,nullptr,&texture.image),"Create screen texture")) return false;
        VkMemoryRequirements req{}; d->vkGetImageMemoryRequirements(device,texture.image,&req);
        VkPhysicalDeviceMemoryProperties properties{}; f->vkGetPhysicalDeviceMemoryProperties(physical,&properties);
        uint32_t type=properties.memoryTypeCount;
        for (uint32_t i=0;i<properties.memoryTypeCount;++i)
            if ((req.memoryTypeBits&(1u<<i)) && (properties.memoryTypes[i].propertyFlags&VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { type=i; break; }
        if (type==properties.memoryTypeCount) { error="No local texture memory"; return false; }
        VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        memory.allocationSize=req.size; memory.memoryTypeIndex=type;
        if (!Check(d->vkAllocateMemory(device,&memory,nullptr,&texture.memory),"Allocate screen texture") ||
            !Check(d->vkBindImageMemory(device,texture.image,texture.memory,0),"Bind screen texture")) return false;
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image=texture.image; view.viewType=VK_IMAGE_VIEW_TYPE_2D; view.format=VK_FORMAT_R32_UINT;
        view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        if (!Check(d->vkCreateImageView(device,&view,nullptr,&texture.view),"Create screen texture view")) return false;
        texture.width=width; texture.height=height;
        return true;
    }
    Result DrawQuads(uint32_t width,uint32_t height,std::span<const Texture> textures,std::span<const Quad> quads) {
        if (!width || !height || IsIconic(window)) return Result::Skipped;
        uint64_t bytes=0;
        for (const auto& texture : textures) {
            if (texture.resident) {
                if (!sharedDevice || !texture.resident->BelongsTo(*sharedDevice) || texture.resident->Format()!=VK_FORMAT_R32_UINT ||
                    !(texture.resident->Usage()&VK_IMAGE_USAGE_SAMPLED_BIT)) { error="Incompatible resident screen"; return Result::Failed; }
            } else {
                if (!texture.pixels || !texture.width || !texture.height || texture.width>UINT32_MAX/4 || texture.stride<texture.width*4 ||
                    uint64_t(texture.stride)*texture.height>SIZE_MAX) { error="Invalid screen texture"; return Result::Failed; }
                bytes+=uint64_t(texture.width)*texture.height*4;
            }
        }
        for (const auto& quad : quads) if (quad.texture>=textures.size()) { error="Invalid screen index"; return Result::Failed; }
        if (!InitQuads()) return Result::Failed;
        if (recreate || width!=extent.width || height!=extent.height) {
            const auto result=Recreate(width,height); if (result!=Result::Presented) return result;
        }
        auto& frame=frames[current];
        auto ready=d->vkGetFenceStatus(device,frame.fence);
        if (ready==VK_NOT_READY) return Result::Skipped;
        if (!Check(ready,"Check screen fence") || (bytes && !Allocate(frame,bytes))) return Result::Failed;
        for (auto& image : images) if (image.pending) {
            ready=d->vkGetFenceStatus(device,image.presented);
            if (ready==VK_NOT_READY) return Result::Skipped;
            if (!Check(ready,"Retire presentation")) return Result::Failed;
            image.pending=false;
        }
        if (frame.textures.size()<textures.size()) {
            if (frame.descriptors) d->vkDestroyDescriptorPool(device,frame.descriptors,nullptr);
            frame.descriptors=VK_NULL_HANDLE;
            frame.textures.resize(textures.size());
            VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,uint32_t(textures.size())};
            VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            poolInfo.maxSets=size.descriptorCount; poolInfo.poolSizeCount=1; poolInfo.pPoolSizes=&size;
            if (!Check(d->vkCreateDescriptorPool(device,&poolInfo,nullptr,&frame.descriptors),"Create screen descriptors")) return Result::Failed;
        }
        frame.residents.clear();
        std::vector<VkDescriptorSet> sets(textures.size());
        if (!textures.empty()) {
            if (!Check(d->vkResetDescriptorPool(device,frame.descriptors,0),"Reset screen descriptors")) return Result::Failed;
            std::vector<VkDescriptorSetLayout> layouts(textures.size(),bindings);
            VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            allocation.descriptorPool=frame.descriptors; allocation.descriptorSetCount=uint32_t(sets.size()); allocation.pSetLayouts=layouts.data();
            if (!Check(d->vkAllocateDescriptorSets(device,&allocation,sets.data()),"Allocate screen descriptors")) return Result::Failed;
            for (size_t i=0;i<textures.size();++i)
                if (!textures[i].resident && !AllocateTexture(frame.textures[i],textures[i].width,textures[i].height)) return Result::Failed;
        }
        uint32_t index=0;
        const auto acquired=acquire(device,swapchain,0,frame.available,VK_NULL_HANDLE,&index);
        if (acquired==VK_NOT_READY || acquired==VK_TIMEOUT) return Result::Skipped;
        if (acquired==VK_ERROR_OUT_OF_DATE_KHR) { recreate=true; return Result::Skipped; }
        if (acquired!=VK_SUBOPTIMAL_KHR && !Check(acquired,"Acquire image")) return Result::Failed;
        recreate=acquired==VK_SUBOPTIMAL_KHR;
        if (!Check(d->vkResetCommandBuffer(frame.command,0),"Reset screen commands")) return Result::Failed;
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO}; begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (!Check(d->vkBeginCommandBuffer(frame.command,&begin),"Begin screen commands")) return Result::Failed;
        const auto uploadStart=diagnostics.Enabled ? melonDS::RenderCostNowNs() : 0;
        VkDeviceSize offset=0;
        for (size_t i=0;i<textures.size();++i) {
            const auto& source=textures[i];
            const auto& target=frame.textures[i];
            VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
            barrier.image=source.resident ? source.resident->Handle() : target.image;
            if (source.resident) {
                frame.residents.push_back(source.resident);
                barrier.oldLayout=barrier.newLayout=VK_IMAGE_LAYOUT_GENERAL;
                barrier.srcAccessMask=VK_ACCESS_MEMORY_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
                d->vkCmdPipelineBarrier(frame.command,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
            } else {
                for (uint32_t y=0;y<source.height;++y)
                    std::memcpy(static_cast<char*>(frame.mapped)+offset+size_t(y)*source.width*4,
                        static_cast<const char*>(source.pixels)+size_t(y)*source.stride,size_t(source.width)*4);
                barrier.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; barrier.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                barrier.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;
                d->vkCmdPipelineBarrier(frame.command,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
                VkBufferImageCopy copy{}; copy.bufferOffset=offset; copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};
                copy.imageExtent={source.width,source.height,1};
                d->vkCmdCopyBufferToImage(frame.command,frame.buffer,target.image,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
                barrier.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; barrier.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT;
                d->vkCmdPipelineBarrier(frame.command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,0,0,nullptr,0,nullptr,1,&barrier);
                offset+=VkDeviceSize(source.width)*source.height*4;
            }
            VkDescriptorImageInfo image{sampler,source.resident ? source.resident->View() : target.view,barrier.newLayout};
            VkWriteDescriptorSet descriptor{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            descriptor.dstSet=sets[i]; descriptor.descriptorCount=1; descriptor.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptor.pImageInfo=&image;
            d->vkUpdateDescriptorSets(device,1,&descriptor,0,nullptr);
        }
        if (uploadStart) { diagnostics.UploadNs+=melonDS::RenderCostNowNs()-uploadStart; diagnostics.StagingBytes+=bytes; }
        VkClearValue black{}; black.color.float32[3]=1;
        VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        pass.renderPass=renderPass; pass.framebuffer=images[index].framebuffer; pass.renderArea.extent=extent;
        pass.clearValueCount=1; pass.pClearValues=&black;
        d->vkCmdBeginRenderPass(frame.command,&pass,VK_SUBPASS_CONTENTS_INLINE);
        d->vkCmdBindPipeline(frame.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipeline);
        VkViewport viewport{0,0,float(width),float(height),0,1};
        VkRect2D scissor{{0,0},extent};
        d->vkCmdSetViewport(frame.command,0,1,&viewport); d->vkCmdSetScissor(frame.command,0,1,&scissor);
        for (const auto& quad : quads) {
            const auto& m=quad.transform;
            const auto& source=textures[quad.texture];
            const double determinant=double(m[0])*m[3]-double(m[1])*m[2];
            if (!determinant) continue;
            const double sx=(source.resident ? source.resident->Width() : source.width)/determinant;
            const double sy=(source.resident ? source.resident->Height() : source.height)/determinant;
            const float parameters[]={2*m[0]/width,2*m[2]/width,2*m[4]/width-1,0,
                2*m[1]/height,2*m[3]/height,2*m[5]/height-1,0,
                float(sx*m[3]),float(-sx*m[2]),float(sx*(double(m[2])*m[5]-double(m[3])*m[4])),0,
                float(-sy*m[1]),float(sy*m[0]),float(sy*(double(m[1])*m[4]-double(m[0])*m[5])),0,
                quad.filter ? 1.f : 0.f,
                m[1]==0 && m[2]==0 && m[0]<0 ? 1.f : 0.f,
                m[1]==0 && m[2]==0 && m[3]<0 ? 1.f : 0.f,0};
            d->vkCmdBindDescriptorSets(frame.command,VK_PIPELINE_BIND_POINT_GRAPHICS,pipelineLayout,0,1,&sets[quad.texture],0,nullptr);
            d->vkCmdPushConstants(frame.command,pipelineLayout,VK_SHADER_STAGE_VERTEX_BIT|VK_SHADER_STAGE_FRAGMENT_BIT,0,sizeof(parameters),parameters);
            d->vkCmdDraw(frame.command,6,1,0,0);
        }
        d->vkCmdEndRenderPass(frame.command);
        return Submit(frame,index,VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,bytes);
    }
#endif

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
        const auto uploadStart = diagnostics.Enabled ? melonDS::RenderCostNowNs() : 0;
        for (uint32_t y=0;y<height;++y)
            std::memcpy(static_cast<char*>(frame.mapped)+size_t(y)*width*4,static_cast<const char*>(pixels)+size_t(y)*stride,size_t(width)*4);
        if (uploadStart) {
            diagnostics.UploadNs += melonDS::RenderCostNowNs() - uploadStart;
            diagnostics.StagingBytes += uint64_t(width) * height * 4;
        }
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
        return Submit(frame, index, VK_PIPELINE_STAGE_TRANSFER_BIT, uint64_t(width) * height * 4);
    }
    Result Submit(Frame& frame, uint32_t index, VkPipelineStageFlags waitStage, uint64_t bytes) {
        auto& image = images[index];
        if (!Check(d->vkEndCommandBuffer(frame.command),"End upload") ||
            !Check(d->vkResetFences(device,1,&frame.fence),"Reset upload fence") ||
            !Check(d->vkResetFences(device,1,&image.presented),"Reset presentation fence")) return Result::Failed;
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        submit.waitSemaphoreCount=1; submit.pWaitSemaphores=&frame.available; submit.pWaitDstStageMask=&waitStage;
        submit.commandBufferCount=1; submit.pCommandBuffers=&frame.command;
        submit.signalSemaphoreCount=1; submit.pSignalSemaphores=&image.finished;
        const auto submitStart = diagnostics.Enabled ? melonDS::RenderCostNowNs() : 0;
        const auto submitted = d->vkQueueSubmit(queue,1,&submit,frame.fence);
        if (submitStart) diagnostics.SubmitNs += melonDS::RenderCostNowNs() - submitStart;
        if (!Check(submitted,"Submit upload")) return Result::Failed;
        if (diagnostics.Enabled) {
            ++diagnostics.Submits;
            diagnostics.TransferBytes += bytes;
        }
        VkSwapchainPresentFenceInfoEXT fenceInfo{VK_STRUCTURE_TYPE_SWAPCHAIN_PRESENT_FENCE_INFO_EXT};
        fenceInfo.swapchainCount=1; fenceInfo.pFences=&image.presented;
        VkPresentInfoKHR info{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        info.pNext=&fenceInfo; info.waitSemaphoreCount=1; info.pWaitSemaphores=&image.finished;
        info.swapchainCount=1; info.pSwapchains=&swapchain; info.pImageIndices=&index;
        const auto presentStart = diagnostics.Enabled ? melonDS::RenderCostNowNs() : 0;
        const auto result=present(queue,&info);
        if (presentStart) diagnostics.QueuePresentNs += melonDS::RenderCostNowNs() - presentStart;
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
#ifdef VULKANRENDERER_ENABLED
std::unique_ptr<Presenter> Presenter::CreateShared(void* window,
    std::shared_ptr<melonDS::Vulkan::Device> device, std::string& error) {
#if defined(Q_OS_WIN) && defined(VK_EXT_swapchain_maintenance1) && QT_CONFIG(vulkan)
    if (!device) { error="No renderer device"; return nullptr; }
    auto result=std::make_unique<Presenter>();
    result->impl->sharedDevice=std::move(device);
    if (!result->impl->Init(window)) { error=result->impl->error; return nullptr; }
    return result;
#else
    error="Shared Vulkan presentation unavailable"; return nullptr;
#endif
}
bool Presenter::UsesDevice(const std::shared_ptr<melonDS::Vulkan::Device>& device) const {
#if defined(Q_OS_WIN) && defined(VK_EXT_swapchain_maintenance1) && QT_CONFIG(vulkan)
    return impl->sharedDevice==device;
#else
    return false;
#endif
}
Presenter::Result Presenter::Present(uint32_t width,uint32_t height,std::span<const Texture> textures,
    std::span<const Quad> quads,std::string& error) {
#if defined(Q_OS_WIN) && defined(VK_EXT_swapchain_maintenance1) && QT_CONFIG(vulkan)
    const auto result=impl->DrawQuads(width,height,textures,quads);
    if (impl->diagnostics.Enabled) {
        ++impl->diagnostics.Calls;
        impl->diagnostics.Skips+=result==Result::Skipped;
        impl->diagnostics.Failures+=result==Result::Failed;
    }
    error=impl->error; return result;
#else
    error="Vulkan textured presentation unavailable"; return Result::Failed;
#endif
}
#endif
Presenter::Diagnostics Presenter::GetDiagnostics() const {
#if defined(Q_OS_WIN) && __has_include(<vulkan/vulkan.h>) && defined(VK_EXT_swapchain_maintenance1) && QT_CONFIG(vulkan)
    return impl->diagnostics;
#else
    return {};
#endif
}
std::unique_ptr<Presenter> Presenter::Create(void* nativeWindow,std::string& error,const std::string& preferredId) {
    error.clear();
#if defined(Q_OS_WIN) && __has_include(<vulkan/vulkan.h>) && defined(VK_EXT_swapchain_maintenance1) && QT_CONFIG(vulkan)
    auto result=std::make_unique<Presenter>();
    if (!result->impl->Init(nativeWindow,preferredId)) { error=result->impl->error; return nullptr; }
    return result;
#else
    error="Vulkan display is not built for this platform"; return nullptr;
#endif
}
Presenter::Result Presenter::Present(const void* pixels,uint32_t width,uint32_t height,uint32_t stride,std::string& error) {
#if defined(Q_OS_WIN) && __has_include(<vulkan/vulkan.h>) && defined(VK_EXT_swapchain_maintenance1) && QT_CONFIG(vulkan)
    const auto result=impl->Draw(pixels,width,height,stride);
    if (impl->diagnostics.Enabled) {
        ++impl->diagnostics.Calls;
        impl->diagnostics.Skips += result == Result::Skipped;
        impl->diagnostics.Failures += result == Result::Failed;
    }
    error=impl->error; return result;
#else
    error="Vulkan display is not built for this platform"; return Result::Failed;
#endif
}
}
