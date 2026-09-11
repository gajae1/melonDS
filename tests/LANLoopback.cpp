// SPDX-License-Identifier: GPL-3.0-or-later
// Real ENet handshake and MP frames on IPv4 loopback. Discovery is closed before
// processing, so the fixture sends no broadcast and reads no user configuration.
#include "net/LAN.h"
#include <chrono>
#include <cstdio>
#include <thread>
#include <array>
#include <atomic>

namespace melonDS::Platform
{
u64 GetMSCount()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
}

using namespace melonDS;

namespace melonDS
{
struct LANPacketTest
{
    static bool MeshReady(LAN& net, u16 mask)
    {
        if ((net.ConnectedBitmask & mask) != mask) return false;
        for (int id = 0; id < 16; ++id)
        {
            if (!(mask & (1 << id)) || id == net.MyPlayer.ID) continue;
            if (!net.RemotePeers[id] || net.RemotePeers[id]->state != ENET_PEER_STATE_CONNECTED)
                return false;
        }
        return true;
    }
};
}

template<class Predicate>
bool Pump(LAN& host, LAN& client, Predicate done)
{
    const auto end = Platform::GetMSCount() + 2000;
    while (Platform::GetMSCount() < end)
    {
        host.Process();
        client.Process();
        if (done()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
}

bool Mesh()
{
    LAN host, one, two, three;
    std::array<LAN*, 4> nodes{&host, &one, &two, &three};
    auto pump = [&](auto done)
    {
        const auto end = Platform::GetMSCount() + 2000;
        while (Platform::GetMSCount() < end)
        {
            for (auto* node : nodes) node->Process();
            if (done()) return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return false;
    };
    auto join = [&](LAN& client, const char* name)
    {
        return client.StartClient(name, "127.0.0.1") && pump([&]
        {
            return client.GetClientState() == LAN::ClientState::Connected;
        });
    };
    auto ready = [&](u16 mask)
    {
        return pump([&]
        {
            for (int id = 0; id < 4; ++id)
                if ((mask & (1 << id)) && !LANPacketTest::MeshReady(*nodes[id], mask)) return false;
            return true;
        });
    };
    auto frame = [&](LAN& sender, LAN& receiver, u8 value)
    {
        std::array<u8, 40> input{}, output{};
        input.fill(value);
        constexpr u64 expectedStamp = 123456;
        if (sender.SendCmd(0, input.data(), input.size(), expectedStamp) != int(input.size())) return false;
        u64 stamp = 0;
        return pump([&] { return receiver.RecvHostPacket(0, output.data(), &stamp) == int(output.size()); }) &&
            input == output && stamp == expectedStamp;
    };
    if (!host.StartHost("Mesh host", 4)) return false;
    host.EndDiscovery();
    if (!join(one, "First")) return false;
    host.Begin(0);
    one.Begin(0);
    if (!ready(0x3) || !join(two, "Second")) return false;
    two.Begin(0);
    // Existing participants already called Begin before the new mesh link exists.
    if (!ready(0x7) || !frame(one, two, 0x53)) return false;
    std::array<u8, 40> reply{};
    reply.fill(0xA6);
    std::array<u8, 15 * 1024> replies{};
    if (two.SendReply(0, reply.data(), reply.size(), 123456, 3) != int(reply.size()) ||
        !pump([&] { return one.RecvReplies(0, replies.data(), 123456, 1 << 3) == (1 << 3); }) ||
        !std::equal(reply.begin(), reply.end(), replies.begin() + 2 * 1024)) return false;
    // NP-03: a command that ran at timestamp 0 must still collect its reply;
    // the unsigned stale window may not wrap and drop it.
    std::array<u8, 40> early{};
    early.fill(0xC3);
    std::array<u8, 15 * 1024> window{};
    if (two.SendReply(0, early.data(), early.size(), 0, 3) != int(early.size()) ||
        !pump([&] { return one.RecvReplies(0, window.data(), 0, 1 << 3) == (1 << 3); }) ||
        !std::equal(early.begin(), early.end(), window.begin() + 2 * 1024)) return false;
    // Age 33 stays rejected while exact 32 stays fresh, without relying on
    // the delivery order of the two unsequenced frames.
    if (two.SendReply(0, early.data(), early.size(), 123456 - 33, 3) != int(early.size()))
        return false;
    const auto staleDeadline = Platform::GetMSCount() + 200;
    while (Platform::GetMSCount() < staleDeadline)
    {
        for (auto* node : nodes) node->Process();
        if (one.RecvReplies(0, window.data(), 123456, 1 << 3) != 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (two.SendReply(0, reply.data(), reply.size(), 123456 - 32, 3) != int(reply.size()) ||
        !pump([&] { return one.RecvReplies(0, window.data(), 123456, 1 << 3) == (1 << 3); }) ||
        !std::equal(reply.begin(), reply.end(), window.begin() + 2 * 1024)) return false;
    if (!frame(two, one, 0x27) || !join(three, "Third")) return false;
    three.Begin(0);
    // Three clients share an IPv4 address but have different ENet endpoints.
    if (!ready(0xF) || !frame(two, three, 0x39)) return false;
    one.EndSession();
    if (!pump([&] { return host.GetNumPlayers() == 3 && two.GetNumPlayers() == 3 && three.GetNumPlayers() == 3; }))
        return false;
    if (!join(one, "Replacement")) return false;
    one.Begin(0);
    if (!ready(0xF) || !frame(one, two, 0x71)) return false;
    host.EndSession();
    return pump([&]
    {
        return one.GetClientState() == LAN::ClientState::Disconnected &&
            two.GetClientState() == LAN::ClientState::Disconnected &&
            three.GetClientState() == LAN::ClientState::Disconnected;
    });
}

int main()
{
    LAN host, client;
    if (!host.StartHost("Loopback host", 2))
    {
        std::fprintf(stderr, "Cannot bind loopback test host (port 7064).\n");
        return 1;
    }
    host.EndDiscovery();
    auto connect = [&]
    {
        return client.StartClient("Loopback client", "127.0.0.1") &&
            Pump(host, client, [&] { return client.GetClientState() == LAN::ClientState::Connected; }) &&
            client.GetNumPlayers() == 2 && host.GetNumPlayers() == 2;
    };
    if (!connect()) return 2;
    host.Begin(0);
    client.Begin(0);
    // The control and MP channels are independently sequenced.
    std::array<u8, 40> input{}, output{};
    input.fill(0x5A);
    host.SendCmd(0, input.data(), input.size(), 123456);
    u64 stamp = 0;
    if (!Pump(host, client, [&] {
        return client.RecvHostPacket(0, output.data(), &stamp) == int(output.size());
    }) || input != output || stamp != 123456) return 3;

    client.EndSession();
    if (!Pump(host, client, [&] { return host.GetNumPlayers() == 1; }) || !connect()) return 4;

    // Snapshots race with event processing and cancellation in the real UI.
    std::atomic<bool> stop{false};
    std::thread reader([&] {
        while (!stop.load())
        {
            client.GetPlayerList();
            client.GetMaxPlayers();
            client.Process();
        }
    });
    host.EndSession();
    const bool lost = Pump(host, client, [&] { return client.GetClientState() == LAN::ClientState::Disconnected; });
    stop.store(true);
    reader.join();
    if (!lost || !client.GetPlayerList().empty()) return 5;

    if (!host.StartHost("Restart", 2)) return 6;
    host.EndDiscovery();
    if (!connect()) return 7;
    client.EndSession();
    host.EndSession();
    if (client.GetNumPlayers() || client.GetMaxPlayers()) return 8;

    // Interface ownership survives replacement until the active caller returns.
    MPInterface::Set(MPInterface_LAN);
    auto retained = MPInterface::Acquire();
    MPInterface::Set(MPInterface_Local);
    retained->Process();
    if (retained == MPInterface::Acquire()) return 9;
    retained.reset();
    MPInterface::Set(MPInterface_Dummy);
    if (!Mesh())
    {
        std::fputs("FAIL client mesh, late readiness, reply routing or reused player slot\n", stderr);
        return 10;
    }
    std::puts("PASS real loopback handshake, MP frame, leave/rejoin, host loss, snapshots and interface lifetime");
    std::puts("PASS four-participant mesh, late readiness, client replies and reused player slot");
}
