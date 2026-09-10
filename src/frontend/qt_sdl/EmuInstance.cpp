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

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <memory>
#include <tuple>
#include <string>
#include <utility>
#include <fstream>
#include <limits>
#include <stdexcept>

#include <QDateTime>
#include <QMutexLocker>
#include <QSaveFile>
#include <QStandardPaths>

#include <zstd.h>
#ifdef ARCHIVE_SUPPORT_ENABLED
#include "ArchiveUtil.h"
#endif
#include "EmuInstance.h"
#include "Config.h"
#include "Platform.h"
#include "Net.h"
#include "MPInterface.h"

#include "NDS.h"
#include "DSi.h"
#include "SPI.h"
#include "RTC.h"
#include "DSi_I2C.h"
#include "FreeBIOS.h"
#include "main.h"

#include "NDSCart/CartSD.h"

using std::make_unique;
using std::pair;
using std::string;
using std::tie;
using std::unique_ptr;
using namespace melonDS;
using namespace melonDS::Platform;


MainWindow* topWindow = nullptr;

QString EmuInstance::getAssetRegistryDirectory() const
{
    const auto base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    return base.isEmpty() ? QString{} : base + "/melonDS/asset-ownership";
}

const string kWifiSettingsPath = "wfcsettings.bin";
extern Net net;


EmuInstance::EmuInstance(int inst) : deleting(false),
    instanceID(inst),
    globalCfg(Config::GetGlobalTable()),
    localCfg(Config::GetLocalTable(inst))
{
    consoleType = globalCfg.GetInt("Emu.ConsoleType");

    ndsSave = nullptr;
    cartType = -1;
    baseROMDir = "";
    baseROMName = "";
    baseAssetName = "";
    nextCart = nullptr;
    changeCart = false;

    gbaSave = nullptr;
    gbaCartType = -1;
    baseGBAROMDir = "";
    baseGBAROMName = "";
    baseGBAAssetName = "";
    nextGBACart = nullptr;
    changeGBACart = false;

    cheatFile = nullptr;
    cheatsOn = localCfg.GetBool("EnableCheats");

    doLimitFPS = globalCfg.GetBool("LimitFPS");

    double val = globalCfg.GetDouble("TargetFPS");
    if (val == 0.0)
    {
        Platform::Log(Platform::LogLevel::Error, "Target FPS in config invalid\n");
        targetFPS = 60.0;
    }
    else targetFPS = val;
    curFPS = targetFPS;

    val = globalCfg.GetDouble("FastForwardFPS");
    if (val == 0.0)
    {
        Platform::Log(Platform::LogLevel::Error, "Fast-Forward FPS in config invalid\n");
        fastForwardFPS = 60.0;
    }
    else fastForwardFPS = val;

    val = globalCfg.GetDouble("SlowmoFPS");
    if (val == 0.0)
    {
        Platform::Log(Platform::LogLevel::Error, "Slow-Mo FPS in config invalid\n");
        slowmoFPS = 60.0;
    }
    else slowmoFPS = val;

    doAudioSync = globalCfg.GetBool("AudioSync");

    mpAudioMode = globalCfg.GetInt("MP.AudioMode");

    nds = nullptr;
    //updateConsole();

    audioInit();
    inputInit();

    net.RegisterInstance(instanceID);

    emuThread = new EmuThread(this);

    numWindows = 0;
    mainWindow = nullptr;
    for (int i = 0; i < kMaxWindows; i++)
        windowList[i] = nullptr;

    if (inst == 0) topWindow = nullptr;
    createWindow();
    if (!mainWindow) return;

    emuThread->start();

    // if any extra windows were saved as enabled, open them
    for (int i = 1; i < kMaxWindows; i++)
    {
        std::string key = "Window" + std::to_string(i) + ".Enabled";
        bool enable = localCfg.GetBool(key);
        if (enable)
            createWindow(i);
    }
}

EmuInstance::~EmuInstance()
{
    deleting = true;
    deleteAllWindows();

    if (emuThread->isRunning())
    {
        emuThread->emuExit();
        emuThread->wait();
    }
    delete emuThread;
    emuThread = nullptr;

    net.UnregisterInstance(instanceID);

    audioDeInit();
    inputDeInit();

    if (nds)
    {
        saveRTCData();
        delete nds;
    }
}


std::string EmuInstance::instanceFileSuffix()
{
    if (instanceID == 0) return "";

    char suffix[16] = {0};
    snprintf(suffix, 15, ".%d", instanceID+1);
    return suffix;
}

void EmuInstance::createWindow(int id)
{
    if (numWindows >= kMaxWindows)
    {
        // TODO
        return;
    }

    if (id == -1)
    {
        for (int i = 0; i < kMaxWindows; i++)
        {
            if (windowList[i]) continue;
            id = i;
            break;
        }
    }

    if (id == -1)
        return;
    if (windowList[id])
        return;

    const bool requestedGL = usesOpenGL();
    MainWindow* win;
    {
        // Include this worker even before the instance enters the global registry.
        ScopedGLWorkers workers(emuThread);
        if (!workers) return;
        win = new MainWindow(id, this, mainWindow ? mainWindow : topWindow);
        if (!topWindow) topWindow = win;
        if (!mainWindow) mainWindow = win;
        windowList[id] = win;
        numWindows++;
    }

    emuThread->attachWindow(win);

    // if creating a secondary window, we may need to initialize its OpenGL context here
    if (win->hasOpenGL() && (id != 0))
        emuThread->initContext(id);

    // Creation fallback changes the global renderer preference. Apply it to
    // existing windows after construction/publication and all worker loans end.
    if (requestedGL && !win->hasOpenGL())
        QMetaObject::invokeMethod(win, "onUpdateVideoSettings", Qt::QueuedConnection, Q_ARG(bool, true));

    bool enable = (numWindows < kMaxWindows);
    doOnAllWindows([=](MainWindow* win)
    {
        win->actNewWindow->setEnabled(enable);
    });
}

bool EmuInstance::deleteWindow(int id, bool close)
{
    if (id >= kMaxWindows) return true;

    MainWindow* win = windowList[id];
    if (!win) return true;

    if (win->hasOpenGL() && !emuThread->deinitContext(id)) return false;

    bool removed = false;
    {
        // Deregistration does not reload GLAD or access other instances.
        ScopedGLWorkers workers(emuThread, false);
        if (workers)
        {
            emuThread->detachWindow(win);
            windowList[id] = nullptr;
            numWindows--;
            if (topWindow == win) topWindow = nullptr;
            if (mainWindow == win) mainWindow = nullptr;
            removed = true;
        }
    }
    if (!removed)
    {
        if (win->hasOpenGL()) emuThread->initContext(id);
        return false;
    }

    // close() may re-enter deletion or destroy the instance. Return loans first
    // so its worker can still handle exit/deinit messages.

    if (close)
        win->close();

    if (deleting) return true;

    if (numWindows == 0)
    {
        // if we closed the last window, delete the instance
        // if the main window is closed, Qt will take care of closing any secondary windows
        deleteEmuInstance(instanceID);
        return true;
    }
    else
    {
        bool enable = (numWindows < kMaxWindows);
        doOnAllWindows([=](MainWindow* win)
        {
            win->actNewWindow->setEnabled(enable);
        });
    }
    return true;
}

void EmuInstance::deleteAllWindows()
{
    for (int i = kMaxWindows-1; i >= 0; i--)
        deleteWindow(i, true);
}

void EmuInstance::doOnAllWindows(std::function<void(MainWindow*)> func, int exclude)
{
    for (int i = 0; i < kMaxWindows; i++)
    {
        if (i == exclude) continue;
        if (!windowList[i]) continue;

        func(windowList[i]);
    }
}

void EmuInstance::saveEnabledWindows()
{
    doOnAllWindows([=](MainWindow* win)
    {
        win->saveEnabled(true);
    });
}


void EmuInstance::broadcastCommand(int cmd, QVariant param)
{
    broadcastInstanceCommand(cmd, param, instanceID);
}

void EmuInstance::handleCommand(int cmd, QVariant& param)
{
    switch (cmd)
    {
    case InstCmd_Pause:
        emuThread->emuPause(false);
        break;

    case InstCmd_Unpause:
        emuThread->emuUnpause(false);
        break;

    case InstCmd_UpdateRecentFiles:
        for (int i = 0; i < kMaxWindows; i++)
        {
            if (windowList[i])
                windowList[i]->loadRecentFilesMenu(true);
        }
        break;

    /*case InstCmd_UpdateVideoSettings:
        mainWindow->updateVideoSettings(param.value<bool>());
        break;*/
    }
}


void EmuInstance::osdAddMessage(unsigned int color, const char* fmt, ...)
{
    if (fmt == nullptr)
        return;

    char msg[256];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, 256, fmt, args);
    va_end(args);

    for (int i = 0; i < kMaxWindows; i++)
    {
        if (windowList[i])
            windowList[i]->osdAddMessage(color, msg);
    }
}


bool EmuInstance::emuIsActive()
{
    if (emuThread == nullptr)
        return false;
    return emuThread->emuIsActive();
}

