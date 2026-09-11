#version 140

uniform usampler2D ClearBitmapColor;
uniform usampler2D ClearBitmapDepth;

uniform vec2 uClearBitmapOffset;
uniform uint uOpaquePolyID;

smooth in vec2 fTexcoord;

out vec4 oColor;
out vec4 oAttr;

void main()
{
    vec2 pos = fTexcoord + uClearBitmapOffset;

    uvec4 texel = texture(ClearBitmapColor, pos);
    vec4 color = vec4(vec3(texel.rgb * 4u) / 255.0, float(texel.a) / 31.0);
    uint depth = texture(ClearBitmapDepth, pos).r;
    float fdepth = float(depth & 0xFFFFFFu) / 16777216.0;

    oColor = color;
    oAttr.r = float(uOpaquePolyID) / 63.0;
    oAttr.g = 0;
    oAttr.b = float(depth >> 24);
    oAttr.a = 1;
    gl_FragDepth = fdepth;
}
