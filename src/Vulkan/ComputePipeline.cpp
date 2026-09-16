// SPDX-License-Identifier: GPL-3.0-or-later
#include "ComputePipeline.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace melonDS::Vulkan {
namespace {
void ImageBarrier(const volk::VolkDeviceTable& f,VkCommandBuffer command,VkImage image,
    VkImageLayout before,VkImageLayout after,VkPipelineStageFlags source,VkPipelineStageFlags dest,
    VkAccessFlags read,VkAccessFlags write,uint32_t layers=1,uint32_t firstLayer=0)
{
    VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};barrier.image=image;
    barrier.oldLayout=before;barrier.newLayout=after;barrier.srcQueueFamilyIndex=barrier.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED;
    barrier.srcAccessMask=read;barrier.dstAccessMask=write;barrier.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,firstLayer,layers};
    f.vkCmdPipelineBarrier(command,source,dest,0,0,nullptr,0,nullptr,1,&barrier);
}
}

ComputePipeline::ComputePipeline(std::shared_ptr<Device> device,const Shaders& shaders,int scale)
    :owner(std::move(device)),f(owner->Functions()),device(owner->Handle()),
    Resources(scale, owner->Properties().limits)
{
    try{Init(shaders);}catch(...){Cleanup();throw;}
}

ComputePipeline::~ComputePipeline(){Cleanup();}

void ComputePipeline::Cleanup()
{
    f.vkDeviceWaitIdle(device);
    for(auto pipeline:pipelines)if(pipeline)f.vkDestroyPipeline(device,pipeline,nullptr);
    if(layout)f.vkDestroyPipelineLayout(device,layout,nullptr);
    if(pool)f.vkDestroyDescriptorPool(device,pool,nullptr);
    if(texturePool)f.vkDestroyDescriptorPool(device,texturePool,nullptr);
    for(auto set:setLayouts)if(set)f.vkDestroyDescriptorSetLayout(device,set,nullptr);
    if(indicesView)f.vkDestroyBufferView(device,indicesView,nullptr);
    if(sampler)f.vkDestroySampler(device,sampler,nullptr);
    for(auto value:textureSamplers)if(value)f.vkDestroySampler(device,value,nullptr);
}

