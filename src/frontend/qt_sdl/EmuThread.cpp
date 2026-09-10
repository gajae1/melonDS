/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <stdlib.h>
#include <time.h>
#include <stdio.h>
#include <string.h>

#include <optional>
#include <vector>
#include <string>
#include <algorithm>
#include <limits>
#include <cmath>

#include <SDL2/SDL.h>

#include "main.h"

#include "types.h"
#include "version.h"

#include "ScreenLayout.h"

#include "Args.h"
#include "NDS.h"
#include "NDSCart.h"
#include "GBACart.h"
#include "GPU.h"
#include "SPU.h"
#include "Wifi.h"
#include "Platform.h"
#include "LocalMP.h"
#include "Config.h"
#include "RTC.h"
#include "DSi.h"
#include "DSi_I2C.h"
#include "GPU_Soft.h"
#include "GPU_OpenGL.h"

#include "Savestate.h"

#include "EmuInstance.h"
#include <QMessageBox>
#include <QPushButton>

using namespace melonDS;


EmuThread::EmuThread(EmuInstance* inst, QObject* parent) : QThread(parent)
{
    emuInstance = inst;

    emuStatus = emuStatus_Paused;
    emuPauseStack = emuPauseStackRunning;
    emuActive = false;
}

void EmuThread::attachWindow(MainWindow* window)
{
    connect(this, SIGNAL(windowTitleChange(QString)), window, SLOT(onTitleUpdate(QString)));
    connect(this, SIGNAL(windowEmuStart()), window, SLOT(onEmuStart()));
    connect(this, SIGNAL(windowEmuStop()), window, SLOT(onEmuStop()));
    connect(this, SIGNAL(windowEmuPause(bool)), window, SLOT(onEmuPause(bool)));
    connect(this, SIGNAL(windowEmuReset()), window, SLOT(onEmuReset()));
    connect(this, SIGNAL(windowOpenGLFailed(int)), window, SLOT(onOpenGLFailed(int)),
            Qt::QueuedConnection);
    connect(this, SIGNAL(autoScreenSizingChange(int)), window->panel, SLOT(onAutoScreenSizingChanged(int)));
    connect(this, SIGNAL(windowFullscreenToggle()), window, SLOT(onFullscreenToggled()));
    connect(this, SIGNAL(screenEmphasisToggle()), window, SLOT(onScreenEmphasisToggled()));

    if (window->winHasMenu())
    {
        connect(this, SIGNAL(windowLimitFPSChange()), window->actLimitFramerate, SLOT(trigger()));
        connect(this, SIGNAL(swapScreensToggle()), window->actScreenSwap, SLOT(trigger()));
    }
}

void EmuThread::detachWindow(MainWindow* window)
{
    disconnect(this, SIGNAL(windowTitleChange(QString)), window, SLOT(onTitleUpdate(QString)));
    disconnect(this, SIGNAL(windowEmuStart()), window, SLOT(onEmuStart()));
    disconnect(this, SIGNAL(windowEmuStop()), window, SLOT(onEmuStop()));
    disconnect(this, SIGNAL(windowEmuPause(bool)), window, SLOT(onEmuPause(bool)));
    disconnect(this, SIGNAL(windowEmuReset()), window, SLOT(onEmuReset()));
    disconnect(this, SIGNAL(windowOpenGLFailed(int)), window, SLOT(onOpenGLFailed(int)));
    disconnect(this, SIGNAL(autoScreenSizingChange(int)), window->panel, SLOT(onAutoScreenSizingChanged(int)));
    disconnect(this, SIGNAL(windowFullscreenToggle()), window, SLOT(onFullscreenToggled()));
    disconnect(this, SIGNAL(screenEmphasisToggle()), window, SLOT(onScreenEmphasisToggled()));

    if (window->winHasMenu())
    {
        disconnect(this, SIGNAL(windowLimitFPSChange()), window->actLimitFramerate, SLOT(trigger()));
        disconnect(this, SIGNAL(swapScreensToggle()), window->actScreenSwap, SLOT(trigger()));
    }
}

