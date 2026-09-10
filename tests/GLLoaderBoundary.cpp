// SPDX-License-Identifier: GPL-3.0-or-later
// Real extracted GUI creation calls + Qt borrow handler. Native GL/WGL lookup,
// widgets and config are isolated; no driver pointer is corrupted or invoked.
#include <QCoreApplication>
#include <QMutex>
#include <QQueue>
#include <QSemaphore>
#include <QThread>
#include <QWaitCondition>
#include <QVariant>
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>
#include <vector>

class EmuThread;
class EmuInstance;
class MainWindow;
constexpr int kMaxWindows = 4;
extern EmuInstance* emuInstances[];
MainWindow* topWindow = nullptr;
void broadcastInstanceCommand(int cmd, QVariant& param, int sourceinst);

namespace
{
constexpr int BoundMs = 5000;
[[noreturn]] void StopTest(int result, const char* message)
{
    std::fprintf(stderr, "GLLoaderBoundary: %s\n", message);
    std::fflush(nullptr);
    std::_Exit(result); // Never destruct Qt synchronization under a stuck worker.
}
void Await(QSemaphore& gate)
{
    if (!gate.tryAcquire(1, BoundMs)) StopTest(2, "fixture barrier exceeded bound");
}
struct Observation
{
    bool Recording = false, FailCreation = false, FailCreatedRelease = false;
    EmuInstance* Publishing = nullptr;
    int PublishingWindow = -1;
    std::vector<EmuThread*> Readers;
    std::atomic<bool> LoaderWriting{false};
    std::atomic<unsigned> Overlap{0}, Unpublished{0};
    std::atomic<unsigned> Broadcasts{0};
    std::atomic<bool> BroadcastOnGUI{false};
    unsigned Roots = 0, Shared = 0, DoneCurrent = 0, SettingsUpdates = 0;
} Observe;

struct BoundedCondition
{
    QWaitCondition Real;
    bool wait(QMutex* mutex)
    {
        if (!Real.wait(mutex, static_cast<unsigned long>(BoundMs)))
            StopTest(2, "borrow wait exceeded cleanup bound");
        return true;
    }
    void wakeAll() { Real.wakeAll(); }
};
struct Completion
{
    QSemaphore Real;
    void release() { Real.release(); }
    void acquire() { Await(Real); }
};
struct Action { void setEnabled(bool) {} };
struct Configuration
{
    bool GL = true;
    int Renderer = 1;
    bool GetBool(const char*) const { return GL; }
    int GetInt(const char*) const { return Renderer; }
    void SetBool(const char*, bool value) { GL = value; }
    void SetInt(const char*, int value) { Renderer = value; }
};
}

class EmuInstance
{
public:
    EmuThread* emuThread = nullptr;
    MainWindow* mainWindow = nullptr;
    MainWindow* windowList[kMaxWindows]{};
    int numWindows = 0;
    int preservedFrame = 0;
    unsigned int preservedFrameNumber = 0;
    Configuration Config;
    EmuThread* getEmuThread() { return emuThread; }
    bool usesOpenGL() const { return Config.GL || Config.Renderer != 0; }
    void createWindow(int id = -1);
    void doOnAllWindows(std::function<void(MainWindow*)> func, int exclude = -1);
    int releaseGL();
    void handleCommand(int, QVariant&);
};