void EmuInstance::emuStop(StopReason reason)
{
    if (reason != StopReason::External)
        emuThread->emuStop(false);

    switch (reason)
    {
        case StopReason::GBAModeNotSupported:
            osdAddMessage(0xFFA0A0, "GBA mode not supported");
            break;
        case StopReason::BadExceptionRegion:
            osdAddMessage(0xFFA0A0, "Internal error");
            break;
        case StopReason::PowerOff:
        case StopReason::External:
            osdAddMessage(0xFFC040, "Shutdown");
        default:
            break;
    }
}


bool EmuInstance::usesOpenGL()
{
    return globalCfg.GetBool("Screen.UseGL") ||
           (globalCfg.GetInt("3D.Renderer") != renderer3D_Software);
}

bool EmuInstance::initOpenGL(int win)
{
    if (!windowList[win] || !windowList[win]->initOpenGL()) return false;

    setVSyncGL(true);
    return true;
}

bool EmuInstance::deinitOpenGL(int win)
{
    return !windowList[win] || windowList[win]->deinitOpenGL();
}

void EmuInstance::setVSyncGL(bool vsync)
{
    int intv;

    vsync = vsync && globalCfg.GetBool("Screen.VSync");
    if (vsync)
        intv = globalCfg.GetInt("Screen.VSyncInterval");
    else
        intv = 0;

    for (int i = 0; i < kMaxWindows; i++)
    {
        if (windowList[i])
            windowList[i]->setGLSwapInterval(intv);
    }
}

bool EmuInstance::makeCurrentGL()
{
    return mainWindow && mainWindow->makeCurrentGL();
}

bool EmuInstance::preserveFrame()
{
    if (!nds || !emuThread->emuIsActive()) return true;
    // Repeated close/recovery attempts must not replace a good paused image
    // with the newly constructed renderer's as-yet empty output.
    if (!preservedFrame[0].isNull() && preservedFrameNumber == nds->NumFrames) return true;
    std::array<QImage, 2> images;
    void* top; void* bottom;
    if (nds->GPU.GetFramebuffers(&top, &bottom))
    {
        images[0] = QImage(static_cast<uchar*>(top), 256, 192, QImage::Format_RGB32).copy();
        images[1] = QImage(static_cast<uchar*>(bottom), 256, 192, QImage::Format_RGB32).copy();
    }
    else
    {
        const GLuint texture = *static_cast<GLuint*>(top);
        glBindTexture(GL_TEXTURE_2D_ARRAY, texture);
        GLint width = 0, height = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_WIDTH, &width);
        glGetTexLevelParameteriv(GL_TEXTURE_2D_ARRAY, 0, GL_TEXTURE_HEIGHT, &height);
        if (width <= 0 || height <= 0) return false;
        GLuint framebuffer = 0;
        glGenFramebuffers(1, &framebuffer);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 4);
        glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        bool valid = framebuffer != 0;
        for (int screen = 0; screen < 2 && valid; ++screen)
        {
            glFramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, texture, 0, screen);
            glReadBuffer(GL_COLOR_ATTACHMENT0);
            valid = glCheckFramebufferStatus(GL_READ_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
            QImage image(width, height, QImage::Format_RGB32);
            valid &= !image.isNull();
            if (!valid) break;
            glReadPixels(0, 0, width, height, GL_BGRA, GL_UNSIGNED_BYTE, image.bits());
            valid = glGetError() == GL_NO_ERROR;
            if (valid) images[screen] = image.scaled(256, 192);
        }
        glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &framebuffer);
        if (!valid) return false;
    }
    if (images[0].isNull() || images[1].isNull()) return false;
    preservedFrame = std::move(images);
    preservedFrameNumber = nds->NumFrames;
    return true;
}

void EmuInstance::discardPreservedFrame()
{
    preservedFrame = {};
    for (auto* window : windowList)
        if (window) window->panel->setPreservedFrame({}, 0);
}

int EmuInstance::releaseGL()
{
    for (int i = 0; i < kMaxWindows; i++)
    {
        if (windowList[i] && !windowList[i]->releaseGL()) return i;
    }
    return -1;
}


int EmuInstance::drawScreen()
{
    for (int i = 0; i < kMaxWindows; i++)
    {
        if (windowList[i] && !windowList[i]->drawScreen()) return i;
    }
    return -1;
}


int EmuInstance::lastSep(const std::string& path)
{
    int i = path.length() - 1;
    while (i >= 0)
    {
        if (path[i] == '/' || path[i] == '\\')
            return i;

        i--;
    }

    return -1;
}

static string AssetPath(const string& directory, const string& name, const string& ext)
{
    string result = directory;

    // cut off trailing slashes
    for (;;)
    {
        int i = result.length() - 1;
        if (i < 0) break;
        if (result[i] == '/' || result[i] == '\\')
            result.resize(i);
        else
            break;
    }

    if (!result.empty())
        result += '/';

    result += name.empty() ? "firmware" : name;
    result += ext;
    return result;
}

string EmuInstance::getAssetPath(bool gba, const string& configpath, const string& ext, const string& file = "")
{
    const auto& selected = gba ? gbaAssetPaths : dsAssetPaths;
    // Keep active files stable until load/reset applies new path settings.
    const string directory = selected.Valid() ?
        (ext == ".sav" ? selected.SaveDirectory : ext == ".mch" ? selected.CheatDirectory : selected.StateDirectory).toStdString() :
        (configpath.empty() ? (gba ? baseGBAROMDir : baseROMDir) : configpath);
    const string& name = file.empty() ? (gba ? baseGBAAssetName : baseAssetName) : file;
    return AssetPath(directory, name, ext);
}

static bool FlushSave(SaveManager* save, QString& errorstr)
{
    if (!save || save->Flush()) return true;
    errorstr = QString("Unable to save current data. Retry, or close the window to save a recovery copy.\n\n%1")
        .arg(QString::fromStdString(save->GetPath()));
    return false;
}

bool EmuInstance::flushSaveData(QString& errorstr)
{
    for (SaveManager* save : {ndsSave.get(), gbaSave.get(), firmwareSave.get()})
        if (!FlushSave(save, errorstr)) return false;
    return true;
}


QString EmuInstance::verifyDSBIOS()
{
    FileHandle* f;
    long len;

    f = Platform::OpenLocalFile(globalCfg.GetString("DS.BIOS9Path"), FileMode::Read);
    if (!f) return "DS ARM9 BIOS was not found or could not be accessed. Check your emu settings.";

    len = FileLength(f);
    if (len != 0x1000)
    {
        CloseFile(f);
        return "DS ARM9 BIOS is not a valid BIOS dump.";
    }

    CloseFile(f);

    f = Platform::OpenLocalFile(globalCfg.GetString("DS.BIOS7Path"), FileMode::Read);
    if (!f) return "DS ARM7 BIOS was not found or could not be accessed. Check your emu settings.";

    len = FileLength(f);
    if (len != 0x4000)
    {
        CloseFile(f);
        return "DS ARM7 BIOS is not a valid BIOS dump.";
    }

    CloseFile(f);

    return "";
}

QString EmuInstance::verifyDSiBIOS()
{
    FileHandle* f;
    long len;

    // TODO: check the first 32 bytes

    f = Platform::OpenLocalFile(globalCfg.GetString("DSi.BIOS9Path"), FileMode::Read);
    if (!f) return "DSi ARM9 BIOS was not found or could not be accessed. Check your emu settings.";

    len = FileLength(f);
    if (len != 0x10000)
    {
        CloseFile(f);
        return "DSi ARM9 BIOS is not a valid BIOS dump.";
    }

    CloseFile(f);

    f = Platform::OpenLocalFile(globalCfg.GetString("DSi.BIOS7Path"), FileMode::Read);
    if (!f) return "DSi ARM7 BIOS was not found or could not be accessed. Check your emu settings.";

    len = FileLength(f);
    if (len != 0x10000)
    {
        CloseFile(f);
        return "DSi ARM7 BIOS is not a valid BIOS dump.";
    }

    CloseFile(f);

    return "";
}

QString EmuInstance::verifyDSFirmware()
{
    FileHandle* f;
    long len;

    std::string fwpath = globalCfg.GetString("DS.FirmwarePath");

    f = Platform::OpenLocalFile(fwpath, FileMode::Read);
    if (!f) return "DS firmware was not found or could not be accessed. Check your emu settings.";

    if (!Platform::CheckFileWritable(fwpath))
        return "DS firmware is unable to be written to.\nPlease check file/folder write permissions.";

    len = FileLength(f);
    if (len == 0x20000)
    {
        // 128KB firmware, not bootable
        CloseFile(f);
        // TODO report it somehow? detect in core?
        return "";
    }
    else if (len != 0x40000 && len != 0x80000)
    {
        CloseFile(f);
        return "DS firmware is not a valid firmware dump.";
    }

    CloseFile(f);

    return "";
}

