// SPDX-License-Identifier: GPL-3.0-or-later
// A block that ends in a static branch can enter its successor directly instead
// of returning to the dispatcher. The link point has to repeat every check the
// dispatcher performs between two blocks (execution permission, pipeline drain,
// stop flag, cycle budget, fast lookup entry) and commit the cycle boundary the
// same way, or cycles and state diverge from the plain tail call.
#include "Args.h"
#include "NDS.h"
#include "ARM.h"
#include <cstdarg>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <vector>

using namespace melonDS;

namespace
{
#if defined(JIT_ENABLED) && (defined(__x86_64__) || defined(__aarch64__))

// Two blocks in different 4 KB pages: the trace cannot inline the forward
// branch across the page boundary, so each block ends in a static branch whose
// target is the other block.
constexpr u32 BlockP = 0x02000000; // add r0,r0,#1 ; b BlockQ
constexpr u32 BlockQ = 0x02001000; // add r1,r1,#1 ; b BlockP
// Counting loop with a conditional backward branch, the block shape games
// spend most of their time in. r3 is the iteration limit, r4 the address the
// loop stores its counter to once it falls through, and Park is an idle
// self-branch the machine spins in for the rest of the cycle budget.
constexpr u32 LoopHead = 0x02002000;
constexpr u32 Marker = 0x02010000;
constexpr u32 Park = 0x02000200;
constexpr u32 Iterations = 1000;
// A MainRAM instruction fetch costs more than one cycle in the default timing
// model, so finishing the loop and then spinning in the park block needs a
// budget with room to spare.
constexpr u64 LoopBudget = 200000;
constexpr u32 Mode = 0x000000DF; // system mode, ARM, IRQ and FIQ masked

[[noreturn]] void Failf(const char* format, ...)
{
    char message[256];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    throw std::runtime_error(message);
}

void Require(bool ok, const char* what)
{
    if (!ok) throw std::runtime_error(what);
}

void ExpectSameState(const std::vector<u8>& got, const std::vector<u8>& want, const char* what)
{
    if (got.size() != want.size())
        Failf("%s: savestate is %zu bytes, expected %zu", what, got.size(), want.size());
    if (got == want) return;
    size_t at = 0;
    while (got[at] == want[at]) ++at;
    Failf("%s: savestate differs at byte %zu of %zu", what, at, want.size());
}

// The branch compilers build their target from the pipelined PC, which is the
// instruction address plus 8 in ARM mode.
constexpr u32 ArmBranch(u32 cond, u32 from, u32 to)
{
    return (cond << 28) | 0x0A000000u | (((to - (from + 8)) >> 2) & 0xFFFFFFu);
}

void WriteProgram(NDS& nds)
{
    nds.ARM9Write32(BlockP + 0, 0xE2800001);                               // add r0,r0,#1
    nds.ARM9Write32(BlockP + 4, ArmBranch(0xE, BlockP + 4, BlockQ));       // b BlockQ
    nds.ARM9Write32(BlockQ + 0, 0xE2811001);                               // add r1,r1,#1
    nds.ARM9Write32(BlockQ + 4, ArmBranch(0xE, BlockQ + 4, BlockP));       // b BlockP
    nds.ARM9Write32(LoopHead + 0, 0xE2822001);                             // add r2,r2,#1
    nds.ARM9Write32(LoopHead + 4, 0xE1530002);                             // cmp r3,r2
    nds.ARM9Write32(LoopHead + 8, ArmBranch(0x1, LoopHead + 8, LoopHead)); // bne LoopHead
    nds.ARM9Write32(LoopHead + 12, 0xE5842000);                            // str r2,[r4]
    nds.ARM9Write32(LoopHead + 16, ArmBranch(0xE, LoopHead + 16, Park));   // b Park
    nds.ARM9Write32(Park + 0, ArmBranch(0xE, Park, Park));                 // b Park
    nds.ARM9Write32(Marker, 0);
}

struct Probe
{
    u32 r0 = 0, r1 = 0, r2 = 0;
    s32 cycles = 0;
    u64 timestamp = 0;
};

// One dispatcher call, with a cycle budget that decides where chaining stops.
Probe Dispatch(NDS& nds, u32 blockAddr, JitBlockEntry entry, u64 budget, u32 stop = 0, u32 drain = 0)
{
    auto& cpu = nds.ARM9;
    cpu.R[0] = cpu.R[1] = cpu.R[2] = 0;
    cpu.R[3] = 0xFFFFFFFFu; // the conditional loop always takes its branch
    cpu.CPSR = Mode;
    cpu.Halted = 0;
    cpu.IdleLoop = 0;
    cpu.StopExecution = 0;
    nds.ARM9Timestamp = 0;
    nds.ARM9Target = budget;
    cpu.JumpTo(blockAddr);
    cpu.Cycles = 0;
    cpu.StopExecution = stop;
    cpu.JITPipelineDrain = drain;
    ARM_Dispatch(&cpu, entry);
    return {cpu.R[0], cpu.R[1], cpu.R[2], cpu.Cycles, nds.ARM9Timestamp};
}

std::vector<u8> Capture(NDS& nds)
{
    Savestate state;
    if (!nds.DoSavestate(&state) || state.Error)
        Failf("savestate failed");
    const auto* begin = static_cast<const u8*>(state.Buffer());
    return std::vector<u8>(begin, begin + state.Length());
}

struct LoopRun
{
    u32 counter = 0;
    u32 marker = 0;
    s32 cycles = 0;
    u64 timestamp = 0;
    std::vector<u8> state;
};

LoopRun RunLoop(NDS& nds, bool jit)
{
    auto& cpu = nds.ARM9;
    for (u32 reg = 0; reg < 16; ++reg) cpu.R[reg] = 0;
    cpu.R[3] = Iterations;
    cpu.R[4] = Marker;
    cpu.CPSR = Mode;
    cpu.Halted = 0;
    cpu.IdleLoop = 0;
    cpu.StopExecution = 0;
    nds.ARM9Write32(Marker, 0);
    nds.ARM9Timestamp = 0;
    nds.ARM9Target = LoopBudget;
    cpu.JumpTo(LoopHead);
    cpu.Cycles = 0;
    if (jit)
        cpu.Execute<CPUExecuteMode::JIT>();
    else
        cpu.Execute<CPUExecuteMode::Interpreter>();
    return {cpu.R[2], nds.ARM9Read32(Marker), cpu.Cycles, nds.ARM9Timestamp, Capture(nds)};
}

NDSArgs MakeArgs()
{
    NDSArgs args;
    // Fast memory is covered by the whole-binary ROM runs; the point here is
    // the chain itself.
    args.JIT->FastMemory = false;
    args.JIT->LiteralOptimizations = false;
    args.JIT->BranchOptimizations = true;
    return args;
}

#endif
}

