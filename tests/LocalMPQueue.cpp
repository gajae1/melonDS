// SPDX-License-Identifier: GPL-3.0-or-later
// Generated frames through the production LocalMP API, without ROMs or sockets.
#include "net/LocalMP.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

namespace
{
using namespace melonDS;
constexpr u8 Sentinel = 0xA5;
using Output = std::array<u8, 15 * 1024 + 64>;

bool Check(bool ok, const char* message)
{
    if (!ok) std::fprintf(stderr, "%s\n", message);
    return ok;
}

std::vector<u8> Frame(unsigned id, size_t size)
{
    std::vector<u8> frame(size);
    for (size_t i = 0; i < size; ++i)
        frame[i] = static_cast<u8>(id * 17 + i * 31);
    std::memcpy(frame.data(), &id, sizeof(id));
    return frame;
}

bool Copied(const Output& out, const std::vector<u8>& frame, size_t offset = 0)
{
    for (size_t i = 0; i < out.size(); ++i)
    {
        const size_t begin = 32 + offset;
        const u8 expected = i >= begin && i < begin + frame.size()
            ? frame[i - begin] : Sentinel;
        if (out[i] != expected) return false;
    }
    return true;
}

bool Receive(LocalMP& net, int inst, const std::vector<u8>& frame, u64 stamp,
             bool host = false)
{
    Output out;
    out.fill(Sentinel);
    u64 received = 0;
    int len = host ? net.RecvHostPacket(inst, out.data() + 32, &received)
                   : net.RecvPacket(inst, out.data() + 32, &received);
    return Check(len == static_cast<int>(frame.size()) && received == stamp
                 && Copied(out, frame), "frame length, timestamp, body or guards differ");
}

bool PacketWrap(bool exact = false)
{
    LocalMP net;
    net.SetRecvTimeout(1);
    for (int inst : {0, 1, 2}) net.Begin(inst);
    std::vector<std::vector<u8>> frames(1);
    // Instance 2 stops receiving while 1 consumes every complete frame.
    // Vary lengths so both headers and bodies straddle the ring boundary.
    const unsigned total = exact ? 128 : 180;
    for (unsigned id = 1; id <= total; ++id)
    {
        // 64 records of 1000 + 24 bytes exactly fill the 64 KiB ring.
        frames.push_back(Frame(id, exact ? 1000 :
                              (id % 3 == 0 ? kMaxFrameSize : 1000 + id % 89)));
        auto& frame = frames.back();
        if (!Check(net.SendPacket(0, frame.data(), frame.size(), 1000 + id)
                   == static_cast<int>(frame.size()), "send stalled or failed")
            || !Receive(net, 1, frame, 1000 + id)) return false;
    }

    unsigned last = 0;
    for (unsigned count = 0; count <= total; ++count)
    {
        Output out;
        out.fill(Sentinel);
        u64 stamp = 0;
        int len = net.RecvPacket(2, out.data() + 32, &stamp);
        if (!len) break;
        if (!Check(stamp > 1000 + last && stamp <= 1000 + total,
                   "overwritten, duplicate or unordered packet header")) return false;
        last = static_cast<unsigned>(stamp - 1000);
        if (!Check(len == static_cast<int>(frames[last].size()) && Copied(out, frames[last]),
                   "overwritten partial packet reached receiver")) return false;
    }
    if (!Check(last == total, "paused receiver lost the newest complete packet after wrap")) return false;
    auto next = Frame(181, 2376);
    net.SendPacket(0, next.data(), next.size(), 1181);
    return Receive(net, 1, next, 1181) && Receive(net, 2, next, 1181);
}

bool ReplyWrap(bool exact = false)
{
    LocalMP net;
    net.SetRecvTimeout(1);
    net.Begin(0);
    net.Begin(1);
    auto cmd = Frame(1, 40);
    net.SendCmd(0, cmd.data(), cmd.size(), 1000);
    if (!Receive(net, 1, cmd, 1000, true)) return false;
    std::vector<std::vector<u8>> frames(1);
    const unsigned total = exact ? 128 : 180;
    for (unsigned id = 1; id <= total; ++id)
    {
        frames.push_back(Frame(id, exact ? 1000 : (id % 2 ? 1024 : 713)));
        auto& frame = frames.back();
        if (!Check(net.SendReply(1, frame.data(), frame.size(), 1000 + id, 1)
                   == static_cast<int>(frame.size()), "reply send stalled or failed")) return false;
    }
    unsigned last = 0;
    for (unsigned count = 0; count <= total; ++count)
    {
        Output out;
        out.fill(Sentinel);
        u16 mask = net.RecvReplies(0, out.data() + 32, 1000, 2);
        if (!mask) break;
        unsigned id = 0;
        std::memcpy(&id, out.data() + 32, sizeof(id));
        if (!Check(mask == 2 && id > last && id <= total,
                   "overwritten, duplicate or unordered reply header")) return false;
        if (!Check(Copied(out, frames[id]), "overwritten partial reply reached receiver")) return false;
        last = id;
    }
    if (!Check(last == total, "stopped host lost the newest complete reply after wrap")) return false;
    auto next = Frame(181, 40);
    net.SendReply(1, next.data(), next.size(), 1181, 1);
    Output out;
    out.fill(Sentinel);
    return Check(net.RecvReplies(0, out.data() + 32, 1181, 2) == 2 && Copied(out, next),
                 "reply delivery did not recover");
}

bool Roundtrip()
{
    LocalMP net;
    net.SetRecvTimeout(1);
    for (int inst : {0, 1, 2}) net.Begin(inst);
    auto cmd = Frame(1, 40);
    auto reply = Frame(2, 64);
    auto ack = Frame(3, 44);
    net.SendCmd(0, cmd.data(), cmd.size(), 1000);
    if (!Receive(net, 1, cmd, 1000, true) || !Receive(net, 2, cmd, 1000, true)) return false;
    net.SendReply(1, reply.data(), reply.size(), 1001, 1);
    // Wifi.cpp sends this blank when a client is not ready to reply.
    net.SendReply(2, nullptr, 0, 1001, 0);
    Output out;
    out.fill(Sentinel);
    if (!Check(net.RecvReplies(0, out.data() + 32, 1000, 6) == 2 && Copied(out, reply),
               "normal or blank AID0 reply contract changed")) return false;
    net.SendAck(0, ack.data(), ack.size(), 1002);
    if (!Receive(net, 1, ack, 1002, true) || !Receive(net, 2, ack, 1002, true)) return false;
    net.SendReply(1, reply.data(), reply.size(), 1003, 1);
    net.SendCmd(0, cmd.data(), cmd.size(), 2000);
    out.fill(Sentinel);
    return Check(net.RecvReplies(0, out.data() + 32, 2000, 2) == 0
                 && std::all_of(out.begin(), out.end(), [](u8 b) { return b == Sentinel; }),
                 "previous command reply survived the next command");
}

bool Lifecycle()
{
    LocalMP net;
    net.SetRecvTimeout(1);
    net.Begin(0);
    net.Begin(1);
    auto frame = Frame(1, 80);
    net.SendCmd(0, frame.data(), frame.size(), 1000);
    if (!Receive(net, 1, frame, 1000, true)) return false;
    net.SendPacket(0, frame.data(), frame.size(), 1001);
    net.End(1);
    Output out;
    out.fill(Sentinel);
    u64 stamp = 0;
    if (!Check(net.RecvPacket(1, out.data() + 32, &stamp) == 0,
               "ended receiver consumed a queued packet")) return false;
    for (int i = 0; i < 100; ++i)
        net.SendPacket(0, frame.data(), frame.size(), 1002 + i);
    net.Begin(1);
    if (!Check(net.RecvPacket(1, out.data() + 32, &stamp) == 0,
               "rejoined receiver consumed old packets")) return false;
    net.SendCmd(0, frame.data(), frame.size(), 2000);
    if (!Receive(net, 1, frame, 2000, true)) return false;
    net.End(0);
    if (!Check(net.RecvHostPacket(1, out.data() + 32, &stamp) == -1,
               "host departure was not reported")) return false;
    if (!Check(net.SendReply(1, frame.data(), frame.size(), 2001, 1) == 0,
               "reply accepted for an ended host")) return false;
    net.Begin(0);
    net.Begin(1);
    net.SendCmd(0, frame.data(), frame.size(), 3000);
    if (!Receive(net, 1, frame, 3000, true)) return false;
    net.SendReply(1, frame.data(), frame.size(), 3001, 1);
    out.fill(Sentinel);
    return Check(net.RecvReplies(0, out.data() + 32, 3000, 2) == 2 && Copied(out, frame),
                 "command/reply failed after end/rejoin");
}

bool NewHostSession()
{
    LocalMP net;
    net.SetRecvTimeout(1);
    net.Begin(0);
    net.Begin(1);
    auto frame = Frame(1, 40);
    net.SendCmd(0, frame.data(), frame.size(), 1000);
    if (!Receive(net, 1, frame, 1000, true)) return false;
    net.End(0);
    // A new participant must not inherit another receiver's remembered host.
    net.Begin(2);
    Output out;
    u64 stamp = 0;
    if (!Check(net.RecvHostPacket(2, out.data() + 32, &stamp) == 0,
               "new participant inherited a departed host")) return false;
    net.End(1);
    net.Begin(1);
    if (!Check(net.RecvHostPacket(1, out.data() + 32, &stamp) == 0,
               "resumed participant retained previous session host")) return false;
    net.SendCmd(2, frame.data(), frame.size(), 2000);
    return Receive(net, 1, frame, 2000, true);
}
}

int main(int argc, char** argv)
{
    if (argc != 2) return 2;
    const std::string_view name = argv[1];
    bool ok = false;
    if (name == "packet-wrap") ok = PacketWrap();
    else if (name == "reply-wrap") ok = ReplyWrap();
    else if (name == "packet-exact-wrap") ok = PacketWrap(true);
    else if (name == "reply-exact-wrap") ok = ReplyWrap(true);
    else if (name == "roundtrip") ok = Roundtrip();
    else if (name == "lifecycle") ok = Lifecycle();
    else if (name == "new-host-session") ok = NewHostSession();
    else return 2;
    std::printf("%s: %s\n", argv[1], ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
