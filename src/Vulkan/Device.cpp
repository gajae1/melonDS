// SPDX-License-Identifier: GPL-3.0-or-later
#include "Device.h"
#include "MemoryType.h"
#include "AdapterSelection.h"
#include <algorithm>
#include <cstring>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace melonDS::Vulkan {
void Device::Check(VkResult result,const char* operation)
{
    if(result!=VK_SUCCESS)throw std::runtime_error(std::string(operation)+" (Vulkan "+std::to_string(result)+")");
}

std::vector<Device::Adapter> Device::Enumerate(std::string& error)
{
    error.clear();
    std::vector<Adapter> adapters;
    try { Device probe; probe.Init({}, &adapters); }
    catch (const std::exception& failure) { error = failure.what(); adapters.clear(); }
    return adapters;
}

std::shared_ptr<Device> Device::Create(std::string& error, const std::string& preferred)
{
    error.clear();
    try {
        auto result=std::shared_ptr<Device>(new Device);
        result->Init(preferred);return result;
    }catch(const std::exception& failure){error=failure.what();return nullptr;}
}

void Device::Init(const std::string& preferred, std::vector<Adapter>* adapters)
{
    // Process-wide loader only. Instance/device functions are never published
    // globally, so another emulator device cannot replace this one's dispatch.
    static std::once_flag once;
    static VkResult loaded=VK_ERROR_INITIALIZATION_FAILED;
    std::call_once(once,[]{loaded=volk::volkInitialize();});
    Check(loaded,"Load Vulkan");
    if(volk::volkGetInstanceVersion()<VK_API_VERSION_1_1)throw std::runtime_error("Vulkan 1.1 required");
    uint32_t count=0;
    Check(volk::vkEnumerateInstanceExtensionProperties(nullptr,&count,nullptr),"Instance extensions");
    std::vector<VkExtensionProperties> extensions(count);
    Check(volk::vkEnumerateInstanceExtensionProperties(nullptr,&count,extensions.data()),"Instance extensions");
    const bool portability=std::any_of(extensions.begin(),extensions.end(),[](const auto& e){return !std::strcmp(e.extensionName,VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);});
    const char* extension=VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};app.pApplicationName="melonDS compute";app.apiVersion=VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};info.pApplicationInfo=&app;
    if(portability){info.flags=VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;info.enabledExtensionCount=1;info.ppEnabledExtensionNames=&extension;}
    Check(volk::vkCreateInstance(&info,nullptr,&instance),"Create compute instance");
    volk::volkLoadInstanceTable(&instanceFunctions,instance);
    auto& f=instanceFunctions;
    Check(f.vkEnumeratePhysicalDevices(instance,&count,nullptr),"Enumerate compute GPUs");
    std::vector<VkPhysicalDevice> devices(count);
    Check(f.vkEnumeratePhysicalDevices(instance,&count,devices.data()),"Enumerate compute GPUs");
    uint32_t family=0;
    int bestRank=-1;
    bool portabilitySubset=false;
    for(auto candidate:devices) {
        VkPhysicalDeviceProperties props{};f.vkGetPhysicalDeviceProperties(candidate,&props);
        VkPhysicalDeviceFeatures features{};f.vkGetPhysicalDeviceFeatures(candidate,&features);
        if(props.apiVersion<VK_API_VERSION_1_1||!features.shaderStorageImageExtendedFormats||
            props.limits.maxPerStageDescriptorStorageBuffers<8||props.limits.maxBoundDescriptorSets<4||
            props.limits.maxComputeWorkGroupInvocations<64||props.limits.maxComputeWorkGroupSize[0]<64||
            props.limits.maxComputeWorkGroupSize[1]<8||props.limits.maxComputeWorkGroupCount[0]<4096||
            props.limits.maxComputeWorkGroupCount[2]<12288||props.limits.maxStorageBufferRange<131072*96||
            props.limits.maxTexelBufferElements<131072||props.limits.maxImageDimension2D<1024||
            props.limits.maxImageArrayLayers<64)continue;
        struct RequiredFormat { VkFormat format; VkFormatFeatureFlags image, buffer; };
        const RequiredFormat formats[]={
            {VK_FORMAT_R8G8B8A8_UNORM,VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT|VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|
                VK_FORMAT_FEATURE_TRANSFER_SRC_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT,0},
            {VK_FORMAT_R8G8B8A8_UINT,VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT,0},
            {VK_FORMAT_R32_UINT,VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT|VK_FORMAT_FEATURE_TRANSFER_DST_BIT,0},
            {VK_FORMAT_R16G16B16A16_UINT,0,VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT}};
        bool supported=true;
        for(const auto& required:formats) {
            VkFormatProperties format{};f.vkGetPhysicalDeviceFormatProperties(candidate,required.format,&format);
            supported&=(format.optimalTilingFeatures&required.image)==required.image&&
                (format.bufferFeatures&required.buffer)==required.buffer;
        }
        if(!supported)continue;
        f.vkGetPhysicalDeviceQueueFamilyProperties(candidate,&count,nullptr);
        std::vector<VkQueueFamilyProperties> queues(count);f.vkGetPhysicalDeviceQueueFamilyProperties(candidate,&count,queues.data());
        for(uint32_t i=0;i<count;++i)if(queues[i].queueCount&&(queues[i].queueFlags&VK_QUEUE_COMPUTE_BIT)) {
            VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
            VkPhysicalDeviceProperties2 details{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
            details.pNext=&ids; f.vkGetPhysicalDeviceProperties2(candidate,&details);
            const auto candidateId=AdapterId(ids.deviceUUID);
            if(adapters) adapters->push_back({candidateId,props.deviceName,props.deviceType});
            const int rank=AdapterRank(props.deviceType);
            if((preferred.empty() && rank>bestRank) || (!preferred.empty() && candidateId==preferred)) {
                physical=candidate;properties=props;family=i;id=candidateId;bestRank=rank;
            }
            break;
        }
    }
    if(adapters)return;
    if(!physical)throw std::runtime_error(preferred.empty() ? "No GPU satisfies compute resource limits" :
        "Selected GPU is unavailable or does not satisfy compute resource limits");
    Check(f.vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,nullptr),"Device extensions");
    extensions.resize(count);Check(f.vkEnumerateDeviceExtensionProperties(physical,nullptr,&count,extensions.data()),"Device extensions");
    // Portability subset is mandatory to enable when advertised (e.g. MoltenVK).
    for(const auto& e:extensions)portabilitySubset|=!std::strcmp(e.extensionName,"VK_KHR_portability_subset");
    extension="VK_KHR_portability_subset";
    VkPhysicalDeviceFeatures enabled{};enabled.shaderStorageImageExtendedFormats=VK_TRUE;
    const float priority=1;
    VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};queueInfo.queueFamilyIndex=family;queueInfo.queueCount=1;queueInfo.pQueuePriorities=&priority;
    VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};deviceInfo.queueCreateInfoCount=1;deviceInfo.pQueueCreateInfos=&queueInfo;deviceInfo.pEnabledFeatures=&enabled;
    if(portabilitySubset){deviceInfo.enabledExtensionCount=1;deviceInfo.ppEnabledExtensionNames=&extension;}
    Check(f.vkCreateDevice(physical,&deviceInfo,nullptr,&device),"Create compute device");
    volk::volkLoadDeviceTable(&functions,device);
    functions.vkGetDeviceQueue(device,family,0,&queue);
    f.vkGetPhysicalDeviceMemoryProperties(physical,&memoryProperties);
    VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};poolInfo.queueFamilyIndex=family;poolInfo.flags=VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    Check(functions.vkCreateCommandPool(device,&poolInfo,nullptr,&pool),"Create compute command pool");
    VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};commandInfo.commandPool=pool;commandInfo.commandBufferCount=1;commandInfo.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    Check(functions.vkAllocateCommandBuffers(device,&commandInfo,&command),"Allocate compute command buffer");
    VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    Check(functions.vkCreateFence(device,&fenceInfo,nullptr,&fence),"Create compute fence");
}

