// SPDX-License-Identifier: GPL-3.0-or-later
// Compile extracted production definitions against an observable GL state machine.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <vector>
using GLuint = unsigned;
using GLenum = unsigned;
using GLint = int;
using u32 = std::uint32_t;
constexpr GLenum GL_TEXTURE0 = 0, GL_TEXTURE1 = 1, GL_TEXTURE2 = 2, GL_TEXTURE3 = 3;
constexpr GLenum GL_TEXTURE_2D_ARRAY = 10, GL_TEXTURE_2D = 11;
constexpr GLenum GL_TEXTURE_WRAP_S = 12, GL_TEXTURE_WRAP_T = 13;
constexpr GLint GL_CLAMP_TO_EDGE = 20, GL_REPEAT = 21, GL_MIRRORED_REPEAT = 22;
constexpr GLenum GL_READ_FRAMEBUFFER = 30, GL_COLOR_ATTACHMENT0 = 31;
constexpr GLenum GL_LINES = 40, GL_UNSIGNED_SHORT = 41;
struct Calls { unsigned active = 0, bind = 0, wrap = 0; };
struct Machine
{
    unsigned active = 3;
    std::array<GLuint, 4> arrays {77, 101, 102, 0};
    std::map<GLuint, std::array<GLint, 2>> wraps;
    Calls calls;
    bool operator==(const Machine& other) const
    { return active == other.active && arrays == other.arrays && wraps == other.wraps; }
} machine;
void Require(bool ok, const char* label)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
}
void glActiveTexture(GLenum unit) { machine.active = unit; ++machine.calls.active; }
void glBindTexture(GLenum target, GLuint id)
{
    if (target == GL_TEXTURE_2D_ARRAY) { machine.arrays[machine.active] = id; ++machine.calls.bind; }
}
void glTexParameteri(GLenum target, GLenum parameter, GLint value)
{
    Require(target == GL_TEXTURE_2D_ARRAY, "array parameter target");
    machine.wraps[machine.arrays[machine.active]][parameter == GL_TEXTURE_WRAP_T] = value;
    ++machine.calls.wrap;
}
void glBindFramebuffer(GLenum, GLuint) {}
void glReadBuffer(GLenum) {}
void glCopyTexSubImage2D(GLenum, int, int, int, int, int, int, int) {}
std::vector<Machine> draws;
void glDrawElements(GLenum, unsigned, GLenum, void*) { draws.push_back(machine); }
class GLRenderer3D
{
public:
    struct RendererPolygon
    {
        GLuint TexID = 0;
        u32 TexRepeat = 0;
        GLuint PrimType = 42;
        u32 RenderKey = 0, NumIndices = 3, IndicesOffset = 0;
        u32 NumEdgeIndices = 6, EdgeIndicesOffset = 0;
        int MinX = 0, MinY = 0, MaxX = 8, MaxY = 8;
    };
#include "texture-state.inc"
    void BaselineSetup(const RendererPolygon*) const;
    void SetupPolygonTexture(const RendererPolygon*, PolygonTextureState&) const;
    void SnapshotBlendDestination(int, int) const;
    int RenderSinglePolygon(int, PolygonTextureState&) const;
    int RenderPolygonBatch(int, PolygonTextureState&) const;
    int RenderPolygonEdgeBatch(int, PolygonTextureState&) const;
    std::array<RendererPolygon, 8> PolygonList;
    int NumFinalPolys = 8, CurrentRenderMode = 0;
    static constexpr int RenderMode_Translucent = 1;
    int ScreenW = 256, ScreenH = 192;
    GLuint BlendDestinationTex = 200, MainFramebuffer = 201;
};
#include "texture-setup.inc"
// Frozen pre-cache behavior: an independent reference for observable GL state.
void GLRenderer3D::BaselineSetup(const RendererPolygon* poly) const
{
    if (poly->TexID == GLuint(-1))
        glActiveTexture(GL_TEXTURE1);
    else if (poly->TexID == GLuint(-2))
        glActiveTexture(GL_TEXTURE2);
    else
    {
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D_ARRAY, poly->TexID);
    }
    const GLint s = !(poly->TexRepeat & 1) ? GL_CLAMP_TO_EDGE
        : (poly->TexRepeat & 4) ? GL_MIRRORED_REPEAT : GL_REPEAT;
    const GLint t = !(poly->TexRepeat & 2) ? GL_CLAMP_TO_EDGE
        : (poly->TexRepeat & 8) ? GL_MIRRORED_REPEAT : GL_REPEAT;
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_S, s);
    glTexParameteri(GL_TEXTURE_2D_ARRAY, GL_TEXTURE_WRAP_T, t);
}
using Poly = GLRenderer3D::RendererPolygon;
using State = GLRenderer3D::PolygonTextureState;
Machine Initial(unsigned seed)
{
    Machine m;
    m.active = seed % 4;
    m.arrays[0] = 70 + seed % 5;
    for (GLuint id : {0u, 11u, 12u, 101u, 102u})
        m.wraps[id] = {GLint(20 + seed % 3), GLint(20 + (seed / 3) % 3)};
    return m;
}
int main()
{
    GLRenderer3D renderer;
    std::vector<Poly> choices;
    for (GLuint id : {0u, 11u, 12u, GLuint(-1), GLuint(-2)})
        for (u32 repeat = 0; repeat < 16; ++repeat)
        { Poly p; p.TexID = id; p.TexRepeat = repeat; choices.push_back(p); }
    unsigned transitions = 0;
    // A,A,B,B,A,A checks repetition and object-local wrap changes on return;
    // snapshot interference before every call verifies unit 3 restoration.
    for (const auto& a : choices)
    for (const auto& b : choices)
    for (bool snapshot : {false, true})
    {
        State state;
        Machine expected = Initial(transitions), actual = expected;
        for (const auto* p : {&a, &a, &b, &b, &a, &a})
        {
            machine = expected;
            if (snapshot) glActiveTexture(GL_TEXTURE3);
            renderer.BaselineSetup(p);
            expected = machine;
            machine = actual;
            if (snapshot) glActiveTexture(GL_TEXTURE3);
            renderer.SetupPolygonTexture(p, state);
            actual = machine;
            Require(actual == expected, "exhaustive state transition");
            ++transitions;
        }
    }
    // New chunk after arbitrary external bindings/wrap writes or ID reuse.
    for (const auto& p : choices)
    for (unsigned seed = 0; seed < 16; ++seed)
    {
        machine = Initial(seed);
        renderer.BaselineSetup(&p);
        const auto expected = machine;
        machine = Initial(seed);
        State state;
        renderer.SetupPolygonTexture(&p, state);
        Require(machine == expected, "first call repairs externally changed state");
    }
    // Exercise the production caller bodies, including the actual snapshot.
    for (int mode : {0, 1})
    for (int caller = 0; caller < 3; ++caller)
    {
        renderer.CurrentRenderMode = mode;
        for (int i = 0; i < 8; ++i)
        {
            renderer.PolygonList[i] = choices[(i / 2) * 17];
            renderer.PolygonList[i].RenderKey = i / 2;
        }
        machine = Initial(4);
        draws.clear();
        State state;
        for (int i = 0; i < 8;)
            i += caller == 0 ? renderer.RenderSinglePolygon(i, state) :
                 caller == 1 ? renderer.RenderPolygonBatch(i, state) :
                               renderer.RenderPolygonEdgeBatch(i, state);
        const auto actualDraws = draws;
        machine = Initial(4);
        unsigned draw = 0;
        for (int i = 0; i < 8; i += caller == 0 ? 1 : 2)
        {
            if (caller != 2) renderer.SnapshotBlendDestination(i, caller == 0 ? 1 : 2);
            renderer.BaselineSetup(&renderer.PolygonList[i]);
            Require(actualDraws.at(draw++) == machine, "caller draw-time state");
        }
        Require(draw == actualDraws.size(), "batch draw count");
    }
    std::printf("PASS transitions=%u boundary cases=1280 caller paths=6\n", transitions);
    for (GLuint id : {11u, GLuint(-1), GLuint(-2)})
    {
        Poly p; p.TexID = id; p.TexRepeat = 15;
        machine = Initial(0);
        for (int i = 0; i < 1000; ++i) renderer.BaselineSetup(&p);
        const auto before = machine.calls;
        machine = Initial(0);
        State state;
        for (int i = 0; i < 1000; ++i) renderer.SetupPolygonTexture(&p, state);
        const auto after = machine.calls;
        std::printf("id=%u batches=1000 active=%u->%u bind=%u->%u wrap=%u->%u\n",
                    id, before.active, after.active, before.bind, after.bind, before.wrap, after.wrap);
        Require(after.active == before.active, "active unit restoration retained");
        Require(after.wrap == 2 && (id != 11 || after.bind == 1), "repeated setup call reduction");
    }
}
