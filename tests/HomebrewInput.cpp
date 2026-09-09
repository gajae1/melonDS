// SPDX-License-Identifier: GPL-3.0-or-later
// Execute current production methods extracted by ExtractFunction.py. Only the
// SD injection and guest-memory boundaries are replaced; no host files are used.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "NDS_Header.h"
#include "Platform.h"
#include "NDSCart/melonDLDI.h"
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace melonDS;
using Bytes = std::vector<u8>;
namespace melonDS::Platform { void Log(LogLevel, const char*, ...) {} }

namespace melonDS
{
class NDS
{
public:
    std::vector<std::pair<u32, u32>> writes;
    void ARM9Write32(u32 address, u32 value) { writes.emplace_back(address, value); }
};
namespace NDSCart
{
using Platform::Log;
using Platform::LogLevel;
struct RecordingSD
{
    int injections = 0;
    bool succeed = true;
    std::string path;
    Bytes content;
    bool InjectFile(const std::string& name, u8* data, u32 length)
    {
        ++injections;
        path = name;
        content.assign(data, data + length);
        return succeed;
    }
};
struct CartCommon
{
    int directBoots = 0;
    void SetupDirectBoot(const std::string&, NDS&) { ++directBoots; }
};
struct CartSD : CartCommon
{
    // Non-owning fixture storage permits an exact-size guarded production span.
    struct ROMView
    {
        u8* bytes = nullptr;
        u8* get() const { return bytes; }
        u8& operator[](size_t i) const { return bytes[i]; }
    } ROM;
    u32 ROMLength = 0;
    NDSHeader header{};
    const NDSHeader& GetHeader() const { return header; }
    std::optional<RecordingSD> SD;
    bool ApplyDLDIPatchAt(u8* binary, u32 binarylen, u32 dldioffset, const u8* patch, u32 patchlen, bool readonly) const;
    void ApplyDLDIPatch(const u8* patch, u32 patchlen, bool readonly);
};
struct CartHomebrew : CartSD
{
    void SetupDirectBoot(const std::string& romname, NDS& nds);
};
#include "HomebrewSetupDirectBoot.inc"
#include "HomebrewApplyDLDIPatchAt.inc"
#include "HomebrewApplyDLDIPatch.inc"
}
}

static int failures = 0;
static void Check(bool condition, const char* message)
{
    if (!condition) { ++failures; std::fprintf(stderr, "%s\n", message); }
}

static void Argv(const std::string& name, bool accepted, bool inject = true, bool sd = true,
                 u32 ram = 0x02000000, u32 arm9Size = 0x101)
{
    Bytes rom(0x200, 0x5A);
    NDSCart::CartHomebrew cart;
    cart.ROM.bytes = rom.data();
    cart.ROMLength = static_cast<u32>(rom.size());
    cart.header.ARM9RAMAddress = ram;
    cart.header.ARM9Size = arm9Size;
    if (sd) { cart.SD.emplace(); cart.SD->succeed = inject; }
    NDS nds;
    cart.SetupDirectBoot(name, nds);
    Check(cart.directBoots == 1, "Direct boot did not initialize the base cartridge");
    if (!accepted || !sd)
    {
        Check(nds.writes.empty(), "Rejected argv changed guest memory");
        Check(!cart.SD || cart.SD->injections == 0, "Rejected argv reached SD injection");
        return;
    }
    Check(cart.SD->injections == 1 && cart.SD->path == name && cart.SD->content == rom,
          "Valid argv did not inject the exact ROM and unmodified name");
    if (!inject) { Check(nds.writes.empty(), "Failed SD injection published argv"); return; }

    Bytes expected(name.size() + 6, 0);
    std::copy_n("fat:/", 5, expected.begin());
    std::copy(name.begin(), name.end(), expected.begin() + 5);
    const u32 length = static_cast<u32>(expected.size());
    expected.resize((expected.size() + 3) & ~size_t{3}, 0);
    Check(expected.size() <= 512, "Invalid valid-case fixture");
    Check(nds.writes.size() == expected.size() / 4 + 3, "Unexpected argv write span");
    if (nds.writes.size() != expected.size() / 4 + 3) return;
    const u32 base = 0x02000110;
    for (size_t i = 0; i < expected.size(); i += 4)
    {
        const auto [address, value] = nds.writes[i / 4];
        Check(address == base + i, "argv payload address or alignment changed");
        for (int byte = 0; byte < 4; ++byte)
            Check(u8(value >> (byte * 8)) == expected[i + byte], "argv bytes, NUL or padding changed");
    }
    const size_t meta = expected.size() / 4;
    Check(nds.writes[meta] == std::pair<u32,u32>{0x02FFFE70, 0x5F617267} &&
          nds.writes[meta + 1] == std::pair<u32,u32>{0x02FFFE74, base} &&
          nds.writes[meta + 2] == std::pair<u32,u32>{0x02FFFE78, length},
          "argv descriptor does not reference the exact terminated command line");
}

