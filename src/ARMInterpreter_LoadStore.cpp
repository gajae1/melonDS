/*
    Copyright 2016-2026 melonDS team

    This file is part of melonDS.

    melonDS is free software: you can redistribute it and/or modify it under
    the terms of the GNU General Public License as published by the Free
    Software Foundation, either version 3 of the License, or (at your option)
    any later version.

    melonDS is distributed in the hope that it will be useful, but WITHOUT ANY
    WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
    FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with melonDS. If not, see http://www.gnu.org/licenses/.
*/

#include <bit>
#include <stdio.h>
#include "ARM.h"


namespace melonDS::ARMInterpreter
{


// copypasta from ALU. bad
#define LSL_IMM(x, s) \
    x <<= s;

#define LSR_IMM(x, s) \
    if (s == 0) x = 0; \
    else        x >>= s;

#define ASR_IMM(x, s) \
    if (s == 0) x = ((s32)x) >> 31; \
    else        x = ((s32)x) >> s;

#define ROR_IMM(x, s) \
    if (s == 0) \
    { \
        x = (x >> 1) | ((cpu->CPSR & 0x20000000) << 2); \
    } \
    else \
    { \
        x = ROR(x, s); \
    }



#define A_WB_CALC_OFFSET_IMM \
    u32 offset = (cpu->CurInstr & 0xFFF); \
    if (!(cpu->CurInstr & (1<<23))) offset = -offset;

#define A_WB_CALC_OFFSET_REG(shiftop) \
    u32 offset = cpu->R[cpu->CurInstr & 0xF]; \
    u32 shift = ((cpu->CurInstr>>7)&0x1F); \
    shiftop(offset, shift); \
    if (!(cpu->CurInstr & (1<<23))) offset = -offset;



#define A_STR \
    offset += cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 storeval = cpu->R[(cpu->CurInstr>>12) & 0xF]; \
    if (((cpu->CurInstr>>12) & 0xF) == 0xF) \
        storeval += 4; \
    if (!cpu->DataWrite32(offset, storeval)) return; \
    if (cpu->CurInstr & (1<<21)) cpu->R[(cpu->CurInstr>>16) & 0xF] = offset; \
    cpu->AddCycles_CD();

// TODO: user mode (bit21)
#define A_STR_POST \
    u32 addr = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 storeval = cpu->R[(cpu->CurInstr>>12) & 0xF]; \
    if (((cpu->CurInstr>>12) & 0xF) == 0xF) \
        storeval += 4; \
    if (!cpu->DataWrite32(addr, storeval)) return; \
    cpu->R[(cpu->CurInstr>>16) & 0xF] += offset; \
    cpu->AddCycles_CD();

#define A_STRB \
    offset += cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    if (!cpu->DataWrite8(offset, cpu->R[(cpu->CurInstr>>12) & 0xF])) return; \
    if (cpu->CurInstr & (1<<21)) cpu->R[(cpu->CurInstr>>16) & 0xF] = offset; \
    cpu->AddCycles_CD();

// TODO: user mode (bit21)
#define A_STRB_POST \
    u32 addr = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    if (!cpu->DataWrite8(addr, cpu->R[(cpu->CurInstr>>12) & 0xF])) return; \
    cpu->R[(cpu->CurInstr>>16) & 0xF] += offset; \
    cpu->AddCycles_CD();

#define A_LDR \
    offset += cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 val; if (!cpu->DataRead32(offset, &val)) return; \
    val = ROR(val, ((offset&0x3)<<3)); \
    if (cpu->CurInstr & (1<<21)) cpu->R[(cpu->CurInstr>>16) & 0xF] = offset; \
    cpu->AddCycles_CDI(); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        if (cpu->Num==1) val &= ~0x1; \
        cpu->JumpTo(val); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = val; \
    }

// TODO: user mode
#define A_LDR_POST \
    u32 addr = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 val; if (!cpu->DataRead32(addr, &val)) return; \
    val = ROR(val, ((addr&0x3)<<3)); \
    cpu->R[(cpu->CurInstr>>16) & 0xF] += offset; \
    cpu->AddCycles_CDI(); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) \
    { \
        if (cpu->Num==1) val &= ~0x1; \
        cpu->JumpTo(val); \
    } \
    else \
    { \
        cpu->R[(cpu->CurInstr>>12) & 0xF] = val; \
    }

