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

#include <string.h>
#include <dirent.h>
#include <inttypes.h>
#include <vector>
#include <algorithm>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "FATIO.h"
#include "FATStorage.h"
#include "Platform.h"
#include "UTF8.h"
#include "sha1/sha1.hpp"

namespace melonDS
{
namespace fs = std::filesystem;
using namespace Platform;
using std::string;

// Content evidence for retry reconciliation, using the core's existing SHA-1
// implementation. This is an integrity fingerprint, not authentication.
static std::string FinishFileHash(SHA1_CTX& context)
{
    u8 digest[20];
    SHA1Final(digest, &context);
    std::string hash;
    for (u8 byte : digest)
    {
        hash += "0123456789abcdef"[byte >> 4];
        hash += "0123456789abcdef"[byte & 15];
    }
    return hash;
}

static bool HostFileHash(const fs::path& path, std::string& hash)
{
    FileHandle* file = OpenFile(UTF8ToString(path.u8string()), FileMode::Read);
    if (!file) return false;
    u64 remaining = FileLength(file);
    bool complete = remaining != 0 || IsEndOfFile(file);
    SHA1_CTX context;
    SHA1Init(&context);
    u8 buffer[0x1000];
    while (complete && remaining)
    {
        const u32 count = std::min<u64>(remaining, sizeof(buffer));
        complete = FileRead(buffer, 1, count, file) == count;
        if (complete) SHA1Update(&context, buffer, count);
        remaining -= count;
    }
    if (!IsEndOfFile(file)) complete = false;
    if (!CloseFile(file)) complete = false;
    if (!complete) return false;
    hash = FinishFileHash(context);
    return true;
}

FATStorage::FATStorage(const std::string& filename, u64 size, bool readonly, const std::optional<string>& sourcedir) :
    FATStorage(FATStorageArgs { filename, size, readonly, sourcedir })
{
}

FATStorage::FATStorage(const FATStorageArgs& args) noexcept :
    FATStorage(args.Filename, args.Size, args.ReadOnly, args.SourceDir)
{
}

FATStorage::FATStorage(FATStorageArgs&& args) noexcept :
    FilePath(std::move(args.Filename)),
    FileSize(args.Size),
    ReadOnly(args.ReadOnly),
    SourceDir(std::move(args.SourceDir))
{
    if (!Load(FilePath, FileSize, SourceDir))
    {
        if (File) CloseFile(File);
        File = nullptr;
        FileSize = 0;
    }
}

FATStorage::FATStorage(FATStorage&& other) noexcept
{
    FilePath = std::move(other.FilePath);
    IndexPath = std::move(other.IndexPath);
    SourceDir = std::move(other.SourceDir);
    ReadOnly = other.ReadOnly;
    File = other.File;
    FileSize = other.FileSize;
    DirIndex = std::move(other.DirIndex);
    FileIndex = std::move(other.FileIndex);

    other.File = nullptr;
    other.FileSize = 0;
}

FATStorage& FATStorage::operator=(FATStorage&& other) noexcept
{
    if (this != &other)
    {
        if (File)
        { // Sync this file's contents to the host (if applicable) before closing it
            if (!ReadOnly && !Save())
                Log(LogLevel::Error, "Failed to sync SD image to host directory\n");
            CloseFile(File);
        }

        FilePath = std::move(other.FilePath);
        IndexPath = std::move(other.IndexPath);
        SourceDir = std::move(other.SourceDir);
        ReadOnly = other.ReadOnly;
        File = other.File;
        FileSize = other.FileSize;
        DirIndex = std::move(other.DirIndex);
        FileIndex = std::move(other.FileIndex);

        other.File = nullptr;
        other.FileSize = 0;
        other.SourceDir = std::nullopt;
    }

    return *this;
}

FATStorage::~FATStorage()
{
    if (File && !ReadOnly && !Save())
        Log(LogLevel::Error, "Failed to sync SD image to host directory\n");

    if (File) CloseFile(File);
    File = nullptr;
}

bool FATStorage::InjectFile(const std::string& path, u8* data, u32 len)
{
    if (!File) return false;

    ff_disk_open(FF_ReadStorage(), FF_WriteStorage(), (LBA_t)(FileSize>>9));

    FRESULT res;
    FATFS fs;

    res = f_mount(&fs, "0:", 1);
    if (res != FR_OK)
    {
        f_unmount("0:");
        ff_disk_close();
        return false;
    }

    std::string prefixedPath("0:/");
    prefixedPath += path;
    FF_FIL file;
    res = f_open(&file, prefixedPath.c_str(), FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK)
    {
        f_unmount("0:");
        ff_disk_close();
        return false;
    }

    u32 nwrite;
    f_write(&file, data, len, &nwrite);
    f_close(&file);

    f_unmount("0:");
    ff_disk_close();
    return nwrite==len;
}

u32 FATStorage::ReadFile(const std::string& path, u32 start, u32 len, u8* data)
{
    if (!File) return false;

    ff_disk_open(FF_ReadStorage(), FF_WriteStorage(), (LBA_t)(FileSize>>9));

    FRESULT res;
    FATFS fs;

    res = f_mount(&fs, "0:", 1);
    if (res != FR_OK)
    {
        f_unmount("0:");
        ff_disk_close();
        return false;
    }

    std::string prefixedPath("0:/");
    prefixedPath += path;
    FF_FIL file;
    res = f_open(&file, prefixedPath.c_str(), FA_READ);
    if (res != FR_OK)
    {
        f_unmount("0:");
        ff_disk_close();
        return false;
    }

    u32 nread;
    f_lseek(&file, start);
    f_read(&file, data, len, &nread);
    f_close(&file);

    f_unmount("0:");
    ff_disk_close();
    return nread;
}

u32 FATStorage::ReadSectors(u32 start, u32 num, u8* data) const
{
    return ReadSectorsInternal(File, FileSize, start, num, data);
}

u32 FATStorage::WriteSectors(u32 start, u32 num, const u8* data)
{
    if (ReadOnly) return 0;
    return WriteSectorsInternal(File, FileSize, start, num, data);
}

u64 FATStorage::GetSectorCount() const
{
    return FileSize / 0x200;
}

ff_disk_read_cb FATStorage::FF_ReadStorage() const noexcept
{
    return [this](BYTE* buf, LBA_t sector, UINT num) {
        return ReadSectorsInternal(File, FileSize, sector, num, buf);
    };
}

ff_disk_write_cb FATStorage::FF_WriteStorage() const noexcept
{
    return [this](const BYTE* buf, LBA_t sector, UINT num) {
        return WriteSectorsInternal(File, FileSize, sector, num, buf);
    };
}


u32 FATStorage::ReadSectorsInternal(FileHandle* file, u64 filelen, u32 start, u32 num, u8* data)
{
    if (!file) return 0;

    u64 addr = start * 0x200ULL;
    u32 len = num * 0x200;

    if ((addr+len) > filelen)
    {
        if (addr >= filelen) return 0;
        len = filelen - addr;
        num = len >> 9;
    }

    if (!FileSeek(file, addr, FileSeekOrigin::Start)) return 0;

    u64 res = FileRead(data, 0x200, num, file);
    // Signed backend errors (e.g. Qt's -1) can arrive as unsigned counts.
    // Reject them before the sparse-image EOF path or narrowing to u32.
    if (res > num) return 0;
    if (res < num)
    {
        if (IsEndOfFile(file))
        {
            memset(&data[0x200*res], 0, 0x200*(num-res));
            return num;
        }
    }

    return static_cast<u32>(res);
}

u32 FATStorage::WriteSectorsInternal(FileHandle* file, u64 filelen, u32 start, u32 num, const u8* data)
{
    if (!file) return 0;

    u64 addr = start * 0x200ULL;
    u32 len = num * 0x200;

    if ((addr+len) > filelen)
    {
        if (addr >= filelen) return 0;
        len = filelen - addr;
        num = len >> 9;
    }

    if (!FileSeek(file, addr, FileSeekOrigin::Start)) return 0;

    u64 res = Platform::FileWrite(data, 0x200, num, file);
    if (res > num) return 0;
    return static_cast<u32>(res);
}


bool FATStorage::LoadIndex()
{
    DirIndex.clear();
    FileIndex.clear();

    FileHandle* f = OpenLocalFile(IndexPath, FileMode::ReadText);
    if (!f) return false;

    bool sized = false;
    bool complete = true;
    char linebuf[1536];
    while (!IsEndOfFile(f))
    {
        if (!FileReadLine(linebuf, 1536, f))
        { complete = false; break; }

        if (linebuf[0] == 'S')
        {
            u64 fsize;
            int ret = sscanf(linebuf, "SIZE %" PRIu64, &fsize);
            if (ret < 1) continue;

            FileSize = fsize;
            sized = fsize != 0;
        }
        else if (linebuf[0] == 'D')
        {
            u32 readonly;
            char fpath[1536] = {0};
            int ret = sscanf(linebuf, "DIR %u %[^\t\r\n]",
                             &readonly, fpath);
            if (ret < 2) continue;

            for (int i = 0; i < 1536 && fpath[i] != '\0'; i++)
            {
                if (fpath[i] == '\\')
                    fpath[i] = '/';
            }

            DirIndexEntry entry;
            entry.Path = fpath;
            entry.IsReadOnly = readonly!=0;

            DirIndex[entry.Path] = entry;
        }
        else if (linebuf[0] == 'F')
        {
            u32 readonly;
            u64 fsize;
            s64 lastmodified;
            u32 lastmod_internal;
            char fpath[1536] = {0};
            int ret = sscanf(linebuf, "FILE %u %" PRIu64 " %" PRId64 " %u %[^\t\r\n]",
                             &readonly, &fsize, &lastmodified, &lastmod_internal, fpath);
            if (ret < 5) continue;

            for (int i = 0; i < 1536 && fpath[i] != '\0'; i++)
            {
                if (fpath[i] == '\\')
                    fpath[i] = '/';
            }

            FileIndexEntry entry;
            entry.Path = fpath;
            entry.IsReadOnly = readonly!=0;
            entry.Size = fsize;
            entry.LastModified = lastmodified;
            entry.LastModifiedInternal = lastmod_internal;

            FileIndex[entry.Path] = entry;
        }
        else if (linebuf[0] == 'H')
        {
            char hash[41] = {}, fpath[1536] = {};
            if (sscanf(linebuf, "HASH %40[0-9a-f] %[^\t\r\n]", hash, fpath) == 2 && strlen(hash) == 40)
            {
                const auto entry = FileIndex.find(fpath);
                if (entry != FileIndex.end()) entry->second.HostHash = hash;
            }
        }
    }

    if (!CloseFile(f)) complete = false;

    // ensure the indexes are sane

    std::vector<std::string> removelist;

    for (const auto& [key, val] : DirIndex)
    {
        std::string path = val.Path;

        if ((path.find("/./") != std::string::npos) ||
            (path.find("/../") != std::string::npos) ||
            (path.substr(0,2) == "./") ||
            (path.substr(0,3) == "../"))
        {
            removelist.push_back(key);
            continue;
        }

        int sep = path.rfind('/');
        if (sep == std::string::npos) continue;

        path = path.substr(0, sep);
        if (DirIndex.count(path) < 1)
        {
            removelist.push_back(key);
        }
    }

    for (const auto& key : removelist)
    {
        DirIndex.erase(key);
    }

    removelist.clear();

    for (const auto& [key, val] : FileIndex)
    {
        std::string path = val.Path;

        if ((path.find("/./") != std::string::npos) ||
            (path.find("/../") != std::string::npos) ||
            (path.substr(0,2) == "./") ||
            (path.substr(0,3) == "../"))
        {
            removelist.push_back(key);
            continue;
        }

        int sep = path.rfind('/');
        if (sep == std::string::npos) continue;

        path = path.substr(0, sep);
        if (DirIndex.count(path) < 1)
        {
            removelist.push_back(key);
        }
    }

    for (const auto& key : removelist)
    {
        FileIndex.erase(key);
    }
    return sized && complete;
}

bool FATStorage::SaveIndex()
{
    return WriteFileAtomically(IndexPath, [&](const FileWriteCallback& write)
    {
        const auto line = [&](const std::string& text) { return write(text.data(), text.size()); };
        if (!line("SIZE " + std::to_string(FileSize) + "\n")) return false;
        for (const auto& [key, val] : DirIndex)
            if (!line("DIR " + std::to_string(val.IsReadOnly ? 1 : 0) + " " + val.Path + "\n")) return false;
        for (const auto& [key, val] : FileIndex)
        {
            if (!line("FILE " + std::to_string(val.IsReadOnly ? 1 : 0) + " " + std::to_string(val.Size) + " " +
                std::to_string(val.LastModified) + " " + std::to_string(val.LastModifiedInternal) + " " + val.Path + "\n")) return false;
            // Older versions ignore HASH records and still read FILE records.
            if (!val.HostHash.empty() && !line("HASH " + val.HostHash + " " + val.Path + "\n")) return false;
        }
        return true;
    }, true);
}


bool FATStorage::ExportFile(const std::string& path, fs::path out, std::string& hash)
{
    FF_FIL file;
    FRESULT res;

    res = f_open(&file, path.c_str(), FA_OPEN_EXISTING | FA_READ);
    if (res != FR_OK)
        return false;

    bool sourceOpen = true;
    SHA1_CTX context;
    SHA1Init(&context);
    const bool result = WriteFileAtomically(UTF8ToString(out.u8string()), [&](const FileWriteCallback& write)
    {
        u8 buf[0x1000];
        u32 remaining = f_size(&file);
        bool copied = true;
        while (remaining && copied)
        {
            const u32 count = std::min<u32>(remaining, sizeof(buf));
            u32 got = 0;
            copied = f_read(&file, buf, count, &got) == FR_OK && got == count;
            if (copied)
            {
                SHA1Update(&context, buf, count);
                copied = write(buf, count);
            }
            remaining -= count;
        }
        sourceOpen = false;
        return f_close(&file) == FR_OK && copied;
    });
    if (sourceOpen) f_close(&file);
    if (result) hash = FinishFileHash(context);
    return result;
}

bool FATStorage::FileNeedsExport(const std::string& path, const FF_FILINFO& info, bool& needed) const
{
    const auto previous = FileIndex.find(path);
    needed = previous == FileIndex.end() || info.fsize != previous->second.Size ||
        ((info.fdate << 16) | info.ftime) != previous->second.LastModifiedInternal || previous->second.HostHash.empty();
    if (needed) return true;

    // FAT timestamps have only two-second resolution. Check content before
    // skipping an apparently unchanged file, rather than losing such edits.
    FF_FIL file;
    if (f_open(&file, ("0:/" + path).c_str(), FA_READ) != FR_OK) return false;
    SHA1_CTX context;
    SHA1Init(&context);
    u8 buffer[0x1000];
    u32 remaining = f_size(&file);
    bool complete = true;
    while (remaining && complete)
    {
        const u32 count = std::min<u32>(remaining, sizeof(buffer));
        u32 got = 0;
        complete = f_read(&file, buffer, count, &got) == FR_OK && got == count;
        if (complete) SHA1Update(&context, buffer, count);
        remaining -= count;
    }
    if (f_close(&file) != FR_OK) complete = false;
    if (complete) needed = FinishFileHash(context) != previous->second.HostHash;
    return complete;
}

// A prior export may have committed while its index write failed. Exact
// equality makes that case safe to retry even though host metadata advanced.
static bool HostFileMatchesFATFile(const std::string& path, const fs::path& host)
{
    FileHandle* input = OpenFile(UTF8ToString(host.u8string()), FileMode::Read);
    if (!input) return false;
    FF_FIL file;
    if (f_open(&file, path.c_str(), FA_READ) != FR_OK)
    { CloseFile(input); return false; }
    u32 remaining = f_size(&file);
    bool equal = FileLength(input) == remaining;
    u8 guest[0x1000], external[0x1000];
    while (equal && remaining)
    {
        const u32 count = std::min<u32>(remaining, sizeof(guest));
        u32 got = 0;
        equal = f_read(&file, guest, count, &got) == FR_OK && got == count &&
            FileRead(external, 1, count, input) == count && memcmp(guest, external, count) == 0;
        remaining -= count;
    }
    if (f_close(&file) != FR_OK) equal = false;
    if (!CloseFile(input)) equal = false;
    return equal;
}

bool FATStorage::CheckPendingExports(const std::string& path, const std::string& outbase, int level, bool& pending)
{
    if (level >= 32) return false;
    FF_DIR dir;
    if (f_opendir(&dir, ("0:/" + path).c_str()) != FR_OK) return false;
    bool safe = true;
    FF_FILINFO info;
    while (safe)
    {
        if (f_readdir(&dir, &info) != FR_OK) { safe = false; break; }
        if (!info.fname[0]) break;
        const std::string fullpath = path + info.fname;
        const auto host = melonDS::PathFromUTF8(outbase + "/" + fullpath);
        std::error_code err;
        const bool exists = fs::exists(host, err);
        if (err) { safe = false; break; }
        if (info.fattrib & AM_DIR)
        {
            if (exists && !fs::is_directory(host, err)) { safe = false; break; }
            if (DirIndex.count(fullpath) == 0) pending = true;
            safe = CheckPendingExports(fullpath + "/", outbase, level + 1, pending);
            continue;
        }
        bool needed;
        if (!FileNeedsExport(fullpath, info, needed)) { safe = false; break; }
        if (!needed) continue;

        pending = true;
        const auto previous = FileIndex.find(fullpath);
        if (!exists)
        {
            // An absent previously synced file is a host deletion, hence a
            // conflict with pending guest edits. A fresh guest file is safe.
            safe = previous == FileIndex.end();
            continue;
        }
        if (!fs::is_regular_file(host, err) || err) { safe = false; break; }
        std::string hash;
        if (!HostFileHash(host, hash)) { safe = false; break; }
        const bool unchanged = previous != FileIndex.end() && previous->second.HostHash == hash;
        // Legacy entries lack content evidence: only exact host/guest equality
        // can resolve that ambiguity automatically. Otherwise preserve both.
        safe = unchanged || HostFileMatchesFATFile("0:/" + fullpath, host);
    }
    if (f_closedir(&dir) != FR_OK) safe = false;
    return safe;
}

bool FATStorage::ExportDirectory(const std::string& path, const std::string& outbase, int level)
{
    if (level >= 32) return false;

    FF_DIR dir;
    FF_FILINFO info;
    FRESULT res;

    std::string fullpath = "0:/" + path;
    res = f_opendir(&dir, fullpath.c_str());
    if (res != FR_OK) return false;

    std::vector<std::string> subdirlist;
    bool complete = true;

    for (;;)
    {
        res = f_readdir(&dir, &info);
        if (res != FR_OK) { complete = false; break; }
        if (!info.fname[0]) break;

        std::string fullpath = path + info.fname;
        fs::path outpath = melonDS::PathFromUTF8(outbase + "/" + fullpath);

        if (info.fattrib & AM_DIR)
        {
            if (DirIndex.count(fullpath) < 1)
            {
                std::error_code err;
                fs::create_directory(outpath, err);
                if (err) { complete = false; continue; }

                DirIndexEntry entry;
                entry.Path = fullpath;
                entry.IsReadOnly = (info.fattrib & AM_RDO) != 0;

                DirIndex[entry.Path] = entry;
            }

            subdirlist.push_back(fullpath);
        }
        else
        {
            const u32 lastmod = (info.fdate << 16) | info.ftime;
            bool doexport;
            if (!FileNeedsExport(fullpath, info, doexport)) { complete = false; continue; }
            if (doexport)
            {
                std::string hash;
                if (!ExportFile("0:/"+fullpath, outpath, hash))
                {
                    complete = false;
                    continue; // Keep the previous index entry and host permissions for retry.
                }
                std::error_code err;
                const auto modtime = fs::last_write_time(outpath, err);
                if (err) { complete = false; continue; }
                const s64 modtime_raw = std::chrono::duration_cast<std::chrono::seconds>(modtime.time_since_epoch()).count();
                FileIndex[fullpath] = {fullpath, (info.fattrib & AM_RDO) != 0, info.fsize, modtime_raw, lastmod, std::move(hash)};
            }
        }

        std::error_code err;
        fs::permissions(outpath,
                        fs::perms::owner_read | fs::perms::owner_write,
                        (info.fattrib & AM_RDO) ? fs::perm_options::remove : fs::perm_options::add,
                        err);
    }

    if (f_closedir(&dir) != FR_OK) complete = false;

    for (auto& entry : subdirlist)
    {
        if (!ExportDirectory(entry+"/", outbase, level+1)) complete = false;
    }
    return complete;
}

// Follow the explicitly selected root, but not a descendant replaced by a
// link or non-directory. Missing paths are already-deleted destinations.
static bool HostDeletionStatus(const std::string& outbase, const std::string& path, fs::file_status& status)
{
    fs::path host = melonDS::PathFromUTF8(outbase);
    std::error_code err;
    status = fs::status(host, err);
    if (err || !fs::is_directory(status)) return false;
    const auto relative = melonDS::PathFromUTF8(path);
    for (auto part = relative.begin(); part != relative.end(); ++part)
    {
        host /= *part;
#ifdef _WIN32
        // MinGW's symlink_status can follow directory symlinks. Inspect the
        // reparse attribute before it can lead deletion outside the root.
        const DWORD attributes = GetFileAttributesW(host.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES)
        {
            const DWORD error = GetLastError();
            if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
        }
        else if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) return false;
#endif
        status = fs::symlink_status(host, err);
        if (status.type() == fs::file_type::not_found &&
            (!err || err == std::errc::no_such_file_or_directory)) return true;
        if (err || fs::is_symlink(status)) return false;
        if (std::next(part) != relative.end() && !fs::is_directory(status)) return false;
    }
    return true;
}

