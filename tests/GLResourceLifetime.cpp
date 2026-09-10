// SPDX-License-Identifier: GPL-3.0-or-later
// Real GL object lifetime probe. Caller owns a current GL >= 3.2 context.
// No cache, ROM, configuration, rendering/readback, or global allocator override.
#include "NDS.h"
#include "GPU_OpenGL.h"
#include "GPU_Soft.h"

#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <set>
#include <utility>

namespace
{
using namespace melonDS;

struct ObjectNames
{
    unsigned Generated = 0;
    std::set<GLuint> Seen;
    std::set<GLuint> NotDeleted;
};

ObjectNames Programs, Buffers, Textures;
bool Recording;
bool FailNextSource;
GLuint FailedShader;
unsigned ActualCompileFailures;
unsigned ForeignDeletes;
GLuint ControlProgram, ControlBuffer, ControlTexture;

PFNGLCREATEPROGRAMPROC DriverCreateProgram;
PFNGLGENBUFFERSPROC DriverGenBuffers;
PFNGLGENTEXTURESPROC DriverGenTextures;
PFNGLDELETEPROGRAMPROC DriverDeleteProgram;
PFNGLDELETEBUFFERSPROC DriverDeleteBuffers;
PFNGLDELETETEXTURESPROC DriverDeleteTextures;
PFNGLSHADERSOURCEPROC DriverShaderSource;
PFNGLGETSHADERIVPROC DriverGetShaderiv;

void RecordGenerated(ObjectNames& names, GLsizei count, const GLuint* ids)
{
    if (!Recording) return;
    for (GLsizei i = 0; i < count; ++i)
    {
        if (!ids[i]) continue;
        ++names.Generated;
        names.Seen.insert(ids[i]);
        names.NotDeleted.insert(ids[i]);
    }
}

void RecordDeleted(ObjectNames& names, GLsizei count, const GLuint* ids, GLuint control)
{
    if (!Recording) return;
    for (GLsizei i = 0; i < count; ++i)
    {
        if (!ids[i]) continue;
        names.NotDeleted.erase(ids[i]);
        if (ids[i] == control) ++ForeignDeletes;
    }
}

GLuint APIENTRY CreateProgram()
{
    GLuint id = DriverCreateProgram();
    RecordGenerated(Programs, 1, &id);
    return id;
}

void APIENTRY GenBuffers(GLsizei count, GLuint* ids)
{
    DriverGenBuffers(count, ids);
    RecordGenerated(Buffers, count, ids);
}

void APIENTRY GenTextures(GLsizei count, GLuint* ids)
{
    DriverGenTextures(count, ids);
    RecordGenerated(Textures, count, ids);
}

void APIENTRY DeleteProgram(GLuint id)
{
    RecordDeleted(Programs, 1, &id, ControlProgram);
    DriverDeleteProgram(id);
}

void APIENTRY DeleteBuffers(GLsizei count, const GLuint* ids)
{
    RecordDeleted(Buffers, count, ids, ControlBuffer);
    DriverDeleteBuffers(count, ids);
}

void APIENTRY DeleteTextures(GLsizei count, const GLuint* ids)
{
    RecordDeleted(Textures, count, ids, ControlTexture);
    DriverDeleteTextures(count, ids);
}

void APIENTRY ShaderSource(GLuint shader, GLsizei count,
                          const GLchar* const* strings, const GLint* lengths)
{
    if (Recording && FailNextSource)
    {
        FailNextSource = false;
        FailedShader = shader;
        const char* invalid = "#version 150\n#error deliberate_gl_resource_failure\n";
        DriverShaderSource(shader, 1, &invalid, nullptr);
        return;
    }
    DriverShaderSource(shader, count, strings, lengths);
}

void APIENTRY ShaderStatus(GLuint shader, GLenum pname, GLint* value)
{
    DriverGetShaderiv(shader, pname, value);
    if (Recording && shader == FailedShader && pname == GL_COMPILE_STATUS && *value == GL_FALSE)
        ++ActualCompileFailures;
}

struct DriverHooks
{
    DriverHooks()
    {
        DriverCreateProgram = glad_glCreateProgram;
        DriverGenBuffers = glad_glGenBuffers;
        DriverGenTextures = glad_glGenTextures;
        DriverDeleteProgram = glad_glDeleteProgram;
        DriverDeleteBuffers = glad_glDeleteBuffers;
        DriverDeleteTextures = glad_glDeleteTextures;
        DriverShaderSource = glad_glShaderSource;
        DriverGetShaderiv = glad_glGetShaderiv;
        glad_glCreateProgram = CreateProgram;
        glad_glGenBuffers = GenBuffers;
        glad_glGenTextures = GenTextures;
        glad_glDeleteProgram = DeleteProgram;
        glad_glDeleteBuffers = DeleteBuffers;
        glad_glDeleteTextures = DeleteTextures;
        glad_glShaderSource = ShaderSource;
        glad_glGetShaderiv = ShaderStatus;
    }
    ~DriverHooks()
    {
        Recording = false;
        glad_glCreateProgram = DriverCreateProgram;
        glad_glGenBuffers = DriverGenBuffers;
        glad_glGenTextures = DriverGenTextures;
        glad_glDeleteProgram = DriverDeleteProgram;
        glad_glDeleteBuffers = DriverDeleteBuffers;
        glad_glDeleteTextures = DriverDeleteTextures;
        glad_glShaderSource = DriverShaderSource;
        glad_glGetShaderiv = DriverGetShaderiv;
    }
};

struct Controls
{
    Controls()
    {
        ControlProgram = glCreateProgram();
        glGenBuffers(1, &ControlBuffer);
        glBindBuffer(GL_ARRAY_BUFFER, ControlBuffer);
        const GLuint marker = 0x12345678;
        glBufferData(GL_ARRAY_BUFFER, sizeof(marker), &marker, GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        glGenTextures(1, &ControlTexture);
        glBindTexture(GL_TEXTURE_2D, ControlTexture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1, 1, 0, GL_RGBA, GL_UNSIGNED_BYTE, &marker);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    ~Controls()
    {
        Recording = false;
        // Failure may already have deleted a control. Do not add fixture errors.
        if (glIsProgram(ControlProgram)) glDeleteProgram(ControlProgram);
        glDeleteBuffers(1, &ControlBuffer);
        glDeleteTextures(1, &ControlTexture);
    }
    bool Alive() const
    {
        return glIsProgram(ControlProgram) && glIsBuffer(ControlBuffer) && glIsTexture(ControlTexture);
    }
};

template<class T, class... Args>
std::unique_ptr<T> ConstructOnUsedStorage(GLuint previousName, Args&&... args)
{
    // Simulate reused object storage containing a real, unrelated texture name.
    // The constructor runs normally; no live object field is modified by test.
    // Baseline's uninitialized-handle read is the defect, not a defined C++ value.
    static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);
    void* storage = ::operator new(sizeof(T));
    std::memset(storage, 0, sizeof(T));
    for (size_t offset = 0; offset + sizeof(previousName) <= sizeof(T); offset += sizeof(previousName))
        std::memcpy(static_cast<unsigned char*>(storage) + offset, &previousName, sizeof(previousName));
    try
    {
        return std::unique_ptr<T>(::new (storage) T(std::forward<Args>(args)...));
    }
    catch (...)
    {
        ::operator delete(storage);
        throw;
    }
}

void Begin(bool fail)
{
    Programs = {};
    Buffers = {};
    Textures = {};
    ForeignDeletes = ActualCompileFailures = 0;
    FailedShader = 0;
    FailNextSource = fail;
    Recording = true;
}

bool Check(bool condition, const char* message)
{
    if (!condition) std::fprintf(stderr, "GLResourceLifetime: %s\n", message);
    return condition;
}

template<class IsObject>
bool CheckObjects(const char* kind, const ObjectNames& names, IsObject isObject)
{
    unsigned live = 0;
    for (GLuint id : names.Seen)
    {
        if (!isObject(id)) continue;
        ++live;
        std::fprintf(stderr, "GLResourceLifetime: owned %s %u survived destruction\n", kind, id);
    }
    std::printf("resource=%s generated=%u live=%u without_delete=%zu\n",
                kind, names.Generated, live, names.NotDeleted.size());
    // glIsBuffer/glIsTexture alone misses generated names never first-bound.
    return !live && names.NotDeleted.empty();
}

bool Finish(const Controls& controls)
{
    Recording = false;
    // A deleted current program stays alive until unbound. Release that reference
    // while this same real context is still current, before testing glIsProgram.
    glUseProgram(0);
    glBindVertexArray(0);
    bool good = CheckObjects("program", Programs, glIsProgram);
    good &= CheckObjects("buffer", Buffers, glIsBuffer);
    good &= CheckObjects("texture", Textures, glIsTexture);
    const bool controlsAlive = controls.Alive();
    std::printf("controls_alive=%d foreign_delete_attempts=%u actual_compile_failures=%u\n",
                controlsAlive, ForeignDeletes, ActualCompileFailures);
    good &= Check(controlsAlive && !ForeignDeletes, "unrelated controls were deleted or targeted");
    unsigned errors = 0;
    for (GLenum error; (error = glGetError()) != GL_NO_ERROR; ++errors)
        std::fprintf(stderr, "GLResourceLifetime: GL error 0x%04x\n", error);
    return Check(errors == 0, "GL error during init/destruction") && good;
}
}

// Existing GLFrameReadback target export. Cases: normal, common-fail, 3d-fail.
// 0 pass, 1 semantic failure, 2 fixture setup/fault not observed, 77 unavailable.
int CheckGLResourceLifetime(const char* name)
{
    using namespace melonDS;
    const bool normal = std::strcmp(name, "normal") == 0;
    const bool commonFail = std::strcmp(name, "common-fail") == 0;
    const bool threeDFail = std::strcmp(name, "3d-fail") == 0;
    if (!normal && !commonFail && !threeDFail) return 2;
    if (!GLAD_GL_VERSION_3_2) return 77;
    Recording = false;
    DriverHooks hooks;
    Controls controls;
    if (!controls.Alive() || glGetError() != GL_NO_ERROR) return 2;
    NDSArgs args;
    args.JIT = std::nullopt;
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    bool passed = true;
    if (normal)
    {
        // Two cycles under one still-current context expose repeated leaks.
        for (int cycle = 0; cycle < 2; ++cycle)
        {
            Begin(false);
            nds->SetRenderer(std::make_unique<GLRenderer>(*nds, false));
            if (!dynamic_cast<GLRenderer*>(&nds->GetRenderer())) return 2;
            RendererSettings settings{1, false, false, false};
            nds->GetRenderer().SetRenderSettings(settings);
            // Explicit replacement actually destroys the GL owner. Null does not
            // mean replacement in NDS::SetRenderer, so do not use it here.
            nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
            passed &= Check(Programs.Generated && Buffers.Generated && Textures.Generated,
                            "normal control did not allocate all measured object kinds");
            std::printf("gl_resource_cycle=%d\n", cycle);
            passed &= Finish(controls);
        }
    }
    else if (commonFail)
    {
        Begin(true);
        nds->SetRenderer(ConstructOnUsedStorage<GLRenderer>(ControlTexture, *nds, false));
        if (FailNextSource || ActualCompileFailures != 1) return 2;
        passed &= Check(dynamic_cast<SoftRenderer*>(&nds->GetRenderer()) != nullptr,
                        "failed common Init did not reach actual GPU software fallback");
        passed &= Finish(controls);
    }
    else
    {
        // Give a separate 3D object a valid real parent. Parent setup/teardown is
        // excluded from this case's ledger; normal covers its owned resources.
        nds->SetRenderer(std::make_unique<GLRenderer>(*nds, false));
        auto* parent = dynamic_cast<GLRenderer*>(&nds->GetRenderer());
        if (!parent || glGetError() != GL_NO_ERROR) return 2;
        Begin(true);
        {
            auto renderer = ConstructOnUsedStorage<GLRenderer3D>(ControlTexture, nds->GPU.GPU3D, *parent);
            passed &= Check(!renderer->Init(), "failed 3D Init reported success");
        }
        if (FailNextSource || ActualCompileFailures != 1) return 2;
        passed &= Finish(controls);
        nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
    }
    std::printf("gl_resource=%s %s\n", name, passed ? "PASS" : "FAIL");
    return passed ? 0 : 1;
}