#define A_LDRB \
    offset += cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 val; if (!cpu->DataRead8(offset, &val)) return; \
    if (cpu->CurInstr & (1<<21)) cpu->R[(cpu->CurInstr>>16) & 0xF] = offset; \
    cpu->AddCycles_CDI(); \
    cpu->R[(cpu->CurInstr>>12) & 0xF] = val; \
    if (((cpu->CurInstr>>12) & 0xF) == 15) printf("!! LDRB PC %08X\n", cpu->R[15]); \

// TODO: user mode
#define A_LDRB_POST \
    u32 addr = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 val; if (!cpu->DataRead8(addr, &val)) return; \
    cpu->R[(cpu->CurInstr>>16) & 0xF] += offset; \
    cpu->AddCycles_CDI(); \
    cpu->R[(cpu->CurInstr>>12) & 0xF] = val; \
    if (((cpu->CurInstr>>12) & 0xF) == 15) printf("!! LDRB PC %08X\n", cpu->R[15]); \



#define A_IMPLEMENT_WB_LDRSTR(x) \
\
void A_##x##_IMM(ARM* cpu) \
{ \
    A_WB_CALC_OFFSET_IMM \
    A_##x \
} \
\
void A_##x##_REG_LSL(ARM* cpu) \
{ \
    A_WB_CALC_OFFSET_REG(LSL_IMM) \
    A_##x \
} \
\
void A_##x##_REG_LSR(ARM* cpu) \
{ \
    A_WB_CALC_OFFSET_REG(LSR_IMM) \
    A_##x \
} \
\
void A_##x##_REG_ASR(ARM* cpu) \
{ \
    A_WB_CALC_OFFSET_REG(ASR_IMM) \
    A_##x \
} \
\
void A_##x##_REG_ROR(ARM* cpu) \
{ \
    A_WB_CALC_OFFSET_REG(ROR_IMM) \
    A_##x \
} \
\
void A_##x##_POST_IMM(ARM* cpu) \
{ \
    A_WB_CALC_OFFSET_IMM \
    A_##x##_POST \
} \
\
void A_##x##_POST_REG_LSL(ARM* cpu) \
{ \
    A_WB_CALC_OFFSET_REG(LSL_IMM) \
    A_##x##_POST \
} \
\
void A_##x##_POST_REG_LSR(ARM* cpu) \
{ \
    A_WB_CALC_OFFSET_REG(LSR_IMM) \
    A_##x##_POST \
} \
\
void A_##x##_POST_REG_ASR(ARM* cpu) \
{ \
    A_WB_CALC_OFFSET_REG(ASR_IMM) \
    A_##x##_POST \
} \
\
void A_##x##_POST_REG_ROR(ARM* cpu) \
{ \
    A_WB_CALC_OFFSET_REG(ROR_IMM) \
    A_##x##_POST \
}

A_IMPLEMENT_WB_LDRSTR(STR)
A_IMPLEMENT_WB_LDRSTR(STRB)
A_IMPLEMENT_WB_LDRSTR(LDR)
A_IMPLEMENT_WB_LDRSTR(LDRB)



#define A_HD_CALC_OFFSET_IMM \
    u32 offset = (cpu->CurInstr & 0xF) | ((cpu->CurInstr >> 4) & 0xF0); \
    if (!(cpu->CurInstr & (1<<23))) offset = -offset;

#define A_HD_CALC_OFFSET_REG \
    u32 offset = cpu->R[cpu->CurInstr & 0xF]; \
    if (!(cpu->CurInstr & (1<<23))) offset = -offset;



#define A_STRH \
    offset += cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    if (!cpu->DataWrite16(offset, cpu->R[(cpu->CurInstr>>12) & 0xF])) return; \
    if (cpu->CurInstr & (1<<21)) cpu->R[(cpu->CurInstr>>16) & 0xF] = offset; \
    cpu->AddCycles_CD();

#define A_STRH_POST \
    u32 addr = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    if (!cpu->DataWrite16(addr, cpu->R[(cpu->CurInstr>>12) & 0xF])) return; \
    cpu->R[(cpu->CurInstr>>16) & 0xF] += offset; \
    cpu->AddCycles_CD();

// TODO: CHECK LDRD/STRD TIMINGS!!

