// SPDX-License-Identifier: GPL-3.0-or-later
// Real renderer/frontend allocation boundaries in the existing current context.
#include "NDS.h"
#include "GPU_OpenGL.h"
#include "GPU_Soft.h"
#include <SDL2/SDL.h>
#include <cstdio>
#include <cstring>
#include <memory>
#include <stdexcept>

namespace AllocationFixture
{
using namespace melonDS;
enum { renderer3D_Software, renderer3D_OpenGL, renderer3D_OpenGLCompute };
struct Config
{
    int Scale = 1;
    int GetInt(const char* key) const
    {
        if (!std::strcmp(key, "3D.GL.ScaleFactor")) return Scale;
        if (!std::strcmp(key, "3D.Soft.PixelConversion")) return 0;
        throw std::runtime_error("unexpected config key");
    }
    bool GetBool(const char*) const { return false; }
};
struct Instance
{
    NDS* nds;
    Config Settings;
    unsigned Errors = 0;
    Config& getGlobalConfig() { return Settings; }
    void osdAddMessage(u32 color, const char*, ...) { if (color) ++Errors; }
};
struct EmuThread
{
    Instance* emuInstance;
    int videoRenderer = renderer3D_OpenGLCompute;
    int lastVideoRenderer = renderer3D_Software;
    bool PublishedFailure = false;
    void publishVideoSettings(bool failed = false) { PublishedFailure = failed; }
    void updateRenderer();
};
#include "computeUpdateRenderer.inc"

enum class Fault { None, TextureLimit, ViewportLimit, StorageLimit, TexelLimit, Buffer, Texture, OOM, InvalidScale,
                   CaptureTexture, ObjectDepth, ClassicDepth };
Fault ActiveFault;
bool Injected;
GLenum PendingError;
unsigned Allocations, AfterFailure, Sources, Dispatches;
PFNGLGETINTEGERVPROC DriverGetInt;
PFNGLGETINTEGER64VPROC DriverGetInt64;
PFNGLBUFFERDATAPROC DriverBufferData;
PFNGLTEXSTORAGE2DPROC DriverTexStorage;
PFNGLTEXIMAGE2DPROC DriverTexImage2D;
PFNGLTEXIMAGE3DPROC DriverTexImage3D;
PFNGLGETERRORPROC DriverGetError;
PFNGLSHADERSOURCEPROC DriverShaderSource;
PFNGLDISPATCHCOMPUTEPROC DriverDispatch;
PFNGLDISPATCHCOMPUTEINDIRECTPROC DriverIndirect;

void APIENTRY GetInt(GLenum key, GLint* value)
{
    DriverGetInt(key, value);
    if (ActiveFault == Fault::TextureLimit && key == GL_MAX_TEXTURE_SIZE) *value = 256;
    // 384 is enough for the 2x screen, but not its 512-high capture viewport.
    if (ActiveFault == Fault::ViewportLimit && key == GL_MAX_VIEWPORT_DIMS) value[1] = 384;
    if (ActiveFault == Fault::TexelLimit && key == GL_MAX_TEXTURE_BUFFER_SIZE) *value = 131072;
}
void APIENTRY GetInt64(GLenum key, GLint64* value)
{
    DriverGetInt64(key, value);
    // 1x fits, 2x X-span storage does not. Units are bytes, not texels.
    if (ActiveFault == Fault::StorageLimit && key == GL_MAX_SHADER_STORAGE_BLOCK_SIZE)
        *value = 16 * 1024 * 1024;
}
void APIENTRY BufferData(GLenum target, GLsizeiptr size, const void* data, GLenum usage)
{
    ++Allocations;
    if (Injected) ++AfterFailure;
    if (!Injected && target == GL_SHADER_STORAGE_BUFFER &&
        (ActiveFault == Fault::Buffer || ActiveFault == Fault::OOM))
    {
        Injected = true;
        if (ActiveFault == Fault::Buffer)
            DriverBufferData(target, -1, data, usage); // real driver INVALID_VALUE
        else
            PendingError = GL_OUT_OF_MEMORY; // simulated pressure, no physical exhaustion
        return;
    }
    DriverBufferData(target, size, data, usage);
}
void APIENTRY TexStorage(GLenum target, GLsizei levels, GLenum format, GLsizei w, GLsizei h)
{
    ++Allocations;
    if (Injected) ++AfterFailure;
    if (!Injected && ActiveFault == Fault::Texture)
    {
        Injected = true;
        DriverTexStorage(target, levels, format, 0, h); // real driver INVALID_VALUE
        return;
    }
    DriverTexStorage(target, levels, format, w, h);
}
void APIENTRY TexImage2D(GLenum target, GLint level, GLint format, GLsizei w, GLsizei h,
                        GLint border, GLenum pixels, GLenum type, const void* data)
{
    ++Allocations;
    if (Injected) ++AfterFailure;
    if (!Injected && ((ActiveFault == Fault::ObjectDepth && format == GL_DEPTH_COMPONENT16) ||
                     (ActiveFault == Fault::ClassicDepth && format == GL_DEPTH24_STENCIL8)))
    {
        Injected = true;
        w = -1; // real driver error, no storage mutation
    }
    DriverTexImage2D(target, level, format, w, h, border, pixels, type, data);
}
void APIENTRY TexImage3D(GLenum target, GLint level, GLint format, GLsizei w, GLsizei h,
                        GLsizei depth, GLint border, GLenum pixels, GLenum type, const void* data)
{
    ++Allocations;
    if (Injected) ++AfterFailure;
    if (!Injected && ActiveFault == Fault::CaptureTexture)
    {
        Injected = true;
        w = -1;
    }
    DriverTexImage3D(target, level, format, w, h, depth, border, pixels, type, data);
}
GLenum APIENTRY GetError()
{
    if (PendingError) { const GLenum error = PendingError; PendingError = 0; return error; }
    return DriverGetError();
}
void APIENTRY ShaderSource(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths)
{
    ++Sources;
    DriverShaderSource(shader, count, strings, lengths);
}
void APIENTRY Dispatch(GLuint x, GLuint y, GLuint z)
{
    ++Dispatches;
    DriverDispatch(x, y, z);
}
void APIENTRY Indirect(GLintptr offset)
{
    ++Dispatches;
    DriverIndirect(offset);
}
struct Hooks
{
    Hooks()
    {
        DriverGetInt = glad_glGetIntegerv; glad_glGetIntegerv = GetInt;
        DriverGetInt64 = glad_glGetInteger64v; glad_glGetInteger64v = GetInt64;
        DriverBufferData = glad_glBufferData; glad_glBufferData = BufferData;
        DriverTexStorage = glad_glTexStorage2D; glad_glTexStorage2D = TexStorage;
        DriverTexImage2D = glad_glTexImage2D; glad_glTexImage2D = TexImage2D;
        DriverTexImage3D = glad_glTexImage3D; glad_glTexImage3D = TexImage3D;
        DriverGetError = glad_glGetError; glad_glGetError = GetError;
        DriverShaderSource = glad_glShaderSource; glad_glShaderSource = ShaderSource;
        DriverDispatch = glad_glDispatchCompute; glad_glDispatchCompute = Dispatch;
        DriverIndirect = glad_glDispatchComputeIndirect; glad_glDispatchComputeIndirect = Indirect;
    }
    ~Hooks()
    {
        glad_glGetIntegerv = DriverGetInt; glad_glGetInteger64v = DriverGetInt64;
        glad_glBufferData = DriverBufferData; glad_glTexStorage2D = DriverTexStorage;
        glad_glTexImage2D = DriverTexImage2D; glad_glTexImage3D = DriverTexImage3D;
        glad_glGetError = DriverGetError; glad_glShaderSource = DriverShaderSource;
        glad_glDispatchCompute = DriverDispatch; glad_glDispatchComputeIndirect = DriverIndirect;
    }
};
bool Check(bool value, const char* message)
{
    if (!value) std::fprintf(stderr, "GLAllocationFailure: %s\n", message);
    return value;
}
bool Compile(Renderer& renderer)
{
    for (int i = 0; renderer.NeedsShaderCompile() && i < 40; ++i)
    {
        int step, count;
        if (!renderer.ShaderCompileStep(step, count)) return false;
    }
    return !renderer.NeedsShaderCompile();
}
bool Frame(NDS& nds, bool software)
{
    nds.ARM9Write32(0x02000000, 0xEAFFFFFE);
    nds.ARM9Write32(0x02000200, 0xEAFFFFFE);
    nds.ARM9.JumpTo(0x02000000); nds.ARM7.JumpTo(0x02000200);
    nds.Start();
    if (!nds.RunFrame()) return false;
    void* top = nullptr; void* bottom = nullptr;
    if (nds.GetRenderer().GetFramebuffers(&top, &bottom) != software || !top) return false;
    if (software) return bottom != nullptr;
    // GL exposes one array texture through top; bottom is deliberately null.
    const GLuint texture = *static_cast<GLuint*>(top);
    glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
    GLint width = 0, height = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &width);
    glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &height);
    return glIsTexture(texture) && width >= 256 && height >= 192;
}
}

