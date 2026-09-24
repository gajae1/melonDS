// SPDX-License-Identifier: GPL-3.0-or-later
// A condition-failed transfer is skipped with one conditional jump over the
// compiled body of that transfer. That body has no small upper bound: with fast
// memory a single transfer emits the region check, the data-timing guard and,
// for a transfer that leaves the block (PC destination), the whole exit
// sequence with a spill of every register the block still holds dirty. The
// emitter only asserts when the jump target does not fit the short form, so a
// truncated rel8 lands inside the body and the block runs bytes that were never
// emitted as code; the fault handler then reports a bogus memory access
// ("this is a JIT bug") and aborts.
//
// The guest program runs on the ARM7, where a single transfer had no lower
// bound on the compiled body at all (ARM9 transfers already asked for the
// rel32 form, and the ARM9 body carries the MPU permission guard).
#include "Args.h"
#include "NDS.h"
#include "ARM.h"
#include <cstdio>
#include <iterator>
#include <memory>
#include <utility>

using namespace melonDS;
namespace
{
constexpr u32 Idle = 0x02000200;
constexpr u32 Code = 0x02020000;
constexpr u32 Slot = 0x80;
constexpr u32 Sentinel = 0x5A5A5A5A;
constexpr u32 Word0 = 0x11223344, Word1 = 0x55667788, SwapValue = 0x22334455;
constexpr u32 MainRAM = 0x02000600, ARM7RAM = 0x03800600, SharedWRAM = 0x037F0600;

// mov rN,#1 leaves the register dirty for the transfer's exit path, the
// following add is the use that keeps it in the register cache until the tail.
constexpr u32 DirtyPair[][2] = {
    {0xE3A06001, 0xE2866001},
    {0xE3A07001, 0xE2877001},
    {0xE3A08001, 0xE2888001},
    {0xE3A09001, 0xE2899001},
    {0xE3A0A001, 0xE28AA001},
    {0xE3A0B001, 0xE28BB001},
};
// Reads r6..r11 after the transfer, so the cache keeps them live across it.
constexpr u32 Tail[] = {0xE08C0006, 0xE0800007, 0xE0800008, 0xE0800009, 0xE080000A, 0xE080000B};

// Every transfer is conditioned on EQ, so Z selects execution: "adds r3,r3,#1"
// (r3 = 0) clears Z and the transfer is skipped, "movs r3,#0" sets Z and the
// transfer runs.
constexpr u32 SkipPrefix = 0xE2933001;
constexpr u32 RunPrefix = 0xE3B03000;

enum class Effect
{
    Load,
    Store,
    Swap,
};

struct Case
{
    const char* name;
    u32 instr;
    u32 base;
    u32 offset;      // r2
    unsigned dirty;  // number of register pairs in front of the transfer
    bool loadPC;     // the transfer writes PC, so a taken transfer leaves the block
    bool unaligned;  // a taken word load returns the rotated word
    bool taken;      // check the taken path as well
    Effect effect;
    u32 loaded;      // r4 after a taken load or swap
    u32 writeback;   // r1 after a taken transfer
    unsigned stored;
    u32 storedValue;
};

constexpr Case Cases[] = {
    // Fast memory plus a dirty register cache puts the compiled body well past
    // the 127 byte reach of a short conditional jump. Rd is PC, so r4 keeps the
    // sentinel value and only the writeback and the final PC are checked.
    {"ldr-pc-dirty4", 0x0491F004, MainRAM + 0xC0, 4, 4, true, false, true,
     Effect::Load, Sentinel, MainRAM + 0xC4, 0, 0},
    {"ldr-pc-dirty6", 0x0491F004, MainRAM + 0xE0, 4, 6, true, false, true,
     Effect::Load, Sentinel, MainRAM + 0xE4, 0, 0},
    // A taken byte load into PC can only reach a byte address; only the
    // condition-failed path (the one with the long body) is checked.
    {"ldrb-pc-dirty6", 0x04D1F001, MainRAM + 0x100, 1, 6, true, false, false,
     Effect::Load, Sentinel, MainRAM + 0x101, 0, 0},
    {"ldr-pc-dirty6-arm7ram", 0x0491F004, ARM7RAM, 4, 6, true, false, true,
     Effect::Load, Sentinel, ARM7RAM + 4, 0, 0},
    {"ldr-pc-dirty6-wram", 0x0491F004, SharedWRAM, 4, 6, true, false, true,
     Effect::Load, Sentinel, SharedWRAM + 4, 0, 0},
    {"ldr-pc-dirty6-unaligned", 0x0491F004, MainRAM + 0x121, 4, 6, true, true, false,
     Effect::Load, Sentinel, MainRAM + 0x125, 0, 0},
    // Ordinary transfers keep the same program shape so a wrong skip or
    // writeback shows up as a register difference.
    {"ldr-post-imm", 0x04914004, MainRAM + 0x20, 4, 0, false, false, true,
     Effect::Load, Word0, MainRAM + 0x24, 0, 0},
    {"ldr-imm-offset", 0x05914004, MainRAM + 0x40, 0, 0, false, false, true,
     Effect::Load, Word1, MainRAM + 0x40, 0, 0},
    {"ldrb-post-imm", 0x04D14001, MainRAM + 0x60, 1, 0, false, false, true,
     Effect::Load, Word0 & 0xFF, MainRAM + 0x61, 0, 0},
    {"ldrh-post-imm", 0x00D140B2, MainRAM + 0x80, 2, 0, false, false, true,
     Effect::Load, Word0 & 0xFFFF, MainRAM + 0x82, 0, 0},
    {"str-post-imm", 0x04814004, MainRAM + 0xA0, 4, 0, false, false, true,
     Effect::Store, 0, MainRAM + 0xA4, 4, Sentinel},
    {"swp", 0x01014095, MainRAM + 0x140, 0, 0, false, false, true,
     Effect::Swap, Word0, MainRAM + 0x140, 4, SwapValue},
};

void WriteSlot(NDS& nds, u32 slot, const Case& c, bool run)
{
    u32 at = slot;
    for (unsigned k = 0; k < c.dirty; ++k)
    {
        nds.ARM9Write32(at, DirtyPair[k][0]);
        nds.ARM9Write32(at + 4, DirtyPair[k][1]);
        at += 8;
    }
    nds.ARM9Write32(at, run ? RunPrefix : SkipPrefix);
    at += 4;
    nds.ARM9Write32(at, c.instr);
    at += 4;
    for (u32 instr : Tail)
    {
        nds.ARM9Write32(at, instr);
        at += 4;
    }
    nds.ARM9Write32(at, 0xEAFFFFFE); // b self
}

u32 SlotLoop(const Case& c, unsigned index, bool run)
{
    return Code + (index * 2 + (run ? 1 : 0)) * Slot + 4 * (2 * c.dirty + 2 + std::size(Tail));
}

u32 ReadMemory(NDS& nds, u32 addr, unsigned width)
{
    u32 value = 0;
    switch (width)
    {
    case 1: nds.ARM7.DataRead8(addr, &value); break;
    case 2: nds.ARM7.DataRead16(addr, &value); break;
    default: nds.ARM7.DataRead32(addr, &value); break;
    }
    return value;
}

bool RunCase(NDS& nds, const Case& c, unsigned index, bool run)
{
    const u32 slot = Code + (index * 2 + (run ? 1 : 0)) * Slot;
    const u32 seed = c.loadPC ? Idle : Word0;
    const u32 baseWord = c.base & ~3u;
    for (unsigned pass = 0; pass < 2; ++pass)
    {
        nds.ARM7.DataWrite32(baseWord, seed);
        nds.ARM7.DataWrite32(baseWord + 4, Word1);
        nds.ARM7.R[1] = c.base;
        nds.ARM7.R[2] = c.offset;
        nds.ARM7.R[3] = 0;
        nds.ARM7.R[4] = Sentinel;
        nds.ARM7.R[5] = SwapValue;
        nds.ARM7.R[6] = nds.ARM7.R[7] = nds.ARM7.R[8] = 0;
        nds.ARM7.R[9] = nds.ARM7.R[10] = nds.ARM7.R[11] = 0;
        nds.ARM7.CPSR = 0x00000013; // ARM, supervisor
        nds.ARM7.JumpTo(slot);
        nds.RunFrame();

        u32 wantR1 = c.base, wantR4 = Sentinel, wantPC = SlotLoop(c, index, run);
        if (run && c.loadPC) wantPC = Idle;
        if (run)
        {
            wantR1 = c.writeback;
            if (c.effect == Effect::Load) wantR4 = c.loaded;
            if (c.effect == Effect::Swap) wantR4 = c.loaded;
        }
        if (!nds.IsRunning() || nds.ARM7.R[1] != wantR1 || nds.ARM7.R[4] != wantR4)
        {
            std::fprintf(stderr, "cond transfer %s pass=%u run=%d: r1=%08x (want %08x) r4=%08x (want %08x)\n",
                c.name, pass, run ? 1 : 0, nds.ARM7.R[1], wantR1, nds.ARM7.R[4], wantR4);
            return false;
        }
        // The idle self branch leaves PC on the branch or just past it.
        if (nds.ARM7.R[15] != wantPC && nds.ARM7.R[15] != wantPC + 4)
        {
            std::fprintf(stderr, "cond transfer %s pass=%u run=%d: pc=%08x (want %08x)\n",
                c.name, pass, run ? 1 : 0, nds.ARM7.R[15], wantPC);
            return false;
        }
        if (run && c.stored)
        {
            const u32 got = ReadMemory(nds, c.base, c.stored);
            const u32 mask = c.stored == 4 ? 0xFFFFFFFFu : (1u << (c.stored * 8)) - 1;
            if (got != (c.storedValue & mask))
            {
                std::fprintf(stderr, "cond transfer %s store: %08x = %08x (want %08x)\n",
                    c.name, c.base, got, c.storedValue & mask);
                return false;
            }
        }
    }
    return true;
}
}

