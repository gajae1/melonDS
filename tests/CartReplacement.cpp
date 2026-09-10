// SPDX-License-Identifier: GPL-3.0-or-later
// Current frontend replacement/console methods, real core/carts/save manager.
// Only resource selection and host files are scripted; no user ROM/BIOS.
#include <cassert>
#include <cstring>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <QCoreApplication>
#include <QFile>
#include <QDir>
#include <QMutexLocker>
#include <QSaveFile>
#include <QTemporaryDir>
#include "NDS.h"
#include "DSi.h"
#include "NDS_Header.h"
#include "NDSCart/CartSD.h"
#include "SaveManager.h"
#include "AssetIdentity.h"
#include "Platform.h"
using namespace melonDS;
using namespace melonDS::Platform;
using std::string;
using std::unique_ptr;
using std::make_unique;

static bool denyNewSave = false;
static bool denyRead = false;
enum class ReadFailure { None, Short, Error, Oversize };
static ReadFailure readFailure = ReadFailure::None;
static unsigned openFiles = 0;
namespace melonDS::Platform
{
FileHandle* OpenCartFile(const string& path, FileMode)
{
    if (denyRead) return nullptr;
    auto file = make_unique<QFile>(QString::fromStdString(path));
    if (!file->open(QIODevice::ReadOnly)) return nullptr;
    ++openFiles;
    return reinterpret_cast<FileHandle*>(file.release());
}
bool CloseCartFile(FileHandle* handle)
{
    delete reinterpret_cast<QFile*>(handle);
    --openFiles;
    return true;
}
u64 CartFileLength(FileHandle* handle)
{
    if (readFailure == ReadFailure::Oversize) return u64(UINT32_MAX) + 1;
    return reinterpret_cast<QFile*>(handle)->size();
}
void RewindCartFile(FileHandle* handle) { reinterpret_cast<QFile*>(handle)->seek(0); }
u64 ReadCartFile(void* data, u64 size, u64 count, FileHandle* handle)
{
    if (!size || !count) return 0;
    if (readFailure == ReadFailure::Error) return u64(-1);
    const auto read = reinterpret_cast<QFile*>(handle)->read(static_cast<char*>(data),
        size * count - (readFailure == ReadFailure::Short ? 1 : 0));
    return read > 0 ? read / size : read;
}
bool CartFileWritable(const string&) { return !denyNewSave; }
bool CartFileExists(const string& path) { return QFile::exists(QString::fromStdString(path)); }
}

struct FixtureSettings
{
    int mode = 0;
    string savePath;
    FixtureSettings GetTable(const string&) const { return *this; }
    int GetInt(const string& key) const { return key == "Emu.ConsoleType" ? mode : 0; }
    bool GetBool(const string&) const { return false; }
    string GetString(const string&) const { return savePath; }
};
namespace Config { using Table = FixtureSettings; }

static unique_ptr<u8[]> ROM(bool gba, u32& length, bool invalid = false)
{
    length = invalid ? 8 : (gba ? 0x100 : 0x8000);
    auto data = make_unique<u8[]>(length);
    if (!gba && !invalid)
    {
        auto* header = reinterpret_cast<NDSHeader*>(data.get());
        std::memcpy(header->GameCode, "ZZZA", 4);
        header->ARM9ROMOffset = 0x4000;
        header->ARM7ROMOffset = 0x4004;
        header->ARM9Size = header->ARM7Size = 4;
        header->ARM9RAMAddress = header->ARM9EntryAddress = 0x02000000;
        header->ARM7RAMAddress = header->ARM7EntryAddress = 0x02000200;
        const u32 loop = 0xEAFFFFFE;
        std::memcpy(data.get() + 0x4000, &loop, 4);
        std::memcpy(data.get() + 0x4004, &loop, 4);
    }
    return data;
}

struct FixtureThread { void updateVideoRenderer() {} };
struct CartLoader
{
    NDS* nds;
    FixtureSettings globalCfg, localCfg;
    QMutex renderLock;
    FixtureThread thread;
    FixtureThread* emuThread = &thread;
    int consoleType = 0, audioFreq = 48000;
    int cartType = 0, gbaCartType = 0;
    bool active = true, resourcesOK = true;
    bool changeCart = false, changeGBACart = false;
    string baseROMDir, baseROMName = "current.nds", baseAssetName = "current";
    string baseGBAROMDir, baseGBAROMName = "current.gba", baseGBAAssetName = "current";
    AssetIdentity::Selection dsAssetPaths, gbaAssetPaths;
    string incomingDir, cheatAsset;
    unique_ptr<NDSCart::CartCommon> nextCart;
    unique_ptr<GBACart::CartCommon> nextGBACart;
    unique_ptr<SaveManager> ndsSave, gbaSave, firmwareSave;