#define A_LDRD \
    if (cpu->Num != 0) return; \
    offset += cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 r = (cpu->CurInstr>>12) & 0xF; \
    if (r&1) { r--; printf("!! MISALIGNED LDRD %d\n", r+1); } \
    u32 lo, hi; \
    if (!cpu->DataRead32(offset, &lo) || !cpu->DataRead32S(offset+4, &hi)) return; \
    if (cpu->CurInstr & (1<<21)) cpu->R[(cpu->CurInstr>>16) & 0xF] = offset; \
    cpu->R[r] = lo; cpu->R[r+1] = hi; \
    cpu->AddCycles_CDI();

#define A_LDRD_POST \
    if (cpu->Num != 0) return; \
    u32 addr = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 r = (cpu->CurInstr>>12) & 0xF; \
    if (r&1) { r--; printf("!! MISALIGNED LDRD_POST %d\n", r+1); } \
    u32 lo, hi; \
    if (!cpu->DataRead32(addr, &lo) || !cpu->DataRead32S(addr+4, &hi)) return; \
    cpu->R[(cpu->CurInstr>>16) & 0xF] += offset; \
    cpu->R[r] = lo; cpu->R[r+1] = hi; \
    cpu->AddCycles_CDI();

#define A_STRD \
    if (cpu->Num != 0) return; \
    offset += cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 r = (cpu->CurInstr>>12) & 0xF; \
    if (r&1) { r--; printf("!! MISALIGNED STRD %d\n", r+1); } \
    if (!cpu->DataWrite32(offset, cpu->R[r]) || !cpu->DataWrite32S(offset+4, cpu->R[r+1])) return; \
    if (cpu->CurInstr & (1<<21)) cpu->R[(cpu->CurInstr>>16) & 0xF] = offset; \
    cpu->AddCycles_CD();

#define A_STRD_POST \
    if (cpu->Num != 0) return; \
    u32 addr = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 r = (cpu->CurInstr>>12) & 0xF; \
    if (r&1) { r--; printf("!! MISALIGNED STRD_POST %d\n", r+1); } \
    if (!cpu->DataWrite32(addr, cpu->R[r]) || !cpu->DataWrite32S(addr+4, cpu->R[r+1])) return; \
    cpu->R[(cpu->CurInstr>>16) & 0xF] += offset; \
    cpu->AddCycles_CD();

#define A_LDRH \
    offset += cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 val; \
    if (!cpu->DataRead16(offset, &val)) return; \
    if (cpu->CurInstr & (1<<21)) cpu->R[(cpu->CurInstr>>16) & 0xF] = offset; \
    cpu->R[(cpu->CurInstr>>12) & 0xF] = val; \
    cpu->AddCycles_CDI(); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) printf("!! LDRH PC %08X\n", cpu->R[15]); \

#define A_LDRH_POST \
    u32 addr = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 val; \
    if (!cpu->DataRead16(addr, &val)) return; \
    cpu->R[(cpu->CurInstr>>16) & 0xF] += offset; \
    cpu->R[(cpu->CurInstr>>12) & 0xF] = val; \
    cpu->AddCycles_CDI(); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) printf("!! LDRH PC %08X\n", cpu->R[15]); \

#define A_LDRSB \
    offset += cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 val; \
    if (!cpu->DataRead8(offset, &val)) return; \
    if (cpu->CurInstr & (1<<21)) cpu->R[(cpu->CurInstr>>16) & 0xF] = offset; \
    cpu->R[(cpu->CurInstr>>12) & 0xF] = val; \
    cpu->R[(cpu->CurInstr>>12) & 0xF] = (s32)(s8)cpu->R[(cpu->CurInstr>>12) & 0xF]; \
    cpu->AddCycles_CDI(); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) printf("!! LDRSB PC %08X\n", cpu->R[15]); \

#define A_LDRSB_POST \
    u32 addr = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 val; \
    if (!cpu->DataRead8(addr, &val)) return; \
    cpu->R[(cpu->CurInstr>>16) & 0xF] += offset; \
    cpu->R[(cpu->CurInstr>>12) & 0xF] = val; \
    cpu->R[(cpu->CurInstr>>12) & 0xF] = (s32)(s8)cpu->R[(cpu->CurInstr>>12) & 0xF]; \
    cpu->AddCycles_CDI(); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) printf("!! LDRSB PC %08X\n", cpu->R[15]); \

