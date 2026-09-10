// SPDX-License-Identifier: GPL-3.0-or-later
// Like DSiBootMetadata: actual NAND crypto + FatFs on generated memory storage.
// This includes the complete production NAND implementation; only I/O boundaries
// are intercepted. No private inputs, host files, network or emulator boot.
#include "DSi.h"
#include "DSi_AES.h"
#include "DSi_NAND.h"
#include "sha1/sha1.hpp"
#include "FATIO.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <string>
#include <vector>

using namespace melonDS;
using namespace melonDS::DSi_NAND;
static void Require(bool ok, const char* message)
{ if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }

namespace melonDS {
#include "ROL16.inc"
#include "DeriveNormalKey.inc"
}
// Faults are bounded to named title I/O operations. Normal calls always reach
// real FatFs and the production NAND encryption/sector methods.
static std::string FaultOperation, FaultPath;
static unsigned Faults = 0, RenameCalls = 0, SourceReadCalls = 0, SourceOpens = 0;
static unsigned FailSourceRead = 0;
static bool ShortSource = false, SourceError = false, FailSourceSeek = false;
static bool ChangeSourceHeader = false;
static bool FailMediumWrite = false, FailMediumRead = false, FailMediumSeek = false;
static bool PersistentMedium = false, MediumUnsignedError = false;
static unsigned FailFlush = 0, FlushCalls = 0;
static std::vector<unsigned> FailRenames;
static bool RenameAfterEffect = false;
static unsigned RenameDiskErrorAt = 0;
static std::vector<u8> SourceBytes;
static std::map<FF_FIL*, std::string> OpenPaths;
static bool Trigger(const char* operation, const std::string& path)
{
    if (FaultOperation != operation || path.find(FaultPath) == std::string::npos) return false;
    FaultOperation.clear(); ++Faults;
    return true;
}
namespace melonDS::Platform {
struct FileHandle { std::vector<u8> bytes; u64 position = 0; bool source = false; };
FileHandle* OpenLocalFile(const std::string& path, FileMode mode)
{
    Require(path == "generated.app" && mode == FileMode::Read, "synthetic source only");
    ++SourceOpens;
    return new FileHandle{SourceBytes, 0, true};
}
bool CloseFile(FileHandle* f) { delete f; return true; }
u64 FileLength(FileHandle* f) { return f->bytes.size(); }
bool FileSeek(FileHandle* f, s64 offset, FileSeekOrigin origin)
{
    if ((f->source && FailSourceSeek) || (!f->source && FailMediumSeek))
    { FailSourceSeek = false; FailMediumSeek = false; ++Faults; return false; }
    s64 base = origin == FileSeekOrigin::Start ? 0 :
        static_cast<s64>(origin == FileSeekOrigin::End ? f->bytes.size() : f->position);
    if (offset < -base || offset > static_cast<s64>(f->bytes.size()) - base) return false;
    f->position = base + offset;
    return true;
}
u64 FileRead(void* dst, u64 size, u64 count, FileHandle* f)
{
    if (!size) return 0;
    if (f->source && ++SourceReadCalls == FailSourceRead)
    {
        ++Faults;
        if (SourceError) return UINT64_MAX;
        if (!ShortSource) return 0;
        count /= 2;
    }
    if (!f->source && FailMediumRead)
    { ++Faults; FailMediumRead = PersistentMedium; return MediumUnsignedError ? UINT64_MAX : 0; }
    count = std::min(count, (f->bytes.size() - f->position) / size);
    std::memcpy(dst, f->bytes.data() + f->position, count * size);
    if (f->source && SourceReadCalls == 1 && ChangeSourceHeader)
        static_cast<NDSHeader*>(dst)->ROMVersion ^= 1;
    f->position += count * size;
    return count;
}
u64 FileWrite(const void* src, u64 size, u64 count, FileHandle* f)
{
    if (!size) return 0;
    if (FailMediumWrite)
    { ++Faults; FailMediumWrite = PersistentMedium; return MediumUnsignedError ? UINT64_MAX : 0; }
    count = std::min(count, (f->bytes.size() - f->position) / size);
    std::memcpy(f->bytes.data() + f->position, src, count * size);
    f->position += count * size;
    return count;
}
bool FileFlush(FileHandle*)
{
    if (++FlushCalls == FailFlush) { ++Faults; return false; }
    return true;
}
void Log(LogLevel, const char*, ...) {}
}
static FRESULT TitleOpen(FF_FIL* file, const char* path, BYTE mode)
{
    if (Trigger("open", path)) return FR_DENIED;
    FRESULT result = f_open(file, path, mode);
    if (result == FR_OK) OpenPaths[file] = path;
    return result;
}
static FRESULT TitleWrite(FF_FIL* file, const void* data, UINT len, UINT* written)
{
    const auto& path = OpenPaths[file];
    if (Trigger("short-write", path)) return f_write(file, data, len / 2, written);
    if (Trigger("write", path)) { *written = 0; return FR_DISK_ERR; }
    if (Trigger("medium-write", path)) FailMediumWrite = true;
    if (Trigger("medium-seek", path)) FailMediumSeek = true;
    return f_write(file, data, len, written);
}
static FRESULT TitleRead(FF_FIL* file, void* data, UINT len, UINT* got)
{
    const auto& path = OpenPaths[file];
    if (Trigger("read", path)) { *got = 0; return FR_DISK_ERR; }
    if (Trigger("medium-read", path)) FailMediumRead = true;
    return f_read(file, data, len, got);
}
static FRESULT TitleClose(FF_FIL* file)
{
    bool fail = Trigger("close", OpenPaths[file]);
    // The close/sync failure is reported after the real close, so no fixture
    // handle outlives its scope. Sector errors are tested separately below.
    FRESULT result = f_close(file);
    OpenPaths.erase(file);
    return fail ? FR_DISK_ERR : result;
}
static FRESULT TitleRename(const char* from, const char* to)
{
    ++RenameCalls;
    if (std::find(FailRenames.begin(), FailRenames.end(), RenameCalls) != FailRenames.end())
    {
        ++Faults;
        if (RenameAfterEffect) Require(f_rename(from, to) == FR_OK, "post-rename injection");
        return RenameCalls == RenameDiskErrorAt ? FR_DISK_ERR : FR_DENIED;
    }
    return f_rename(from, to);
}
static FRESULT TitleUnlink(const char* path)
{
    if (Trigger("unlink", path)) return FR_DENIED;
    return f_unlink(path);
}
static FRESULT TitleChmod(const char* path, BYTE attr, BYTE mask)
{
    if (Trigger("chmod", path)) return FR_DENIED;
    return f_chmod(path, attr, mask);
}
static void TitleDiskOpen(const ff_disk_read_cb& read, const ff_disk_write_cb& write, LBA_t count)
{
    // Formatting only: bound this generated partition to the bytes after the
    // production NAND partition offset (and before the synthetic footer).
    // Existing-image mounts do not use this count for their BPB geometry.
    ff_disk_open(read, write, count - (0x10EE00 / 512) - 1);
}
#define f_open TitleOpen
#define f_write TitleWrite
#define f_read TitleRead
#define f_close TitleClose
#define f_rename TitleRename
#define f_unlink TitleUnlink
#define f_chmod TitleChmod
#define ff_disk_open TitleDiskOpen
#include "../src/DSi_NAND.cpp"
#undef ff_disk_open
#undef f_open
#undef f_write
#undef f_read
#undef f_close
#undef f_rename
#undef f_unlink
#undef f_chmod

