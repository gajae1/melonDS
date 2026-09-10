// SPDX-License-Identifier: GPL-3.0-or-later
// Current DSi_SD.cpp + real DSi_SD.h/FIFO. Only scheduler and backing I/O
// dependencies are substituted. No BIOS, NAND image, ROM, or physical device.
// Register/status contracts: https://problemkaputt.de/gbatek.htm#dsisdmmc
// SD Association Physical Layer Simplified Specification 7.10, 4.3/4.10/5.6.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>
#include "DSi_SD.h"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace melonDS;
namespace melonDS::Platform
{
struct FileHandle
{
    FILE* File = std::tmpfile();
    u64 Length = 1536;
    u64 ReadLimit = UINT64_MAX, WriteLimit = UINT64_MAX;
    bool SeekFailure = false;
    unsigned Reads = 0, Writes = 0, Seeks = 0;
    ~FileHandle() { if (File) std::fclose(File); }
};
static FileHandle* Scratch;
bool FileSeek(FileHandle* f, s64 offset, FileSeekOrigin origin)
{
    ++f->Seeks;
    if (f->SeekFailure) return false;
    return std::fseek(f->File, long(offset), origin == FileSeekOrigin::Start ? SEEK_SET : SEEK_END) == 0;
}
u64 FileRead(void* data, u64 size, u64 count, FileHandle* f)
{
    ++f->Reads;
    return std::fread(data, 1, std::min(size * count, f->ReadLimit), f->File) / size;
}
u64 FileWrite(const void* data, u64 size, u64 count, FileHandle* f)
{
    ++f->Writes;
    return std::fwrite(data, 1, std::min(size * count, f->WriteLimit), f->File) / size;
}
bool FileFlush(FileHandle* f) { return std::fflush(f->File) == 0; }
bool IsEndOfFile(FileHandle* f) { return std::feof(f->File) != 0; }
void Log(LogLevel, const char*, ...) {}
}
namespace melonDS
{
// Construction bypasses image formatting; sector access uses current production
// FATStorage definitions, including seek failure and sparse-image EOF handling.
FATStorage::FATStorage(const std::string&, u64 size, bool readonly, const std::optional<std::string>&)
    : ReadOnly(readonly), File(Platform::Scratch), FileSize(size) {}
FATStorage::FATStorage(FATStorage&& other) noexcept
    : ReadOnly(other.ReadOnly), File(other.File), FileSize(other.FileSize) {}
FATStorage& FATStorage::operator=(FATStorage&& other) noexcept
{ ReadOnly = other.ReadOnly; File = other.File; FileSize = other.FileSize; return *this; }
FATStorage::~FATStorage() = default;
using namespace Platform;
#include "DSiSDFATMethods.inc"
namespace DSi_NAND
{
NANDImage::NANDImage(Platform::FileHandle* f, const DSiKey&) noexcept : CurFile(f), eMMC_CID{}, Length(f->Length) {}
NANDImage::NANDImage(NANDImage&& other) noexcept : CurFile(other.CurFile), eMMC_CID{}, Length(other.Length) {}
NANDImage& NANDImage::operator=(NANDImage&& other) noexcept
{ CurFile = other.CurFile; Length = other.Length; return *this; }
NANDImage::~NANDImage() = default;
}
// Serialization is not exercised. Keep accidental use loud.
void Savestate::Section(const char*) { std::abort(); }
void Savestate::VarArray(void*, u32) { std::abort(); }

enum { Event_DSi_SDMMCTransfer, Event_DSi_SDIOTransfer };
enum { IRQ2_DSi_SDMMC, IRQ2_DSi_SDIO, IRQ2_DSi_SD_Data1, IRQ2_DSi_SDIO_Data1 };
#define MakeEventThunk(type, func) [](void* that, u32 param) { static_cast<type*>(that)->func(param); }
class DSi
{
public:
    using Callback = void (*)(void*, u32);
    void* Target = nullptr;
    std::vector<Callback> Callbacks;
    bool Pending = false;
    u32 Kind = 0, Parameter = 0;
    unsigned IRQs = 0, DMAs = 0;
    void RegisterEventFuncs(u32, void* target, std::initializer_list<Callback> callbacks)
    { Target = target; Callbacks = callbacks; }
    void UnregisterEventFuncs(u32) {}
    void ScheduleEvent(u32, bool, s32, u32 kind, u32 param)
    { Pending = true; Kind = kind; Parameter = param; }
    void CancelEvent(u32) { Pending = false; }
    void SetIRQ2(u32) { ++IRQs; }
    void CheckNDMAs(u32, u32) { ++DMAs; }
    u32 GetPC(u32) { return 0; }
    void Run() { if (Pending) { Pending = false; Callbacks.at(Kind)(Target, Parameter); } }
};
class DSi_NWifi final : public DSi_SDDevice
{
public:
    DSi_NWifi(DSi&, DSi_SDHost* host) : DSi_SDDevice(host) {}
    void Reset() override {}
    void DoSavestate(Savestate*) override { std::abort(); }
    void SendCMD(MMCCommand, u32) override { std::abort(); }
    void ContinueTransfer() override { std::abort(); }
};
}
#include "DSiSDSource.inc"

