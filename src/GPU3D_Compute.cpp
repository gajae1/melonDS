/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include "GPU_OpenGL.h"

#include <assert.h>
#include <algorithm>

#include "Utils.h"

#include "OpenGLSupport.h"

#include "GPU3D_ComputeShader.h"

namespace melonDS
{
using ComputeData::SetupYSpan;
using ComputeData::SetupYSpanDummy;

ComputeRenderer3D::ComputeRenderer3D(melonDS::GPU3D& gpu3D, GLRenderer& parent)
    : Renderer3D(gpu3D), Parent(parent), Texcache(gpu3D.GPU, TexcacheOpenGLLoader(true))
{
    ScaleFactor = 0;
    HiresCoordinates = false;
}

bool ComputeRenderer3D::ShaderCompileStep(int& current, int& count)
{
    current = ShaderStepIdx;
    count = ComputeShader::Count;
    if (ShaderCompileFailed) return false;
    if (ShaderStepIdx == count) return true;
    GLuint* programs[] = {
        &ShaderInterpXSpans[0],
        &ShaderInterpXSpans[1],
        &ShaderBinCombined,
        &ShaderDepthBlend[0],
        &ShaderDepthBlend[1],
        &ShaderRasteriseNoTexture[0],
        &ShaderRasteriseNoTexture[1],
        &ShaderRasteriseNoTextureToon[0],
        &ShaderRasteriseNoTextureToon[1],
        &ShaderRasteriseNoTextureHighlight[0],
        &ShaderRasteriseNoTextureHighlight[1],
        &ShaderRasteriseUseTextureDecal[0],
        &ShaderRasteriseUseTextureDecal[1],
        &ShaderRasteriseUseTextureModulate[0],
        &ShaderRasteriseUseTextureModulate[1],
        &ShaderRasteriseUseTextureToon[0],
        &ShaderRasteriseUseTextureToon[1],
        &ShaderRasteriseUseTextureHighlight[0],
        &ShaderRasteriseUseTextureHighlight[1],
        &ShaderRasteriseShadowMask[0],
        &ShaderRasteriseShadowMask[1],
        &ShaderClearCoarseBinMask,
        &ShaderCalculateWorkListOffset,
        &ShaderSortWork,
        &ShaderFinalPass[0],
        &ShaderFinalPass[1],
        &ShaderFinalPass[2],
        &ShaderFinalPass[3],
        &ShaderFinalPass[4],
        &ShaderFinalPass[5],
        &ShaderFinalPass[6],
        &ShaderFinalPass[7],
};
    static_assert(std::size(programs) == ComputeShader::Count);
    const ComputeShader::Config config{ScreenWidth, ScreenHeight, MaxWorkTiles, TileSize, CoarseTileCountY, CoarseTileArea, ClearCoarseBinMaskLocalSize};
    const auto source = ComputeShader::BuildSource(ShaderStepIdx, config, false);
    const auto name = "Compute variant " + std::to_string(ShaderStepIdx);
    if (!OpenGL::CompileComputeProgram(*programs[ShaderStepIdx++], source.c_str(), name.c_str()))
    {
        ShaderCompileFailed = true;
        return false;
    }
    return true;
}

void blah(GLenum source, GLenum type, GLuint id, GLenum severity, GLsizei length, const GLchar *message, const void *userParam)
{
    printf("%s\n", message);
}

bool ComputeRenderer3D::Init()
{
    if (!OpenGL::SupportsCompute())
    {
        Platform::Log(Platform::LogLevel::Error, "Compute renderer requires OpenGL 4.3 and loaded compute functions\n");
        return false;
    }

    //glDebugMessageCallback(blah, NULL);
    //glEnable(GL_DEBUG_OUTPUT);
    glGenBuffers(1, &YSpanSetupMemory);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, YSpanSetupMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(SpanSetupY)*MaxYSpanSetups, nullptr, GL_DYNAMIC_DRAW);
    
