// SPDX-License-Identifier: GPL-3.0-or-later
// Compile the real backend below. Only the dynamic-library/pcap and adapter
// enumeration boundaries are fake: no library, adapter or socket is opened.
// Packet data ends at a real inaccessible page. The callback records an invalid
// advertised length without copying it, so the baseline produces a useful RED.
// Contracts: https://npcap.com/guide/wpcap/pcap_loop.html
// https://npcap.com/guide/wpcap/pcap_datalink.html
// https://npcap.com/guide/wpcap/pcap_inject.html
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "net/Net_PCap.h"
#ifdef __WIN32__
#include <iphlpapi.h>
#else
#include <ifaddrs.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace melonDS;

static int failures = 0;
static void Check(bool ok, const char* message)
{
    if (!ok) { ++failures; std::fprintf(stderr, "%s\n", message); }
}
static void Require(bool ok, const char* message)
{
    if (!ok) throw std::runtime_error(message);
}

namespace melonDS::Platform
{
struct DynamicLibrary { int unloads = 0; bool live = true; };
}
struct pcap { Platform::DynamicLibrary* owner; bool live = true; };

static std::vector<std::unique_ptr<Platform::DynamicLibrary>> libraries;
static std::vector<std::unique_ptr<pcap_t>> handles;
static const char* missingSymbol = nullptr;
static bool failLoad = false, failOpen = false, failNonblock = false;
static int linkType = DLT_EN10MB, sendResult = 0, dispatchResult = 0;
static int opens = 0, closes = 0, sends = 0, dispatches = 0, errors = 0;
static std::string openedName;
static std::vector<u8> sent;
static pcap_pkthdr packetHeader{};
static const u8* packetData = nullptr;
static int findResult = 0, freedLists = 0, enumerationCalls = 0;
static bool emptyList = false, failEnumeration = false;
static char deviceName[] = "\\Device\\NPF_synthetic";
static pcap_if_t device{};

static void CheckHandle(pcap_t* handle)
{
    Check(handle && handle->live && handle->owner->live,
          "pcap handle was used after close or library unload");
}
static int FindDevices(pcap_if_t** result, char* error)
{
    if (findResult < 0) std::strcpy(error, "synthetic enumeration error");
    device.name = deviceName;
    device.flags = PCAP_IF_LOOPBACK;
    *result = findResult < 0 || emptyList ? nullptr : &device;
    return findResult;
}
static void FreeDevices(pcap_if_t* list)
{
    Check(list == &device, "Unexpected device list released");
    ++freedLists;
}
static pcap_t* OpenLive(const char* name, int snaplen, int promisc, int timeout, char* error)
{
    ++opens;
    openedName = name;
    Check(snaplen == 2048 && promisc != 0 && timeout == 1,
          "Existing capture size/mode/timeout policy changed");
    if (failOpen) { std::strcpy(error, "synthetic open error"); return nullptr; }
    const auto owner = std::find_if(libraries.rbegin(), libraries.rend(),
                                  [](const auto& lib) { return lib->live; });
    Require(owner != libraries.rend(), "Fixture has no loaded library");
    handles.push_back(std::make_unique<pcap_t>(pcap_t{owner->get()}));
    return handles.back().get();
}
static void Close(pcap_t* handle)
{
    CheckHandle(handle);
    handle->live = false;
    ++closes;
}
static int Nonblock(pcap_t* handle, int enabled, char* error)
{
    CheckHandle(handle);
    Check(enabled == 1, "Capture did not request nonblocking mode");
    if (failNonblock) std::strcpy(error, "synthetic nonblock error");
    return failNonblock ? PCAP_ERROR : 0;
}
static int Datalink(pcap_t* handle) { CheckHandle(handle); return linkType; }
static int Send(pcap_t* handle, const u_char* data, int len)
{
    CheckHandle(handle);
    ++sends;
    if (len >= 0 && len <= 2048) sent.assign(data, data + len);
    return sendResult;
}
static int Dispatch(pcap_t* handle, int count, pcap_handler callback, u_char* userdata)
{
    CheckHandle(handle);
    ++dispatches;
    Check(count == 1, "Existing one-packet polling policy changed");
    if (dispatchResult > 0) callback(userdata, &packetHeader, packetData);
    return dispatchResult;
}
static const u_char* Next(pcap_t*, pcap_pkthdr*) { return nullptr; }