Device::~Device()
{
    if(device) {
        functions.vkDeviceWaitIdle(device);
        if(fence)functions.vkDestroyFence(device,fence,nullptr);
        if(pool)functions.vkDestroyCommandPool(device,pool,nullptr);
        if(pipelineCache)functions.vkDestroyPipelineCache(device,pipelineCache,nullptr);
        functions.vkDestroyDevice(device,nullptr);
    }
    if(instance)instanceFunctions.vkDestroyInstance(instance,nullptr);
}

VkPipelineCache Device::GetPipelineCache()
{
    if (!pipelineCacheInitialized) {
        VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
        VkPipelineCache created{};
        const auto result=functions.vkCreatePipelineCache(device,&info,nullptr,&created);
        if (result==VK_SUCCESS) pipelineCache=created;
        else if (result!=VK_ERROR_OUT_OF_HOST_MEMORY && result!=VK_ERROR_OUT_OF_DEVICE_MEMORY)
            Check(result,"Create compute pipeline cache");
        pipelineCacheInitialized=true;
    }
    return pipelineCache;
}

void Device::ClearPipelineCache()
{
    // Pipeline construction and cache controls share the rendering thread.
    // Compiled VkPipeline objects do not retain this cache object.
    if (pipelineCache) functions.vkDestroyPipelineCache(device,pipelineCache,nullptr);
    pipelineCache=VK_NULL_HANDLE;
    pipelineCacheInitialized=false;
}

