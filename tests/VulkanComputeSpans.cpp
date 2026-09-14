// SPDX-License-Identifier: GPL-3.0-or-later
// Real GPU differential check of shared DS edge interpolation: GL vs Vulkan.
#include "frontend/glad/glad.h"
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include "Vulkan/Device.h"
#include "Vulkan/ComputePipeline.h"
#include "Vulkan/EmbeddedShaders.h"
#include "GPU3D.h"
#include "GPU3D_ComputeData.h"
#include "GPU3D_ComputeShader.h"
#include <array>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <type_traits>
#include <vector>

using namespace melonDS;
using namespace melonDS::ComputeData;
constexpr unsigned Polygons = 6, Lines = 32, Spans = Polygons * Lines;

static_assert(!std::is_copy_constructible_v<Vulkan::Device::Buffer>);
static_assert(!std::is_copy_constructible_v<Vulkan::Device::Image>);

static VKAPI_ATTR VkResult VKAPI_CALL FailedWait(VkDevice,uint32_t,const VkFence*,VkBool32,uint64_t)
{
    return VK_ERROR_OUT_OF_HOST_MEMORY;
}

static void SubmissionFailure()
{
    std::string error;auto device=Vulkan::Device::Create(error);
    if(!device)throw std::runtime_error(error);
    auto& functions=const_cast<volk::VolkDeviceTable&>(device->Functions());
    const auto wait=functions.vkWaitForFences;
    device->Begin();functions.vkWaitForFences=FailedWait;
    bool failed=false;
    try{device->SubmitAndWait();}catch(const std::runtime_error&){failed=true;}
    functions.vkWaitForFences=wait;
    if(!failed)throw std::runtime_error("Failed GPU wait was not reported");
    try{device->Begin();}catch(const std::runtime_error&){
        std::puts("Vulkan failed wait: device drained and subsequent recording rejected PASS");return;
    }
    throw std::runtime_error("Failed GPU submission was reused");
}

static void Check(VkResult result)
{
    if (result != VK_SUCCESS) throw std::runtime_error("Vulkan result " + std::to_string(result));
}

struct Inputs {
    std::array<RenderPolygon,Polygons> polygons{};
    std::array<SpanSetupY,Polygons*2> edges{};
    std::vector<SetupIndices> indices;
    MetaUniform meta{};
    const unsigned scale;
    explicit Inputs(unsigned scale=1) : indices(Spans*scale),scale(scale) {
        meta.NumPolygons=Polygons; meta.NumVariants=1; meta.DispCnt=(1<<3)|(1<<4);
        for (unsigned p=0;p<Polygons;++p) {
            Vertex vertices[4]{}; Polygon polygon{};
            polygon.NumVertices=4;
            s32 positions[10][2]{{10+int(p)*7,0},{180-int(p)*3,0},
                {150+int(p)*5,Lines},{45-int(p)*2,Lines}};
            for (unsigned v=0;v<4;++v) {
                polygon.Vertices[v]=&vertices[v];
                polygon.FinalZ[v]=0x10000+p*0x20000+v*0x1234;
                polygon.FinalW[v]=p%2 ? 0x1000+v*0x321 : 0x1000;
                for (unsigned c=0;c<3;++c) vertices[v].FinalColor[c]=((p*7+v*11+c*13)%64)<<3;
                vertices[v].TexCoords[0]=int(v)*123-int(p)*71;
                vertices[v].TexCoords[1]=int(p)*57-int(v)*91;
            }
            for (unsigned v=0;v<4;++v)
                for (auto& coordinate:positions[v]) coordinate*=scale;
            auto& out=polygons[p]; out.FirstXSpan=p*Lines*scale;
            out.YTop=0;out.YBot=Lines*scale;out.XMin=256*scale;out.XMax=-1;
            out.Attr=((p%2?15u:31u)<<16)|(3<<6);out.Variant=0;
            SetupYSpan(&out,&edges[p*2],&polygon,0,3,0,positions);
            SetupYSpan(&out,&edges[p*2+1],&polygon,1,2,1,positions);
            for (unsigned y=0;y<Lines*scale;++y) indices[p*Lines*scale+y]={u16(p),u16(p*2),u16(p*2+1),u16(y)};
        }
    }
};