void EmuThread::run()
{
    Config::Table& globalCfg = emuInstance->getGlobalConfig();
    u32 mainScreenPos[3];

    //emuInstance->updateConsole();
    // No carts are inserted when melonDS first boots

    mainScreenPos[0] = 0;
    mainScreenPos[1] = 0;
    mainScreenPos[2] = 0;
    autoScreenSizing = 0;

    //videoSettingsDirty = false;

    if (emuInstance->usesOpenGL())
    {
        useOpenGL = initializeGL(0);
        videoRenderer = useOpenGL ? globalCfg.GetInt("3D.Renderer") : renderer3D_Software;
    }
    else
    {
        useOpenGL = false;
        videoRenderer = 0;
    }

    //updateRenderer();
    videoSettingsDirty = true;

    u32 nframes = 0;
    double perfCountsSec = 1.0 / SDL_GetPerformanceFrequency();
    double lastTime = SDL_GetPerformanceCounter() * perfCountsSec;
    double frameLimitError = 0.0;
    double lastMeasureTime = lastTime;

    u32 winUpdateCount = 0, winUpdateFreq = 1;
    u8 dsiVolumeLevel = 0x1F;

    char melontitle[100];

    bool fastforward = false;
    bool slowmo = false;
    emuInstance->fastForwardToggled = false;
    emuInstance->slowmoToggled = false;

    while (emuStatus != emuStatus_Exit)
    {
        if (emuInstance->instanceID == 0)
            MPInterface::Get().Process();

        emuInstance->inputProcess();

        if (emuInstance->hotkeyPressed(HK_FrameLimitToggle)) emit windowLimitFPSChange();

        if (emuInstance->hotkeyPressed(HK_Pause)) emuTogglePause();
        if (emuInstance->hotkeyPressed(HK_Reset)) emuReset();
        if (emuInstance->hotkeyPressed(HK_FrameStep)) emuFrameStep();

        if (emuInstance->hotkeyPressed(HK_FullscreenToggle)) emit windowFullscreenToggle();

        if (emuInstance->hotkeyPressed(HK_SwapScreens)) emit swapScreensToggle();
        if (emuInstance->hotkeyPressed(HK_SwapScreenEmphasis)) emit screenEmphasisToggle();

        if (!prepareGL())
        {
            handleMessages();
            SDL_Delay(20);
            continue;
        }

        if (emuStatus == emuStatus_Running || emuStatus == emuStatus_FrameStep)
        {
            if (emuStatus == emuStatus_FrameStep) emuStatus = emuStatus_Paused;

            if (emuInstance->hotkeyPressed(HK_SolarSensorDecrease))
            {
                int level = emuInstance->nds->GBACartSlot.SetInput(GBACart::Input_SolarSensorDown, true);
                if (level != -1)
                {
                    emuInstance->osdAddMessage(0, "Solar sensor level: %d", level);
                }
            }
            if (emuInstance->hotkeyPressed(HK_SolarSensorIncrease))
            {
                int level = emuInstance->nds->GBACartSlot.SetInput(GBACart::Input_SolarSensorUp, true);
                if (level != -1)
                {
                    emuInstance->osdAddMessage(0, "Solar sensor level: %d", level);
                }
            }

            if (emuInstance->nds->ConsoleType == 1)
            {
                DSi* dsi = static_cast<DSi*>(emuInstance->nds);
                double currentTime = SDL_GetPerformanceCounter() * perfCountsSec;

                // Handle power button
                if (emuInstance->hotkeyDown(HK_PowerButton))
                {
                    dsi->I2C.GetBPTWL()->SetPowerButtonHeld(currentTime);
                }
                else if (emuInstance->hotkeyReleased(HK_PowerButton))
                {
                    dsi->I2C.GetBPTWL()->SetPowerButtonReleased(currentTime);
                }

                // Handle volume buttons
                if (emuInstance->hotkeyDown(HK_VolumeUp))
                {
                    dsi->I2C.GetBPTWL()->SetVolumeSwitchHeld(DSi_BPTWL::volumeKey_Up);
                }
                else if (emuInstance->hotkeyReleased(HK_VolumeUp))
                {
                    dsi->I2C.GetBPTWL()->SetVolumeSwitchReleased(DSi_BPTWL::volumeKey_Up);
                }

                if (emuInstance->hotkeyDown(HK_VolumeDown))
                {
                    dsi->I2C.GetBPTWL()->SetVolumeSwitchHeld(DSi_BPTWL::volumeKey_Down);
                }
                else if (emuInstance->hotkeyReleased(HK_VolumeDown))
                {
                    dsi->I2C.GetBPTWL()->SetVolumeSwitchReleased(DSi_BPTWL::volumeKey_Down);
                }

                dsi->I2C.GetBPTWL()->ProcessVolumeSwitchInput(currentTime);
            }

            // update render settings if needed
            if (videoSettingsDirty)
            {
                emuInstance->renderLock.lock();
                if (useOpenGL)
                {
                    emuInstance->setVSyncGL(true);
                    videoRenderer = globalCfg.GetInt("3D.Renderer");
                }
#ifdef OGLRENDERER_ENABLED
                else
#endif
                {
                    videoRenderer = 0;
                }

                updateRenderer();

                videoSettingsDirty = false;
                emuInstance->renderLock.unlock();
            }

            // process input and hotkeys
            emuInstance->nds->SetKeyMask(emuInstance->inputMask);

            u16 touchX, touchY;
            if (emuInstance->inputGetTouch(touchX, touchY))
                emuInstance->nds->TouchScreen(touchX, touchY);
            else
                emuInstance->nds->ReleaseScreen();

            if (emuInstance->hotkeyPressed(HK_Lid))
            {
                bool lid = !emuInstance->nds->IsLidClosed();
                emuInstance->nds->SetLidClosed(lid);
                emuInstance->osdAddMessage(0, lid ? "Lid closed" : "Lid opened");
            }

            // auto screen layout
            {
                mainScreenPos[2] = mainScreenPos[1];
                mainScreenPos[1] = mainScreenPos[0];
                mainScreenPos[0] = emuInstance->nds->PowerControl9 >> 15;

                int guess;
                if (mainScreenPos[0] == mainScreenPos[2] &&
                    mainScreenPos[0] != mainScreenPos[1])
                {
                    // constant flickering, likely displaying 3D on both screens
                    // TODO: when both screens are used for 2D only...???
                    guess = screenSizing_Even;
                }
                else
                {
                    if (mainScreenPos[0] == 1)
                        guess = screenSizing_EmphTop;
                    else
                        guess = screenSizing_EmphBot;
                }

                if (guess != autoScreenSizing)
                {
                    autoScreenSizing = guess;
                    emit autoScreenSizingChange(autoScreenSizing);
                }
            }

            // RTC sync
            emuInstance->syncRTC();


            // emulate
            // blip buffers belong to this thread. Adjust their clock for the
            // requested emulation speed before producing device-rate samples.
            // Fast-forward retains the existing queue-trimming behavior; its
            // requested speed may exceed what the host can actually execute.
            const double audioFPS = std::min(emuInstance->curFPS.load(std::memory_order_relaxed), emuInstance->targetFPS);
            const double outputFPS = std::max(audioFPS, 59.8260982880808 * 0.5);
            emuInstance->nds->SPU.SetOutputSkew(outputFPS / 59.8260982880808);
            u32 nlines;
            if (emuInstance->nds->GPU.GetRenderer().NeedsShaderCompile())
            {
                compileShaders();
                nlines = 1;
            }
            else
            {
                emuInstance->nds->AREngine.SetStopToken(cheatStopToken());
                nlines = emuInstance->nds->RunFrame();
                for (const auto& error : emuInstance->nds->AREngine.TakeErrors())
                {
                    const char* reason = error.Reason == melonDS::AREngine::Result::Interrupted ? "interrupted; earlier changes remain" :
                        error.Reason == melonDS::AREngine::Result::UnsupportedCode ? "unsupported code" : "invalid code";
                    emuInstance->osdAddMessage(0xFFA0A0, "Cheat disabled for this session (%s): %s",
                        reason, error.Name.c_str());
                }
            }

            if (emuInstance->ndsSave)
                emuInstance->ndsSave->CheckFlush();

            if (emuInstance->gbaSave)
                emuInstance->gbaSave->CheckFlush();

            if (emuInstance->firmwareSave)
                emuInstance->firmwareSave->CheckFlush();

            if (int failedWindow = emuInstance->drawScreen(); failedWindow >= 0)
            {
                reportGLFailure(failedWindow);
                handleMessages();
                continue;
            }

#ifdef MELONCAP
            MelonCap::Update();
#endif // MELONCAP

            winUpdateCount++;
            if (winUpdateCount >= winUpdateFreq && !useOpenGL)
            {
                emit windowUpdate();
                winUpdateCount = 0;
            }
            
            if (emuInstance->hotkeyPressed(HK_FastForwardToggle)) emuInstance->fastForwardToggled = !emuInstance->fastForwardToggled;
            if (emuInstance->hotkeyPressed(HK_SlowMoToggle)) emuInstance->slowmoToggled = !emuInstance->slowmoToggled;

            if (emuInstance->hotkeyPressed(HK_AudioMuteToggle)) emuInstance->toggleAudioMute();

            bool enablefastforward = emuInstance->hotkeyDown(HK_FastForward) | emuInstance->fastForwardToggled;
            bool enableslowmo = emuInstance->hotkeyDown(HK_SlowMo) | emuInstance->slowmoToggled;

            if (useOpenGL)
            {
                // when using OpenGL: when toggling fast-forward or slowmo, change the vsync interval
                if ((enablefastforward || enableslowmo) && !(fastforward || slowmo))
                {
                    emuInstance->setVSyncGL(false);
                }
                else if (!(enablefastforward || enableslowmo) && (fastforward || slowmo))
                {
                    emuInstance->setVSyncGL(true);
                }
            }

            fastforward = enablefastforward;
            slowmo = enableslowmo;
            emuInstance->updateFastForwardMute(fastforward);

            double currentFPS = emuInstance->targetFPS;
            if (slowmo) currentFPS = emuInstance->slowmoFPS;
            else if (fastforward) currentFPS = emuInstance->fastForwardFPS;
            else if (!emuInstance->doLimitFPS && !emuInstance->doAudioSync) currentFPS = 1000.0;
            emuInstance->curFPS.store(currentFPS, std::memory_order_relaxed);

            if (emuInstance->audioDSiVolumeSync && emuInstance->nds->ConsoleType == 1)
            {
                DSi* dsi = static_cast<DSi*>(emuInstance->nds);
                u8 volumeLevel = dsi->I2C.GetBPTWL()->GetVolumeLevel();
                if (volumeLevel != dsiVolumeLevel)
                {
                    dsiVolumeLevel = volumeLevel;
                    emit syncVolumeLevel();
                }

                emuInstance->audioVolume = volumeLevel * (256.0 / 31.0);
            }

            if (emuInstance->doAudioSync && !(fastforward || slowmo))
                emuInstance->audioSync(static_cast<int>(std::ceil(
                    emuInstance->audioFreq * nlines / (outputFPS * 263.0))));

            double frametimeStep = nlines / (currentFPS * 263.0);

            if (frametimeStep < 0.001) frametimeStep = 0.001;

            if (emuInstance->doLimitFPS)
            {
                double curtime = SDL_GetPerformanceCounter() * perfCountsSec;

                frameLimitError += frametimeStep - (curtime - lastTime);
                if (frameLimitError < -frametimeStep)
                    frameLimitError = -frametimeStep;
                if (frameLimitError > frametimeStep)
                    frameLimitError = frametimeStep;

                if (round(frameLimitError * 1000.0) > 0.0)
                {
                    SDL_Delay(round(frameLimitError * 1000.0));
                    double timeBeforeSleep = curtime;
                    curtime = SDL_GetPerformanceCounter() * perfCountsSec;
                    frameLimitError -= curtime - timeBeforeSleep;
                }

                lastTime = curtime;
            }

            nframes++;
            if (nframes >= 30)
            {
                double time = SDL_GetPerformanceCounter() * perfCountsSec;
                double dt = time - lastMeasureTime;
                lastMeasureTime = time;

                u32 fps = round(nframes / dt);
                nframes = 0;

                float fpstarget = 1.0/frametimeStep;

                winUpdateFreq = fps / (u32)round(fpstarget);
                if (winUpdateFreq < 1)
                    winUpdateFreq = 1;
                    
                double actualfps = (59.8261 * 263.0) / nlines;
                snprintf(melontitle, sizeof(melontitle), "[%d/%.0f] melonDS " MELONDS_VERSION, fps, actualfps);
                changeWindowTitle(melontitle);
            }
        }
        else
        {
            // paused
            nframes = 0;
            lastTime = SDL_GetPerformanceCounter() * perfCountsSec;
            lastMeasureTime = lastTime;

            emit windowUpdate();

            snprintf(melontitle, sizeof(melontitle), "melonDS " MELONDS_VERSION);
            changeWindowTitle(melontitle);

            SDL_Delay(75);

            if (int failedWindow = emuInstance->drawScreen(); failedWindow >= 0)
                reportGLFailure(failedWindow);
        }

        handleMessages();
    }
}