namespace melonDS::Platform
{
DynamicLibrary* DynamicLibrary_Load(const char*)
{
    if (failLoad) return nullptr;
    libraries.push_back(std::make_unique<DynamicLibrary>());
    return libraries.back().get();
}
void DynamicLibrary_Unload(DynamicLibrary* lib)
{
    ++lib->unloads;
    lib->live = false; // Keep the observer alive, including on a double unload.
    for (const auto& handle : handles)
        Check(handle->owner != lib || !handle->live, "Library unloaded before its capture handle closed");
}
void* DynamicLibrary_LoadFunction(DynamicLibrary* lib, const char* name)
{
    Check(lib->live, "Symbol lookup used an unloaded library");
    if (missingSymbol && std::strcmp(missingSymbol, name) == 0) return nullptr;
#define SYMBOL(symbol, function) if (std::strcmp(name, symbol) == 0) return reinterpret_cast<void*>(&function)
    SYMBOL("pcap_findalldevs", FindDevices);
    SYMBOL("pcap_freealldevs", FreeDevices);
    SYMBOL("pcap_open_live", OpenLive);
    SYMBOL("pcap_close", Close);
    SYMBOL("pcap_setnonblock", Nonblock);
    SYMBOL("pcap_datalink", Datalink);
    SYMBOL("pcap_sendpacket", Send);
    SYMBOL("pcap_dispatch", Dispatch);
    SYMBOL("pcap_next", Next);
#undef SYMBOL
    return nullptr;
}
void Log(LogLevel level, const char*, ...)
{
    if (level == LogLevel::Error) ++errors;
}
}

#ifdef __WIN32__
static std::vector<void*> adapterBuffers;
static void* AdapterAlloc(HANDLE, DWORD, SIZE_T size)
{
    void* buffer = std::malloc(size);
    Require(buffer != nullptr, "Fixture adapter buffer allocation failed");
    adapterBuffers.push_back(buffer);
    return buffer;
}
static BOOL AdapterFree(HANDLE, DWORD, void* buffer)
{
    const auto found = std::find(adapterBuffers.begin(), adapterBuffers.end(), buffer);
    Check(found != adapterBuffers.end(), "Adapter buffer freed twice");
    if (found == adapterBuffers.end()) return FALSE;
    adapterBuffers.erase(found);
    std::free(buffer);
    return TRUE;
}
static ULONG AdapterAddresses(ULONG, ULONG, PVOID, PIP_ADAPTER_ADDRESSES buffer, PULONG size)
{
    ++enumerationCalls;
    // Exercise the existing allocation retry before either success or failure.
    if (*size < 32768) { *size = 32768; return ERROR_BUFFER_OVERFLOW; }
    if (failEnumeration) return ERROR_NO_DATA;
    Require(buffer != nullptr, "Fixture received no adapter buffer");
    *buffer = {};
    static char name[] = "synthetic";
    static wchar_t friendly[128] = L"Synthetic adapter";
    static wchar_t description[128] = L"No hardware enumeration";
    buffer->AdapterName = name;
    buffer->FriendlyName = friendly;
    buffer->Description = description;
    buffer->PhysicalAddressLength = 6;
    return ERROR_SUCCESS;
}
#define GetAdaptersAddresses AdapterAddresses
#define HeapAlloc AdapterAlloc
#define HeapFree AdapterFree
#else
static int AdapterAddresses(ifaddrs** result)
{
    ++enumerationCalls;
    *result = nullptr;
    return failEnumeration ? -1 : 0;
}
static void AdapterFree(ifaddrs*) {}
#define getifaddrs AdapterAddresses
#define freeifaddrs AdapterFree
#endif

// No copied method bodies or access-control changes; all public entry points,
// moves, private receive callback and cleanup paths are the production source.
#include "../src/net/Net_PCap.cpp"

#ifdef __WIN32__
#undef GetAdaptersAddresses
#undef HeapAlloc
#undef HeapFree
#else
#undef getifaddrs
#undef freeifaddrs
#endif