static std::vector<SpanSetupX> GLSpans(const Inputs& input, unsigned variant)
{
    const auto source=ComputeShader::BuildSource(variant,ComputeShader::VulkanConfig(input.scale),false);
    const char* text=source.c_str();
    GLuint shader=glCreateShader(GL_COMPUTE_SHADER);glShaderSource(shader,1,&text,nullptr);glCompileShader(shader);
    GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if (!ok) { char log[4096];glGetShaderInfoLog(shader,sizeof(log),nullptr,log);throw std::runtime_error(log); }
    GLuint program=glCreateProgram();glAttachShader(program,shader);glLinkProgram(program);glDeleteShader(shader);
    glGetProgramiv(program,GL_LINK_STATUS,&ok);if(!ok)throw std::runtime_error("GL link failed");
    std::vector<SpanSetupX> output(input.indices.size()+1);std::memset(output.data(),0xCD,output.size()*sizeof(SpanSetupX));
    GLuint buffers[5],texture;glGenBuffers(5,buffers);glGenTextures(1,&texture);
    const void* data[]={input.polygons.data(),output.data(),input.edges.data(),&input.meta,input.indices.data()};
    const size_t sizes[]={sizeof(input.polygons),output.size()*sizeof(SpanSetupX),sizeof(input.edges),sizeof(input.meta),input.indices.size()*sizeof(SetupIndices)};
    for(unsigned i=0;i<5;++i) {
        const GLenum target=i<3?GL_SHADER_STORAGE_BUFFER:i==3?GL_UNIFORM_BUFFER:GL_TEXTURE_BUFFER;
        glBindBuffer(target,buffers[i]);glBufferData(target,sizes[i],data[i],GL_DYNAMIC_DRAW);
        if(i<4)glBindBufferBase(target,i<3?i:0,buffers[i]);
    }
    glBindTexture(GL_TEXTURE_BUFFER,texture);glTexBuffer(GL_TEXTURE_BUFFER,GL_RGBA16UI,buffers[4]);
    glBindImageTexture(0,texture,0,GL_FALSE,0,GL_READ_ONLY,GL_RGBA16UI);
    glUseProgram(program);glDispatchCompute(input.indices.size()/32,1,1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[1]);glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,0,sizes[1],output.data());
    glDeleteTextures(1,&texture);glDeleteBuffers(5,buffers);glDeleteProgram(program);
    if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("GL span execution failed");
    return output;
}