static bool RemoveHostEntry(const fs::path& path, fs::perms original)
{
    const auto required = fs::perms::owner_read | fs::perms::owner_write;
    const bool relax = (original & required) != required;
    std::error_code err;
    if (relax)
    {
        // A guest may clear AM_RDO and then delete a formerly read-only file.
        fs::permissions(path, required, fs::perm_options::add, err);
        if (err) return false;
    }
    fs::remove(path, err);
    if (!err) return true;
    if (relax)
    {
        fs::permissions(path, original, fs::perm_options::replace, err);
        if (err) Log(LogLevel::Error, "Failed to restore host permissions after SD sync deletion failure: %s\n",
            UTF8ToString(path.u8string()).c_str());
    }
    return false;
}

bool FATStorage::CheckPendingDeletions(const std::string& outbase, bool& pending)
{
    const auto check = [&](const std::string& path, bool directory)
    {
        FF_FILINFO info;
        const FRESULT res = f_stat(("0:/" + path).c_str(), &info);
        if (res == FR_OK && bool(info.fattrib & AM_DIR) == directory) return true;
        if (res != FR_OK && res != FR_NO_FILE && res != FR_NO_PATH) return false;
        pending = true;

        fs::file_status status;
        if (!HostDeletionStatus(outbase, path, status)) return false;
        if (!fs::exists(status)) return true;
        const auto host = melonDS::PathFromUTF8(outbase + "/" + path);
        if (!directory)
        {
            std::string hash;
            return fs::is_regular_file(status) && HostFileHash(host, hash) &&
                hash == FileIndex.at(path).HostHash;
        }
        if (!fs::is_directory(status)) return false;
        // Every indexed descendant is checked separately. Reject new host
        // children before removing any of the deleted directory's contents.
        std::error_code err;
        fs::directory_iterator entry(host, err), end;
        while (!err && entry != end)
        {
            const auto child = path + "/" + UTF8ToString(entry->path().filename().u8string());
            if (!FileIndex.count(child) && !DirIndex.count(child)) return false;
            entry.increment(err);
        }
        return !err;
    };
    for (const auto& [path, entry] : FileIndex)
        if (!check(path, false)) return false;
    for (const auto& [path, entry] : DirIndex)
        if (!check(path, true)) return false;
    return true;
}

