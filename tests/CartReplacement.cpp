// SPDX-License-Identifier: GPL-3.0-or-later
// Current frontend replacement/console methods, real core/carts/save manager.
// Only resource selection and host files are scripted; no user ROM/BIOS.
#include <cassert>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <stdexcept>
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
#include "ROMPreparation.h"
#include "Platform.h"
using namespace melonDS;
using namespace melonDS::Platform;
using std::string;
using std::unique_ptr;
using std::make_unique;

// Fail only a size-matched array allocation on the RequestFlush caller. Core
// setup, Qt allocations and the save worker run outside the injection scope.
static thread_local size_t failCaptureSize = 0;
static thread_local unsigned captureAllocationFailures = 0;

void* operator new[](size_t size)
{
    if (size && size == failCaptureSize)
    {
        failCaptureSize = 0;
        ++captureAllocationFailures;
        throw std::bad_alloc();
    }
    if (void* data = std::malloc(size ? size : 1)) return data;
    throw std::bad_alloc();
}
void operator delete[](void* data) noexcept { std::free(data); }
void operator delete[](void* data, size_t) noexcept { std::free(data); }

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
    bool forbidROMRead = false;
    bool changeCart = false, changeGBACart = false;
    string baseROMDir, baseROMName = "current.nds", baseAssetName = "current";
    string baseGBAROMDir, baseGBAROMName = "current.gba", baseGBAAssetName = "current";
    AssetIdentity::Selection dsAssetPaths, gbaAssetPaths;
    string incomingDir, cheatAsset, fileSuffix;
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
    string instanceFileSuffix() { return fileSuffix; }
    bool loadROMData(const QStringList& paths, unique_ptr<u8[]>& data, u32& length,
                     string& base, string& name) noexcept
    {
        if (forbidROMRead) std::abort(); // Prepared apply must never re-open ROM input.
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
    void retrySaveCapture();
    bool flushSaveData(QString& errorstr);
    bool loadSaveRAM(string path, string original, bool gba,
                     unique_ptr<u8[]>& data, u32& length, QString& errorstr);
    string getAssetPath(bool gba, const string& configpath, const string& ext, const string& file);
    QString getSavErrorString(string& filepath, bool gba);
    bool loadROM(QStringList filepath, bool reset, QString& errorstr, const AssetIdentity::Selection& assets = {}, const std::shared_ptr<ROMPreparation::Data>& prepared = {});
    bool loadGBAROM(QStringList filepath, QString& errorstr, const AssetIdentity::Selection& assets = {}, const std::shared_ptr<ROMPreparation::Data>& prepared = {}, u32 initialSaveLength = 0);
    bool updateConsole(bool directBoot = false) noexcept;
    bool reset(const AssetIdentity::Selection& dsAssets = {}, const AssetIdentity::Selection& gbaAssets = {});
};