struct VulkanSpans {
    std::shared_ptr<melonDS::Vulkan::Device> owner;
    const volk::VolkDeviceTable* d=nullptr;
    VkDevice device{};VkDescriptorPool descriptors{};VkPipelineLayout layout{};
    std::array<VkDescriptorSetLayout,4> sets{};
    std::array<std::shared_ptr<melonDS::Vulkan::Device::Buffer>,5> buffers{};
    VkBufferView indices{};
    ~VulkanSpans() {
        if(!d)return;
        d->vkDeviceWaitIdle(device);
        if(indices)d->vkDestroyBufferView(device,indices,nullptr);
        if(descriptors)d->vkDestroyDescriptorPool(device,descriptors,nullptr);
        if(layout)d->vkDestroyPipelineLayout(device,layout,nullptr);
        for(auto set:sets)if(set)d->vkDestroyDescriptorSetLayout(device,set,nullptr);
    }
    bool Init() {
        std::string error;owner=melonDS::Vulkan::Device::Create(error);
        if(!owner){std::fprintf(stderr,"Vulkan compute unavailable: %s\n",error.c_str());return false;}
        device=owner->Handle();d=&owner->Functions();
        const VkDescriptorType types[]={VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER};
        for(unsigned i=0;i<4;++i) {
            VkDescriptorSetLayoutBinding bindings[3]{};unsigned count=i==0?3:i==2?0:1;
            for(unsigned j=0;j<count;++j)bindings[j]={j,types[i],1,VK_SHADER_STAGE_COMPUTE_BIT,nullptr};
            VkDescriptorSetLayoutCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};info.bindingCount=count;info.pBindings=bindings;
            Check(d->vkCreateDescriptorSetLayout(device,&info,nullptr,&sets[i]));
        }
        VkPipelineLayoutCreateInfo layoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};layoutInfo.setLayoutCount=4;layoutInfo.pSetLayouts=sets.data();
        Check(d->vkCreatePipelineLayout(device,&layoutInfo,nullptr,&layout));
        const VkDescriptorPoolSize sizes[]={{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,3},{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,1},{VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER,1}};
        VkDescriptorPoolCreateInfo descInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};descInfo.maxSets=4;descInfo.poolSizeCount=3;descInfo.pPoolSizes=sizes;
        Check(d->vkCreateDescriptorPool(device,&descInfo,nullptr,&descriptors));
        return true;
    }
    void Allocate(unsigned index,size_t size,const void* data) {
        const auto usage=index<3?VK_BUFFER_USAGE_STORAGE_BUFFER_BIT:index==3?VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT:VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
        buffers[index]=owner->CreateBuffer(size,usage,true);
        std::memcpy(buffers[index]->Data(),data,size);
    }

    std::vector<SpanSetupX> Run(const Inputs& input,std::span<const uint32_t> words) {
        std::vector<SpanSetupX> output(input.indices.size()+1);std::memset(output.data(),0xCD,output.size()*sizeof(SpanSetupX));
        const size_t sizes[]={sizeof(input.polygons),output.size()*sizeof(SpanSetupX),sizeof(input.edges),sizeof(input.meta),input.indices.size()*sizeof(SetupIndices)};
        const void* data[]={input.polygons.data(),output.data(),input.edges.data(),&input.meta,input.indices.data()};
        for(unsigned i=0;i<5;++i)Allocate(i,sizes[i],data[i]);
        VkBufferViewCreateInfo view{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};view.buffer=buffers[4]->Handle();view.format=VK_FORMAT_R16G16B16A16_UINT;view.range=VK_WHOLE_SIZE;
        Check(d->vkCreateBufferView(device,&view,nullptr,&indices));
        VkDescriptorSet descriptorSets[4];VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool=descriptors;allocation.descriptorSetCount=4;allocation.pSetLayouts=sets.data();
        Check(d->vkAllocateDescriptorSets(device,&allocation,descriptorSets));
        VkDescriptorBufferInfo infos[4]{};VkWriteDescriptorSet writes[5]{};
        for(unsigned i=0;i<5;++i) {
            auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=descriptorSets[i<3?0:i==3?1:3];
            w.dstBinding=i<3?i:0;w.descriptorCount=1;w.descriptorType=i<3?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:i==3?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            if(i<4){infos[i]={buffers[i]->Handle(),0,sizes[i]};w.pBufferInfo=&infos[i];}else w.pTexelBufferView=&indices;
        }
        d->vkUpdateDescriptorSets(device,5,writes,0,nullptr);
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        moduleInfo.codeSize=words.size_bytes();moduleInfo.pCode=words.data();VkShaderModule module{};
        Check(d->vkCreateShaderModule(device,&moduleInfo,nullptr,&module));
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipelineInfo.layout=layout;
        pipelineInfo.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};VkPipeline pipeline{};
        const auto pipelineResult=d->vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipelineInfo,nullptr,&pipeline);d->vkDestroyShaderModule(device,module,nullptr);Check(pipelineResult);
        const auto command=owner->Begin();
        d->vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);d->vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,4,descriptorSets,0,nullptr);
        d->vkCmdDispatch(command,input.indices.size()/32,1,1);VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        d->vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);
        owner->SubmitAndWait();
        std::memcpy(output.data(),buffers[1]->Data(),sizes[1]);d->vkDestroyPipeline(device,pipeline,nullptr);
        return output;
    }
};

// Independent GL command sequence using the same shader math, including its
// original separate image/texture binding namespaces.
struct TextureInput {
    std::shared_ptr<const Vulkan::ComputePipeline::Texture> texture;
    std::vector<uint32_t> pixels;
};

