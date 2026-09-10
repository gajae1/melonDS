// SPDX-License-Identifier: GPL-3.0-or-later
// Real NAND crypto and FatFs over synthetic, bounded memory-only backing I/O.
#include "Args.h"
#include "DSi.h"
#include "DSi_NAND.h"
#include "NDSCart.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

using namespace melonDS;
using namespace melonDS::DSi_NAND;

namespace melonDS::Platform {
struct FileHandle
{
    std::vector<u8> bytes;
    u64 position = 0;
    u64 failReadAt = std::numeric_limits<u64>::max();
    u64 lastReadAt = 0;
    unsigned injectedFailures = 0;
    unsigned successfulMatchingReads = 0;
};
bool CloseFile(FileHandle* f) { delete f; return true; }
bool IsEndOfFile(FileHandle* f) { return f->position == f->bytes.size(); }
bool FileReadLine(char*, int, FileHandle*) { std::abort(); }
u64 FilePosition(FileHandle* f) { return f->position; }
bool FileSeek(FileHandle* f, s64 offset, FileSeekOrigin origin)
{
    const s64 base = origin == FileSeekOrigin::Start ? 0 :
        static_cast<s64>(origin == FileSeekOrigin::End ? f->bytes.size() : f->position);
    if (offset < -base || offset > static_cast<s64>(f->bytes.size()) - base) return false;
    f->position = base + offset;
    return true;
}
void FileRewind(FileHandle* f) { f->position = 0; }
u64 FileRead(void* dst, u64 size, u64 count, FileHandle* f)
{
    if (!size) return 0;
    f->lastReadAt = f->position;
    if (f->position == f->failReadAt)
    {
        if (f->successfulMatchingReads) --f->successfulMatchingReads;
        else { ++f->injectedFailures; return 0; }
    }
    count = std::min(count, (f->bytes.size() - f->position) / size);
    if (count) std::memcpy(dst, f->bytes.data() + f->position, count * size);
    f->position += count * size;
    return count;
}
bool FileFlush(FileHandle*) { return true; }
u64 FileWrite(const void* src, u64 size, u64 count, FileHandle* f)
{
    if (!size) return 0;
    count = std::min(count, (f->bytes.size() - f->position) / size);
    if (count) std::memcpy(f->bytes.data() + f->position, src, count * size);
    f->position += count * size;
    return count;
}
u64 FileWriteFormatted(FileHandle*, const char*, ...) { std::abort(); }
u64 FileLength(FileHandle* f) { return f->bytes.size(); }
}

static void Require(bool ok, const char* what)
{
    if (!ok) { std::fprintf(stderr, "fixture failed: %s\n", what); std::exit(2); }
}

std::optional<NANDImage> MakeDSiBootNAND()
{
    auto* backing = new Platform::FileHandle{std::vector<u8>(8 * 1024 * 1024)};
    // Only the public nocash footer signature is needed. All identity/key input
    // bytes are synthetic zeroes. No bootloader or physical NAND dump is used.
    std::memcpy(backing->bytes.data() + backing->bytes.size() - 0x40, "DSi eMMC CID/CPU", 16);
    std::optional<NANDImage> image(std::in_place, backing, DSiKey{});
    Require(static_cast<bool>(*image), "synthetic footer");
    {
        NANDMount mount(*image);
        Require(static_cast<bool>(mount), "mount");
        FF_MKFS_PARM options{};
        options.fmt = FM_FAT | FM_SFD;
        std::vector<u8> work(4096);
        Require(f_mkfs("0:", &options, work.data(), work.size()) == FR_OK, "format synthetic FAT");
        Require(f_mkdir("0:/sys") == FR_OK, "sys directory");
    }
    return image;
}

void FailDSiBootHardwareRead(NANDImage& image, unsigned successfulReads)
{
    NANDMount mount(image);
    DSiHardwareInfoN data;
    Require(mount.ReadHardwareInfoN(data), "calibrate hardware data-sector fault");
    image.GetFile()->failReadAt = image.GetFile()->lastReadAt;
    image.GetFile()->successfulMatchingReads = successfulReads;
}

static const char* MetadataPath(bool serial)
{
    return serial ? "0:/sys/HWINFO_S.dat" : "0:/sys/HWINFO_N.dat";
}

static std::vector<u8> MetadataBytes(size_t size)
{
    std::vector<u8> data(size);
    for (size_t i = 0; i < size; ++i) data[i] = static_cast<u8>(i * 13 + 7);
    return data;
}

