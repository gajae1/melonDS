// SPDX-License-Identifier: GPL-3.0-or-later
#include "NDS.h"
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
using namespace melonDS;

int main()
{
    NDSArgs args;
    args.JIT.reset();
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    nds->ARM9.Halt(1);
    nds->ARM7.Halt(1);
    nds->Start();
    std::array<s16, 4096> samples;
    for (double speed : {1.0, 0.5, 1.0})
    {
        nds->SPU.DrainOutput();
        nds->SPU.SetOutputSkew(speed);
        int total = 0;
        for (int frame = 0; frame < 20; ++frame)
        {
            nds->RunFrame();
            total += nds->SPU.ReadOutput(samples.data(), samples.size() / 2);
        }
        const double expected = 20 * 48000.0 / 59.8260982880808 / speed;
        // One unfinished core audio batch can remain in blip at each boundary.
        if (std::abs(total - expected) > 400)
        {
            std::fprintf(stderr, "Audio clock %.1fx produced %d frames, expected %.0f\n", speed, total, expected);
            return 1;
        }
        std::printf("Audio clock %.1fx: %d output frames in 20 emulated frames\n", speed, total);
    }
    if (nds->SPU.GetOutputDroppedFrames() != 0) return 2;
    // Stop consuming: the actual bounded core FIFO must report overwritten
    // frames. Draining later must not erase that diagnostic evidence.
    for (int frame = 0; frame < 20; ++frame) nds->RunFrame();
    const u64 dropped = nds->SPU.GetOutputDroppedFrames();
    nds->SPU.DrainOutput();
    if (!dropped || nds->SPU.GetOutputDroppedFrames() != dropped) return 3;
    std::printf("Audio FIFO: overwritten frames=%llu; drain preserves the counter\n", (unsigned long long)dropped);
}