class EmuThread final : public QThread
{
public:
    enum MessageType { msg_BorrowGL };
    struct Message { MessageType type; };
    explicit EmuThread(EmuInstance& instance) : emuInstance(&instance) { instance.emuThread = this; }
    bool borrowGL();
    void reportGLFailure(int) { ++Failures; }
    int msgResult = 0;
    unsigned Failures = 0;
    void returnGL();
    void handleMessages();
    void attachWindow(MainWindow*) {}
    void initContext(int);
    bool Borrowed()
    {
        glBorrowMutex.lock();
        const bool held = glBorrowed;
        glBorrowMutex.unlock();
        return held;
    }
    void Start() { start(); Await(Started); }
    void QueueProbe() { ++Requested; ++Pending; Wake.release(); }
    void Drain()
    {
        if (Borrowed()) StopTest(1, "attempted to wait for work on a borrowed worker");
        while (Consumed < Requested) { Await(ProbeDone); ++Consumed; }
    }
    void Finish()
    {
        Drain();
        Stop = true;
        Wake.release();
        if (!wait(BoundMs)) StopTest(2, "worker join exceeded bound");
    }
    unsigned Releases = 0, Requests = 0, Returns = 0, Inits = 0;
    bool BroadcastBeforeWork = false;
    bool FailRelease = false;
#define QWaitCondition BoundedCondition
#include "glBorrowState.inc"
#undef QWaitCondition

private:
    void sendMessage(MessageType type)
    {
        if (Borrowed()) StopTest(1, "nested borrow would wait on an already borrowed worker");
        ++Requests;
        msgMutex.lock();
        msgQueue.enqueue({type});
        msgMutex.unlock();
        Wake.release();
    }
    void waitMessage() { msgSemaphore.acquire(); }
    void run() override
    {
        Started.release();
        for (;;)
        {
            Await(Wake);
            if (Stop) return;
            if (BroadcastBeforeWork)
            {
                BroadcastBeforeWork = false;
                QVariant parameter;
                // B's hotkey can broadcast just before it handles the GUI's
                // borrow request, while A has already acknowledged its loan.
                broadcastInstanceCommand(0, parameter, 1);
            }
            handleMessages();
            const unsigned count = Pending.exchange(0);
            for (unsigned i = 0; i < count; ++i)
            {
                // Model a frontend GL dispatch access without racing real GLAD
                // pointers. All scheduling is real Qt; only the driver is absent.
                if (Observe.LoaderWriting) ++Observe.Overlap;
                if (Observe.Publishing == emuInstance &&
                    !emuInstance->windowList[Observe.PublishingWindow])
                    ++Observe.Unpublished;
                ProbeDone.release();
            }
        }
    }
    EmuInstance* emuInstance;
    QMutex msgMutex;
    QQueue<Message> msgQueue;
    Completion msgSemaphore;
    QSemaphore Wake, Started, ProbeDone;
    std::atomic<unsigned> Pending{0};
    std::atomic<bool> Stop{false};
    unsigned Requested = 0, Consumed = 0;
};

#include "glBorrowHandler.inc"
#include "glBorrowRequest.inc"
#include "glBorrowReturn.inc"

int EmuInstance::releaseGL() { ++emuThread->Releases; return emuThread->FailRelease ? 0 : -1; }
void EmuInstance::handleCommand(int, QVariant&)
{
    if (emuThread->Borrowed()) StopTest(1, "synchronous broadcast would wait on a borrowed peer");
    ++Observe.Broadcasts;
    Observe.BroadcastOnGUI = QThread::currentThread() == QCoreApplication::instance()->thread();
}
void EmuThread::initContext(int)
{
    if (Borrowed()) StopTest(1, "initContext was called before the GUI guard released the worker");
    ++Inits;
    QueueProbe();
    Drain();
}

// Generated from current main.cpp/header. Before the helper exists, only the
// existing registry declarations are emitted; no substitute guard is installed.
#include "glLoaderGuard.inc"
#include "glLoaderBroadcast.inc"

struct WindowInfo {};
namespace GL
{
class Context
{
public:
    enum class Profile { Core };
    struct Version { Profile profile; int major, minor; };
    static std::unique_ptr<Context> Create(const WindowInfo&, const std::array<Version, 2>&);
    std::unique_ptr<Context> CreateSharedContext(const WindowInfo&);
    bool DoneCurrent()
    {
        if (Observe.Recording) ++Observe.DoneCurrent;
        return !Observe.Recording || !Observe.FailCreatedRelease;
    }
};
}

class Panel
{
public:
    virtual ~Panel() = default;
    void show() {}
    void osdSetEnabled(bool) {}
    void setPreservedFrame(int, unsigned int) {}
};
class ScreenPanelGL : public Panel
{
public:
    explicit ScreenPanelGL(MainWindow* window) : Window(window) {}
    bool createContext();
    MainWindow* parentWidget() { return Window; }
    std::optional<WindowInfo> getWindowInfo() { return WindowInfo{}; }
    GL::Context* getContext() { return glContext.get(); }
private:
    MainWindow* Window;
    std::unique_ptr<GL::Context> glContext;
};
class ScreenPanelNative : public Panel
{
public:
    explicit ScreenPanelNative(MainWindow*) {}
};
constexpr int renderer3D_Software = 0;
namespace Platform { enum class LogLevel { Error }; }
void Log(Platform::LogLevel, const char*, ...) {}