QString EmuInstance::verifyDSiFirmware()
{
    FileHandle* f;
    long len;

    std::string fwpath = globalCfg.GetString("DSi.FirmwarePath");

    f = Platform::OpenLocalFile(fwpath, FileMode::Read);
    if (!f) return "DSi firmware was not found or could not be accessed. Check your emu settings.";

    if (!Platform::CheckFileWritable(fwpath))
        return "DSi firmware is unable to be written to.\nPlease check file/folder write permissions.";

    len = FileLength(f);
    if (len != 0x20000)
    {
        // not 128KB
        // TODO: check whether those work
        CloseFile(f);
        return "DSi firmware is not a valid firmware dump.";
    }

    CloseFile(f);

    return "";
}

QString EmuInstance::verifyDSiNAND(bool isoptional)
{
    FileHandle* f;
    long len;

    std::string nandpath = globalCfg.GetString("DSi.NANDPath");

    f = Platform::OpenLocalFile(nandpath, FileMode::ReadWriteExisting);
    if (!f && isoptional) return "";
    if (!f) return "DSi NAND was not found or could not be accessed. Check your emu settings.";

    if (!Platform::CheckFileWritable(nandpath))
        return "DSi NAND is unable to be written to.\nPlease check file/folder write permissions.";

    // TODO: some basic checks
    // check that it has the nocash footer, and all

    CloseFile(f);

    return "";
}

QString EmuInstance::verifySetup()
{
    QString res;

    bool extbios = globalCfg.GetBool("Emu.ExternalBIOSEnable");
    bool extbiostwl = globalCfg.GetBool("DSi.ExternalBIOSEnable");
    bool directboot = globalCfg.GetBool("Emu.DirectBoot");
    int console = globalCfg.GetInt("Emu.ConsoleType");

    if (extbios)
    {
        res = verifyDSBIOS();
        if (!res.isEmpty()) return res;
    }

    if (console == 1)
    {
        if (extbiostwl)
        {
            res = verifyDSiBIOS();
            if (!res.isEmpty()) return res;

            res = verifyDSiFirmware();
            if (!res.isEmpty()) return res;
        }

        res = verifyDSiNAND(!extbiostwl || directboot);
        if (!res.isEmpty()) return res;
    }
    else
    {
        if (extbios)
        {
            res = verifyDSFirmware();
            if (!res.isEmpty()) return res;
        }
    }

    return "";
}


std::string EmuInstance::getEffectiveFirmwareSavePath()
{
    if (consoleType == 1)
    {
        if (!globalCfg.GetBool("DSi.ExternalBIOSEnable"))
            return GetLocalFilePath(kWifiSettingsPath);
        return globalCfg.GetString("DSi.FirmwarePath");
    }
    else
    {
        if (!globalCfg.GetBool("Emu.ExternalBIOSEnable"))
            return GetLocalFilePath(kWifiSettingsPath);
        return globalCfg.GetString("DS.FirmwarePath");
    }
}

// Initializes the firmware save manager with the selected firmware image's path
// OR the path to the wi-fi settings.
void EmuInstance::initFirmwareSaveManager() noexcept
{
    firmwareSave = std::make_unique<SaveManager>(getEffectiveFirmwareSavePath() + instanceFileSuffix());
}

std::string EmuInstance::getSavestateName(int slot)
{
    std::string ext = ".ml";
    ext += (char)('0'+slot);
    return getAssetPath(false, localCfg.GetString("SavestatePath"), ext);
}

bool EmuInstance::savestateExists(int slot)
{
    std::string ssfile = getSavestateName(slot);
    return Platform::FileExists(ssfile);
}

StateLoadResult EmuInstance::loadState(const std::string& filename)
{
    const std::unique_ptr<Platform::FileHandle, decltype(&Platform::CloseFile)> file(
        Platform::OpenFile(filename, Platform::FileMode::Read), Platform::CloseFile);
    if (!file)
    {
        Platform::Log(Platform::LogLevel::Error, "Failed to open state file \"%s\"\n", filename.c_str());
        return StateLoadResult::Failed;
    }

    // The format has a 16-byte global header and a 32-bit total length.
    const u64 size = Platform::FileLength(file.get());
    if (size < 16 || size > std::numeric_limits<u32>::max())
    {
        Platform::Log(Platform::LogLevel::Error, "Invalid state file size\n");
        return StateLoadResult::Failed;
    }

    try
    {
        std::vector<u8> buffer(static_cast<size_t>(size));
        if (Platform::FileRead(buffer.data(), 1, size, file.get()) != size)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to read complete state file\n");
            return StateLoadResult::Failed;
        }
        Savestate state(buffer.data(), static_cast<u32>(size), false);
        return applyState(state, false);
    }
    catch (const std::bad_alloc&)
    {
        Platform::Log(Platform::LogLevel::Error, "Failed to allocate state file buffer\n");
        return StateLoadResult::Failed;
    }
}

StateLoadResult EmuInstance::applyState(Savestate& state, bool undo)
{
    if (state.Error) return StateLoadResult::Failed;

    // Device state can allocate while loading. An allocation error after RAM
    // was changed needs the same recovery as a malformed later section.
    const auto transfer = [this](Savestate& file)
    {
        if (file.Error) return false;
        try
        {
            return nds->DoSavestate(&file) && !file.Error;
        }
        catch (const std::bad_alloc&) {}
        catch (const std::length_error&) {}
        file.Error = true;
        return false;
    };

    std::unique_ptr<Savestate> backup;
    try
    {
        backup = std::make_unique<Savestate>();
    }
    catch (const std::bad_alloc&)
    {
        return StateLoadResult::Failed;
    }
    if (!transfer(*backup)) return StateLoadResult::Failed;

    if (!transfer(state))
    {
        Savestate recovery(backup->Buffer(), backup->Length(), false);
        if (transfer(recovery))
        {
            Platform::Log(Platform::LogLevel::Error, "State load failed; previous state restored\n");
            return StateLoadResult::Failed;
        }
        // Do not run another frame with partially restored CPU/device state.
        nds->Stop();
        backupState.reset();
        Platform::Log(Platform::LogLevel::Error, "State recovery failed; emulation stopped\n");
        return StateLoadResult::RecoveryFailed;
    }

    // Publish undo only on success. Keep its serialized length and save mode
    // intact: Rewind() changes the cursor used by Length().
    if (undo) backupState.reset();
    else backupState = std::move(backup);
    return StateLoadResult::Success;
}

bool EmuInstance::saveState(const std::string& filename)
{
    Savestate state;
    if (state.Error) return false;

    // Finish serialization before touching the existing file.
    if (!nds->DoSavestate(&state) || state.Error) return false;

    QSaveFile file(QString::fromStdString(filename));
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(static_cast<const char*>(state.Buffer()), state.Length()) != state.Length() ||
        !file.commit())
    {
        Platform::Log(Platform::Error,
                      "Failed to save state to %s: %s\n",
                      filename.c_str(), file.errorString().toUtf8().constData()
        );
        return false;
    }

    return true;
}

StateLoadResult EmuInstance::undoStateLoad()
{
    if (!backupState) return StateLoadResult::Failed;
    Savestate state(backupState->Buffer(), backupState->Length(), false);
    return applyState(state, true);
}


void EmuInstance::unloadCheats()
{
    cheatFile = nullptr; // cleaned up by unique_ptr
    nds->AREngine.Cheats.clear();
}

void EmuInstance::loadCheats()
{
    unloadCheats();

    std::string filename = getAssetPath(false, localCfg.GetString("CheatFilePath"), ".mch");

    // TODO: check for error (malformed cheat file, ...)
    cheatFile = std::make_unique<ARCodeFile>(filename);

    if (cheatsOn)
    {
        nds->AREngine.Cheats = cheatFile->GetCodes();
    }
    else
    {
        nds->AREngine.Cheats.clear();
    }
}

std::unique_ptr<ARM9BIOSImage> EmuInstance::loadARM9BIOS() noexcept
{
    if (!globalCfg.GetBool("Emu.ExternalBIOSEnable"))
    {
        return std::make_unique<ARM9BIOSImage>(FreeBIOSGetNtrArm9());
    }

    string path = globalCfg.GetString("DS.BIOS9Path");

    if (FileHandle* f = OpenLocalFile(path, Read))
    {
        std::unique_ptr<ARM9BIOSImage> bios = std::make_unique<ARM9BIOSImage>();
        FileRewind(f);
        FileRead(bios->data(), bios->size(), 1, f);
        CloseFile(f);
        Log(Info, "ARM9 BIOS loaded from %s\n", path.c_str());
        return bios;
    }

    Log(Warn, "ARM9 BIOS not found\n");
    return nullptr;
}

std::unique_ptr<ARM7BIOSImage> EmuInstance::loadARM7BIOS() noexcept
{
    if (!globalCfg.GetBool("Emu.ExternalBIOSEnable"))
    {
        return std::make_unique<ARM7BIOSImage>(FreeBIOSGetNtrArm7());
    }

    string path = globalCfg.GetString("DS.BIOS7Path");

    if (FileHandle* f = OpenLocalFile(path, Read))
    {
        std::unique_ptr<ARM7BIOSImage> bios = std::make_unique<ARM7BIOSImage>();
        FileRead(bios->data(), bios->size(), 1, f);
        CloseFile(f);
        Log(Info, "ARM7 BIOS loaded from %s\n", path.c_str());
        return bios;
    }

    Log(Warn, "ARM7 BIOS not found\n");
    return nullptr;
}

