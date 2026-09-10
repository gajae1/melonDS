// SPDX-License-Identifier: GPL-3.0-or-later
// Called by GLFrameReadback after creating/loading a real current GL context.
// Driver fault injection is limited to compute shaders/capability queries.
// Dispatch calls are counted but not forwarded: invalid work never reaches GPU.
#include "NDS.h"
#include "GPU_OpenGL.h"
#include "GPU_Soft.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <SDL2/SDL.h>

// The production frontend methods run against the real GPU here. Only config
// reads and OSD delivery are isolated; this is not the Qt window/event loop.
using namespace melonDS;
enum { renderer3D_Software, renderer3D_OpenGL, renderer3D_OpenGLCompute };
struct FixtureConfig
{
    int Scale = 1;
    int GetInt(const char* key) const
    {
        if (std::strcmp(key, "3D.GL.ScaleFactor") == 0) return Scale;
        if (std::strcmp(key, "3D.Soft.PixelConversion") == 0) return 0;
        throw std::runtime_error("unexpected renderer config key");
    }
    bool GetBool(const char* key) const
    {
        if (std::strcmp(key, "3D.Soft.Threaded") == 0 ||
            std::strcmp(key, "3D.GL.HiresCoordinates") == 0 ||
            std::strcmp(key, "3D.GL.BetterPolygons") == 0) return false;
        throw std::runtime_error("unexpected renderer config key");
    }
};
struct FixtureInstance
{
    NDS* nds;
    FixtureConfig Config;
    unsigned Errors = 0;
    unsigned Progress = 0;
    FixtureConfig& getGlobalConfig() { return Config; }
    void osdAddMessage(u32 color, const char*, ...)
    {
        if (color) ++Errors;
        else ++Progress;
    }
};
struct EmuThread
{
    FixtureInstance* emuInstance;
    int videoRenderer = renderer3D_OpenGLCompute;
    int lastVideoRenderer = renderer3D_Software;
    void updateRenderer();
    void compileShaders();
};
#include "computeUpdateRenderer.inc"
#include "computeCompileShaders.inc"