void EmuThread::sendMessage(Message msg)
{
    msgMutex.lock();
    msgQueue.enqueue(msg);
    // Queued UI work must be able to interrupt an unbounded cheat loop before
    // waitMessage() waits for the emulation thread to handle that work.
    cheatStopSource.request_stop();
    msgMutex.unlock();
}

std::stop_token EmuThread::cheatStopToken()
{
    QMutexLocker lock(&msgMutex);
    // Do not lose a request queued before this frame acquires its token.
    if (msgQueue.empty() && cheatStopSource.stop_requested())
        cheatStopSource = std::stop_source{};
    return cheatStopSource.get_token();
}

void EmuThread::waitMessage(int num)
{
    if (QThread::currentThread() == this) return;
    msgSemaphore.acquire(num);
}

void EmuThread::waitAllMessages()
{
    if (QThread::currentThread() == this) return;
    while (!msgQueue.empty())
        msgSemaphore.acquire();
}

void EmuThread::handleMessages()
{
    bool glborrow = false;

    msgMutex.lock();
    while (!msgQueue.empty())
    {
        Message msg = msgQueue.dequeue();
        // These control messages can resolve a failed context without touching
        // the core. All other consumers must have the root context first.
        const bool control = msg.type == msg_Exit || msg.type == msg_EmuPause ||
            msg.type == msg_EmuUnpause || msg.type == msg_InitGL ||
            msg.type == msg_DeInitGL || msg.type == msg_BorrowGL;
        if (!control && !prepareGL())
        {
            msgResult = 0; // Also StateLoadResult::Failed: the old state is intact.
            msgError = "OpenGL is unavailable. Retry graphics recovery before changing emulation state.";
            msgSemaphore.release();
            continue;
        }
        switch (msg.type)
        {
        case msg_Exit:
            emuStatus = emuStatus_Exit;
            emuPauseStack = emuPauseStackRunning;

            emuInstance->audioDisable();
            MPInterface::Get().End(emuInstance->instanceID);
            break;

        case msg_EmuRun:
            if (stateRecoveryFailed) break;
            emuStatus = emuStatus_Running;
            emuPauseStack = emuPauseStackRunning;
            emuActive = true;

            emuInstance->audioEnable();
            emit windowEmuStart();
            break;

        case msg_EmuPause:
            emuPauseStack++;
            if (emuPauseStack > emuPauseStackPauseThreshold) break;

            prevEmuStatus = emuStatus;
            emuStatus = emuStatus_Paused;

            if (prevEmuStatus != emuStatus_Paused)
            {
                emuInstance->audioDisable();
                emit windowEmuPause(true);
                emuInstance->osdAddMessage(0, "Paused");
            }
            break;

        case msg_EmuUnpause:
            if (stateRecoveryFailed) break;
            if (emuPauseStack < emuPauseStackPauseThreshold) break;

            emuPauseStack--;
            if (emuPauseStack >= emuPauseStackPauseThreshold) break;

            emuStatus = prevEmuStatus;

            if (emuStatus != emuStatus_Paused)
            {
                if (!hasGLFailure()) emuInstance->audioEnable();
                emit windowEmuPause(false);
                emuInstance->osdAddMessage(0, "Resumed");
            }
            break;

        case msg_EmuStop:
            emuInstance->audioDisable();
            if (msg.param.value<bool>())
                emuInstance->nds->Stop();
            emuStatus = emuStatus_Paused;
            emuActive = false;

            emit windowEmuStop();
            break;

        case msg_EmuFrameStep:
            if (stateRecoveryFailed) break;
            emuStatus = emuStatus_FrameStep;
            break;

        case msg_EmuReset:
        case msg_ImportSavefile:
        {
            // Prepare the import before resetting; no frame or audio callback
            // may run between a successful reset and applying these bytes.
            std::unique_ptr<u8[]> savedata;
            u32 savelen = 0;
            if (msg.type == msg_ImportSavefile)
            {
                msgResult = 0;
                // An inserted cart may still be queued until reset starts it.
                if (!emuInstance->cartInserted()) break;
                try
                {
                    const std::unique_ptr<Platform::FileHandle, decltype(&Platform::CloseFile)> file(
                        Platform::OpenFile(msg.param.value<QString>().toStdString(), Platform::FileMode::Read),
                        Platform::CloseFile);
                    if (!file) break;
                    const u64 size = Platform::FileLength(file.get());
                    if (!size || size > std::numeric_limits<u32>::max()) break;
                    savedata = std::make_unique_for_overwrite<u8[]>(static_cast<u32>(size));
                    if (Platform::FileRead(savedata.get(), 1, size, file.get()) != size) break;
                    savelen = static_cast<u32>(size);
                }
                catch (const std::bad_alloc&)
                {
                    break;
                }
            }
            emuInstance->audioDisable();
            const auto assets = msg.type == msg_EmuReset ? msg.param.value<AssetResetRequest>() : AssetResetRequest{};
            msgResult = emuInstance->reset(assets.DS, assets.GBA);
            if (!msgResult)
            {
                if (emuStatus == emuStatus_Running) emuInstance->audioEnable();
                emuInstance->osdAddMessage(0xFFA0A0, "Reset failed; current session retained");
                break;
            }
            if (savedata) emuInstance->nds->SetNDSSave(savedata.get(), savelen);
            emuInstance->clearBackupState();
            emuInstance->discardPreservedFrame();
            stateRecoveryFailed = false;

            emuStatus = emuStatus_Running;
            emuPauseStack = emuPauseStackRunning;
            emuActive = true;

            emuInstance->audioEnable();
            emit windowEmuReset();
            emuInstance->osdAddMessage(0, "Reset");
            break;
        }

        case msg_InitGL:
            initializeGL(msg.param.value<int>());
            break;

        case msg_DeInitGL:
            msgResult = 0;
            if (msg.param.value<int>() == 0 && useOpenGL && emuInstance->nds)
            {
                // Drain captures and retire core objects in the root context
                // before the GUI destroys it or builds a new, unrelated one.
                QMutexLocker lock(&emuInstance->renderLock);
                if (!emuInstance->makeCurrentGL() || !emuInstance->preserveFrame())
                {
                    reportGLFailure(0);
                    break;
                }
                videoRenderer = renderer3D_Software;
                updateRenderer();
            }
            if (!emuInstance->deinitOpenGL(msg.param.value<int>()))
            {
                reportGLFailure(msg.param.value<int>());
                break;
            }
            if (msg.param.value<int>() == 0)
                useOpenGL = false;
            clearGLFailure(msg.param.value<int>());
            msgResult = 1;
            break;

        case msg_BorrowGL:
            emuInstance->releaseGL();
            glBorrowMutex.lock();
            glBorrowed = true;
            glBorrowMutex.unlock();
            glborrow = true;
            break;

        case msg_BootROM:
            emuInstance->audioDisable();
            msgResult = 0;
            if (!emuInstance->loadROM(msg.param.value<CartLoadRequest>().Files, true, msgError,
                                     msg.param.value<CartLoadRequest>().Assets))
            {
                if (emuStatus == emuStatus_Running) emuInstance->audioEnable();
                break;
            }

            assert(emuInstance->nds != nullptr);
            emuInstance->nds->Start();
            emuInstance->discardPreservedFrame();
            stateRecoveryFailed = false;
            msgResult = 1;
            break;

        case msg_BootFirmware:
            emuInstance->audioDisable();
            msgResult = 0;
            if (!emuInstance->bootToMenu(msgError))
            {
                if (emuStatus == emuStatus_Running) emuInstance->audioEnable();
                break;
            }

            assert(emuInstance->nds != nullptr);
            emuInstance->nds->Start();
            emuInstance->discardPreservedFrame();
            stateRecoveryFailed = false;
            msgResult = 1;
            break;

        case msg_InsertCart:
            msgResult = 0;
            if (!emuInstance->loadROM(msg.param.value<CartLoadRequest>().Files, false, msgError,
                                     msg.param.value<CartLoadRequest>().Assets))
                break;

            msgResult = 1;
            break;

        case msg_EjectCart:
            emuInstance->ejectCart();
            break;

        case msg_InsertGBACart:
            msgResult = 0;
            if (!emuInstance->loadGBAROM(msg.param.value<CartLoadRequest>().Files, msgError,
                                        msg.param.value<CartLoadRequest>().Assets))
                break;

            msgResult = 1;
            break;

        case msg_InsertGBAAddon:
            msgResult = 0;
            msgError.clear();
            emuInstance->loadGBAAddon(msg.param.value<int>(), msgError);
            msgResult = msgError.isEmpty();
            break;

        case msg_EjectGBACart:
            emuInstance->ejectGBACart();
            break;

        case msg_SaveState:
            msgResult = !stateRecoveryFailed &&
                emuInstance->saveState(msg.param.value<QString>().toStdString());
            break;

        case msg_LoadState:
        case msg_UndoStateLoad:
        {
            // Neither audio nor microphone callbacks may observe a partial load.
            emuInstance->audioDisable();
            const auto result = stateRecoveryFailed ? StateLoadResult::RecoveryFailed :
                (msg.type == msg_LoadState ?
                    emuInstance->loadState(msg.param.value<QString>().toStdString()) :
                    emuInstance->undoStateLoad());
            msgResult = static_cast<int>(result);
            if (result == StateLoadResult::Success) emuInstance->discardPreservedFrame();
            if (result == StateLoadResult::RecoveryFailed)
            {
                stateRecoveryFailed = true;
                emuStatus = prevEmuStatus = emuStatus_Paused;
                emuPauseStack = emuPauseStackRunning;
                emuActive = false;
                emit windowEmuStop();
            }
            else if (emuStatus == emuStatus_Running)
            {
                emuInstance->audioEnable();
            }
            break;
        }

        case msg_EnableCheats:
            emuInstance->enableCheats(msg.param.value<bool>());
            break;
        }

        msgSemaphore.release();
    }
    msgMutex.unlock();

    if (glborrow)
    {
        glBorrowMutex.lock();
        while (glBorrowed)
            glBorrowCond.wait(&glBorrowMutex);
        glBorrowMutex.unlock();
    }
}