std::unique_ptr<DSiBIOSImage> EmuInstance::loadDSiARM9BIOS() noexcept
{
    if (!globalCfg.GetBool("DSi.ExternalBIOSEnable"))
    {
        return std::make_unique<DSiBIOSImage>(FreeBIOSGetTwlArm9());
    }

    string path = globalCfg.GetString("DSi.BIOS9Path");

    if (FileHandle* f = OpenLocalFile(path, Read))
    {
        std::unique_ptr<DSiBIOSImage> bios = std::make_unique<DSiBIOSImage>();
        FileRead(bios->data(), bios->size(), 1, f);
        CloseFile(f);

        Log(Info, "ARM9i BIOS loaded from %s\n", path.c_str());
        return bios;
    }

    Log(Warn, "ARM9i BIOS not found\n");
    return nullptr;
}

std::unique_ptr<DSiBIOSImage> EmuInstance::loadDSiARM7BIOS() noexcept
{
    if (!globalCfg.GetBool("DSi.ExternalBIOSEnable"))
    {
        return std::make_unique<DSiBIOSImage>(FreeBIOSGetTwlArm7());
    }

    string path = globalCfg.GetString("DSi.BIOS7Path");

    if (FileHandle* f = OpenLocalFile(path, Read))
    {
        std::unique_ptr<DSiBIOSImage> bios = std::make_unique<DSiBIOSImage>();
        FileRead(bios->data(), bios->size(), 1, f);
        CloseFile(f);

        Log(Info, "ARM7i BIOS loaded from %s\n", path.c_str());
        return bios;
    }

    Log(Warn, "ARM7i BIOS not found\n");
    return nullptr;
}

Firmware EmuInstance::generateFirmware(int type) noexcept
{
    // Construct the default firmware...
    string settingspath;
    Firmware firmware = Firmware(type);
    assert(firmware.Buffer() != nullptr);

    // If using generated firmware, we keep the wi-fi settings on the host disk separately.
    // Wi-fi access point data includes Nintendo WFC settings,
    // and if we didn't keep them then the player would have to reset them in each session.
    // We don't need to save the whole firmware, just the part that may actually change.
    if (FileHandle* f = OpenLocalFile(kWifiSettingsPath, Read))
    {// If we have Wi-fi settings to load...
        constexpr unsigned TOTAL_WFC_SETTINGS_SIZE = 3 * (sizeof(Firmware::WifiAccessPoint) + sizeof(Firmware::ExtendedWifiAccessPoint));

        if (!FileRead(firmware.GetExtendedAccessPointPosition(), TOTAL_WFC_SETTINGS_SIZE, 1, f))
        { // If we couldn't read the Wi-fi settings from this file...
            Log(Warn, "Failed to read Wi-fi settings from \"%s\"; using defaults instead\n", kWifiSettingsPath.c_str());

            // The access point and extended access point segments might
            // be in different locations depending on the firmware revision,
            // but our generated firmware always keeps them next to each other.
            // (Extended access points first, then regular ones.)
            firmware.GetAccessPoints() = {
                    Firmware::WifiAccessPoint(type),
                    Firmware::WifiAccessPoint(),
                    Firmware::WifiAccessPoint(),
            };

            firmware.GetExtendedAccessPoints() = {
                    Firmware::ExtendedWifiAccessPoint(),
                    Firmware::ExtendedWifiAccessPoint(),
                    Firmware::ExtendedWifiAccessPoint(),
            };
            firmware.UpdateChecksums();
        }
        CloseFile(f);
    }

    customizeFirmware(firmware, true);

    // If we don't have Wi-fi settings to load,
    // then the defaults will have already been populated by the constructor.
    return firmware;
}

std::optional<Firmware> EmuInstance::loadFirmware(int type) noexcept
{
    string firmwarepath;
    if (type == 1)
    {
        if (!globalCfg.GetBool("DSi.ExternalBIOSEnable"))
            return generateFirmware(type);
        firmwarepath = globalCfg.GetString("DSi.FirmwarePath");
    }
    else
    {
        if (!globalCfg.GetBool("Emu.ExternalBIOSEnable"))
            return generateFirmware(type);
        firmwarepath = globalCfg.GetString("DS.FirmwarePath");
    }

    string fwpath_inst = firmwarepath + instanceFileSuffix();

    Log(Debug, "Loading firmware from file %s\n", fwpath_inst.c_str());
    FileHandle* file = OpenLocalFile(fwpath_inst, Read);

    if (!file)
    {
        Log(Debug, "Loading firmware from file %s\n", firmwarepath.c_str());
        file = OpenLocalFile(firmwarepath, Read);
        if (!file)
        {
            Log(Error, "Couldn't open firmware file!\n");
            return std::nullopt;
        }
    }

    Firmware firmware(file);
    CloseFile(file);

    if (!firmware.Buffer())
    {
        Log(Error, "Couldn't read firmware file!\n");
        return std::nullopt;
    }

    customizeFirmware(firmware, localCfg.GetBool("Firmware.OverrideSettings"));

    return firmware;
}


std::optional<DSi_NAND::NANDImage> EmuInstance::loadNAND(const std::array<u8, DSiBIOSSize>& arm7ibios) noexcept
{
    string path = globalCfg.GetString("DSi.NANDPath");

    FileHandle* nandfile = OpenLocalFile(path, ReadWriteExisting);
    if (!nandfile)
        return std::nullopt;

    DSi_NAND::NANDImage nandImage(nandfile, &arm7ibios[0x8308]);
    if (!nandImage)
    {
        Log(Error, "Failed to parse DSi NAND\n");
        return std::nullopt;
        // the NANDImage takes ownership of the FileHandle, no need to clean it up here
    }

    // scoped so that mount isn't alive when we move the NAND image to DSi::NANDImage
    {
        auto mount = DSi_NAND::NANDMount(nandImage);
        if (!mount)
        {
            Log(Error, "Failed to mount DSi NAND\n");
            return std::nullopt;
        }

        DSi_NAND::DSiFirmwareSystemSettings settings {};
        if (!mount.ReadUserData(settings))
        {
            Log(Error, "Failed to read DSi NAND user data\n");
            return std::nullopt;
        }

        // override user settings, if needed
        if (localCfg.GetBool("Firmware.OverrideSettings"))
        {
            auto firmcfg = localCfg.GetTable("Firmware");

            // we store relevant strings as UTF-8, so we need to convert them to UTF-16

            // setting up username
            auto username = firmcfg.GetQString("Username");
            size_t usernameLength = std::min((int) username.length(), 10);
            memset(&settings.Nickname, 0, sizeof(settings.Nickname));
            memcpy(&settings.Nickname, username.utf16(), usernameLength * sizeof(char16_t));

            // setting language
            settings.Language = static_cast<Firmware::Language>(firmcfg.GetInt("Language"));

            // setting up color
            settings.FavoriteColor = firmcfg.GetInt("FavouriteColour");

            // setting up birthday
            settings.BirthdayMonth = firmcfg.GetInt("BirthdayMonth");
            settings.BirthdayDay = firmcfg.GetInt("BirthdayDay");

            // setup message
            auto message = firmcfg.GetQString("Message");
            size_t messageLength = std::min((int) message.length(), 26);
            memset(&settings.Message, 0, sizeof(settings.Message));
            memcpy(&settings.Message, message.utf16(), messageLength * sizeof(char16_t));

            // TODO: make other items configurable?
        }

        // fix touchscreen coords
        settings.TouchCalibrationADC1 = {0, 0};
        settings.TouchCalibrationPixel1 = {0, 0};
        settings.TouchCalibrationADC2 = {255 << 4, 191 << 4};
        settings.TouchCalibrationPixel2 = {255, 191};

        settings.UpdateHash();

        if (!mount.ApplyUserData(settings))
        {
            Log(LogLevel::Error, "Failed to write patched DSi NAND user data\n");
            return std::nullopt;
        }
    }

    return nandImage;
}

constexpr u64 MB(u64 i)
{
    return i * 1024 * 1024;
}

constexpr u64 imgsizes[] = {0, MB(256), MB(512), MB(1024), MB(2048), MB(4096)};

std::optional<FATStorageArgs> EmuInstance::getSDCardArgs(const string& key) noexcept
{
    // key = DSi.SD or DLDI
    Config::Table sdopt = globalCfg.GetTable(key);

    if (!sdopt.GetBool("Enable"))
        return std::nullopt;

    return FATStorageArgs {
            sdopt.GetString("ImagePath"),
            imgsizes[sdopt.GetInt("ImageSize")],
            sdopt.GetBool("ReadOnly"),
            sdopt.GetBool("FolderSync") ? std::make_optional(sdopt.GetString("FolderPath")) : std::nullopt
    };
}

std::optional<FATStorage> EmuInstance::loadSDCard(const string& key) noexcept
{
    auto args = getSDCardArgs(key);
    if (!args.has_value())
        return std::nullopt;

    return FATStorage(args.value());
}