    CartLoader()
    {
        NDSArgs args;
        args.JIT = std::nullopt;
        nds = new NDS(std::move(args));
        nds->Reset();
        u32 size;
        auto ds = ROM(false, size);
        nds->SetNDSCart(NDSCart::ParseROM(std::move(ds), size, this));
        auto gba = ROM(true, size);
        nds->SetGBACart(GBACart::ParseROM(std::move(gba), size, this));
        nds->ARM9Write32(0x02001000, 0xDEADBEEF);
    }
    ~CartLoader() { delete nds; }
    bool emuIsActive() { return active; }
    string instanceFileSuffix() { return {}; }
    bool loadROMData(const QStringList& paths, unique_ptr<u8[]>& data, u32& length,
                     string& base, string& name) noexcept
    {
        const auto path = paths.front();
        if (path.startsWith("missing")) return false;
        data = ROM(path.endsWith(".gba"), length, path.startsWith("invalid"));
        base = incomingDir;
        name = path.toStdString();
        return true;
    }
    std::optional<FATStorageArgs> getSDCardArgs(const string&) { return std::nullopt; }
    std::optional<FATStorage> loadSDCard(const string&) { return std::nullopt; }
    unique_ptr<ARM9BIOSImage> loadARM9BIOS()
    { return resourcesOK ? make_unique<ARM9BIOSImage>(FreeBIOSGetNtrArm9()) : nullptr; }
    unique_ptr<ARM7BIOSImage> loadARM7BIOS() { return make_unique<ARM7BIOSImage>(FreeBIOSGetNtrArm7()); }
    unique_ptr<DSiBIOSImage> loadDSiARM7BIOS() { return nullptr; }
    unique_ptr<DSiBIOSImage> loadDSiARM9BIOS() { return nullptr; }
    std::optional<Firmware> loadFirmware(int) { return Firmware(0); }
    std::optional<DSi_NAND::NANDImage> loadNAND(const DSiBIOSImage&) { return std::nullopt; }
    void initFirmwareSaveManager() {}
    void setBatteryLevels() {}
    void setDateTime() {}
    void saveRTCData() {}
    void loadRTCData() {}
    void loadCheats() { cheatAsset = baseAssetName; }
    void clearBackupState() {}
    void osdAddMessage(unsigned, const char*, ...) {}
    void ejectGBACart() { std::abort(); } // DSi success is outside this fixture.
    bool flushSaveData(QString& errorstr);
    bool loadSaveRAM(string path, string original, bool gba,
                     unique_ptr<u8[]>& data, u32& length, QString& errorstr);
    string getAssetPath(bool gba, const string& configpath, const string& ext, const string& file);
    QString getSavErrorString(string& filepath, bool gba);
    bool loadROM(QStringList filepath, bool reset, QString& errorstr, const AssetIdentity::Selection& assets = {});
    bool loadGBAROM(QStringList filepath, QString& errorstr, const AssetIdentity::Selection& assets = {});
    bool updateConsole() noexcept;
    bool reset(const AssetIdentity::Selection& dsAssets = {}, const AssetIdentity::Selection& gbaAssets = {});
};

static bool captureCartSave = false;
namespace melonDS::Platform
{
void WriteNDSSave(const u8* data, u32 length, u32 offset, u32 count, void* userdata)
{
    if (!captureCartSave) return;
    auto* loader = static_cast<CartLoader*>(userdata);
    if (!loader || !loader->ndsSave) std::abort();
    loader->ndsSave->RequestFlush(data, length, offset, count);
}
}

// Core uses PlatformHeadless; only these frontend definitions use Qt fixture files.
#define OpenFile OpenCartFile
#define CloseFile CloseCartFile
#define FileLength CartFileLength
#define FileRewind RewindCartFile
#define FileRead ReadCartFile
#define CheckFileWritable CartFileWritable
#define FileExists CartFileExists
#include "SaveManager.cpp"
#define EmuInstance CartLoader
#include "cartBuildPath.inc"
#include "cartFlushSave.inc"
#include "cartFlushAll.inc"
#include "cartAssetPath.inc"
#include "cartSaveError.inc"
#include "cartReadSave.inc"
#include "cartLoadROM.inc"
#include "cartLoadGBA.inc"
#include "cartUpdateConsole.inc"
#include "cartReset.inc"
#undef EmuInstance
#undef FileExists
#undef CheckFileWritable
#undef FileRead
#undef FileRewind
#undef FileLength
#undef CloseFile
#undef OpenFile