void ComputePipeline::Init(const Shaders& shaders)
{
    for(unsigned i=0;i<buffers.size();++i) {
        const auto usage=(i==9?VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT:i==10?VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT:VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)|
            VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT|(i==7?VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT:0);
        buffers[i]=owner->CreateBuffer(Resources.Sizes[i],usage,false);
    }
    readback=owner->CreateBuffer(Resources.Pixels*4,VK_BUFFER_USAGE_TRANSFER_DST_BIT,true);
    output=owner->CreateImage(Resources.config.ScreenWidth,Resources.config.ScreenHeight,1,VK_FORMAT_R8G8B8A8_UNORM,VK_IMAGE_USAGE_STORAGE_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    clearColor=owner->CreateImage(256,256,1,VK_FORMAT_R32_UINT,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    clearDepth=owner->CreateImage(256,256,1,VK_FORMAT_R32_UINT,VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT);
    VkSamplerCreateInfo sampling{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};sampling.magFilter=sampling.minFilter=VK_FILTER_NEAREST;
    sampling.mipmapMode=VK_SAMPLER_MIPMAP_MODE_NEAREST;sampling.addressModeU=sampling.addressModeV=sampling.addressModeW=VK_SAMPLER_ADDRESS_MODE_REPEAT;
    Device::Check(f.vkCreateSampler(device,&sampling,nullptr,&sampler),"Create clear sampler");
    const VkSamplerAddressMode modes[]={VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,VK_SAMPLER_ADDRESS_MODE_REPEAT,VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT};
    for(unsigned i=0;i<textureSamplers.size();++i) {
        sampling.addressModeU=modes[i%3];sampling.addressModeV=modes[i/3];
        sampling.addressModeW=VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        Device::Check(f.vkCreateSampler(device,&sampling,nullptr,&textureSamplers[i]),"Create texture sampler");
    }
    VkBufferViewCreateInfo view{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};view.buffer=buffers[10]->Handle();view.format=VK_FORMAT_R16G16B16A16_UINT;view.range=VK_WHOLE_SIZE;
    Device::Check(f.vkCreateBufferView(device,&view,nullptr,&indicesView),"Create setup index view");
    // Set 3 keeps setup indices and final output at distinct bindings.
    for(unsigned set=0;set<4;++set) {
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        unsigned count=set==0?8:set==1?1:set==2?3:2;
        for(unsigned i=0;i<count;++i) {
            const auto type=set==0?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:set==1?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                set==2?VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:i==0?VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings.push_back({i,type,1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr});
        }
        VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};info.bindingCount=bindings.size();info.pBindings=bindings.data();
        Device::Check(f.vkCreateDescriptorSetLayout(device,&info,nullptr,&setLayouts[set]),"Create compute bindings");
    }
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT,0,24};
    VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layoutInfo.setLayoutCount=4;layoutInfo.pSetLayouts=setLayouts.data();layoutInfo.pushConstantRangeCount=1;layoutInfo.pPushConstantRanges=&push;
    Device::Check(f.vkCreatePipelineLayout(device,&layoutInfo,nullptr,&layout),"Create compute pipeline layout");
    for(unsigned i=0;i<shaders.size();++i) {
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};moduleInfo.codeSize=shaders[i].size_bytes();moduleInfo.pCode=shaders[i].data();VkShaderModule module{};
        Device::Check(f.vkCreateShaderModule(device,&moduleInfo,nullptr,&module),"Create compute shader");
        VkComputePipelineCreateInfo info{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};info.layout=layout;info.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};
        const auto result=f.vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&info,nullptr,&pipelines[i]);f.vkDestroyShaderModule(device,module,nullptr);Device::Check(result,"Create compute pipeline");
    }
    const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,16},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,3},{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,1}};
    VkDescriptorPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};poolInfo.maxSets=5;poolInfo.poolSizeCount=5;poolInfo.pPoolSizes=sizes;
    Device::Check(f.vkCreateDescriptorPool(device,&poolInfo,nullptr,&pool),"Create compute descriptor pool");
    const VkDescriptorPoolSize textureSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,2048*3};
    poolInfo.maxSets=2048;poolInfo.poolSizeCount=1;poolInfo.pPoolSizes=&textureSize;
    Device::Check(f.vkCreateDescriptorPool(device,&poolInfo,nullptr,&texturePool),"Create texture descriptor pool");
    VkDescriptorSetLayout allocateLayouts[]={setLayouts[0],setLayouts[0],setLayouts[1],setLayouts[2],setLayouts[3]};
    VkDescriptorSet sets[5];VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};allocation.descriptorPool=pool;allocation.descriptorSetCount=5;allocation.pSetLayouts=allocateLayouts;
    Device::Check(f.vkAllocateDescriptorSets(device,&allocation,sets),"Allocate compute descriptors");
    setupSet=sets[0];rasterSet=sets[1];metaSet=sets[2];textureSet=sets[3];indicesSet=outputSet=sets[4];
    auto bufferWrite=[&](VkDescriptorSet set,unsigned binding,unsigned index,VkDescriptorType type) {
        VkDescriptorBufferInfo info{buffers[index]->Handle(),0,Resources.Sizes[index]};VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        write.dstSet=set;write.dstBinding=binding;write.descriptorCount=1;write.descriptorType=type;write.pBufferInfo=&info;f.vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    };
    for(unsigned binding=0;binding<8;++binding) {
        const unsigned raster[]={0,1,3,4,5,6,7,8};
        bufferWrite(rasterSet,binding,raster[binding],VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
        bufferWrite(setupSet,binding,binding==2?2:raster[binding],VK_DESCRIPTOR_TYPE_STORAGE_BUFFER);
    }
    bufferWrite(metaSet,0,9,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER);
    for(unsigned i=0;i<2;++i) {
        VkDescriptorImageInfo image{sampler,i?clearDepth->View():clearColor->View(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=textureSet;write.dstBinding=i;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;write.pImageInfo=&image;f.vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    }
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};write.dstSet=indicesSet;write.dstBinding=0;write.descriptorCount=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;write.pTexelBufferView=&indicesView;
    f.vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    VkDescriptorImageInfo image{VK_NULL_HANDLE,output->View(),VK_IMAGE_LAYOUT_GENERAL};write.dstBinding=1;write.descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;write.pTexelBufferView=nullptr;write.pImageInfo=&image;
    f.vkUpdateDescriptorSets(device,1,&write,0,nullptr);
    const auto command=owner->Begin();
    for(auto target:{clearColor,clearDepth}) {
        ImageBarrier(f,command,target->Handle(),VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,VK_ACCESS_TRANSFER_WRITE_BIT);
        VkClearColorValue zero{};VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};f.vkCmdClearColorImage(command,target->Handle(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&range);
        ImageBarrier(f,command,target->Handle(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT);
    }
    ImageBarrier(f,command,output->Handle(),VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,VK_ACCESS_SHADER_WRITE_BIT);
    owner->SubmitAndWait();
    const uint32_t zero=0;
    dummyTexture=UploadTexture(1,1,1,{&zero,1});
    dummyCapture=UploadTexture(1,1,1,{&zero,1},true);
}

void ComputePipeline::UploadImage(const std::shared_ptr<Device::Image>& image,uint32_t width,uint32_t height,
    uint32_t layers,std::span<const uint32_t> pixels,VkImageLayout oldLayout,uint32_t firstLayer)
{
    auto staging=owner->CreateBuffer(pixels.size_bytes(),VK_BUFFER_USAGE_TRANSFER_SRC_BIT,true);
    std::memcpy(staging->Data(),pixels.data(),pixels.size_bytes());
    const auto command=owner->Begin();
    const bool fresh=oldLayout==VK_IMAGE_LAYOUT_UNDEFINED;
    ImageBarrier(f,command,image->Handle(),oldLayout,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        fresh?VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT:VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,fresh?0:VK_ACCESS_SHADER_READ_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,layers,firstLayer);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,firstLayer,layers};copy.imageExtent={width,height,1};
    f.vkCmdCopyBufferToImage(command,staging->Handle(),image->Handle(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,1,&copy);
    ImageBarrier(f,command,image->Handle(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,layers,firstLayer);
    owner->SubmitAndWait();
}

std::shared_ptr<const ComputePipeline::Texture> ComputePipeline::UploadTexture(uint32_t width,uint32_t height,
    uint32_t layers,std::span<const uint32_t> pixels,bool capture)
{
    const auto& limits=owner->Properties().limits;
    if(!width||!height||!layers||width>limits.maxImageDimension2D||height>limits.maxImageDimension2D||
        layers>limits.maxImageArrayLayers||uint64_t(width)*height*layers!=pixels.size())
        throw std::invalid_argument("Invalid decoded texture array dimensions");
    auto image=owner->CreateImage(width,height,layers,capture?VK_FORMAT_R8G8B8A8_UNORM:VK_FORMAT_R8G8B8A8_UINT,
        VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,true);
    UploadImage(image,width,height,layers,pixels,VK_IMAGE_LAYOUT_UNDEFINED);
    return std::make_shared<Texture>(Texture{std::move(image),width,height,layers,capture});
}

std::shared_ptr<const ComputePipeline::Texture> ComputePipeline::CreateTexture(uint32_t width,uint32_t height,uint32_t layers)
{
    const auto& limits=owner->Properties().limits;
    if(!width||!height||!layers||width>limits.maxImageDimension2D||height>limits.maxImageDimension2D||layers>limits.maxImageArrayLayers)
        throw std::invalid_argument("Invalid texture cache dimensions");
    auto image=owner->CreateImage(width,height,layers,VK_FORMAT_R8G8B8A8_UINT,
        VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT,true);
    const auto command=owner->Begin();
    ImageBarrier(f,command,image->Handle(),VK_IMAGE_LAYOUT_UNDEFINED,VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,0,VK_ACCESS_TRANSFER_WRITE_BIT,layers);
    VkClearColorValue zero{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,layers};
    f.vkCmdClearColorImage(command,image->Handle(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,&zero,1,&range);
    ImageBarrier(f,command,image->Handle(),VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_WRITE_BIT,VK_ACCESS_SHADER_READ_BIT,layers);
    owner->SubmitAndWait();
    return std::make_shared<Texture>(Texture{std::move(image),width,height,layers,false});
}

void ComputePipeline::UploadTextureLayer(const Texture& texture,uint32_t layer,std::span<const uint32_t> pixels)
{
    if(!texture.image||!texture.image->BelongsTo(*owner)||layer>=texture.layers||
        pixels.size()!=uint64_t(texture.width)*texture.height)
        throw std::invalid_argument("Invalid texture cache upload");
    UploadImage(texture.image,texture.width,texture.height,1,pixels,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,layer);
}

void ComputePipeline::UploadClearBitmap(std::span<const uint32_t> colors,std::span<const uint32_t> depths)
{
    if(colors.size()!=256*256||depths.size()!=256*256)throw std::invalid_argument("Invalid clear bitmap dimensions");
    clearBitmapReady=false;
    UploadImage(clearColor,256,256,1,colors,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    UploadImage(clearDepth,256,256,1,depths,VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    clearBitmapReady=true;
}

void ComputePipeline::WriteTextureSet(VkDescriptorSet set,const Variant& variant)
{
    const auto& integerTexture=variant.texture&&!variant.texture->capture?variant.texture:dummyTexture;
    const auto& captureTexture=variant.texture&&variant.texture->capture?variant.texture:dummyCapture;
    const auto sampler=textureSamplers[variant.wrapU+variant.wrapV*3];
    const VkDescriptorImageInfo images[]={
        {sampler,integerTexture->image->View(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler,captureTexture->image->View(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
        {sampler,captureTexture->image->View(),VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
    VkWriteDescriptorSet writes[3]{};
    for(unsigned i=0;i<3;++i) {
        writes[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;writes[i].dstSet=set;writes[i].dstBinding=i;
        writes[i].descriptorCount=1;writes[i].descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;writes[i].pImageInfo=&images[i];
    }
    f.vkUpdateDescriptorSets(device,3,writes,0,nullptr);
}

void ComputePipeline::Bind(VkCommandBuffer command,unsigned shader,VkDescriptorSet storage,VkDescriptorSet image,VkDescriptorSet textures)
{
    const VkDescriptorSet sets[]={storage,metaSet,textures?textures:textureSet,image};
    f.vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipelines[shader]);
    f.vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,4,sets,0,nullptr);
}

void ComputePipeline::Barrier(VkCommandBuffer command)
{
    VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;
    barrier.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
    f.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,0,1,&barrier,0,nullptr,0,nullptr);
}

void ComputePipeline::Validate(const Batch& batch) const
{
    if((batch.meta.DispCnt&(1u<<14))&&!clearBitmapReady)throw std::invalid_argument("Clear bitmap not uploaded");
    if(batch.polygons.size()>2048||batch.variants.size()>256||
        batch.indices.size()>Resources.MaxSpans||batch.edges.size()>12288||
        batch.meta.NumPolygons!=batch.polygons.size()||batch.meta.NumVariants!=batch.variants.size())throw std::invalid_argument("Invalid initial compute batch");
    for(const auto& variant:batch.variants) {
        const auto shader=variant.shader;
        if(shader<5||shader>20||!pipelines[shader])throw std::invalid_argument("Invalid compute raster shader");
        if(((shader-5)&1)!=unsigned(batch.wbuffer))throw std::invalid_argument("Inconsistent compute depth mode");
        if(variant.wrapU>2||variant.wrapV>2)throw std::invalid_argument("Invalid texture wrap mode");
        if(shader>=11&&shader<=18&&!variant.texture)throw std::invalid_argument("Missing compute texture");
        if(variant.texture&&(!variant.texture->image||!variant.texture->image->BelongsTo(*owner)))
            throw std::invalid_argument("Texture belongs to another compute device");
    }
    // A full-width estimate is conservative even for malformed horizontal edges.
    // The caller must split batches before the fixed work storage can overflow.
    uint32_t work=0;
    for (const auto& polygon : batch.polygons) {
        if (polygon.Variant >= batch.variants.size() || polygon.FirstXSpan >= batch.indices.size() ||
            polygon.YTop < 0 || polygon.YBot > Resources.config.ScreenHeight || polygon.YBot <= polygon.YTop ||
            polygon.FirstXSpan + polygon.YBot - polygon.YTop > batch.indices.size())
            throw std::invalid_argument("Invalid polygon spans");
        work += (Resources.config.ScreenWidth / Resources.config.TileSize) *
            ((polygon.YBot + Resources.config.TileSize - 1) / Resources.config.TileSize - polygon.YTop / Resources.config.TileSize);
        if (((polygon.Attr & 0x3F000030u) == 0x30) != (batch.variants[polygon.Variant].shader >= 19))
            throw std::invalid_argument("Inconsistent compute shadow mask variant");
    }
    if(work>Resources.BatchWork)throw std::invalid_argument("Compute batch work capacity exceeded");
    for(const auto& index:batch.indices)if(index.PolyIdx>=batch.polygons.size()||index.SpanIdxL>=batch.edges.size()||index.SpanIdxR>=batch.edges.size())throw std::invalid_argument("Invalid edge index");
}

void ComputePipeline::RecordBatch(VkCommandBuffer command,const Batch& batch,bool first,std::span<const VkDescriptorSet> textures)
{
    auto upload=[&](unsigned index,const void* data,size_t size) {
        const auto* bytes=static_cast<const unsigned char*>(data);
        for(size_t offset=0;offset<size;offset+=65536)f.vkCmdUpdateBuffer(command,buffers[index]->Handle(),offset,std::min(size-offset,size_t(65536)),bytes+offset);
    };
    upload(0,batch.polygons.data(),batch.polygons.size_bytes());upload(2,batch.edges.data(),batch.edges.size_bytes());
    upload(9,&batch.meta,sizeof(batch.meta));upload(10,batch.indices.data(),batch.indices.size_bytes());
    if(batch.indices.size()%32) {
        // The shared interpolation shader has no tail branch. Repeat a valid
        // edge into unused output slots instead of reading uninitialized indices.
        std::array<ComputeData::SetupIndices,31> padding;
        padding.fill(batch.indices.back());
        f.vkCmdUpdateBuffer(command,buffers[10]->Handle(),batch.indices.size_bytes(),
            (32-batch.indices.size()%32)*sizeof(padding[0]),padding.data());
    }
    VkMemoryBarrier uploaded{VK_STRUCTURE_TYPE_MEMORY_BARRIER};uploaded.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;uploaded.dstAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_UNIFORM_READ_BIT;
    f.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&uploaded,0,nullptr,0,nullptr);
    Bind(command,21,setupSet,indicesSet);f.vkCmdDispatch(command,Resources.Tiles/Resources.config.ClearCoarseBinMaskLocalSize,1,1);
    if (!batch.polygons.empty()) {
        Bind(command, batch.wbuffer ? 1 : 0, setupSet, indicesSet);
        f.vkCmdDispatch(command, (batch.indices.size()+31)/32, 1, 1);
        Barrier(command);
        Bind(command, 2, setupSet, indicesSet);
        f.vkCmdDispatch(command, (batch.polygons.size()+31)/32,
            Resources.config.ScreenWidth/(8*Resources.config.TileSize), Resources.config.ScreenHeight/(Resources.config.CoarseTileCountY*Resources.config.TileSize));
        Barrier(command);
        Bind(command, 22, setupSet, indicesSet);
        f.vkCmdDispatch(command, (batch.variants.size()+31)/32, 1, 1);
        Barrier(command);
        Bind(command, 23, setupSet, indicesSet);
        f.vkCmdDispatchIndirect(command, buffers[7]->Handle(), offsetof(ComputeData::BinResultHeader, SortWorkWorkCount));
        Barrier(command);
        for (unsigned variant=0; variant<batch.variants.size(); ++variant) {
            const auto& state=batch.variants[variant];
            Bind(command, state.shader, rasterSet, indicesSet, textures[variant]);
            struct Push {
                uint32_t variant,padding;
                float invWidth,invHeight;
                int32_t capture;
                float captureYOffset;
            } push{variant,0,0,0,0,state.captureYOffset};
            static_assert(sizeof(Push)==24);
            if(state.texture) {
                push.invWidth=1.f/state.texture->width;push.invHeight=1.f/state.texture->height;
                if(state.texture->capture)push.capture=state.texture->width==128?1:2;
            }
            f.vkCmdPushConstants(command, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
            f.vkCmdDispatchIndirect(command, buffers[7]->Handle(), variant*16);
        }
    }
    Barrier(command);Bind(command,batch.wbuffer?4:3,rasterSet,indicesSet);
    const uint32_t firstBatch=first;
    f.vkCmdPushConstants(command,layout,VK_SHADER_STAGE_COMPUTE_BIT,0,4,&firstBatch);
    f.vkCmdDispatch(command,Resources.config.ScreenWidth/Resources.config.TileSize,Resources.config.ScreenHeight/Resources.config.TileSize,1);Barrier(command);
}

std::vector<uint32_t> ComputePipeline::Render(const Batch& batch)
{
    return Render(std::span<const Batch>(&batch,1));
}

std::vector<uint32_t> ComputePipeline::Render(std::span<const Batch> batches)
{
    if(batches.empty())throw std::invalid_argument("Compute frame needs a clear batch");
    size_t variantCount=0,polygonCount=0;
    for(const auto& batch:batches) {
        Validate(batch);
        if(batch.wbuffer!=batches.front().wbuffer||
            std::memcmp(&batch.meta.AlphaRef,&batches.front().meta.AlphaRef,
                sizeof(ComputeData::MetaUniform)-offsetof(ComputeData::MetaUniform,AlphaRef)))
            throw std::invalid_argument("Compute frame state changed between batches");
        variantCount+=batch.variants.size();polygonCount+=batch.polygons.size();
    }
    if(variantCount>2048||polygonCount>2048)throw std::invalid_argument("Compute frame exceeds DS polygon capacity");
    Device::Check(f.vkResetDescriptorPool(device,texturePool,0),"Reset texture descriptors");
    std::vector<VkDescriptorSet> textures(variantCount);
    if(variantCount) {
        std::vector<VkDescriptorSetLayout> layouts(variantCount,setLayouts[2]);
        VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool=texturePool;allocation.descriptorSetCount=variantCount;allocation.pSetLayouts=layouts.data();
        Device::Check(f.vkAllocateDescriptorSets(device,&allocation,textures.data()),"Allocate texture descriptors");
        size_t index=0;
        for(const auto& batch:batches)for(const auto& variant:batch.variants)WriteTextureSet(textures[index++],variant);
    }
    const auto command=owner->Begin();
    size_t offset=0;
    bool first=true;
    for(const auto& batch:batches) {
        if(!first) {
            // Reuse scratch buffers only after prior compute/indirect consumers.
            VkMemoryBarrier reuse{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            reuse.srcAccessMask=VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT|VK_ACCESS_UNIFORM_READ_BIT|VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
            reuse.dstAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT|VK_ACCESS_SHADER_READ_BIT|VK_ACCESS_SHADER_WRITE_BIT;
            f.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT|VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT|VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,0,1,&reuse,0,nullptr,0,nullptr);
        }
        RecordBatch(command,batch,first,std::span<const VkDescriptorSet>(textures).subspan(offset,batch.variants.size()));
        first=false;offset+=batch.variants.size();
    }
    const auto dispCnt=batches.back().meta.DispCnt;
    const unsigned effect=((dispCnt>>5)&1)|((dispCnt>>6)&2)|((dispCnt>>2)&4);
    Bind(command,24+effect,rasterSet,outputSet);f.vkCmdDispatch(command,Resources.config.ScreenWidth/32,Resources.config.ScreenHeight,1);
    ImageBarrier(f,command,output->Handle(),VK_IMAGE_LAYOUT_GENERAL,VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_ACCESS_SHADER_WRITE_BIT,VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy copy{};copy.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1};copy.imageExtent={uint32_t(Resources.config.ScreenWidth),uint32_t(Resources.config.ScreenHeight),1};f.vkCmdCopyImageToBuffer(command,output->Handle(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,readback->Handle(),1,&copy);
    ImageBarrier(f,command,output->Handle(),VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,VK_IMAGE_LAYOUT_GENERAL,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_ACCESS_TRANSFER_READ_BIT,VK_ACCESS_SHADER_WRITE_BIT);
    VkMemoryBarrier download{VK_STRUCTURE_TYPE_MEMORY_BARRIER};download.srcAccessMask=VK_ACCESS_TRANSFER_WRITE_BIT;download.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
    f.vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_TRANSFER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&download,0,nullptr,0,nullptr);
    owner->SubmitAndWait();std::vector<uint32_t> result(Resources.Pixels);std::memcpy(result.data(),readback->Data(),Resources.Pixels*4);return result;
}
}
