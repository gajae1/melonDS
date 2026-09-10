// SPDX-License-Identifier: GPL-3.0-or-later
// Actual presentation methods and shaders with an SDL-owned native GL context.
// The Qt window, layout calculation and OSD text queue are isolated here.
#include <SDL.h>
#include <SDL_syswm.h>
#undef main
#include <QApplication>
#include <QImage>
#include <QPixmap>
#include <QMutex>
#include <QThread>
#include <QSemaphore>
#include <QMutexLocker>
#include <QDateTime>
#include <array>
#include <map>
#include <memory>
#include <cstdio>
#include "frontend/glad/glad.h"
#include "frontend/graphics/window_info.h"
#ifdef _WIN32
#include "frontend/graphics/gl/context.h"
#endif
#include "frontend/qt_sdl/main_shaders.h"
#include "frontend/qt_sdl/OSD_shaders.h"
#include "OpenGLSupport.h"
#include "NDS.h"
#include "ARM.h"
#include "GPU_OpenGL.h"
#include "GPU_Soft.h"
#include <cstring>

using namespace melonDS;
struct NativeContext
{
    SDL_Window* window;
    SDL_GLContext context;
    int currentCalls = 0, swaps = 0;
    bool failCurrent = false;
    bool failSwap = false;
#ifdef _WIN32
    std::unique_ptr<GL::Context> native;
    bool Replace()
    {
        native.reset();
        SDL_SysWMinfo info{}; SDL_VERSION(&info.version);
        if (!SDL_GetWindowWMInfo(window, &info)) return false;
        WindowInfo wi{};
        wi.type = WindowInfo::Type::Win32;
        wi.window_handle = info.info.win.window;
        wi.surface_width = 256; wi.surface_height = 192; wi.surface_scale = 1;
        std::array<GL::Context::Version, 1> versions{{{GL::Context::Profile::Core, 4, 3}}};
        native = GL::Context::Create(wi, versions);
        return native && native->DoneCurrent();
    }
#endif
    bool MakeCurrent()
    {
        ++currentCalls;
        if (failCurrent) return false;
#ifdef _WIN32
        if (native) return native->MakeCurrent();
#endif
        return SDL_GL_MakeCurrent(window, context) == 0;
    }
    bool DoneCurrent()
    {
#ifdef _WIN32
        if (native) return native->DoneCurrent();
#endif
        return SDL_GL_MakeCurrent(window, nullptr) == 0;
    }
    bool SwapBuffers()
    {
        ++swaps;
        if (failSwap) return false;
#ifdef _WIN32
        if (native) return native->SwapBuffers();
#endif
        SDL_GL_SwapWindow(window); return true;
    }
    bool IsCurrent() const
    {
#ifdef _WIN32
        return wglGetCurrentContext() != nullptr;
#else
        return SDL_GL_GetCurrentContext() != nullptr;
#endif
    }
};
struct EmuThread { bool emuIsActive() const { return false; } };
struct EmuInstance
{
    EmuThread thread;
    EmuThread* getEmuThread() { return &thread; }
    NDS* getNDS() { return nullptr; } // The test presents the paused splash.
};
struct OSDItem
{
    unsigned id = 0;
    QImage bitmap;
    bool rendered = false;
    qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
    int rainbowstart = 0, rainbowend = 0;
};
class ScreenPanelGL
{
public:
    bool initOpenGL();
    bool deinitOpenGL();
    bool drawScreen();
    void transferLayout() {} // GUI-produced layout snapshot below.
    void osdUpdate();
    void calcSplashLayout() {}
    void osdRenderItem(OSDItem* item)
    {
        item->bitmap = QImage(8, 8, QImage::Format_RGBA8888);
        item->bitmap.fill(Qt::white);
        GLuint tex;
        glGenTextures(1, &tex);
        glBindTexture(GL_TEXTURE_2D, tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 8, 8, 0, GL_RGBA, GL_UNSIGNED_BYTE, item->bitmap.bits());
        osdTextures[item->id] = tex;
    }
    void osdDeleteItem(OSDItem* item)
    {
        if (auto it = osdTextures.find(item->id); it != osdTextures.end())
        {
            glDeleteTextures(1, &it->second);
            osdTextures.erase(it);
        }
    }
    std::unique_ptr<NativeContext> glContext;
    bool glInited = false, glOwned = false;
    std::array<QImage, 2> preservedFrame;
    unsigned int preservedFrameNumber = 0;
    GLuint screenVertexBuffer = 0, screenVertexArray = 0, screenTexture = 0, screenShaderProgram = 0;
    GLint screenShaderTransformULoc = 0, screenShaderScreenSizeULoc = 0;
    QMutex screenSettingsLock;
    WindowInfo windowInfo{};
    int lastScreenWidth = -1, lastScreenHeight = -1;
    GLuint osdShader = 0, osdVertexArray = 0, osdVertexBuffer = 0, logoTexture = 0;
    GLint osdScreenSizeULoc = 0, osdPosULoc = 0, osdSizeULoc = 0, osdScaleFactorULoc = 0, osdTexScaleULoc = 0;
    std::map<unsigned, GLuint> osdTextures;
    EmuInstance* emuInstance;
    QMutex osdMutex;
    QPixmap splashLogo;
    std::array<QPoint, 4> splashPos{};
    std::array<OSDItem, 3> splashText{};
    std::vector<OSDItem> osdItems;
    bool osdEnabled = false, filter = false;
    int numScreens = 0, screenKind[4]{};
    float screenMatrix[4][6]{};
    static constexpr int kLogoWidth = 32, kOSDMargin = 4;
};