static int Readers(bool failures)
{
    auto image = MakeDSiBootNAND();
    int errors = 0;
    for (bool serial : {true, false})
    {
        const size_t size = serial ? sizeof(DSiSerialData) : sizeof(DSiHardwareInfoN);
        const auto bytes = MetadataBytes(0x4000);
        for (int condition = failures ? 0 : 4; condition < (failures ? 4 : 6); ++condition)
        {
            NANDMount mount(*image);
            const char* path = MetadataPath(serial);
            if (condition == 0)
                Require(f_stat(path, nullptr) == FR_NO_FILE, "missing metadata");
            else
            {
                const size_t length = condition == 1 ? 0 : condition == 2 ? size-1 :
                    condition == 5 ? bytes.size() : size;
                if (length)
                    Require(mount.ImportFile(path, bytes.data(), length), "write metadata");
                else
                {
                    FF_FIL empty;
                    Require(f_open(&empty, path, FA_CREATE_ALWAYS | FA_WRITE) == FR_OK, "empty metadata");
                    Require(f_close(&empty) == FR_OK, "close empty metadata");
                }
            }
            DSiSerialData dataS;
            DSiHardwareInfoN dataN;
            std::memset(&dataS, 0xA5, sizeof(dataS));
            dataN.fill(0xA5);
            auto* backing = image->GetFile();
            auto read = [&] { return serial ? mount.ReadSerialData(dataS) : mount.ReadHardwareInfoN(dataN); };
            if (condition == 3)
            {
                Require(read(), "healthy read before fault injection");
                // Last backing read was the metadata data sector, after open
                // succeeded. Fail just this sector on the next real FatFs read.
                backing->failReadAt = backing->lastReadAt;
                std::memset(&dataS, 0xA5, sizeof(dataS));
                dataN.fill(0xA5);
            }
            const bool ok = read();
            const u8* output = serial ? dataS.Bytes : dataN.data();
            const bool expectedSuccess = condition >= 4;
            const bool dataOK = expectedSuccess ? std::equal(output, output+size, bytes.begin()) :
                std::all_of(output, output+size, [](u8 byte) { return byte == 0xA5; });
            if (condition == 3) Require(backing->injectedFailures != 0, "backing fault reached");
            backing->failReadAt = std::numeric_limits<u64>::max();
            const bool pass = ok == expectedSuccess && dataOK;
            const char* names[] = {"missing", "empty", "truncated", "backing-read-error", "exact", "padded"};
            std::printf("%s %s: status=%d output=%s %s\n", serial ? "HWINFO_S" : "HWINFO_N",
                names[condition], ok, dataOK ? "preserved/exact" : "changed", pass ? "PASS" : "FAIL");
            errors += !pass;
        }
    }
    return errors ? 1 : 0;
}

class BootCart : public NDSCart::CartCommon
{
public:
    using CartCommon::CartCommon;
    unsigned bootCalls = 0;
    void SetupDirectBoot(const std::string& name, NDS& nds) override
    {
        ++bootCalls;
        CartCommon::SetupDirectBoot(name, nds);
    }
};