void EmuInstance::enableCheats(bool enable)
{
    cheatsOn = enable;
    if (cheatsOn && cheatFile)
        nds->AREngine.Cheats = cheatFile->GetCodes();
    else
        nds->AREngine.Cheats.clear();
}

ARCodeFile* EmuInstance::getCheatFile()
{
    return cheatFile.get();
}

void EmuInstance::setBatteryLevels()
{
    if (consoleType == 1)
    {
        auto dsi = static_cast<DSi*>(nds);
        dsi->I2C.GetBPTWL()->SetBatteryLevel(localCfg.GetInt("DSi.Battery.Level"));
        dsi->I2C.GetBPTWL()->SetBatteryCharging(localCfg.GetBool("DSi.Battery.Charging"));
    }
    else
    {
        nds->SPI.GetPowerMan()->SetBatteryLevelOkay(localCfg.GetBool("DS.Battery.LevelOkay"));
    }
}

void EmuInstance::loadRTCData()
{
    const unique_ptr<FileHandle, decltype(&Platform::CloseFile)> file(
        Platform::OpenLocalFile("rtc.bin", FileMode::Read), Platform::CloseFile);
    if (!file) return;

    RTC::StateData state{};
    if (Platform::FileLength(file.get()) != sizeof(state) ||
        Platform::FileRead(&state, 1, sizeof(state), file.get()) != sizeof(state))
    {
        Log(LogLevel::Error, "Failed to read complete RTC state\n");
        return;
    }
    nds->RTC.SetState(state);
}

void EmuInstance::saveRTCData()
{
    RTC::StateData state{};
    nds->RTC.GetState(state);
    QSaveFile file(QString::fromStdString(Platform::GetLocalFilePath("rtc.bin")));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) ||
        file.write(reinterpret_cast<const char*>(&state), sizeof(state)) != sizeof(state) ||
        !file.commit())
        Log(LogLevel::Error, "Failed to save RTC state: %s\n", file.errorString().toUtf8().constData());
}

void EmuInstance::setDateTime()
{
    QDateTime hosttime = QDateTime::currentDateTime();
    QDateTime time = hosttime.addSecs(localCfg.GetInt64("RTC.Offset"));

    nds->RTC.SetDateTime(time.date().year(), time.date().month(), time.date().day(),
                         time.time().hour(), time.time().minute(), time.time().second());
}

void EmuInstance::syncRTC()
{
    if (!localCfg.GetBool("RTC.SyncToHost"))
        return;

    setDateTime();
}