static constexpr const char* Title = "0:/title/00030004/12345678";
static constexpr const char* Ticket = "0:/ticket/00030004/12345678.tik";
static std::vector<u8> ReadBack(const std::string& path)
{
    FF_FIL f;
    Require(f_open(&f, path.c_str(), FA_READ) == FR_OK, path.c_str());
    std::vector<u8> bytes(f_size(&f)); UINT n = 0;
    Require(f_read(&f, bytes.data(), bytes.size(), &n) == FR_OK && n == bytes.size(), "readback");
    Require(f_close(&f) == FR_OK, "readback close");
    return bytes;
}
static std::vector<u8> App(u8 version)
{
    std::vector<u8> bytes(0x8000, version);
    NDSHeader header{};
    header.UnitCode = 2;
    header.DSiTitleIDHigh = 0x00030004; header.DSiTitleIDLow = 0x12345678;
    header.ROMVersion = version;
    header.DSiTotalROMSize = bytes.size();
    header.DSiPublicSavSize = 0x4000; header.DSiPrivateSavSize = 0x4000;
    header.AppFlags = 4;
    std::memcpy(bytes.data(), &header, sizeof(header));
    return bytes;
}
static DSi_TMD::TitleMetadata Metadata(const std::vector<u8>& app, u8 version)
{
    DSi_TMD::TitleMetadata tmd{};
    const u8 id[] = {0,3,0,4,0x12,0x34,0x56,0x78};
    std::memcpy(tmd.TitleId, id, sizeof(id));
    tmd.NumberOfContents = 0x100; tmd.Contents.ContentType[1] = 1;
    tmd.Contents.ContentId[3] = version;
    u64 len = app.size();
    for (int i = 7; i >= 0; --i) { tmd.Contents.ContentSize[i] = len; len >>= 8; }
    SHA1_CTX sha; SHA1Init(&sha); SHA1Update(&sha, app.data(), app.size());
    SHA1Final(tmd.Contents.ContentSha1Hash, &sha);
    return tmd;
}