class MainWindow
{
public:
    MainWindow(int id, EmuInstance* instance, MainWindow* parent)
        : windowID(id), emuInstance(instance), emuThread(instance->getEmuThread()),
          globalCfg(instance->Config), Parent(parent)
    {
        createScreenPanel();
        // After the actual inner call, before the real createWindow publishes
        // this object. If the worker was released too soon, let its queued probe
        // finish HERE; otherwise publication proceeds while it remains borrowed.
        if (Observe.Recording && Observe.Publishing == instance && emuThread->isRunning() &&
            !emuThread->Borrowed())
            emuThread->Drain();
    }
    ~MainWindow() { delete panel; }
    void createScreenPanel();
    MainWindow* parentWidget() { return Parent; }
    int getWindowID() const { return windowID; }
    bool hasOpenGL() const { return hasOGL; }
    GL::Context* getOGLContext()
    {
        auto* gl = dynamic_cast<ScreenPanelGL*>(panel);
        return gl ? gl->getContext() : nullptr;
    }
    void setCentralWidget(Panel*) {}
    void screenLayoutChange() {}
    Action NewWindow, Filtering;
    Action* actNewWindow = &NewWindow;
    Action* actScreenFiltering = &Filtering;
    int windowID;
    EmuInstance* emuInstance;
    EmuThread* emuThread;
    Configuration& globalCfg;
    MainWindow* Parent;
    Panel* panel = nullptr;
    bool hasOGL = false, hasMenu = false, showOSD = false;
};

#define connect(...) static_cast<void>(0)
#include "glLoaderCreatePanel.inc"
#undef connect
#include "glLoaderCreateContext.inc"
struct QueuedSettingsBoundary
{
    // QWidget/queued recovery is exercised by the separate whole-Qt driver.
    template<class... Args> static void invokeMethod(Args&&...) { ++Observe.SettingsUpdates; }
};
#define QMetaObject QueuedSettingsBoundary
#include "glLoaderCreateWindow.inc"
#undef QMetaObject

void EmuInstance::doOnAllWindows(std::function<void(MainWindow*)> func, int exclude)
{
    for (int i = 0; i < kMaxWindows; ++i)
        if (windowList[i] && i != exclude) func(windowList[i]);
}

