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
    static bool ReceiveHoldsMutex(LAN& net, const std::atomic<bool>& done)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (!done.load() && std::chrono::steady_clock::now() < deadline)
        {
            if (!net.SessionMutex.try_lock()) return !done.load();
            net.SessionMutex.unlock();
            std::this_thread::yield();
        }
        return false;
    }

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

bool ReceiveWaits()
{
    using Clock = std::chrono::steady_clock;
    const auto elapsedMS = [](Clock::time_point start) {
        return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
    };
    LAN host, client;
    auto connect = [&]
    {
        if (!client.StartClient("Wait client", "127.0.0.1")) return false;
        client.EndDiscovery();
        if (!Pump(host, client, [&] { return client.GetClientState() == LAN::ClientState::Connected; }))
            return false;
        host.Begin(0);
        client.Begin(0);
        return Pump(host, client, [&] {
            return host.GetNumPlayers() == 2 && client.GetNumPlayers() == 2 &&
                LANPacketTest::MeshReady(host, 0x3) && LANPacketTest::MeshReady(client, 0x3);
        });
    };
    if (!host.StartHost("Wait host", 2)) return false;
    host.EndDiscovery();
    if (!connect()) return false;

    bool passed = true;
    std::array<u8, 15 * 1024 + 64> untouched{};
    untouched.fill(0xA5);
    constexpr u64 untouchedStamp = 0xBADC0FFEE;
    std::array<u8, 40> command{};
    for (size_t i = 0; i < command.size(); ++i) command[i] = u8(i * 7 + 3);
    constexpr u64 commandStamp = 123456;
    auto expected = untouched;
    std::copy(command.begin(), command.end(), expected.begin() + 32);

    for (const bool replies : {false, true})
    {
        LAN& receiving = replies ? host : client;
        receiving.SetRecvTimeout(1000);
        auto output = untouched;
        u64 stamp = untouchedStamp;
        int result = -99;
        double receiveMS = 0;
        std::atomic<bool> done{false};
        std::thread receiver([&] {
            const auto start = Clock::now();
            result = replies ? receiving.RecvReplies(0, output.data() + 32, stamp, 1 << 1)
                             : receiving.RecvHostPacket(0, output.data() + 32, &stamp);
            receiveMS = elapsedMS(start);
            done.store(true);
        });
        // Only the receive thread touches this LAN object until cancellation.
        // Lock contention proves it entered the receive; elapsed sleeps do not.
        const bool held = LANPacketTest::ReceiveHoldsMutex(receiving, done);
        const bool inFlight = held && !done.load();
        const auto cancelStart = Clock::now();
        receiving.EndSession();
        const double cancelMS = elapsedMS(cancelStart);
        receiver.join();
        const bool cancelled = inFlight && cancelMS < 250 && result == 0 &&
            output == untouched && stamp == untouchedStamp &&
            receiving.GetClientState() == LAN::ClientState::Idle && receiving.GetPlayerList().empty() &&
            receiving.GetNumPlayers() == 0 && receiving.GetMaxPlayers() == 0 &&
            receiving.GetReceiveStats().QueuedPackets == 0;
        std::printf("%s cancel_ms=%.3f receive_ms=%.3f mutex_held=%d result=%d %s\n",
            replies ? "RecvReplies" : "RecvHostPacket", cancelMS, receiveMS, int(inFlight), result,
            cancelled ? "PASS" : "FAIL");
        passed &= cancelled;

        if (replies)
        {
            client.EndSession();
            if (!host.StartHost("Wait host restarted", 2)) return false;
            host.EndDiscovery();
        }
        else if (!Pump(host, client, [&] { return host.GetNumPlayers() == 1; })) return false;
        if (!connect()) return false;
        client.SetRecvTimeout(25);
        output = untouched;
        stamp = untouchedStamp;
        if (host.SendCmd(0, command.data(), command.size(), commandStamp) != int(command.size()) ||
            !Pump(host, client, [&] {
                return client.RecvHostPacket(0, output.data() + 32, &stamp) == int(command.size());
            }) || output != expected || stamp != commandStamp)
        {
            std::fputs("FAIL valid command after receive cancellation and rejoin\n", stderr);
            return false;
        }
    }

    client.SetRecvTimeout(80);
    auto output = untouched;
    u64 stamp = untouchedStamp;
    const auto idleStart = Clock::now();
    const int idleResult = client.RecvHostPacket(0, output.data() + 32, &stamp);
    const double idleMS = elapsedMS(idleStart);
    const bool waited = idleResult == 0 && idleMS >= 50 && idleMS < 2000 &&
        output == untouched && stamp == untouchedStamp;
    std::printf("configured_wait_ms=80 actual_ms=%.3f result=%d %s\n",
        idleMS, idleResult, waited ? "PASS" : "FAIL");
    passed &= waited;

    client.SetRecvTimeout(1000);
    output = untouched;
    stamp = untouchedStamp;
    std::atomic<bool> done{false};
    bool held = false;
    int sent = 0;
    double sendMS = 0;
    const auto delayedStart = Clock::now();
    std::thread sender([&] {
        held = LANPacketTest::ReceiveHoldsMutex(client, done);
        if (!held) return;
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
        sent = host.SendCmd(0, command.data(), command.size(), commandStamp);
        sendMS = elapsedMS(delayedStart);
    });
    const int delayedResult = client.RecvHostPacket(0, output.data() + 32, &stamp);
    const double delayedMS = elapsedMS(delayedStart);
    done.store(true);
    sender.join();
    const bool arrived = held && sent == int(command.size()) && delayedResult == int(command.size()) &&
        delayedMS >= 50 && delayedMS < 2000 && output == expected && stamp == commandStamp;
    std::printf("configured_wait_ms=1000 delayed_send_ms=%.3f receive_ms=%.3f result=%d %s\n",
        sendMS, delayedMS, delayedResult, arrived ? "PASS" : "FAIL");
    return passed && arrived;
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
    if (!ReceiveWaits())
    {
        std::fputs("FAIL receive cancellation, rejoin or configured wait preservation\n", stderr);
        return 11;
    }
    std::puts("PASS real loopback handshake, MP frame, leave/rejoin, host loss, snapshots and interface lifetime");
    std::puts("PASS four-participant mesh, late readiness, client replies and reused player slot");
    std::puts("PASS blocked host/reply cancellation, rejoin and untruncated receive waits");
}
