// SPDX-License-Identifier: GPL-3.0-or-later
#include "Native2DCapture.h"
#include "Native2DPipeline.h"
#include <array>
#include <bitset>
#include <cstring>
#include <stdexcept>

namespace melonDS::Vulkan::Native2D {
namespace {
void Require(bool value, const char* reason) { if (!value) throw std::invalid_argument(reason); }
void Barrier(Device& owner, VkCommandBuffer cmd, VkPipelineStageFlags from, VkPipelineStageFlags to,
    VkAccessFlags src, VkAccessFlags dst) {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER}; b.srcAccessMask=src; b.dstAccessMask=dst;
    owner.Functions().vkCmdPipelineBarrier(cmd,from,to,0,1,&b,0,nullptr,0,nullptr);
}
}
CapturePipeline::CapturePipeline(std::shared_ptr<Device> device, std::span<const uint32_t> shader)
    : owner(std::move(device))
{
    Require(owner && !shader.empty(), "Invalid native capture input");
    const auto& f=owner->Functions(); const auto d=owner->Handle();
    VkShaderModule module{};
    try {
        std::array<VkDescriptorSetLayoutBinding,7> entries{};
        for(uint32_t i=0;i<7;++i) entries[i]={i,(i==3||i==6)?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
        VkDescriptorSetLayoutCreateInfo bi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        bi.bindingCount=7; bi.pBindings=entries.data();
        Device::Check(f.vkCreateDescriptorSetLayout(d,&bi,nullptr,&bindings),"Native capture bindings");
        const VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,16};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount=1; li.pSetLayouts=&bindings; li.pushConstantRangeCount=1; li.pPushConstantRanges=&push;
        Device::Check(f.vkCreatePipelineLayout(d,&li,nullptr,&layout),"Native capture layout");
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize=shader.size_bytes(); si.pCode=shader.data();
        Device::Check(f.vkCreateShaderModule(d,&si,nullptr,&module),"Native capture shader");
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO}; pi.layout=layout;
        pi.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
        Device::Check(f.vkCreateComputePipelines(d,owner->GetPipelineCache(),1,&pi,nullptr,&pipeline),"Native capture pipeline");
        f.vkDestroyShaderModule(d,module,nullptr); module=VK_NULL_HANDLE;
        const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,5},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,2}};
        VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pci.maxSets=1; pci.poolSizeCount=2; pci.pPoolSizes=sizes;
        Device::Check(f.vkCreateDescriptorPool(d,&pci,nullptr,&pool),"Native capture pool");
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool=pool; ai.descriptorSetCount=1; ai.pSetLayouts=&bindings;
        Device::Check(f.vkAllocateDescriptorSets(d,&ai,&set),"Native capture descriptors");
        source=owner->CreateBuffer(256*sizeof(CaptureCommand),VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true);
        dispatches.reserve(257);
        banks=owner->CreateBuffer(4*131072,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,false);
        blank=owner->CreateImage(256,192,1,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    } catch(...) {
        if(module) f.vkDestroyShaderModule(d,module,nullptr);
        Cleanup(); throw;
    }
}
CapturePipeline::~CapturePipeline() {
    try { CompletePending(); } catch (...) {}
    Cleanup();
}
void CapturePipeline::CompletePending() const {
    if(pendingWriter) pendingWriter->Complete();
}
uint64_t CapturePipeline::Revision() const {
    CompletePending();
    return revision;
}
bool CapturePipeline::EnableHires(std::span<const uint32_t> shader,uint32_t scale) {
    CompletePending();
    if(hires) return scale==hiresScale;
    if(scale<2 || scale>11 || shader.empty() || !dispatches.empty()) return false;
    const uint32_t pitch=(scale*scale+1u)&~1u;
    const VkDeviceSize bytes=VkDeviceSize(524288)*pitch;
    if(bytes>owner->Properties().limits.maxStorageBufferRange) return false;
    const auto& f=owner->Functions();const auto d=owner->Handle();
    VkShaderModule module{};VkPipeline nextPipeline{};
    try {
        auto next=owner->CreateBuffer(bytes,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,false);
        VkShaderModuleCreateInfo si{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        si.codeSize=shader.size_bytes();si.pCode=shader.data();
        Device::Check(f.vkCreateShaderModule(d,&si,nullptr,&module),"Hires capture shader");
        VkComputePipelineCreateInfo pi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pi.layout=layout;
        pi.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
        Device::Check(f.vkCreateComputePipelines(d,owner->GetPipelineCache(),1,&pi,nullptr,&nextPipeline),"Hires capture pipeline");
        f.vkDestroyShaderModule(d,module,nullptr);module=VK_NULL_HANDLE;
        hires=std::move(next);hiresPipeline=nextPipeline;hiresScale=scale;hiresPitch=pitch;
        return true;
    } catch(...) {
        if(module)f.vkDestroyShaderModule(d,module,nullptr);
        if(nextPipeline)f.vkDestroyPipeline(d,nextPipeline,nullptr);
        return false;
    }
}
std::shared_ptr<Device::Buffer> CapturePipeline::Snapshot() const {
    CompletePending();
    if(!initialized || previousBanks) throw std::logic_error("Native capture snapshot is not complete");
    return banks;
}
void CapturePipeline::DisableHires() {
    CompletePending();
    if(!dispatches.empty()) throw std::logic_error("Native hires capture is still pending");
    if(hiresPipeline) owner->Functions().vkDestroyPipeline(owner->Handle(),hiresPipeline,nullptr);
    hiresPipeline=VK_NULL_HANDLE;hires.reset();hiresScale=0;hiresPitch=0;
}
void CapturePipeline::Cleanup() {
    const auto& f=owner->Functions(); const auto d=owner->Handle();
    if(pool) f.vkDestroyDescriptorPool(d,pool,nullptr);
    if(pipeline) f.vkDestroyPipeline(d,pipeline,nullptr);
    if(hiresPipeline) f.vkDestroyPipeline(d,hiresPipeline,nullptr);
    if(layout) f.vkDestroyPipelineLayout(d,layout,nullptr);
    if(bindings) f.vkDestroyDescriptorSetLayout(d,bindings,nullptr);
    pool=VK_NULL_HANDLE; pipeline=hiresPipeline=VK_NULL_HANDLE; layout=VK_NULL_HANDLE; bindings=VK_NULL_HANDLE;
}
void CapturePipeline::Prepare(std::span<const CaptureCommand> commands,
    const std::shared_ptr<Device::Buffer>& raw, uint32_t rawWords,
    const std::shared_ptr<Device::Image>& native3D,
    const std::shared_ptr<Device::Buffer>& scaledRaw,const std::shared_ptr<Device::Image>& display3D)
{
    CompletePending();
    Require(!commands.empty() && commands.size()<=256 && raw && raw->BelongsTo(*owner) &&
        (raw->Usage()&VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) && uint64_t(rawWords)*4<=raw->Size(),
        "Invalid native capture batch/raw buffer");
    const auto image=native3D?native3D:blank;
    Require(image->BelongsTo(*owner) && image->Format()==VK_FORMAT_R8G8B8A8_UNORM &&
        (image->Usage()&VK_IMAGE_USAGE_STORAGE_BIT),"Invalid native capture 3D image");
    const auto enhanced=display3D?display3D:image;
    if(hires) Require(enhanced->BelongsTo(*owner) && enhanced->Format()==VK_FORMAT_R8G8B8A8_UNORM &&
        (enhanced->Usage()&VK_IMAGE_USAGE_STORAGE_BIT),"Invalid hires capture display image");
    const auto scaled=scaledRaw?scaledRaw:raw;
    if(hires) Require(scaled->BelongsTo(*owner) && (scaled->Usage()&VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) &&
        scaled!=hires && scaled!=banks,"Invalid hires capture raw buffer");
    dispatches.clear(); dispatches.push_back(0);
    std::array<std::bitset<512>,4> reads{}, writesSinceBarrier{};
    for(uint32_t i=0;i<commands.size();++i) {
        const auto& c=commands[i]; const auto& row=c.row;
        const uint32_t size=(row.control>>20)&3, width=size?256:128, height=size?size*64:128;
        const uint32_t segments=width/128, dstBank=(row.control>>16)&3;
        const uint32_t dst=(((((row.control>>18)&3)<<14)+row.line*width)&65535)/128;
        Require(row.line<height && uint64_t(row.rawFirst)+width<=rawWords && c.hasB<=1 &&
            c.gpuMask<(1u<<segments) && (c.hasB || !c.gpuMask) &&
            c.hiresMask<(1u<<segments) && c.hiresWriteMask<(1u<<segments) &&
            (c.hasB || !c.hiresMask) && (hires || (!c.hiresMask && !c.scaledRawFirst && !c.hiresWriteMask)),
            "Invalid native capture command");
        const uint32_t srcBank=c.sourceWord/32768, src=(c.sourceWord%32768)/64;
        const uint32_t readMask=c.gpuMask|c.hiresMask;
        if(readMask) Require(srcBank<4 && c.sourceWord%(width/2)==0 && src+segments<=512,
            "Invalid native capture GPU source B");
        if((row.control&(1u<<24)) && !row.sourceAbort && ((row.control>>29)&3)!=1)
            Require(row.sourceScale && image->Width()==row.sourceScale*256 && image->Height()==row.sourceScale*192,
                "Invalid native capture 3D scale");
        if(c.hiresWriteMask && ((row.control>>29)&3)!=1) {
            if(!(row.control&(1u<<24)))
                Require(scaledRaw && uint64_t(c.scaledRawFirst)+uint64_t(256)*hiresScale*hiresScale<=scaled->Size()/4,
                    "Invalid hires capture composed source A");
            else if(!row.sourceAbort)
                Require(row.sourceScale && enhanced->Width()==row.sourceScale*256 && enhanced->Height()==row.sourceScale*192,
                    "Invalid hires capture 3D scale");
        }
        bool dependency=false;
        for(uint32_t s=0;s<segments;++s) {
            // RAW, WAW and WAR all matter. An earlier B read must finish before
            // a later row overwrites that address, even if it wrote elsewhere.
            dependency |= writesSinceBarrier[dstBank][dst+s] || reads[dstBank][dst+s];
            if(readMask&(1u<<s)) dependency |= writesSinceBarrier[srcBank][src+s];
        }
        if(i && dependency) {
            dispatches.push_back(i); reads={}; writesSinceBarrier={};
        }
        for(uint32_t s=0;s<segments;++s) {
            writesSinceBarrier[dstBank].set(dst+s);
            if(readMask&(1u<<s)) reads[srcBank].set(src+s);
        }
    }
    dispatches.push_back(uint32_t(commands.size()));
    // Old LCDC/page consumers keep this completed version immutable. Reuse the
    // allocation when nobody retains it; otherwise copy entirely on-device.
    if(initialized && banks.use_count()>1) {
        auto next=owner->CreateBuffer(banks->Size(),banks->Usage(),false);
        previousBanks=banks; banks=std::move(next);
    }
    std::memcpy(source->Data(),commands.data(),commands.size_bytes());
    const auto& f=owner->Functions(); const auto d=owner->Handle();
    const std::array inputs{raw,source,banks,std::shared_ptr<Device::Buffer>{},hires,scaled};
    std::array<VkDescriptorBufferInfo,6> infos{};
    const std::array<VkDescriptorImageInfo,2> imageInfo{{
        {VK_NULL_HANDLE,image->View(),VK_IMAGE_LAYOUT_GENERAL},
        {VK_NULL_HANDLE,enhanced->View(),VK_IMAGE_LAYOUT_GENERAL}}};
    std::array<VkWriteDescriptorSet,7> writes{};
    const uint32_t count=hires?7:4;
    for(uint32_t i=0;i<count;++i) {
        writes[i]={VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}; writes[i].dstSet=set;
        writes[i].dstBinding=i; writes[i].descriptorCount=1;
        writes[i].descriptorType=(i==3||i==6)?VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        if(i==3||i==6) writes[i].pImageInfo=&imageInfo[i==3?0:1];
        else { infos[i]={inputs[i]->Handle(),0,inputs[i]->Size()}; writes[i].pBufferInfo=&infos[i]; }
    }
    f.vkUpdateDescriptorSets(d,count,writes.data(),0,nullptr);
}
void CapturePipeline::Record(VkCommandBuffer cmd)
{
    const auto& f=owner->Functions();
    if(previousBanks) {
        Barrier(*owner,cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
        const VkBufferCopy copy{0,0,banks->Size()};
        f.vkCmdCopyBuffer(cmd,previousBanks->Handle(),banks->Handle(),1,&copy);
    }
    if(!initialized) {
        f.vkCmdFillBuffer(cmd,banks->Handle(),0,banks->Size(),0);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.image=blank->Handle(); b.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout=VK_IMAGE_LAYOUT_GENERAL;
        b.srcQueueFamilyIndex=b.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
        b.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        f.vkCmdPipelineBarrier(cmd,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,0,nullptr,0,nullptr,1,&b);
        const VkClearColorValue zero{};
        f.vkCmdClearColorImage(cmd,blank->Handle(),VK_IMAGE_LAYOUT_GENERAL,&zero,1,&b.subresourceRange);
    }
    if(hires && !hiresInitialized) f.vkCmdFillBuffer(cmd,hires->Handle(),0,hires->Size(),0);
    Barrier(*owner,cmd,VK_PIPELINE_STAGE_ALL_COMMANDS_BIT|VK_PIPELINE_STAGE_HOST_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_ACCESS_MEMORY_WRITE_BIT|VK_ACCESS_HOST_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
    f.vkCmdBindDescriptorSets(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,1,&set,0,nullptr);
    for(size_t i=1;i<dispatches.size();++i) {
        if(i>1) Barrier(*owner,cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        const uint32_t push[]={dispatches[i-1],dispatches[i]-dispatches[i-1],hiresScale,hiresPitch};
        f.vkCmdPushConstants(cmd,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,sizeof(push),push);
        if(hires) {
            // Read the previous native B before the exact guest writer updates
            // it. Disjoint u32 subpixel blocks make exact self-alias race-free.
            f.vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,hiresPipeline);
            f.vkCmdDispatch(cmd,2*hiresPitch,push[1],1);
            Barrier(*owner,cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT);
        }
        f.vkCmdBindPipeline(cmd,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);
        f.vkCmdDispatch(cmd,2,push[1],1);
    }
}
void CapturePipeline::ReadRange(uint32_t bank, uint32_t first, std::span<uint16_t> destination)
{
    CompletePending();
    Require(initialized && bank<4 && first<65536 && destination.size()<=65536-first &&
        !(first&1) && !(destination.size()&1),"Invalid native capture readback range");
    if(destination.empty()) return;
    if(!landing || landing->Size()<destination.size_bytes())
        landing=owner->CreateBuffer(destination.size_bytes(),VK_BUFFER_USAGE_TRANSFER_DST_BIT,true,VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    const auto cmd=owner->Begin(Device::SubmitKind::Display);
    Barrier(*owner,cmd,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    const VkBufferCopy copy{VkDeviceSize(bank)*131072+first*2,0,destination.size_bytes()};
    owner->Functions().vkCmdCopyBuffer(cmd,banks->Handle(),landing->Handle(),1,&copy);
    Barrier(*owner,cmd,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,
        VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_HOST_READ_BIT);
    owner->SubmitAndWait();
    std::memcpy(destination.data(),landing->Data(),destination.size_bytes());
}
}
