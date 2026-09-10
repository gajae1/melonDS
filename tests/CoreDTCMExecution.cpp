// SPDX-License-Identifier: GPL-3.0-or-later
// CP15 remaps followed by real guest loads/stores, including cached JIT entry.
#include "Args.h"
#include "NDS.h"
#include "ARM.h"
#include <cstdio>
#include <memory>
#include <optional>
#include <utility>

using namespace melonDS;
namespace
{
constexpr u32 Code = 0x02020000, Idle = 0x02000200;

void EnableDTCM(NDS& nds, u32 base, u32 setting = 0xA)
{
    nds.ARM9.CP15Write(0x910, base | setting); // 16 KiB by default
    nds.ARM9.CP15Write(0x100, nds.ARM9.CP15Read(0x100) | (1u << 16));
}

void DisableDTCM(NDS& nds)
{
    nds.ARM9.CP15Write(0x100, nds.ARM9.CP15Read(0x100) & ~(1u << 16));
}

bool GuestAccess(NDS& nds, bool jit, u32 addr, u32 before, u32 after)
{
    for (unsigned run = 0; run < 2; ++run)
    {
#ifdef JIT_ENABLED
        if (jit && run && !nds.JIT.JitBlocks9.contains(Code))
        {
            std::fprintf(stderr, "DTCM guest load/store block was not compiled\n");
            return false;
        }
#endif
        nds.ARM9.R[0] = addr;
        nds.ARM9.R[1] = 0;
        nds.ARM9.R[2] = after;
        nds.ARM9.CPSR = 0xA00000DF;
        nds.ARM9.JumpTo(Code);
        nds.RunFrame();
        u32 stored = 0;
        nds.ARM9.DataRead32(addr, &stored);
        const u32 expected = run ? after : before;
        if (!nds.IsRunning() || nds.ARM9.R[1] != expected || stored != after ||
            nds.ARM9.CPSR != 0xA00000DF)
        {
            std::fprintf(stderr, "DTCM guest access %08x run=%u: read=%08x expected=%08x stored=%08x flags=%08x\n",
                addr, run, nds.ARM9.R[1], expected, stored, nds.ARM9.CPSR);
            return false;
        }
    }
    return true;
}

// Guest values alone can pass after a fastmem fault rewrites the instruction to
// a slow helper. Inspect this process's actual Windows views without faulting.
// The native reservation places the two 4 GiB CPU views after physical backing.
bool HostView(NDS& nds, bool fast, unsigned cpu, u32 addr,
              std::optional<u32> expected, const char* stage, bool code = false)
{
#if defined(JIT_ENABLED) && defined(_WIN32)
    if (fast)
    {
        const auto base = reinterpret_cast<uintptr_t>(nds.JIT.Memory.GetMainRAM());
        const auto* view = reinterpret_cast<const void*>(base + MemoryTotalSize +
            uintptr_t(cpu) * 0x100000000ULL + addr);
        MEMORY_BASIC_INFORMATION info{};
        u32 value = 0;
        SIZE_T count = 0;
        const bool queried = VirtualQuery(view, &info, sizeof(info)) == sizeof(info);
        const bool readable = ReadProcessMemory(GetCurrentProcess(), view, &value, sizeof(value), &count)
                              && count == sizeof(value);
        const bool valid = expected ? readable && value == *expected && info.State == MEM_COMMIT &&
                                      (!code || info.Protect == PAGE_READONLY)
                                    : !readable && info.State == MEM_RESERVE;
        if (!queried || !valid)
        {
            std::fprintf(stderr, "DTCM host view %s CPU%u %08x: readable=%d value=%08x state=%lx protection=%lx expected=%s\n",
                stage, cpu, addr, readable, value, info.State, info.Protect,
                expected ? "mapped value" : "unmapped");
            return false;
        }
    }
#endif
    return true;
}

bool Map(NDS& nds, bool fast, unsigned cpu, u32 addr)
{
#ifdef JIT_ENABLED
    if (fast)
    {
        nds.CurCPU = cpu;
        const bool mapped = nds.JIT.Memory.MapAtAddress(addr);
        nds.CurCPU = 0;
        if (!mapped) std::fprintf(stderr, "DTCM MapAtAddress rejected CPU%u %08x\n", cpu, addr);
        return mapped;
    }
#endif
    return true;
}
}