static std::vector<uint32_t> GLFrame(const Vulkan::ComputePipeline::Batch& batch,unsigned scale,
    std::span<const TextureInput> sources={},std::span<const uint32_t> clearColors={},std::span<const uint32_t> clearDepths={})
{
    const auto config=ComputeShader::VulkanConfig(scale);
    struct Resources {
        GLuint buffers[11]{},textures[4]{},programs[32]{};
        std::vector<GLuint> materials;
        ~Resources() {
            glUseProgram(0);
            for(auto program:programs)if(program)glDeleteProgram(program);
            glDeleteBuffers(11,buffers);glDeleteTextures(4,textures);
            glDeleteTextures(materials.size(),materials.data());
        }
    } resources;
    auto use=[&](unsigned variant) {
        auto& program=resources.programs[variant];
        if(!program) {
            const auto source=ComputeShader::BuildSource(variant,config,false);
            const char* text=source.c_str();
            const auto shader=glCreateShader(GL_COMPUTE_SHADER);
            glShaderSource(shader,1,&text,nullptr);glCompileShader(shader);
            GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
            if(!ok) {
                char log[4096];glGetShaderInfoLog(shader,sizeof(log),nullptr,log);
                glDeleteShader(shader);throw std::runtime_error(log);
            }
            program=glCreateProgram();glAttachShader(program,shader);glLinkProgram(program);glDeleteShader(shader);
            glGetProgramiv(program,GL_LINK_STATUS,&ok);
            if(!ok)throw std::runtime_error("GL reference link failed");
        }
        glUseProgram(program);
    };
    const size_t tiles=32*24*scale*scale,work=tiles*16,pixels=256*192*scale*scale;
    const size_t sizes[]={2048*sizeof(RenderPolygon),131072*scale*sizeof(SpanSetupX),12288*sizeof(SpanSetupY),
        work*64*4,work*64*4,work*64*4,pixels*7*4,
        sizeof(BinResultHeader)+tiles*(2+64+64)*4,work*2*8,sizeof(MetaUniform),131072*scale*sizeof(SetupIndices)};
    glGenBuffers(11,resources.buffers);glGenTextures(4,resources.textures);
    resources.materials.resize(batch.variants.size()*2);glGenTextures(resources.materials.size(),resources.materials.data());
    const GLint modes[]={GL_CLAMP_TO_EDGE,GL_REPEAT,GL_MIRRORED_REPEAT};
    for(unsigned i=0;i<batch.variants.size();++i) {
        const auto& variant=batch.variants[i];
        const TextureInput* source=nullptr;
        if(variant.texture) {
            for(const auto& candidate:sources)if(candidate.texture==variant.texture)source=&candidate;
            if(!source)throw std::runtime_error("GL reference texture data missing");
        }
        for(unsigned capture=0;capture<2;++capture) {
            const bool actual=source&&source->texture->capture==bool(capture);
            const unsigned width=actual?source->texture->width:1,height=actual?source->texture->height:1,layers=actual?source->texture->layers:1;
            const uint32_t zero=0;
            glBindTexture(GL_TEXTURE_2D_ARRAY,resources.materials[i*2+capture]);
            glTexStorage3D(GL_TEXTURE_2D_ARRAY,1,capture?GL_RGBA8:GL_RGBA8UI,width,height,layers);
            glTexSubImage3D(GL_TEXTURE_2D_ARRAY,0,0,0,0,width,height,layers,capture?GL_RGBA:GL_RGBA_INTEGER,
                GL_UNSIGNED_BYTE,actual?source->pixels.data():&zero);
            glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
            glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_S,modes[variant.wrapU]);
            glTexParameteri(GL_TEXTURE_2D_ARRAY,GL_TEXTURE_WRAP_T,modes[variant.wrapV]);
        }
    }
    for(unsigned i=0;i<11;++i) {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,resources.buffers[i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER,sizes[i],nullptr,GL_DYNAMIC_DRAW);
    }
    auto upload=[&](unsigned index,const void* data,size_t size) {
        if(!size)return;
        glBindBuffer(GL_SHADER_STORAGE_BUFFER,resources.buffers[index]);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER,0,size,data);
    };
    upload(0,batch.polygons.data(),batch.polygons.size_bytes());
    upload(2,batch.edges.data(),batch.edges.size_bytes());upload(9,&batch.meta,sizeof(batch.meta));
    std::vector<SetupIndices> indices(batch.indices.begin(),batch.indices.end());
    if(!indices.empty())indices.resize((indices.size()+31)&~size_t(31),indices.back());
    upload(10,indices.data(),indices.size()*sizeof(SetupIndices));
    for(unsigned binding=0;binding<8;++binding) {
        const unsigned mapping[]={0,1,2,4,5,6,7,8};
        glBindBufferBase(GL_SHADER_STORAGE_BUFFER,binding,resources.buffers[mapping[binding]]);
    }
    glBindBufferBase(GL_UNIFORM_BUFFER,0,resources.buffers[9]);
    glBindTexture(GL_TEXTURE_BUFFER,resources.textures[0]);
    glTexBuffer(GL_TEXTURE_BUFFER,GL_RGBA16UI,resources.buffers[10]);
    glBindImageTexture(0,resources.textures[0],0,GL_FALSE,0,GL_READ_ONLY,GL_RGBA16UI);
    auto barrier=[] {glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT|GL_COMMAND_BARRIER_BIT);};
    use(21);glDispatchCompute(12*scale*scale,1,1);
    if(!batch.polygons.empty()) {
        use(batch.wbuffer?1:0);glDispatchCompute(indices.size()/32,1,1);barrier();
        use(2);glDispatchCompute((batch.polygons.size()+31)/32,4*scale,6*scale);barrier();
        use(22);glDispatchCompute((batch.variants.size()+31)/32,1,1);barrier();
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER,resources.buffers[7]);
        use(23);glDispatchComputeIndirect(offsetof(BinResultHeader,SortWorkWorkCount));barrier();
        for(unsigned i=0;i<3;++i)glBindBufferBase(GL_SHADER_STORAGE_BUFFER,2+i,resources.buffers[3+i]);
        for(unsigned i=0;i<batch.variants.size();++i) {
            const auto& variant=batch.variants[i];
            for(unsigned unit=0;unit<3;++unit) {
                glActiveTexture(GL_TEXTURE0+unit);glBindSampler(unit,0);
                glBindTexture(GL_TEXTURE_2D_ARRAY,resources.materials[i*2+(unit?1:0)]);
            }
            use(variant.shader);glUniform1ui(0,i);
            const auto program=resources.programs[variant.shader];
            glUniform2f(glGetUniformLocation(program,"InvTextureSize"),variant.texture?1.f/variant.texture->width:0,variant.texture?1.f/variant.texture->height:0);
            glUniform1i(glGetUniformLocation(program,"TexIsCapture"),variant.texture&&variant.texture->capture?(variant.texture->width==128?1:2):0);
            glUniform1f(glGetUniformLocation(program,"CaptureYOffset"),variant.captureYOffset);
            glDispatchComputeIndirect(i*16);
        }
    }
    barrier();
    // Bind valid integer clear textures even when the bitmap path is disabled.
    for(unsigned i=0;i<2;++i) {
        glActiveTexture(GL_TEXTURE0+i);glBindSampler(i,0);
        glBindTexture(GL_TEXTURE_2D,resources.textures[2+i]);
        const uint32_t zero=0;
        const auto data=i?clearDepths:clearColors;
        if((batch.meta.DispCnt&(1<<14))&&data.size()!=256*256)throw std::runtime_error("GL reference clear bitmap missing");
        const auto size=data.empty()?1:256;
        glTexImage2D(GL_TEXTURE_2D,0,GL_R32UI,size,size,0,GL_RED_INTEGER,GL_UNSIGNED_INT,data.empty()?&zero:data.data());
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
    }
    use(batch.wbuffer?4:3);glUniform1i(0,1);glDispatchCompute(32*scale,24*scale,1);barrier();
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,resources.textures[1]);
    glTexStorage2D(GL_TEXTURE_2D,1,GL_RGBA8,256*scale,192*scale);
    glBindImageTexture(0,resources.textures[1],0,GL_FALSE,0,GL_WRITE_ONLY,GL_RGBA8);
    unsigned final=24;
    if(batch.meta.DispCnt&(1<<5))final+=1;
    if(batch.meta.DispCnt&(1<<7))final+=2;
    if(batch.meta.DispCnt&(1<<4))final+=4;
    use(final);glDispatchCompute(8*scale,192*scale,1);glMemoryBarrier(GL_TEXTURE_UPDATE_BARRIER_BIT);
    std::vector<uint32_t> result(pixels);glGetTexImage(GL_TEXTURE_2D,0,GL_RGBA,GL_UNSIGNED_BYTE,result.data());
    if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("GL reference graph error");
    return result;
}