void EmuThread::changeWindowTitle(char* title)
{
    emit windowTitleChange(QString(title));
}

bool EmuThread::initializeGL(int win)
{
    const bool initialized = emuInstance->initOpenGL(win);
    if (win == 0) useOpenGL = initialized;
    if (!initialized) reportGLFailure(win);
    else
    {
        clearGLFailure(win);
        if (win == 0) videoSettingsDirty = true;
    }
    return initialized;
}

bool EmuThread::prepareGL()
{
    if (hasGLFailure()) return false;
    if (!useOpenGL || emuInstance->makeCurrentGL()) return true;
    reportGLFailure(0);
    return false;
}

void EmuThread::reportGLFailure(int win)
{
    const int previous = glFailureWindow.exchange(win);
    emuInstance->audioDisable();
    if (previous < 0) emit windowOpenGLFailed(win);
}

void EmuThread::clearGLFailure(int win)
{
    if (glFailureWindow.compare_exchange_strong(win, -1) &&
        emuActive && emuStatus == emuStatus_Running)
        emuInstance->audioEnable();
}

void EmuThread::initContext(int win)
{
    sendMessage({.type = msg_InitGL, .param = win});
    waitMessage();
}

bool EmuThread::deinitContext(int win)
{
    sendMessage({.type = msg_DeInitGL, .param = win});
    waitMessage();
    return msgResult != 0;
}

