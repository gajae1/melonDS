// SPDX-License-Identifier: GPL-3.0-or-later
// Real Qt message publication/stop-token methods and ARM7 VBlank cheat hook.
// A controlled worker acknowledges the queue after the core returns. The full
// dispatcher has separate StateLoadMessages coverage; no GUI/devices are used.
#include <atomic>
#include <list>
#include <optional>
#include <variant>
#include <stop_token>
#include <semaphore>
#include <thread>
#include <cstdio>
#include <string_view>
#include <QCoreApplication>
#include <QThread>
#include <QMutex>
#include <QSemaphore>
#include <QWaitCondition>
#include <QQueue>
#include <QVariant>
#include "Args.h"
#include "NDS.h"
#include "ARM.h"
#include "NDSCart.h"
#include "GBACart.h"
#define private public
#include "EmuThread.h"
#undef private

using namespace melonDS;
void EmuThread::run() {}
#include "stateThreadConstructor.inc"
#include "cheatSendMessage.inc"
#include "cheatWaitMessage.inc"
#include "cheatStopToken.inc"

struct ObservedNDS : NDS
{
    using NDS::NDS;
    std::binary_semaphore firstWrite{0}, queuePublished{0};
    bool recording = false, holdFirstWrite = false;
    unsigned writes = 0;
    void ARM7Write32(u32 address, u32 value) override
    {
        if (recording && ++writes == 1 && holdFirstWrite)
        {
            firstWrite.release();
            queuePublished.acquire();
        }
        NDS::ARM7Write32(address, value);
    }
    void Execute()
    {
        writes = 0;
        IF[1] = IE[1] = 1u << IRQ_VBlank;
        ARM7.CPSR &= ~0x80;
        recording = true;
        ARM7.TriggerIRQ();
        recording = false;
    }
};

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    const std::string_view mode = argv[1];
    EmuThread thread(nullptr);
    NDSArgs args;
    args.JIT = std::nullopt;
    auto core = std::make_unique<ObservedNDS>(std::move(args));
    core->Reset();
    int failures = 0;
    const auto check = [&](bool ok, const char* why) {
        if (!ok) { ++failures; std::fprintf(stderr, "%s\n", why); }
    };
    core->AREngine.Cheats = {{.Parent = nullptr, .Name = "First", .Description = "", .Enabled = true,
        .Code = {0x02000100, 0x11223344}}};
    if (mode == "pending")
    {
        thread.sendMessage(EmuThread::msg_EmuPause);
        core->AREngine.SetStopToken(thread.cheatStopToken());
        core->Execute();
        check(core->writes == 0 && core->AREngine.Cheats[0].Enabled && core->AREngine.TakeErrors().empty(),
              "A request published before the frame was lost or disabled untouched code");
        QMutexLocker lock(&thread.msgMutex);
        check(thread.msgQueue.size() == 1, "Token acquisition consumed queued work");
        thread.msgQueue.clear();
    }
    else
    {
        const auto type = mode == "pause" ? EmuThread::msg_EmuPause :
            mode == "stop" ? EmuThread::msg_EmuStop : EmuThread::msg_Exit;
        if (mode != "pause" && mode != "stop" && mode != "exit") return 2;
        core->AREngine.Cheats[0].Code = mode == "stop" ?
            std::vector<u32>{0xD3000000, 0x02000100, 0xF2000200, 0xffffffff} :
            std::vector<u32>{0xC0000000, 0xffffffff, 0x02000200, 0x11223344, 0xD2000000, 0};
        core->AREngine.Cheats.push_back({.Parent = nullptr, .Name = "Later", .Description = "", .Enabled = true,
            .Code = {0x02000300, 0xabcdef01}});
        core->holdFirstWrite = true;
        core->AREngine.SetStopToken(thread.cheatStopToken());
        std::jthread worker([&] {
            core->Execute();
            // Only acknowledge after the production cheat hook yields. This
            // models the queue boundary, not pause/stop/exit device effects.
            QMutexLocker lock(&thread.msgMutex);
            check(thread.msgQueue.size() == 1 && thread.msgQueue.front().type == type,
                  "Cancellation lost or changed the queued UI message");
            thread.msgQueue.clear();
            thread.msgSemaphore.release();
        });
        core->firstWrite.acquire();
        thread.sendMessage(type);
        core->queuePublished.release();
        thread.waitMessage();
        worker.join();
        const auto errors = core->AREngine.TakeErrors();
        check(core->writes == 1 && !core->AREngine.Cheats[0].Enabled && core->AREngine.Cheats[1].Enabled &&
              core->ARM7Read32(0x02000300) == 0 && errors.size() == 1 &&
              errors[0].Reason == AREngine::Result::Interrupted, "Queued work did not interrupt only the running code");
    }
    // Public publication contract: after draining, a new frame can run cheats.
    core->AREngine.SetStopToken(thread.cheatStopToken());
    core->holdFirstWrite = false;
    core->AREngine.Cheats = {{.Parent = nullptr, .Name = "Fresh", .Description = "", .Enabled = true,
        .Code = {0x02000100, 0x55667788}}};
    core->Execute();
    check(core->writes == 1 && core->ARM7Read32(0x02000100) == 0x55667788,
          "Drained queue left a stale stop request in the next frame");
    std::printf("Cheat message cancellation %s: %d failures\n", argv[1], failures);
    return failures ? 1 : 0;
}