namespace
{
using namespace melonDS;

enum class Fault { None, Capability, Compile, Link };
Fault ActiveFault;
bool Injected;
unsigned ComputeSources;
unsigned Dispatches;
unsigned ObservedCompileFailures;
unsigned ObservedLinkFailures;
GLuint FaultShader;

PFNGLGETINTEGERVPROC DriverGetIntegerv;
PFNGLSHADERSOURCEPROC DriverShaderSource;
PFNGLLINKPROGRAMPROC DriverLinkProgram;
PFNGLGETSHADERIVPROC DriverGetShaderiv;
PFNGLGETPROGRAMIVPROC DriverGetProgramiv;
PFNGLDISPATCHCOMPUTEPROC DriverDispatch;
PFNGLDISPATCHCOMPUTEINDIRECTPROC DriverDispatchIndirect;

void APIENTRY QueryCapability(GLenum pname, GLint* value)
{
    if (ActiveFault == Fault::Capability &&
        (pname == GL_MAJOR_VERSION || pname == GL_MINOR_VERSION))
    {
        *value = pname == GL_MAJOR_VERSION ? 3 : 2;
        return;
    }
    DriverGetIntegerv(pname, value);
}

void APIENTRY ShaderSource(GLuint shader, GLsizei count,
                          const GLchar* const* strings, const GLint* lengths)
{
    GLint type = 0;
    DriverGetShaderiv(shader, GL_SHADER_TYPE, &type);
    if (type == GL_COMPUTE_SHADER)
    {
        ++ComputeSources;
        if (ActiveFault == Fault::Compile && !Injected)
        {
            Injected = true;
            FaultShader = shader;
            const char* invalid = "#version 430 core\n#error deliberate_compute_failure\n";
            DriverShaderSource(shader, 1, &invalid, nullptr);
            return;
        }
    }
    DriverShaderSource(shader, count, strings, lengths);
}

void APIENTRY ShaderStatus(GLuint shader, GLenum pname, GLint* value)
{
    DriverGetShaderiv(shader, pname, value);
    if (pname == GL_COMPILE_STATUS && shader == FaultShader && *value == GL_FALSE)
        ++ObservedCompileFailures;
}

void APIENTRY ProgramStatus(GLuint program, GLenum pname, GLint* value)
{
    DriverGetProgramiv(program, pname, value);
    if (ActiveFault == Fault::Link && Injected && pname == GL_LINK_STATUS && *value == GL_FALSE)
        ++ObservedLinkFailures;
}

void APIENTRY LinkProgram(GLuint program)
{
    GLuint attached[8]{};
    GLsizei count = 0;
    glGetAttachedShaders(program, 8, &count, attached);
    bool compute = false;
    for (GLsizei i = 0; i < count; ++i)
    {
        GLint type = 0;
        DriverGetShaderiv(attached[i], GL_SHADER_TYPE, &type);
        compute |= type == GL_COMPUTE_SHADER;
    }
    if (ActiveFault == Fault::Link && compute && !Injected)
    {
        // Compile a second valid object, then link two definitions of main.
        // The real driver supplies the failure status; no fabricated GL status.
        GLuint duplicate = glCreateShader(GL_COMPUTE_SHADER);
        const char* source = "#version 430 core\nlayout(local_size_x=1) in; void main() {}\n";
        DriverShaderSource(duplicate, 1, &source, nullptr);
        glCompileShader(duplicate);
        GLint compiled = GL_FALSE;
        DriverGetShaderiv(duplicate, GL_COMPILE_STATUS, &compiled);
        if (compiled == GL_TRUE)
        {
            Injected = true;
            glAttachShader(program, duplicate);
            DriverLinkProgram(program);
            glDetachShader(program, duplicate);
            glDeleteShader(duplicate);
            return;
        }
        glDeleteShader(duplicate);
    }
    DriverLinkProgram(program);
}

void APIENTRY CountDispatch(GLuint, GLuint, GLuint) { ++Dispatches; }
void APIENTRY CountIndirectDispatch(GLintptr) { ++Dispatches; }

struct DriverHooks
{
    DriverHooks()
    {
        DriverGetIntegerv = glad_glGetIntegerv;
        DriverShaderSource = glad_glShaderSource;
        DriverLinkProgram = glad_glLinkProgram;
        DriverGetShaderiv = glad_glGetShaderiv;
        DriverGetProgramiv = glad_glGetProgramiv;
        DriverDispatch = glad_glDispatchCompute;
        DriverDispatchIndirect = glad_glDispatchComputeIndirect;
        glad_glGetIntegerv = QueryCapability;
        glad_glShaderSource = ShaderSource;
        glad_glLinkProgram = LinkProgram;
        glad_glGetShaderiv = ShaderStatus;
        glad_glGetProgramiv = ProgramStatus;
        glad_glDispatchCompute = CountDispatch;
        glad_glDispatchComputeIndirect = CountIndirectDispatch;
    }
    ~DriverHooks()
    {
        glad_glGetIntegerv = DriverGetIntegerv;
        glad_glShaderSource = DriverShaderSource;
        glad_glLinkProgram = DriverLinkProgram;
        glad_glGetShaderiv = DriverGetShaderiv;
        glad_glGetProgramiv = DriverGetProgramiv;
        glad_glDispatchCompute = DriverDispatch;
        glad_glDispatchComputeIndirect = DriverDispatchIndirect;
    }
};

bool CompileAll(Renderer& renderer)
{
    for (int attempts = 0; renderer.NeedsShaderCompile() && attempts < 40; ++attempts)
    {
        int current = -1, total = 0;
        if (!renderer.ShaderCompileStep(current, total)) return false;
    }
    return !renderer.NeedsShaderCompile();
}

bool Check(bool condition, const char* text)
{
    if (!condition) std::fprintf(stderr, "ComputeFailure: %s\n", text);
    return condition;
}

bool SoftwareFrame(NDS& nds)
{
    if (!dynamic_cast<SoftRenderer*>(&nds.GetRenderer())) return false;
    RendererSettings settings{1, false, false, false};
    nds.GetRenderer().SetRenderSettings(settings);
    // Public synthetic ARM loops; no cartridge or private firmware file.
    nds.ARM9Write32(0x02000000, 0xEAFFFFFE);
    nds.ARM9Write32(0x02000200, 0xEAFFFFFE);
    nds.ARM9.JumpTo(0x02000000);
    nds.ARM7.JumpTo(0x02000200);
    nds.Start();
    if (!nds.RunFrame()) return false;
    void* top = nullptr;
    void* bottom = nullptr;
    return nds.GetRenderer().GetFramebuffers(&top, &bottom) && top && bottom;
}
bool CheckFrontendFailure(NDS& nds)
{
    bool passed = true;
    FixtureInstance instance{&nds};
    EmuThread thread{&instance};
    thread.updateRenderer();
    ActiveFault = Fault::Compile;
    thread.compileShaders();
    passed &= Check(ObservedCompileFailures == 1, "frontend driver failure not observed");
    passed &= Check(thread.videoRenderer == renderer3D_Software &&
                    thread.lastVideoRenderer == renderer3D_Software,
                    "frontend did not update active renderer state");
    passed &= Check(instance.Errors == 1 && instance.Progress == 0,
                    "frontend did not report one terminal error");
    passed &= Check(SoftwareFrame(nds), "frontend fallback did not produce RAM framebuffers");
    passed &= Check(Dispatches == 0, "frontend fallback reached compute dispatch");

    // Explicit reselection recovers, then scale recompilation can fail
    // independently of the previous successful programs.
    ActiveFault = Fault::None;
    thread.videoRenderer = renderer3D_OpenGLCompute;
    thread.updateRenderer();
    for (int attempts = 0; nds.GetRenderer().NeedsShaderCompile() && attempts < 40; ++attempts)
        thread.compileShaders();
    passed &= Check(dynamic_cast<GLRenderer*>(&nds.GetRenderer()) &&
                    !nds.GetRenderer().NeedsShaderCompile(), "frontend reselection did not compile");
    instance.Config.Scale = 2;
    thread.updateRenderer();
    const unsigned progress = instance.Progress;
    ActiveFault = Fault::Link;
    Injected = false;
    thread.compileShaders();
    passed &= Check(ObservedLinkFailures == 1 && instance.Errors == 2 &&
                    instance.Progress == progress, "frontend recompile failure lost its terminal error");
    passed &= Check(SoftwareFrame(nds), "frontend recompile fallback did not produce RAM framebuffers");
    passed &= Check(Dispatches == 0, "frontend recompile fallback reached compute dispatch");
    ActiveFault = Fault::Capability;
    thread.videoRenderer = renderer3D_OpenGLCompute;
    thread.updateRenderer();
    thread.updateRenderer();
    passed &= Check(thread.videoRenderer == renderer3D_Software &&
                    thread.lastVideoRenderer == renderer3D_Software && instance.Errors == 3,
                    "frontend capability fallback repeated or lost active state");
    passed &= Check(SoftwareFrame(nds), "frontend capability fallback did not produce RAM framebuffers");
    return passed;
}

}