#include "presentationInit.inc"
#include "presentationDeinit.inc"
#include "presentationDraw.inc"
#include "presentationOSD.inc"

namespace InitFailure
{
PFNGLSHADERSOURCEPROC driverShaderSource;
PFNGLCREATEPROGRAMPROC driverCreateProgram;
PFNGLGENBUFFERSPROC driverGenBuffers;
PFNGLGENTEXTURESPROC driverGenTextures;
PFNGLGENVERTEXARRAYSPROC driverGenVertexArrays;
std::vector<GLuint> programs, buffers, textures, arrays;
int shaderSources = 0, failSource = 0;
GLuint APIENTRY CreateProgram()
{
    const GLuint id = driverCreateProgram(); programs.push_back(id); return id;
}
void APIENTRY GenBuffers(GLsizei count, GLuint* ids)
{
    driverGenBuffers(count, ids); buffers.insert(buffers.end(), ids, ids + count);
}
void APIENTRY GenTextures(GLsizei count, GLuint* ids)
{
    driverGenTextures(count, ids); textures.insert(textures.end(), ids, ids + count);
}
void APIENTRY GenVertexArrays(GLsizei count, GLuint* ids)
{
    driverGenVertexArrays(count, ids); arrays.insert(arrays.end(), ids, ids + count);
}
void APIENTRY ShaderSource(GLuint shader, GLsizei count, const GLchar* const* strings, const GLint* lengths)
{
    if (++shaderSources == failSource)
    {
        const char* invalid = "#version 150\ninvalid shader input\n";
        driverShaderSource(shader, 1, &invalid, nullptr);
        return;
    }
    driverShaderSource(shader, count, strings, lengths);
}
bool Check(ScreenPanelGL& panel, const char* mode)
{
    failSource = !std::strcmp(mode, "fail-screen") ? 1 : !std::strcmp(mode, "fail-osd") ? 3 : 0;
    panel.glContext->failCurrent = !std::strcmp(mode, "fail-current");
    driverShaderSource = glad_glShaderSource;
    glad_glShaderSource = ShaderSource;
    driverCreateProgram = glad_glCreateProgram; glad_glCreateProgram = CreateProgram;
    driverGenBuffers = glad_glGenBuffers; glad_glGenBuffers = GenBuffers;
    driverGenTextures = glad_glGenTextures; glad_glGenTextures = GenTextures;
    driverGenVertexArrays = glad_glGenVertexArrays; glad_glGenVertexArrays = GenVertexArrays;
    panel.initOpenGL();
    const bool rejected = !panel.glInited;
    const bool noInvalidCalls = !panel.glContext->failCurrent || shaderSources == 0;
    const bool released = !panel.screenShaderProgram && !panel.screenVertexBuffer &&
        !panel.screenVertexArray && !panel.screenTexture && !panel.osdShader &&
        !panel.osdVertexBuffer && !panel.osdVertexArray && !panel.logoTexture;
    const bool detached = !panel.glContext->IsCurrent();
    std::printf("%s: rejected=%d no-invalid-calls=%d partial-cleanup=%d detached=%d\n",
        mode, rejected, noInvalidCalls, released, detached);
    glad_glShaderSource = driverShaderSource;
    glad_glCreateProgram = driverCreateProgram;
    glad_glGenBuffers = driverGenBuffers;
    glad_glGenTextures = driverGenTextures;
    glad_glGenVertexArrays = driverGenVertexArrays;
    panel.glContext->failCurrent = false;
    panel.glContext->MakeCurrent();
    glUseProgram(0); // Complete the driver's deferred deletion of a bound program.
    bool noLiveObjects = true;
    for (GLuint id : programs) noLiveObjects &= !glIsProgram(id);
    for (GLuint id : buffers) noLiveObjects &= !glIsBuffer(id);
    for (GLuint id : textures) noLiveObjects &= !glIsTexture(id);
    for (GLuint id : arrays) noLiveObjects &= !glIsVertexArray(id);
    std::printf("partial-live-objects-zero=%d\n", noLiveObjects);
    // Clean up the baseline only after observing its failure.
    panel.deinitOpenGL();
    panel.glContext->MakeCurrent();
    while (glGetError() != GL_NO_ERROR) {}
    panel.glContext->DoneCurrent();
    panel.initOpenGL();
    panel.drawScreen();
    const bool retried = panel.glInited && glIsProgram(panel.screenShaderProgram) &&
        glIsProgram(panel.osdShader) && glGetError() == GL_NO_ERROR;
    panel.deinitOpenGL();
    panel.deinitOpenGL();
    std::printf("retry=%d\n", retried);
    return rejected && noInvalidCalls && released && detached && noLiveObjects && retried;
}
bool OSD(ScreenPanelGL& panel)
{
    panel.osdEnabled = true;
    panel.osdItems.push_back(OSDItem{.id = 4});
    panel.initOpenGL();
    panel.drawScreen();
    const bool initial = panel.osdTextures.size() == 4 && panel.osdItems[0].rendered;
    panel.deinitOpenGL();
    bool reset = !panel.osdItems[0].rendered && panel.osdTextures.empty();
    for (const auto& item : panel.splashText) reset &= !item.rendered;
    std::printf("osd-reinit: initial=%d rendered-reset=%d\n", initial, reset);
    if (!reset) return false; // Fail before entering the baseline's non-advancing OSD loop.
    panel.initOpenGL();
    panel.drawScreen();
    bool retried = panel.osdTextures.size() == 4 && glGetError() == GL_NO_ERROR;
    for (const auto& [id, tex] : panel.osdTextures) retried &= glIsTexture(tex);
    panel.deinitOpenGL();
    std::printf("osd-retry=%d\n", retried);
    return initial && retried;
}
}

