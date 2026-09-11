// SPDX-License-Identifier: GPL-3.0-or-later
// Generated FAT images, full FATStorage/FatFs and current Qt I/O methods.
// Lifecycle faults use QFile boundaries; export faults also use FatFs I/O seams.
#include <array>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include "FATStorage.h"
#include "StorageExportTest.h"

using namespace melonDS;
namespace {
enum class Failure { None, Read, Seek, LengthAndRead, Write };
Failure failure = Failure::None;
unsigned openImages = 0, imageWrites = 0, indexWrites = 0, injectedFailures = 0;
constexpr u64 Capacity = 8 * 1024 * 1024;

void Require(bool ok, const char* message)
{
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}

class ImageFile final : public QFile
{
public:
    using QFile::QFile;
    ~ImageFile() override { --openImages; }
    qint64 size() const override
    { return failure == Failure::LengthAndRead ? 0 : QFile::size(); }
    bool seek(qint64 offset) override
    {
        if (failure == Failure::Seek)
        { failure = Failure::None; ++injectedFailures; return false; }
        return QFile::seek(offset);
    }
protected:
    qint64 readData(char* data, qint64 size) override
    {
        if (ExportTest::backingRead)
        { ExportTest::backingRead = false; ++ExportTest::faults; return -1; }
        if (failure == Failure::Read || failure == Failure::LengthAndRead)
        { failure = Failure::None; ++injectedFailures; return -1; }
        return QFile::readData(data, size);
    }
    qint64 writeData(const char* data, qint64 size) override
    {
        ++imageWrites;
        if (failure == Failure::Write) { ++injectedFailures; return -1; }
        return QFile::writeData(data, size);
    }
};

QByteArray Bytes(const QString& path)
{
    QFile file(path);
    Require(file.open(QIODevice::ReadOnly), "readback open");
    return file.readAll();
}

void WriteBytes(const QString& path, const QByteArray& data)
{
    QFile file(path);
    Require(file.open(QIODevice::WriteOnly) && file.write(data) == data.size(), "fixture write");
}
}

namespace melonDS::Platform {
FileHandle* OpenFile(const std::string& path, FileMode mode)
{
    const QString name = QString::fromStdString(path);
    const bool image = name.endsWith(".img");
    QFile* file = image ? static_cast<QFile*>(new ImageFile(name)) : new ExportTest::HostFile(name);
    if (image) ++openImages;
    QIODevice::OpenMode access = QIODevice::Unbuffered;
    if (mode & Read) access |= QIODevice::ReadOnly;
    if (mode & Write) access |= QIODevice::WriteOnly;
    if ((mode & Write) && !(mode & Preserve)) access |= QIODevice::Truncate;
    if (mode & Text) access |= QIODevice::Text;
    if (!file->open(access)) { delete file; return nullptr; }
    if (!image && (mode & Write) && name.endsWith(".idx")) ++indexWrites;
    return reinterpret_cast<FileHandle*>(file);
}
FileHandle* OpenLocalFile(const std::string& path, FileMode mode) { return OpenFile(path, mode); }
bool LocalFileExists(const std::string& path) { return QFile::exists(QString::fromStdString(path)); }
bool CloseFile(FileHandle* handle) { delete reinterpret_cast<QFile*>(handle); return true; }
void Log(LogLevel, const char* format, ...)
{
    if (ExportTest::active && std::string(format).find("sync") != std::string::npos) ++ExportTest::errors;
    va_list args; va_start(args, format); std::vfprintf(stderr, format, args); va_end(args);
}
#include "DSiSDFATQtMethods.inc"
std::string GetLocalFilePath(const std::string& path) { return path; }
#define QSaveFile ExportTest::SaveFile
#include "StorageExportAtomic.inc"
#undef QSaveFile
}

// Compile the entire current implementation; only FatFs I/O boundaries change.
#define f_read ExportTest::Read
#define f_close ExportTest::Close
#include "../src/FATStorage.cpp"
#undef f_read
#undef f_close

