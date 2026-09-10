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

#include <cstring>

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
    memset(MPReplyQueue, 0, kReplyQueueSize);
    memset(&MPStatus, 0, sizeof(MPStatus));
    memset(PacketReadOffset, 0, sizeof(PacketReadOffset));
    memset(ReplyReadOffset, 0, sizeof(ReplyReadOffset));
    MPStatus.MPHostinst = 16; // no command host yet
    for (int& host : LastHostID) host = -1;

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
    Mutex_Lock(MPQueueLock);
    ResetFIFO(inst, 0);
    ResetFIFO(inst, 1);
    LastHostID[inst] = -1;
    if (MPStatus.MPHostinst == inst)
    {
        MPStatus.MPHostinst = 16;
        MPStatus.MPReplyBitmask = 0;
    }
    MPStatus.ConnectedBitmask |= (1 << inst);
    Mutex_Unlock(MPQueueLock);
}

void LocalMP::End(int inst)
{
    Mutex_Lock(MPQueueLock);
    MPStatus.ConnectedBitmask &= ~(1 << inst);
    ResetFIFO(inst, 0);
    ResetFIFO(inst, 1);
    LastHostID[inst] = -1;
    if (MPStatus.MPHostinst == inst)
    {
        MPStatus.MPHostinst = 16;
        MPStatus.MPReplyBitmask = 0;
    }
    Mutex_Unlock(MPQueueLock);
}

void LocalMP::ResetFIFO(int inst, int fifo) noexcept
{
    if (fifo == 0) PacketReadOffset[inst] = MPStatus.PacketWriteOffset;
    else           ReplyReadOffset[inst] = MPStatus.ReplyWriteOffset;

    // A receiver can already be waiting outside MPQueueLock. Do not use a
    // blocking reset (available/acquire can race with that receiver). All
    // posts hold MPQueueLock, so this nonblocking drain is bounded.
    while (Semaphore_TryWait(SemPool[fifo * 16 + inst], 0)) {}
}

void LocalMP::MakeRoom(int inst, int fifo, u32 len) noexcept
{
    const u32 size = fifo == 0 ? kPacketQueueSize : kReplyQueueSize;
    const u32 write = fifo == 0 ? MPStatus.PacketWriteOffset : MPStatus.ReplyWriteOffset;
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
        data = MPReplyQueue;
        datalen = kReplyQueueSize;
    }

    if ((offset + len) >= datalen)
    {
        u32 part1 = datalen - offset;
        memcpy(buf, &data[offset], part1);
        memcpy(&((u8*)buf)[part1], data, len - part1);
        offset = len - part1;
    }
    else
    {
        memcpy(buf, &data[offset], len);
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
        offset = MPStatus.ReplyWriteOffset;
        data = MPReplyQueue;
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
    else           MPStatus.ReplyWriteOffset = offset;
}