bool RuntimeFailure(ScreenPanelGL& panel, bool current)
{
    panel.initOpenGL();
    const bool normal = panel.drawScreen();
    panel.glContext->DoneCurrent();
    panel.glContext->failCurrent = current;
    panel.glContext->failSwap = !current;
    const int swaps = panel.glContext->swaps;
    const bool rejected = !panel.drawScreen();
    const bool noSwapWithoutCurrent = !current || panel.glContext->swaps == swaps;
    std::printf("runtime-%s: normal=%d rejected=%d no-swap-without-current=%d\n",
        current ? "current" : "swap", normal, rejected, noSwapWithoutCurrent);
    panel.glContext->failCurrent = panel.glContext->failSwap = false;
    panel.glContext->MakeCurrent();
    while (glGetError() != GL_NO_ERROR) {}
    const bool retried = panel.drawScreen() && glGetError() == GL_NO_ERROR;
    panel.deinitOpenGL();
    return normal && rejected && noSwapWithoutCurrent && retried;
}

namespace CoreLifetime
{
enum { renderer3D_Software, renderer3D_OpenGL, renderer3D_OpenGLCompute };
struct Config
{
    int GetInt(const char* key) const { return !std::strcmp(key, "3D.GL.ScaleFactor") ? 1 : 0; }
    bool GetBool(const char*) const { return false; }
};
struct Instance
{
    NDS* nds;
    ScreenPanelGL* panel;
    Config cfg;
    QMutex renderLock;
    Config& getGlobalConfig() { return cfg; }
    void osdAddMessage(u32, const char*, ...) {}
    bool makeCurrentGL() { return panel->glContext->MakeCurrent(); }
    bool preserveFrame() { return true; } // Full Qt recovery probes actual frame snapshots.
    bool deinitOpenGL(int) { return panel->deinitOpenGL(); }
};
struct EmuThread
{
    Instance* emuInstance;
    int videoRenderer, lastVideoRenderer = renderer3D_Software;
    bool useOpenGL = true, videoSettingsDirty = false;
    int unsafeRetires = 0;
    int msgResult = 0, glFailure = -1;
    void publishVideoSettings(bool = false) {}
    void setComputeSupport(int) {}
    void reportGLFailure(int win) { glFailure = win; }
    void clearGLFailure(int win) { if (glFailure == win) glFailure = -1; }
    void updateRenderer();
    void updateRendererBody();
    struct Param { int win; template<class T> T value() const { return win; } };
    void retire(int window)
    {
        struct Message { Param param; } msg{{window}};
        switch (0) { case 0:
#include "presentationDeinitHandler.inc"
        }
    }
};
#define updateRenderer updateRendererBody
#include "presentationRenderer.inc"
#undef updateRenderer
void EmuThread::updateRenderer()
{
    // Observe the invalid consumer before executing GL teardown without current.
    if (emuInstance->panel->glContext->failCurrent) { ++unsafeRetires; return; }
    updateRendererBody();
}

bool Check(ScreenPanelGL& panel, int renderer, bool failCurrent = false)
{
    NDSArgs args; args.JIT = std::nullopt;
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    Instance instance{nds.get(), &panel};
    EmuThread thread{&instance, renderer};
    panel.initOpenGL();
    thread.updateRenderer();
    int current, count;
    while (nds->GetRenderer().NeedsShaderCompile())
        if (!nds->GetRenderer().ShaderCompileStep(current, count)) return false;
    if (!dynamic_cast<GLRenderer*>(&nds->GetRenderer())) return false;
    nds->ARM9Write32(0x02000000, 0xEAFFFFFE);
    nds->ARM9Write32(0x02000200, 0xEAFFFFFE);
    nds->ARM9.JumpTo(0x02000000); nds->ARM7.JumpTo(0x02000200);
    nds->Start();
    if (!nds->RunFrame()) return false;
    nds->ARM9Write16(0x04000304, 0x020F);
    nds->ARM9Write8(0x04000240, 0x80);
    nds->ARM9Write8(0x04000241, 0x80);
    nds->ARM9Write32(0x04000000, 0x00010000);
    nds->ARM9Write16(0x06800000, 0x801F);
    nds->ARM9Write16(0x06820000, 0xDEAD);
    nds->ARM9Write32(0x04000064, 0xA0010000);
    nds->RunFrame();
    if ((nds->GPU.CaptureCnt & (1u << 31)) || nds->GPU.GetCaptureBlock_LCDC(0x20000) < 0) return false;
    bool guarded = true;
    if (failCurrent)
    {
        panel.glContext->DoneCurrent();
        panel.glContext->failCurrent = true;
        thread.retire(0);
        guarded = thread.unsafeRetires == 0 && thread.useOpenGL &&
            dynamic_cast<GLRenderer*>(&nds->GetRenderer()) && panel.glInited;
        std::printf("retire-current-failure: unsafe-consumers=%d retained=%d\n", thread.unsafeRetires, guarded);
        panel.glContext->failCurrent = false;
        thread.useOpenGL = true; // Restore only after recording the old handler's failure.
    }
    thread.retire(0);
    const bool software = dynamic_cast<SoftRenderer*>(&nds->GetRenderer()) != nullptr;
    // Inspect CPU RAM directly: a GPU read helper would hide a missing drain.
    const bool captured = nds->GPU.VRAM_B[0] == 0x1F && nds->GPU.VRAM_B[1] == 0x80;
    bool passed = guarded && software && captured && !thread.useOpenGL &&
        thread.videoRenderer == renderer3D_Software && thread.lastVideoRenderer == renderer3D_Software &&
        !panel.glContext->IsCurrent();
    std::printf("core-retire: software=%d capture-preserved=%d active=%d last=%d %s\n",
        software, captured, thread.videoRenderer, thread.lastVideoRenderer, passed ? "PASS" : "FAIL");
    // Recover the baseline only after recording its failure; never destroy a
    // still-owned GL renderer after its native context has gone away.
    panel.glContext->MakeCurrent();
    nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
    nds.reset();
    panel.glContext->DoneCurrent();
    panel.initOpenGL();
    panel.drawScreen();
    passed &= glIsProgram(panel.screenShaderProgram) && glGetError() == GL_NO_ERROR;
    panel.deinitOpenGL();
    return passed;
}
}

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    const bool native = argc > 1 && !std::strcmp(argv[1], "native");
    if (SDL_Init(SDL_INIT_VIDEO) != 0) return 77;
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    auto* window = SDL_CreateWindow("Presentation lifecycle", 0, 0, 256, 192,
                                   (native ? 0 : SDL_WINDOW_OPENGL) | SDL_WINDOW_HIDDEN);
    if (!window) { SDL_Quit(); return 77; }
    auto context = native ? nullptr : SDL_GL_CreateContext(window);
    if (!native && (!context || !gladLoadGLLoader(SDL_GL_GetProcAddress)))
    {
        if (context) SDL_GL_DeleteContext(context);
        SDL_DestroyWindow(window); SDL_Quit(); return 77;
    }
    EmuInstance instance;
    ScreenPanelGL panel;
    panel.emuInstance = &instance;
    panel.glContext = std::make_unique<NativeContext>(window, context);