static void TestExport(const std::string& scenario, const QString& path, const QString& source)
{
    using namespace ExportTest;
    const std::string mode = scenario.substr(7);
    const bool newFile = mode == "new-read" || mode == "new-conflict";
    const bool mixed = mode == "mixed";
    const bool conflict = mode == "host-conflict" || mode == "new-conflict" || mode == "host-collision";
    const QString output = source + (newFile ? "/NEW.BIN" : "/KEEP.BIN");
    const QString index = path + ".idx";
    QByteArray before, indexBefore;
    std::filesystem::file_time_type hostTime;
    const QByteArray next = Payload(mode == "empty" ? 0 : mode == "guest-collision" ? 8193 : 12291);
    targetSize = next.size();
    ReplacementLock lock;
    QStringList entries;
    QStringList rootEntries;
    {
        FATStorage storage(path.toStdString(), Capacity, false, source.toStdString());
        Check(storage.IsValid(), "export seed valid");
        before = newFile ? QByteArray{} : ExportTest::Bytes(output);
        if (!newFile) hostTime = std::filesystem::last_write_time(PathFromUTF8(output.toStdString()));
        indexBefore = ExportTest::Bytes(index);
        entries = Entries(source);
        rootEntries = Entries(QFileInfo(path).absolutePath());
        FF_FILINFO timestamp{};
        const auto guestTimestamp = [&](bool restore)
        {
            ff_disk_open([&](BYTE* data, LBA_t start, UINT count) { return storage.ReadSectors(start, count, data); },
                [&](const BYTE* data, LBA_t start, UINT count) { return storage.WriteSectors(start, count, data); }, storage.GetSectorCount());
            FATFS volume;
            Check(f_mount(&volume, "0:", 1) == FR_OK, "guest timestamp fixture mount");
            Check((restore ? f_utime("0:/KEEP.BIN", &timestamp) : f_stat("0:/KEEP.BIN", &timestamp)) == FR_OK,
                "preserve exact guest FAT timestamp");
            f_unmount("0:"); ff_disk_close();
        };
        if (mode == "guest-collision") guestTimestamp(false);
        Check(storage.InjectFile(newFile ? "NEW.BIN" : "KEEP.BIN",
            reinterpret_cast<u8*>(const_cast<char*>(next.constData())), next.size()), "guest update");
        if (mode == "guest-collision") guestTimestamp(true);
        if (mixed)
        {
            auto other = Payload(513);
            Check(storage.InjectFile("ZGOOD.BIN", reinterpret_cast<u8*>(other.data()), other.size()), "second guest file");
            entries.append("ZGOOD.BIN"); entries.sort();
        }
        if (mode == "replacement") lock.Lock(output);
        Reset((newFile || conflict || mixed) ? Fault::Read : Parse(mode));
    } // Actual production destructor -> Save -> ExportChanges -> ExportFile.
    active = false;
    lock.Unlock();
    const bool failing = fault != Fault::None || mode == "replacement";
    const bool indexFailure = fault == Fault::IndexWrite || fault == Fault::IndexCommit;
    const bool preserved = newFile ? !QFile::exists(output) : ExportTest::Bytes(output) == before;
    std::printf("export=%s preserved=%d index_unchanged=%d reads=%u closes=%u faults=%u commits=%u owner_errors=%u\n",
        mode.c_str(), preserved, ExportTest::Bytes(index) == indexBefore, reads, closes, faults, commits, errors);
    if (failing)
    {
        Check(indexFailure || preserved, "failed export preserves prior host destination");
        if (mixed)
        {
            const auto record = [](const QByteArray& text)
            {
                QByteArray result;
                for (const auto& line : text.split('\n')) if (line.endsWith(" KEEP.BIN")) result += line + '\n';
                return result;
            };
            Check(record(ExportTest::Bytes(index)) == record(indexBefore), "failed entry unchanged while another file succeeds");
            Check(ExportTest::Bytes(source + "/ZGOOD.BIN") == Payload(513) && ExportTest::Bytes(index).contains(" ZGOOD.BIN\n"),
                "other file and its index commit independently");
        }
        else Check(ExportTest::Bytes(index) == indexBefore, "failed export/index commit preserves exact prior index");
        Check(mode == "replacement" || faults == 1, "requested failure reached once");
        Check(errors > 0, "destructor reports sync failure");
        Check(Entries(source) == entries, "failed export leaves no temporary file");
        Check(Entries(QFileInfo(path).absolutePath()) == rootEntries, "failed index write leaves no temporary file");
        if (conflict)
        {
            QByteArray hostEdit("independent host edit after failed export\0binary", 47);
            if (mode == "host-collision")
            {
                hostEdit = before;
                hostEdit[hostEdit.size() / 2] ^= 0x5A;
            }
            Put(output, hostEdit);
            if (mode == "host-collision")
                std::filesystem::last_write_time(PathFromUTF8(output.toStdString()), hostTime);
            const QByteArray imageBefore = ExportTest::Bytes(path);
            {
                FATStorage retry(path.toStdString(), Capacity, false, source.toStdString());
                Check(!retry.IsValid(), "conflicting retry refuses safely");
            }
            Check(ExportTest::Bytes(path) == imageBefore && ExportTest::Bytes(output) == hostEdit &&
                ExportTest::Bytes(index) == indexBefore, "conflict preserves guest image host edit and index");
            // Resolve only this generated conflict by restoring the prior host side.
            if (newFile) Check(QFile::remove(output), "remove generated conflicting host file");
            else
            {
                Put(output, before);
                std::filesystem::last_write_time(PathFromUTF8(output.toStdString()), hostTime);
            }
        }
        Reset();
        {
            FATStorage retry(path.toStdString(), Capacity, false, source.toStdString());
            Check(retry.IsValid(), "unchanged-host retry opens");
        }
        active = false;
    }
    Check(ExportTest::Bytes(output) == next, "normal/retry exact empty binary multiblock export");
    Check(ExportTest::Bytes(index).contains(QByteArray::number(next.size()) + " "), "successful export indexed");
    Check(openImages == 0, "export lifecycle releases backing image");
    std::printf("PASS: %s\n", scenario.c_str());
}