void EmuThread::borrowGL()
{
    sendMessage(msg_BorrowGL);
    waitMessage();
}

void EmuThread::returnGL()
{
    glBorrowMutex.lock();
    glBorrowed = false;
    glBorrowCond.wakeAll();
    glBorrowMutex.unlock();
}

void EmuThread::emuRun()
{
    sendMessage(msg_EmuRun);
    waitMessage();
}

void EmuThread::emuPause(bool broadcast)
{
    sendMessage(msg_EmuPause);
    waitMessage();

    if (broadcast)
        emuInstance->broadcastCommand(InstCmd_Pause);
}

void EmuThread::emuUnpause(bool broadcast)
{
    sendMessage(msg_EmuUnpause);
    waitMessage();

    if (broadcast)
        emuInstance->broadcastCommand(InstCmd_Unpause);
}

void EmuThread::emuTogglePause(bool broadcast)
{
    if (emuStatus == emuStatus_Paused)
        emuUnpause(broadcast);
    else
        emuPause(broadcast);
}

void EmuThread::emuStop(bool external)
{
    sendMessage({.type = msg_EmuStop, .param = external});
    waitMessage();
}

void EmuThread::emuExit()
{
    sendMessage(msg_Exit);
    waitAllMessages();
}

void EmuThread::emuFrameStep()
{
    if (emuPauseStack < emuPauseStackPauseThreshold)
        sendMessage(msg_EmuPause);
    sendMessage(msg_EmuFrameStep);
    waitAllMessages();
}

