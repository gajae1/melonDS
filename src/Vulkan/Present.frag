// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(set=0,binding=0) uniform usampler2D pixels;
layout(push_constant) uniform Parameters { vec4 x; vec4 y; vec4 sourceX; vec4 sourceY; vec4 options; } p;
layout(location=0) in vec2 uv;
layout(location=0) out vec4 color;
vec4 samplePixel(ivec2 at) {
    uint c = texelFetch(pixels,clamp(at,ivec2(0),textureSize(pixels,0)-1),0).r;
    return vec4((c>>16)&255u,(c>>8)&255u,c&255u,c>>24)/255.0;
}
void main() {
    vec2 at = uv * vec2(textureSize(pixels,0));
    if (p.options.x == 0) {
        // Match QPainter's nearest-neighbor ties, including mirrored axes.
        // Window coordinates avoid interpolator drift at exact texel edges.
        at = vec2(dot(p.sourceX.xyz,vec3(gl_FragCoord.xy,1)),
                  dot(p.sourceY.xyz,vec3(gl_FragCoord.xy,1)));
        color = samplePixel(ivec2(mix(ceil(at)-1,floor(at),p.options.yz)));
    }
    else {
        at -= .5;
        ivec2 base = ivec2(floor(at));
        vec2 f = fract(at);
        color = mix(mix(samplePixel(base),samplePixel(base+ivec2(1,0)),f.x),
                    mix(samplePixel(base+ivec2(0,1)),samplePixel(base+ivec2(1,1)),f.x),f.y);
    }
}