bool FATStorage::DeleteHostDirectory(const std::string& path, const std::string& outbase)
{
    fs::file_status status;
    if (!HostDeletionStatus(outbase, path, status)) return false;
    if (fs::exists(status))
    {
        if (!fs::is_directory(status)) return false;
        // Indexed children have already been removed. Never recursively
        // remove an unindexed file that appeared after the preflight.
        if (!RemoveHostEntry(melonDS::PathFromUTF8(outbase + "/" + path), status.permissions())) return false;
    }
    DirIndex.erase(path);
    return true;
}

bool FATStorage::ExportChanges(const std::string& outbase)
{
    // reflect changes in the FAT volume to the host filesystem
    // * delete directories and files that exist in the index but not in the volume
    // * copy files to the host FS if they exist within the index and their size or
    //   internal last-modified time is different
    // * index and copy directories and files that exist in the volume but not in
    //   the index

    bool pending = false;
    if (!CheckPendingDeletions(outbase, pending)) return false;
    bool complete = true;
    std::vector<std::string> deletelist;

    for (const auto& [key, val] : FileIndex)
    {
        std::string innerpath = "0:/" + val.Path;
        FF_FILINFO finfo;
        FRESULT res = f_stat(innerpath.c_str(), &finfo);
        if (res == FR_OK)
        {
            if (finfo.fattrib & AM_DIR)
            {
                deletelist.push_back(key);
            }
        }
        else if (res == FR_NO_FILE || res == FR_NO_PATH)
        {
            deletelist.push_back(key);
        }
    }

    for (const auto& key : deletelist)
    {
        fs::file_status status;
        if (!HostDeletionStatus(outbase, key, status) ||
            (fs::exists(status) && !fs::is_regular_file(status)))
        { complete = false; continue; }
        if (fs::exists(status))
        {
            if (!RemoveHostEntry(melonDS::PathFromUTF8(outbase + "/" + key), status.permissions()))
            { complete = false; continue; }
        }
        FileIndex.erase(key);
    }

    deletelist.clear();

    for (const auto& [key, val] : DirIndex)
    {
        std::string innerpath = "0:/" + val.Path;
        FF_FILINFO finfo;
        FRESULT res = f_stat(innerpath.c_str(), &finfo);
        if (res == FR_OK)
        {
            if (!(finfo.fattrib & AM_DIR))
            {
                deletelist.push_back(key);
            }
        }
        else if (res == FR_NO_FILE || res == FR_NO_PATH)
        {
            deletelist.push_back(key);
        }
    }

    // Map order puts ancestors before descendants; remove empty directories
    // in reverse order, retaining every failed entry for the next sync.
    for (auto entry = deletelist.rbegin(); entry != deletelist.rend(); ++entry)
        if (!DeleteHostDirectory(*entry, outbase)) complete = false;

    return ExportDirectory("", outbase, 0) && complete;
}