static bool DirectoryAlias(const QString& target, const QString& alias)
{
    std::error_code err;
#ifdef _WIN32
    // MinGW's create_directory_symlink can be unavailable even when Windows
    // permits ordinary, unprivileged symbolic links. No junction emulation.
    if (!CreateSymbolicLinkW(reinterpret_cast<LPCWSTR>(alias.utf16()), reinterpret_cast<LPCWSTR>(target.utf16()),
        SYMBOLIC_LINK_FLAG_DIRECTORY | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE))
        err = std::error_code(GetLastError(), std::system_category());
#else
    std::filesystem::create_directory_symlink(PathFromUTF8(target.toStdString()), PathFromUTF8(alias.toStdString()), err);
#endif
    if (err)
    {
        std::printf("SKIP: native directory symlink unavailable, error=%d\n", err.value());
        return false;
    }
    std::printf("alias_qt_symlink=%d alias_qt_directory=%d alias_std_symlink=%d\n",
        QFileInfo(alias).isSymLink(), QFileInfo(alias).isDir(),
        std::filesystem::is_symlink(std::filesystem::symlink_status(PathFromUTF8(alias.toStdString()))));
    Require(QFileInfo(alias).isSymLink() && QFileInfo(alias).isDir(), "fixture is a real directory symlink");
    return true;
}