#define A_LDRSH \
    offset += cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 val; \
    if (!cpu->DataRead16(offset, &val)) return; \
    if (cpu->CurInstr & (1<<21)) cpu->R[(cpu->CurInstr>>16) & 0xF] = offset; \
    cpu->R[(cpu->CurInstr>>12) & 0xF] = val; \
    cpu->R[(cpu->CurInstr>>12) & 0xF] = (s32)(s16)cpu->R[(cpu->CurInstr>>12) & 0xF]; \
    cpu->AddCycles_CDI(); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) printf("!! LDRSH PC %08X\n", cpu->R[15]); \

#define A_LDRSH_POST \
    u32 addr = cpu->R[(cpu->CurInstr>>16) & 0xF]; \
    u32 val; \
    if (!cpu->DataRead16(addr, &val)) return; \
    cpu->R[(cpu->CurInstr>>16) & 0xF] += offset; \
    cpu->R[(cpu->CurInstr>>12) & 0xF] = val; \
    cpu->R[(cpu->CurInstr>>12) & 0xF] = (s32)(s16)cpu->R[(cpu->CurInstr>>12) & 0xF]; \
    cpu->AddCycles_CDI(); \
    if (((cpu->CurInstr>>12) & 0xF) == 15) printf("!! LDRSH PC %08X\n", cpu->R[15]); \


#define A_IMPLEMENT_HD_LDRSTR(x) \
\
void A_##x##_IMM(ARM* cpu) \
{ \
    A_HD_CALC_OFFSET_IMM \
    A_##x \
} \
\
void A_##x##_REG(ARM* cpu) \
{ \
    A_HD_CALC_OFFSET_REG \
    A_##x \
} \
void A_##x##_POST_IMM(ARM* cpu) \
{ \
    A_HD_CALC_OFFSET_IMM \
    A_##x##_POST \
} \
\
void A_##x##_POST_REG(ARM* cpu) \
{ \
    A_HD_CALC_OFFSET_REG \
    A_##x##_POST \
}

A_IMPLEMENT_HD_LDRSTR(STRH)
A_IMPLEMENT_HD_LDRSTR(LDRD)
A_IMPLEMENT_HD_LDRSTR(STRD)
A_IMPLEMENT_HD_LDRSTR(LDRH)
A_IMPLEMENT_HD_LDRSTR(LDRSB)
A_IMPLEMENT_HD_LDRSTR(LDRSH)



void A_SWP(ARM* cpu)
{
    u32 base = cpu->R[(cpu->CurInstr >> 16) & 0xF];
    u32 rm = cpu->R[cpu->CurInstr & 0xF];

    u32 val;
    if (!cpu->DataRead32(base, &val)) return;

    u32 numD = cpu->DataCycles;
    if (!cpu->DataWrite32(base, rm)) return;
    // Either access can abort. Commit Rd only after both have succeeded.
    cpu->R[(cpu->CurInstr >> 12) & 0xF] = ROR(val, 8*(base&0x3));
    cpu->DataCycles += numD;

    cpu->AddCycles_CDI();
}

void A_SWPB(ARM* cpu)
{
    u32 base = cpu->R[(cpu->CurInstr >> 16) & 0xF];
    u32 rm = cpu->R[cpu->CurInstr & 0xF] & 0xFF;

    u32 val;
    if (!cpu->DataRead8(base, &val)) return;

    u32 numD = cpu->DataCycles;
    if (!cpu->DataWrite8(base, rm)) return;
    cpu->R[(cpu->CurInstr >> 12) & 0xF] = val;
    cpu->DataCycles += numD;

    cpu->AddCycles_CDI();
}



