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
    std::puts("PASS real loopback handshake, MP frame, leave/rejoin, host loss, snapshots and interface lifetime");
}