static QByteArray ReadSaveFile(const string& path)
{
    QFile file(QString::fromStdString(path));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
static void Queue(SaveManager& manager, const QByteArray& bytes)
{
    manager.RequestFlush(reinterpret_cast<const u8*>(bytes.data()), bytes.size(), 0, bytes.size());
    manager.CheckFlush();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    const string test = argv[1];
    const bool gba = test.starts_with("gba-");
    const bool blocked = test.ends_with("pending-failure");
    const bool readInput = test.starts_with("read-");
    const bool sameSave = test == "ds-same-save";
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    CartLoader loader;
    loader.baseROMDir = loader.baseGBAROMDir = directory.path().toStdString();
    loader.incomingDir = loader.baseROMDir;
    const string oldPath = directory.filePath(blocked ? "missing/current.sav" :
                                              gba ? "current-gba.sav" : "current.sav").toStdString();
    const string newPath = sameSave ? oldPath : directory.filePath("incoming.sav").toStdString();
    const QByteArray previousTarget(8192, '\xA5');
    if (readInput || test.ends_with("existing-writable"))
    {
        QFile file(QString::fromStdString(newPath));
        if (!file.open(QIODevice::WriteOnly) || file.write(previousTarget) != previousTarget.size()) return 2;
    }
    auto& manager = gba ? loader.gbaSave : loader.ndsSave;
    manager = make_unique<SaveManager>(oldPath);
    SaveManager* oldManager = manager.get();
    const QByteArray pending(8192, '\x5A');
    Queue(*manager, pending);
    NDS* oldConsole = loader.nds;
    const auto* oldDS = loader.nds->GetNDSCart();
    const auto* oldGBA = loader.nds->GetGBACart();
    if (!oldDS || !oldGBA) return 2;
    if (test.ends_with("writable")) denyNewSave = true;
    if (test == "ds-console-failure" || test == "ds-queued-failure") loader.resourcesOK = false;
    if (test == "console-retain") { loader.resourcesOK = false; loader.globalCfg.mode = 1; }
    if (test == "gba-queued") loader.active = false;
    if (test == "ds-queued-failure")
    {
        u32 size;
        auto queued = ROM(false, size);
        loader.nextCart = NDSCart::ParseROM(std::move(queued), size, &loader);
        loader.changeCart = true;
        loader.active = false;
    }
    const auto* oldQueued = loader.nextCart.get();
    if (test == "read-short") readFailure = ReadFailure::Short;
    if (test == "read-error") readFailure = ReadFailure::Error;
    if (test == "read-oversize") readFailure = ReadFailure::Oversize;
    if (test == "read-denied") denyRead = true;
    QString error;
    if (test == "ds-import-partial")
    {
        if (!loader.loadROM({"current.nds"}, false, error)) return 2;
        const u32 length = loader.nds->GetNDSSaveLength();
        if (length < 64) return 2;
        QByteArray expected(reinterpret_cast<const char*>(loader.nds->GetNDSSave()), length);
        const QByteArray prefix(32, '\x3C');
        std::memcpy(expected.data(), prefix.constData(), prefix.size());
        captureCartSave = true;
        loader.nds->SetNDSSave(reinterpret_cast<const u8*>(prefix.constData()), prefix.size());
        captureCartSave = false;
        const bool passed = loader.nds->GetNDSSaveLength() == length &&
            !std::memcmp(loader.nds->GetNDSSave(), expected.constData(), length) &&
            loader.ndsSave->Flush() && ReadSaveFile(oldPath) == expected;
        std::printf("partial import preserves complete cart SRAM and save file: %s\n", passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }
    if (test == "ds-asset-path" || test == "gba-asset-path" || test == "asset-reset" || test == "asset-reset-failure")
    {
        const QString target = directory.filePath("separate");
        if (!QDir().mkpath(target)) return 2;
        AssetIdentity::Selection selected{{"generated.nds"}, "current-separated", target, target, target};
        const bool accepted = gba ? loader.loadGBAROM({"current.gba"}, error, selected) :
            loader.loadROM({"current.nds"}, false, error, selected);
        auto& currentManager = gba ? loader.gbaSave : loader.ndsSave;
        const auto expected = (target + "/current-separated.sav").toStdString();
        bool passed = accepted && currentManager && currentManager->GetPath() == expected &&
            ReadSaveFile(oldPath) == pending &&
            loader.getAssetPath(gba, "edited-but-not-applied", ".mch", "") ==
                (target + "/current-separated.mch").toStdString() &&
            loader.getAssetPath(gba, "edited-but-not-applied", ".ml0", "") ==
                (target + "/current-separated.ml0").toStdString();
        if (passed && (test == "asset-reset" || test == "asset-reset-failure"))
        {
            const auto nextDir = directory.filePath("relocated");
            if (!QDir().mkpath(nextDir)) return 2;
            auto next = selected;
            next.Name = "explicit-relocation";
            next.SaveDirectory = next.StateDirectory = next.CheatDirectory = nextDir;
            loader.localCfg.savePath = nextDir.toStdString();
            const QByteArray bytes(8192, '\x6B');
            captureCartSave = true;
            loader.nds->SetNDSSave(reinterpret_cast<const u8*>(bytes.constData()), bytes.size());
            captureCartSave = false;
            loader.resourcesOK = test != "asset-reset-failure";
            const auto oldSelection = loader.dsAssetPaths;
            const bool reset = loader.reset(next, {});
            const auto nextPath = (nextDir + "/explicit-relocation.sav").toStdString();
            passed &= reset == loader.resourcesOK && currentManager->Flush() &&
                ReadSaveFile(expected) == bytes &&
                std::memcmp(loader.nds->GetNDSSave(), bytes.constData(), bytes.size()) == 0;
            if (reset)
                passed &= currentManager->GetPath() == nextPath && ReadSaveFile(nextPath) == bytes &&
                    loader.dsAssetPaths.Name == next.Name;
            else
                passed &= currentManager->GetPath() == expected && !QFile::exists(QString::fromStdString(nextPath)) &&
                    loader.dsAssetPaths.Name == oldSelection.Name;
        }
        std::printf("selected asset paths preserve old data and frozen roots: %s\n", passed ? "PASS" : "FAIL");
        if (!passed) std::printf("accepted=%d path=%s expected=%s old-after=%lld pending=%lld\n",
            accepted, currentManager ? currentManager->GetPath().c_str() : "(null)", expected.c_str(),
            static_cast<long long>(ReadSaveFile(oldPath).size()),
            static_cast<long long>(pending.size()));
        return passed ? 0 : 1;
    }
    const QString name = QString(test.ends_with("invalid") ? "invalid" : sameSave ? "current" : "incoming") + (gba ? ".gba" : ".nds");
    const bool accepted = test == "console-retain" ? loader.updateConsole() :
        gba ? loader.loadGBAROM({name}, error) : loader.loadROM({name},
            test == "ds-console-failure" || test == "ds-queued-failure" || test == "ds-reset-success", error);
    const bool expectSuccess = test.ends_with("success") || test == "gba-queued" || sameSave;
    bool passed = accepted == expectSuccess && loader.consoleType == 0 && openFiles == 0;
    if (!expectSuccess)
    {
        passed &= manager.get() == oldManager && manager && manager->GetPath() == oldPath &&
            loader.baseROMName == "current.nds" && loader.baseGBAROMName == "current.gba" &&
            loader.nds == oldConsole && loader.nds->GetNDSCart() == oldDS && loader.nds->GetGBACart() == oldGBA &&
            loader.nds->ARM9Read32(0x02001000) == 0xDEADBEEF;
        if (test == "ds-queued-failure") passed &= loader.nextCart.get() == oldQueued && loader.changeCart;
        // Route a subsequent write just as the frontend's save callback does.
        if (manager)
        {
            if (blocked)
            {
                passed &= manager->NeedsFlush();
                if (!QDir(directory.path()).mkdir("missing")) return 2;
                passed &= manager->Flush() && ReadSaveFile(oldPath) == pending;
            }
            const QByteArray continued(8192, '\x3C');
            Queue(*manager, continued);
            manager->FlushSecondaryBuffer();
            passed &= ReadSaveFile(oldPath) == continued;
            passed &= readInput || test.ends_with("existing-writable") ?
                ReadSaveFile(newPath) == previousTarget : !QFile::exists(QString::fromStdString(newPath));
        }
    }
    else
    {
        passed &= manager && manager.get() != oldManager && manager->GetPath() == newPath && ReadSaveFile(oldPath) == pending;
        if (gba)
            passed &= loader.baseGBAROMName == "incoming.gba" &&
                (loader.active ? loader.nds->GetGBACart() != oldGBA : loader.nextGBACart != nullptr);
        else
            passed &= loader.baseROMName == (sameSave ? "current.nds" : "incoming.nds") &&
                loader.nds->GetNDSCart() != oldDS && loader.cheatAsset == (sameSave ? "current" : "incoming");
        if (sameSave)
            passed &= loader.nds->GetNDSSaveLength() == u32(pending.size()) &&
                std::memcmp(loader.nds->GetNDSSave(), pending.data(), pending.size()) == 0;
    }
    std::printf("cart replacement %s: %s (accepted=%d error=%s openFiles=%u)\n", test.c_str(),
                passed ? "PASS" : "FAIL", accepted, error.toUtf8().constData(), openFiles);
    return passed ? 0 : 1;
}