bool FATStorage::CanFitFile(u32 len)
{
    FATFS* fs;
    DWORD freeclusters;
    FRESULT res;

    res = f_getfree("0:", &freeclusters, &fs);
    if (res != FR_OK) return false;

    u32 clustersize = fs->csize * 0x200;

    len = (len + clustersize - 1) / clustersize;
    return (freeclusters >= len);
}

bool FATStorage::DeleteDirectory(const std::string& path, int level)
{
    if (level >= 32) return false;
    if (path.length() < 1) return false;

    FF_DIR dir;
    FF_FILINFO info;
    FRESULT res;

    std::string fullpath = "0:/" + path;
    f_chmod(fullpath.c_str(), 0, AM_RDO);
    res = f_opendir(&dir, fullpath.c_str());
    if (res != FR_OK) return false;

    std::vector<std::string> deletelist;
    std::vector<std::string> subdirlist;
    int survivors = 0;

    for (;;)
    {
        res = f_readdir(&dir, &info);
        if (res != FR_OK) break;
        if (!info.fname[0]) break;

        std::string fullpath = path + info.fname;

        if (info.fattrib & AM_DIR)
        {
            subdirlist.push_back(fullpath);
        }
        else
        {
            deletelist.push_back(fullpath);
        }
    }

    f_closedir(&dir);

    for (auto& entry : deletelist)
    {
        std::string fullpath = "0:/" + entry;
        f_chmod(fullpath.c_str(), 0, AM_RDO);
        res = f_unlink(fullpath.c_str());
        if (res != FR_OK) return false;
    }

    for (auto& entry : subdirlist)
    {
        if (!DeleteDirectory(entry+"/", level+1))
            return false;
    }

    res = f_unlink(fullpath.c_str());
    if (res != FR_OK) return false;

    return true;
}

