// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Device.h"
#include "ComputeResources.h"
#include "GPU3D_ComputeData.h"
#include "GPU3D_ComputeShader.h"
#include <array>
#include <span>
#include <vector>

namespace melonDS::Vulkan {
// Scalable compute graph. Prepared polygons and decoded texture arrays
// use the same integer formats as the GL compute renderer.
class ComputePipeline {
public:
    using Shaders=std::array<std::span<const uint32_t>,32>;
    struct Texture {
        std::shared_ptr<Device::Image> image;
        uint32_t width, height, layers;
        bool capture;
    };
    struct Variant {
        uint32_t shader;
        std::shared_ptr<const Texture> texture;
        // 0 clamp, 1 repeat, 2 mirrored repeat, as in the DS texture parameters.
        uint32_t wrapU=0, wrapV=0;
        float captureYOffset=0;
        // Capture images retain scale*scale samples per native DS texel.
        uint32_t captureScale=1;
    };
    struct Batch {
        std::span<const ComputeData::RenderPolygon> polygons;
        std::span<const ComputeData::SpanSetupY> edges;
        std::span<const ComputeData::SetupIndices> indices;
        std::span<const Variant> variants;
        ComputeData::MetaUniform meta;
        bool wbuffer=false;
    };
    explicit ComputePipeline(std::shared_ptr<Device> device,const Shaders& shaders,int scale=1);
    ~ComputePipeline();
    ComputePipeline(const ComputePipeline&)=delete;
    ComputePipeline& operator=(const ComputePipeline&)=delete;
    std::vector<uint32_t> Render(const Batch& batch);
    std::vector<uint32_t> Render(std::span<const Batch> batches);
    // Completed readback in cached CPU memory. Valid until the next render attempt
    // or pipeline destruction; consume before submitting another frame.
    // Call Render when an independently owned snapshot is needed instead.
    enum class Readback { Full, Native, None };
    std::span<const uint32_t> RenderView(std::span<const Batch> batches, Readback mode=Readback::Full);
    // Optional origin extraction shares the render submission. Failure to enable
    // it leaves the existing full-readback graph usable.
    void EnableNativeReadback(std::span<const uint32_t> shader);
    std::span<const uint32_t> ReadbackView();
    const std::shared_ptr<Device::Image>& OutputImage() const { return output; }
    std::shared_ptr<const Texture> UploadTexture(uint32_t width, uint32_t height,
        uint32_t layers, std::span<const uint32_t> pixels, bool capture=false);
    std::shared_ptr<const Texture> CreateTexture(uint32_t width, uint32_t height, uint32_t layers);
    void UploadTextureLayer(const Texture& texture, uint32_t layer, std::span<const uint32_t> pixels);
    void UploadClearBitmap(std::span<const uint32_t> colors, std::span<const uint32_t> depths);
    // Opt-in: snapshot upload bytes now, submit together at FlushUploads or
    // RenderView. No command buffer is left recording between upload calls.
    // The default remains synchronous for other pipeline users.
    void SetUploadBatching(bool enabled);
    void FlushUploads();
    uint32_t WorkCapacity() const { return Resources.BatchWork; }
    uint32_t SpanCapacity() const { return Resources.MaxSpans; }
    uint32_t TileSize() const { return Resources.config.TileSize; }
private:
    void Init(const Shaders& shaders);
    void Cleanup();
    void CleanupNativeReadback();
    void RecordFullReadback(VkCommandBuffer command);
    void Bind(VkCommandBuffer command,unsigned shader,VkDescriptorSet storage,VkDescriptorSet image,
        VkDescriptorSet textures=VK_NULL_HANDLE);
    void Validate(const Batch& batch) const;
    void RecordBatch(VkCommandBuffer command,const Batch& batch,bool first,std::span<const VkDescriptorSet> textures);
    void WriteTextureSet(VkDescriptorSet set,const Variant& variant);
    void UploadImage(const std::shared_ptr<Device::Image>& image,uint32_t width,uint32_t height,
        uint32_t layers,std::span<const uint32_t> pixels,VkImageLayout oldLayout,uint32_t firstLayer=0);
    struct ImageUpload {
        std::shared_ptr<Device::Image> image;
        uint32_t width,height,layers,firstLayer;
        VkImageLayout oldLayout;
        VkDeviceSize offset;
        bool clear;
    };
    void ReserveUpload(VkDeviceSize bytes,size_t images);
    void SubmitUploadsIfNeeded();
    void RecordImageUpload(VkCommandBuffer command,const ImageUpload& upload);
    void Barrier(VkCommandBuffer command);
    std::shared_ptr<Device> owner;
    const volk::VolkDeviceTable& f;
    VkDevice device;
    const ComputeResources Resources;
    VkDescriptorPool pool{};
    VkDescriptorPool texturePool{};
    VkPipelineLayout layout{};
    std::array<VkDescriptorSetLayout,4> setLayouts{};
    std::array<VkPipeline,32> pipelines{};
    VkDescriptorSet setupSet{},rasterSet{},metaSet{},textureSet{},indicesSet{},outputSet{};
    // polygon, X span, Y span, color/depth/attributes, result, bin, work, meta, indices
    std::array<std::shared_ptr<Device::Buffer>,11> buffers;
    std::shared_ptr<Device::Buffer> readback;
    // Mapped host-coherent memory need not be CPU-cached. Copy once with memcpy,
    // then let color conversion/native sampling read this reusable allocation.
    std::vector<uint32_t> hostReadback;
    bool fullReadbackValid=false;
    VkPipeline nativePipeline{};
    VkPipelineLayout nativeLayout{};
    VkDescriptorSetLayout nativeBindings{};
    VkDescriptorPool nativePool{};
    VkDescriptorSet nativeSet{};
    std::shared_ptr<Device::Buffer> nativeReadback;
    std::vector<uint32_t> nativeHostReadback;
    // Disjoint coherent ranges and retained images survive through the existing
    // submission fence. Growth is safe before recording; reuse requires a wait.
    // Bound each chunk, except for one individually larger upload.
    static constexpr VkDeviceSize UploadBatchBytes=16u*1024u*1024u;
    static constexpr size_t UploadBatchImages=256;
    std::shared_ptr<Device::Buffer> uploadStaging;
    std::vector<ImageUpload> pendingUploads;
    VkDeviceSize uploadUsed=0;
    bool deferredUploads=false;
    bool clearBitmapPending=false;
    std::shared_ptr<Device::Image> output,clearColor,clearDepth;
    std::shared_ptr<const Texture> dummyTexture,dummyCapture;
    VkBufferView indicesView{};
    VkSampler sampler{};
    std::array<VkSampler,9> textureSamplers{};
    bool clearBitmapReady=false;
};
}