// Integration: int CheckComputeFailure(const char* name); current GL >= 4.3.
// Names: control, capability, compile, link, recompile, frontend. Returns 0/1, 2 fixture
// setup failure, 77 unavailable context. Recompile is not a binary-cache hit.
int CheckComputeFailure(const char* name)
{
    using namespace melonDS;
    const bool control = std::strcmp(name, "control") == 0;
    const bool capability = std::strcmp(name, "capability") == 0;
    const bool compile = std::strcmp(name, "compile") == 0;
    const bool link = std::strcmp(name, "link") == 0;
    const bool recompile = std::strcmp(name, "recompile") == 0;
    const bool frontend = std::strcmp(name, "frontend") == 0;
    if (!control && !capability && !compile && !link && !recompile && !frontend) return 2;
    if (!GLAD_GL_VERSION_4_3 || !glad_glDispatchCompute) return 77;

    ActiveFault = capability ? Fault::Capability : Fault::None;
    Injected = false;
    ComputeSources = Dispatches = ObservedCompileFailures = ObservedLinkFailures = 0;
    FaultShader = 0;
    DriverHooks hooks;
    bool passed = true;
    {
        NDSArgs args;
        args.JIT = std::nullopt;
        auto nds = std::make_unique<NDS>(std::move(args));
        nds->Reset();
        if (frontend)
            passed &= CheckFrontendFailure(*nds);
        else
        {
            nds->SetRenderer(std::make_unique<GLRenderer>(*nds, true));
            const bool initiallySoftware = dynamic_cast<SoftRenderer*>(&nds->GetRenderer()) != nullptr;
            if (capability)
            {
                passed &= Check(initiallySoftware, "GL 3.2 capability accepted Compute instead of software fallback");
                passed &= Check(ComputeSources == 0 && Dispatches == 0,
                                "unsupported capability reached compute shader/dispatch");
                if (initiallySoftware)
                    passed &= Check(SoftwareFrame(*nds), "capability fallback did not produce RAM framebuffers");
            }
            else
            {
                if (initiallySoftware)
                {
                    std::fprintf(stderr, "ComputeFailure: fixture could not initialize normal GL renderer\n");
                    return 2;
                }
                RendererSettings settings{1, false, false, false};
                auto& renderer = nds->GetRenderer();
                renderer.SetRenderSettings(settings);
                if (recompile)
                {
                    if (!CompileAll(renderer)) return 2;
                    settings.ScaleFactor = 2;
                    renderer.SetRenderSettings(settings);
                }
                ActiveFault = (compile || recompile) ? Fault::Compile : link ? Fault::Link : Fault::None;
                const unsigned sourcesBeforeFault = ComputeSources;
                const bool successful = CompileAll(renderer);
                if (!control && (!Injected || (link ? !ObservedLinkFailures : !ObservedCompileFailures)))
                {
                    std::fprintf(stderr, "ComputeFailure: requested real driver failure was not observed\n");
                    return 2;
                }
                passed &= Check(successful == control, "failed shader compilation was promoted to completion");
                if (!control)
                {
                    passed &= Check(!renderer.NeedsShaderCompile(), "failed renderer remained in compile loop");
                    passed &= Check(ComputeSources == sourcesBeforeFault + 1,
                                    "compiler continued after the first failed step");
                }

                // Baseline completes all 33 steps despite failure and enters RenderFrame.
                // Candidate failed state must reject rendering even before caller fallback.
                if (!renderer.NeedsShaderCompile())
                {
                    nds->GPU.GPU3D.RenderFrameIdentical = false;
                    renderer.Start3DRendering();
                }
                if (control)
                    passed &= Check(Dispatches > 0, "normal control never reached compute dispatch boundary");
                else
                {
                    passed &= Check(Dispatches == 0, "failed renderer attempted compute dispatch");
                    if (!successful)
                    {
                        // Same explicit replacement used by the frontend update method.
                        nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
                        passed &= Check(SoftwareFrame(*nds), "shader fallback did not produce RAM framebuffers");
                        passed &= Check(Dispatches == 0, "software fallback attempted compute dispatch");
                    }
                }
            }
        }
    }
    // Real driver errors (including failed-object cleanup errors) remain visible.
    unsigned errors = 0;
    for (GLenum error; (error = glGetError()) != GL_NO_ERROR; ++errors)
        std::fprintf(stderr, "ComputeFailure: GL error 0x%04x\n", error);
    passed &= Check(errors == 0, "GL error during failure handling or teardown");
    std::printf("compute_failure=%s injected=%d compile_failures=%u link_failures=%u sources=%u dispatch_attempts=%u %s\n",
                name, Injected, ObservedCompileFailures, ObservedLinkFailures,
                ComputeSources, Dispatches, passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
