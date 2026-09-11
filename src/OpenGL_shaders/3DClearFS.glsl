#version 140

uniform uvec4 uColor;
uniform uint uOpaquePolyID;
uniform uint uFogFlag;

out vec4 oColor;
out vec4 oAttr;

void main()
{
    // The 2D compositor/capture consumes six-bit RGB in the upper six bits.
    uvec3 rgb = uColor.rgb * 2u + uvec3(notEqual(uColor.rgb, uvec3(0)));
    oColor = vec4(vec3(rgb * 4u) / 255.0, float(uColor.a) / 31.0);
    oAttr.r = float(uOpaquePolyID) / 63.0;
    oAttr.g = 0;
    oAttr.b = float(uFogFlag);
    oAttr.a = 1;
}