void FATStorage::CleanupDirectory(const std::string& sourcedir, const std::string& path, int level)
{
    if (level >= 32) return;

    FF_DIR dir;
    FF_FILINFO info;
    FRESULT res;

    std::string fullpath = "0:/" + path;
    res = f_opendir(&dir, fullpath.c_str());
    if (res != FR_OK) return;

    std::vector<std::string> filedeletelist;
    std::vector<std::string> dirdeletelist;
    std::vector<std::string> subdirlist;

    for (;;)
    {
        res = f_readdir(&dir, &info);
        if (res != FR_OK) break;
        if (!info.fname[0]) break;

        std::string fullpath = path + info.fname;

        if (info.fattrib & AM_DIR)
        {
            if (DirIndex.count(fullpath) < 1)
                dirdeletelist.push_back(fullpath);
            else if (!fs::is_directory(melonDS::PathFromUTF8(sourcedir+"/"+fullpath)))
            {
                DirIndex.erase(fullpath);
                dirdeletelist.push_back(fullpath);
            }
            else
                subdirlist.push_back(fullpath);
        }
        else
        {
            if (FileIndex.count(fullpath) < 1)
                filedeletelist.push_back(fullpath);
            else if (!fs::is_regular_file(melonDS::PathFromUTF8(sourcedir+"/"+fullpath)))
            {
                FileIndex.erase(fullpath);
                filedeletelist.push_back(fullpath);
            }
        }
    }

    f_closedir(&dir);

    for (auto& entry : filedeletelist)
    {
        std::string fullpath = "0:/" + entry;
        f_chmod(fullpath.c_str(), 0, AM_RDO);
        f_unlink(fullpath.c_str());
    }

    for (auto& entry : dirdeletelist)
    {
        DeleteDirectory(entry+"/", level+1);
    }

    for (auto& entry : subdirlist)
    {
        CleanupDirectory(sourcedir, entry+"/", level+1);
    }
}