void EmuThread::emuReset()
{
    if (QThread::currentThread() == this)
    {
        // Hotkeys originate in run(); ownership choices belong to the UI.
        QMetaObject::invokeMethod(this, [this] { emuReset(); }, Qt::QueuedConnection);
        return;
    }
    AssetResetRequest assets;
    QString error;
    if ((!emuInstance->dsAssetPaths.Source.empty() &&
         !prepareAssets(emuInstance->dsAssetPaths.Source, false, false, assets.DS, error)) ||
        (!emuInstance->gbaAssetPaths.Source.empty() &&
         !prepareAssets(emuInstance->gbaAssetPaths.Source, true, false, assets.GBA, error)))
    {
        if (!error.isEmpty()) QMessageBox::critical(emuInstance->getMainWindow(), "melonDS", error);
        return;
    }
    sendMessage({.type = msg_EmuReset, .param = QVariant::fromValue(assets)});
    waitMessage();
}

bool EmuThread::prepareAssets(const QStringList& source, bool gba, bool allowExisting, AssetIdentity::Selection& selection, QString& error)
{
    auto& cfg = emuInstance->getLocalConfig();
    const QStringList directories{cfg.GetQString("SaveFilePath"), cfg.GetQString("SavestatePath"), cfg.GetQString("CheatFilePath")};
    const auto& active = gba ? emuInstance->gbaAssetPaths : emuInstance->dsAssetPaths;
    if (!allowExisting && AssetIdentity::PathsUnchanged(active, gba, directories))
    {
        selection = active;
        error.clear();
        return true;
    }
    return AssetIdentity::Prepare(emuInstance->getAssetRegistryDirectory(), source, gba, directories,
        allowExisting, [this](const AssetIdentity::Conflict& conflict) {
            QMessageBox dialog(QMessageBox::Question, "Choose game files",
                conflict.CanUseExisting ?
                    "Files with this name already exist. Use them only if they belong to this ROM, or start with separate files." :
                    "These file names are already used. Select separate files to preserve their contents.",
                QMessageBox::NoButton, emuInstance->getMainWindow());
            dialog.setDetailedText(conflict.Paths.join('\n'));
            auto* separate = dialog.addButton("Use separate files", QMessageBox::AcceptRole);
            auto* existing = conflict.CanUseExisting ? dialog.addButton("Use existing files", QMessageBox::AcceptRole) : nullptr;
            dialog.addButton(QMessageBox::Cancel);
            dialog.setDefaultButton(separate);
            dialog.exec();
            if (dialog.clickedButton() == separate) return AssetIdentity::Choice::Separate;
            if (existing && dialog.clickedButton() == existing) return AssetIdentity::Choice::Existing;
            return AssetIdentity::Choice::Cancel;
        }, selection, error);
}

