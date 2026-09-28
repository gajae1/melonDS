// SPDX-License-Identifier: GPL-3.0-or-later
#include "TextureCache.h"
#include "GPU_Vulkan.h"
#include "Native2DCapture.h"
#include <algorithm>
#include <cstring>
#include <stdexcept>

namespace melonDS::Vulkan {
struct TextureLoader::Source {
    explicit Source(GPU& gpu) : gpu(gpu) {}
    GPU& gpu;
    std::array<u32,2048> masks{};
    // CPU contribution followed by one GPU-bank mask per 256 logical bytes.
    std::vector<u32> words;
    std::shared_ptr<Device::Buffer> texture, palette;
    u64 revision=0,generation=0;
    std::array<u64,4> bankRevisions{};
    bool active=false,hasCaptured=false,refresh=true;
};
TextureLoader::TextureLoader(ComputePipeline& pipeline,VulkanRenderer& parent,GPU& gpu)
    : Pipeline(pipeline),source(std::make_shared<Source>(gpu)),parent(parent) {}

bool TextureLoader::BeginTextureUpdate(u64& generation)
{
    auto& s=*source;
    s.active=bool(parent.NativePipeline);
    std::array<u32,2048> masks{};
    u64 revision=0;
    if(s.active) {
        // Normally DrawFrame already finished these consumers. Keep the hook
        // valid when the shared cache is explicitly updated by another caller.
        parent.FinishNative2D();
        if(parent.HasRenderFailure())throw std::runtime_error("Captured texture dependency failed");
        for(u32 segment=0;segment<masks.size();++segment)
            for(u32 bank=0;bank<4;++bank)
                if((s.gpu.VRAMMap_Texture[segment/512]&(1u<<bank)) && parent.NativeCaptureDirty[bank][segment%512])
                    masks[segment]|=1u<<bank;
        if(std::any_of(masks.begin(),masks.end(),[](u32 value){return value!=0;}))
            revision=parent.NativeCapture->Revision();
    }
    // The assembled words encode only the CPU baseline and the per-segment bank
    // masks. Captured bank bytes are bound per decode from a fresh snapshot, so
    // a capture revision bump only re-stamps TextureSource/generation; it does
    // not change the assembled bytes. Rebuilding only on a mask change keeps a
    // capture-updated frame from re-materializing the whole 512KiB baseline.
    const bool masksChanged=masks!=s.masks;
    if(masksChanged) { s.masks=masks; s.refresh=true; }
    if(masksChanged || revision!=s.revision) { s.revision=revision; ++s.generation; }
    s.hasCaptured=revision!=0;
    if(s.hasCaptured)s.bankRevisions=parent.NativeCapture->BankRevisions();
    if(!s.hasCaptured)s.texture.reset();
    // No GPU bank snapshot is retained here. Queued decodes own it only until
    // the upload/render fence, avoiding unnecessary COW on the next capture.
    // The palette snapshot is immutable and stays valid while its coherent
    // bytes are unchanged, so it survives updates without captures (a bank
    // recapture maps it LCDC for a frame). TextureBytes drops it only on a
    // real palette change or when the native pipeline goes away, and drops
    // the assembled texture only when its bytes actually change.
    generation=s.generation;
    return s.active;
}
const u8* TextureLoader::TextureBytes(bool cpuTextureChanged,bool texPalChanged)
{
    auto& s=*source;
    // Checked before any early return: a coherent-byte change must retire the
    // snapshot even in an update with no captured decode, and losing the
    // native pipeline must not retain it. Queued decodes still holding the
    // previous buffer keep it alive through shared ownership.
    if(texPalChanged||!s.active)s.palette.reset();
    if(!s.hasCaptured)return s.gpu.VRAMFlat_Texture;
    if(s.refresh || cpuTextureChanged || s.words.empty()) {
        s.words.resize(131072+2048);
        std::memcpy(s.words.data(),s.gpu.VRAMFlat_Texture,524288);
        std::copy(s.masks.begin(),s.masks.end(),s.words.begin()+131072);
        for(u32 segment=0;segment<s.masks.size();++segment)if(s.masks[segment]) {
            auto* out=s.words.data()+segment*64;
            std::fill_n(out,64,0u);
            const u32 cpuBanks=s.gpu.VRAMMap_Texture[segment/512]&~s.masks[segment];
            for(u32 bank=0;bank<4;++bank)if(cpuBanks&(1u<<bank)) {
                const auto* bytes=s.gpu.VRAM[bank]+(segment%512)*256;
                for(u32 word=0;word<64;++word) {
                    u32 value;std::memcpy(&value,bytes+word*4,4);out[word]|=value;
                }
            }
        }
        // The device copy read by decodes must match the rebuilt bytes.
        s.texture.reset();
        s.refresh=false;
    }
    return reinterpret_cast<const u8*>(s.words.data());
}
u64 TextureLoader::TextureSource(u32 start,u32 size)
{
    auto& s=*source;
    if(!s.hasCaptured || !size)return 0;
    // Ownership remains part of the range stamp, but unrelated physical bank
    // writes must not evict an otherwise unchanged decoded texture/clear plane.
    u64 hash=0;
    u32 banks=0;
    u32 segment=(start&0x7FFFF)/256;
    u64 count=(u64(start&255)+size+255)/256;
    while(count) {
        const u32 length=u32(std::min<u64>(count,2048-segment));
        for(u32 i=0;i<length;++i)banks|=s.masks[segment+i];
        hash=XXH64(s.masks.data()+segment,size_t(length)*4,hash);
        count-=length;segment=0;
    }
    if(!banks)return 0;
    for(u32 bank=0;bank<4;++bank)if(banks&(1u<<bank))
        hash=XXH64(&s.bankRevisions[bank],sizeof(u64),hash);
    return hash|1;
}
void TextureLoader::PrepareInputs()
{
    auto& s=*source;
    if(!s.hasCaptured || s.words.empty())throw std::logic_error("Missing captured texture source");
    auto device=parent.DisplayDevice();
    if(!s.texture) {
        auto input=device->CreateBuffer(s.words.size()*4,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true);
        std::memcpy(input->Data(),s.words.data(),s.words.size()*4);
        s.texture=std::move(input);
    }
    if(!s.palette) {
        auto palette=device->CreateBuffer(131072,VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,true);
        std::memcpy(palette->Data(),s.gpu.VRAMFlat_TexPal,131072);
        s.palette=std::move(palette);
    }
}
void TextureLoader::DecodeTexture(const TextureHandle& texture,u32 layer,u32 texParam,u32 palBase)
{
    PrepareInputs();
    Pipeline.QueueTextureDecode(*texture,layer,texParam,palBase,source->texture,source->palette,parent.NativeCapture->Snapshot());
}
bool TextureLoader::DecodeClearBitmap()
{
    if(!TextureSource(0x40000,0x40000))return false;
    PrepareInputs();
    Pipeline.QueueClearBitmapDecode(source->texture,source->palette,parent.NativeCapture->Snapshot());
    return true;
}
}
