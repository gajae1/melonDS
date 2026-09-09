// SPDX-License-Identifier: GPL-3.0-or-later
// Generated codes enter through the real ARM7 VBlank IRQ hook. All memory
// accesses use the production core; no ROM, BIOS dump or saved cheats are read.
#include "Args.h"
#include "NDS.h"
#include "ARM.h"
#include <array>
#include <cstdio>
#include <memory>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

using namespace melonDS;
struct ObservedNDS : NDS
{
    using NDS::NDS;
    struct Write { u32 address; unsigned width; u32 value; };
    std::vector<Write> writes;
    bool recording = false;
    std::stop_source* cancel = nullptr;
    void Record(u32 address, unsigned width, u32 value)
    {
        if (!recording) return;
        writes.push_back({address, width, value});
        if (cancel && writes.size() == 3) cancel->request_stop();
    }
    void ARM7Write8(u32 address, u8 value) override
    { Record(address, 8, value); NDS::ARM7Write8(address, value); }
    void ARM7Write16(u32 address, u16 value) override
    { Record(address, 16, value); NDS::ARM7Write16(address, value); }
    void ARM7Write32(u32 address, u32 value) override
    { Record(address, 32, value); NDS::ARM7Write32(address, value); }
    void Execute(std::vector<u32> words)
    {
        AREngine.Cheats = {{.Parent = nullptr, .Name = "Generated regression", .Description = "",
                           .Enabled = true, .Code = std::move(words)}};
        writes.clear();
        IF[1] = IE[1] = 1u << IRQ_VBlank;
        ARM7.CPSR &= ~0x80;
        recording = true;
        ARM7.TriggerIRQ();
        recording = false;
    }
};

int main(int argc, char** argv)
{
#ifdef _WIN32
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
#endif
    if (argc != 2) return 2;
    NDSArgs args;
    args.JIT = std::nullopt;
    auto core = std::make_unique<ObservedNDS>(std::move(args));
    core->Reset();
    const std::string_view mode = argv[1];
    int failures = 0;
    const auto check = [&](bool value, const char* why) {
        if (!value) { ++failures; std::fprintf(stderr, "%s\n", why); }
    };
    if (mode == "controls")
    {
        core->Execute({0x02000100, 0x12345678, 0x12000104, 0xabcdef, 0x22000106, 0x77,
                       0xE2000108, 7, 0x04030201, 0x08070605,
                       0xD3000000, 0x02000108, 0xF2000120, 7});
        check(core->ARM7Read32(0x02000100) == 0x12345678 && core->ARM7Read16(0x02000104) == 0xcdef &&
              core->ARM7Read8(0x02000106) == 0x77, "Ordinary 32/16/8 writes changed");
        for (u32 i = 0; i < 7; ++i)
            check(core->ARM7Read8(0x02000108 + i) == i + 1 && core->ARM7Read8(0x02000120 + i) == i + 1,
                  "Literal or guest-memory copy changed bytes");
        check(core->writes.size() == 11 && core->writes[3].width == 32 && core->writes[4].width == 8,
              "Literal tail access widths changed");
        core->Execute({0x52000180, 1, 0xE2000180, 8, 0xaaaaaaaa, 0xbbbbbbbb,
                       0xD0000000, 0, 0x02000184, 0xabcdef01});
        check(core->writes.size() == 1 && core->ARM7Read32(0x02000180) == 0 &&
              core->ARM7Read32(0x02000184) == 0xabcdef01, "Conditional literal skip changed execution");
    }
    else if (mode == "loop-control")
    {
        core->Execute({0xD5000000, 0x22, 0xC0000000, 2, 0xD4000000, 1,
                       0xD6000000, 0x02000180, 0xD2000000, 0});
        for (u32 i = 0; i < 3; ++i)
            check(core->ARM7Read32(0x02000180 + 4 * i) == 0x23 + i, "C0 inclusive loop or D2 changed");
        check(core->writes.size() == 3, "Counted loop wrote extra values");
    }
    else if (mode == "empty")
    {
        core->Execute({});
        check(core->writes.empty(), "An empty code changed guest memory");
    }
    else if (mode == "odd" || mode == "literal-short" || mode == "literal-overflow" || mode == "skipped-overflow")
    {
        if (mode == "odd") core->Execute({0x02000100, 0x11111111, 0x02000104});
        else if (mode == "literal-short")
            core->Execute({0x02000100, 0x11111111, 0xE2000120, 9, 0x04030201, 0x08070605});
        else if (mode == "literal-overflow") core->Execute({0xE2000120, 0xffffffff});
        else core->Execute({0x52000180, 1, 0xE2000120, 0xfffffffc});
        check(core->writes.empty(), "Malformed code applied a partial guest write");
        check(core->ARM7Read32(0x02000100) == 0, "Malformed code changed a valid prefix before rejection");
    }
    else if (mode == "cancel-loop" || mode == "cancel-copy" || mode == "cancel-literal")
    {
        std::stop_source stop;
        core->cancel = &stop;
        core->AREngine.SetStopToken(stop.get_token());
        if (mode == "cancel-loop")
            core->Execute({0xD5000000, 0x11223344, 0xC0000000, 0xffffffff,
                           0xD6000000, 0x02000180, 0xD2000000, 0});
        else if (mode == "cancel-copy")
            core->Execute({0xD3000000, 0x02000100, 0xF2000200, 0xffffffff});
        else
            core->Execute({0xE2000200, 32, 1, 2, 3, 4, 5, 6, 7, 8, 0x02000100, 0xabcdef01});
        check(core->writes.size() >= 3 && core->writes.size() <= 4 &&
              !core->AREngine.Cheats.front().Enabled, "Requested cancellation did not bound in-flight writes");
        const auto errors = core->AREngine.TakeErrors();
        check(errors.size() == 1 && errors[0].Reason == AREngine::Result::Interrupted &&
              errors[0].Name == "Generated regression", "Interrupted code was not reported once");
        check(core->AREngine.TakeErrors().empty(), "Consumed error was reported again");
        core->cancel = nullptr;
        core->AREngine.SetStopToken({});
        core->Execute({0x02000100, 0x55667788});
        check(core->ARM7Read32(0x02000100) == 0x55667788 && core->AREngine.Cheats.front().Enabled,
              "Fresh execution retained the previous stop request");
    }
    else if (mode == "pending-cancel")
    {
        std::stop_source stop;
        stop.request_stop();
        core->AREngine.SetStopToken(stop.get_token());
        core->Execute({0x02000100, 0x55667788});
        check(core->writes.empty() && core->AREngine.Cheats.front().Enabled && core->AREngine.TakeErrors().empty(),
              "Cancellation before execution disabled an untouched code");
    }
    else if (mode == "unsupported")
    {
        for (u32 opcode : {0xC4000000u, 0xD4000009u, 0xCF000000u})
        {
            core->Execute({opcode, 0, 0x02000100, 0x11223344});
            const auto errors = core->AREngine.TakeErrors();
            check(core->writes.empty() && !core->AREngine.Cheats.front().Enabled && errors.size() == 1 &&
                  errors[0].Reason == AREngine::Result::UnsupportedCode,
                  "An unsupported opcode was silently continued or not reported");
        }
    }
    else return 2;
    std::printf("AR execution %s: %d failures\n", argv[1], failures);
    return failures ? 1 : 0;
}
