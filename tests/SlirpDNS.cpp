// SPDX-License-Identifier: GPL-3.0-or-later
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "Platform.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#else
#include <arpa/inet.h>
#include <netdb.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace melonDS;
using Bytes = std::vector<u8>;
static int lookups = 0, forwards = 0, releases = 0, resolverError = 0;
static std::string hostname;
static Bytes response, forwarded;
struct Slirp {};
static Slirp fixtureContext;

static int ResolveFixture(const char* name, const char*, const addrinfo* hints, addrinfo** result)
{
    ++lookups;
    hostname = name;
    *result = nullptr;
    if (resolverError) return resolverError;
    static sockaddr_in address{};
    static addrinfo answer{};
    address.sin_family = AF_INET;
    const u8 bytes[4] = {203, 0, 113, 7};
    std::memcpy(&address.sin_addr, bytes, 4);
    answer.ai_family = AF_INET;
    answer.ai_addrlen = sizeof(address);
    answer.ai_addr = reinterpret_cast<sockaddr*>(&address);
    answer.ai_next = nullptr;
    if (hints->ai_family != AF_INET) return EAI_FAMILY;
    *result = &answer;
    return 0;
}
static void ReleaseFixture(addrinfo*) { ++releases; }
static void slirp_input(Slirp*, const u8* bytes, int length)
{
    ++forwards;
    forwarded.assign(bytes, bytes + length);
}
namespace melonDS::Platform { void Log(LogLevel, const char*, ...) {} }

namespace melonDS
{
using Platform::Log;
using Platform::LogLevel;
constexpr u32 kDNSIP = 0x0A400002;
constexpr u8 kServerMAC[6] = {0, 0xAB, 0x33, 0x28, 0x99, 0x44};
struct Net_Slirp
{
    Slirp* Ctx = &fixtureContext;
    u32 IPv4ID = 0;
    Platform::SendPacketCallback Callback = [](const u8* data, int length) {
        response.assign(data, data + length);
    };
    int SendPacket(u8* data, int len) noexcept;
    void HandleDNSFrame(u8* data, int len) noexcept;
};

// Only current production definitions execute. Both external boundaries are
// stubs, so neither a host resolver nor a live slirp network can be reached.
#undef getaddrinfo
#undef freeaddrinfo
#define getaddrinfo ResolveFixture
#define freeaddrinfo ReleaseFixture
#include "SlirpFinishUDPFrame.inc"
#include "SlirpHandleDNSFrame.inc"
#include "SlirpSendPacket.inc"
#undef getaddrinfo
#undef freeaddrinfo
}