using Snapshot = std::map<std::string, std::pair<std::vector<u8>, BYTE>>;
static void Capture(const std::string& path, Snapshot& result)
{
    FF_FILINFO info;
    FRESULT found = f_stat(path.c_str(), &info);
    if (found == FR_NO_FILE || found == FR_NO_PATH) return;
    Require(found == FR_OK, "snapshot stat");
    if (!(info.fattrib & AM_DIR)) { result[path] = {ReadBack(path), info.fattrib}; return; }
    FF_DIR dir; Require(f_opendir(&dir, path.c_str()) == FR_OK, "snapshot dir");
    while (true)
    {
        Require(f_readdir(&dir, &info) == FR_OK, "snapshot entries");
        if (!info.fname[0]) break;
        Capture(path + "/" + info.fname, result);
    }
    Require(f_closedir(&dir) == FR_OK, "snapshot close");
}
static Snapshot Installed()
{
    Snapshot result; Capture(Title, result); Capture(Ticket, result); return result;
}
static void ClearFaults()
{
    FaultOperation.clear(); FaultPath.clear(); Faults = RenameCalls = SourceReadCalls = SourceOpens = 0;
    FailSourceRead = 0; ShortSource = SourceError = FailSourceSeek = ChangeSourceHeader = false;
    FailMediumWrite = FailMediumRead = FailMediumSeek = PersistentMedium = MediumUnsignedError = false;
    FailRenames.clear(); RenameAfterEffect = false; RenameDiskErrorAt = 0; FailFlush = FlushCalls = 0;
}
static void Seed(NANDMount& mount)
{
    auto old = App(1); auto tmd = Metadata(old, 1);
    Require(mount.ImportTitle(old.data(), old.size(), tmd, true), "normal new install");
    const std::array<const char*, 4> names = {"public.sav", "private.sav", "banner.sav", "unknown.dat"};
    for (unsigned i = 0; i < names.size(); ++i)
    {
        const std::string path = std::string(Title) + "/data/" + names[i];
        const std::vector<u8> save(i == 3 ? 53 : 0x4000, 0xA5 + i);
        Require(mount.ImportFile(path.c_str(), save.data(), save.size()), "seed custom save");
        Require(f_chmod(path.c_str(), AM_RDO | AM_HID, AM_RDO | AM_HID) == FR_OK, "save attributes");
    }
    Require(f_mkdir((std::string(Title) + "/data/nested").c_str()) == FR_OK, "unknown data folder");
    const u8 unknown[] = {7,8,9};
    Require(mount.ImportFile((std::string(Title) + "/data/nested/extra").c_str(), unknown, sizeof(unknown)), "unknown nested save");
    ClearFaults();
}
static bool Replace(NANDMount& mount, bool file = false)
{
    auto next = App(2); auto tmd = Metadata(next, 2);
    if (!file) return mount.ImportTitle(next.data(), next.size(), tmd, true);
    SourceBytes = next;
    return mount.ImportTitle("generated.app", tmd, true);
}
static void ExpectReplacement(const Snapshot& before)
{
    const auto next = App(2); const auto tmd = Metadata(next, 2);
    Require(ReadBack(std::string(Title) + "/content/00000002.app") == next, "new content exact");
    Require(ReadBack(std::string(Title) + "/content/title.tmd") == std::vector<u8>(
        reinterpret_cast<const u8*>(&tmd), reinterpret_cast<const u8*>(&tmd) + sizeof(tmd)), "new TMD exact");
    Require(f_stat((std::string(Title) + "/content/00000001.app").c_str(), nullptr) == FR_NO_FILE, "old content gone after commit");
    auto after = Installed();
    for (const auto& [path, data] : before)
        if (path.find("/data/") != std::string::npos) Require(after.at(path) == data, "all saves/unknown data/attributes preserved");
    Require(after.at(Ticket).first != before.at(Ticket).first, "new ticket installed");
}
static void ExpectOld(const Snapshot& before) { Require(Installed() == before, "prior app/ticket/TMD/all saves and attributes exact"); }
static constexpr const char* Stage = "0:/_install/00030004-12345678";