    glGenBuffers(1, &RenderPolygonMemory);
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, RenderPolygonMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(RenderPolygon)*2048, nullptr, GL_DYNAMIC_DRAW);

    glGenBuffers(1, &XSpanSetupMemory);
    glGenBuffers(1, &BinResultMemory);
    glGenBuffers(1, &FinalTileMemory);
    glGenBuffers(1, &YSpanIndicesTextureMemory);
    glGenBuffers(tilememoryLayer_Num, TileMemory);
    glGenBuffers(1, &WorkDescMemory);

    glGenTextures(1, &YSpanIndicesTexture);

    glGenBuffers(1, &MetaUniformMemory);
    glBindBuffer(GL_UNIFORM_BUFFER, MetaUniformMemory);
    glBufferData(GL_UNIFORM_BUFFER, sizeof(MetaUniform), nullptr, GL_DYNAMIC_DRAW);

    glGenSamplers(9, Samplers);
    for (u32 j = 0; j < 3; j++)
    {
        for (u32 i = 0; i < 3; i++)
        {
            const GLenum translateWrapMode[3] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT};
            glSamplerParameteri(Samplers[i+j*3], GL_TEXTURE_WRAP_S, translateWrapMode[i]);
            glSamplerParameteri(Samplers[i+j*3], GL_TEXTURE_WRAP_T, translateWrapMode[j]);
            glSamplerParameteri(Samplers[i+j*3], GL_TEXTURE_MIN_FILTER, GL_NEAREST);
            glSamplerParameteri(Samplers[i+j*3], GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        }
    }

    // init textures for the clear bitmap
    glGenTextures(2, ClearBitmapTex);

    glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[0]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, 256, 256, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);

    glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[1]);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32UI, 256, 256, 0, GL_RED_INTEGER, GL_UNSIGNED_INT, nullptr);

    ClearBitmap[0] = new u32[256*256];
    ClearBitmap[1] = new u32[256*256];

    Initialized = true;
    return true;
}

ComputeRenderer3D::~ComputeRenderer3D()
{
    // Init can reject the context before creating any compute resources.
    if (!Initialized) return;
    DeleteShaders();
    Texcache.Reset();

    glDeleteBuffers(1, &YSpanSetupMemory);
    glDeleteBuffers(1, &RenderPolygonMemory);
    glDeleteBuffers(1, &XSpanSetupMemory);
    glDeleteBuffers(1, &BinResultMemory);
    glDeleteBuffers(tilememoryLayer_Num, TileMemory);
    glDeleteBuffers(1, &WorkDescMemory);
    glDeleteBuffers(1, &FinalTileMemory);
    glDeleteBuffers(1, &YSpanIndicesTextureMemory);
    glDeleteTextures(1, &YSpanIndicesTexture);
    glDeleteTextures(1, &Framebuffer);
    glDeleteBuffers(1, &MetaUniformMemory);

    glDeleteSamplers(9, Samplers);

    glDeleteTextures(2, ClearBitmapTex);
    delete[] ClearBitmap[0];
    delete[] ClearBitmap[1];
}

void ComputeRenderer3D::DeleteShaders()
{
    std::initializer_list<GLuint*> allPrograms =
    {
        &ShaderInterpXSpans[0],
        &ShaderInterpXSpans[1],
        &ShaderBinCombined,
        &ShaderDepthBlend[0],
        &ShaderDepthBlend[1],
        &ShaderRasteriseNoTexture[0],
        &ShaderRasteriseNoTexture[1],
        &ShaderRasteriseNoTextureToon[0],
        &ShaderRasteriseNoTextureToon[1],
        &ShaderRasteriseNoTextureHighlight[0],
        &ShaderRasteriseNoTextureHighlight[1],
        &ShaderRasteriseUseTextureDecal[0],
        &ShaderRasteriseUseTextureDecal[1],
        &ShaderRasteriseUseTextureModulate[0],
        &ShaderRasteriseUseTextureModulate[1],
        &ShaderRasteriseUseTextureToon[0],
        &ShaderRasteriseUseTextureToon[1],
        &ShaderRasteriseUseTextureHighlight[0],
        &ShaderRasteriseUseTextureHighlight[1],
        &ShaderRasteriseShadowMask[0],
        &ShaderRasteriseShadowMask[1],
        &ShaderClearCoarseBinMask,
        &ShaderCalculateWorkListOffset,
        &ShaderSortWork,
        &ShaderFinalPass[0],
        &ShaderFinalPass[1],
        &ShaderFinalPass[2],
        &ShaderFinalPass[3],
        &ShaderFinalPass[4],
        &ShaderFinalPass[5],
        &ShaderFinalPass[6],
        &ShaderFinalPass[7],
    };
    for (GLuint* program : allPrograms)
    {
        if (*program) glDeleteProgram(*program);
        *program = 0;
    }
}