int TestJITCondTransferReach(NDSArgs&& args, bool jit)
{
    const bool fast = args.JIT && args.JIT->FastMemory;
#ifdef JIT_ENABLED
    if (fast && !ARMJIT_Memory::IsFastMemSupported()) return 77;
#endif
    auto instance = std::make_unique<NDS>(std::move(args));
    auto& nds = *instance;
    nds.Reset();
    NDS::Current = instance.get();

    nds.ARM9Write32(Idle, 0xEAFFFFFE);
    for (unsigned i = 0; i < std::size(Cases); ++i)
    {
        for (unsigned run = 0; run < 2; ++run)
            WriteSlot(nds, Code + (i * 2 + run) * Slot, Cases[i], run != 0);
    }
    nds.ARM7.DataWrite32(MainRAM, Word0);
    nds.ARM9.JumpTo(Idle);
    nds.ARM7.JumpTo(Idle);
    nds.Start();

    unsigned checked = 0;
    for (unsigned i = 0; i < std::size(Cases); ++i)
    {
        for (unsigned run = 0; run < 2; ++run)
        {
            if (run && !Cases[i].taken) continue;
            if (!RunCase(nds, Cases[i], i, run != 0)) return 1;
            ++checked;
        }
    }
    std::printf("cond transfer reach: %zu transfers, %u paths, jit=%d fastmem=%d\n",
                std::size(Cases), checked, jit ? 1 : 0, fast ? 1 : 0);
    return 0;
}