static void A_EmptyBlockTransfer(ARM* cpu, bool load)
{
    const u32 rn = (cpu->CurInstr >> 16) & 0xF;
    const u32 base = cpu->R[rn];
    const bool up = cpu->CurInstr & (1 << 23);
    const bool pre = cpu->CurInstr & (1 << 24);
    const u32 writeback = up ? base + 0x40 : base - 0x40;

    // ARM9 only updates the base. Do not charge stale DataCycles from an
    // earlier instruction; the exact silicon timing of this invalid list is
    // not specified by the ISA, so use a code-only compatibility cost.
    if (cpu->Num == 0)
    {
        if (cpu->CurInstr & (1 << 21)) cpu->R[rn] = writeback;
        cpu->AddCycles_C();
        return;
    }

    // ARM7 transfers one PC word but computes its address / writeback as a
    // sixteen-register list. This is not equivalent to replacing {} by {PC}.
    const u32 address = (up ? base : writeback) + (pre == up ? 4 : 0);
    u32 pc;
    if (load) cpu->DataRead32(address, &pc);
    else      cpu->DataWrite32(address, cpu->R[15] + 4);
    if (cpu->CurInstr & (1 << 21)) cpu->R[rn] = writeback;
    if (load)
    {
        cpu->JumpTo(pc & ~1u, cpu->CurInstr & (1 << 22));
        cpu->AddCycles_CDI();
    }
    else
        cpu->AddCycles_CD();
}

// User-register transfers retain privileged access permissions. Address the
// user bank directly so an abort never enters with a phony bank selected.
static u32& UserTransferReg(ARM* cpu, unsigned reg)
{
    const u32 mode = cpu->CPSR & 0x1F;
    if (mode == 0x11 && reg >= 8 && reg < 15) return cpu->R_FIQ[reg-8];
    if (reg == 13 || reg == 14)
    {
        switch (mode)
        {
        case 0x12: return cpu->R_IRQ[reg-13];
        case 0x13: return cpu->R_SVC[reg-13];
        case 0x17: return cpu->R_ABT[reg-13];
        case 0x1B: return cpu->R_UND[reg-13];
        }
    }
    return cpu->R[reg];
}

static void RestoreTransferBase(ARM* cpu, unsigned reg, u32 base, u32 oldcpsr)
{
    // Access helpers have already entered Abort mode. Restore the original
    // bank, including a base loaded earlier in the list, without changing CPSR.
    // Exception LR takes precedence when the fault originated in Abort mode.
    if (reg == 15 || (reg == 14 && (oldcpsr & 0x1F) == 0x17)) return;
    cpu->UpdateMode(cpu->CPSR, oldcpsr, true);
    cpu->R[reg] = base;
    cpu->UpdateMode(oldcpsr, cpu->CPSR, true);
}

void A_LDM(ARM* cpu)
{
    if (!(cpu->CurInstr & 0xFFFF))
    {
        A_EmptyBlockTransfer(cpu, true);
        return;
    }

    u32 baseid = (cpu->CurInstr >> 16) & 0xF;
    u32 base = cpu->R[baseid];
    const u32 oldbase = base, oldcpsr = cpu->CPSR;
    u32 wbbase;
    u32 preinc = (cpu->CurInstr & (1<<24));
    bool first = true;

    if (!(cpu->CurInstr & (1<<23)))
    {
        base -= 4 * std::popcount(cpu->CurInstr & 0xFFFFu);

        if (cpu->CurInstr & (1<<21))
        {
            // pre writeback
            wbbase = base;
        }

        preinc = !preinc;
    }

    const bool user = (cpu->CurInstr & (1<<22)) && !(cpu->CurInstr & (1<<15));

    for (int i = 0; i < 15; i++)
    {
        if (cpu->CurInstr & (1<<i))
        {
            if (preinc) base += 4;
            u32* dest = user ? &UserTransferReg(cpu, i) : &cpu->R[i];
            if (!(first ? cpu->DataRead32(base, dest) : cpu->DataRead32S(base, dest)))
            {
                RestoreTransferBase(cpu, baseid, oldbase, oldcpsr);
                return;
            }
            first = false;
            if (!preinc) base += 4;
        }
    }

    u32 pc = 0;
    if (cpu->CurInstr & (1<<15))
    {
        if (preinc) base += 4;
        if (!(first ? cpu->DataRead32(base, &pc) : cpu->DataRead32S(base, &pc)))
        {
            RestoreTransferBase(cpu, baseid, oldbase, oldcpsr);
            return;
        }
        if (!preinc) base += 4;

        if (cpu->Num == 1)
            pc &= ~0x1;
    }

    if (cpu->CurInstr & (1<<21))
    {
        // post writeback
        if (cpu->CurInstr & (1<<23))
            wbbase = base;

        if (cpu->CurInstr & (1 << baseid))
        {
            if (cpu->Num == 0)
            {
                u32 rlist = cpu->CurInstr & 0xFFFF;
                if ((!(rlist & ~(1 << baseid))) || (rlist & ~((2 << baseid) - 1)))
                    cpu->R[baseid] = wbbase;
            }
        }
        else
            cpu->R[baseid] = wbbase;
    }

    if (cpu->CurInstr & (1<<15))
        cpu->JumpTo(pc, cpu->CurInstr & (1<<22));

    cpu->AddCycles_CDI();
}

