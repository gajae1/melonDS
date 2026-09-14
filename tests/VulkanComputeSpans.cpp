// SPDX-License-Identifier: GPL-3.0-or-later
// Real GPU differential check of shared DS edge interpolation: GL vs Vulkan.
#include "frontend/glad/glad.h"
#include <QGuiApplication>
#include <QOffscreenSurface>
#include <QOpenGLContext>
#include <QVulkanInstance>
#include <QVulkanFunctions>
#include <QFile>
#include "GPU3D.h"
#include "GPU3D_ComputeData.h"
#include "GPU3D_ComputeShader.h"
#include <array>
#include <cstring>
#include <cstdio>
#include <stdexcept>
#include <vector>

using namespace melonDS;
using namespace melonDS::ComputeData;
constexpr unsigned Polygons = 6, Lines = 32, Spans = Polygons * Lines;
constexpr ComputeShader::Config Config{256,192,12288,8,4,32,64};

static void Check(VkResult result)
{
    if (result != VK_SUCCESS) throw std::runtime_error("Vulkan result " + std::to_string(result));
}

struct Inputs {
    std::array<RenderPolygon,Polygons> polygons{};
    std::array<SpanSetupY,Polygons*2> edges{};
    std::array<SetupIndices,Spans> indices{};
    MetaUniform meta{};
    Inputs() {
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
            auto& out=polygons[p]; out.FirstXSpan=p*Lines;
            out.YTop=0;out.YBot=Lines;out.XMin=256;out.XMax=-1;
            out.Attr=((p%2?15u:31u)<<16)|(3<<6);out.Variant=0;
            SetupYSpan(&out,&edges[p*2],&polygon,0,3,0,positions);
            SetupYSpan(&out,&edges[p*2+1],&polygon,1,2,1,positions);
            for (unsigned y=0;y<Lines;++y) indices[p*Lines+y]={u16(p),u16(p*2),u16(p*2+1),u16(y)};
        }
    }
};

static std::vector<SpanSetupX> GLSpans(const Inputs& input, unsigned variant)
{
    const auto source=ComputeShader::BuildSource(variant,Config,false);
    const char* text=source.c_str();
    GLuint shader=glCreateShader(GL_COMPUTE_SHADER);glShaderSource(shader,1,&text,nullptr);glCompileShader(shader);
    GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if (!ok) { char log[4096];glGetShaderInfoLog(shader,sizeof(log),nullptr,log);throw std::runtime_error(log); }
    GLuint program=glCreateProgram();glAttachShader(program,shader);glLinkProgram(program);glDeleteShader(shader);
    glGetProgramiv(program,GL_LINK_STATUS,&ok);if(!ok)throw std::runtime_error("GL link failed");
    std::vector<SpanSetupX> output(Spans+1);std::memset(output.data(),0xCD,output.size()*sizeof(SpanSetupX));
    GLuint buffers[5],texture;glGenBuffers(5,buffers);glGenTextures(1,&texture);
    const void* data[]={input.polygons.data(),output.data(),input.edges.data(),&input.meta,input.indices.data()};
    const size_t sizes[]={sizeof(input.polygons),output.size()*sizeof(SpanSetupX),sizeof(input.edges),sizeof(input.meta),sizeof(input.indices)};
    for(unsigned i=0;i<5;++i) {
        const GLenum target=i<3?GL_SHADER_STORAGE_BUFFER:i==3?GL_UNIFORM_BUFFER:GL_TEXTURE_BUFFER;
        glBindBuffer(target,buffers[i]);glBufferData(target,sizes[i],data[i],GL_DYNAMIC_DRAW);
        if(i<4)glBindBufferBase(target,i<3?i:0,buffers[i]);
    }
    glBindTexture(GL_TEXTURE_BUFFER,texture);glTexBuffer(GL_TEXTURE_BUFFER,GL_RGBA16UI,buffers[4]);
    glBindImageTexture(0,texture,0,GL_FALSE,0,GL_READ_ONLY,GL_RGBA16UI);
    glUseProgram(program);glDispatchCompute(Spans/32,1,1);
    glMemoryBarrier(GL_BUFFER_UPDATE_BARRIER_BIT);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER,buffers[1]);glGetBufferSubData(GL_SHADER_STORAGE_BUFFER,0,sizes[1],output.data());
    glDeleteTextures(1,&texture);glDeleteBuffers(5,buffers);glDeleteProgram(program);
    if(glGetError()!=GL_NO_ERROR)throw std::runtime_error("GL span execution failed");
    return output;
}

