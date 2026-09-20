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
#include <mutex>

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
#ifdef VULKANRENDERER_ENABLED
#include "GPU_Vulkan.h"
#endif
#include "GPU_OpenGL.h"
#include "OpenGLSupport.h"

#include "Savestate.h"

#include "EmuInstance.h"
#ifdef GDBSTUB_ENABLED
#include "GdbFrame.h"
#endif
#include <QMessageBox>
#include <QPushButton>
#include <QInputDialog>
#include <QFileInfo>

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
#ifdef GDBSTUB_ENABLED
    std::unique_ptr<GdbFrame> debugger;
#endif
    Config::Table& globalCfg = emuInstance->getGlobalConfig();
#ifdef VULKANRENDERER_ENABLED
    {
        std::string error;
        const bool supported = VulkanRenderer::IsAvailable(error);
        {
            QMutexLocker locker(&videoSettingsMutex);
            videoStatus.vulkanSupport = supported;
        }
        emit videoSettingsStatusChanged();
    }
#endif
    const auto applyPendingVideo = [&] {
        if (!videoSettingsDirty) return;
        QMutexLocker renderLocker(&emuInstance->renderLock);
        if (useOpenGL)
            emuInstance->setVSyncGL(true);
        videoRenderer = globalCfg.GetInt("3D.Renderer");
        if (!useOpenGL && RendererUsesOpenGL(videoRenderer)) videoRenderer = renderer3D_Software;
        updateRenderer();
        videoSettingsDirty = false;
    };
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
        videoRenderer = globalCfg.GetInt("3D.Renderer");
        if (!useOpenGL && RendererUsesOpenGL(videoRenderer)) videoRenderer = renderer3D_Software;
    }
    else
    {
        useOpenGL = false;
        videoRenderer = globalCfg.GetInt("3D.Renderer");
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
    bool audioSyncInterrupted = false;
    emuInstance->fastForwardToggled = false;
    emuInstance->slowmoToggled = false;

    while (emuStatus != emuStatus_Exit)
    {
        if (emuInstance->instanceID == 0)
            MPInterface::Acquire()->Process();

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
            if (emuStatus == emuStatus_FrameStep && !emuInstance->nds->IsGdbInterpreter()) emuStatus = emuStatus_Paused;

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

            applyPendingVideo();

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
            // The optional pitch-preserving path instead keeps the source
            // clock unchanged and converts speed after the core produces PCM.
            const double requestedFPS = emuInstance->curFPS.load(std::memory_order_relaxed);
            const double audioFPS = emuInstance->audioTimeStretchEnabled ? requestedFPS
                : std::min(requestedFPS, emuInstance->targetFPS);
            const double outputFPS = std::max(audioFPS, 59.8260982880808 * 0.5);
            emuInstance->audioSetSpeed(outputFPS / 59.8260982880808);
            emuInstance->nds->SPU.SetOutputSkew(emuInstance->audioTimeStretchEnabled
                ? 1.0 : outputFPS / 59.8260982880808);
            // A control request can interrupt the previous frame's audio wait
            // with a nearly full PCM queue. Finish that wait before producing
            // another frame after resume; otherwise the core may discard PCM.
            if (audioSyncInterrupted && emuInstance->doAudioSync &&
                (emuInstance->audioTimeStretchEnabled || !(fastforward || slowmo)))
            {
                const auto stop = cheatStopToken();
                emuInstance->audioSync(static_cast<int>(std::ceil(emuInstance->audioFreq / outputFPS)), stop);
                if (stop.stop_requested())
                {
                    handleMessages();
                    continue;
                }
                audioSyncInterrupted = false;
            }
            u32 nlines;
            if (emuInstance->nds->GPU.GetRenderer().NeedsShaderCompile())
            {
                compileShaders();
                nlines = 1;
            }
            else
            {
                emuInstance->nds->AREngine.SetStopToken(cheatStopToken());
                const auto runFrame = [&] {
#ifdef VULKANRENDERER_ENABLED
                    auto* renderer = dynamic_cast<VulkanRenderer*>(&emuInstance->nds->GetRenderer());
                    auto* cost = renderer ? renderer->Costs() : nullptr;
                    u32 lines;
                    {
                        RenderCostVulkanFrame interval(cost);
                        lines = emuInstance->nds->RunFrame();
                        if (!lines && cost) cost->Discard();
                    }
                    if (cost && cost->TakeReport())
                    {
                        char report[8192];
                        cost->Report(report, sizeof(report), "RunFrame");
                        Platform::Log(Platform::LogLevel::Info, "%s\n", report);
                    }
                    return lines;
#else
                    return emuInstance->nds->RunFrame();
#endif
                };

#ifdef GDBSTUB_ENABLED
                if (emuInstance->nds->IsGdbInterpreter())
                {
                    if (!debugger)
                    {
                        try { debugger = std::make_unique<GdbFrame>(); }
                        catch (const std::exception& failure)
                        {
                            emuInstance->nds->SetGdbArgs(std::nullopt);
                            emuInstance->osdAddMessage(0xFFA0A0, "GDB disabled: %s", failure.what());
                            continue;
                        }
                    }
                    const auto frame = debugger->Run(
                        runFrame,
                        [&] {
                            handleMessages();
                            if (emuStatus == emuStatus_Paused)
                            {
                                QMutexLocker lock(&msgMutex);
                                if (msgQueue.empty()) msgAvailable.wait(&msgMutex, 40);
                                return false;
                            }
                            if (!prepareGL()) { SDL_Delay(20); return false; }
                            applyPendingVideo();
                            if (emuInstance->nds->GetRenderer().NeedsShaderCompile())
                            {
                                compileShaders();
                                return false;
                            }
                            return true;
                        });
                    if (!frame) continue;
                    nlines = *frame;
                    if (emuStatus == emuStatus_FrameStep) emuStatus = emuStatus_Paused;
                }
                else
#endif
                {
#ifdef GDBSTUB_ENABLED
                    debugger.reset();
#endif
                    nlines = runFrame();
                }
                if (emuInstance->nds->GetRenderer().HasRenderFailure())
                {
                    // Native painting may still be copying the previous frame.
                    std::lock_guard renderLocker(emuInstance->renderLock);
                    videoRenderer = renderer3D_Software;
                    updateRenderer();
                    publishVideoSettings(true);
                    emuInstance->osdAddMessage(0xFFA0A0, "3D renderer failed; using software rendering");
                }
                for (const auto& error : emuInstance->nds->AREngine.TakeErrors())
                {
                    const char* reason = error.Reason == melonDS::AREngine::Result::Interrupted ? "interrupted; earlier changes remain" :
                        error.Reason == melonDS::AREngine::Result::UnsupportedCode ? "unsupported code" : "invalid code";
                    emuInstance->osdAddMessage(0xFFA0A0, "Cheat disabled for this session (%s): %s",
                        reason, error.Name.c_str());
                }
            }

            const int outputFrameSamples = static_cast<int>(std::ceil(
                emuInstance->audioFreq * nlines / (outputFPS * 263.0)));
            emuInstance->audioPumpTimeStretch(std::max(emuInstance->audioBufSize, outputFrameSamples));
            emuInstance->audioStartPending();

            emuInstance->retrySaveCapture();
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

            const bool synchronizeAudio = emuInstance->doAudioSync &&
                (emuInstance->audioTimeStretchEnabled || !(fastforward || slowmo));
            if (synchronizeAudio)
            {
                const auto stop = cheatStopToken();
                emuInstance->audioSync(outputFrameSamples, stop);
                audioSyncInterrupted = stop.stop_requested();
            }

            double frametimeStep = nlines / (currentFPS * 263.0);

            if (frametimeStep < 0.001) frametimeStep = 0.001;

            // When audio already paces this speed, a second wall-clock wait
            // delays the next producer frame and can starve device delivery.
            // Keep the FPS limit if output is unavailable or its clamped rate
            // differs from the requested speed (including slow motion).
            const bool audioPacesFrames = synchronizeAudio && outputFPS == currentFPS &&
                (emuInstance->audioIsRunning() || emuInstance->audioStartRequested);
            if (emuInstance->doLimitFPS && !audioPacesFrames)
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
            else
            {
                lastTime = SDL_GetPerformanceCounter() * perfCountsSec;
                frameLimitError = 0.0;
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

            // Keep the idle redraw interval, but handle queued controls promptly.
            // Checking the queue under its mutex also covers messages sent before
            // we enter the wait.
            {
                QMutexLocker lock(&msgMutex);
                if (msgQueue.empty())
                    msgAvailable.wait(&msgMutex, 75);
            }

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
    // Queued UI work must interrupt both cheat execution and audio waits before
    // waitMessage() waits for the emulation thread to handle that work.
    cheatStopSource.request_stop();
    msgAvailable.wakeOne();
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

bool EmuThread::clearRendererCacheOnThread()
{
#ifdef VULKANRENDERER_ENABLED
    if (emuInstance->nds)
        if (auto* renderer = dynamic_cast<VulkanRenderer*>(&emuInstance->nds->GetRenderer()))
        {
            renderer->ClearPipelineCache();
            return true;
        }
#endif
    return false;
}

void EmuThread::handleMessages()
{
    bool glborrow = false;

    msgMutex.lock();
    while (!msgQueue.empty())
    {
        Message msg = msgQueue.dequeue();
#ifdef GDBSTUB_ENABLED
        if (GdbFrame::IsSuspended() &&
            (msg.type == msg_SaveState || msg.type == msg_LoadState || msg.type == msg_UndoStateLoad))
        {
            msgResult = 0;
            msgError = "Savestates require a completed frame. Resume execution in the debugger first.";
            emuInstance->osdAddMessage(0xFFA0A0, "Savestates require a completed frame. Resume execution in the debugger first.");
            msgSemaphore.release();
            continue;
        }
#endif

        // These control messages can resolve a failed context without touching
        // the core. All other consumers must have the root context first.
        const bool control = msg.type == msg_Exit || msg.type == msg_EmuPause ||
            msg.type == msg_EmuUnpause || msg.type == msg_InitGL ||
            msg.type == msg_DeInitGL || msg.type == msg_BorrowGL || msg.type == msg_AudioSettings ||
            msg.type == msg_ClearRendererCache;
        if (!control && !prepareGL())
        {
            msgResult = 0; // Also StateLoadResult::Failed: the old state is intact.
            msgError = "OpenGL is unavailable. Retry graphics recovery before changing emulation state.";
            msgSemaphore.release();
            continue;
        }

#ifdef GDBSTUB_ENABLED
        // Ending the session unwinds here. Console resets/replacements unwind
        // at updateConsole's commit point; failed preparation and hotplug keep
        // the suspended CPU stack (no cartridge mapping is cached by GDB CPU).
        if (msg.type == msg_Exit || msg.type == msg_EmuStop)
            GdbFrame::CancelActive();
#endif
        switch (msg.type)
        {
        case msg_Exit:
            emuStatus = emuStatus_Exit;
            emuPauseStack = emuPauseStackRunning;

            emuInstance->audioDisable();
            MPInterface::Acquire()->End(emuInstance->instanceID);
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
            // Even a nested host pause must suspend an outstanding frame step.
            if (emuStatus == emuStatus_FrameStep)
            {
                emuStatus = emuStatus_Paused;
                emuInstance->audioDisable();
            }
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
                if (emuInstance->nds && !emuInstance->nds->IsRunning())
                {
                    stateRecoveryFailed = true;
                    emuStatus = prevEmuStatus = emuStatus_Paused;
                    emuPauseStack = emuPauseStackRunning;
                    emuActive = false;
                    emit windowEmuStop();
                    emuInstance->osdAddMessage(0xFFA0A0, "Reset failed; emulation stopped");
                }
                else
                {
                    if (emuStatus == emuStatus_Running && !hasGLFailure()) emuInstance->audioEnable();
                    emuInstance->osdAddMessage(0xFFA0A0, "Reset failed; current session retained");
                }
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
                if (RendererUsesOpenGL(videoRenderer))
                {
                    videoRenderer = renderer3D_Software;
                    updateRenderer();
                }
            }
            if (!emuInstance->deinitOpenGL(msg.param.value<int>()))
            {
                reportGLFailure(msg.param.value<int>());
                break;
            }
            if (msg.param.value<int>() == 0)
            {
                useOpenGL = false;
                setComputeSupport(-1);
            }
            clearGLFailure(msg.param.value<int>());
            msgResult = 1;
            break;

        case msg_BorrowGL:
            msgResult = 0;
            if (int failedWindow = emuInstance->releaseGL(); failedWindow >= 0)
            {
                reportGLFailure(failedWindow);
                break;
            }
            glBorrowMutex.lock();
            glBorrowed = true;
            glBorrowMutex.unlock();
            glborrow = true;
            msgResult = 1;
            break;

        case msg_BootROM:
            emuInstance->audioDisable();
            msgResult = 0;
            if (!emuInstance->loadROM(msg.param.value<CartLoadRequest>().Files, true, msgError,
                                     msg.param.value<CartLoadRequest>().Assets,
                                     msg.param.value<CartLoadRequest>().Prepared,
                                     msg.param.value<CartLoadRequest>().DSSaveType))
            {
                if (emuInstance->nds && !emuInstance->nds->IsRunning())
                {
                    stateRecoveryFailed = true;
                    emuStatus = prevEmuStatus = emuStatus_Paused;
                    emuPauseStack = emuPauseStackRunning;
                    emuActive = false;
                    emit windowEmuStop();
                }
                else if (emuStatus == emuStatus_Running && !hasGLFailure()) emuInstance->audioEnable();
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
                                     msg.param.value<CartLoadRequest>().Assets,
                                     msg.param.value<CartLoadRequest>().Prepared,
                                     msg.param.value<CartLoadRequest>().DSSaveType))
                break;

            msgResult = 1;
            break;

        case msg_EjectCart:
            emuInstance->ejectCart();
            break;

        case msg_InsertGBACart:
            msgResult = 0;
            if (!emuInstance->loadGBAROM(msg.param.value<CartLoadRequest>().Files, msgError,
                                        msg.param.value<CartLoadRequest>().Assets,
                                        msg.param.value<CartLoadRequest>().Prepared,
                                        msg.param.value<CartLoadRequest>().InitialGBASaveLength))
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
            if (StateLoadSucceeded(result)) emuInstance->discardPreservedFrame();
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

        case msg_ClearRendererCache:
            emit rendererCacheCleared(clearRendererCacheOnThread());
            break;
        case msg_EnableCheats:
            emuInstance->enableCheats(msg.param.value<bool>());
            break;
        case msg_AudioSettings:
        {
            const auto settings = msg.param.toList();
            if (emuInstance->nds)
            {
                // The dialog prepares the high-quality mode with output stopped.
                // Unrelated filter/microphone edits must not allocate or reset it.
                const auto interpolation = static_cast<AudioInterpolation>(settings[0].toInt());
                if (interpolation != AudioInterpolation::MinimumPhase)
                    emuInstance->nds->SPU.SetInterpolation(interpolation);
                emuInstance->nds->SPU.SetDegrade10Bit(static_cast<AudioBitDepth>(settings[1].toInt()));
            }
            if (settings[2].toBool()) emuInstance->audioUpdateSettings();
            break;
        }
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
        if (win == 0)
        {
            setComputeSupport(OpenGL::SupportsCompute());
            videoSettingsDirty = true;
        }
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
    emit videoSettingsStatusChanged();
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

bool EmuThread::borrowGL()
{
    sendMessage(msg_BorrowGL);
    waitMessage();
    return msgResult != 0;
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

bool EmuThread::prepareAssets(const QStringList& source, bool gba, bool allowExisting, AssetIdentity::Selection& selection, QString& error, std::stop_token stop)
{
    if (stop.stop_requested()) { error.clear(); return false; }
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
        allowExisting, [this, stop](const AssetIdentity::Conflict& conflict) {
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
            std::stop_callback cancelDialog(stop, [&dialog] {
                QMetaObject::invokeMethod(&dialog, &QDialog::reject, Qt::QueuedConnection);
            });
            dialog.exec();
            if (stop.stop_requested()) return AssetIdentity::Choice::Cancel;
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

bool EmuThread::chooseDSSaveType(CartLoadRequest& request, QString& errorstr)
{
    const auto stop = request.Prepared ? request.Prepared->Stop : std::stop_token{};
    if (stop.stop_requested()) { errorstr.clear(); return false; }
    const QStringList choices{tr("Automatic"), tr("No save"), tr("EEPROM - 512 bytes"),
        tr("EEPROM - 8 KiB"), tr("EEPROM - 64 KiB"), tr("EEPROM - 128 KiB"),
        tr("FRAM - 32 KiB"),
        tr("Flash - 256 KiB"), tr("Flash - 512 KiB"), tr("Flash - 1 MiB"),
        tr("Flash M25PE20 (T9HX) - 256 KiB"), tr("Flash M25PE40 (T9HX) - 512 KiB"),
        tr("Flash M25PE80 (T9HX) - 1 MiB")};
    const std::optional<u32> types[] = {std::nullopt, 0, 1, 11, 12, 13, 14, 5, 6, 7, 15, 16, 17};
    QInputDialog dialog(emuInstance->getMainWindow());
    dialog.setWindowTitle(tr("DS save type"));
    dialog.setLabelText(tr("Save hardware for %1 (this load only).\n"
        "Existing save bytes are retained; smaller files expand when needed.\n"
        "Automatic uses ROM metadata or the existing save size.")
        .arg(QFileInfo(request.Files.last()).fileName()));
    dialog.setComboBoxItems(choices);
    dialog.setComboBoxEditable(false);
    std::stop_callback cancelDialog(stop, [&dialog] {
        QMetaObject::invokeMethod(&dialog, &QDialog::reject, Qt::QueuedConnection);
    });
    if (dialog.exec() != QDialog::Accepted || stop.stop_requested())
    {
        errorstr.clear();
        return false;
    }
    const auto selected = choices.indexOf(dialog.textValue());
    if (selected < 0) { errorstr.clear(); return false; }
    request.DSSaveType = types[selected];
    return true;
}

void EmuThread::updateAudioSettings(int interpolation, int bitDepth, bool reloadMic)
{
    sendMessage({.type = msg_AudioSettings, .param = QVariantList{interpolation, bitDepth, reloadMic}});
    waitMessage();
}

int EmuThread::bootROM(const QStringList& filename, QString& errorstr, const std::shared_ptr<ROMPreparation::Data>& prepared, bool chooseDSSave)
{
    const auto stop = prepared ? prepared->Stop : std::stop_token{};
    if (stop.stop_requested()) { errorstr.clear(); return 0; }
    CartLoadRequest request{filename, {}, prepared};
    if (!prepareAssets(filename, false, true, request.Assets, errorstr, stop)) return 0;
    if (chooseDSSave && !chooseDSSaveType(request, errorstr)) return 0;
    if (stop.stop_requested()) { errorstr.clear(); return 0; }
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

int EmuThread::insertCart(const QStringList& filename, bool gba, QString& errorstr, const std::shared_ptr<ROMPreparation::Data>& prepared, bool chooseGBASave, bool chooseDSSave)
{
    MessageType msgtype = gba ? msg_InsertGBACart : msg_InsertCart;

    const auto stop = prepared ? prepared->Stop : std::stop_token{};
    if (stop.stop_requested()) { errorstr.clear(); return 0; }
    CartLoadRequest request{filename, {}, prepared};
    if (!prepareAssets(filename, gba, true, request.Assets, errorstr, stop)) return 0;
    if (!gba && chooseDSSave && !chooseDSSaveType(request, errorstr)) return 0;
    if (stop.stop_requested()) { errorstr.clear(); return 0; }
    if (gba && chooseGBASave)
    {
        const QStringList choices{tr("Automatic"), tr("EEPROM - 512 bytes"),
            tr("EEPROM - 8 KiB"), tr("SRAM - 32 KiB"),
            tr("Flash - 64 KiB"), tr("Flash - 128 KiB")};
        constexpr u32 lengths[] = {0, 512, 8192, 32768, 65536, 131072};
        QInputDialog dialog(emuInstance->getMainWindow());
        dialog.setWindowTitle(tr("GBA save type"));
        dialog.setLabelText(tr("Initial save memory for %1.\n"
            "Existing saves keep their current size and contents.\n"
            "Automatic detects the save type when possible.")
            .arg(QFileInfo(filename.last()).fileName()));
        dialog.setComboBoxItems(choices);
        dialog.setComboBoxEditable(false);
        std::stop_callback cancelDialog(stop, [&dialog] {
            QMetaObject::invokeMethod(&dialog, &QDialog::reject, Qt::QueuedConnection);
        });
        if (dialog.exec() != QDialog::Accepted || stop.stop_requested())
        {
            errorstr.clear();
            return 0;
        }
        const auto selected = choices.indexOf(dialog.textValue());
        if (selected < 0) { errorstr.clear(); return 0; }
        request.InitialGBASaveLength = lengths[selected];
    }
    if (stop.stop_requested()) { errorstr.clear(); return 0; }
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
    bool failed = false;
    auto& cfg = emuInstance->getGlobalConfig();
    const auto preferredGPU = cfg.GetString("Video.GPU");

    if (videoRenderer != lastVideoRenderer ||
        (videoRenderer == renderer3D_Vulkan && preferredGPU != lastVideoGPU))
    {
        switch (videoRenderer)
        {
            case renderer3D_Software:
                nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
                break;
            case renderer3D_OpenGL:
#ifdef OGLRENDERER_ENABLED
                nds->SetRenderer(std::make_unique<GLRenderer>(*nds, false));
#else
                nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
#endif
                break;
            case renderer3D_OpenGLCompute:
#ifdef OGLRENDERER_ENABLED
                nds->SetRenderer(std::make_unique<GLRenderer>(*nds, true));
#else
                nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
#endif
                break;
            case renderer3D_Vulkan:
#ifdef VULKANRENDERER_ENABLED
                nds->SetRenderer(std::make_unique<VulkanRenderer>(*nds, preferredGPU));
#else
                nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
#endif
                break;
            default:
                nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
                break;
        }
    }
    if (videoRenderer != renderer3D_Software &&
        typeid(nds->GetRenderer()) == typeid(SoftRenderer))
    {
        videoRenderer = renderer3D_Software;
        failed = true;
        emuInstance->osdAddMessage(0xFFA0A0, "3D renderer initialization failed; using software rendering");
    }
    lastVideoRenderer = videoRenderer;
    lastVideoGPU = preferredGPU;

    // A shared OpenGL configuration may request more than Vulkan's embedded
    // scales. Normalize only Vulkan; leave Software/OpenGL preferences alone.
    if (videoRenderer == renderer3D_Vulkan)
        cfg.SetInt("3D.GL.ScaleFactor", std::clamp(cfg.GetInt("3D.GL.ScaleFactor"), 1, 16));
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
        failed = true;
        nds->SetRenderer(std::make_unique<SoftRenderer>(*nds));
        nds->GetRenderer().SetRenderSettings(settings);
        lastVideoRenderer = videoRenderer;
        emuInstance->osdAddMessage(0xFFA0A0, "3D resolution or allocation failed; using software rendering");
    }
    publishVideoSettings(failed);
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
            std::lock_guard renderLocker(emuInstance->renderLock);
            videoRenderer = renderer3D_Software;
            updateRenderer();
            publishVideoSettings(true);
            emuInstance->osdAddMessage(0xFFA0A0, "Compute shader compilation failed; using software rendering");
            return;
        }
    }
    while (renderer.NeedsShaderCompile() &&
             (SDL_GetPerformanceCounter() - startTime) * perfCountsSec < 1.0 / 6.0);
    emuInstance->osdAddMessage(0, "Compiling shader %d/%d", currentShader+1, shadersCount);
    if (!renderer.NeedsShaderCompile()) publishVideoSettings();
}

EmuThread::VideoSettingsStatus EmuThread::videoSettingsStatus()
{
    QMutexLocker locker(&videoSettingsMutex);
    return videoStatus;
}

void EmuThread::setComputeSupport(int supported)
{
    {
        QMutexLocker locker(&videoSettingsMutex);
        videoStatus.computeSupport = supported;
    }
    emit videoSettingsStatusChanged();
}

void EmuThread::updateVideoSettings()
{
    {
        QMutexLocker locker(&videoSettingsMutex);
        videoStatus.pending = true;
    }
    videoSettingsDirty = true;
    emit videoSettingsStatusChanged();
}

void EmuThread::publishVideoSettings(bool failed)
{
    {
        QMutexLocker locker(&videoSettingsMutex);
        videoStatus.renderer = videoRenderer;
        videoStatus.pending = false;
        videoStatus.compiling = emuInstance->nds->GetRenderer().NeedsShaderCompile();
        videoStatus.failed = failed;
        videoStatus.gpuName.clear();
#ifdef VULKANRENDERER_ENABLED
        if (const auto* renderer = dynamic_cast<const VulkanRenderer*>(&emuInstance->nds->GetRenderer()))
            videoStatus.gpuName = QString::fromStdString(renderer->DeviceName());
#endif
    }
    emit videoSettingsStatusChanged();
}