int TestDTCMExecution(NDSArgs&& args, bool jit)
{
    const bool fast = args.JIT && args.JIT->FastMemory;
#ifdef JIT_ENABLED
    if (fast && !ARMJIT_Memory::IsFastMemSupported()) return 77;
#endif
    auto instance = std::make_unique<NDS>(std::move(args));
    auto& nds = *instance;
    nds.Reset();
    nds.ARM9Write32(Code, 0xE5901000);     // ldr r1,[r0]
    nds.ARM9Write32(Code + 4, 0xE5802000); // str r2,[r0]
    nds.ARM9Write32(Code + 8, 0xEAFFFFFE);
    nds.ARM9Write32(Idle, 0xEAFFFFFE);
    nds.ARM9.JumpTo(Idle);
    nds.ARM7.JumpTo(Idle);
    nds.Start();

    // DTCM is the backing to map here, not a hole in another memory region.
    EnableDTCM(nds, 0x00800000);
    nds.ARM9.DataWrite32(0x00800000, 0x11223344);
    if (!Map(nds, fast, 0, 0x00800000) ||
        !HostView(nds, fast, 0, 0x00800000, 0x11223344, "initial") ||
        !GuestAccess(nds, jit, 0x00800000, 0x11223344, 0x22334455)) return 1;
    EnableDTCM(nds, 0x00804000);
    if (!HostView(nds, fast, 0, 0x00800000, std::nullopt, "moved old") ||
        !Map(nds, fast, 0, 0x00804000) ||
        !HostView(nds, fast, 0, 0x00804000, 0x22334455, "moved new") ||
        !GuestAccess(nds, jit, 0x00804000, 0x22334455, 0x33445566)) return 2;
    DisableDTCM(nds);
    if (!HostView(nds, fast, 0, 0x00804000, std::nullopt, "disabled")) return 3;

    // Move between different 4 MiB RAM mirrors: the old mirror must lose its
    // DTCM hole before the old address can be mapped back to ordinary RAM.
    constexpr u32 Old = 0x02004000, New = 0x02404000, RAM = 0x02000000;
    nds.ARM9Write32(Old, 0x55667788);
    nds.ARM9Write32(RAM, 0x89ABCDEF);
    EnableDTCM(nds, Old);
    nds.ARM9.DataWrite32(Old, 0x44556677);
    if (!Map(nds, fast, 0, RAM) || !Map(nds, fast, 0, Old) ||
        !Map(nds, fast, 1, RAM) ||
        !HostView(nds, fast, 0, Old, 0x44556677, "overlaid TCM") ||
        !HostView(nds, fast, 1, Old, 0x55667788, "ARM7 underlying RAM")) return 4;
    EnableDTCM(nds, New);
    if (!HostView(nds, fast, 0, RAM, std::nullopt, "old mirror invalidated") ||
        !HostView(nds, fast, 0, Old, std::nullopt, "old hole invalidated") ||
        !HostView(nds, fast, 1, Old, 0x55667788, "ARM7 preserved") ||
        !Map(nds, fast, 0, Old) ||
        !HostView(nds, fast, 0, Old, 0x55667788, "RAM restored") ||
        !HostView(nds, fast, 0, Code, 0xE5901000, "code protection restored", true) ||
        !GuestAccess(nds, jit, Old, 0x55667788, 0x66778899)) return 5;
    if (!Map(nds, fast, 0, New) ||
        !HostView(nds, fast, 0, New, 0x44556677, "new mirror TCM") ||
        !GuestAccess(nds, jit, New, 0x44556677, 0x778899AA)) return 6;
    DisableDTCM(nds);
    if (!HostView(nds, fast, 0, New, std::nullopt, "second disable") ||
        !Map(nds, fast, 0, New) ||
        !HostView(nds, fast, 0, New, 0x66778899, "RAM alias restored") ||
        !GuestAccess(nds, jit, New, 0x66778899, 0xAABBCCDD)) return 7;

    // Subranges of the physical 16 KiB bank must neither map outside the
    // configured DTCM nor change the offset of the backing bytes.
    for (const auto& small : {std::pair{0x00801000u, 0x6u}, // 4 KiB, offset 4 KiB
                              std::pair{0x00802000u, 0x8u}}) // 8 KiB, offset 8 KiB
    {
        const auto [base, setting] = small;
        EnableDTCM(nds, base, setting);
        nds.ARM9.DataWrite32(base, 0x12345678);
        bool nativeSmall = fast;
#ifdef JIT_ENABLED
        nativeSmall = fast && ARMJIT_Memory::PageSize <= (0x200u << (setting >> 1));
        if (fast && !nativeSmall)
        {
            nds.CurCPU = 0;
            if (nds.JIT.Memory.MapAtAddress(base))
            {
                std::fprintf(stderr, "DTCM smaller than a host page was mapped\n");
                return 8;
            }
        }
#endif
        if (!Map(nds, nativeSmall, 0, base) ||
            !HostView(nds, fast, 0, 0x00800000, std::nullopt, "outside small TCM") ||
            !HostView(nds, fast, 0, base, nativeSmall ? std::optional<u32>{0x12345678} : std::nullopt,
                      "small TCM backing offset") ||
            !GuestAccess(nds, jit, base, 0x12345678, 0x23456789)) return 8;
        DisableDTCM(nds);
        if (!HostView(nds, fast, 0, base, std::nullopt, "small TCM disabled")) return 9;
    }

    // Two virtual 16 KiB mirrors share one physical bank. The second ends at
    // 4 GiB; neither that exclusive endpoint nor disabling may leave a view.
    EnableDTCM(nds, 0xFFFF8000, 0xC);
    nds.ARM9.DataWrite32(0xFFFF8000, 0x3456789A);
    if (!Map(nds, fast, 0, 0xFFFF8000) || !Map(nds, fast, 0, 0xFFFFC000) ||
        !HostView(nds, fast, 0, 0xFFFF8000, 0x3456789A, "large TCM first mirror") ||
        !HostView(nds, fast, 0, 0xFFFFC000, 0x3456789A, "large TCM last mirror") ||
        !GuestAccess(nds, jit, 0xFFFFC000, 0x3456789A, 0x456789AB) ||
        !HostView(nds, fast, 0, 0xFFFF8000, 0x456789AB, "physical alias")) return 10;
    DisableDTCM(nds);
    if (!HostView(nds, fast, 0, 0xFFFF8000, std::nullopt, "large TCM disabled") ||
        !HostView(nds, fast, 0, 0xFFFFC000, std::nullopt, "4 GiB endpoint disabled")) return 11;

    std::printf("DTCM move/disable/RAM alias/cached guest access: PASS; fastmem=%d Windows views=%d\n", fast,
#if defined(JIT_ENABLED) && defined(_WIN32)
        fast
#else
        false
#endif
    );
    return 0;
}