bool EmuThread::emuIsRunning()
{
    return emuStatus == emuStatus_Running;
}

bool EmuThread::emuIsActive()
{
    return emuActive;
}

int EmuThread::bootROM(const QStringList& filename, QString& errorstr)
{
    CartLoadRequest request{filename, {}};
    if (!prepareAssets(filename, false, true, request.Assets, errorstr)) return 0;
    sendMessage({.type = msg_BootROM, .param = QVariant::fromValue(request)});
    waitMessage();
    if (!msgResult)
    {
        errorstr = msgError;
        return msgResult;
    }

    sendMessage(msg_EmuRun);
    waitMessage();
    errorstr = "";
    return msgResult;
}

int EmuThread::bootFirmware(QString& errorstr)
{
    sendMessage(msg_BootFirmware);
    waitMessage();
    if (!msgResult)
    {
        errorstr = msgError;
        return msgResult;
    }

    sendMessage(msg_EmuRun);
    waitMessage();
    errorstr = "";
    return msgResult;
}

int EmuThread::insertCart(const QStringList& filename, bool gba, QString& errorstr)
{
    MessageType msgtype = gba ? msg_InsertGBACart : msg_InsertCart;

    CartLoadRequest request{filename, {}};
    if (!prepareAssets(filename, gba, true, request.Assets, errorstr)) return 0;
    sendMessage({.type = msgtype, .param = QVariant::fromValue(request)});
    waitMessage();
    errorstr = msgResult ? "" : msgError;
    return msgResult;
}

