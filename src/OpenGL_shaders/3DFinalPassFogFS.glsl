#version 140

uniform sampler2D DepthBuffer;
uniform sampler2D AttrBuffer;
uniform sampler2D ColorBuffer;

layout(std140) uniform uConfig
{
    vec2 uScreenSize;
    int uDispCnt;
    vec4 uToonColors[32];
    vec4 uEdgeColors[8];
    vec4 uFogColor;
    float uFogDensity[34];
    int uFogOffset;
    int uFogShift;
};

out vec4 oColor;

uint CalculateFog(float depth)
{
    int idepth = int(depth * 16777216.0);
    int densityid, densityfrac;

    if (idepth < uFogOffset)
    {
        densityid = 0;
        densityfrac = 0;
    }
    else
    {
        uint udepth = uint(idepth);
        udepth -= uint(uFogOffset);
        udepth = (udepth >> 2) << uint(uFogShift);

        densityid = int(udepth >> 17);
        if (densityid >= 32)
        {
            densityid = 32;
            densityfrac = 0;
        }
        else
        densityfrac = int(udepth & uint(0x1FFFF));
    }

    uint density = (uint(uFogDensity[densityid]) * (131072u - uint(densityfrac))
                  + uint(uFogDensity[densityid+1]) * uint(densityfrac)) >> 17u;
    return density >= 127u ? 128u : density;
}

void main()
{
    ivec2 coord = ivec2(gl_FragCoord.xy);

    vec4 source = texelFetch(ColorBuffer, coord, 0);
    vec4 depth = texelFetch(DepthBuffer, coord, 0);
    vec4 attr = texelFetch(AttrBuffer, coord, 0);

    if (attr.b == 0)
    {
        oColor = source;
        return;
    }

    uvec4 color = uvec4(floor(source * 255.0 + 0.5)) >> uvec4(2, 2, 2, 3);
    uvec4 fog = uvec4(floor(uFogColor * 31.0 + 0.5));
    fog.rgb = fog.rgb * 2u + min(fog.rgb, uvec3(1u));
    uint density = CalculateFog(depth.r);
    uvec4 result = (fog * density + color * (128u - density)) >> 7u;
    if ((uDispCnt & (1 << 6)) != 0) result.rgb = color.rgb;
    oColor = vec4(vec3(result.rgb * 4u) / 255.0, float(result.a) / 31.0);
}
