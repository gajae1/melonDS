// SPDX-License-Identifier: GPL-3.0-or-later
#include <atomic>
#include <cstdio>
#include <thread>
#include "types.h"
using namespace melonDS;

struct InputState
{
    std::atomic<u64> touchInput{0};
    void touchScreen(int x, int y);
    void releaseScreen();
    bool inputGetTouch(u16& x, u16& y);
};
#define EmuInstance InputState
#include "touchPublish.inc"
#include "touchRelease.inc"
#include "touchRead.inc"
#undef EmuInstance

int main()
{
    InputState input;
    u16 x = 0, y = 0;
    if (input.inputGetTouch(x, y)) return 1;
    input.touchScreen(65535, 12345);
    if (!input.inputGetTouch(x, y) || x != 65535 || y != 12345) return 1;
    input.releaseScreen();
    if (input.inputGetTouch(x, y)) return 1;
    input.touchScreen(17, 31);

    std::atomic<bool> ready{false}, done{false};
    std::thread writer([&] {
        while (!ready.load()) std::this_thread::yield();
        for (int i = 0; i < 200000; ++i)
        {
            input.touchScreen(213, 179);
            input.releaseScreen();
            input.touchScreen(17, 31);
        }
        done.store(true);
    });
    unsigned mixed = 0, reads = 0;
    ready.store(true);
    do
    {
        if (input.inputGetTouch(x, y) &&
            !((x == 17 && y == 31) || (x == 213 && y == 179)))
            ++mixed;
        ++reads;
    } while (!done.load());
    writer.join();
    input.releaseScreen();
    const bool stillPressed = input.inputGetTouch(x, y);
    std::printf("Touch publication: %u mixed pairs in %u reads; released=%d\n",
                mixed, reads, !stillPressed);
    return mixed || stillPressed ? 1 : 0;
}
