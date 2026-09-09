// SPDX-License-Identifier: GPL-3.0-or-later
// Hand-authored ARM instructions, real core/scheduler/software renderer.
// No external ROM/BIOS; this is a regression test, NOT real-hardware validation.
#include "Args.h"
#include "NDS.h"
#include "ARM.h"
#include <cstdio>
#include <cstring>
#include <memory>
#include <vector>
using namespace melonDS;
int main(int argc, char** argv) {
    const bool jit = argc > 1 && std::strcmp(argv[1], "interpreter") != 0;
    const bool fast = argc > 1 && std::strcmp(argv[1], "fastmem") == 0;
#ifndef JIT_ENABLED
    if (jit) return 77;
#endif
    NDSArgs args;
    if (!jit) args.JIT = std::nullopt;
    else args.JIT->FastMemory = fast;
    auto nds = std::make_unique<NDS>(std::move(args));
    nds->Reset();
    RendererSettings settings{1, false, false, false};
    nds->GetRenderer().SetRenderSettings(settings);
    // r0=100 after a counted loop; store it then stay in a self-loop.
    constexpr u32 code[] = {
        0xE3A00000, // mov r0,#0
        0xE3A01402, // mov r1,#0x02000000
        0xE3A02064, // mov r2,#100
        0xE2800001, // add r0,r0,#1
        0xE2522001, // subs r2,r2,#1
        0x1AFFFFFC, // bne back to add
        0xE5810100, // str r0,[r1,#0x100]
        0xE16F3F10, // clz r3,r0 -> 25
        0xE5813104, // str r3,[r1,#0x104]
        0xE3A04000, // mov r4,#0
        0xE16F5F14, // clz r5,r4 -> 32
        0xE5815108, // str r5,[r1,#0x108]
        0xE3A06102, // mov r6,#0x80000000
        0xE16F7F16, // clz r7,r6 -> 0
        0xE581710C, // str r7,[r1,#0x10c]
        0xEAFFFFFE  // b self
    };
    for (unsigned i=0;i<std::size(code);++i) nds->ARM9Write32(0x02000000+4*i,code[i]);
    nds->ARM9Write32(0x02000200,0xEAFFFFFE);
    nds->ARM9.JumpTo(0x02000000);
    nds->ARM7.JumpTo(0x02000200);
    // Exercise the software output conversion with a constant RGB555 VRAM line.
    nds->ARM9Write16(0x04000304, 0x0203); // LCD, 2D A/B powered; default screen mapping
    nds->ARM9Write8(0x04000240, 0x80);    // VRAM A -> LCDC
    nds->ARM9Write32(0x04000000, 0x00020000); // Main display reads VRAM A
    for (unsigned i=0;i<256*192;++i) nds->ARM9Write16(0x06800000+2*i,0x001F);
    nds->Start();
    const auto lines = nds->RunFrame();
    if (!nds->IsRunning() || nds->ARM9Read32(0x02000100)!=100) {
        std::fprintf(stderr,"execution failed: lines=%u result=%u r0=%u r1=%08x pc=%08x\n",lines,
            nds->ARM9Read32(0x02000100),nds->ARM9.R[0],nds->ARM9.R[1],nds->ARM9.R[15]);
        return 1;
    }
    if (nds->ARM9Read32(0x02000104) != 25 || nds->ARM9Read32(0x02000108) != 32 ||
        nds->ARM9Read32(0x0200010C) != 0) return 8;
    Savestate saved;
    if (!nds->DoSavestate(&saved)) return 2;
    saved.Finish();
    if (saved.Error) return 3;
    nds->ARM9Write32(0x02000100,0xDEADBEEF);
    Savestate restore(saved.Buffer(),saved.Length(),false);
    if (!nds->DoSavestate(&restore) || restore.Error || nds->ARM9Read32(0x02000100)!=100) return 4;
    nds->RunFrame();
    if (!nds->IsRunning() || nds->ARM9Read32(0x02000100)!=100) return 5;
    void *top=nullptr, *bottom=nullptr;
    if (!nds->GetRenderer().GetFramebuffers(&top,&bottom) || !top || !bottom) return 6;
    const u32* screen = static_cast<const u32*>(bottom);
    // 5-bit 31 is doubled to 6-bit 62, then replicated to 8-bit 251, not 255.
    for (unsigned i=0;i<256*192;++i) {
        if (screen[i]!=0xFFFB0000) {
            std::fprintf(stderr,"framebuffer mismatch at %u: %08x\n",i,screen[i]);
            return 7;
        }
    }
#ifdef JIT_ENABLED
    if (jit) {
        // Two independent loops share a 512-byte index range, but not a
        // 16-byte code granule. Modifying one must preserve the other.
        constexpr u32 first = 0x02000400, second = 0x02000480;
        nds->ARM9Write32(first, 0xEAFFFFFE);
        nds->ARM9Write32(second, 0xEAFFFFFE);
        nds->JIT.JitEnableWrite();
        nds->ARM9.JumpTo(first);
        nds->JIT.CompileBlock(&nds->ARM9);
        nds->ARM9.JumpTo(second);
        nds->JIT.CompileBlock(&nds->ARM9);
        nds->JIT.JitEnableExecute();
        if (!nds->JIT.JitBlocks9.contains(first) || !nds->JIT.JitBlocks9.contains(second)) return 9;
        nds->ARM9Write32(first, 0xE3A0002A); // mov r0,#42
        if (nds->JIT.JitBlocks9.contains(first) || !nds->JIT.JitBlocks9.contains(second)) {
            std::fprintf(stderr, "JIT invalidation removed an unrelated code granule\n");
            return 10;
        }
        // The retained block still has to be protected from later writes.
        nds->ARM9Write32(second, 0xE3A00007);
        if (nds->JIT.JitBlocks9.contains(second)) return 11;
    }
#endif
    // Exercise the shipped replacement BIOS through its real SWI entry point.
    // sqrt is unsigned even when bit 31 is set.
    // Isolate BIOS execution from the preceding cache/savestate scenarios.
    // Reusing the same guest addresses on ARM9 then ARM7 also checks that a
    // retired JIT block can never be restored for the other CPU.
    nds->Reset();
    nds->ARM9Write32(0x02000200, 0xEAFFFFFE);
    nds->ARM9.JumpTo(0x02000200);
    nds->ARM7.JumpTo(0x02000200);
    nds->Start();
    constexpr u32 sqrtCode[] = {
        0xE59F000C, // ldr r0,[pc,#12] -> input at +20
        0xEF0D0000, // swi 0x0D0000
        0xE59F1008, // ldr r1,[pc,#8] -> result address at +24
        0xE5810000, // str r0,[r1]
        0xEAFFFFFE,
        0, 0x02000110
    };
    constexpr u32 sqrtCases[] = {0, 1, 4, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF};
    for (bool arm7 : {false, true})
    {
        auto& cpu = arm7 ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
        cpu.CPSR = 0xDF; // System mode, interrupts disabled; stacks normally set by boot.
        cpu.R[13] = 0x02002000;
        cpu.R_SVC[0] = 0x02003000;
        for (u32 value : sqrtCases)
        {
            for (unsigned i = 0; i < std::size(sqrtCode); ++i)
                nds->ARM9Write32(0x02000600 + i * 4, sqrtCode[i]);
            nds->ARM9Write32(0x02000614, value);
            nds->ARM9Write32(0x02000110, 0xDEADBEEF);
            cpu.JumpTo(0x02000600);
            nds->RunFrame();
            const u32 result = nds->ARM9Read32(0x02000110);
            if (u64(result) * result > value || u64(result + 1) * (result + 1) <= value)
            {
                std::fprintf(stderr, "ARM%d FreeBIOS sqrt(%08x) = %u\n", arm7 ? 7 : 9, value, result);
                return 12;
            }
        }
        cpu.JumpTo(0x02000200); // stop this CPU before checking the other
    }
    // Branch following includes the self-branch twice. These different traces
    // have the same low 32 bits of XXH3_64bits (0x846DC331). Replacing code must
    // not restore the first program's JIT block.
    nds->Reset();
    nds->ARM9Write32(0x02000200, 0xEAFFFFFE);
    nds->ARM7.JumpTo(0x02000200);
    nds->Start();
    constexpr u32 collisionCode[][3] = {
        {0xE3A00EB9, 0xE3A01251, 0xEAFFFFFE}, // mov r0,#0xB90; mov r1,#0x10000005; b .
        {0xE3A000C1, 0xE3A0169B, 0xEAFFFFFE}  // mov r0,#0xC1; mov r1,#0x9B00000; b .
    };
    constexpr u32 collisionResults[][2] = {{0xB90, 0x10000005}, {0xC1, 0x9B00000}};
    for (unsigned version = 0; version < std::size(collisionCode); ++version)
    {
        for (unsigned i = 0; i < std::size(collisionCode[version]); ++i)
            nds->ARM9Write32(0x02000800 + 4 * i, collisionCode[version][i]);
        // CompileBlock interprets instructions while compiling. Enter again to
        // check the generated code rather than only that initial execution.
        for (unsigned run = 0; run < 2; ++run)
        {
            nds->ARM9.R[0] = nds->ARM9.R[1] = 0;
            nds->ARM9.JumpTo(0x02000800);
            nds->RunFrame();
            if (nds->ARM9.R[0] != collisionResults[version][0] ||
                nds->ARM9.R[1] != collisionResults[version][1])
            {
                std::fprintf(stderr, "JIT hash collision: program=%u run=%u r0=%08x r1=%08x\n",
                    version, run, nds->ARM9.R[0], nds->ARM9.R[1]);
                return 13;
            }
        }
    }
    // The same bytes are valid in both instruction sets: ARM sets r2 to 7;
    // Thumb sets r0 to 7 and branches to a separate Thumb self-loop.
    nds->Reset();
    nds->ARM9Write32(0x02000200, 0xEAFFFFFE);
    nds->ARM9Write32(0x02000804, 0xEAFFFFFE);
    nds->ARM9Write16(0x02000F46, 0xE7FE);
    nds->ARM9.JumpTo(0x02000200);
    nds->ARM7.JumpTo(0x02000200);
    nds->Start();
    for (u32 value : {7u, 9u})
    {
        // Invalidate both instruction sets, on both CPUs, with the same write.
        nds->ARM9Write32(0x02000800, 0xE3A02000 | value);
        for (bool arm7 : {false, true})
        {
            auto& cpu = arm7 ? static_cast<ARM&>(nds->ARM7) : static_cast<ARM&>(nds->ARM9);
            for (bool thumb : {false, true, false, true})
            {
                cpu.R[0] = cpu.R[2] = 0;
                cpu.JumpTo(0x02000800 | u32(thumb));
                nds->RunFrame();
                if (cpu.R[0] != (thumb ? value : 0u) || cpu.R[2] != (thumb ? 0u : value))
                {
                    std::fprintf(stderr, "ARM%d instruction-set cache mismatch: thumb=%d r0=%08x r2=%08x\n",
                        arm7 ? 7 : 9, thumb, cpu.R[0], cpu.R[2]);
                    return 14;
                }
            }
            cpu.JumpTo(0x02000200);
        }
    }
    std::printf("core=%s fastmem=%d lines=%u ARM-result=100 save-restore=PASS pixels=49152\n",jit?"JIT":"interpreter",fast,lines);
}
