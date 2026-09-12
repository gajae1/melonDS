// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the current message dispatcher with scripted core results. Core
// memory/CPU recovery is covered separately by SavestateLoad.
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <limits>
#include <new>
#include <list>
#include <optional>
#include <variant>
#include <vector>
#include <utility>
#include <stop_token>
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QMutex>
#include <QSemaphore>
#include <QWaitCondition>
#include <QQueue>
#include <QVariant>
#include "NDSCart.h"
#include "GBACart.h"
#include "Platform.h"
#include "AssetIdentity.h"
// Dependencies are already included: expose only the dispatcher's state.
#define private public
#include "EmuThread.h"
#undef private
using namespace melonDS;
using Platform::FileLength;
using Platform::CloseFile;

enum class ImportFailure { None, Short, Error, Oversize };
constexpr int renderer3D_Software = 0;
static ImportFailure importFailure = ImportFailure::None;
static unsigned openFiles = 0;
static bool failAllocation = false;
void* operator new[](std::size_t size)
{
    if (std::exchange(failAllocation, false)) throw std::bad_alloc();
    if (auto* data = std::malloc(size ? size : 1)) return data;
    throw std::bad_alloc();
}
void operator delete[](void* data) noexcept { std::free(data); }
void operator delete[](void* data, std::size_t) noexcept { std::free(data); }
namespace melonDS::Platform
{
FileHandle* OpenImportFile(const std::string& path, FileMode)
{
    auto file = std::make_unique<QFile>(QString::fromStdString(path));
    if (!file->open(QIODevice::ReadOnly)) return nullptr;
    ++openFiles;
    return reinterpret_cast<FileHandle*>(file.release());
}
bool CloseImportFile(FileHandle* file)
{ delete reinterpret_cast<QFile*>(file); --openFiles; return true; }
u64 ImportFileLength(FileHandle* file)
{
    return importFailure == ImportFailure::Oversize ? (u64(1) << 32) + 32 :
        reinterpret_cast<QFile*>(file)->size();
}
void RewindImportFile(FileHandle* file) { reinterpret_cast<QFile*>(file)->seek(0); }
u64 ReadImportFile(void* data, u64 size, u64 count, FileHandle* file)
{
    if (!size || !count) return 0;
    if (importFailure == ImportFailure::Error) return u64(-1);
    auto n = reinterpret_cast<QFile*>(file)->read(static_cast<char*>(data),
        size * count - (importFailure == ImportFailure::Short ? 1 : 0));
    return n > 0 ? n / size : n;
}
}

struct FixtureConsole
{
    bool running = true;
    bool* audio = nullptr;
    bool importedWithAudio = false;
    bool hasCart = true;
    unsigned imports = 0;
    QByteArray save;
    void Start() { running = true; }
    void Stop() { running = false; }
    bool IsRunning() const { return running; }
    void SetNDSSave(const u8* data, u32 length)
    {
        ++imports;
        importedWithAudio |= *audio;
        save = QByteArray(reinterpret_cast<const char*>(data), length);
    }
};

class EmuInstance
{
public:
    int instanceID = 0;
    FixtureConsole console;
    FixtureConsole* nds = &console;
    StateLoadResult result = StateLoadResult::Success;
    bool audio = true;
    bool bootOK = true;
    bool stopOnBootFailure = false;
    std::string lastOSD;
    bool callbacksDuringLoad = false;
    int loads = 0, undos = 0;
    unsigned resets = 0;
    bool currentAvailable = true;
    std::vector<EmuThread::CartLoadRequest> cartLoads;
    std::vector<bool> cartResets;
    QMutex renderLock;