bool FATStorage::ImportFile(const std::string& path, fs::path in)
{
    FF_FIL file;
    FileHandle* fin;
    FRESULT res;

    fin = Platform::OpenFile(UTF8ToString(in.u8string()), FileMode::Read);
    if (!fin)
        return false;

    u32 len = FileLength(fin);

    if (!CanFitFile(len))
    {
        CloseFile(fin);
        return false;
    }

    res = f_open(&file, path.c_str(), FA_CREATE_ALWAYS | FA_WRITE);
    if (res != FR_OK)
    {
        CloseFile(fin);
        return false;
    }

    u8 buf[0x1000];
    for (u32 i = 0; i < len; i += 0x1000)
    {
        u32 blocklen;
        if ((i + 0x1000) > len)
            blocklen = len - i;
        else
            blocklen = 0x1000;

        u32 nwrite;
        FileRead(buf, blocklen, 1, fin);
        f_write(&file, buf, blocklen, &nwrite);
    }

    CloseFile(fin);
    f_close(&file);

    return true;
}

bool FATStorage::ImportDirectory(const std::string& sourcedir)
{
    // remove whatever isn't in the index
    CleanupDirectory(sourcedir, "", 0);

    int srclen = sourcedir.length();

    // iterate through the host directory:
    // * directories will be added if they aren't in the index
    // * files will be added if they aren't in the index, or if the size or last-modified-date don't match
    for (auto& entry : fs::recursive_directory_iterator(melonDS::PathFromUTF8(sourcedir)))
    {
        std::string fullpath = UTF8ToString(entry.path().u8string());
        std::string innerpath = fullpath.substr(srclen);
        if (innerpath[0] == '/' || innerpath[0] == '\\')
            innerpath = innerpath.substr(1);

        int ilen = innerpath.length();
        for (int i = 0; i < ilen; i++)
        {
            if (innerpath[i] == '\\')
                innerpath[i] = '/';
        }

        bool readonly = (entry.status().permissions() & fs::perms::owner_write) == fs::perms::none;

        if (entry.is_directory())
        {
            if (DirIndex.count(innerpath) < 1)
            {
                DirIndexEntry ientry;
                ientry.Path = innerpath;
                ientry.IsReadOnly = readonly;

                innerpath = "0:/" + innerpath;
                FRESULT res = f_mkdir(innerpath.c_str());
                if (res == FR_OK)
                {
                    DirIndex[ientry.Path] = ientry;
                }
            }
        }
        else if (entry.is_regular_file())
        {
            u64 filesize = entry.file_size();
            std::string hash;
            if (!HostFileHash(entry.path(), hash)) return false;

            auto lastmodified = entry.last_write_time();
            s64 lastmodified_raw = std::chrono::duration_cast<std::chrono::seconds>(lastmodified.time_since_epoch()).count();

            bool import = false;
            if (FileIndex.count(innerpath) < 1)
            {
                import = true;
            }
            else
            {
                FileIndexEntry& chk = FileIndex[innerpath];
                if (chk.Size != filesize) import = true;
                if (chk.LastModified != lastmodified_raw) import = true;
                if (!chk.HostHash.empty() && chk.HostHash != hash) import = true;
            }

            if (import)
            {
                FileIndexEntry ientry;
                ientry.Path = innerpath;
                ientry.IsReadOnly = readonly;
                ientry.Size = filesize;
                ientry.LastModified = lastmodified_raw;
                ientry.HostHash = hash;

                innerpath = "0:/" + innerpath;
                if (ImportFile(innerpath, entry.path()))
                {
                    FF_FILINFO finfo;
                    f_stat(innerpath.c_str(), &finfo);

                    ientry.LastModifiedInternal = (finfo.fdate << 16) | finfo.ftime;

                    FileIndex[ientry.Path] = ientry;
                }
            }
            else FileIndex[innerpath].HostHash = std::move(hash);
        }

        f_chmod(innerpath.c_str(), readonly?AM_RDO:0, AM_RDO);
    }

    return SaveIndex();
}

