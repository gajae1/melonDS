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

#include <stdio.h>
#include <string.h>
#include "Net.h"
#include "Net_Slirp.h"
#include "FIFO.h"
#include "Platform.h"

#include <libslirp.h>

#ifdef __WIN32__
	#include <ws2tcpip.h>
#else
	#include <sys/socket.h>
	#include <netdb.h>
	#include <poll.h>
	#include <time.h>
#endif

namespace melonDS
{

using Platform::Log;
using Platform::LogLevel;

const u32 kSubnet   = 0x0A400000;
const u32 kServerIP = kSubnet | 0x01;
const u32 kDNSIP    = kSubnet | 0x02;
const u32 kClientIP = kSubnet | 0x10;

const u8 kServerMAC[6] = {0x00, 0xAB, 0x33, 0x28, 0x99, 0x44};

#ifdef __WIN32__

#define poll WSAPoll
#ifndef CLOCK_MONOTONIC
#define CLOCK_MONOTONIC 1
#endif

// https://stackoverflow.com/questions/5404277/porting-clock-gettime-to-windows

struct timespec { long tv_sec; long tv_nsec; };

int clock_gettime(int, struct timespec *spec)
{
    __int64 wintime;
    GetSystemTimeAsFileTime((FILETIME*)&wintime);
    wintime -=116444736000000000LL;                 //1jan1601 to 1jan1970
    spec->tv_sec  = wintime / 10000000LL;           //seconds
    spec->tv_nsec = wintime % 10000000LL * 100;     //nano-seconds
    return 0;
}

#endif // __WIN32__


ssize_t Net_Slirp::SlirpCbSendPacket(const void* buf, size_t len, void* opaque) noexcept
{
    if (len > 2048)
    {
        Log(LogLevel::Warn, "slirp: packet too big (%zu)\n", len);
        return 0;
    }

    Log(LogLevel::Debug, "slirp: response packet of %zu bytes, type %04X\n", len, ntohs(((u16*)buf)[6]));

    Net_Slirp& self = *static_cast<Net_Slirp*>(opaque);
    if (self.Callback)
    {
        self.Callback((const u8*)buf, len);
    }

    return len;
}

void SlirpCbGuestError(const char* msg, void* opaque)
{
    Log(LogLevel::Error, "SLIRP: error: %s\n", msg);
}

int64_t SlirpCbClockGetNS(void* opaque)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void* SlirpCbTimerNew(SlirpTimerCb cb, void* cb_opaque, void* opaque)
{
    return nullptr;
}

void SlirpCbTimerFree(void* timer, void* opaque)
{
}

void SlirpCbTimerMod(void* timer, int64_t expire_time, void* opaque)
{
}

void SlirpCbRegisterPollFD(int fd, void* opaque)
{
    Log(LogLevel::Debug, "Slirp: register poll FD %d\n", fd);
}

void SlirpCbUnregisterPollFD(int fd, void* opaque)
{
    Log(LogLevel::Debug, "Slirp: unregister poll FD %d\n", fd);
}

void SlirpCbNotify(void* opaque)
{
    Log(LogLevel::Debug, "Slirp: notify???\n");
}

const SlirpCb Net_Slirp::cb =
{
    .send_packet = SlirpCbSendPacket,
    .guest_error = SlirpCbGuestError,
    .clock_get_ns = SlirpCbClockGetNS,
    .timer_new = SlirpCbTimerNew,
    .timer_free = SlirpCbTimerFree,
    .timer_mod = SlirpCbTimerMod,
    .register_poll_fd = SlirpCbRegisterPollFD,
    .unregister_poll_fd = SlirpCbUnregisterPollFD,
    .notify = SlirpCbNotify
};

Net_Slirp::Net_Slirp(const Platform::SendPacketCallback& callback) noexcept : Callback(callback)
{
    SlirpConfig cfg {};
    memset(&cfg, 0, sizeof(cfg));
    cfg.version = 1;

    cfg.in_enabled = true;
    *(u32*)&cfg.vnetwork = htonl(kSubnet);
    *(u32*)&cfg.vnetmask = htonl(0xFFFFFF00);
    *(u32*)&cfg.vhost = htonl(kServerIP);
    cfg.vhostname = "melonServer";
    *(u32*)&cfg.vdhcp_start = htonl(kClientIP);
    *(u32*)&cfg.vnameserver = htonl(kDNSIP);

    Ctx = slirp_new(&cfg, &cb, this);
}

Net_Slirp::~Net_Slirp() noexcept
{
    if (Ctx)
    {
        slirp_cleanup(Ctx);
        Ctx = nullptr;
    }
}

void FinishUDPFrame(u8* data, int len)
{
    u8* ipheader = &data[0xE];
    u8* udpheader = &data[0x22];

    // lengths
    *(u16*)&ipheader[2] = htons(len - 0xE);
    *(u16*)&udpheader[4] = htons(len - (0xE + 0x14));

    // IP checksum
    u32 tmp = 0;

    for (int i = 0; i < 20; i += 2)
        tmp += ntohs(*(u16*)&ipheader[i]);
    while (tmp >> 16)
        tmp = (tmp & 0xFFFF) + (tmp >> 16);
    tmp ^= 0xFFFF;
    *(u16*)&ipheader[10] = htons(tmp);

    // UDP checksum
    // (note: normally not mandatory, but some older sgIP versions require it)
    tmp = 0;
    tmp += ntohs(*(u16*)&ipheader[12]);
    tmp += ntohs(*(u16*)&ipheader[14]);
    tmp += ntohs(*(u16*)&ipheader[16]);
    tmp += ntohs(*(u16*)&ipheader[18]);
    tmp += ntohs(0x1100);
    tmp += (len-0x22);
    for (u8* i = udpheader; i < &udpheader[len-0x23]; i += 2)
        tmp += ntohs(*(u16*)i);
    if (len & 1)
        tmp += ntohs((u_short)udpheader[len-0x23]);
    while (tmp >> 16)
        tmp = (tmp & 0xFFFF) + (tmp >> 16);
    tmp ^= 0xFFFF;
    if (tmp == 0) tmp = 0xFFFF;
    *(u16*)&udpheader[6] = htons(tmp);
}

void Net_Slirp::HandleDNSFrame(u8* data, int len) noexcept
{
    // SendPacket bounds the unfragmented IPv4/UDP frame and trims len to UDP.
    const auto read16 = [](const u8* p) -> u16 { return (u16(p[0]) << 8) | p[1]; };
    const u8* ipheader = &data[14];
    const int iplen = (ipheader[0] & 0x0F) * 4;
    const u8* udpheader = ipheader + iplen;
    const u8* dnsbody = udpheader + 8;
    if (len < 14 + iplen + 8 + 12) return;
    const u32 dnslen = len - (14 + iplen + 8);

    const u16 id = read16(dnsbody);
    const u16 flags = read16(dnsbody + 2);
    // This override handles one ordinary question, with optional RD only.
    // Other sections, opcodes, truncated messages and replies are unsupported.
    if ((flags & ~0x0100) || read16(dnsbody + 4) != 1 ||
        read16(dnsbody + 6) || read16(dnsbody + 8) || read16(dnsbody + 10))
        return;

    char domainname[256];
    u32 namelen = 0;
    u32 cursor = 12;
    for (;;)
    {
        if (cursor >= dnslen || cursor - 12 >= 255) return;
        const u8 labellen = dnsbody[cursor++];
        if (!labellen) break;
        // Compression and reserved label forms are not part of this simple
        // query override. Include the root byte in the 255-byte wire limit.
        if (labellen > 63 || labellen > dnslen - cursor || cursor - 12 + labellen >= 255)
            return;
        if (namelen) domainname[namelen++] = '.';
        if (namelen + labellen >= sizeof(domainname)) return;
        for (u32 i = 0; i < labellen; ++i)
        {
            const u8 ch = dnsbody[cursor++];
            // These labels cannot be represented faithfully as a resolver C string.
            if (!ch || ch == '.') return;
            domainname[namelen++] = ch;
        }
    }
    if (!namelen || dnslen - cursor != 4) return;
    domainname[namelen] = '\0';
    if (read16(dnsbody + cursor) != 1 || read16(dnsbody + cursor + 2) != 1)
        return; // Only A/IN has a four-byte answer here.

    const u32 qlen = cursor + 4 - 12;
    u8 resp[1024];
    // Ethernet + fixed IPv4 + UDP + DNS header + question + A record + pad.
    if (14 + 20 + 8 + 12 + qlen + 16 + 1 > sizeof(resp)) return;

    // Preserve the existing zero-address answer when hostname lookup fails.
    // All query validation has finished before entering the host resolver.
    u32 addr_res = 0;
    struct addrinfo dns_hint {};
    struct addrinfo* dns_res = nullptr;
    dns_hint.ai_family = AF_INET;
    if (getaddrinfo(domainname, "0", &dns_hint, &dns_res) == 0)
    {
        for (const struct addrinfo* p = dns_res; p; p = p->ai_next)
        {
            if (p->ai_family != AF_INET || !p->ai_addr || p->ai_addrlen < sizeof(struct sockaddr_in))
                continue;
            const auto* addr = reinterpret_cast<const struct sockaddr_in*>(p->ai_addr);
            memcpy(&addr_res, &addr->sin_addr, sizeof(addr_res));
            break;
        }
        if (dns_res) freeaddrinfo(dns_res);
    }

    u8* out = resp;
    const auto put16 = [&out](u16 value) {
        *out++ = value >> 8;
        *out++ = value;
    };
    const auto put32 = [&out](u32 value) {
        *out++ = value >> 24;
        *out++ = value >> 16;
        *out++ = value >> 8;
        *out++ = value;
    };

    memcpy(out, &data[6], 6); out += 6;
    memcpy(out, kServerMAC, 6); out += 6;
    put16(0x0800);

    *out++ = 0x45;
    *out++ = 0;
    put16(0); // IPv4 length, filled by FinishUDPFrame.
    put16(IPv4ID++);
    put16(0); // No fragmentation or options in the response.
    *out++ = 0x80;
    *out++ = 0x11;
    put16(0); // IPv4 checksum.
    put32(kDNSIP);
    memcpy(out, ipheader + 12, 4); out += 4;

    put16(53);
    put16(read16(udpheader));
    put16(0); // UDP length.
    put16(0); // UDP checksum.

    put16(id);
    put16(0x8000);
    put16(1); // One question and one answer.
    put16(1);
    put16(0);
    put16(0);
    memcpy(out, dnsbody + 12, qlen); out += qlen;
    put16(0xC00C); // The question's name starts at DNS offset 12.
    put16(1); // A.
    put16(1); // IN.
    put32(3600);
    put16(4);
    memcpy(out, &addr_res, 4); out += 4;

    u32 framelen = static_cast<u32>(out - resp);
    if (framelen & 1) { *out++ = 0; framelen++; }
    FinishUDPFrame(resp, framelen);
    if (Callback) Callback(resp, framelen);
}

int Net_Slirp::SendPacket(u8* data, int len) noexcept
{
    if (!Ctx || !data || len < 14) return 0;
    if (len > 2048)
    {
        Log(LogLevel::Error, "Net_SendPacket: error: packet too long (%d)\n", len);
        return 0;
    }

    const auto read16 = [](const u8* p) -> u16 { return (u16(p[0]) << 8) | p[1]; };
    if (read16(data + 12) == 0x0800)
    {
        if (len < 14 + 20) return 0;
        const u8* ipheader = data + 14;
        const int iplen = (ipheader[0] & 0x0F) * 4;
        const int total = read16(ipheader + 2);
        if ((ipheader[0] >> 4) != 4 || iplen < 20 || iplen > len - 14 ||
            total < iplen || total > len - 14)
            return 0;

        const u32 dnsip = htonl(kDNSIP);
        if (ipheader[9] == 0x11 && memcmp(ipheader + 16, &dnsip, 4) == 0)
        {
            // A fragment has no complete UDP/DNS query. Do not bypass this
            // override via libslirp for fragmented traffic to the virtual DNS IP.
            // DF is allowed; reserved flags, MF and nonzero offsets are not.
            if (read16(ipheader + 6) & 0xBFFF) return 0;
            if (total < iplen + 8) return 0;
            const u8* udpheader = ipheader + iplen;
            const int udplen = read16(udpheader + 4);
            if (udplen < 8 || udplen > total - iplen) return 0;
            if (read16(udpheader + 2) == 53)
            {
                // Accept NOP/EOL padding options. Routing and other options
                // require IP-stack semantics outside this local DNS override.
                for (int i = 20; i < iplen; ++i)
                {
                    if (ipheader[i] == 0)
                    {
                        for (++i; i < iplen; ++i)
                            if (ipheader[i] != 0) return 0;
                        break;
                    }
                    if (ipheader[i] != 1) return 0;
                }
                HandleDNSFrame(data, 14 + iplen + udplen);
                return len;
            }
        }
    }

    slirp_input(Ctx, data, len);
    return len;
}

int Net_Slirp::SlirpCbAddPoll(int fd, int events, void* opaque) noexcept
{
    Net_Slirp& self = *static_cast<Net_Slirp*>(opaque);

    if (self.PollListSize >= PollListMax)
    {
        Log(LogLevel::Error, "slirp: POLL LIST FULL\n");
        return -1;
    }

    int idx = self.PollListSize++;

    u16 evt = 0;

    if (events & SLIRP_POLL_IN) evt |= POLLIN;
    if (events & SLIRP_POLL_OUT) evt |= POLLWRNORM;

#ifndef __WIN32__
    // CHECKME
    if (events & SLIRP_POLL_PRI) evt |= POLLPRI;
    if (events & SLIRP_POLL_ERR) evt |= POLLERR;
    if (events & SLIRP_POLL_HUP) evt |= POLLHUP;
#endif // !__WIN32__

    self.PollList[idx].fd = fd;
    self.PollList[idx].events = evt;

    return idx;
}

int Net_Slirp::SlirpCbGetREvents(int idx, void* opaque) noexcept
{
    Net_Slirp& self = *static_cast<Net_Slirp*>(opaque);

    if (idx < 0 || idx >= self.PollListSize)
        return 0;

    u16 evt = self.PollList[idx].revents;
    int ret = 0;

    if (evt & POLLIN) ret |= SLIRP_POLL_IN;
    if (evt & POLLWRNORM) ret |= SLIRP_POLL_OUT;
    if (evt & POLLPRI) ret |= SLIRP_POLL_PRI;
    if (evt & POLLERR) ret |= SLIRP_POLL_ERR;
    if (evt & POLLHUP) ret |= SLIRP_POLL_HUP;

    return ret;
}

void Net_Slirp::RecvCheck() noexcept
{
    if (!Ctx) return;

    //if (PollListSize > 0)
    {
        u32 timeout = 0;
        PollListSize = 0;
        slirp_pollfds_fill(Ctx, &timeout, SlirpCbAddPoll, this);
        int res = poll(PollList, PollListSize, timeout);
        slirp_pollfds_poll(Ctx, res<0, SlirpCbGetREvents, this);
    }
}

}