static void Frames(unsigned scale)
{
    const auto& shaders=Vulkan::EmbeddedShaders(scale);
    std::string error;auto device=Vulkan::Device::Create(error);
    if(!device)throw std::runtime_error(error);
    Vulkan::ComputePipeline pipeline(device,shaders,scale);
    std::vector<TextureInput> sources;
    for(unsigned kind=0;kind<3;++kind) {
        const unsigned width=kind==0?8:kind==1?128:256,height=16,layers=2;
        TextureInput input;
        input.pixels.resize(width*height*layers);
        const unsigned mask=kind?255:63;
        for(unsigned i=0;i<input.pixels.size();++i) {
            const unsigned alpha=i%3==0?0:i%3==1?(kind?57:7):(kind?255:31);
            input.pixels[i]=((i*13)&mask)|(((i*7+19)&mask)<<8)|(((i*3+29)&mask)<<16)|(alpha<<24);
        }
        input.texture=pipeline.UploadTexture(width,height,layers,input.pixels,kind!=0);
        sources.push_back(std::move(input));
    }
    std::vector<uint32_t> clearColors(256*256),clearDepths(256*256);
    for(unsigned i=0;i<clearColors.size();++i) {
        clearColors[i]=(i%64)|(((i/256)%64)<<8)|(27<<16)|((i%32)<<24);
        clearDepths[i]=0x10000+(i%256+i/256)*0x200;
        if(i%3==0)clearDepths[i]|=1<<24;
    }
    pipeline.UploadClearBitmap(clearColors,clearDepths);
    for(unsigned mode=0;mode<2;++mode) {
        for(unsigned scene=0;scene<8;++scene) {
            Inputs input(scale);input.meta.ClearDepth=0xFFFFFF;input.meta.ClearColor=0x1F020406;
            for(unsigned i=0;i<34;++i) {
                input.meta.ToonTable[i*4]=((i*11)%64)|(((i*7)%64)<<8)|(((i*3)%64)<<16);
                input.meta.ToonTable[i*4+1]=i*3;input.meta.ToonTable[i*4+2]=0x3F0020;
            }
            std::vector<Vulkan::ComputePipeline::Variant> variants{{5+mode}};
            if(scene==1) {
                variants={{5+mode},{7+mode},{9+mode},{19+mode}};
                input.meta.DispCnt|=(1<<5)|(1<<7);
                input.meta.FogColor=0x100B2030;input.meta.FogShift=2;
                for(unsigned p=0;p<Polygons;++p) {
                    input.polygons[p].Variant=p%variants.size();
                    input.polygons[p].Attr|=(p<<24)|(1<<15);
                    if(p%variants.size()==3)
                        input.polygons[p].Attr=(input.polygons[p].Attr&~0x3F000030u)|0x30;
                    else if(p==4)
                        input.polygons[p].Attr|=0x30; // Draw through the preceding shadow mask.
                }
            }
            if(scene>=4&&scene<=6) {
                variants={{11+mode},{13+mode},{15+mode},{17+mode}};
                input.meta.DispCnt|=1;
                for(unsigned i=0;i<variants.size();++i) {
                    variants[i].texture=sources[scene-4].texture;
                    variants[i].wrapU=i%3;variants[i].wrapV=(i+1)%3;
                    variants[i].captureYOffset=.25f;
                }
                for(unsigned p=0;p<Polygons;++p) {
                    input.polygons[p].Variant=p%variants.size();input.polygons[p].TextureLayer=p%2;
                    input.polygons[p].Attr|=p<<24;
                }
            }
            if(scene==7) {
                input.meta.DispCnt|=(1<<14)|(1<<7);
                input.meta.ClearBitmapOffset[0]=17.f/256;input.meta.ClearBitmapOffset[1]=239.f/256;
                input.meta.FogColor=0x1F3F0700;input.meta.FogShift=2;
            }
            Vulkan::ComputePipeline::Batch batch{input.polygons,input.edges,input.indices,variants,input.meta,mode!=0};
            batch.meta.NumVariants=variants.size();
            if(scene==2) {
                --input.polygons.back().YBot;
                batch.indices=batch.indices.first(input.indices.size()-1);
            }
            if(scene==3) {
                batch.polygons={};batch.edges={};batch.indices={};batch.variants={};
                batch.meta.NumPolygons=0;batch.meta.NumVariants=0;
            }
            const auto pixels=pipeline.Render(batch);
            const auto expected=GLFrame(batch,scale,sources,clearColors,clearDepths);
            if(pixels.size()!=size_t(256*192*scale*scale))throw std::runtime_error("Unexpected compute output dimensions");
            if(pixels!=expected) {
                for(unsigned i=0;i<pixels.size();++i)if(pixels[i]!=expected[i]) {
                    std::fprintf(stderr,"Frame mismatch scale=%u mode=%u scene=%u xy=%u,%u Vk=%08x GL=%08x\n",scale,mode,scene,i%(256*scale),i/(256*scale),pixels[i],expected[i]);break;
                }
                throw std::runtime_error("Vulkan/GL final pixels differ");
            }
            unsigned changed=0;for(auto pixel:pixels)changed+=pixel!=pixels.back();
            if(scene!=3&&changed<100)throw std::runtime_error("Compute graph produced no polygon coverage");
            if(scene==3&&changed)throw std::runtime_error("Empty compute frame retained old polygons");
            std::printf("Vulkan/GL %ux %s full graph scene=%u: all %zu pixels equal, %u non-background PASS\n",scale,mode?"W":"Z",scene,pixels.size(),changed);
            if(scene==1||scene>=4) {
                // End the first batch after the shadow mask, so the following
                // shadow polygon needs the retained depth/stencil state.
                constexpr unsigned split=4;
                const unsigned spanSplit=split*Lines*scale;
                auto tailPolygons=input.polygons;
                for(unsigned p=split;p<Polygons;++p)tailPolygons[p].FirstXSpan-=spanSplit;
                std::vector<SetupIndices> tailIndices(batch.indices.begin()+spanSplit,batch.indices.end());
                for(auto& index:tailIndices)index.PolyIdx-=split;
                std::array<Vulkan::ComputePipeline::Batch,2> parts{batch,batch};
                parts[0].polygons=batch.polygons.first(split);parts[0].indices=batch.indices.first(spanSplit);parts[0].meta.NumPolygons=split;
                parts[1].polygons=std::span<const RenderPolygon>(tailPolygons).subspan(split);
                parts[1].indices=tailIndices;parts[1].meta.NumPolygons=Polygons-split;
                if(pipeline.Render(parts)!=expected)throw std::runtime_error("Split Vulkan frame differs from combined GL frame");
                std::printf("Vulkan %ux %s scene=%u two-batch depth/stencil/texture composition equal PASS\n",scale,mode?"W":"Z",scene);
            }
        }
    }
}