static void Require(bool ok, const char* message)
{ if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
struct Guarded
{
    u8* Base;
    size_t Page;
    Guarded()
    {
#ifdef _WIN32
        SYSTEM_INFO info; GetSystemInfo(&info); Page = info.dwPageSize;
        Base = static_cast<u8*>(VirtualAlloc(nullptr, 2 * Page, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        DWORD previous;
        Require(Base && VirtualProtect(Base + Page, Page, PAGE_NOACCESS, &previous), "guard allocation");
#else
        Page = sysconf(_SC_PAGESIZE);
        Base = static_cast<u8*>(mmap(nullptr, 2 * Page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
        Require(Base != MAP_FAILED && mprotect(Base + Page, Page, PROT_NONE) == 0, "guard allocation");
#endif
    }
    ~Guarded()
    {
#ifdef _WIN32
        VirtualFree(Base, 0, MEM_RELEASE);
#else
        munmap(Base, 2 * Page);
#endif
    }
    u8* End(size_t len) { return Base + Page - len; }
};
struct Fixture
{
    Platform::FileHandle File;
    DSi Dsi;
    std::unique_ptr<DSi_SDHost> Host;
    bool NAND;
    Fixture(bool nand = false, bool readonly = false) : NAND(nand)
    {
        Require(File.File != nullptr, "scratch file");
        Platform::Scratch = &File;
        for (unsigned i = 0; i < File.Length; ++i) std::fputc(u8(i * 13 + 7), File.File);
        std::fflush(File.File);
        if (nand)
            Host = std::make_unique<DSi_SDHost>(Dsi, DSi_NAND::NANDImage(&File, DSi_NAND::DSiKey{}));
        else
            Host = std::make_unique<DSi_SDHost>(Dsi, std::nullopt, FATStorage("synthetic", File.Length, readonly));
        Host->Reset();
        Host->Write(2, nand ? 1 : 0);
    }
    void Cmd(MMCCommand cmd, u32 param = 0)
    { Host->Write(4, param); Host->Write(6, param >> 16); Host->Write(0, u16(cmd)); }
    void Setup(u16 len = 512, u16 count = 1, bool wide = false)
    {
        Host->Write(0x1C, 0); Host->Write(0x1E, 0);
        Host->Write(0x20, 0x031D); // mask response; observe data/error IRQ separately
        Host->Write(0x22, 0x8B7F & ~(1 << 3));
        Host->Write(0x26, len); Host->Write(0x0A, count);
        Host->Write(0x08, 0x100);
        Host->Write(0x104, len); Host->Write(0x108, count);
        Host->Write(0xD8, wide ? 2 : 0); Host->Write(0x100, wide ? 2 : 0);
    }
    u32 Status() { return Host->Read(0x1C) | (u32(Host->Read(0x1E)) << 16); }
    void Fill(u16 len = 512, bool wide = false)
    { for (unsigned i = 0; i < len; i += wide ? 4 : 2) { if (wide) Host->WriteFIFO32(0xA6A6A6A6); else Host->WriteFIFO16(0xA6A6); } }
    std::vector<u8> Bytes()
    {
        std::vector<u8> bytes(File.Length);
        std::fflush(File.File); std::rewind(File.File);
        Require(std::fread(bytes.data(), 1, bytes.size(), File.File) == bytes.size(), "scratch readback");
        return bytes;
    }
    void Failed()
    {
        Dsi.Run();
        Require((Status() & (1 << 19)) != 0, "data error IRQ status missing");
        Require((Status() & ((1 << 2) | (1 << 24) | (1 << 25))) == 0, "failed transfer reported completion/readiness");
        Require(!Dsi.Pending && !Host->TXReq, "failed transfer still pending");
        Require(Host->ReadFIFO16() == 0 && Host->ReadFIFO32() == 0, "failed transfer exposed FIFO bytes");
        Require(Dsi.IRQs > 0, "unmasked failure did not signal IRQ");
    }
};

int main(int argc, char** argv)
{
    Require(argc == 2, "one case required");
    const std::string name = argv[1];
    if (name == "payload-scr" || name == "payload-ssr")
    {
        Fixture f; f.Setup();
        f.Cmd(MMCCommand::AppCommand);
        f.Cmd(MMCCommand(name == "payload-scr" ? 51 : 13));
        f.Failed();
        // Match the same producer size at the end of a protected page: the
        // rejection must precede *any* attempted payload extension.
        Guarded guard;
        u32 len = name == "payload-scr" ? 8 : 64;
        std::memset(guard.End(len), 0x37, len);
        f.Setup();
        Require(f.Host->DataRX(guard.End(len), len) == 0, "short producer accepted");
        f.Failed();
    }
    else if (name == "guarded-rx" || name == "guarded-tx")
    {
        Fixture f; Guarded guard; f.Setup(3);
        u8* data = guard.End(3); std::memset(data, 0xA6, 3);
        if (name == "guarded-rx")
        {
            Require(f.Host->DataRX(data, 3) == 3, "odd RX accepted"); f.Dsi.Run();
            Require(f.Host->ReadFIFO16() == 0xA6A6 && f.Host->ReadFIFO16() == 0x00A6, "odd RX padding");
        }
        else
        {
            f.Fill(3); Require(f.Host->DataTX(data, 3) == 3, "odd TX accepted");
            Require(data[0] == 0xA6 && data[2] == 0xA6, "odd TX bytes");
        }
    }
    else if (name.starts_with("normal"))
    {
        Fixture f(name.starts_with("normal-nand")); bool wide = name.ends_with("32");
        for (u16 count : {1, 2})
        {
            f.Setup(512, count, wide);
            f.Host->Write(0x20, 0x031D & ~4);
            unsigned irqs = f.Dsi.IRQs;
            f.Cmd(count == 1 ? MMCCommand::ReadSingleBlock : MMCCommand::ReadMultipleBlocks);
            for (unsigned block = 0; block < count; ++block)
            {
                Require(!(f.Status() & 4), "read completed before FIFO consumption"); f.Dsi.Run();
                for (unsigned i = 0; i < 512; i += wide ? 4 : 2)
                {
                    u32 expected = 0;
                    for (unsigned j = 0; j < (wide ? 4u : 2u); ++j) expected |= u32(u8((i+j) * 13 + 7)) << (8*j);
                    Require((wide ? f.Host->ReadFIFO32() : f.Host->ReadFIFO16()) == expected, "read data order");
                }
            }
            Require((f.Status() & 4) && !(f.Status() & (1 << 19)), "normal read completion");
            Require(f.Dsi.IRQs == irqs + 1, "single data-end IRQ for normal read");
        }
        for (u16 count : {1, 2})
        {
            f.Setup(512, count, wide);
            f.Host->Write(0x20, 0x031D & ~4);
            unsigned irqs = f.Dsi.IRQs;
            f.Cmd(count == 1 ? MMCCommand::WriteSingleBlock : MMCCommand::WriteMultipleBlocks);
            for (unsigned block = 0; block < count; ++block)
            { f.Fill(512, wide); Require(!(f.Status() & 4), "write completed before event"); f.Dsi.Run(); }
            Require((f.Status() & 4) && !(f.Status() & (1 << 19)), "normal write completion");
            Require(f.Dsi.IRQs == irqs + 1, "single data-end IRQ for normal write");
            auto bytes = f.Bytes();
            Require(std::all_of(bytes.begin(), bytes.begin() + count * 512, [](u8 b) { return b == 0xA6; }), "write readback");
        }
        for (u16 len : {8, 64})
        {
            f.Setup(len, 1, wide); f.Cmd(MMCCommand::AppCommand); f.Cmd(MMCCommand(len == 8 ? 51 : 13)); f.Dsi.Run();
            for (unsigned i = 0; i < len; i += wide ? 4 : 2) { if (wide) f.Host->ReadFIFO32(); else f.Host->ReadFIFO16(); }
            Require((f.Status() & 4) && !(f.Status() & (1 << 19)), "normal SCR/SSR completion");
        }
    }
    else if (name == "partial")
    {
        for (bool nand : {false, true}) for (bool wide : {false, true}) for (u16 len : {3, 8, 64})
        {
            Fixture f(nand); auto before = f.Bytes();
            u32 addr = 512 - len;
            f.Cmd(MMCCommand::SetBlockLength, len); f.Setup(len, 1, wide);
            f.Cmd(MMCCommand::ReadSingleBlock, addr); f.Dsi.Run();
            for (unsigned i = 0; i < len; i += wide ? 4 : 2)
            {
                u32 expected = 0;
                for (unsigned j = 0; j < (wide ? 4u : 2u) && i+j < len; ++j)
                    expected |= u32(before[addr+i+j]) << (8*j);
                Require((wide ? f.Host->ReadFIFO32() : f.Host->ReadFIFO16()) == expected, "partial/odd RX padding");
            }
            Require((f.Status() & 4) && !(f.Status() & (1 << 19)), "partial read completion");
            f.Setup(len, 1, wide); f.Cmd(MMCCommand::WriteSingleBlock, addr); f.Fill(len, wide); f.Dsi.Run();
            Require((f.Status() & 4) && !(f.Status() & (1 << 19)), "partial write completion");
            auto expected = before;
            std::fill(expected.begin()+addr, expected.begin()+addr+len, 0xA6);
            Require(f.Bytes() == expected, "partial write changed neighboring bytes");
        }
    }
    else if (name.starts_with("multiblock"))
    {
        for (bool nand : {false, true}) for (bool wide : {false, true})
        {
            Fixture f(nand); f.Setup(512, 2, wide);
            bool write = name == "multiblock-write";
            f.Cmd(write ? MMCCommand::WriteMultipleBlocks : MMCCommand::ReadMultipleBlocks);
            if (write)
            {
                f.Fill(512, wide); f.Dsi.Run();
                f.File.WriteLimit = 7; f.Fill(512, wide);
            }
            else
            {
                f.Dsi.Run(); f.File.ReadLimit = 7;
                for (unsigned i = 0; i < 512; i += wide ? 4 : 2)
                { if (wide) f.Host->ReadFIFO32(); else f.Host->ReadFIFO16(); }
            }
            f.Failed();
            // Acknowledge + CMD12 + a new command is an explicit retry.
            f.Cmd(MMCCommand::StopTransmission);
            u32 response = f.Host->Read(0x0C) | (u32(f.Host->Read(0x0E)) << 16);
            Require(response & (1 << 19), "card error missing in next R1 response");
            f.Cmd(MMCCommand::GetCSR);
            response = f.Host->Read(0x0C) | (u32(f.Host->Read(0x0E)) << 16);
            Require(!(response & (1 << 19)) && ((response >> 9) & 15) == 4, "reported card error not cleared/in TRAN");
            f.File.WriteLimit = f.File.ReadLimit = UINT64_MAX;
            f.Setup(512, 1, wide); f.Cmd(MMCCommand::WriteSingleBlock, 512); f.Fill(512, wide); f.Dsi.Run();
            Require((f.Status() & 4) && !(f.Status() & (1 << 19)), "CMD12 recovery");
        }
    }
    else if (name == "error-irq")
    {
        Fixture f; f.Setup(); f.Host->Write(0x22, 0x8B7F); f.File.ReadLimit = 7;
        unsigned irqs = f.Dsi.IRQs;
        f.Cmd(MMCCommand::ReadSingleBlock);
        Require((f.Status() & (1 << 19)) && f.Dsi.IRQs == irqs, "masked error IRQ");
        f.Host->Write(0x22, 0x8B7F & ~(1 << 3));
        Require(f.Dsi.IRQs == irqs+1, "unmask pending error IRQ");
        f.Host->Write(0x1E, u16(~(1 << 3)));
        Require(!(f.Status() & (1 << 19)), "ack error IRQ");
    }
    else if (name == "invalid-length")
    {
        Fixture f;
        for (u32 len : {0u, 513u})
        {
            f.Cmd(MMCCommand::SetBlockLength, len);
            Require(f.Host->Read(0x0E) & (1 << 13), "CMD16 length error missing");
        }
        f.Setup(0); f.Cmd(MMCCommand::ReadSingleBlock); f.Failed();
        Require(f.File.Reads == 0, "zero host block touched backing data");
    }
    else
    {
        bool nand = name.starts_with("nand-");
        Fixture f(nand, name == "readonly");
        auto before = f.Bytes();
        bool write = name.find("write") != std::string::npos || name == "sd-rmw-read" || name == "readonly" || name == "recovery";
        u16 len = name == "sd-rmw-read" ? 64 : 512;
        f.Setup(len);
        if (name.find("short-read") != std::string::npos || name == "sd-rmw-read") f.File.ReadLimit = 7;
        if (name.find("short-write") != std::string::npos || name == "recovery") f.File.WriteLimit = 7;
        if (name.find("seek") != std::string::npos) f.File.SeekFailure = true;
        u32 addr = name.starts_with("sector-") ? 511 : name.ends_with("-end") ? f.File.Length : 0;
        if (write) f.Fill(len);
        f.Cmd(write ? MMCCommand::WriteSingleBlock : MMCCommand::ReadSingleBlock, addr);
        f.Failed();
        if (name.find("short-write") != std::string::npos)
        {
            auto expected = before; std::fill_n(expected.begin(), 7, 0xA6);
            Require(f.Bytes() == expected, "short write changed bytes outside reported prefix");
        }
        if (!(name.find("short-write") != std::string::npos || name == "recovery"))
            Require(before == f.Bytes(), "rejected transfer modified backing storage");
        if (name.find("seek") != std::string::npos)
            Require(f.File.Reads == 0 && f.File.Writes == 0, "I/O continued after seek failure");
        if (name == "recovery")
        {
            unsigned writes = f.File.Writes;
            f.Host->WriteFIFO16(0x5555); f.Dsi.Run();
            Require(f.File.Writes == writes && !(f.Status() & 4), "failure retried implicitly");
            f.Cmd(MMCCommand::StopTransmission); f.Host->Write(0xE0, 0); f.Host->Write(0xE0, 1);
            f.File.WriteLimit = UINT64_MAX; f.Setup();
            f.Cmd(MMCCommand::WriteSingleBlock); f.Fill(); f.Dsi.Run();
            Require((f.Status() & 4) && !(f.Status() & (1 << 19)), "explicit recovery failed");
            auto after = f.Bytes();
            Require(std::all_of(after.begin(), after.begin()+512, [](u8 b) { return b == 0xA6; }), "recovery used stale FIFO bytes");
        }
    }
    std::printf("PASS: %s\n", name.c_str());
}
