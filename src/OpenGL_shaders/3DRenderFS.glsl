#version 140

uniform usampler2DArray CurTexture;
uniform sampler2DArray Capture128Texture;
uniform sampler2DArray Capture256Texture;
uniform sampler2D BlendDestination;

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

uniform int uRenderMode; // 0=opaque 1=translucent 2=shadowmask
uniform int uAlphaRef;

smooth in vec4 fColor;
smooth in vec2 fTexcoord;
flat in ivec3 fPolygonAttr;

#ifdef WBuffer
smooth in float fZ;
#endif

out vec4 oColor;
out vec4 oAttr;

vec4 FinalColor()
{
    vec4 col;
    vec4 vcol = fColor;
    int blendmode = (fPolygonAttr.x >> 4) & 0x3;

    if (blendmode == 2)
    {
        if ((uDispCnt & (1<<1)) == 0)
        {
            // toon
            vec3 tooncolor = uToonColors[int(vcol.r * 31)].rgb;
            vcol.rgb = tooncolor;
        }
        else
        {
            // highlight
            vcol.rgb = vcol.rrr;
        }
    }

    if (fPolygonAttr.y == 0xFFFF)
    {
        // no texture
        col = vcol;
    }
    else
    {
        vec3 texcoord = vec3(fTexcoord, fPolygonAttr.y);
        vec4 tcol;
        if (fPolygonAttr.z == 0)
            tcol = vec4(texture(CurTexture, texcoord)) / vec4(63,63,63,31);
        else if (fPolygonAttr.z == 1)
            tcol = texture(Capture128Texture, texcoord);
        else
            tcol = texture(Capture256Texture, texcoord);

        if ((blendmode & 1) != 0)
        {
            // decal
            col.rgb = (tcol.rgb * tcol.a) + (vcol.rgb * (1.0-tcol.a));
            col.a = vcol.a;
        }
        else
        {
            // modulate
            col = vcol * tcol;
            uint textureAlpha = uint(floor(tcol.a * 31.0 + 0.5));
            uint polygonAlpha = uint(floor(vcol.a * 31.0 + 0.5));
            col.a = float(((textureAlpha + 1u) * (polygonAlpha + 1u) - 1u) >> 5u) / 31.0;
        }
    }

    if (blendmode == 2)
    {
        if ((uDispCnt & (1<<1)) != 0)
        {
            vec3 tooncolor = uToonColors[int(vcol.r * 31)].rgb;
            col.rgb = min(col.rgb + tooncolor, 1.0);
        }
    }

    return col.rgba;
}

void main()
{
    if (uRenderMode == 2)
    {
        oColor = vec4(0,0,0,1);
    }
    else
    {
        vec4 col = FinalColor();
        // Match the existing half-step thresholds for the 5-bit alpha value.
        if (col.a < (float(uAlphaRef) + 0.5) / 31.0) discard;
        if (uRenderMode == 0)
        {
            // opaque pixels
            if (col.a < 30.5/31) discard;

            oAttr.r = float((fPolygonAttr.x >> 24) & 0x3F) / 63.0;
            oAttr.g = 0;
            oAttr.b = float((fPolygonAttr.x >> 15) & 0x1);
            oAttr.a = 1;
        }
        else
        {
            // translucent pixels
            if (col.a >= 30.5/31) discard;

            oAttr.b = 0;
            oAttr.a = 1;
        }

        if (uRenderMode == 1)
        {
            vec4 destination = texelFetch(BlendDestination, ivec2(gl_FragCoord.xy), 0);
            uvec3 src = uvec3(round(clamp(col.rgb, 0.0, 1.0) * 255.0)) >> 2u;
            uvec3 dst = uvec3(round(destination.rgb * 255.0)) >> 2u;
            uint srcAlpha = uint(round(col.a * 31.0));
            uint dstAlpha = uint(round(destination.a * 31.0));
            if (dstAlpha != 0u && (uDispCnt & (1 << 3)) != 0)
                src = (src * (srcAlpha + 1u) + dst * (31u - srcAlpha)) >> 5u;
            oColor = vec4(vec3(src * 4u) / 255.0, float(max(srcAlpha, dstAlpha)) / 31.0);
        }
        else
            oColor = col;
    }

#ifdef WBuffer
    gl_FragDepth = fZ;
#endif
}