static int Caller()
{
    auto dsi = std::make_unique<DSi>(DSiArgs{});
    auto serial = MetadataBytes(sizeof(DSiSerialData));
    auto hardware = MetadataBytes(sizeof(DSiHardwareInfoN));
    int errors = 0;
    for (int condition = 0; condition < 6; ++condition)
    {
        auto image = MakeDSiBootNAND();
        {
            NANDMount mount(*image);
            if (condition != 0)
                Require(mount.ImportFile(MetadataPath(true), serial.data(),
                    condition == 2 ? serial.size()-1 : serial.size()), "caller serial");
            if (condition != 1)
                Require(mount.ImportFile(MetadataPath(false), hardware.data(),
                    condition == 3 ? hardware.size()-1 : hardware.size()), "caller hardware");
            if (condition == 5)
            {
                DSiHardwareInfoN data;
                Require(mount.ReadHardwareInfoN(data), "caller read before injected data fault");
                image->GetFile()->failReadAt = image->GetFile()->lastReadAt;
            }
        }
        NDSHeader header{};
        header.UnitCode = 2;
        header.ARM9ROMOffset = 0x10000;
        header.ARM7ROMOffset = 0x11000;
        header.ARM9RAMAddress = header.ARM9EntryAddress = 0x02010000;
        header.ARM7RAMAddress = header.ARM7EntryAddress = 0x02011000;
        std::vector<u8> rom(0x20000);
        std::memcpy(rom.data(), &header, sizeof(header));
        auto cart = std::make_unique<BootCart>(rom.data(), rom.size(), 0x400000C2,
            false, ROMListEntry{}, NDSCart::Default, nullptr);
        auto* observedCart = cart.get();
        dsi->SetNDSCart(std::move(cart));
        dsi->SetNAND(std::nullopt);
        dsi->Halt();
        dsi->Reset();
        dsi->SetNAND(std::move(image));
        for (unsigned i = 0; i < 0x14; ++i) dsi->ARM9Write8(0x02000600+i, 0xA5);
        for (unsigned i = 0; i < 0x18; ++i) dsi->ARM9Write8(0x02FFFD68+i, 0xA5);
        dsi->ARM9Write32(0x02FFC000, 0xC0DEFACE);
        std::array<u32, 16> before9, before7;
        std::copy_n(dsi->ARM9.R, 16, before9.begin());
        std::copy_n(dsi->ARM7.R, 16, before7.begin());
        // Exercise the actual public wrapper, including virtual preparation,
        // cartridge notification, CPU entry and success register finalization.
        const bool booted = static_cast<NDS&>(*dsi).SetupDirectBoot("synthetic.nds");
        bool pass = booted == (condition == 4);
        for (unsigned i = 0; i < 0x14; ++i)
            pass &= dsi->ARM9Read8(0x02000600+i) == (condition == 4 ? hardware[0x88+i] : 0xA5);
        for (unsigned i = 0; i < 0x18; ++i)
            pass &= dsi->ARM9Read8(0x02FFFD68+i) == (condition == 4 ? serial[0x88+i] : 0xA5);
        const bool success = condition == 4;
        pass &= dsi->ARM9Read16(0x02FFFC40) == (success ? 1 : 0);
        pass &= observedCart->bootCalls == (success ? 1u : 0u);
        pass &= !dsi->IsRunning(); // Preparation never starts execution.
        if (success)
            pass &= dsi->ARM9.R[12] == header.ARM9EntryAddress && dsi->ARM7.R[12] == header.ARM7EntryAddress;
        else
            pass &= dsi->ARM9Read32(0x02FFC000) == 0xC0DEFACE &&
                std::equal(before9.begin(), before9.end(), dsi->ARM9.R) &&
                std::equal(before7.begin(), before7.end(), dsi->ARM7.R);
        if (condition == 5) Require(dsi->GetNAND().GetFile()->injectedFailures != 0, "caller fault reached");
        std::printf("wrapper condition=%d %s; boot-indicator=%u cart-boot-calls=%u\n", condition,
            pass ? "PASS" : "FAIL", dsi->ARM9Read16(0x02FFFC40), observedCart->bootCalls);
        errors += !pass;
    }
    return errors ? 1 : 0;
}

static int Controls()
{
    for (int mode = 0; mode < 3; ++mode)
    {
        std::unique_ptr<NDS> nds = mode == 0 ? std::make_unique<NDS>(NDSArgs{}) :
            std::unique_ptr<NDS>(std::make_unique<DSi>(DSiArgs{}));
        NDSHeader header{};
        header.UnitCode = mode == 2 ? 2 : 0;
        header.ARM9ROMOffset = 0x10000;
        header.ARM7ROMOffset = 0x11000;
        header.ARM9RAMAddress = header.ARM9EntryAddress = 0x02010000;
        header.ARM7RAMAddress = header.ARM7EntryAddress = 0x02011000;
        header.ARM9Size = header.ARM7Size = 4;
        std::vector<u8> rom(0x20000);
        std::memcpy(rom.data(), &header, sizeof(header));
        const u32 loop = 0xEAFFFFFE;
        std::memcpy(rom.data() + header.ARM9ROMOffset, &loop, sizeof(loop));
        std::memcpy(rom.data() + header.ARM7ROMOffset, &loop, sizeof(loop));
        auto cart = std::make_unique<BootCart>(rom.data(), rom.size(), 0x400000C2,
            false, ROMListEntry{}, NDSCart::Default, nullptr);
        auto* observedCart = cart.get();
        nds->SetNDSCart(std::move(cart));
        nds->Halt();
        nds->Reset();
        const bool booted = nds->SetupDirectBoot("synthetic.nds");
        const bool pass = booted && observedCart->bootCalls == 1 && !nds->IsRunning() &&
            nds->ARM9.R[12] == header.ARM9EntryAddress && nds->ARM7.R[12] == header.ARM7EntryAddress &&
            nds->ARM9Read32(header.ARM9RAMAddress) == loop && nds->ARM7Read32(header.ARM7RAMAddress) == loop &&
            nds->ARM9Read16(mode == 2 ? 0x02FFFC40 : 0x027FFC40) == 1;
        std::printf("normal %s direct-boot wrapper: %s\n",
            mode == 0 ? "DS" : mode == 1 ? "DS-on-DSi" : "NAND-less DSi", pass ? "PASS" : "FAIL");
        if (!pass) return 1;
    }
    return 0;
}

int DSiBootMetadata(const char* mode)
{
    if (std::strcmp(mode, "metadata-exact") == 0) return Readers(false);
    if (std::strcmp(mode, "metadata-failures") == 0) return Readers(true);
    if (std::strcmp(mode, "metadata-caller") == 0) return Caller();
    if (std::strcmp(mode, "core-controls") == 0) return Controls();
    return 2;
}