struct GuardedBytes
{
    void* allocation = nullptr;
    size_t allocationSize = 0;
    u8* bytes = nullptr;
    explicit GuardedBytes(const Bytes& input)
    {
#ifdef _WIN32
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        const size_t page = info.dwPageSize;
#else
        const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
#endif
        const size_t readable = ((input.size() + page - 1) / page + 1) * page;
        allocationSize = readable + page;
#ifdef _WIN32
        allocation = VirtualAlloc(nullptr, allocationSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        DWORD previous;
        if (!allocation || !VirtualProtect(static_cast<u8*>(allocation) + readable, page, PAGE_NOACCESS, &previous))
            std::abort();
#else
        allocation = mmap(nullptr, allocationSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (allocation == MAP_FAILED || mprotect(static_cast<u8*>(allocation) + readable, page, PROT_NONE))
            std::abort();
#endif
        bytes = static_cast<u8*>(allocation) + readable - input.size();
        if (!input.empty()) std::memcpy(bytes, input.data(), input.size());
    }
    ~GuardedBytes()
    {
#ifdef _WIN32
        VirtualFree(allocation, 0, MEM_RELEASE);
#else
        munmap(allocation, allocationSize);
#endif
    }
};

static u32 Word(const u8* data, size_t offset)
{
    return u32(data[offset]) | (u32(data[offset + 1]) << 8) |
           (u32(data[offset + 2]) << 16) | (u32(data[offset + 3]) << 24);
}
static void Put(Bytes& data, size_t offset, u32 value)
{
    for (int i = 0; i < 4; ++i) data[offset + i] = u8(value >> (i * 8));
}
static Bytes Patch() { return Bytes(std::begin(melonDLDI), std::end(melonDLDI)); }

static Bytes ROM(size_t arm9Size = 0x500, size_t dldiOffset = 0x20, u32 memory = 0x02000200)
{
    Bytes rom(0x200 + arm9Size, 0xCC);
    Put(rom, 0x20, 0x200);
    Put(rom, 0x2C, static_cast<u32>(arm9Size));
    const size_t start = 0x200 + dldiOffset;
    std::fill_n(rom.begin() + start, 0x400, 0xA5);
    const Bytes patch = Patch();
    std::copy_n(patch.begin(), 0x80, rom.begin() + start);
    rom[start + 0x0F] = 10;
    Put(rom, start + 0x40, memory);
    Put(rom, start + 0x68, memory + 0x80);
    return rom;
}

static Bytes Apply(const Bytes& rom, const Bytes& patch, bool readonly = false, bool exact = false)
{
    // Separate the scan-tail crash gate from assertions about partial writes.
    // The cart always receives the original logical length, including when
    // readable canary bytes follow it. Source bytes always end at a guard page.
    Bytes storage = rom;
    if (!exact) storage.resize(storage.size() + 16, 0xCC);
    GuardedBytes target(storage), source(patch);
    NDSCart::CartSD cart;
    cart.ROM.bytes = target.bytes;
    cart.ROMLength = static_cast<u32>(rom.size());
    cart.ApplyDLDIPatch(source.bytes, static_cast<u32>(patch.size()), readonly);
    Check(std::equal(storage.begin() + rom.size(), storage.end(), target.bytes + rom.size()),
          "Patch wrote beyond the actual ROM length into the canary");
    return Bytes(target.bytes, target.bytes + rom.size());
}
static void Reject(const Bytes& rom, const Bytes& patch, bool readonly = false, bool exact = false)
{
    Check(Apply(rom, patch, readonly, exact) == rom, "Invalid DLDI input partially changed the ROM");
}

static Bytes FixPatch()
{
    Bytes patch = Patch();
    patch.erase(patch.begin() + 0x200, patch.end());
    patch[0x0E] = 0x0F;
    Put(patch, 0x40, 0x02300000);
    Put(patch, 0x44, 0x02300180);
    Put(patch, 0x48, 0x02300100); Put(patch, 0x4C, 0x02300110);
    Put(patch, 0x50, 0x02300110); Put(patch, 0x54, 0x02300120);
    Put(patch, 0x58, 0x02300200); Put(patch, 0x5C, 0x02300220);
    for (size_t i = 0x68; i <= 0x7C; i += 4) Put(patch, i, 0x02300080 + (i - 0x68) * 2);
    Put(patch, 0x100, 0x02300084); Put(patch, 0x110, 0x02300080);
    Put(patch, 0x120, 0x08000000); Put(patch, 0x124, 0); Put(patch, 0x128, 0x02300400);
    return patch;
}

static void DLDI(const std::string& test)
{
    constexpr size_t at = 0x220;
    if (test == "dldi-control")
    {
        for (bool ro : {false, true}) for (bool fallback : {false, true})
        {
            Bytes rom = ROM(0x500, 0x21); // The ROM signature need not be host-aligned.
            const size_t pos = at + 1;
            if (fallback) Put(rom, pos + 0x40, 0);
            const Bytes result = Apply(rom, Patch(), ro);
            Check(Word(result.data(), pos + 0x40) == 0x02000200 &&
                  Word(result.data(), pos + 0x44) == 0x02000424 &&
                  Word(result.data(), pos + 0x74) == 0x020003E0, "melonDLDI relocation changed");
            Check(result[pos + 0x64] == (ro ? 0x21 : 0x23), "Read-only feature flag changed");
            Check(Word(result.data(), pos + 0x1E0) == (ro ? 0xE3A00000 : 0xE92D4078) &&
                  Word(result.data(), pos + 0x1E4) == (ro ? 0xE12FFF1E : 0xE1A04000),
                  "Write-sector code or read-only failure thunk changed");
            Check(std::equal(rom.begin(), rom.begin() + pos, result.begin()) &&
                  std::equal(rom.begin() + pos + sizeof(melonDLDI), rom.end(), result.begin() + pos + sizeof(melonDLDI)),
                  "DLDI patch changed bytes outside its actual file length");
        }
    }
    else if (test == "dldi-repatch")
    {
        const Bytes once = Apply(ROM(), Patch());
        Check(once[at + 0x0F] == 10, "Patching lost the target's allocated-space header");
        const Bytes twice = Apply(once, Patch(), true);
        Check(twice[at + 0x64] == 0x21 && Word(twice.data(), at + 0x1E0) == 0xE3A00000,
              "Read-only reset could not repatch the existing melonDLDI image");
    }
    else if (test == "dldi-patch-guard")
    {
        for (size_t size : {0U, 11U, 15U, 127U})
        { Bytes patch = Patch(); patch.resize(size); Reject(ROM(), patch); }
    }
    else if (test == "dldi-rom-guard")
    {
        for (size_t size : {0U, 0x20U, 0x2FU})
        { Bytes rom = ROM(); rom.resize(size); Reject(rom, Patch(), false, true); }
    }
    else if (test == "dldi-arm9-span")
    {
        for (const auto [offset, length] : std::array<std::pair<u32,u32>, 5>{
             {{0x700, 1}, {0x600, 0x101}, {0xFFFFFF00, 0x500}, {0x1FF, 0x500}, {0x200, 0xFFFFFFFF}}})
        {
            Bytes rom = ROM(); Put(rom, 0x20, offset); Put(rom, 0x2C, length); Reject(rom, Patch());
        }
    }
    else if (test == "dldi-scan-tail")
    {
        for (size_t size : {1U, 4U, 11U, 0x80U})
        {
            Bytes rom(0x200 + size, 0);
            Put(rom, 0x20, 0x200); Put(rom, 0x2C, static_cast<u32>(size));
            if (size < 12) std::copy_n(std::begin(melonDLDI), size, rom.begin() + 0x200);
            Reject(rom, Patch(), false, true);
        }
    }
    else if (test == "dldi-target-header")
    {
        for (size_t size : {12U, 16U, 127U})
        {
            Bytes rom = ROM(); rom.resize(at + size); Put(rom, 0x2C, 0x20 + static_cast<u32>(size));
            Reject(rom, Patch());
        }
    }
    else if (test == "dldi-source-header")
    {
        for (int mutation = 0; mutation < 8; ++mutation)
        {
            Bytes patch = Patch();
            if (mutation == 0) patch[7] ^= 1;
            if (mutation == 1) patch[0x0C] = 2;
            if (mutation == 2) patch[0x0D] = 6;
            if (mutation == 3) patch[0x0D] = 32;
            if (mutation == 4) patch[0x0E] = 0x10;
            if (mutation == 5) patch.resize(0x200); // data_end still needs 0x224 bytes.
            if (mutation == 6) Put(patch, 0x68, 0xBF800500);
            if (mutation == 7) patch.resize(0x401, 0xDD); // Actual bytes exceed driverSize.
            Reject(ROM(), patch);
        }
    }
    else if (test == "dldi-target-layout")
    {
        for (int mutation = 0; mutation < 7; ++mutation)
        {
            Bytes rom = ROM();
            if (mutation == 0) rom[at + 0x0C] = 2;
            if (mutation == 1) rom[at + 0x0F] = 32;
            if (mutation == 2) rom[at + 0x0F] = 9;
            if (mutation == 3) rom[at + 0x0F] = 11; // Header capacity is outside ARM9.
            if (mutation == 4) { Put(rom, at + 0x40, 0); Put(rom, at + 0x68, 0x40); }
            if (mutation == 5) Put(rom, at + 0x40, 0xFFFFFF00);
            if (mutation == 6) { Put(rom, 0x2C, 0x200); } // Physical ROM tail is not ARM9 space.
            Reject(rom, Patch());
        }
    }
    else if (test == "dldi-fix-control" || test == "dldi-fix-overlap")
    {
        const u32 base = test == "dldi-fix-overlap" ? 0x02300100 : 0x02000200;
        const Bytes rom = ROM(0x500, 0x20, base);
        const Bytes result = Apply(rom, FixPatch());
        Check(Word(result.data(), at + 0x40) == base && Word(result.data(), at + 0x68) == base + 0x80 &&
              Word(result.data(), at + 0x100) == base + 0x84 && Word(result.data(), at + 0x110) == base + 0x80,
              "Overlapping relocation ranges or addresses relocated a pointer more than once");
        Check(Word(result.data(), at + 0x120) == 0x08000000 && Word(result.data(), at + 0x124) == 0 &&
              Word(result.data(), at + 0x128) == 0x02300400, "Relocation changed a non-driver pointer");
        Check(std::all_of(result.begin() + at + 0x200, result.begin() + at + 0x220, [](u8 b) { return b == 0; }),
              "Validated BSS beyond patch file length was not zeroed");
        Check(result[at + 0x220] == 0xA5, "BSS clearing escaped its declared range");
    }
    else if (test == "dldi-fix-range")
    {
        for (const auto [field, value] : std::array<std::pair<size_t,u32>, 8>{
             {{0x44, 0x02300204}, {0x48, 0x022FFFFC}, {0x4C, 0x023000FC}, {0x54, 0x02300113},
              {0x58, 0x022FFFFC}, {0x5C, 0x023001FC}, {0x5C, 0x02300404}, {0x58, 0x02300078}}})
        { Bytes patch = FixPatch(); Put(patch, field, value); Reject(ROM(), patch); }
    }
    else if (test == "dldi-readonly-range")
    {
        for (u32 address : {0xBF7FFFFCU, 0xBF800078U, 0xBF800220U, 0xBF800225U, 0xBF8001E1U})
        { Bytes patch = Patch(); Put(patch, 0x74, address); Reject(ROM(), patch, true); }
    }
    else if (test == "dldi-address-overflow")
    {
        Bytes patch = Patch(); Put(patch, 0x40, 0xFFFFFE00); Reject(ROM(), patch);
    }
    else { std::fprintf(stderr, "Unknown DLDI case\n"); ++failures; }
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    const std::string test = argv[1];
    if (test == "argv-control")
    {
        for (const std::string& name : {std::string("demo.nds"), std::string(506, 'a'),
                                       std::string("dir/\xED\x99\x88\xEB\xB8\x8C\xEB\xA5\x98.nds")})
            Argv(name, true);
        Argv("demo.nds", true, false);
        Argv("demo.nds", false, true, false);
    }
    else if (test == "argv-oversize")
    {
        Argv(std::string(507, 'a'), false);
        Argv(std::string(1024, 'b'), false);
    }
    else if (test == "argv-nul") Argv(std::string("good.nds\0other.nds", 18), false);
    else if (test == "argv-address")
    {
        Argv("demo.nds", false, true, true, 0xFFFFFF00, 0x200); // ARM9 end wraps.
        Argv("demo.nds", false, true, true, 0xFFFFFE00, 0x1F1); // Alignment wraps.
        Argv(std::string(506, 'a'), false, true, true, 0xFFFFFD00, 0x101); // argv end wraps.
    }
    else if (test.starts_with("dldi-")) DLDI(test);
    else return 2;
    std::printf("%s: %s\n", argv[1], failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
