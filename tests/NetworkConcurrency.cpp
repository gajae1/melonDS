// SPDX-License-Identifier: GPL-3.0-or-later
#include "net/Net.h"
#include <array>
#include <atomic>
#include <barrier>
#include <cstdio>
#include <cstring>
#include <thread>
using namespace melonDS;
namespace {
struct Recorder : NetDriver {
    std::atomic<unsigned> active{0};
    std::atomic<bool> overlap{false};
    void Call() noexcept {
        if (active.fetch_add(1) != 0) overlap = true;
        for (int i = 0; i < 8; ++i) std::this_thread::yield();
        active.fetch_sub(1);
    }
    int SendPacket(u8*, int length) noexcept override { Call(); return length; }
    void RecvCheck() noexcept override { Call(); }
};
int Queues() {
    PacketDispatcher q;
    std::array<u8, 128> out{};
    int length = 7;
    if (q.recvPacket(nullptr, nullptr, out.data(), &length, 0)) return 1;
    q.registerInstance(-1); q.registerInstance(16); q.unregisterInstance(16);
    q.registerInstance(0); q.registerInstance(15);
    const u8 payload[] = {1,2,3,4};
    q.sendPacket(nullptr, 0, payload, 4, 0, 0xffff);
    if (q.recvPacket(nullptr,nullptr,out.data(),&length,0)) return 2;
    if (!q.recvPacket(nullptr,nullptr,out.data(),&length,15) || length != 4 || memcmp(out.data(),payload,4)) return 3;
    q.sendPacket(nullptr,0,payload,-1,16,0xffff);
    if (q.recvPacket(nullptr,nullptr,out.data(),&length,15)) return 4;
    q.unregisterInstance(15);
    q.sendPacket(nullptr,0,payload,4,16,0xffff);
    if (q.recvPacket(nullptr,nullptr,out.data(),&length,15)) return 5;
    if (!q.recvPacket(nullptr,nullptr,out.data(),&length,0)) return 6;
    return 0;
}
int Threads() {
    Net net;
    net.RegisterInstance(0); net.RegisterInstance(1);
    auto driver = std::make_unique<Recorder>(); auto* recorder = driver.get();
    net.SetDriver(std::move(driver));
    std::barrier start(3);
    std::thread sender([&] { u8 data[4]{}; start.arrive_and_wait(); for(int i=0;i<1000;++i) net.SendPacket(data,4,0); });
    std::thread receiver([&] { u8 data[4]{}; start.arrive_and_wait(); for(int i=0;i<1000;++i) net.RecvPacket(data,1); });
    start.arrive_and_wait(); sender.join(); receiver.join();
    return recorder->overlap ? 10 : 0;
}
}
int main(int argc, char** argv) {
    const int result = argc > 1 && std::strcmp(argv[1],"threads") == 0 ? Threads() : Queues();
    if (result) std::fprintf(stderr,"Network regression failed: %d\n", result);
    return result;
}
