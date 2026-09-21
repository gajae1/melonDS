// SPDX-License-Identifier: GPL-3.0-or-later
// Net driver availability contract used by the frontend: the packet path
// reports "Wi-Fi unavailable" only when HasActiveDriver() is false, and only
// once per driver change via WarnUnavailableOnce().
#include "Net.h"
#include <cstdio>
#include <cstring>
#include <memory>

using namespace melonDS;

namespace
{
class FakeDriver : public NetDriver
{
public:
    explicit FakeDriver(bool active) noexcept : Active(active) {}
    [[nodiscard]] bool IsActive() const noexcept override { return Active; }
    int SendPacket(u8*, int) noexcept override { return 0; }
    void RecvCheck() noexcept override {}
private:
    bool Active;
};
}

int main(int argc, char** argv)
{
    const char* mode = argc > 1 ? argv[1] : "availability";

    Net net;
    if (std::strcmp(mode, "availability") == 0)
    {
        if (net.HasActiveDriver()) return 1;
        net.SetDriver(std::make_unique<FakeDriver>(true));
        if (!net.HasActiveDriver()) return 2;
        net.SetDriver(std::make_unique<FakeDriver>(false));
        if (net.HasActiveDriver()) return 3;
        net.SetDriver(nullptr);
        if (net.HasActiveDriver()) return 4;
        return 0;
    }
    if (std::strcmp(mode, "warn-once") == 0)
    {
        if (!net.WarnUnavailableOnce()) return 1;
        if (net.WarnUnavailableOnce()) return 2;
        // Any driver change re-arms the latch, including clearing it.
        net.SetDriver(std::make_unique<FakeDriver>(true));
        if (!net.WarnUnavailableOnce()) return 3;
        if (net.WarnUnavailableOnce()) return 4;
        net.SetDriver(nullptr);
        if (!net.WarnUnavailableOnce()) return 5;
        return 0;
    }
    if (std::strcmp(mode, "inert") == 0)
    {
        // No driver and a dead driver both stay inert on the packet path.
        u8 buf[64] = {};
        if (net.SendPacket(buf, sizeof(buf), 0) != 0) return 1;
        if (net.RecvPacket(buf, 0) != 0) return 2;
        net.SetDriver(std::make_unique<FakeDriver>(false));
        if (net.SendPacket(buf, sizeof(buf), 0) != 0) return 3;
        if (net.RecvPacket(buf, 0) != 0) return 4;
        return 0;
    }
    return 64;
}
