// SPDX-License-Identifier: GPL-3.0-or-later
// As in LANPacket.cpp, inject only a host scheduling boundary. All queue bytes,
// cursors and semaphore operations belong to the production implementation.
#include "net/LocalMP.h"
#include <array>
#include <cstdio>
#include <functional>
#include <string_view>
#include <utility>

namespace
{
std::function<void()> AfterAcquire;
bool InterleavedTryWait(melonDS::Platform::Semaphore* sem, int timeout)
{
    bool acquired = melonDS::Platform::Semaphore_TryWait(sem, timeout);
    if (acquired && AfterAcquire)
    {
        // Simulate the receiver being descheduled after taking a permit but
        // before acquiring MPQueueLock; run the other participant's operation.
        auto run = std::exchange(AfterAcquire, {});
        run();
    }
    return acquired;
}
}

#define Semaphore_TryWait InterleavedTryWait
#include "../src/net/LocalMP.cpp"
#undef Semaphore_TryWait

int main(int argc, char** argv)
{
    using namespace melonDS;
    if (argc != 2) return 2;
    const bool reply = std::string_view(argv[1]) == "reply";
    LocalMP net;
    net.SetRecvTimeout(1);
    net.Begin(0);
    net.Begin(1);
    // 64 x (24-byte header + 1000-byte body) places the write cursor exactly
    // on a valid but no longer pending header. A stale permit cannot read it.
    std::array<u8, 1000> frame{};
    if (reply) net.SendCmd(0, frame.data(), 40, 1000);
    for (unsigned i = 0; i < 64; ++i)
    {
        if (reply) net.SendReply(1, frame.data(), frame.size(), 1000, 1);
        else net.SendPacket(0, frame.data(), frame.size(), 1000);
    }
    AfterAcquire = [&] {
        if (reply) net.SendCmd(0, frame.data(), 40, 1001);
        else net.Begin(1);
    };
    std::array<u8, 15 * 1024> out;
    out.fill(0xA5);
    u64 stamp = 0;
    const int result = reply ? net.RecvReplies(0, out.data(), 1000, 2)
                             : net.RecvPacket(1, out.data(), &stamp);
    bool ok = result == 0 && stamp == 0;
    for (u8 byte : out) ok &= byte == 0xA5;
    if (!ok) std::fprintf(stderr, "in-flight permit delivered data discarded by session reset\n");
    // Both paths must deliver new data after the injected reset, then empty.
    if (reply)
    {
        net.SendReply(1, frame.data(), frame.size(), 1002, 1);
        ok &= net.RecvReplies(0, out.data(), 1002, 2) == 2;
        ok &= net.RecvReplies(0, out.data(), 1002, 2) == 0;
    }
    else
    {
        net.SendPacket(0, frame.data(), frame.size(), 1002);
        ok &= net.RecvPacket(1, out.data(), &stamp) == 1000 && stamp == 1002;
        ok &= net.RecvPacket(1, out.data(), &stamp) == 0;
    }
    std::printf("in-flight-%s-reset: %s\n", argv[1], ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