int CheckGLAllocationFailure(const char* name)
{
    using namespace AllocationFixture;
    Fault fault;
    if (!std::strcmp(name, "control")) fault = Fault::None;
    else if (!std::strcmp(name, "texture-limit")) fault = Fault::TextureLimit;
    else if (!std::strcmp(name, "viewport-limit")) fault = Fault::ViewportLimit;
    else if (!std::strcmp(name, "ssbo-limit")) fault = Fault::StorageLimit;
    else if (!std::strcmp(name, "texel-limit")) fault = Fault::TexelLimit;
    else if (!std::strcmp(name, "buffer-fail")) fault = Fault::Buffer;
    else if (!std::strcmp(name, "texture-fail")) fault = Fault::Texture;
    else if (!std::strcmp(name, "oom")) fault = Fault::OOM;
    else if (!std::strcmp(name, "invalid-scale")) fault = Fault::InvalidScale;
    else if (!std::strcmp(name, "capture-fail")) fault = Fault::CaptureTexture;
    else if (!std::strcmp(name, "2d-fail")) fault = Fault::ObjectDepth;
    else if (!std::strcmp(name, "classic-fail")) fault = Fault::ClassicDepth;
    else return 2;
    if (!GLAD_GL_VERSION_4_3) return 77;
    ActiveFault = Fault::None; Injected = false; PendingError = 0;
    Hooks hooks;
    NDSArgs args; args.JIT = std::nullopt;
    auto nds = std::make_unique<NDS>(std::move(args)); nds->Reset();
    Instance instance{nds.get()}; EmuThread thread{&instance};
    const int selectedRenderer = fault == Fault::ClassicDepth ? renderer3D_OpenGL : renderer3D_OpenGLCompute;
    thread.videoRenderer = selectedRenderer;
    thread.updateRenderer();
    if (!dynamic_cast<GLRenderer*>(&nds->GetRenderer()) || !Compile(nds->GetRenderer()) || !Frame(*nds, false)) return 2;
    // Keep a real source-B capture pending when settings fail. Replacement must
    // preserve the guest's pixels even if some host storage was already resized.
    nds->ARM9Write16(0x04000304, 0x020F);
    nds->ARM9Write8(0x04000240, 0x80);
    nds->ARM9Write8(0x04000241, 0x80);
    nds->ARM9Write32(0x04000000, 0x00010000);
    nds->ARM9Write16(0x06800000, 0x801F);
    nds->ARM9Write16(0x06820000, 0xDEAD);
    nds->ARM9Write32(0x04000064, 0xA0010000);
    nds->RunFrame();
    if ((nds->GPU.CaptureCnt & (1u << 31)) || nds->GPU.GetCaptureBlock_LCDC(0x20000) < 0) return 2;
    ActiveFault = fault;
    Allocations = AfterFailure = Sources = Dispatches = 0;
    // Huge config input is only used once a preflight validator exists; 0 is a
    // bounded baseline counterexample without causing physical VRAM exhaustion.
    instance.Settings.Scale = fault == Fault::InvalidScale ? 0 : 2;
    thread.updateRenderer();
    const bool failed = fault != Fault::None;
    bool passed = Check((dynamic_cast<SoftRenderer*>(&nds->GetRenderer()) != nullptr) == failed,
                        "failed settings did not select software rendering");
    passed &= Check(instance.Errors == unsigned(failed), "wrong terminal error count");
    passed &= Check(thread.PublishedFailure == failed, "allocation result was not published to the settings UI");
    if (failed)
    {
        passed &= Check(thread.videoRenderer == renderer3D_Software && thread.lastVideoRenderer == renderer3D_Software,
                        "active frontend renderer state did not recover");
        passed &= Check(!Sources && !Dispatches, "failed settings reached compiler/dispatch");
        const bool allocationFault = fault == Fault::Buffer || fault == Fault::Texture || fault == Fault::OOM ||
            fault == Fault::CaptureTexture || fault == Fault::ObjectDepth || fault == Fault::ClassicDepth;
        passed &= Check(allocationFault ? Injected : Allocations == 0, "fault not injected or preflight allocated");
        passed &= Check(!AfterFailure, "allocations continued after the first failure");
        // Baseline is invalid here; never compile or dispatch its broken storage.
        if (dynamic_cast<SoftRenderer*>(&nds->GetRenderer()))
        {
            passed &= Check(nds->ARM9Read16(0x06820000) == 0x801F, "pending capture lost during failure fallback");
            thread.updateRenderer();
            passed &= Check(instance.Errors == 1 && Frame(*nds, true), "fallback repeated or produced no RAM frame");
        }
    }
    else
        passed &= Check(Compile(nds->GetRenderer()) && Frame(*nds, false), "normal scale control did not render");
    unsigned errors = 0;
    for (GLenum error; (error = glGetError()) != GL_NO_ERROR; ++errors)
        std::fprintf(stderr, "GLAllocationFailure: unhandled GL error %04x\n", error);
    passed &= Check(!errors, "allocation error left unhandled");
    std::printf("allocation=%s injected=%d allocations=%u after_failure=%u errors=%u\n",
                name, Injected, Allocations, AfterFailure, errors);
    // Explicit retry after the bounded fault is removed must produce valid frames.
    ActiveFault = Fault::None; Injected = false;
    instance.Settings.Scale = 1;
    thread.videoRenderer = selectedRenderer;
    if (passed)
    {
        thread.updateRenderer();
        passed &= Check(dynamic_cast<GLRenderer*>(&nds->GetRenderer()) &&
                        Compile(nds->GetRenderer()) && Frame(*nds, false), "explicit retry failed");
        passed &= Check(!thread.PublishedFailure, "successful allocation retry retained the settings failure");
    }
    return passed ? 0 : 1;
}