int LocalMP::SendPacketGeneric(int inst, u32 type, u8* packet, int len, u64 timestamp) noexcept
{
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
    if (reply)
    {
        // LocalMP still has one command host, not independent wireless groups.
        if (MPStatus.MPHostinst == 16 || !(mask & (1 << MPStatus.MPHostinst)))
        {
            Mutex_Unlock(MPQueueLock);
            return 0;
        }
        MakeRoom(MPStatus.MPHostinst, 1, recordlen);
    }
    else
    {
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
    FIFOWrite(inst, nfifo, &pktheader, sizeof(pktheader));
    if (len)
        FIFOWrite(inst, nfifo, packet, len);

    if (type == 1)
    {
        // NOTE: this is not guarded against, say, multiple multiplay games happening on the same machine
        // we would need to pass the packet's SenderID through the wifi module for that
        MPStatus.MPHostinst = inst;
        MPStatus.MPReplyBitmask = 0;
        ResetFIFO(inst, 1);
    }
    else if (type == 2)
    {
        MPStatus.MPReplyBitmask |= (1 << inst);
    }

    // Publish the complete record and its permit in the same critical section
    // as overflow recovery, Begin/End and command-host selection.
    if (type == 2)
    {
        Semaphore_Post(SemPool[16 +  MPStatus.MPHostinst]);
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

int LocalMP::RecvPacketGeneric(int inst, u8* packet, bool block, u64* timestamp) noexcept
{
    for (;;)
    {
        if (!Semaphore_TryWait(SemPool[inst], block ? RecvTimeout : 0))
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
            PacketReadOffset[inst] += pktheader.Length;
            if (PacketReadOffset[inst] >= kPacketQueueSize)
                PacketReadOffset[inst] -= kPacketQueueSize;

            Mutex_Unlock(MPQueueLock);
            continue;
        }

        if (pktheader.Length)
        {
            FIFORead(inst, 0, packet, pktheader.Length);

            if (pktheader.Type == 1)
                LastHostID[inst] = pktheader.SenderID;
        }

        if (timestamp) *timestamp = pktheader.Timestamp;
        Mutex_Unlock(MPQueueLock);
        return pktheader.Length;
    }
}

int LocalMP::SendPacket(int inst, u8* packet, int len, u64 timestamp)
{
    return SendPacketGeneric(inst, 0, packet, len, timestamp);
}

int LocalMP::RecvPacket(int inst, u8* packet, u64* timestamp)
{
    return RecvPacketGeneric(inst, packet, false, timestamp);
}

int LocalMP::SendCmd(int inst, u8* packet, int len, u64 timestamp)
{
    return SendPacketGeneric(inst, 1, packet, len, timestamp);
}

int LocalMP::SendReply(int inst, u8* packet, int len, u64 timestamp, u16 aid)
{
    return SendPacketGeneric(inst, 2 | (aid<<16), packet, len, timestamp);
}

int LocalMP::SendAck(int inst, u8* packet, int len, u64 timestamp)
{
    return SendPacketGeneric(inst, 3, packet, len, timestamp);
}

int LocalMP::RecvHostPacket(int inst, u8* packet, u64* timestamp)
{
    Mutex_Lock(MPQueueLock);
    const int host = LastHostID[inst];
    const bool hostleft = host != -1 && !(MPStatus.ConnectedBitmask & (1 << host));
    Mutex_Unlock(MPQueueLock);
    if (hostleft) return -1;

    return RecvPacketGeneric(inst, packet, true, timestamp);
}

u16 LocalMP::RecvReplies(int inst, u8* packets, u64 timestamp, u16 aidmask)
{
    u16 ret = 0;
    u16 myinstmask = (1 << inst);
    u16 curinstmask;

    for (;;)
    {
        Mutex_Lock(MPQueueLock);
        curinstmask = MPStatus.ConnectedBitmask;
        const bool receiving = MPStatus.MPHostinst == inst &&
                               (curinstmask & (1 << inst));
        Mutex_Unlock(MPQueueLock);
        // If the command session ended or all clients left, return early.
        if (!receiving || ((myinstmask & curinstmask) == curinstmask))
            return ret;

        if (!Semaphore_TryWait(SemPool[16+inst], RecvTimeout))
        {
            // no more replies available
            return ret;
        }

        Mutex_Lock(MPQueueLock);

        if (MPStatus.MPHostinst != inst ||
            ReplyReadOffset[inst] == MPStatus.ReplyWriteOffset)
        {
            ResetFIFO(inst, 1);
            Mutex_Unlock(MPQueueLock);
            return ret;
        }

        const u32 available = (MPStatus.ReplyWriteOffset + kReplyQueueSize
                               - ReplyReadOffset[inst]) % kReplyQueueSize;
        MPPacketHeader pktheader = {};
        if (available >= sizeof(pktheader))
            FIFORead(inst, 1, &pktheader, sizeof(pktheader));

        if (pktheader.Magic != 0x4946494E || pktheader.Length > kMaxFrameSize ||
            available < sizeof(pktheader) + pktheader.Length)
        {
            Log(LogLevel::Warn, "INVALID REPLY FIFO RECORD\n");
            ResetFIFO(inst, 1);
            Mutex_Unlock(MPQueueLock);
            return 0;
        }

        if ((pktheader.SenderID == inst) || // packet we sent out (shouldn't happen, but hey)
            (pktheader.Timestamp < (timestamp - 32))) // stale packet
        {
            // skip this packet
            ReplyReadOffset[inst] += pktheader.Length;
            if (ReplyReadOffset[inst] >= kReplyQueueSize)
                ReplyReadOffset[inst] -= kReplyQueueSize;

            Mutex_Unlock(MPQueueLock);
            continue;
        }

        if (pktheader.Length)
        {
            u32 aid = (pktheader.Type >> 16);
            FIFORead(inst, 1, &packets[(aid-1)*1024], pktheader.Length);
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

