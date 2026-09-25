// SPDX-License-Identifier: GPL-3.0-or-later
#version 450
layout(push_constant) uniform Parameters { vec4 x; vec4 y; vec4 sourceX; vec4 sourceY; vec4 options; } p;
layout(location=0) out vec2 uv;
void main() {
    const vec2 corners[6] = vec2[6](vec2(0,0),vec2(1,0),vec2(0,1),vec2(0,1),vec2(1,0),vec2(1,1));
    uv = corners[gl_VertexIndex];
    gl_Position = vec4(dot(p.x.xyz,vec3(uv,1)),dot(p.y.xyz,vec3(uv,1)),0,1);
}
