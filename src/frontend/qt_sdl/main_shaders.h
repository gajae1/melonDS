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

#ifndef MAIN_SHADERS_H
#define MAIN_SHADERS_H

const char* kScreenVS = R"(#version 140

uniform vec2 uScreenSize;
uniform mat2x3 uTransform;

in vec2 vPosition;
in vec3 vTexcoord;

smooth out vec3 fTexcoord;

void main()
{
    vec4 fpos;

    fpos.xy = vec3(vPosition, 1.0) * uTransform;

    fpos.xy = ((fpos.xy * 2.0) / uScreenSize) - 1.0;
    fpos.y *= -1;
    fpos.z = 0.0;
    fpos.w = 1.0;

    gl_Position = fpos;
    fTexcoord = vTexcoord;
}
)";

const char* kScreenFS = R"(#version 140

uniform sampler2DArray ScreenTex;

smooth in vec3 fTexcoord;

out vec4 oColor;

void main()
{
    vec4 pixel = texture(ScreenTex, fTexcoord);

    oColor = vec4(pixel.rgb, 1.0);
}
)";

#ifdef VULKANRENDERER_ENABLED
// Shared Vulkan R32_UINT output contains packed BGRA, not a normalized texture.
// Decode before interpolation to match the existing GL screen filter.
const char* kScreenExternalFS = R"(#version 140
uniform usampler2D TopScreenTex;
uniform usampler2D BottomScreenTex;
uniform bool uFilter;
smooth in vec3 fTexcoord;
out vec4 oColor;
vec3 samplePixel(ivec2 at, ivec2 size, bool top) {
    at = clamp(at, ivec2(0), size - 1);
    uint pixel = top ? texelFetch(TopScreenTex, at, 0).r : texelFetch(BottomScreenTex, at, 0).r;
    return vec3((pixel >> 16) & 255u, (pixel >> 8) & 255u, pixel & 255u) / 255.0;
}
void main() {
    bool top = fTexcoord.z < 0.5;
    ivec2 size = top ? textureSize(TopScreenTex, 0) : textureSize(BottomScreenTex, 0);
    vec2 at = fTexcoord.xy * vec2(size);
    vec3 pixel;
    if (!uFilter) pixel = samplePixel(ivec2(floor(at)), size, top);
    else {
        at -= 0.5;
        ivec2 base = ivec2(floor(at));
        vec2 f = fract(at);
        pixel = mix(mix(samplePixel(base, size, top), samplePixel(base + ivec2(1,0), size, top), f.x),
                    mix(samplePixel(base + ivec2(0,1), size, top), samplePixel(base + ivec2(1,1), size, top), f.x), f.y);
    }
    oColor = vec4(pixel, 1.0);
}
)";
#endif
#endif // MAIN_SHADERS_H