bool EmuInstance::updateConsole() noexcept
{
    // Prepare resources before moving either the active or queued cartridges.
    const int requestedType = globalCfg.GetInt("Emu.ConsoleType");

    auto arm9bios = loadARM9BIOS();
    if (!arm9bios)
        return false;

    auto arm7bios = loadARM7BIOS();
    if (!arm7bios)
        return false;

    auto firmware = loadFirmware(requestedType);
    if (!firmware)
        return false;

#ifdef JIT_ENABLED
    Config::Table jitopt = globalCfg.GetTable("JIT");
    JITArgs _jitargs {
            static_cast<unsigned>(jitopt.GetInt("MaxBlockSize")),
            jitopt.GetBool("LiteralOptimisations"),
            jitopt.GetBool("BranchOptimisations"),
            jitopt.GetBool("FastMemory"),
    };
    auto jitargs = jitopt.GetBool("Enable") ? std::make_optional(_jitargs) : std::nullopt;
#else
    std::optional<JITArgs> jitargs = std::nullopt;
#endif

#ifdef GDBSTUB_ENABLED
    Config::Table gdbopt = localCfg.GetTable("Gdb");
    GDBArgs _gdbargs {
            static_cast<u16>(gdbopt.GetInt("ARM7.Port")),
            static_cast<u16>(gdbopt.GetInt("ARM9.Port")),
            gdbopt.GetBool("ARM7.BreakOnStartup"),
            gdbopt.GetBool("ARM9.BreakOnStartup"),
    };
    auto gdbargs = gdbopt.GetBool("Enabled") ? std::make_optional(_gdbargs) : std::nullopt;
#else
    std::optional<GDBArgs> gdbargs = std::nullopt;
#endif

    NDSArgs ndsargs {
            std::move(arm9bios),
            std::move(arm7bios),
            std::move(*firmware),
            jitargs,
            static_cast<AudioBitDepth>(globalCfg.GetInt("Audio.BitDepth")),
            static_cast<AudioInterpolation>(globalCfg.GetInt("Audio.Interpolation")),
            (double) audioFreq,
            gdbargs,
    };
    NDSArgs* args = &ndsargs;

    std::optional<DSiArgs> dsiargs = std::nullopt;
    if (requestedType == 1)
    {
        auto arm7ibios = loadDSiARM7BIOS();
        if (!arm7ibios)
            return false;

        auto arm9ibios = loadDSiARM9BIOS();
        if (!arm9ibios)
            return false;

        auto nand = loadNAND(*arm7ibios);
        auto sdcard = loadSDCard("DSi.SD");

        DSiArgs _dsiargs {
                std::move(ndsargs),
                std::move(arm9ibios),
                std::move(arm7ibios),
                std::move(nand),
                std::move(sdcard),
                globalCfg.GetBool("DSi.DSP.HLE")
        };

        dsiargs = std::move(_dsiargs);
        args = &(*dsiargs);
    }

    std::unique_ptr<NDS> replacement;
    if (!nds || requestedType != nds->ConsoleType)
    {
        try
        {
            if (requestedType == 1)
                replacement = std::make_unique<DSi>(std::move(dsiargs.value()), this);
            else
                replacement = std::make_unique<NDS>(std::move(ndsargs), this);
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
    }

    QMutexLocker lock(&renderLock);
    auto nextndscart = changeCart ? std::move(nextCart) : (nds ? nds->EjectCart() : nullptr);
    auto nextgbacart = changeGBACart ? std::move(nextGBACart) : (nds ? nds->EjectGBACart() : nullptr);
    changeCart = changeGBACart = false;
    if (auto* cartsd = dynamic_cast<NDSCart::CartSD*>(nextndscart.get()))
        cartsd->SetSDCard(getSDCardArgs("DLDI"));

    consoleType = requestedType;
    if (replacement)
    {
        if (nds)
        {
            saveRTCData();
            delete nds;
        }

        nds = replacement.release();

        nds->Reset();
        loadRTCData();
        emuThread->updateVideoRenderer();
    }
    else
    {
        nds->SetARM7BIOS(*args->ARM7BIOS);
        nds->SetARM9BIOS(*args->ARM9BIOS);
        nds->SetFirmware(std::move(args->Firmware));
        nds->SetJITArgs(args->JIT);
        nds->SetGdbArgs(args->GDB);
        nds->SPU.SetInterpolation(args->Interpolation);
        nds->SPU.SetDegrade10Bit(args->BitDepth);

        if (consoleType == 1)
        {
            DSi* dsi = (DSi*)nds;
            DSiArgs& _dsiargs = *dsiargs;

            dsi->SetDSPHLE(_dsiargs.DSPHLE);
            dsi->ARM7iBIOS = *_dsiargs.ARM7iBIOS;
            dsi->ARM9iBIOS = *_dsiargs.ARM9iBIOS;
            dsi->SetNAND(std::move(_dsiargs.NANDImage));
            dsi->SetSDCard(std::move(_dsiargs.DSiSDCard));
            // We're moving the optional, not the card
            // (inserting std::nullopt here is okay, it means no card)
        }
    }

    // loads the carts later -- to be sure that everything else is initialized
    nds->SetNDSCart(std::move(nextndscart));
    if (consoleType == 1)
        nds->EjectGBACart();
    else
        nds->SetGBACart(std::move(nextgbacart));

    return true;
}

bool EmuInstance::reset(const AssetIdentity::Selection& dsAssets, const AssetIdentity::Selection& gbaAssets)
{
    QString errorstr;
    if (!flushSaveData(errorstr))
    {
        osdAddMessage(0xFFA0A0, "%s", errorstr.toUtf8().constData());
        return false;
    }
    if (!updateConsole()) return false;

    if (consoleType == 1) ejectGBACart();

    nds->Reset();
    setBatteryLevels();
    setDateTime();

    if ((cartType != -1) && ndsSave)
    {
        if (dsAssets.Valid())
        {
            dsAssetPaths = dsAssets;
            baseAssetName = dsAssets.Name.toStdString();
        }
        std::string oldsave = ndsSave->GetPath();
        std::string newsave = getAssetPath(false, localCfg.GetString("SaveFilePath"), ".sav");
        newsave += instanceFileSuffix();
        if (oldsave != newsave)
            ndsSave->SetPath(newsave);
    }

    if ((gbaCartType != -1) && gbaSave)
    {
        if (gbaAssets.Valid())
        {
            gbaAssetPaths = gbaAssets;
            baseGBAAssetName = gbaAssets.Name.toStdString();
        }
        std::string oldsave = gbaSave->GetPath();
        std::string newsave = getAssetPath(true, localCfg.GetString("SaveFilePath"), ".sav");
        newsave += instanceFileSuffix();
        if (oldsave != newsave)
            gbaSave->SetPath(newsave);
    }

    initFirmwareSaveManager();

    if (!baseROMName.empty())
    {
        if (globalCfg.GetBool("Emu.DirectBoot") || nds->NeedsDirectBoot())
        {
            nds->SetupDirectBoot(baseROMName);
        }
    }

    nds->Start();
    loadCheats();
    return true;
}


bool EmuInstance::bootToMenu(QString& errorstr)
{
    if (!flushSaveData(errorstr)) return false;
    // Keep whatever cart is in the console, if any.
    if (!updateConsole())
    {
        // Try to update the console, but keep the existing cart. If that fails...
        errorstr = "Failed to boot the firmware.";
        return false;
    }

    // BIOS and firmware files are loaded, patched, and installed in UpdateConsole
    if (nds->NeedsDirectBoot())
    {
        errorstr = "This firmware is not bootable.";
        return false;
    }

    initFirmwareSaveManager();
    nds->Reset();
    setBatteryLevels();
    setDateTime();
    loadCheats();
    return true;
}

u32 EmuInstance::decompressROM(const u8* inContent, const u32 inSize, unique_ptr<u8[]>& outContent)
{
    const u64 realSize = ZSTD_getFrameContentSize(inContent, inSize);
    const u32 maxSize = 0x40000000;

    if (realSize == ZSTD_CONTENTSIZE_ERROR || (realSize > maxSize && realSize != ZSTD_CONTENTSIZE_UNKNOWN))
    {
        return 0;
    }

    // A frame's declared size does not include any following frames.
    if (realSize != ZSTD_CONTENTSIZE_UNKNOWN &&
        ZSTD_findFrameCompressedSize(inContent, inSize) == inSize)
    {
        if (realSize == 0) return 0;

        try
        {
            auto newOutContent = make_unique<u8[]>(realSize);
            size_t decompressed = ZSTD_decompress(newOutContent.get(), realSize, inContent, inSize);

            if (ZSTD_isError(decompressed) || decompressed != realSize) return 0;

            outContent = std::move(newOutContent);
            return static_cast<u32>(decompressed);
        }
        catch (const std::bad_alloc&)
        {
            return 0;
        }
    }

    unique_ptr<ZSTD_DStream, decltype(&ZSTD_freeDStream)> dStream(ZSTD_createDStream(), ZSTD_freeDStream);
    if (!dStream || ZSTD_isError(ZSTD_initDStream(dStream.get()))) return 0;

    const u32 startSize = 1024 * 1024 * 16;
    unique_ptr<void, decltype(&free)> partialOutContent(malloc(startSize), free);
    if (!partialOutContent) return 0;

    ZSTD_inBuffer inBuf = {inContent, inSize, 0};
    ZSTD_outBuffer outBuf = {partialOutContent.get(), startSize, 0};

    for (;;)
    {
        if (outBuf.pos == outBuf.size && outBuf.size < maxSize)
        {
            const size_t newSize = outBuf.size * 2;
            void* grown = realloc(partialOutContent.get(), newSize);
            if (!grown) return 0;
            partialOutContent.release();
            partialOutContent.reset(grown);
            outBuf.dst = grown;
            outBuf.size = newSize;
        }

        // At the cap, allow checksums/empty frames to finish, but reject another byte.
        u8 overflowByte;
        ZSTD_outBuffer overflow = {&overflowByte, 1, 0};
        ZSTD_outBuffer* output = outBuf.pos == maxSize ? &overflow : &outBuf;
        const size_t previousInput = inBuf.pos;
        const size_t previousOutput = outBuf.pos;
        size_t result = ZSTD_decompressStream(dStream.get(), output, &inBuf);

        if (ZSTD_isError(result) || overflow.pos != 0) return 0;
        if (result == 0 && inBuf.pos == inBuf.size) break;

        // Exhausted input with an unfinished frame must not publish partial output.
        if (inBuf.pos == previousInput && outBuf.pos == previousOutput) return 0;
    }

    if (outBuf.pos == 0) return 0;

    try
    {
        auto newOutContent = make_unique<u8[]>(outBuf.pos);
        memcpy(newOutContent.get(), outBuf.dst, outBuf.pos);

        // inContent can belong to outContent, so replace it only after all decoding.
        outContent = std::move(newOutContent);
        return static_cast<u32>(outBuf.pos);
    }
    catch (const std::bad_alloc&)
    {
        return 0;
    }
}

void EmuInstance::clearBackupState()
{
    if (backupState != nullptr)
    {
        backupState = nullptr;
    }
}

pair<unique_ptr<Firmware>, string> EmuInstance::generateDefaultFirmware()
{
    // Construct the default firmware...
    string settingspath;
    std::unique_ptr<Firmware> firmware = std::make_unique<Firmware>(consoleType);
    assert(firmware->Buffer() != nullptr);

    // Try to open the instanced Wi-fi settings, falling back to the regular Wi-fi settings if they don't exist.
    // We don't need to save the whole firmware, just the part that may actually change.
    std::string wfcsettingspath = kWifiSettingsPath;
    settingspath = wfcsettingspath + instanceFileSuffix();
    FileHandle* f = Platform::OpenLocalFile(settingspath, FileMode::Read);
    if (!f)
    {
        settingspath = wfcsettingspath;
        f = Platform::OpenLocalFile(settingspath, FileMode::Read);
    }

    // If using generated firmware, we keep the wi-fi settings on the host disk separately.
    // Wi-fi access point data includes Nintendo WFC settings,
    // and if we didn't keep them then the player would have to reset them in each session.
    if (f)
    { // If we have Wi-fi settings to load...
        constexpr unsigned TOTAL_WFC_SETTINGS_SIZE = 3 * (sizeof(Firmware::WifiAccessPoint) + sizeof(Firmware::ExtendedWifiAccessPoint));

        // The access point and extended access point segments might
        // be in different locations depending on the firmware revision,
        // but our generated firmware always keeps them next to each other.
        // (Extended access points first, then regular ones.)

        if (!FileRead(firmware->GetExtendedAccessPointPosition(), TOTAL_WFC_SETTINGS_SIZE, 1, f))
        { // If we couldn't read the Wi-fi settings from this file...
            Platform::Log(Platform::LogLevel::Warn, "Failed to read Wi-fi settings from \"%s\"; using defaults instead\n", wfcsettingspath.c_str());

            firmware->GetAccessPoints() = {
                    Firmware::WifiAccessPoint(consoleType),
                    Firmware::WifiAccessPoint(),
                    Firmware::WifiAccessPoint(),
            };

            firmware->GetExtendedAccessPoints() = {
                    Firmware::ExtendedWifiAccessPoint(),
                    Firmware::ExtendedWifiAccessPoint(),
                    Firmware::ExtendedWifiAccessPoint(),
            };
        }

        firmware->UpdateChecksums();

        CloseFile(f);
    }

    // If we don't have Wi-fi settings to load,
    // then the defaults will have already been populated by the constructor.
    return std::make_pair(std::move(firmware), std::move(wfcsettingspath));
}

bool EmuInstance::parseMacAddress(void* data)
{
    const std::string mac_in = localCfg.GetString("Firmware.MAC");
    u8* mac_out = (u8*)data;

    int o = 0;
    u8 tmp = 0;
    for (int i = 0; i < 18; i++)
    {
        char c = mac_in[i];
        if (c == '\0') break;

        int n;
        if      (c >= '0' && c <= '9') n = c - '0';
        else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
        else continue;

        if (!(o & 1))
            tmp = n;
        else
            mac_out[o >> 1] = n | (tmp << 4);

        o++;
        if (o >= 12) return true;
    }

    return false;
}

void EmuInstance::customizeFirmware(Firmware& firmware, bool overridesettings) noexcept
{
    if (overridesettings)
    {
        auto &currentHeader = firmware.GetHeader();
        auto &currentData = firmware.GetEffectiveUserData();

        auto firmcfg = localCfg.GetTable("Firmware");

        // setting up username
        auto username = firmcfg.GetQString("Username");
        if (!username.isEmpty())
        { // If the frontend defines a username, take it. If not, leave the existing one.
            size_t usernameLength = std::min((int) username.length(), 10);
            currentData.NameLength = usernameLength;
            memcpy(currentData.Nickname, username.utf16(), usernameLength * sizeof(char16_t));
        }

        auto language = static_cast<Firmware::Language>(firmcfg.GetInt("Language"));
        if (language <= Firmware::Language::Korean)
        { // If the frontend specifies a language (rather than using the existing value)...
            bool extlang = language >= Firmware::Language::Chinese;

            // ..clear the existing language...
            currentData.Settings &= ~0x7;

            // ...and set the new one.
            currentData.Settings |= extlang ? Firmware::Language::English : language;
            currentData.ExtendedSettings.ExtendedLanguage = language;

            if (extlang && currentData.ExtendedSettings.Unknown0 != 0x01)
            {
                // enable the extended settings header if not present
                if (currentHeader.ConsoleType == 0xFF)
                    currentHeader.ConsoleType = 0x43;
                else
                    currentHeader.ConsoleType |= 0x43;
                currentData.ExtendedSettings.Unknown0 = 0x01;
                currentData.ExtendedSettings.SupportedLanguageMask = 0x7F;
            }
            if (currentData.ExtendedSettings.Unknown0 == 0x01)
                currentData.ExtendedSettings.SupportedLanguageMask |= 1 << language;
        }

        // setting up color
        u8 favoritecolor = firmcfg.GetInt("FavouriteColour");
        if (favoritecolor != 0xFF)
        {
            currentData.FavoriteColor = favoritecolor;
        }

        u8 birthmonth = firmcfg.GetInt("BirthdayMonth");
        if (birthmonth != 0)
        { // If the frontend specifies a birth month (rather than using the existing value)...
            currentData.BirthdayMonth = birthmonth;
        }

        u8 birthday = firmcfg.GetInt("BirthdayDay");
        if (birthday != 0)
        { // If the frontend specifies a birthday (rather than using the existing value)...
            currentData.BirthdayDay = birthday;
        }

        // setup message
        auto message = firmcfg.GetQString("Message");
        if (!message.isEmpty())
        {
            size_t messageLength = std::min((int) message.length(), 26);
            currentData.MessageLength = messageLength;
            memcpy(currentData.Message, message.data(), messageLength * sizeof(char16_t));
        }

        // Do not repair a stale backup's checksum: it may then supersede the
        // valid profile that we just customized.
        currentData.UpdateChecksum();
    }

    MacAddress mac;
    bool rep = false;
    auto& header = firmware.GetHeader();

    memcpy(&mac, header.MacAddr.data(), sizeof(MacAddress));

    if (overridesettings)
    {
        MacAddress configuredMac{};
        rep = parseMacAddress(&configuredMac) && configuredMac != MacAddress();

        if (rep)
        {
            mac = configuredMac;
        }
    }

    if (instanceID > 0)
    {
        rep = true;
        mac[3] += instanceID;
        mac[4] += instanceID*0x44;
        mac[5] += instanceID*0x10;
    }

    if (rep)
    {
        mac[0] &= 0xFC; // ensure the MAC isn't a broadcast MAC
        header.MacAddr = mac;
        header.UpdateChecksum();
    }
}

// Loads ROM data without parsing it. Works for GBA and NDS ROMs.
bool EmuInstance::loadROMData(const QStringList& filepath, std::unique_ptr<u8[]>& filedata, u32& filelen, string& basepath, string& romname) noexcept
{
    try
    {
        if (filepath.empty()) return false;
        string filename = filepath.at(0).toStdString();
        string membername = filename;
        unique_ptr<u8[]> data;
        u32 length = 0;

        if (filepath.count() == 1)
        {
            const unique_ptr<FileHandle, decltype(&Platform::CloseFile)> file(
                Platform::OpenFile(filename, FileMode::Read), Platform::CloseFile);
            if (!file) return false;
            const u64 size = Platform::FileLength(file.get());
            if (!size || size > 0x40000000) return false;

            data = std::make_unique_for_overwrite<u8[]>(static_cast<u32>(size));
            if (Platform::FileRead(data.get(), 1, size, file.get()) != size) return false;
            length = static_cast<u32>(size);

            if (filename.length() > 4 && filename.ends_with(".zst"))
            {
                length = decompressROM(data.get(), length, data);
                if (!length) return false;
                filename.resize(filename.length() - 4);
                membername = filename;
            }
        }
#ifdef ARCHIVE_SUPPORT_ENABLED
        else if (filepath.count() == 2)
        {
            const s32 read = Archive::ExtractFileFromArchive(filepath.at(0), filepath.at(1), data, &length);
            if (read < 0 || !data || !length || length > 0x40000000 || static_cast<u32>(read) != length)
                return false;
            membername = filepath.at(1).toStdString();
        }
#endif
        else return false;

        const int separator = lastSep(filename);
        string directory = separator < 0 ? "" : filename.substr(0, separator);
        string name = membername.substr(lastSep(membername) + 1);
        // Commit bytes and names together, after every read/decode/allocation.
        filedata = std::move(data);
        filelen = length;
        basepath = std::move(directory);
        romname = std::move(name);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        return false;
    }
}

QString EmuInstance::getSavErrorString(std::string& filepath, bool gba)
{
    std::string console = gba ? "GBA" : "DS";
    std::string err1 = "Unable to write to ";
    std::string err2 = " save.\nPlease check file/folder write permissions.\n\nAttempted to Access:\n";

    err1 += console + err2 + filepath;

    return QString::fromStdString(err1);
}

bool EmuInstance::loadSaveRAM(string path, string original, bool gba, unique_ptr<u8[]>& data, u32& length, QString& errorstr)
{
    std::unique_ptr<FileHandle, decltype(&Platform::CloseFile)> file(
        Platform::OpenFile(path, FileMode::Read), Platform::CloseFile);
    if (!file && Platform::FileExists(path))
    {
        errorstr = "Failed to read the existing save file.";
        return false;
    }
    string writable = file ? path : original;
    if (!Platform::CheckFileWritable(writable))
    {
        errorstr = getSavErrorString(writable, gba);
        return false;
    }
    if (!file) file.reset(Platform::OpenFile(original, FileMode::Read));
    if (!file)
    {
        if (Platform::FileExists(original))
        {
            errorstr = "Failed to read the existing save file.";
            return false;
        }
        return true; // A new game has no save yet.
    }

    const u64 size = Platform::FileLength(file.get());
    if (size > std::numeric_limits<u32>::max())
    {
        errorstr = "The save file is too large.";
        return false;
    }
    try
    {
        auto bytes = size ? std::make_unique<u8[]>(static_cast<u32>(size)) : nullptr;
        if (size && Platform::FileRead(bytes.get(), 1, size, file.get()) != size)
        {
            errorstr = "Failed to read the complete save file.";
            return false;
        }
        data = std::move(bytes);
        length = static_cast<u32>(size);
        return true;
    }
    catch (const std::bad_alloc&)
    {
        errorstr = "Not enough memory to load the save file.";
        return false;
    }
}

bool EmuInstance::loadROM(QStringList filepath, bool reset, QString& errorstr, const AssetIdentity::Selection& assets)
{
    unique_ptr<u8[]> filedata = nullptr;
    u32 filelen;
    std::string basepath;
    std::string romname;

    if (!loadROMData(filepath, filedata, filelen, basepath, romname))
    {
        errorstr = "Failed to load the DS ROM.";
        return false;
    }

    // Commit current bytes before reading a prospective save, including when
    // reopening the same game/path. Keep the current manager on any failure.
    if (reset ? !flushSaveData(errorstr) : !FlushSave(ndsSave.get(), errorstr)) return false;
    string asset = assets.Valid() ? assets.Name.toStdString() : romname.substr(0, romname.rfind('.'));
    const string saveDir = assets.Valid() ? assets.SaveDirectory.toStdString() : localCfg.GetString("SaveFilePath");

    u32 savelen = 0;
    std::unique_ptr<u8[]> savedata = nullptr;

    std::string savname = AssetPath(saveDir.empty() ? basepath : saveDir, asset, ".sav");
    std::string origsav = savname;
    savname += instanceFileSuffix();

    if (!loadSaveRAM(savname, origsav, false, savedata, savelen, errorstr)) return false;

    NDSCart::NDSCartArgs cartargs {
            // Don't load the SD card itself yet, because we don't know if
            // the ROM is homebrew or not.
            // So this is the card we *would* load if the ROM were homebrew.
            .SDCard = getSDCardArgs("DLDI"),
            .SRAM = std::move(savedata),
            .SRAMLength = savelen,
    };

    unique_ptr<NDSCart::CartCommon> cart;
    unique_ptr<SaveManager> newSave;
    try
    {
        cart = NDSCart::ParseROM(std::move(filedata), filelen, this, std::move(cartargs));
        if (cart) newSave = std::make_unique<SaveManager>(savname);
    }
    catch (const std::bad_alloc&)
    {
        errorstr = "Not enough memory to prepare the DS cartridge.";
        return false;
    }
    if (!cart)
    {
        // If we couldn't parse the ROM...
        errorstr = "Failed to load the DS ROM.";
        return false;
    }

    auto oldSave = std::move(ndsSave);
    ndsSave = std::move(newSave);
    if (reset)
    {
        auto queuedCart = std::move(nextCart);
        const bool queuedChange = changeCart;
        nextCart = std::move(cart);
        changeCart = true;

        if (!updateConsole())
        {
            nextCart = std::move(queuedCart);
            changeCart = queuedChange;
            ndsSave = std::move(oldSave);
            errorstr = "Failed to load the DS ROM.";
            return false;
        }

        initFirmwareSaveManager();
        if (consoleType == 1) ejectGBACart();
        nds->Reset();

        if (globalCfg.GetBool("Emu.DirectBoot") || nds->NeedsDirectBoot())
        { // If direct boot is enabled or forced...
            nds->SetupDirectBoot(romname);
        }

        setBatteryLevels();
        setDateTime();
    }
    else
    {
        if (emuIsActive())
        {
            nds->SetNDSCart(std::move(cart));
        }
        else
        {
            nextCart = std::move(cart);
            changeCart = true;
        }
    }

    cartType = 0;
    baseROMDir = std::move(basepath);
    baseROMName = std::move(romname);
    baseAssetName = std::move(asset);
    dsAssetPaths = assets;
    clearBackupState();
    if (reset || emuIsActive()) loadCheats();

    return true; // success
}

void EmuInstance::ejectCart()
{
    QString errorstr;
    if (!FlushSave(ndsSave.get(), errorstr))
    {
        osdAddMessage(0xFFA0A0, "%s", errorstr.toUtf8().constData());
        return;
    }
    ndsSave = nullptr;
    clearBackupState();

    if (emuIsActive())
    {
        nds->EjectCart();
        unloadCheats();
    }
    else
    {
        nextCart = nullptr;
        changeCart = true;
    }

    cartType = -1;
    baseROMDir = "";
    baseROMName = "";
    baseAssetName = "";
    dsAssetPaths = {};
}

bool EmuInstance::cartInserted()
{
    return cartType != -1;
}

QString EmuInstance::cartLabel()
{
    if (cartType == -1)
        return "(none)";

    QString ret = QString::fromStdString(baseROMName);

    int maxlen = 32;
    if (ret.length() > maxlen)
        ret = ret.left(maxlen-6) + "..." + ret.right(3);

    return ret;
}


bool EmuInstance::loadGBAROM(QStringList filepath, QString& errorstr, const AssetIdentity::Selection& assets)
{
    if (consoleType == 1)
    {
        errorstr = "The DSi doesn't have a GBA slot.";
        return false;
    }

    unique_ptr<u8[]> filedata = nullptr;
    u32 filelen;
    std::string basepath;
    std::string romname;

    if (!loadROMData(filepath, filedata, filelen, basepath, romname))
    {
        errorstr = "Failed to load the GBA ROM.";
        return false;
    }

    if (!FlushSave(gbaSave.get(), errorstr)) return false;
    string asset = assets.Valid() ? assets.Name.toStdString() : romname.substr(0, romname.rfind('.'));
    const string saveDir = assets.Valid() ? assets.SaveDirectory.toStdString() : localCfg.GetString("SaveFilePath");

    u32 savelen = 0;
    std::unique_ptr<u8[]> savedata = nullptr;

    std::string savname = AssetPath(saveDir.empty() ? basepath : saveDir, asset, ".sav");
    std::string origsav = savname;
    savname += instanceFileSuffix();

    if (!loadSaveRAM(savname, origsav, true, savedata, savelen, errorstr)) return false;

    unique_ptr<GBACart::CartCommon> cart;
    unique_ptr<SaveManager> newSave;
    try
    {
        cart = GBACart::ParseROM(std::move(filedata), filelen, std::move(savedata), savelen, this);
        if (cart) newSave = std::make_unique<SaveManager>(savname);
    }
    catch (const std::bad_alloc&)
    {
        errorstr = "Not enough memory to prepare the GBA cartridge.";
        return false;
    }
    if (!cart)
    {
        errorstr = "Failed to load the GBA ROM.";
        return false;
    }

    gbaSave = std::move(newSave);
    if (emuIsActive())
    {
        nds->SetGBACart(std::move(cart));
    }
    else
    {
        nextGBACart = std::move(cart);
        changeGBACart = true;
    }

    gbaCartType = 0;
    baseGBAROMDir = std::move(basepath);
    baseGBAROMName = std::move(romname);
    baseGBAAssetName = std::move(asset);
    gbaAssetPaths = assets;
    clearBackupState();
    return true;
}

void EmuInstance::loadGBAAddon(int type, QString& errorstr)
{
    if (consoleType == 1)
    {
        errorstr = "The DSi doesn't have a GBA slot.";
        return;
    }

    if (!FlushSave(gbaSave.get(), errorstr)) return;

    auto cart = GBACart::LoadAddon(type, this);
    if (!cart)
    {
        errorstr = "Failed to load the GBA addon.";
        return;
    }

    if (emuIsActive())
    {
        nds->SetGBACart(std::move(cart));
    }
    else
    {
        nextGBACart = std::move(cart);
        changeGBACart = true;
    }

    gbaSave = nullptr;
    clearBackupState();
    gbaCartType = type;
    gbaAssetPaths = {};
    baseGBAROMDir = "";
    baseGBAROMName = "";
    baseGBAAssetName = "";
}

void EmuInstance::ejectGBACart()
{
    QString errorstr;
    if (!FlushSave(gbaSave.get(), errorstr))
    {
        osdAddMessage(0xFFA0A0, "%s", errorstr.toUtf8().constData());
        return;
    }
    gbaSave = nullptr;
    clearBackupState();

    if (emuIsActive())
    {
        nds->EjectGBACart();
    }
    else
    {
        nextGBACart = nullptr;
        changeGBACart = true;
    }

    gbaCartType = -1;
    baseGBAROMDir = "";
    baseGBAROMName = "";
    baseGBAAssetName = "";
    gbaAssetPaths = {};
}

bool EmuInstance::gbaCartInserted()
{
    return gbaCartType != -1;
}

QString EmuInstance::gbaAddonName(int addon)
{
    switch (addon)
    {
    case GBAAddon_RumblePak:
        return "Rumble Pak";
    case GBAAddon_RAMExpansion:
        return "Memory expansion";
    case GBAAddon_SolarSensorBoktai1:
        return "Solar Sensor (Boktai 1)";
    case GBAAddon_SolarSensorBoktai2:
        return "Solar Sensor (Boktai 2)";
    case GBAAddon_SolarSensorBoktai3:
        return "Solar Sensor (Boktai 3)";
    case GBAAddon_MotionPakHomebrew:
        return "Motion Pak (Homebrew)";
    case GBAAddon_MotionPakRetail:
        return "Motion Pack (Retail)";
    case GBAAddon_GuitarGrip:
        return "Guitar Grip";
    }

    return "???";
}

QString EmuInstance::gbaCartLabel()
{
    if (consoleType == 1) return "none (DSi)";

    if (gbaCartType == 0)
    {
        QString ret = QString::fromStdString(baseGBAROMName);

        int maxlen = 32;
        if (ret.length() > maxlen)
            ret = ret.left(maxlen-6) + "..." + ret.right(3);

        return ret;
    }
    else if (gbaCartType != -1)
    {
        return gbaAddonName(gbaCartType);
    }

    return "(none)";
}


void EmuInstance::romIcon(const u8 (&data)[512], const u16 (&palette)[16], u32 (&iconRef)[32*32])
{
    u32 paletteRGBA[16];
    for (int i = 0; i < 16; i++)
    {
        u8 r = ((palette[i] >> 0)  & 0x1F) * 255 / 31;
        u8 g = ((palette[i] >> 5)  & 0x1F) * 255 / 31;
        u8 b = ((palette[i] >> 10) & 0x1F) * 255 / 31;
        u8 a = i ? 255 : 0;
        paletteRGBA[i] = r | (g << 8) | (b << 16) | (a << 24);
    }

    int count = 0;
    for (int ytile = 0; ytile < 4; ytile++)
    {
        for (int xtile = 0; xtile < 4; xtile++)
        {
            for (int ypixel = 0; ypixel < 8; ypixel++)
            {
                for (int xpixel = 0; xpixel < 8; xpixel++)
                {
                    u8 pal_index = count % 2 ? data[count/2] >> 4 : data[count/2] & 0x0F;
                    iconRef[ytile*256 + ypixel*32 + xtile*8 + xpixel] = paletteRGBA[pal_index];
                    count++;
                }
            }
        }
    }
}

#define SEQ_FLIPV(i) ((i & 0b1000000000000000) >> 15)
#define SEQ_FLIPH(i) ((i & 0b0100000000000000) >> 14)
#define SEQ_PAL(i) ((i & 0b0011100000000000) >> 11)
#define SEQ_BMP(i) ((i & 0b0000011100000000) >> 8)
#define SEQ_DUR(i) ((i & 0b0000000011111111) >> 0)

void EmuInstance::animatedROMIcon(const u8 (&data)[8][512], const u16 (&palette)[8][16], const u16 (&sequence)[64], u32 (&animatedIconRef)[64][32*32], std::vector<int> &animatedSequenceRef)
{
    for (int i = 0; i < 64; i++)
    {
        if (!sequence[i])
            break;

        romIcon(data[SEQ_BMP(sequence[i])], palette[SEQ_PAL(sequence[i])], animatedIconRef[i]);
        u32* frame = animatedIconRef[i];

        if (SEQ_FLIPH(sequence[i]))
        {
            for (int x = 0; x < 32; x++)
            {
                for (int y = 0; y < 32/2; y++)
                {
                    std::swap(frame[x * 32 + y], frame[x * 32 + (32 - 1 - y)]);
                }
            }
        }
        if (SEQ_FLIPV(sequence[i]))
        {
            for (int x = 0; x < 32/2; x++)
            {
                for (int y = 0; y < 32; y++)
                {
                    std::swap(frame[x * 32 + y], frame[(32 - 1 - x) * 32 + y]);
                }
            }
        }

        for (int j = 0; j < SEQ_DUR(sequence[i]); j++)
            animatedSequenceRef.push_back(i);
    }
}