void ComputeRenderer3D::Reset()
{
    Texcache.Reset();
    ClearBitmapDirty = 0x3;
    RenderSettingsDirty = true;
}

bool ComputeRenderer3D::CheckScaleFactor(int scale) const
{
    // Parent validates 1..16 before querying us. Use wide arithmetic for byte
    // counts; the texture-buffer limit instead counts RGBA16UI texels (8 bytes).
    const u64 pixels = u64(256) * 192 * scale * scale;
    const u64 indices = u64(64) * 2048 * scale;
    // Tile storage is 4 * TileSize^2 * MaxWorkTiles = 64 * screen pixels.
    // Other scalable SSBOs are smaller than this or the X-span setup buffer.
    const u64 largest = std::max({64 * pixels, sizeof(SpanSetupX) * indices,
        u64(sizeof(SpanSetupY)) * MaxYSpanSetups, u64(sizeof(RenderPolygon)) * 2048});
    GLint64 storageLimit = 0;
    GLint texelLimit = 0;
    glGetInteger64v(GL_MAX_SHADER_STORAGE_BLOCK_SIZE, &storageLimit);
    glGetIntegerv(GL_MAX_TEXTURE_BUFFER_SIZE, &texelLimit);
    if (!OpenGL::CheckError("Compute storage limits")) return false;
    if (storageLimit <= 0 || texelLimit <= 0 || largest > u64(storageLimit) || indices > u64(texelLimit))
    {
        Platform::Log(Platform::LogLevel::Error, "Compute: resolution scale %d exceeds buffer limits\n", scale);
        return false;
    }
    return true;
}

