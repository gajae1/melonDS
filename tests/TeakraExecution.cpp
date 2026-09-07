// SPDX-License-Identifier: GPL-3.0-or-later
// Executes the real interpreter and memory callbacks. Not a hardware oracle.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>
#include "interpreter.h"
#include "shared_memory.h"

struct TickRecorder : Teakra::CoreTiming::Callbacks {
    u64 ticks = 0;
    std::function<void(u64)> on_tick;
    void Tick() override { ++ticks; if(on_tick) on_tick(ticks); }
    u64 GetMaxSkip() const override { return 0; }
    void Skip(u64 n) override { ticks += n; }
};
struct Machine {
    std::vector<u16> words = std::vector<u16>(0x40000, 0);
    Teakra::CoreTiming timing;
    TickRecorder clock;
    Teakra::SharedMemory shared;
    Teakra::MemoryInterfaceUnit miu;
    Teakra::MemoryInterface memory{shared, miu};
    Teakra::RegisterState regs;
    Teakra::Interpreter interpreter{timing, regs, memory};
    u64 reads = 0, writes = 0;
    Machine() {
        timing.RegisterCallbacks(&clock);
        shared.SetExternalMemoryCallback(
            [this](u32 a) { ++reads; return words.at(a / 2); },
            [this](u32 a, u16 v) { ++writes; words.at(a / 2) = v; });
        // Match Processor::Reset: value-initialize shadow registers as well.
        regs.Reset();
        interpreter.Reset();
        regs.sp = 0x100;
    }
    std::vector<u8> Save() {
        melonDS::Savestate state(4096);
        state.Section("REGS"); regs.DoSavestate(&state);
        state.Section("EXEC"); interpreter.DoSavestate(&state);
        state.Finish();
        if(state.Error) std::abort();
        const auto* p = static_cast<const u8*>(state.Buffer());
        return {p, p + state.Length()};
    }
    void Load(std::vector<u8>& bytes) {
        melonDS::Savestate state(bytes.data(), bytes.size(), false);
        state.Section("REGS"); regs.DoSavestate(&state);
        state.Section("EXEC"); interpreter.DoSavestate(&state);
        if(state.Error) std::abort();
    }
};
static u64 Digest(const std::vector<u8>& bytes) {
    u64 h = 14695981039346656037ULL;
    for(auto c: bytes) { h ^= c; h *= 1099511628211ULL; }
    return h;
}
int main(int argc, char** argv) {
    if(argc > 1 && std::strcmp(argv[1], "bench") == 0) {
        Machine machine;
        constexpr unsigned cycles = 50000;
        constexpr int passes = 60;
        const auto start = std::chrono::steady_clock::now();
        for(int i=0;i<passes;++i) { machine.regs.pc = 0; machine.interpreter.Run(cycles); }
        const double ns = std::chrono::duration<double,std::nano>(std::chrono::steady_clock::now()-start).count()/(cycles*passes);
        std::printf("ns_per_instruction=%.4f ticks=%llu reads=%llu state=%016llx\n",ns,
            (unsigned long long)machine.clock.ticks,(unsigned long long)machine.reads,(unsigned long long)Digest(machine.Save()));
        return 0;
    }
    for(int scenario=0;scenario<6;++scenario) {
        Machine m;
        if(scenario==0) {
            m.interpreter.Run(40);
            if(m.regs.pc!=40 || m.clock.ticks!=40 || m.reads!=40) return 1;
        } else if(scenario==1) {
            m.regs.rep=true; m.regs.repc=7;
            m.interpreter.Run(8);
            if(m.regs.pc!=1 || m.regs.rep || m.clock.ticks!=8) return 2;
        } else if(scenario==2) {
            // Expanded MOV REPC,#value: two program words, one interpreter tick.
            m.words[0]=0x0001; m.words[1]=0x1234;
            m.interpreter.Run(1);
            if(m.regs.repc!=0x1234 || m.regs.pc!=2 || m.reads!=2 || m.clock.ticks!=1) return 3;
        } else if(scenario==3 || scenario==4) {
            m.regs.ie=1; m.regs.im[0]=1; m.regs.imv=1;
            m.clock.on_tick=[&](u64 tick) {
                if(tick==3) {
                    if(scenario==3) m.interpreter.SignalInterrupt(0);
                    else m.interpreter.SignalVectoredInterrupt(0x200,false);
                }
            };
            m.interpreter.Run(3);
            if(m.regs.pc!=3 || m.regs.ie!=1) return 4;
            m.interpreter.Run(1);
            if(m.regs.pc!=(scenario==3?6u:0x200u) || m.regs.ie!=0 || m.regs.sp!=0xfe || m.writes!=2 || m.clock.ticks!=4) return 5;
        } else {
            m.interpreter.SignalInterrupt(2);
            auto saved=m.Save();
            m.interpreter.Run(11); auto expected=m.Save();
            m.Load(saved); m.interpreter.Run(11);
            if(m.Save()!=expected || m.regs.ip[2]!=1) return 6;
        }
        std::printf("scenario=%d ticks=%llu reads=%llu writes=%llu state=%016llx\n",scenario,
            (unsigned long long)m.clock.ticks,(unsigned long long)m.reads,(unsigned long long)m.writes,
            (unsigned long long)Digest(m.Save()));
    }
}
