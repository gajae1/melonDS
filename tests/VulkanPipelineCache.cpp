// SPDX-License-Identifier: GPL-3.0-or-later
// Native Vulkan pipelines; failures are injected only at optional cache creation.
#include "Vulkan/ComputePipeline.h"
#include "Vulkan/EmbeddedShaders.h"
#include <cstdio>
#include <stdexcept>
#include <vector>
using namespace melonDS::Vulkan;
namespace {
PFN_vkCreatePipelineCache DriverCache;
PFN_vkCreateComputePipelines DriverPipelines;
unsigned Creates=0, Pipelines=0, Cached=0;
VkResult CacheFailure=VK_SUCCESS;
VKAPI_ATTR VkResult VKAPI_CALL CreateCache(VkDevice d,const VkPipelineCacheCreateInfo* i,
    const VkAllocationCallbacks* a,VkPipelineCache* out)
{
    ++Creates;
    if(CacheFailure!=VK_SUCCESS) return CacheFailure;
    return DriverCache(d,i,a,out);
}
VKAPI_ATTR VkResult VKAPI_CALL CreatePipelines(VkDevice d,VkPipelineCache cache,uint32_t n,
    const VkComputePipelineCreateInfo* i,const VkAllocationCallbacks* a,VkPipeline* out)
{
    Pipelines+=n; if(cache) Cached+=n;
    return DriverPipelines(d,cache,n,i,a,out);
}
void Require(bool ok,const char* message) { if(!ok)throw std::runtime_error(message); }
struct Observe {
    volk::VolkDeviceTable& f;
    Observe(Device& d,VkResult failure):f(const_cast<volk::VolkDeviceTable&>(d.Functions())) {
        Creates=Pipelines=Cached=0; CacheFailure=failure;
        DriverCache=f.vkCreatePipelineCache; DriverPipelines=f.vkCreateComputePipelines;
        f.vkCreatePipelineCache=CreateCache; f.vkCreateComputePipelines=CreatePipelines;
    }
    ~Observe() { f.vkCreatePipelineCache=DriverCache; f.vkCreateComputePipelines=DriverPipelines; }
};
bool Check(VkResult failure)
{
    std::string error;auto device=Device::Create(error);
    if(!device) { std::fprintf(stderr,"%s\n",error.c_str());return false; }
    Observe observe(*device,failure);
    std::vector<uint32_t> expected;
    for(unsigned i=0;i<2;++i) {
        ComputePipeline pipeline(device,EmbeddedShaders());
        ComputePipeline::Batch batch{};batch.meta.ClearColor=0x1F102030;batch.meta.ClearDepth=0xFFFFFF;
        const auto pixels=pipeline.Render(batch);
        if(i==0) expected=pixels;
        else Require(expected==pixels,"recreated cached pipeline changed output");
    }
    std::printf("cache failure=%d creates=%u pipelines=%u cached=%u\n",int(failure),Creates,Pipelines,Cached);
    Require(Creates==1 && Pipelines==64,"pipeline cache was not retained on its device");
    Require(Cached==(failure==VK_SUCCESS?64u:0u),"optional cache fallback was not respected");
    return true;
}
}
int main()
{
    try {
        if(!Check(VK_SUCCESS) || !Check(VK_ERROR_OUT_OF_DEVICE_MEMORY)) return 77;
        std::string error;auto device=Device::Create(error);
        if(!device)return 77;
        Observe observe(*device,VK_ERROR_DEVICE_LOST);
        bool rejected=false;
        try { ComputePipeline pipeline(device,EmbeddedShaders()); }
        catch(const std::runtime_error&) { rejected=true; }
        Require(rejected && Creates==1 && Pipelines==0,"non-memory cache failure was hidden");
        std::puts("Vulkan pipeline cache reuse, identical pixels, optional OOM and fatal error propagation PASS");
        return 0;
    } catch(const std::exception& e) { std::fprintf(stderr,"%s\n",e.what());return 1; }
}