void A_STM(ARM* cpu)
{
    if (!(cpu->CurInstr & 0xFFFF))
    {
        A_EmptyBlockTransfer(cpu, false);
        return;
    }

    u32 baseid = (cpu->CurInstr >> 16) & 0xF;
    u32 base = cpu->R[baseid];
    u32 oldbase = base;
    u32 preinc = (cpu->CurInstr & (1<<24));
    bool first = true;

    if (!(cpu->CurInstr & (1<<23)))
    {
        base -= 4 * std::popcount(cpu->CurInstr & 0xFFFFu);

        preinc = !preinc;
    }

    bool isbanked = false;
    if (cpu->CurInstr & (1<<22))
    {
        u32 mode = (cpu->CPSR & 0x1F);
        if (mode == 0x11)
            isbanked = (baseid >= 8 && baseid < 15);
        else if (mode != 0x10 && mode != 0x1F)
            isbanked = (baseid >= 13 && baseid < 15);
    }

    for (u32 i = 0; i < 16; i++)
    {
        if (cpu->CurInstr & (1<<i))
        {
            if (preinc) base += 4;

            u32 value;
            if (i == baseid && !isbanked)
            {
                if ((cpu->Num == 0) || (!(cpu->CurInstr & ((1<<i)-1))))
                    value = oldbase;
                else
                    value = base; // checkme
            }
            else
            {
                // ARM7 stores PC one pipeline stage later than its visible A+8.
                value = ((cpu->CurInstr & (1<<22)) ? UserTransferReg(cpu, i) : cpu->R[i])
                    + (i == 15 && cpu->Num == 1 ? 4 : 0);
            }

            if (!(first ? cpu->DataWrite32(base, value) : cpu->DataWrite32S(base, value))) return;
            first = false;

            if (!preinc) base += 4;
        }
    }

    if (cpu->CurInstr & (1<<21))
        cpu->R[baseid] = (cpu->CurInstr & (1<<23)) ? base
            : oldbase - 4 * std::popcount(cpu->CurInstr & 0xFFFFu);

    cpu->AddCycles_CD();
}




// ---- THUMB -----------------------



void T_LDR_PCREL(ARM* cpu)
{
    u32 addr = (cpu->R[15] & ~0x2) + ((cpu->CurInstr & 0xFF) << 2);
    if (!cpu->DataRead32(addr, &cpu->R[(cpu->CurInstr >> 8) & 0x7])) return;

    cpu->AddCycles_CDI();
}


void T_STR_REG(ARM* cpu)
{
    u32 addr = cpu->R[(cpu->CurInstr >> 3) & 0x7] + cpu->R[(cpu->CurInstr >> 6) & 0x7];
    if (!cpu->DataWrite32(addr, cpu->R[cpu->CurInstr & 0x7])) return;

    cpu->AddCycles_CD();
}

void T_STRB_REG(ARM* cpu)
{
    u32 addr = cpu->R[(cpu->CurInstr >> 3) & 0x7] + cpu->R[(cpu->CurInstr >> 6) & 0x7];
    if (!cpu->DataWrite8(addr, cpu->R[cpu->CurInstr & 0x7])) return;

    cpu->AddCycles_CD();
}

void T_LDR_REG(ARM* cpu)
{
    u32 addr = cpu->R[(cpu->CurInstr >> 3) & 0x7] + cpu->R[(cpu->CurInstr >> 6) & 0x7];

    u32 val;
    if (!cpu->DataRead32(addr, &val)) return;
    cpu->R[cpu->CurInstr & 0x7] = ROR(val, 8*(addr&0x3));

    cpu->AddCycles_CDI();
}

void T_LDRB_REG(ARM* cpu)
{
    u32 addr = cpu->R[(cpu->CurInstr >> 3) & 0x7] + cpu->R[(cpu->CurInstr >> 6) & 0x7];
    if (!cpu->DataRead8(addr, &cpu->R[cpu->CurInstr & 0x7])) return;

    cpu->AddCycles_CDI();
}