void EmuThread::ejectCart(bool gba)
{
    sendMessage(gba ? msg_EjectGBACart : msg_EjectCart);
    waitMessage();
}

int EmuThread::insertGBAAddon(int type, QString& errorstr)
{
    sendMessage({.type = msg_InsertGBAAddon, .param = type});
    waitMessage();
    errorstr = msgResult ? "" : msgError;
    return msgResult;
}

int EmuThread::saveState(const QString& filename)
{
    sendMessage({.type = msg_SaveState, .param = filename});
    waitMessage();
    return msgResult;
}

StateLoadResult EmuThread::loadState(const QString& filename)
{
    sendMessage({.type = msg_LoadState, .param = filename});
    waitMessage();
    return static_cast<StateLoadResult>(msgResult);
}

StateLoadResult EmuThread::undoStateLoad()
{
    sendMessage(msg_UndoStateLoad);
    waitMessage();
    return static_cast<StateLoadResult>(msgResult);
}

int EmuThread::importSavefile(const QString& filename)
{
    sendMessage({.type = msg_ImportSavefile, .param = filename});
    waitMessage();
    return msgResult;
}

void EmuThread::enableCheats(bool enable)
{
    sendMessage({.type = msg_EnableCheats, .param = enable});
    waitMessage();
}

void EmuThread::updateRenderer()
{
    auto nds = emuInstance->nds;

    if (videoRenderer != lastVideoRenderer)
    {
        switch (videoRenderer)
        {
            case renderer3D_Software:
                nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
                break;
            case renderer3D_OpenGL:
                nds->SetRenderer(std::make_unique<GLRenderer>(*nds, false));
                break;
            case renderer3D_OpenGLCompute:
                nds->SetRenderer(std::make_unique<GLRenderer>(*nds, true));
                break;
            default: __builtin_unreachable();
        }
    }
    if (videoRenderer != renderer3D_Software &&
        dynamic_cast<SoftRenderer*>(&nds->GetRenderer()))
    {
        videoRenderer = renderer3D_Software;
        emuInstance->osdAddMessage(0xFFA0A0, "OpenGL renderer initialization failed; using software rendering");
    }
    lastVideoRenderer = videoRenderer;

    auto& cfg = emuInstance->getGlobalConfig();
    melonDS::RendererSettings settings = {
        .ScaleFactor = cfg.GetInt("3D.GL.ScaleFactor"),
        .Threaded = cfg.GetBool("3D.Soft.Threaded"),
        .HiresCoordinates = cfg.GetBool("3D.GL.HiresCoordinates"),
        .BetterPolygons = cfg.GetBool("3D.GL.BetterPolygons"),
        .PixelConversion = static_cast<melonDS::PixelConvert::Backend>(cfg.GetInt("3D.Soft.PixelConversion"))
    };

    if (!nds->GetRenderer().SetRenderSettings(settings))
    {
        videoRenderer = renderer3D_Software;
        nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
        nds->GetRenderer().SetRenderSettings(settings);
        lastVideoRenderer = videoRenderer;
        emuInstance->osdAddMessage(0xFFA0A0, "OpenGL resolution or allocation failed; using software rendering");
    }
}

void EmuThread::compileShaders()
{
    auto& renderer = emuInstance->nds->GPU.GetRenderer();
    int currentShader, shadersCount;
    u64 startTime = SDL_GetPerformanceCounter();
    const double perfCountsSec = 1.0 / SDL_GetPerformanceFrequency();
    // kind of hacky to look at the wallclock, though it is easier than
    // than disabling vsync
    do
    {
        if (!renderer.ShaderCompileStep(currentShader, shadersCount))
        {
            videoRenderer = renderer3D_Software;
            updateRenderer();
            emuInstance->osdAddMessage(0xFFA0A0, "Compute shader compilation failed; using software rendering");
            return;
        }
    }
    while (renderer.NeedsShaderCompile() &&
             (SDL_GetPerformanceCounter() - startTime) * perfCountsSec < 1.0 / 6.0);
    emuInstance->osdAddMessage(0, "Compiling shader %d/%d", currentShader+1, shadersCount);
}