int main()
{
#if defined(JIT_ENABLED) && (defined(__x86_64__) || defined(__aarch64__))
    try
    {
        auto nds = std::make_unique<NDS>(MakeArgs());
        NDS::Current = nds.get();
        nds->Reset();
        nds->CurCPU = 0;
        WriteProgram(*nds);

        // A dispatcher run compiles the block at P and, through its plain tail
        // call, the block at Q. Compiling Q links both tails together.
        const auto warm = [&](u32 blockAddr, u64 budget)
        {
            auto& cpu = nds->ARM9;
            cpu.R[0] = cpu.R[1] = cpu.R[2] = 0;
            cpu.R[3] = 0xFFFFFFFFu;
            cpu.CPSR = Mode;
            cpu.Halted = 0;
            cpu.IdleLoop = 0;
            cpu.StopExecution = 0;
            nds->ARM9Timestamp = 0;
            nds->ARM9Target = budget;
            cpu.JumpTo(blockAddr);
            cpu.Cycles = 0;
            cpu.Execute<CPUExecuteMode::JIT>();
        };
        warm(BlockP, 1000);
        Require(nds->JIT.JitBlocks9.contains(BlockP), "block P was not compiled");
        Require(nds->JIT.JitBlocks9.contains(BlockQ), "block Q was not compiled");
        Require(nds->JIT.ChainSiteCount() >= 2, "the branch tails registered no chain sites");
        Require(nds->JIT.LinkedChainSiteCount() == nds->JIT.ChainSiteCount(),
            "a chain site stayed unlinked although its successor is compiled");

        // An exhausted budget has to leave through the plain tail call, which
        // leaves the block's cycles pending for the dispatcher to commit.
        const Probe aloneP = Dispatch(*nds, BlockP, nds->JIT.JitBlocks9.at(BlockP)->EntryPoint, 1);
        Require(aloneP.r0 == 1 && aloneP.r1 == 0 && aloneP.cycles > 0 && aloneP.timestamp == 0,
            "an exhausted budget still entered the successor");
        const s32 costP = aloneP.cycles;
        const Probe aloneQ = Dispatch(*nds, BlockQ, nds->JIT.JitBlocks9.at(BlockQ)->EntryPoint, 1);
        Require(aloneQ.r1 == 1 && aloneQ.r0 == 0 && aloneQ.cycles > 0 && aloneQ.timestamp == 0,
            "an exhausted budget still entered the successor from Q");
        const s32 costQ = aloneQ.cycles;

        // With one cycle of room the tail enters Q inside the same dispatcher
        // call and commits exactly P's cost.
        const Probe hop = Dispatch(*nds, BlockP, nds->JIT.JitBlocks9.at(BlockP)->EntryPoint, u64(costP) + 1);
        Require(hop.r0 == 1 && hop.r1 == 1, "the linked tail did not enter the successor");
        if (hop.timestamp != u64(costP) || hop.cycles != costQ)
            Failf("the linked tail committed the wrong boundary: timestamp=%llu (want %llu) cycles=%d (want %d)",
                (unsigned long long)hop.timestamp, (unsigned long long)costP, hop.cycles, costQ);

        // The budget check has to stop the second hop exactly where the
        // dispatcher loop would stop it.
        const Probe twoHops = Dispatch(*nds, BlockP, nds->JIT.JitBlocks9.at(BlockP)->EntryPoint,
            u64(costP) + u64(costQ) + 1);
        if (twoHops.r0 != 2 || twoHops.r1 != 1 || twoHops.timestamp != u64(costP + costQ)
            || twoHops.cycles != costP)
            Failf("the second hop stopped elsewhere: r0=%u r1=%u timestamp=%llu cycles=%d",
                twoHops.r0, twoHops.r1, (unsigned long long)twoHops.timestamp, twoHops.cycles);

        // Every check the dispatcher performs between two blocks has to hold
        // the chain, with the cycles left pending as in the plain tail call.
        const auto refused = [&](const Probe& probe, const char* what)
        {
            if (probe.r0 != 1 || probe.r1 != 0 || probe.timestamp != 0 || probe.cycles != costP)
                Failf("%s: r0=%u r1=%u timestamp=%llu cycles=%d", what, probe.r0, probe.r1,
                    (unsigned long long)probe.timestamp, probe.cycles);
        };
        const JitBlockEntry entryP = nds->JIT.JitBlocks9.at(BlockP)->EntryPoint;
        refused(Dispatch(*nds, BlockP, entryP, u64(costP) + 1, /*stop*/1), "stop flag");
        refused(Dispatch(*nds, BlockP, entryP, u64(costP) + 1, 0, /*drain*/1), "pending pipeline drain");

        // The ARM9 checks the execution permission of every block it enters.
        nds->ARM9.PU_Map[BlockQ >> 12] &= ~0x04u;
        refused(Dispatch(*nds, BlockP, entryP, u64(costP) + 1), "revoked execution permission");
        nds->ARM9.PU_Map[BlockQ >> 12] |= 0x04u;
        Require(Dispatch(*nds, BlockP, entryP, u64(costP) + 1).r1 == 1, "the chain did not recover");

        // Without chaining the same budget leaves after one block, which is
        // the reference the link has to reproduce.
        nds->JIT.ChainEnabled = false;
        nds->JIT.ResetBlockCache();
        Require(nds->JIT.ChainSiteCount() == 0, "chain sites survived the block cache reset");
        warm(BlockP, 1000);
        Require(nds->JIT.ChainSiteCount() == 0, "chaining was re-enabled by the compile");
        refused(Dispatch(*nds, BlockP, nds->JIT.JitBlocks9.at(BlockP)->EntryPoint, u64(costP) + 1),
            "plain tail call");
        nds->JIT.ChainEnabled = true;
        nds->JIT.ResetBlockCache();
        warm(BlockP, 1000);
        Require(nds->JIT.ChainSiteCount() >= 2
            && nds->JIT.LinkedChainSiteCount() == nds->JIT.ChainSiteCount(),
            "the chains were not rebuilt after the reset");

        // A successor that is invalidated by a write must not be entered from
        // the stale entry the tail still holds.
        nds->ARM9Write32(BlockQ, 0xE2811001);
        Require(!nds->JIT.JitBlocks9.contains(BlockQ), "the write did not invalidate the successor");
        refused(Dispatch(*nds, BlockP, nds->JIT.JitBlocks9.at(BlockP)->EntryPoint, u64(costP) + 1),
            "stale successor entry");

        // The dispatcher recompiles it and links the tail again.
        warm(BlockP, 1000);
        Require(nds->JIT.JitBlocks9.contains(BlockQ), "the invalidated successor was not recompiled");
        const Probe relinked = Dispatch(*nds, BlockP, nds->JIT.JitBlocks9.at(BlockP)->EntryPoint, u64(costP) + 1);
        if (relinked.r0 != 1 || relinked.r1 != 1 || relinked.timestamp != u64(costP))
            Failf("the relinked tail did not chain: r0=%u r1=%u timestamp=%llu",
                relinked.r0, relinked.r1, (unsigned long long)relinked.timestamp);

        // Interpreter, first compile and cached run of the counting loop. The
        // loop finishes by its own condition, so the guest result is the same
        // in every mode, while the cached run has to match the compiled one
        // down to the savestate bytes once the chains are in place. Only the
        // guest result is compared with the interpreter: it commits the cycle
        // budget after every instruction where a block commits it at its tail,
        // so the two modes legitimately stop the trailing park loop elsewhere.
        const LoopRun interpreted = RunLoop(*nds, false);
        const LoopRun compiled = RunLoop(*nds, true);
        const LoopRun cached = RunLoop(*nds, true);
        for (const auto* run : {&interpreted, &compiled, &cached})
            if (run->counter != Iterations || run->marker != Iterations)
                Failf("the loop did not finish: counter=%u marker=%u", run->counter, run->marker);
        ExpectSameState(cached.state, compiled.state, "cached/compiled");

        // The same guest code compiled without chain sites, and again with
        // them, has to leave the identical state.
        nds->JIT.ChainEnabled = false;
        nds->JIT.ResetBlockCache();
        const LoopRun plain = RunLoop(*nds, true);
        Require(plain.counter == Iterations && plain.marker == Iterations, "the plain-tail run did not finish the loop");
        ExpectSameState(plain.state, cached.state, "plain tail/cached");
        nds->JIT.ChainEnabled = true;
        nds->JIT.ResetBlockCache();
        const LoopRun rechained = RunLoop(*nds, true);
        Require(rechained.counter == Iterations && rechained.marker == Iterations,
            "the rechained run did not finish the loop");
        ExpectSameState(rechained.state, cached.state, "rechained/cached");

        std::printf("PASS: chaining keeps dispatcher semantics (%u sites, %u linked, "
                    "loop timestamp %llu, state %zu bytes)\n",
            nds->JIT.ChainSiteCount(), nds->JIT.LinkedChainSiteCount(),
            (unsigned long long)cached.timestamp, cached.state.size());
        return 0;
    }
    catch (const std::exception& error)
    {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
#else
    std::printf("SKIP: block chaining needs an x86-64 or AArch64 JIT\n");
    return 77;
#endif
}