bool ComputeRenderer3D::SetRenderSettings(int scale, bool highResolutionCoordinates)
{
    // Native resolution uses the DS's quantized coordinates, matching the
    // software and classic OpenGL renderers. This switch only affects CPU setup.
    const bool hires = highResolutionCoordinates && scale > 1;
    RenderSettingsDirty |= HiresCoordinates != hires;
    HiresCoordinates = hires;
    if (ScaleFactor == scale)
        return true;

    RenderSettingsDirty = true;
    u8 TileScale;

    if (ScaleFactor != -1)
    {
        DeleteShaders();
    }

    ShaderStepIdx = 0;
    ShaderCompileFailed = false;

    ScaleFactor = scale;
    ScreenWidth = 256 * ScaleFactor;
    ScreenHeight = 192 * ScaleFactor;

    //Starting at 4.5x we want to double TileSize every time scale doubles
    TileScale = 2 * ScaleFactor / 9;
    TileScale = GetMSBit(TileScale);
    TileScale <<= 1;
    TileScale += TileScale == 0;

    std::printf("Scale: %d\n", ScaleFactor);
    std::printf("TileScale: %d\n", TileScale);

    TileSize = std::min(8 * TileScale, 32);
    CoarseTileCountY = TileSize < 32 ? 4 : 6;
    ClearCoarseBinMaskLocalSize = TileSize < 32 ? 64 : 48;
    CoarseTileArea = CoarseTileCountX * CoarseTileCountY;
    CoarseTileW = CoarseTileCountX * TileSize;
    CoarseTileH = CoarseTileCountY * TileSize;

    TilesPerLine = ScreenWidth/TileSize;
    TileLines = ScreenHeight/TileSize;

    MaxWorkTiles = TilesPerLine*TileLines*16;

    // Bound both the producer's storage and its indirect consumers. Indirect
    // dispatch does not validate counts and exceeding a device limit is UB.
    GLint groupsX = 0, groupsZ = 0;
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 0, &groupsX);
    glGetIntegeri_v(GL_MAX_COMPUTE_WORK_GROUP_COUNT, 2, &groupsZ);
    if (!OpenGL::CheckError("Compute dispatch limits") || groupsX <= 0 || groupsZ <= 0)
        return false;
    MaxBatchWork = int(std::min({u64(MaxWorkTiles), u64(groupsZ), u64(groupsX)*32}));
    MaxBatchSpans = int(std::min(u64(64)*2048*ScaleFactor, u64(groupsX)*32));
    if (MaxBatchWork < TilesPerLine*TileLines || MaxBatchSpans < ScreenHeight)
        return false;

    for (int i = 0; i < tilememoryLayer_Num; i++)
    {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, TileMemory[i]);
        glBufferData(GL_SHADER_STORAGE_BUFFER, 4*TileSize*TileSize*MaxWorkTiles, nullptr, GL_DYNAMIC_DRAW);
        if (!OpenGL::CheckError("Compute tile storage")) return false;
    }

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, FinalTileMemory);
    // Two color/depth/attribute layers and the stencil/shadow-run state carried
    // between batches. Final shading still runs only after every polygon.
    glBufferData(GL_SHADER_STORAGE_BUFFER, 4*7*ScreenWidth*ScreenHeight, nullptr, GL_DYNAMIC_DRAW);
    if (!OpenGL::CheckError("Compute final tile storage")) return false;

    int binResultSize = sizeof(BinResultHeader)
        + TilesPerLine*TileLines*CoarseBinStride*4 // BinnedMaskCoarse
        + TilesPerLine*TileLines*BinStride*4 // BinnedMask
        + TilesPerLine*TileLines*BinStride*4; // WorkOffsets
    glBindBuffer(GL_SHADER_STORAGE_BUFFER, BinResultMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, binResultSize, nullptr, GL_DYNAMIC_DRAW);
    if (!OpenGL::CheckError("Compute bin storage")) return false;

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, WorkDescMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, MaxWorkTiles*2*4*2, nullptr, GL_DYNAMIC_DRAW);
    if (!OpenGL::CheckError("Compute work storage")) return false;

    if (Framebuffer != 0)
        glDeleteTextures(1, &Framebuffer);
    glGenTextures(1, &Framebuffer);
    glBindTexture(GL_TEXTURE_2D, Framebuffer);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, ScreenWidth, ScreenHeight);
    if (!OpenGL::CheckError("Compute framebuffer storage")) return false;

    Parent.OutputTex3D = Framebuffer;

    // BatchSize keeps each upload within these fixed allocations.
    int maxYSpanIndices = 64*2048 * ScaleFactor;
    YSpanIndices.resize(maxYSpanIndices);

    glBindBuffer(GL_TEXTURE_BUFFER, YSpanIndicesTextureMemory);
    glBufferData(GL_TEXTURE_BUFFER, maxYSpanIndices*2*4, nullptr, GL_DYNAMIC_DRAW);
    if (!OpenGL::CheckError("Compute span index storage")) return false;

    glBindBuffer(GL_SHADER_STORAGE_BUFFER, XSpanSetupMemory);
    glBufferData(GL_SHADER_STORAGE_BUFFER, sizeof(SpanSetupX)*maxYSpanIndices, nullptr, GL_DYNAMIC_DRAW);
    if (!OpenGL::CheckError("Compute X-span storage")) return false;

    glBindTexture(GL_TEXTURE_BUFFER, YSpanIndicesTexture);
    glTexBuffer(GL_TEXTURE_BUFFER, GL_RGBA16UI, YSpanIndicesTextureMemory);
    return OpenGL::CheckError("Compute span texture binding");
}



struct Variant
{
    GLuint Texture, Sampler;
    u16 Width, Height;
    u8 BlendMode;
    int CaptureYOffset;

    bool operator==(const Variant& other) const noexcept
    {
        return Texture == other.Texture && Sampler == other.Sampler && BlendMode == other.BlendMode &&
               CaptureYOffset == other.CaptureYOffset;
    }
};

/*
    Antialiasing
    W-Buffer
    With Texture
    0
    1, 3
    2
    without Texture
    2
    0, 1, 3

    => 20 Shader + 1x Shadow Mask
*/