static bool captureCartSave = false;
static unsigned gbaSaveCalls = 0;
namespace melonDS::Platform
{
void WriteNDSSave(const u8* data, u32 length, u32 offset, u32 count, void* userdata)
{
    if (!captureCartSave) return;
    auto* loader = static_cast<CartLoader*>(userdata);
    if (!loader || !loader->ndsSave) std::abort();
    loader->ndsSave->RequestFlush(data, length, offset, count);
}
void WriteGBASave(const u8* data, u32 length, u32 offset, u32 count, void* userdata)
{
    if (!captureCartSave) return;
    ++gbaSaveCalls;
    auto* loader = static_cast<CartLoader*>(userdata);
    if (!loader || !loader->gbaSave) std::abort();
    loader->gbaSave->RequestFlush(data, length, offset, count);
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
#include "cartRetryCapture.inc"
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

static int CaptureRecovery(const string& test)
{
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    CartLoader loader; // No emulation thread: the real core's producer is paused.
    const bool gba = test == "capture-gba";
    const bool generated = test == "capture-generated-firmware";
    const bool firmware = generated || test == "capture-raw-firmware";
    if (!firmware && !gba && test != "capture-ds") return 2;

    const u8* source;
    QByteArray expected;
    if (firmware)
    {
        auto& data = loader.nds->GetFirmware();
        std::memset(data.GetExtendedAccessPointPosition(), 0x42, sizeof(data.GetExtendedAccessPoints()));
        std::memset(data.GetWifiAccessPointPosition(), 0xC3, sizeof(data.GetAccessPoints()));
        // Distinct bytes outside the settings region detect a wrong start/length.
        data.GetExtendedAccessPointPosition()[-1] = 0x17;
        data.GetWifiAccessPointPosition()[sizeof(data.GetAccessPoints())] = 0x29;
        if (generated)
        {
            if (data.GetHeader().Identifier != GENERATED_FIRMWARE_IDENTIFIER) return 2;
            source = data.GetExtendedAccessPointPosition();
            expected = QByteArray(sizeof(data.GetExtendedAccessPoints()), '\x42') +
                QByteArray(sizeof(data.GetAccessPoints()), '\xC3');
        }
        else
        {
            // Synthetic non-generated image; no private physical firmware dump.
            data.GetHeader().Identifier = {'M', 'A', 'C', 'P'};
            data.Buffer()[0x400] = 0x5A;
            data.Buffer()[data.Length() - 1] = 0x6B;
            source = data.Buffer();
            expected = QByteArray(reinterpret_cast<const char*>(source), data.Length());
        }
    }
    else
    {
        const u32 length = gba ? 32768 : loader.nds->GetNDSSaveLength();
        if (length < 512) return 2;
        expected.resize(length);
        for (int i = 0; i < expected.size(); ++i)
            expected[i] = static_cast<char>((i * 17 + (gba ? 91 : 37)) & 0xFF);
        if (gba) loader.nds->SetGBASave(reinterpret_cast<const u8*>(expected.constData()), length);
        else loader.nds->SetNDSSave(reinterpret_cast<const u8*>(expected.constData()), length);
        if ((gba ? loader.nds->GetGBASaveLength() : loader.nds->GetNDSSaveLength()) != length) return 2;
        source = gba ? loader.nds->GetGBASave() : loader.nds->GetNDSSave();
    }
    if (!source || std::memcmp(source, expected.constData(), expected.size())) return 2;

    int failures = 0;
    const auto check = [&](bool value, const char* message) {
        if (!value) { ++failures; std::fprintf(stderr, "%s: %s\n", test.c_str(), message); }
    };
    auto& owner = firmware ? loader.firmwareSave : gba ? loader.gbaSave : loader.ndsSave;
    for (bool existing : {false, true})
    {
        const string path = directory.filePath(existing ? "existing.sav" : "first.sav").toStdString();
        owner = make_unique<SaveManager>(path);
        const QByteArray original = existing ? QByteArray(16, '\xA5') : QByteArray();
        if (existing)
        {
            Queue(*owner, original);
            if (!owner->Flush()) return 2;
        }

        // Model one failed full callback after the core accepted its latest
        // bytes. No guest write, import or reset occurs before either retry.
        const unsigned before = captureAllocationFailures;
        bool escaped = false;
        failCaptureSize = expected.size();
        try { owner->RequestFlush(source, expected.size(), 0, expected.size()); }
        catch (const std::bad_alloc&) { escaped = true; }
        failCaptureSize = 0;
        check(!escaped && captureAllocationFailures == before + 1,
              "RequestFlush did not contain the injected capture allocation failure");
        check(owner->NeedsCapture() && owner->NeedsFlush() && !owner->Flush(),
              "Failed capture was not pending or allowed a stale flush");
        check(ReadSaveFile(path) == original, "Failed capture changed the original file");

        QString error;
        if (existing)
            check(FlushSave(&loader, owner.get(), error) && error.isEmpty(),
                  "Frontend flush did not recapture the current core bytes");
        else
        {
            loader.retrySaveCapture();
            owner->CheckFlush(); // Same producer-side ordering as the frame boundary.
            check(owner->Flush(), "Producer retry could not flush the first captured save");
        }
        check(!owner->NeedsCapture() && !owner->NeedsFlush() && ReadSaveFile(path) == expected,
              "Recovery before the next guest write did not persist every latest byte");
        owner.reset(); // Join this worker before inspecting the next recovery path.
        check(ReadSaveFile(path) == expected, "Teardown replaced the recovered save with stale data");
    }
    std::printf("core save capture recovery %s: %d failures\n", test.c_str(), failures);
    return failures ? 1 : 0;
}

static int InitialGBASave(const string& test)
{
    const auto require = [](bool ok, const char* why) {
        if (!ok) throw std::runtime_error(why);
    };
    const auto bytes = [](GBACart::CartCommon* cart) {
        return QByteArray(reinterpret_cast<const char*>(cart->GetSaveMemory()), cart->GetSaveMemoryLength());
    };
    try
    {
        QTemporaryDir directory;
        require(directory.isValid(), "temporary save directory");
        CartLoader loader;
        loader.incomingDir = directory.path().toStdString();
        loader.localCfg.savePath = loader.incomingDir;
        captureCartSave = true;
        QString error;
        const auto prepared = [&](const QString& name) {
            auto data = std::make_shared<ROMPreparation::Data>();
            data->Source = {name}; data->Name = name.toStdString();
            data->BasePath = loader.incomingDir;
            data->Bytes = ROM(true, data->Length);
            return data;
        };
        if (test == "gba-initial-roundtrip")
        {
            for (u32 length : {512u, 8192u, 32768u, 65536u, 131072u})
            {
                const QString name = QString("manual-%1.gba").arg(length);
                const string save = directory.filePath(QString("manual-%1.sav").arg(length)).toStdString();
                auto data = prepared(name);
                const auto callbacks = gbaSaveCalls;
                loader.forbidROMRead = true;
                require(loader.loadGBAROM(data->Source, error, {}, data, length), "explicit prepared GBA load");
                auto* cart = loader.nds->GetGBACart();
                QByteArray expected(length, '\xFF');
                require(cart && bytes(cart) == expected && !data->Bytes, "initial storage must be erased at selected capacity");
                require(gbaSaveCalls == callbacks && loader.gbaSave->Flush() && !QFile::exists(QString::fromStdString(save)),
                        "initial storage must not notify or create a file");
                if (length <= 8192)
                {
                    // Real cart serial bus: one complete write at the final EEPROM block.
                    const u32 block = length / 8 - 1;
                    cart->ROMWriteBus(0x01000000, 1, 1000, true);
                    cart->ROMWriteBus(0x01000000, 0, 1000, true);
                    for (int bit = (length == 512 ? 6 : 14) - 1; bit >= 0; --bit)
                        cart->ROMWriteBus(0x01000000, (block >> bit) & 1, 1000, true);
                    for (u8 value : {0x52, 0xA6, 0x03, 0xFF, 0x00, 0x81, 0x37, 0xC4})
                        for (int bit = 7; bit >= 0; --bit)
                            cart->ROMWriteBus(0x01000000, (value >> bit) & 1, 1000, true);
                    cart->ROMWriteBus(0x01000000, 0, 1000, true);
                    expected.replace(length - 8, 8, QByteArray::fromHex("52a603ff008137c4"));
                }
                else
                {
                    loader.nds->ARM9Write16(0x04000204, loader.nds->ARM9Read16(0x04000204) & ~0x80);
                    if (length != 32768)
                    {
                        loader.nds->ARM9Write8(0x0A005555, 0xAA);
                        loader.nds->ARM9Write8(0x0A002AAA, 0x55);
                        loader.nds->ARM9Write8(0x0A005555, 0xA0);
                    }
                    loader.nds->ARM9Write8(0x0A001234, 0x52);
                    require(loader.nds->ARM9Read8(0x0A001234) == 0x52, "Slot-2 guest readback");
                    expected[0x1234] = '\x52';
                }
                require(gbaSaveCalls == callbacks + 1 && bytes(cart) == expected &&
                        loader.gbaSave->Flush() && ReadSaveFile(save) == expected,
                        "first guest write must persist full storage through the real SaveManager");
                loader.forbidROMRead = false;
                require(loader.loadGBAROM({name}, error, {}, {}, length == 512 ? 131072 : 512) &&
                        bytes(loader.nds->GetGBACart()) == expected && ReadSaveFile(save) == expected &&
                        gbaSaveCalls == callbacks + 1, "existing save must win over a conflicting new choice without callbacks");
            }
            require(loader.loadGBAROM({"automatic.gba"}, error) && loader.nds->GetGBASaveLength() == 0 &&
                    loader.gbaSave->Flush() && !QFile::exists(directory.filePath("automatic.sav")),
                    "automatic unknown ROM must not inherit the previous explicit length");
            loader.active = false;
            require(loader.loadGBAROM({"queued.gba"}, error, {}, {}, 8192) && loader.changeGBACart &&
                    loader.nextGBACart && bytes(loader.nextGBACart.get()) == QByteArray(8192, '\xFF') &&
                    loader.nds->GetGBASaveLength() == 0 && loader.gbaSave->Flush() &&
                    !QFile::exists(directory.filePath("queued.sav")), "inactive insertion must queue the selected erased storage");
        }
        else
        {
            require(loader.loadGBAROM({"owner.gba"}, error, {}, {}, 32768), "initial owner");
            auto* oldCart = loader.nds->GetGBACart();
            auto* oldManager = loader.gbaSave.get();
            oldCart->SRAMWrite(9, 0x63);
            require(oldManager->Flush(), "old owner persistence");
            const string oldPath = oldManager->GetPath();
            const auto oldBytes = ReadSaveFile(oldPath);
            const auto callbacks = gbaSaveCalls;
            const auto retained = [&] {
                require(loader.nds->GetGBACart() == oldCart && loader.gbaSave.get() == oldManager &&
                        loader.gbaSave->GetPath() == oldPath && loader.baseGBAROMName == "owner.gba" &&
                        bytes(oldCart) == oldBytes && ReadSaveFile(oldPath) == oldBytes && gbaSaveCalls == callbacks,
                        "rejected replacement changed the old cart, manager, identity, callback count or file");
            };
            if (test == "gba-initial-existing")
            {
                for (const auto& suffix : {string{}, string{".2"}})
                {
                    loader.fileSuffix = suffix;
                    for (bool original : {false, true})
                    {
                        const QString name = QString("empty-%1-%2").arg(suffix.empty() ? 0 : 2).arg(original);
                        const QString target = directory.filePath(name + ".sav" + (original ? QString{} : QString::fromStdString(suffix)));
                        QFile file(target);
                        require(file.open(QIODevice::WriteOnly | QIODevice::NewOnly), "new empty existing save"); file.close();
                        require(!loader.loadGBAROM({name + ".gba"}, error, {}, {}, 65536) && !error.isEmpty(),
                                "explicit initialization must reject an empty existing save");
                        retained();
                        require(QFile::exists(target) && ReadSaveFile(target.toStdString()).isEmpty(), "empty file must remain empty and present");
                    }
                }
                loader.fileSuffix.clear();
                const auto suppliedPath = directory.filePath("supplied.sav");
                const QByteArray supplied(37, '\xA6');
                QFile file(suppliedPath);
                require(file.open(QIODevice::WriteOnly) && file.write(supplied) == supplied.size(), "nonstandard existing save"); file.close();
                require(loader.loadGBAROM({"supplied.gba"}, error, {}, {}, 131072) &&
                        bytes(loader.nds->GetGBACart()) == supplied && ReadSaveFile(suppliedPath.toStdString()) == supplied &&
                        gbaSaveCalls == callbacks, "nonempty existing data must not be resized, initialized or rewritten");
            }
            else if (test == "gba-initial-rejects")
            {
                loader.forbidROMRead = true;
                for (u32 length : {1u, 511u, 513u, 8191u, 8193u, 32767u, 65535u, 131073u, UINT32_MAX})
                {
                    // Both raw and prepared paths must reject before reading/consuming input.
                    require(!loader.loadGBAROM({"unread.gba"}, error, {}, {}, length) && !error.isEmpty(), "invalid raw size accepted");
                    auto data = prepared("unread.gba");
                    auto* input = data->Bytes.get();
                    require(!loader.loadGBAROM(data->Source, error, {}, data, length) && data->Bytes.get() == input,
                            "invalid size consumed prepared bytes");
                    retained();
                }
                require(!QFile::exists(directory.filePath("unread.sav")), "invalid size created a file");
            }
            else if (test == "gba-initial-prepared")
            {
                loader.forbidROMRead = true;
                for (bool cancelled : {false, true})
                {
                    auto data = prepared("prepared.gba");
                    std::stop_source stop; data->Stop = stop.get_token();
                    if (cancelled) stop.request_stop();
                    const auto source = cancelled ? data->Source : QStringList{"other.gba"};
                    auto* input = data->Bytes.get();
                    require(!loader.loadGBAROM(source, error, {}, data, 8192) && data->Bytes.get() == input,
                            "stopped or mismatched source consumed prepared bytes");
                    retained();
                }
                auto data = prepared("allocation.gba");
                failCaptureSize = 65536;
                require(!loader.loadGBAROM(data->Source, error, {}, data, 65536) && !error.isEmpty() &&
                        failCaptureSize == 0 && captureAllocationFailures == 1, "manual allocation failure must reject");
                retained();
                require(!QFile::exists(directory.filePath("prepared.sav")) && !QFile::exists(directory.filePath("allocation.sav")),
                        "failed preparation created a save file");
            }
            else return 2;
        }
        require(openFiles == 0, "save handles leaked");
        std::printf("%s: PASS\n", test.c_str());
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "%s: %s\n", test.c_str(), error.what());
        return 1;
    }
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    const string test = argv[1];
    if (test.starts_with("gba-initial-")) return InitialGBASave(test);
    if (test.starts_with("capture-")) return CaptureRecovery(test);
    if (test == "invalid-sd")
    {
        // The headless Platform refuses file opens. Use the real failed
        // FATStorage constructor and both core owners, without user images.
        const FATStorageArgs missing{"generated-missing-sd.img", 8 * 1024 * 1024, false, std::nullopt};
        NDSCart::CartSD cart(make_unique<u8[]>(0x20000), 0x20000, 0, {}, nullptr, FATStorage(missing));
        bool passed = !cart.GetSDCard();
        cart.SetSDCard(FATStorage(missing));
        passed &= !cart.GetSDCard();
        cart.SetSDCard(std::make_optional<FATStorage>(missing));
        passed &= !cart.GetSDCard();
        cart.SetSDCard(std::make_optional(missing));
        passed &= !cart.GetSDCard();

        DSiArgs args;
        args.DSiSDCard.emplace(missing);
        auto console = make_unique<DSi>(std::move(args));
        passed &= !console->GetSDCard();
        console->SetSDCard(FATStorage(missing));
        passed &= !console->GetSDCard();
        console->SetSDCard(std::make_optional<FATStorage>(missing));
        passed &= !console->GetSDCard();
        std::printf("failed SD image stays absent from cart and DSi slots: %s\n", passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }
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
    if (test.find("-prepared-") != string::npos)
    {
        const bool cancelled = test.ends_with("cancel");
        const bool queued = test.find("queued") != string::npos;
        const bool failed = test.ends_with("failure");
        if (queued)
        {
            loader.active = false;
            u32 length;
            auto ds = ROM(false, length);
            loader.nextCart = NDSCart::ParseROM(std::move(ds), length, &loader);
            auto gbaROM = ROM(true, length);
            loader.nextGBACart = GBACart::ParseROM(std::move(gbaROM), length, &loader);
            loader.changeCart = loader.changeGBACart = true;
        }
        const auto* queuedDS = loader.nextCart.get();
        const auto* queuedGBA = loader.nextGBACart.get();
        std::stop_source stop;
        auto prepared = std::make_shared<ROMPreparation::Data>();
        prepared->Source = {"generated-archive.zip", gba ? "folder/member.gba" : "folder/member.nds"};
        prepared->Name = gba ? "member.gba" : "member.nds";
        prepared->BasePath = loader.incomingDir;
        prepared->Bytes = ROM(gba, prepared->Length, failed && !queued);
        prepared->Stop = stop.get_token();
        if (cancelled) stop.request_stop();
        if (failed && queued) loader.resourcesOK = false;
        loader.forbidROMRead = true;
        const AssetIdentity::Selection selected{prepared->Source, "prepared-member", directory.path(), directory.path(), directory.path()};
        const bool accepted = gba ? loader.loadGBAROM(prepared->Source, error, selected, prepared) :
            loader.loadROM(prepared->Source, failed && queued, error, selected, prepared);
        bool passed = accepted == (!cancelled && !failed);
        if (!accepted)
        {
            passed &= manager.get() == oldManager && manager->GetPath() == oldPath &&
                loader.nds == oldConsole && loader.nds->GetNDSCart() == oldDS && loader.nds->GetGBACart() == oldGBA &&
                loader.nextCart.get() == queuedDS && loader.nextGBACart.get() == queuedGBA &&
                loader.changeCart == queued && loader.changeGBACart == queued &&
                loader.baseROMName == "current.nds" && loader.baseGBAROMName == "current.gba" &&
                loader.nds->ARM9Read32(0x02001000) == 0xDEADBEEF;
            const QByteArray continued(8192, '\x3C');
            Queue(*manager, continued);
            passed &= manager->Flush() && ReadSaveFile(oldPath) == continued &&
                !QFile::exists(directory.filePath("prepared-member.sav"));
        }
        else
        {
            const auto& assets = gba ? loader.gbaAssetPaths : loader.dsAssetPaths;
            passed &= manager.get() != oldManager && manager->GetPath() == directory.filePath("prepared-member.sav").toStdString() &&
                assets.Source == selected.Source && assets.Name == selected.Name &&
                ReadSaveFile(oldPath) == pending && !prepared->Bytes;
            passed &= gba ? loader.baseGBAROMName == "member.gba" && loader.nds->GetGBACart() != oldGBA :
                loader.baseROMName == "member.nds" && loader.nds->GetNDSCart() != oldDS;
        }
        std::printf("prepared cart %s: %s (queued DS/GBA, current cart, save callbacks, member identity; no ROM reread)\n",
                    test.c_str(), passed ? "PASS" : "FAIL");
        return passed ? 0 : 1;
    }
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