int main(int argc,char** argv)
{
    QGuiApplication app(argc,argv);
    QSurfaceFormat format;format.setVersion(4,3);format.setProfile(QSurfaceFormat::CoreProfile);
    QOpenGLContext context;context.setFormat(format);if(!context.create())return 77;
    QOffscreenSurface surface;surface.setFormat(context.format());surface.create();if(!context.makeCurrent(&surface))return 77;
    if(!gladLoadGLLoader([](const char* name)->void*{return reinterpret_cast<void*>(QOpenGLContext::currentContext()->getProcAddress(name));}))return 77;
    try {
        for(unsigned scale=1;scale<=3;++scale) {
            const Inputs input(scale);
            std::array<std::unique_ptr<VulkanSpans>,2> devices;
            for(auto& vk:devices){vk=std::make_unique<VulkanSpans>();if(!vk->Init())return 77;}
            for(unsigned variant=0;variant<2;++variant) {
                const auto expected=GLSpans(input,variant);
                const auto actual=devices[variant]->Run(input,Vulkan::EmbeddedShaders(scale)[variant]);
                if(std::memcmp(expected.data(),actual.data(),actual.size()*sizeof(SpanSetupX))) {
                    for(unsigned i=0;i<actual.size();++i)if(std::memcmp(&expected[i],&actual[i],sizeof(SpanSetupX))){std::fprintf(stderr,"Span mismatch scale=%u variant=%u index=%u\n",scale,variant,i);break;}
                    return 2;
                }
                const unsigned char* guard=reinterpret_cast<const unsigned char*>(&actual.back());
                for(unsigned i=0;i<sizeof(SpanSetupX);++i)if(guard[i]!=0xCD)return 3;
                devices[variant].reset(); // The other initialized device must remain usable.
                std::printf("Vulkan/GL %ux %s: %zu spans, all 24 words and output guard equal PASS\n",scale,variant?"W":"Z",input.indices.size());
            }
            Frames(scale);
        }
        SubmissionFailure();
    }catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 4;}
    return 0;
}
