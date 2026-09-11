// SPDX-License-Identifier: GPL-3.0-or-later
// Current frontend readers/writer with real Qt temporary files, zstd and RTC.
// Scripted file/allocator failures never touch a user's emulator directory.
#include <cstdlib>
#include <clocale>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include <vector>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QTemporaryDir>
#include <zstd.h>
#include "NDS.h"
#include "Platform.h"
#include "ArchiveUtil.h"
using namespace melonDS;
using namespace melonDS::Platform;
using std::string;
using std::unique_ptr;
using std::make_unique;

static bool failAllocation = false;
void* operator new[](std::size_t size)
{
    if (std::exchange(failAllocation, false)) throw std::bad_alloc();
    if (auto* data = std::malloc(size ? size : 1)) return data;
    throw std::bad_alloc();
}
void operator delete[](void* data) noexcept { std::free(data); }
void operator delete[](void* data, std::size_t) noexcept { std::free(data); }

enum class Failure { None, Short, Error, Oversize, WrappedLength, Write, Commit, Unwritable };
static Failure failure = Failure::None;
static unsigned openFiles = 0;
static unsigned openAttempts = 0;
static unsigned readCalls = 0;
static std::vector<FileHandle*> openHandles;
static string failurePath;
static string localPath;
namespace melonDS::Platform
{
FileHandle* OpenFixtureFile(const string& path, FileMode mode)
{
    ++openAttempts;
    auto file = make_unique<QFile>(QString::fromStdString(path));
    if (!file->open(mode == FileMode::Read ? QIODevice::ReadOnly : QIODevice::WriteOnly)) return nullptr;
    ++openFiles;
    openHandles.push_back(reinterpret_cast<FileHandle*>(file.get()));
    return reinterpret_cast<FileHandle*>(file.release());
}
string FixtureLocalPath(const string& name) { return localPath + "/" + name; }
FileHandle* OpenFixtureLocalFile(const string& name, FileMode mode)
{ return OpenFixtureFile(FixtureLocalPath(name), mode); }
bool CloseFixtureFile(FileHandle* file)
{
    std::erase(openHandles, file);
    delete reinterpret_cast<QFile*>(file);
    --openFiles;
    return true;
}
u64 FixtureFileLength(FileHandle* file)
{
    if (failure == Failure::Oversize) return 0x40000001;
    if (failure == Failure::WrappedLength && (failurePath.empty() ||
        reinterpret_cast<QFile*>(file)->fileName() == QString::fromStdString(FixtureLocalPath(failurePath))))
        return (u64(1) << 32) + reinterpret_cast<QFile*>(file)->size();
    return reinterpret_cast<QFile*>(file)->size();
}
void RewindFixtureFile(FileHandle* file) { reinterpret_cast<QFile*>(file)->seek(0); }
u64 ReadFixtureFile(void* data, u64 size, u64 count, FileHandle* file)
{
    ++readCalls;
    if (!size || !count) return 0;
    if (failure == Failure::Error) return u64(-1);
    auto n = reinterpret_cast<QFile*>(file)->read(static_cast<char*>(data),
        size * count - (failure == Failure::Short ? 1 : 0));
    return n > 0 ? n / size : n;
}
u64 WriteFixtureFile(const void* data, u64 size, u64 count, FileHandle* file)
{
    const auto n = reinterpret_cast<QFile*>(file)->write(static_cast<const char*>(data),
        failure == Failure::Write ? 1 : size * count);
    return n > 0 ? n / size : n;
}
bool CheckFixtureFileWritable(const string& path)
{
    return failure != Failure::Unwritable &&
        QFileInfo(QString::fromStdString(FixtureLocalPath(path))).isWritable();
}
}
class FixtureSaveFile : public QSaveFile
{
public:
    using QSaveFile::QSaveFile;
    qint64 write(const char* data, qint64 length)
    { return failure == Failure::Write ? 1 : QSaveFile::write(data, length); }
    bool commit() { return failure != Failure::Commit && QSaveFile::commit(); }
};
struct FileLoader
{
    NDS* nds;
    struct FixtureConfig
    {
        bool externalDS = true;
        bool externalDSi = true;
        bool GetBool(const string& key) const
        { return key == "Emu.ExternalBIOSEnable" ? externalDS : externalDSi; }
        string GetString(const string& key) const { return key; }
    } globalCfg;
    static int lastSep(const string& path);
    bool loadROMData(const QStringList&, unique_ptr<u8[]>&, u32&, string&, string&) noexcept;
    u32 decompressROM(const u8*, u32, unique_ptr<u8[]>&);
    void loadRTCData();
    void saveRTCData();
    QString verifyDSBIOS();
    QString verifyDSiBIOS();
    QString verifyDSFirmware();
    QString verifyDSiFirmware();
    unique_ptr<ARM9BIOSImage> loadARM9BIOS() noexcept;
    unique_ptr<ARM7BIOSImage> loadARM7BIOS() noexcept;
    unique_ptr<DSiBIOSImage> loadDSiARM9BIOS() noexcept;
    unique_ptr<DSiBIOSImage> loadDSiARM7BIOS() noexcept;
};
#define EmuInstance FileLoader
#define OpenFile OpenFixtureFile
#define OpenLocalFile OpenFixtureLocalFile
#define GetLocalFilePath FixtureLocalPath
#define CloseFile CloseFixtureFile
#define FileLength FixtureFileLength
#define FileRead ReadFixtureFile
#define FileWrite WriteFixtureFile
#define FileRewind RewindFixtureFile
#define QSaveFile FixtureSaveFile
#define CheckFileWritable CheckFixtureFileWritable
#include "fileLastSep.inc"
#include "decompressROM.inc"
#include "fileLoadROMData.inc"
#include "fileLoadRTC.inc"
#include "fileSaveRTC.inc"
#include "fileVerifyDSBIOS.inc"
#include "fileVerifyDSiBIOS.inc"
#include "fileVerifyDSFirmware.inc"
#include "fileVerifyDSiFirmware.inc"
#include "fileLoadARM9BIOS.inc"
#include "fileLoadARM7BIOS.inc"
#include "fileLoadDSiARM9BIOS.inc"
#include "fileLoadDSiARM7BIOS.inc"
#undef CheckFileWritable
#undef QSaveFile
#undef FileRewind
#undef FileWrite
#undef FileRead
#undef FileLength
#undef CloseFile
#undef GetLocalFilePath
#undef OpenLocalFile
#undef OpenFile
#undef EmuInstance