void Device::TrimPipelineCache()
{
    if (!pipelineCache) return;
    // Exportable data size is a soft growth limit, not total driver residency.
    // Query only: no large allocation and no cache files are written.
    constexpr size_t maxDataBytes=32u*1024u*1024u;
    size_t bytes=0;
    const auto result=functions.vkGetPipelineCacheData(device,pipelineCache,&bytes,nullptr);
    if (result==VK_ERROR_OUT_OF_HOST_MEMORY || result==VK_ERROR_OUT_OF_DEVICE_MEMORY ||
        (result==VK_SUCCESS && bytes>maxDataBytes)) ClearPipelineCache();
    else Check(result,"Query compute pipeline cache size");
}

uint32_t Device::MemoryType(uint32_t bits,VkMemoryPropertyFlags required,VkMemoryPropertyFlags preferred) const
{
    return SelectMemoryType(memoryProperties,bits,required,preferred);
}

std::shared_ptr<Device::Buffer> Device::CreateBuffer(VkDeviceSize size,VkBufferUsageFlags usage,bool hostVisible,
    VkMemoryPropertyFlags preferred,VkMemoryPropertyFlags additionalRequired)
{
    if(!size)throw std::invalid_argument("Zero-sized Vulkan buffer");
    auto buffer=std::shared_ptr<Buffer>(new Buffer(shared_from_this()));buffer->size=size;buffer->usage=usage;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=size;info.usage=usage;
    Check(functions.vkCreateBuffer(device,&info,nullptr,&buffer->buffer),"Create compute buffer");
    VkMemoryRequirements req{};functions.vkGetBufferMemoryRequirements(device,buffer->buffer,&req);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=req.size;
    const VkMemoryPropertyFlags required=(hostVisible?
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT:VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)|additionalRequired;
    allocation.memoryTypeIndex=MemoryType(req.memoryTypeBits,required,preferred);
    VkDeviceMemory allocated{};
    auto result=functions.vkAllocateMemory(device,&allocation,nullptr,&allocated);
    if (preferred && (result==VK_ERROR_OUT_OF_DEVICE_MEMORY || result==VK_ERROR_OUT_OF_HOST_MEMORY)) {
        const auto fallback=MemoryType(req.memoryTypeBits,required);
        if (fallback!=allocation.memoryTypeIndex) {
            allocation.memoryTypeIndex=fallback;
            allocated=VK_NULL_HANDLE;
            result=functions.vkAllocateMemory(device,&allocation,nullptr,&allocated);
        }
    }
    Check(result,"Allocate compute buffer memory");
    buffer->memory=allocated;
    buffer->properties=memoryProperties.memoryTypes[allocation.memoryTypeIndex].propertyFlags;
    Check(functions.vkBindBufferMemory(device,buffer->buffer,buffer->memory,0),"Bind compute buffer");
    if(hostVisible)Check(functions.vkMapMemory(device,buffer->memory,0,size,0,&buffer->mapped),"Map compute buffer");
    return buffer;
}