static void TestDeletion(const std::string& mode, const QString& path, const QString& source)
{
    using namespace ExportTest;
    const bool directory = mode.starts_with("dir-");
    const bool denied = mode.ends_with("denied");
    const bool readonly = mode.starts_with("readonly-");
    const bool linked = mode == "dir-symlink";
    const bool edit = mode.ends_with("edit");
    const bool type = mode.ends_with("type");
    const bool added = mode == "dir-new";
    const bool indexFailure = mode == "index-commit";
    const bool conflict = edit || type || added || denied || linked;
    const QString target = source + (directory ? "/TREE" : "/KEEP.BIN");
    const QString item = directory && !denied ? target + "/NEST/ITEM.BIN" : target;
    const QString index = path + ".idx";
    const auto original = Payload(8193), control = Payload(257);
    const QString external = QFileInfo(path).dir().filePath("external");
    const QString alias = QFileInfo(path).dir().filePath("replacement-link");
    if (linked)
    {
        Check(QDir().mkdir(external), "generated external symlink target");
        Put(external + "/ITEM.BIN", original);
        if (!DirectoryAlias(external, alias)) std::exit(77);
    }
    Put(source + "/ALIVE.BIN", control);
    if (directory)
    {
        Check(QDir().mkdir(target), "generated deletion directory");
        if (!denied)
        {
            Check(QDir().mkdir(target + "/NEST"), "generated nested deletion directory");
            Put(item, original);
        }
    }
    else Put(item, original);
    const auto itemPath = PathFromUTF8(item.toStdString());
    if (readonly)
        std::filesystem::permissions(itemPath,
            std::filesystem::perms::owner_write | std::filesystem::perms::group_write | std::filesystem::perms::others_write,
            std::filesystem::perm_options::remove);
    const auto originalPerms = std::filesystem::status(itemPath).permissions();
    Check(!readonly || (originalPerms & std::filesystem::perms::owner_write) == std::filesystem::perms::none,
        "host fixture starts read-only");
    QByteArray indexBefore, imageBefore;
    auto expected = original;
    std::filesystem::file_time_type hostTime;
    ReplacementLock lock;
    const auto guestMissing = [&](FATStorage& storage, bool remove)
    {
        ff_disk_open([&](BYTE* data, LBA_t start, UINT count) { return storage.ReadSectors(start, count, data); },
            [&](const BYTE* data, LBA_t start, UINT count) { return storage.WriteSectors(start, count, data); }, storage.GetSectorCount());
        FATFS volume;
        Check(f_mount(&volume, "0:", 1) == FR_OK, "guest deletion mount");
        if (remove && directory && !denied)
        {
            Check(f_unlink("0:/TREE/NEST/ITEM.BIN") == FR_OK, "guest deletes nested file");
            Check(f_unlink("0:/TREE/NEST") == FR_OK, "guest deletes nested directory");
        }
        const char* guest = directory ? "0:/TREE" : "0:/KEEP.BIN";
        if (remove && readonly)
        {
            FF_FILINFO info;
            Check(f_stat(guest, &info) == FR_OK && (info.fattrib & AM_RDO), "host read-only imported as guest AM_RDO");
            Check(f_chmod(guest, 0, AM_RDO) == FR_OK, "guest clears read-only before deletion");
        }
        if (remove) Check(f_unlink(guest) == FR_OK, "real FatFs guest deletion");
        FF_FILINFO info;
        Check(f_stat(guest, &info) == FR_NO_FILE, "guest deletion stays absent");
        Check(f_unmount("0:") == FR_OK, "guest deletion unmount");
        ff_disk_close();
    };
    {
        FATStorage storage(path.toStdString(), Capacity, false, source.toStdString());
        Check(storage.IsValid(), "deletion seed valid");
        indexBefore = ExportTest::Bytes(index);
        if (!denied) hostTime = std::filesystem::last_write_time(PathFromUTF8(item.toStdString()));
        guestMissing(storage, true);
        imageBefore = ExportTest::Bytes(path);
        if (edit)
        {
            expected[41] ^= 0x5A;
            Put(item, expected);
            std::filesystem::last_write_time(PathFromUTF8(item.toStdString()), hostTime);
        }
        if (added) Put(target + "/NEST/NEW.BIN", control);
        if (linked)
        {
            Check(QFile::remove(item) && QDir().rmdir(target + "/NEST"), "remove generated descendant before symlink replacement");
            std::filesystem::rename(PathFromUTF8(alias.toStdString()), PathFromUTF8((target + "/NEST").toStdString()));
        }
        if (type)
        {
            Check(QFile::remove(item), "remove generated file for host kind replacement");
            if (directory)
            {
                Check(QDir().rmdir(target + "/NEST") && QDir().rmdir(target), "remove generated empty directories");
                Put(target, control);
            }
            else Check(QDir().mkdir(target), "replace generated host file with directory");
        }
        if (denied) lock.Lock(target);
        Reset(indexFailure ? Fault::IndexCommit : Fault::None);
    } // Real destructor -> Save -> ExportChanges.
    active = false;
    const auto preserved = [&]
    {
        if (readonly && std::filesystem::status(itemPath).permissions() != originalPerms) return false;
        if (linked) return QFileInfo(target + "/NEST").isSymLink() &&
            ExportTest::Bytes(external + "/ITEM.BIN") == original;
        if (type) return directory ? ExportTest::Bytes(target) == control : QFileInfo(target).isDir();
        if (denied && directory) return QFileInfo(target).isDir();
        return ExportTest::Bytes(item) == expected && (!added || ExportTest::Bytes(target + "/NEST/NEW.BIN") == control);
    };
    std::printf("delete=%s host_exists=%d index_unchanged=%d owner_errors=%u\n", mode.c_str(),
        QFileInfo::exists(target), ExportTest::Bytes(index) == indexBefore, errors);
    if (conflict || indexFailure)
    {
        Check(!conflict || (QFileInfo::exists(target) && preserved()), "failed deletion preserves host content and kind");
        if (readonly) std::printf("read_only_permissions_preserved=%d\n", std::filesystem::status(itemPath).permissions() == originalPerms);
        Check(ExportTest::Bytes(index) == indexBefore, "failed deletion/index commit preserves exact index");
        Check(errors > 0, "destructor reports failed deletion sync");
        Check(ExportTest::Bytes(path) == imageBefore, "failed deletion leaves guest image intact");
        if (conflict)
        {
            FATStorage retry(path.toStdString(), Capacity, false, source.toStdString());
            Check(!retry.IsValid(), "unresolved deletion refuses host import");
            Check(preserved() && ExportTest::Bytes(index) == indexBefore && ExportTest::Bytes(path) == imageBefore,
                "unresolved retry preserves host image and index");
        }
        lock.Unlock();
        if (linked)
        {
            Check(std::filesystem::remove(PathFromUTF8((target + "/NEST").toStdString())) && QDir().mkdir(target + "/NEST"),
                "remove generated symlink without following its target");
            Put(item, original);
            Check(ExportTest::Bytes(external + "/ITEM.BIN") == original, "external symlink target unchanged");
        }
        if (added) Check(QFile::remove(target + "/NEST/NEW.BIN"), "resolve generated new-file conflict");
        if (type)
        {
            if (directory)
                Check(QFile::remove(target) && QDir().mkdir(target) && QDir().mkdir(target + "/NEST"), "restore generated directory kind");
            else Check(QDir().rmdir(target), "remove generated empty replacement directory");
        }
        if (edit || type)
        {
            Put(item, original);
            std::filesystem::last_write_time(PathFromUTF8(item.toStdString()), hostTime);
        }
        Reset();
        {
            FATStorage retry(path.toStdString(), Capacity, false, source.toStdString());
            Check(retry.IsValid(), "resolved deletion retry opens");
            Check(!QFileInfo::exists(target), "retry applies pending deletion before host import");
            guestMissing(retry, false);
        }
        active = false;
    }
    Check(!QFileInfo::exists(target), "unchanged host deletion is applied");
    const auto indexAfter = ExportTest::Bytes(index);
    Check(!indexAfter.contains(directory ? " TREE" : " KEEP.BIN"), "successful deletion removes index records");
    Check(ExportTest::Bytes(source + "/ALIVE.BIN") == control && openImages == 0, "unrelated host bytes and image lifetime preserved");
    std::printf("PASS: delete-%s\n", mode.c_str());
}

