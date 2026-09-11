// SPDX-License-Identifier: GPL-3.0-or-later
// Exercise the real file constructor at its Platform boundary, without private dumps.
#include "SPI_Firmware.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

namespace melonDS {
#include "FirmwareCRC16.inc"
}
namespace melonDS::Platform {
struct FileHandle
{
    std::vector<u8> data;
    u64 length;
    u64 position = 7;
    unsigned reads = 0;
    bool shortRead = false;
    bool readError = false;
};
u64 FileLength(FileHandle* file) { return file->length; }
void FileRewind(FileHandle* file) { file->position = 0; }
u64 FileRead(void* data, u64 size, u64 count, FileHandle* file)
{
    ++file->reads;
    if (file->readError) return u64(-1); // Qt's failed read is signed before conversion.
    // A rejected length must not reach this call. Avoid corrupting the test process
    // when reproducing the old constructor's oversized transfer request.
    if (count != 1 || size > file->data.size()) return 0;
    const u64 actual = size - (file->shortRead && size ? 1 : 0);
    std::memcpy(data, file->data.data(), actual);
    file->position += actual;
    return actual == size ? 1 : 0;
}
void Log(LogLevel, const char*, ...) {}
}

int main()
{
    using namespace melonDS;
    unsigned failures = 0;
    const auto check = [&](bool passed, const char* message) {
        if (!passed) { ++failures; std::fprintf(stderr, "%s\n", message); }
    };
    for (u64 length : {u64(0), u64(0x80001), (u64(1) << 32) + 0x40000,
                       std::numeric_limits<u64>::max()})
    {
        Platform::FileHandle file{{}, length};
        Firmware firmware(&file);
        check(!firmware.Buffer() && firmware.Length() == 0 && file.reads == 0 && file.position == 0,
            "Invalid firmware length reached a read or produced a firmware buffer");
    }
    for (u32 length : {0x20000U, 0x40000U, 0x80000U, 0x20001U})
    {
        Platform::FileHandle file{std::vector<u8>(length, 0x5A), length};
        const auto original = file.data;
        Firmware firmware(&file);
        const u32 padded = length == 0x20001 ? 0x40000 : length;
        check(firmware.Buffer() && firmware.Length() == padded && file.reads == 1 && file.position == 0,
            "Accepted firmware length/read/rewind changed");
        if (firmware.Buffer())
        {
            check(std::memcmp(firmware.Buffer(), original.data(), length) == 0,
                "Firmware bytes changed during loading");
            check(std::all_of(firmware.Buffer() + length, firmware.Buffer() + padded,
                [](u8 byte) { return byte == 0; }), "Legacy firmware padding changed");
        }
        check(file.data == original, "Source firmware bytes changed");
    }
    for (bool error : {false, true})
    {
        Platform::FileHandle file{std::vector<u8>(0x20000, 0x5A), 0x20000};
        file.shortRead = !error;
        file.readError = error;
        Firmware firmware(&file);
        check(!firmware.Buffer() && firmware.Length() == 0 && file.reads == 1 && file.position == 0,
            "Short or failed firmware read was accepted");
    }
    std::printf("Firmware file length, content, read failures and rewind: %u failures\n", failures);
    return failures ? 1 : 0;
}