void ComputeRenderer3D::RenderFrame()
{
    if (ShaderCompileFailed) return;
    assert(!NeedsShaderCompile());
    u8 clrBitmapDirty;
    if (!Texcache.Update(clrBitmapDirty) && GPU3D.RenderFrameIdentical && !RenderSettingsDirty)
    {
        return;
    }
    RenderSettingsDirty = false;

    // figure out which chunks of texture memory contain display captures
    int captureinfo[16];
    GPU.GetCaptureInfo_Texture(captureinfo);

    // if we're using a clear bitmap, set that up
    ClearBitmapDirty |= clrBitmapDirty;
    if (GPU3D.RenderDispCnt & (1<<14))
    {
        ComputeData::DecodeClearBitmap(GPU.VRAMFlat_Texture, ClearBitmap[0], ClearBitmap[1], ClearBitmapDirty);
        if (ClearBitmapDirty & (1<<0))
        {
            glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[0]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RED_INTEGER, GL_UNSIGNED_INT, ClearBitmap[0]);
        }

        if (ClearBitmapDirty & (1<<1))
        {
            glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[1]);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 256, GL_RED_INTEGER, GL_UNSIGNED_INT, ClearBitmap[1]);
        }

        ClearBitmapDirty = 0;
    }

    int first = 0;
    do
    {
        const int count = BatchSize(first);
        RenderBatch(first, count, captureinfo);
        first += count;
    } while (first < GPU3D.RenderNumPolygons);

    glBindImageTexture(0, Framebuffer, 0, GL_FALSE, 0, GL_WRITE_ONLY, GL_RGBA8);
    u32 finalPassShader = 0;
    if (GPU3D.RenderDispCnt & (1<<4))
        finalPassShader |= 0x4;
    if (GPU3D.RenderDispCnt & (1<<7))
        finalPassShader |= 0x2;
    if (GPU3D.RenderDispCnt & (1<<5))
        finalPassShader |= 0x1;

    glUseProgram(ShaderFinalPass[finalPassShader]);
    glDispatchCompute(ScreenWidth/32, ScreenHeight, 1);
    // The 2D compositor and display capture sample this image as a texture.
    glMemoryBarrier(GL_SHADER_IMAGE_ACCESS_BARRIER_BIT | GL_TEXTURE_FETCH_BARRIER_BIT);

    glBindSampler(0, 0);
    glBindSampler(1, 0);
    glBindSampler(2, 0);
}

// Conservative screen-tile bounds avoid a per-frame GPU counter readback.
// Each polygon remains whole and in order; zero-area polygons still need a
// scanline. Edge setup may cover x-1 at a vertical right edge.
int ComputeRenderer3D::BatchSize(int first) const
{
    int work = 0, spans = 0, edges = 0, count = 0;
    while (first + count < GPU3D.RenderNumPolygons && count < MaxVariants)
    {
        const Polygon& poly = *GPU3D.RenderPolygonRAM[first + count];
        int xmin = ScreenWidth, xmax = 0, ymin = ScreenHeight, ymax = 0;
        for (u32 v = 0; v < poly.NumVertices; ++v)
        {
            const Vertex& vertex = *poly.Vertices[v];
            const int x = HiresCoordinates ? (vertex.HiresPosition[0] * ScaleFactor) >> 4 : vertex.FinalPosition[0] * ScaleFactor;
            const int y = HiresCoordinates ? (vertex.HiresPosition[1] * ScaleFactor) >> 4 : vertex.FinalPosition[1] * ScaleFactor;
            xmin = std::min(xmin, x); xmax = std::max(xmax, x);
            ymin = std::min(ymin, y); ymax = std::max(ymax, y);
        }
        const int height = std::max(1, ymax - ymin);
        const int left = std::clamp(xmin - 1, 0, ScreenWidth - 1) / TileSize;
        const int right = std::clamp(xmax, 0, ScreenWidth - 1) / TileSize;
        const int top = std::clamp(ymin, 0, ScreenHeight - 1) / TileSize;
        const int bottom = std::clamp(ymin + height - 1, 0, ScreenHeight - 1) / TileSize;
        const int tiles = (right - left + 1) * (bottom - top + 1);
        const int edgeCount = poly.NumVertices + 2;
        if (work + tiles > MaxBatchWork || spans + height > MaxBatchSpans || edges + edgeCount > MaxYSpanSetups)
            break;
        work += tiles;
        spans += height;
        edges += edgeCount;
        ++count;
    }
    // Scale validation guarantees room for one full-screen polygon.
    assert(count || first == GPU3D.RenderNumPolygons);
    return count;
}