struct VulkanSpans {
    QVulkanInstance instance;
    QVulkanFunctions* f=nullptr; QVulkanDeviceFunctions* d=nullptr;
    VkDevice device{};VkQueue queue{};VkPhysicalDevice physical{};
    VkCommandPool pool{};VkDescriptorPool descriptors{};VkPipelineLayout layout{};
    std::array<VkDescriptorSetLayout,4> sets{};
    struct Buffer { VkBuffer buffer{};VkDeviceMemory memory{};void* data{}; };
    std::array<Buffer,5> buffers{};
    VkBufferView indices{};
    ~VulkanSpans() {
        if(!d)return;
        d->vkDeviceWaitIdle(device);
        if(indices)d->vkDestroyBufferView(device,indices,nullptr);
        for(auto& b:buffers) {
            if(b.data)d->vkUnmapMemory(device,b.memory);
            if(b.buffer)d->vkDestroyBuffer(device,b.buffer,nullptr);
            if(b.memory)d->vkFreeMemory(device,b.memory,nullptr);
        }
        if(pool)d->vkDestroyCommandPool(device,pool,nullptr);
        if(descriptors)d->vkDestroyDescriptorPool(device,descriptors,nullptr);
        if(layout)d->vkDestroyPipelineLayout(device,layout,nullptr);
        for(auto set:sets)if(set)d->vkDestroyDescriptorSetLayout(device,set,nullptr);
        d->vkDestroyDevice(device,nullptr);instance.resetDeviceFunctions(device);
    }
    bool Init() {
        instance.setApiVersion(QVersionNumber(1,1));if(!instance.create())return false;
        f=instance.functions();uint32_t n=0;Check(f->vkEnumeratePhysicalDevices(instance.vkInstance(),&n,nullptr));
        std::vector<VkPhysicalDevice> devices(n);Check(f->vkEnumeratePhysicalDevices(instance.vkInstance(),&n,devices.data()));
        uint32_t family=0;VkPhysicalDeviceFeatures features{};
        for(auto candidate:devices) {
            f->vkGetPhysicalDeviceFeatures(candidate,&features);if(!features.shaderStorageImageExtendedFormats)continue;
            f->vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,nullptr);std::vector<VkQueueFamilyProperties> queues(n);
            f->vkGetPhysicalDeviceQueueFamilyProperties(candidate,&n,queues.data());
            for(uint32_t i=0;i<n;++i)if(queues[i].queueFlags&VK_QUEUE_COMPUTE_BIT){physical=candidate;family=i;break;}
            if(physical)break;
        }
        if(!physical)return false;
        features={};features.shaderStorageImageExtendedFormats=VK_TRUE;
        float priority=1;VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        queueInfo.queueFamilyIndex=family;queueInfo.queueCount=1;queueInfo.pQueuePriorities=&priority;
        VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};deviceInfo.queueCreateInfoCount=1;
        deviceInfo.pQueueCreateInfos=&queueInfo;deviceInfo.pEnabledFeatures=&features;
        Check(f->vkCreateDevice(physical,&deviceInfo,nullptr,&device));d=instance.deviceFunctions(device);
        if(!d)throw std::runtime_error("Qt Vulkan functions unavailable");
        d->vkGetDeviceQueue(device,family,0,&queue);
        VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};poolInfo.queueFamilyIndex=family;
        Check(d->vkCreateCommandPool(device,&poolInfo,nullptr,&pool));
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
        auto& b=buffers[index];
        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};info.size=size;
        info.usage=index<3?VK_BUFFER_USAGE_STORAGE_BUFFER_BIT:index==3?VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT:VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT;
        Check(d->vkCreateBuffer(device,&info,nullptr,&b.buffer));
        VkMemoryRequirements req{};d->vkGetBufferMemoryRequirements(device,b.buffer,&req);
        VkPhysicalDeviceMemoryProperties memory{};f->vkGetPhysicalDeviceMemoryProperties(physical,&memory);unsigned type=0;
        for(;type<memory.memoryTypeCount;++type)if((req.memoryTypeBits&(1u<<type))&&
            (memory.memoryTypes[type].propertyFlags&(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))==
            (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))break;
        if(type==memory.memoryTypeCount)throw std::runtime_error("Coherent test memory unavailable");
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};alloc.allocationSize=req.size;alloc.memoryTypeIndex=type;
        Check(d->vkAllocateMemory(device,&alloc,nullptr,&b.memory));Check(d->vkBindBufferMemory(device,b.buffer,b.memory,0));
        Check(d->vkMapMemory(device,b.memory,0,size,0,&b.data));std::memcpy(b.data,data,size);
    }
    std::vector<SpanSetupX> Run(const Inputs& input,const QString& path) {
        std::vector<SpanSetupX> output(Spans+1);std::memset(output.data(),0xCD,output.size()*sizeof(SpanSetupX));
        const size_t sizes[]={sizeof(input.polygons),output.size()*sizeof(SpanSetupX),sizeof(input.edges),sizeof(input.meta),sizeof(input.indices)};
        const void* data[]={input.polygons.data(),output.data(),input.edges.data(),&input.meta,input.indices.data()};
        for(unsigned i=0;i<5;++i)Allocate(i,sizes[i],data[i]);
        VkBufferViewCreateInfo view{VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO};view.buffer=buffers[4].buffer;view.format=VK_FORMAT_R16G16B16A16_UINT;view.range=VK_WHOLE_SIZE;
        Check(d->vkCreateBufferView(device,&view,nullptr,&indices));
        VkDescriptorSet descriptorSets[4];VkDescriptorSetAllocateInfo allocation{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        allocation.descriptorPool=descriptors;allocation.descriptorSetCount=4;allocation.pSetLayouts=sets.data();
        Check(d->vkAllocateDescriptorSets(device,&allocation,descriptorSets));
        VkDescriptorBufferInfo infos[4]{};VkWriteDescriptorSet writes[5]{};
        for(unsigned i=0;i<5;++i) {
            auto& w=writes[i];w.sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;w.dstSet=descriptorSets[i<3?0:i==3?1:3];
            w.dstBinding=i<3?i:0;w.descriptorCount=1;w.descriptorType=i<3?VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:i==3?VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER;
            if(i<4){infos[i]={buffers[i].buffer,0,sizes[i]};w.pBufferInfo=&infos[i];}else w.pTexelBufferView=&indices;
        }
        d->vkUpdateDescriptorSets(device,5,writes,0,nullptr);
        QFile file(path);if(!file.open(QIODevice::ReadOnly))throw std::runtime_error("Missing SPIR-V");const auto bytes=file.readAll();
        std::vector<uint32_t> words(bytes.size()/4);if(bytes.isEmpty()||bytes.size()%4)throw std::runtime_error("Invalid SPIR-V size");std::memcpy(words.data(),bytes.data(),bytes.size());
        VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};moduleInfo.codeSize=bytes.size();moduleInfo.pCode=words.data();VkShaderModule module{};
        Check(d->vkCreateShaderModule(device,&moduleInfo,nullptr,&module));
        VkComputePipelineCreateInfo pipelineInfo{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};pipelineInfo.layout=layout;
        pipelineInfo.stage={VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,nullptr,0,VK_SHADER_STAGE_COMPUTE_BIT,module,"main",nullptr};VkPipeline pipeline{};
        const auto pipelineResult=d->vkCreateComputePipelines(device,VK_NULL_HANDLE,1,&pipelineInfo,nullptr,&pipeline);d->vkDestroyShaderModule(device,module,nullptr);Check(pipelineResult);
        VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};commandInfo.commandPool=pool;commandInfo.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY;commandInfo.commandBufferCount=1;VkCommandBuffer command{};
        Check(d->vkAllocateCommandBuffers(device,&commandInfo,&command));VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};Check(d->vkBeginCommandBuffer(command,&begin));
        d->vkCmdBindPipeline(command,VK_PIPELINE_BIND_POINT_COMPUTE,pipeline);d->vkCmdBindDescriptorSets(command,VK_PIPELINE_BIND_POINT_COMPUTE,layout,0,4,descriptorSets,0,nullptr);
        d->vkCmdDispatch(command,Spans/32,1,1);VkMemoryBarrier barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER};barrier.srcAccessMask=VK_ACCESS_SHADER_WRITE_BIT;barrier.dstAccessMask=VK_ACCESS_HOST_READ_BIT;
        d->vkCmdPipelineBarrier(command,VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,VK_PIPELINE_STAGE_HOST_BIT,0,1,&barrier,0,nullptr,0,nullptr);Check(d->vkEndCommandBuffer(command));
        VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};VkFence fence{};Check(d->vkCreateFence(device,&fenceInfo,nullptr,&fence));
        VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};submit.commandBufferCount=1;submit.pCommandBuffers=&command;
        Check(d->vkQueueSubmit(queue,1,&submit,fence));Check(d->vkWaitForFences(device,1,&fence,VK_TRUE,5000000000ull));
        std::memcpy(output.data(),buffers[1].data,sizes[1]);d->vkDestroyFence(device,fence,nullptr);d->vkDestroyPipeline(device,pipeline,nullptr);
        return output;
    }
};