u64 FATStorage::GetDirectorySize(fs::path sourcedir) const
{
    u64 ret = 0;
    u32 csize = 0x1000; // this is an estimate

    for (auto& entry : fs::recursive_directory_iterator(sourcedir))
    {
        if (entry.is_directory())
        {
            ret += csize;
        }
        else if (entry.is_regular_file())
        {
            u64 filesize = entry.file_size();

            filesize = (filesize + (csize-1)) & ~(csize-1);
            ret += filesize;
        }
    }

    return ret;
}

bool FATStorage::Load(const std::string& filename, u64 size, const std::optional<string>& sourcedir)
{
    bool hasdir = sourcedir && !sourcedir->empty();
    if (sourcedir)
    {
        if (!fs::is_directory(melonDS::PathFromUTF8(*sourcedir)))
        {
            hasdir = false;
            SourceDir = std::nullopt;
        }
    }

    // 'auto' size management: (size=0)
    // * if an index exists: the size from the index is used
    // * if no index, and an image file exists: the file size is used
    // * if sourcing from a directory, size is calculated from that
    //   with a minimum 128MB extra, otherwise size is defaulted to 512MB

    bool isnew = !Platform::LocalFileExists(filename);
    File = Platform::OpenLocalFile(filename, static_cast<FileMode>(FileMode::ReadWrite | FileMode::Preserve));
    if (!File)
        return false;

    IndexPath = FilePath + ".idx";
    const bool hadIndex = !isnew && LocalFileExists(IndexPath);
    const bool loadedIndex = !isnew && LoadIndex();

    const u64 physicalSize = FileLength(File);
    if (FileSize == 0)
        FileSize = physicalSize;

    // A new or genuinely empty image can be formatted. A failed size query
    // also returns zero, so confirm EOF before treating it as an empty image.
    bool needformat = physicalSize == 0;
    FATFS fs;
    FRESULT res;

    if (needformat)
    {
        u8 probe;
        if (!FileSeek(File, 0, FileSeekOrigin::Start) ||
            FileRead(&probe, 1, 1, File) != 0 || !IsEndOfFile(File))
        {
            Log(LogLevel::Error, "Failed to read SD image; refusing to format it\n");
            return false;
        }
    }
    else
    {
        ff_disk_open(FF_ReadStorage(), FF_WriteStorage(), (LBA_t)(FileSize>>9));

        res = f_mount(&fs, "0:", 1);
        // Mount errors on existing data must never authorize formatting.
        if (res == FR_OK && size > 0 && size != FileSize)
        {
            needformat = true;
        }
    }

    if (needformat)
    {
        FileSize = size;
        if (FileSize == 0)
        {
            if (hasdir)
            {
                FileSize = GetDirectorySize(melonDS::PathFromUTF8(*sourcedir));
                FileSize += 0x8000000ULL; // 128MB leeway

                // make it a power of two
                FileSize |= (FileSize >> 1);
                FileSize |= (FileSize >> 2);
                FileSize |= (FileSize >> 4);
                FileSize |= (FileSize >> 8);
                FileSize |= (FileSize >> 16);
                FileSize |= (FileSize >> 32);
                FileSize++;
            }
            else
                FileSize = 0x20000000ULL; // 512MB
        }

        ff_disk_close();
        ff_disk_open(FF_ReadStorage(), FF_WriteStorage(), (LBA_t)(FileSize>>9));

        DirIndex.clear();
        FileIndex.clear();

        FF_MKFS_PARM fsopt;

        // FAT type: we force it to FAT32 for any volume that is 1GB or more
        // libfat attempts to determine the FAT type from the volume size and other parameters
        // which can lead to it trying to interpret a FAT16 volume as FAT32
        if (FileSize >= 0x40000000ULL)
            fsopt.fmt = FM_FAT32;
        else
            fsopt.fmt = FM_FAT;

        fsopt.au_size = 0;
        fsopt.align = 1;
        fsopt.n_fat = 1;
        fsopt.n_root = 512;

        BYTE workbuf[FF_MAX_SS];
        res = f_mkfs("0:", &fsopt, workbuf, sizeof(workbuf));

        if (res == FR_OK)
            res = f_mount(&fs, "0:", 1);
    }

    bool synced = true;
    if (res == FR_OK)
    {
        if (needformat)
        {
            const bool indexed = SaveIndex();
            // Image-only storage historically treats the sidecar as optional;
            // frontends need atomic host-file support only for folder sync.
            synced = indexed || !hasdir;
        }
        if (hasdir && !needformat && hadIndex)
        {
            // Preflight every pending guest export before host-import cleanup
            // can remove unindexed guest files. Host-only edits still import.
            bool pending = false;
            synced = loadedIndex && CheckPendingDeletions(*sourcedir, pending) &&
                CheckPendingExports("", *sourcedir, 0, pending);
            if (synced && pending)
            {
                const bool exported = ExportChanges(*sourcedir);
                const bool indexed = SaveIndex();
                synced = exported && indexed;
            }
            if (!synced)
                Log(LogLevel::Error, "SD sync refused: pending guest exports conflict with host changes or could not be saved; preserve the image, host directory and index before resolving\n");
        }
        if (hasdir && synced)
            synced = ImportDirectory(*sourcedir);
    }
    else
        Log(LogLevel::Error, "Failed to mount or format SD image (FAT error %d)\n", res);

    f_unmount("0:");

    ff_disk_close();

    return res == FR_OK && synced;
}

bool FATStorage::Save()
{
    if (!File) return false;

    if (!SourceDir)
    { // If we're not syncing the SD card image to a host directory...
        return true; // Not an error.
    }

    ff_disk_open(FF_ReadStorage(), FF_WriteStorage(), (LBA_t)(FileSize>>9));

    FRESULT res;
    FATFS fs;

    res = f_mount(&fs, "0:", 1);
    if (res != FR_OK)
    {
        f_unmount("0:");
        ff_disk_close();
        return false;
    }

    const bool exported = ExportChanges(*SourceDir);
    // Successful files may advance independently, but failed entries retain
    // their old metadata. Atomic index writes preserve that retry evidence.
    const bool indexed = SaveIndex();

    f_unmount("0:");

    ff_disk_close();

    return exported && indexed;
}

}
