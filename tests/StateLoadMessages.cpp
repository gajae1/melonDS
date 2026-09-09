// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the current message dispatcher with scripted core results. Core
// memory/CPU recovery is covered separately by SavestateLoad.
#include <cassert>
#include <cstdlib>
#include <cstdio>
#include <list>
#include <optional>
#include <variant>
#include <QCoreApplication>
#include <QThread>
#include <QMutex>
#include <QSemaphore>
#include <QWaitCondition>
#include <QQueue>
#include <QVariant>
#include "NDSCart.h"
#include "GBACart.h"
#include "Platform.h"
// Dependencies are already included: expose only the dispatcher's state.
#define private public
#include "EmuThread.h"
#undef private
using namespace melonDS;
using Platform::FileLength;
using Platform::CloseFile;

struct FixtureConsole
{
    bool running = true;
    void Start() { running = true; }
    void Stop() { running = false; }
    void SetNDSSave(const u8*, u32) { std::abort(); }
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
    bool callbacksDuringLoad = false;
    int loads = 0, undos = 0;

    void audioDisable() { audio = false; }
    void audioEnable() { audio = true; }
    void osdAddMessage(unsigned, const char*) {}
    void clearBackupState() {}
    bool reset() { callbacksDuringLoad |= audio; if (!bootOK) return false; nds->Start(); return true; }
    bool loadROM(const QStringList&, bool, QString&) { callbacksDuringLoad |= audio; return bootOK; }
    bool bootToMenu(QString&) { callbacksDuringLoad |= audio; return bootOK; }
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
    void deinitOpenGL(int) { std::abort(); }
    void releaseGL() { std::abort(); }
    void ejectCart() { std::abort(); }
    bool loadGBAROM(const QStringList&, QString&) { std::abort(); }
    void loadGBAAddon(int, QString&) { std::abort(); }
    void ejectGBACart() { std::abort(); }
    void enableCheats(bool) { std::abort(); }
};

struct MPInterface
{
    static MPInterface& Get() { static MPInterface instance; return instance; }
    void End(int) { std::abort(); }
};

// A real Qt object and signals, but no event-loop or device thread is started.
void EmuThread::run() {}
#include "stateThreadConstructor.inc"
#include "stateMessages.inc"

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
    dispatch(EmuThread::msg_EmuRun);
    instance.bootOK = false;
    for (auto message : {EmuThread::msg_EmuReset, EmuThread::msg_BootROM, EmuThread::msg_BootFirmware})
    {
        dispatch(message);
        check(thread.msgResult == 0 && instance.audio && thread.emuActive &&
              thread.emuStatus == EmuThread::emuStatus_Running, "Failed reset/boot changed the running session");
    }
    instance.bootOK = true;
    for (auto result : {StateLoadResult::Success, StateLoadResult::Failed})
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
    check(!instance.callbacksDuringLoad && instance.undos == 3,
          "Audio callbacks were active during state transfer or undo was skipped");
    std::printf("state message results and recovery stop: %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
