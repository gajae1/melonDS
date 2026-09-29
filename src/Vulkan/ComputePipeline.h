// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "Device.h"
#include "ComputeResources.h"
#include "TextureDecode.h"
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
    using Shaders=std::array<std::span<const uint32_t>,ComputeShader::VulkanCount>;
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
    // RenderView split around the fence, for results first consumed later.
    // SubmitView queues the frame and keeps its textures alive, so the batches
    // may be released; CompleteView waits and returns what RenderView returns.
    // Nothing may use this pipeline or its Device in between.
    void SubmitView(std::span<const Batch> batches, Readback mode=Readback::Full);
    std::span<const uint32_t> CompleteView();
    bool ViewPending() const { return viewPending; }
    // Optional origin extraction shares the render submission. Failure to enable
    // it leaves the existing full-readback graph usable.
    void EnableNativeReadback(std::span<const uint32_t> shader);
    std::span<const uint32_t> ReadbackView();
    // Read native pixel origins from this retained image, which may differ from
    // OutputImage() after a separate high-resolution capture display render.
    // The returned staging view lasts until the next native readback/render.
    std::span<const uint32_t> ReadNativeView(const std::shared_ptr<Device::Image>& image);
    const std::shared_ptr<Device::Image>& OutputImage() const { return output; }
    // Opt-in retained outputs for GPU consumers. While enabled, every RenderView
    // first selects an output referenced only by this pipeline, so holding a copy
    // of OutputImage() keeps that image immutable until the copy is released.
    // Ownership is rendering-thread shared_ptr use; outputs stay in GENERAL layout
    // and are reused, created lazily up to MaxRetainedOutputs, after which
    // RenderView throws logic_error. Disabled (default): one output rewritten in place.
    static constexpr size_t MaxRetainedOutputs=4;
    void SetOutputRetention(bool enabled);
    std::shared_ptr<const Texture> UploadTexture(uint32_t width, uint32_t height,
        uint32_t layers, std::span<const uint32_t> pixels, bool capture=false);
    std::shared_ptr<const Texture> CreateTexture(uint32_t width, uint32_t height, uint32_t layers);
    std::shared_ptr<const Texture> CreateCaptureTexture(uint32_t texParam, uint32_t bank, uint32_t scale,
        const std::shared_ptr<Device::Buffer>& hires);
    void UploadTextureLayer(const Texture& texture, uint32_t layer, std::span<const uint32_t> pixels);
    // Source words were produced on-device (for example by TextureDecode).
    // Retain them through the upload fence without copying through host staging.
    void UploadTextureLayerFromBuffer(const Texture& texture, uint32_t layer,
        const std::shared_ptr<Device::Buffer>& source, uint32_t firstWord);
    // Decode and upload share the next existing upload/render submission.
    void QueueTextureDecode(const Texture& texture, uint32_t layer, uint32_t texParam, uint32_t palBase,
        const std::shared_ptr<Device::Buffer>& textures, const std::shared_ptr<Device::Buffer>& palettes,
        const std::shared_ptr<Device::Buffer>& captured);
    void QueueClearBitmapDecode(const std::shared_ptr<Device::Buffer>& textures,
        const std::shared_ptr<Device::Buffer>& palettes, const std::shared_ptr<Device::Buffer>& captured);
    void UploadClearBitmap(std::span<const uint32_t> colors, std::span<const uint32_t> depths);
    // Opt-in: snapshot upload bytes now, submit together at FlushUploads or
    // RenderView. No command buffer is left recording between upload calls.
    // The default remains synchronous for other pipeline users.
    void SetUploadBatching(bool enabled);
    // Keep the ordinary raster path available for comparisons and fallback.
    void SetFusedRaster(bool enabled) { fusedRaster = enabled; }
    void FlushUploads();
    uint32_t WorkCapacity() const { return Resources.BatchWork; }
    uint32_t SpanCapacity() const { return Resources.MaxSpans; }
    uint32_t TileSize() const { return Resources.config.TileSize; }
private:
    void Init(const Shaders& shaders);
    void Cleanup();
    void CleanupNativeReadback();
    std::shared_ptr<Device::Image> CreateOutput() const;
    void SelectOutput();
    void BindOutput();
    void BindNativeImage(const Device::Image& image);
    void RecordNativeReadback(VkCommandBuffer command);
    std::span<const uint32_t> NativeReadbackPixels();
    void PrepareFullReadback();
    void RecordFullReadback(VkCommandBuffer command);
    std::span<const uint32_t> FullReadbackPixels() const;
    bool viewPending=false;
    bool fusedRaster=true;
    Readback pendingMode=Readback::None;
    std::vector<std::shared_ptr<const Texture>> pendingTextures;
    void Bind(VkCommandBuffer command,unsigned shader,VkDescriptorSet storage,VkDescriptorSet image,
        VkDescriptorSet textures=VK_NULL_HANDLE);
    void Validate(const Batch& batch) const;
    void RecordBatch(VkCommandBuffer command,const Batch& batch,bool first,bool final,std::span<const VkDescriptorSet> textures);
    void WriteTextureSet(VkDescriptorSet set,const Variant& variant);
    void UploadImage(const std::shared_ptr<Device::Image>& image,uint32_t width,uint32_t height,
        uint32_t layers,std::span<const uint32_t> pixels,VkImageLayout oldLayout,uint32_t firstLayer=0);
    struct ImageUpload {
        std::shared_ptr<Device::Image> image;
        uint32_t width,height,layers,firstLayer;
        VkImageLayout oldLayout;
        VkDeviceSize offset;
        bool clear;
        std::shared_ptr<Device::Buffer> source{};
        uint32_t decodeJob = ~0u;
    };
    void ReserveUpload(VkDeviceSize bytes,size_t images);
    void SubmitUploadsIfNeeded();
    void PrepareUploads();
    void ReserveDecode(VkDeviceSize bytes, size_t images,
        const std::array<std::shared_ptr<Device::Buffer>,3>& inputs);
    void RecordUploads(VkCommandBuffer command);
    void CompleteUploads();
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
    std::array<VkPipeline,ComputeShader::VulkanCount> pipelines{};
    VkDescriptorSet setupSet{},rasterSet{},metaSet{},textureSet{},indicesSet{},outputSet{};
    // polygon, X span, Y span, color/depth/attributes, result, bin, work, meta, indices
    std::array<std::shared_ptr<Device::Buffer>,11> buffers;
    std::shared_ptr<Device::Buffer> readback;
    // Cached landing memory is consumed directly after the existing fence.
    // Keep this CPU allocation for the unchanged uncached-landing memcpy path.
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
    std::unique_ptr<TextureDecode> textureDecode;
    std::vector<TextureDecode::Job> decodeJobs;
    std::array<std::shared_ptr<Device::Buffer>,3> decodeInputs;
    VkDeviceSize decodeBytes=0;
    bool decodePrepared=false;
    std::shared_ptr<Device::Image> output,clearColor,clearDepth;
    // Retention pool; includes output. Empty while retention is disabled.
    std::vector<std::shared_ptr<Device::Image>> outputs;
    bool retainOutputs=false;
    std::shared_ptr<const Texture> dummyTexture,dummyCapture;
    VkBufferView indicesView{};
    VkSampler sampler{};
    std::array<VkSampler,9> textureSamplers{};
    bool clearBitmapReady=false;
};
}