int main(int argc,char** argv)
{
    QGuiApplication app(argc,argv);if(argc!=2)return 1;
    QSurfaceFormat format;format.setVersion(4,3);format.setProfile(QSurfaceFormat::CoreProfile);
    QOpenGLContext context;context.setFormat(format);if(!context.create())return 77;
    QOffscreenSurface surface;surface.setFormat(context.format());surface.create();if(!context.makeCurrent(&surface))return 77;
    if(!gladLoadGLLoader([](const char* name)->void*{return reinterpret_cast<void*>(QOpenGLContext::currentContext()->getProcAddress(name));}))return 77;
    try {
        const Inputs input;
        for(unsigned variant=0;variant<2;++variant) {
            const auto expected=GLSpans(input,variant);
            VulkanSpans vk;if(!vk.Init())return 77;
            const auto actual=vk.Run(input,QString::fromLocal8Bit(argv[1])+QString("/%1.spv").arg(variant));
            if(std::memcmp(expected.data(),actual.data(),actual.size()*sizeof(SpanSetupX))) {
                for(unsigned i=0;i<actual.size();++i)if(std::memcmp(&expected[i],&actual[i],sizeof(SpanSetupX))){std::fprintf(stderr,"Span mismatch variant=%u index=%u\n",variant,i);break;}
                return 2;
            }
            const unsigned char* guard=reinterpret_cast<const unsigned char*>(&actual.back());
            for(unsigned i=0;i<sizeof(SpanSetupX);++i)if(guard[i]!=0xCD)return 3;
            std::printf("Vulkan/GL %s: %u spans, all 24 words and output guard equal PASS\n",variant?"W":"Z",Spans);
        }
    }catch(const std::exception& error){std::fprintf(stderr,"%s\n",error.what());return 4;}
    return 0;
}
