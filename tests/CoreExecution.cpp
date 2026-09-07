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
    std::printf("core=%s fastmem=%d lines=%u ARM-result=100 save-restore=PASS pixels=49152\n",jit?"JIT":"interpreter",fast,lines);
}