void T_STRH_REG(ARM* cpu)
{
    u32 addr = cpu->R[(cpu->CurInstr >> 3) & 0x7] + cpu->R[(cpu->CurInstr >> 6) & 0x7];
    if (!cpu->DataWrite16(addr, cpu->R[cpu->CurInstr & 0x7])) return;

    cpu->AddCycles_CD();
}

void T_LDRSB_REG(ARM* cpu)
{
    u32 addr = cpu->R[(cpu->CurInstr >> 3) & 0x7] + cpu->R[(cpu->CurInstr >> 6) & 0x7];
    if (!cpu->DataRead8(addr, &cpu->R[cpu->CurInstr & 0x7])) return;
    cpu->R[cpu->CurInstr & 0x7] = (s32)(s8)cpu->R[cpu->CurInstr & 0x7];

    cpu->AddCycles_CDI();
}

void T_LDRH_REG(ARM* cpu)
{
    u32 addr = cpu->R[(cpu->CurInstr >> 3) & 0x7] + cpu->R[(cpu->CurInstr >> 6) & 0x7];
    if (!cpu->DataRead16(addr, &cpu->R[cpu->CurInstr & 0x7])) return;

    cpu->AddCycles_CDI();
}

void T_LDRSH_REG(ARM* cpu)
{
    u32 addr = cpu->R[(cpu->CurInstr >> 3) & 0x7] + cpu->R[(cpu->CurInstr >> 6) & 0x7];
    if (!cpu->DataRead16(addr, &cpu->R[cpu->CurInstr & 0x7])) return;
    cpu->R[cpu->CurInstr & 0x7] = (s32)(s16)cpu->R[cpu->CurInstr & 0x7];

    cpu->AddCycles_CDI();
}


void T_STR_IMM(ARM* cpu)
{
    u32 offset = (cpu->CurInstr >> 4) & 0x7C;
    offset += cpu->R[(cpu->CurInstr >> 3) & 0x7];

    if (!cpu->DataWrite32(offset, cpu->R[cpu->CurInstr & 0x7])) return;
    cpu->AddCycles_CD();
}

void T_LDR_IMM(ARM* cpu)
{
    u32 offset = (cpu->CurInstr >> 4) & 0x7C;
    offset += cpu->R[(cpu->CurInstr >> 3) & 0x7];

    u32 val;
    if (!cpu->DataRead32(offset, &val)) return;
    cpu->R[cpu->CurInstr & 0x7] = ROR(val, 8*(offset&0x3));
    cpu->AddCycles_CDI();
}

void T_STRB_IMM(ARM* cpu)
{
    u32 offset = (cpu->CurInstr >> 6) & 0x1F;
    offset += cpu->R[(cpu->CurInstr >> 3) & 0x7];

    if (!cpu->DataWrite8(offset, cpu->R[cpu->CurInstr & 0x7])) return;
    cpu->AddCycles_CD();
}

void T_LDRB_IMM(ARM* cpu)
{
    u32 offset = (cpu->CurInstr >> 6) & 0x1F;
    offset += cpu->R[(cpu->CurInstr >> 3) & 0x7];

    if (!cpu->DataRead8(offset, &cpu->R[cpu->CurInstr & 0x7])) return;
    cpu->AddCycles_CDI();
}


void T_STRH_IMM(ARM* cpu)
{
    u32 offset = (cpu->CurInstr >> 5) & 0x3E;
    offset += cpu->R[(cpu->CurInstr >> 3) & 0x7];

    if (!cpu->DataWrite16(offset, cpu->R[cpu->CurInstr & 0x7])) return;
    cpu->AddCycles_CD();
}

void T_LDRH_IMM(ARM* cpu)
{
    u32 offset = (cpu->CurInstr >> 5) & 0x3E;
    offset += cpu->R[(cpu->CurInstr >> 3) & 0x7];

    if (!cpu->DataRead16(offset, &cpu->R[cpu->CurInstr & 0x7])) return;
    cpu->AddCycles_CDI();
}


void T_STR_SPREL(ARM* cpu)
{
    u32 offset = (cpu->CurInstr << 2) & 0x3FC;
    offset += cpu->R[13];

    if (!cpu->DataWrite32(offset, cpu->R[(cpu->CurInstr >> 8) & 0x7])) return;
    cpu->AddCycles_CD();
}