std::unique_ptr<GL::Context> NativeBoundary(bool root)
{
    if (!Observe.Recording) return std::make_unique<GL::Context>();
    if (root) ++Observe.Roots;
    else ++Observe.Shared;
    // Shared creation does NOT reload common GLAD. Only root creation is
    // treated as a writer; shared checks nesting and publication independently.
    Observe.LoaderWriting = root;
    for (auto* reader : Observe.Readers)
    {
        if (!reader->isRunning()) continue;
        reader->QueueProbe();
        if (!reader->Borrowed()) reader->Drain();
    }
    Observe.LoaderWriting = false;
    if (Observe.FailCreation) return nullptr;
    return std::make_unique<GL::Context>();
}
std::unique_ptr<GL::Context> GL::Context::Create(const WindowInfo&, const std::array<Version, 2>&)
{
    return NativeBoundary(true);
}
std::unique_ptr<GL::Context> GL::Context::CreateSharedContext(const WindowInfo&)
{
    return NativeBoundary(false);
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    const std::string_view mode = argv[1];
    if (mode != "root" && mode != "replace" && mode != "shared" &&
        mode != "unregistered" && mode != "failure" && mode != "idle" && mode != "broadcast" &&
        mode != "release-failure" && mode != "release-nested" &&
        mode != "release-created-root" && mode != "release-created-shared") return 2;

    EmuInstance first, second, constructing;
    EmuThread a(first), b(second), c(constructing);
    emuInstances[0] = &first;
    emuInstances[1] = &second;
    first.createWindow(0);
    second.createWindow(0);
    Observe.Readers = {&a, &b, &c};
    b.BroadcastBeforeWork = mode == "broadcast";
    if (mode != "idle") { a.Start(); b.Start(); }
    if (mode == "unregistered") emuInstances[0] = nullptr;
    Observe.FailCreation = mode == "failure";
    Observe.FailCreatedRelease = mode == "release-created-root" || mode == "release-created-shared";
    b.FailRelease = mode == "release-failure" || mode == "release-nested";
    Observe.Recording = true;

    MainWindow* result = nullptr;
    bool outerRetained = true;
    const bool shared = mode == "shared" || mode == "unregistered" || mode == "release-created-shared";
    if (mode == "release-nested")
    {
        ScopedGLWorkers outer(&a, false);
        if (!outer) StopTest(2, "normal outer loan failed");
        constructing.createWindow(0);
        result = constructing.mainWindow;
        outerRetained = a.Borrowed() && !b.Borrowed();
    }
    else if (shared)
    {
        Observe.Publishing = &first;
        Observe.PublishingWindow = 1;
        first.createWindow(1);
        result = first.windowList[1];
    }
    else if (mode == "replace")
    {
        first.mainWindow->createScreenPanel();
        result = first.mainWindow;
    }
    else
    {
        constructing.createWindow(0); // c is not running or globally registered.
        result = constructing.mainWindow;
    }

    // A guard must end before queued init work, external close, or waiting here.
    for (auto* reader : Observe.Readers)
        if (reader->isRunning()) reader->Drain();
    app.processEvents(); // Outside every loan: deliver queued worker broadcasts.
    bool passed = Observe.Overlap == 0 && Observe.Unpublished == 0 && result && result->panel;
    if (mode == "release-failure" || mode == "release-nested")
    {
        passed = !result && !constructing.numWindows && Observe.Roots == 0 && Observe.Shared == 0 &&
                 !a.Borrowed() && !b.Borrowed() && a.Requests == 1 && b.Requests == 1 &&
                 b.Failures == 1 && outerRetained;
        std::printf("%s: published=%d loader-entries=%u partial-loan-returned=%d outer-retained=%d result=%s\n",
                    argv[1], result != nullptr, Observe.Roots + Observe.Shared, !a.Borrowed(), outerRetained,
                    passed ? "PASS" : "FAIL");
        b.FailRelease = false;
        constructing.createWindow(0);
        passed &= constructing.numWindows == 1 && constructing.mainWindow &&
                  constructing.mainWindow->hasOpenGL() && !a.Borrowed() && !b.Borrowed();
        std::printf("retry after failed release: %s\n", passed ? "PASS" : "FAIL");
        Observe.Recording = false;
        for (auto* reader : Observe.Readers) if (reader->isRunning()) reader->Finish();
        for (auto* instance : {&first, &second, &constructing})
            for (auto* window : instance->windowList) delete window;
        return passed ? 0 : 1;
    }
    passed &= result && result->hasOpenGL() == !(Observe.FailCreation || Observe.FailCreatedRelease);
    passed &= Observe.Roots == (shared ? 0u : 1u) && Observe.Shared == (shared ? 1u : 0u);
    passed &= Observe.DoneCurrent == (Observe.FailCreation ? 0u : 1u);
    passed &= Observe.SettingsUpdates == (Observe.FailCreation || Observe.FailCreatedRelease ? 1u : 0u);
    if (shared) passed &= a.Inits == (Observe.FailCreatedRelease ? 0u : 1u);
    if (mode == "broadcast") passed &= Observe.Broadcasts == 1 && Observe.BroadcastOnGUI;

    std::printf("GLLoaderBoundary %s: loader_overlap=%u before_publication=%u roots=%u shared=%u "
                "done_current=%u borrow_requests=%u,%u,%u init_calls=%u result=%s\n",
                argv[1], Observe.Overlap.load(), Observe.Unpublished.load(), Observe.Roots, Observe.Shared,
                Observe.DoneCurrent, a.Requests, b.Requests, c.Requests, a.Inits, passed ? "PASS" : "FAIL");
    Observe.Recording = false;
    for (auto* reader : Observe.Readers) if (reader->isRunning()) reader->Finish();
    for (auto* instance : {&first, &second, &constructing})
        for (auto* window : instance->windowList) delete window;
    emuInstances[0] = emuInstances[1] = nullptr;
    return passed ? 0 : 1;
}