static void TestReconciliation(const std::string& mode, const QString& path, const QString& source)
{
    using namespace ExportTest;
    const QString host = source + "/KEEP.BIN", index = path + ".idx";
    QByteArray expected = ExportTest::Bytes(host);
    if (mode == "no-source-index-error")
    {
        Reset(Fault::IndexCommit);
        FATStorage storage(path.toStdString(), Capacity, false);
        active = false;
        Check(storage.IsValid() && faults == 1, "image-only storage does not require host-sync index support");
        Check(storage.InjectFile("KEEP.BIN", reinterpret_cast<u8*>(expected.data()), expected.size()), "image-only guest I/O after sidecar failure");
        Check(!QFile::exists(index), "failed optional sidecar remains absent");
        std::printf("PASS: sync-%s\n", mode.c_str());
        return;
    }
    {
        FATStorage seed(path.toStdString(), Capacity, false,
            mode == "no-index-adoption" ? std::nullopt : std::optional(source.toStdString()));
        Check(seed.IsValid(), "reconciliation seed");
        if (mode == "no-index-adoption")
            Check(seed.InjectFile("OLDONLY.BIN", reinterpret_cast<u8*>(expected.data()), expected.size()), "unindexed adoption fixture");
    }
    if (mode.starts_with("legacy-"))
    {
        QByteArray legacy;
        for (const auto& line : ExportTest::Bytes(index).split('\n'))
            if (!line.startsWith("HASH ") && !line.isEmpty()) legacy += line + '\n';
        Put(index, legacy);
    }
    if (mode == "legacy-pending")
    {
        {
            FATStorage guest(path.toStdString(), Capacity, false);
            auto data = Payload(12291);
            Check(guest.InjectFile("KEEP.BIN", reinterpret_cast<u8*>(data.data()), data.size()), "legacy pending guest edit");
        }
        const auto imageBefore = ExportTest::Bytes(path), indexBefore = ExportTest::Bytes(index);
        {
            FATStorage retry(path.toStdString(), Capacity, false, source.toStdString());
            Check(!retry.IsValid(), "ambiguous legacy pending data refuses");
        }
        Check(ExportTest::Bytes(path) == imageBefore && ExportTest::Bytes(index) == indexBefore && ExportTest::Bytes(host) == expected,
            "legacy refusal preserves exact image host and index");
        FATStorage recovery(path.toStdString(), Capacity, true);
        QByteArray actual(12291, '\0');
        Check(recovery.ReadFile("KEEP.BIN", 0, actual.size(), reinterpret_cast<u8*>(actual.data())) == actual.size() && actual == Payload(12291),
            "refused guest data remains accessible without folder sync");
    }
    else
    {
        if (mode == "no-index-adoption") Check(QFile::remove(index), "remove generated sidecar for explicit no-index adoption");
        if (mode == "host-only")
        {
            const auto time = std::filesystem::last_write_time(PathFromUTF8(host.toStdString()));
            expected[23] ^= 0x6A;
            Put(host, expected);
            std::filesystem::last_write_time(PathFromUTF8(host.toStdString()), time);
        }
        {
            FATStorage storage(path.toStdString(), Capacity, false, source.toStdString());
            Check(storage.IsValid(), "ordinary legacy/no-index/host-only adoption");
            QByteArray actual(expected.size(), '\0');
            Check(storage.ReadFile("KEEP.BIN", 0, actual.size(), reinterpret_cast<u8*>(actual.data())) == actual.size() && actual == expected,
                "ordinary host import stays exact");
            if (mode == "no-index-adoption")
                Check(storage.ReadFile("OLDONLY.BIN", 0, 1, reinterpret_cast<u8*>(actual.data())) == 0, "no-index source adoption retains existing semantics");
        }
        Check(ExportTest::Bytes(host) == expected && ExportTest::Bytes(index).contains("HASH "), "ordinary adoption retains host bytes and upgrades index");
    }
    std::printf("PASS: sync-%s\n", mode.c_str());
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    Require(argc == 2, "one scenario required");
    const std::string scenario = argv[1];
    QTemporaryDir temp;
    Require(temp.isValid(), "temporary directory");
    const QString path = temp.filePath("card.img");
    const QString index = path + ".idx";
    const QString source = temp.filePath("source");
    Require(QDir().mkdir(source), "source directory");
    const QString hostFile = source + "/KEEP.BIN";
    QByteArray payload(8193, '\0');
    for (qsizetype i = 0; i < payload.size(); ++i) payload[i] = char((i * 17 + i / 512 + 9) & 255);
    WriteBytes(hostFile, payload);

    if (scenario.starts_with("delete-"))
    {
        if (scenario == "delete-root-alias")
        {
            const QString alias = temp.filePath("source-alias");
            if (!DirectoryAlias(source, alias)) return 77;
            TestDeletion("root-alias", path, alias);
            Require(std::filesystem::remove(PathFromUTF8(alias.toStdString())), "remove generated root alias only");
        }
        else TestDeletion(scenario.substr(7), path, source);
        return 0;
    }

    if (scenario.starts_with("sync-"))
    {
        TestReconciliation(scenario.substr(5), path, source);
        return 0;
    }

    if (scenario.starts_with("export-"))
    {
        TestExport(scenario, path, source);
        return 0;
    }

    if (scenario == "empty" || scenario == "format-write-error")
    {
        if (scenario == "empty") WriteBytes(path, {}); // Existing empty selection remains supported.
        else
        {
            WriteBytes(index, "existing sidecar must survive a failed format\n");
            failure = Failure::Write;
        }
        {
            FATStorage storage(path.toStdString(), Capacity, false, source.toStdString());
            if (scenario == "empty")
            {
                QByteArray actual(payload.size(), '\0');
                Require(storage.ReadFile("KEEP.BIN", 0, actual.size(), reinterpret_cast<u8*>(actual.data())) == actual.size()
                    && actual == payload, "empty image format and import");
            }
            else
            {
                std::printf("capacity_after_failed_format=%llu index_writes=%u\n",
                    static_cast<unsigned long long>(storage.GetSectorCount()), indexWrites);
                Require(!storage.IsValid() && storage.GetSectorCount() == 0 && openImages == 0, "failed format left an active image");
                Require(indexWrites == 0 && Bytes(index) == "existing sidecar must survive a failed format\n", "failed format replaced index");
            }
        }
        failure = Failure::None;
    }
    else
    {
        // Production constructor formats and imports a multi-cluster file.
        {
            FATStorage storage(path.toStdString(), Capacity, false, source.toStdString());
            Require(storage.GetSectorCount() == Capacity / 512, "new image capacity");
            QByteArray actual(payload.size(), '\0');
            Require(storage.ReadFile("KEEP.BIN", 0, actual.size(), reinterpret_cast<u8*>(actual.data())) == actual.size()
                && actual == payload, "new image format and import");
        }
        Require(openImages == 0, "seed handle closed");
        if (scenario == "normal")
        {
            FATStorage original(path.toStdString(), 0, false, source.toStdString());
            FATStorage moved(std::move(original));
            Require(moved.IsValid() && !original.IsValid() && original.GetSectorCount() == 0
                && moved.GetSectorCount() == Capacity / 512, "sparse indexed image capacity after move");
            QByteArray actual(payload.size(), '\0');
            Require(moved.ReadFile("KEEP.BIN", 0, actual.size(), reinterpret_cast<u8*>(actual.data())) == actual.size()
                && actual == payload, "existing image payload after move");
        }
        else
        {
            if (scenario == "mount-read-error") failure = Failure::Read;
            else if (scenario == "mount-seek-error") failure = Failure::Seek;
            else if (scenario == "length-error")
            {
                // No index capacity fallback: a host size error must not make
                // an existing nonempty image eligible for formatting.
                WriteBytes(index, "unrecognized index, retain verbatim\n");
                failure = Failure::LengthAndRead;
            }
            else if (scenario == "malformed") WriteBytes(path, QByteArray(4096, char(0xA5)));
            else Require(false, "unknown scenario");

            const QByteArray before = Bytes(path), indexBefore = Bytes(index);
            imageWrites = indexWrites = 0;
            {
                FATStorage storage(path.toStdString(), 0, false, source.toStdString());
                std::printf("capacity_after_failed_mount=%llu image_writes=%u index_writes=%u injected=%u\n",
                    static_cast<unsigned long long>(storage.GetSectorCount()), imageWrites, indexWrites, injectedFailures);
                Require(imageWrites == 0 && indexWrites == 0, "mount failure triggered formatting or index replacement");
                Require(!storage.IsValid() && storage.GetSectorCount() == 0 && openImages == 0, "failed mount left an active image");
                std::array<u8, 512> sector{};
                Require(storage.ReadSectors(0, 1, sector.data()) == 0 && storage.WriteSectors(0, 1, sector.data()) == 0,
                    "failed image accepted guest I/O");
                if (scenario != "malformed") Require(injectedFailures == 1, "host failure was not exercised");
            }
            Require(imageWrites == 0 && indexWrites == 0 && Bytes(path) == before && Bytes(index) == indexBefore
                && Bytes(hostFile) == payload, "failed mount/destructor changed persistent data");
            failure = Failure::None;
            if (scenario != "malformed")
            {
                // Retry the same preserved image after the one-shot error.
                FATStorage retry(path.toStdString(), Capacity, true);
                QByteArray actual(payload.size(), '\0');
                Require(retry.ReadFile("KEEP.BIN", 0, actual.size(), reinterpret_cast<u8*>(actual.data())) == actual.size()
                    && actual == payload, "same-image retry lost guest data");
            }
        }
    }
    Require(openImages == 0 && Bytes(hostFile) == payload, "lifetime or host file preservation");
    std::printf("PASS: %s\n", scenario.c_str());
}