static void Put16(Bytes& bytes, size_t offset, u16 value)
{
    bytes[offset] = value >> 8;
    bytes[offset + 1] = value;
}
static u16 Get16(const Bytes& bytes, size_t offset)
{
    return (u16(bytes[offset]) << 8) | bytes[offset + 1];
}
static u32 SumWords(const u8* bytes, size_t length, u32 sum = 0)
{
    for (size_t i = 0; i < length; ++i)
        sum += u32(bytes[i]) << ((i & 1) ? 0 : 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return sum;
}
static Bytes Name(std::initializer_list<std::string> labels)
{
    Bytes name;
    for (const auto& label : labels)
    {
        name.push_back(static_cast<u8>(label.size()));
        name.insert(name.end(), label.begin(), label.end());
    }
    name.push_back(0);
    return name;
}
static Bytes Query(const Bytes& name = Name({"unit", "test"}))
{
    Bytes dns(12, 0);
    Put16(dns, 0, 0x1234);
    Put16(dns, 2, 0x0100);
    Put16(dns, 4, 1);
    dns.insert(dns.end(), name.begin(), name.end());
    dns.insert(dns.end(), {0, 1, 0, 1}); // A, IN.
    return dns;
}
static Bytes Packet(const Bytes& dns = Query(), const Bytes& options = {})
{
    const size_t ipLength = 20 + options.size();
    const size_t udp = 14 + ipLength;
    Bytes packet(udp + 8 + dns.size(), 0);
    std::copy(std::begin(kServerMAC), std::end(kServerMAC), packet.begin());
    const u8 sourceMAC[] = {2, 4, 6, 8, 10, 12};
    std::copy(std::begin(sourceMAC), std::end(sourceMAC), packet.begin() + 6);
    Put16(packet, 12, 0x0800);
    packet[14] = 0x40 | (ipLength / 4);
    Put16(packet, 16, packet.size() - 14);
    packet[22] = 64;
    packet[23] = 17;
    const u8 addresses[] = {10, 64, 0, 16, 10, 64, 0, 2};
    std::copy(std::begin(addresses), std::end(addresses), packet.begin() + 26);
    std::copy(options.begin(), options.end(), packet.begin() + 34);
    Put16(packet, 24, static_cast<u16>(~SumWords(packet.data() + 14, ipLength)));
    Put16(packet, udp, 0x4321);
    Put16(packet, udp + 2, 53);
    Put16(packet, udp + 4, 8 + dns.size());
    std::copy(dns.begin(), dns.end(), packet.begin() + udp + 8);
    return packet;
}
static void Reset()
{
    lookups = forwards = releases = resolverError = 0;
    hostname.clear();
    response.clear();
    forwarded.clear();
}
static int Send(const Bytes& packet)
{
    // Extra physical space makes logical-boundary errors observable as unwanted
    // resolver calls, independently of the separate guard-page crash cases.
    Bytes storage = packet;
    storage.resize(2048, 0);
    Net_Slirp driver;
    return driver.SendPacket(storage.data(), static_cast<int>(packet.size()));
}
struct GuardedPacket
{
    void* allocation = nullptr;
    size_t pageSize = 0;
    u8* bytes = nullptr;
    explicit GuardedPacket(const Bytes& packet)
    {
#ifdef _WIN32
        SYSTEM_INFO info;
        GetSystemInfo(&info);
        pageSize = info.dwPageSize;
        if (packet.size() > pageSize) return;
        allocation = VirtualAlloc(nullptr, pageSize * 2, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!allocation) return;
        DWORD previous;
        if (!VirtualProtect(static_cast<u8*>(allocation) + pageSize, pageSize, PAGE_NOACCESS, &previous)) return;
#else
        pageSize = static_cast<size_t>(sysconf(_SC_PAGESIZE));
        if (packet.size() > pageSize) return;
        allocation = mmap(nullptr, pageSize * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (allocation == MAP_FAILED) { allocation = nullptr; return; }
        if (mprotect(static_cast<u8*>(allocation) + pageSize, pageSize, PROT_NONE)) return;
#endif
        bytes = static_cast<u8*>(allocation) + pageSize - packet.size();
        if (!packet.empty()) std::memcpy(bytes, packet.data(), packet.size());
    }
    ~GuardedPacket()
    {
        if (!allocation) return;
#ifdef _WIN32
        VirtualFree(allocation, 0, MEM_RELEASE);
#else
        munmap(allocation, pageSize * 2);
#endif
    }
};

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    int failures = 0;
    const auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::fprintf(stderr, "%s\n", message); }
    };
    const auto discarded = [&] {
        check(lookups == 0 && forwards == 0 && response.empty(),
              "Rejected DNS input reached a resolver, network boundary, or response");
    };
    const auto reply = [&](const Bytes& query, std::array<u8, 4> address) {
        check(lookups == 1 && forwards == 0, "DNS override did not use exactly one fixture lookup");
        const size_t answer = 42 + query.size();
        const size_t expectedLength = (answer + 16 + 1) & ~size_t{1};
        check(response.size() == expectedLength && response.size() <= 1024, "DNS response length escaped its bounded layout");
        if (response.size() != expectedLength) return;
        check(Get16(response, 12) == 0x0800 && response[14] == 0x45 && response[23] == 17,
              "Reply Ethernet/IPv4/UDP header changed");
        check(Get16(response, 16) == response.size() - 14 && Get16(response, 38) == response.size() - 34,
              "Reply lengths do not describe the emitted packet");
        check(Get16(response, 34) == 53 && Get16(response, 36) == 0x4321,
              "Reply ports do not route back to the query");
        const u8 addresses[] = {10, 64, 0, 2, 10, 64, 0, 16};
        check(std::equal(std::begin(addresses), std::end(addresses), response.begin() + 26), "Reply IPv4 addresses changed");
        check(Get16(response, 42) == 0x1234 && Get16(response, 44) == 0x8000 &&
              Get16(response, 46) == 1 && Get16(response, 48) == 1 &&
              Get16(response, 50) == 0 && Get16(response, 52) == 0,
              "Existing simple-query reply contract changed");
        check(std::equal(query.begin() + 12, query.end(), response.begin() + 54), "Reply question differs from the validated query");
        check(Get16(response, answer) == 0xC00C && Get16(response, answer + 2) == 1 &&
              Get16(response, answer + 4) == 1 && Get16(response, answer + 8) == 3600 &&
              Get16(response, answer + 10) == 4 &&
              std::equal(address.begin(), address.end(), response.begin() + answer + 12),
              "DNS A answer does not contain the fixture address and existing TTL");
        check(SumWords(response.data() + 14, 20) == 0xFFFF, "Reply IPv4 checksum is invalid");
        const u32 pseudo = SumWords(response.data() + 26, 8) + 17 + Get16(response, 38);
        check(Get16(response, 40) != 0 && SumWords(response.data() + 34, response.size() - 34, pseudo) == 0xFFFF,
              "Reply UDP checksum is missing or invalid");
    };

    if (!std::strcmp(argv[1], "query-control"))
    {
        for (int error : {0, EAI_NONAME})
        {
            Reset(); resolverError = error;
            const Bytes query = Query();
            const Bytes packet = Packet(query);
            check(Send(packet) == packet.size(), "Normal query was not consumed");
            check(hostname == "unit.test", "Resolver hostname changed");
            reply(query, error ? std::array<u8, 4>{0, 0, 0, 0} : std::array<u8, 4>{203, 0, 113, 7});
        }
    }
    else if (!std::strcmp(argv[1], "header-guard"))
    {
        for (size_t length : std::array<size_t, 8>{13, 0, 14, 33, 34, 41, 42, 53})
        {
            Reset();
            Bytes packet = Packet(); packet.resize(length);
            GuardedPacket guard(packet);
            if (!guard.bytes) return 2;
            std::fprintf(stderr, "Guarded truncated frame: %zu bytes\n", length);
            Net_Slirp driver;
            driver.SendPacket(guard.bytes, static_cast<int>(length));
            discarded();
        }
    }
    else if (!std::strcmp(argv[1], "transport-lengths"))
    {
        for (int mutation = 0; mutation < 9; ++mutation)
        {
            Reset(); Bytes packet = Packet();
            if (mutation == 0) packet[14] = 0x65;
            if (mutation == 1) packet[14] = 0x44;
            if (mutation == 2) packet[14] = 0x4F;
            if (mutation == 3) Put16(packet, 16, 19);
            if (mutation == 4) Put16(packet, 16, packet.size());
            if (mutation == 5) Put16(packet, 38, 7);
            if (mutation == 6) Put16(packet, 38, packet.size());
            if (mutation == 7) Put16(packet, 38, 8 + 12 + 1); // Physical question extends beyond UDP.
            if (mutation == 8) Put16(packet, 16, 20 + 7);
            Send(packet); discarded();
        }
    }
    else if (!std::strcmp(argv[1], "ipv4-options"))
    {
        for (const Bytes& options : {Bytes{1, 1, 1, 1}, Bytes{1, 0, 0, 0}, Bytes(40, 1)})
        {
            Reset(); const Bytes query = Query();
            Send(Packet(query, options));
            check(hostname == "unit.test", "IPv4 IHL did not locate the actual UDP/DNS query");
            reply(query, {203, 0, 113, 7});
        }
        for (const Bytes& options : {Bytes{0x82, 1, 0, 0}, Bytes{0x82, 5, 0, 0}, Bytes{0x83, 3, 4, 0}})
        {
            Reset(); Send(Packet(Query(), options)); discarded();
        }
    }
    else if (!std::strcmp(argv[1], "label-guard"))
    {
        std::array<Bytes, 3> queries{Query(Bytes{63, 'x'}), Query(Bytes{1, 'x'}), Query(Name({"x"}))};
        queries[0].resize(14); // A label whose advertised bytes are unavailable.
        queries[1].resize(14); // No terminating root label.
        queries[2].pop_back(); // Truncated QCLASS.
        for (const Bytes& query : queries)
        {
            Reset(); const Bytes packet = Packet(query); GuardedPacket guard(packet);
            if (!guard.bytes) return 2;
            std::fprintf(stderr, "Guarded DNS name/question: %zu bytes\n", query.size());
            Net_Slirp driver; driver.SendPacket(guard.bytes, packet.size()); discarded();
        }
    }
    else if (!std::strcmp(argv[1], "query-shape"))
    {
        const std::array<Bytes, 6> names{
            Bytes{0xC0, 0x0C}, Bytes{0xC0, 0xFF}, Name({std::string(64, 'a')}),
            Name({std::string("a\0b", 3)}), Name({"a.b"}), Name({})};
        for (const Bytes& name : names)
        {
            Reset(); Send(Packet(Query(name))); discarded();
        }
        for (int mutation = 0; mutation < 10; ++mutation)
        {
            Reset(); Bytes query = Query();
            if (mutation == 0) Put16(query, query.size() - 4, 28); // AAAA.
            if (mutation == 1) Put16(query, query.size() - 2, 3); // Non-IN.
            if (mutation == 2) Put16(query, 2, 0x0900); // Unsupported opcode.
            if (mutation == 3) Put16(query, 2, 0x0300); // Truncated request.
            if (mutation == 4) Put16(query, 4, 2);
            if (mutation == 5) Put16(query, 6, 1);
            if (mutation == 6) Put16(query, 8, 1);
            if (mutation == 7) Put16(query, 10, 1);
            if (mutation == 8) query.push_back(0); // Undeclared trailing DNS input.
            if (mutation == 9) Put16(query, 2, 0x8100); // Already a response.
            Send(Packet(query)); discarded();
        }
    }
    else if (!std::strcmp(argv[1], "fragments-forwarding"))
    {
        for (u16 fragment : {u16{0x2000}, u16{1}, u16{0x8000}})
        {
            Reset(); Bytes packet = Packet(); Put16(packet, 20, fragment); Send(packet); discarded();
        }
        Reset(); Bytes query = Query(); Bytes packet = Packet(query);
        Put16(packet, 20, 0x4000); Send(packet); reply(query, {203, 0, 113, 7}); // DF is allowed.
        for (int kind = 0; kind < 3; ++kind)
        {
            Reset(); packet = Packet();
            if (kind == 0) { packet[33] = 3; Put16(packet, 20, 0x2000); }
            if (kind == 1) Put16(packet, 36, 54);
            if (kind == 2) Put16(packet, 12, 0x0806);
            check(Send(packet) == packet.size() && forwards == 1 && lookups == 0 && response.empty() && forwarded == packet,
                  "Non-override traffic did not reach the slirp boundary unchanged");
        }
    }
    else if (!std::strcmp(argv[1], "name-response-bounds"))
    {
        const Bytes maximum = Name({std::string(63, 'a'), std::string(63, 'b'), std::string(63, 'c'), std::string(61, 'd')});
        Reset(); Bytes query = Query(maximum); Bytes packet = Packet(query);
        packet.resize(packet.size() + 20, 0xCC); // Ethernet padding is outside IPv4/UDP.
        Send(packet); reply(query, {203, 0, 113, 7});
        check(hostname.size() == 253, "Maximum supported domain was truncated before lookup");
        Reset();
        const Bytes oversized = Name({std::string(63, 'a'), std::string(63, 'b'), std::string(63, 'c'), std::string(62, 'd')});
        Send(Packet(Query(oversized))); discarded();
    }
    else return 2;

    std::printf("%s: %s\n", argv[1], failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
