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
struct OSDItem { unsigned id = 0; QImage bitmap; };
class ScreenPanelGL
{
public:
    void initOpenGL();
    void deinitOpenGL();
    void drawScreen();
    void transferLayout() {} // GUI-produced layout snapshot below.
    void osdUpdate() {}
    std::unique_ptr<NativeContext> glContext;
    bool glInited = false;
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
    void makeCurrentGL() { panel->glContext->MakeCurrent(); }
    void deinitOpenGL(int) { panel->deinitOpenGL(); }
};
struct EmuThread
{
    Instance* emuInstance;
    int videoRenderer, lastVideoRenderer = renderer3D_Software;
    bool useOpenGL = true, videoSettingsDirty = false;
    void updateRenderer();
    struct Param { int win; template<class T> T value() const { return win; } };
    void retire(int window)
    {
        struct Message { Param param; } msg{{window}};
        switch (0) { case 0:
#include "presentationDeinitHandler.inc"
        }
    }
};
#include "presentationRenderer.inc"

bool Check(ScreenPanelGL& panel, int renderer)
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
    thread.retire(0);
    const bool software = dynamic_cast<SoftRenderer*>(&nds->GetRenderer()) != nullptr;
    // Inspect CPU RAM directly: a GPU read helper would hide a missing drain.
    const bool captured = nds->GPU.VRAM_B[0] == 0x1F && nds->GPU.VRAM_B[1] == 0x80;
    bool passed = software && captured && !thread.useOpenGL &&
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
    panel.glContext->DoneCurrent();

    if (argc > 1 && !native)
    {
        bool passed = false;
        const int renderer = !std::strcmp(argv[1], "compute") ? CoreLifetime::renderer3D_OpenGLCompute : CoreLifetime::renderer3D_OpenGL;
        auto worker = std::unique_ptr<QThread>(QThread::create([&] { passed = CoreLifetime::Check(panel, renderer); }));
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
