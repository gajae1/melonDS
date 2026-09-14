// Copyright 2016-2026 melonDS team
// SPDX-License-Identifier: GPL-3.0-or-later
#include "GPU3D_ComputeShader.h"
#include "GPU3D_Compute_shaders.h"
#include <array>
#include <stdexcept>

namespace melonDS::ComputeShader {
std::string BuildSource(unsigned variant, const Config& config, bool vulkan)
{
    struct Program { const std::string* body; const char* defines; };
    const std::array<Program, Count> programs{{
        {&ComputeRendererShaders::InterpSpans, "#define InterpSpans\n#define ZBuffer\n"},
        {&ComputeRendererShaders::InterpSpans, "#define InterpSpans\n#define WBuffer\n"},
        {&ComputeRendererShaders::BinCombined, "#define BinCombined\n"},
        {&ComputeRendererShaders::DepthBlend, "#define DepthBlend\n#define ZBuffer\n"},
        {&ComputeRendererShaders::DepthBlend, "#define DepthBlend\n#define WBuffer\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define ZBuffer\n#define NoTexture\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define WBuffer\n#define NoTexture\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define ZBuffer\n#define NoTexture\n#define Toon\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define WBuffer\n#define NoTexture\n#define Toon\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define ZBuffer\n#define NoTexture\n#define Highlight\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define WBuffer\n#define NoTexture\n#define Highlight\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define ZBuffer\n#define UseTexture\n#define Decal\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define WBuffer\n#define UseTexture\n#define Decal\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define ZBuffer\n#define UseTexture\n#define Modulate\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define WBuffer\n#define UseTexture\n#define Modulate\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define ZBuffer\n#define UseTexture\n#define Toon\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define WBuffer\n#define UseTexture\n#define Toon\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define ZBuffer\n#define UseTexture\n#define Highlight\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define WBuffer\n#define UseTexture\n#define Highlight\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define ZBuffer\n#define ShadowMask\n"},
        {&ComputeRendererShaders::Rasterise, "#define Rasterise\n#define WBuffer\n#define ShadowMask\n"},
        {&ComputeRendererShaders::ClearCoarseBinMask, "#define ClearCoarseBinMask\n"},
        {&ComputeRendererShaders::CalcOffsets, "#define CalculateWorkOffsets\n"},
        {&ComputeRendererShaders::SortWork, "#define SortWork\n"},
        {&ComputeRendererShaders::FinalPass, "#define FinalPass\n"},
        {&ComputeRendererShaders::FinalPass, "#define FinalPass\n#define EdgeMarking\n"},
        {&ComputeRendererShaders::FinalPass, "#define FinalPass\n#define Fog\n"},
        {&ComputeRendererShaders::FinalPass, "#define FinalPass\n#define EdgeMarking\n#define Fog\n"},
        {&ComputeRendererShaders::FinalPass, "#define FinalPass\n#define AntiAliasing\n"},
        {&ComputeRendererShaders::FinalPass, "#define FinalPass\n#define AntiAliasing\n#define EdgeMarking\n"},
        {&ComputeRendererShaders::FinalPass, "#define FinalPass\n#define AntiAliasing\n#define Fog\n"},
        {&ComputeRendererShaders::FinalPass, "#define FinalPass\n#define AntiAliasing\n#define EdgeMarking\n#define Fog\n"},
    }};
    if (variant >= programs.size()) throw std::out_of_range("Compute shader variant");
    std::string source = vulkan ? "#version 450\n" : "#version 430 core\n";
    source += programs[variant].defines;
    source += "\n#define ScreenWidth " + std::to_string(config.ScreenWidth);
    source += "\n#define ScreenHeight " + std::to_string(config.ScreenHeight);
    source += "\n#define MaxWorkTiles " + std::to_string(config.MaxWorkTiles);
    source += "\n#define TileSize " + std::to_string(config.TileSize);
    source += "\n#define CoarseTileCountY " + std::to_string(config.CoarseTileCountY);
    source += "\n#define CoarseTileArea " + std::to_string(config.CoarseTileArea);
    source += "\n#define ClearCoarseBinMaskLocalSize " + std::to_string(config.ClearCoarseBinMaskLocalSize);
    source += '\n';
    source += ComputeRendererShaders::Common;
    source += *programs[variant].body;
    return source;
}
}