    EmuInstance() { console.audio = &audio; }
    void audioDisable() { audio = false; }
    void audioEnable() { audio = true; }
    void osdAddMessage(unsigned, const char* text) { lastOSD = text; }
    void clearBackupState() {}
    void discardPreservedFrame() {}
    bool reset(const AssetIdentity::Selection& = {}, const AssetIdentity::Selection& = {})
    {
        ++resets; callbacksDuringLoad |= audio;
        if (!bootOK) { if (stopOnBootFailure) nds->Stop(); return false; }
        nds->Start(); return true;
    }
    bool loadROM(QStringList files, bool reset, QString&, const AssetIdentity::Selection& assets,
                 const std::shared_ptr<ROMPreparation::Data>& prepared, std::optional<melonDS::u32> dsSaveType)
    {
        EmuThread::CartLoadRequest request{files, assets, prepared};
        request.DSSaveType = dsSaveType;
        cartLoads.push_back(request);
        cartResets.push_back(reset);
        callbacksDuringLoad |= audio;
        if (!bootOK && stopOnBootFailure) nds->Stop();
        return bootOK;
    }
    bool bootToMenu(QString&) { callbacksDuringLoad |= audio; return bootOK; }
    bool cartInserted() { return console.hasCart; }
    StateLoadResult loadState(const std::string&)
    {
        ++loads;
        callbacksDuringLoad |= audio;
        if (result == StateLoadResult::RecoveryFailed) nds->Stop();
        return result;
    }
    StateLoadResult undoStateLoad()
    {
        ++undos;
        return loadState({});
    }
    bool saveState(const std::string&) { std::abort(); }
    void initOpenGL(int) { std::abort(); }
    bool deinitOpenGL(int) { std::abort(); }
    bool makeCurrentGL() { return currentAvailable; }
    bool preserveFrame() { std::abort(); }
    int releaseGL() { std::abort(); }
    void ejectCart() { std::abort(); }
    bool loadGBAROM(const QStringList&, QString&, const AssetIdentity::Selection&, const std::shared_ptr<ROMPreparation::Data>&, u32) { std::abort(); }
    void loadGBAAddon(int, QString&) { std::abort(); }
    void ejectGBACart() { std::abort(); }
    void enableCheats(bool) { std::abort(); }
};

struct MPInterface
{
    static MPInterface& Get() { static MPInterface instance; return instance; }
    static MPInterface* Acquire() { return &Get(); }
    void End(int) { std::abort(); }
};