#ifdef _WIN32
    if (native && !panel.glContext->Replace()) return 77;
#else
    if (native) return 77;
#endif
    panel.windowInfo.surface_width = 256;
    panel.windowInfo.surface_height = 192;
    panel.windowInfo.surface_scale = 1;
    panel.splashLogo = QPixmap(64, 64);
    panel.splashLogo.fill(Qt::red);
    for (int i = 0; i < 3; ++i) panel.splashText[i].id = i + 1;
    panel.glContext->DoneCurrent();

    if (argc > 1 && !native)
    {
        bool passed = false;
        const int renderer = !std::strcmp(argv[1], "compute") ? CoreLifetime::renderer3D_OpenGLCompute : CoreLifetime::renderer3D_OpenGL;
        auto worker = std::unique_ptr<QThread>(QThread::create([&] {
            if (!std::strncmp(argv[1], "fail-", 5)) passed = InitFailure::Check(panel, argv[1]);
            else if (!std::strcmp(argv[1], "osd-reinit")) passed = InitFailure::OSD(panel);
            else if (!std::strcmp(argv[1], "runtime-current")) passed = RuntimeFailure(panel, true);
            else if (!std::strcmp(argv[1], "runtime-swap")) passed = RuntimeFailure(panel, false);
            else if (!std::strcmp(argv[1], "retire-current")) passed = CoreLifetime::Check(panel, renderer, true);
            else passed = CoreLifetime::Check(panel, renderer);
        }));
        worker->start();
        if (!worker->wait(15000)) std::_Exit(2);
        SDL_GL_DeleteContext(context); SDL_DestroyWindow(window); SDL_Quit();
        return passed ? 0 : 1;
    }

    QSemaphore deinitialized, resume;
    bool passed = true, initialControl = false, reinitControl = false;
    int callsAfterDeinit = -1, swapsAfterDeinit = -1;
    auto worker = std::unique_ptr<QThread>(QThread::create([&] {
        panel.initOpenGL();
        panel.drawScreen();
        initialControl = panel.glInited && glIsProgram(panel.screenShaderProgram) &&
                  glIsProgram(panel.osdShader) && panel.glContext->swaps == 1 && glGetError() == GL_NO_ERROR;
        passed &= initialControl;
        panel.deinitOpenGL();
        passed &= !panel.glInited && !panel.glContext->IsCurrent();
        deinitialized.release(); // Same ordering as the frontend deinit acknowledgement.
        if (!resume.tryAcquire(1, 5000)) std::_Exit(2);
        const int calls = panel.glContext->currentCalls, swaps = panel.glContext->swaps;
        panel.drawScreen(); // Paused iteration before the GUI can replace the old panel.
        callsAfterDeinit = panel.glContext->currentCalls - calls;
        swapsAfterDeinit = panel.glContext->swaps - swaps;
        passed &= callsAfterDeinit == 0 && swapsAfterDeinit == 0 && !panel.glContext->IsCurrent();
        // Do not carry the RED path's invalid calls into the positive reinit control.
        panel.glContext->MakeCurrent();
        while (glGetError() != GL_NO_ERROR) {}
        panel.glContext->DoneCurrent();
        panel.initOpenGL();
        panel.drawScreen();
        reinitControl = panel.glInited && glIsProgram(panel.screenShaderProgram) && glGetError() == GL_NO_ERROR;
        passed &= reinitControl;
        panel.deinitOpenGL();
        passed &= !panel.glContext->IsCurrent();
    }));
    worker->start();
    if (!deinitialized.tryAcquire(1, 5000)) std::_Exit(2);
#ifdef _WIN32
    // Destroy/recreate through the production WGL backend on the GUI thread,
    // while the worker is held and has acknowledged DoneCurrent.
    if (native && !panel.glContext->Replace()) std::_Exit(2);
#endif
    resume.release();
    if (!worker->wait(5000)) std::_Exit(2);
    std::printf("presentation-deinit: current-after=%d swap-after=%d initial=%d reinit=%d %s\n",
                callsAfterDeinit, swapsAfterDeinit, initialControl, reinitControl, passed ? "PASS" : "FAIL");
    panel.glContext.reset();
    if (context) SDL_GL_DeleteContext(context);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return passed ? 0 : 1;
}