void T_LDR_SPREL(ARM* cpu)
{
    u32 offset = (cpu->CurInstr << 2) & 0x3FC;
    offset += cpu->R[13];

    if (!cpu->DataRead32(offset, &cpu->R[(cpu->CurInstr >> 8) & 0x7])) return;
    cpu->AddCycles_CDI();
}


void T_PUSH(ARM* cpu)
{
    int nregs = std::popcount(cpu->CurInstr & 0x1FFu);
    bool first = true;

    u32 base = cpu->R[13];
    base -= (nregs<<2);

    for (int i = 0; i < 8; i++)
    {
        if (cpu->CurInstr & (1<<i))
        {
            if (!(first ? cpu->DataWrite32(base, cpu->R[i]) : cpu->DataWrite32S(base, cpu->R[i]))) return;
            first = false;
            base += 4;
        }
    }

    if (cpu->CurInstr & (1<<8))
    {
        if (!(first ? cpu->DataWrite32(base, cpu->R[14]) : cpu->DataWrite32S(base, cpu->R[14]))) return;
    }

    cpu->R[13] -= nregs << 2;
    cpu->AddCycles_CD();
}

void T_POP(ARM* cpu)
{
    u32 base = cpu->R[13];
    bool first = true;

    for (int i = 0; i < 8; i++)
    {
        if (cpu->CurInstr & (1<<i))
        {
            if (!(first ? cpu->DataRead32(base, &cpu->R[i]) : cpu->DataRead32S(base, &cpu->R[i]))) return;
            first = false;
            base += 4;
        }
    }

    if (cpu->CurInstr & (1<<8))
    {
        u32 pc;
        if (!(first ? cpu->DataRead32(base, &pc) : cpu->DataRead32S(base, &pc))) return;
        if (cpu->Num==1) pc |= 0x1;
        cpu->JumpTo(pc);
        base += 4;
    }

    cpu->R[13] = base;
    cpu->AddCycles_CDI();
}

void T_STMIA(ARM* cpu)
{
    u32 base = cpu->R[(cpu->CurInstr >> 8) & 0x7];
    if (!(cpu->CurInstr & 0xFF))
    {
        cpu->R[(cpu->CurInstr >> 8) & 0x7] = base + 0x40;
        if (cpu->Num == 1)
        {
            cpu->DataWrite32(base, cpu->R[15] + 2);
            cpu->AddCycles_CD();
        }
        else
            cpu->AddCycles_C();
        return;
    }
    bool first = true;

    for (int i = 0; i < 8; i++)
    {
        if (cpu->CurInstr & (1<<i))
        {
            if (!(first ? cpu->DataWrite32(base, cpu->R[i]) : cpu->DataWrite32S(base, cpu->R[i]))) return;
            first = false;
            base += 4;
        }
    }

    // TODO: check "Rb included in Rlist" case
    cpu->R[(cpu->CurInstr >> 8) & 0x7] = base;
    cpu->AddCycles_CD();
}

void T_LDMIA(ARM* cpu)
{
    u32 base = cpu->R[(cpu->CurInstr >> 8) & 0x7];
    if (!(cpu->CurInstr & 0xFF))
    {
        cpu->R[(cpu->CurInstr >> 8) & 0x7] = base + 0x40;
        if (cpu->Num == 1)
        {
            u32 pc;
            cpu->DataRead32(base, &pc);
            cpu->JumpTo(pc | 1u);
            cpu->AddCycles_CDI();
        }
        else
            cpu->AddCycles_C();
        return;
    }
    const u32 oldbase = base, oldcpsr = cpu->CPSR;
    bool first = true;

    for (int i = 0; i < 8; i++)
    {
        if (cpu->CurInstr & (1<<i))
        {
            if (!(first ? cpu->DataRead32(base, &cpu->R[i]) : cpu->DataRead32S(base, &cpu->R[i])))
            {
                RestoreTransferBase(cpu, (cpu->CurInstr >> 8) & 0x7, oldbase, oldcpsr);
                return;
            }
            first = false;
            base += 4;
        }
    }

    if (!(cpu->CurInstr & (1<<((cpu->CurInstr >> 8) & 0x7))))
        cpu->R[(cpu->CurInstr >> 8) & 0x7] = base;

    cpu->AddCycles_CDI();
}


}