// A real Qt object and signals, but no event-loop or device thread is started.
void EmuThread::run() {}
bool EmuThread::initializeGL(int) { std::abort(); }
void EmuThread::setComputeSupport(int) { std::abort(); }
void EmuThread::updateRenderer() { std::abort(); }
#include "statePrepareGL.inc"
#include "stateReportGL.inc"
#include "stateClearGL.inc"
#include "stateThreadConstructor.inc"
#define OpenFile OpenImportFile
#define CloseFile CloseImportFile
#define FileLength ImportFileLength
#define FileRead ReadImportFile
#define FileRewind RewindImportFile
using Platform::ImportFileLength;
using Platform::CloseImportFile;
#include "stateMessages.inc"
#undef FileRewind
#undef FileRead
#undef FileLength
#undef CloseFile
#undef OpenFile
// Synchronous delivery preserves the wrapper's command ordering without starting
// a device thread. The handler and public wrapper are both current source bodies.
void EmuThread::sendMessage(Message message) { msgQueue.enqueue(message); handleMessages(); }
void EmuThread::waitMessage(int count) { if (!msgSemaphore.tryAcquire(count)) std::abort(); }
#include "importSaveWrapper.inc"

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    EmuInstance instance;
    EmuThread thread(&instance);
    unsigned stops = 0, starts = 0, failures = 0;
    QObject::connect(&thread, &EmuThread::windowEmuStop, [&] { ++stops; });
    QObject::connect(&thread, &EmuThread::windowEmuStart, [&] { ++starts; });
    const auto check = [&](bool passed, const char* name)
    {
        if (!passed) { ++failures; std::fprintf(stderr, "%s\n", name); }
    };
    const auto dispatch = [&](EmuThread::MessageType type)
    {
        thread.msgQueue.enqueue({.type = type});
        thread.handleMessages();
        check(thread.msgSemaphore.tryAcquire(), "Message failed to acknowledge completion");
    };
    if (argc == 2 && std::string(argv[1]) == "ds-save-request")
    {
        // Queue distinct QVariant snapshots before consuming any of them.
        // The real dispatcher retains exact chip profiles and legacy codes,
        // including explicit zero separately from Automatic.
        const std::vector<std::optional<u32>> choices = {0, 11, 12, 13, 14, 7, std::nullopt, 2, 3, 4, 1, std::nullopt};
        const int count = static_cast<int>(choices.size());
        std::vector<EmuThread::CartLoadRequest> expected;
        for (int i = 0; i < count; ++i)
        {
            EmuThread::CartLoadRequest request;
            request.Files = {QString("generated-%1.nds").arg(i)};
            request.Assets.Source = request.Files;
            request.Prepared = std::make_shared<ROMPreparation::Data>();
            request.Prepared->Source = request.Files;
            request.DSSaveType = choices[i];
            expected.push_back(request);
            thread.msgQueue.enqueue({.type = i % 2 ? EmuThread::msg_InsertCart : EmuThread::msg_BootROM,
                                     .param = QVariant::fromValue(request)});
            request.DSSaveType = 99;
            request.Files = {"later-reselection.nds"};
        }
        thread.handleMessages();
        check(thread.msgSemaphore.tryAcquire(count) && instance.cartLoads.size() == expected.size(),
              "Queued DS requests did not all reach the loader");
        for (size_t i = 0; i < instance.cartLoads.size() && i < expected.size(); ++i)
        {
            const auto& actual = instance.cartLoads[i];
            check(actual.Files == expected[i].Files && actual.Assets.Source == expected[i].Files &&
                  actual.Prepared == expected[i].Prepared && actual.DSSaveType == choices[i] &&
                  instance.cartResets[i] == (i % 2 == 0),
                  "DS queue consumer lost per-request source/prepared/choice or boot/insert mode");
        }
        std::printf("ds-save-request: %s (actual dispatcher; recorded loader)\n", failures ? "FAIL" : "PASS");
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "gl-gate")
    {
        unsigned graphicsErrors = 0;
        QObject::connect(&thread, &EmuThread::windowOpenGLFailed, [&](int) { ++graphicsErrors; });
        thread.useOpenGL = true;
        dispatch(EmuThread::msg_EmuRun);
        instance.currentAvailable = false;
        for (auto message : {EmuThread::msg_SaveState, EmuThread::msg_LoadState,
                             EmuThread::msg_UndoStateLoad, EmuThread::msg_EmuReset,
                             EmuThread::msg_BootROM, EmuThread::msg_BootFirmware,
                             EmuThread::msg_EmuFrameStep, EmuThread::msg_EmuRun})
        {
            dispatch(message);
            check(thread.msgResult == 0 && instance.resets == 0 && instance.loads == 0 && starts == 1,
                  "A core state consumer escaped the graphics failure gate");
        }
        dispatch(EmuThread::msg_EmuPause);
        dispatch(EmuThread::msg_EmuUnpause);
        check(!instance.audio && thread.hasGLFailure() && graphicsErrors == 1 && thread.emuActive,
              "Graphics failure lost the session, resumed audio, or repeated its error");
        thread.clearGLFailure(1);
        check(thread.hasGLFailure(), "A different window cleared the graphics failure");
        instance.currentAvailable = true;
        thread.clearGLFailure(0);
        dispatch(EmuThread::msg_EmuReset);
        check(!thread.hasGLFailure() && instance.resets == 1 && instance.audio,
              "Context recovery did not restore normal message processing");
        std::printf("graphics state-message gate: %s\n", failures ? "FAIL" : "PASS");
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "boot-failure")
    {
        for (auto message : {EmuThread::msg_EmuReset, EmuThread::msg_BootROM})
        for (bool stopped : {false, true})
        {
            instance.bootOK = true;
            dispatch(EmuThread::msg_EmuReset);
            const auto previousStops = stops;
            instance.bootOK = false;
            instance.stopOnBootFailure = stopped;
            dispatch(message);
            check(thread.msgResult == 0 && instance.audio == !stopped &&
                  instance.console.running == !stopped && thread.emuActive == !stopped &&
                  thread.stateRecoveryFailed == stopped && stops == previousStops + stopped,
                  "Boot failure confused a retained session with a stopped core");
            if (stopped)
            {
                if (message == EmuThread::msg_EmuReset)
                    check(instance.lastOSD.find("retained") == std::string::npos,
                          "Stopped reset reported a retained session");
                for (auto resume : {EmuThread::msg_EmuUnpause, EmuThread::msg_EmuRun,
                                    EmuThread::msg_EmuFrameStep}) dispatch(resume);
                check(!instance.audio && !thread.emuActive &&
                      thread.emuStatus == EmuThread::emuStatus_Paused,
                      "Resume escaped a failed direct boot");
            }
        }
        instance.bootOK = true;
        dispatch(EmuThread::msg_BootROM);
        dispatch(EmuThread::msg_EmuRun);
        check(thread.msgResult == 1 && instance.audio && instance.console.running &&
              thread.emuActive && !thread.stateRecoveryFailed && !instance.callbacksDuringLoad,
              "Successful explicit boot retry did not recover");
        std::printf("direct boot message failure and retry: %s\n", failures ? "FAIL" : "PASS");
        return failures ? 1 : 0;
    }
    if (argc == 2)
    {
        const std::string scenario = argv[1];
        QTemporaryDir directory;
        if (!directory.isValid()) return 2;
        const QString path = directory.filePath("import.sav");
        QByteArray bytes(32, '\xA7');
        if (scenario == "empty") bytes.clear();
        if (scenario != "missing")
        {
            QFile file(path);
            if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return 2;
        }
        if (scenario == "short") importFailure = ImportFailure::Short;
        if (scenario == "error") importFailure = ImportFailure::Error;
        if (scenario == "oversize") importFailure = ImportFailure::Oversize;
        if (scenario == "reset-failure") instance.bootOK = false;
        if (scenario == "no-cart") instance.console.hasCart = false;
        dispatch(EmuThread::msg_EmuRun);
        if (scenario == "paused-failure")
        {
            dispatch(EmuThread::msg_EmuPause);
            importFailure = ImportFailure::Short;
        }
        const auto status = thread.emuStatus;
        const auto pauseDepth = thread.emuPauseStack;
        const bool audio = instance.audio;
        failAllocation = scenario == "allocation";
        const int result = thread.importSavefile(path);
        const bool success = scenario == "normal";
        check(result == success, "Import returned the wrong result");
        check(instance.resets == ((success || scenario == "reset-failure") ? 1u : 0u),
              "Import reset the session before validating its input");
        check(instance.console.imports == (success ? 1u : 0u), "Failed import still applied save bytes");
        check(!instance.callbacksDuringLoad && !instance.console.importedWithAudio,
              "Audio callback was active during import/reset");
        if (success)
            check(instance.console.save == bytes && instance.audio && thread.emuActive,
                  "Successful import lost data or failed to resume");
        else
            check(thread.emuStatus == status && thread.emuPauseStack == pauseDepth && instance.audio == audio,
                  "Failed import changed playback or pause state");
        check(openFiles == 0, "Import leaked its file handle");
        return failures ? 1 : 0;
    }
    dispatch(EmuThread::msg_EmuRun);
    instance.bootOK = false;
    for (auto message : {EmuThread::msg_EmuReset, EmuThread::msg_BootROM, EmuThread::msg_BootFirmware})
    {
        dispatch(message);
        check(thread.msgResult == 0 && instance.audio && thread.emuActive &&
              thread.emuStatus == EmuThread::emuStatus_Running, "Failed reset/boot changed the running session");
    }
    instance.bootOK = true;
    for (auto result : {StateLoadResult::Success, StateLoadResult::SuccessLegacy, StateLoadResult::Failed})
    {
        instance.result = result;
        dispatch(EmuThread::msg_LoadState);
        check(thread.msgResult == static_cast<int>(result) && instance.audio &&
              thread.emuActive && stops == 0, "Safe load result was lost or stopped playback");
        dispatch(EmuThread::msg_EmuPause);
        dispatch(EmuThread::msg_UndoStateLoad);
        check(thread.msgResult == static_cast<int>(result) && !instance.audio &&
              thread.emuStatus == EmuThread::emuStatus_Paused, "Undo failure/result changed paused state");
        dispatch(EmuThread::msg_EmuUnpause);
    }
    for (auto failureMessage : {EmuThread::msg_LoadState, EmuThread::msg_UndoStateLoad})
    {
        instance.result = StateLoadResult::RecoveryFailed;
        dispatch(EmuThread::msg_EmuPause);
        dispatch(failureMessage);
        check(thread.msgResult == static_cast<int>(StateLoadResult::RecoveryFailed) &&
              !instance.audio && !instance.console.running && !thread.emuActive &&
              thread.stateRecoveryFailed, "Recovery failure did not stop the session");
        const auto calls = instance.loads;
        const auto previousStarts = starts;
        for (auto message : {EmuThread::msg_EmuUnpause, EmuThread::msg_EmuRun,
                             EmuThread::msg_EmuFrameStep, EmuThread::msg_LoadState})
            dispatch(message);
        check(!instance.audio && !thread.emuActive && instance.loads == calls &&
              starts == previousStarts && thread.emuStatus == EmuThread::emuStatus_Paused,
              "Queued resume/frame-step/load escaped the recovery stop");
        dispatch(EmuThread::msg_SaveState);
        check(thread.msgResult == 0, "Stopped recovery allowed an invalid state to overwrite a save");
        instance.bootOK = false;
        dispatch(EmuThread::msg_BootROM);
        dispatch(EmuThread::msg_EmuRun);
        check(thread.stateRecoveryFailed && !instance.audio, "Failed boot cleared recovery stop");
        dispatch(EmuThread::msg_EmuReset);
        check(thread.stateRecoveryFailed && !instance.audio, "Failed reset cleared recovery stop");
        instance.bootOK = true;
        if (failureMessage == EmuThread::msg_LoadState)
            dispatch(EmuThread::msg_EmuReset);
        else
        {
            instance.bootOK = true;
            dispatch(EmuThread::msg_BootFirmware);
            dispatch(EmuThread::msg_EmuRun);
        }
        check(!thread.stateRecoveryFailed && thread.emuActive && instance.audio &&
              instance.console.running, "Successful boot/reset did not permit recovery");
    }
    check(!instance.callbacksDuringLoad && instance.undos == 4,
          "Audio callbacks were active during state transfer or undo was skipped");
    std::printf("state message results and recovery stop: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