static bool WriteFixtureBytes(const QString& path, const QByteArray& data)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}
static QByteArray ReadFixtureBytes(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
static QByteArray ArchiveFixture(const QByteArray& payload)
{
    QByteArray bytes(4096, '\0');
    size_t used = 0;
    unique_ptr<archive, decltype(&archive_write_free)> writer(archive_write_new(), archive_write_free);
    unique_ptr<archive_entry, decltype(&archive_entry_free)> entry(archive_entry_new(), archive_entry_free);
    if (!writer || !entry || archive_write_set_format_zip(writer.get()) != ARCHIVE_OK ||
        archive_write_set_bytes_per_block(writer.get(), 0) != ARCHIVE_OK ||
        archive_write_open_memory(writer.get(), bytes.data(), bytes.size(), &used) != ARCHIVE_OK)
        return {};
    archive_entry_set_pathname_utf8(entry.get(), "folder/\xEA\xB2\x8C\xEC\x9E\x84.nds");
    archive_entry_set_filetype(entry.get(), AE_IFREG);
    archive_entry_set_perm(entry.get(), 0644);
    archive_entry_set_size(entry.get(), payload.size());
    if (archive_write_header(writer.get(), entry.get()) != ARCHIVE_OK ||
        archive_write_data(writer.get(), payload.constData(), payload.size()) != payload.size() ||
        archive_write_close(writer.get()) != ARCHIVE_OK)
    {
        std::fprintf(stderr, "Archive fixture could not be written: %s\n", archive_error_string(writer.get()));
        return {};
    }
    bytes.resize(used);
    return bytes;
}
int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    std::setlocale(LC_ALL, "");
    if (argc != 2) return 2;
    const string scenario = argv[1];
    QTemporaryDir directory;
    if (!directory.isValid()) return 2;
    localPath = directory.path().toStdString();
    NDSArgs args;
    args.JIT = std::nullopt;
    auto console = make_unique<NDS>(std::move(args));
    NDS& core = *console;
    core.Reset();
    FileLoader loader{&core};
    unsigned failures = 0;
    string context = scenario;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::fprintf(stderr, "%s: %s\n", context.c_str(), message); }
    };
    if (scenario.starts_with("rom-"))
    {
        const QByteArray payload(32, '\x5A');
        QString path = directory.filePath(QString::fromUtf8("게임.nds"));
        QByteArray input = payload;
        const bool archiveInput = scenario.starts_with("rom-archive");
        if (archiveInput)
        {
            path = directory.filePath("games.zip");
            input = ArchiveFixture(payload);
            if (input.isEmpty()) return 2;
        }
        if (scenario == "rom-zstd" || scenario == "rom-zstd-invalid")
        {
            path += ".zst";
            input.resize(ZSTD_compressBound(payload.size()));
            const auto n = ZSTD_compress(input.data(), input.size(), payload.data(), payload.size(), 1);
            if (ZSTD_isError(n)) return 2;
            input.resize(n);
            if (scenario == "rom-zstd-invalid") input.chop(1);
        }
        if (scenario == "rom-empty") input.clear();
        if (scenario != "rom-missing" && !WriteFixtureBytes(path, input)) return 2;
        if (scenario == "rom-short") failure = Failure::Short;
        if (scenario == "rom-error") failure = Failure::Error;
        if (scenario == "rom-oversize") failure = Failure::Oversize;
        if (scenario == "rom-wrapped") failure = Failure::WrappedLength;
        auto output = make_unique<u8[]>(4);
        std::memset(output.get(), 0xA5, 4);
        auto* original = output.get();
        u32 length = 4;
        string base = "previous", name = "previous.nds";
        QStringList paths{path};
        const bool relative = scenario == "rom-relative" || scenario == "rom-archive-relative";
        if (relative)
        {
            QDir::setCurrent(directory.path());
            paths = {QFileInfo(path).fileName()};
        }
        if (archiveInput) paths.append(scenario == "rom-archive-missing" ? "absent.nds" :
                                      QString::fromUtf8("folder/게임.nds"));
        failAllocation = scenario == "rom-allocation";
        const bool ok = loader.loadROMData(paths, output, length, base, name);
        const bool success = scenario == "rom-normal" || relative || scenario == "rom-zstd" || scenario == "rom-archive";
        check(ok == success, "ROM reader returned the wrong result");
        if (success)
            check(length == payload.size() && !std::memcmp(output.get(), payload.data(), length) &&
                  name == QString::fromUtf8("게임.nds").toStdString() &&
                  base == (relative ? "" : localPath), "ROM bytes or asset names changed");
        else
            check(output.get() == original && length == 4 && base == "previous" && name == "previous.nds" &&
                  output[0] == 0xA5, "Failed ROM read changed caller-owned output");
    }
    else if (scenario.starts_with("rtc-"))
    {
        const QString path = directory.filePath("rtc.bin");
        RTC::StateData original{};
        core.RTC.GetState(original);
        const QByteArray bytes(reinterpret_cast<const char*>(&original), sizeof(original));
        if (scenario == "rtc-write" || scenario == "rtc-commit" || scenario == "rtc-roundtrip")
        {
            const QByteArray previous("previous RTC file");
            if (!WriteFixtureBytes(path, previous)) return 2;
            failure = scenario == "rtc-write" ? Failure::Write :
                      scenario == "rtc-commit" ? Failure::Commit : Failure::None;
            loader.saveRTCData();
            check(ReadFixtureBytes(path) == (failure == Failure::None ? bytes : previous), "RTC save damaged the committed file");
            if (failure == Failure::None)
            {
                core.RTC.SetDateTime(2030, 3, 4, 5, 6, 7);
                loader.loadRTCData();
                RTC::StateData after{};
                core.RTC.GetState(after);
                check(!std::memcmp(&original, &after, sizeof(after)), "RTC roundtrip changed state");
            }
        }
        else
        {
            RTC::StateData incoming = original;
            incoming.ClockAdjust ^= 0x55;
            QByteArray input(reinterpret_cast<const char*>(&incoming), sizeof(incoming));
            if (scenario == "rtc-truncated") input.chop(1);
            else if (scenario == "rtc-oversize") input.append('x');
            else if (scenario == "rtc-short") failure = Failure::Short;
            else if (scenario == "rtc-error") failure = Failure::Error;
            else if (scenario != "rtc-missing") return 2;
            if (scenario != "rtc-missing" && !WriteFixtureBytes(path, input)) return 2;
            loader.loadRTCData();
            RTC::StateData after{};
            core.RTC.GetState(after);
            check(!std::memcmp(&original, &after, sizeof(after)), "Failed RTC read partially applied state");
        }
    }
    else if (scenario == "bios-boundaries")
    {
        for (const auto& [name, size] : {std::pair{"DS.BIOS9Path", ARM9BIOSSize},
             {"DS.BIOS7Path", ARM7BIOSSize}, {"DSi.BIOS9Path", DSiBIOSSize}, {"DSi.BIOS7Path", DSiBIOSSize}})
            if (!WriteFixtureBytes(directory.filePath(name), QByteArray(size, '\x5A'))) return 2;

        const auto exercise = [&](const char* name, auto load, auto verify, const auto& builtin, bool& external) {
            const QString path = directory.filePath(name);
            QByteArray payload(builtin.size(), '\0');
            for (qsizetype i = 0; i < payload.size(); ++i) payload[i] = char(i * 37 + 11);
            failurePath = name;
            for (const string mode : {"normal", "short", "error", "truncated", "oversize", "wrapped", "missing"})
            {
                context = string(name) + "/" + mode;
                failure = mode == "short" ? Failure::Short : mode == "error" ? Failure::Error :
                          mode == "wrapped" ? Failure::WrappedLength : Failure::None;
                QByteArray input = payload;
                if (mode == "truncated") input.chop(1);
                if (mode == "oversize") input.append('x');
                if (mode == "missing") { if (!QFile::remove(path)) return false; }
                else if (!WriteFixtureBytes(path, input)) return false;
                const auto readsBefore = readCalls;
                const auto bios = (loader.*load)();
                check(bool(bios) == (mode == "normal"), "BIOS load returned the wrong result");
                if (bios && mode == "normal")
                    check(bios->size() == payload.size() && !std::memcmp(bios->data(), payload.constData(), payload.size()),
                          "Valid BIOS bytes changed");
                const bool sizeValid = mode == "normal" || mode == "short" || mode == "error";
                check(readCalls - readsBefore == (sizeValid ? 1u : 0u), "BIOS read occurred before exact size validation");
                check((loader.*verify)().isEmpty() == sizeValid, "BIOS verifier accepted invalid size or rejected valid size");
                check(openFiles == 0, "BIOS load/verify leaked a handle");
            }
            context = string(name) + "/builtin";
            external = false;
            failure = Failure::Error;
            const auto opensBefore = openAttempts;
            const auto readsBefore = readCalls;
            const auto bios = (loader.*load)();
            check(bios && *bios == builtin, "Built-in BIOS bytes changed");
            check(openAttempts == opensBefore && readCalls == readsBefore, "Built-in BIOS selection attempted file I/O");
            external = true;
            failure = Failure::None;
            failurePath.clear();
            return WriteFixtureBytes(path, payload);
        };
        if (!exercise("DS.BIOS9Path", &FileLoader::loadARM9BIOS, &FileLoader::verifyDSBIOS,
                      FreeBIOSGetNtrArm9(), loader.globalCfg.externalDS) ||
            !exercise("DS.BIOS7Path", &FileLoader::loadARM7BIOS, &FileLoader::verifyDSBIOS,
                      FreeBIOSGetNtrArm7(), loader.globalCfg.externalDS) ||
            !exercise("DSi.BIOS9Path", &FileLoader::loadDSiARM9BIOS, &FileLoader::verifyDSiBIOS,
                      FreeBIOSGetTwlArm9(), loader.globalCfg.externalDSi) ||
            !exercise("DSi.BIOS7Path", &FileLoader::loadDSiARM7BIOS, &FileLoader::verifyDSiBIOS,
                      FreeBIOSGetTwlArm7(), loader.globalCfg.externalDSi)) return 2;
    }
    else if (scenario == "firmware-verify")
    {
        for (const bool dsi : {false, true})
        {
            const char* name = dsi ? "DSi.FirmwarePath" : "DS.FirmwarePath";
            const QString path = directory.filePath(name);
            const auto verify = dsi ? &FileLoader::verifyDSiFirmware : &FileLoader::verifyDSFirmware;
            for (const int size : {0x20000, 0x40000, 0x80000})
            {
                context = string(name) + "/size-" + std::to_string(size);
                if (!WriteFixtureBytes(path, QByteArray(size, '\x5A'))) return 2;
                check((loader.*verify)().isEmpty() == (!dsi || size == 0x20000), "Firmware size policy changed");
                check(openFiles == 0, "Firmware size verification leaked a handle");
            }
            const QByteArray payload(0x20000, '\x5A');
            for (const string mode : {"truncated", "oversize", "wrapped", "unwritable", "missing"})
            {
                context = string(name) + "/" + mode;
                failure = mode == "wrapped" ? Failure::WrappedLength : mode == "unwritable" ? Failure::Unwritable : Failure::None;
                QByteArray input = payload;
                if (mode == "truncated") input.chop(1);
                if (mode == "oversize") input.append('x');
                if (mode == "missing") { if (!QFile::remove(path)) return 2; }
                else if (!WriteFixtureBytes(path, input)) return 2;
                const auto readsBefore = readCalls;
                const QString error = (loader.*verify)();
                check(!error.isEmpty(), "Firmware verification accepted invalid input");
                if (mode == "unwritable") check(error.contains("unable to be written"), "Write refusal reported the wrong error");
                check(readCalls == readsBefore, "Firmware verification read or consumed input");
                check(openFiles == 0, "Firmware refusal leaked a handle");
                if (mode != "missing") check(ReadFixtureBytes(path) == input, "Firmware verifier modified input bytes");
                // Release baseline leaks only after recording the failed assertion.
                while (!openHandles.empty()) CloseFixtureFile(openHandles.back());
            }
            failure = Failure::None;
        }
    }
    else return 2;
    check(openFiles == 0, "Frontend file handle leaked");
    return failures ? 1 : 0;
}