int main(int argc, char** argv)
{
    Require(argc == 2, "one case"); const std::string mode = argv[1];
    auto* backing = new Platform::FileHandle{std::vector<u8>(8 * 1024 * 1024)};
    std::memcpy(backing->bytes.data() + backing->bytes.size() - 0x40, "DSi eMMC CID/CPU", 16);
    NANDImage image(backing, DSiKey{});
    {
        NANDMount mount(image);
        FF_MKFS_PARM options{}; options.fmt = FM_FAT | FM_SFD;
        std::array<u8, 4096> work{};
        Require(f_mkfs("0:", &options, work.data(), work.size()) == FR_OK, "format");
    }
    Snapshot before;
    {
        NANDMount mount(image);
        if (mode == "new-rollback-disk")
        {
            FailRenames = {5,6}; RenameDiskErrorAt = 6;
            Require(!Replace(mount), "new import and rollback disk error rejected");
            Require(mount.GetTitleImportResult() == TitleImportResult::RollbackFailed, "uncertain rollback reported");
            Require(RenameCalls == 6, "uncertain rollback stops further namespace writes");
        }
        else if (mode == "new-failures")
        {
            for (const char* path : {"public.sav", "banner.sav"})
                for (const char* operation : {"short-write", "close"})
                {
                    ClearFaults(); FaultOperation = operation; FaultPath = path;
                    Require(!Replace(mount) && Faults == 1 && RenameCalls == 0, "new save staging failure");
                    Require(Installed().empty(), "save failure does not publish fresh title");
                }
            // Each fixed object's publishing boundary on a fresh installation.
            for (unsigned boundary = 1; boundary <= 5; ++boundary)
            {
                ClearFaults(); FailRenames = {boundary};
                Require(!Replace(mount), "fresh install interrupted");
                Require(Faults == 1 && mount.GetTitleImportResult() == TitleImportResult::Failed, "fresh rollback result");
                Require(Installed().empty() && !mount.TitleExists(0x00030004, 0x12345678), "no half-installed fresh title");
                Require(f_stat(Title, nullptr) == FR_NO_PATH || f_stat(Title, nullptr) == FR_NO_FILE, "fresh title directory removed");
            }
            ClearFaults(); Require(Replace(mount), "new install retry");
        }
        else
        {
            Seed(mount); before = Installed();
            if (mode == "normal")
            {
                Require(Replace(mount, true) && SourceOpens == 1, "same-ID file replacement, one handle");
                Require(mount.GetTitleImportResult() == TitleImportResult::Success, "clean success");
                ExpectReplacement(before);
            }
            else if (mode == "invalid")
            {
                for (unsigned condition = 0; condition < 7; ++condition)
                {
                    auto next = App(2); auto tmd = Metadata(next, 2);
                    if (condition == 0) next.resize(sizeof(NDSHeader)-1);
                    if (condition == 1) { NDSHeader h; std::memcpy(&h,next.data(),sizeof(h)); h.DSiPublicSavSize = 1; std::memcpy(next.data(),&h,sizeof(h)); }
                    if (condition == 2) tmd.TitleId[7] ^= 1;
                    if (condition == 3) tmd.Contents.ContentSize[7] ^= 1;
                    if (condition == 4) next.back() ^= 1;
                    if (condition == 5) tmd.NumberOfContents = 0;
                    if (condition == 6) tmd.BootContentIndex = 0x100;
                    Require(!mount.ImportTitle(next.data(), next.size(), tmd, false), "invalid input rejected");
                    Require(mount.GetTitleImportResult() == TitleImportResult::InvalidInput && RenameCalls == 0, "invalid before swap");
                    ExpectOld(before);
                }
                Require(!mount.ImportTitle(nullptr, 0, Metadata(App(2),2), false), "null memory rejected");
                Require(Replace(mount), "retry after invalid input");
            }
            else if (mode == "source")
            {
                for (unsigned condition = 0; condition < 6; ++condition)
                {
                    ClearFaults();
                    FailSourceRead = condition == 0 ? 1 : 3;
                    ShortSource = condition == 1; SourceError = condition == 2;
                    FailSourceSeek = condition == 3;
                    if (condition == 5) { FailSourceRead = 0; ChangeSourceHeader = true; }
                    SourceBytes = App(2); const auto tmd = Metadata(SourceBytes, 2);
                    if (condition == 4) SourceBytes.resize(0x5000);
                    Require(!mount.ImportTitle("generated.app", tmd, false), "short/error source rejected");
                    Require(RenameCalls == 0 && SourceOpens == 1, "no destructive source retry/reopen");
                    if (condition < 4) Require(Faults == 1, "source fault reached");
                    ExpectOld(before);
                }
                ClearFaults(); Require(Replace(mount, true), "source retry");
            }
            else if (mode == "short-write" || mode == "staging")
            {
                const std::vector<std::pair<std::string,std::string>> cases = mode == "short-write" ?
                    std::vector<std::pair<std::string,std::string>>{{"short-write", ".app"}, {"short-write", "/ticket"}, {"short-write", "title.tmd"}} :
                    std::vector<std::pair<std::string,std::string>>{{"open", ".app"}, {"write", ".app"}, {"close", ".app"}, {"read", ".app"},
                        {"write", "/ticket"}, {"close", "/ticket"}, {"write", "title.tmd"}, {"close", "title.tmd"}, {"chmod", "title.tmd"},
                        {"medium-write", ".app"}, {"medium-seek", ".app"}, {"medium-read", ".app"}};
                for (const auto& [operation,path] : cases)
                {
                    ClearFaults(); FaultOperation = operation; FaultPath = path; MediumUnsignedError = true;
                    Require(!Replace(mount), "staging I/O failure rejected");
                    Require(Faults >= 1 && RenameCalls == 0, "fault reached before swap");
                    Require(mount.GetTitleImportResult() == TitleImportResult::Failed, "staging safely discarded");
                    ExpectOld(before);
                }
                ClearFaults(); Require(Replace(mount), "staging retry");
            }
            else if (mode == "swap")
            {
                for (bool after : {false,true}) for (unsigned boundary = 1; boundary <= 4; ++boundary)
                {
                    ClearFaults(); FailRenames = {boundary}; RenameAfterEffect = after;
                    Require(!Replace(mount), "swap failure rejected");
                    Require(Faults == 1 && mount.GetTitleImportResult() == TitleImportResult::Failed, "swap rollback succeeded");
                    ExpectOld(before);
                    Require(f_stat(Stage,nullptr) == FR_NO_FILE, "rollback stage cleaned");
                }
                for (unsigned boundary : {1U,2U})
                {
                    ClearFaults(); FailFlush = boundary;
                    Require(!Replace(mount) && Faults == 1, "staging/commit flush failure");
                    Require(mount.GetTitleImportResult() == TitleImportResult::Failed, "flush rollback");
                    ExpectOld(before);
                }
                ClearFaults(); Require(Replace(mount), "swap retry");
            }
            else if (mode.starts_with("rollback") || mode == "uncertain")
            {
                unsigned rollbackBoundary = mode.starts_with("rollback") ? std::stoul(mode.substr(9)) : 0;
                FailRenames = rollbackBoundary ? std::vector<unsigned>{4,rollbackBoundary} : std::vector<unsigned>{4};
                RenameDiskErrorAt = mode == "uncertain" ? 4 : 0;
                Require(!Replace(mount), "unrecoverable swap/rollback rejected");
                Require(mount.GetTitleImportResult() == (rollbackBoundary ? TitleImportResult::RollbackFailed : TitleImportResult::RecoveryRequired), "recovery outcome");
                const std::string oldContent = rollbackBoundary > 5 ? std::string(Title)+"/content" : std::string(Stage)+"/old-content";
                Require(ReadBack(oldContent+"/00000001.app") == App(1), "recoverable old app backup retained or restored");
                Require(ReadBack(oldContent+"/title.tmd") == before.at(std::string(Title)+"/content/title.tmd").first, "old TMD backup retained or restored");
                const std::string oldTicket = rollbackBoundary == 5 ? Ticket : std::string(Stage)+"/old-ticket";
                Require(ReadBack(oldTicket) == before.at(Ticket).first, "old ticket backup retained or restored");
                for (const auto& [path,data] : before) if (path.find("/data/") != std::string::npos) Require(ReadBack(path) == data.first,"saves survive recovery failure");
                Require(mount.GetTitleImportRecoveryPath() == Stage, "recovery location reported");
                Snapshot recovery; Capture(Stage,recovery); auto visible = Installed();
                ClearFaults(); Require(!Replace(mount) && mount.GetTitleImportResult() == TitleImportResult::RecoveryRequired, "unsafe retry blocked");
                Snapshot after; Capture(Stage,after); Require(after == recovery && Installed() == visible && RenameCalls == 0,"retry kept all recoverable objects");
            }
            else if (mode == "cleanup")
            {
                FaultOperation = "unlink"; FaultPath = "old-content";
                Require(Replace(mount), "all content installed despite cleanup failure");
                Require(Faults == 1 && mount.GetTitleImportResult() == TitleImportResult::InstalledCleanupPending, "cleanup warning distinct from clean success");
                ExpectReplacement(before);
                Require(ReadBack(std::string(Stage)+"/old-content/00000001.app") == App(1), "remaining backup not discarded");
            }
            else if (mode == "space")
            {
                // Real FR_OK + short f_write when FAT runs out of free clusters.
                FF_FIL filler; Require(f_open(&filler,"0:/filler",FA_CREATE_NEW|FA_WRITE)==FR_OK,"filler");
                std::array<u8,4096> zero{}; UINT written = 0; FRESULT result;
                do { result = f_write(&filler,zero.data(),zero.size(),&written); } while (result == FR_OK && written == zero.size());
                Require(result == FR_OK && written < zero.size(),"actual FAT full, not synthetic I/O error");
                Require(f_close(&filler)==FR_OK,"full filler close");
                Require(!Replace(mount),"space failure rejected"); ExpectOld(before);
                Require(f_unlink("0:/filler")==FR_OK,"release disposable filler");
                Require(Replace(mount),"retry after space freed");
            }
            else if (mode == "persistent")
            {
                FaultOperation = "medium-write"; FaultPath = ".app"; PersistentMedium = true; MediumUnsignedError = true;
                Require(!Replace(mount),"persistent medium failure never success");
                Require(mount.GetTitleImportResult() == TitleImportResult::CleanupPending,"persistent cleanup failure reported");
                ClearFaults(); ExpectOld(before);
            }
            else Require(false,"known case");
        }
    }
    // Remount before the final observation: do not mistake an in-memory FatFs
    // window for a persisted directory/content outcome.
    {
        NANDMount remount(image);
        if (mode.starts_with("rollback") || mode == "uncertain" || mode == "persistent" || mode == "new-rollback-disk")
        {
            Require(f_stat(Stage,nullptr) == FR_OK,"recovery directory survived remount");
            if (mode == "persistent") ExpectOld(before);
        }
        else if (mode == "new-failures") Require(remount.TitleExists(0x00030004,0x12345678),"fresh retry persisted");
        else ExpectReplacement(before);
    }
    std::printf("PASS: %s (real encrypted memory NAND/FatFs; no physical acceptance claim)\n",mode.c_str());
}