Device::Buffer::~Buffer()
{
    const auto& f=owner->functions;const auto d=owner->device;
    if(mapped)f.vkUnmapMemory(d,memory);
    if(buffer)f.vkDestroyBuffer(d,buffer,nullptr);
    if(memory)f.vkFreeMemory(d,memory,nullptr);
}

std::shared_ptr<Device::Image> Device::CreateImage(uint32_t width,uint32_t height,uint32_t layers,
    VkFormat format,VkImageUsageFlags usage,bool arrayView)
{
    auto image=std::shared_ptr<Image>(new Image(shared_from_this()));
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};info.imageType=VK_IMAGE_TYPE_2D;info.extent={width,height,1};
    info.mipLevels=1;info.arrayLayers=layers;info.format=format;info.tiling=VK_IMAGE_TILING_OPTIMAL;info.samples=VK_SAMPLE_COUNT_1_BIT;info.usage=usage;
    Check(functions.vkCreateImage(device,&info,nullptr,&image->image),"Create compute image");
    VkMemoryRequirements req{};functions.vkGetImageMemoryRequirements(device,image->image,&req);
    VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};allocation.allocationSize=req.size;allocation.memoryTypeIndex=MemoryType(req.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    Check(functions.vkAllocateMemory(device,&allocation,nullptr,&image->memory),"Allocate compute image memory");
    Check(functions.vkBindImageMemory(device,image->image,image->memory,0),"Bind compute image");
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};view.image=image->image;view.format=format;
    view.viewType=arrayView||layers>1?VK_IMAGE_VIEW_TYPE_2D_ARRAY:VK_IMAGE_VIEW_TYPE_2D;view.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,layers};
    Check(functions.vkCreateImageView(device,&view,nullptr,&image->view),"Create compute image view");
    return image;
}

Device::Image::~Image()
{
    const auto& f=owner->functions;const auto d=owner->device;
    if(view)f.vkDestroyImageView(d,view,nullptr);
    if(image)f.vkDestroyImage(d,image,nullptr);
    if(memory)f.vkFreeMemory(d,memory,nullptr);
}

VkCommandBuffer Device::Begin()
{
    if(failed)throw std::runtime_error("Compute device retired after submission failure");
    if(recording)throw std::logic_error("Compute command recording already active");
    Check(functions.vkResetCommandBuffer(command,0),"Reset compute commands");
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};begin.flags=VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    Check(functions.vkBeginCommandBuffer(command,&begin),"Begin compute commands");recording=true;return command;
}

void Device::SubmitAndWait()
{
    if(!recording)throw std::logic_error("No compute commands to submit");
    recording=false;
    try {
        Check(functions.vkEndCommandBuffer(command),"End compute commands");
        Check(functions.vkResetFences(device,1,&fence),"Reset compute fence");
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        Check(functions.vkQueueSubmit(queue,1,&submit,fence),"Submit compute commands");
        ++submissionCount;
        Check(functions.vkWaitForFences(device,1,&fence,VK_TRUE,UINT64_MAX),"Wait for compute commands");
    }catch(...) {
        failed=true;
        // A failed wait does not imply queue completion. Drain outstanding work
        // before stack unwinding can release resources referenced by it.
        functions.vkDeviceWaitIdle(device);
        throw;
    }
}
}