void ComputeRenderer3D::RenderBatch(int first, int count, const int* captureinfo)
{
    int numYSpans = 0;
    int numSetupIndices = 0;

    /*
        Some games really like to spam small textures, often
        to store the data like PPU tiles. E.g. Shantae
        or some Mega Man game. Fortunately they are usually kind
        enough to not vary the texture size all too often (usually
        they just use 8x8 or 16x for everything).

        This is the reason we have this whole mess where textures of
        the same size are put into array textures. This allows
        to increase the batch size.
        Less variance between each Variant hah!
    */
    u32 numVariants = 0, prevVariant = 0, prevTexLayer = 0;
    Variant variants[MaxVariants];
    u32 capLastVariant[16] = {0};

    bool enableTextureMaps = GPU3D.RenderDispCnt & (1<<0);

    for (int i = 0; i < count; i++)
    {
        Polygon* polygon = GPU3D.RenderPolygonRAM[first + i];

        bool foundVariant = false;
        if (i > 0)
        {
            // if the whole texture attribute matches
            // the texture layer will also match
            Polygon* prevPolygon = GPU3D.RenderPolygonRAM[first + i - 1];
            foundVariant = prevPolygon->TexParam == polygon->TexParam
                && prevPolygon->TexPalette == polygon->TexPalette
                && (prevPolygon->Attr & 0x30) == (polygon->Attr & 0x30)
                && prevPolygon->IsShadowMask == polygon->IsShadowMask;
        }

        if (!foundVariant)
        {
            Variant variant{};
            variant.BlendMode = polygon->IsShadowMask ? 4 : ((polygon->Attr >> 4) & 0x3);
            variant.Texture = 0;
            variant.Sampler = 0;
            u32* textureLastVariant = nullptr;
            // we always need to look up the texture to get the layer of the array texture
            u32 textype = (polygon->TexParam >> 26) & 0x7;
            if (enableTextureMaps && textype)
            {
                u32 texaddr = polygon->TexParam & 0xFFFF;
                u32 texwidth = TextureWidth(polygon->TexParam);
                int capblock = GetTextureCaptureBlock(polygon->TexParam, captureinfo);

                if (capblock != -1)
                {
                    if (texwidth == 128)
                    {
                        variant.Texture = -1;
                        variant.CaptureYOffset = (int)((texaddr >> 5) & 0x7F);
                        prevTexLayer = capblock;
                    }
                    else
                    {
                        variant.Texture = -2;
                        variant.CaptureYOffset = (int)((texaddr >> 6) & 0xFF);
                        prevTexLayer = capblock >> 2;
                    }

                    textureLastVariant = &capLastVariant[capblock];
                }
                else
                {
                    Texcache.GetTexture(polygon->TexParam, polygon->TexPalette, variant.Texture, prevTexLayer, textureLastVariant);
                    variant.CaptureYOffset = -1;
                }

                bool wrapS = (polygon->TexParam >> 16) & 1;
                bool wrapT = (polygon->TexParam >> 17) & 1;
                bool mirrorS = (polygon->TexParam >> 18) & 1;
                bool mirrorT = (polygon->TexParam >> 19) & 1;
                variant.Sampler = Samplers[(wrapS ? (mirrorS ? 2 : 1) : 0) + (wrapT ? (mirrorT ? 2 : 1) : 0) * 3];

                if (*textureLastVariant < numVariants && variants[*textureLastVariant] == variant)
                {
                    foundVariant = true;
                    prevVariant = *textureLastVariant;
                }
            }

            if (!foundVariant)
            {
                for (int j = numVariants - 1; j >= 0; j--)
                {
                    if (variants[j] == variant)
                    {
                        foundVariant = true;
                        prevVariant = j;
                        goto foundVariant;
                    }
                }

                prevVariant = numVariants;
                variants[numVariants] = variant;
                variants[numVariants].Width = TextureWidth(polygon->TexParam);
                variants[numVariants].Height = TextureHeight(polygon->TexParam);
                numVariants++;
                assert(numVariants <= MaxVariants);
            foundVariant:;

                if (textureLastVariant)
                    *textureLastVariant = prevVariant;
            }
        }
        RenderPolygons[i].Variant = prevVariant;
        RenderPolygons[i].TextureLayer = (float)prevTexLayer;

        ComputeData::PreparePolygon(polygon, i, RenderPolygons[i], YSpanSetups,
            numYSpans, YSpanIndices, numSetupIndices, ScaleFactor, HiresCoordinates);
        //printf("polygon min max %d %d | %d %d\n", RenderPolygons[i].XMin, RenderPolygons[i].XMinY, RenderPolygons[i].XMax, RenderPolygons[i].XMaxY);
    }

    /*for (u32 i = 0; i < RenderNumPolygons; i++)
    {
        if (RenderPolygons[i].Variant >= numVariants)
        {
            printf("blarb2 %d %d %d\n", RenderPolygons[i].Variant, i, RenderNumPolygons);
        }
        //assert(RenderPolygons[i].Variant < numVariants);
    }*/

    if (numYSpans > 0)
    {
        glBindBuffer(GL_SHADER_STORAGE_BUFFER, YSpanSetupMemory);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, sizeof(SpanSetupY)*numYSpans, YSpanSetups);

        glBindBuffer(GL_TEXTURE_BUFFER, YSpanIndicesTextureMemory);
        glBufferSubData(GL_TEXTURE_BUFFER, 0, numSetupIndices*4*2, YSpanIndices.data());

        glBindBuffer(GL_SHADER_STORAGE_BUFFER, RenderPolygonMemory);
        glBufferSubData(GL_SHADER_STORAGE_BUFFER, 0, count*sizeof(RenderPolygon), RenderPolygons);
        // we haven't accessed image data yet, so we don't need to invalidate anything
    }

    //printf("found via %d %d %d of %d\n", foundviatexcache, foundviaprev, numslow, RenderNumPolygons);

    // bind everything
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 0, RenderPolygonMemory);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 1, XSpanSetupMemory);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2, YSpanSetupMemory);

    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 5, FinalTileMemory);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 6, BinResultMemory);
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 7, WorkDescMemory);

    const MetaUniform meta = ComputeData::PrepareMeta(GPU3D, count, numVariants);

    glBindBuffer(GL_UNIFORM_BUFFER, MetaUniformMemory);
    glBufferSubData(GL_UNIFORM_BUFFER, 0, sizeof(MetaUniform), &meta);
    glBindBufferBase(GL_UNIFORM_BUFFER, 0, MetaUniformMemory);

    glUseProgram(ShaderClearCoarseBinMask);
    glDispatchCompute(TilesPerLine*TileLines/ClearCoarseBinMaskLocalSize, 1, 1);

    bool wbuffer = false;
    if (numYSpans > 0)
    {
        wbuffer = GPU3D.RenderPolygonRAM[0]->WBuffer;

        // calculate x-spans
        glBindImageTexture(0, YSpanIndicesTexture, 0, GL_FALSE, 0, GL_READ_ONLY, GL_RGBA16UI);
        glUseProgram(ShaderInterpXSpans[wbuffer]);
        glDispatchCompute((numSetupIndices + 31) / 32, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

        // bin polygons
        glUseProgram(ShaderBinCombined);
        glDispatchCompute(((count + 31) / 32), ScreenWidth/CoarseTileW, ScreenHeight/CoarseTileH);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

        // calculate list offsets
        glUseProgram(ShaderCalculateWorkListOffset);
        glDispatchCompute((numVariants + 31) / 32, 1, 1);
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

        // sort shader work
        glUseProgram(ShaderSortWork);
        glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, BinResultMemory);
        glDispatchComputeIndirect(offsetof(BinResultHeader, SortWorkWorkCount));
        glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_COMMAND_BARRIER_BIT);

        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D_ARRAY, Parent.CaptureOutput128Tex);
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D_ARRAY, Parent.CaptureOutput256Tex);

        glActiveTexture(GL_TEXTURE0);

        for (int i = 0; i < tilememoryLayer_Num; i++)
            glBindBufferBase(GL_SHADER_STORAGE_BUFFER, 2+i, TileMemory[i]);

        // rasterise
        {
            bool highLightMode = GPU3D.RenderDispCnt & (1<<1);

            GLuint shadersNoTexture[] =
            {
                ShaderRasteriseNoTexture[wbuffer],
                ShaderRasteriseNoTexture[wbuffer],
                highLightMode
                    ? ShaderRasteriseNoTextureHighlight[wbuffer]
                    : ShaderRasteriseNoTextureToon[wbuffer],
                ShaderRasteriseNoTexture[wbuffer],
                ShaderRasteriseShadowMask[wbuffer]
            };
            GLuint shadersUseTexture[] =
            {
                ShaderRasteriseUseTextureModulate[wbuffer],
                ShaderRasteriseUseTextureDecal[wbuffer],
                highLightMode
                    ? ShaderRasteriseUseTextureHighlight[wbuffer]
                    : ShaderRasteriseUseTextureToon[wbuffer],
                ShaderRasteriseUseTextureDecal[wbuffer],
                ShaderRasteriseShadowMask[wbuffer]
            };

            GLuint prevShader = 0;
            s32 prevTexture = 0, prevSampler = 0;
            for (int i = 0; i < numVariants; i++)
            {
                GLuint shader = 0;
                if (variants[i].Texture == 0)
                {
                    shader = shadersNoTexture[variants[i].BlendMode];
                }
                else
                {
                    shader = shadersUseTexture[variants[i].BlendMode];

                    // Sampler-only variants still use the capture texture's
                    // unit, even when the texture binding itself is unchanged.
                    const GLuint texunit = variants[i].Texture == (GLuint)-1 ? 1 :
                        variants[i].Texture == (GLuint)-2 ? 2 : 0;
                    bool unitchange = false;
                    if (variants[i].Texture != prevTexture)
                    {
                        bool previscap = (prevTexture == (GLuint)-1 || prevTexture == (GLuint)-2);
                        unitchange = texunit != 0 || previscap;

                        if (texunit == 0)
                            glBindTexture(GL_TEXTURE_2D_ARRAY, variants[i].Texture);
                        prevTexture = variants[i].Texture;
                    }
                    if ((variants[i].Sampler != prevSampler) || unitchange)
                    {
                        glBindSampler(texunit, variants[i].Sampler);
                        prevSampler = variants[i].Sampler;
                    }
                }
                assert(shader != 0);
                if (shader != prevShader)
                {
                    glUseProgram(shader);
                    prevShader = shader;
                }

                glUniform1ui(UniformIdxCurVariant, i);
                glUniform2f(UniformIdxTextureSize, 1.f / variants[i].Width, 1.f / variants[i].Height);
                if (variants[i].CaptureYOffset != -1)
                {
                    if (variants[i].Width == 128)
                        glUniform1i(UniformIdxTexIsCapture, 1);
                    else
                        glUniform1i(UniformIdxTexIsCapture, 2);
                    glUniform1f(UniformIdxCaptureYOffset, (float)variants[i].CaptureYOffset / (float)variants[i].Height);
                }
                else
                    glUniform1i(UniformIdxTexIsCapture, 0);
                glBindBuffer(GL_DISPATCH_INDIRECT_BUFFER, BinResultMemory);
                glDispatchComputeIndirect(offsetof(BinResultHeader, VariantWorkCount) + i*4*4);
            }
        }
    }
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT);

    glBindSampler(0, 0);
    glBindSampler(1, 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, ClearBitmapTex[1]);

    // compose final image
    glUseProgram(ShaderDepthBlend[wbuffer]);
    glUniform1i(0, first == 0);
    glDispatchCompute(ScreenWidth/TileSize, ScreenHeight/TileSize, 1);
    glMemoryBarrier(GL_SHADER_STORAGE_BARRIER_BIT | GL_BUFFER_UPDATE_BARRIER_BIT);
}

void ComputeRenderer3D::RestartFrame()
{
}

u32* ComputeRenderer3D::GetLine(int line)
{
    return nullptr;
}

}
