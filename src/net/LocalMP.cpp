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

#include <algorithm>
#include <cstring>
#include <new>

#include "LocalMP.h"

using namespace melonDS;
using namespace melonDS::Platform;

using Platform::Log;
using Platform::LogLevel;

namespace melonDS
{

LocalMP::LocalMP() noexcept :
    MPQueueLock(Mutex_Create())
{
    memset(MPPacketQueue, 0, kPacketQueueSize);
    memset(&MPStatus, 0, sizeof(MPStatus));
    memset(PacketReadOffset, 0, sizeof(PacketReadOffset));
    memset(ReplyReadOffset, 0, sizeof(ReplyReadOffset));
    for (int& host : LastHostID) host = -1;
    for (int& host : ReplyHostID) host = -1;

    // prepare semaphores
    // semaphores 0-15: regular frames; semaphore I is posted when instance I needs to process a new frame
    // semaphores 16-31: MP replies; semaphore I is posted when instance I needs to process a new MP reply

    for (int i = 0; i < 32; i++)
    {
        SemPool[i] = Semaphore_Create();
    }

    Log(LogLevel::Info, "MP comm init OK\n");
}

LocalMP::~LocalMP() noexcept
{
    for (int i = 0; i < 32; i++)
    {
        Semaphore_Free(SemPool[i]);
        SemPool[i] = nullptr;
    }

    Mutex_Free(MPQueueLock);
}

void LocalMP::Begin(int inst)
{
    if (static_cast<u32>(inst) >= 16) return;
    Mutex_Lock(MPQueueLock);
    ResetFIFO(inst, 0);
    ResetFIFO(inst, 1);
    LastHostID[inst] = -1;
    ReplyHostID[inst] = -1;
    MPStatus.ActiveHosts &= ~(1 << inst);
    HostChannel[inst] = 0;
    MPStatus.ConnectedBitmask |= (1 << inst);
    Mutex_Unlock(MPQueueLock);
}

void LocalMP::End(int inst)
{
    if (static_cast<u32>(inst) >= 16) return;
    Mutex_Lock(MPQueueLock);
    MPStatus.ConnectedBitmask &= ~(1 << inst);
    ResetFIFO(inst, 0);
    ResetFIFO(inst, 1);
    LastHostID[inst] = -1;
    ReplyHostID[inst] = -1;
    MPStatus.ActiveHosts &= ~(1 << inst);
    HostChannel[inst] = 0;
    Mutex_Unlock(MPQueueLock);
}

void LocalMP::ResetFIFO(int inst, int fifo) noexcept
{
    if (fifo == 0) PacketReadOffset[inst] = MPStatus.PacketWriteOffset;
    else           ReplyReadOffset[inst] = MPStatus.ReplyWriteOffset[inst];

    // A receiver can already be waiting outside MPQueueLock. Do not use a
    // blocking reset (available/acquire can race with that receiver). All
    // posts hold MPQueueLock, so this nonblocking drain is bounded.
    while (Semaphore_TryWait(SemPool[fifo * 16 + inst], 0)) {}
}

void LocalMP::MakeRoom(int inst, int fifo, u32 len) noexcept
{
    const u32 size = fifo == 0 ? kPacketQueueSize : kReplyQueueSize;
    const u32 write = fifo == 0 ? MPStatus.PacketWriteOffset : MPStatus.ReplyWriteOffset[inst];
    const u32 read = fifo == 0 ? PacketReadOffset[inst] : ReplyReadOffset[inst];
    const u32 used = (write + size - read) % size;

    // Nonblocking overflow policy: discard the lagging receiver's entire
    // backlog before writing a whole new record. Other receivers keep theirs.
    // Reserve one byte to distinguish a full ring from an empty one.
    if (len >= size - used)
        ResetFIFO(inst, fifo);
}

void LocalMP::FIFORead(int inst, int fifo, void* buf, int len) noexcept
{
    u8* data;

    u32 offset, datalen;
    if (fifo == 0)
    {
        offset = PacketReadOffset[inst];
        data = MPPacketQueue;
        datalen = kPacketQueueSize;
    }
    else
    {
        offset = ReplyReadOffset[inst];
        data = MPReplyQueue[inst].get();
        datalen = kReplyQueueSize;
    }

    if ((offset + len) >= datalen)
    {
        u32 part1 = datalen - offset;
        if (buf)
        {
            memcpy(buf, &data[offset], part1);
            memcpy(&((u8*)buf)[part1], data, len - part1);
        }
        offset = len - part1;
    }
    else
    {
        if (buf) memcpy(buf, &data[offset], len);
        offset += len;
    }

    if (fifo == 0) PacketReadOffset[inst] = offset;
    else           ReplyReadOffset[inst] = offset;
}

void LocalMP::FIFOWrite(int inst, int fifo, void* buf, int len) noexcept
{
    u8* data;

    u32 offset, datalen;
    if (fifo == 0)
    {
        offset = MPStatus.PacketWriteOffset;
        data = MPPacketQueue;
        datalen = kPacketQueueSize;
    }
    else
    {
        offset = MPStatus.ReplyWriteOffset[inst];
        data = MPReplyQueue[inst].get();
        datalen = kReplyQueueSize;
    }

    if ((offset + len) >= datalen)
    {
        u32 part1 = datalen - offset;
        memcpy(&data[offset], buf, part1);
        memcpy(data, &((u8*)buf)[part1], len - part1);
        offset = len - part1;
    }
    else
    {
        memcpy(&data[offset], buf, len);
        offset += len;
    }

    if (fifo == 0) MPStatus.PacketWriteOffset = offset;
    else           MPStatus.ReplyWriteOffset[inst] = offset;
}

namespace
{
bool HasWifiHeader(const u8* packet, int len) noexcept
{
    // Twelve-byte TX header followed by the 24-byte 802.11 MAC header.
    return len >= 36 && packet && packet[9] >= 1 && packet[9] <= 14 &&
           (packet[10] | (packet[11] << 8)) == len - 12;
}
}

int LocalMP::FindReplyHost(int inst, const u8* packet, int len) const noexcept
{
    const u16 hosts = MPStatus.ActiveHosts & MPStatus.ConnectedBitmask & ~(1 << inst);
    if (HasWifiHeader(packet, len))
    {
        int match = -1;
        for (int host = 0; host < 16; ++host)
        {
            if (!(hosts & (1 << host)) || HostChannel[host] != packet[9] ||
                memcmp(HostAddress[host], packet + 16, 6) != 0) continue;
            // Duplicate addresses on the same channel cannot identify a group.
            if (match != -1) return -1;
            match = host;
        }
        return match;
    }

    // Blank replies are sent immediately for the CMD just received. Retain
    // the opaque-frame API used by transport callers without a Wi-Fi header.
    const int observed = LastHostID[inst];
    if (observed != -1)
        return (hosts & (1 << observed)) ? observed : -1;
    int match = -1;
    for (int host = 0; host < 16; ++host)
    {
        if (!(hosts & (1 << host))) continue;
        if (match != -1) return -1;
        match = host;
    }
    return match;
}

int LocalMP::SendPacketGeneric(int inst, u32 type, u8* packet, int len, u64 timestamp) noexcept
{
    const u32 kind = type & 0xFFFF;
    const u32 aid = type >> 16;
    if (static_cast<u32>(inst) >= 16 || len < 0 || (len && !packet) || kind > 3 ||
        (kind == 2 ? (aid > 15 || (!aid && len)) : aid != 0))
        return 0;
    if (len > kMaxFrameSize)
    {
        Log(LogLevel::Warn, "wifi: attempting to send frame too big (len=%d max=%d)\n", len, kMaxFrameSize);
        return 0;
    }

    Mutex_Lock(MPQueueLock);

    u16 mask = MPStatus.ConnectedBitmask;

    if (!(mask & (1 << inst)))
    {
        Mutex_Unlock(MPQueueLock);
        return 0;
    }

    const u32 recordlen = sizeof(MPPacketHeader) + len;
    const bool reply = (type & 0xFFFF) == 2;
    int destination = inst;
    if (reply)
    {
        destination = FindReplyHost(inst, packet, len);
        if (destination == -1)
        {
            Mutex_Unlock(MPQueueLock);
            return 0;
        }
        MakeRoom(destination, 1, recordlen);
    }
    else
    {
        if (kind == 1 && !MPReplyQueue[inst])
        {
            MPReplyQueue[inst].reset(new (std::nothrow) u8[kReplyQueueSize]);
            if (!MPReplyQueue[inst])
            {
                Mutex_Unlock(MPQueueLock);
                return 0;
            }
        }
        for (int i = 0; i < 16; i++)
        {
            if (mask & (1 << i))
                MakeRoom(i, 0, recordlen);
        }
    }

    MPPacketHeader pktheader;
    pktheader.Magic = 0x4946494E;
    pktheader.SenderID = inst;
    pktheader.Type = type;
    pktheader.Length = len;
    pktheader.Timestamp = timestamp;

    type &= 0xFFFF;
    int nfifo = (type == 2) ? 1 : 0;
    FIFOWrite(destination, nfifo, &pktheader, sizeof(pktheader));
    if (len)
        FIFOWrite(destination, nfifo, packet, len);

    if (type == 1)
    {
        MPStatus.ActiveHosts |= (1 << inst);
        HostChannel[inst] = HasWifiHeader(packet, len) ? packet[9] : 0;
        if (HostChannel[inst]) memcpy(HostAddress[inst], packet + 22, 6);
        ResetFIFO(inst, 1);
    }
    else if (type == 2)
    {
        ReplyHostID[inst] = destination;
    }

    // Publish the complete record and its permit in the same critical section
    // as overflow recovery, Begin/End and command-host selection.
    if (type == 2)
    {
        Semaphore_Post(SemPool[16 + destination]);
    }
    else
    {
        for (int i = 0; i < 16; i++)
        {
            if (mask & (1<<i))
                Semaphore_Post(SemPool[i]);
        }
    }

    Mutex_Unlock(MPQueueLock);
    return len;
}

int LocalMP::RecvPacketGeneric(int inst, u8* packet, bool block, u64* timestamp, u32 capacity) noexcept
{
    if (static_cast<u32>(inst) >= 16 || !packet || !capacity) return 0;
    for (;;)
    {
        if (!Semaphore_TryWait(SemPool[inst], block ? GetRecvTimeout() : 0))
        {
            return 0;
        }

        Mutex_Lock(MPQueueLock);

        // A permit may have been acquired just before another thread reset
        // this receiver. Cursors, not an in-flight permit, own the queue data.
        if (!(MPStatus.ConnectedBitmask & (1 << inst)) ||
            PacketReadOffset[inst] == MPStatus.PacketWriteOffset)
        {
            ResetFIFO(inst, 0);
            Mutex_Unlock(MPQueueLock);
            return 0;
        }

        const u32 available = (MPStatus.PacketWriteOffset + kPacketQueueSize
                               - PacketReadOffset[inst]) % kPacketQueueSize;
        MPPacketHeader pktheader = {};
        if (available >= sizeof(pktheader))
            FIFORead(inst, 0, &pktheader, sizeof(pktheader));

        if (pktheader.Magic != 0x4946494E || pktheader.Length > kMaxFrameSize ||
            pktheader.SenderID >= 16 || pktheader.Type > 3 || pktheader.Type == 2 ||
            available < sizeof(pktheader) + pktheader.Length)
        {
            Log(LogLevel::Warn, "INVALID PACKET FIFO RECORD\n");
            ResetFIFO(inst, 0);
            Mutex_Unlock(MPQueueLock);
            return 0;
        }

        if (pktheader.SenderID == inst)
        {
            // skip this packet
            FIFORead(inst, 0, nullptr, pktheader.Length);

            Mutex_Unlock(MPQueueLock);
            continue;
        }

        const u32 len = std::min(pktheader.Length, capacity);
        if (pktheader.Length)
        {
            FIFORead(inst, 0, packet, len);
            FIFORead(inst, 0, nullptr, pktheader.Length - len);

            if (pktheader.Type == 1)
                LastHostID[inst] = pktheader.SenderID;
        }

        if (timestamp) *timestamp = pktheader.Timestamp;
        Mutex_Unlock(MPQueueLock);
        return len;
    }
}

int LocalMP::SendPacket(int inst, u8* packet, int len, u64 timestamp)
{
    return SendPacketGeneric(inst, 0, packet, len, timestamp);
}

int LocalMP::RecvPacket(int inst, u8* packet, u64* timestamp, u32 capacity)
{
    return RecvPacketGeneric(inst, packet, false, timestamp, capacity);
}

int LocalMP::SendCmd(int inst, u8* packet, int len, u64 timestamp)
{
    return SendPacketGeneric(inst, 1, packet, len, timestamp);
}

int LocalMP::SendReply(int inst, u8* packet, int len, u64 timestamp, u16 aid)
{
    return SendPacketGeneric(inst, 2 | (u32(aid)<<16), packet, len, timestamp);
}

int LocalMP::SendAck(int inst, u8* packet, int len, u64 timestamp)
{
    return SendPacketGeneric(inst, 3, packet, len, timestamp);
}

int LocalMP::RecvHostPacket(int inst, u8* packet, u64* timestamp, u32 capacity)
{
    if (static_cast<u32>(inst) >= 16) return 0;
    Mutex_Lock(MPQueueLock);
    // A foreign CMD can be delivered before Wifi rejects its BSSID. It must
    // not replace the host to which this client has actually sent replies.
    const int host = ReplyHostID[inst] != -1 ? ReplyHostID[inst] : LastHostID[inst];
    const bool hostleft = host != -1 && !(MPStatus.ConnectedBitmask & (1 << host));
    Mutex_Unlock(MPQueueLock);
    if (hostleft) return -1;

    return RecvPacketGeneric(inst, packet, true, timestamp, capacity);
}

u16 LocalMP::RecvReplies(int inst, u8* packets, u64 timestamp, u16 aidmask)
{
    if (static_cast<u32>(inst) >= 16 || !packets) return 0;
    u16 ret = 0;
    u16 myinstmask = (1 << inst);
    u16 curinstmask;

    for (;;)
    {
        Mutex_Lock(MPQueueLock);
        curinstmask = MPStatus.ConnectedBitmask;
        const bool receiving = (MPStatus.ActiveHosts & (1 << inst)) &&
                               (curinstmask & (1 << inst));
        for (int client = 0; client < 16; ++client)
        {
            // Sending a reply establishes membership even if this instance
            // previously sent CMDs without powering Wi-Fi off in between.
            const bool otherGroup = ReplyHostID[client] != -1
                ? ReplyHostID[client] != inst : (MPStatus.ActiveHosts & (1 << client)) != 0;
            if (client != inst && otherGroup)
                curinstmask &= ~(1 << client);
        }
        Mutex_Unlock(MPQueueLock);
        // If the command session ended or all clients left, return early.
        if (!receiving || ((myinstmask & curinstmask) == curinstmask))
            return ret;

        if (!Semaphore_TryWait(SemPool[16+inst], GetRecvTimeout()))
        {
            // no more replies available
            return ret;
        }

        Mutex_Lock(MPQueueLock);

        if (!(MPStatus.ActiveHosts & (1 << inst)) ||
            ReplyReadOffset[inst] == MPStatus.ReplyWriteOffset[inst])
        {
            ResetFIFO(inst, 1);
            Mutex_Unlock(MPQueueLock);
            return ret;
        }

        const u32 available = (MPStatus.ReplyWriteOffset[inst] + kReplyQueueSize
                               - ReplyReadOffset[inst]) % kReplyQueueSize;
        MPPacketHeader pktheader = {};
        if (available >= sizeof(pktheader))
            FIFORead(inst, 1, &pktheader, sizeof(pktheader));

        const u32 aid = pktheader.Type >> 16;
        if (pktheader.Magic != 0x4946494E || pktheader.Length > kMaxFrameSize ||
            pktheader.SenderID >= 16 || (pktheader.Type & 0xFFFF) != 2 ||
            aid > 15 || (!aid && pktheader.Length) ||
            available < sizeof(pktheader) + pktheader.Length)
        {
            Log(LogLevel::Warn, "INVALID REPLY FIFO RECORD\n");
            ResetFIFO(inst, 1);
            Mutex_Unlock(MPQueueLock);
            return 0;
        }

        if ((pktheader.SenderID == inst) || // packet we sent out (shouldn't happen, but hey)
            (pktheader.Timestamp < timestamp && timestamp - pktheader.Timestamp > 32)) // stale packet
        {
            // skip this packet
            FIFORead(inst, 1, nullptr, pktheader.Length);

            Mutex_Unlock(MPQueueLock);
            continue;
        }

        if (pktheader.Length)
        {
            const u32 len = std::min(pktheader.Length, 1024u);
            FIFORead(inst, 1, &packets[(aid-1)*1024], len);
            FIFORead(inst, 1, nullptr, pktheader.Length - len);
            ret |= (1 << aid);
        }

        myinstmask |= (1 << pktheader.SenderID);
        if (((myinstmask & curinstmask) == curinstmask) ||
            ((ret & aidmask) == aidmask))
        {
            // all the clients have sent their reply

            Mutex_Unlock(MPQueueLock);
            return ret;
        }

        Mutex_Unlock(MPQueueLock);
    }
}

}