class GuardedPacket
{
    void* allocation;
    size_t allocationSize;
public:
    u8* data;
    explicit GuardedPacket(size_t length)
    {
#ifdef __WIN32__
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        const size_t page = info.dwPageSize;
#else
        const size_t page = sysconf(_SC_PAGESIZE);
#endif
        const size_t dataSize = std::max(size_t{1}, (length + page - 1) / page) * page;
        allocationSize = dataSize + page;
#ifdef __WIN32__
        allocation = VirtualAlloc(nullptr, allocationSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        Require(allocation != nullptr, "Fixture guarded allocation failed");
        DWORD oldProtection;
        Require(VirtualProtect(static_cast<u8*>(allocation) + dataSize, page, PAGE_NOACCESS, &oldProtection),
                "Fixture guard protection failed");
#else
        allocation = mmap(nullptr, allocationSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Require(allocation != MAP_FAILED, "Fixture guarded allocation failed");
        Require(mprotect(static_cast<u8*>(allocation) + dataSize, page, PROT_NONE) == 0,
                "Fixture guard protection failed");
#endif
        data = static_cast<u8*>(allocation) + dataSize - length;
        for (size_t i = 0; i < length; ++i) data[i] = static_cast<u8>(i * 17 + 3);
        if (length >= 14) { data[12] = 0x08; data[13] = 0x00; }
    }
    ~GuardedPacket()
    {
#ifdef __WIN32__
        VirtualFree(allocation, 0, MEM_RELEASE);
#else
        munmap(allocation, allocationSize);
#endif
    }
};

static auto Load()
{
    auto library = LibPCap::New();
    Require(library && library->IsValid(), "Fixture could not load complete fake library");
    return library;
}

static void Controls()
{
    auto library = Load();
    std::vector<u8> received;
    auto driver = library->Open("synthetic", [&](const u8* data, int len) { received.assign(data, data + len); });
    Require(driver != nullptr, "Fixture could not open Ethernet backend");
    for (unsigned length : {14u, 60u, 2048u})
    {
        GuardedPacket packet(length);
        packetHeader.caplen = packetHeader.len = length;
        packetData = packet.data;
        dispatchResult = 1;
        driver->RecvCheck();
        Check(received == std::vector<u8>(packet.data, packet.data + length), "Complete Ethernet input was changed");
        Check(driver->SendPacket(packet.data, length) == static_cast<int>(length), "Normal send did not report its length");
        Check(sent == received, "Normal send changed the frame bytes");
    }
    Check(errors == 0, "Successful traffic was reported as an error");
}

static void MissingSymbols()
{
    for (const char* symbol : {"pcap_findalldevs", "pcap_next", "pcap_datalink"})
    {
        missingSymbol = symbol;
        Check(!LibPCap::New(), "Library missing a required symbol was accepted");
    }
    missingSymbol = nullptr;
    failLoad = true;
    Check(!LibPCap::New(), "Library load failure was accepted");
    failLoad = false;
    auto recovered = Load();
}

static void CaptureBounds()
{
    auto library = Load();
    int callbacks = 0;
    auto driver = library->Open("synthetic", [&](const u8*, int len) {
        ++callbacks;
        Check(len >= 0 && static_cast<unsigned>(len) <= packetHeader.caplen,
              "Callback advertised bytes beyond the guarded capture buffer");
    });
    Require(driver != nullptr, "Fixture could not open Ethernet backend");
    for (const auto [captured, original] : {std::pair{0u, 0u}, {13u, 13u}, {40u, 60u},
             {60u, 40u}, {2048u, 9000u}, {9000u, 9000u}, {60u, 0xFFFFFFFFu}})
    {
        GuardedPacket packet(captured);
        packetHeader.caplen = captured;
        packetHeader.len = original;
        packetData = packet.data;
        dispatchResult = 1;
        const int before = callbacks;
        driver->RecvCheck();
        if (callbacks != before)
            std::fprintf(stderr, "Rejected-frame trigger: caplen=%u len=%u\n", captured, original);
        Check(callbacks == before, "Incomplete, invalid or unsupported-size frame reached the guest callback");
    }
}

static void LinkType()
{
    auto library = Load();
    for (int type : {DLT_RAW, PCAP_ERROR_NOT_ACTIVATED})
    {
        linkType = type;
        const int beforeClose = closes, beforeError = errors;
        auto driver = library->Open("synthetic", {});
        Check(!driver, "Non-Ethernet or failed datalink query was accepted");
        Check(closes == beforeClose + 1, "Rejected datalink leaked its open capture handle");
        Check(errors > beforeError, "Rejected datalink was not diagnosed");
    }
}

static void IOErrors()
{
    auto library = Load();
    auto driver = library->Open("synthetic", {});
    Require(driver != nullptr, "Fixture could not open Ethernet backend");
    GuardedPacket packet(2048);
    for (int length : {-1, 0, 13, 2049})
    {
        const int before = sends;
        Check(driver->SendPacket(packet.data, length) == 0, "Invalid send length was reported as successful");
        Check(sends == before, "Invalid Ethernet send reached pcap");
    }
    Check(driver->SendPacket(nullptr, 60) == 0, "Null send buffer was accepted");
    for (int result : {PCAP_ERROR, PCAP_ERROR_NOT_ACTIVATED})
    {
        sendResult = result;
        const int before = errors;
        Check(driver->SendPacket(packet.data, 60) == 0, "Failed pcap send was reported as successful");
        Check(errors > before, "Failed pcap send was not diagnosed");
    }
    for (int result : {0, PCAP_ERROR_BREAK, PCAP_ERROR, PCAP_ERROR_NOT_ACTIVATED})
    {
        dispatchResult = result;
        const int before = errors;
        driver->RecvCheck();
        Check((errors > before) == (result < 0 && result != PCAP_ERROR_BREAK),
              "Receive error was not distinguished from idle polling or breakloop");
    }
}

static void OpenLifetime()
{
    auto library = Load();
    failOpen = true;
    Check(!library->Open("synthetic", {}) && closes == 0, "Failed open attempted to close a nonexistent handle");
    failOpen = false;
    failNonblock = true;
    Check(!library->Open("synthetic", {}) && closes == 1, "Nonblocking failure did not close its handle once");
    failNonblock = false;
    // A string_view may name just a prefix, with a valid terminator later on.
    const std::string name = "synthetic-unselected-suffix";
    auto first = library->Open(std::string_view(name.data(), 9), {});
    Require(first != nullptr, "Fixture could not open Ethernet backend");
    Check(openedName == "synthetic", "Open consumed bytes beyond the device-name string_view");
    auto replacement = Load();
    *replacement = std::move(*library);
    library.reset();
    LibPCap moved(std::move(*replacement));
    replacement.reset();
    auto second = moved.Open("synthetic", {});
    Require(second != nullptr, "Library move lost a required function");
    *second = std::move(*first);
    first.reset();
    Net_PCap finalDriver(std::move(*second));
    second.reset();
    // The final capture handle retains the DLL after its originating LibPCap.
    moved = std::move(*Load());
    const int before = dispatches;
    finalDriver.RecvCheck();
    Check(dispatches == before + 1, "Moved backend lost its capture dispatch function");
}

static void Enumeration()
{
    auto library = Load();
    findResult = PCAP_ERROR;
    Check(library->GetAdapters().empty() && enumerationCalls == 0, "Failed pcap enumeration continued into host enumeration");
    findResult = 0;
    emptyList = true;
    Check(library->GetAdapters().empty() && enumerationCalls == 0, "Empty pcap device list continued into host enumeration");
    emptyList = false;
    const auto adapters = library->GetAdapters();
    Check(adapters.size() == 1 && std::strcmp(adapters[0].DeviceName, deviceName) == 0,
          "Normal synthetic adapter enumeration changed");
    Check(freedLists == 1, "Normal pcap device list was not freed once");
    failEnumeration = true;
    Check(library->GetAdapters().empty(), "Failed host enumeration returned usable adapters");
    Check(freedLists == 2, "Host enumeration failure leaked the pcap device list");
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
#ifdef __WIN32__
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    const std::string scenario = argv[1];
    try
    {
        if (scenario == "controls") Controls();
        else if (scenario == "missing-symbols") MissingSymbols();
        else if (scenario == "capture") CaptureBounds();
        else if (scenario == "link-type") LinkType();
        else if (scenario == "io-errors") IOErrors();
        else if (scenario == "open-lifetime") OpenLifetime();
        else if (scenario == "enumeration") Enumeration();
        else return 2;
        for (const auto& handle : handles) Check(!handle->live, "Capture handle leaked after teardown");
        for (const auto& lib : libraries) Check(lib->unloads == 1, "Loaded library was not unloaded exactly once");
#ifdef __WIN32__
        Check(adapterBuffers.empty(), "Host enumeration failure leaked its adapter buffer");
        for (void* buffer : adapterBuffers) std::free(buffer);
#endif
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "Fixture error (not a valid RED): %s\n", error.what());
        return 2;
    }
    std::printf("PCap input %s: %d failures (loads=%zu, opens=%d, closes=%d)\n",
                scenario.c_str(), failures, libraries.size(), opens, closes);
    return failures ? 1 : 0;
}
